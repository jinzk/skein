#ifndef SKEIN_JIT_H
#define SKEIN_JIT_H

#include "bytecode.h"
#include "valuetype.h"
#include "vm_eval.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct eval_context;
struct function_def;

namespace skein {
namespace jit {

/*
 * Opaque compiled representation of one Proto.  Owned by the VM bridge's
 * per-function cache; release() unmaps the executable code.
 */
struct JitCode;

/*
 * Per-call state passed to generated code.  The struct lives on the caller's
 * C++ stack for the duration of the JIT call, so it is also used by nested
 * JIT entries.
 */
struct thread_state {
    eval_context *ctx = nullptr;
    int max_depth = 0;
    int saved_handler_depth = 0;
    int handler_body_depth = 0;
    size_t scope_mark = 0;
    /* Scope depth after the frame's own (prologue) scope was pushed. */
    size_t frame_scope_mark = 0;
    value_t *unwind_value = nullptr;
    std::string saved_package;
    std::string saved_source;
    std::string root_module;
    std::string root_source;
    bool root_set = false;
    bool saved_package_valid = false;
    bool saved_source_valid = false;
    bool top_level = false;
    bool tail_call = false;
    bool depth_counted = false;
    bool diag_pushed = false;
    int retained_diag_frames = 0;
    bool entered = false;

    /* Nested direct JIT calls reuse this state; save one entry per call. */
    struct call_save {
        int saved_handler_depth = 0;
        int handler_body_depth = 0;
        size_t scope_mark = 0;
        size_t frame_scope_mark = 0;
        size_t frame_defer_mark = 0;
        std::string saved_package;
        std::string saved_source;
        bool entered = false;
        bool depth_counted = false;
        bool diag_pushed = false;
        bool tail_call = false;
        bool package_valid = false;
        bool source_valid = false;
    };
    std::vector<call_save> saves;

    /*
     * Cells opened by JIT frames on this thread.  Each generated frame keeps
     * a mark (header slot) and closes the cells it created before freeing its
     * frame values.
     */
    std::vector<std::shared_ptr<struct upvalue_cell>> open_cells;

    /*
     * Handlers pushed by JIT frames on this thread.  Each frame stores a mark
     * in its header and discards the entries it created on exit.  Captured
     * snapshots are plain state (not values) and are ended in LIFO order.
     */
    struct handler_state {
        int kind = 0; /* 0 = fail, 1 = recover */
        int32_t target = -1;
        int32_t stack_depth = 0;
        size_t scope_depth = 0;
        int body_depth = 0;
        int eval_handler_depth = 0;
        bool capture = false;
        eval_capture_snapshot snapshot;
    };
    std::vector<handler_state> handlers;

    /*
     * Deferred expressions per scope.  A JIT frame records its base depth and
     * runs every scope above it on exit; block PushScope/PopScope add/remove
     * one vector each.
     */
    struct defer_record {
        skein::bc::ProtoPtr proto;
        std::vector<std::shared_ptr<struct upvalue_cell>> upvalues;
    };
    std::vector<std::vector<defer_record>> defer_scopes;
    size_t frame_defer_mark = 0;

    /* Pending trampoline request produced by a non-self TAIL_CALL. */
    value_t *tail_callee = nullptr;
    std::vector<value_t *> tail_args;
    uint32_t tail_argc = 0;
    bool tail_pending = false;
};

/*
 * Callback used by generated CALL instructions.  The VM bridge installs an
 * implementation that tries a direct JIT entry first and falls back to the
 * normal VM/evaluator call path.  The callback takes ownership of `callee`
 * and every arg pointer.
 */
using invoke_fn = value_t *(*)(value_t *callee, value_t **args,
                               uint32_t argc);

/*
 * Struct constructor callback.  The VM bridge implements this on top of the
 * interpreter's struct metadata; the JIT only passes the descriptor and the
 * field values (ownership transfers to the callback).
 */
using construct_fn = value_t *(*)(const skein::bc::ConstructDesc &desc,
                                  value_t **fields);

/*
 * Pattern-parameter binder.  Called from the JIT prologue for functions
 * whose parameters use destructuring; it must push the function scope and
 * bind names before the body runs.  Values are borrowed and may be consumed
 * by setting entries to null.
 */
using bind_frame_fn = int (*)(function_def *def, value_t **values,
                              uint32_t argc);

void set_invoke(invoke_fn fn);
invoke_fn invoke();
void set_tail_invoke(invoke_fn fn);
invoke_fn tail_invoke();
void set_construct(construct_fn fn);
construct_fn construct();
void set_bind_frame(bind_frame_fn fn);
bind_frame_fn bind_frame();

/*
 * Generator hooks installed by the VM bridge.  `step` advances a generator
 * value and returns false on error; `done` reports exhaustion.
 */
using generator_test_fn = bool (*)(value_t *value);
using generator_step_fn = bool (*)(value_t *value, value_t *&out, bool &done);
void set_generator_test(generator_test_fn fn);
generator_test_fn generator_test();
void set_generator_step(generator_step_fn fn);
generator_step_fn generator_step();

/* Enum construction (field-or-enum resolution) installed by the VM bridge. */
using build_enum_fn = value_t *(
    *)(const char *source, const char *name, const char *variant,
       std::vector<std::pair<std::string, value_t *>> &fields,
       const std::vector<bool> &spread);
void set_build_enum(build_enum_fn fn);
build_enum_fn build_enum();

/*
 * Runs a scope's deferred expressions (LIFO) installed by the VM bridge.  The
 * callback mirrors the VM's run_defer_scope, including eval-control handling.
 */
using run_defer_scope_fn = void (*)(
    std::vector<thread_state::defer_record> &records);
void set_run_defer_scope(run_defer_scope_fn fn);
run_defer_scope_fn run_defer_scope();

/*
 * Fast-path call resolver installed by the VM bridge.  Returns a native entry
 * for an already-compiled callee, or null when the generic invoke path must
 * be used.  It must not consume the callee or its arguments.
 */
using direct_fn = void *(*)(value_t *callee, value_t **args, uint32_t argc,
                            thread_state *ts, function_def **def_out);
void set_direct(direct_fn fn);
direct_fn direct();

/* True when the runtime switch (SKEIN_JIT=1) enables the JIT. */
bool available();

/* Compile-time and Proto-level eligibility check. */
bool can_compile(const skein::bc::Proto &proto, bool top_level = false,
                 bool pattern_frame = false);

/*
 * Compile one Proto.  `top_level` selects statement semantics for the
 * implicit end-of-code result (the VM returns the top operand for statements,
 * while a function fallthrough returns null).
 */
JitCode *compile(const skein::bc::Proto &proto, bool top_level = false,
                 bool pattern_frame = false);

/* Execute a compiled Proto.  Takes ownership of the argument pointers. */
value_t *enter(JitCode *code, thread_state *ts, function_def *def,
               value_t **args, uint32_t argc);

/* Raw native entry point of compiled code, for direct JIT-to-JIT calls. */
void *entry(JitCode *code);

/* Release executable memory and the JitCode object. */
void release(JitCode *code);

} // namespace jit
} // namespace skein

#endif
