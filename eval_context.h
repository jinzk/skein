#ifndef CALC_EVAL_CONTEXT_H
#define CALC_EVAL_CONTEXT_H

#include "scopestack.h"

enum eval_control_kind {
    EVAL_CONTROL_NONE,
    EVAL_CONTROL_FAIL,
    EVAL_CONTROL_RECOVER
};

struct eval_context {
    scope_context scopes;
    eval_control_kind control = EVAL_CONTROL_NONE;
    value_t *control_value = NULL;
    int handler_depth = 0;
    const char *pending_label = NULL;
    int runtime_error_count = 0;
    bool allow_await = false;

    eval_context() = default;
    eval_context(const eval_context &) = delete;
    eval_context &operator=(const eval_context &) = delete;
    ~eval_context();
};

eval_context &active_eval_context();
void set_active_eval_context(eval_context *context);

#endif
