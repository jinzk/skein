#ifndef SKEIN_EVAL_CONTEXT_H
#define SKEIN_EVAL_CONTEXT_H

#include "scopestack.h"

#include <string>
#include <vector>

struct ast;

enum eval_control_kind {
    EVAL_CONTROL_NONE,
    EVAL_CONTROL_FAIL,
    EVAL_CONTROL_RECOVER,
    /* Internal, non-catchable unwind used by generator.close. */
    EVAL_CONTROL_CLOSE
};

struct eval_context {
    scope_context scopes;
    eval_control_kind control = EVAL_CONTROL_NONE;
    value_t *control_value = NULL;
    int handler_depth = 0;
    const char *pending_label = NULL;
    int runtime_error_count = 0;
    /* VM frame depth; kept separate from the AST call-depth counter. */
    int vm_call_depth = 0;
    /* Pending VM defer records across the current fiber's VM frames. */
    int vm_defer_count = 0;
    bool capture_errors = false;
    bool has_captured_error = false;
    std::string captured_error;
    std::vector<std::vector<struct ast *>> defer_stack;
    size_t gc_root_mark = 0;

    eval_context();
    eval_context(const eval_context &) = delete;
    eval_context &operator=(const eval_context &) = delete;
    ~eval_context();
};

eval_context &active_eval_context();
void set_active_eval_context(eval_context *context);

#endif
