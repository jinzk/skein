#include "builtins_internal.h"

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


struct value_t *call_heap_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    struct value_t *arg = args.empty() ? NULL : args[0];
    struct value_t *ret = NULL;
    switch (functype) {
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
        default:
            yyerror("unknown built-in function %d", functype);
            break;
    }
    return ret;
}
