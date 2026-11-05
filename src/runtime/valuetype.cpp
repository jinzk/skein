#include "valuetype.h"
#include "dict.h"
#include "value_ops.h"
#include "structtype.h"
#include "unicode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
thread_local int render_depth = 0;
struct render_depth_guard {
    render_depth_guard() { ++render_depth; }
    ~render_depth_guard() { --render_depth; }
};

thread_local int free_depth = 0;
struct free_depth_guard {
    free_depth_guard() { ++free_depth; }
    ~free_depth_guard() { --free_depth; }
};
} // namespace


static char *dupstr(const char *s) {
    if (!s) s = "";
    size_t len = std::strlen(s) + 1;
    char *p = (char *)std::malloc(len);
    if (!p) {
        std::fprintf(stderr, "out of space\n");
        std::exit(1);
    }
    std::memcpy(p, s, len);
    return p;
}

struct value_t *newreturn() {
    struct value_t *v = new value_t;
    v->type = T_return;
    return v;
}

struct value_t *newbreak(const char *label) {
    struct control_value_t *v = new control_value_t;
    v->type = T_break;
    v->label = label ? dupstr(label) : NULL;
    return (struct value_t *)v;
}

struct value_t *newcontinue(const char *label) {
    struct control_value_t *v = new control_value_t;
    v->type = T_continue;
    v->label = label ? dupstr(label) : NULL;
    return (struct value_t *)v;
}

namespace {

constexpr long long SMALL_INT_MIN = -512;
constexpr long long SMALL_INT_MAX = 4096;
constexpr size_t SMALL_INT_COUNT =
    (size_t)(SMALL_INT_MAX - SMALL_INT_MIN + 1);

struct small_int_table {
    int_value_t values[SMALL_INT_COUNT];
    small_int_table() {
        for (size_t i = 0; i < SMALL_INT_COUNT; ++i) {
            values[i].type = T_int;
            values[i].value = SMALL_INT_MIN + (long long)i;
            values[i].interned = true;
        }
    }
};

small_int_table &small_ints() {
    static small_int_table table;
    return table;
}

bool_value_t &bool_singleton(bool value) {
    static bool_value_t false_value{T_bool, false};
    static bool_value_t true_value{T_bool, true};
    return value ? true_value : false_value;
}

value_t &null_singleton() {
    static value_t null_value{T_null};
    return null_value;
}

} // namespace

struct value_t *newintval(long long i) {
    if (i >= SMALL_INT_MIN && i <= SMALL_INT_MAX)
        return (struct value_t *)&small_ints().values[i - SMALL_INT_MIN];
    struct int_value_t *v = new int_value_t;
    v->type = T_int;
    v->value = i;
    return (struct value_t *)v;
}

struct value_t *newfloatval(double f) {
    struct float_value_t *v = new float_value_t;
    v->type = T_float;
    v->value = f;
    return (struct value_t *)v;
}

struct value_t *newboolval(bool b) {
    return (struct value_t *)&bool_singleton(b);
}

struct value_t *newstrval(const char *s) {
    struct string_value_t *v = new string_value_t;
    v->type = T_string;
    v->value = dupstr(s);
    return (struct value_t *)v;
}

struct value_t *newcharval(unsigned int codepoint) {
    struct char_value_t *v = new char_value_t;
    v->type = T_char;
    v->value = codepoint;
    return (struct value_t *)v;
}

struct value_t *newbyteval(unsigned int value) {
    struct byte_value_t *v = new byte_value_t;
    v->type = T_byte;
    v->value = value & 0xFF;
    return (struct value_t *)v;
}

struct value_t *newbytesval(std::string data) {
    struct bytes_value_t *v = new bytes_value_t;
    v->type = T_bytes;
    v->value = std::move(data);
    return (struct value_t *)v;
}

native_function_value::~native_function_value() {
    for (struct value_t *child : children)
        freeval(child);
}

