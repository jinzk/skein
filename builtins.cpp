#include "builtins.h"

#include "runtime_context.h"
#include "symbol.h"
#include "builtin_ids.h"
#include "compress_builtin.h"
#include "crypto_builtin.h"
#include "dict.h"
#include "encoding.h"
#include "env_builtin.h"
#include "eval_helpers.h"
#include "error.h"
#include "fs_extra.h"
#include "generator.h"
#include "http.h"
#include "http2.h"
#include "websocket.h"
#include "io.h"
#include "json.h"
#include "net.h"
#include "result.h"
#include "task.h"
#ifdef SKEIN_ENABLE_TLS
#include "tls.h"
#endif
#ifdef SKEIN_ENABLE_FFI
#include "ffi_module.h"
#endif
#include "filetype.h"
#include "format.h"
#include "platform.h"
#include "process_builtin.h"
#include "process_spawn.h"
#include "random_builtin.h"
#include "regex_builtin.h"
#include "serialization.h"
#include "repl.h"
#include "structtype.h"
#include "time_builtin.h"
#include "worker_pool.h"
#include "unicode.h"
#include "valuetype.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
static size_t utf8_length_prefix(const char *s, size_t bytes) {
    size_t len = std::strlen(s);
    if (bytes > len)
        bytes = len;
    size_t pos = 0;
    size_t count = 0;
    while (pos < bytes) {
        pos += utf8_valid_sequence_length(s + pos, len - pos);
        ++count;
    }
    return count;
}

static std::string utf8_substr(const char *s, size_t start, size_t count) {
    size_t len = std::strlen(s);
    size_t pos = 0;
    size_t current = 0;
    while (pos < len && current < start) {
        pos += utf8_valid_sequence_length(s + pos, len - pos);
        ++current;
    }
    size_t begin = pos;
    size_t taken = 0;
    while (pos < len && taken < count) {
        pos += utf8_valid_sequence_length(s + pos, len - pos);
        ++taken;
    }
    return std::string(s + begin, pos - begin);
}

static bool is_valid_utf8(const std::string &data) {
    size_t pos = 0;
    while (pos < data.size()) {
        unsigned char c = (unsigned char)data[pos];
        if (c < 0x80) {
            ++pos;
            continue;
        }
        size_t n;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            n = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            n = 3;
            if (c == 0xE0)
                low = 0xA0;
            else if (c == 0xED)
                high = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            n = 4;
            if (c == 0xF0)
                low = 0x90;
            else if (c == 0xF4)
                high = 0x8F;
        } else {
            return false;
        }
        if (pos + n > data.size())
            return false;
        unsigned char second = (unsigned char)data[pos + 1];
        if (second < low || second > high)
            return false;
        for (size_t i = 2; i < n; ++i) {
            if (((unsigned char)data[pos + i] & 0xC0) != 0x80)
                return false;
        }
        pos += n;
    }
    return true;
}

static std::string bytes_to_hex_string(const std::string &data) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char byte = (unsigned char)data[i];
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_byteorder(const char *text, bool &little) {
    if (std::strcmp(text, "little") == 0 || std::strcmp(text, "le") == 0 ||
        std::strcmp(text, "little-endian") == 0) {
        little = true;
        return true;
    }
    if (std::strcmp(text, "big") == 0 || std::strcmp(text, "be") == 0 ||
        std::strcmp(text, "big-endian") == 0) {
        little = false;
        return true;
    }
    return false;
}

static const char *kind_of_value(const struct value_t *v) {
    if (!v) return "null";
    switch (v->type) {
        case T_int: return "int";
        case T_float: return "float";
        case T_bool: return "bool";
        case T_string: return "string";
        case T_char: return "char";
        case T_byte: return "byte";
        case T_bytes: return "bytes";
        case T_list: return "list";
        case T_dict: return "dict";
        case T_func: return "function";
        case T_builtin: return "builtin";
        case T_userdata: return "userdata";
        case T_result: return "result";
        case T_error: return "error";
        case T_struct: return "struct";
        case T_enum: return "enum";
        case T_lru: return "lru";
        case T_deque: return "deque";
        case T_heap: return "heap";
        case T_set: return "set";
        case T_method: return "method";
        case T_null: return "null";
        case T_class: return "class";
        case T_return: return "return";
        case T_break: return "break";
        case T_continue: return "continue";
        default: return "unknown";
    }
}

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

static struct value_t *enum_tag_from_value(struct value_t *value) {
    if (!value || value->type != T_enum) {
        yyerror("std.enum.tag expects an enum value");
        return NULL;
    }
    struct enum_value_t *ev = (struct enum_value_t *)value;
    if (!ev->def || ev->variant < 0 ||
        (size_t)ev->variant >= ev->def->variants.size()) {
        yyerror("invalid enum value");
        return NULL;
    }
    const struct variant_def &variant =
        ev->def->variants[(size_t)ev->variant];
    std::string tag = ev->def->module_name + "::" + ev->def->name + "::" +
                      variant.name;
    return newstrval(tag.c_str());
}

static struct value_t *enum_tag_from_name(struct value_t *type_value,
                                          struct value_t *variant_value) {
    if (!type_value || type_value->type != T_string) {
        yyerror("std.enum.tag expects a type name string");
        return NULL;
    }
    if (!variant_value || variant_value->type != T_string) {
        yyerror("std.enum.tag expects a variant name string");
        return NULL;
    }

    std::string type_name = ((struct string_value_t *)type_value)->value;
    std::string variant_name =
        ((struct string_value_t *)variant_value)->value;
    std::string module_name;
    size_t sep = type_name.find("::");
    if (sep != std::string::npos) {
        module_name = type_name.substr(0, sep);
        type_name = type_name.substr(sep + 2);
    }

    enum_def *def = structtype_find_enum_by_name(
        module_name.empty() ? NULL : module_name.c_str(),
        type_name.c_str());
    if (!def) {
        yyerror("unknown or ambiguous enum type %s", type_name.c_str());
        return NULL;
    }
    if (structtype_variant_index(def, variant_name) < 0) {
        yyerror("unknown enum variant %s::%s", def->name.c_str(),
                variant_name.c_str());
        return NULL;
    }

    std::string tag =
        def->module_name + "::" + def->name + "::" + variant_name;
    return newstrval(tag.c_str());
}

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

static std::shared_ptr<deque_payload> deque_shell_copy(
    const std::shared_ptr<deque_payload> &source) {
    std::shared_ptr<deque_payload> out = std::make_shared<deque_payload>();
    if (!source)
        return out;
    out->front = source->front;
    out->front_tail = source->front_tail;
    out->back = source->back;
    out->back_tail = source->back_tail;
    out->front_size = source->front_size;
    out->back_size = source->back_size;
    return out;
}

static struct value_t *deque_from_payload(
    const std::shared_ptr<deque_payload> &payload) {
    struct deque_value_t *out = new deque_value_t;
    out->type = T_deque;
    out->payload = payload;
    return (struct value_t *)out;
}

static std::shared_ptr<deque_payload> deque_push_front_payload(
    const std::shared_ptr<deque_payload> &source, struct value_t *value) {
    std::shared_ptr<deque_payload> out = deque_shell_copy(source);
    std::shared_ptr<deque_node> entry = std::make_shared<deque_node>();
    entry->value = dupval(value);
    entry->next = out->front;
    out->front = entry;
    out->front_size += 1;
    if (out->front_size == 1)
        out->front_tail = entry;
    return out;
}

static std::shared_ptr<deque_payload> deque_push_back_payload(
    const std::shared_ptr<deque_payload> &source, struct value_t *value) {
    std::shared_ptr<deque_payload> out = deque_shell_copy(source);
    std::shared_ptr<deque_node> entry = std::make_shared<deque_node>();
    entry->value = dupval(value);
    entry->next = out->back;
    out->back = entry;
    out->back_size += 1;
    if (out->back_size == 1)
        out->back_tail = entry;
    return out;
}

/* Move every back element to the front, reversing the order. */
static void deque_reverse_back_to_front(deque_payload *payload) {
    std::shared_ptr<deque_node> new_front;
    std::shared_ptr<deque_node> new_tail;
    for (std::shared_ptr<deque_node> node = payload->back; node;
         node = node->next) {
        std::shared_ptr<deque_node> entry = std::make_shared<deque_node>();
        entry->value = dupval(node->value);
        entry->next = new_front;
        if (!new_tail)
            new_tail = entry;
        new_front = entry;
    }
    payload->front = new_front;
    payload->front_tail = new_tail;
    payload->front_size = payload->back_size;
    payload->back.reset();
    payload->back_tail.reset();
    payload->back_size = 0;
}

/* Move every front element to the back, reversing the order. */
static void deque_reverse_front_to_back(deque_payload *payload) {
    std::shared_ptr<deque_node> new_back;
    std::shared_ptr<deque_node> new_tail;
    for (std::shared_ptr<deque_node> node = payload->front; node;
         node = node->next) {
        std::shared_ptr<deque_node> entry = std::make_shared<deque_node>();
        entry->value = dupval(node->value);
        entry->next = new_back;
        if (!new_tail)
            new_tail = entry;
        new_back = entry;
    }
    payload->back = new_back;
    payload->back_tail = new_tail;
    payload->back_size = payload->front_size;
    payload->front.reset();
    payload->front_tail.reset();
    payload->front_size = 0;
}

static struct value_t *deque_pop_result(
    const std::shared_ptr<deque_payload> &payload, struct value_t *value) {
    struct dict_value_t *out = newdictval();
    dict_append_item(out, newstrval("ok"), newboolval(true));
    dict_append_item(out, newstrval("deque"),
                     deque_from_payload(payload));
    dict_append_item(out, newstrval("value"), value);
    return (struct value_t *)out;
}

static struct value_t *deque_empty_result(
    const struct deque_value_t *deque) {
    struct dict_value_t *out = newdictval();
    dict_append_item(out, newstrval("ok"), newboolval(false));
    dict_append_item(out, newstrval("error"), newstrval("empty deque"));
    dict_append_item(out, newstrval("deque"),
                     dupval((struct value_t *)deque));
    return (struct value_t *)out;
}

static int heap_rank(const std::shared_ptr<heap_node> &node) {
    return node ? node->rank : 0;
}

static bool heap_call_less(struct value_t *less, struct value_t *a,
                           struct value_t *b, bool &result) {
    std::vector<struct value_t *> call_args;
    call_args.push_back(dupval(a));
    call_args.push_back(dupval(b));
    struct value_t *r = skein_call_function(less, std::move(call_args));
    if (!r)
        return false;
    result = truthy(r);
    freeval(r);
    return true;
}

