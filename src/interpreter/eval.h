#ifndef _EVAL_H_
#define _EVAL_H_
#include "ast.h"
#include "vm_eval.h"

#include <memory>
#include <string>
#include <vector>

struct scope_t;

struct value_t *eval(struct ast *);
struct value_t *call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args);
/* `launch callee(args)`; evaluates args left-to-right on a new fiber. */
struct value_t *launch_value(struct value_t *callee, struct ast *args_ast);

/* Suppress and record runtime errors until eval_capture_end. */
void eval_capture_begin(struct eval_capture_snapshot &snapshot);
/* Restore the previous state and return any fail or runtime error. */
struct eval_capture_result eval_capture_end(
    const struct eval_capture_snapshot &snapshot);
/* Raise a structured error through the normal fail control flow.
   Takes ownership of `error`. */
void eval_raise(struct value_t *error);
/* Request an uncatchable generator close unwind. */
void eval_request_close();

void eval_pushscope();
void eval_push_isolated_scope();
void eval_push_closure_scope(const std::shared_ptr<struct scope_t> &parent);
void eval_popscope();
void eval_begin_root_defers();
void eval_run_root_defers();

bool eval_control_pending();
void eval_finish_statement_control();

#endif