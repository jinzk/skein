#include "dict.h"

#include "runtime_context.h"
#include "structtype.h"
#include "value_ops.h"

#include <bit>
#include <cmath>
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
        case T_float:
            /*
             * NaN is not equal to itself, so it cannot be a stable dict key.
             * Every other float is accepted; hashing canonicalizes -0.0 and
             * integral floats so they agree with the matching int keys.
             */
            return !std::isnan(((const struct float_value_t *)key)->value);
        default:
            return false;
    }
}

const struct type_def *value_type_def(const struct value_t *value) {
    if (!value)
        return NULL;
    if (value->type == T_struct)
        return ((const struct struct_value_t *)value)->def;
    if (value->type == T_enum)
        return ((const struct enum_value_t *)value)->def;
    return NULL;
}

std::shared_ptr<function_def> custom_method(const struct type_def *def,
                                           const char *name) {
    if (!def || !name)
        return NULL;
    std::shared_ptr<function_def> method = structtype_method(def, name);
    if (method)
        return method;
    return NULL;
}

std::shared_ptr<function_def> custom_hash_method(const struct type_def *def) {
    return custom_method(def, "dict_hash");
}

std::shared_ptr<function_def> custom_key_eq_method(const struct type_def *def) {
    return custom_method(def, "key_eq");
}

thread_local bool g_custom_hash_active = false;

/*
 * Key validation/hashing only recurses through tuple, struct, and enum
 * payloads.  Typical keys are shallow, so keep the first levels inline and
 * fall back to a vector only for unusually deep keys.
 */
struct key_stack {
    static constexpr std::size_t kInline = 32;
    const void *inline_items[kInline];
    std::vector<const void *> overflow;
    std::size_t depth = 0;

    bool contains(const void *item) const {
        const std::size_t inline_count =
            depth < kInline ? depth : kInline;
        for (std::size_t i = 0; i < inline_count; ++i)
            if (inline_items[i] == item)
                return true;
        for (const void *entry : overflow)
            if (entry == item)
                return true;
        return false;
    }

    void push(const void *item) {
        if (depth < kInline)
            inline_items[depth] = item;
        else
            overflow.push_back(item);
        ++depth;
    }

    void pop() {
        --depth;
        if (depth >= kInline)
            overflow.pop_back();
    }
};

