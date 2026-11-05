#include "builtins_internal.h"

static struct value_t *lru_clone(const struct lru_value_t *lru) {
    struct lru_value_t *out = newlruval(lru->payload->capacity);
    out->payload->items.reserve(lru->payload->items.size());
    for (const auto &entry : lru->payload->items)
        out->payload->items.push_back(
            std::make_pair(dupval(entry.first), dupval(entry.second)));
    return (struct value_t *)out;
}

static long long lru_find(const struct lru_value_t *lru,
                          struct value_t *key) {
    for (std::size_t i = 0; i < lru->payload->items.size(); ++i) {
        if (value_equals(lru->payload->items[i].first, key))
            return (long long)i;
    }
    return -1;
}

static void lru_move_to_back(struct lru_payload *payload, std::size_t index) {
    auto entry = payload->items[index];
    payload->items.erase(payload->items.begin() + index);
    payload->items.push_back(entry);
}

static struct value_t *lru_result(const struct lru_value_t *lru,
                                  struct value_t *value, bool found) {
    struct dict_value_t *out = newdictval();
    dict_append_item(out, newstrval("lru"),
                     dupval((struct value_t *)lru));
    dict_append_item(out, newstrval("found"), newboolval(found));
    dict_append_item(out, newstrval("value"),
                     value ? value : newnull());
    return (struct value_t *)out;
}

static struct value_t *lru_evicted_result(struct value_t *lru,
                                          struct value_t *key,
                                          struct value_t *value) {
    struct dict_value_t *out = newdictval();
    dict_append_item(out, newstrval("lru"), lru);
    if (!key) {
        dict_append_item(out, newstrval("evicted"), newnull());
    } else {
        struct dict_value_t *entry = newdictval();
        dict_append_item(entry, newstrval("key"), key);
        dict_append_item(entry, newstrval("value"), value);
        dict_append_item(out, newstrval("evicted"),
                         (struct value_t *)entry);
    }
    return (struct value_t *)out;
}


