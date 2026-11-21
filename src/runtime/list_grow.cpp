#include "list_grow.h"

#include "valuetype.h"

#include <memory>
#include <vector>

bool list_grow_append(struct value_t *list, struct value_t **args,
                      uint32_t argc) {
    if (!list || list->type != T_list)
        return false;
    struct list_value_t *target = (struct list_value_t *)list;
    if (!target->payload || target->payload->frozen)
        return false;
    if (target->payload.use_count() <= 2) {
        for (uint32_t i = 0; i < argc; ++i) {
            target->payload->items.push_back(args[i]);
            args[i] = NULL;
        }
        return true;
    }
    auto copy = std::make_shared<struct list_payload>();
    copy->items.reserve(target->payload->items.size() + argc);
    for (struct value_t *item : target->payload->items)
        copy->items.push_back(dupval(item));
    for (uint32_t i = 0; i < argc; ++i) {
        copy->items.push_back(args[i]);
        args[i] = NULL;
    }
    target->payload = copy;
    return true;
}

bool list_grow_concat(struct value_t *left, struct value_t *right) {
    if (!left || !right || left->type != T_list || right->type != T_list)
        return false;
    struct list_value_t *target = (struct list_value_t *)left;
    struct list_value_t *extra = (struct list_value_t *)right;
    if (!target->payload || !extra->payload)
        return false;
    if (target->payload->frozen)
        return false;
    /* A freshly built literal is uniquely owned and its items can be moved.
     * A variable operand is shared with its symbol and must be copied. */
    const bool take_extra =
        extra->payload.use_count() == 1 && !extra->payload->frozen;
    if (target->payload.use_count() <= 2) {
        for (struct value_t *item : extra->payload->items)
            target->payload->items.push_back(take_extra ? item
                                                       : dupval(item));
    } else {
        auto copy = std::make_shared<struct list_payload>();
        copy->items.reserve(target->payload->items.size() +
                            extra->payload->items.size());
        for (struct value_t *item : target->payload->items)
            copy->items.push_back(dupval(item));
        for (struct value_t *item : extra->payload->items)
            copy->items.push_back(take_extra ? item : dupval(item));
        target->payload = copy;
    }
    if (take_extra)
        extra->payload->items.clear();
    return true;
}