bool call_custom_hash(const struct value_t *value, std::uint64_t &out) {
    const struct type_def *def = value_type_def(value);
    std::shared_ptr<function_def> method = custom_hash_method(def);
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
        newmethodval(dupval((struct value_t *)value), method);
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

thread_local bool g_key_eq_active = false;

bool call_custom_key_eq(const struct value_t *left,
                        const struct value_t *right, bool &out) {
    const struct type_def *def = value_type_def(left);
    if (!def || def != value_type_def(right))
        return false;
    if (!custom_hash_method(def))
        return false;
    std::shared_ptr<function_def> method = custom_key_eq_method(def);
    if (!method)
        return false;
    if (g_key_eq_active) {
        yyerror("key_eq must not re-enter dict/set key equality");
        return false;
    }
    if (!skein_context_current() || !skein_context_current()->call) {
        yyerror("key_eq requires an interpreter context");
        return false;
    }

    struct key_eq_guard {
        key_eq_guard() { g_key_eq_active = true; }
        ~key_eq_guard() { g_key_eq_active = false; }
    } guard;

    struct value_t *callee =
        newmethodval(dupval((struct value_t *)left), method);
    struct value_t *result =
        callee ? skein_call_function(callee, {dupval((struct value_t *)right)})
               : NULL;
    freeval(callee);
    if (!result)
        return false;
    if (result->type != T_bool) {
        yyerror("key_eq must return a bool");
        freeval(result);
        return false;
    }
    out = ((struct bool_value_t *)result)->value;
    freeval(result);
    return true;
}

bool key_equals_inner(const struct value_t *a, const struct value_t *b,
                      std::size_t depth) {
    if (!a || !b)
        return a == b;
    if (depth > kMaxKeyDepth)
        return false;
    if (a->type != b->type)
        return value_equals((struct value_t *)a, (struct value_t *)b);

    if (a->type == T_struct) {
        const struct struct_value_t *sa = (const struct struct_value_t *)a;
        const struct struct_value_t *sb = (const struct struct_value_t *)b;
        if (sa->def != sb->def)
            return false;
        if (!sa->payload || !sb->payload)
            return sa->payload == sb->payload;
        if (sa->payload == sb->payload)
            return true;
        if (custom_hash_method(sa->def) && custom_key_eq_method(sa->def)) {
            bool equal = false;
            if (!call_custom_key_eq(a, b, equal))
                return false;
            return equal;
        }
        if (sa->payload->fields.size() != sb->payload->fields.size())
            return false;
        for (std::size_t i = 0; i < sa->payload->fields.size(); ++i)
            if (!key_equals_inner(sa->payload->fields[i],
                                  sb->payload->fields[i], depth + 1))
                return false;
        return true;
    }

    if (a->type == T_enum) {
        const struct enum_value_t *ea = (const struct enum_value_t *)a;
        const struct enum_value_t *eb = (const struct enum_value_t *)b;
        if (ea->def != eb->def || ea->variant != eb->variant)
            return false;
        if (!ea->payload || !eb->payload)
            return ea->payload == eb->payload;
        if (ea->payload == eb->payload)
            return true;
        if (custom_hash_method(ea->def) && custom_key_eq_method(ea->def)) {
            bool equal = false;
            if (!call_custom_key_eq(a, b, equal))
                return false;
            return equal;
        }
        if (ea->payload->fields.size() != eb->payload->fields.size())
            return false;
        for (std::size_t i = 0; i < ea->payload->fields.size(); ++i)
            if (!key_equals_inner(ea->payload->fields[i],
                                  eb->payload->fields[i], depth + 1))
                return false;
        return true;
    }

    if (a->type == T_list) {
        const struct list_value_t *la = (const struct list_value_t *)a;
        const struct list_value_t *lb = (const struct list_value_t *)b;
        if (!la->payload || !lb->payload)
            return la->payload == lb->payload;
        if (!la->payload->frozen || !lb->payload->frozen)
            return value_equals((struct value_t *)a, (struct value_t *)b);
        if (la->payload == lb->payload)
            return true;
        if (la->payload->items.size() != lb->payload->items.size())
            return false;
        for (std::size_t i = 0; i < la->payload->items.size(); ++i)
            if (!key_equals_inner(la->payload->items[i],
                                  lb->payload->items[i], depth + 1))
                return false;
        return true;
    }

    return value_equals((struct value_t *)a, (struct value_t *)b);
}

bool key_allowed_inner(
    const struct value_t *key, key_stack &stack, std::size_t depth) {
    if (!key || depth > kMaxKeyDepth)
        return false;
    if (scalar_key_allowed(key))
        return true;

    const std::vector<struct value_t *> *children = NULL;
    const void *identity = NULL;
    if (key->type == T_list) {
        const struct list_value_t *list = (const struct list_value_t *)key;
        if (!list->payload || !list->payload->frozen)
            return false;
        identity = list->payload.get();
        children = &list->payload->items;
    } else if (key->type == T_struct) {
        const struct struct_value_t *structure =
            (const struct struct_value_t *)key;
        if (!structure->def || !structure->payload)
            return false;
        if (custom_hash_method(structure->def))
            return true;
        identity = structure->payload.get();
        children = &structure->payload->fields;
    } else if (key->type == T_enum) {
        const struct enum_value_t *enumeration =
            (const struct enum_value_t *)key;
        if (!enumeration->def || !enumeration->payload)
            return false;
        if (custom_hash_method(enumeration->def))
            return true;
        identity = enumeration->payload.get();
        children = &enumeration->payload->fields;
    } else {
        return false;
    }

    if (stack.contains(identity))
        return false;
    stack.push(identity);

    bool ok = true;
    for (const struct value_t *child : *children) {
        if (!key_allowed_inner(child, stack, depth + 1)) {
            ok = false;
            break;
        }
    }

    stack.pop();
    return ok;
}

std::uint64_t hash_string(std::uint64_t hash,
                          const std::string &text) {
    return mix_hash(hash, fnv1a(text.data(), text.size()));
}

std::uint64_t type_hash(const struct type_def *def) {
    std::uint64_t hash = 0x9;
    if (!def)
        return hash;
    hash = hash_string(hash, def->source);
    return hash_string(hash, def->name);
}

std::uint64_t hash_key_inner(
    const struct value_t *key, key_stack &stack, std::size_t depth) {
    if (!key || depth > kMaxKeyDepth)
        return 0;
    switch (key->type) {
        case T_int:
            return mix_hash(0x1,
                            (std::uint64_t)((struct int_value_t *)key)->value);
        case T_float: {
            double value = ((struct float_value_t *)key)->value;
            if (std::isnan(value))
                return 0;
            /* Canonicalize -0.0 so it hashes like 0.0 and int 0. */
            if (value == 0.0)
                value = 0.0;
            /*
             * Integral floats hash exactly like the int with the same value.
             * This keeps 1 and 1.0 (which compare equal) in the same bucket.
             * Values outside the int64 range fall back to the IEEE bits.
             */
            const double two63 = 9223372036854775808.0;
            if (value >= -two63 && value < two63 &&
                std::trunc(value) == value) {
                return mix_hash(0x1, (std::uint64_t)(long long)value);
            }
            std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
            return mix_hash(0x9, bits);
        }
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
            if (stack.contains(identity))
                return 0;
            stack.push(identity);
            std::uint64_t hash =
                mix_hash(0x8, (std::uint64_t)list->payload->items.size());
            for (const struct value_t *item : list->payload->items)
                hash = mix_hash(hash, hash_key_inner(item, stack, depth + 1));
            stack.pop();
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
                if (!call_custom_hash(key, hash))
                    return 0;
                structure->payload->hash = hash;
                structure->payload->hash_valid = true;
                return hash;
            }
            const void *identity = structure->payload.get();
            if (stack.contains(identity))
                return 0;
            stack.push(identity);
            std::uint64_t hash = mix_hash(0xA, type_hash(structure->def));
            hash = mix_hash(hash,
                            (std::uint64_t)structure->payload->fields.size());
            for (const struct value_t *field : structure->payload->fields)
                hash = mix_hash(hash, hash_key_inner(field, stack, depth + 1));
            stack.pop();
            structure->payload->hash = hash;
            structure->payload->hash_valid = true;
            return hash;
        }
        case T_enum: {
            const struct enum_value_t *enumeration =
                (const struct enum_value_t *)key;
            if (!enumeration->def || !enumeration->payload)
                return 0;
            if (enumeration->payload->hash_valid)
                return enumeration->payload->hash;
            std::shared_ptr<function_def> custom =
                custom_hash_method(enumeration->def);
            if (custom) {
                std::uint64_t hash = 0;
                if (!call_custom_hash(key, hash))
                    return 0;
                enumeration->payload->hash = hash;
                enumeration->payload->hash_valid = true;
                return hash;
            }
            const void *identity = enumeration->payload.get();
            if (stack.contains(identity))
                return 0;
            stack.push(identity);
            std::uint64_t hash = mix_hash(0xB, type_hash(enumeration->def));
            hash = mix_hash(hash, (std::uint64_t)enumeration->variant);
            hash = mix_hash(hash,
                            (std::uint64_t)enumeration->payload->fields.size());
            for (const struct value_t *field : enumeration->payload->fields)
                hash = mix_hash(hash, hash_key_inner(field, stack, depth + 1));
            stack.pop();
            enumeration->payload->hash = hash;
            enumeration->payload->hash_valid = true;
            return hash;
        }
        default:
            return 0;
    }
}

} // namespace

bool dict_key_allowed(const struct value_t *key) {
    key_stack stack;
    return key_allowed_inner(key, stack, 0);
}

const char *dict_key_error() {
    return "dict key must be int, float (not NaN), bool, string, char, "
           "byte, bytes, null, an immutable tuple of these, or a hashable "
           "struct/enum";
}

std::uint64_t dict_key_hash(const struct value_t *key) {
    key_stack stack;
    return hash_key_inner(key, stack, 0);
}

bool dict_key_equals(const struct value_t *a, const struct value_t *b) {
    return key_equals_inner(a, b, 0);
}

int dict_find_index(const struct dict_value_t *dict,
                    const struct value_t *key) {
    if (!dict || !dict->payload || !dict_key_allowed(key))
        return -1;

    struct dict_payload *payload = dict->payload.get();
    const auto &items = payload->items;

    if (items.size() < kDictIndexThreshold) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (dict_key_equals(items[i].first, key))
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
            dict_key_equals(items[index].first, key))
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
    const struct type_def *key_def = value_type_def(key);
    const bool custom_key = key_def && custom_hash_method(key_def);
    if (allowed && (custom_key ||
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
