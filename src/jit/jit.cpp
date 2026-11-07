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
SKEIN_JIT_SYSV value_t *skein_jit_bitnot(value_t *value, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_load_name(const skein::bc::Proto *proto,
                             uint32_t name_index, int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_store_name(const skein::bc::Proto *proto,
                              uint32_t name_index, value_t *value, int line,
                              int column);
SKEIN_JIT_SYSV void skein_jit_push_scope();
SKEIN_JIT_SYSV void skein_jit_pop_scope();
SKEIN_JIT_SYSV value_t *skein_jit_new_list(value_t **items, uint32_t count, int line,
                            int column);
SKEIN_JIT_SYSV value_t *skein_jit_new_dict(value_t **items, uint32_t count, int line,
                            int column);
SKEIN_JIT_SYSV int skein_jit_bind_patterns(function_def *def, value_t **values,
                            uint32_t argc);
SKEIN_JIT_SYSV value_t *skein_jit_invoke(value_t *callee, value_t **args, uint32_t argc,
                          int line, int column);
SKEIN_JIT_SYSV value_t *skein_jit_tail_invoke(value_t *callee, value_t **args,
                               uint32_t argc, int line, int column);
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
SKEIN_JIT_SYSV value_t *skein_jit_assign_index(const skein::bc::IndexDesc *desc,
                                value_t **indices, uint32_t count,
                                value_t *value, int line, int column);
SKEIN_JIT_SYSV int skein_jit_enter(skein::jit::thread_state *ts, function_def *def);
SKEIN_JIT_SYSV void skein_jit_leave(skein::jit::thread_state *ts);
SKEIN_JIT_SYSV void skein_jit_cleanup(skein::jit::thread_state *ts, value_t **values,
                       uint32_t count);
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
constexpr int kHeaderSize = 64;
#else
constexpr int kRootMarkOff = 40;
constexpr int kHeaderSize = 48;
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

} // namespace

void set_invoke(invoke_fn fn) { g_invoke = fn; }
invoke_fn invoke() { return g_invoke; }
void set_tail_invoke(invoke_fn fn) { g_tail_invoke = fn; }
invoke_fn tail_invoke() { return g_tail_invoke; }
void set_construct(construct_fn fn) { g_build_struct = fn; }
construct_fn construct() { return g_build_struct; }
void set_bind_frame(bind_frame_fn fn) { g_bind_frame = fn; }
bind_frame_fn bind_frame() { return g_bind_frame; }

bool available() {
    static int state = -1;
    if (state < 0)
        state = read_bool_env("SKEIN_JIT", false) ? 1 : 0;
    return state != 0;
}

bool can_compile(const skein::bc::Proto &proto, bool top_level,
                 bool pattern_frame) {
    const bool name_mode =
        (top_level && proto.uses_names && proto.nparams == 0) ||
        (pattern_frame && proto.uses_names);
    if ((!proto.params_direct && !name_mode) ||
        (proto.uses_names && !name_mode) || proto.has_closures ||
        proto.uses_defers || proto.uses_handlers || proto.is_generator ||
        proto.is_async || proto.variadic || !proto.upvalues.empty())
        return false;
    if (proto.nparams != proto.required_params)
        return false;
    if (proto.stack_depth.size() < proto.code.size())
        return false;

    const uint64_t value_count =
        static_cast<uint64_t>(proto.nparams) + proto.slots.size() +
        proto.max_stack;
    if (value_count > 512)
        return false;

    for (size_t pc = 0; pc < proto.code.size(); ++pc) {
        const skein::bc::Instr &ins = proto.code[pc];
        switch (static_cast<skein::bc::Op>(ins.op)) {
            case skein::bc::Op::Nop:
            case skein::bc::Op::LoadNull:
            case skein::bc::Op::LoadParam:
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
            case skein::bc::Op::CmpJump:
            case skein::bc::Op::Neg:
            case skein::bc::Op::Not:
            case skein::bc::Op::BitNot:
            case skein::bc::Op::Jump:
            case skein::bc::Op::JumpIfFalse:
            case skein::bc::Op::JumpIfTrue:
            case skein::bc::Op::Call:
            case skein::bc::Op::TailCall:
            case skein::bc::Op::Return:
            case skein::bc::Op::PushScope:
            case skein::bc::Op::PopScope:
            case skein::bc::Op::Pop:
                break;
            case skein::bc::Op::LoadConst:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Value ||
                    !proto.consts[ins.b].value)
                    return false;
                break;
            case skein::bc::Op::LoadGlobal:
            case skein::bc::Op::StoreGlobal:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return false;
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
            case skein::bc::Op::LoadSlot:
            case skein::bc::Op::StoreSlot:
            case skein::bc::Op::ClearSlot:
                if (ins.b >= proto.slots.size())
                    return false;
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
                    return false;
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
                    return false;
                break;
            case skein::bc::Op::GetIndexCached:
                if (ins.b >= proto.index_read_ic.size())
                    return false;
                break;
            case skein::bc::Op::AssignIndex: {
                if (ins.b >= proto.index_ops.size())
                    return false;
                const skein::bc::IndexDesc &desc =
                    proto.index_ops[ins.b];
                if (desc.fields.empty() ||
                    (desc.base_kind != skein::bc::IndexBaseKind::Global &&
                     desc.base_kind != skein::bc::IndexBaseKind::Name))
                    return false;
                break;
            }
            case skein::bc::Op::LoadName:
            case skein::bc::Op::StoreName:
                if (!name_mode || ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Name)
                    return false;
                break;
            default:
                return false;
        }
    }
    return true;
}

