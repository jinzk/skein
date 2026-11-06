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
            set_eval_control(EVAL_CONTROL_NONE, NULL);
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
