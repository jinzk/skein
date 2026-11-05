#include "builtins_internal.h"

static struct value_t *dict_get_own(struct dict_value_t *dict,
                                    const char *key) {
    if (!dict || !key)
        return NULL;
    for (auto &entry : dict->payload->items) {
        if (entry.first && entry.first->type == T_string &&
            std::strcmp(((struct string_value_t *)entry.first)->value,
                        key) == 0)
            return entry.second;
    }
    return NULL;
}

static bool value_is_nan(const struct value_t *v) {
    return v && v->type == T_float &&
           std::isnan(((const struct float_value_t *)v)->value);
}

static bool list_sort_kind(struct list_value_t *list, bool &numeric,
                           bool &chars, bool &bytes) {
    numeric = true;
    bool strings = true;
    chars = true;
    bytes = true;
    for (struct value_t *item : list->payload->items) {
        if (!is_numeric(item))
            numeric = false;
        if (!item || item->type != T_string)
            strings = false;
        if (!item || item->type != T_char)
            chars = false;
        if (!item || item->type != T_byte)
            bytes = false;
    }
    return numeric || strings || chars || bytes;
}

struct default_order {
    bool numeric = true;
    bool chars = false;
    bool bytes = false;
    bool desc = false;

    bool operator()(struct value_t *a, struct value_t *b) const {
        bool a_nan = value_is_nan(a);
        bool b_nan = value_is_nan(b);
        if (a_nan || b_nan) {
            if (a_nan && b_nan)
                return false;
            return !a_nan;
        }
        int cmp = 0;
        if (chars) {
            unsigned int ac = ((struct char_value_t *)a)->value;
            unsigned int bc = ((struct char_value_t *)b)->value;
            cmp = (ac < bc) ? -1 : (ac > bc ? 1 : 0);
        } else if (bytes) {
            unsigned int ac = ((struct byte_value_t *)a)->value;
            unsigned int bc = ((struct byte_value_t *)b)->value;
            cmp = (ac < bc) ? -1 : (ac > bc ? 1 : 0);
        } else if (numeric) {
            bool unordered = false;
            cmp = compare_numeric(a, b, unordered);
            if (unordered)
                return false;
        } else {
            cmp = std::strcmp(
                ((struct string_value_t *)a)->value,
                ((struct string_value_t *)b)->value);
        }
        return desc ? cmp > 0 : cmp < 0;
    }
};

static struct value_t *list_sort_default(struct list_value_t *list,
                                         bool desc, const char *name) {
    bool numeric = true;
    bool chars = false;
    bool bytes = false;
    if (!list_sort_kind(list, numeric, chars, bytes)) {
        yyerror("%s expects all numbers, strings, chars, or bytes", name);
        return NULL;
    }

    struct list_value_t *out = newlistval();
    out->payload->items.reserve(list->payload->items.size());
    for (struct value_t *item : list->payload->items)
        out->payload->items.push_back(dupval(item));

    std::stable_sort(out->payload->items.begin(),
                     out->payload->items.end(),
                     default_order{numeric, chars, bytes, desc});
    return (struct value_t *)out;
}

static struct value_t *list_sort_with_cmp(struct list_value_t *list,
                                          struct value_t *cmp_fn,
                                          const char *name) {
    struct list_value_t *out = newlistval();
    out->payload->items.reserve(list->payload->items.size());
    for (struct value_t *item : list->payload->items)
        out->payload->items.push_back(dupval(item));

    bool failed = false;
    std::stable_sort(
        out->payload->items.begin(), out->payload->items.end(),
        [&](struct value_t *a, struct value_t *b) {
            if (failed)
                return false;
            std::vector<struct value_t *> call_args;
            call_args.push_back(dupval(a));
            call_args.push_back(dupval(b));
            struct value_t *result =
                skein_call_function(cmp_fn, std::move(call_args));
            if (!result) {
                failed = true;
                return false;
            }
            if (result->type != T_int) {
                yyerror("%s comparator must return int, got %s", name,
                        typename_of(result));
                freeval(result);
                failed = true;
                return false;
            }
            long long order = ((struct int_value_t *)result)->value;
            freeval(result);
            return order < 0;
        });

    if (failed) {
        freeval((struct value_t *)out);
        return NULL;
    }
    return (struct value_t *)out;
}

