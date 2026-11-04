#include "eval.h"
#include "builtins.h"
#include "diagnostics.h"
#include "dict.h"
#include "eval_context.h"
#include "eval_helpers.h"
#include "fiber.h"
#include "filetype.h"
#include "format.h"
#include "package.h"
#include "pattern.h"
#include "cycle.h"
#include "platform.h"
#include "repl.h"
#include "scopestack.h"
#include "structeval.h"
#include "structtype.h"
#include "symbol.h"
#include "valuetype.h"
#include "unicode.h"

#include <cstring>
#include <unordered_set>

static eval_context &eval_ctx() {
    return active_eval_context();
}

static const char *take_pending_label() {
    eval_context &context = eval_ctx();
    const char *label = context.pending_label;
    context.pending_label = NULL;
    return label;
}

static bool label_directly_applies(const struct ast *node) {
    if (!node) return false;
    return node->nodetype == 'P' || node->nodetype == 'F' ||
           node->nodetype == 'W' || node->nodetype == 'E' ||
           node->nodetype == 'S';
}

static bool control_matches_label(struct value_t *value, const char *label) {
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

namespace {

struct eval_location_guard {
    int old_line;
    int old_column;

    eval_location_guard(int line, int column)
        : old_line(g_error_line), old_column(g_error_column) {
        if (line > 0) {
            g_error_line = line;
            g_error_column = column > 0 ? column : old_column;
        }
    }

    ~eval_location_guard() {
        g_error_line = old_line;
        g_error_column = old_column;
    }
};

struct call_stack_guard {
    call_stack_guard(const std::string &name, int line, int column) {
        diag_push_call(name, line, column);
    }
    ~call_stack_guard() {
        diag_pop_call();
    }
};

} // namespace

bool is_numeric(const struct value_t *v) {
    return v && (v->type == T_int || v->type == T_float);
}

struct value_t *new_int_checked(__int128 n) {
    if (n < LLONG_MIN || n > LLONG_MAX) {
        yyerror("integer overflow");
        return NULL;
    }
    return newintval((long long)n);
}

double as_double(const struct value_t *v) {
    if (!v) return 0.0;
    if (v->type == T_int) return (double)((struct int_value_t *)v)->value;
    if (v->type == T_float) return ((struct float_value_t *)v)->value;
    return 0.0;
}

static void clear_eval_control() {
    freeval(eval_ctx().control_value);
    eval_ctx().control_value = NULL;
    eval_ctx().control = EVAL_CONTROL_NONE;
}

static void set_eval_control(enum eval_control_kind kind,
                             struct value_t *value) {
    clear_eval_control();
    eval_ctx().control = kind;
    eval_ctx().control_value = value;
}

static struct value_t *take_eval_control(enum eval_control_kind kind) {
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

static int compare_int_double(long long i, double d, bool &unordered) {
    unordered = false;
    if (std::isnan(d)) {
        unordered = true;
        return 0;
    }
    if (std::isinf(d))
        return d > 0 ? -1 : 1;

    /* 2^63, the first double that is outside the int64 range. */
    const double two63 = 9223372036854775808.0;
    if (d >= two63) return -1;
    if (d < -two63) return 1;

    double truncated = std::trunc(d);
    long long ti = (long long)truncated;
    if (i < ti) return -1;
    if (i > ti) return 1;
    if (d > truncated) return -1;
    if (d < truncated) return 1;
    return 0;
}

int compare_numeric(const struct value_t *a, const struct value_t *b,
                    bool &unordered) {
    unordered = false;
    if (a->type == T_int && b->type == T_int) {
        long long x = ((const struct int_value_t *)a)->value;
        long long y = ((const struct int_value_t *)b)->value;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a->type == T_float && b->type == T_float) {
        double x = ((const struct float_value_t *)a)->value;
        double y = ((const struct float_value_t *)b)->value;
        if (std::isnan(x) || std::isnan(y)) {
            unordered = true;
            return 0;
        }
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a->type == T_int) {
        return compare_int_double(((const struct int_value_t *)a)->value,
                                  ((const struct float_value_t *)b)->value,
                                  unordered);
    }
    int cmp = compare_int_double(((const struct int_value_t *)b)->value,
                                 ((const struct float_value_t *)a)->value,
                                 unordered);
    return unordered ? 0 : -cmp;
}

size_t utf8_sequence_length(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t utf8_valid_sequence_length(const char *s, size_t available) {
    size_t n = utf8_sequence_length((unsigned char)s[0]);
    if (n > available) return 1;
    for (size_t i = 1; i < n; ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            return 1;
    }
    return n;
}

size_t utf8_length(const char *s) {
    size_t len = std::strlen(s);
    size_t count = 0;
    for (size_t i = 0; i < len; ++count)
        i += utf8_valid_sequence_length(s + i, len - i);
    return count;
}

bool utf8_char_at(const char *s, size_t index, std::string &out) {
    size_t len = std::strlen(s);
    size_t pos = 0;
    size_t current = 0;
    while (pos < len) {
        size_t n = utf8_valid_sequence_length(s + pos, len - pos);
        if (current == index) {
            out.assign(s + pos, n);
            return true;
        }
        pos += n;
        ++current;
    }
    return false;
}

bool truthy(const struct value_t *v) {
    if (!v) return false;
    switch (v->type) {
        case T_int: return ((struct int_value_t *)v)->value != 0;
        case T_float: return ((struct float_value_t *)v)->value != 0.0;
        case T_bool: return ((struct bool_value_t *)v)->value;
        case T_string: {
            const char *s = ((struct string_value_t *)v)->value;
            return s && *s;
        }
        case T_char: return true;
        case T_byte: return ((struct byte_value_t *)v)->value != 0;
        case T_bytes: return !((struct bytes_value_t *)v)->value.empty();
        case T_list: return !((struct list_value_t *)v)->payload->items.empty();
        case T_dict: return !((struct dict_value_t *)v)->payload->items.empty();
        case T_struct: return true;
        case T_lru: return !((struct lru_value_t *)v)->payload->items.empty();
        case T_deque:
            return ((struct deque_value_t *)v)->payload->front_size +
                       ((struct deque_value_t *)v)->payload->back_size >
                   0;
        case T_heap:
            return ((struct heap_value_t *)v)->payload->size > 0;
        case T_set:
            return ((struct set_value_t *)v)->payload->size > 0;
        case T_method: return true;
        case T_func: return true;
        case T_builtin: return true;
        case T_userdata: return true;
        case T_result: return ((struct result_value_t *)v)->ok;
        case T_error: return true;
        case T_null: return false;
        default: return false;
    }
}

static bool switch_value_supported(const struct value_t *v) {
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

static bool case_values_have_condition(struct ast *values) {
    for (struct list_node *p = (struct list_node *)values; p;
         p = (struct list_node *)p->next) {
        if (case_value_is_condition(p->value))
            return true;
    }
    return false;
}

namespace {
thread_local int equals_depth = 0;
struct equals_depth_guard {
    equals_depth_guard() { ++equals_depth; }
    ~equals_depth_guard() { --equals_depth; }
};
} // namespace

bool value_equals(struct value_t *a, struct value_t *b) {
    if (!a || !b) return a == b;
    if (equals_depth > 512)
        return false;
    equals_depth_guard guard;
    if (is_numeric(a) && is_numeric(b)) {
        bool unordered = false;
        int cmp = compare_numeric(a, b, unordered);
        return !unordered && cmp == 0;
    }
    if (a->type == T_error && b->type == T_string) {
        struct string_value_t *sb = (struct string_value_t *)b;
        return ((struct error_value_t *)a)->message ==
               (sb->value ? sb->value : "");
    }
    if (a->type == T_string && b->type == T_error)
        return value_equals(b, a);
    if (a->type != b->type) return false;
    switch (a->type) {
        case T_bool:
            return ((struct bool_value_t *)a)->value ==
                   ((struct bool_value_t *)b)->value;
        case T_string:
            return std::strcmp(((struct string_value_t *)a)->value,
                               ((struct string_value_t *)b)->value) == 0;
        case T_char:
            return ((struct char_value_t *)a)->value ==
                   ((struct char_value_t *)b)->value;
        case T_byte:
            return ((struct byte_value_t *)a)->value ==
                   ((struct byte_value_t *)b)->value;
        case T_bytes:
            return ((struct bytes_value_t *)a)->value ==
                   ((struct bytes_value_t *)b)->value;
        case T_list: {
            struct list_value_t *la = (struct list_value_t *)a;
            struct list_value_t *lb = (struct list_value_t *)b;
            if (la->payload == lb->payload) return true;
            if (la->payload->items.size() != lb->payload->items.size()) return false;
            for (size_t i = 0; i < la->payload->items.size(); ++i)
                if (!value_equals(la->payload->items[i], lb->payload->items[i])) return false;
            return true;
        }
        case T_dict: {
            struct dict_value_t *da = (struct dict_value_t *)a;
            struct dict_value_t *db = (struct dict_value_t *)b;
            if (da->payload == db->payload) return true;
            if (da->payload->items.size() != db->payload->items.size()) return false;
            for (const auto &entry : da->payload->items) {
                int found = dict_find_index(db, entry.first);
                if (found < 0) return false;
                if (!value_equals(entry.second,
                                  db->payload->items[(size_t)found].second))
                    return false;
            }
            return true;
        }
        case T_func: {
            struct func_value_t *fa = (struct func_value_t *)a;
            struct func_value_t *fb = (struct func_value_t *)b;
            return fa->def == fb->def;
        }
        case T_builtin:
            return ((struct builtin_value_t *)a)->functype ==
                   ((struct builtin_value_t *)b)->functype;
        case T_userdata: {
            struct userdata_value_t *ua = (struct userdata_value_t *)a;
            struct userdata_value_t *ub = (struct userdata_value_t *)b;
            return ua->meta == ub->meta && ua->data == ub->data;
        }
        case T_error: {
            struct error_value_t *ea = (struct error_value_t *)a;
            struct error_value_t *eb = (struct error_value_t *)b;
            return ea->kind == eb->kind && ea->message == eb->message;
        }
        case T_result: {
            struct result_value_t *ra = (struct result_value_t *)a;
            struct result_value_t *rb = (struct result_value_t *)b;
            if (ra->ok != rb->ok) return false;
            if (!ra->ok) return value_equals(ra->error, rb->error);
            return value_equals(ra->value, rb->value);
        }
        case T_struct: {
            struct struct_value_t *sa = (struct struct_value_t *)a;
            struct struct_value_t *sb = (struct struct_value_t *)b;
            if (sa->def != sb->def) return false;
            if (!sa->payload || !sb->payload) return sa->payload == sb->payload;
            if (sa->payload == sb->payload) return true;
            if (sa->payload->fields.size() != sb->payload->fields.size())
                return false;
            for (size_t i = 0; i < sa->payload->fields.size(); ++i)
                if (!value_equals(sa->payload->fields[i],
                                  sb->payload->fields[i]))
                    return false;
            return true;
        }
        case T_enum: {
            struct enum_value_t *ea = (struct enum_value_t *)a;
            struct enum_value_t *eb = (struct enum_value_t *)b;
            if (ea->def != eb->def || ea->variant != eb->variant)
                return false;
            if (!ea->payload || !eb->payload)
                return ea->payload == eb->payload;
            if (ea->payload == eb->payload) return true;
            if (ea->payload->fields.size() != eb->payload->fields.size())
                return false;
            for (size_t i = 0; i < ea->payload->fields.size(); ++i)
                if (!value_equals(ea->payload->fields[i],
                                  eb->payload->fields[i]))
                    return false;
            return true;
        }
        case T_lru: {
            struct lru_value_t *la = (struct lru_value_t *)a;
            struct lru_value_t *lb = (struct lru_value_t *)b;
            if (la->payload == lb->payload) return true;
            if (la->payload->capacity != lb->payload->capacity) return false;
            if (la->payload->items.size() != lb->payload->items.size())
                return false;
            for (size_t i = 0; i < la->payload->items.size(); ++i) {
                if (!value_equals(la->payload->items[i].first,
                                  lb->payload->items[i].first) ||
                    !value_equals(la->payload->items[i].second,
                                  lb->payload->items[i].second))
                    return false;
            }
            return true;
        }
        case T_deque: {
            struct deque_value_t *da = (struct deque_value_t *)a;
            struct deque_value_t *db = (struct deque_value_t *)b;
            if (da->payload == db->payload) return true;
            if (da->payload->front_size + da->payload->back_size !=
                db->payload->front_size + db->payload->back_size)
                return false;
            std::vector<value_t *> left;
            std::vector<value_t *> right;
            deque_items_in_order(da->payload.get(), left);
            deque_items_in_order(db->payload.get(), right);
            for (size_t i = 0; i < left.size(); ++i)
                if (!value_equals(left[i], right[i]))
                    return false;
            return true;
        }
        case T_heap: {
            struct heap_value_t *ha = (struct heap_value_t *)a;
            struct heap_value_t *hb = (struct heap_value_t *)b;
            if (ha->payload == hb->payload) return true;
            if (ha->payload->size != hb->payload->size) return false;
            std::vector<value_t *> left;
            std::vector<value_t *> right;
            heap_items_level_order(ha->payload.get(), left);
            heap_items_level_order(hb->payload.get(), right);
            for (size_t i = 0; i < left.size(); ++i)
                if (!value_equals(left[i], right[i]))
                    return false;
            return true;
        }
        case T_set: {
            struct set_value_t *sa = (struct set_value_t *)a;
            struct set_value_t *sb = (struct set_value_t *)b;
            if (sa->payload == sb->payload) return true;
            if (sa->payload->size != sb->payload->size) return false;
            std::vector<value_t *> items;
            set_values_in_order(sa->payload.get(), items);
            for (value_t *item : items)
                if (!set_contains(sb->payload.get(), item))
                    return false;
            return true;
        }
        case T_method: {
            struct method_value_t *ma = (struct method_value_t *)a;
            struct method_value_t *mb = (struct method_value_t *)b;
            return ma->method == mb->method && ma->receiver == mb->receiver;
        }
        case T_null:
            return true;
        default:
            return false;
    }
}

static struct value_t *do_compare(int op, struct value_t *l, struct value_t *r) {
    if (!l || !r) return NULL;

    if (!(is_numeric(l) && is_numeric(r)) &&
        !(l->type == T_string && r->type == T_string) &&
        !(l->type == T_char && r->type == T_char) &&
        !(l->type == T_byte && r->type == T_byte)) {
        if (op == 3 || op == 4) {
            bool eq = value_equals(l, r);
            return newboolval((op == 4) ? eq : !eq);
        }
        yyerror("cannot compare %s and %s", typename_of(l), typename_of(r));
        return NULL;
    }

    if (is_numeric(l) && is_numeric(r)) {
        bool unordered = false;
        int cmp = compare_numeric(l, r, unordered);
        if (unordered) {
            /* IEEE 754: NaN is unequal to everything and unordered. */
            if (op == 3) return newboolval(true);
            return newboolval(false);
        }
        switch (op) {
            case 1: return newboolval(cmp > 0);
            case 2: return newboolval(cmp < 0);
            case 3: return newboolval(cmp != 0);
            case 4: return newboolval(cmp == 0);
            case 5: return newboolval(cmp >= 0);
            case 6: return newboolval(cmp <= 0);
            default: return NULL;
        }
    }

    int cmp = 0;
    if (l->type == T_char) {
        unsigned int lc = ((struct char_value_t *)l)->value;
        unsigned int rc = ((struct char_value_t *)r)->value;
        cmp = (lc < rc) ? -1 : (lc > rc ? 1 : 0);
    } else if (l->type == T_byte) {
        unsigned int lc = ((struct byte_value_t *)l)->value;
        unsigned int rc = ((struct byte_value_t *)r)->value;
        cmp = (lc < rc) ? -1 : (lc > rc ? 1 : 0);
    } else {
        cmp = std::strcmp(((struct string_value_t *)l)->value,
                          ((struct string_value_t *)r)->value);
        cmp = (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
    }

    switch (op) {
        case 1: return newboolval(cmp > 0);
        case 2: return newboolval(cmp < 0);
        case 3: return newboolval(cmp != 0);
        case 4: return newboolval(cmp == 0);
        case 5: return newboolval(cmp >= 0);
        case 6: return newboolval(cmp <= 0);
        default: return NULL;
    }
}

static void free_args(std::vector<struct value_t *> &args) {
    for (size_t i = 0; i < args.size(); ++i)
        freeval(args[i]);
    args.clear();
}

/*
 * Function arguments are represented as a right-nested 'L' chain.  Evaluate
 * them left-to-right without using eval('L'), because eval('L') intentionally
 * frees intermediate statement values.
 */
static bool collect_args(struct ast *a, std::vector<struct value_t *> &out,
                         struct value_t *&control) {
    if (!a) return true;
    if (out.capacity() == 0)
        out.reserve(8);
    if (a->nodetype == 'L') {
        if (!collect_args(a->l, out, control)) return false;
        return collect_args(a->r, out, control);
    }
    if (a->nodetype == 'x') {
        struct value_t *v = eval(((struct spread_node *)a)->value);
        if (!v) return false;
        if (v->type == T_return || v->type == T_break ||
            v->type == T_continue) {
            control = v;
            return false;
        }
        if (v->type != T_list) {
            yyerror("spread expects a list, got %s", typename_of(v));
            freeval(v);
            return false;
        }
        struct list_value_t *list = (struct list_value_t *)v;
        for (size_t i = 0; i < list->payload->items.size(); ++i)
            out.push_back(dupval(list->payload->items[i]));
        freeval(v);
        return true;
    }
    struct value_t *v = eval(a);
    if (!v) return false;
    if (v->type == T_return || v->type == T_break || v->type == T_continue) {
        control = v;
        return false;
    }
    out.push_back(v);
    return true;
}

static struct value_t *invoke_value(
    struct value_t *callee, std::vector<struct value_t *> &args);
struct value_t *call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args);

static constexpr int MAX_CALL_DEPTH = 1000;
static int call_depth = 0;

namespace {
struct call_depth_guard {
    call_depth_guard() { ++call_depth; }
    ~call_depth_guard() { --call_depth; }
};
}

static struct value_t *invoke_function_body(
    const std::shared_ptr<function_def> &def,
    std::vector<struct value_t *> &args) {
    int nparams = def->code->nparams;
    std::string frame_name =
        def->code->name.empty() ? "<anonymous>" : def->code->name;

    struct symlist *rest = NULL;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next) {
        if (sl->variadic) {
            rest = sl;
            break;
        }
    }

    int errors_before = eval_ctx().runtime_error_count;
    int frame_line = g_error_line;
    int frame_column = g_error_column;
    if (frame_line <= 0 && def->code->ast) {
        frame_line = def->code->ast->line;
        frame_column = def->code->ast->column;
    }
    call_stack_guard frame_guard(frame_name, frame_line, frame_column);
    bool saved_allow_await = eval_ctx().allow_await;
    eval_ctx().allow_await = def->code->is_async;
    eval_push_closure_scope(def->closure);
    int i = 0;
    for (struct symlist *sl = def->code->sl; sl && !sl->variadic;
         sl = sl->next, ++i) {
        struct symbol *sym = newsym(sl->sym->name);
        sym->val = args[i];
        if (sl->type) {
            sym->type.kind = sl->type->kind;
            sym->type.name = sl->type->name;
        }
        if (i == 0 && def->code->is_method) {
            sym->is_self = true;
            sym->is_const = true;
        }
        savesym(sym);
    }
    if (rest) {
        struct list_value_t *list = newlistval();
        for (size_t k = (size_t)nparams; k < args.size(); ++k) {
            list->payload->items.push_back(args[k]);
            args[k] = NULL;
        }
        struct symbol *sym = newsym(rest->sym->name);
        sym->val = (struct value_t *)list;
        sym->type.kind = TYPE_LIST;
        sym->type.name = NULL;
        savesym(sym);
    }
    args.clear(); /* ownership transferred to the function scope */

    int saved_handler_depth = eval_ctx().handler_depth;
    eval_ctx().handler_depth = 0;
    struct value_t *body_ret = eval(def->code->ast);
    eval_ctx().handler_depth = saved_handler_depth;
    struct value_t *result = topscope()->ret;
    topscope()->ret = NULL;

    if (body_ret) {
        if (body_ret->type == T_break) {
            diag_set_location(g_flow_line, g_flow_column);
            yyerror("break outside loop in function");
        } else if (body_ret->type == T_continue) {
            diag_set_location(g_flow_line, g_flow_column);
            yyerror("continue outside loop in function");
        }
        freeval(body_ret);
    }

    eval_popscope();
    eval_ctx().allow_await = saved_allow_await;

    if (eval_ctx().control != EVAL_CONTROL_NONE) {
        freeval(result);
        return NULL;
    }

    bool errored = eval_ctx().runtime_error_count != errors_before;
    if (g_interrupted || errored) {
        freeval(result);
        return NULL;
    }
    return result ? result : newnull();
}

static struct value_t *invoke_value(
    struct value_t *callee, std::vector<struct value_t *> &args) {
    if (!callee) {
        free_args(args);
        return NULL;
    }

    if (callee->type == T_builtin) {
        struct value_t *ret = call_builtin(
            ((struct builtin_value_t *)callee)->functype, args);
        free_args(args);
        return ret;
    }

    if (callee->type == T_method) {
        struct method_value_t *method = (struct method_value_t *)callee;
        std::vector<struct value_t *> all_args;
        all_args.reserve(args.size() + 1);
        all_args.push_back(newstructval(method->def, method->receiver));
        for (size_t i = 0; i < args.size(); ++i)
            all_args.push_back(args[i]);
        args.clear();
        struct func_value_t function;
        function.type = T_func;
        function.def = method->method;
        return invoke_value((struct value_t *)&function, all_args);
    }

    if (callee->type != T_func) {
        yyerror("cannot call %s", typename_of(callee));
        free_args(args);
        return NULL;
    }

    struct func_value_t *fv = (struct func_value_t *)callee;
    /*
     * Keep the function definition alive for the whole call.  The body can
     * reassign/free the variable that holds this function value.
     */
    std::shared_ptr<function_def> def = fv->def;
    int nparams = def->code->nparams;
    bool variadic = def->code->variadic;
    std::string frame_name =
        def->code->name.empty() ? "<anonymous>" : def->code->name;
    if ((int)args.size() < nparams ||
        (!variadic && (int)args.size() != nparams)) {
        if (variadic)
            yyerror("function %s expects at least %d argument(s), got %d",
                    frame_name.c_str(), nparams, (int)args.size());
        else
            yyerror("function expects %d argument(s), got %d",
                    nparams, (int)args.size());
        free_args(args);
        return NULL;
    }

    struct symlist *rest = NULL;
    int i = 0;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next, ++i) {
        if (sl->variadic) {
            rest = sl;
            break;
        }
        if (!type_constraint_matches(sl->type, args[i])) {
            yyerror("function %s argument %d (%s) expects %s, got %s",
                    frame_name.c_str(), i + 1, sl->sym->name,
                    type_constraint_name(sl->type), typename_of(args[i]));
            free_args(args);
            return NULL;
        }
    }
    if (rest) {
        for (size_t k = (size_t)nparams; k < args.size(); ++k) {
            if (!type_constraint_matches(rest->type, args[k])) {
                yyerror("function %s rest argument %d expects %s, got %s",
                        frame_name.c_str(), (int)(k - (size_t)nparams + 1),
                        type_constraint_name(rest->type),
                        typename_of(args[k]));
                free_args(args);
                return NULL;
            }
        }
    }

    if (def->code->is_generator) {
        std::shared_ptr<std::vector<value_t *>> owned(
            new std::vector<value_t *>(std::move(args)),
            [](std::vector<value_t *> *values) {
                free_args(*values);
                delete values;
            });
        generator_ptr generator = generator_create(
            [def, owned]() {
                return invoke_function_body(def, *owned);
            });
        if (!generator) {
            return NULL;
        }
        return new_generator_value(std::move(generator));
    }

    if (def->code->is_async) {
        task_ptr task = fiber_start_async(
            [def, owned = std::move(args)]() mutable {
                return invoke_function_body(def, owned);
            });
        if (!task) {
            free_args(args);
            return NULL;
        }
        return new_task_value(std::move(task));
    }

    return invoke_function_body(def, args);
}

struct value_t *call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args) {
    if (!callee) {
        free_args(args);
        return NULL;
    }
    if (call_depth >= MAX_CALL_DEPTH) {
        yyerror("maximum recursion depth (%d) exceeded", MAX_CALL_DEPTH);
        free_args(args);
        return NULL;
    }
    call_depth_guard depth_guard;
    return invoke_value(callee, args);
}

static const char *import_binding_name(const char *path) {
    while (path && (*path == '.' || *path == '@'))
        ++path;
    if (!path || !*path)
        return "";
    const char *dot = std::strrchr(path, '.');
    return dot ? dot + 1 : path;
}

static struct value_t *callvalue(struct value_t *callee, struct ast *args_ast) {
    if (!callee)
        return NULL;

    /*
     * Keep the call-depth guard before argument evaluation so deeply nested
     * call expressions cannot exhaust the C++ stack.
     */
    if (call_depth >= MAX_CALL_DEPTH) {
        yyerror("maximum recursion depth (%d) exceeded", MAX_CALL_DEPTH);
        return NULL;
    }
    call_depth_guard depth_guard;

    std::vector<struct value_t *> args;
    struct value_t *control = NULL;
    if (!collect_args(args_ast, args, control)) {
        free_args(args);
        return control;
    }

    return invoke_value(callee, args);
}

static struct ast *index_base(struct ast *a) {
    while (a && (a->nodetype == 'G' || a->nodetype == 'j')) a = a->l;
    return a;
}

/*
 * Collect the index expressions of an lvalue chain such as a[i][j].  The base
 * must be a variable; assigning through function calls or literals is not
 * allowed.  `fields` records whether each step used dot access.
 */
static bool collect_index_asts(struct ast *a, std::vector<struct ast *> &out,
                               std::vector<bool> &fields) {
    if (!a) return false;
    if (a->nodetype == 'G' || a->nodetype == 'j') {
        if (!collect_index_asts(a->l, out, fields)) return false;
        out.push_back(a->r);
        fields.push_back(a->nodetype == 'j');
        return true;
    }
    return a->nodetype == 'N';
}

/*
 * Detach a shared payload before mutating it.  The new payload owns fresh
 * wrapper values; nested containers are still shared through their own
 * copy-on-write payloads.
 */
static void ensure_unique_list(struct list_value_t *list) {
    if (!list->payload) {
        list->payload = std::make_shared<list_payload>();
        return;
    }
    if (list->payload.use_count() == 1)
        return;

    std::shared_ptr<list_payload> copy = std::make_shared<list_payload>();
    copy->items.reserve(list->payload->items.size());
    for (size_t i = 0; i < list->payload->items.size(); ++i)
        copy->items.push_back(dupval(list->payload->items[i]));
    list->payload = std::move(copy);
}

static void ensure_unique_dict(struct dict_value_t *dict) {
    if (!dict->payload) {
        dict->payload = std::make_shared<dict_payload>();
        return;
    }
    if (dict->payload.use_count() == 1)
        return;

    std::shared_ptr<dict_payload> copy = std::make_shared<dict_payload>();
    copy->is_namespace = dict->payload->is_namespace;
    copy->items.reserve(dict->payload->items.size());
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        copy->items.push_back(std::make_pair(
            dupval(dict->payload->items[i].first),
            dupval(dict->payload->items[i].second)));
    }
    dict->payload = std::move(copy);
}

