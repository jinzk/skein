#ifndef SKEIN_VM_H
#define SKEIN_VM_H

#include "pattern.h"
#include "valuetype.h"

#include <memory>

struct ast;
struct function_code;
struct function_def;

enum class ParamPatternResult {
    Matched,
    NotMatched,
    Unsupported
};

/* Enabled by default; --no-vm or SKEIN_VM=0 selects the AST backend. */
bool skein_vm_enabled();
/*
 * Turn the VM on or off.  This only affects work that has not been compiled
 * yet: functions with a cached Proto continue to run through the VM even
 * after the VM is disabled, because their source AST may already be gone.
 */
void skein_vm_set_enabled(bool enabled);

/* Enable/disable VM statistics and print the final report. */
void skein_vm_set_stats(bool enabled);
void skein_vm_stats_report();

/*
 * Execute a top-level statement through the VM.  Falls back to eval() when
 * the VM is disabled or the statement cannot be compiled safely.
 */
struct value_t *skein_vm_eval_statement(struct ast *statement);

/*
 * Try to execute a function body through the VM.  The caller must already
 * have pushed the function scope and bound the parameters.
 *
 * Returns false if the body could not be compiled and the caller should use
 * the AST interpreter instead.  Returns true when the VM ran; `out` then
 * contains the body result/control value (possibly NULL after an error).
 */
bool skein_vm_try_execute_function_body(
    const std::shared_ptr<function_def> &def, struct value_t *&out);

/* Try to evaluate one default-parameter expression in the current scope. */
bool skein_vm_try_eval_default(const std::shared_ptr<function_def> &def,
                               int index, struct value_t *&out);

/*
 * Match one destructuring parameter using its compiled descriptor.
 *
 * `Unsupported` means the pattern has not been compiled (or compilation
 * failed); the caller should fall back to the AST matcher.  `NotMatched`
 * leaves any partial bindings in `bindings`; the caller owns and frees them.
 */
ParamPatternResult skein_vm_match_parameter_pattern(
    const std::shared_ptr<function_def> &def, int index,
    struct value_t *value, std::vector<pattern_binding> &bindings);

/* Drop cached bytecode before a function_code is freed. */
void skein_vm_forget_function_code(struct function_code *code);
/* Clear VM-side function caches before runtime/type teardown. */
void skein_vm_shutdown();

#endif