static struct value_t *list_top_k_impl(struct list_value_t *list, long long k,
                                       struct value_t *cmp_fn,
                                       const char *name) {
    if (k <= 0)
        return (struct value_t *)newlistval();
    size_t size = list->payload->items.size();
    if ((size_t)k >= size) {
        return cmp_fn ? list_sort_with_cmp(list, cmp_fn, name)
                      : list_sort_default(list, false, name);
    }

    std::vector<struct value_t *> items;
    items.reserve(size);
    for (struct value_t *item : list->payload->items)
        items.push_back(dupval(item));

    bool failed = false;
    if (!cmp_fn) {
        bool numeric = true;
        bool chars = false;
        bool bytes = false;
        if (!list_sort_kind(list, numeric, chars, bytes)) {
            yyerror("%s expects all numbers, strings, chars, or bytes", name);
            for (struct value_t *item : items)
                freeval(item);
            return NULL;
        }
        std::partial_sort(items.begin(), items.begin() + (size_t)k,
                          items.end(),
                          default_order{numeric, chars, bytes, false});
    } else {
        std::partial_sort(
            items.begin(), items.begin() + (size_t)k, items.end(),
            [&](struct value_t *a, struct value_t *b) {
                if (failed)
                    return false;
                std::vector<struct value_t *> call_args;
                call_args.push_back(dupval(a));
                call_args.push_back(dupval(b));
                struct value_t *result =
                    skein_call_function(cmp_fn, std::move(call_args));
                if (!result) {
                    failed = true;
                    return false;
                }
                if (result->type != T_int) {
                    yyerror("%s comparator must return int, got %s", name,
                            typename_of(result));
                    freeval(result);
                    failed = true;
                    return false;
                }
                long long order = ((struct int_value_t *)result)->value;
                freeval(result);
                return order < 0;
            });
        if (failed) {
            for (struct value_t *item : items)
                freeval(item);
            return NULL;
        }
    }

    struct list_value_t *out = newlistval();
    out->payload->items.reserve((size_t)k);
    for (long long i = 0; i < k; ++i)
        out->payload->items.push_back(items[(size_t)i]);
    for (size_t i = (size_t)k; i < items.size(); ++i)
        freeval(items[i]);
    return (struct value_t *)out;
}

struct keyed_item {
    struct value_t *key;
    struct value_t *value;
};

static void free_keyed_items(std::vector<keyed_item> &items, bool owned) {
    for (auto &item : items) {
        freeval(item.key);
        if (owned)
            freeval(item.value);
    }
    items.clear();
}

static struct value_t *list_sort_by_key(struct list_value_t *list,
                                        struct value_t *key_fn, bool desc,
                                        const char *name) {
    std::vector<keyed_item> items;
    items.reserve(list->payload->items.size());

    for (struct value_t *item : list->payload->items) {
        std::vector<struct value_t *> call_args;
        call_args.push_back(dupval(item));
        struct value_t *key = skein_call_function(key_fn, std::move(call_args));
        if (!key) {
            free_keyed_items(items, true);
            return NULL;
        }
        items.push_back(keyed_item{key, dupval(item)});
    }

    bool numeric = true;
    bool strings = true;
    bool chars = true;
    bool bytes = true;
    for (auto &item : items) {
        if (!is_numeric(item.key))
            numeric = false;
        if (!item.key || item.key->type != T_string)
            strings = false;
        if (!item.key || item.key->type != T_char)
            chars = false;
        if (!item.key || item.key->type != T_byte)
            bytes = false;
    }
    if (!numeric && !strings && !chars && !bytes) {
        yyerror("%s key must return numbers, strings, chars, or bytes", name);
        free_keyed_items(items, true);
        return NULL;
    }

    std::stable_sort(
        items.begin(), items.end(),
        [numeric, chars, bytes, desc](const keyed_item &a, const keyed_item &b) {
            bool a_nan = value_is_nan(a.key);
            bool b_nan = value_is_nan(b.key);
            if (a_nan || b_nan) {
                if (a_nan && b_nan)
                    return false;
                return !a_nan;
            }
            int cmp = 0;
            if (chars) {
                unsigned int ac = ((struct char_value_t *)a.key)->value;
                unsigned int bc = ((struct char_value_t *)b.key)->value;
                cmp = (ac < bc) ? -1 : (ac > bc ? 1 : 0);
            } else if (bytes) {
                unsigned int ac = ((struct byte_value_t *)a.key)->value;
                unsigned int bc = ((struct byte_value_t *)b.key)->value;
                cmp = (ac < bc) ? -1 : (ac > bc ? 1 : 0);
            } else if (numeric) {
                bool unordered = false;
                cmp = compare_numeric(a.key, b.key, unordered);
                if (unordered)
                    return false;
            } else {
                cmp = std::strcmp(
                    ((struct string_value_t *)a.key)->value,
                    ((struct string_value_t *)b.key)->value);
            }
            return desc ? cmp > 0 : cmp < 0;
        });

    struct list_value_t *out = newlistval();
    out->payload->items.reserve(items.size());
    for (auto &item : items) {
        out->payload->items.push_back(item.value);
        item.value = NULL;
    }
    free_keyed_items(items, true);
    return (struct value_t *)out;
}