static void ensure_unique_container(struct value_t *container) {
    if (!container) return;
    if (container->type == T_list)
        ensure_unique_list((struct list_value_t *)container);
    else if (container->type == T_dict)
        ensure_unique_dict((struct dict_value_t *)container);
    else if (container->type == T_struct)
        ensure_unique_struct((struct struct_value_t *)container);
}

static struct value_t **index_slot_ptr(struct value_t *container,
                                       struct value_t *idx) {
    if (!container || !idx) return NULL;
    if (container->type == T_list) {
        if (idx->type != T_int) return NULL;
        long long i = ((struct int_value_t *)idx)->value;
        struct list_value_t *lv = (struct list_value_t *)container;
        if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) return NULL;
        return &lv->payload->items[(size_t)i];
    }
    if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        for (size_t i = 0; i < dv->payload->items.size(); ++i)
            if (value_equals(dv->payload->items[i].first, idx))
                return &dv->payload->items[i].second;
    }
    if (container->type == T_struct) {
        if (idx->type != T_string) return NULL;
        struct struct_value_t *sv = (struct struct_value_t *)container;
        if (!sv->def || !sv->payload) return NULL;
        int field = structtype_field_index(
            sv->def, ((struct string_value_t *)idx)->value);
        if (field < 0 || (size_t)field >= sv->payload->fields.size())
            return NULL;
        return &sv->payload->fields[(size_t)field];
    }
    return NULL;
}