JitCode *compile(const skein::bc::Proto &proto, bool top_level,
                 bool pattern_frame) {
    if (!available() || !can_compile(proto, top_level, pattern_frame))
        return nullptr;

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

    const uint32_t value_count =
        proto.nparams + static_cast<uint32_t>(proto.slots.size()) +
        proto.max_stack;
    const int values_low =
        kHeaderSize + static_cast<int>(value_count) * 8;
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
    a.xor_(asmjit::x86::eax, asmjit::x86::eax);
    for (uint32_t i = 0; i < value_count; ++i)
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                     value_off(i)),
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
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(constant.value.get())));
                call_addr(fn_address(&skein_jit_dup));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::LoadParam:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             param_off(ins.b)));
                call_addr(fn_address(&skein_jit_dup));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

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
                push_reg(depth, asmjit::x86::rax);
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

            case skein::bc::Op::ClearSlot:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             slot_off(ins.b)));
                call_addr(fn_address(&skein_jit_free));
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             slot_off(ins.b)),
                      asmjit::imm(0));
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
                a.mov(asmjit::x86::edx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.column));
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
                a.mov(asmjit::x86::edx, asmjit::imm(count));
                a.mov(asmjit::x86::rcx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r9d, asmjit::imm(loc.column));
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
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(right_depth)));
                const int op = slot_op_char(ins.op);
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
                break;
            }

            case skein::bc::Op::CmpJump: {
                const int left_depth = depth - 2;
                const int right_depth = depth - 1;
                const int op = ins.flags & 0x7f;
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
                if ((ins.flags & 0x80) != 0)
                    a.jnz(target_label(ins.b));
                else
                    a.jz(target_label(ins.b));
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
                break;
            }

            case skein::bc::Op::TailCall: {
                const int argc = static_cast<int>(ins.b);
                const int callee_depth = depth - argc - 1;
                const asmjit::Label slow_tail = a.newLabel();

                /* Reuse the current frame when tail-calling this same code. */
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
                if (proto.uses_names)
                    call_addr(fn_address(&skein_jit_push_scope));
                break;

            case skein::bc::Op::PopScope:
                if (proto.uses_names)
                    call_addr(fn_address(&skein_jit_pop_scope));
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
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff),
          asmjit::imm(0));

    a.bind(epilogue_label);
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    a.mov(asmjit::x86::rsi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kRootMarkOff));
    call_addr(fn_address(&skein_jit_pop_roots));
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
