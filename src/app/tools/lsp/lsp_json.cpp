#include "lsp_json.h"

#include <cstring>

#include "dict.h"
#include "json.h"
#include "valuetype.h"

namespace skein {
namespace lsp {
namespace json {

const struct value_t *member(const struct value_t *object, const char *key) {
    if (!object || object->type != T_dict || !key)
        return nullptr;
    const struct dict_value_t *dict = (const struct dict_value_t *)object;
    for (const auto &item : dict->payload->items) {
        if (item.first && item.first->type == T_string) {
            const char *text = ((const struct string_value_t *)item.first)->value;
            if (text && std::strcmp(text, key) == 0)
                return item.second;
        }
    }
    return nullptr;
}

std::string string_value(const struct value_t *value,
                         const std::string &fallback) {
    if (!value || value->type != T_string)
        return fallback;
    const char *text = ((const struct string_value_t *)value)->value;
    return text ? text : fallback;
}

long long int_value(const struct value_t *value, long long fallback) {
    if (!value)
        return fallback;
    if (value->type == T_int)
        return ((const struct int_value_t *)value)->value;
    if (value->type == T_float)
        return (long long)((const struct float_value_t *)value)->value;
    return fallback;
}

bool bool_value(const struct value_t *value, bool fallback) {
    if (!value || value->type != T_bool)
        return fallback;
    return ((const struct bool_value_t *)value)->value;
}

struct value_t *object() { return (struct value_t *)newdictval(); }

struct value_t *array() { return (struct value_t *)newlistval(); }

struct value_t *string(const std::string &text) {
    return newstrval(text.c_str());
}

struct value_t *integer(long long value) { return newintval(value); }

struct value_t *boolean(bool value) { return newboolval(value); }

struct value_t *null() { return newnull(); }

void set(struct value_t *target, const char *key, struct value_t *value) {
    if (!target || target->type != T_dict || !key) {
        freeval(value);
        return;
    }
    dict_append_item((struct dict_value_t *)target, newstrval(key), value);
}

void push(struct value_t *target, struct value_t *value) {
    if (!target || target->type != T_list) {
        freeval(value);
        return;
    }
    ((struct list_value_t *)target)->payload->items.push_back(value);
}

std::string stringify(const struct value_t *value, std::string &error) {
    return json_stringify_text(value, error);
}

} // namespace json
} // namespace lsp
} // namespace skein