static struct value_t *apply_increment(struct value_t *v, int delta) {
    if (v && v->type == T_int) {
        long long n = ((struct int_value_t *)v)->value;
        return new_int_checked((__int128)n + delta);
    }
    if (v && v->type == T_float) {
        double d = ((struct float_value_t *)v)->value;
        return newfloatval(d + (double)delta);
    }
    yyerror("cannot %s %s", delta > 0 ? "increment" : "decrement",
            typename_of(v));
    return NULL;
}

/*
 * Resolve the container that owns the final index of a write path.  Every
 * intermediate container is made unique before its child is inspected, so a
 * subsequent write cannot mutate a value that is still shared.
 */
static bool resolve_write_container(
    struct symbol *s, const std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields, struct value_t *&container,
    std::vector<const void *> *path_payloads = NULL) {
    container = s ? s->val : NULL;
    if (path_payloads)
        path_payloads->push_back(value_payload(container));
    for (size_t k = 0; k + 1 < indices.size(); ++k) {
        ensure_unique_container(container);
        if (fields[k] && container->type == T_dict &&
            !((struct dict_value_t *)container)->payload->is_namespace) {
            yyerror("cannot access dict key with '.', use [\"...\"]");
            return false;
        }
        struct value_t **slot = index_slot_ptr(container, indices[k]);
        if (!slot || !*slot) {
            yyerror("invalid index assignment path");
            return false;
        }
        struct value_t *elem = *slot;
        if (elem->type != T_list && elem->type != T_dict &&
            elem->type != T_struct) {
            yyerror("cannot index-assign into %s", typename_of(elem));
            return false;
        }
        container = elem;
        if (path_payloads)
            path_payloads->push_back(value_payload(container));
    }
    return true;
}

/* Assigns through an index/field slot.  Ownership of `val` is consumed. */
static struct value_t *write_member_value(struct value_t *container,
                                          struct value_t *idx,
                                          struct value_t *val,
                                          bool field_access) {
    struct value_t *ret = NULL;

    if (container->type == T_list) {
        if (field_access) {
            yyerror("cannot access a list field with '.'");
        } else if (idx->type != T_int) {
            yyerror("list index must be an integer, got %s", typename_of(idx));
        } else {
            long long i = ((struct int_value_t *)idx)->value;
            struct list_value_t *lv = (struct list_value_t *)container;
            if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) {
                yyerror("list index %lld out of range", i);
            } else {
                ensure_unique_list(lv);
                freeval(lv->payload->items[(size_t)i]);
                lv->payload->items[(size_t)i] = val;
                val = NULL;
                ret = dupval(lv->payload->items[(size_t)i]);
            }
        }
    } else if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        if (field_access && !dv->payload->is_namespace) {
            yyerror("cannot access dict key with '.', use [\"...\"]");
        } else if (!dict_key_allowed(idx)) {
            yyerror("%s, got %s", dict_key_error(), typename_of(idx));
        } else {
            ensure_unique_dict(dv);
            int found = dict_find_index(dv, idx);
            if (found >= 0) {
                freeval(dv->payload->items[(size_t)found].second);
                dv->payload->items[(size_t)found].second = val;
                val = NULL;
                ret = dupval(dv->payload->items[(size_t)found].second);
            } else {
                dict_append_item(dv, dupval(idx), val);
                val = NULL;
                ret = dupval(dv->payload->items.back().second);
            }
        }
    } else if (container->type == T_struct) {
        if (idx->type != T_string) {
            yyerror("struct field name must be a string, got %s",
                    typename_of(idx));
        } else {
            struct struct_value_t *sv = (struct struct_value_t *)container;
            const char *name = ((struct string_value_t *)idx)->value;
            int field = sv->def ? structtype_field_index(sv->def, name) : -1;
            if (field < 0 || !sv->def ||
                (size_t)field >= sv->def->fields.size()) {
                yyerror("field %s not found in %s", name,
                        sv->def ? sv->def->name.c_str() : "struct");
            } else if (!type_constraint_matches(
                           &sv->def->fields[(size_t)field].type, val)) {
                yyerror("field %s of %s expects %s, got %s", name,
                        sv->def->name.c_str(),
                        type_constraint_name(
                            &sv->def->fields[(size_t)field].type),
                        typename_of(val));
            } else {
                ensure_unique_struct(sv);
                if (!sv->payload ||
                    (size_t)field >= sv->payload->fields.size()) {
                    yyerror("invalid struct field assignment");
                } else {
                    freeval(sv->payload->fields[(size_t)field]);
                    sv->payload->fields[(size_t)field] = val;
                    val = NULL;
                    ret = dupval(sv->payload->fields[(size_t)field]);
                }
            }
        }
    } else if (container->type == T_bytes) {
        if (idx->type != T_int) {
            yyerror("bytes index must be an integer, got %s", typename_of(idx));
        } else if (val->type != T_int) {
            yyerror("bytes value must be an integer, got %s", typename_of(val));
        } else {
            long long i = ((struct int_value_t *)idx)->value;
            long long byte = ((struct int_value_t *)val)->value;
            std::string &data = ((struct bytes_value_t *)container)->value;
            if (i < 0 || (unsigned long long)i >= data.size()) {
                yyerror("bytes index %lld out of range", i);
            } else if (byte < 0 || byte > 255) {
                yyerror("bytes value %lld out of range", byte);
            } else {
                data[(size_t)i] = (char)(unsigned char)byte;
                ret = newintval(byte);
            }
        }
    } else {
        yyerror("cannot index-assign into %s", typename_of(container));
    }

    freeval(val);
    return ret;
}

static struct value_t *increment_member_value(struct value_t *container,
                                              struct value_t *idx, int delta,
                                              bool prefix, bool field_access) {
    struct value_t *ret = NULL;

    if (container->type == T_list) {
        if (field_access) {
            yyerror("cannot access a list field with '.'");
        } else if (idx->type != T_int) {
            yyerror("list index must be an integer, got %s", typename_of(idx));
        } else {
            long long i = ((struct int_value_t *)idx)->value;
            struct list_value_t *lv = (struct list_value_t *)container;
            if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) {
                yyerror("list index %lld out of range", i);
            } else {
                ensure_unique_list(lv);
                struct value_t **slot = &lv->payload->items[(size_t)i];
                struct value_t *newv = apply_increment(*slot, delta);
                if (newv) {
                    ret = prefix ? dupval(newv) : dupval(*slot);
                    freeval(*slot);
                    *slot = newv;
                }
            }
        }
    } else if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        if (field_access && !dv->payload->is_namespace) {
            yyerror("cannot access dict key with '.', use [\"...\"]");
        } else {
            ensure_unique_dict(dv);
            int index = dict_find_index(dv, idx);
            if (index >= 0) {
                struct value_t *old =
                    dv->payload->items[(size_t)index].second;
                struct value_t *newv = apply_increment(old, delta);
                if (newv) {
                    ret = prefix ? dupval(newv) : dupval(old);
                    freeval(old);
                    dv->payload->items[(size_t)index].second = newv;
                }
            } else {
                yyerror("key not found");
            }
        }
    } else if (container->type == T_struct) {
        if (idx->type != T_string) {
            yyerror("struct field name must be a string, got %s",
                    typename_of(idx));
        } else {
            struct struct_value_t *sv = (struct struct_value_t *)container;
            const char *name = ((struct string_value_t *)idx)->value;
            int field = sv->def ? structtype_field_index(sv->def, name) : -1;
            if (field < 0 || !sv->def ||
                (size_t)field >= sv->def->fields.size()) {
                yyerror("field %s not found in %s", name,
                        sv->def ? sv->def->name.c_str() : "struct");
            } else {
                ensure_unique_struct(sv);
                if (!sv->payload ||
                    (size_t)field >= sv->payload->fields.size()) {
                    yyerror("invalid struct field assignment");
                } else {
                    struct value_t *old =
                        sv->payload->fields[(size_t)field];
                    struct value_t *newv = apply_increment(old, delta);
                    if (newv &&
                        !type_constraint_matches(
                            &sv->def->fields[(size_t)field].type, newv)) {
                        yyerror("field %s of %s expects %s, got %s", name,
                                sv->def->name.c_str(),
                                type_constraint_name(
                                    &sv->def->fields[(size_t)field].type),
                                typename_of(newv));
                        freeval(newv);
                    } else if (newv) {
                        ret = prefix ? dupval(newv) : dupval(old);
                        freeval(old);
                        sv->payload->fields[(size_t)field] = newv;
                    }
                }
            }
        }
    } else if (container->type == T_bytes) {
        if (idx->type != T_int) {
            yyerror("bytes index must be an integer, got %s", typename_of(idx));
        } else {
            long long i = ((struct int_value_t *)idx)->value;
            std::string &data = ((struct bytes_value_t *)container)->value;
            if (i < 0 || (unsigned long long)i >= data.size()) {
                yyerror("bytes index %lld out of range", i);
            } else {
                long long old = (unsigned char)data[(size_t)i];
                long long updated = old + delta;
                if (updated < 0 || updated > 255) {
                    yyerror("bytes value %lld out of range", updated);
                } else {
                    ret = newintval(prefix ? updated : old);
                    data[(size_t)i] = (char)(unsigned char)updated;
                }
            }
        }
    } else {
        yyerror("cannot index-assign into %s", typename_of(container));
    }

    return ret;
}

