#include "control_runtime.h"

#include "eval_context.h"
#include "symbol.h"
#include "valuetype.h"

#include <string>

static void clear_eval_control() {
    freeval(active_eval_context().control_value);
    active_eval_context().control_value = NULL;
    active_eval_context().control = EVAL_CONTROL_NONE;
}

void set_eval_control(enum eval_control_kind kind,
                             struct value_t *value) {
    clear_eval_control();
    active_eval_context().control = kind;
    active_eval_context().control_value = value;
}

struct value_t *take_eval_control(enum eval_control_kind kind) {
    if (active_eval_context().control != kind)
        return NULL;
    struct value_t *value = active_eval_context().control_value;
    active_eval_context().control_value = NULL;
    active_eval_context().control = EVAL_CONTROL_NONE;
    return value;
}

bool eval_control_pending() {
    return active_eval_context().control != EVAL_CONTROL_NONE;
}

void eval_finish_statement_control() {
    if (active_eval_context().control == EVAL_CONTROL_NONE)
        return;
    if (active_eval_context().control == EVAL_CONTROL_CLOSE) {
        clear_eval_control();
        return;
    }
    if (active_eval_context().control == EVAL_CONTROL_FAIL) {
        if (active_eval_context().control_value)
            yyerror("uncaught fail: %s",
                    error_message_of(active_eval_context().control_value).c_str());
        else
            yyerror("uncaught fail");
    } else {
        yyerror("recover outside error handler");
    }
    clear_eval_control();
}

void eval_capture_begin(struct eval_capture_snapshot &snapshot) {
    eval_context &context = active_eval_context();
    snapshot.capture_errors = context.capture_errors;
    snapshot.has_captured_error = context.has_captured_error;
    snapshot.captured_error = context.captured_error;
    snapshot.runtime_error_count = context.runtime_error_count;
    context.capture_errors = true;
    context.has_captured_error = false;
    context.captured_error.clear();
}

struct eval_capture_result eval_capture_end(
    const struct eval_capture_snapshot &snapshot) {
    eval_context &context = active_eval_context();
    bool has_error = context.has_captured_error;
    std::string message = context.captured_error;

    context.capture_errors = snapshot.capture_errors;
    context.has_captured_error = snapshot.has_captured_error;
    context.captured_error = snapshot.captured_error;
    context.runtime_error_count = snapshot.runtime_error_count;

    struct eval_capture_result result;
    if (context.control == EVAL_CONTROL_FAIL) {
        result.failed = true;
        result.error = normalize_error(take_eval_control(EVAL_CONTROL_FAIL),
                                       "fail");
    } else if (has_error) {
        result.failed = true;
        result.error = newerrorval(
            "runtime", message.empty() ? "runtime error" : message);
    }
    return result;
}

void eval_raise(struct value_t *error) {
    set_eval_control(EVAL_CONTROL_FAIL, normalize_error(error, "fail"));
}

void eval_request_close() {
    clear_eval_control();
    active_eval_context().control = EVAL_CONTROL_CLOSE;
}
