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

struct value_t *do_compare(int op, struct value_t *l, struct value_t *r) {
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

struct value_t *apply_binary_value(int op, struct value_t *l,
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