static struct value_t *apply_binary_value(int op, struct value_t *l,
                                          struct value_t *r) {
    if (op == '+') {
        if (l->type == T_int && r->type == T_int) {
            long long x = ((struct int_value_t *)l)->value;
            long long y = ((struct int_value_t *)r)->value;
            return new_int_checked((__int128)x + y);
        }
        if (is_numeric(l) && is_numeric(r))
            return newfloatval(as_double(l) + as_double(r));
        if (l->type == T_string && r->type == T_string) {
            std::string s = ((struct string_value_t *)l)->value;
            s += ((struct string_value_t *)r)->value;
            return newstrval(s.c_str());
        }
        if ((l->type == T_string || l->type == T_char) &&
            (r->type == T_string || r->type == T_char)) {
            std::string s;
            if (l->type == T_string)
                s += ((struct string_value_t *)l)->value;
            else
                utf8_encode(((struct char_value_t *)l)->value, s);
            if (r->type == T_string)
                s += ((struct string_value_t *)r)->value;
            else
                utf8_encode(((struct char_value_t *)r)->value, s);
            return newstrval(s.c_str());
        }
        if (l->type == T_list && r->type == T_list) {
            struct list_value_t *lv = newlistval();
            struct list_value_t *ll = (struct list_value_t *)l;
            struct list_value_t *rl = (struct list_value_t *)r;
            lv->payload->items.reserve(ll->payload->items.size() +
                                       rl->payload->items.size());
            for (size_t i = 0; i < ll->payload->items.size(); ++i)
                lv->payload->items.push_back(dupval(ll->payload->items[i]));
            for (size_t i = 0; i < rl->payload->items.size(); ++i)
                lv->payload->items.push_back(dupval(rl->payload->items[i]));
            return (struct value_t *)lv;
        }
        yyerror("cannot add %s and %s", typename_of(l), typename_of(r));
        return NULL;
    }

    if (op == '-' || op == '*') {
        if (!is_numeric(l) || !is_numeric(r)) {
            yyerror("cannot use %c on %s and %s", op, typename_of(l),
                    typename_of(r));
            return NULL;
        }
        if (l->type == T_int && r->type == T_int) {
            long long x = ((struct int_value_t *)l)->value;
            long long y = ((struct int_value_t *)r)->value;
            return new_int_checked(op == '-' ? (__int128)x - y
                                             : (__int128)x * y);
        }
        double x = as_double(l), y = as_double(r);
        return newfloatval(op == '-' ? x - y : x * y);
    }

    if (op == '/' || op == '%') {
        if (!is_numeric(l) || !is_numeric(r)) {
            yyerror(op == '/' ? "cannot divide %s by %s"
                              : "cannot take %s modulo %s",
                    typename_of(l), typename_of(r));
            return NULL;
        }
        if (as_double(r) == 0.0) {
            yyerror(op == '/' ? "division by zero" : "modulo by zero");
            return NULL;
        }
        if (l->type == T_int && r->type == T_int) {
            long long x = ((struct int_value_t *)l)->value;
            long long y = ((struct int_value_t *)r)->value;
            return new_int_checked(op == '/' ? (__int128)x / y
                                             : (__int128)x % y);
        }
        return newfloatval(op == '/' ? as_double(l) / as_double(r)
                                     : std::fmod(as_double(l), as_double(r)));
    }

    if (op == '&' || op == '|' || op == '^') {
        if (l->type != T_int || r->type != T_int) {
            yyerror("bitwise operator expects ints, got %s and %s",
                    typename_of(l), typename_of(r));
            return NULL;
        }
        long long x = ((struct int_value_t *)l)->value;
        long long y = ((struct int_value_t *)r)->value;
        if (op == '&') return newintval(x & y);
        if (op == '|') return newintval(x | y);
        return newintval(x ^ y);
    }

    if (op == 's' || op == 'r') {
        if (l->type != T_int || r->type != T_int) {
            yyerror("shift operator expects ints, got %s and %s",
                    typename_of(l), typename_of(r));
            return NULL;
        }
        long long x = ((struct int_value_t *)l)->value;
        long long y = ((struct int_value_t *)r)->value;
        if (y < 0 || y >= 64) {
            yyerror("shift count %lld out of range", y);
            return NULL;
        }
        if (op == 's')
            return newintval((long long)((unsigned long long)x << y));
        return newintval(x >> y);
    }

    yyerror("unsupported compound assignment operator");
    return NULL;
}

static struct value_t *assign_index(struct ast *target, struct ast *value_ast) {
    std::vector<struct ast *> index_asts;
    std::vector<bool> fields;
    if (!collect_index_asts(target, index_asts, fields)) {
        yyerror("cannot assign through this expression");
        return NULL;
    }
    struct ast *base = index_base(target);
    if (!base || base->nodetype != 'N') {
        yyerror("cannot assign through this expression");
        return NULL;
    }

    /*
     * Evaluate the right-hand side and every index expression before walking
     * the container.  This prevents user code from invalidating pointers into
     * a list/dict vector while we are assigning.
     */
    struct value_t *val = eval(value_ast);
    if (!val) return NULL;

    std::vector<struct value_t *> indices;
    for (size_t i = 0; i < index_asts.size(); ++i) {
        struct value_t *idx = eval(index_asts[i]);
        if (!idx) {
            freeval(val);
            free_args(indices);
            return NULL;
        }
        indices.push_back(idx);
    }

    struct symbol *s = lookupsym(((struct symref *)base)->name);
    if (!s || !s->val) {
        yyerror("symbol not found %s", ((struct symref *)base)->name);
        freeval(val);
        free_args(indices);
        return NULL;
    }
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        freeval(val);
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    std::vector<const void *> path_payloads;
    if (!resolve_write_container(s, indices, fields, container,
                                 &path_payloads)) {
        freeval(val);
        free_args(indices);
        return NULL;
    }
    if (value_reaches_any(val, path_payloads)) {
        yyerror("recursive value would create a cycle");
        freeval(val);
        free_args(indices);
        return NULL;
    }

    struct value_t *ret =
        write_member_value(container, indices.back(), val, fields.back());
    free_args(indices);
    return ret;
}

static struct value_t *increment_index(struct ast *target, int delta,
                                       bool prefix) {
    std::vector<struct ast *> index_asts;
    std::vector<bool> fields;
    if (!collect_index_asts(target, index_asts, fields)) {
        yyerror("cannot assign through this expression");
        return NULL;
    }
    struct ast *base = index_base(target);
    if (!base || base->nodetype != 'N') {
        yyerror("cannot assign through this expression");
        return NULL;
    }

    std::vector<struct value_t *> indices;
    for (size_t i = 0; i < index_asts.size(); ++i) {
        struct value_t *idx = eval(index_asts[i]);
        if (!idx) {
            free_args(indices);
            return NULL;
        }
        indices.push_back(idx);
    }

    struct symbol *s = lookupsym(((struct symref *)base)->name);
    if (!s || !s->val) {
        yyerror("symbol not found %s", ((struct symref *)base)->name);
        free_args(indices);
        return NULL;
    }
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    if (!resolve_write_container(s, indices, fields, container)) {
        free_args(indices);
        return NULL;
    }

    struct value_t *ret = increment_member_value(
        container, indices.back(), delta, prefix, fields.back());
    free_args(indices);
    return ret;
}

static struct value_t *read_member_value(struct value_t *container,
                                         struct value_t *index,
                                         bool field_access = false);

static struct value_t *compound_name(struct symref *ref, int op,
                                     struct ast *value_ast) {
    struct symbol *s = lookupsym(ref->name);
    if (!s) {
        yyerror("symbol not found %s", ref->name);
        return NULL;
    }
    if (s->is_const) {
        yyerror("cannot assign to constant %s", ref->name);
        return NULL;
    }
    struct value_t *rhs = eval(value_ast);
    if (!rhs) return NULL;
    struct value_t *old = dupval(s->val);
    struct value_t *value = apply_binary_value(op, old, rhs);
    freeval(old);
    freeval(rhs);
    if (!value) return NULL;
    if (!type_constraint_matches(&s->type, value)) {
        yyerror("variable %s expects %s, got %s", s->name,
                type_constraint_name(&s->type), typename_of(value));
        freeval(value);
        return NULL;
    }
    freeval(s->val);
    s->val = value;
    return dupval(value);
}

static struct value_t *compound_index(struct ast *target, int op,
                                      struct ast *value_ast) {
    std::vector<struct ast *> index_asts;
    std::vector<bool> fields;
    if (!collect_index_asts(target, index_asts, fields)) {
        yyerror("cannot assign through this expression");
        return NULL;
    }
    struct ast *base = index_base(target);
    if (!base || base->nodetype != 'N') {
        yyerror("cannot assign through this expression");
        return NULL;
    }

    struct value_t *rhs = eval(value_ast);
    if (!rhs) return NULL;

    std::vector<struct value_t *> indices;
    for (size_t i = 0; i < index_asts.size(); ++i) {
        struct value_t *idx = eval(index_asts[i]);
        if (!idx) {
            freeval(rhs);
            free_args(indices);
            return NULL;
        }
        indices.push_back(idx);
    }

    struct symbol *s = lookupsym(((struct symref *)base)->name);
    if (!s || !s->val) {
        yyerror("symbol not found %s", ((struct symref *)base)->name);
        freeval(rhs);
        free_args(indices);
        return NULL;
    }
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        freeval(rhs);
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    if (!resolve_write_container(s, indices, fields, container)) {
        freeval(rhs);
        free_args(indices);
        return NULL;
    }

    struct value_t *idx = indices.back();
    struct value_t *old = read_member_value(container, idx, fields.back());
    if (!old) {
        freeval(rhs);
        free_args(indices);
        return NULL;
    }
    struct value_t *value = apply_binary_value(op, old, rhs);
    freeval(old);
    freeval(rhs);
    if (!value) {
        free_args(indices);
        return NULL;
    }

    struct value_t *ret = write_member_value(container, idx, value,
                                             fields.back());
    free_args(indices);
    return ret;
}

static bool is_read_only(const struct ast *a) {
    if (!a) return true;
    switch (a->nodetype) {
        case 'V':
        case 'N':
            return true;
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 'A':
        case 'Q':
        case 's':
        case 'r':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case 'M':
        case '!':
        case '~':
        case 'b':
        case 'G':
        case 'j':
            return is_read_only(a->l) && is_read_only(a->r);
        default:
            return false;
    }
}

/*
 * Read one indexing step without copying the container.  For lists and
 * dictionaries `next` points into the container and stays borrowed; for
 * strings a new one-character string is returned and must be freed.
 */
