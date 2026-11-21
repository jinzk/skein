#include "dict.h"

#include "runtime_context.h"
#include "structtype.h"
#include "value_ops.h"

#include <cstring>
#include <vector>

namespace {

constexpr std::size_t kDictIndexThreshold = 16;
constexpr std::size_t kMaxKeyDepth = 512;

std::uint64_t mix_hash(std::uint64_t hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

std::uint64_t fnv1a(const char *data, std::size_t size) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= (unsigned char)data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void rebuild_index(struct dict_payload *payload) {
    payload->index.clear();
    payload->index.reserve(payload->items.size());
    for (std::size_t i = 0; i < payload->items.size(); ++i) {
        struct value_t *key = payload->items[i].first;
        if (!key || !dict_key_allowed(key))
            continue;
        payload->index[dict_key_hash(key)].push_back(i);
    }
    payload->index_valid = true;
    payload->index_size = payload->items.size();
}

bool scalar_key_allowed(const struct value_t *key) {
    if (!key)
        return false;
    switch (key->type) {
        case T_int:
        case T_bool:
        case T_string:
        case T_char:
        case T_byte:
        case T_bytes:
        case T_null:
            return true;
        default:
            return false;
    }
}

std::shared_ptr<function_def> custom_hash_method(const struct_def *def) {
    if (!def)
        return NULL;
    auto method = def->methods.find("dict_hash");
    if (method != def->methods.end() && method->second)
        return method->second;
    for (const trait_def *trait : def->traits) {
        if (!trait)
            continue;
        auto fallback = trait->defaults.find("dict_hash");
        if (fallback != trait->defaults.end() && fallback->second)
            return fallback->second;
    }
    return NULL;
}

thread_local bool g_custom_hash_active = false;

bool call_custom_hash(const struct_value_t *structure,
                      std::uint64_t &out) {
    if (!structure || !structure->def)
        return false;
    std::shared_ptr<function_def> method =
        custom_hash_method(structure->def);
    if (!method)
        return false;
    if (g_custom_hash_active) {
        yyerror("dict_hash must not re-enter dict/set key hashing");
        return false;
    }
    if (!skein_context_current() || !skein_context_current()->call) {
        yyerror("dict_hash requires an interpreter context");
        return false;
    }

    struct custom_hash_guard {
        custom_hash_guard() { g_custom_hash_active = true; }
        ~custom_hash_guard() { g_custom_hash_active = false; }
    } guard;

    struct value_t *callee =
        newmethodval(dupval((struct value_t *)structure), method);
    struct value_t *result =
        callee ? skein_call_function(callee, {}) : NULL;
    freeval(callee);
    if (!result)
        return false;
    if (result->type != T_int) {
        yyerror("dict_hash must return an int");
        freeval(result);
        return false;
    }
    out = (std::uint64_t)((struct int_value_t *)result)->value;
    freeval(result);
    return true;
}

bool key_allowed_inner(
    const struct value_t *key, std::vector<const void *> &stack,
    std::size_t depth) {
    if (!key || depth > kMaxKeyDepth)
        return false;
    if (scalar_key_allowed(key))
        return true;

    const void *identity = NULL;
    if (key->type == T_list) {
        const struct list_value_t *list = (const struct list_value_t *)key;
        if (!list->payload || !list->payload->frozen)
            return false;
        identity = list->payload.get();
    } else if (key->type == T_struct) {
        const struct struct_value_t *structure =
            (const struct struct_value_t *)key;
        if (!structure->def || !structure->payload)
            return false;
        if (custom_hash_method(structure->def))
            return true;
        identity = structure->payload.get();
    } else {
        return false;
    }

    for (const void *parent : stack)
        if (parent == identity)
            return false;
    stack.push_back(identity);

    bool ok = true;
    if (key->type == T_list) {
        const struct list_value_t *list = (const struct list_value_t *)key;
        for (const struct value_t *item : list->payload->items) {
            if (!key_allowed_inner(item, stack, depth + 1)) {
                ok = false;
                break;
            }
        }
    } else {
        const struct struct_value_t *structure =
            (const struct struct_value_t *)key;
        for (const struct value_t *field : structure->payload->fields) {
            if (!key_allowed_inner(field, stack, depth + 1)) {
                ok = false;
                break;
            }
        }
    }

    stack.pop_back();
    return ok;
}

std::uint64_t hash_string(std::uint64_t hash,
                          const std::string &text) {
    return mix_hash(hash, fnv1a(text.data(), text.size()));
}

std::uint64_t struct_type_hash(const struct_def *def) {
    std::uint64_t hash = 0x9;
    if (!def)
        return hash;
    hash = hash_string(hash, def->source);
    return hash_string(hash, def->name);
}

std::uint64_t hash_key_inner(
    const struct value_t *key, std::vector<const void *> &stack,
    std::size_t depth) {
    if (!key || depth > kMaxKeyDepth)
        return 0;
    switch (key->type) {
        case T_int:
            return mix_hash(0x1,
                            (std::uint64_t)((struct int_value_t *)key)->value);
        case T_bool:
            return mix_hash(0x2,
                            ((struct bool_value_t *)key)->value ? 1 : 0);
        case T_char:
            return mix_hash(0x3,
                            ((struct char_value_t *)key)->value);
        case T_byte:
            return mix_hash(0x4,
                            ((struct byte_value_t *)key)->value);
        case T_string: {
            const char *text = ((struct string_value_t *)key)->value;
            return mix_hash(0x5, fnv1a(text, std::strlen(text)));
        }
        case T_bytes: {
            const std::string &data = ((struct bytes_value_t *)key)->value;
            return mix_hash(0x6, fnv1a(data.data(), data.size()));
        }
        case T_null:
            return 0x7;
        case T_list: {
            const struct list_value_t *list =
                (const struct list_value_t *)key;
            if (!list->payload || !list->payload->frozen)
                return 0;
            if (list->payload->hash_valid)
                return list->payload->hash;
            const void *identity = list->payload.get();
            for (const void *parent : stack)
                if (parent == identity)
                    return 0;
            stack.push_back(identity);
            std::uint64_t hash =
                mix_hash(0x8, (std::uint64_t)list->payload->items.size());
            for (const struct value_t *item : list->payload->items)
                hash = mix_hash(hash, hash_key_inner(item, stack, depth + 1));
            stack.pop_back();
            list->payload->hash = hash;
            list->payload->hash_valid = true;
            return hash;
        }
        case T_struct: {
            const struct struct_value_t *structure =
                (const struct struct_value_t *)key;
            if (!structure->def || !structure->payload)
                return 0;
            if (structure->payload->hash_valid)
                return structure->payload->hash;
            std::shared_ptr<function_def> custom =
                custom_hash_method(structure->def);
            if (custom) {
                std::uint64_t hash = 0;
                if (!call_custom_hash(structure, hash))
                    return 0;
                structure->payload->hash = hash;
                structure->payload->hash_valid = true;
                return hash;
            }
            const void *identity = structure->payload.get();
            for (const void *parent : stack)
                if (parent == identity)
                    return 0;
            stack.push_back(identity);
            std::uint64_t hash = mix_hash(0xA,
                                          struct_type_hash(structure->def));
            hash = mix_hash(hash,
                            (std::uint64_t)structure->payload->fields.size());
            for (const struct value_t *field : structure->payload->fields)
                hash = mix_hash(hash, hash_key_inner(field, stack, depth + 1));
            stack.pop_back();
            structure->payload->hash = hash;
            structure->payload->hash_valid = true;
            return hash;
        }
        default:
            return 0;
    }
}

} // namespace

bool dict_key_allowed(const struct value_t *key) {
    std::vector<const void *> stack;
    return key_allowed_inner(key, stack, 0);
}

const char *dict_key_error() {
    return "dict key must be int, bool, string, char, byte, bytes, null, "
           "an immutable tuple of these, or a hashable struct";
}

std::uint64_t dict_key_hash(const struct value_t *key) {
    std::vector<const void *> stack;
    return hash_key_inner(key, stack, 0);
}

int dict_find_index(const struct dict_value_t *dict,
                    const struct value_t *key) {
    if (!dict || !dict->payload || !dict_key_allowed(key))
        return -1;

    struct dict_payload *payload = dict->payload.get();
    const auto &items = payload->items;

    if (items.size() < kDictIndexThreshold) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (value_equals(items[i].first, (struct value_t *)key))
                return (int)i;
        }
        return -1;
    }

    if (!payload->index_valid ||
        payload->index_size != items.size()) {
        rebuild_index(payload);
    }

    std::uint64_t hash = dict_key_hash(key);
    auto it = payload->index.find(hash);
    if (it == payload->index.end())
        return -1;
    for (std::size_t index : it->second) {
        if (index < items.size() &&
            value_equals(items[index].first, (struct value_t *)key))
            return (int)index;
    }
    return -1;
}

void dict_append_item(struct dict_value_t *dict, struct value_t *key,
                      struct value_t *value) {
    if (!dict || !dict->payload)
        return;
    struct dict_payload *payload = dict->payload.get();
    std::size_t index = payload->items.size();
    const bool allowed = dict_key_allowed(key);
    const bool custom_struct =
        key && key->type == T_struct &&
        custom_hash_method(((const struct struct_value_t *)key)->def);
    if (allowed && (custom_struct ||
                    (payload->index_valid && payload->index_size == index)))
        dict_key_hash(key);
    payload->items.push_back(std::make_pair(key, value));
    if (payload->index_valid && payload->index_size == index && allowed) {
        payload->index[dict_key_hash(key)].push_back(index);
        payload->index_size = index + 1;
    }
}

void dict_invalidate_index(struct dict_value_t *dict) {
    if (!dict || !dict->payload)
        return;
    dict->payload->index_valid = false;
    dict->payload->index.clear();
    dict->payload->index_size = 0;
}
