#include "dict.h"
#include "eval_helpers.h"
#include "cycle.h"
#include "scopestack.h"
#include "structtype.h"
#include "symbol.h"
#include "unicode.h"
#include "valuetype.h"
#include "value_ops.h"
#include "value_runtime.h"

#include "builtin_ids.h"

#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct value_t *new_int_checked(__int128 n) {
    if (n < LLONG_MIN || n > LLONG_MAX) {
        yyerror("integer overflow");
        return NULL;
    }
    return newintval((long long)n);
}


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

static void free_args(std::vector<struct value_t *> &args) {
    for (struct value_t *value : args)
        freeval(value);
    args.clear();
}

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

static bool reject_enum_payload_write(struct value_t *container) {
    if (container && container->type == T_enum) {
        yyerror("enum payload fields are read-only");
        return true;
    }
    return false;
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
    if (reject_enum_payload_write(container))
        return false;
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

    if (reject_enum_payload_write(container))
        return NULL;

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
            std::string &data = ((struct bytes_value_t *)container)->value.write();
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

    if (reject_enum_payload_write(container))
        return NULL;

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
            std::string &data = ((struct bytes_value_t *)container)->value.write();
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



static value_t *assign_index_resolved(struct symbol *s,
                                      const char *base_name,
                                      std::vector<value_t *> &indices,
                                      const std::vector<bool> &fields,
                                      value_t *value) {
    if (!s || !s->val) {
        yyerror("symbol not found %s", base_name ? base_name : "");
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

value_t *assign_index_values(const char *base_name,
                             std::vector<value_t *> &indices,
                             const std::vector<bool> &fields,
                             value_t *value) {
    return assign_index_resolved(lookupsym(base_name), base_name,
                                 indices, fields, value);
}

value_t *assign_index_symbol(struct symbol *base, const char *base_name,
                             std::vector<value_t *> &indices,
                             const std::vector<bool> &fields,
                             value_t *value) {
    return assign_index_resolved(base, base_name, indices, fields, value);
}

static value_t *compound_index_resolved(struct symbol *s,
                                        const char *base_name, int op,
                                        std::vector<value_t *> &indices,
                                        const std::vector<bool> &fields,
                                        value_t *value) {
    if (!s || !s->val) {
        yyerror("symbol not found %s", base_name ? base_name : "");
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

value_t *compound_index_values(const char *base_name, int op,
                               std::vector<value_t *> &indices,
                               const std::vector<bool> &fields,
                               value_t *value) {
    return compound_index_resolved(lookupsym(base_name), base_name, op,
                                   indices, fields, value);
}

value_t *compound_index_symbol(struct symbol *base, const char *base_name,
                               int op, std::vector<value_t *> &indices,
                               const std::vector<bool> &fields,
                               value_t *value) {
    return compound_index_resolved(base, base_name, op, indices, fields,
                                   value);
}

static value_t *increment_index_resolved(struct symbol *s,
                                         const char *base_name, int delta,
                                         bool prefix,
                                         std::vector<value_t *> &indices,
                                         const std::vector<bool> &fields) {
    if (!s || !s->val) {
        yyerror("symbol not found %s", base_name ? base_name : "");
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

value_t *increment_index_values(const char *base_name, int delta,
                                bool prefix,
                                std::vector<value_t *> &indices,
                                const std::vector<bool> &fields) {
    return increment_index_resolved(lookupsym(base_name), base_name, delta,
                                    prefix, indices, fields);
}

value_t *increment_index_symbol(struct symbol *base, const char *base_name,
                                int delta, bool prefix,
                                std::vector<value_t *> &indices,
                                const std::vector<bool> &fields) {
    return increment_index_resolved(base, base_name, delta, prefix,
                                    indices, fields);
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
        case TYPE_VOID:
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
        const std::string &data = ((struct bytes_value_t *)container)->value.read();
        if (i < 0 || (unsigned long long)i >= data.size()) {
            yyerror("bytes index %lld out of range", i);
            return NULL;
        }
        return newintval((unsigned char)data[(size_t)i]);
    }

    if (container->type == T_builtin) {
        if (!field_access) {
            yyerror("cannot index %s", typename_of(container));
            return NULL;
        }
        if (index->type != T_string) {
            yyerror("builtin field name must be a string, got %s",
                    typename_of(index));
            return NULL;
        }
        int functype = ((struct builtin_value_t *)container)->functype;
        const char *name = ((struct string_value_t *)index)->value;
        if (functype == B_thread_channel) {
            if (std::strcmp(name, "unbounded") == 0)
                return newbuiltinval(B_thread_unbounded_channel);
            if (std::strcmp(name, "bounded") == 0 ||
                std::strcmp(name, "new") == 0)
                return newbuiltinval(B_thread_channel);
        }
        yyerror("builtin has no field %s", name);
        return NULL;
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
            return newmethodval(dupval(container), method->second);
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
        auto method = ev->def->methods.find(name);
        if (method != ev->def->methods.end())
            return newmethodval(dupval(container), method->second);
        yyerror("field %s not found in %s.%s", name,
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

bool validate_type_constraint(const struct type_constraint *type) {
    if (!type) return true;
    if (type->kind == TYPE_NAMED) {
        if (!type->name ||
            !structtype_type_exists(type->source, type->name)) {
            yyerror("unknown type name %s",
                    type->name ? type->name : "invalid");
            return false;
        }
        return true;
    }
    if (type->kind == TYPE_INVALID) {
        yyerror("invalid type");
        return false;
    }
    return true;
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

const char *import_binding_name(const char *path) {
    while (path && (*path == '.' || *path == '@'))
        ++path;
    if (!path || !*path)
        return "";
    const char *dot = std::strrchr(path, '.');
    return dot ? dot + 1 : path;
}

bool validate_function_params(const char *name, struct symlist *params) {
    const char *display_name = name && *name ? name : "<anonymous>";
    bool bad = false;
    for (struct symlist *p = params; p && !bad; p = p->next) {
        for (struct symlist *q = p->next; q; q = q->next) {
            if (!std::strcmp(p->sym->name, q->sym->name)) {
                yyerror("duplicate parameter %s in function %s",
                        p->sym->name, display_name);
                bad = true;
                break;
            }
        }
    }
    for (struct symlist *p = params; p && !bad; p = p->next) {
        if (!validate_type_constraint(p->type))
            bad = true;
        if (p->variadic && p->next) {
            yyerror("variadic parameter must be last");
            bad = true;
        }
    }
    bool seen_default = false;
    for (struct symlist *p = params; p && !bad; p = p->next) {
        if (p->variadic)
            continue;
        if (p->has_default) {
            seen_default = true;
        } else if (seen_default) {
            yyerror("parameter %s without default cannot follow a "
                    "parameter with default",
                    p->sym->name);
            bad = true;
        }
    }
    return !bad;
}
