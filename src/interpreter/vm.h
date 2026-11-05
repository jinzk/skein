#ifndef SKEIN_VM_H
#define SKEIN_VM_H

#include "valuetype.h"

#include <memory>

struct ast;
struct function_code;
struct function_def;

/* Enabled by SKEIN_VM=1 or --vm; disabled by default during bring-up. */
bool skein_vm_enabled();
void skein_vm_set_enabled(bool enabled);

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

/* Drop cached bytecode before a function_code is freed. */
void skein_vm_forget_function_code(struct function_code *code);

#endif
