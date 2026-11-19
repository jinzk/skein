#ifndef SKEIN_CONTROL_RUNTIME_H
#define SKEIN_CONTROL_RUNTIME_H

#include "vm_eval.h"

/*
 * Value-only control flow primitives shared by the AST evaluator and the
 * bytecode executor.  These do not depend on the AST, but they do operate on
 * the active eval_context, so they live in skein_value_runtime alongside the
 * scope and symbol tables.
 */

void set_eval_control(enum eval_control_kind kind, struct value_t *value);
struct value_t *take_eval_control(enum eval_control_kind kind);
bool eval_control_pending();
void eval_finish_statement_control();

void eval_capture_begin(struct eval_capture_snapshot &snapshot);
struct eval_capture_result eval_capture_end(
    const struct eval_capture_snapshot &snapshot);
/* Build a "runtime" error from captured diagnostics state. */
struct value_t *eval_build_error(const std::string &message,
                                 const std::string &source, int line,
                                 int column,
                                 const std::vector<error_frame> &trace);
/* Attach the current location and call stack to an error that has none. */
void eval_annotate_error(struct value_t *error);
void eval_raise(struct value_t *error);
void eval_request_close();

#endif
