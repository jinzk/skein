#include "eval.h"
#include "format.h"
#include "repl.h"
#include "scopestack.h"
#include "symbol.h"
#include "valuetype.h"

int g_runtime_error_count = 0;

#include <climits>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool is_numeric(const struct value_t *v) {
    return v && (v->type == T_int || v->type == T_float);
}

static struct value_t *new_int_checked(__int128 n) {
    if (n < LLONG_MIN || n > LLONG_MAX) {
        yyerror("integer overflow");
        return NULL;
    }
    return newintval((long long)n);
}

static double as_double(const struct value_t *v) {
    if (!v) return 0.0;
    if (v->type == T_int) return (double)((struct int_value_t *)v)->value;
    if (v->type == T_float) return ((struct float_value_t *)v)->value;
    return 0.0;
}

static const char *skip_space(const char *p) {
    while (*p && std::isspace((unsigned char)*p)) ++p;
    return p;
}

static bool parse_int_string(const char *s, long long &out) {
    if (!s) return false;
    errno = 0;
    char *end = NULL;
    long long v = std::strtoll(s, &end, 10);
    if (end == s || errno == ERANGE) return false;
    if (*skip_space(end) != '\0') return false;
    out = v;
    return true;
}

static bool parse_float_string(const char *s, double &out) {
    if (!s) return false;
    errno = 0;
    char *end = NULL;
    double v = std::strtod(s, &end);
    if (end == s) return false;
    if (errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL)) return false;
    if (*skip_space(end) != '\0') return false;
    out = v;
    return true;
}

/*
 * Compare an int64 with a double exactly, returning the sign of (i - d).
 * `unordered` is set for NaN, where all comparisons are false.
 */
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

