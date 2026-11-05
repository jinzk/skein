#include "eval.h"
#include "diagnostics.h"
#include "dict.h"
#include "eval_context.h"
#include "eval_helpers.h"
#include "fiber.h"
#include "format.h"
#include "package.h"
#include "pattern.h"
#include "cycle.h"
#include "platform.h"
#include "repl.h"
#include "runtime_context.h"
#include "scopestack.h"
#include "structeval.h"
#include "structtype.h"
#include "symbol.h"
#include "valuetype.h"
#include "unicode.h"

#include <cstring>
#include <unordered_set>

#include "eval_internal.h"

eval_context &eval_ctx() {
    return active_eval_context();
}

const char *take_pending_label() {
    eval_context &context = eval_ctx();
    const char *label = context.pending_label;
    context.pending_label = NULL;
    return label;
}

bool label_directly_applies(const struct ast *node) {
    if (!node) return false;
    return node->nodetype == 'P' || node->nodetype == 'F' ||
           node->nodetype == 'W' || node->nodetype == 'E' ||
           node->nodetype == 'S';
}

bool control_matches_label(struct value_t *value, const char *label) {
    if (!value || (value->type != T_break && value->type != T_continue))
        return false;
    struct control_value_t *control = (struct control_value_t *)value;
    if (!control->label)
        return true;
    return label && std::strcmp(control->label, label) == 0;
}

#include <climits>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>



struct value_t *new_int_checked(__int128 n) {
    if (n < LLONG_MIN || n > LLONG_MAX) {
        yyerror("integer overflow");
        return NULL;
    }
    return newintval((long long)n);
}


static void clear_eval_control() {
    freeval(eval_ctx().control_value);
    eval_ctx().control_value = NULL;
    eval_ctx().control = EVAL_CONTROL_NONE;
}

void set_eval_control(enum eval_control_kind kind,
                             struct value_t *value) {
    clear_eval_control();
    eval_ctx().control = kind;
    eval_ctx().control_value = value;
}

struct value_t *take_eval_control(enum eval_control_kind kind) {
    if (eval_ctx().control != kind)
        return NULL;
    struct value_t *value = eval_ctx().control_value;
    eval_ctx().control_value = NULL;
    eval_ctx().control = EVAL_CONTROL_NONE;
    return value;
}

bool eval_control_pending() {
    return eval_ctx().control != EVAL_CONTROL_NONE;
}

void eval_finish_statement_control() {
    if (eval_ctx().control == EVAL_CONTROL_NONE)
        return;
    if (eval_ctx().control == EVAL_CONTROL_CLOSE) {
        clear_eval_control();
        return;
    }
    if (eval_ctx().control == EVAL_CONTROL_FAIL) {
        if (eval_ctx().control_value)
            yyerror("uncaught fail: %s",
                    error_message_of(eval_ctx().control_value).c_str());
        else
            yyerror("uncaught fail");
    } else {
        yyerror("recover outside error handler");
    }
    clear_eval_control();
}

void eval_capture_begin(struct eval_capture_snapshot &snapshot) {
    eval_context &context = eval_ctx();
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
    eval_context &context = eval_ctx();
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
    eval_ctx().control = EVAL_CONTROL_CLOSE;
}

static void push_defer_scope() {
    eval_ctx().defer_stack.emplace_back();
}

static void run_current_defers() {
    eval_context &context = eval_ctx();
    if (context.defer_stack.empty())
        return;
    std::vector<ast *> defers = std::move(context.defer_stack.back());
    context.defer_stack.pop_back();

    /* Run cleanup with a clean control state so an in-flight error does not
       get mistaken for an error raised by a defer. */
    eval_control_kind saved_control = context.control;
    value_t *saved_value = context.control_value;
    context.control = EVAL_CONTROL_NONE;
    context.control_value = NULL;

    /* While another error is unwinding, defer failures are secondary
       diagnostics and must not be swallowed by an outer capture. */
    bool saved_capture = context.capture_errors;
    if (saved_control != EVAL_CONTROL_NONE)
        context.capture_errors = false;

    eval_control_kind pending_control = EVAL_CONTROL_NONE;
    value_t *pending_value = NULL;

    for (size_t i = defers.size(); i > 0; --i) {
        struct value_t *result = eval(defers[i - 1]);
        freeval(result);
        if (context.control == EVAL_CONTROL_NONE)
            continue;

        if (pending_control != EVAL_CONTROL_NONE ||
            (saved_control != EVAL_CONTROL_NONE &&
             saved_control != EVAL_CONTROL_CLOSE)) {
            /* A second error while one is already unwinding. Keep the first
               error and report this one as a secondary diagnostic. */
            if (context.control == EVAL_CONTROL_FAIL)
                yyerror("error in defer: %s",
                        error_message_of(context.control_value).c_str());
            else
                yyerror("error in defer: recover outside error handler");
            clear_eval_control();
            continue;
        }

        pending_control = context.control;
        pending_value = context.control_value;
        context.control = EVAL_CONTROL_NONE;
        context.control_value = NULL;
        context.capture_errors = false;
    }

    if (saved_control == EVAL_CONTROL_CLOSE &&
        pending_control == EVAL_CONTROL_FAIL) {
        /* A failing defer during close replaces the internal close signal,
           so the caller can observe it through fail / on error. */
        freeval(saved_value);
        context.control = pending_control;
        context.control_value = pending_value;
    } else if (saved_control != EVAL_CONTROL_NONE) {
        freeval(pending_value);
        context.control = saved_control;
        context.control_value = saved_value;
    } else if (pending_control != EVAL_CONTROL_NONE) {
        context.control = pending_control;
        context.control_value = pending_value;
    }
    context.capture_errors = saved_capture;
}

void eval_pushscope() {
    pushscope();
    push_defer_scope();
}

void eval_push_isolated_scope() {
    pushscope_isolated();
    push_defer_scope();
}

void eval_push_closure_scope(
    const std::shared_ptr<struct scope_t> &parent) {
    pushscope_closure(parent);
    push_defer_scope();
}

void eval_popscope() {
    run_current_defers();
    popscope();
}

void eval_begin_root_defers() {
    push_defer_scope();
}

void eval_run_root_defers() {
    run_current_defers();
}





bool switch_value_supported(const struct value_t *v) {
    if (!v) return false;
    switch (v->type) {
        case T_int:
        case T_bool:
        case T_string:
        case T_char:
        case T_byte:
        case T_bytes:
        case T_null:
            return true;
        default:
            return false;
    }
}

static bool case_value_is_condition(struct ast *a) {
    if (!a) return false;
    switch (a->nodetype) {
        case 'A':
        case 'Q':
        case '!':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
            return true;
        default:
            return false;
    }
}

bool case_values_have_condition(struct ast *values) {
    for (struct list_node *p = (struct list_node *)values; p;
         p = (struct list_node *)p->next) {
        if (case_value_is_condition(p->value))
            return true;
    }
    return false;
}
