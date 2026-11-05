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
    std::vector<const void *> *path_payloads = NULL,
    std::vector<struct value_t *> *holds = NULL) {
    container = s ? s->val : NULL;
    if (path_payloads)
        path_payloads->push_back(value_payload(container));
    for (size_t k = 0; k + 1 < indices.size(); ++k) {
        if (container->type == T_userdata) {
            struct userdata_value_t *userdata =
                (struct userdata_value_t *)container;
            if (!fields[k] || indices[k]->type != T_string ||
                !userdata->meta || !userdata->meta->get_field) {
                yyerror("invalid index assignment path");
                return false;
            }
            struct value_t *elem = userdata->meta->get_field(
                userdata->data.get(),
                ((struct string_value_t *)indices[k])->value);
            if (!elem)
                return false;
            if (holds)
                holds->push_back(elem);
            container = elem;
            if (path_payloads)
                path_payloads->push_back(value_payload(container));
            continue;
        }
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
    } else if (container->type == T_userdata) {
        struct userdata_value_t *userdata =
            (struct userdata_value_t *)container;
        if (!field_access) {
            yyerror("cannot index-assign into %s", typename_of(container));
        } else if (idx->type != T_string) {
            yyerror("%s field name must be a string, got %s",
                    typename_of(container), typename_of(idx));
        } else if (!userdata->meta || !userdata->meta->set_field) {
            yyerror("%s fields are read-only", typename_of(container));
        } else if (userdata->meta->set_field(
                       userdata->data.get(),
                       ((struct string_value_t *)idx)->value, val)) {
            ret = dupval(val);
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
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        freeval(val);
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    std::vector<const void *> path_payloads;
    std::vector<struct value_t *> holds;
    if (!resolve_write_container(s, indices, fields, container,
                                 &path_payloads, &holds)) {
        free_args(holds);
        freeval(val);
        free_args(indices);
        return NULL;
    }
    if (value_reaches_any(val, path_payloads)) {
        yyerror("recursive value would create a cycle");
        free_args(holds);
        freeval(val);
        free_args(indices);
        return NULL;
    }

    struct value_t *ret =
        write_member_value(container, indices.back(), val, fields.back());
    free_args(holds);
    free_args(indices);
    return ret;
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
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    std::vector<struct value_t *> holds;
    if (!resolve_write_container(s, indices, fields, container, NULL,
                                 &holds)) {
        free_args(holds);
        free_args(indices);
        return NULL;
    }

    struct value_t *ret = increment_member_value(
        container, indices.back(), delta, prefix, fields.back());
    free_args(holds);
    free_args(indices);
    return ret;
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

value_t *assign_index_values(const char *base_name,
                             std::vector<value_t *> &indices,
                             const std::vector<bool> &fields,
                             value_t *value) {
    struct symbol *s = lookupsym(base_name);
    if (!s || !s->val) {
        yyerror("symbol not found %s", base_name ? base_name : "");
        freeval(value);
        free_args(indices);
        return NULL;
    }
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        freeval(value);
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    std::vector<const void *> path_payloads;
    std::vector<struct value_t *> holds;
    if (!resolve_write_container(s, indices, fields, container,
                                 &path_payloads, &holds)) {
        free_args(holds);
        freeval(value);
        free_args(indices);
        return NULL;
    }
    if (value_reaches_any(value, path_payloads)) {
        yyerror("recursive value would create a cycle");
        free_args(holds);
        freeval(value);
        free_args(indices);
        return NULL;
    }

    struct value_t *ret =
        write_member_value(container, indices.back(), value, fields.back());
    free_args(holds);
    free_args(indices);
    return ret;
}

value_t *compound_index_values(const char *base_name, int op,
                               std::vector<value_t *> &indices,
                               const std::vector<bool> &fields,
                               value_t *value) {
    struct symbol *s = lookupsym(base_name);
    if (!s || !s->val) {
        yyerror("symbol not found %s", base_name ? base_name : "");
        freeval(value);
        free_args(indices);
        return NULL;
    }
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        freeval(value);
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    std::vector<struct value_t *> holds;
    if (!resolve_write_container(s, indices, fields, container, NULL,
                                 &holds)) {
        free_args(holds);
        freeval(value);
        free_args(indices);
        return NULL;
    }

    struct value_t *idx = indices.back();
    struct value_t *old = read_member_value(container, idx, fields.back());
    if (!old) {
        free_args(holds);
        freeval(value);
        free_args(indices);
        return NULL;
    }
    struct value_t *result = apply_binary_value(op, old, value);
    freeval(old);
    freeval(value);
    if (!result) {
        free_args(holds);
        free_args(indices);
        return NULL;
    }

    struct value_t *ret =
        write_member_value(container, idx, result, fields.back());
    free_args(holds);
    free_args(indices);
    return ret;
}

value_t *increment_index_values(const char *base_name, int delta,
                                bool prefix,
                                std::vector<value_t *> &indices,
                                const std::vector<bool> &fields) {
    struct symbol *s = lookupsym(base_name);
    if (!s || !s->val) {
        yyerror("symbol not found %s", base_name ? base_name : "");
        free_args(indices);
        return NULL;
    }
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    std::vector<struct value_t *> holds;
    if (!resolve_write_container(s, indices, fields, container, NULL,
                                 &holds)) {
        free_args(holds);
        free_args(indices);
        return NULL;
    }

    struct value_t *ret = increment_member_value(
        container, indices.back(), delta, prefix, fields.back());
    free_args(holds);
    free_args(indices);
    return ret;
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
    if (s->is_self) {
        yyerror("cannot modify self in a read-only method");
        freeval(rhs);
        free_args(indices);
        return NULL;
    }

    struct value_t *container = NULL;
    std::vector<struct value_t *> holds;
    if (!resolve_write_container(s, indices, fields, container, NULL,
                                 &holds)) {
        free_args(holds);
        freeval(rhs);
        free_args(indices);
        return NULL;
    }

    struct value_t *idx = indices.back();
    struct value_t *old = read_member_value(container, idx, fields.back());
    if (!old) {
        free_args(holds);
        freeval(rhs);
        free_args(indices);
        return NULL;
    }
    struct value_t *value = apply_binary_value(op, old, rhs);
    freeval(old);
    freeval(rhs);
    if (!value) {
        free_args(holds);
        free_args(indices);
        return NULL;
    }

    struct value_t *ret = write_member_value(container, idx, value,
                                             fields.back());
    free_args(holds);
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

struct value_t *default_value_for_type(
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

struct value_t *assign_symbol_direct(struct symbol *s,
                                     struct value_t *value) {
    if (!s) {
        freeval(value);
        return NULL;
    }
    if (s->is_const) {
        yyerror("cannot assign to constant %s", s->name);
        freeval(value);
        return NULL;
    }
    if (!value)
        return NULL;
    if (value->type == T_return || value->type == T_break ||
        value->type == T_continue)
        return value;
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

struct value_t *compound_symbol_direct(struct symbol *s, int op,
                                       struct value_t *value) {
    if (!s) {
        freeval(value);
        return NULL;
    }
    if (s->is_const) {
        yyerror("cannot assign to constant %s", s->name);
        freeval(value);
        return NULL;
    }
    if (!value)
        return NULL;
    if (value->type == T_return || value->type == T_break ||
        value->type == T_continue)
        return value;
    struct value_t *result = apply_binary_value(op, s->val, value);
    freeval(value);
    if (!result)
        return NULL;
    if (!type_constraint_matches(&s->type, result)) {
        yyerror("variable %s expects %s, got %s", s->name,
                type_constraint_name(&s->type), typename_of(result));
        freeval(result);
        return NULL;
    }
    freeval(s->val);
    s->val = result;
    return dupval(result);
}

struct value_t *increment_symbol_direct(struct symbol *s, int delta,
                                        bool prefix) {
    if (!s)
        return NULL;
    if (s->is_const) {
        yyerror("cannot assign to constant %s", s->name);
        return NULL;
    }
    struct value_t *newv = apply_increment(s->val, delta);
    if (!newv)
        return NULL;
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

struct value_t *assign_symbol_value(const char *name, struct value_t *value) {
    struct symbol *s = lookupsym(name);
    if (!s) {
        yyerror("symbol not found %s", name ? name : "");
        freeval(value);
        return NULL;
    }
    return assign_symbol_direct(s, value);
}

struct value_t *compound_symbol_value(const char *name, int op,
                                      struct value_t *value) {
    struct symbol *s = lookupsym(name);
    if (!s) {
        yyerror("symbol not found %s", name ? name : "");
        freeval(value);
        return NULL;
    }
    return compound_symbol_direct(s, op, value);
}

struct value_t *increment_symbol_value(const char *name, int delta,
                                       bool prefix) {
    struct symbol *s = lookupsym(name);
    if (!s) {
        yyerror("symbol not found %s", name ? name : "");
        return NULL;
    }
    return increment_symbol_direct(s, delta, prefix);
}

struct value_t *read_member_value(struct value_t *container,
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

    if (container->type == T_userdata) {
        struct userdata_value_t *userdata =
            (struct userdata_value_t *)container;
        if (!field_access) {
            yyerror("cannot index %s", typename_of(container));
            return NULL;
        }
        if (index->type != T_string) {
            yyerror("%s field name must be a string, got %s",
                    typename_of(container), typename_of(index));
            return NULL;
        }
        if (!userdata->meta || !userdata->meta->get_field) {
            yyerror("%s values do not have fields", typename_of(container));
            return NULL;
        }
        return userdata->meta->get_field(
            userdata->data.get(), ((struct string_value_t *)index)->value);
    }

    yyerror("cannot index %s", typename_of(container));
    return NULL;
}