static struct value_t *list_is_sorted_value(struct list_value_t *list,
                                            struct value_t *cmp_fn,
                                            const char *name) {
    if (list->payload->items.size() < 2)
        return newboolval(true);

    if (cmp_fn) {
        for (size_t i = 0; i + 1 < list->payload->items.size(); ++i) {
            std::vector<struct value_t *> call_args;
            call_args.push_back(dupval(list->payload->items[i]));
            call_args.push_back(dupval(list->payload->items[i + 1]));
            struct value_t *result =
                skein_call_function(cmp_fn, std::move(call_args));
            if (!result)
                return NULL;
            if (result->type != T_int) {
                yyerror("%s comparator must return int, got %s", name,
                        typename_of(result));
                freeval(result);
                return NULL;
            }
            long long order = ((struct int_value_t *)result)->value;
            freeval(result);
            if (order > 0)
                return newboolval(false);
        }
        return newboolval(true);
    }

    bool numeric = true;
    bool chars = false;
    bool bytes = false;
    if (!list_sort_kind(list, numeric, chars, bytes)) {
        yyerror("%s expects all numbers, strings, chars, or bytes", name);
        return NULL;
    }
    for (size_t i = 0; i + 1 < list->payload->items.size(); ++i) {
        struct value_t *prev = list->payload->items[i];
        struct value_t *next = list->payload->items[i + 1];
        bool prev_nan = value_is_nan(prev);
        bool next_nan = value_is_nan(next);
        if (prev_nan) {
            if (!next_nan)
                return newboolval(false);
            continue;
        }
        if (next_nan)
            continue;

        int cmp = 0;
        if (chars) {
            unsigned int pc = ((struct char_value_t *)prev)->value;
            unsigned int nc = ((struct char_value_t *)next)->value;
            cmp = (pc < nc) ? -1 : (pc > nc ? 1 : 0);
        } else if (bytes) {
            unsigned int pc = ((struct byte_value_t *)prev)->value;
            unsigned int nc = ((struct byte_value_t *)next)->value;
            cmp = (pc < nc) ? -1 : (pc > nc ? 1 : 0);
        } else if (numeric) {
            bool unordered = false;
            cmp = compare_numeric(prev, next, unordered);
            if (unordered)
                continue;
        } else {
            cmp = std::strcmp(
                ((struct string_value_t *)prev)->value,
                ((struct string_value_t *)next)->value);
        }
        if (cmp > 0)
            return newboolval(false);
    }
    return newboolval(true);
}

