#ifndef SKEIN_JIT_H
#define SKEIN_JIT_H

#include "bytecode.h"
#include "valuetype.h"

#include <cstddef>
#include <cstdint>
#include <string>

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
    size_t scope_mark = 0;
    std::string saved_module;
    std::string saved_source;
    bool top_level = false;
    bool tail_call = false;
    bool depth_counted = false;
    bool entered = false;
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

/* Release executable memory and the JitCode object. */
void release(JitCode *code);

} // namespace jit
} // namespace skein

#endif