struct heap_merge_state {
    struct value_t *less;
    bool failed = false;
};

/* Merge two leftist heaps; returns NULL and sets `failed` on comparator error. */
static std::shared_ptr<heap_node> heap_merge(
    struct heap_merge_state &state, std::shared_ptr<heap_node> left,
    std::shared_ptr<heap_node> right) {
    if (!left)
        return right;
    if (!right)
        return left;

    bool left_first = false;
    if (!heap_call_less(state.less, left->value, right->value, left_first)) {
        state.failed = true;
        return NULL;
    }
    if (!left_first)
        std::swap(left, right);

    std::shared_ptr<heap_node> rest = heap_merge(state, left->right, right);
    if (state.failed)
        return NULL;

    std::shared_ptr<heap_node> node = std::make_shared<heap_node>();
    node->value = dupval(left->value);
    node->left = left->left;
    node->right = rest;
    if (heap_rank(node->left) < heap_rank(node->right))
        std::swap(node->left, node->right);
    node->rank = heap_rank(node->right) + 1;
    return node;
}

static std::shared_ptr<heap_node> heap_singleton(struct value_t *value) {
    std::shared_ptr<heap_node> node = std::make_shared<heap_node>();
    node->value = dupval(value);
    node->rank = 1;
    return node;
}

static struct value_t *heap_from_parts(const struct heap_payload *source,
                                       std::shared_ptr<heap_node> root,
                                       std::size_t size) {
    struct heap_value_t *out = newheapval(dupval(source->less));
    out->payload->root = std::move(root);
    out->payload->size = size;
    return (struct value_t *)out;
}

static struct value_t *heap_pop_result(const struct heap_value_t *heap,
                                       std::shared_ptr<heap_node> root,
                                       struct value_t *value) {
    struct dict_value_t *out = newdictval();
    dict_append_item(out, newstrval("ok"), newboolval(true));
    dict_append_item(out, newstrval("heap"),
                     heap_from_parts(heap->payload.get(), std::move(root),
                                     heap->payload->size - 1));
    dict_append_item(out, newstrval("value"), value);
    return (struct value_t *)out;
}

static struct value_t *heap_empty_result(const struct heap_value_t *heap) {
    struct dict_value_t *out = newdictval();
    dict_append_item(out, newstrval("ok"), newboolval(false));
    dict_append_item(out, newstrval("error"), newstrval("empty heap"));
    dict_append_item(out, newstrval("heap"),
                     dupval((struct value_t *)heap));
    return (struct value_t *)out;
}

static bool heap_callable(struct value_t *value) {
    return value && (value->type == T_func || value->type == T_builtin);
}

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

static bool collect_command_env(
    const std::vector<struct value_t *> &args, size_t index,
    const char *who,
    std::vector<std::pair<std::string, std::string>> &env) {
    if (index >= args.size())
        return true;

    struct value_t *value = args[index];
    if (value->type != T_dict) {
        yyerror("%s env must be a dict, got %s", who, typename_of(value));
        return false;
    }

    struct dict_value_t *dict = (struct dict_value_t *)value;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        struct value_t *key = dict->payload->items[i].first;
        struct value_t *val = dict->payload->items[i].second;
        if (key->type != T_string) {
            yyerror("%s env keys must be strings", who);
            return false;
        }
        if (val->type != T_string) {
            yyerror("%s env values must be strings", who);
            return false;
        }
        const char *name = ((struct string_value_t *)key)->value;
        if (!name[0] || std::strchr(name, '=') != NULL) {
            yyerror("invalid environment variable name");
            return false;
        }
        env.emplace_back(name,
                         ((struct string_value_t *)val)->value);
    }
    return true;
}

static bool get_file_resource(struct value_t *value,
                              std::shared_ptr<file_resource> &file,
                              const char *who) {
    if (!is_file_value(value)) {
        yyerror("%s expects a file, got %s", who, typename_of(value));
        return false;
    }
    file = file_value(value);
    if (!file) {
        yyerror("%s expects a file", who);
        return false;
    }
    return true;
}

static void erase_last_utf8_character(std::string &line) {
    if (line.empty())
        return;
    size_t pos = line.size() - 1;
    while (pos > 0 && (((unsigned char)line[pos]) & 0xC0) == 0x80)
        --pos;
    line.erase(pos);
}

static bool read_console_line(const char *prompt, std::string &line,
                              bool &at_eof) {
    line.clear();
    at_eof = false;

    if (prompt) {
        std::fputs(prompt, stdout);
        std::fflush(stdout);
    }

    for (;;) {
        int c = std::fgetc(stdin);
        if (c == EOF) {
            if (g_interrupted) {
                std::clearerr(stdin);
                return false;
            }
            if (std::ferror(stdin)) {
                std::clearerr(stdin);
                yyerror("input: read error");
                return false;
            }
            at_eof = true;
            return true;
        }
        if (c == '\n')
            return true;
        if (c == '\r')
            continue;
        if (c == '\b' || c == 0x7f) {
            erase_last_utf8_character(line);
            continue;
        }
        line.push_back((char)c);
    }
}

static const char *math_builtin_name(int functype) {
    switch (functype) {
        case B_sin: return "sin";
        case B_cos: return "cos";
        case B_tan: return "tan";
        case B_asin: return "asin";
        case B_acos: return "acos";
        case B_atan: return "atan";
        case B_atan2: return "atan2";
        case B_pow: return "pow";
        case B_floor: return "floor";
        case B_ceil: return "ceil";
        case B_round: return "round";
        case B_trunc: return "trunc";
        case B_log2: return "log2";
        case B_log10: return "log10";
        case B_hypot: return "hypot";
        case B_min: return "min";
        case B_max: return "max";
        case B_math_sign: return "std.math.sign";
        case B_math_clamp: return "std.math.clamp";
        case B_math_fmod: return "std.math.fmod";
        case B_math_mod: return "std.math.mod";
        case B_math_is_nan: return "std.math.is_nan";
        case B_math_is_inf: return "std.math.is_inf";
        case B_math_is_finite: return "std.math.is_finite";
        case B_math_cbrt: return "std.math.cbrt";
        case B_math_gcd: return "std.math.gcd";
        case B_math_lcm: return "std.math.lcm";
        case B_math_degrees: return "std.math.degrees";
        case B_math_radians: return "std.math.radians";
        default: return "math function";
    }
}