static struct value_t *list_unique_impl(struct list_value_t *list,
                                        struct value_t *key_fn,
                                        const char *name) {
    struct list_value_t *out = newlistval();
    out->payload->items.reserve(list->payload->items.size());
    std::vector<struct value_t *> keys;
    keys.reserve(list->payload->items.size());

    bool failed = false;
    for (struct value_t *item : list->payload->items) {
        struct value_t *key = NULL;
        if (key_fn) {
            std::vector<struct value_t *> call_args;
            call_args.push_back(dupval(item));
            key = skein_call_function(key_fn, std::move(call_args));
            if (!key) {
                failed = true;
                break;
            }
        } else {
            key = dupval(item);
        }
        keys.push_back(key);
    }

    if (failed) {
        for (struct value_t *entry : keys)
            freeval(entry);
        freeval((struct value_t *)out);
        return NULL;
    }

    bool hashable = true;
    for (struct value_t *key : keys) {
        if (!dict_key_allowed(key)) {
            hashable = false;
            break;
        }
    }

    if (hashable) {
        struct dict_value_t *seen = newdictval();
        for (size_t i = 0; i < list->payload->items.size(); ++i) {
            if (dict_find_index(seen, keys[i]) >= 0) {
                freeval(keys[i]);
                continue;
            }
            dict_append_item(seen, dupval(keys[i]), newboolval(true));
            out->payload->items.push_back(
                dupval(list->payload->items[i]));
            freeval(keys[i]);
        }
        freeval((struct value_t *)seen);
    } else {
        std::vector<struct value_t *> seen;
        for (size_t i = 0; i < list->payload->items.size(); ++i) {
            bool duplicate = false;
            for (struct value_t *entry : seen) {
                if (value_equals(entry, keys[i])) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                freeval(keys[i]);
            } else {
                seen.push_back(keys[i]);
                out->payload->items.push_back(
                    dupval(list->payload->items[i]));
            }
        }
        for (struct value_t *entry : seen)
            freeval(entry);
    }

    (void)name;
    return (struct value_t *)out;
}

static struct value_t *list_group_by_impl(struct list_value_t *list,
                                          struct value_t *key_fn,
                                          const char *name) {
    struct dict_value_t *out = newdictval();

    for (struct value_t *item : list->payload->items) {
        std::vector<struct value_t *> call_args;
        call_args.push_back(dupval(item));
        struct value_t *key = skein_call_function(key_fn, std::move(call_args));
        if (!key) {
            freeval((struct value_t *)out);
            return NULL;
        }
        if (!dict_key_allowed(key)) {
            yyerror("%s key: %s", name, dict_key_error());
            freeval(key);
            freeval((struct value_t *)out);
            return NULL;
        }

        int index = dict_find_index(out, key);
        if (index >= 0) {
            struct list_value_t *bucket = (struct list_value_t *)
                out->payload->items[(size_t)index].second;
            bucket->payload->items.push_back(dupval(item));
            freeval(key);
        } else {
            struct list_value_t *bucket = newlistval();
            bucket->payload->items.push_back(dupval(item));
            dict_append_item(out, key, (struct value_t *)bucket);
        }
    }
    return (struct value_t *)out;
}

static struct value_t *list_chunk_impl(struct list_value_t *list,
                                       long long size, const char *name) {
    if (size < 1) {
        yyerror("%s size must be at least 1", name);
        return NULL;
    }
    struct list_value_t *out = newlistval();
    size_t step = (size_t)size;
    for (size_t start = 0; start < list->payload->items.size();
         start += step) {
        struct list_value_t *bucket = newlistval();
        size_t end = start + step;
        if (end > list->payload->items.size())
            end = list->payload->items.size();
        bucket->payload->items.reserve(end - start);
        for (size_t i = start; i < end; ++i)
            bucket->payload->items.push_back(
                dupval(list->payload->items[i]));
        out->payload->items.push_back((struct value_t *)bucket);
    }
    return (struct value_t *)out;
}

static struct value_t *list_window_impl(struct list_value_t *list,
                                        long long size, long long step_value,
                                        const char *name) {
    if (size < 1) {
        yyerror("%s size must be at least 1", name);
        return NULL;
    }
    if (step_value < 1) {
        yyerror("%s step must be at least 1", name);
        return NULL;
    }
    struct list_value_t *out = newlistval();
    size_t window = (size_t)size;
    size_t step = (size_t)step_value;
    if (window > list->payload->items.size())
        return (struct value_t *)out;

    for (size_t start = 0; start + window <= list->payload->items.size();
         start += step) {
        struct list_value_t *bucket = newlistval();
        bucket->payload->items.reserve(window);
        for (size_t i = 0; i < window; ++i)
            bucket->payload->items.push_back(
                dupval(list->payload->items[start + i]));
        out->payload->items.push_back((struct value_t *)bucket);
    }
    return (struct value_t *)out;
}

