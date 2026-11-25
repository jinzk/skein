#include "string_grow.h"

#include "unicode.h"
#include "value_ops.h"
#include "valuetype.h"

#include <memory>
#include <string>

bool string_append_in_place(struct value_t *target,
                            const struct value_t *rhs,
                            size_t extra_owners) {
    if (!target || target->type != T_string || !rhs)
        return false;
    std::string addition;
    if (rhs->type == T_string) {
        addition.assign(((const struct string_value_t *)rhs)->value);
    } else if (rhs->type == T_char) {
        if (!utf8_encode(((const struct char_value_t *)rhs)->value,
                         addition))
            return false;
    } else {
        return false;
    }
    struct string_value_t *text = (struct string_value_t *)target;
    if (!text->storage)
        text->storage = std::make_shared<string_storage>(
            std::string(text->value ? text->value : ""));
    /* The caller holds one reference (the target variable), plus the
     * references it declared through `extra_owners`. */
    if (text->storage.use_count() >
        static_cast<long>(extra_owners) + 1)
        text->storage = std::make_shared<string_storage>(*text->storage);
    text->storage->data.append(addition);
    text->value = text->storage->data.data();
    string_index_invalidate(text);
    return true;
}

void string_refresh_cache(struct value_t *old_value,
                          const struct value_t *new_value) {
    if (!old_value || !new_value || old_value == new_value)
        return;
    if (old_value->type != T_string || new_value->type != T_string)
        return;
    struct string_value_t *old_text = (struct string_value_t *)old_value;
    const struct string_value_t *new_text =
        (const struct string_value_t *)new_value;
    if (old_text->storage && old_text->storage == new_text->storage)
        old_text->value = new_text->value;
}
