#include "control_runtime.h"

#include "diagnostics.h"
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
        value_t *error = active_eval_context().control_value;
        ++active_eval_context().runtime_error_count;
        if (error && error->type == T_error) {
            const std::string message =
                "uncaught fail: " + error_message_of(error);
            diag_report_error(*(const struct error_value_t *)error,
                              "runtime error", message.c_str());
        } else {
            diag_report(g_error_line, g_error_column, "runtime error",
                        "uncaught fail");
        }
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
    snapshot.captured_source = context.captured_source;
    snapshot.captured_line = context.captured_line;
    snapshot.captured_column = context.captured_column;
    snapshot.captured_trace = context.captured_trace;
    snapshot.runtime_error_count = context.runtime_error_count;
    context.capture_errors = true;
    context.has_captured_error = false;
    context.captured_error.clear();
    context.captured_source.clear();
    context.captured_line = 0;
    context.captured_column = 0;
    context.captured_trace.clear();
}

struct eval_capture_result eval_capture_end(
    const struct eval_capture_snapshot &snapshot) {
    eval_context &context = active_eval_context();
    const bool has_error = context.has_captured_error;
    const std::string captured_message = context.captured_error;
    const std::string captured_source = context.captured_source;
    const int captured_line = context.captured_line;
    const int captured_column = context.captured_column;
    const std::vector<error_frame> captured_trace = context.captured_trace;

    context.capture_errors = snapshot.capture_errors;
    context.has_captured_error = snapshot.has_captured_error;
    context.captured_error = snapshot.captured_error;
    context.captured_source = snapshot.captured_source;
    context.captured_line = snapshot.captured_line;
    context.captured_column = snapshot.captured_column;
    context.captured_trace = snapshot.captured_trace;
    context.runtime_error_count = snapshot.runtime_error_count;

    struct eval_capture_result result;
    if (context.control == EVAL_CONTROL_FAIL) {
        result.failed = true;
        result.error = normalize_error(take_eval_control(EVAL_CONTROL_FAIL),
                                       "fail");
    } else if (has_error) {
        result.failed = true;
        result.error = eval_build_error(
            captured_message.empty() ? std::string("runtime error")
                                     : captured_message,
            captured_source, captured_line, captured_column, captured_trace);
    }
    return result;
}

value_t *eval_build_error(const std::string &message,
                          const std::string &source, int line, int column,
                          const std::vector<error_frame> &trace) {
    value_t *error = newerrorval("runtime", message);
    error_set_trace(error, source, line, column, trace);
    return error;
}

void eval_annotate_error(struct value_t *error) {
    if (!error || error->type != T_error || error_has_trace(error))
        return;
    std::vector<error_frame> trace;
    diag_snapshot_trace(trace);
    error_set_trace(error, diag_current_source_name(), g_error_line,
                    g_error_column, trace);
}

void eval_raise(struct value_t *error) {
    error = normalize_error(error, "fail");
    eval_annotate_error(error);
    set_eval_control(EVAL_CONTROL_FAIL, error);
}

void eval_request_close() {
    clear_eval_control();
    active_eval_context().control = EVAL_CONTROL_CLOSE;
}