static struct value_t *list_zip_impl(
    const std::vector<struct value_t *> &lists) {
    size_t shortest = 0;
    bool first = true;
    for (struct value_t *value : lists) {
        struct list_value_t *list = (struct list_value_t *)value;
        if (first || list->payload->items.size() < shortest) {
            shortest = list->payload->items.size();
            first = false;
        }
    }

    struct list_value_t *out = newlistval();
    out->payload->items.reserve(shortest);
    for (size_t i = 0; i < shortest; ++i) {
        struct list_value_t *row = newlistval();
        row->payload->items.reserve(lists.size());
        for (struct value_t *value : lists) {
            struct list_value_t *list = (struct list_value_t *)value;
            row->payload->items.push_back(
                dupval(list->payload->items[i]));
        }
        out->payload->items.push_back((struct value_t *)row);
    }
    return (struct value_t *)out;
}

static bool list_compare_pair(struct value_t *a, struct value_t *b,
                              bool numeric, bool chars, bool bytes,
                              struct value_t *cmp_fn, const char *name,
                              int &cmp) {
    cmp = 0;
    if (cmp_fn) {
        std::vector<struct value_t *> call_args;
        call_args.push_back(dupval(a));
        call_args.push_back(dupval(b));
        struct value_t *result =
            skein_call_function(cmp_fn, std::move(call_args));
        if (!result)
            return false;
        if (result->type != T_int) {
            yyerror("%s comparator must return int, got %s", name,
                    typename_of(result));
            freeval(result);
            return false;
        }
        cmp = (int)((struct int_value_t *)result)->value;
        freeval(result);
        return true;
    }

    bool a_nan = value_is_nan(a);
    bool b_nan = value_is_nan(b);
    if (a_nan || b_nan) {
        if (a_nan && b_nan)
            cmp = 0;
        else
            cmp = a_nan ? 1 : -1; /* NaN sorts last */
        return true;
    }

    if (chars) {
        unsigned int ac = ((struct char_value_t *)a)->value;
        unsigned int bc = ((struct char_value_t *)b)->value;
        cmp = (ac < bc) ? -1 : (ac > bc ? 1 : 0);
    } else if (bytes) {
        unsigned int ac = ((struct byte_value_t *)a)->value;
        unsigned int bc = ((struct byte_value_t *)b)->value;
        cmp = (ac < bc) ? -1 : (ac > bc ? 1 : 0);
    } else if (numeric) {
        bool unordered = false;
        cmp = compare_numeric(a, b, unordered);
        if (unordered)
            cmp = 0;
    } else {
        cmp = std::strcmp(((struct string_value_t *)a)->value,
                          ((struct string_value_t *)b)->value);
    }
    return true;
}

static struct value_t *list_binary_search_impl(struct list_value_t *list,
                                               struct value_t *needle,
                                               struct value_t *cmp_fn,
                                               const char *name) {
    bool numeric = true;
    bool chars = false;
    bool bytes = false;
    if (!cmp_fn &&
        !list_sort_kind(list, numeric, chars, bytes)) {
        yyerror("%s expects all numbers, strings, chars, or bytes", name);
        return NULL;
    }

    size_t low = 0;
    size_t high = list->payload->items.size();
    bool found = false;

    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int cmp = 0;
        if (!list_compare_pair(list->payload->items[mid], needle,
                               numeric, chars, bytes, cmp_fn, name, cmp))
            return NULL;
        if (cmp == 0) {
            found = true;
            low = mid;
            break;
        }
        if (cmp < 0)
            low = mid + 1;
        else
            high = mid;
    }

    struct dict_value_t *out = newdictval();
    dict_append_item(out, newstrval("found"), newboolval(found));
    dict_append_item(out, newstrval("index"),
                     newintval((long long)low));
    return (struct value_t *)out;
}