static bool index_read_step(struct value_t *container, struct value_t *index,
                            struct value_t **next, bool *next_owned) {
    *next = NULL;
    *next_owned = false;

    if (container->type == T_list) {
        if (index->type != T_int) {
            yyerror("list index must be an integer, got %s",
                    typename_of(index));
            return false;
        }
        long long i = ((struct int_value_t *)index)->value;
        struct list_value_t *lv = (struct list_value_t *)container;
        if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) {
            yyerror("list index %lld out of range", i);
            return false;
        }
        *next = lv->payload->items[(size_t)i];
        return true;
    }

    if (container->type == T_string) {
        if (index->type != T_int) {
            yyerror("string index must be an integer, got %s",
                    typename_of(index));
            return false;
        }
        long long i = ((struct int_value_t *)index)->value;
        const char *s = ((struct string_value_t *)container)->value;
        size_t len = utf8_length(s);
        if (i < 0 || (unsigned long long)i >= len) {
            yyerror("string index %lld out of range", i);
            return false;
        }
        std::string ch;
        if (!utf8_char_at(s, (size_t)i, ch))
            return false;
        size_t consumed = 0;
        unsigned int codepoint = 0;
        if (!utf8_decode_one(ch.data(), ch.size(), consumed, codepoint))
            return false;
        *next = newcharval(codepoint);
        *next_owned = true;
        return true;
    }

    if (container->type == T_bytes) {
        if (index->type != T_int) {
            yyerror("bytes index must be an integer, got %s",
                    typename_of(index));
            return false;
        }
        long long i = ((struct int_value_t *)index)->value;
        std::string &data = ((struct bytes_value_t *)container)->value;
        if (i < 0 || (unsigned long long)i >= data.size()) {
            yyerror("bytes index %lld out of range", i);
            return false;
        }
        *next = newintval((unsigned char)data[(size_t)i]);
        *next_owned = true;
        return true;
    }

    if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        int found = dict_find_index(dv, index);
        if (found >= 0) {
            *next = dv->payload->items[(size_t)found].second;
            return true;
        }
        yyerror("key not found");
        return false;
    }

    yyerror("cannot index %s", typename_of(container));
    return false;
}

/*
 * Fast read path for expressions rooted at a variable, e.g. a[i] or a[i][j].
 * It borrows the variable's container instead of duplicating the whole value.
 * Only used when every index expression is side-effect free, so evaluating
 * the indices cannot invalidate the borrowed container.
 */
static bool read_index_chain(struct ast *a, struct value_t *&out) {
    std::vector<struct ast *> index_asts;
    std::vector<bool> fields;
    if (!collect_index_asts(a, index_asts, fields))
        return false;
    /* Dot access has its own validation; use the slow path for it. */
    for (bool field : fields)
        if (field)
            return false;
    for (struct ast *idx : index_asts) {
        if (!is_read_only(idx))
            return false;
    }

    struct ast *base = index_base(a);
    struct symbol *sym = lookupsym(((struct symref *)base)->name);
    if (!sym || !sym->val) {
        yyerror("symbol not found %s", ((struct symref *)base)->name);
        out = NULL;
        return true;
    }

    struct value_t *container = sym->val;
    bool container_owned = false;

    for (size_t k = 0; k < index_asts.size(); ++k) {
        if (container->type == T_struct) {
            if (container_owned) freeval(container);
            return false;
        }
        struct value_t *idx = eval(index_asts[k]);
        if (!idx) {
            if (container_owned) freeval(container);
            out = NULL;
            return true;
        }

        struct value_t *next = NULL;
        bool next_owned = false;
        bool ok = index_read_step(container, idx, &next, &next_owned);
        freeval(idx);
        if (!ok) {
            if (container_owned) freeval(container);
            out = NULL;
            return true;
        }

        if (k + 1 == index_asts.size()) {
            out = next_owned ? next : dupval(next);
            if (container_owned) freeval(container);
            return true;
        }

        /*
         * A borrowed child of an owned string cannot occur in the current
         * type system, but duplicate defensively if it ever does.
         */
        if (container_owned && !next_owned) {
            next = dupval(next);
            next_owned = true;
        }
        if (container_owned) freeval(container);
        container = next;
        container_owned = next_owned;
    }

    out = NULL;
    return true;
}

static struct value_t *default_value_for_type(
    const struct type_constraint *type) {
    if (type && type->nullable)
        return newnull();
    enum type_constraint_kind kind = type ? type->kind : TYPE_ANY;
    switch (kind) {
        case TYPE_INT:
        case TYPE_NUMBER:
            return newintval(0);
        case TYPE_FLOAT:
            return newfloatval(0.0);
        case TYPE_BOOL:
            return newboolval(false);
        case TYPE_STRING:
            return newstrval("");
        case TYPE_CHAR:
            return newcharval(0);
        case TYPE_BYTE:
            return newbyteval(0);
        case TYPE_BYTES:
            return newbytesval(std::string());
        case TYPE_LIST:
            return (struct value_t *)newlistval();
        case TYPE_DICT:
            return (struct value_t *)newdictval();
        case TYPE_DEQUE:
            return (struct value_t *)newdequeval();
        case TYPE_SET:
            return (struct value_t *)newsetval();
        case TYPE_NULL:
        case TYPE_ANY:
            return newnull();
        case TYPE_FUNCTION:
        case TYPE_BUILTIN:
        case TYPE_CALLABLE:
        case TYPE_USERDATA:
        case TYPE_INVALID:
        default:
            return NULL;
    }
}

static struct value_t *assign_name(struct symref *ref, struct ast *value_ast) {
    struct symbol *s = lookupsym(ref->name);
    if (!s) {
        yyerror("symbol not found %s", ref->name);
        return NULL;
    }
    if (s->is_const) {
        yyerror("cannot assign to constant %s", ref->name);
        return NULL;
    }
    struct value_t *v = eval(value_ast);
    if (!v) return NULL;
    if (v->type == T_return || v->type == T_break ||
        v->type == T_continue)
        return v;
    if (!type_constraint_matches(&s->type, v)) {
        yyerror("variable %s expects %s, got %s", s->name,
                type_constraint_name(&s->type), typename_of(v));
        freeval(v);
        return NULL;
    }
    freeval(s->val);
    s->val = v;
    return dupval(v);
}

static struct value_t *increment_name(struct symref *ref, int delta,
                                      bool prefix) {
    struct symbol *s = lookupsym(ref->name);
    if (!s) {
        yyerror("symbol not found %s", ref->name);
        return NULL;
    }
    if (s->is_const) {
        yyerror("cannot assign to constant %s", ref->name);
        return NULL;
    }
    struct value_t *newv = apply_increment(s->val, delta);
    if (!newv) return NULL;
    if (!type_constraint_matches(&s->type, newv)) {
        yyerror("variable %s expects %s, got %s", s->name,
                type_constraint_name(&s->type), typename_of(newv));
        freeval(newv);
        return NULL;
    }
    struct value_t *ret = prefix ? dupval(newv) : dupval(s->val);
    freeval(s->val);
    s->val = newv;
    return ret;
}

static struct value_t *read_member_value(struct value_t *container,
                                         struct value_t *index,
                                         bool field_access) {
    if (!container || !index)
        return NULL;

    if (container->type == T_list) {
        if (index->type != T_int) {
            yyerror("list index must be an integer, got %s", typename_of(index));
            return NULL;
        }
        long long i = ((struct int_value_t *)index)->value;
        struct list_value_t *lv = (struct list_value_t *)container;
        if (i < 0 || (unsigned long long)i >= lv->payload->items.size()) {
            yyerror("list index %lld out of range", i);
            return NULL;
        }
        return dupval(lv->payload->items[(size_t)i]);
    }

    if (container->type == T_string) {
        if (index->type != T_int) {
            yyerror("string index must be an integer, got %s",
                    typename_of(index));
            return NULL;
        }
        long long i = ((struct int_value_t *)index)->value;
        const char *s = ((struct string_value_t *)container)->value;
        size_t len = utf8_length(s);
        if (i < 0 || (unsigned long long)i >= len) {
            yyerror("string index %lld out of range", i);
            return NULL;
        }
        std::string ch;
        if (!utf8_char_at(s, (size_t)i, ch))
            return NULL;
        size_t consumed = 0;
        unsigned int codepoint = 0;
        if (!utf8_decode_one(ch.data(), ch.size(), consumed, codepoint))
            return NULL;
        return newcharval(codepoint);
    }

    if (container->type == T_bytes) {
        if (index->type != T_int) {
            yyerror("bytes index must be an integer, got %s",
                    typename_of(index));
            return NULL;
        }
        long long i = ((struct int_value_t *)index)->value;
        std::string &data = ((struct bytes_value_t *)container)->value;
        if (i < 0 || (unsigned long long)i >= data.size()) {
            yyerror("bytes index %lld out of range", i);
            return NULL;
        }
        return newintval((unsigned char)data[(size_t)i]);
    }

    if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        if (field_access && !dv->payload->is_namespace) {
            yyerror("cannot access dict key with '.', use [\"...\"]");
            return NULL;
        }
        if (!dict_key_allowed(index)) {
            yyerror("%s, got %s", dict_key_error(), typename_of(index));
            return NULL;
        }
        int found = dict_find_index(dv, index);
        if (found >= 0)
            return dupval(dv->payload->items[(size_t)found].second);
        yyerror("key not found");
        return NULL;
    }

    if (container->type == T_struct) {
        if (index->type != T_string) {
            yyerror("struct field name must be a string, got %s",
                    typename_of(index));
            return NULL;
        }
        struct struct_value_t *sv = (struct struct_value_t *)container;
        if (!sv->def) {
            yyerror("invalid struct value");
            return NULL;
        }
        const char *name = ((struct string_value_t *)index)->value;
        int field = structtype_field_index(sv->def, name);
        if (field >= 0 && sv->payload &&
            (size_t)field < sv->payload->fields.size())
            return dupval(sv->payload->fields[(size_t)field]);
        auto method = sv->def->methods.find(name);
        if (method != sv->def->methods.end())
            return newmethodval(sv->def, sv->payload, method->second);
        yyerror("field %s not found in %s", name, sv->def->name.c_str());
        return NULL;
    }

    if (container->type == T_enum) {
        if (index->type != T_string) {
            yyerror("enum field name must be a string, got %s",
                    typename_of(index));
            return NULL;
        }
        struct enum_value_t *ev = (struct enum_value_t *)container;
        if (!ev->def || ev->variant < 0 ||
            (size_t)ev->variant >= ev->def->variants.size()) {
            yyerror("invalid enum value");
            return NULL;
        }
        const struct variant_def &variant =
            ev->def->variants[(size_t)ev->variant];
        const char *name = ((struct string_value_t *)index)->value;
        int field = structtype_variant_field_index(&variant, name);
        if (field >= 0 && ev->payload &&
            (size_t)field < ev->payload->fields.size())
            return dupval(ev->payload->fields[(size_t)field]);
        yyerror("field %s not found in %s::%s", name,
                ev->def->name.c_str(), variant.name.c_str());
        return NULL;
    }

    yyerror("cannot index %s", typename_of(container));
    return NULL;
}

static struct value_t *call_method_value(struct value_t *receiver,
                                         std::shared_ptr<function_def> def,
                                         struct ast *args_ast) {
    if (call_depth >= MAX_CALL_DEPTH) {
        yyerror("maximum recursion depth (%d) exceeded", MAX_CALL_DEPTH);
        return NULL;
    }
    call_depth_guard depth_guard;

    std::vector<struct value_t *> args;
    args.reserve(8);
    args.push_back(dupval(receiver));
    struct value_t *control = NULL;
    if (!collect_args(args_ast, args, control)) {
        free_args(args);
        return control;
    }

    struct func_value_t callee;
    callee.type = T_func;
    callee.def = std::move(def);
    return invoke_value((struct value_t *)&callee, args);
}

struct value_t *eval(struct ast *a) {
    if (!a) return NULL;
    if (g_interrupted) return NULL;
    eval_location_guard location_guard(a->line, a->column);
    struct value_t *ret = NULL;