struct value_t *newnativeval(
    std::function<struct value_t *(
        const std::vector<struct value_t *> &args)> call,
    std::shared_ptr<void> owner) {
    native_function_value *v = new native_function_value;
    v->type = T_native;
    v->call = std::move(call);
    v->owner = std::move(owner);
    return (struct value_t *)v;
}

struct value_t *newbuiltinval(int functype) {
    struct builtin_value_t *v = new builtin_value_t;
    v->type = T_builtin;
    v->functype = functype;
    return (struct value_t *)v;
}

struct value_t *newuserdataval(const struct native_type *meta,
                               std::shared_ptr<void> data) {
    struct userdata_value_t *v = new userdata_value_t;
    v->type = T_userdata;
    v->meta = meta;
    v->data = std::move(data);
    return (struct value_t *)v;
}

struct value_t *newerrorval(const std::string &kind,
                            const std::string &message) {
    struct error_value_t *v = new error_value_t;
    v->type = T_error;
    v->kind = kind;
    v->message = message;
    return (struct value_t *)v;
}

struct value_t *newresultok(struct value_t *value) {
    struct result_value_t *v = new result_value_t;
    v->type = T_result;
    v->ok = true;
    v->value = value;
    v->error = NULL;
    return (struct value_t *)v;
}

struct value_t *newresulterr(struct value_t *error) {
    struct result_value_t *v = new result_value_t;
    v->type = T_result;
    v->ok = false;
    v->value = NULL;
    v->error = error;
    return (struct value_t *)v;
}

struct value_t *newresulterr_message(const std::string &kind,
                                     const std::string &message) {
    return newresulterr(newerrorval(kind, message));
}

struct value_t *normalize_error(struct value_t *value,
                                const char *default_kind) {
    if (value && value->type == T_error)
        return value;
    std::string message = value ? value_to_string(value) : "null";
    freeval(value);
    return newerrorval(default_kind ? default_kind : "error", message);
}

const char *error_kind_of(const struct value_t *value) {
    if (!value || value->type != T_error)
        return "";
    return ((const struct error_value_t *)value)->kind.c_str();
}

std::string error_message_of(const struct value_t *value) {
    if (!value || value->type != T_error)
        return value ? value_to_string(value) : "";
    return ((const struct error_value_t *)value)->message;
}

struct value_t *newnull() {
    return &null_singleton();
}

struct value_t *newfuncval(std::shared_ptr<function_code> code,
                           std::shared_ptr<struct scope_t> closure,
                           std::vector<struct symbol *> upvalues) {
    struct func_value_t *v = new func_value_t;
    v->type = T_func;
    v->def = std::make_shared<function_def>();
    v->def->code = code;
    v->def->closure = closure;
    v->def->upvalues = std::move(upvalues);
    return (struct value_t *)v;
}

struct list_value_t *newlistval() {
    struct list_value_t *v = new list_value_t;
    v->type = T_list;
    v->payload = std::make_shared<list_payload>();
    return v;
}

struct dict_value_t *newdictval() {
    struct dict_value_t *v = new dict_value_t;
    v->type = T_dict;
    v->payload = std::make_shared<dict_payload>();
    return v;
}

struct lru_value_t *newlruval(std::size_t capacity) {
    struct lru_value_t *v = new lru_value_t;
    v->type = T_lru;
    v->payload = std::make_shared<lru_payload>();
    v->payload->capacity = capacity;
    return v;
}

struct deque_value_t *newdequeval() {
    struct deque_value_t *v = new deque_value_t;
    v->type = T_deque;
    v->payload = std::make_shared<deque_payload>();
    return v;
}

struct heap_value_t *newheapval(struct value_t *less) {
    struct heap_value_t *v = new heap_value_t;
    v->type = T_heap;
    v->payload = std::make_shared<heap_payload>();
    v->payload->less = less;
    return v;
}

struct set_value_t *newsetval() {
    struct set_value_t *v = new set_value_t;
    v->type = T_set;
    v->payload = std::make_shared<set_payload>();
    v->payload->order = std::make_shared<deque_payload>();
    return v;
}

