#include "builtins_internal.h"

static struct value_t *set_from_parts_payload(
    std::shared_ptr<set_node> root, std::shared_ptr<deque_payload> order,
    std::size_t size) {
    struct set_value_t *out = new set_value_t;
    out->type = T_set;
    out->payload = std::make_shared<set_payload>();
    out->payload->root = std::move(root);
    out->payload->order = std::move(order);
    out->payload->size = size;
    return (struct value_t *)out;
}

static struct value_t *set_add_value(const struct set_value_t *set,
                                     struct value_t *key, bool &added) {
    std::shared_ptr<set_node> root =
        set_insert(set->payload->root, key, added);
    if (!added)
        return dupval((struct value_t *)set);
    std::shared_ptr<deque_payload> order =
        deque_push_back_payload(set->payload->order, key);
    return set_from_parts_payload(std::move(root), std::move(order),
                                  set->payload->size + 1);
}

static struct value_t *set_remove_value(const struct set_value_t *set,
                                        struct value_t *key, bool &removed) {
    std::shared_ptr<set_node> root =
        set_erase(set->payload->root, key, removed);
    if (!removed)
        return dupval((struct value_t *)set);
    return set_from_parts_payload(std::move(root), set->payload->order,
                                  set->payload->size - 1);
}

static struct value_t *set_values_list(const struct set_value_t *set) {
    std::vector<value_t *> items;
    set_values_in_order(set->payload.get(), items);
    struct list_value_t *out = newlistval();
    out->payload->items.reserve(items.size());
    for (struct value_t *item : items)
        out->payload->items.push_back(dupval(item));
    return (struct value_t *)out;
}

static void set_add_in_place(struct set_payload *payload,
                             struct value_t *key) {
    bool added = false;
    std::shared_ptr<set_node> root = set_insert(payload->root, key, added);
    if (!added)
        return;
    payload->root = std::move(root);
    payload->order = deque_push_back_payload(payload->order, key);
    payload->size += 1;
}

static bool set_key_ok(struct value_t *key, const char *name) {
    if (dict_key_allowed(key))
        return true;
    yyerror("%s: %s, got %s", name, dict_key_error(), typename_of(key));
    return false;
}


struct value_t *call_set_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_set_new:
            ret = (struct value_t *)newsetval();
            break;
        case B_set_from_list: {
            if (arg->type != T_list) {
                yyerror("std.set.from_list expects a list, got %s",
                        typename_of(arg));
                break;
            }
            struct list_value_t *list = (struct list_value_t *)arg;
            struct set_value_t *out = newsetval();
            bool ok = true;
            for (struct value_t *item : list->payload->items) {
                if (!set_key_ok(item, "std.set.from_list")) {
                    ok = false;
                    break;
                }
                set_add_in_place(out->payload.get(), item);
            }
            if (!ok) {
                freeval((struct value_t *)out);
                break;
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_set_add: {
            if (arg->type != T_set) {
                yyerror("std.set.add expects a set, got %s",
                        typename_of(arg));
                break;
            }
            if (!set_key_ok(args[1], "std.set.add"))
                break;
            bool added = false;
            ret = set_add_value((struct set_value_t *)arg, args[1], added);
            break;
        }
        case B_set_remove: {
            if (arg->type != T_set) {
                yyerror("std.set.remove expects a set, got %s",
                        typename_of(arg));
                break;
            }
            if (!set_key_ok(args[1], "std.set.remove"))
                break;
            bool removed = false;
            ret = set_remove_value((struct set_value_t *)arg, args[1],
                                   removed);
            break;
        }
        case B_set_has: {
            if (arg->type != T_set) {
                yyerror("std.set.has expects a set, got %s",
                        typename_of(arg));
                break;
            }
            if (!set_key_ok(args[1], "std.set.has"))
                break;
            ret = newboolval(set_contains(((struct set_value_t *)arg)
                                              ->payload.get(),
                                          args[1]));
            break;
        }
        case B_set_size: {
            if (arg->type != T_set) {
                yyerror("std.set.size expects a set, got %s",
                        typename_of(arg));
                break;
            }
            ret = newintval((long long)((struct set_value_t *)arg)
                                ->payload->size);
            break;
        }
        case B_set_values: {
            if (arg->type != T_set) {
                yyerror("std.set.values expects a set, got %s",
                        typename_of(arg));
                break;
            }
            ret = set_values_list((struct set_value_t *)arg);
            break;
        }
        case B_set_union:
        case B_set_intersect:
        case B_set_difference: {
            if (args[0]->type != T_set || args[1]->type != T_set) {
                yyerror("%s expects two sets", functype == B_set_union
                                                   ? "std.set.union"
                                               : functype == B_set_intersect
                                                   ? "std.set.intersect"
                                                   : "std.set.difference");
                break;
            }
            struct set_value_t *left = (struct set_value_t *)args[0];
            struct set_value_t *right = (struct set_value_t *)args[1];
            struct set_value_t *out = newsetval();
            std::vector<value_t *> items;
            set_values_in_order(left->payload.get(), items);
            for (struct value_t *item : items) {
                bool keep = true;
                if (functype == B_set_intersect)
                    keep = set_contains(right->payload.get(), item);
                else if (functype == B_set_difference)
                    keep = !set_contains(right->payload.get(), item);
                if (keep)
                    set_add_in_place(out->payload.get(), item);
            }
            if (functype == B_set_union) {
                set_values_in_order(right->payload.get(), items);
                for (struct value_t *item : items)
                    set_add_in_place(out->payload.get(), item);
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_set_is_subset: {
            if (args[0]->type != T_set || args[1]->type != T_set) {
                yyerror("std.set.is_subset expects two sets");
                break;
            }
            struct set_value_t *left = (struct set_value_t *)args[0];
            struct set_value_t *right = (struct set_value_t *)args[1];
            std::vector<value_t *> items;
            set_values_in_order(left->payload.get(), items);
            bool subset = true;
            for (struct value_t *item : items) {
                if (!set_contains(right->payload.get(), item)) {
                    subset = false;
                    break;
                }
            }
            ret = newboolval(subset);
            break;
        }
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