    switch (a->nodetype) {
        case 'V':
            ret = dupval(((struct val *)a)->val);
            break;

        case 'N': {
            struct symbol *s = lookupsym(((struct symref *)a)->name);
            if (!s) {
                yyerror("symbol not found %s", ((struct symref *)a)->name);
            } else {
                ret = dupval(s->val);
            }
            break;
        }

        case 'J': {
            struct import_node *imp = (struct import_node *)a;
            std::string error;
            int errors_before = diag_error_count();
            struct value_t *ns = package_import(imp->path, imp->base, error);
            if (!ns) {
                if (diag_error_count() == errors_before)
                    yyerror("import %s: %s", imp->path, error.c_str());
                else
                    diag_mark_script_failure();
                break;
            }
            const char *name = imp->alias
                                   ? imp->alias
                                   : import_binding_name(imp->path);
            if (isrepeated(name)) {
                yyerror("import name already exists: %s", name);
                freeval(ns);
            } else {
                struct symbol *sym = newsym(name);
                sym->val = ns;
                savesym(sym);
                const char *path = imp->path;
                if (path && *path && path[0] != '.' && path[0] != '@') {
                    const char *alias = imp->alias;
                    if (!alias) {
                        const char *dot = std::strrchr(path, '.');
                        alias = dot ? dot + 1 : path;
                    }
                    structtype_register_alias(
                        diag_current_source_name().c_str(), alias, path);
                }
            }
            break;
        }

        case 'K': {
            struct package_node *pkg = (struct package_node *)a;
            int errors_before = eval_ctx().runtime_error_count;
            std::string error;

            if (package_is_loading())
                eval_pushscope();
            else
                eval_push_isolated_scope();

            std::string saved_module_name = diag_current_module_name();
            std::string package_module_name =
                (saved_module_name.empty() || saved_module_name == "main")
                    ? pkg->name
                    : saved_module_name;
            diag_set_module_name(package_module_name);
            struct value_t *body_ret = eval(pkg->body);
            if (body_ret &&
                (body_ret->type == T_return || body_ret->type == T_break ||
                 body_ret->type == T_continue)) {
                yyerror("invalid control flow in package %s", pkg->name);
            }
            value_t *ns = NULL;
            if (eval_ctx().runtime_error_count == errors_before)
                ns = package_build_namespace(current_scope().get(), error);
            eval_popscope();
            diag_set_module_name(saved_module_name);

            freeval(body_ret);

            if (!ns) {
                if (eval_ctx().runtime_error_count == errors_before)
                    yyerror("package %s: %s", pkg->name,
                            error.empty() ? "cannot build namespace"
                                          : error.c_str());
                break;
            }

            if (!package_publish(pkg->name, ns, error)) {
                yyerror("package %s: %s", pkg->name, error.c_str());
                freeval(ns);
            }
            break;
        }

        case NODE_DESTRUCTURE: {
            struct destructure_node *d = (struct destructure_node *)a;
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *value = eval(d->value);
            if (!value ||
                eval_ctx().runtime_error_count != errors_before) {
                freeval(value);
                break;
            }

            std::vector<pattern_binding> bindings;
            if (!match_pattern(d->pattern, value, bindings)) {
                free_pattern_bindings(bindings);
                if (eval_ctx().runtime_error_count == errors_before)
                    yyerror("destructuring pattern did not match %s",
                            typename_of(value));
                freeval(value);
                break;
            }

            bool ok = true;
            for (auto &binding : bindings) {
                if (isrepeated(binding.name.c_str())) {
                    yyerror("symbol repeated %s", binding.name.c_str());
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                free_pattern_bindings(bindings);
                freeval(value);
                break;
            }

            for (auto &binding : bindings) {
                struct symbol *symbol = newsym(binding.name.c_str());
                symbol->val = binding.value;
                binding.value = NULL;
                symbol->is_const = d->is_const != 0;
                savesym(symbol);
            }
            bindings.clear();
            freeval(value);
            break;
        }

        case 'D': {
            struct symdef *def = (struct symdef *)a;
            char *name = def->name;
            if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else if (!validate_type_constraint(def->type)) {
                break;
            } else {
                struct value_t *v = def->v
                    ? eval(def->v)
                    : default_value_for_type(def->type);
                if (!v) {
                    if (!def->v)
                        yyerror("variable %s of type %s requires an initializer",
                                name, type_constraint_name(def->type));
                    break;
                }
                if (v->type == T_return || v->type == T_break ||
                    v->type == T_continue) {
                    ret = v;
                    break;
                }
                if (!type_constraint_matches(def->type, v)) {
                    yyerror("variable %s expects %s, got %s", name,
                            type_constraint_name(def->type), typename_of(v));
                    freeval(v);
                    break;
                }
                struct symbol *s = newsym(name);
                s->val = v;
                if (def->type) {
                    s->type.kind = def->type->kind;
                    s->type.name = def->type->name;
                }
                s->exported = def->exported;
                s->is_const = def->is_const;
                savesym(s);
            }
            break;
        }

        case 'Y':
            eval_struct_decl((struct struct_node *)a);
            break;

        case NODE_ENUM_DECL:
            eval_enum_decl((struct enum_node *)a);
            break;

        case 'H':
            eval_impl_decl((struct impl_node *)a);
            break;

        case 't':
            eval_trait_decl((struct trait_node *)a);
            break;

        case 'y':
            ret = eval_struct_lit((struct struct_lit_node *)a);
            break;

        case NODE_ENUM_LITERAL:
            ret = eval_enum_lit((struct enum_lit_node *)a);
            break;

        case 'Z': {
            struct funcdef *f = (struct funcdef *)a;
            char *name = f->name;
            struct symlist *sl = f->sl;
            const char *display_name = name ? name : "<anonymous>";
            bool bad = false;
            for (struct symlist *p = sl; p && !bad; p = p->next)
                for (struct symlist *q = p->next; q; q = q->next)
                    if (!std::strcmp(p->sym->name, q->sym->name)) {
                        yyerror("duplicate parameter %s in function %s",
                                p->sym->name, display_name);
                        bad = true;
                        break;
                    }
            for (struct symlist *p = sl; p && !bad; p = p->next) {
                if (!validate_type_constraint(p->type))
                    bad = true;
                if (p->variadic && p->next) {
                    yyerror("variadic parameter must be last");
                    bad = true;
                }
            }
            if (bad)
                break;

            std::shared_ptr<scope_t> closure = current_scope();
            /* Top-level functions resolve globals through the fallback parent. */
            if (closure && closure->is_global)
                closure.reset();

            if (!name) {
                std::shared_ptr<function_code> code = get_function_code(f);
                f->moved = 1;
                ret = newfuncval(code, closure);
            } else if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else {
                std::shared_ptr<function_code> code = get_function_code(f);
                struct symbol *sym = newsym(name);
                sym->val = newfuncval(code, closure);
                sym->exported = f->exported;
                f->moved = 1;
                savesym(sym);
            }
            break;
        }

        case 'R': {
            struct ast *value_ast = ((struct fnret *)a)->v;
            struct value_t *v = eval(value_ast);
            if (value_ast && !v)
                break;
            if (v && (v->type == T_return || v->type == T_break ||
                      v->type == T_continue)) {
                ret = v;
                break;
            }
            if (!v)
                v = newnull();
            if (!retval(v)) {
                yyerror("return outside function");
                freeval(v);
                break;
            }
            ret = newreturn();
            break;
        }

        case 'B':
            diag_set_flow_location(a->line, a->column);
            ret = newbreak(((struct nbreak *)a)->label);
            break;

        case 'O':
            diag_set_flow_location(a->line, a->column);
            ret = newcontinue(((struct ncontinue *)a)->label);
            break;

        case 'g': {
            struct label_node *node = (struct label_node *)a;
            if (!label_directly_applies(node->stmt)) {
                yyerror("label must be attached to a loop or switch");
                break;
            }
            eval_ctx().pending_label = node->name;
            ret = eval(node->stmt);
            eval_ctx().pending_label = NULL;
            break;
        }

        case 'u': {
            struct control_stmt *stmt = (struct control_stmt *)a;
            struct value_t *value =
                stmt->value ? eval(stmt->value) : newnull();
            if (!value)
                break;
            if (eval_ctx().control != EVAL_CONTROL_NONE) {
                freeval(value);
                break;
            }
            set_eval_control(EVAL_CONTROL_FAIL,
                             normalize_error(value, "fail"));
            break;
        }

        case 'v': {
            if (eval_ctx().handler_depth <= 0) {
                yyerror("recover outside error handler");
                break;
            }
            struct control_stmt *stmt = (struct control_stmt *)a;
            struct value_t *value =
                stmt->value ? eval(stmt->value) : newnull();
            if (!value)
                break;
            if (eval_ctx().control != EVAL_CONTROL_NONE) {
                freeval(value);
                break;
            }
            set_eval_control(EVAL_CONTROL_RECOVER, value);
            break;
        }

        case 'p': {
            if (eval_ctx().handler_depth <= 0) {
                yyerror("pass outside error handler");
                break;
            }
            struct control_stmt *stmt = (struct control_stmt *)a;
            struct value_t *value = eval(stmt->value);
            if (!value)
                break;
            if (eval_ctx().control != EVAL_CONTROL_NONE) {
                freeval(value);
                break;
            }
            set_eval_control(EVAL_CONTROL_FAIL,
                             normalize_error(value, "fail"));
            break;
        }

        case 'o': {
            struct on_error_node *handler = (struct on_error_node *)a;
            eval_capture_snapshot snapshot;
            eval_capture_begin(snapshot);
            struct value_t *value = eval(handler->call);
            struct eval_capture_result captured = eval_capture_end(snapshot);
            if (g_interrupted) {
                freeval(value);
                freeval(captured.error);
                break;
            }
            struct value_t *error_value = captured.error;
            if (captured.failed) {
                freeval(value);
                value = NULL;
            } else if (value && value->type == T_result) {
                struct result_value_t *result =
                    (struct result_value_t *)value;
                if (result->ok) {
                    ret = dupval(result->value);
                    freeval(value);
                    freeval(error_value);
                    break;
                }
                freeval(error_value);
                error_value = dupval(result->error);
                freeval(value);
                value = NULL;
            }
            if (value) {
                freeval(error_value);
                ret = value;
                break;
            }

            if (!error_value) {
                if (eval_ctx().control != EVAL_CONTROL_FAIL)
                    break;
                error_value = take_eval_control(EVAL_CONTROL_FAIL);
            }
            error_value = normalize_error(error_value, "fail");
            eval_pushscope();
            struct symbol *error_sym = newsym(handler->name);
            error_sym->val = error_value;
            error_sym->is_const = true;
            savesym(error_sym);

            int handler_errors = eval_ctx().runtime_error_count;
            int saved_handler_depth = eval_ctx().handler_depth;
            eval_ctx().handler_depth = saved_handler_depth + 1;
            struct value_t *body = eval(handler->body);
            eval_ctx().handler_depth = saved_handler_depth;
            eval_popscope();

            if (eval_ctx().control == EVAL_CONTROL_RECOVER) {
                ret = take_eval_control(EVAL_CONTROL_RECOVER);
                freeval(body);
                break;
            }
            if (eval_ctx().control == EVAL_CONTROL_FAIL) {
                freeval(body);
                break;
            }
            if (body && (body->type == T_return || body->type == T_break ||
                         body->type == T_continue)) {
                freeval(body);
                yyerror("error handler cannot return, break, or continue; "
                        "use recover, pass, or fail");
                break;
            }
            if (eval_ctx().runtime_error_count != handler_errors) {
                freeval(body);
                break;
            }
            freeval(body);
            yyerror("error handler must end with recover, pass, or fail");
            break;
        }

        case 'X': {
            eval_pushscope();
            ret = eval(((struct nscope *)a)->ast);
            eval_popscope();
            break;
        }

        case '=': {
            struct symasgn *asgn = (struct symasgn *)a;
            if (asgn->target->nodetype == 'N')
                ret = assign_name((struct symref *)asgn->target, asgn->v);
            else if (asgn->target->nodetype == 'G' ||
                     asgn->target->nodetype == 'j')
                ret = assign_index(asgn->target, asgn->v);
            else
                yyerror("cannot assign to this expression");
            break;
        }

        case 'a': {
            struct compound_asgn *asgn = (struct compound_asgn *)a;
            if (asgn->target->nodetype == 'N')
                ret = compound_name((struct symref *)asgn->target, asgn->op,
                                    asgn->v);
            else if (asgn->target->nodetype == 'G' ||
                     asgn->target->nodetype == 'j')
                ret = compound_index(asgn->target, asgn->op, asgn->v);
            else
                yyerror("cannot assign to this expression");
            break;
        }

        case 'U': {
            struct incdec_node *u = (struct incdec_node *)a;
            if (u->target->nodetype == 'N')
                ret = increment_name((struct symref *)u->target, u->delta,
                                     u->prefix);
            else if (u->target->nodetype == 'G' ||
                     u->target->nodetype == 'j')
                ret = increment_index(u->target, u->delta, u->prefix);
            else
                yyerror("cannot %s this expression",
                        u->delta > 0 ? "increment" : "decrement");
            break;
        }

        case 'l': {
            struct list_node *p = (struct list_node *)a;
            struct list_value_t *v = newlistval();
            while (p != NULL) {
                if (p->value) {
                    struct value_t *item = eval(p->value);
                    if (!item) {
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    v->payload->items.push_back(item);
                }
                p = (struct list_node *)p->next;
            }
            ret = (struct value_t *)v;
            break;
        }

        case 'd': {
            struct dict_node *p = (struct dict_node *)a;
            struct dict_value_t *v = newdictval();
            while (p != NULL) {
                if (p->key && p->value) {
                    struct value_t *key = eval(p->key);
                    if (!key) {
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    struct value_t *val = eval(p->value);
                    if (!val) {
                        freeval(key);
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    if (!dict_key_allowed(key)) {
                        yyerror("%s, got %s", dict_key_error(),
                                typename_of(key));
                        freeval(key);
                        freeval(val);
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    int found = dict_find_index(v, key);
                    if (found >= 0) {
                        freeval(v->payload->items[(size_t)found].second);
                        v->payload->items[(size_t)found].second = val;
                        freeval(key);
                    } else {
                        dict_append_item(v, key, val);
                    }
                }
                p = (struct dict_node *)p->next;
            }
            ret = (struct value_t *)v;
            break;
        }

        case 'b': {
            if (!eval_ctx().allow_await) {
                yyerror("await outside async function");
                break;
            }
            struct value_t *v = eval(a->l);
            if (!v)
                break;
            task_ptr task;
            if (get_task_value(v, task))
                ret = fiber_await_task(task);
            freeval(v);
            break;
        }

        case 'w': {
            struct value_t *v = a->l ? eval(a->l) : newnull();
            if (!v)
                break;
            ret = fiber_yield_value(v);
            break;
        }

        case 'n': {
            eval_context &context = eval_ctx();
            if (context.defer_stack.size() <= 1) {
                yyerror("defer is only allowed inside a function or block");
                break;
            }
            context.defer_stack.back().push_back(a->l);
            ret = newnull();
            break;
        }

        case 'M': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            if (v->type == T_int) {
                long long n = ((struct int_value_t *)v)->value;
                ret = new_int_checked(-(__int128)n);
            } else if (v->type == T_float) {
                ret = newfloatval(-((struct float_value_t *)v)->value);
            } else {
                yyerror("cannot negate %s", typename_of(v));
            }
            freeval(v);
            break;
        }

        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 's':
        case 'r': {
            struct value_t *l = eval(a->l);
            if (!l)
                break;
            struct value_t *r = eval(a->r);
            if (!r) {
                freeval(l);
                break;
            }
            ret = apply_binary_value(a->nodetype, l, r);
            freeval(l);
            freeval(r);
            break;
        }

        case 'A': {
            struct value_t *l = eval(a->l);
            if (!l) break;
            bool left = truthy(l);
            freeval(l);
            if (!left) {
                ret = newboolval(false);
                break;
            }
            struct value_t *r = eval(a->r);
            if (!r) break;
            ret = newboolval(truthy(r));
            freeval(r);
            break;
        }

        case 'Q': {
            struct value_t *l = eval(a->l);
            if (!l) break;
            bool left = truthy(l);
            freeval(l);
            if (left) {
                ret = newboolval(true);
                break;
            }
            struct value_t *r = eval(a->r);
            if (!r) break;
            ret = newboolval(truthy(r));
            freeval(r);
            break;
        }

        case '!': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            ret = newboolval(!truthy(v));
            freeval(v);
            break;
        }

        case '~': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            if (v->type != T_int) {
                yyerror("bitwise not expects an int, got %s", typename_of(v));
            } else {
                ret = newintval(~((struct int_value_t *)v)->value);
            }
            freeval(v);
            break;
        }

        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l && r)
                ret = do_compare(a->nodetype - '0', l, r);
            freeval(l);
            freeval(r);
            break;
        }

        case NODE_MATCH: {
            struct match_node *m = (struct match_node *)a;
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *subject = eval(m->expr);
            if (!subject ||
                eval_ctx().runtime_error_count != errors_before) {
                freeval(subject);
                break;
            }

            bool matched = false;
            bool aborted = false;

            for (struct ast *arm_ast = m->arms; arm_ast;
                 arm_ast = ((struct match_arm *)arm_ast)->next) {
                struct match_arm *arm = (struct match_arm *)arm_ast;
                std::vector<pattern_binding> bindings;

                if (arm->pattern &&
                    !match_pattern(arm->pattern, subject, bindings)) {
                    free_pattern_bindings(bindings);
                    if (eval_ctx().runtime_error_count != errors_before) {
                        aborted = true;
                        break;
                    }
                    continue;
                }

                eval_pushscope();
                for (auto &binding : bindings) {
                    struct symbol *symbol = newsym(binding.name.c_str());
                    symbol->val = binding.value;
                    binding.value = NULL;
                    symbol->is_const = true;
                    savesym(symbol);
                }
                bindings.clear();

                bool guard_ok = true;
                if (arm->guard) {
                    struct value_t *guard = eval(arm->guard);
                    if (!guard) {
                        eval_popscope();
                        aborted = true;
                        break;
                    }
                    guard_ok = truthy(guard);
                    freeval(guard);
                }
                if (!guard_ok) {
                    eval_popscope();
                    continue;
                }

                int body_errors = eval_ctx().runtime_error_count;
                struct value_t *body_ret = eval(arm->body);
                eval_popscope();
                if (eval_ctx().runtime_error_count != body_errors) {
                    freeval(body_ret);
                    aborted = true;
                    break;
                }

                matched = true;
                ret = body_ret;
                break;
            }

            if (!matched && !aborted &&
                eval_ctx().runtime_error_count == errors_before) {
                yyerror("match: no arm matched");
            }
            freeval(subject);
            break;
        }

        case 'S': {
            struct switch_node *sw = (struct switch_node *)a;
            const char *switch_label = take_pending_label();
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *subject = eval(sw->expr);
            if (!subject || eval_ctx().runtime_error_count != errors_before) {
                freeval(subject);
                break;
            }
            if (!switch_value_supported(subject)) {
                yyerror("switch does not support %s", typename_of(subject));
                freeval(subject);
                break;
            }

            auto run_body = [&](struct ast *body) {
                eval_pushscope();
                int body_errors = eval_ctx().runtime_error_count;
                struct value_t *body_ret = eval(body);
                if (eval_ctx().runtime_error_count != body_errors) {
                    freeval(body_ret);
                } else if (body_ret) {
                    if (body_ret->type == T_break) {
                        if (control_matches_label(body_ret, switch_label))
                            freeval(body_ret);
                        else
                            ret = body_ret;
                    } else if (body_ret->type == T_continue ||
                               body_ret->type == T_return) {
                        ret = body_ret;
                    } else {
                        freeval(body_ret);
                    }
                }
                eval_popscope();
            };

            struct case_node *default_case = NULL;
            bool matched = false;
            bool stop = false;

            for (struct case_node *c = (struct case_node *)sw->cases;
                 c && !stop; c = (struct case_node *)c->next) {
                if (!c->values) {
                    default_case = c;
                    continue;
                }
                if (case_values_have_condition(c->values)) {
                    yyerror("logical or comparison expressions are not allowed "
                            "in case values");
                    stop = true;
                    break;
                }

                errors_before = eval_ctx().runtime_error_count;
                struct value_t *values = eval(c->values);
                if (!values || eval_ctx().runtime_error_count != errors_before) {
                    freeval(values);
                    stop = true;
                    break;
                }
                if (values->type != T_list) {
                    yyerror("switch case values must be a list");
                    freeval(values);
                    stop = true;
                    break;
                }

                struct list_value_t *list = (struct list_value_t *)values;
                for (size_t i = 0; i < list->payload->items.size(); ++i) {
                    if (!switch_value_supported(list->payload->items[i])) {
                        yyerror("switch does not support %s case value",
                                typename_of(list->payload->items[i]));
                        stop = true;
                        break;
                    }
                }
                if (!stop) {
                    for (size_t i = 0; i < list->payload->items.size(); ++i) {
                        if (value_equals(subject, list->payload->items[i])) {
                            matched = true;
                            break;
                        }
                    }
                }
                freeval(values);

                if (stop) break;
                if (matched) {
                    run_body(c->body);
                    stop = true;
                    break;
                }
            }

            if (!stop && !matched && default_case)
                run_body(default_case->body);

            freeval(subject);
            break;
        }

        case 'T': {
            struct flow *f = (struct flow *)a;
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *cond = eval(f->cond);
            if (eval_ctx().runtime_error_count != errors_before) {
                freeval(cond);
                break;
            }
            if (!cond) break;
            bool b = truthy(cond);
            freeval(cond);
            ret = eval(b ? f->tl : f->el);
            break;
        }

        case 'I': {
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *cond = eval(((struct flow *)a)->cond);
            if (eval_ctx().runtime_error_count != errors_before) {
                freeval(cond);
                break;
            }
            if (!cond) break;
            bool b = truthy(cond);
            freeval(cond);
            if (b) {
                if (((struct flow *)a)->tl)
                    ret = eval(((struct flow *)a)->tl);
            } else {
                if (((struct flow *)a)->el)
                    ret = eval(((struct flow *)a)->el);
            }
            break;
        }

        case 'E': {
            struct flow *f = (struct flow *)a;
            const char *loop_label = take_pending_label();
            if (!f->cond || !f->tl) break;

            for (;;) {
                if (g_interrupted) break;

                int errors_before = eval_ctx().runtime_error_count;
                struct value_t *body = eval(f->tl);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(body);
                    break;
                }
                if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(body);
                    break;
                }

                bool do_break = false;
                if (body) {
                    if (body->type == T_return) {
                        ret = body;
                        break;
                    }
                    if (body->type == T_break) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                            do_break = true;
                        } else {
                            ret = body;
                            break;
                        }
                    } else if (body->type == T_continue) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                        } else {
                            ret = body;
                            break;
                        }
                    } else {
                        freeval(body);
                    }
                }
                if (do_break) break;

                errors_before = eval_ctx().runtime_error_count;
                struct value_t *cond = eval(f->cond);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(cond);
                    break;
                }
                if (!cond) break;
                bool b = truthy(cond);
                freeval(cond);
                if (!b) break;
            }
            break;
        }

        case 'W': {
            const char *loop_label = take_pending_label();
            struct ast *cond_ast = ((struct flow *)a)->cond;
            struct ast *body = ((struct flow *)a)->tl;
            if (!body) break;
            for (;;) {
                if (g_interrupted) break;
                int errors_before = eval_ctx().runtime_error_count;
                struct value_t *cond = eval(cond_ast);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(cond);
                    break;
                }
                if (!cond) break;
                bool b = truthy(cond);
                freeval(cond);
                if (!b) break;

                errors_before = eval_ctx().runtime_error_count;
                ret = eval(body);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(ret);
                    ret = NULL;
                    break;
                }
                if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(ret);
                    ret = NULL;
                    break;
                }
                if (ret && ret->type == T_return)
                    break;
                if (ret && ret->type == T_break) {
                    if (control_matches_label(ret, loop_label)) {
                        freeval(ret);
                        ret = NULL;
                        break;
                    }
                    break;
                }
                if (ret && ret->type == T_continue) {
                    if (control_matches_label(ret, loop_label)) {
                        freeval(ret);
                        ret = NULL;
                        continue;
                    }
                    break;
                }
                freeval(ret);
                ret = NULL;
            }
            break;
        }

        case 'F': {
            struct forinloop *f = (struct forinloop *)a;
            const char *loop_label = take_pending_label();
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *iter = eval(f->iterable);
            if (!iter || eval_ctx().runtime_error_count != errors_before) {
                freeval(iter);
                break;
            }

            if (fiber_is_generator(iter)) {
                if (f->value_name) {
                    yyerror("two-variable for-in requires dict, got generator");
                    freeval(iter);
                    break;
                }
                generator_ptr generator;
                if (!get_generator_value(iter, generator)) {
                    freeval(iter);
                    break;
                }

                eval_pushscope();
                struct symbol *sym = newsym(f->name);
                sym->val = NULL;
                savesym(sym);

                bool leave = false;
                while (!leave) {
                    if (g_interrupted)
                        break;

                    struct value_t *item = NULL;
                    bool done = false;
                    if (!generator_next_value(generator, NULL, item, done)) {
                        freeval(item);
                        leave = true;
                        break;
                    }
                    if (done) {
                        freeval(item);
                        break;
                    }
                    if (sym->val)
                        freeval(sym->val);
                    sym->val = item;

                    errors_before = eval_ctx().runtime_error_count;
                    struct value_t *body = eval(f->body);
                    if (eval_ctx().runtime_error_count != errors_before) {
                        freeval(body);
                        leave = true;
                        break;
                    }
                    if (eval_ctx().control != EVAL_CONTROL_NONE) {
                        freeval(body);
                        leave = true;
                        break;
                    }
                    if (body) {
                        if (body->type == T_return) {
                            ret = body;
                            leave = true;
                            break;
                        }
                        if (body->type == T_break) {
                            if (control_matches_label(body, loop_label)) {
                                freeval(body);
                                leave = true;
                                break;
                            }
                            ret = body;
                            leave = true;
                            break;
                        }
                        if (body->type == T_continue) {
                            if (control_matches_label(body, loop_label)) {
                                freeval(body);
                                continue;
                            }
                            ret = body;
                            leave = true;
                            break;
                        }
                        freeval(body);
                    }
                }

                eval_popscope();
                freeval(iter);
                break;
            }

            if (iter->type == T_deque || iter->type == T_heap ||
                iter->type == T_set) {
                struct list_value_t *materialized = newlistval();
                std::vector<value_t *> items;
                if (iter->type == T_deque)
                    deque_items_in_order(
                        ((struct deque_value_t *)iter)->payload.get(), items);
                else if (iter->type == T_heap)
                    heap_items_level_order(
                        ((struct heap_value_t *)iter)->payload.get(), items);
                else
                    set_values_in_order(
                        ((struct set_value_t *)iter)->payload.get(), items);
                materialized->payload->items.reserve(items.size());
                for (struct value_t *item : items)
                    materialized->payload->items.push_back(dupval(item));
                freeval(iter);
                iter = (struct value_t *)materialized;
            }

            size_t count = 0;
            if (iter->type == T_list)
                count = ((struct list_value_t *)iter)->payload->items.size();
            else if (iter->type == T_dict)
                count = ((struct dict_value_t *)iter)->payload->items.size();
            else if (iter->type == T_string)
                count = utf8_length(((struct string_value_t *)iter)->value);
            else if (iter->type == T_bytes)
                count = ((struct bytes_value_t *)iter)->value.size();
            else {
                yyerror("cannot iterate %s", typename_of(iter));
                freeval(iter);
                break;
            }

            if (f->value_name && iter->type != T_dict) {
                yyerror("two-variable for-in requires dict, got %s",
                        typename_of(iter));
                freeval(iter);
                break;
            }

            if (f->value_name &&
                std::strcmp(f->name, f->value_name) == 0) {
                yyerror("loop variable repeated: %s", f->name);
                freeval(iter);
                break;
            }

            eval_pushscope();
            struct symbol *sym = newsym(f->name);
            sym->val = NULL;
            savesym(sym);
            struct symbol *value_sym = NULL;
            if (f->value_name) {
                value_sym = newsym(f->value_name);
                value_sym->val = NULL;
                savesym(value_sym);
            }

            bool leave = false;
            for (size_t i = 0; i < count && !leave; ++i) {
                if (g_interrupted)
                    break;

                struct value_t *item = NULL;
                struct value_t *second = NULL;
                if (iter->type == T_list) {
                    item = dupval(
                        ((struct list_value_t *)iter)->payload->items[i]);
                } else if (iter->type == T_dict) {
                    struct dict_value_t *dict = (struct dict_value_t *)iter;
                    item = dupval(dict->payload->items[i].first);
                    if (f->value_name)
                        second = dupval(dict->payload->items[i].second);
                } else if (iter->type == T_string) {
                    std::string ch;
                    if (!utf8_char_at(((struct string_value_t *)iter)->value, i,
                                      ch)) {
                        yyerror("invalid UTF-8 string iteration");
                        leave = true;
                        break;
                    }
                    size_t consumed = 0;
                    unsigned int codepoint = 0;
                    if (!utf8_decode_one(ch.data(), ch.size(), consumed,
                                         codepoint)) {
                        yyerror("invalid UTF-8 string iteration");
                        leave = true;
                        break;
                    }
                    item = newcharval(codepoint);
                } else {
                    item = newintval((unsigned char)
                        ((struct bytes_value_t *)iter)->value[i]);
                }

                freeval(sym->val);
                sym->val = item;
                if (value_sym) {
                    freeval(value_sym->val);
                    value_sym->val = second;
                }

                errors_before = eval_ctx().runtime_error_count;
                struct value_t *body = eval(f->body);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (body) {
                    if (body->type == T_return) {
                        ret = body;
                        leave = true;
                        break;
                    }
                    if (body->type == T_break) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                            leave = true;
                            break;
                        }
                        ret = body;
                        leave = true;
                        break;
                    }
                    if (body->type == T_continue) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                            continue;
                        }
                        ret = body;
                        leave = true;
                        break;
                    }
                    freeval(body);
                }
            }

            eval_popscope();
            freeval(iter);
            break;
        }

        case 'P': {
            struct forloop *f = (struct forloop *)a;
            const char *loop_label = take_pending_label();
            eval_pushscope();
            bool leave = false;

            if (f->init) {
                int errors_before = eval_ctx().runtime_error_count;
                struct value_t *v = eval(f->init);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(v);
                    leave = true;
                } else if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(v);
                    leave = true;
                } else if (v) {
                    if (v->type == T_return) {
                        ret = v;
                        leave = true;
                    } else {
                        if (v->type == T_break || v->type == T_continue) {
                            diag_set_location(g_flow_line, g_flow_column);
                            yyerror("break/continue outside loop");
                            leave = true;
                        }
                        freeval(v);
                    }
                }
            }

            for (; !leave;) {
                if (g_interrupted) break;

                bool cond_true = true;
                if (f->cond) {
                    int errors_before = eval_ctx().runtime_error_count;
                    struct value_t *cond = eval(f->cond);
                    if (eval_ctx().runtime_error_count != errors_before) {
                        freeval(cond);
                        leave = true;
                        break;
                    }
                    if (!cond) break;
                    cond_true = truthy(cond);
                    freeval(cond);
                }
                if (!cond_true) break;

                bool do_break = false;
                int errors_before = eval_ctx().runtime_error_count;
                struct value_t *body = eval(f->body);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (body) {
                    if (body->type == T_return) {
                        ret = body;
                        leave = true;
                    } else if (body->type == T_break) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                            do_break = true;
                        } else {
                            ret = body;
                            leave = true;
                        }
                    } else if (body->type == T_continue) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                        } else {
                            ret = body;
                            leave = true;
                        }
                    } else {
                        freeval(body);
                    }
                }
                if (leave || do_break) break;

                if (f->step) {
                    errors_before = eval_ctx().runtime_error_count;
                    struct value_t *step = eval(f->step);
                    if (eval_ctx().runtime_error_count != errors_before) {
                        freeval(step);
                        leave = true;
                        break;
                    }
                    if (eval_ctx().control != EVAL_CONTROL_NONE) {
                        freeval(step);
                        leave = true;
                        break;
                    }
                    if (step) {
                        if (step->type == T_return) {
                            ret = step;
                            leave = true;
                        } else {
                            if (step->type == T_break || step->type == T_continue) {
                                diag_set_location(g_flow_line, g_flow_column);
                                yyerror("break/continue outside loop");
                                leave = true;
                            }
                            freeval(step);
                        }
                    }
                }
            }

            eval_popscope();
            break;
        }

        case 'L': {
            int errors_before = eval_ctx().runtime_error_count;
            ret = eval(a->l);
            if (eval_ctx().runtime_error_count != errors_before) {
                freeval(ret);
                ret = NULL;
                break;
            }
            if (eval_ctx().control != EVAL_CONTROL_NONE)
                break;
            if (ret && (ret->type == T_return ||
                        ret->type == T_break ||
                        ret->type == T_continue))
                break;
            freeval(ret);
            ret = eval(a->r);
            break;
        }

        case 'k': {
            struct is_node *node = (struct is_node *)a;
            struct value_t *value = eval(node->value);
            if (!value)
                break;
            if (!validate_type_constraint(node->type)) {
                freeval(value);
                break;
            }
            bool matched = type_constraint_matches(node->type, value);
            freeval(value);
            ret = newboolval(matched);
            break;
        }

        case 'G': {
            struct value_t *fast = NULL;
            if (read_index_chain(a, fast)) {
                ret = fast;
                break;
            }
            struct value_t *container = eval(a->l);
            if (!container)
                break;
            struct value_t *index = eval(a->r);
            if (!index) {
                freeval(container);
                break;
            }
            ret = read_member_value(container, index);
            freeval(container);
            freeval(index);
            break;
        }

        case 'j': {
            struct value_t *container = eval(a->l);
            if (!container)
                break;
            struct value_t *index = eval(a->r);
            if (!index) {
                freeval(container);
                break;
            }
            ret = read_member_value(container, index, true);
            freeval(container);
            freeval(index);
            break;
        }

        case 'C': {
            struct ufncall *call = (struct ufncall *)a;
            if (call->callee && (call->callee->nodetype == 'G' ||
                                 call->callee->nodetype == 'j')) {
                bool field_access = call->callee->nodetype == 'j';
                struct value_t *receiver = eval(call->callee->l);
                if (!receiver)
                    break;
                struct value_t *key = eval(call->callee->r);
                if (!key) {
                    freeval(receiver);
                    break;
                }
                if (receiver->type == T_struct) {
                    if (key->type != T_string) {
                        yyerror("struct method name must be a string, got %s",
                                typename_of(key));
                    } else {
                        struct struct_value_t *sv =
                            (struct struct_value_t *)receiver;
                        const char *name =
                            ((struct string_value_t *)key)->value;
                        int field =
                            sv->def ? structtype_field_index(sv->def, name)
                                    : -1;
                        if (field >= 0 && sv->payload &&
                            (size_t)field < sv->payload->fields.size()) {
                            struct value_t *member =
                                dupval(sv->payload->fields[(size_t)field]);
                            freeval(receiver);
                            freeval(key);
                            if (member) {
                                ret = callvalue(member, call->args);
                                freeval(member);
                            }
                            break;
                        }
                        bool called = false;
                        if (sv->def) {
                            auto method = sv->def->methods.find(name);
                            if (method != sv->def->methods.end()) {
                                ret = call_method_value(receiver,
                                                        method->second,
                                                        call->args);
                                called = true;
                            }
                        }
                        if (!called) {
                            yyerror("method %s not found in %s", name,
                                    sv->def ? sv->def->name.c_str()
                                            : "struct");
                        }
                    }
                    freeval(receiver);
                    freeval(key);
                    break;
                }

                struct value_t *member =
                    read_member_value(receiver, key, field_access);
                freeval(receiver);
                freeval(key);
                if (member) {
                    ret = callvalue(member, call->args);
                    freeval(member);
                }
                break;
            }

            struct value_t *callee = eval(call->callee);
            if (callee) {
                ret = callvalue(callee, call->args);
                freeval(callee);
            }
            break;
        }

        default:
            std::fprintf(stderr, "internal error: bad node %c\n", a->nodetype);
            break;
    }
    return ret;
}