list_payload::~list_payload() {
    for (size_t i = 0; i < items.size(); ++i)
        freeval(items[i]);
}

dict_payload::~dict_payload() {
    for (size_t i = 0; i < items.size(); ++i) {
        freeval(items[i].first);
        freeval(items[i].second);
    }
}

lru_payload::~lru_payload() {
    for (auto &entry : items) {
        freeval(entry.first);
        freeval(entry.second);
    }
}

deque_node::~deque_node() {
    freeval(value);
}

namespace {

/*
 * Release a chain iteratively: a long deque would otherwise recurse through
 * shared_ptr destructors and overflow the C++ stack.
 */
void release_deque_chain(std::shared_ptr<deque_node> &head) {
    while (head) {
        if (head.use_count() != 1) {
            head.reset();
            return;
        }
        std::shared_ptr<deque_node> next = std::move(head->next);
        head.reset();
        head = std::move(next);
    }
}

} // namespace

deque_payload::~deque_payload() {
    /* Drop the tail references first so the last node becomes unique. */
    front_tail.reset();
    back_tail.reset();
    release_deque_chain(front);
    release_deque_chain(back);
}

void deque_items_in_order(const struct deque_payload *payload,
                          std::vector<value_t *> &out) {
    out.clear();
    if (!payload)
        return;
    out.reserve(payload->front_size + payload->back_size);
    for (std::shared_ptr<deque_node> node = payload->front; node;
         node = node->next)
        out.push_back(node->value);
    std::vector<value_t *> reversed_back;
    reversed_back.reserve(payload->back_size);
    for (std::shared_ptr<deque_node> node = payload->back; node;
         node = node->next)
        reversed_back.push_back(node->value);
    for (std::size_t i = reversed_back.size(); i > 0; --i)
        out.push_back(reversed_back[i - 1]);
}

heap_node::~heap_node() {
    freeval(value);
}

namespace {

/*
 * Release a heap tree iteratively.  A leftist heap bounds the right spine but
 * the left spine can still be linear, so recursive shared_ptr teardown is not
 * safe for large heaps.
 */
void release_heap_tree(std::shared_ptr<heap_node> root) {
    std::vector<std::shared_ptr<heap_node>> pending;
    if (root)
        pending.push_back(std::move(root));
    while (!pending.empty()) {
        std::shared_ptr<heap_node> node = std::move(pending.back());
        pending.pop_back();
        if (!node)
            continue;
        if (node.use_count() != 1)
            continue;
        if (node->left)
            pending.push_back(std::move(node->left));
        if (node->right)
            pending.push_back(std::move(node->right));
        node.reset();
    }
}

} // namespace

heap_payload::~heap_payload() {
    freeval(less);
    less = nullptr;
    release_heap_tree(std::move(root));
}

void heap_items_level_order(const struct heap_payload *payload,
                            std::vector<value_t *> &out) {
    out.clear();
    if (!payload || !payload->root)
        return;
    out.reserve(payload->size);
    std::vector<const heap_node *> queue;
    queue.push_back(payload->root.get());
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const heap_node *node = queue[i];
        out.push_back(node->value);
        if (node->left)
            queue.push_back(node->left.get());
        if (node->right)
            queue.push_back(node->right.get());
    }
}

set_bucket::~set_bucket() {
    for (value_t *key : keys)
        freeval(key);
}

