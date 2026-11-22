#include "jit.h"

#include "jit_internal.h"

#include "diagnostics.h"
#include "dict.h"
#include "eval_context.h"
#include "gc.h"
#include "gc_roots.h"
#include "scopestack.h"
#include "structtype.h"
#include "symbol.h"
#include "value_ops.h"
#include "value_runtime.h"
#include "vm_eval.h"

#include <asmjit/asmjit.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/*
 * JIT helpers use the SysV register convention even on Windows.  The JIT
 * entry thunk itself follows the native ABI (Win64 on MinGW); using SysV for
 * the small helper calls keeps the emitted call sequences identical across
 * platforms.  MinGW GCC/Clang support this attribute.
 */
#if defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#define SKEIN_JIT_SYSV __attribute__((sysv_abi))
#else
#define SKEIN_JIT_SYSV
#endif

extern "C" {
SKEIN_JIT_SYSV value_t *skein_jit_null();
SKEIN_JIT_SYSV value_t *skein_jit_int(long long value);
SKEIN_JIT_SYSV value_t *skein_jit_dup(value_t *value);
SKEIN_JIT_SYSV void skein_jit_free(value_t *value);
SKEIN_JIT_SYSV value_t *skein_jit_binary(value_t *left, value_t *right, int op, int line,
                          int column);
SKEIN_JIT_SYSV int skein_jit_compare(int op, value_t *left, value_t *right, int *valid,
                      int line, int column);
SKEIN_JIT_SYSV int skein_jit_truthy(value_t *value);
SKEIN_JIT_SYSV value_t *skein_jit_load_global(const skein::bc::Proto *proto, uint32_t slot,
                               uint32_t name_index, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_store_global(const skein::bc::Proto *proto, uint32_t slot,
                                uint32_t name_index, value_t *value,
                                int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_compound_global(const skein::bc::Proto *proto,
                                   uint32_t slot, uint32_t name_index,
                                   value_t *value, int op, uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_incdec_global(const skein::bc::Proto *proto,
                                 uint32_t slot, uint32_t name_index,
                                 int delta, int prefix, uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_neg(value_t *value, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_not(value_t *value, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_compare_value(int op, value_t *left,
                                                value_t *right, int line,
                                                int column);
SKEIN_JIT_SYSV value_t *skein_jit_bitnot(value_t *value, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_load_name(const skein::bc::Proto *proto,
                             uint32_t name_index, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_get_field_or_enum(
    const skein::bc::FieldOrEnumDesc *desc, int line, int column);
SKEIN_JIT_SYSV void skein_jit_fail(value_t *value, uint64_t loc);
SKEIN_JIT_SYSV void skein_jit_runtime_error(const char *message,
                                            uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_store_name(const skein::bc::Proto *proto,
                              uint32_t name_index, value_t *value, int line,
                              int column);
SKEIN_JIT_SYSV value_t *skein_jit_compound_name(const skein::bc::Proto *proto,
                                uint32_t name_index, value_t *value, int op,
                                uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_incdec_name(const skein::bc::Proto *proto,
                              uint32_t name_index, int delta, int prefix,
                              uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_define_name(const skein::bc::Proto *proto,
                              uint32_t decl_index, value_t *value,
                              uint64_t loc);
SKEIN_JIT_SYSV int skein_jit_check_redefine(const skein::bc::Proto *proto,
                            uint32_t name_index, uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_iter_new(value_t **slot, uint64_t *state,
                           const skein::bc::IterDesc *desc,
                           value_t *iterable, uint64_t loc);
SKEIN_JIT_SYSV int skein_jit_iter_next(value_t **slot, uint64_t *state,
                       value_t **dest_key, value_t **dest_value,
                       const skein::bc::IterDesc *desc, uint64_t loc);
SKEIN_JIT_SYSV void skein_jit_iter_end(value_t **slot, uint64_t *state);
SKEIN_JIT_SYSV value_t *skein_jit_load_upvalue(function_def *def,
                               uint32_t index, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_store_upvalue(function_def *def,
                                uint32_t index, value_t *value, int line,
                                int column);
SKEIN_JIT_SYSV value_t *skein_jit_compound_upvalue(function_def *def,
                                   uint32_t index, int op, value_t *value,
                                   int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_incdec_upvalue(function_def *def,
                                 uint32_t index, int delta, int prefix,
                                 int line, int column);
SKEIN_JIT_SYSV size_t skein_jit_push_cell_mark(skein::jit::thread_state *ts);
SKEIN_JIT_SYSV void skein_jit_close_cells(skein::jit::thread_state *ts,
                           size_t mark);
SKEIN_JIT_SYSV void skein_jit_push_frame_scope(function_def *def);
SKEIN_JIT_SYSV void skein_jit_frame_scope_pushed(
    skein::jit::thread_state *ts);
SKEIN_JIT_SYSV size_t skein_jit_push_handler_mark(
    skein::jit::thread_state *ts);
SKEIN_JIT_SYSV void skein_jit_discard_handlers(
    skein::jit::thread_state *ts, size_t mark);
SKEIN_JIT_SYSV int skein_jit_push_handler(skein::jit::thread_state *ts,
                                          int kind, int32_t target,
                                          int32_t stack_depth, int flags,
                                          uint64_t loc);
SKEIN_JIT_SYSV void skein_jit_pop_handler(skein::jit::thread_state *ts,
                                          size_t mark);
SKEIN_JIT_SYSV void skein_jit_enter_handler_body(
    skein::jit::thread_state *ts);
SKEIN_JIT_SYSV void skein_jit_leave_handler_body(
    skein::jit::thread_state *ts);
SKEIN_JIT_SYSV int skein_jit_check_result(skein::jit::thread_state *ts,
                                          value_t **slot, uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_bind_name(skein::jit::thread_state *ts,
                                            value_t **slot, const char *name,
                                            int is_const, uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_bind_func(skein::jit::thread_state *ts,
                                            value_t *value,
                                            const skein::bc::Proto *proto,
                                            uint32_t name_index, int exported,
                                            uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_import(const skein::bc::Proto *proto,
                                         uint32_t index, value_t **slot,
                                         uint64_t loc);
SKEIN_JIT_SYSV void skein_jit_recover(skein::jit::thread_state *ts,
                                      value_t *value, uint64_t loc);
SKEIN_JIT_SYSV void skein_jit_pass(skein::jit::thread_state *ts,
                                   value_t *value, uint64_t loc);
SKEIN_JIT_SYSV int64_t skein_jit_unwind(skein::jit::thread_state *ts,
                                        size_t mark);
SKEIN_JIT_SYSV value_t *skein_jit_take_unwind_value(
    skein::jit::thread_state *ts);
SKEIN_JIT_SYSV void skein_jit_drop_slots(value_t **slots, uint32_t count);
SKEIN_JIT_SYSV void skein_jit_clear_slot(skein::jit::thread_state *ts,
                                         value_t **slot);
SKEIN_JIT_SYSV int skein_jit_defer_push(skein::jit::thread_state *ts,
                                        const skein::bc::Proto *proto,
                                        uint32_t index, value_t **frame_values,
                                        function_def *def, uint64_t loc);
SKEIN_JIT_SYSV void skein_jit_push_defer_scope(
    skein::jit::thread_state *ts);
SKEIN_JIT_SYSV void skein_jit_pop_defer_scope(
    skein::jit::thread_state *ts);
SKEIN_JIT_SYSV void skein_jit_enter_defer_frame(
    skein::jit::thread_state *ts);
SKEIN_JIT_SYSV int skein_jit_run_frame_defers(
    skein::jit::thread_state *ts);
SKEIN_JIT_SYSV value_t *skein_jit_closure(
    skein::jit::thread_state *ts, function_def *def,
    const skein::bc::Proto *proto, uint32_t const_index,
    value_t **frame_values, uint64_t loc);
SKEIN_JIT_SYSV void skein_jit_push_scope();
SKEIN_JIT_SYSV void skein_jit_pop_scope(skein::jit::thread_state *ts);
SKEIN_JIT_SYSV value_t *skein_jit_new_list(value_t **items, uint32_t count,
                            uint32_t frozen, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_new_dict(value_t **items, uint32_t count, int line,
                            int column);
SKEIN_JIT_SYSV int skein_jit_bind_patterns(function_def *def, value_t **values,
                            uint32_t argc);
SKEIN_JIT_SYSV value_t *skein_jit_invoke(value_t *callee, value_t **args, uint32_t argc,
                          int line, int column);
/*
 * Fast-path resolver used by generated CALL sites.  Returns the native entry
 * of an already-compiled callee, or null when the generic invoke path must be
 * used.  It does not consume `callee` or `args`.
 */
SKEIN_JIT_SYSV void *skein_jit_direct_target(value_t *callee, value_t **args,
                                             uint32_t argc,
                                             skein::jit::thread_state *ts,
                                             function_def **def_out);
SKEIN_JIT_SYSV void skein_jit_set_call_location(int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_invoke_method_cached(
    value_t *receiver, const char *name, value_t **args, uint32_t argc,
    skein::bc::MethodCallCacheEntry *cache,
    skein::jit::thread_state *ts);
char skein_jit_method_fallback_marker = 0;
SKEIN_JIT_SYSV value_t *skein_jit_tail_invoke(value_t *callee, value_t **args,
                               uint32_t argc, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_invoke_method(
    value_t *receiver, const char *name, value_t **args, uint32_t argc,
    int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_list_append_self(
    value_t *callee, value_t *list, value_t **args, uint32_t argc,
    int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_list_concat_self(value_t *left,
                                                   value_t *right,
                                                   int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_string_append_self(
    value_t *left, value_t *right, int line, int column);
SKEIN_JIT_SYSV int skein_jit_tail_self(skein::jit::thread_state *ts,
                        function_def *current_def, value_t *callee,
                        value_t **params, value_t **args, uint32_t argc);
SKEIN_JIT_SYSV value_t *skein_jit_tail_request(skein::jit::thread_state *ts,
                                value_t *callee, value_t **args,
                                uint32_t argc);
SKEIN_JIT_SYSV size_t skein_jit_push_roots(skein::jit::thread_state *ts,
                                           value_t **values,
                                           uint32_t count);
SKEIN_JIT_SYSV void skein_jit_pop_roots(skein::jit::thread_state *ts,
                                        size_t mark);
SKEIN_JIT_SYSV value_t *skein_jit_define_slot(const skein::bc::SlotDesc *desc,
                               value_t *value, value_t **dest, int line,
                               int column);
SKEIN_JIT_SYSV value_t *skein_jit_load_slot(value_t *value, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_store_slot(const skein::bc::SlotDesc *desc,
                              value_t *value, value_t **dest, int line,
                              int column);
SKEIN_JIT_SYSV value_t *skein_jit_compound_slot(const skein::bc::SlotDesc *desc,
                                 value_t *value, value_t **dest, int op,
                                 uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_incdec_slot(const skein::bc::SlotDesc *desc,
                               value_t **dest, int delta, int prefix,
                               uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_compound_param(
    function_def *def, uint32_t index, value_t *value, value_t **dest,
    int op, uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_incdec_param(
    function_def *def, uint32_t index, int delta, int prefix,
    value_t **dest, uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_slot_binary(value_t *left, value_t *right,
                               value_t **dest,
                               const skein::bc::SlotDesc *desc, int op,
                               uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_slot_binary_const(value_t *left,
                                     const value_t *constant,
                                     value_t **dest,
                                     const skein::bc::SlotDesc *desc,
                                     int op, uint64_t loc);
SKEIN_JIT_SYSV value_t *skein_jit_new_struct(const skein::bc::ConstructDesc *desc,
                              value_t **fields);
SKEIN_JIT_SYSV value_t *skein_jit_get_field(value_t *container, const char *name, int line,
                             int column);
SKEIN_JIT_SYSV value_t *skein_jit_get_index_cached(
    value_t *container, value_t *index,
    skein::bc::IndexReadCacheEntry *cache, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_get_index_base_cached(
    const skein::bc::IndexDesc *desc, value_t **base_slot, value_t *key,
    skein::bc::IndexReadCacheEntry *cache, uint64_t packed_loc);
SKEIN_JIT_SYSV value_t *skein_jit_assign_index(const skein::bc::IndexDesc *desc,
                                value_t **indices, value_t *value,
                                value_t **base_slot, uint64_t packed_loc);
SKEIN_JIT_SYSV value_t *skein_jit_compound_index(
    const skein::bc::IndexDesc *desc, value_t **indices, value_t *value,
    value_t **base_slot, int op, uint64_t packed_loc);
SKEIN_JIT_SYSV value_t *skein_jit_incdec_index(
    const skein::bc::IndexDesc *desc, value_t **indices, int delta,
    int prefix, value_t **base_slot, uint64_t packed_loc);
SKEIN_JIT_SYSV int skein_jit_enter(skein::jit::thread_state *ts, function_def *def);
SKEIN_JIT_SYSV void skein_jit_leave(skein::jit::thread_state *ts);
SKEIN_JIT_SYSV void skein_jit_cleanup(skein::jit::thread_state *ts, value_t **values,
                       uint32_t count);
SKEIN_JIT_SYSV int skein_jit_check_interrupt();
}

namespace {

using skein::jit::internal::read_bool_env;

struct JitRuntimeState {
    asmjit::JitRuntime runtime;
    std::mutex mutex;
};

JitRuntimeState &runtime_state() {
    static JitRuntimeState state;
    return state;
}

template <typename Fn>
uint64_t fn_address(Fn fn) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fn));
}

constexpr int kTsOff = 8;
constexpr int kDefOff = 16;
constexpr int kResultOff = 24;
constexpr int kStatusOff = 32;
#if defined(_WIN32)
constexpr int kSavedRdiOff = 40;
constexpr int kSavedRsiOff = 48;
constexpr int kRootMarkOff = 56;
constexpr int kDirectDefOff = 64;
constexpr int kCellMarkOff = 72;
constexpr int kHandlerMarkOff = 80;
constexpr int kHeaderSize = 88;
#else
constexpr int kRootMarkOff = 40;
constexpr int kDirectDefOff = 48;
constexpr int kCellMarkOff = 56;
constexpr int kHandlerMarkOff = 64;
constexpr int kHeaderSize = 72;
#endif

int slot_op_char(uint8_t opcode) {
    switch (static_cast<skein::bc::Op>(opcode)) {
        case skein::bc::Op::Add: return '+';
        case skein::bc::Op::Sub: return '-';
        case skein::bc::Op::Mul: return '*';
        case skein::bc::Op::Div: return '/';
        case skein::bc::Op::Mod: return '%';
        case skein::bc::Op::BitAnd: return '&';
        case skein::bc::Op::BitOr: return '|';
        case skein::bc::Op::BitXor: return '^';
        case skein::bc::Op::Shl: return 's';
        case skein::bc::Op::Shr: return 'r';
        default: return 0;
    }
}

bool is_binary_op_char(int op) {
    switch (op) {
        case '+': case '-': case '*': case '/': case '%':
        case '&': case '|': case '^': case 's': case 'r':
            return true;
        default:
            return false;
    }
}

uint64_t pack_loc(const skein::bc::SourceLoc &loc) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(loc.line)) << 32) |
           static_cast<uint32_t>(loc.column);
}

} // namespace

namespace skein {
namespace jit {

struct JitCode {
    const skein::bc::Proto *proto = nullptr;
    asmjit::JitRuntime *runtime = nullptr;
    void (*entry)() = nullptr;
    size_t size = 0;
    bool top_level = false;
};

namespace {

invoke_fn g_invoke = nullptr;
invoke_fn g_tail_invoke = nullptr;
construct_fn g_build_struct = nullptr;
bind_frame_fn g_bind_frame = nullptr;
direct_fn g_direct = nullptr;
/*
 * The VM bridge installs the cached method dispatcher at runtime.  Keeping it
 * behind a callback lets libskein_jit stay independent of the interpreter
 * layer that owns struct/type metadata.
 */
invoke_method_cached_fn g_invoke_method_cached = nullptr;
generator_test_fn g_generator_test = nullptr;
generator_step_fn g_generator_step = nullptr;
build_enum_fn g_build_enum = nullptr;
run_defer_scope_fn g_run_defer_scope = nullptr;
import_fn g_import = nullptr;

} // namespace

void set_invoke(invoke_fn fn) { g_invoke = fn; }
invoke_fn invoke() { return g_invoke; }
void set_tail_invoke(invoke_fn fn) { g_tail_invoke = fn; }
invoke_fn tail_invoke() { return g_tail_invoke; }
void set_construct(construct_fn fn) { g_build_struct = fn; }
construct_fn construct() { return g_build_struct; }
void set_bind_frame(bind_frame_fn fn) { g_bind_frame = fn; }
bind_frame_fn bind_frame() { return g_bind_frame; }
void set_direct(direct_fn fn) { g_direct = fn; }
direct_fn direct() { return g_direct; }
void set_invoke_method_cached(invoke_method_cached_fn fn) {
    g_invoke_method_cached = fn;
}
invoke_method_cached_fn invoke_method_cached() {
    return g_invoke_method_cached;
}
value_t *method_fallback_marker() {
    return reinterpret_cast<value_t *>(&skein_jit_method_fallback_marker);
}
void set_generator_test(generator_test_fn fn) { g_generator_test = fn; }
generator_test_fn generator_test() { return g_generator_test; }
void set_generator_step(generator_step_fn fn) { g_generator_step = fn; }
generator_step_fn generator_step() { return g_generator_step; }
void set_build_enum(build_enum_fn fn) { g_build_enum = fn; }
build_enum_fn build_enum() { return g_build_enum; }
void set_run_defer_scope(run_defer_scope_fn fn) { g_run_defer_scope = fn; }
run_defer_scope_fn run_defer_scope() { return g_run_defer_scope; }
void set_import(import_fn fn) { g_import = fn; }
import_fn import() { return g_import; }

bool available() {
    static int state = -1;
    if (state < 0)
        state = read_bool_env("SKEIN_JIT", true) ? 1 : 0;
    return state != 0;
}

bool can_compile(const skein::bc::Proto &proto, bool top_level,
                 bool pattern_frame) {
    auto reject = [&](const char *why) {
        if (read_bool_env("SKEIN_JIT_DEBUG", false))
            std::fprintf(stderr, "[jit] reject %s: %s\n",
                         proto.name.empty() ? "<anonymous>"
                                            : proto.name.c_str(),
                         why);
        return false;
    };
    /*
     * Name-based locals are compiled through scope helpers, and the prologue
     * binds direct parameters into the frame.  A top-level statement has no
     * parameters; only its name-mode form is compiled because the plain form
     * would route calls through the generic (non-tail) path.
     */
    const bool frame_shape_ok =
        proto.params_direct || (top_level && proto.nparams == 0) ||
        pattern_frame;
    if (!frame_shape_ok)
        return reject("frame shape");
    if (proto.uses_defers) {
        if (proto.uses_handlers)
            return reject("handler + defer");
        if (top_level)
            return reject("top-level defer");
    }
    /* Top-level statements execute in the global scope; keep handler
     * statements on the VM path. */
    if (top_level && proto.uses_handlers)
        return reject("top-level handler");
    if (proto_may_suspend(proto))
        return reject("suspend");
    if (proto.variadic)
        return reject("variadic");
    if (proto.nparams != proto.required_params)
        return reject("default parameters");
    if (proto.stack_depth.size() < proto.code.size())
        return reject("stack depth");

    const uint64_t value_count =
        static_cast<uint64_t>(proto.nparams) + proto.slots.size() +
        proto.max_stack + proto.iters.size();
    if (value_count > 512)
        return reject("frame too large");

    for (size_t pc = 0; pc < proto.code.size(); ++pc) {
        const skein::bc::Instr &ins = proto.code[pc];
        switch (static_cast<skein::bc::Op>(ins.op)) {
            case skein::bc::Op::Nop:
            case skein::bc::Op::LoadNull:
            case skein::bc::Op::Add:
            case skein::bc::Op::Sub:
            case skein::bc::Op::Mul:
            case skein::bc::Op::Div:
            case skein::bc::Op::Mod:
            case skein::bc::Op::BitAnd:
            case skein::bc::Op::BitOr:
            case skein::bc::Op::BitXor:
            case skein::bc::Op::Shl:
            case skein::bc::Op::Shr:
            case skein::bc::Op::Neg:
            case skein::bc::Op::Not:
            case skein::bc::Op::BitNot:
            case skein::bc::Op::Jump:
            case skein::bc::Op::JumpIfFalse:
            case skein::bc::Op::JumpIfTrue:
            case skein::bc::Op::CmpJump:
            case skein::bc::Op::CmpEq:
            case skein::bc::Op::CmpNe:
            case skein::bc::Op::CmpLt:
            case skein::bc::Op::CmpLe:
            case skein::bc::Op::CmpGt:
            case skein::bc::Op::CmpGe:
                break;
            case skein::bc::Op::LoadParam:
                if (ins.b >= proto.nparams)
                    return reject("parameter index");
                break;
            case skein::bc::Op::CompoundParam:
                if (ins.b >= proto.nparams || !is_binary_op_char(ins.flags))
                    return reject("compound parameter");
                break;
            case skein::bc::Op::IncDecParam:
                if (ins.b >= proto.nparams)
                    return reject("incdec parameter");
                break;
            case skein::bc::Op::Call:
                /* `launch f()` uses flags & 1 to start a new fiber.  The JIT
                 * call helpers do not implement fiber launch, so compiling
                 * such a call would invoke the callee synchronously in the
                 * caller's fiber and misattribute its control flow (e.g. a
                 * `fail` would escape as an uncaught error instead of being
                 * reported as a detached task failure). */
                if (ins.flags & 1)
                    return reject("fiber launch call");
                break;
            case skein::bc::Op::CallMethod:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return reject("call-method constant");
                break;
            case skein::bc::Op::ListAppendSelf:
            case skein::bc::Op::ListConcatSelf:
            case skein::bc::Op::StringAppendSelf:
                break;
            case skein::bc::Op::TailCall:
                /* Defensive: compile_call() never sets launch on a tail call,
                 * but the JIT call path is synchronous either way. */
                if (ins.flags & 1)
                    return reject("launch tail call");
                if (top_level)
                    return reject("top-level tail call");
                break;
            case skein::bc::Op::Return:
            case skein::bc::Op::PushScope:
            case skein::bc::Op::PopScope:
            case skein::bc::Op::Pop:
            case skein::bc::Op::Dup:
            case skein::bc::Op::Fail:
            case skein::bc::Op::Pass:
            case skein::bc::Op::Recover:
            case skein::bc::Op::CheckResult:
            case skein::bc::Op::EnterHandlerBody:
            case skein::bc::Op::LeaveHandlerBody:
            case skein::bc::Op::PopHandler:
                if (top_level &&
                    static_cast<skein::bc::Op>(ins.op) ==
                        skein::bc::Op::Return)
                    return reject("top-level return");
                break;
            case skein::bc::Op::PushFailHandler:
            case skein::bc::Op::PushRecoverHandler:
                if (ins.b >= proto.code.size())
                    return reject("handler target");
                break;
            case skein::bc::Op::BindName:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return reject("bind name constant");
                if (ins.a != UINT16_MAX && ins.a >= proto.slots.size())
                    return reject("bind name slot");
                break;
            case skein::bc::Op::BindFunc:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return reject("bind func name");
                break;
            case skein::bc::Op::Import:
                if (ins.a >= proto.imports.size())
                    return reject("import descriptor");
                if (ins.b != UINT32_MAX && ins.b >= proto.slots.size())
                    return reject("import slot");
                break;
            case skein::bc::Op::RuntimeError:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return reject("runtime-error message");
                break;
            case skein::bc::Op::DeferPush:
                if (ins.b >= proto.defer_protos.size())
                    return reject("defer descriptor");
                break;
            case skein::bc::Op::LoadConst:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Value ||
                    !proto.consts[ins.b].value)
                    return reject("load-const");
                break;
            case skein::bc::Op::LoadGlobal:
            case skein::bc::Op::StoreGlobal:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return reject("global constant");
                break;
            case skein::bc::Op::CompoundGlobal:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name ||
                    !is_binary_op_char(ins.flags))
                    return false;
                break;
            case skein::bc::Op::IncDecGlobal:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return false;
                break;
            case skein::bc::Op::LoadUpvalue:
            case skein::bc::Op::StoreUpvalue:
                if (ins.b >= proto.upvalues.size())
                    return false;
                break;
            case skein::bc::Op::CompoundUpvalue:
                if (ins.b >= proto.upvalues.size() ||
                    !is_binary_op_char(ins.flags))
                    return false;
                break;
            case skein::bc::Op::IncDecUpvalue:
                if (ins.b >= proto.upvalues.size())
                    return false;
                break;
            case skein::bc::Op::Closure:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Function ||
                    !proto.consts[ins.b].function)
                    return reject("closure constant");
                break;
            case skein::bc::Op::LoadSlot:
            case skein::bc::Op::StoreSlot:
            case skein::bc::Op::ClearSlot:
                if (ins.b >= proto.slots.size())
                    return reject("slot index");
                break;
            case skein::bc::Op::CompoundSlot:
                if (ins.b >= proto.slots.size() ||
                    !is_binary_op_char(ins.flags))
                    return false;
                break;
            case skein::bc::Op::IncDecSlot:
                if (ins.b >= proto.slots.size())
                    return false;
                break;
            case skein::bc::Op::DefineSlot:
                if (ins.b >= proto.slots.size() || (ins.flags & 1) == 0)
                    return reject("define slot");
                break;
            case skein::bc::Op::BinarySlots: {
                const uint32_t left = ins.a;
                const uint32_t right = ins.b & 0xffffu;
                const uint32_t dest = ins.b >> 16;
                if (left >= proto.slots.size() ||
                    right >= proto.slots.size() ||
                    dest >= proto.slots.size() ||
                    slot_op_char(ins.flags) == 0)
                    return false;
                break;
            }
            case skein::bc::Op::BinarySlotConst:
                if (ins.a >= proto.slots.size() ||
                    ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Value ||
                    !proto.consts[ins.b].value ||
                    slot_op_char(ins.flags) == 0)
                    return false;
                break;
            case skein::bc::Op::NewList:
                if (proto.stack_depth[pc] < static_cast<int>(ins.b))
                    return false;
                break;
            case skein::bc::Op::NewDict:
                if (proto.stack_depth[pc] <
                    static_cast<int>(ins.b) * 2)
                    return false;
                break;
            case skein::bc::Op::NewStruct: {
                if (ins.b >= proto.constructs.size())
                    return false;
                const skein::bc::ConstructDesc &desc =
                    proto.constructs[ins.b];
                const int depth = proto.stack_depth[pc];
                if (depth < static_cast<int>(desc.fields.size()))
                    return false;
                for (bool spread : desc.spread)
                    if (spread)
                        return false;
                break;
            }
            case skein::bc::Op::GetFieldName:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return reject("field-name constant");
                break;
            case skein::bc::Op::GetFieldOrEnum:
                if (ins.b >= proto.field_or_enum.size())
                    return reject("field-or-enum descriptor");
                break;
            case skein::bc::Op::GetIndexCached:
                if (ins.b >= proto.index_read_ic.size())
                    return reject("index-read descriptor");
                break;
            case skein::bc::Op::GetIndexBaseCached: {
                if (ins.b >= proto.index_ops.size())
                    return reject("index-read-base descriptor");
                const skein::bc::IndexDesc &desc =
                    proto.index_ops[ins.b];
                if (desc.fields.size() != 1)
                    return reject("index-read-base fields");
                using skein::bc::IndexBaseKind;
                if (desc.base_kind == IndexBaseKind::Slot ||
                    desc.base_kind == IndexBaseKind::Param) {
                    const size_t limit =
                        desc.base_kind == IndexBaseKind::Slot
                            ? proto.slots.size()
                            : proto.param_names.size();
                    if (desc.base_index >= limit)
                        return reject("index-read-base slot");
                } else if (desc.base_kind != IndexBaseKind::Global) {
                    return reject("index-read-base kind");
                }
                if (desc.read_cache != UINT32_MAX &&
                    desc.read_cache >= proto.index_read_ic.size())
                    return reject("index-read-base cache");
                if (desc.const_key != UINT32_MAX) {
                    if (desc.const_key >= proto.consts.size() ||
                        proto.consts[desc.const_key].kind !=
                            skein::bc::ConstantKind::Value ||
                        !proto.consts[desc.const_key].value)
                        return reject("index-read-base key");
                }
                break;
            }
            case skein::bc::Op::AssignIndex:
            case skein::bc::Op::CompoundIndex:
            case skein::bc::Op::IncDecIndex: {
                if (ins.b >= proto.index_ops.size())
                    return reject("index-write descriptor");
                if (static_cast<skein::bc::Op>(ins.op) ==
                        skein::bc::Op::CompoundIndex &&
                    !is_binary_op_char(ins.flags))
                    return reject("compound index operator");
                const skein::bc::IndexDesc &desc =
                    proto.index_ops[ins.b];
                if (desc.fields.empty())
                    return reject("index-write fields");
                using skein::bc::IndexBaseKind;
                if (desc.base_kind == IndexBaseKind::Slot ||
                    desc.base_kind == IndexBaseKind::Param) {
                    const size_t limit =
                        desc.base_kind == IndexBaseKind::Slot
                            ? proto.slots.size()
                            : proto.nparams;
                    if (desc.base_index >= limit)
                        return reject("index-write base slot");
                } else if (desc.base_kind == IndexBaseKind::Upvalue) {
                    if (desc.base_index >= proto.upvalues.size())
                        return reject("index-write upvalue");
                } else if (desc.base_kind != IndexBaseKind::Global &&
                           desc.base_kind != IndexBaseKind::Name) {
                    return reject("index-write base kind");
                }
                break;
            }
            case skein::bc::Op::LoadName:
            case skein::bc::Op::StoreName:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return reject("name constant");
                break;
            case skein::bc::Op::CompoundName:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name ||
                    !is_binary_op_char(ins.flags))
                    return false;
                break;
            case skein::bc::Op::IncDecName:
            case skein::bc::Op::CheckRedefine:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return false;
                break;
            case skein::bc::Op::DefineName:
                if (ins.b >= proto.decls.size() || (ins.flags & 1) == 0)
                    return false;
                break;
            case skein::bc::Op::IterNew:
            case skein::bc::Op::IterEnd: {
                if (ins.a >= proto.iters.size())
                    return reject("iterator index");
                const skein::bc::IterDesc &desc = proto.iters[ins.a];
                if (desc.key_slot >= proto.slots.size())
                    return reject("iterator key slot");
                if (desc.two_vars && desc.value_slot >= proto.slots.size())
                    return reject("iterator value slot");
                break;
            }
            case skein::bc::Op::IterNext: {
                if (ins.a >= proto.iters.size() ||
                    ins.b > proto.code.size())
                    return reject("iterator target");
                const skein::bc::IterDesc &desc = proto.iters[ins.a];
                if (desc.key_slot >= proto.slots.size())
                    return reject("iterator key slot");
                if (desc.two_vars && desc.value_slot >= proto.slots.size())
                    return reject("iterator value slot");
                break;
            }
            default:
                if (read_bool_env("SKEIN_JIT_DEBUG", false))
                    std::fprintf(stderr, "[jit] unsupported op %s in %s\n",
                                 skein::bc::op_name(
                                     static_cast<skein::bc::Op>(ins.op)),
                                 proto.name.empty() ? "<anonymous>"
                                                    : proto.name.c_str());
                return false;
        }
    }
    return true;
}

JitCode *compile(const skein::bc::Proto &proto, bool top_level,
                 bool pattern_frame) {
    if (!available() || !can_compile(proto, top_level, pattern_frame))
        return nullptr;
    if (proto.call_method_ic.size() < proto.code.size())
        proto.call_method_ic.resize(proto.code.size());

    asmjit::JitRuntime &rt = runtime_state().runtime;
    asmjit::CodeHolder code;
    if (code.init(rt.environment(), rt.cpuFeatures()) != asmjit::kErrorOk)
        return nullptr;

    asmjit::StringLogger logger;
    const bool dump = read_bool_env("SKEIN_JIT_DUMP", false);
    if (dump)
        code.setLogger(&logger);

    asmjit::x86::Assembler a(&code);

    std::vector<asmjit::Label> labels(proto.code.size());
    for (asmjit::Label &label : labels)
        label = a.newLabel();
    const asmjit::Label end_label = a.newLabel();
    const asmjit::Label abort_label = a.newLabel();
    const asmjit::Label epilogue_label = a.newLabel();

    /* Bytecode targets of PUSH_*_HANDLER, used by the abort trampoline. */
    std::vector<uint32_t> handler_targets;
    std::vector<int> handler_depths;
    std::vector<asmjit::Label> handler_dispatch;
    for (size_t pc = 0; pc < proto.code.size(); ++pc) {
        const skein::bc::Instr &ins = proto.code[pc];
        const skein::bc::Op op = static_cast<skein::bc::Op>(ins.op);
        if (op != skein::bc::Op::PushFailHandler &&
            op != skein::bc::Op::PushRecoverHandler)
            continue;
        if (std::find(handler_targets.begin(), handler_targets.end(),
                      ins.b) != handler_targets.end())
            continue;
        handler_targets.push_back(ins.b);
        /* The handler is entered with the raised value on the stack; the
         * verifier does not model that edge, so derive the depth from the
         * push site instead of stack_depth[target]. */
        handler_depths.push_back(
            pc < proto.stack_depth.size() ? proto.stack_depth[pc] + 1 : 0);
        handler_dispatch.push_back(a.newLabel());
    }

    /*
     * The VM polls g_interrupted every 1024 instructions.  Generated code
     * must do the same at loop headers, otherwise a JIT-compiled tight loop
     * cannot be stopped with Ctrl-C.  A poll at the header (instead of at
     * the branching instruction) also avoids clobbering flags that CmpJump
     * carries into its conditional branch.  Self tail calls jump back to pc 0
     * without a bytecode branch, so mark that header as well.
     */
    std::vector<bool> poll_at(labels.size(), false);
    for (size_t pc = 0; pc < proto.code.size(); ++pc) {
        const skein::bc::Instr &ins = proto.code[pc];
        switch (static_cast<skein::bc::Op>(ins.op)) {
            case skein::bc::Op::Jump:
            case skein::bc::Op::CmpJump:
            case skein::bc::Op::JumpIfFalse:
            case skein::bc::Op::JumpIfTrue:
                if (ins.b < labels.size() && ins.b <= pc)
                    poll_at[ins.b] = true;
                break;
            case skein::bc::Op::TailCall:
                if (!proto.code.empty())
                    poll_at[0] = true;
                break;
            default:
                break;
        }
    }

    const uint32_t iter_count =
        static_cast<uint32_t>(proto.iters.size());
    /* 32 bytes per iterator: index, count, flags, padding. */
    const int iter_stride = 32;
    const int iter_bytes = static_cast<int>(iter_count) * iter_stride;
    const uint32_t value_count =
        proto.nparams + static_cast<uint32_t>(proto.slots.size()) +
        proto.max_stack + iter_count;
    const int values_low =
        kHeaderSize + iter_bytes + static_cast<int>(value_count) * 8;
    const int frame_size = (values_low + 15) & ~15;
#if defined(_WIN32)
    /* Windows stack probing is only needed for large frames; fall back to VM. */
    if (frame_size > 3072)
        return nullptr;
#endif

    auto value_off = [&](uint32_t index) -> int {
        return -values_low + static_cast<int>(index) * 8;
    };
    auto param_off = [&](uint32_t index) -> int {
        return value_off(index);
    };
    auto slot_off = [&](uint32_t index) -> int {
        return value_off(proto.nparams + index);
    };
    auto stack_off = [&](int depth) -> int {
        return value_off(proto.nparams +
                         static_cast<uint32_t>(proto.slots.size()) +
                         static_cast<uint32_t>(depth));
    };
    /*
     * Iterator state lives in the frame header, directly below the fixed
     * header fields; the iterable value itself is kept in a hidden value slot
     * (see `stack_off(max_stack + index)`) so the existing frame root covers
     * it.
     */
    auto iter_state_off = [&](uint32_t index) -> int {
        return -(kHeaderSize + static_cast<int>(index) * iter_stride + 8);
    };
    auto target_label = [&](uint32_t target) -> asmjit::Label {
        return target < labels.size() ? labels[target] : end_label;
    };
    auto call_addr = [&](uint64_t address) {
        a.mov(asmjit::x86::r11, asmjit::imm(address));
        a.call(asmjit::x86::r11);
    };
    auto push_reg = [&](int depth, const asmjit::x86::Gp &reg) {
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                     stack_off(depth)),
              reg);
    };
    auto clear_slot = [&](int depth) {
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                     stack_off(depth)),
              asmjit::imm(0));
    };
    /*
     * Consume an operand known to be an int.  Interned small ints need no
     * work; allocate a branch around the helper for the uncommon case.
     */
    auto free_int_slot = [&](int depth) {
        asmjit::Label skip = a.newLabel();
        a.mov(asmjit::x86::rdi,
              asmjit::x86::qword_ptr(asmjit::x86::rbp, stack_off(depth)));
        a.cmp(asmjit::x86::byte_ptr(
                  asmjit::x86::rdi,
                  static_cast<int>(offsetof(int_value_t, interned))),
              asmjit::imm(0));
        a.jne(skip);
        call_addr(fn_address(&skein_jit_free));
        a.bind(skip);
        clear_slot(depth);
    };

    /* Prologue. */
    a.push(asmjit::x86::rbp);
    a.mov(asmjit::x86::rbp, asmjit::x86::rsp);
    a.sub(asmjit::x86::rsp, frame_size);
#if defined(_WIN32)
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kSavedRdiOff),
          asmjit::x86::rdi);
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kSavedRsiOff),
          asmjit::x86::rsi);
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff),
          asmjit::x86::rcx);
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff),
          asmjit::x86::rdx);
#else
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff),
          asmjit::x86::rdi);
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff),
          asmjit::x86::rsi);
#endif
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff),
          asmjit::imm(0));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kStatusOff),
          asmjit::imm(0));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kRootMarkOff),
          asmjit::imm(static_cast<int64_t>(-1)));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kCellMarkOff),
          asmjit::imm(static_cast<int64_t>(-1)));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kHandlerMarkOff),
          asmjit::imm(static_cast<int64_t>(-1)));
    a.xor_(asmjit::x86::eax, asmjit::x86::eax);
    for (uint32_t i = 0; i < value_count; ++i)
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                     value_off(i)),
              asmjit::x86::rax);
    for (uint32_t i = 0; i < iter_count * 4; ++i)
        a.mov(asmjit::x86::qword_ptr(
                  asmjit::x86::rbp, -(kHeaderSize + 8 + (int)i * 8)),
              asmjit::x86::rax);
    for (uint32_t i = 0; i < proto.nparams; ++i) {
#if defined(_WIN32)
        a.mov(asmjit::x86::rax,
              asmjit::x86::ptr(asmjit::x86::r8, i * 8));
#else
        a.mov(asmjit::x86::rax,
              asmjit::x86::ptr(asmjit::x86::rdx, i * 8));
#endif
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                     param_off(i)),
              asmjit::x86::rax);
    }

    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    a.mov(asmjit::x86::rsi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
    call_addr(fn_address(&skein_jit_enter));
    a.test(asmjit::x86::eax, asmjit::x86::eax);
    a.jz(abort_label);
    if (pattern_frame) {
        a.mov(asmjit::x86::rdi,
              asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
        a.lea(asmjit::x86::rsi,
              asmjit::x86::ptr(asmjit::x86::rbp, -values_low));
        a.mov(asmjit::x86::edx, asmjit::imm(proto.nparams));
        call_addr(fn_address(&skein_jit_bind_patterns));
        a.test(asmjit::x86::eax, asmjit::x86::eax);
        a.jz(abort_label);
        a.mov(asmjit::x86::rdi,
              asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
        call_addr(fn_address(&skein_jit_frame_scope_pushed));
    } else if ((proto.has_closures || proto.uses_names ||
                proto.uses_handlers) &&
               !top_level) {
        /* Name-based locals and closure creation both resolve through the
         * live scope chain, so such frames need their own scope.  Top-level
         * code deliberately runs in the global scope. */
        a.mov(asmjit::x86::rdi,
              asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
        call_addr(fn_address(&skein_jit_push_frame_scope));
        a.mov(asmjit::x86::rdi,
              asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
        call_addr(fn_address(&skein_jit_frame_scope_pushed));
    }
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    call_addr(fn_address(&skein_jit_push_cell_mark));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kCellMarkOff),
          asmjit::x86::rax);
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    call_addr(fn_address(&skein_jit_push_handler_mark));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kHandlerMarkOff),
          asmjit::x86::rax);
    if (proto.uses_defers) {
        a.mov(asmjit::x86::rdi,
              asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
        call_addr(fn_address(&skein_jit_enter_defer_frame));
    }
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    a.lea(asmjit::x86::rsi,
          asmjit::x86::ptr(asmjit::x86::rbp, -values_low));
    a.mov(asmjit::x86::edx, asmjit::imm(value_count));
    call_addr(fn_address(&skein_jit_push_roots));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kRootMarkOff),
          asmjit::x86::rax);
    a.jmp(proto.code.empty() ? end_label : labels[0]);

    for (size_t pc = 0; pc < proto.code.size(); ++pc) {
        a.bind(labels[pc]);
        if (poll_at[pc]) {
            call_addr(fn_address(&skein_jit_check_interrupt));
            a.test(asmjit::x86::eax, asmjit::x86::eax);
            a.jnz(abort_label);
        }
        const int depth = proto.stack_depth[pc];
        if (depth < 0)
            continue;

        const skein::bc::Instr &ins = proto.code[pc];
        const skein::bc::SourceLoc loc =
            pc < proto.locs.size() ? proto.locs[pc]
                                   : skein::bc::SourceLoc();
        switch (static_cast<skein::bc::Op>(ins.op)) {
            case skein::bc::Op::Nop:
                break;

            case skein::bc::Op::LoadNull:
                call_addr(fn_address(&skein_jit_null));
                push_reg(depth, asmjit::x86::rax);
                break;

            case skein::bc::Op::LoadConst: {
                const skein::bc::Constant &constant = proto.consts[ins.b];
                const value_t *value = constant.value.get();
                bool shared_singleton = false;
                if (value) {
                    if (value->type == T_int)
                        shared_singleton =
                            ((const int_value_t *)value)->interned;
                    else if (value->type == T_bool ||
                             value->type == T_null)
                        shared_singleton = true;
                }
                if (shared_singleton) {
                    a.mov(asmjit::x86::rax,
                          asmjit::imm(fn_address(value)));
                    push_reg(depth, asmjit::x86::rax);
                } else {
                    a.mov(asmjit::x86::rdi,
                          asmjit::imm(fn_address(value)));
                    call_addr(fn_address(&skein_jit_dup));
                    push_reg(depth, asmjit::x86::rax);
                    a.test(asmjit::x86::rax, asmjit::x86::rax);
                    a.jz(abort_label);
                }
                break;
            }

            case skein::bc::Op::LoadParam: {
                const asmjit::Label slow_load = a.newLabel();
                const asmjit::Label load_done = a.newLabel();
                a.mov(asmjit::x86::rax,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             param_off(ins.b)));
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(slow_load);
                a.cmp(asmjit::x86::dword_ptr(asmjit::x86::rax),
                      asmjit::imm(T_int));
                a.jne(slow_load);
                a.cmp(asmjit::x86::byte_ptr(
                          asmjit::x86::rax,
                          static_cast<int>(
                              offsetof(int_value_t, interned))),
                      asmjit::imm(0));
                a.je(slow_load);
                push_reg(depth, asmjit::x86::rax);
                a.jmp(load_done);
                a.bind(slow_load);
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             param_off(ins.b)));
                call_addr(fn_address(&skein_jit_dup));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                a.bind(load_done);
                break;
            }

            case skein::bc::Op::LoadGlobal:
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.a));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.b));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_load_global));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::LoadName:
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::edx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_load_name));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::StoreName: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_store_name));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::CompoundName: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::ecx, asmjit::imm(ins.flags));
                a.mov(asmjit::x86::r8, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_compound_name));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::IncDecName:
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.a ? 1 : -1));
                a.mov(asmjit::x86::ecx,
                      asmjit::imm((ins.flags & 1) ? 1 : 0));
                a.mov(asmjit::x86::r8, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_incdec_name));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::CheckRedefine:
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::rdx, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_check_redefine));
                a.test(asmjit::x86::eax, asmjit::x86::eax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::DefineName: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::rcx, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_define_name));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::IterNew: {
                const int value_depth = depth - 1;
                const uint32_t desc_index = ins.a;
                a.lea(asmjit::x86::rdi,
                      asmjit::x86::ptr(
                          asmjit::x86::rbp,
                          stack_off(static_cast<int>(proto.max_stack) +
                                    static_cast<int>(desc_index))));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       iter_state_off(desc_index)));
                a.mov(asmjit::x86::rdx,
                      asmjit::imm(fn_address(&proto.iters[desc_index])));
                a.mov(asmjit::x86::rcx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::r8, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_iter_new));
                clear_slot(value_depth);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::IterNext: {
                const uint32_t desc_index = ins.a;
                const skein::bc::IterDesc &desc = proto.iters[desc_index];
                a.lea(asmjit::x86::rdi,
                      asmjit::x86::ptr(
                          asmjit::x86::rbp,
                          stack_off(static_cast<int>(proto.max_stack) +
                                    static_cast<int>(desc_index))));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       iter_state_off(desc_index)));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       slot_off(desc.key_slot)));
                if (desc.two_vars)
                    a.lea(asmjit::x86::rcx,
                          asmjit::x86::ptr(asmjit::x86::rbp,
                                           slot_off(desc.value_slot)));
                else
                    a.xor_(asmjit::x86::ecx, asmjit::x86::ecx);
                a.mov(asmjit::x86::r8,
                      asmjit::imm(fn_address(&desc)));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_iter_next));
                a.test(asmjit::x86::eax, asmjit::x86::eax);
                a.js(abort_label);
                a.jnz(target_label(ins.b));
                break;
            }

            case skein::bc::Op::IterEnd:
                a.lea(asmjit::x86::rdi,
                      asmjit::x86::ptr(
                          asmjit::x86::rbp,
                          stack_off(static_cast<int>(proto.max_stack) +
                                    static_cast<int>(ins.a))));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       iter_state_off(ins.a)));
                call_addr(fn_address(&skein_jit_iter_end));
                break;

            case skein::bc::Op::StoreGlobal: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.a));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.b));
                a.mov(asmjit::x86::rcx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r9d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_store_global));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::CompoundGlobal: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.a));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.b));
                a.mov(asmjit::x86::rcx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::r8d, asmjit::imm(ins.flags));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_compound_global));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::IncDecGlobal:
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.a));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.b));
                a.mov(asmjit::x86::ecx,
                      asmjit::imm((ins.flags & 2) ? 1 : -1));
                a.mov(asmjit::x86::r8d,
                      asmjit::imm((ins.flags & 1) ? 1 : 0));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_incdec_global));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::Closure: {
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
                a.mov(asmjit::x86::rdx,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::ecx, asmjit::imm(ins.b));
                a.lea(asmjit::x86::r8,
                      asmjit::x86::ptr(asmjit::x86::rbp, -values_low));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_closure));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::LoadUpvalue:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::edx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_load_upvalue));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::StoreUpvalue: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_store_upvalue));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::CompoundUpvalue: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.flags));
                a.mov(asmjit::x86::rcx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r9d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_compound_upvalue));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::IncDecUpvalue:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.a ? 1 : -1));
                a.mov(asmjit::x86::ecx,
                      asmjit::imm((ins.flags & 1) ? 1 : 0));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r9d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_incdec_upvalue));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::LoadSlot: {
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             slot_off(ins.b)));
                a.mov(asmjit::x86::esi, asmjit::imm(loc.line));
                a.mov(asmjit::x86::edx, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_load_slot));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::DefineSlot: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto.slots[ins.b])));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       slot_off(ins.b)));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_define_slot));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::StoreSlot: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto.slots[ins.b])));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       slot_off(ins.b)));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_store_slot));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::CompoundSlot: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto.slots[ins.b])));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       slot_off(ins.b)));
                a.mov(asmjit::x86::ecx, asmjit::imm(ins.flags));
                a.mov(asmjit::x86::r8, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_compound_slot));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::IncDecSlot:
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto.slots[ins.b])));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       slot_off(ins.b)));
                a.mov(asmjit::x86::edx,
                      asmjit::imm(ins.a ? 1 : -1));
                a.mov(asmjit::x86::ecx,
                      asmjit::imm((ins.flags & 1) ? 1 : 0));
                a.mov(asmjit::x86::r8, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_incdec_slot));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::CompoundParam: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.lea(asmjit::x86::rcx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       param_off(ins.b)));
                a.mov(asmjit::x86::r8d, asmjit::imm(ins.flags));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_compound_param));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::IncDecParam:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.b));
                a.mov(asmjit::x86::edx,
                      asmjit::imm(ins.a ? 1 : -1));
                a.mov(asmjit::x86::ecx,
                      asmjit::imm((ins.flags & 1) ? 1 : 0));
                a.lea(asmjit::x86::r8,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       param_off(ins.b)));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_incdec_param));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::ClearSlot:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       slot_off(ins.b)));
                call_addr(fn_address(&skein_jit_clear_slot));
                break;

            case skein::bc::Op::BinarySlots: {
                const uint32_t left = ins.a;
                const uint32_t right = ins.b & 0xffffu;
                const uint32_t dest = ins.b >> 16;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             slot_off(left)));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             slot_off(right)));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       slot_off(dest)));
                a.mov(asmjit::x86::rcx,
                      asmjit::imm(fn_address(&proto.slots[dest])));
                a.mov(asmjit::x86::r8d,
                      asmjit::imm(slot_op_char(ins.flags)));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_slot_binary));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::BinarySlotConst: {
                const uint32_t dest = ins.a;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             slot_off(dest)));
                a.mov(asmjit::x86::rsi,
                      asmjit::imm(fn_address(
                          proto.consts[ins.b].value.get())));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       slot_off(dest)));
                a.mov(asmjit::x86::rcx,
                      asmjit::imm(fn_address(&proto.slots[dest])));
                a.mov(asmjit::x86::r8d,
                      asmjit::imm(slot_op_char(ins.flags)));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_slot_binary_const));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::NewList: {
                const int count = static_cast<int>(ins.b);
                const int first = depth - count;
                a.lea(asmjit::x86::rdi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(first)));
                a.mov(asmjit::x86::esi, asmjit::imm(count));
                a.mov(asmjit::x86::edx,
                      asmjit::imm(static_cast<uint32_t>(ins.flags & 1)));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_new_list));
                for (int i = first; i < depth; ++i)
                    clear_slot(i);
                push_reg(first, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::NewDict: {
                const int count = static_cast<int>(ins.b);
                const int first = depth - count * 2;
                a.lea(asmjit::x86::rdi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(first)));
                a.mov(asmjit::x86::esi, asmjit::imm(count));
                a.mov(asmjit::x86::edx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_new_dict));
                for (int i = first; i < depth; ++i)
                    clear_slot(i);
                push_reg(first, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::NewStruct: {
                const skein::bc::ConstructDesc &desc =
                    proto.constructs[ins.b];
                const int count = static_cast<int>(desc.fields.size());
                const int first = depth - count;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&desc)));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(first)));
                call_addr(fn_address(&skein_jit_new_struct));
                for (int i = first; i < depth; ++i)
                    clear_slot(i);
                push_reg(first, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::GetFieldName: {
                const int value_depth = depth - 1;
                const std::string &name = proto.consts[ins.b].name;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::imm(fn_address(name.c_str())));
                a.mov(asmjit::x86::edx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_get_field));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::GetFieldOrEnum: {
                const skein::bc::FieldOrEnumDesc &desc =
                    proto.field_or_enum[ins.b];
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&desc)));
                a.mov(asmjit::x86::esi, asmjit::imm(loc.line));
                a.mov(asmjit::x86::edx, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_get_field_or_enum));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::GetIndexBaseCached: {
                const skein::bc::IndexDesc &desc =
                    proto.index_ops[ins.b];
                const bool const_key = desc.const_key != UINT32_MAX;
                const int key_depth = depth - 1;
                const int result_depth = const_key ? depth : key_depth;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&desc)));
                switch (desc.base_kind) {
                    case skein::bc::IndexBaseKind::Slot:
                        a.lea(asmjit::x86::rsi,
                              asmjit::x86::ptr(asmjit::x86::rbp,
                                               slot_off(desc.base_index)));
                        break;
                    case skein::bc::IndexBaseKind::Param:
                        a.lea(asmjit::x86::rsi,
                              asmjit::x86::ptr(asmjit::x86::rbp,
                                               param_off(desc.base_index)));
                        break;
                    default:
                        a.xor_(asmjit::x86::esi, asmjit::x86::esi);
                        break;
                }
                if (const_key) {
                    a.mov(asmjit::x86::rdx,
                          asmjit::imm(fn_address(
                              proto.consts[desc.const_key].value.get())));
                } else {
                    a.mov(asmjit::x86::rdx,
                          asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                 stack_off(key_depth)));
                }
                if (desc.read_cache < proto.index_read_ic.size()) {
                    a.mov(asmjit::x86::rcx,
                          asmjit::imm(fn_address(
                              &proto.index_read_ic[desc.read_cache])));
                } else {
                    a.xor_(asmjit::x86::ecx, asmjit::x86::ecx);
                }
                a.mov(asmjit::x86::r8, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_get_index_base_cached));
                if (!const_key)
                    clear_slot(key_depth);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(result_depth)),
                      asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::GetIndexCached: {
                const int container_depth = depth - 2;
                const int index_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(container_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(index_depth)));
                a.mov(asmjit::x86::rdx,
                      asmjit::imm(fn_address(
                          &proto.index_read_ic[ins.b])));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_get_index_cached));
                clear_slot(container_depth);
                clear_slot(index_depth);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(container_depth)),
                      asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::AssignIndex: {
                const skein::bc::IndexDesc &desc =
                    proto.index_ops[ins.b];
                const int count = static_cast<int>(desc.fields.size());
                const int value_depth = depth - count - 1;
                const int indices_start = depth - count;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&desc)));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(indices_start)));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                switch (desc.base_kind) {
                    case skein::bc::IndexBaseKind::Slot:
                        a.lea(asmjit::x86::rcx,
                              asmjit::x86::ptr(asmjit::x86::rbp,
                                               slot_off(desc.base_index)));
                        break;
                    case skein::bc::IndexBaseKind::Param:
                        a.lea(asmjit::x86::rcx,
                              asmjit::x86::ptr(asmjit::x86::rbp,
                                               param_off(desc.base_index)));
                        break;
                    case skein::bc::IndexBaseKind::Upvalue:
                        a.mov(asmjit::x86::rcx,
                              asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                     -kDefOff));
                        break;
                    default:
                        a.xor_(asmjit::x86::ecx, asmjit::x86::ecx);
                        break;
                }
                a.mov(asmjit::x86::r8, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_assign_index));
                for (int i = value_depth; i < depth; ++i)
                    clear_slot(i);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)),
                      asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::CompoundIndex: {
                const skein::bc::IndexDesc &desc =
                    proto.index_ops[ins.b];
                const int count = static_cast<int>(desc.fields.size());
                const int value_depth = depth - count - 1;
                const int indices_start = depth - count;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&desc)));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(indices_start)));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                switch (desc.base_kind) {
                    case skein::bc::IndexBaseKind::Slot:
                        a.lea(asmjit::x86::rcx,
                              asmjit::x86::ptr(asmjit::x86::rbp,
                                               slot_off(desc.base_index)));
                        break;
                    case skein::bc::IndexBaseKind::Param:
                        a.lea(asmjit::x86::rcx,
                              asmjit::x86::ptr(asmjit::x86::rbp,
                                               param_off(desc.base_index)));
                        break;
                    case skein::bc::IndexBaseKind::Upvalue:
                        a.mov(asmjit::x86::rcx,
                              asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                     -kDefOff));
                        break;
                    default:
                        a.xor_(asmjit::x86::ecx, asmjit::x86::ecx);
                        break;
                }
                a.mov(asmjit::x86::r8d, asmjit::imm(ins.flags));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_compound_index));
                for (int i = value_depth; i < depth; ++i)
                    clear_slot(i);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)),
                      asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::IncDecIndex: {
                const skein::bc::IndexDesc &desc =
                    proto.index_ops[ins.b];
                const int count = static_cast<int>(desc.fields.size());
                const int base_depth = depth - count;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&desc)));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(base_depth)));
                a.mov(asmjit::x86::edx,
                      asmjit::imm(ins.a ? 1 : -1));
                a.mov(asmjit::x86::ecx,
                      asmjit::imm((ins.flags & 1) ? 1 : 0));
                switch (desc.base_kind) {
                    case skein::bc::IndexBaseKind::Slot:
                        a.lea(asmjit::x86::r8,
                              asmjit::x86::ptr(asmjit::x86::rbp,
                                               slot_off(desc.base_index)));
                        break;
                    case skein::bc::IndexBaseKind::Param:
                        a.lea(asmjit::x86::r8,
                              asmjit::x86::ptr(asmjit::x86::rbp,
                                               param_off(desc.base_index)));
                        break;
                    case skein::bc::IndexBaseKind::Upvalue:
                        a.mov(asmjit::x86::r8,
                              asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                     -kDefOff));
                        break;
                    default:
                        a.xor_(asmjit::x86::r8d, asmjit::x86::r8d);
                        break;
                }
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_incdec_index));
                for (int i = base_depth; i < depth; ++i)
                    clear_slot(i);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(base_depth)),
                      asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::Neg:
            case skein::bc::Op::Not:
            case skein::bc::Op::BitNot: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::esi, asmjit::imm(loc.line));
                a.mov(asmjit::x86::edx, asmjit::imm(loc.column));
                const skein::bc::Op unary =
                    static_cast<skein::bc::Op>(ins.op);
                uint64_t fn = unary == skein::bc::Op::Neg
                                  ? fn_address(&skein_jit_neg)
                                  : unary == skein::bc::Op::Not
                                        ? fn_address(&skein_jit_not)
                                        : fn_address(&skein_jit_bitnot);
                call_addr(fn);
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::Dup: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                call_addr(fn_address(&skein_jit_dup));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::CmpEq:
            case skein::bc::Op::CmpNe:
            case skein::bc::Op::CmpLt:
            case skein::bc::Op::CmpLe:
            case skein::bc::Op::CmpGt:
            case skein::bc::Op::CmpGe: {
                const int left_depth = depth - 2;
                const int right_depth = depth - 1;
                int op = 4;
                switch (static_cast<skein::bc::Op>(ins.op)) {
                    case skein::bc::Op::CmpGt: op = 1; break;
                    case skein::bc::Op::CmpLt: op = 2; break;
                    case skein::bc::Op::CmpNe: op = 3; break;
                    case skein::bc::Op::CmpEq: op = 4; break;
                    case skein::bc::Op::CmpGe: op = 5; break;
                    case skein::bc::Op::CmpLe: op = 6; break;
                    default: break;
                }
                a.mov(asmjit::x86::rdi, asmjit::imm(op));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(right_depth)));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_compare_value));
                clear_slot(left_depth);
                clear_slot(right_depth);
                push_reg(left_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::Add:
            case skein::bc::Op::Sub:
            case skein::bc::Op::Mul:
            case skein::bc::Op::Div:
            case skein::bc::Op::Mod:
            case skein::bc::Op::BitAnd:
            case skein::bc::Op::BitOr:
            case skein::bc::Op::BitXor:
            case skein::bc::Op::Shl:
            case skein::bc::Op::Shr: {
                const int left_depth = depth - 2;
                const int right_depth = depth - 1;
                const int op = slot_op_char(ins.op);
                const bool int_fast_path = op == '+' || op == '-';
                asmjit::Label slow_binary;
                asmjit::Label binary_done;
                if (int_fast_path) {
                    slow_binary = a.newLabel();
                    binary_done = a.newLabel();

                    a.mov(asmjit::x86::rdi,
                          asmjit::x86::qword_ptr(
                              asmjit::x86::rbp, stack_off(left_depth)));
                    a.mov(asmjit::x86::rsi,
                          asmjit::x86::qword_ptr(
                              asmjit::x86::rbp, stack_off(right_depth)));
                    a.test(asmjit::x86::rdi, asmjit::x86::rdi);
                    a.jz(slow_binary);
                    a.test(asmjit::x86::rsi, asmjit::x86::rsi);
                    a.jz(slow_binary);
                    a.cmp(asmjit::x86::dword_ptr(asmjit::x86::rdi),
                          asmjit::imm(T_int));
                    a.jne(slow_binary);
                    a.cmp(asmjit::x86::dword_ptr(asmjit::x86::rsi),
                          asmjit::imm(T_int));
                    a.jne(slow_binary);

                    a.mov(asmjit::x86::rax,
                          asmjit::x86::qword_ptr(
                              asmjit::x86::rdi,
                              static_cast<int>(
                                  offsetof(int_value_t, value))));
                    if (op == '+') {
                        a.add(asmjit::x86::rax,
                              asmjit::x86::qword_ptr(
                                  asmjit::x86::rsi,
                                  static_cast<int>(
                                      offsetof(int_value_t, value))));
                    } else {
                        a.sub(asmjit::x86::rax,
                              asmjit::x86::qword_ptr(
                                  asmjit::x86::rsi,
                                  static_cast<int>(
                                      offsetof(int_value_t, value))));
                    }
                    a.jo(slow_binary);

                    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                 -kResultOff),
                          asmjit::x86::rax);
                    free_int_slot(left_depth);
                    free_int_slot(right_depth);
                    a.mov(asmjit::x86::rdi,
                          asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                 -kResultOff));
                    call_addr(fn_address(&skein_jit_int));
                    a.mov(asmjit::x86::qword_ptr(
                              asmjit::x86::rbp, stack_off(left_depth)),
                          asmjit::x86::rax);
                    clear_slot(right_depth);
                    a.test(asmjit::x86::rax, asmjit::x86::rax);
                    a.jz(abort_label);
                    a.jmp(binary_done);
                    a.bind(slow_binary);
                }

                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(right_depth)));
                a.mov(asmjit::x86::edx, asmjit::imm(op));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_binary));
                clear_slot(right_depth);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)),
                      asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                if (int_fast_path)
                    a.bind(binary_done);
                break;
            }

            case skein::bc::Op::CmpJump: {
                const int left_depth = depth - 2;
                const int right_depth = depth - 1;
                const int op = ins.flags & 0x7f;
                const bool jump_true = (ins.flags & 0x80) != 0;
                const bool int_fast_path = op >= 1 && op <= 6;
                asmjit::Label slow_compare;
                asmjit::Label compare_done;
                if (int_fast_path) {
                    asmjit::x86::CondCode cond =
                        asmjit::x86::CondCode::kNE;
                    switch (op) {
                        case 1:
                            cond = asmjit::x86::CondCode::kSignedGT;
                            break;
                        case 2:
                            cond = asmjit::x86::CondCode::kSignedLT;
                            break;
                        case 3: cond = asmjit::x86::CondCode::kNE; break;
                        case 4: cond = asmjit::x86::CondCode::kE; break;
                        case 5:
                            cond = asmjit::x86::CondCode::kSignedGE;
                            break;
                        case 6:
                            cond = asmjit::x86::CondCode::kSignedLE;
                            break;
                        default: break;
                    }

                    slow_compare = a.newLabel();
                    compare_done = a.newLabel();
                    a.mov(asmjit::x86::rdi,
                          asmjit::x86::qword_ptr(
                              asmjit::x86::rbp, stack_off(left_depth)));
                    a.mov(asmjit::x86::rsi,
                          asmjit::x86::qword_ptr(
                              asmjit::x86::rbp, stack_off(right_depth)));
                    a.test(asmjit::x86::rdi, asmjit::x86::rdi);
                    a.jz(slow_compare);
                    a.test(asmjit::x86::rsi, asmjit::x86::rsi);
                    a.jz(slow_compare);
                    a.cmp(asmjit::x86::dword_ptr(asmjit::x86::rdi),
                          asmjit::imm(T_int));
                    a.jne(slow_compare);
                    a.cmp(asmjit::x86::dword_ptr(asmjit::x86::rsi),
                          asmjit::imm(T_int));
                    a.jne(slow_compare);
                    a.mov(asmjit::x86::rax,
                          asmjit::x86::qword_ptr(
                              asmjit::x86::rdi,
                              static_cast<int>(
                                  offsetof(int_value_t, value))));
                    a.cmp(asmjit::x86::rax,
                          asmjit::x86::qword_ptr(
                              asmjit::x86::rsi,
                              static_cast<int>(
                                  offsetof(int_value_t, value))));
                    a.set(cond, asmjit::x86::al);
                    a.movzx(asmjit::x86::eax, asmjit::x86::al);
                    a.mov(asmjit::x86::dword_ptr(asmjit::x86::rbp,
                                                 -kStatusOff),
                          asmjit::x86::eax);
                    free_int_slot(left_depth);
                    free_int_slot(right_depth);
                    a.mov(asmjit::x86::eax,
                          asmjit::x86::dword_ptr(asmjit::x86::rbp,
                                                 -kStatusOff));
                    a.test(asmjit::x86::al, asmjit::x86::al);
                    if (jump_true)
                        a.jnz(target_label(ins.b));
                    else
                        a.jz(target_label(ins.b));
                    a.jmp(compare_done);
                    a.bind(slow_compare);
                }

                a.mov(asmjit::x86::edi, asmjit::imm(op));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(right_depth)));
                a.lea(asmjit::x86::rcx,
                      asmjit::x86::ptr(asmjit::x86::rbp, -kStatusOff));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r9d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_compare));
                clear_slot(left_depth);
                clear_slot(right_depth);
                a.cmp(asmjit::x86::dword_ptr(asmjit::x86::rbp,
                                             -kStatusOff),
                      asmjit::imm(0));
                a.je(abort_label);
                a.test(asmjit::x86::al, asmjit::x86::al);
                if (jump_true)
                    a.jnz(target_label(ins.b));
                else
                    a.jz(target_label(ins.b));
                if (int_fast_path)
                    a.bind(compare_done);
                break;
            }

            case skein::bc::Op::Jump:
                a.jmp(target_label(ins.b));
                break;

            case skein::bc::Op::JumpIfFalse:
            case skein::bc::Op::JumpIfTrue: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                call_addr(fn_address(&skein_jit_truthy));
                clear_slot(value_depth);
                a.test(asmjit::x86::al, asmjit::x86::al);
                if (static_cast<skein::bc::Op>(ins.op) ==
                    skein::bc::Op::JumpIfFalse)
                    a.jz(target_label(ins.b));
                else
                    a.jnz(target_label(ins.b));
                break;
            }

            case skein::bc::Op::Pop: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                call_addr(fn_address(&skein_jit_free));
                clear_slot(value_depth);
                break;
            }

            case skein::bc::Op::Call: {
                const int argc = static_cast<int>(ins.b);
                const int callee_depth = depth - argc - 1;
                const asmjit::Label slow_call = a.newLabel();
                const asmjit::Label call_done = a.newLabel();

                /*
                 * Fast path: resolve an already-compiled callee and call its
                 * native entry directly.  This avoids the generic
                 * invoke -> VM bridge -> enter round trip on every call.
                 * The resolver does not consume callee or arguments.
                 */
                a.mov(asmjit::x86::edi, asmjit::imm(loc.line));
                a.mov(asmjit::x86::esi, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_set_call_location));
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(depth - argc)));
                a.mov(asmjit::x86::edx, asmjit::imm(argc));
                a.mov(asmjit::x86::rcx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.lea(asmjit::x86::r8,
                      asmjit::x86::ptr(asmjit::x86::rbp, -kDirectDefOff));
                call_addr(fn_address(&skein_jit_direct_target));
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(slow_call);

#if defined(_WIN32)
                a.mov(asmjit::x86::rcx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             -kDirectDefOff));
                a.lea(asmjit::x86::r8,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(depth - argc)));
                a.mov(asmjit::x86::r9d, asmjit::imm(argc));
