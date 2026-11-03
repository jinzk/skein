#include "cycle.h"

#include "structtype.h"

const void *value_payload(const struct value_t *v) {
    if (!v)
        return NULL;
    switch (v->type) {
        case T_list:
            return ((const struct list_value_t *)v)->payload.get();
        case T_dict:
            return ((const struct dict_value_t *)v)->payload.get();
        case T_struct:
            return ((const struct struct_value_t *)v)->payload.get();
        case T_enum:
            return ((const struct enum_value_t *)v)->payload.get();
        case T_lru:
            return ((const struct lru_value_t *)v)->payload.get();
        default:
            return NULL;
    }
}

void for_each_value_child(
    const struct value_t *v,
    const std::function<void(const struct value_t *)> &fn) {
    if (!v)
        return;
    switch (v->type) {
        case T_list: {
            const struct list_value_t *list =
                (const struct list_value_t *)v;
            for (struct value_t *item : list->payload->items)
                fn(item);
            break;
        }
        case T_dict: {
            const struct dict_value_t *dict =
                (const struct dict_value_t *)v;
            for (const auto &entry : dict->payload->items) {
                fn(entry.first);
                fn(entry.second);
            }
            break;
        }
        case T_struct: {
            const struct struct_value_t *sv =
                (const struct struct_value_t *)v;
            for (struct value_t *field : sv->payload->fields)
                fn(field);
            break;
        }
        case T_enum: {
            const struct enum_value_t *ev = (const struct enum_value_t *)v;
            for (struct value_t *field : ev->payload->fields)
                fn(field);
            break;
        }
        case T_lru: {
            const struct lru_value_t *lv = (const struct lru_value_t *)v;
            for (const auto &entry : lv->payload->items) {
                fn(entry.first);
                fn(entry.second);
            }
            break;
        }
        case T_result: {
            const struct result_value_t *rv =
                (const struct result_value_t *)v;
            fn(rv->value);
            fn(rv->error);
            break;
        }
        case T_method: {
            const struct method_value_t *method =
                (const struct method_value_t *)v;
            if (method->receiver) {
                for (struct value_t *field : method->receiver->fields)
                    fn(field);
            }
            break;
        }
        default:
            break;
    }
}

bool value_reaches(const struct value_t *v, const void *target,
                   std::unordered_set<const void *> &visited) {
    if (!v)
        return false;
    const void *payload = value_payload(v);
    if (payload) {
        if (payload == target)
            return true;
        if (!visited.insert(payload).second)
            return false;
    }

    bool found = false;
    for_each_value_child(v, [&](const struct value_t *child) {
        if (!found && value_reaches(child, target, visited))
            found = true;
    });
    return found;
}

bool value_reaches_any(const struct value_t *v,
                       const std::vector<const void *> &targets) {
    for (const void *target : targets) {
        if (!target)
            continue;
        std::unordered_set<const void *> visited;
        if (value_reaches(v, target, visited))
            return true;
    }
    return false;
}