namespace {

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

bool bucket_has(const set_bucket *bucket, const value_t *key) {
    if (!bucket)
        return false;
    for (value_t *entry : bucket->keys) {
        if (value_equals(entry, const_cast<value_t *>(key)))
            return true;
    }
    return false;
}

void release_set_tree(std::shared_ptr<set_node> root) {
    std::vector<std::shared_ptr<set_node>> pending;
    if (root)
        pending.push_back(std::move(root));
    while (!pending.empty()) {
        std::shared_ptr<set_node> node = std::move(pending.back());
        pending.pop_back();
        if (!node)
            continue;
        if (node.use_count() != 1)
            continue;
        if (node->left)
            pending.push_back(std::move(node->left));
        if (node->right)
            pending.push_back(std::move(node->right));
        node.reset();
    }
}

std::shared_ptr<set_node> make_set_node(std::uint64_t hash,
                                        const value_t *key) {
    std::shared_ptr<set_node> node = std::make_shared<set_node>();
    node->hash = hash;
    node->priority = splitmix64(hash);
    node->bucket = std::make_shared<set_bucket>();
    node->bucket->keys.push_back(dupval(const_cast<value_t *>(key)));
    return node;
}

std::shared_ptr<set_node> clone_set_node(
    const std::shared_ptr<set_node> &node) {
    std::shared_ptr<set_node> out = std::make_shared<set_node>();
    out->hash = node->hash;
    out->priority = node->priority;
    out->bucket = node->bucket;
    out->left = node->left;
    out->right = node->right;
    return out;
}

std::shared_ptr<set_node> set_rotate_right(std::shared_ptr<set_node> y) {
    std::shared_ptr<set_node> x = clone_set_node(y->left);
    std::shared_ptr<set_node> y2 = clone_set_node(y);
    y2->left = x->right;
    x->right = y2;
    return x;
}

std::shared_ptr<set_node> set_rotate_left(std::shared_ptr<set_node> x) {
    std::shared_ptr<set_node> y = clone_set_node(x->right);
    std::shared_ptr<set_node> x2 = clone_set_node(x);
    x2->right = y->left;
    y->left = x2;
    return y;
}

std::shared_ptr<set_node> set_merge(std::shared_ptr<set_node> a,
                                    std::shared_ptr<set_node> b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (a->priority < b->priority) {
        std::shared_ptr<set_node> out = clone_set_node(a);
        out->right = set_merge(a->right, b);
        return out;
    }
    std::shared_ptr<set_node> out = clone_set_node(b);
    out->left = set_merge(a, b->left);
    return out;
}

std::shared_ptr<set_node> set_insert_impl(
    const std::shared_ptr<set_node> &node, std::uint64_t hash,
    const value_t *key, bool &added) {
    if (!node) {
        added = true;
        return make_set_node(hash, key);
    }
    if (hash == node->hash) {
        if (bucket_has(node->bucket.get(), key)) {
            added = false;
            return node;
        }
        added = true;
        std::shared_ptr<set_node> out = clone_set_node(node);
        std::shared_ptr<set_bucket> bucket = std::make_shared<set_bucket>();
        bucket->keys.reserve(node->bucket->keys.size() + 1);
        for (value_t *entry : node->bucket->keys)
            bucket->keys.push_back(dupval(entry));
        bucket->keys.push_back(dupval(const_cast<value_t *>(key)));
        out->bucket = bucket;
        return out;
    }
    std::shared_ptr<set_node> out = clone_set_node(node);
    if (hash < node->hash) {
        out->left = set_insert_impl(node->left, hash, key, added);
        if (!added)
            return node;
        if (out->left->priority < out->priority)
            out = set_rotate_right(out);
    } else {
        out->right = set_insert_impl(node->right, hash, key, added);
        if (!added)
            return node;
        if (out->right->priority < out->priority)
            out = set_rotate_left(out);
    }
    return out;
}

std::shared_ptr<set_node> set_erase_impl(
    const std::shared_ptr<set_node> &node, std::uint64_t hash,
    const value_t *key, bool &removed) {
    if (!node) {
        removed = false;
        return node;
    }
    if (hash == node->hash) {
        std::size_t index = node->bucket->keys.size();
        for (std::size_t i = 0; i < node->bucket->keys.size(); ++i) {
            if (value_equals(node->bucket->keys[i],
                             const_cast<value_t *>(key))) {
                index = i;
                break;
            }
        }
        if (index >= node->bucket->keys.size()) {
            removed = false;
            return node;
        }
        removed = true;
        if (node->bucket->keys.size() == 1)
            return set_merge(node->left, node->right);
        std::shared_ptr<set_node> out = clone_set_node(node);
        std::shared_ptr<set_bucket> bucket = std::make_shared<set_bucket>();
        bucket->keys.reserve(node->bucket->keys.size() - 1);
        for (std::size_t i = 0; i < node->bucket->keys.size(); ++i) {
            if (i != index)
                bucket->keys.push_back(dupval(node->bucket->keys[i]));
        }
        out->bucket = bucket;
        return out;
    }
    std::shared_ptr<set_node> out = clone_set_node(node);
    if (hash < node->hash)
        out->left = set_erase_impl(node->left, hash, key, removed);
    else
        out->right = set_erase_impl(node->right, hash, key, removed);
    if (!removed)
        return node;
    return out;
}

} // namespace

