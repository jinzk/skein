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
void eval_raise(struct value_t *error);
void eval_request_close();

#endif