#else
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             -kDirectDefOff));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(depth - argc)));
                a.mov(asmjit::x86::ecx, asmjit::imm(argc));
#endif
                a.call(asmjit::x86::rax);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             -kResultOff),
                      asmjit::x86::rax);

                /* Arguments now belong to the callee's frame; callee is ours. */
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)));
                call_addr(fn_address(&skein_jit_free));
                for (int i = callee_depth; i < depth; ++i)
                    clear_slot(i);
                a.mov(asmjit::x86::rax,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             -kResultOff));
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)),
                      asmjit::x86::rax);
                /* The direct JIT-to-JIT path bypasses skein_jit_invoke(),
                 * which polls after a call; keep interrupt handling equal. */
                call_addr(fn_address(&skein_jit_check_interrupt));
                a.test(asmjit::x86::eax, asmjit::x86::eax);
                a.jnz(abort_label);
                a.mov(asmjit::x86::rax,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)));
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                a.jmp(call_done);

                a.bind(slow_call);
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(depth - argc)));
                a.mov(asmjit::x86::edx, asmjit::imm(argc));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_invoke));
                for (int i = callee_depth; i < depth; ++i)
                    clear_slot(i);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)),
                      asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                a.bind(call_done);
                break;
            }

            case skein::bc::Op::CallMethod: {
                const int argc = static_cast<int>(ins.a);
                const int receiver_depth = depth - argc - 1;
                const std::string &method_name = proto.consts[ins.b].name;
                const asmjit::Label slow_method = a.newLabel();
                const asmjit::Label method_done = a.newLabel();
                a.mov(asmjit::x86::edi, asmjit::imm(loc.line));
                a.mov(asmjit::x86::esi, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_set_call_location));
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(receiver_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::imm(fn_address(method_name.c_str())));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(depth - argc)));
                a.mov(asmjit::x86::ecx, asmjit::imm(argc));
                a.mov(asmjit::x86::r8,
                      asmjit::imm(fn_address(&proto.call_method_ic[pc])));
                a.mov(asmjit::x86::r9,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                call_addr(fn_address(&skein_jit_invoke_method_cached));
                a.mov(asmjit::x86::r10,
                      asmjit::imm(fn_address(
                          &skein_jit_method_fallback_marker)));
                a.cmp(asmjit::x86::rax, asmjit::x86::r10);
                a.je(slow_method);
                for (int i = receiver_depth; i < depth; ++i)
                    clear_slot(i);
                push_reg(receiver_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                a.jmp(method_done);

                a.bind(slow_method);
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(receiver_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::imm(fn_address(method_name.c_str())));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(depth - argc)));
                a.mov(asmjit::x86::ecx, asmjit::imm(argc));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r9d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_invoke_method));
                for (int i = receiver_depth; i < depth; ++i)
                    clear_slot(i);
                push_reg(receiver_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                a.bind(method_done);
                break;
            }

            case skein::bc::Op::ListAppendSelf: {
                const int argc = static_cast<int>(ins.a);
                const int callee_depth = depth - argc - 2;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth + 1)));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(depth - argc)));
                a.mov(asmjit::x86::ecx, asmjit::imm(argc));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r9d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_list_append_self));
                for (int i = callee_depth; i < depth; ++i)
                    clear_slot(i);
                push_reg(callee_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::ListConcatSelf: {
                const int left_depth = depth - 2;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth + 1)));
                a.mov(asmjit::x86::edx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_list_concat_self));
                clear_slot(left_depth);
                clear_slot(left_depth + 1);
                push_reg(left_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::StringAppendSelf: {
                const int left_depth = depth - 2;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth + 1)));
                a.mov(asmjit::x86::edx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_string_append_self));
                clear_slot(left_depth);
                clear_slot(left_depth + 1);
                push_reg(left_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::TailCall: {
                const int argc = static_cast<int>(ins.b);
                const int callee_depth = depth - argc - 1;
                const asmjit::Label slow_tail = a.newLabel();

                /*
                 * Deferred expressions must run after the callee returns.
                 * A tail request exits this frame first, so use a regular
                 * call and then leave when the function has defers.
                 */
                if (proto.uses_defers) {
                    a.mov(asmjit::x86::rdi,
                          asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                 stack_off(callee_depth)));
                    a.lea(asmjit::x86::rsi,
                          asmjit::x86::ptr(asmjit::x86::rbp,
                                           stack_off(depth - argc)));
                    a.mov(asmjit::x86::edx, asmjit::imm(argc));
                    a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                    a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                    call_addr(fn_address(&skein_jit_invoke));
                    for (int i = callee_depth; i < depth; ++i)
                        clear_slot(i);
                    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                 -kResultOff),
                          asmjit::x86::rax);
                    a.test(asmjit::x86::rax, asmjit::x86::rax);
                    a.jz(abort_label);
                    a.jmp(epilogue_label);
                    break;
                }

                /* Reuse the current frame when tail-calling this same code.
                 * An active iterator holds per-frame state, so skip the
                 * reuse then; the slow path keeps the VM's behaviour. */
                if (proto.iters.empty()) {
                    a.mov(asmjit::x86::rdi,
                          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                    a.mov(asmjit::x86::rsi,
                          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
                    a.mov(asmjit::x86::rdx,
                          asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                 stack_off(callee_depth)));
                    a.lea(asmjit::x86::rcx,
                          asmjit::x86::ptr(asmjit::x86::rbp, -values_low));
                    a.lea(asmjit::x86::r8,
                          asmjit::x86::ptr(asmjit::x86::rbp,
                                           stack_off(depth - argc)));
                    a.mov(asmjit::x86::r9d, asmjit::imm(argc));
                    call_addr(fn_address(&skein_jit_tail_self));
                    a.test(asmjit::x86::eax, asmjit::x86::eax);
                    a.jz(slow_tail);
                    for (int i = callee_depth; i < depth; ++i)
                        clear_slot(i);
                    a.jmp(proto.code.empty() ? end_label : labels[0]);
                }

                a.bind(slow_tail);
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)));
                a.lea(asmjit::x86::rdx,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(depth - argc)));
                a.mov(asmjit::x86::ecx, asmjit::imm(argc));
                call_addr(fn_address(&skein_jit_tail_request));
                for (int i = callee_depth; i < depth; ++i)
                    clear_slot(i);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             -kResultOff),
                      asmjit::imm(0));
                a.jmp(epilogue_label);
                break;
            }

            case skein::bc::Op::Fail: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::rsi, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_fail));
                clear_slot(value_depth);
                a.jmp(abort_label);
                break;
            }

            case skein::bc::Op::Pass: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::rdx, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_pass));
                clear_slot(value_depth);
                a.jmp(abort_label);
                break;
            }

            case skein::bc::Op::Recover: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             -kHandlerMarkOff));
                a.mov(asmjit::x86::rcx, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_recover));
                clear_slot(value_depth);
                a.jmp(abort_label);
                break;
            }

            case skein::bc::Op::PushFailHandler:
            case skein::bc::Op::PushRecoverHandler: {
                const bool fail =
                    static_cast<skein::bc::Op>(ins.op) ==
                    skein::bc::Op::PushFailHandler;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::esi, asmjit::imm(fail ? 0 : 1));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.b));
                a.mov(asmjit::x86::ecx, asmjit::imm(depth));
                a.mov(asmjit::x86::r8d, asmjit::imm(ins.flags));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_push_handler));
                a.test(asmjit::x86::eax, asmjit::x86::eax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::PopHandler:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             -kHandlerMarkOff));
                call_addr(fn_address(&skein_jit_pop_handler));
                break;

            case skein::bc::Op::EnterHandlerBody:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                call_addr(fn_address(&skein_jit_enter_handler_body));
                break;

            case skein::bc::Op::LeaveHandlerBody:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                call_addr(fn_address(&skein_jit_leave_handler_body));
                break;

            case skein::bc::Op::CheckResult: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(value_depth)));
                a.mov(asmjit::x86::rdx, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_check_result));
                a.test(asmjit::x86::eax, asmjit::x86::eax);
                a.jnz(abort_label);
                break;
            }

            case skein::bc::Op::BindName: {
                const int value_depth = depth - 1;
                const std::string &name = proto.consts[ins.b].name;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                if (ins.a != UINT16_MAX)
                    a.lea(asmjit::x86::rdx,
                          asmjit::x86::ptr(asmjit::x86::rbp,
                                           slot_off(ins.a)));
                else
                    a.xor_(asmjit::x86::edx, asmjit::x86::edx);
                a.mov(asmjit::x86::rcx,
                      asmjit::imm(fn_address(name.c_str())));
                a.mov(asmjit::x86::r8d,
                      asmjit::imm((ins.flags & 1) ? 1 : 0));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_bind_name));
                clear_slot(value_depth);
                a.test(asmjit::x86::eax, asmjit::x86::eax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::DeferPush: {
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rsi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.b));
                a.lea(asmjit::x86::rcx,
                      asmjit::x86::ptr(asmjit::x86::rbp, -values_low));
                a.mov(asmjit::x86::r8,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_defer_push));
                a.test(asmjit::x86::eax, asmjit::x86::eax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::BindFunc: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::rdx,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::ecx, asmjit::imm(ins.b));
                a.mov(asmjit::x86::r8d,
                      asmjit::imm((ins.flags & 1) ? 1 : 0));
                a.mov(asmjit::x86::r9, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_bind_func));
                clear_slot(value_depth);
                push_reg(value_depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::Import: {
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.a));
                if (ins.b != UINT32_MAX)
                    a.lea(asmjit::x86::rdx,
                          asmjit::x86::ptr(asmjit::x86::rbp,
                                           slot_off(ins.b)));
                else
                    a.xor_(asmjit::x86::edx, asmjit::x86::edx);
                a.mov(asmjit::x86::rcx, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_import));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::RuntimeError: {
                const std::string &message = proto.consts[ins.b].name;
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(message.c_str())));
                a.mov(asmjit::x86::rsi, asmjit::imm(pack_loc(loc)));
                call_addr(fn_address(&skein_jit_runtime_error));
                a.jmp(abort_label);
                break;
            }

            case skein::bc::Op::Return: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rax,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             -kResultOff),
                      asmjit::x86::rax);
                clear_slot(value_depth);
                a.jmp(epilogue_label);
                break;
            }

            case skein::bc::Op::PushScope:
                if (proto.uses_names || proto.uses_handlers)
                    call_addr(fn_address(&skein_jit_push_scope));
                if (proto.uses_defers) {
                    a.mov(asmjit::x86::rdi,
                          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                    call_addr(fn_address(&skein_jit_push_defer_scope));
                }
                break;

            case skein::bc::Op::PopScope:
                if (proto.uses_defers) {
                    a.mov(asmjit::x86::rdi,
                          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                    call_addr(fn_address(&skein_jit_pop_defer_scope));
                }
                if (proto.uses_names || proto.uses_handlers) {
                    a.mov(asmjit::x86::rdi,
                          asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                                 -kTsOff));
                    call_addr(fn_address(&skein_jit_pop_scope));
                }
                break;

            default:
                a.jmp(abort_label);
                break;
        }
    }

    a.bind(end_label);
    if (top_level) {
        const int final_depth =
            proto.stack_depth.size() > proto.code.size()
                ? proto.stack_depth[proto.code.size()]
                : -1;
        if (final_depth > 0) {
            const int value_depth = final_depth - 1;
            a.mov(asmjit::x86::rax,
                  asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                         stack_off(value_depth)));
            a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                         -kResultOff),
                  asmjit::x86::rax);
            clear_slot(value_depth);
        } else {
            call_addr(fn_address(&skein_jit_null));
            a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                         -kResultOff),
                  asmjit::x86::rax);
        }
    } else {
        call_addr(fn_address(&skein_jit_null));
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff),
              asmjit::x86::rax);
    }
    a.jmp(epilogue_label);

    a.bind(abort_label);
    if (proto.uses_handlers) {
        const asmjit::Label no_handler = a.newLabel();
        a.mov(asmjit::x86::rdi,
              asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
        a.mov(asmjit::x86::rsi,
              asmjit::x86::qword_ptr(asmjit::x86::rbp, -kHandlerMarkOff));
        call_addr(fn_address(&skein_jit_unwind));
        a.cmp(asmjit::x86::rax, asmjit::imm(-1));
        a.je(no_handler);
        for (size_t i = 0; i < handler_targets.size(); ++i) {
            a.cmp(asmjit::x86::rax, asmjit::imm(handler_targets[i]));
            a.je(handler_dispatch[i]);
        }
        a.jmp(no_handler);

        for (size_t i = 0; i < handler_targets.size(); ++i) {
            a.bind(handler_dispatch[i]);
            const uint32_t target = handler_targets[i];
            const int target_depth = handler_depths[i];
            const int start = target_depth - 1;
            if (target < labels.size() && start >= 0 &&
                start < static_cast<int>(proto.max_stack)) {
                const uint32_t count =
                    proto.max_stack - static_cast<uint32_t>(start);
                a.lea(asmjit::x86::rdi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(start)));
                a.mov(asmjit::x86::esi, asmjit::imm(count));
                call_addr(fn_address(&skein_jit_drop_slots));
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                call_addr(fn_address(&skein_jit_take_unwind_value));
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(start)),
                      asmjit::x86::rax);
            } else {
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
                call_addr(fn_address(&skein_jit_take_unwind_value));
                a.mov(asmjit::x86::rdi, asmjit::x86::rax);
                call_addr(fn_address(&skein_jit_free));
            }
            a.jmp(target < labels.size() ? labels[target] : end_label);
        }
        a.bind(no_handler);
    }
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff),
          asmjit::imm(0));

    a.bind(epilogue_label);
    {
        const asmjit::Label defer_done = a.newLabel();
        a.mov(asmjit::x86::rdi,
              asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
        call_addr(fn_address(&skein_jit_run_frame_defers));
        a.test(asmjit::x86::eax, asmjit::x86::eax);
        a.jz(defer_done);
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff),
              asmjit::imm(0));
        a.bind(defer_done);
    }
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    a.mov(asmjit::x86::rsi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kRootMarkOff));
    call_addr(fn_address(&skein_jit_pop_roots));
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    a.mov(asmjit::x86::rsi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kCellMarkOff));
    call_addr(fn_address(&skein_jit_close_cells));
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    a.mov(asmjit::x86::rsi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kHandlerMarkOff));
    call_addr(fn_address(&skein_jit_discard_handlers));
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    a.lea(asmjit::x86::rsi,
          asmjit::x86::ptr(asmjit::x86::rbp, -values_low));
    a.mov(asmjit::x86::edx, asmjit::imm(value_count));
    call_addr(fn_address(&skein_jit_cleanup));
    a.mov(asmjit::x86::rax,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff));
