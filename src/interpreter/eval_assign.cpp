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
struct value_t *assign_index(struct ast *target, struct ast *value_ast) {
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
    return assign_index_symbol(s, ((struct symref *)base)->name, indices,
                               fields, val);
}

struct value_t *increment_index(struct ast *target, int delta,
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
    return increment_index_symbol(s, ((struct symref *)base)->name, delta,
                                  prefix, indices, fields);
}

struct value_t *compound_name(struct symref *ref, int op,
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

struct value_t *compound_index(struct ast *target, int op,
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
    return compound_index_symbol(s, ((struct symref *)base)->name, op,
                                 indices, fields, rhs);
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
bool read_index_chain(struct ast *a, struct value_t *&out) {
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

struct value_t *assign_name(struct symref *ref, struct ast *value_ast) {
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

struct value_t *increment_name(struct symref *ref, int delta,
                                      bool prefix) {
    struct symbol *s = lookupsym(ref->name);
    if (!s) {
        yyerror("symbol not found %s", ref->name);
        return NULL;
    }
    return increment_symbol_direct(s, delta, prefix);
}