static int compare_numeric(const struct value_t *a, const struct value_t *b,
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

static size_t utf8_sequence_length(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static size_t utf8_valid_sequence_length(const char *s, size_t available) {
    size_t n = utf8_sequence_length((unsigned char)s[0]);
    if (n > available) return 1;
    for (size_t i = 1; i < n; ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            return 1;
    }
    return n;
}

static size_t utf8_length(const char *s) {
    size_t len = std::strlen(s);
    size_t count = 0;
    for (size_t i = 0; i < len; ++count)
        i += utf8_valid_sequence_length(s + i, len - i);
    return count;
}

static bool utf8_char_at(const char *s, size_t index, std::string &out) {
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

static bool truthy(const struct value_t *v) {
    if (!v) return false;
    switch (v->type) {
        case T_int: return ((struct int_value_t *)v)->value != 0;
        case T_float: return ((struct float_value_t *)v)->value != 0.0;
        case T_string: {
            const char *s = ((struct string_value_t *)v)->value;
            return s && *s;
        }
        case T_list: return !((struct list_value_t *)v)->value.empty();
        case T_dict: return !((struct dict_value_t *)v)->value.empty();
        case T_func: return true;
        case T_builtin: return true;
        case T_null: return false;
        default: return false;
    }
}

static bool value_equals(struct value_t *a, struct value_t *b) {
    if (!a || !b) return a == b;
    if (is_numeric(a) && is_numeric(b)) {
        bool unordered = false;
        int cmp = compare_numeric(a, b, unordered);
        return !unordered && cmp == 0;
    }
    if (a->type != b->type) return false;
    switch (a->type) {
        case T_string:
            return std::strcmp(((struct string_value_t *)a)->value,
                               ((struct string_value_t *)b)->value) == 0;
        case T_list: {
            struct list_value_t *la = (struct list_value_t *)a;
            struct list_value_t *lb = (struct list_value_t *)b;
            if (la->value.size() != lb->value.size()) return false;
            for (size_t i = 0; i < la->value.size(); ++i)
                if (!value_equals(la->value[i], lb->value[i])) return false;
            return true;
        }
        case T_dict: {
            struct dict_value_t *da = (struct dict_value_t *)a;
            struct dict_value_t *db = (struct dict_value_t *)b;
            if (da->value.size() != db->value.size()) return false;
            for (size_t i = 0; i < da->value.size(); ++i) {
                bool found = false;
                for (size_t j = 0; j < db->value.size(); ++j) {
                    if (value_equals(da->value[i].first, db->value[j].first)) {
                        if (!value_equals(da->value[i].second, db->value[j].second))
                            return false;
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
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
        case T_null:
            return true;
        default:
            return false;
    }
}

static struct value_t *do_compare(int op, struct value_t *l, struct value_t *r) {
    if (!l || !r) return NULL;

    if (!(is_numeric(l) && is_numeric(r)) &&
        !(l->type == T_string && r->type == T_string)) {
        if (op == 3 || op == 4) {
            bool eq = value_equals(l, r);
            return newintval((op == 4) ? (eq ? 1 : 0) : (eq ? 0 : 1));
        }
        yyerror("cannot compare %s and %s", typename_of(l), typename_of(r));
        return NULL;
    }

    if (is_numeric(l) && is_numeric(r)) {
        bool unordered = false;
        int cmp = compare_numeric(l, r, unordered);
        if (unordered) {
            /* IEEE 754: NaN is unequal to everything and unordered. */
            if (op == 3) return newintval(1);
            return newintval(0);
        }
        switch (op) {
            case 1: return newintval(cmp > 0 ? 1 : 0);
            case 2: return newintval(cmp < 0 ? 1 : 0);
            case 3: return newintval(cmp != 0 ? 1 : 0);
            case 4: return newintval(cmp == 0 ? 1 : 0);
            case 5: return newintval(cmp >= 0 ? 1 : 0);
            case 6: return newintval(cmp <= 0 ? 1 : 0);
            default: return NULL;
        }
    }

    int cmp = std::strcmp(((struct string_value_t *)l)->value,
                          ((struct string_value_t *)r)->value);
    cmp = (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);

    switch (op) {
        case 1: return newintval(cmp > 0 ? 1 : 0);
        case 2: return newintval(cmp < 0 ? 1 : 0);
        case 3: return newintval(cmp != 0 ? 1 : 0);
        case 4: return newintval(cmp == 0 ? 1 : 0);
        case 5: return newintval(cmp >= 0 ? 1 : 0);
        case 6: return newintval(cmp <= 0 ? 1 : 0);
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
static bool collect_args(struct ast *a, std::vector<struct value_t *> &out) {
    if (!a) return true;
    if (out.capacity() == 0)
        out.reserve(8);
    if (a->nodetype == 'L') {
        if (!collect_args(a->l, out)) return false;
        return collect_args(a->r, out);
    }
    struct value_t *v = eval(a);
    if (!v) return false;
    if (v->type == T_return || v->type == T_break || v->type == T_continue) {
        freeval(v);
        yyerror("invalid argument expression");
        return false;
    }
    out.push_back(v);
    return true;
}

static struct value_t *call_builtin(int functype,
                                    const std::vector<struct value_t *> &args) {
    if (functype == B_list || functype == B_dict) {
        if (!args.empty()) {
            yyerror("built-in function expects 0 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
        return (functype == B_list)
                   ? (struct value_t *)newlistval()
                   : (struct value_t *)newdictval();
    }

    if (functype == B_format || functype == B_print) {
        struct value_t *ret = NULL;
        if (functype == B_format && args.empty()) {
            yyerror("format expects at least 1 argument");
        } else if (functype == B_print && args.empty()) {
            std::printf("\n");
            ret = newnull();
        } else if (functype == B_print && args.size() == 1) {
            printvalue(args[0]);
            std::printf("\n");
            ret = newnull();
        } else if (args[0]->type != T_string) {
            yyerror("%s first argument must be a string, got %s",
                    functype == B_format ? "format" : "print",
                    typename_of(args[0]));
        } else {
            std::vector<struct value_t *> fmt_args(args.begin() + 1, args.end());
            std::string out;
            std::string error;
            if (!format_values(((struct string_value_t *)args[0])->value,
                               fmt_args, out, error)) {
                yyerror("%s", error.c_str());
            } else if (functype == B_format) {
                ret = newstrval(out.c_str());
            } else {
                std::printf("%s\n", out.c_str());
                ret = newnull();
            }
        }
        return ret;
    }

    if (args.size() != 1) {
        yyerror("built-in function expects 1 argument, got %d", (int)args.size());
        return NULL;
    }

    struct value_t *arg = args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_abs:
            if (arg->type == T_int) {
                long long n = ((struct int_value_t *)arg)->value;
                ret = new_int_checked(n < 0 ? -(__int128)n : (__int128)n);
            } else if (arg->type == T_float) {
                ret = newfloatval(std::fabs(((struct float_value_t *)arg)->value));
            } else {
                yyerror("abs expects a number, got %s", typename_of(arg));
            }
            break;
        case B_int:
            if (arg->type == T_int) {
                ret = newintval(((struct int_value_t *)arg)->value);
            } else if (arg->type == T_float) {
                double d = ((struct float_value_t *)arg)->value;
                const double two63 = 9223372036854775808.0;
                if (!std::isfinite(d)) {
                    yyerror("int() value out of range");
                } else {
                    d = std::trunc(d);
                    if (d < -two63 || d >= two63)
                        yyerror("int() value out of range");
                    else
                        ret = newintval((long long)d);
                }
            } else if (arg->type == T_string) {
                long long value = 0;
                if (parse_int_string(((struct string_value_t *)arg)->value, value))
                    ret = newintval(value);
                else
                    yyerror("int() cannot convert string \"%s\"",
                            ((struct string_value_t *)arg)->value);
            } else {
                yyerror("int() expects int, float, or string, got %s",
                        typename_of(arg));
            }
            break;
        case B_float:
            if (arg->type == T_int) {
                ret = newfloatval((double)((struct int_value_t *)arg)->value);
            } else if (arg->type == T_float) {
                ret = newfloatval(((struct float_value_t *)arg)->value);
            } else if (arg->type == T_string) {
                double value = 0.0;
                if (parse_float_string(((struct string_value_t *)arg)->value, value))
                    ret = newfloatval(value);
                else
                    yyerror("float() cannot convert string \"%s\"",
                            ((struct string_value_t *)arg)->value);
            } else {
                yyerror("float() expects int, float, or string, got %s",
                        typename_of(arg));
            }
            break;
        case B_str:
            ret = newstrval(value_to_string(arg).c_str());
            break;
        case B_sqrt:
        case B_exp:
        case B_log:
            if (!is_numeric(arg)) {
                yyerror("math function expects a number, got %s", typename_of(arg));
                break;
            }
            if (functype == B_sqrt)
                ret = newfloatval(std::sqrt(as_double(arg)));
            else if (functype == B_exp)
                ret = newfloatval(std::exp(as_double(arg)));
            else
                ret = newfloatval(std::log(as_double(arg)));
            break;
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}

static constexpr int MAX_CALL_DEPTH = 1000;
static int call_depth = 0;

namespace {
struct call_depth_guard {
    call_depth_guard() { ++call_depth; }
    ~call_depth_guard() { --call_depth; }
};
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
    if (!collect_args(args_ast, args)) {
        free_args(args);
        return NULL;
    }

    if (callee->type == T_builtin) {
        struct value_t *ret = call_builtin(
            ((struct builtin_value_t *)callee)->functype, args);
        free_args(args);
        return ret;
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
    if ((int)args.size() != nparams) {
        yyerror("function expects %d argument(s), got %d",
                nparams, (int)args.size());
        free_args(args);
        return NULL;
    }

    int errors_before = g_runtime_error_count;
    pushscope_closure(def->closure);
    int i = 0;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next, ++i) {
        struct symbol *sym = newsym(sl->sym->name);
        sym->val = args[i];
        savesym(sym);
    }
    args.clear(); /* ownership transferred to the function scope */

    struct value_t *body_ret = eval(def->code->ast);
    struct value_t *result = topscope()->ret;
    topscope()->ret = NULL;

    if (body_ret) {
        if (body_ret->type == T_break)
            yyerror("break outside loop in function");
        else if (body_ret->type == T_continue)
            yyerror("continue outside loop in function");
        freeval(body_ret);
    }

    popscope();

    bool errored = g_runtime_error_count != errors_before;
    if (g_interrupted || errored) {
        freeval(result);
        return NULL;
    }
    return result ? result : newnull();
}

static struct ast *index_base(struct ast *a) {
    while (a && a->nodetype == 'G') a = a->l;
    return a;
}

/*
 * Collect the index expressions of an lvalue chain such as a[i][j].  The base
 * must be a variable; assigning through function calls or literals is not
 * allowed.
 */
static bool collect_index_asts(struct ast *a, std::vector<struct ast *> &out) {
    if (!a) return false;
    if (a->nodetype == 'G') {
        if (!collect_index_asts(a->l, out)) return false;
        out.push_back(a->r);
        return true;
    }
    return a->nodetype == 'N';
}

static struct value_t **index_slot_ptr(struct value_t *container,
                                       struct value_t *idx) {
    if (!container || !idx) return NULL;
    if (container->type == T_list) {
        if (idx->type != T_int) return NULL;
        long long i = ((struct int_value_t *)idx)->value;
        struct list_value_t *lv = (struct list_value_t *)container;
        if (i < 0 || (unsigned long long)i >= lv->value.size()) return NULL;
        return &lv->value[(size_t)i];
    }
    if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        for (size_t i = 0; i < dv->value.size(); ++i)
            if (value_equals(dv->value[i].first, idx))
                return &dv->value[i].second;
    }
    return NULL;
}

static struct value_t *assign_index(struct ast *target, struct ast *value_ast) {
    std::vector<struct ast *> index_asts;
    if (!collect_index_asts(target, index_asts)) {
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

    struct value_t *container = s->val;
    for (size_t k = 0; k + 1 < indices.size(); ++k) {
        struct value_t **slot = index_slot_ptr(container, indices[k]);
        if (!slot || !*slot) {
            yyerror("invalid index assignment path");
            freeval(val);
            free_args(indices);
            return NULL;
        }
        struct value_t *elem = *slot;
        if (elem->type != T_list && elem->type != T_dict) {
            yyerror("cannot index-assign into %s", typename_of(elem));
            freeval(val);
            free_args(indices);
            return NULL;
        }
        container = elem;
    }

    struct value_t *idx = indices.back();
    struct value_t *ret = NULL;
    if (container->type == T_list) {
        if (idx->type != T_int) {
            yyerror("list index must be an integer, got %s", typename_of(idx));
        } else {
            long long i = ((struct int_value_t *)idx)->value;
            struct list_value_t *lv = (struct list_value_t *)container;
            if (i < 0 || (unsigned long long)i >= lv->value.size()) {
                yyerror("list index %lld out of range", i);
            } else {
                freeval(lv->value[(size_t)i]);
                lv->value[(size_t)i] = val;
                val = NULL;
                ret = dupval(lv->value[(size_t)i]);
            }
        }
    } else if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        bool found = false;
        for (size_t i = 0; i < dv->value.size(); ++i) {
            if (value_equals(dv->value[i].first, idx)) {
                freeval(dv->value[i].second);
                dv->value[i].second = val;
                val = NULL;
                found = true;
                ret = dupval(dv->value[i].second);
                break;
            }
        }
        if (!found) {
            dv->value.push_back(std::make_pair(dupval(idx), val));
            val = NULL;
            ret = dupval(dv->value.back().second);
        }
    } else {
        yyerror("cannot index-assign into %s", typename_of(container));
    }

    free_args(indices);
    freeval(val);
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
        case 'G':
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
        if (i < 0 || (unsigned long long)i >= lv->value.size()) {
            yyerror("list index %lld out of range", i);
            return false;
        }
        *next = lv->value[(size_t)i];
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
        *next = newstrval(ch.c_str());
        *next_owned = true;
        return true;
    }

    if (container->type == T_dict) {
        struct dict_value_t *dv = (struct dict_value_t *)container;
        for (size_t i = 0; i < dv->value.size(); ++i) {
            if (value_equals(dv->value[i].first, index)) {
                *next = dv->value[i].second;
                return true;
            }
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
    if (!collect_index_asts(a, index_asts))
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

static struct value_t *assign_name(struct symref *ref, struct ast *value_ast) {
    struct symbol *s = lookupsym(ref->name);
    if (!s) {
        yyerror("symbol not found %s", ref->name);
        return NULL;
    }
    struct value_t *v = eval(value_ast);
    if (!v) return NULL;
    freeval(s->val);
    s->val = v;
    return dupval(v);
}

struct value_t *eval(struct ast *a) {
    if (!a) return NULL;
    if (g_interrupted) return NULL;
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

        case 'D': {
            char *name = ((struct symdef *)a)->name;
            if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else {
                struct value_t *v = eval(((struct symdef *)a)->v);
                if (v) {
                    struct symbol *s = newsym(name);
                    s->val = v;
                    savesym(s);
                }
            }
            break;
        }

        case 'Z': {
            char *name = ((struct funcdef *)a)->name;
            struct symlist *sl = ((struct funcdef *)a)->sl;
            bool bad = false;
            for (struct symlist *p = sl; p && !bad; p = p->next)
                for (struct symlist *q = p->next; q; q = q->next)
                    if (!std::strcmp(p->sym->name, q->sym->name)) {
                        yyerror("duplicate parameter %s in function %s",
                                p->sym->name, name);
                        bad = true;
                        break;
                    }
            if (bad)
                break;
            if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else {
                std::shared_ptr<scope_t> closure = current_scope();
                /* The global scope is always reachable as the fallback parent. */
                if (closure && !closure->parent)
                    closure.reset();
                struct symbol *sym = newsym(name);
                std::shared_ptr<function_code> code =
                    get_function_code((struct funcdef *)a);
                sym->val = newfuncval(code, closure);
                ((struct funcdef *)a)->moved = 1;
                savesym(sym);
            }
            break;
        }

        case 'R': {
            struct ast *value_ast = ((struct fnret *)a)->v;
            struct value_t *v = eval(value_ast);
            if (value_ast && !v)
                break;
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
            ret = newbreak();
            break;

        case 'O':
            ret = newcontinue();
            break;

        case 'X': {
            pushscope();
            ret = eval(((struct nscope *)a)->ast);
            popscope();
            break;
        }

        case '=': {
            struct symasgn *asgn = (struct symasgn *)a;
            if (asgn->target->nodetype == 'N')
                ret = assign_name((struct symref *)asgn->target, asgn->v);
            else if (asgn->target->nodetype == 'G')
                ret = assign_index(asgn->target, asgn->v);
            else
                yyerror("cannot assign to this expression");
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
                    v->value.push_back(item);
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
                    struct value_t *val = eval(p->value);
                    if (!key || !val) {
                        freeval(key);
                        freeval(val);
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    bool replaced = false;
                    for (size_t i = 0; i < v->value.size(); ++i) {
                        if (value_equals(v->value[i].first, key)) {
                            freeval(v->value[i].second);
                            v->value[i].second = val;
                            freeval(key);
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced)
                        v->value.push_back(std::make_pair(key, val));
                }
                p = (struct dict_node *)p->next;
            }
            ret = (struct value_t *)v;
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

        case '+': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_checked((__int128)x + y);
            } else if (is_numeric(l) && is_numeric(r)) {
                ret = newfloatval(as_double(l) + as_double(r));
            } else if (l->type == T_string && r->type == T_string) {
                std::string s = ((struct string_value_t *)l)->value;
                s += ((struct string_value_t *)r)->value;
                ret = newstrval(s.c_str());
            } else if (l->type == T_list && r->type == T_list) {
                struct list_value_t *lv = newlistval();
                struct list_value_t *ll = (struct list_value_t *)l;
                struct list_value_t *rl = (struct list_value_t *)r;
                lv->value.reserve(ll->value.size() + rl->value.size());
                for (size_t i = 0; i < ll->value.size(); ++i)
                    lv->value.push_back(dupval(ll->value[i]));
                for (size_t i = 0; i < rl->value.size(); ++i)
                    lv->value.push_back(dupval(rl->value[i]));
                ret = (struct value_t *)lv;
            } else {
                yyerror("cannot add %s and %s", typename_of(l), typename_of(r));
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '-':
        case '*': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (!is_numeric(l) || !is_numeric(r)) {
                yyerror("cannot use %c on %s and %s", a->nodetype,
                        typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_checked(a->nodetype == '-'
                                          ? (__int128)x - y
                                          : (__int128)x * y);
            } else {
                double x = as_double(l), y = as_double(r);
                ret = newfloatval(a->nodetype == '-' ? x - y : x * y);
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '/': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (!is_numeric(l) || !is_numeric(r)) {
                yyerror("cannot divide %s by %s", typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            if (as_double(r) == 0.0) {
                yyerror("division by zero");
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_checked((__int128)x / y);
            } else {
                ret = newfloatval(as_double(l) / as_double(r));
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '%': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (!is_numeric(l) || !is_numeric(r)) {
                yyerror("cannot take %s modulo %s", typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            if (as_double(r) == 0.0) {
                yyerror("modulo by zero");
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_checked((__int128)x % y);
            } else {
                ret = newfloatval(std::fmod(as_double(l), as_double(r)));
            }
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
                ret = newintval(0);
                break;
            }
            struct value_t *r = eval(a->r);
            if (!r) break;
            ret = newintval(truthy(r) ? 1 : 0);
            freeval(r);
            break;
        }

        case 'Q': {
            struct value_t *l = eval(a->l);
            if (!l) break;
            bool left = truthy(l);
            freeval(l);
            if (left) {
                ret = newintval(1);
                break;
            }
            struct value_t *r = eval(a->r);
            if (!r) break;
            ret = newintval(truthy(r) ? 1 : 0);
            freeval(r);
            break;
        }

        case '!': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            ret = newintval(truthy(v) ? 0 : 1);
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

        case '&':
        case '|':
        case '^': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type != T_int || r->type != T_int) {
                yyerror("bitwise operator expects ints, got %s and %s",
                        typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            long long x = ((struct int_value_t *)l)->value;
            long long y = ((struct int_value_t *)r)->value;
            if (a->nodetype == '&')
                ret = newintval(x & y);
            else if (a->nodetype == '|')
                ret = newintval(x | y);
            else
                ret = newintval(x ^ y);
            freeval(l);
            freeval(r);
            break;
        }

        case 's':
        case 'r': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type != T_int || r->type != T_int) {
                yyerror("shift operator expects ints, got %s and %s",
                        typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            long long x = ((struct int_value_t *)l)->value;
            long long y = ((struct int_value_t *)r)->value;
            if (y < 0 || y >= 64) {
                yyerror("shift count %lld out of range", y);
            } else if (a->nodetype == 's') {
                ret = newintval((long long)((unsigned long long)x << y));
            } else {
                ret = newintval(x >> y);
            }
            freeval(l);
            freeval(r);
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

        case 'I': {
            int errors_before = g_runtime_error_count;
            struct value_t *cond = eval(((struct flow *)a)->cond);
            if (g_runtime_error_count != errors_before) {
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

        case 'W': {
            struct ast *cond_ast = ((struct flow *)a)->cond;
            struct ast *body = ((struct flow *)a)->tl;
            if (!body) break;
            for (;;) {
                if (g_interrupted) break;
                int errors_before = g_runtime_error_count;
                struct value_t *cond = eval(cond_ast);
                if (g_runtime_error_count != errors_before) {
                    freeval(cond);
                    break;
                }
                if (!cond) break;
                bool b = truthy(cond);
                freeval(cond);
                if (!b) break;

                errors_before = g_runtime_error_count;
                ret = eval(body);
                if (g_runtime_error_count != errors_before) {
                    freeval(ret);
                    ret = NULL;
                    break;
                }
                if (ret && ret->type == T_return)
                    break;
                if (ret && ret->type == T_break) {
                    freeval(ret);
                    ret = NULL;
                    break;
                }
                if (ret && ret->type == T_continue) {
                    freeval(ret);
                    ret = NULL;
                    continue;
                }
                freeval(ret);
                ret = NULL;
            }
            break;
        }

        case 'P': {
            struct forloop *f = (struct forloop *)a;
            pushscope();
            bool leave = false;

            if (f->init) {
                int errors_before = g_runtime_error_count;
                struct value_t *v = eval(f->init);
                if (g_runtime_error_count != errors_before) {
                    freeval(v);
                    leave = true;
                } else if (v) {
                    if (v->type == T_return) {
                        ret = v;
                        leave = true;
                    } else {
                        if (v->type == T_break || v->type == T_continue) {
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
                    int errors_before = g_runtime_error_count;
                    struct value_t *cond = eval(f->cond);
                    if (g_runtime_error_count != errors_before) {
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
                int errors_before = g_runtime_error_count;
                struct value_t *body = eval(f->body);
                if (g_runtime_error_count != errors_before) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (body) {
                    if (body->type == T_return) {
                        ret = body;
                        leave = true;
                    } else if (body->type == T_break) {
                        freeval(body);
                        do_break = true;
                    } else {
                        /* normal value and T_continue both run the step */
                        freeval(body);
                    }
                }
                if (leave || do_break) break;

                if (f->step) {
                    errors_before = g_runtime_error_count;
                    struct value_t *step = eval(f->step);
                    if (g_runtime_error_count != errors_before) {
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
                                yyerror("break/continue outside loop");
                                leave = true;
                            }
                            freeval(step);
                        }
                    }
                }
            }

            popscope();
            break;
        }

        case 'L': {
            int errors_before = g_runtime_error_count;
            ret = eval(a->l);
            if (g_runtime_error_count != errors_before) {
                freeval(ret);
                ret = NULL;
                break;
            }
            if (ret && (ret->type == T_return ||
                        ret->type == T_break ||
                        ret->type == T_continue))
                break;
            freeval(ret);
            ret = eval(a->r);
            break;
        }

        case 'G': {
            struct value_t *fast = NULL;
            if (read_index_chain(a, fast)) {
                ret = fast;
                break;
            }
            struct value_t *container = eval(a->l);
            struct value_t *index = eval(a->r);
            if (!container || !index) {
                freeval(container);
                freeval(index);
                break;
            }
            if (container->type == T_list) {
                if (index->type != T_int) {
                    yyerror("list index must be an integer, got %s",
                            typename_of(index));
                } else {
                    long long i = ((struct int_value_t *)index)->value;
                    struct list_value_t *lv = (struct list_value_t *)container;
                    if (i < 0 || (unsigned long long)i >= lv->value.size())
                        yyerror("list index %lld out of range", i);
                    else
                        ret = dupval(lv->value[(size_t)i]);
                }
            } else if (container->type == T_string) {
                if (index->type != T_int) {
                    yyerror("string index must be an integer, got %s",
                            typename_of(index));
                } else {
                    long long i = ((struct int_value_t *)index)->value;
                    const char *s = ((struct string_value_t *)container)->value;
                    size_t len = utf8_length(s);
                    if (i < 0 || (unsigned long long)i >= len) {
                        yyerror("string index %lld out of range", i);
                    } else {
                        std::string ch;
                        if (utf8_char_at(s, (size_t)i, ch))
                            ret = newstrval(ch.c_str());
                    }
                }
            } else if (container->type == T_dict) {
                struct dict_value_t *dv = (struct dict_value_t *)container;
                bool found = false;
                for (size_t i = 0; i < dv->value.size(); ++i) {
                    if (value_equals(dv->value[i].first, index)) {
                        ret = dupval(dv->value[i].second);
                        found = true;
                        break;
                    }
                }
                if (!found)
                    yyerror("key not found");
            } else {
                yyerror("cannot index %s", typename_of(container));
            }
            freeval(container);
            freeval(index);
            break;
        }

        case 'C': {
            struct ufncall *call = (struct ufncall *)a;
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
