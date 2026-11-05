#include "builtins_internal.h"

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

std::shared_ptr<deque_payload> deque_push_back_payload(
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


struct value_t *call_deque_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
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
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