#if defined(_WIN32)
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kSavedRdiOff));
    a.mov(asmjit::x86::rsi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kSavedRsiOff));
#endif
    a.mov(asmjit::x86::rsp, asmjit::x86::rbp);
    a.pop(asmjit::x86::rbp);
    a.ret();

    if (dump) {
        std::fprintf(stderr, "[jit] compile %s\n%s\n",
                     proto.name.empty() ? "<anonymous>" : proto.name.c_str(),
                     logger.data());
    }

    void (*entry)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mutex);
        asmjit::Error err = rt.add(&entry, &code);
        if (err != asmjit::kErrorOk) {
            if (read_bool_env("SKEIN_JIT_DEBUG", false))
                std::fprintf(stderr, "[jit] asmjit error: %s\n",
                             asmjit::DebugUtils::errorAsString(err));
            return nullptr;
        }
    }

    JitCode *result = new JitCode;
    result->proto = &proto;
    result->runtime = &rt;
    result->entry = entry;
    result->size = code.codeSize();
    result->top_level = top_level;
    return result;
}

value_t *enter(JitCode *code, thread_state *ts, function_def *def,
               value_t **args, uint32_t argc) {
    if (!code || !code->entry || !ts)
        return nullptr;
    using Entry = value_t *(*)(thread_state *, function_def *, value_t **,
                               uint32_t);
    Entry entry = reinterpret_cast<Entry>(code->entry);
    return entry(ts, def, args, argc);
}

void *entry(JitCode *code) {
    return code ? reinterpret_cast<void *>(code->entry) : nullptr;
}

void release(JitCode *code) {
    if (!code)
        return;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mutex);
        if (code->runtime && code->entry)
            code->runtime->release(code->entry);
    }
    delete code;
}

} // namespace jit
} // namespace skein