struct value_t *call_builtin(int functype,
                             const std::vector<struct value_t *> &args) {
    if (functype >= B_net_tcp_connect && functype <= B_net_shutdown_async)
        return call_net_builtin(functype, args);
#ifdef SKEIN_ENABLE_FFI
    if (functype >= B_ffi_open && functype <= B_ffi_type_lookup)
        return call_ffi_builtin(functype, args);
#endif
    if (functype >= B_task_sleep && functype <= B_task_bounded_parallel_map)
        return call_task_builtin(functype, args);
    if (functype >= B_generator_next && functype <= B_generator_close)
        return call_generator_builtin(functype, args);
    if (functype >= B_result_ok && functype <= B_result_capture)
        return call_result_builtin(functype, args);
    if (functype >= B_error_new && functype <= B_error_message)
        return call_error_builtin(functype, args);
    if (functype >= B_io_read && functype <= B_io_write_async)
        return call_io_builtin(functype, args);
    if (functype >= B_json_parse && functype <= B_json_stringify)
        return call_json_builtin(functype, args);
    if (functype >= B_encoding_decode && functype <= B_encoding_supports)
        return call_encoding_builtin(functype, args);
    if (functype >= B_encoding_base64_encode &&
        functype <= B_encoding_url_decode)
        return call_encoding_builtin(functype, args);
    if (functype >= B_unicode_normalize && functype <= B_unicode_version)
        return call_unicode_builtin(functype, args);
    if (functype >= B_regex_match && functype <= B_regex_split)
        return call_regex_builtin(functype, args);
    if (functype >= B_time_now && functype <= B_time_sleep)
        return call_time_builtin(functype, args);
    if (functype >= B_random_seed && functype <= B_random_uuid)
        return call_random_builtin(functype, args);
    if (functype >= B_fs_listdir && functype <= B_fs_path_normalize)
        return call_fs_extra_builtin(functype, args);
    if (functype >= B_env_get && functype <= B_env_all)
        return call_env_builtin(functype, args);
    if (functype == B_process_run)
        return call_process_builtin(functype, args);
    if (functype >= B_process_spawn && functype <= B_process_pid)
        return call_process_spawn_builtin(functype, args);
    if (functype >= B_worker_pool && functype <= B_worker_stats)
        return call_worker_builtin(functype, args);
    if (functype >= B_crypto_sha256 && functype <= B_crypto_hmac_sha256)
        return call_crypto_builtin(functype, args);
    if (functype >= B_compress_gzip && functype <= B_compress_inflate)
        return call_compress_builtin(functype, args);
    if (functype >= B_http_request && functype <= B_http_close)
        return call_http_builtin(functype, args);
#ifdef SKEIN_ENABLE_HTTP2
    if (functype >= B_http2_request && functype <= B_http2_serve)
        return call_http2_builtin(functype, args);
#endif
#ifdef SKEIN_ENABLE_TLS
    if (functype >= B_tls_client && functype <= B_tls_alpn)
        return call_tls_builtin(functype, args);
#endif
    if (functype >= B_ws_connect && functype <= B_ws_close)
        return call_websocket_builtin(functype, args);

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

    if (functype == B_command || functype == B_command_status) {
        if (args.empty() || args.size() > 2) {
            yyerror("%s expects 1 or 2 arguments, got %d",
                    functype == B_command ? "command" : "command_status",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_write_file || functype == B_append_file) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_write_file ? "write_file" : "append_file",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_open || functype == B_file_write) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_file_open ? "file_open" : "file_write",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_read) {
        if (args.empty() || args.size() > 2) {
            yyerror("file_read expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_read_text) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.fs.read_text expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_write_text) {
        if (args.size() < 3 || args.size() > 4) {
            yyerror("std.fs.write_text expects 3 or 4 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_seek) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("file_seek expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_input) {
        if (args.size() > 1) {
            yyerror("input expects 0 or 1 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_split || functype == B_find) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_split ? "split" : "find",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_substr) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("substr expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_bytes_get || functype == B_bytes_concat ||
               functype == B_write_bytes || functype == B_append_bytes ||
               functype == B_file_write_bytes) {
        if (args.size() != 2) {
            const char *name =
                functype == B_bytes_get ? "bytes_get" :
                functype == B_bytes_concat ? "bytes_concat" :
                functype == B_write_bytes ? "write_bytes" :
                functype == B_append_bytes ? "append_bytes" :
                "file_write_bytes";
            yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
            return NULL;
        }
    } else if (functype == B_bytes_slice) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("bytes_slice expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_bytes_to_int) {
        if (args.size() < 3 || args.size() > 5) {
            yyerror("bytes_to_int expects 3 to 5 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_int_to_bytes) {
        if (args.size() != 3) {
            yyerror("int_to_bytes expects 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_file_read_bytes) {
        if (args.empty() || args.size() > 2) {
            yyerror("file_read_bytes expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_range) {
        if (args.empty() || args.size() > 3) {
            yyerror("range expects 1 to 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_slice) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.slice expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_contains || functype == B_list_index ||
               functype == B_dict_has || functype == B_string_join ||
               functype == B_string_starts_with ||
               functype == B_string_ends_with) {
        if (args.size() != 2) {
            const char *name =
                functype == B_list_contains ? "std.list.contains" :
                functype == B_list_index ? "std.list.index" :
                functype == B_dict_has ? "std.dict.has" :
                functype == B_string_join ? "std.string.join" :
                functype == B_string_starts_with ? "std.string.starts_with" :
                "std.string.ends_with";
            yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
            return NULL;
        }
    } else if (functype == B_dict_get) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.dict.get expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_string_replace) {
        if (args.size() != 3) {
            yyerror("std.string.replace expects 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_append || functype == B_dict_delete ||
               functype == B_dict_merge || functype == B_string_contains ||
               functype == B_string_repeat || functype == B_list_map ||
               functype == B_list_filter) {
        if (args.size() != 2) {
            const char *name =
                functype == B_list_append ? "std.list.append" :
                functype == B_dict_delete ? "std.dict.delete" :
                functype == B_dict_merge ? "std.dict.merge" :
                functype == B_string_contains ? "std.string.contains" :
                functype == B_string_repeat ? "std.string.repeat" :
                functype == B_list_map ? "std.list.map" : "std.list.filter";
            yyerror("%s expects 2 arguments, got %d", name, (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_reduce) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.reduce expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_sin || functype == B_cos ||
               functype == B_tan || functype == B_asin ||
               functype == B_acos || functype == B_atan ||
               functype == B_floor || functype == B_ceil ||
               functype == B_round || functype == B_trunc ||
               functype == B_log2 || functype == B_log10) {
        if (args.size() != 1) {
            yyerror("%s expects 1 argument, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_atan2 || functype == B_pow ||
               functype == B_hypot) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_math_sign ||
               functype == B_math_is_nan ||
               functype == B_math_is_inf ||
               functype == B_math_is_finite ||
               functype == B_math_cbrt ||
               functype == B_math_degrees ||
               functype == B_math_radians) {
        if (args.size() != 1) {
            yyerror("%s expects 1 argument, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_math_fmod ||
               functype == B_math_mod ||
               functype == B_math_gcd ||
               functype == B_math_lcm) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_math_clamp) {
        if (args.size() != 3) {
            yyerror("%s expects 3 arguments, got %d",
                    math_builtin_name(functype), (int)args.size());
            return NULL;
        }
    } else if (functype == B_min || functype == B_max) {
        if (args.empty()) {
            yyerror("%s expects at least 1 argument", math_builtin_name(functype));
            return NULL;
        }
    } else if (functype == B_struct_decode) {
        if (args.size() != 2) {
            yyerror("std.struct.decode expects 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_enum_tag) {
        if (args.size() < 1 || args.size() > 2) {
            yyerror("std.enum.tag expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_sort) {
        if (args.size() < 1 || args.size() > 2) {
            yyerror("std.list.sort expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_sort_by) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.sort_by expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_is_sorted) {
        if (args.size() < 1 || args.size() > 2) {
            yyerror("std.list.is_sorted expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_unique ||
               functype == B_list_unique_by) {
        size_t expected = functype == B_list_unique ? 1 : 2;
        if (args.size() != expected) {
            yyerror("%s expects %d argument%s, got %d",
                    functype == B_list_unique ? "std.list.unique"
                                              : "std.list.unique_by",
                    (int)expected, expected == 1 ? "" : "s",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_group_by ||
               functype == B_list_chunk) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_list_group_by ? "std.list.group_by"
                                                : "std.list.chunk",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_window) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("std.list.window expects 2 or 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_zip) {
        if (args.size() < 2) {
            yyerror("std.list.zip expects at least 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_list_top_k ||
               functype == B_list_binary_search) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("%s expects 2 or 3 arguments, got %d",
                    functype == B_list_top_k ? "std.list.top_k"
                                             : "std.list.binary_search",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_lru_new) {
        if (args.size() != 1) {
            yyerror("std.lru.new expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_lru_get || functype == B_lru_peek) {
        if (args.size() < 2 || args.size() > 3) {
            yyerror("%s expects 2 or 3 arguments, got %d",
                    functype == B_lru_get ? "std.lru.get" : "std.lru.peek",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_lru_put) {
        if (args.size() != 3) {
            yyerror("std.lru.put expects 3 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_lru_delete || functype == B_lru_has) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_lru_delete ? "std.lru.delete"
                                             : "std.lru.has",
                    (int)args.size());
            return NULL;
        }
    } else if (functype >= B_lru_keys && functype <= B_lru_clear) {
        if (args.size() != 1) {
            yyerror("std.lru function expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_deque_new) {
        if (!args.empty()) {
            yyerror("std.deque.new expects 0 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_deque_push_front ||
               functype == B_deque_push_back) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_deque_push_front
                        ? "std.deque.push_front"
                        : "std.deque.push_back",
                    (int)args.size());
            return NULL;
        }
    } else if (functype >= B_deque_from_list &&
               functype <= B_deque_to_list) {
        if (args.size() != 1) {
            yyerror("std.deque function expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_heap_new) {
        if (args.size() != 1) {
            yyerror("std.heap.new expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_heap_from_list || functype == B_heap_push) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_heap_from_list ? "std.heap.from_list"
                                                 : "std.heap.push",
                    (int)args.size());
            return NULL;
        }
    } else if (functype >= B_heap_size && functype <= B_heap_to_list) {
        if (args.size() != 1) {
            yyerror("std.heap function expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_set_new) {
        if (!args.empty()) {
            yyerror("std.set.new expects 0 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_set_from_list || functype == B_set_size ||
               functype == B_set_values) {
        if (args.size() != 1) {
            yyerror("%s expects 1 argument, got %d",
                    functype == B_set_from_list ? "std.set.from_list"
                    : functype == B_set_size ? "std.set.size"
                                             : "std.set.values",
                    (int)args.size());
            return NULL;
        }
    } else if (functype == B_set_add || functype == B_set_remove ||
               functype == B_set_has || functype == B_set_union ||
               functype == B_set_intersect || functype == B_set_difference ||
               functype == B_set_is_subset) {
        if (args.size() != 2) {
            yyerror("%s expects 2 arguments, got %d",
                    functype == B_set_add ? "std.set.add"
                    : functype == B_set_remove ? "std.set.remove"
                    : functype == B_set_has ? "std.set.has"
                    : functype == B_set_union ? "std.set.union"
                    : functype == B_set_intersect ? "std.set.intersect"
                    : functype == B_set_difference ? "std.set.difference"
                                                   : "std.set.is_subset",
                    (int)args.size());
            return NULL;
        }
    } else if (args.size() != 1) {
        yyerror("built-in function expects 1 argument, got %d", (int)args.size());
        return NULL;
    }

    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
        case B_sin:
        case B_cos:
        case B_tan:
        case B_asin:
        case B_acos:
        case B_atan: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            double result = 0.0;
            if (functype == B_sin) result = std::sin(value);
            else if (functype == B_cos) result = std::cos(value);
            else if (functype == B_tan) result = std::tan(value);
            else if (functype == B_asin) result = std::asin(value);
            else if (functype == B_acos) result = std::acos(value);
            else result = std::atan(value);
            ret = newfloatval(result);
            break;
        }
        case B_atan2:
        case B_pow:
        case B_hypot: {
            if (!is_numeric(args[0]) || !is_numeric(args[1])) {
                yyerror("%s expects two numbers, got %s and %s",
                        math_builtin_name(functype), typename_of(args[0]),
                        typename_of(args[1]));
                break;
            }
            double a = as_double(args[0]);
            double b = as_double(args[1]);
            if (functype == B_atan2) ret = newfloatval(std::atan2(a, b));
            else if (functype == B_pow) ret = newfloatval(std::pow(a, b));
            else ret = newfloatval(std::hypot(a, b));
            break;
        }
        case B_floor:
        case B_ceil:
        case B_round:
        case B_trunc: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            if (std::isnan(value) || std::isinf(value)) {
                yyerror("%s expects a finite number", math_builtin_name(functype));
                break;
            }
            double result = 0.0;
            if (functype == B_floor) result = std::floor(value);
            else if (functype == B_ceil) result = std::ceil(value);
            else if (functype == B_round) result = std::round(value);
            else result = std::trunc(value);
            if (result < -9223372036854775808.0 ||
                result >= 9223372036854775808.0)
                yyerror("%s result out of integer range",
                        math_builtin_name(functype));
            else
                ret = newintval((long long)result);
            break;
        }
        case B_log2:
        case B_log10: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            if (value <= 0.0)
                ret = newfloatval(std::nan(""));
            else
                ret = newfloatval(functype == B_log2
                                      ? std::log2(value)
                                      : std::log10(value));
            break;
        }
        case B_math_sign: {
            if (!is_numeric(arg)) {
                yyerror("std.math.sign expects a number, got %s",
                        typename_of(arg));
                break;
            }
            if (arg->type == T_float) {
                double value = ((struct float_value_t *)arg)->value;
                if (std::isnan(value))
                    ret = newfloatval(value);
                else
                    ret = newintval(value < 0.0 ? -1 : (value > 0.0 ? 1 : 0));
            } else {
                long long value = ((struct int_value_t *)arg)->value;
                ret = newintval(value < 0 ? -1 : (value > 0 ? 1 : 0));
            }
            break;
        }
        case B_math_clamp: {
            for (struct value_t *value : args) {
                if (!is_numeric(value)) {
                    yyerror("std.math.clamp expects numbers, got %s",
                            typename_of(value));
                    return NULL;
                }
            }
            if (args[0]->type == T_int && args[1]->type == T_int &&
                args[2]->type == T_int) {
                long long value = ((struct int_value_t *)args[0])->value;
                long long low = ((struct int_value_t *)args[1])->value;
                long long high = ((struct int_value_t *)args[2])->value;
                if (low > high) {
                    yyerror("std.math.clamp lower bound exceeds upper bound");
                    break;
                }
                if (value < low)
                    value = low;
                else if (value > high)
                    value = high;
                ret = newintval(value);
            } else {
                double value = as_double(args[0]);
                double low = as_double(args[1]);
                double high = as_double(args[2]);
                if (low > high) {
                    yyerror("std.math.clamp lower bound exceeds upper bound");
                    break;
                }
                if (value < low)
                    value = low;
                else if (value > high)
                    value = high;
                ret = newfloatval(value);
            }
            break;
        }
        case B_math_fmod: {
            if (!is_numeric(args[0]) || !is_numeric(args[1])) {
                yyerror("std.math.fmod expects numbers, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            ret = newfloatval(
                std::fmod(as_double(args[0]), as_double(args[1])));
            break;
        }
        case B_math_mod: {
            if (!is_numeric(args[0]) || !is_numeric(args[1])) {
                yyerror("std.math.mod expects numbers, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            if (args[0]->type == T_int && args[1]->type == T_int) {
                long long left = ((struct int_value_t *)args[0])->value;
                long long right = ((struct int_value_t *)args[1])->value;
                if (right == 0) {
                    yyerror("modulo by zero");
                    break;
                }
                long long value = left % right;
                if (value != 0 && ((value < 0) != (right < 0)))
                    value += right;
                ret = newintval(value);
            } else {
                double left = as_double(args[0]);
                double right = as_double(args[1]);
                double value = std::fmod(left, right);
                if (value != 0.0 && ((value < 0.0) != (right < 0.0)))
                    value += right;
                ret = newfloatval(value);
            }
            break;
        }
        case B_math_is_nan:
        case B_math_is_inf:
        case B_math_is_finite: {
            if (!is_numeric(arg)) {
                yyerror("%s expects a number, got %s",
                        math_builtin_name(functype), typename_of(arg));
                break;
            }
            double value = as_double(arg);
            if (functype == B_math_is_nan)
                ret = newboolval(std::isnan(value));
            else if (functype == B_math_is_inf)
                ret = newboolval(std::isinf(value));
            else
                ret = newboolval(std::isfinite(value));
            break;
        }
        case B_math_cbrt: {
            if (!is_numeric(arg)) {
                yyerror("std.math.cbrt expects a number, got %s",
                        typename_of(arg));
                break;
            }
            ret = newfloatval(std::cbrt(as_double(arg)));
            break;
        }
        case B_math_gcd:
        case B_math_lcm: {
            if (args[0]->type != T_int || args[1]->type != T_int) {
                yyerror("%s expects integers, got %s and %s",
                        math_builtin_name(functype),
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            __int128 left = ((struct int_value_t *)args[0])->value;
            __int128 right = ((struct int_value_t *)args[1])->value;
            if (left < 0)
                left = -left;
            if (right < 0)
                right = -right;
            __int128 a = left;
            __int128 b = right;
            while (b != 0) {
                __int128 next = a % b;
                a = b;
                b = next;
            }
            if (functype == B_math_gcd) {
                ret = new_int_checked(a);
            } else if (left == 0 || right == 0) {
                ret = newintval(0);
            } else {
                ret = new_int_checked((left / a) * right);
            }
            break;
        }
        case B_math_degrees: {
            if (!is_numeric(arg)) {
                yyerror("std.math.degrees expects a number, got %s",
                        typename_of(arg));
                break;
            }
            ret = newfloatval(
                as_double(arg) * (180.0 / 3.14159265358979323846));
            break;
        }
        case B_math_radians: {
            if (!is_numeric(arg)) {
                yyerror("std.math.radians expects a number, got %s",
                        typename_of(arg));
                break;
            }
            ret = newfloatval(
                as_double(arg) * (3.14159265358979323846 / 180.0));
            break;
        }
        case B_min:
        case B_max: {
            bool all_int = true;
            for (const struct value_t *value : args) {
                if (!is_numeric(value)) {
                    yyerror("%s expects numbers, got %s",
                            math_builtin_name(functype), typename_of(value));
                    return NULL;
                }
                if (value->type != T_int)
                    all_int = false;
            }
            if (all_int) {
                long long best = ((struct int_value_t *)args[0])->value;
                for (size_t i = 1; i < args.size(); ++i) {
                    long long value =
                        ((struct int_value_t *)args[i])->value;
                    if ((functype == B_min && value < best) ||
                        (functype == B_max && value > best))
                        best = value;
                }
                ret = newintval(best);
            } else {
                double best = as_double(args[0]);
                for (size_t i = 1; i < args.size(); ++i) {
                    double value = as_double(args[i]);
                    if ((functype == B_min && value < best) ||
                        (functype == B_max && value > best))
                        best = value;
                }
                ret = newfloatval(best);
            }
            break;
        }
        case B_kind_of:
            ret = newstrval(kind_of_value(arg));
            break;
        case B_type_name:
            ret = newstrval(typename_of(arg));
            break;
        case B_enum_decode:
            ret = enum_decode_value(arg);
            break;
        case B_enum_tag:
            if (args.size() == 1)
                ret = enum_tag_from_value(arg);
            else
                ret = enum_tag_from_name(args[0], args[1]);
            break;
        case B_struct_decode:
            ret = struct_decode_value(args[0], args[1]);
            break;
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
        case B_deque_new:
            ret = (struct value_t *)newdequeval();
            break;
        case B_deque_from_list: {
            if (arg->type != T_list) {
                yyerror("std.deque.from_list expects a list, got %s",
                        typename_of(arg));
                break;
            }
            struct list_value_t *list = (struct list_value_t *)arg;
            std::shared_ptr<deque_payload> payload =
                std::make_shared<deque_payload>();
            std::shared_ptr<deque_node> head;
            std::shared_ptr<deque_node> tail;
            for (struct value_t *item : list->payload->items) {
                std::shared_ptr<deque_node> entry =
                    std::make_shared<deque_node>();
                entry->value = dupval(item);
                entry->next = head;
                head = entry;
                if (!tail)
                    tail = entry;
            }
            payload->back = head;
            payload->back_tail = tail;
            payload->back_size = list->payload->items.size();
            ret = deque_from_payload(payload);
            break;
        }
        case B_deque_size: {
            if (arg->type != T_deque) {
                yyerror("std.deque.size expects a deque, got %s",
                        typename_of(arg));
                break;
            }
            struct deque_value_t *deque = (struct deque_value_t *)arg;
            ret = newintval((long long)(deque->payload->front_size +
                                        deque->payload->back_size));
            break;
        }
        case B_deque_push_front: {
            if (arg->type != T_deque) {
                yyerror("std.deque.push_front expects a deque, got %s",
                        typename_of(arg));
                break;
            }
            ret = deque_from_payload(deque_push_front_payload(
                ((struct deque_value_t *)arg)->payload, args[1]));
            break;
        }
        case B_deque_push_back: {
            if (arg->type != T_deque) {
                yyerror("std.deque.push_back expects a deque, got %s",
                        typename_of(arg));
                break;
            }
            ret = deque_from_payload(deque_push_back_payload(
                ((struct deque_value_t *)arg)->payload, args[1]));
            break;
        }
        case B_deque_peek_front: {
            if (arg->type != T_deque) {
                yyerror("std.deque.peek_front expects a deque, got %s",
                        typename_of(arg));
                break;
            }
            struct deque_value_t *deque = (struct deque_value_t *)arg;
            if (deque->payload->front_size > 0)
                ret = dupval(deque->payload->front->value);
            else if (deque->payload->back_size > 0)
                ret = dupval(deque->payload->back_tail->value);
            else
                ret = newnull();
            break;
        }
        case B_deque_peek_back: {
            if (arg->type != T_deque) {
                yyerror("std.deque.peek_back expects a deque, got %s",
                        typename_of(arg));
                break;
            }
            struct deque_value_t *deque = (struct deque_value_t *)arg;
            if (deque->payload->back_size > 0)
                ret = dupval(deque->payload->back->value);
            else if (deque->payload->front_size > 0)
                ret = dupval(deque->payload->front_tail->value);
            else
                ret = newnull();
            break;
        }
        case B_deque_pop_front: {
            if (arg->type != T_deque) {
                yyerror("std.deque.pop_front expects a deque, got %s",
                        typename_of(arg));
                break;
            }
            struct deque_value_t *deque = (struct deque_value_t *)arg;
            if (deque->payload->front_size == 0 &&
                deque->payload->back_size == 0) {
                ret = deque_empty_result(deque);
                break;
            }
            std::shared_ptr<deque_payload> payload =
                deque_shell_copy(deque->payload);
            if (payload->front_size == 0)
                deque_reverse_back_to_front(payload.get());
            std::shared_ptr<deque_node> node = payload->front;
            struct value_t *value = dupval(node->value);
            payload->front = node->next;
            payload->front_size -= 1;
            if (payload->front_size == 0)
                payload->front_tail.reset();
            ret = deque_pop_result(payload, value);
            break;
        }
        case B_deque_pop_back: {
            if (arg->type != T_deque) {
                yyerror("std.deque.pop_back expects a deque, got %s",
                        typename_of(arg));
                break;
            }
            struct deque_value_t *deque = (struct deque_value_t *)arg;
            if (deque->payload->front_size == 0 &&
                deque->payload->back_size == 0) {
                ret = deque_empty_result(deque);
                break;
            }
            std::shared_ptr<deque_payload> payload =
                deque_shell_copy(deque->payload);
            if (payload->back_size == 0)
                deque_reverse_front_to_back(payload.get());
            std::shared_ptr<deque_node> node = payload->back;
            struct value_t *value = dupval(node->value);
            payload->back = node->next;
            payload->back_size -= 1;
            if (payload->back_size == 0)
                payload->back_tail.reset();
            ret = deque_pop_result(payload, value);
            break;
        }
        case B_deque_to_list: {
            if (arg->type != T_deque) {
                yyerror("std.deque.to_list expects a deque, got %s",
                        typename_of(arg));
                break;
            }
            struct deque_value_t *deque = (struct deque_value_t *)arg;
            std::vector<value_t *> items;
            deque_items_in_order(deque->payload.get(), items);
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(items.size());
            for (struct value_t *item : items)
                out->payload->items.push_back(dupval(item));
            ret = (struct value_t *)out;
            break;
        }
        case B_heap_new: {
            if (!heap_callable(arg)) {
                yyerror("std.heap.new expects a callable comparator, got %s",
                        typename_of(arg));
                break;
            }
            ret = (struct value_t *)newheapval(dupval(arg));
            break;
        }
        case B_heap_from_list: {
            if (arg->type != T_list) {
                yyerror("std.heap.from_list expects a list, got %s",
                        typename_of(arg));
                break;
            }
            if (!heap_callable(args[1])) {
                yyerror("std.heap.from_list expects a callable comparator, got %s",
                        typename_of(args[1]));
                break;
            }
            struct list_value_t *list = (struct list_value_t *)arg;
            struct heap_merge_state state;
            state.less = args[1];
            std::shared_ptr<heap_node> root;
            for (struct value_t *item : list->payload->items) {
                root = heap_merge(state, root, heap_singleton(item));
                if (state.failed)
                    break;
            }
            if (state.failed)
                break;
            struct heap_value_t *out = newheapval(dupval(args[1]));
            out->payload->root = std::move(root);
            out->payload->size = list->payload->items.size();
            ret = (struct value_t *)out;
            break;
        }
        case B_heap_size: {
            if (arg->type != T_heap) {
                yyerror("std.heap.size expects a heap, got %s",
                        typename_of(arg));
                break;
            }
            ret = newintval((long long)((struct heap_value_t *)arg)
                                ->payload->size);
            break;
        }
        case B_heap_peek: {
            if (arg->type != T_heap) {
                yyerror("std.heap.peek expects a heap, got %s",
                        typename_of(arg));
                break;
            }
            struct heap_value_t *heap = (struct heap_value_t *)arg;
            if (heap->payload->size == 0)
                ret = newnull();
            else
                ret = dupval(heap->payload->root->value);
            break;
        }
        case B_heap_push: {
            if (arg->type != T_heap) {
                yyerror("std.heap.push expects a heap, got %s",
                        typename_of(arg));
                break;
            }
            struct heap_value_t *heap = (struct heap_value_t *)arg;
            struct heap_merge_state state;
            state.less = heap->payload->less;
            std::shared_ptr<heap_node> root =
                heap_merge(state, heap->payload->root,
                           heap_singleton(args[1]));
            if (state.failed)
                break;
            ret = heap_from_parts(heap->payload.get(), std::move(root),
                                  heap->payload->size + 1);
            break;
        }
        case B_heap_pop: {
            if (arg->type != T_heap) {
                yyerror("std.heap.pop expects a heap, got %s",
                        typename_of(arg));
                break;
            }
            struct heap_value_t *heap = (struct heap_value_t *)arg;
            if (heap->payload->size == 0) {
                ret = heap_empty_result(heap);
                break;
            }
            struct value_t *value = dupval(heap->payload->root->value);
            struct heap_merge_state state;
            state.less = heap->payload->less;
            std::shared_ptr<heap_node> root =
                heap_merge(state, heap->payload->root->left,
                           heap->payload->root->right);
            if (state.failed) {
                freeval(value);
                break;
            }
            ret = heap_pop_result(heap, std::move(root), value);
            break;
        }
        case B_heap_to_list: {
            if (arg->type != T_heap) {
                yyerror("std.heap.to_list expects a heap, got %s",
                        typename_of(arg));
                break;
            }
            struct heap_value_t *heap = (struct heap_value_t *)arg;
            std::vector<value_t *> items;
            heap_items_level_order(heap->payload.get(), items);
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(items.size());
            for (struct value_t *item : items)
                out->payload->items.push_back(dupval(item));
            ret = (struct value_t *)out;
            break;
        }
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
        case B_type_module:
            if (arg && arg->type == T_struct) {
                struct struct_value_t *sv = (struct struct_value_t *)arg;
                ret = newstrval(
                    sv->def ? sv->def->module_name.c_str() : "");
            } else if (arg && arg->type == T_enum) {
                struct enum_value_t *ev = (struct enum_value_t *)arg;
                ret = newstrval(
                    ev->def ? ev->def->module_name.c_str() : "");
            } else {
                ret = newstrval("");
            }
            break;
        case B_type_source:
            if (arg && arg->type == T_struct) {
                struct struct_value_t *sv = (struct struct_value_t *)arg;
                ret = newstrval(sv->def ? sv->def->module.c_str() : "");
            } else if (arg && arg->type == T_enum) {
                struct enum_value_t *ev = (struct enum_value_t *)arg;
                ret = newstrval(ev->def ? ev->def->module.c_str() : "");
            } else {
                ret = newstrval("");
            }
            break;
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
        case B_len:
            if (arg->type == T_string) {
                ret = newintval((long long)utf8_length(
                    ((struct string_value_t *)arg)->value));
            } else if (arg->type == T_char || arg->type == T_byte) {
                ret = newintval(1);
            } else if (arg->type == T_bytes) {
                ret = newintval((long long)((struct bytes_value_t *)arg)->value.size());
            } else if (arg->type == T_list) {
                ret = newintval((long long)((struct list_value_t *)arg)->payload->items.size());
            } else if (arg->type == T_dict) {
                ret = newintval((long long)((struct dict_value_t *)arg)->payload->items.size());
            } else if (arg->type == T_deque) {
                struct deque_value_t *deque = (struct deque_value_t *)arg;
                ret = newintval((long long)(deque->payload->front_size +
                                            deque->payload->back_size));
            } else if (arg->type == T_heap) {
                ret = newintval(
                    (long long)((struct heap_value_t *)arg)->payload->size);
            } else if (arg->type == T_set) {
                ret = newintval(
                    (long long)((struct set_value_t *)arg)->payload->size);
            } else {
                yyerror("len expects string, bytes, list, dict, deque, heap, or set, got %s",
                        typename_of(arg));
            }
            break;
        case B_range: {
            long long start = 0;
            long long length = 0;
            long long step = 1;

            if (args.size() == 1) {
                if (args[0]->type != T_int) {
                    yyerror("range length must be an integer");
                    break;
                }
                length = ((struct int_value_t *)args[0])->value;
            } else {
                if (args[0]->type != T_int) {
                    yyerror("range start must be an integer");
                    break;
                }
                start = ((struct int_value_t *)args[0])->value;
                if (args[1]->type != T_int) {
                    yyerror("range length must be an integer");
                    break;
                }
                length = ((struct int_value_t *)args[1])->value;
                if (args.size() == 3) {
                    if (args[2]->type != T_int) {
                        yyerror("range step must be an integer");
                        break;
                    }
                    step = ((struct int_value_t *)args[2])->value;
                }
            }

            if (length < 0) {
                yyerror("range length must be non-negative");
                break;
            }
            if (step == 0) {
                yyerror("range step must not be zero");
                break;
            }

            struct list_value_t *list = newlistval();
            list->payload->items.reserve((size_t)length);
            for (long long i = 0; i < length; ++i) {
                __int128 value = (__int128)start + (__int128)i * step;
                if (value < LLONG_MIN || value > LLONG_MAX) {
                    yyerror("range value overflow");
                    freeval((struct value_t *)list);
                    list = NULL;
                    break;
                }
                list->payload->items.push_back(newintval((long long)value));
            }
            if (list)
                ret = (struct value_t *)list;
            break;
        }
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
        case B_dict_get:
        case B_dict_has: {
            if (args[0]->type != T_dict) {
                yyerror("%s expects a dict, got %s",
                        functype == B_dict_get ? "std.dict.get"
                                               : "std.dict.has",
                        typename_of(args[0]));
                break;
            }
            struct dict_value_t *dict = (struct dict_value_t *)args[0];
            if (!dict_key_allowed(args[1])) {
                yyerror("%s: %s, got %s",
                        functype == B_dict_get ? "std.dict.get"
                                               : "std.dict.has",
                        dict_key_error(), typename_of(args[1]));
                break;
            }
            int found_index = dict_find_index(dict, args[1]);
            struct value_t *found =
                found_index >= 0
                    ? dict->payload->items[(size_t)found_index].second
                    : NULL;
            if (functype == B_dict_has) {
                ret = newintval(found ? 1 : 0);
            } else if (found) {
                ret = dupval(found);
            } else if (args.size() == 3) {
                ret = dupval(args[2]);
            } else {
                ret = newnull();
            }
            break;
        }
        case B_dict_keys:
        case B_dict_values: {
            if (arg->type != T_dict) {
                yyerror("%s expects a dict, got %s",
                        functype == B_dict_keys ? "std.dict.keys"
                                                : "std.dict.values",
                        typename_of(arg));
                break;
            }
            struct dict_value_t *dict = (struct dict_value_t *)arg;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(dict->payload->items.size());
            for (auto &entry : dict->payload->items)
                out->payload->items.push_back(dupval(
                    functype == B_dict_keys ? entry.first : entry.second));
            ret = (struct value_t *)out;
            break;
        }
        case B_dict_items: {
            if (arg->type != T_dict) {
                yyerror("std.dict.items expects a dict, got %s",
                        typename_of(arg));
                break;
            }
            struct dict_value_t *dict = (struct dict_value_t *)arg;
            struct list_value_t *out = newlistval();
            out->payload->items.reserve(dict->payload->items.size());
            for (auto &entry : dict->payload->items) {
                struct list_value_t *pair = newlistval();
                pair->payload->items.push_back(dupval(entry.first));
                pair->payload->items.push_back(dupval(entry.second));
                out->payload->items.push_back((struct value_t *)pair);
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_dict_delete: {
            if (args[0]->type != T_dict) {
                yyerror("std.dict.delete expects a dict, got %s",
                        typename_of(args[0]));
                break;
            }
            struct dict_value_t *source = (struct dict_value_t *)args[0];
            if (!dict_key_allowed(args[1])) {
                yyerror("std.dict.delete: %s, got %s", dict_key_error(),
                        typename_of(args[1]));
                break;
            }
            int remove_index = dict_find_index(source, args[1]);
            struct dict_value_t *out = newdictval();
            out->payload->items.reserve(source->payload->items.size());
            for (size_t i = 0; i < source->payload->items.size(); ++i) {
                if ((int)i == remove_index)
                    continue;
                dict_append_item(out,
                                 dupval(source->payload->items[i].first),
                                 dupval(source->payload->items[i].second));
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_dict_merge: {
            if (args[0]->type != T_dict || args[1]->type != T_dict) {
                yyerror("std.dict.merge expects two dicts");
                break;
            }
            struct dict_value_t *left = (struct dict_value_t *)args[0];
            struct dict_value_t *right = (struct dict_value_t *)args[1];
            struct dict_value_t *out = newdictval();
            out->payload->items.reserve(left->payload->items.size() +
                                        right->payload->items.size());
            for (auto &entry : left->payload->items) {
                if (!dict_key_allowed(entry.first)) {
                    yyerror("std.dict.merge: %s, got %s", dict_key_error(),
                            typename_of(entry.first));
                    freeval((struct value_t *)out);
                    return NULL;
                }
                dict_append_item(out, dupval(entry.first),
                                 dupval(entry.second));
            }
            for (auto &entry : right->payload->items) {
                if (!dict_key_allowed(entry.first)) {
                    yyerror("std.dict.merge: %s, got %s", dict_key_error(),
                            typename_of(entry.first));
                    freeval((struct value_t *)out);
                    return NULL;
                }
                int found = dict_find_index(out, entry.first);
                if (found >= 0) {
                    freeval(out->payload->items[(size_t)found].second);
                    out->payload->items[(size_t)found].second =
                        dupval(entry.second);
                } else {
                    dict_append_item(out, dupval(entry.first),
                                     dupval(entry.second));
                }
            }
            ret = (struct value_t *)out;
            break;
        }
        case B_string_join: {
            if (args[0]->type != T_list || args[1]->type != T_string) {
                yyerror("std.string.join expects a list and a string separator");
                break;
            }
            struct list_value_t *list = (struct list_value_t *)args[0];
            const char *separator = ((struct string_value_t *)args[1])->value;
            std::string result;
            bool bad = false;
            for (size_t i = 0; i < list->payload->items.size(); ++i) {
                struct value_t *item = list->payload->items[i];
                if (item->type != T_string) {
                    yyerror("std.string.join expects string elements, got %s",
                            typename_of(item));
                    bad = true;
                    break;
                }
                if (i)
                    result += separator;
                result += ((struct string_value_t *)item)->value;
            }
            if (!bad)
                ret = newstrval(result.c_str());
            break;
        }
        case B_string_replace: {
            if (args[0]->type != T_string || args[1]->type != T_string ||
                args[2]->type != T_string) {
                yyerror("std.string.replace expects three strings");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *old_text = ((struct string_value_t *)args[1])->value;
            const char *new_text = ((struct string_value_t *)args[2])->value;
            if (!*old_text) {
                yyerror("std.string.replace search must not be empty");
                break;
            }
            std::string source(text);
            std::string old_value(old_text);
            std::string new_value(new_text);
            std::string result;
            size_t position = 0;
            for (;;) {
                size_t found = source.find(old_value, position);
                if (found == std::string::npos) {
                    result.append(source, position, std::string::npos);
                    break;
                }
                result.append(source, position, found - position);
                result += new_value;
                position = found + old_value.size();
            }
            ret = newstrval(result.c_str());
            break;
        }
        case B_string_starts_with:
        case B_string_ends_with: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("%s expects two strings",
                        functype == B_string_starts_with
                            ? "std.string.starts_with"
                            : "std.string.ends_with");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *part = ((struct string_value_t *)args[1])->value;
            size_t text_len = std::strlen(text);
            size_t part_len = std::strlen(part);
            bool match = part_len <= text_len;
            if (match && functype == B_string_starts_with)
                match = std::memcmp(text, part, part_len) == 0;
            else if (match)
                match = std::memcmp(text + text_len - part_len, part,
                                    part_len) == 0;
            ret = newintval(match ? 1 : 0);
            break;
        }
        case B_string_contains: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("std.string.contains expects two strings");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *needle = ((struct string_value_t *)args[1])->value;
            ret = newintval(std::strstr(text, needle) ? 1 : 0);
            break;
        }
        case B_string_repeat: {
            if (args[0]->type != T_string || args[1]->type != T_int) {
                yyerror("std.string.repeat expects a string and an integer");
                break;
            }
            long long count = ((struct int_value_t *)args[1])->value;
            if (count < 0) {
                yyerror("std.string.repeat count must be non-negative");
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            std::string result;
            size_t length = std::strlen(text);
            result.reserve(length * (size_t)count);
            for (long long i = 0; i < count; ++i)
                result += text;
            ret = newstrval(result.c_str());
            break;
        }
        case B_string_trim_start:
        case B_string_trim_end: {
            if (arg->type != T_string) {
                yyerror("%s expects a string",
                        functype == B_string_trim_start
                            ? "std.string.trim_start"
                            : "std.string.trim_end");
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            const char *begin = text;
            const char *end = text + std::strlen(text);
            if (functype == B_string_trim_start) {
                while (begin < end && std::isspace((unsigned char)*begin))
                    ++begin;
            } else {
                while (end > begin && std::isspace((unsigned char)end[-1]))
                    --end;
            }
            ret = newstrval(std::string(begin, (size_t)(end - begin)).c_str());
            break;
        }
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
            } else if (arg->type == T_bool) {
                ret = newintval(((struct bool_value_t *)arg)->value ? 1 : 0);
            } else if (arg->type == T_char) {
                ret = newintval(((struct char_value_t *)arg)->value);
            } else if (arg->type == T_byte) {
                ret = newintval(((struct byte_value_t *)arg)->value);
            } else if (arg->type == T_string) {
                long long value = 0;
                if (parse_int_string(((struct string_value_t *)arg)->value, value))
                    ret = newintval(value);
                else
                    yyerror("int() cannot convert string \"%s\"",
                            ((struct string_value_t *)arg)->value);
            } else {
                yyerror("int() expects int, float, bool, char, or string, got %s",
                        typename_of(arg));
            }
            break;
        case B_float:
            if (arg->type == T_int) {
                ret = newfloatval((double)((struct int_value_t *)arg)->value);
            } else if (arg->type == T_float) {
                ret = newfloatval(((struct float_value_t *)arg)->value);
            } else if (arg->type == T_bool) {
                ret = newfloatval(((struct bool_value_t *)arg)->value ? 1.0 : 0.0);
            } else if (arg->type == T_string) {
                double value = 0.0;
                if (parse_float_string(((struct string_value_t *)arg)->value, value))
                    ret = newfloatval(value);
                else
                    yyerror("float() cannot convert string \"%s\"",
                            ((struct string_value_t *)arg)->value);
            } else {
                yyerror("float() expects int, float, bool, or string, got %s",
                        typename_of(arg));
            }
            break;
        case B_bool:
            ret = newboolval(truthy(arg));
            break;
        case B_str:
            ret = newstrval(value_to_string(arg).c_str());
            break;
        case B_char:
            if (arg->type == T_char) {
                ret = dupval(arg);
            } else if (arg->type == T_int) {
                long long value = ((struct int_value_t *)arg)->value;
                if (value < 0 || value > 0x10FFFF ||
                    (value >= 0xD800 && value <= 0xDFFF)) {
                    yyerror("char() codepoint out of range");
                } else {
                    ret = newcharval((unsigned int)value);
                }
            } else if (arg->type == T_string) {
                const char *text = ((struct string_value_t *)arg)->value;
                size_t consumed = 0;
                unsigned int codepoint = 0;
                if (!utf8_decode_one(text, std::strlen(text), consumed,
                                     codepoint) ||
                    text[consumed] != '\0') {
                    yyerror("char() expects exactly one UTF-8 character");
                } else {
                    ret = newcharval(codepoint);
                }
            } else if (arg->type == T_bytes) {
                const std::string &data =
                    ((struct bytes_value_t *)arg)->value;
                size_t consumed = 0;
                unsigned int codepoint = 0;
                if (!utf8_decode_one(data.data(), data.size(), consumed,
                                     codepoint) ||
                    consumed != data.size()) {
                    yyerror("char() expects exactly one UTF-8 character");
                } else {
                    ret = newcharval(codepoint);
                }
            } else {
                yyerror("char() expects int, char, string, or bytes, got %s",
                        typename_of(arg));
            }
            break;
        case B_byte:
            if (arg->type == T_byte) {
                ret = dupval(arg);
            } else if (arg->type == T_int) {
                long long value = ((struct int_value_t *)arg)->value;
                if (value < 0 || value > 255) {
                    yyerror("byte() value out of range");
                } else {
                    ret = newbyteval((unsigned int)value);
                }
            } else {
                yyerror("byte() expects int or byte, got %s",
                        typename_of(arg));
            }
            break;
        case B_command: {
            if (arg->type != T_string) {
                yyerror("command expects a string, got %s", typename_of(arg));
            } else {
                std::vector<std::pair<std::string, std::string>> env;
                if (!collect_command_env(args, 1, "command", env))
                    break;
                std::string output;
                int status = 0;
                if (!platform_run_command(
                        ((struct string_value_t *)arg)->value, env, output,
                        status))
                    yyerror("command failed to start");
                else
                    ret = newstrval(output.c_str());
            }
            break;
        }
        case B_command_status: {
            if (arg->type != T_string) {
                yyerror("command_status expects a string, got %s",
                        typename_of(arg));
            } else {
                std::vector<std::pair<std::string, std::string>> env;
                if (!collect_command_env(args, 1, "command_status", env))
                    break;
                int status = 0;
                if (!platform_run_command_status(
                        ((struct string_value_t *)arg)->value, env, status))
                    yyerror("command_status failed to start");
                else
                    ret = newintval(status);
            }
            break;
        }
        case B_read_text: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("std.fs.read_text expects a string path and "
                        "encoding, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            std::string errors = "strict";
            if (args.size() == 3) {
                if (args[2]->type != T_string) {
                    yyerror("std.fs.read_text expects an errors string, got %s",
                            typename_of(args[2]));
                    break;
                }
                errors = ((struct string_value_t *)args[2])->value;
            }
            std::string data;
            std::string error;
            if (!platform_read_file(
                    ((struct string_value_t *)args[0])->value, data, error)) {
                yyerror("std.fs.read_text: %s", error.c_str());
                break;
            }
            std::string text;
            if (!decode_text(
                    data, ((struct string_value_t *)args[1])->value, errors,
                    text, error)) {
                yyerror("std.fs.read_text: %s", error.c_str());
                break;
            }
            ret = newstrval(text.c_str());
            break;
        }
        case B_write_text: {
            if (args[0]->type != T_string || args[1]->type != T_string ||
                args[2]->type != T_string) {
                yyerror("std.fs.write_text expects a string path, text and "
                        "encoding, got %s, %s and %s",
                        typename_of(args[0]), typename_of(args[1]),
                        typename_of(args[2]));
                break;
            }
            std::string errors = "strict";
            if (args.size() == 4) {
                if (args[3]->type != T_string) {
                    yyerror("std.fs.write_text expects an errors string, got %s",
                            typename_of(args[3]));
                    break;
                }
                errors = ((struct string_value_t *)args[3])->value;
            }
            std::string data;
            std::string error;
            if (!encode_text(
                    ((struct string_value_t *)args[1])->value,
                    ((struct string_value_t *)args[2])->value, errors, data,
                    error)) {
                yyerror("std.fs.write_text: %s", error.c_str());
                break;
            }
            if (!platform_write_file(
                    ((struct string_value_t *)args[0])->value, data, false,
                    error)) {
                yyerror("std.fs.write_text: %s", error.c_str());
                break;
            }
            ret = newnull();
            break;
        }
        case B_read_file: {
            if (arg->type != T_string) {
                yyerror("read_file expects a string path, got %s",
                        typename_of(arg));
            } else {
                std::string data;
                std::string error;
                if (!platform_read_file(
                        ((struct string_value_t *)arg)->value, data, error))
                    yyerror("read_file: %s", error.c_str());
                else if (data.find('\0') != std::string::npos)
                    yyerror("read_file: file contains NUL byte");
                else
                    ret = newstrval(data.c_str());
            }
            break;
        }
        case B_write_file:
        case B_append_file: {
            const char *name =
                functype == B_write_file ? "write_file" : "append_file";
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("%s expects a string path and data, got %s and %s",
                        name, typename_of(args[0]), typename_of(args[1]));
            } else {
                std::string error;
                if (!platform_write_file(
                        ((struct string_value_t *)args[0])->value,
                        ((struct string_value_t *)args[1])->value,
                        functype == B_append_file, error))
                    yyerror("%s: %s", name, error.c_str());
                else
                    ret = newnull();
            }
            break;
        }
        case B_file_exists:
            if (arg->type != T_string) {
                yyerror("file_exists expects a string path, got %s",
                        typename_of(arg));
            } else {
                ret = newintval(platform_file_exists(
                                    ((struct string_value_t *)arg)->value)
                                    ? 1
                                    : 0);
            }
            break;
        case B_file_open: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("file_open expects a string path and mode, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            std::string mode_error;
            std::string mode = normalize_file_mode(
                ((struct string_value_t *)args[1])->value, mode_error);
            if (mode.empty()) {
                yyerror("file_open: %s", mode_error.c_str());
                break;
            }
            std::string error;
            const char *path = ((struct string_value_t *)args[0])->value;
            platform_file_ptr file = platform_file_open(path, mode, error);
            if (!file)
                yyerror("file_open: %s", error.c_str());
            else
                ret = new_file_value(file, path);
            break;
        }
        case B_file_read: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_read"))
                break;
            size_t limit = 0;
            if (args.size() == 2) {
                if (args[1]->type != T_int ||
                    ((struct int_value_t *)args[1])->value < 0) {
                    yyerror("file_read size must be a non-negative integer");
                    break;
                }
                limit = (size_t)((struct int_value_t *)args[1])->value;
            }
            std::string data;
            std::string error;
            if (!platform_file_read(file->file, limit, data, error)) {
                yyerror("file_read: %s", error.c_str());
            } else if (data.find('\0') != std::string::npos) {
                yyerror("file_read: file contains NUL byte");
            } else {
                ret = newstrval(data.c_str());
            }
            break;
        }
        case B_file_write: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_write"))
                break;
            if (args[1]->type != T_string) {
                yyerror("file_write expects string data, got %s",
                        typename_of(args[1]));
                break;
            }
            std::string error;
            if (!platform_file_write(
                    file->file,
                    ((struct string_value_t *)args[1])->value, error))
                yyerror("file_write: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_seek: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_seek"))
                break;
            if (args[1]->type != T_int) {
                yyerror("file_seek offset must be an integer");
                break;
            }
            int whence = 0;
            if (args.size() == 3) {
                if (args[2]->type != T_int) {
                    yyerror("file_seek whence must be an integer");
                    break;
                }
                long long value = ((struct int_value_t *)args[2])->value;
                if (value < 0 || value > 2) {
                    yyerror("file_seek whence must be 0, 1, or 2");
                    break;
                }
                whence = (int)value;
            }
            std::string error;
            if (!platform_file_seek(
                    file->file,
                    ((struct int_value_t *)args[1])->value, whence, error))
                yyerror("file_seek: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_tell: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_tell"))
                break;
            std::string error;
            long long position = platform_file_tell(file->file, error);
            if (position < 0)
                yyerror("file_tell: %s", error.c_str());
            else
                ret = newintval(position);
            break;
        }
        case B_file_eof: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_eof"))
                break;
            if (!platform_file_is_open(file->file)) {
                yyerror("file_eof: file is closed");
                break;
            }
            ret = newintval(platform_file_eof(file->file) ? 1 : 0);
            break;
        }
        case B_file_close: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_close"))
                break;
            std::string error;
            platform_file_close(file->file, error);
            if (!error.empty())
                yyerror("file_close: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_flush: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_flush"))
                break;
            std::string error;
            if (!platform_file_flush(file->file, error))
                yyerror("file_flush: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_input: {
            const char *prompt = NULL;
            if (!args.empty()) {
                if (args[0]->type != T_string) {
                    yyerror("input prompt must be a string, got %s",
                            typename_of(args[0]));
                    break;
                }
                prompt = ((struct string_value_t *)args[0])->value;
            }
            std::string line;
            bool at_eof = false;
            if (!read_console_line(prompt, line, at_eof))
                break;
            if (at_eof && line.empty()) {
                ret = newnull();
            } else if (line.find('\0') != std::string::npos) {
                yyerror("input: line contains NUL byte");
            } else {
                ret = newstrval(line.c_str());
            }
            break;
        }
        case B_split: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("split expects string text and separator, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *separator = ((struct string_value_t *)args[1])->value;
            if (!separator[0]) {
                yyerror("split separator must not be empty");
                break;
            }
            std::string input = text;
            std::string sep = separator;
            struct list_value_t *parts = newlistval();
            size_t position = 0;
            for (;;) {
                size_t found = input.find(sep, position);
                if (found == std::string::npos) {
                    parts->payload->items.push_back(
                        newstrval(input.substr(position).c_str()));
                    break;
                }
                parts->payload->items.push_back(
                    newstrval(input.substr(position, found - position).c_str()));
                position = found + sep.size();
            }
            ret = (struct value_t *)parts;
            break;
        }
        case B_split_ws: {
            if (arg->type != T_string) {
                yyerror("split_ws expects a string, got %s", typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            struct list_value_t *parts = newlistval();
            const char *p = text;
            while (*p) {
                while (*p && std::isspace((unsigned char)*p))
                    ++p;
                if (!*p)
                    break;
                const char *start = p;
                while (*p && !std::isspace((unsigned char)*p))
                    ++p;
                std::string part(start, (size_t)(p - start));
                parts->payload->items.push_back(newstrval(part.c_str()));
            }
            ret = (struct value_t *)parts;
            break;
        }
        case B_trim: {
            if (arg->type != T_string) {
                yyerror("trim expects a string, got %s", typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            const char *begin = text;
            const char *end = text + std::strlen(text);
            while (begin < end && std::isspace((unsigned char)*begin))
                ++begin;
            while (end > begin && std::isspace((unsigned char)end[-1]))
                --end;
            ret = newstrval(std::string(begin, (size_t)(end - begin)).c_str());
            break;
        }
        case B_find: {
            if (args[0]->type != T_string || args[1]->type != T_string) {
                yyerror("find expects string text and substring, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            const char *text = ((struct string_value_t *)args[0])->value;
            const char *needle = ((struct string_value_t *)args[1])->value;
            const char *found = std::strstr(text, needle);
            if (!found) {
                ret = newintval(-1);
            } else {
                ret = newintval((long long)utf8_length_prefix(
                    text, (size_t)(found - text)));
            }
            break;
        }
        case B_substr: {
            if (arg->type != T_string) {
                yyerror("substr expects a string, got %s", typename_of(arg));
                break;
            }
            if (args[1]->type != T_int ||
                ((struct int_value_t *)args[1])->value < 0) {
                yyerror("substr start must be a non-negative integer");
                break;
            }
            size_t start = (size_t)((struct int_value_t *)args[1])->value;
            size_t count = std::string::npos;
            if (args.size() == 3) {
                if (args[2]->type != T_int ||
                    ((struct int_value_t *)args[2])->value < 0) {
                    yyerror("substr length must be a non-negative integer");
                    break;
                }
                count = (size_t)((struct int_value_t *)args[2])->value;
            }
            std::string part = utf8_substr(
                ((struct string_value_t *)arg)->value, start, count);
            ret = newstrval(part.c_str());
            break;
        }
        case B_bytes_from_string: {
            if (arg->type != T_string) {
                yyerror("bytes_from_string expects a string, got %s",
                        typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            ret = newbytesval(std::string(text, std::strlen(text)));
            break;
        }
        case B_bytes_to_string: {
            if (arg->type != T_bytes) {
                yyerror("bytes_to_string expects bytes, got %s",
                        typename_of(arg));
                break;
            }
            const std::string &data = ((struct bytes_value_t *)arg)->value;
            if (data.find('\0') != std::string::npos) {
                yyerror("bytes_to_string: bytes contain NUL byte");
                break;
            }
            if (!is_valid_utf8(data)) {
                yyerror("bytes_to_string: bytes are not valid UTF-8");
                break;
            }
            ret = newstrval(data.c_str());
            break;
        }
        case B_bytes_from_char: {
            if (arg->type != T_char) {
                yyerror("bytes_from_char expects char, got %s",
                        typename_of(arg));
                break;
            }
            std::string data;
            if (!utf8_encode(((struct char_value_t *)arg)->value, data)) {
                yyerror("bytes_from_char: invalid char value");
                break;
            }
            ret = newbytesval(data);
            break;
        }
        case B_char_from_bytes: {
            if (arg->type != T_bytes) {
                yyerror("char_from_bytes expects bytes, got %s",
                        typename_of(arg));
                break;
            }
            const std::string &data = ((struct bytes_value_t *)arg)->value;
            size_t consumed = 0;
            unsigned int codepoint = 0;
            if (!utf8_decode_one(data.data(), data.size(), consumed,
                                 codepoint) ||
                consumed != data.size()) {
                yyerror("char_from_bytes expects exactly one UTF-8 character");
                break;
            }
            ret = newcharval(codepoint);
            break;
        }
        case B_bytes_len: {
            if (arg->type != T_bytes) {
                yyerror("bytes_len expects bytes, got %s", typename_of(arg));
                break;
            }
            ret = newintval((long long)((struct bytes_value_t *)arg)->value.size());
            break;
        }
        case B_bytes_get: {
            if (args[0]->type != T_bytes) {
                yyerror("bytes_get expects bytes, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int) {
                yyerror("bytes_get index must be an integer, got %s",
                        typename_of(args[1]));
                break;
            }
            const std::string &data = ((struct bytes_value_t *)args[0])->value;
            long long i = ((struct int_value_t *)args[1])->value;
            if (i < 0 || (unsigned long long)i >= data.size()) {
                yyerror("bytes_get index %lld out of range", i);
                break;
            }
            ret = newintval((unsigned char)data[(size_t)i]);
            break;
        }
        case B_bytes_slice: {
            if (arg->type != T_bytes) {
                yyerror("bytes_slice expects bytes, got %s", typename_of(arg));
                break;
            }
            if (args[1]->type != T_int ||
                ((struct int_value_t *)args[1])->value < 0) {
                yyerror("bytes_slice start must be a non-negative integer");
                break;
            }
            size_t start = (size_t)((struct int_value_t *)args[1])->value;
            size_t count = std::string::npos;
            if (args.size() == 3) {
                if (args[2]->type != T_int ||
                    ((struct int_value_t *)args[2])->value < 0) {
                    yyerror("bytes_slice length must be a non-negative integer");
                    break;
                }
                count = (size_t)((struct int_value_t *)args[2])->value;
            }
            const std::string &data = ((struct bytes_value_t *)arg)->value;
            if (start > data.size())
                start = data.size();
            if (count > data.size() - start)
                count = data.size() - start;
            ret = newbytesval(data.substr(start, count));
            break;
        }
        case B_bytes_concat: {
            if (args[0]->type != T_bytes || args[1]->type != T_bytes) {
                yyerror("bytes_concat expects two bytes values, got %s and %s",
                        typename_of(args[0]), typename_of(args[1]));
                break;
            }
            std::string data = ((struct bytes_value_t *)args[0])->value;
            data += ((struct bytes_value_t *)args[1])->value;
            ret = newbytesval(std::move(data));
            break;
        }
        case B_bytes_hex: {
            if (arg->type != T_bytes) {
                yyerror("bytes_hex expects bytes, got %s", typename_of(arg));
                break;
            }
            ret = newstrval(bytes_to_hex_string(
                ((struct bytes_value_t *)arg)->value).c_str());
            break;
        }
        case B_bytes_from_hex: {
            if (arg->type != T_string) {
                yyerror("bytes_from_hex expects a string, got %s",
                        typename_of(arg));
                break;
            }
            const char *text = ((struct string_value_t *)arg)->value;
            size_t len = std::strlen(text);
            if (len % 2 != 0) {
                yyerror("bytes_from_hex expects an even number of hex digits");
                break;
            }
            std::string data;
            data.reserve(len / 2);
            bool ok = true;
            for (size_t i = 0; i < len; i += 2) {
                int hi = hex_digit_value(text[i]);
                int lo = hex_digit_value(text[i + 1]);
                if (hi < 0 || lo < 0) {
                    yyerror("bytes_from_hex: invalid hex digit at position %zu",
                            i);
                    ok = false;
                    break;
                }
                data.push_back((char)(unsigned char)((hi << 4) | lo));
            }
            if (ok)
                ret = newbytesval(std::move(data));
            break;
        }
        case B_bytes_to_int: {
            if (args[0]->type != T_bytes) {
                yyerror("bytes_to_int expects bytes, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int) {
                yyerror("bytes_to_int size must be an integer");
                break;
            }
            long long size = ((struct int_value_t *)args[1])->value;
            if (size < 1 || size > 8) {
                yyerror("bytes_to_int size must be between 1 and 8");
                break;
            }
            if (args[2]->type != T_string) {
                yyerror("bytes_to_int byteorder must be a string");
                break;
            }
            bool little = false;
            if (!parse_byteorder(((struct string_value_t *)args[2])->value,
                                 little)) {
                yyerror("bytes_to_int byteorder must be \"little\" or \"big\"");
                break;
            }
            long long offset = 0;
            if (args.size() >= 4) {
                if (args[3]->type != T_int ||
                    ((struct int_value_t *)args[3])->value < 0) {
                    yyerror("bytes_to_int offset must be a non-negative integer");
                    break;
                }
                offset = ((struct int_value_t *)args[3])->value;
            }
            bool is_signed = args.size() == 5 && truthy(args[4]);
            const std::string &data = ((struct bytes_value_t *)args[0])->value;
            size_t start = (size_t)offset;
            size_t count = (size_t)size;
            if (start > data.size() || count > data.size() - start) {
                yyerror("bytes_to_int: need %lld bytes at offset %lld, got %zu",
                        size, offset, data.size());
                break;
            }
            unsigned long long value = 0;
            for (size_t i = 0; i < count; ++i) {
                size_t pos = little ? start + count - 1 - i : start + i;
                value = (value << 8) | (unsigned char)data[pos];
            }
            if (is_signed) {
                if (count < 8) {
                    unsigned long long sign_bit = 1ULL << (count * 8 - 1);
                    if (value & sign_bit)
                        value -= (1ULL << (count * 8));
                }
                ret = newintval((long long)value);
            } else if (value > 0x7FFFFFFFFFFFFFFFULL) {
                yyerror("bytes_to_int: unsigned 64-bit value is too large");
            } else {
                ret = newintval((long long)value);
            }
            break;
        }
        case B_int_to_bytes: {
            if (args[0]->type != T_int) {
                yyerror("int_to_bytes expects an integer value, got %s",
                        typename_of(args[0]));
                break;
            }
            if (args[1]->type != T_int) {
                yyerror("int_to_bytes size must be an integer");
                break;
            }
            long long size = ((struct int_value_t *)args[1])->value;
            if (size < 1 || size > 8) {
                yyerror("int_to_bytes size must be between 1 and 8");
                break;
            }
            if (args[2]->type != T_string) {
                yyerror("int_to_bytes byteorder must be a string");
                break;
            }
            bool little = false;
            if (!parse_byteorder(((struct string_value_t *)args[2])->value,
                                 little)) {
                yyerror("int_to_bytes byteorder must be \"little\" or \"big\"");
                break;
            }
            long long value = ((struct int_value_t *)args[0])->value;
            if (size < 8) {
                unsigned bits = (unsigned)(size * 8);
                long long minimum = -(1LL << (bits - 1));
                long long maximum = (1LL << bits) - 1;
                if (value < minimum || value > maximum) {
                    yyerror("int_to_bytes value %lld does not fit in %lld bytes",
                            value, size);
                    break;
                }
            }
            unsigned long long bits = (unsigned long long)value;
            std::string data((size_t)size, '\0');
            for (long long i = 0; i < size; ++i) {
                unsigned char byte = (unsigned char)(bits & 0xFF);
                size_t pos = little ? (size_t)i : (size_t)(size - 1 - i);
                data[pos] = (char)byte;
                bits >>= 8;
            }
            ret = newbytesval(std::move(data));
            break;
        }
        case B_read_bytes: {
            if (arg->type != T_string) {
                yyerror("read_bytes expects a string path, got %s",
                        typename_of(arg));
                break;
            }
            std::string data;
            std::string error;
            if (!platform_read_file(
                    ((struct string_value_t *)arg)->value, data, error))
                yyerror("read_bytes: %s", error.c_str());
            else
                ret = newbytesval(std::move(data));
            break;
        }
        case B_write_bytes:
        case B_append_bytes: {
            const char *name =
                functype == B_write_bytes ? "write_bytes" : "append_bytes";
            if (args[0]->type != T_string || args[1]->type != T_bytes) {
                yyerror("%s expects a string path and bytes, got %s and %s",
                        name, typename_of(args[0]), typename_of(args[1]));
                break;
            }
            std::string error;
            if (!platform_write_file(
                    ((struct string_value_t *)args[0])->value,
                    ((struct bytes_value_t *)args[1])->value,
                    functype == B_append_bytes, error))
                yyerror("%s: %s", name, error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_file_read_bytes: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_read_bytes"))
                break;
            size_t limit = 0;
            if (args.size() == 2) {
                if (args[1]->type != T_int ||
                    ((struct int_value_t *)args[1])->value < 0) {
                    yyerror("file_read_bytes size must be a non-negative integer");
                    break;
                }
                limit = (size_t)((struct int_value_t *)args[1])->value;
            }
            std::string data;
            std::string error;
            if (!platform_file_read(file->file, limit, data, error))
                yyerror("file_read_bytes: %s", error.c_str());
            else
                ret = newbytesval(std::move(data));
            break;
        }
        case B_file_write_bytes: {
            std::shared_ptr<file_resource> file;
            if (!get_file_resource(arg, file, "file_write_bytes"))
                break;
            if (args[1]->type != T_bytes) {
                yyerror("file_write_bytes expects bytes data, got %s",
                        typename_of(args[1]));
                break;
            }
            std::string error;
            if (!platform_file_write(
                    file->file, ((struct bytes_value_t *)args[1])->value,
                    error))
                yyerror("file_write_bytes: %s", error.c_str());
            else
                ret = newnull();
            break;
        }
        case B_sqrt:
        case B_exp:
        case B_log:
            if (!is_numeric(arg)) {
                yyerror("math function expects a number, got %s", typename_of(arg));
                break;
            }
            if (functype == B_sqrt) {
                double value = as_double(arg);
                ret = newfloatval(value < 0.0 ? std::nan("")
                                             : std::sqrt(value));
            } else if (functype == B_exp) {
                ret = newfloatval(std::exp(as_double(arg)));
            } else {
                double value = as_double(arg);
                ret = newfloatval(value <= 0.0 ? std::nan("")
                                              : std::log(value));
            }
            break;
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}

