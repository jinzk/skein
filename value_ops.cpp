#include "valuetype.h"
#include "value_ops.h"
#include "dict.h"
#include "structtype.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

bool is_numeric(const struct value_t *v) {
    return v && (v->type == T_int || v->type == T_float);
}

double as_double(const struct value_t *v) {
    if (!v) return 0.0;
    if (v->type == T_int) return (double)((struct int_value_t *)v)->value;
    if (v->type == T_float) return ((struct float_value_t *)v)->value;
    return 0.0;
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
        case T_native: return true;
        case T_userdata: return true;
        case T_result: return ((struct result_value_t *)v)->ok;
        case T_error: return true;
        case T_null: return false;
        default: return false;
    }
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
        case T_native: {
            struct native_function_value *fa =
                (struct native_function_value *)a;
            struct native_function_value *fb =
                (struct native_function_value *)b;
            return fa->owner.get() == fb->owner.get();
        }
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