set_payload::~set_payload() {
    release_set_tree(std::move(root));
    order.reset();
}

bool set_contains(const struct set_payload *payload,
                  const struct value_t *key) {
    if (!payload || !payload->root || !key || !dict_key_allowed(key))
        return false;
    std::uint64_t hash = dict_key_hash(key);
    for (std::shared_ptr<set_node> node = payload->root; node;) {
        if (hash == node->hash)
            return bucket_has(node->bucket.get(), key);
        node = hash < node->hash ? node->left : node->right;
    }
    return false;
}

std::shared_ptr<set_node> set_insert(const std::shared_ptr<set_node> &root,
                                     const struct value_t *key, bool &added) {
    if (!key || !dict_key_allowed(key)) {
        added = false;
        return root;
    }
    return set_insert_impl(root, dict_key_hash(key), key, added);
}

std::shared_ptr<set_node> set_erase(const std::shared_ptr<set_node> &root,
                                    const struct value_t *key,
                                    bool &removed) {
    if (!key || !dict_key_allowed(key)) {
        removed = false;
        return root;
    }
    return set_erase_impl(root, dict_key_hash(key), key, removed);
}

void set_values_in_order(const struct set_payload *payload,
                         std::vector<value_t *> &out) {
    out.clear();
    if (!payload)
        return;
    std::vector<value_t *> log;
    deque_items_in_order(payload->order.get(), log);
    std::vector<value_t *> latest;
    latest.reserve(payload->size);
    struct dict_value_t *seen = newdictval();
    for (std::size_t i = log.size(); i > 0; --i) {
        value_t *key = log[i - 1];
        if (!set_contains(payload, key))
            continue;
        if (dict_find_index(seen, key) >= 0)
            continue;
        dict_append_item(seen, dupval(key), newboolval(true));
        latest.push_back(key);
    }
    freeval((struct value_t *)seen);
    out.reserve(latest.size());
    for (std::size_t i = latest.size(); i > 0; --i)
        out.push_back(latest[i - 1]);
}