struct value_t *call_list_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_list_slice: {
            if (args[0]->type != T_list) {
                yyerror("std.list.slice expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int ||
                ((struct int_value_t *)args[1])->value < 0) {
                yyerror("std.list.slice start must be a non-negative integer");
                break;
            }
            size_t start = (size_t)((struct int_value_t *)args[1])->value;
            size_t count = std::string::npos;
            if (args.size() == 3) {
                if (args[2]->type != T_int ||
                    ((struct int_value_t *)args[2])->value < 0) {
                    yyerror("std.list.slice length must be a non-negative integer");
                    break;
                }
                count = (size_t)((struct int_value_t *)args[2])->value;
            }
            struct list_value_t *source = (struct list_value_t *)args[0];
            size_t size = source->payload->items.size();
            if (start > size)
                start = size;
            if (count > size - start)
                count = size - start;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(count);
            for (size_t i = 0; i < count; ++i)
                out->payload->items.push_back(
                    dupval(source->payload->items[start + i]));
            ret = (struct value_t *)out;
            break;
        }
        case B_list_contains:
        case B_list_index: {
            if (args[0]->type != T_list) {
                yyerror("%s expects a list, got %s",
                        functype == B_list_contains ? "std.list.contains"
                                                   : "std.list.index",
                        typename_of(args[0]));
                break;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            long long found = -1;
            for (size_t i = 0; i < list->payload->items.size(); ++i) {
                if (value_equals(list->payload->items[i], args[1])) {
                    found = (long long)i;
                    break;
                }
            }
            if (functype == B_list_contains)
                ret = newintval(found >= 0 ? 1 : 0);
            else
                ret = newintval(found);
            break;
        }
        case B_list_reverse: {
            if (arg->type != T_list) {
                yyerror("std.list.reverse expects a list, got %s",
                        typename_of(arg));
                break;
            }
            struct list_value_t *source = (struct list_value_t *)arg;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(source->payload->items.size());
            for (size_t i = source->payload->items.size(); i > 0; --i)
                out->payload->items.push_back(
                    dupval(source->payload->items[i - 1]));
            ret = (struct value_t *)out;
            break;
        }
        case B_list_sort: {
            if (args[0]->type != T_list) {
                yyerror("std.list.sort expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            struct list_value_t *source = (struct list_value_t *)args[0];
            if (args.size() == 1) {
                ret = list_sort_default(source, false, "std.list.sort");
            } else {
                ret = list_sort_with_cmp(source, args[1], "std.list.sort");
            }
            break;
        }
        case B_list_sort_desc: {
            if (args[0]->type != T_list) {
                yyerror("std.list.sort_desc expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            ret = list_sort_default((struct list_value_t *)args[0], true,
                                    "std.list.sort_desc");
            break;
        }
        case B_list_sort_by: {
            if (args[0]->type != T_list) {
                yyerror("std.list.sort_by expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            bool desc = false;
            if (args.size() == 3) {
                if (args[2]->type != T_dict) {
                    yyerror("std.list.sort_by options must be a dict, got %s",
                            typename_of(args[2]));
                    break;
                }
                struct value_t *desc_value = dict_get_own(
                    (struct dict_value_t *)args[2], "desc");
                desc = desc_value && truthy(desc_value);
            }
            ret = list_sort_by_key((struct list_value_t *)args[0], args[1],
                                   desc, "std.list.sort_by");
            break;
        }
        case B_list_is_sorted: {
            if (args[0]->type != T_list) {
                yyerror("std.list.is_sorted expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            ret = list_is_sorted_value(
                (struct list_value_t *)args[0],
                args.size() == 2 ? args[1] : NULL, "std.list.is_sorted");
            break;
        }
        case B_list_append: {
            if (args[0]->type != T_list) {
                yyerror("std.list.append expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            struct list_value_t *source = (struct list_value_t *)args[0];
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(source->payload->items.size() + 1);
            for (value_t *item : source->payload->items)
                out->payload->items.push_back(dupval(item));
            out->payload->items.push_back(dupval(args[1]));
            ret = (struct value_t *)out;
            break;
        }
        case B_list_map:
        case B_list_filter:
        case B_list_reduce: {
            if (args[0]->type != T_list) {
                yyerror("%s expects a list, got %s",
                        functype == B_list_map ? "std.list.map" :
                        functype == B_list_filter ? "std.list.filter" :
                        "std.list.reduce",
                        typename_of(args[0]));
                break;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            struct value_t *fn = args[1];

            if (functype == B_list_reduce) {
                struct value_t *acc = NULL;
                size_t index = 0;
                if (args.size() == 3) {
                    acc = dupval(args[2]);
                } else {
                    if (list->payload->items.empty()) {
                        yyerror("std.list.reduce empty list without initial value");
                        break;
                    }
                    acc = dupval(list->payload->items[0]);
                    index = 1;
                }
                for (; index < list->payload->items.size(); ++index) {
                    std::vector<struct value_t *> call_args;
                    call_args.push_back(dupval(acc));
                    call_args.push_back(
                        dupval(list->payload->items[index]));
                    struct value_t *next =
                        skein_call_function(fn, std::move(call_args));
                    if (!next) {
                        freeval(acc);
                        acc = NULL;
                        break;
                    }
                    freeval(acc);
                    acc = next;
                }
                ret = acc;
                break;
            }

            struct list_value_t *out = newlistval();
            out->payload->items.reserve(list->payload->items.size());
            for (value_t *item : list->payload->items) {
                std::vector<struct value_t *> call_args;
                call_args.push_back(dupval(item));
                struct value_t *result =
                    skein_call_function(fn, std::move(call_args));
                if (!result) {
                    freeval((struct value_t *)out);
                    out = NULL;
                    break;
                }
                if (functype == B_list_map) {
                    out->payload->items.push_back(result);
                } else {
                    bool keep = truthy(result);
                    freeval(result);
                    if (keep)
                        out->payload->items.push_back(dupval(item));
                }
            }
            if (out)
                ret = (struct value_t *)out;
            break;
        }
        case B_list_unique:
        case B_list_unique_by: {
            if (args[0]->type != T_list) {
                yyerror("%s expects a list, got %s",
                        functype == B_list_unique ? "std.list.unique"
                                                  : "std.list.unique_by",
                        typename_of(args[0]));
                break;
            }
            ret = list_unique_impl(
                (struct list_value_t *)args[0],
                functype == B_list_unique_by ? args[1] : NULL,
                functype == B_list_unique ? "std.list.unique"
                                          : "std.list.unique_by");
            break;
        }
        case B_list_group_by: {
            if (args[0]->type != T_list) {
                yyerror("std.list.group_by expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            ret = list_group_by_impl((struct list_value_t *)args[0],
                                     args[1], "std.list.group_by");
            break;
        }
        case B_list_chunk: {
            if (args[0]->type != T_list) {
                yyerror("std.list.chunk expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int) {
                yyerror("std.list.chunk size must be an integer, got %s",
                        typename_of(args[1]));
                break;
            }
            ret = list_chunk_impl(
                (struct list_value_t *)args[0],
                ((struct int_value_t *)args[1])->value, "std.list.chunk");
            break;
        }
        case B_list_window: {
            if (args[0]->type != T_list) {
                yyerror("std.list.window expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int) {
                yyerror("std.list.window size must be an integer, got %s",
                        typename_of(args[1]));
                break;
            }
            long long step = 1;
            if (args.size() == 3) {
                if (args[2]->type != T_int) {
                    yyerror("std.list.window step must be an integer, got %s",
                            typename_of(args[2]));
                    break;
                }
                step = ((struct int_value_t *)args[2])->value;
            }
            ret = list_window_impl(
                (struct list_value_t *)args[0],
                ((struct int_value_t *)args[1])->value, step,
                "std.list.window");
            break;
        }
        case B_list_zip: {
            bool only_lists = true;
            for (struct value_t *value : args) {
                if (value->type != T_list) {
                    yyerror("std.list.zip expects lists, got %s",
                            typename_of(value));
                    only_lists = false;
                    break;
                }
            }
            if (!only_lists)
                break;
            ret = list_zip_impl(args);
            break;
        }
        case B_list_top_k: {
            if (args[0]->type != T_list) {
                yyerror("std.list.top_k expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int) {
                yyerror("std.list.top_k k must be an integer, got %s",
                        typename_of(args[1]));
                break;
            }
            long long k = ((struct int_value_t *)args[1])->value;
            if (k < 0) {
                yyerror("std.list.top_k k must be non-negative");
                break;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            bool use_cmp = args.size() == 3 && args[2]->type != T_null;
            ret = list_top_k_impl(list, k,
                                  use_cmp ? args[2] : NULL,
                                  "std.list.top_k");
            break;
        }
        case B_list_binary_search: {
            if (args[0]->type != T_list) {
                yyerror("std.list.binary_search expects a list, got %s",
                        typename_of(args[0]));
                break;
            }
            struct value_t *cmp_fn =
                args.size() == 3 && args[2]->type != T_null ? args[2] : NULL;
            ret = list_binary_search_impl(
                (struct list_value_t *)args[0], args[1], cmp_fn,
                "std.list.binary_search");
            break;
        }
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