struct value_t *call_lru_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_lru_new: {
            if (arg->type != T_int) {
                yyerror("std.lru.new expects an int capacity, got %s",
                        typename_of(arg));
                break;
            }
            long long capacity = ((struct int_value_t *)arg)->value;
            if (capacity < 1) {
                yyerror("std.lru.new capacity must be at least 1");
                break;
            }
            ret = (struct value_t *)newlruval((std::size_t)capacity);
            break;
        }
        case B_lru_get:
        case B_lru_peek: {
            if (args[0]->type != T_lru) {
                yyerror("%s expects an lru, got %s",
                        functype == B_lru_get ? "std.lru.get" : "std.lru.peek",
                        typename_of(args[0]));
                break;
            }
            if (!dict_key_allowed(args[1])) {
                yyerror("std.lru key: %s", dict_key_error());
                break;
            }
            struct lru_value_t *lru = (struct lru_value_t *)args[0];
            long long index = lru_find(lru, args[1]);
            if (index < 0) {
                struct value_t *missing =
                    args.size() == 3 ? dupval(args[2]) : newnull();
                ret = lru_result(lru, missing, false);
                break;
            }
            if (functype == B_lru_peek) {
                ret = lru_result(
                    lru, dupval(lru->payload->items[(size_t)index].second),
                    true);
                break;
            }
            struct value_t *copy = lru_clone(lru);
            struct lru_value_t *cloned = (struct lru_value_t *)copy;
            struct value_t *value =
                dupval(cloned->payload->items[(size_t)index].second);
            lru_move_to_back(cloned->payload.get(), (size_t)index);
            ret = lru_result(cloned, value, true);
            freeval(copy);
            break;
        }
        case B_lru_put: {
            if (args[0]->type != T_lru) {
                yyerror("std.lru.put expects an lru, got %s",
                        typename_of(args[0]));
                break;
            }
            if (!dict_key_allowed(args[1])) {
                yyerror("std.lru key: %s", dict_key_error());
                break;
            }
            struct lru_value_t *lru = (struct lru_value_t *)args[0];
            struct value_t *copy = lru_clone(lru);
            struct lru_value_t *cloned = (struct lru_value_t *)copy;
            long long index = lru_find(cloned, args[1]);
            struct value_t *evicted_key = NULL;
            struct value_t *evicted_value = NULL;

            if (index >= 0) {
                freeval(cloned->payload->items[(size_t)index].second);
                cloned->payload->items[(size_t)index].second =
                    dupval(args[2]);
                lru_move_to_back(cloned->payload.get(), (size_t)index);
            } else {
                cloned->payload->items.push_back(std::make_pair(
                    dupval(args[1]), dupval(args[2])));
                if (cloned->payload->items.size() >
                    cloned->payload->capacity) {
                    evicted_key = cloned->payload->items.front().first;
                    evicted_value = cloned->payload->items.front().second;
                    cloned->payload->items.erase(
                        cloned->payload->items.begin());
                }
            }

            ret = lru_evicted_result(copy, evicted_key, evicted_value);
            break;
        }
        case B_lru_delete: {
            if (args[0]->type != T_lru) {
                yyerror("std.lru.delete expects an lru, got %s",
                        typename_of(args[0]));
                break;
            }
            if (!dict_key_allowed(args[1])) {
                yyerror("std.lru key: %s", dict_key_error());
                break;
            }
            struct lru_value_t *lru = (struct lru_value_t *)args[0];
            struct value_t *copy = lru_clone(lru);
            struct lru_value_t *cloned = (struct lru_value_t *)copy;
            long long index = lru_find(cloned, args[1]);
            bool removed = index >= 0;
            if (removed) {
                freeval(cloned->payload->items[(size_t)index].first);
                freeval(cloned->payload->items[(size_t)index].second);
                cloned->payload->items.erase(
                    cloned->payload->items.begin() + index);
            }
            struct dict_value_t *out = newdictval();
            dict_append_item(out, newstrval("lru"), copy);
            dict_append_item(out, newstrval("removed"), newboolval(removed));
            ret = (struct value_t *)out;
            break;
        }
        case B_lru_has: {
            if (args[0]->type != T_lru) {
                yyerror("std.lru.has expects an lru, got %s",
                        typename_of(args[0]));
                break;
            }
            if (!dict_key_allowed(args[1])) {
                yyerror("std.lru key: %s", dict_key_error());
                break;
            }
            struct lru_value_t *lru = (struct lru_value_t *)args[0];
            ret = newboolval(lru_find(lru, args[1]) >= 0);
            break;
        }
        case B_lru_keys:
        case B_lru_values: {
            if (arg->type != T_lru) {
                yyerror("%s expects an lru, got %s",
                        functype == B_lru_keys ? "std.lru.keys"
                                               : "std.lru.values",
                        typename_of(arg));
                break;
            }
            struct lru_value_t *lru = (struct lru_value_t *)arg;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(lru->payload->items.size());
            for (const auto &entry : lru->payload->items)
                out->payload->items.push_back(dupval(
                    functype == B_lru_keys ? entry.first : entry.second));
            ret = (struct value_t *)out;
            break;
        }
        case B_lru_len: {
            if (arg->type != T_lru) {
                yyerror("std.lru.len expects an lru, got %s",
                        typename_of(arg));
                break;
            }
            ret = newintval((long long)((struct lru_value_t *)arg)
                                ->payload->items.size());
            break;
        }
        case B_lru_capacity: {
            if (arg->type != T_lru) {
                yyerror("std.lru.capacity expects an lru, got %s",
                        typename_of(arg));
                break;
            }
            ret = newintval((long long)((struct lru_value_t *)arg)
                                ->payload->capacity);
            break;
        }
        case B_lru_clear: {
            if (arg->type != T_lru) {
                yyerror("std.lru.clear expects an lru, got %s",
                        typename_of(arg));
                break;
            }
            ret = (struct value_t *)newlruval(
                ((struct lru_value_t *)arg)->payload->capacity);
            break;
        }
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