struct value_t *dupval(struct value_t *v) {
    if (!v) return NULL;
    switch (v->type) {
        case T_int:
            return newintval(((struct int_value_t *)v)->value);
        case T_float:
            return newfloatval(((struct float_value_t *)v)->value);
        case T_bool:
            return newboolval(((struct bool_value_t *)v)->value);
        case T_string:
            return newstrval(((struct string_value_t *)v)->value);
        case T_char:
            return newcharval(((struct char_value_t *)v)->value);
        case T_byte:
            return newbyteval(((struct byte_value_t *)v)->value);
        case T_bytes:
            return newbytesval(((struct bytes_value_t *)v)->value);
        case T_list: {
            struct list_value_t *src = (struct list_value_t *)v;
            struct list_value_t *dst = new list_value_t;
            dst->type = T_list;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_dict: {
            struct dict_value_t *src = (struct dict_value_t *)v;
            struct dict_value_t *dst = new dict_value_t;
            dst->type = T_dict;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_func: {
            struct func_value_t *src = (struct func_value_t *)v;
            struct func_value_t *dst = new func_value_t;
            dst->type = T_func;
            dst->def = src->def;
            return (struct value_t *)dst;
        }
        case T_builtin:
            return newbuiltinval(((struct builtin_value_t *)v)->functype);
        case T_native: {
            struct native_function_value *src =
                (struct native_function_value *)v;
            struct value_t *copy = newnativeval(src->call, src->owner);
            struct native_function_value *dst =
                (struct native_function_value *)copy;
            dst->children.reserve(src->children.size());
            for (struct value_t *child : src->children)
                dst->children.push_back(dupval(child));
            return copy;
        }
        case T_userdata: {
            struct userdata_value_t *src = (struct userdata_value_t *)v;
            return newuserdataval(src->meta, src->data);
        }
        case T_error: {
            struct error_value_t *src = (struct error_value_t *)v;
            return newerrorval(src->kind, src->message);
        }
        case T_result: {
            struct result_value_t *src = (struct result_value_t *)v;
            if (!src->ok)
                return newresulterr(dupval(src->error));
            return newresultok(dupval(src->value));
        }
        case T_struct: {
            struct struct_value_t *src = (struct struct_value_t *)v;
            struct struct_value_t *dst = new struct_value_t;
            dst->type = T_struct;
            dst->def = src->def;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_enum: {
            struct enum_value_t *src = (struct enum_value_t *)v;
            struct enum_value_t *dst = new enum_value_t;
            dst->type = T_enum;
            dst->def = src->def;
            dst->variant = src->variant;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_lru: {
            struct lru_value_t *src = (struct lru_value_t *)v;
            struct lru_value_t *dst = new lru_value_t;
            dst->type = T_lru;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_deque: {
            struct deque_value_t *src = (struct deque_value_t *)v;
            struct deque_value_t *dst = new deque_value_t;
            dst->type = T_deque;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_heap: {
            struct heap_value_t *src = (struct heap_value_t *)v;
            struct heap_value_t *dst = new heap_value_t;
            dst->type = T_heap;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_set: {
            struct set_value_t *src = (struct set_value_t *)v;
            struct set_value_t *dst = new set_value_t;
            dst->type = T_set;
            dst->payload = src->payload;
            return (struct value_t *)dst;
        }
        case T_method: {
            struct method_value_t *src = (struct method_value_t *)v;
            struct method_value_t *dst = new method_value_t;
            dst->type = T_method;
            dst->def = src->def;
            dst->receiver = src->receiver;
            dst->method = src->method;
            return (struct value_t *)dst;
        }
        case T_null:
            return newnull();
        case T_return:
            return newreturn();
        case T_break:
            return newbreak(((struct control_value_t *)v)->label);
        case T_continue:
            return newcontinue(((struct control_value_t *)v)->label);
        default:
            return NULL;
    }
}

void freeval(struct value_t *v) {
    if (!v) return;
    if (free_depth > 1000)
        return; /* avoid C++ stack overflow on very deep values */
    free_depth_guard guard;
    switch (v->type) {
        case T_int: {
            struct int_value_t *integer = (struct int_value_t *)v;
            if (!integer->interned)
                delete integer;
            break;
        }
        case T_float:
            delete (struct float_value_t *)v;
            break;
        case T_bool:
            /* newboolval returns one of two process-wide singletons. */
            break;
        case T_string: {
            struct string_value_t *s = (struct string_value_t *)v;
            std::free(s->value);
            delete s;
            break;
        }
        case T_char:
            delete (struct char_value_t *)v;
            break;
        case T_byte:
            delete (struct byte_value_t *)v;
            break;
        case T_bytes:
            delete (struct bytes_value_t *)v;
            break;
        case T_list:
            delete (struct list_value_t *)v;
            break;
        case T_dict:
            delete (struct dict_value_t *)v;
            break;
        case T_func:
            delete (struct func_value_t *)v;
            break;
        case T_builtin:
            delete (struct builtin_value_t *)v;
            break;
        case T_native:
            delete (struct native_function_value *)v;
            break;
        case T_userdata:
            delete (struct userdata_value_t *)v;
            break;
        case T_error:
            delete (struct error_value_t *)v;
            break;
        case T_result: {
            struct result_value_t *r = (struct result_value_t *)v;
            freeval(r->value);
            freeval(r->error);
            delete r;
            break;
        }
        case T_struct:
            delete (struct struct_value_t *)v;
            break;
        case T_enum:
            delete (struct enum_value_t *)v;
            break;
        case T_lru:
            delete (struct lru_value_t *)v;
            break;
        case T_deque:
            delete (struct deque_value_t *)v;
            break;
        case T_heap:
            delete (struct heap_value_t *)v;
            break;
        case T_set:
            delete (struct set_value_t *)v;
            break;
        case T_method:
            delete (struct method_value_t *)v;
            break;
        case T_null:
            /* newnull returns a process-wide singleton. */
            break;
        case T_return:
            delete v;
            break;
        case T_break:
        case T_continue: {
            struct control_value_t *c = (struct control_value_t *)v;
            std::free(c->label);
            delete c;
            break;
        }
        default:
            break;
    }
}

const char *typename_of(const struct value_t *v) {
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
        case T_userdata: {
            const struct userdata_value_t *u = (const struct userdata_value_t *)v;
            return u->meta ? u->meta->name : "userdata";
        }
        case T_result: return "result";
        case T_error: return "error";
        case T_struct: {
            const struct struct_value_t *s = (const struct struct_value_t *)v;
            return s->def ? s->def->name.c_str() : "struct";
        }
        case T_enum: {
            const struct enum_value_t *e = (const struct enum_value_t *)v;
            return e->def ? e->def->name.c_str() : "enum";
        }
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

static std::string format_double(double d) {
    if (std::isnan(d))
        return std::signbit(d) ? "-nan" : "nan";
    if (std::isinf(d))
        return d < 0 ? "-inf" : "inf";

    char buf[64];
    for (int precision = 15; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, d);
        if (std::strtod(buf, NULL) == d)
            break;
    }

    std::string s(buf);
    if (s.find_first_of(".eE") == std::string::npos)
        s += ".0";
    return s;
}

std::string value_to_string(const struct value_t *v) {
    if (!v) return "null";
    if (render_depth > 100)
        return "...";
    render_depth_guard guard;
    switch (v->type) {
        case T_int:
            return std::to_string(((const struct int_value_t *)v)->value);
        case T_float:
            return format_double(((const struct float_value_t *)v)->value);
        case T_bool:
            return ((const struct bool_value_t *)v)->value ? "true" : "false";
        case T_string: {
            const char *s = ((const struct string_value_t *)v)->value;
            return s ? s : "";
        }
        case T_char: {
            std::string out;
            if (!utf8_encode(((const struct char_value_t *)v)->value, out))
                return "<char>";
            return out;
        }
        case T_byte:
            return std::to_string(((const struct byte_value_t *)v)->value);
        case T_bytes: {
            const struct bytes_value_t *b = (const struct bytes_value_t *)v;
            return "<bytes: " + std::to_string(b->value.size()) + ">";
        }
        case T_list: {
            const struct list_value_t *l = (const struct list_value_t *)v;
            std::string out = "[";
            for (size_t i = 0; i < l->payload->items.size(); ++i) {
                if (i) out += ", ";
                out += value_to_string(l->payload->items[i]);
            }
            out += "]";
            return out;
        }
        case T_dict: {
            const struct dict_value_t *d = (const struct dict_value_t *)v;
            std::string out = "{";
            for (size_t i = 0; i < d->payload->items.size(); ++i) {
                if (i) out += ", ";
                out += value_to_string(d->payload->items[i].first);
                out += ": ";
                out += value_to_string(d->payload->items[i].second);
            }
            out += "}";
            return out;
        }
        case T_func:
            return "<function>";
        case T_builtin:
            return "<builtin>";
        case T_native:
            return "<native>";
        case T_userdata: {
            const struct userdata_value_t *u = (const struct userdata_value_t *)v;
            if (u->meta && u->meta->describe)
                return u->meta->describe(u->data.get());
            return "<userdata>";
        }
        case T_error:
            return ((const struct error_value_t *)v)->message;
        case T_result: {
            const struct result_value_t *r =
                (const struct result_value_t *)v;
            if (r->ok)
                return "ok(" + value_to_string(r->value) + ")";
            return "err(" + value_to_string(r->error) + ")";
        }
        case T_struct: {
            const struct struct_value_t *s = (const struct struct_value_t *)v;
            if (!s->def || !s->payload)
                return "<struct>";
            if (s->def->fields.empty())
                return s->def->name + " {}";
            std::string out = s->def->name + " { ";
            for (size_t i = 0; i < s->def->fields.size(); ++i) {
                if (i) out += ", ";
                out += s->def->fields[i].name;
                out += ": ";
                out += (i < s->payload->fields.size())
                           ? value_to_string(s->payload->fields[i])
                           : "null";
            }
            out += " }";
            return out;
        }
        case T_enum: {
            const struct enum_value_t *e = (const struct enum_value_t *)v;
            if (!e->def || !e->payload)
                return "<enum>";
            if (e->variant < 0 ||
                (size_t)e->variant >= e->def->variants.size())
                return "<enum>";
            const struct variant_def &variant =
                e->def->variants[(size_t)e->variant];
            std::string out = e->def->name + "::" + variant.name;
            if (variant.fields.empty())
                return out;
            out += " { ";
            for (size_t i = 0; i < variant.fields.size(); ++i) {
                if (i) out += ", ";
                out += variant.fields[i].name;
                out += ": ";
                out += (i < e->payload->fields.size())
                           ? value_to_string(e->payload->fields[i])
                           : "null";
            }
            out += " }";
            return out;
        }
        case T_lru: {
            const struct lru_value_t *l = (const struct lru_value_t *)v;
            std::string out = "lru(" +
                              std::to_string(l->payload->items.size()) +
                              "/" + std::to_string(l->payload->capacity) +
                              ", {";
            for (size_t i = 0; i < l->payload->items.size(); ++i) {
                if (i) out += ", ";
                out += value_to_string(l->payload->items[i].first);
                out += ": ";
                out += value_to_string(l->payload->items[i].second);
            }
            out += "})";
            return out;
        }
        case T_deque: {
            const struct deque_value_t *d = (const struct deque_value_t *)v;
            std::vector<value_t *> items;
            deque_items_in_order(d->payload.get(), items);
            std::string out = "deque([";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) out += ", ";
                out += value_to_string(items[i]);
            }
            out += "])";
            return out;
        }
        case T_heap: {
            const struct heap_value_t *h = (const struct heap_value_t *)v;
            std::vector<value_t *> items;
            heap_items_level_order(h->payload.get(), items);
            std::string out = "heap([";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) out += ", ";
                out += value_to_string(items[i]);
            }
            out += "])";
            return out;
        }
        case T_set: {
            const struct set_value_t *s = (const struct set_value_t *)v;
            std::vector<value_t *> items;
            set_values_in_order(s->payload.get(), items);
            std::string out = "set([";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) out += ", ";
                out += value_to_string(items[i]);
            }
            out += "])";
            return out;
        }
        case T_method:
            return "<method>";
        case T_null:
            return "null";
        case T_class:
            return "<class>";
        case T_return:
            return "<return>";
        case T_break:
            return "<break>";
        case T_continue:
            return "<continue>";
        default:
            return "<unknown>";
    }
}

void printvalue(struct value_t *v) {
    std::string s = value_to_string(v);
    std::fputs(s.c_str(), stdout);
}
