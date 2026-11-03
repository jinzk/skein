#include "dict.h"

#include "eval_helpers.h"

#include <cstring>

namespace {

constexpr std::size_t kDictIndexThreshold = 8;

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

} // namespace

bool dict_key_allowed(const struct value_t *key) {
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

const char *dict_key_error() {
    return "dict key must be int, bool, string, char, byte, bytes, or null";
}

std::uint64_t dict_key_hash(const struct value_t *key) {
    if (!key)
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
        default:
            return 0;
    }
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
    payload->items.push_back(std::make_pair(key, value));
    if (payload->index_valid && payload->index_size == index &&
        dict_key_allowed(key)) {
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
