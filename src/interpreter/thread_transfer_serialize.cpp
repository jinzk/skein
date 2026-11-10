#include "vm.h"

#include "ast.h"
#include "compiler.h"
#include "dict.h"
#include "eval.h"
#include "scopestack.h"
#include "structeval.h"
#include "structtype.h"
#include "symbol.h"
#include "verifier.h"
#include "vm_internal.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kPayloadMagic = 0x48545453u; /* "STTH" */
constexpr uint32_t kPayloadVersion = 7u;
constexpr uint32_t kEndianMarker = 0x01020304u;
constexpr uint32_t kMaxCount = 1000000u;
constexpr uint32_t kMaxString = 64u * 1024u * 1024u;
constexpr int kMaxDepth = 64;

thread_local std::unordered_map<
    const function_code *,
    std::shared_ptr<thread_transfer_function>>
    g_inline_function_payloads;

struct value_deleter {
    void operator()(struct value_t *value) const { freeval(value); }
};

using owned_value = std::shared_ptr<struct value_t>;
using owned_type = std::shared_ptr<struct type_constraint>;

struct value_owner {
    struct value_t *pointer = nullptr;

    value_owner() = default;
    explicit value_owner(struct value_t *value) : pointer(value) {}
    value_owner(const value_owner &) = delete;
    value_owner &operator=(const value_owner &) = delete;
    value_owner(value_owner &&other) noexcept : pointer(other.pointer) {
        other.pointer = nullptr;
    }
    value_owner &operator=(value_owner &&other) noexcept {
        if (this != &other) {
            reset();
            pointer = other.pointer;
            other.pointer = nullptr;
        }
        return *this;
    }
    ~value_owner() { reset(); }

    struct value_t *get() const { return pointer; }
    struct value_t *release() {
        struct value_t *value = pointer;
        pointer = nullptr;
        return value;
    }
    void reset() {
        if (pointer) {
            freeval(pointer);
            pointer = nullptr;
        }
    }
};

owned_value take_owned(value_owner &owner) {
    return owned_value(owner.release(), value_deleter());
}

owned_type make_owned_type(struct type_constraint *type) {
    return owned_type(type, [](struct type_constraint *value) {
        free_type_constraint(value);
    });
}

struct writer {
    std::string data;

    void u8(uint8_t value) { data.push_back((char)value); }

    void u16(uint16_t value) {
        u8((uint8_t)(value & 0xFF));
        u8((uint8_t)((value >> 8) & 0xFF));
    }

    void u32(uint32_t value) {
        for (int i = 0; i < 4; ++i)
            u8((uint8_t)((value >> (8 * i)) & 0xFF));
    }

    void u64(uint64_t value) {
        for (int i = 0; i < 8; ++i)
            u8((uint8_t)((value >> (8 * i)) & 0xFF));
    }

    void i64(int64_t value) { u64((uint64_t)value); }

    void f64(double value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }

    void str(const std::string &value) {
        u32((uint32_t)value.size());
        data.append(value);
    }

    void raw(const std::string &value) { data.append(value); }
};

struct reader {
    const std::string &data;
    std::size_t pos = 0;
    std::string error;

    explicit reader(const std::string &bytes) : data(bytes) {}

    bool need(std::size_t count) {
        if (pos + count <= data.size())
            return true;
        error = "truncated thread payload";
        return false;
    }

    bool u8(uint8_t &value) {
        if (!need(1))
            return false;
        value = (uint8_t)data[pos++];
        return true;
    }

    bool u16(uint16_t &value) {
        value = 0;
        for (int i = 0; i < 2; ++i) {
            uint8_t byte = 0;
            if (!u8(byte))
                return false;
            value |= (uint16_t)byte << (8 * i);
        }
        return true;
    }

    bool u32(uint32_t &value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t byte = 0;
            if (!u8(byte))
                return false;
            value |= (uint32_t)byte << (8 * i);
        }
        return true;
    }

    bool u64(uint64_t &value) {
        value = 0;
        for (int i = 0; i < 8; ++i) {
            uint8_t byte = 0;
            if (!u8(byte))
                return false;
            value |= (uint64_t)byte << (8 * i);
        }
        return true;
    }

    bool i64(int64_t &value) {
        uint64_t bits = 0;
        if (!u64(bits))
            return false;
        value = (int64_t)bits;
        return true;
    }

    bool f64(double &value) {
        uint64_t bits = 0;
        if (!u64(bits))
            return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

    bool str(std::string &value) {
        uint32_t size = 0;
        if (!u32(size))
            return false;
        if (size > kMaxString) {
            error = "thread payload string is too large";
            return false;
        }
        if (!need(size))
            return false;
        value.assign(data.data() + pos, size);
        pos += size;
        return true;
    }

    bool count(uint32_t &value) {
        if (!u32(value))
            return false;
        if (value > kMaxCount) {
            error = "thread payload item count is too large";
            return false;
        }
        return true;
    }
};

bool write_type(writer &out, const struct type_constraint *type) {
    if (!type) {
        out.u8(0);
        return true;
    }
    out.u8(1);
    out.u8((uint8_t)type->kind);
    out.str(type->name ? type->name : "");
    out.str(type->module ? type->module : "");
    out.u8(type->nullable ? 1 : 0);
    out.str(type->display ? type->display : "");
    return true;
}

bool read_type_ptr(reader &in, owned_type &type, std::string &error) {
    uint8_t present = 0;
    if (!in.u8(present))
        return false;
    if (!present) {
        type.reset();
        return true;
    }
    uint8_t kind = 0;
    std::string name;
    std::string module;
    std::string display;
    uint8_t nullable = 0;
    if (!in.u8(kind) || !in.str(name) || !in.str(module) ||
        !in.u8(nullable) || !in.str(display)) {
        error = in.error;
        return false;
    }
    struct type_constraint *value =
        new_named_type_constraint(name.c_str(), module.c_str());
    value->kind = (enum type_constraint_kind)kind;
    value->nullable = nullable != 0;
    value->display = display.empty() ? nullptr : nullptr;
    value->detail = nullptr;
    type = make_owned_type(value);
    return true;
}

bool read_type_value(reader &in, struct type_constraint &type,
                     std::string &error) {
    owned_type parsed;
    if (!read_type_ptr(in, parsed, error))
        return false;
    if (parsed)
        type_constraint_assign(&type, parsed.get());
    else
        type_constraint_set_any(&type);
    return true;
}

enum class value_tag : uint8_t {
    Null = 0,
    Bool,
    Int,
    Float,
    Char,
    Byte,
    String,
    Bytes,
    Error,
    Result,
    List,
    Dict,
    Set,
    Deque,
    Lru,
    Builtin,
    Struct,
    Enum,
};

bool write_value(writer &out, const struct value_t *value, int depth,
                 std::string &error);

bool write_value_inner(writer &out, const struct value_t *value, int depth,
                       std::string &error) {
    if (!value) {
        error = "cannot serialize a null value pointer";
        return false;
    }
    if (depth > kMaxDepth) {
        error = "thread payload value nesting is too deep";
        return false;
    }
    switch (value->type) {
        case T_null:
            out.u8((uint8_t)value_tag::Null);
            return true;
        case T_bool:
            out.u8((uint8_t)value_tag::Bool);
            out.u8(((const struct bool_value_t *)value)->value ? 1 : 0);
            return true;
        case T_int:
            out.u8((uint8_t)value_tag::Int);
            out.i64(((const struct int_value_t *)value)->value);
            return true;
        case T_float:
            out.u8((uint8_t)value_tag::Float);
            out.f64(((const struct float_value_t *)value)->value);
            return true;
        case T_char:
            out.u8((uint8_t)value_tag::Char);
            out.u32(((const struct char_value_t *)value)->value);
            return true;
        case T_byte:
            out.u8((uint8_t)value_tag::Byte);
            out.u32(((const struct byte_value_t *)value)->value);
            return true;
        case T_string:
            out.u8((uint8_t)value_tag::String);
            out.str(((const struct string_value_t *)value)->value
                        ? ((const struct string_value_t *)value)->value
                        : "");
            return true;
        case T_bytes:
            out.u8((uint8_t)value_tag::Bytes);
            out.str(((const struct bytes_value_t *)value)->value.read());
            return true;
        case T_error: {
            const struct error_value_t *error_value =
                (const struct error_value_t *)value;
            out.u8((uint8_t)value_tag::Error);
            out.str(error_value->kind);
            out.str(error_value->message);
            return true;
        }
        case T_result: {
            const struct result_value_t *result =
                (const struct result_value_t *)value;
            out.u8((uint8_t)value_tag::Result);
            out.u8(result->ok ? 1 : 0);
            return write_value(out, result->ok ? result->value : result->error,
                               depth + 1, error);
        }
        case T_list: {
            const struct list_value_t *list =
                (const struct list_value_t *)value;
            out.u8((uint8_t)value_tag::List);
            out.u32((uint32_t)list->payload->items.size());
            for (const struct value_t *item : list->payload->items) {
                if (!write_value(out, item, depth + 1, error))
                    return false;
            }
            return true;
        }
        case T_dict: {
            const struct dict_value_t *dict =
                (const struct dict_value_t *)value;
            out.u8((uint8_t)value_tag::Dict);
            out.u8(dict->payload->is_namespace ? 1 : 0);
            out.u32((uint32_t)dict->payload->items.size());
            for (const auto &item : dict->payload->items) {
                if (!write_value(out, item.first, depth + 1, error) ||
                    !write_value(out, item.second, depth + 1, error))
                    return false;
            }
            return true;
        }
        case T_set: {
            const struct set_value_t *set = (const struct set_value_t *)value;
            std::vector<struct value_t *> items;
            set_values_in_order(set->payload.get(), items);
            out.u8((uint8_t)value_tag::Set);
            out.u32((uint32_t)items.size());
            for (const struct value_t *item : items) {
                if (!write_value(out, item, depth + 1, error))
                    return false;
            }
            return true;
        }
        case T_deque: {
            const struct deque_value_t *deque =
                (const struct deque_value_t *)value;
            std::vector<struct value_t *> items;
            deque_items_in_order(deque->payload.get(), items);
            out.u8((uint8_t)value_tag::Deque);
            out.u32((uint32_t)items.size());
            for (const struct value_t *item : items) {
                if (!write_value(out, item, depth + 1, error))
                    return false;
            }
            return true;
        }
        case T_lru: {
            const struct lru_value_t *lru = (const struct lru_value_t *)value;
            out.u8((uint8_t)value_tag::Lru);
            out.u64((uint64_t)lru->payload->capacity);
            out.u32((uint32_t)lru->payload->items.size());
            for (const auto &item : lru->payload->items) {
                if (!write_value(out, item.first, depth + 1, error) ||
                    !write_value(out, item.second, depth + 1, error))
                    return false;
            }
            return true;
        }
        case T_builtin:
            out.u8((uint8_t)value_tag::Builtin);
            out.u32((uint32_t)((const struct builtin_value_t *)value)
                        ->functype);
            return true;
        case T_struct: {
            const struct struct_value_t *structure =
                (const struct struct_value_t *)value;
            out.u8((uint8_t)value_tag::Struct);
            out.str(structure->def ? structure->def->module : "");
            out.str(structure->def ? structure->def->name : "");
            out.u32((uint32_t)structure->payload->fields.size());
            for (const struct value_t *field : structure->payload->fields) {
                if (!write_value(out, field, depth + 1, error))
                    return false;
            }
            return true;
        }
        case T_enum: {
            const struct enum_value_t *enumeration =
                (const struct enum_value_t *)value;
            out.u8((uint8_t)value_tag::Enum);
            out.str(enumeration->def ? enumeration->def->module : "");
            out.str(enumeration->def ? enumeration->def->name : "");
            out.u32((uint32_t)enumeration->variant);
            out.u32((uint32_t)enumeration->payload->fields.size());
            for (const struct value_t *field : enumeration->payload->fields) {
                if (!write_value(out, field, depth + 1, error))
                    return false;
            }
            return true;
        }
        default:
            error = std::string("cannot serialize value of type ") +
                    typename_of(value);
            return false;
    }
}

bool write_value(writer &out, const struct value_t *value, int depth,
                 std::string &error) {
    return write_value_inner(out, value, depth, error);
}

bool read_value(reader &in, value_owner &value, int depth,
                std::string &error);

bool read_value_inner(reader &in, value_owner &value, int depth,
                      std::string &error) {
    if (depth > kMaxDepth) {
        error = "thread payload value nesting is too deep";
        return false;
    }
    uint8_t tag = 0;
    if (!in.u8(tag)) {
        error = in.error;
        return false;
    }
    switch ((value_tag)tag) {
        case value_tag::Null:
            value = value_owner(newnull());
            return true;
        case value_tag::Bool: {
            uint8_t raw = 0;
            if (!in.u8(raw)) {
                error = in.error;
                return false;
            }
            value = value_owner(newboolval(raw != 0));
            return true;
        }
        case value_tag::Int: {
            int64_t raw = 0;
            if (!in.i64(raw)) {
                error = in.error;
                return false;
            }
            value = value_owner(newintval((long long)raw));
            return true;
        }
        case value_tag::Float: {
            double raw = 0;
            if (!in.f64(raw)) {
                error = in.error;
                return false;
            }
            value = value_owner(newfloatval(raw));
            return true;
        }
        case value_tag::Char: {
            uint32_t raw = 0;
            if (!in.u32(raw)) {
                error = in.error;
                return false;
            }
            value = value_owner(newcharval(raw));
            return true;
        }
        case value_tag::Byte: {
            uint32_t raw = 0;
            if (!in.u32(raw)) {
                error = in.error;
                return false;
            }
            value = value_owner(newbyteval(raw));
            return true;
        }
        case value_tag::String: {
            std::string raw;
            if (!in.str(raw)) {
                error = in.error;
                return false;
            }
            value = value_owner(newstrval(raw.c_str()));
            return true;
        }
        case value_tag::Bytes: {
            std::string raw;
            if (!in.str(raw)) {
                error = in.error;
                return false;
            }
            value = value_owner(newbytesval(std::move(raw)));
            return true;
        }
        case value_tag::Error: {
            std::string kind;
            std::string message;
            if (!in.str(kind) || !in.str(message)) {
                error = in.error;
                return false;
            }
            value = value_owner(newerrorval(kind, message));
            return true;
        }
        case value_tag::Result: {
            uint8_t ok = 0;
            if (!in.u8(ok)) {
                error = in.error;
                return false;
            }
            value_owner nested;
            if (!read_value(in, nested, depth + 1, error))
                return false;
            struct value_t *raw = nested.release();
            value = value_owner(ok ? newresultok(raw)
                                   : newresulterr(raw));
            return true;
        }
        case value_tag::List: {
            uint32_t count = 0;
            if (!in.count(count)) {
                error = in.error;
                return false;
            }
            struct list_value_t *list = newlistval();
            value = value_owner((struct value_t *)list);
            for (uint32_t i = 0; i < count; ++i) {
                value_owner item;
                if (!read_value(in, item, depth + 1, error))
                    return false;
                list->payload->items.push_back(item.release());
            }
            return true;
        }
        case value_tag::Dict: {
            uint8_t namespace_flag = 0;
            uint32_t count = 0;
            if (!in.u8(namespace_flag) || !in.count(count)) {
                error = in.error;
                return false;
            }
            struct dict_value_t *dict = newdictval();
            value = value_owner((struct value_t *)dict);
            dict->payload->is_namespace = namespace_flag != 0;
            for (uint32_t i = 0; i < count; ++i) {
                value_owner key;
                value_owner item;
                if (!read_value(in, key, depth + 1, error) ||
                    !read_value(in, item, depth + 1, error)) {
                    return false;
                }
                dict_append_item(dict, key.release(), item.release());
            }
            return true;
        }
        case value_tag::Set: {
            uint32_t count = 0;
            if (!in.count(count)) {
                error = in.error;
                return false;
            }
            struct set_value_t *set = newsetval();
            value = value_owner((struct value_t *)set);
            std::shared_ptr<set_node> root = set->payload->root;
            std::shared_ptr<deque_payload> order = set->payload->order;
            std::size_t size = 0;
            for (uint32_t i = 0; i < count; ++i) {
                value_owner item;
                if (!read_value(in, item, depth + 1, error))
                    return false;
                bool added = false;
                root = set_insert(root, item.get(), added);
                if (added) {
                    order = deque_push_back_payload(order, item.get());
                    ++size;
                }
            }
            set->payload->root = std::move(root);
            set->payload->order = std::move(order);
            set->payload->size = size;
            return true;
        }
        case value_tag::Deque: {
            uint32_t count = 0;
            if (!in.count(count)) {
                error = in.error;
                return false;
            }
            struct deque_value_t *deque = newdequeval();
            value = value_owner((struct value_t *)deque);
            std::shared_ptr<deque_payload> payload = deque->payload;
            for (uint32_t i = 0; i < count; ++i) {
                value_owner item;
                if (!read_value(in, item, depth + 1, error))
                    return false;
                payload = deque_push_back_payload(payload, item.get());
            }
            deque->payload = std::move(payload);
            return true;
        }
        case value_tag::Lru: {
            uint64_t capacity = 0;
            uint32_t count = 0;
            if (!in.u64(capacity) || !in.count(count)) {
                error = in.error;
                return false;
            }
            if (count > capacity) {
                error = "thread payload LRU item count exceeds capacity";
                return false;
            }
            struct lru_value_t *lru =
                newlruval((std::size_t)capacity);
            value = value_owner((struct value_t *)lru);
            for (uint32_t i = 0; i < count; ++i) {
                value_owner key;
                value_owner item;
                if (!read_value(in, key, depth + 1, error) ||
                    !read_value(in, item, depth + 1, error)) {
                    return false;
                }
                lru->payload->items.emplace_back(key.release(),
                                                 item.release());
            }
            return true;
        }
        case value_tag::Builtin: {
            uint32_t functype = 0;
            if (!in.u32(functype)) {
                error = in.error;
                return false;
            }
            value = value_owner(newbuiltinval((int)functype));
            return true;
        }
        case value_tag::Struct: {
            std::string module;
            std::string name;
            uint32_t count = 0;
            if (!in.str(module) || !in.str(name) || !in.count(count)) {
                error = in.error;
                return false;
            }
            struct struct_def *def =
                structtype_find_struct(module.c_str(), name.c_str());
            if (!def) {
                error = "thread payload struct type is not registered: " +
                        module + "." + name;
                return false;
            }
            std::shared_ptr<struct_payload> payload =
                std::make_shared<struct_payload>();
            payload->fields.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                value_owner field;
                if (!read_value(in, field, depth + 1, error))
                    return false;
                payload->fields.push_back(field.release());
            }
            value = value_owner(newstructval(def, std::move(payload)));
            return true;
        }
        case value_tag::Enum: {
            std::string module;
            std::string name;
            uint32_t variant = 0;
            uint32_t count = 0;
            if (!in.str(module) || !in.str(name) || !in.u32(variant) ||
                !in.count(count)) {
                error = in.error;
                return false;
            }
            struct enum_def *def =
                structtype_find_enum(module.c_str(), name.c_str());
            if (!def) {
                error = "thread payload enum type is not registered: " +
                        module + "." + name;
                return false;
            }
            if (variant >= def->variants.size()) {
                error = "thread payload enum variant is out of range";
                return false;
            }
            std::shared_ptr<struct_payload> payload =
                std::make_shared<struct_payload>();
            payload->fields.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                value_owner field;
                if (!read_value(in, field, depth + 1, error))
                    return false;
                payload->fields.push_back(field.release());
            }
            value = value_owner(newenumval(def, (int)variant,
                                           std::move(payload)));
            return true;
        }
    }
    error = "invalid thread payload value tag";
    return false;
}

bool read_value(reader &in, value_owner &value, int depth,
                std::string &error) {
    return read_value_inner(in, value, depth, error);
}

struct type_record {
    bool is_enum = false;
    const struct struct_def *structure = nullptr;
    const struct enum_def *enumeration = nullptr;
    std::vector<std::pair<std::string,
                          std::shared_ptr<thread_transfer_function>>>
        methods;
    std::unordered_map<std::string,
                       std::shared_ptr<thread_transfer_function>>
        field_defaults;
};

using type_map = std::unordered_map<std::string, type_record>;

std::string type_identity(const std::string &module,
                          const std::string &name) {
    return module + '\x1f' + name;
}

struct type_collect_context {
    std::vector<std::pair<std::string, thread_transfer_value>> *globals = nullptr;
    std::unordered_set<std::string> *global_names = nullptr;
};

void collect_used_upvalues(const skein::bc::Proto &proto,
                           std::vector<bool> &used) {
    for (const skein::bc::Instr &ins : proto.code) {
        switch ((skein::bc::Op)ins.op) {
        case skein::bc::Op::LoadUpvalue:
        case skein::bc::Op::StoreUpvalue:
        case skein::bc::Op::CompoundUpvalue:
        case skein::bc::Op::IncDecUpvalue:
            if (ins.b < used.size())
                used[ins.b] = true;
            break;
        default:
            break;
        }
    }
    std::unordered_map<std::string, std::size_t> by_name;
    for (std::size_t i = 0; i < proto.upvalues.size(); ++i)
        by_name.emplace(proto.upvalues[i], i);
    auto note_name = [&](const std::string &name) {
        auto it = by_name.find(name);
        if (it != by_name.end())
            used[it->second] = true;
    };
    for (const skein::bc::Instr &ins : proto.code) {
        switch ((skein::bc::Op)ins.op) {
        case skein::bc::Op::LoadName:
        case skein::bc::Op::StoreName:
        case skein::bc::Op::CompoundName:
        case skein::bc::Op::IncDecName:
            if (ins.b < proto.consts.size()) {
                const skein::bc::Constant &constant = proto.consts[ins.b];
                if (constant.kind == skein::bc::ConstantKind::Name)
                    note_name(constant.name);
            }
            break;
        default:
            break;
        }
    }
    for (const skein::bc::IndexDesc &index : proto.index_ops) {
        if (index.base_kind == skein::bc::IndexBaseKind::Upvalue &&
            index.base_index < used.size())
            used[index.base_index] = true;
        else if (index.base_kind == skein::bc::IndexBaseKind::Name)
            note_name(index.base_name);
    }
}

void collect_used_globals(const skein::bc::Proto &proto,
                          std::vector<std::string> &names) {
    std::unordered_set<std::string> seen;
    auto note = [&](const std::string &name) {
        if (name.empty() || name == "std")
            return;
        if (seen.insert(name).second)
            names.push_back(name);
    };
    for (const skein::bc::Instr &ins : proto.code) {
        switch ((skein::bc::Op)ins.op) {
        case skein::bc::Op::LoadGlobal:
        case skein::bc::Op::StoreGlobal:
        case skein::bc::Op::CompoundGlobal:
        case skein::bc::Op::IncDecGlobal:
            if (ins.b < proto.consts.size()) {
                const skein::bc::Constant &constant = proto.consts[ins.b];
                if (constant.kind == skein::bc::ConstantKind::Name)
                    note(constant.name);
            }
            break;
        default:
            break;
        }
    }
    for (const skein::bc::IndexDesc &index : proto.index_ops) {
        if (index.base_kind == skein::bc::IndexBaseKind::Global)
            note(index.base_name);
    }
}

bool data_capture_supported(const struct value_t *value) {
    if (!value)
        return true;
    if (value->type == T_func || value->type == T_heap ||
        value->type == T_native)
        return false;
    if (value->type == T_dict) {
        const struct dict_value_t *dict = (const struct dict_value_t *)value;
        if (dict->payload && dict->payload->is_namespace)
            return false;
    }
    return true;
}

struct value_t *find_captured_value(const std::shared_ptr<function_def> &def,
                                    const std::string &name, std::size_t index) {
    struct value_t *captured = nullptr;
    if (index < def->upvalues.size() && def->upvalues[index]) {
        const std::shared_ptr<upvalue_cell> &cell = def->upvalues[index];
        captured = cell->slot ? *cell->slot : cell->value;
    }
    if (!captured && def->closure) {
        for (scope_t *scope = def->closure.get();
             scope && !scope->is_global; scope = scope->parent.get()) {
            struct symbol *sym = scope->syms.find(name);
            if (sym) {
                captured = sym->val;
                break;
            }
        }
    }
    return captured;
}

bool build_simple_function(const std::shared_ptr<function_def> &def,
                           const char *what,
                           std::shared_ptr<thread_transfer_function> &out,
                           std::string &error,
                           type_collect_context *context = nullptr,
                           bool allow_unresolved_upvalues = false) {
    if (!def || !def->code) {
        error = std::string(what) + " has no function code";
        return false;
    }
    std::vector<thread_transfer_value> defaults(
        (std::size_t)def->code->nparams, thread_transfer_value());
    int param_index = 0;
    for (struct symlist *param = def->code->sl;
         param && param_index < def->code->nparams;
         param = param->next, ++param_index) {
        if (!param->has_default)
            continue;
        struct value_t *default_value = nullptr;
        if (!skein_vm_try_eval_default(def, param_index, default_value)) {
            struct ast *default_ast = symlist_default_ast(param);
            if (!default_ast) {
                error = std::string(what) +
                        " has an unsupported default for " +
                        (param->sym && param->sym->name ? param->sym->name
                                                        : "parameter");
                return false;
            }
            default_value = eval(default_ast);
        }
        if (!default_value) {
            error = std::string(what) + " cannot evaluate default for " +
                    (param->sym && param->sym->name ? param->sym->name
                                                    : "parameter");
            return false;
        }
        if (!data_capture_supported(default_value)) {
            freeval(default_value);
            error = std::string(what) + " has a non-data default for " +
                    (param->sym && param->sym->name ? param->sym->name
                                                    : "parameter");
            return false;
        }
        thread_transfer_value transfer;
        transfer.kind = thread_transfer_value::Kind::Data;
        transfer.data = std::shared_ptr<struct value_t>(
            default_value, value_deleter());
        defaults[(std::size_t)param_index] = std::move(transfer);
    }
    skein::bc::ProtoPtr source = skein_vm_prepare_function_body(def);
    if (!source) {
        error = std::string("cannot compile ") + what;
        return false;
    }
    if (!source->packages.empty()) {
        error = std::string(what) + " contains nested packages";
        return false;
    }
    std::shared_ptr<skein::bc::Proto> copy =
        std::make_shared<skein::bc::Proto>(*source);
    copy->name_ic.clear();
    copy->member_ic.clear();
    copy->index_read_ic.clear();
    out = std::make_shared<thread_transfer_function>();
    out->name = def->code->name;
    out->proto = std::move(copy);
    out->defaults = std::move(defaults);

    std::vector<bool> used(out->proto->upvalues.size(), false);
    collect_used_upvalues(*out->proto, used);
    out->captures.assign(out->proto->upvalues.size(), thread_transfer_value());
    for (std::size_t i = 0; i < used.size(); ++i) {
        if (!used[i])
            continue;
        const std::string &name = out->proto->upvalues[i];
        struct value_t *captured = find_captured_value(def, name, i);
        if (!captured) {
            if (allow_unresolved_upvalues)
                continue;
            error = std::string(what) + " captures an unavailable value: " +
                    name;
            return false;
        }
        if (!data_capture_supported(captured)) {
            error = std::string(what) + " captures a non-data value: " + name;
            return false;
        }
        thread_transfer_value capture;
        capture.kind = thread_transfer_value::Kind::Data;
        capture.data = std::shared_ptr<struct value_t>(
            dupval(captured), value_deleter());
        out->captures[i] = std::move(capture);
    }

    bool has_param_patterns = false;
    for (struct symlist *param = def->code->sl; param; param = param->next) {
        if (param->has_pattern) {
            has_param_patterns = true;
            break;
        }
    }
    if (has_param_patterns) {
        std::string pattern_error;
        std::shared_ptr<const compiled_pattern_set> compiled =
            skein_compile_parameter_patterns(def->code.get(),
                                             def->code->module,
                                             pattern_error);
        if (!compiled || compiled->failed || !compiled->proto) {
            error = std::string(what) + " cannot compile parameter patterns" +
                    (pattern_error.empty() ? "" : ": " + pattern_error);
            return false;
        }
        out->pattern_proto = compiled->proto;
        out->pattern_roots = compiled->roots;
    }

    if (context && context->globals && context->global_names) {
        std::vector<std::string> names;
        collect_used_globals(*out->proto, names);
        for (const std::string &name : names) {
            if (!context->global_names->insert(name).second)
                continue;
            struct value_t *value = lookup_global_symbol(name.c_str());
            if (!value)
                continue;
            if (!data_capture_supported(value)) {
                error = std::string(what) + " uses a non-data global: " +
                        name;
                return false;
            }
            thread_transfer_value transfer;
            transfer.kind = thread_transfer_value::Kind::Data;
            transfer.data = std::shared_ptr<struct value_t>(
                dupval(value), value_deleter());
            context->globals->emplace_back(name, std::move(transfer));
        }
    }
    return true;
}


bool note_type(type_map &types, std::vector<type_record> &order,
               const struct struct_def *def, std::string &error,
               type_collect_context *context = nullptr);

bool note_type(type_map &types, std::vector<type_record> &order,
               const struct enum_def *def, std::string &error,
               type_collect_context *context = nullptr);

bool note_type(type_map &types, std::vector<type_record> &order,
               const struct struct_def *def, std::string &error,
               type_collect_context *context) {
    if (!def)
        return true;
    std::string key = type_identity(def->module, def->name);
    if (types.count(key))
        return true;
    type_record record;
    record.structure = def;
    types.emplace(key, record);
    type_record &stored = types[key];
    for (const struct field_def &field : def->fields) {
        if (!field.default_value)
            continue;
        std::shared_ptr<thread_transfer_function> function;
        if (!build_simple_function(field.default_value, "field default",
                                   function, error, context))
            return false;
        stored.field_defaults.emplace(field.name, std::move(function));
    }
    for (const auto &method : def->methods) {
        if (!method.second)
            continue;
        std::shared_ptr<thread_transfer_function> function;
        if (!build_simple_function(method.second, "method", function, error,
                                   context))
            return false;
        stored.methods.emplace_back(method.first, std::move(function));
    }
    order.push_back(stored);
    return true;
}

bool note_type(type_map &types, std::vector<type_record> &order,
               const struct enum_def *def, std::string &error,
               type_collect_context *context) {
    if (!def)
        return true;
    std::string key = type_identity(def->module, def->name);
    if (types.count(key))
        return true;
    type_record record;
    record.is_enum = true;
    record.enumeration = def;
    types.emplace(key, record);
    type_record &stored = types[key];
    for (const struct variant_def &variant : def->variants) {
        for (const struct field_def &field : variant.fields) {
            if (!field.default_value)
                continue;
            std::shared_ptr<thread_transfer_function> function;
            if (!build_simple_function(field.default_value, "field default",
                                       function, error, context))
                return false;
            stored.field_defaults.emplace(
                variant.name + '\x1f' + field.name, std::move(function));
        }
    }
    for (const auto &method : def->methods) {
        if (!method.second)
            continue;
        std::shared_ptr<thread_transfer_function> function;
        if (!build_simple_function(method.second, "method", function, error,
                                   context))
            return false;
        stored.methods.emplace_back(method.first, std::move(function));
    }
    order.push_back(stored);
    return true;
}

bool collect_value_types(const struct value_t *value, type_map &types,
                         std::vector<type_record> &order, int depth,
                         std::string &error,
                         type_collect_context *context = nullptr) {
    if (!value || depth > kMaxDepth)
        return true;
    switch (value->type) {
        case T_struct: {
            const struct struct_value_t *structure =
                (const struct struct_value_t *)value;
            if (!note_type(types, order, structure->def, error, context))
                return false;
            for (const struct value_t *field : structure->payload->fields) {
                if (!collect_value_types(field, types, order, depth + 1,
                                         error, context))
                    return false;
            }
            break;
        }
        case T_enum: {
            const struct enum_value_t *enumeration =
                (const struct enum_value_t *)value;
            if (!note_type(types, order, enumeration->def, error, context))
                return false;
            for (const struct value_t *field : enumeration->payload->fields) {
                if (!collect_value_types(field, types, order, depth + 1,
                                         error, context))
                    return false;
            }
            break;
        }
        case T_result: {
            const struct result_value_t *result =
                (const struct result_value_t *)value;
            return collect_value_types(
                result->ok ? result->value : result->error, types, order,
                depth + 1, error, context);
        }
        case T_list: {
            const struct list_value_t *list =
                (const struct list_value_t *)value;
            for (const struct value_t *item : list->payload->items) {
                if (!collect_value_types(item, types, order, depth + 1,
                                         error, context))
                    return false;
            }
            break;
        }
        case T_dict: {
            const struct dict_value_t *dict =
                (const struct dict_value_t *)value;
            for (const auto &item : dict->payload->items) {
                if (!collect_value_types(item.first, types, order, depth + 1,
                                         error, context) ||
                    !collect_value_types(item.second, types, order, depth + 1,
                                         error, context))
                    return false;
            }
            break;
        }
        case T_lru: {
            const struct lru_value_t *lru = (const struct lru_value_t *)value;
            for (const auto &item : lru->payload->items) {
                if (!collect_value_types(item.first, types, order, depth + 1,
                                         error, context) ||
                    !collect_value_types(item.second, types, order, depth + 1,
                                         error, context))
                    return false;
            }
            break;
        }
        case T_deque: {
            const struct deque_value_t *deque =
                (const struct deque_value_t *)value;
            std::vector<struct value_t *> items;
            deque_items_in_order(deque->payload.get(), items);
            for (const struct value_t *item : items) {
                if (!collect_value_types(item, types, order, depth + 1,
                                         error, context))
                    return false;
            }
            break;
        }
        case T_set: {
            const struct set_value_t *set = (const struct set_value_t *)value;
            std::vector<struct value_t *> items;
            set_values_in_order(set->payload.get(), items);
            for (const struct value_t *item : items) {
                if (!collect_value_types(item, types, order, depth + 1,
                                         error, context))
                    return false;
            }
            break;
        }
        default:
            break;
    }
    return true;
}

bool collect_transfer_types(const thread_transfer_value &value,
                            type_map &types,
                            std::vector<type_record> &order, int depth,
                            std::string &error,
                            type_collect_context *context = nullptr) {
    if (depth > kMaxDepth)
        return true;
    if (value.kind == thread_transfer_value::Kind::Data) {
        return collect_value_types(value.data.get(), types, order, depth + 1,
                                   error, context);
    }
    if (value.kind == thread_transfer_value::Kind::Namespace) {
        for (const auto &field : value.fields) {
            if (!collect_transfer_types(field.second, types, order, depth + 1,
                                        error, context))
                return false;
        }
    } else if (value.kind == thread_transfer_value::Kind::Heap) {
        if (value.heap) {
            if (!collect_transfer_types(value.heap->less, types, order,
                                        depth + 1, error, context))
                return false;
            for (const thread_transfer_heap_node &node : value.heap->nodes) {
                if (!collect_transfer_types(node.value, types, order,
                                            depth + 1, error, context))
                    return false;
            }
        }
    } else if (value.kind == thread_transfer_value::Kind::Container) {
        if (value.container) {
            for (const thread_transfer_value &item :
                 value.container->items) {
                if (!collect_transfer_types(item, types, order, depth + 1,
                                            error, context))
                    return false;
            }
            for (const auto &entry : value.container->entries) {
                if (!collect_transfer_types(entry.first, types, order,
                                            depth + 1, error, context) ||
                    !collect_transfer_types(entry.second, types, order,
                                            depth + 1, error, context))
                    return false;
            }
        }
    }
    return true;
}

bool collect_proto_types(const skein::bc::Proto &proto, type_map &types,
                         std::vector<type_record> &order, int depth,
                         std::string &error,
                         type_collect_context *context = nullptr) {
    if (depth > kMaxDepth)
        return true;
    for (const skein::bc::Constant &constant : proto.consts) {
        if (constant.kind == skein::bc::ConstantKind::Value) {
            if (!collect_value_types(constant.value.get(), types, order,
                                     depth + 1, error, context))
                return false;
        }
    }
    for (const skein::bc::ConstructDesc &construct : proto.constructs) {
        if (construct.variant.empty()) {
            if (!note_type(types, order,
                           structtype_find_struct(construct.module.c_str(),
                                                  construct.name.c_str()),
                           error, context))
                return false;
        } else {
            if (!note_type(types, order,
                           structtype_find_enum(construct.module.c_str(),
                                                construct.name.c_str()),
                           error, context))
                return false;
        }
    }
    for (const skein::bc::FieldOrEnumDesc &field : proto.field_or_enum) {
        if (!note_type(types, order,
                       structtype_find_struct(field.module.c_str(),
                                              field.base.c_str()),
                       error, context) ||
            !note_type(types, order,
                       structtype_find_enum(field.module.c_str(),
                                            field.base.c_str()),
                       error, context))
            return false;
    }
    for (const skein::bc::PatternDesc &pattern : proto.patterns) {
        if (pattern.literal_value &&
            !collect_value_types(pattern.literal_value.get(), types, order,
                                 depth + 1, error, context))
            return false;
        if (pattern.literal_expr &&
            !collect_proto_types(*pattern.literal_expr, types, order,
                                 depth + 1, error, context))
            return false;
    }
    return true;
}

bool write_type_metadata(
    writer &out, const std::vector<type_record> &types,
    const std::unordered_map<const thread_transfer_function *, uint32_t> &ids,
    std::string &error) {
    (void)error;
    auto function_index =
        [&](const std::shared_ptr<thread_transfer_function> &function) {
            if (!function)
                return UINT32_MAX;
            auto it = ids.find(function.get());
            return it == ids.end() ? UINT32_MAX : it->second;
        };
    out.u32((uint32_t)types.size());
    for (const type_record &record : types) {
        out.u8(record.is_enum ? 1 : 0);
        if (record.is_enum) {
            const struct enum_def *def = record.enumeration;
            out.str(def->module);
            out.str(def->name);
            out.u8(def->exported ? 1 : 0);
            out.u32((uint32_t)def->variants.size());
            for (const struct variant_def &variant : def->variants) {
                out.str(variant.name);
                out.u32((uint32_t)variant.fields.size());
                for (const struct field_def &field : variant.fields) {
                    out.str(field.name);
                    write_type(out, &field.type);
                    auto it = record.field_defaults.find(
                        variant.name + '\x1f' + field.name);
                    bool has_default = it != record.field_defaults.end();
                    out.u8(has_default ? 1 : 0);
                    out.u32(has_default ? function_index(it->second)
                                        : UINT32_MAX);
                }
            }
        } else {
            const struct struct_def *def = record.structure;
            out.str(def->module);
            out.str(def->name);
            out.u8(def->exported ? 1 : 0);
            out.u32((uint32_t)def->fields.size());
            for (const struct field_def &field : def->fields) {
                out.str(field.name);
                write_type(out, &field.type);
                auto it = record.field_defaults.find(field.name);
                bool has_default = it != record.field_defaults.end();
                out.u8(has_default ? 1 : 0);
                out.u32(has_default ? function_index(it->second)
                                    : UINT32_MAX);
            }
        }
        out.u32((uint32_t)record.methods.size());
        for (const auto &method : record.methods) {
            out.str(method.first);
            out.u32(function_index(method.second));
        }
    }
    return true;
}

bool read_type_metadata(reader &in,
                        std::vector<struct thread_transfer_type> &types,
                        std::string &error) {
    uint32_t count = 0;
    if (!in.count(count)) {
        error = in.error;
        return false;
    }
    types.clear();
    types.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        thread_transfer_type type;
        uint8_t is_enum = 0;
        uint8_t exported = 0;
        if (!in.u8(is_enum) || !in.str(type.module) || !in.str(type.name) ||
            !in.u8(exported)) {
            error = in.error;
            return false;
        }
        type.is_enum = is_enum != 0;
        type.exported = exported != 0;

        if (type.is_enum) {
            uint32_t variant_count = 0;
            if (!in.count(variant_count)) {
                error = in.error;
                return false;
            }
            type.variants.reserve(variant_count);
            std::vector<variant_meta> variants;
            variants.reserve(variant_count);
            for (uint32_t v = 0; v < variant_count; ++v) {
                thread_transfer_type_variant stored_variant;
                variant_meta variant;
                if (!in.str(variant.name)) {
                    error = in.error;
                    return false;
                }
                stored_variant.name = variant.name;
                uint32_t field_count = 0;
                if (!in.count(field_count)) {
                    error = in.error;
                    return false;
                }
                variant.fields.reserve(field_count);
                stored_variant.fields.reserve(field_count);
                for (uint32_t f = 0; f < field_count; ++f) {
                    field_meta field;
                    thread_transfer_type_field stored_field;
                    if (!in.str(field.name) ||
                        !read_type_value(in, field.type, error)) {
                        if (error.empty())
                            error = in.error;
                        return false;
                    }
                    uint8_t has_default = 0;
                    uint32_t default_index = UINT32_MAX;
                    if (!in.u8(has_default) || !in.u32(default_index)) {
                        error = in.error;
                        return false;
                    }
                    stored_field.name = field.name;
                    stored_field.type = field.type;
                    stored_field.has_default = has_default != 0;
                    stored_field.default_index = default_index;
                    variant.fields.push_back(std::move(field));
                    stored_variant.fields.push_back(std::move(stored_field));
                }
                variants.push_back(std::move(variant));
                type.variants.push_back(std::move(stored_variant));
            }
            if (!structtype_find_enum(type.module.c_str(), type.name.c_str())) {
                std::string declare_error;
                if (!declare_enum_meta(type.module.c_str(),
                                       type.module.c_str(),
                                       type.name.c_str(), type.exported,
                                       variants, nullptr, declare_error)) {
                    error = declare_error.empty()
                                ? "cannot register thread payload enum type"
                                : declare_error;
                    return false;
                }
            }
        } else {
            uint32_t field_count = 0;
            if (!in.count(field_count)) {
                error = in.error;
                return false;
            }
            std::vector<field_meta> fields;
            fields.reserve(field_count);
            type.fields.reserve(field_count);
            for (uint32_t f = 0; f < field_count; ++f) {
                field_meta field;
                thread_transfer_type_field stored_field;
                if (!in.str(field.name) ||
                    !read_type_value(in, field.type, error)) {
                    if (error.empty())
                        error = in.error;
                    return false;
                }
                uint8_t has_default = 0;
                uint32_t default_index = UINT32_MAX;
                if (!in.u8(has_default) || !in.u32(default_index)) {
                    error = in.error;
                    return false;
                }
                stored_field.name = field.name;
                stored_field.type = field.type;
                stored_field.has_default = has_default != 0;
                stored_field.default_index = default_index;
                fields.push_back(std::move(field));
                type.fields.push_back(std::move(stored_field));
            }
            if (!structtype_find_struct(type.module.c_str(),
                                        type.name.c_str())) {
                std::string declare_error;
                if (!declare_struct_meta(type.module.c_str(),
                                         type.module.c_str(), type.name.c_str(),
                                         type.exported, fields, nullptr,
                                         declare_error)) {
                    error = declare_error.empty()
                                ? "cannot register thread payload struct type"
                                : declare_error;
                    return false;
                }
            }
        }

        uint32_t method_count = 0;
        if (!in.count(method_count)) {
            error = in.error;
            return false;
        }
        type.methods.reserve(method_count);
        type.method_indices.reserve(method_count);
        for (uint32_t m = 0; m < method_count; ++m) {
            std::string name;
            uint32_t index = UINT32_MAX;
            if (!in.str(name) || !in.u32(index)) {
                error = in.error;
                return false;
            }
            type.methods.emplace_back(std::move(name), nullptr);
            type.method_indices.push_back(index);
        }
        types.push_back(std::move(type));
    }
    return true;
}

bool write_proto(writer &out, const skein::bc::Proto &proto,
                 std::string &error);
bool read_proto(reader &in, std::shared_ptr<skein::bc::Proto> &proto,
                std::string &error);

bool write_pattern(writer &out, const skein::bc::PatternDesc &pattern,
                   std::string &error) {
    out.u8((uint8_t)pattern.kind);
    out.str(pattern.name);
    out.str(pattern.module);
    out.str(pattern.type_name);
    out.str(pattern.variant);
    out.u8(pattern.kind == skein::bc::PatternDesc::Kind::Type ? 1 : 0);
    if (pattern.kind == skein::bc::PatternDesc::Kind::Type)
        write_type(out, &pattern.type);
    out.i64((int64_t)pattern.range_start);
    out.i64((int64_t)pattern.range_end);
    out.u8(pattern.literal_value ? 1 : 0);
    if (pattern.literal_value &&
        !write_value(out, pattern.literal_value.get(), 0, error))
        return false;
    out.u8(pattern.literal_expr ? 1 : 0);
    if (pattern.literal_expr) {
        if (!write_proto(out, *pattern.literal_expr, error))
            return false;
    }
    out.u32((uint32_t)pattern.items.size());
    for (uint32_t item : pattern.items)
        out.u32(item);
    out.u32(pattern.child);
    out.u32((uint32_t)pattern.fields.size());
    for (const skein::bc::PatternFieldDesc &field : pattern.fields) {
        out.str(field.key);
        out.str(field.rest_name);
        out.u32(field.pattern);
        out.u8(field.rest ? 1 : 0);
    }
    out.u32((uint32_t)pattern.binding_slots.size());
    for (const skein::bc::PatternSlotDesc &slot : pattern.binding_slots) {
        out.str(slot.name);
        out.u32(slot.slot);
    }
    out.u8(pattern.is_dict ? 1 : 0);
    return true;
}

bool read_pattern(reader &in, skein::bc::PatternDesc &pattern,
                  std::string &error) {
    uint8_t kind = 0;
    if (!in.u8(kind) || !in.str(pattern.name) || !in.str(pattern.module) ||
        !in.str(pattern.type_name) || !in.str(pattern.variant)) {
        error = in.error;
        return false;
    }
    pattern.kind = (skein::bc::PatternDesc::Kind)kind;
    uint8_t has_type = 0;
    if (!in.u8(has_type)) {
        error = in.error;
        return false;
    }
    if (has_type) {
        if (!read_type_value(in, pattern.type, error))
            return false;
    } else {
        type_constraint_set_any(&pattern.type);
    }
    int64_t range_start = 0;
    int64_t range_end = 0;
    if (!in.i64(range_start) || !in.i64(range_end)) {
        error = in.error;
        return false;
    }
    pattern.range_start = (long long)range_start;
    pattern.range_end = (long long)range_end;

    uint8_t has_literal = 0;
    if (!in.u8(has_literal)) {
        error = in.error;
        return false;
    }
    if (has_literal) {
        value_owner value;
        if (!read_value(in, value, 0, error))
            return false;
        pattern.literal_value = take_owned(value);
    }
    uint8_t has_expr = 0;
    if (!in.u8(has_expr)) {
        error = in.error;
        return false;
    }
    if (has_expr) {
        std::shared_ptr<skein::bc::Proto> expr;
        if (!read_proto(in, expr, error))
            return false;
        pattern.literal_expr = std::move(expr);
    }

    uint32_t item_count = 0;
    if (!in.count(item_count)) {
        error = in.error;
        return false;
    }
    pattern.items.reserve(item_count);
    for (uint32_t i = 0; i < item_count; ++i) {
        uint32_t item = 0;
        if (!in.u32(item)) {
            error = in.error;
            return false;
        }
        pattern.items.push_back(item);
    }
    if (!in.u32(pattern.child)) {
        error = in.error;
        return false;
    }
    uint32_t field_count = 0;
    if (!in.count(field_count)) {
        error = in.error;
        return false;
    }
    pattern.fields.reserve(field_count);
    for (uint32_t i = 0; i < field_count; ++i) {
        skein::bc::PatternFieldDesc field;
        uint8_t rest = 0;
        if (!in.str(field.key) || !in.str(field.rest_name) ||
            !in.u32(field.pattern) || !in.u8(rest)) {
            error = in.error;
            return false;
        }
        field.rest = rest != 0;
        pattern.fields.push_back(std::move(field));
    }
    uint32_t slot_count = 0;
    if (!in.count(slot_count)) {
        error = in.error;
        return false;
    }
    pattern.binding_slots.reserve(slot_count);
    for (uint32_t i = 0; i < slot_count; ++i) {
        skein::bc::PatternSlotDesc slot;
        if (!in.str(slot.name) || !in.u32(slot.slot)) {
            error = in.error;
            return false;
        }
        pattern.binding_slots.push_back(std::move(slot));
    }
    uint8_t is_dict = 0;
    if (!in.u8(is_dict)) {
        error = in.error;
        return false;
    }
    pattern.is_dict = is_dict != 0;
    return true;
}

bool write_transfer(
    writer &out, const thread_transfer_value &value,
    const std::unordered_map<const thread_transfer_function *, uint32_t> &ids,
    int depth, std::string &error) {
    if (depth > kMaxDepth) {
        error = "thread payload function nesting is too deep";
        return false;
    }
    switch (value.kind) {
        case thread_transfer_value::Kind::Null:
            out.u8(0);
            return true;
        case thread_transfer_value::Kind::Data:
            out.u8(1);
            return write_value(out, value.data.get(), depth + 1, error);
        case thread_transfer_value::Kind::Namespace:
            out.u8(2);
            out.u32((uint32_t)value.fields.size());
            for (const auto &field : value.fields) {
                out.str(field.first);
                if (!write_transfer(out, field.second, ids, depth + 1, error))
                    return false;
            }
            return true;
        case thread_transfer_value::Kind::Function: {
            out.u8(3);
            auto it = ids.find(value.function.get());
            if (it == ids.end()) {
                error = "thread payload refers to an unknown function";
                return false;
            }
            out.u32(it->second);
            return true;
        }
        case thread_transfer_value::Kind::Heap: {
            if (!value.heap) {
                error = "invalid thread payload heap";
                return false;
            }
            out.u8(4);
            if (!write_transfer(out, value.heap->less, ids, depth + 1, error))
                return false;
            out.u32(value.heap->root);
            out.u64(value.heap->size);
            out.u32((uint32_t)value.heap->nodes.size());
            for (const thread_transfer_heap_node &node :
                 value.heap->nodes) {
                if (!write_transfer(out, node.value, ids, depth + 1, error))
                    return false;
                out.u32(node.rank);
                out.u32(node.left);
                out.u32(node.right);
            }
            return true;
        }
        case thread_transfer_value::Kind::Container: {
            if (!value.container) {
                error = "invalid thread payload container";
                return false;
            }
            out.u8(5);
            out.u8((uint8_t)value.container->kind);
            if (value.container->kind ==
                    thread_transfer_container::Kind::Dict ||
                value.container->kind ==
                    thread_transfer_container::Kind::Lru) {
                out.u64(value.container->capacity);
                out.u32((uint32_t)value.container->entries.size());
                for (const auto &entry : value.container->entries) {
                    if (!write_transfer(out, entry.first, ids, depth + 1,
                                        error) ||
                        !write_transfer(out, entry.second, ids, depth + 1,
                                        error))
                        return false;
                }
            } else {
                out.u32((uint32_t)value.container->items.size());
                for (const thread_transfer_value &item :
                     value.container->items) {
                    if (!write_transfer(out, item, ids, depth + 1, error))
                        return false;
                }
            }
            return true;
        }
    }
    error = "invalid thread payload transfer kind";
    return false;
}

bool read_transfer(
    reader &in,
    const std::vector<std::shared_ptr<thread_transfer_function>> &functions,
    thread_transfer_value &value, int depth, std::string &error) {
    if (depth > kMaxDepth) {
        error = "thread payload function nesting is too deep";
        return false;
    }
    uint8_t kind = 0;
    if (!in.u8(kind)) {
        error = in.error;
        return false;
    }
    switch (kind) {
        case 0:
            value = thread_transfer_value();
            return true;
        case 1: {
            value_owner data;
            if (!read_value(in, data, depth + 1, error))
                return false;
            value = thread_transfer_value();
            value.kind = thread_transfer_value::Kind::Data;
            value.data = take_owned(data);
            return true;
        }
        case 2: {
            uint32_t count = 0;
            if (!in.count(count)) {
                error = in.error;
                return false;
            }
            value = thread_transfer_value();
            value.kind = thread_transfer_value::Kind::Namespace;
            value.fields.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                std::string name;
                if (!in.str(name)) {
                    error = in.error;
                    return false;
                }
                thread_transfer_value field;
                if (!read_transfer(in, functions, field, depth + 1, error))
                    return false;
                value.fields.emplace_back(std::move(name), std::move(field));
            }
            return true;
        }
        case 3: {
            uint32_t index = 0;
            if (!in.u32(index)) {
                error = in.error;
                return false;
            }
            if (index >= functions.size()) {
                error = "thread payload function index is out of range";
                return false;
            }
            value = thread_transfer_value();
            value.kind = thread_transfer_value::Kind::Function;
            value.function = functions[index];
            return true;
        }
        case 4: {
            std::shared_ptr<thread_transfer_heap> heap =
                std::make_shared<thread_transfer_heap>();
            if (!read_transfer(in, functions, heap->less, depth + 1, error))
                return false;
            uint32_t node_count = 0;
            if (!in.u32(heap->root) || !in.u64(heap->size) ||
                !in.count(node_count)) {
                error = in.error;
                return false;
            }
            heap->nodes.resize(node_count);
            for (uint32_t i = 0; i < node_count; ++i) {
                thread_transfer_heap_node &node = heap->nodes[i];
                if (!read_transfer(in, functions, node.value, depth + 1,
                                   error) ||
                    !in.u32(node.rank) || !in.u32(node.left) ||
                    !in.u32(node.right)) {
                    if (error.empty())
                        error = in.error;
                    return false;
                }
                if ((node.left != UINT32_MAX && node.left >= node_count) ||
                    (node.right != UINT32_MAX && node.right >= node_count)) {
                    error = "thread payload heap child index is out of range";
                    return false;
                }
            }
            if (node_count == 0) {
                if (heap->root != UINT32_MAX) {
                    error = "thread payload heap root is out of range";
                    return false;
                }
            } else if (heap->root >= node_count) {
                error = "thread payload heap root is out of range";
                return false;
            }
            if (node_count > 0) {
                std::vector<uint8_t> state(node_count, 0);
                std::vector<std::pair<uint32_t, bool>> stack;
                stack.emplace_back(heap->root, false);
                while (!stack.empty()) {
                    uint32_t index = stack.back().first;
                    bool exiting = stack.back().second;
                    stack.pop_back();
                    if (exiting) {
                        state[index] = 2;
                        continue;
                    }
                    if (state[index] == 1) {
                        error = "thread payload heap contains a cycle";
                        return false;
                    }
                    if (state[index] == 2)
                        continue;
                    state[index] = 1;
                    stack.emplace_back(index, true);
                    const thread_transfer_heap_node &node =
                        heap->nodes[index];
                    if (node.left != UINT32_MAX)
                        stack.emplace_back(node.left, false);
                    if (node.right != UINT32_MAX)
                        stack.emplace_back(node.right, false);
                }
            }
            value = thread_transfer_value();
            value.kind = thread_transfer_value::Kind::Heap;
            value.heap = std::move(heap);
            return true;
        }
        case 5: {
            uint8_t raw_kind = 0;
            if (!in.u8(raw_kind)) {
                error = in.error;
                return false;
            }
            if (raw_kind > (uint8_t)thread_transfer_container::Kind::Lru) {
                error = "invalid thread payload container kind";
                return false;
            }
            std::shared_ptr<thread_transfer_container> container =
                std::make_shared<thread_transfer_container>();
            container->kind = (thread_transfer_container::Kind)raw_kind;
            value = thread_transfer_value();
            value.kind = thread_transfer_value::Kind::Container;
            value.container = container;
            if (container->kind == thread_transfer_container::Kind::Dict ||
                container->kind == thread_transfer_container::Kind::Lru) {
                uint64_t capacity = 0;
                uint32_t count = 0;
                if (!in.u64(capacity) || !in.count(count)) {
                    error = in.error;
                    return false;
                }
                container->capacity = capacity;
                container->entries.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    thread_transfer_value key;
                    thread_transfer_value item;
                    if (!read_transfer(in, functions, key, depth + 1, error) ||
                        !read_transfer(in, functions, item, depth + 1, error))
                        return false;
                    container->entries.emplace_back(std::move(key),
                                                    std::move(item));
                }
            } else {
                uint32_t count = 0;
                if (!in.count(count)) {
                    error = in.error;
                    return false;
                }
                container->items.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    thread_transfer_value item;
                    if (!read_transfer(in, functions, item, depth + 1, error))
                        return false;
                    container->items.push_back(std::move(item));
                }
            }
            return true;
        }
        default:
            error = "invalid thread payload transfer tag";
            return false;
    }
}

bool build_decl_function(const std::shared_ptr<function_code> &code,
                         const char *what,
                         std::shared_ptr<thread_transfer_function> &out,
                         std::string &error,
                         type_collect_context *context) {
    if (!code) {
        out.reset();
        return true;
    }
    std::shared_ptr<function_def> def = std::make_shared<function_def>();
    def->code = code;
    return build_simple_function(def, what, out, error, context, true);
}

bool prepare_inline_functions(const skein::bc::Proto &proto,
                              type_collect_context *context,
                              std::string &error) {
    static thread_local std::unordered_set<const skein::bc::Proto *> visiting;
    if (!visiting.insert(&proto).second)
        return true;
    struct visiting_guard {
        std::unordered_set<const skein::bc::Proto *> &set;
        const skein::bc::Proto *proto;
        ~visiting_guard() { set.erase(proto); }
    } guard{visiting, &proto};

    auto prepare = [&](const std::shared_ptr<function_code> &code,
                       const char *what) -> bool {
        if (!code || g_inline_function_payloads.count(code.get()))
            return true;
        std::shared_ptr<thread_transfer_function> function;
        if (!build_decl_function(code, what, function, error, context))
            return false;
        g_inline_function_payloads.emplace(code.get(), function);
        return prepare_inline_functions(*function->proto, context, error);
    };
    for (const skein::bc::StructDeclMeta &decl : proto.struct_decls) {
        for (const skein::bc::DeclFieldMeta &field : decl.fields) {
            if (!prepare(field.default_code, "struct field default"))
                return false;
        }
    }
    for (const skein::bc::EnumDeclMeta &decl : proto.enum_decls) {
        for (const skein::bc::EnumVariantMeta &variant : decl.variants) {
            for (const skein::bc::DeclFieldMeta &field : variant.fields) {
                if (!prepare(field.default_code, "enum field default"))
                    return false;
            }
        }
    }
    for (const skein::bc::ImplDeclMeta &decl : proto.impl_decls) {
        for (const skein::bc::DeclMethodMeta &method : decl.methods) {
            if (!prepare(method.code, "impl method"))
                return false;
        }
    }
    for (const skein::bc::Constant &constant : proto.consts) {
        if (!prepare(constant.function, "nested function"))
            return false;
    }
    for (const std::shared_ptr<const skein::bc::Proto> &defer :
         proto.defer_protos) {
        if (defer && !prepare_inline_functions(*defer, context, error))
            return false;
    }
    return true;
}

bool write_inline_function(writer &out,
                           const std::shared_ptr<function_code> &code,
                           const char *what,
                           type_collect_context *context,
                           std::string &error) {
    out.u8(code ? 1 : 0);
    if (!code)
        return true;
    std::shared_ptr<thread_transfer_function> function;
    auto prepared = g_inline_function_payloads.find(code.get());
    if (prepared != g_inline_function_payloads.end())
        function = prepared->second;
    else if (!build_decl_function(code, what, function, error, context))
        return false;
    if (!write_proto(out, *function->proto, error))
        return false;
    std::unordered_map<const thread_transfer_function *, uint32_t> no_functions;
    out.u32((uint32_t)function->captures.size());
    for (const thread_transfer_value &capture : function->captures) {
        if (!write_transfer(out, capture, no_functions, 0, error))
            return false;
    }
    out.u32((uint32_t)function->defaults.size());
    for (const thread_transfer_value &value : function->defaults) {
        if (!write_transfer(out, value, no_functions, 0, error))
            return false;
    }
    out.u8(function->pattern_proto ? 1 : 0);
    if (function->pattern_proto &&
        !write_proto(out, *function->pattern_proto, error))
        return false;
    out.u32((uint32_t)function->pattern_roots.size());
    for (uint32_t root : function->pattern_roots)
        out.u32(root);
    return true;
}

bool read_inline_function(reader &in, std::shared_ptr<function_code> &code,
                          std::string &error) {
    uint8_t present = 0;
    if (!in.u8(present)) {
        error = in.error;
        return false;
    }
    if (!present) {
        code.reset();
        return true;
    }
    std::shared_ptr<thread_transfer_function> function =
        std::make_shared<thread_transfer_function>();
    std::shared_ptr<skein::bc::Proto> proto;
    if (!read_proto(in, proto, error))
        return false;
    function->proto = proto;
    std::vector<std::shared_ptr<thread_transfer_function>> no_functions;
    uint32_t capture_count = 0;
    if (!in.count(capture_count)) {
        error = in.error;
        return false;
    }
    function->captures.reserve(capture_count);
    for (uint32_t i = 0; i < capture_count; ++i) {
        thread_transfer_value capture;
        if (!read_transfer(in, no_functions, capture, 0, error))
            return false;
        function->captures.push_back(std::move(capture));
    }
    uint32_t default_count = 0;
    if (!in.count(default_count)) {
        error = in.error;
        return false;
    }
    function->defaults.reserve(default_count);
    for (uint32_t i = 0; i < default_count; ++i) {
        thread_transfer_value value;
        if (!read_transfer(in, no_functions, value, 0, error))
            return false;
        function->defaults.push_back(std::move(value));
    }
    uint8_t has_patterns = 0;
    if (!in.u8(has_patterns)) {
        error = in.error;
        return false;
    }
    if (has_patterns) {
        std::shared_ptr<skein::bc::Proto> pattern_proto;
        if (!read_proto(in, pattern_proto, error))
            return false;
        function->pattern_proto = std::move(pattern_proto);
    }
    uint32_t root_count = 0;
    if (!in.count(root_count)) {
        error = in.error;
        return false;
    }
    function->pattern_roots.reserve(root_count);
    for (uint32_t i = 0; i < root_count; ++i) {
        uint32_t root = 0;
        if (!in.u32(root)) {
            error = in.error;
            return false;
        }
        function->pattern_roots.push_back(root);
    }
    code = skein_vm_make_transferred_function_code(*function, error);
    return code != nullptr;
}

bool is_declaration_function(const skein::bc::Proto &proto,
                             const function_code *code) {
    if (!code)
        return false;
    for (const skein::bc::StructDeclMeta &decl : proto.struct_decls) {
        for (const skein::bc::DeclFieldMeta &field : decl.fields) {
            if (field.default_code.get() == code)
                return true;
        }
    }
    for (const skein::bc::EnumDeclMeta &decl : proto.enum_decls) {
        for (const skein::bc::EnumVariantMeta &variant : decl.variants) {
            for (const skein::bc::DeclFieldMeta &field : variant.fields) {
                if (field.default_code.get() == code)
                    return true;
            }
        }
    }
    for (const skein::bc::ImplDeclMeta &decl : proto.impl_decls) {
        for (const skein::bc::DeclMethodMeta &method : decl.methods) {
            if (method.code.get() == code)
                return true;
        }
    }
    return false;
}

bool write_proto(writer &out, const skein::bc::Proto &proto,
                 std::string &error) {
    if (!proto.packages.empty()) {
        error = "cannot serialize a Proto with nested packages";
        return false;
    }

    out.str(proto.name);
    out.str(proto.module);
    out.u32((uint32_t)proto.def_line);
    out.u32((uint32_t)proto.def_column);
    out.u32(proto.version);

    out.u32((uint32_t)proto.code.size());
    for (const skein::bc::Instr &ins : proto.code) {
        out.u8(ins.op);
        out.u8(ins.flags);
        out.u16(ins.a);
        out.u32(ins.b);
    }

    out.u32((uint32_t)proto.consts.size());
    for (const skein::bc::Constant &constant : proto.consts) {
        out.u8((uint8_t)constant.kind);
        switch (constant.kind) {
            case skein::bc::ConstantKind::Value:
                if (!write_value(out, constant.value.get(), 0, error))
                    return false;
                break;
            case skein::bc::ConstantKind::Name:
                out.str(constant.name);
                break;
            case skein::bc::ConstantKind::Type:
                if (!write_type(out, constant.type))
                    return false;
                break;
            case skein::bc::ConstantKind::Function:
                if (is_declaration_function(proto, constant.function.get())) {
                    /* Declaration functions are serialized through the
                     * struct/enum/impl metadata below; keep a placeholder in
                     * the constant table so indices stay stable. */
                    out.u8(0);
                    break;
                }
                if (!write_inline_function(out, constant.function,
                                           "nested function", nullptr, error))
                    return false;
                break;
        }
        if (constant.kind != skein::bc::ConstantKind::Type) {
            if (!write_type(out, constant.type))
                return false;
        }
    }

    out.u32((uint32_t)proto.locs.size());
    for (const skein::bc::SourceLoc &loc : proto.locs) {
        out.u32((uint32_t)loc.line);
        out.u32((uint32_t)loc.column);
    }

    out.u32((uint32_t)proto.iters.size());
    for (const skein::bc::IterDesc &iter : proto.iters) {
        out.str(iter.key_name);
        out.str(iter.value_name);
        out.u8(iter.two_vars ? 1 : 0);
        out.u32(iter.key_slot);
        out.u32(iter.value_slot);
    }

    out.u32((uint32_t)proto.index_ops.size());
    for (const skein::bc::IndexDesc &index : proto.index_ops) {
        out.str(index.base_name);
        out.u32((uint32_t)index.fields.size());
        for (bool field : index.fields)
            out.u8(field ? 1 : 0);
        out.u8((uint8_t)index.base_kind);
        out.u32(index.base_index);
    }

    out.u32((uint32_t)proto.imports.size());
    for (const skein::bc::ImportDesc &import : proto.imports) {
        out.str(import.path);
        out.str(import.alias);
        out.str(import.base);
    }

    out.u32((uint32_t)proto.constructs.size());
    for (const skein::bc::ConstructDesc &construct : proto.constructs) {
        out.str(construct.module);
        out.str(construct.name);
        out.str(construct.variant);
        out.u32((uint32_t)construct.fields.size());
        for (const std::string &field : construct.fields)
            out.str(field);
        out.u32((uint32_t)construct.spread.size());
        for (bool spread : construct.spread)
            out.u8(spread ? 1 : 0);
    }
    out.u32((uint32_t)proto.field_or_enum.size());
    for (const skein::bc::FieldOrEnumDesc &field : proto.field_or_enum) {
        out.str(field.module);
        out.str(field.base);
        out.str(field.field);
    }

    out.u32((uint32_t)proto.call_descs.size());
    for (const skein::bc::CallDesc &call : proto.call_descs) {
        out.u32((uint32_t)call.spread.size());
        for (bool spread : call.spread)
            out.u8(spread ? 1 : 0);
    }

    out.u32((uint32_t)proto.patterns.size());
    for (const skein::bc::PatternDesc &pattern : proto.patterns) {
        if (!write_pattern(out, pattern, error))
            return false;
    }
    out.u32((uint32_t)proto.match_arms.size());
    for (const auto &arms : proto.match_arms) {
        out.u32((uint32_t)arms.size());
        for (const skein::bc::MatchArmDesc &arm : arms) {
            out.u32(arm.pattern);
            out.u8(arm.has_guard ? 1 : 0);
        }
    }

    out.u32((uint32_t)proto.param_names.size());
    for (const std::string &name : proto.param_names)
        out.str(name);
    out.u32((uint32_t)proto.upvalues.size());
    for (const std::string &name : proto.upvalues)
        out.str(name);

    out.u32((uint32_t)proto.slots.size());
    for (const skein::bc::SlotDesc &slot : proto.slots) {
        out.str(slot.name);
        write_type(out, &slot.type);
        out.u8(slot.is_const ? 1 : 0);
    }

    out.u32((uint32_t)proto.decls.size());
    for (const skein::bc::DeclDesc &decl : proto.decls) {
        out.str(decl.name);
        write_type(out, &decl.type);
        out.u8(decl.exported ? 1 : 0);
        out.u8(decl.is_const ? 1 : 0);
    }

    out.u8(proto.uses_names ? 1 : 0);
    out.u8(proto.uses_params ? 1 : 0);
    out.u8(proto.uses_defers ? 1 : 0);
    out.u8(proto.uses_slots ? 1 : 0);
    out.u8(proto.slots_direct_safe ? 1 : 0);
    out.u8(proto.params_direct ? 1 : 0);
    out.u8(proto.has_closures ? 1 : 0);
    out.u8(proto.uses_handlers ? 1 : 0);
    out.u8(proto.variadic ? 1 : 0);
    out.u8(proto.is_generator ? 1 : 0);
    out.u8(proto.is_method ? 1 : 0);
    out.u32(proto.nparams);
    out.u32(proto.required_params);
    out.u32(proto.nlocals);
    out.u32(proto.max_stack);

    out.u32((uint32_t)proto.stack_depth.size());
    for (int depth : proto.stack_depth)
        out.u32((uint32_t)depth);

    out.u32((uint32_t)proto.struct_decls.size());
    for (const skein::bc::StructDeclMeta &decl : proto.struct_decls) {
        out.str(decl.module);
        out.str(decl.name);
        out.u8(decl.exported ? 1 : 0);
        out.u32((uint32_t)decl.fields.size());
        for (const skein::bc::DeclFieldMeta &field : decl.fields) {
            out.str(field.name);
            if (!write_type(out, &field.type))
                return false;
            if (!write_inline_function(out, field.default_code,
                                       "struct field default", nullptr,
                                       error))
                return false;
        }
    }
    out.u32((uint32_t)proto.enum_decls.size());
    for (const skein::bc::EnumDeclMeta &decl : proto.enum_decls) {
        out.str(decl.module);
        out.str(decl.name);
        out.u8(decl.exported ? 1 : 0);
        out.u32((uint32_t)decl.variants.size());
        for (const skein::bc::EnumVariantMeta &variant : decl.variants) {
            out.str(variant.name);
            out.u32((uint32_t)variant.fields.size());
            for (const skein::bc::DeclFieldMeta &field : variant.fields) {
                out.str(field.name);
                if (!write_type(out, &field.type))
                    return false;
                if (!write_inline_function(out, field.default_code,
                                           "enum field default", nullptr,
                                           error))
                    return false;
            }
        }
    }
    out.u32((uint32_t)proto.trait_decls.size());
    for (const skein::bc::TraitDeclMeta &decl : proto.trait_decls) {
        out.str(decl.module);
        out.str(decl.name);
        out.u8(decl.exported ? 1 : 0);
        out.u32((uint32_t)decl.methods.size());
        for (const skein::bc::TraitMethodMeta &method : decl.methods) {
            out.str(method.name);
            out.u32((uint32_t)method.params.size());
            for (const skein::bc::TraitParamMeta &param : method.params) {
                out.str(param.name);
                if (!write_type(out, &param.type))
                    return false;
                out.u8(param.variadic ? 1 : 0);
                out.u8(param.has_default ? 1 : 0);
            }
            if (!write_inline_function(out, method.code,
                                       "trait default method", nullptr,
                                       error))
                return false;
        }
    }
    out.u32((uint32_t)proto.impl_decls.size());
    for (const skein::bc::ImplDeclMeta &decl : proto.impl_decls) {
        out.str(decl.module);
        out.str(decl.type_name);
        out.str(decl.trait_name);
        out.u8(decl.has_trait ? 1 : 0);
        out.u32((uint32_t)decl.methods.size());
        for (const skein::bc::DeclMethodMeta &method : decl.methods) {
            out.str(method.name);
            if (!write_inline_function(out, method.code, "impl method",
                                       nullptr, error))
                return false;
        }
    }

    out.u32((uint32_t)proto.defer_protos.size());
    for (const std::shared_ptr<const skein::bc::Proto> &defer :
         proto.defer_protos) {
        out.u8(defer ? 1 : 0);
        if (defer && !write_proto(out, *defer, error))
            return false;
    }
    out.u32((uint32_t)proto.defer_captures.size());
    for (const std::vector<skein::bc::DeferCapture> &captures :
         proto.defer_captures) {
        out.u32((uint32_t)captures.size());
        for (const skein::bc::DeferCapture &capture : captures) {
            out.str(capture.name);
            out.u8((uint8_t)capture.kind);
            out.u32(capture.index);
        }
    }
    return true;
}

bool read_proto(reader &in, std::shared_ptr<skein::bc::Proto> &proto,
                std::string &error) {
    proto = std::make_shared<skein::bc::Proto>();

    if (!in.str(proto->name) || !in.str(proto->module))
        return false;
    uint32_t line = 0;
    uint32_t column = 0;
    if (!in.u32(line) || !in.u32(column) || !in.u32(proto->version)) {
        error = in.error;
        return false;
    }
    proto->def_line = (int)(int32_t)line;
    proto->def_column = (int)(int32_t)column;

    uint32_t code_count = 0;
    if (!in.count(code_count)) {
        error = in.error;
        return false;
    }
    proto->code.reserve(code_count);
    for (uint32_t i = 0; i < code_count; ++i) {
        skein::bc::Instr ins;
        if (!in.u8(ins.op) || !in.u8(ins.flags) || !in.u16(ins.a) ||
            !in.u32(ins.b)) {
            error = in.error;
            return false;
        }
        proto->code.push_back(ins);
    }
    uint32_t index_cache_slots = 0;
    for (const skein::bc::Instr &ins : proto->code) {
        if ((skein::bc::Op)ins.op != skein::bc::Op::GetIndexCached)
            continue;
        if (ins.b >= kMaxCount) {
            error = "thread payload index cache is too large";
            return false;
        }
        index_cache_slots = std::max(index_cache_slots, ins.b + 1);
    }
    proto->index_read_ic.assign(index_cache_slots, {});

    uint32_t const_count = 0;
    if (!in.count(const_count)) {
        error = in.error;
        return false;
    }
    proto->consts.reserve(const_count);
    for (uint32_t i = 0; i < const_count; ++i) {
        uint8_t kind = 0;
        if (!in.u8(kind)) {
            error = in.error;
            return false;
        }
        skein::bc::Constant constant;
        constant.kind = (skein::bc::ConstantKind)kind;
        switch (constant.kind) {
            case skein::bc::ConstantKind::Value: {
                value_owner value;
                if (!read_value(in, value, 0, error))
                    return false;
                constant.value = take_owned(value);
                break;
            }
            case skein::bc::ConstantKind::Name:
                if (!in.str(constant.name))
                    return false;
                break;
            case skein::bc::ConstantKind::Type: {
                owned_type type;
                if (!read_type_ptr(in, type, error))
                    return false;
                constant.type_storage = type;
                constant.type = type.get();
                break;
            }
            case skein::bc::ConstantKind::Function:
                if (!read_inline_function(in, constant.function, error))
                    return false;
                break;
        }
        if (constant.kind != skein::bc::ConstantKind::Type) {
            owned_type type;
            if (!read_type_ptr(in, type, error))
                return false;
            if (type) {
                constant.type_storage = type;
                constant.type = type.get();
            }
        }
        proto->consts.push_back(std::move(constant));
    }

    uint32_t loc_count = 0;
    if (!in.count(loc_count)) {
        error = in.error;
        return false;
    }
    proto->locs.reserve(loc_count);
    for (uint32_t i = 0; i < loc_count; ++i) {
        uint32_t line = 0;
        uint32_t column = 0;
        if (!in.u32(line) || !in.u32(column)) {
            error = in.error;
            return false;
        }
        proto->locs.push_back({(int)(int32_t)line, (int)(int32_t)column});
    }

    uint32_t iter_count = 0;
    if (!in.count(iter_count)) {
        error = in.error;
        return false;
    }
    proto->iters.reserve(iter_count);
    for (uint32_t i = 0; i < iter_count; ++i) {
        skein::bc::IterDesc iter;
        uint8_t two = 0;
        if (!in.str(iter.key_name) || !in.str(iter.value_name) ||
            !in.u8(two) || !in.u32(iter.key_slot) ||
            !in.u32(iter.value_slot)) {
            error = in.error;
            return false;
        }
        iter.two_vars = two != 0;
        proto->iters.push_back(std::move(iter));
    }

    uint32_t index_count = 0;
    if (!in.count(index_count)) {
        error = in.error;
        return false;
    }
    proto->index_ops.reserve(index_count);
    for (uint32_t i = 0; i < index_count; ++i) {
        skein::bc::IndexDesc index;
        uint32_t field_count = 0;
        uint8_t kind = 0;
        if (!in.str(index.base_name) || !in.count(field_count)) {
            error = in.error;
            return false;
        }
        index.fields.reserve(field_count);
        for (uint32_t j = 0; j < field_count; ++j) {
            uint8_t field = 0;
            if (!in.u8(field)) {
                error = in.error;
                return false;
            }
            index.fields.push_back(field != 0);
        }
        if (!in.u8(kind) || !in.u32(index.base_index)) {
            error = in.error;
            return false;
        }
        index.base_kind = (skein::bc::IndexBaseKind)kind;
        proto->index_ops.push_back(std::move(index));
    }

    uint32_t import_count = 0;
    if (!in.count(import_count)) {
        error = in.error;
        return false;
    }
    proto->imports.reserve(import_count);
    for (uint32_t i = 0; i < import_count; ++i) {
        skein::bc::ImportDesc import;
        if (!in.str(import.path) || !in.str(import.alias) ||
            !in.str(import.base)) {
            error = in.error;
            return false;
        }
        proto->imports.push_back(std::move(import));
    }

    uint32_t construct_count = 0;
    if (!in.count(construct_count)) {
        error = in.error;
        return false;
    }
    proto->constructs.reserve(construct_count);
    for (uint32_t i = 0; i < construct_count; ++i) {
        skein::bc::ConstructDesc construct;
        uint32_t field_count = 0;
        uint32_t spread_count = 0;
        if (!in.str(construct.module) || !in.str(construct.name) ||
            !in.str(construct.variant) || !in.count(field_count)) {
            error = in.error;
            return false;
        }
        construct.fields.reserve(field_count);
        for (uint32_t j = 0; j < field_count; ++j) {
            std::string field;
            if (!in.str(field)) {
                error = in.error;
                return false;
            }
            construct.fields.push_back(std::move(field));
        }
        if (!in.count(spread_count)) {
            error = in.error;
            return false;
        }
        construct.spread.reserve(spread_count);
        for (uint32_t j = 0; j < spread_count; ++j) {
            uint8_t spread = 0;
            if (!in.u8(spread)) {
                error = in.error;
                return false;
            }
            construct.spread.push_back(spread != 0);
        }
        proto->constructs.push_back(std::move(construct));
    }
    uint32_t field_or_enum_count = 0;
    if (!in.count(field_or_enum_count)) {
        error = in.error;
        return false;
    }
    proto->field_or_enum.reserve(field_or_enum_count);
    for (uint32_t i = 0; i < field_or_enum_count; ++i) {
        skein::bc::FieldOrEnumDesc field;
        if (!in.str(field.module) || !in.str(field.base) ||
            !in.str(field.field)) {
            error = in.error;
            return false;
        }
        proto->field_or_enum.push_back(std::move(field));
    }

    uint32_t call_count = 0;
    if (!in.count(call_count)) {
        error = in.error;
        return false;
    }
    proto->call_descs.reserve(call_count);
    for (uint32_t i = 0; i < call_count; ++i) {
        uint32_t spread_count = 0;
        if (!in.count(spread_count)) {
            error = in.error;
            return false;
        }
        skein::bc::CallDesc call;
        call.spread.reserve(spread_count);
        for (uint32_t j = 0; j < spread_count; ++j) {
            uint8_t spread = 0;
            if (!in.u8(spread)) {
                error = in.error;
                return false;
            }
            call.spread.push_back(spread != 0);
        }
        proto->call_descs.push_back(std::move(call));
    }

    uint32_t pattern_count = 0;
    if (!in.count(pattern_count)) {
        error = in.error;
        return false;
    }
    proto->patterns.reserve(pattern_count);
    for (uint32_t i = 0; i < pattern_count; ++i) {
        skein::bc::PatternDesc pattern;
        if (!read_pattern(in, pattern, error))
            return false;
        proto->patterns.push_back(std::move(pattern));
    }

    uint32_t match_count = 0;
    if (!in.count(match_count)) {
        error = in.error;
        return false;
    }
    proto->match_arms.reserve(match_count);
    for (uint32_t i = 0; i < match_count; ++i) {
        uint32_t arm_count = 0;
        if (!in.count(arm_count)) {
            error = in.error;
            return false;
        }
        std::vector<skein::bc::MatchArmDesc> arms;
        arms.reserve(arm_count);
        for (uint32_t j = 0; j < arm_count; ++j) {
            skein::bc::MatchArmDesc arm;
            uint8_t guard = 0;
            if (!in.u32(arm.pattern) || !in.u8(guard)) {
                error = in.error;
                return false;
            }
            arm.has_guard = guard != 0;
            arms.push_back(arm);
        }
        proto->match_arms.push_back(std::move(arms));
    }

    uint32_t param_count = 0;
    if (!in.count(param_count)) {
        error = in.error;
        return false;
    }
    proto->param_names.reserve(param_count);
    for (uint32_t i = 0; i < param_count; ++i) {
        std::string name;
        if (!in.str(name)) {
            error = in.error;
            return false;
        }
        proto->param_names.push_back(std::move(name));
    }

    uint32_t upvalue_count = 0;
    if (!in.count(upvalue_count)) {
        error = in.error;
        return false;
    }
    proto->upvalues.reserve(upvalue_count);
    for (uint32_t i = 0; i < upvalue_count; ++i) {
        std::string name;
        if (!in.str(name)) {
            error = in.error;
            return false;
        }
        proto->upvalues.push_back(std::move(name));
    }

    uint32_t slot_count = 0;
    if (!in.count(slot_count)) {
        error = in.error;
        return false;
    }
    proto->slots.reserve(slot_count);
    for (uint32_t i = 0; i < slot_count; ++i) {
        skein::bc::SlotDesc slot;
        uint8_t is_const = 0;
        if (!in.str(slot.name) || !read_type_value(in, slot.type, error) ||
            !in.u8(is_const)) {
            error = error.empty() ? in.error : error;
            return false;
        }
        slot.is_const = is_const != 0;
        proto->slots.push_back(std::move(slot));
    }

    uint32_t decl_count = 0;
    if (!in.count(decl_count)) {
        error = in.error;
        return false;
    }
    proto->decls.reserve(decl_count);
    for (uint32_t i = 0; i < decl_count; ++i) {
        skein::bc::DeclDesc decl;
        uint8_t exported = 0;
        uint8_t is_const = 0;
        if (!in.str(decl.name) || !read_type_value(in, decl.type, error) ||
            !in.u8(exported) || !in.u8(is_const)) {
            error = error.empty() ? in.error : error;
            return false;
        }
        decl.exported = exported != 0;
        decl.is_const = is_const != 0;
        proto->decls.push_back(std::move(decl));
    }

    uint8_t flags[11] = {0};
    for (uint8_t &flag : flags) {
        if (!in.u8(flag)) {
            error = in.error;
            return false;
        }
    }
    proto->uses_names = flags[0] != 0;
    proto->uses_params = flags[1] != 0;
    proto->uses_defers = flags[2] != 0;
    proto->uses_slots = flags[3] != 0;
    proto->slots_direct_safe = flags[4] != 0;
    proto->params_direct = flags[5] != 0;
    proto->has_closures = flags[6] != 0;
    proto->uses_handlers = flags[7] != 0;
    proto->variadic = flags[8] != 0;
    proto->is_generator = flags[9] != 0;
    proto->is_method = flags[10] != 0;

    if (!in.u32(proto->nparams) || !in.u32(proto->required_params) ||
        !in.u32(proto->nlocals) || !in.u32(proto->max_stack)) {
        error = in.error;
        return false;
    }

    uint32_t depth_count = 0;
    if (!in.count(depth_count)) {
        error = in.error;
        return false;
    }
    proto->stack_depth.reserve(depth_count);
    for (uint32_t i = 0; i < depth_count; ++i) {
        uint32_t depth = 0;
        if (!in.u32(depth)) {
            error = in.error;
            return false;
        }
        proto->stack_depth.push_back((int)(int32_t)depth);
    }

    uint32_t struct_count = 0;
    if (!in.count(struct_count)) {
        error = in.error;
        return false;
    }
    proto->struct_decls.reserve(struct_count);
    for (uint32_t i = 0; i < struct_count; ++i) {
        skein::bc::StructDeclMeta decl;
        uint8_t exported = 0;
        uint32_t field_count = 0;
        if (!in.str(decl.module) || !in.str(decl.name) ||
            !in.u8(exported) || !in.count(field_count)) {
            error = in.error;
            return false;
        }
        decl.exported = exported != 0;
        decl.fields.reserve(field_count);
        for (uint32_t j = 0; j < field_count; ++j) {
            skein::bc::DeclFieldMeta field;
            if (!in.str(field.name) ||
                !read_type_value(in, field.type, error) ||
                !read_inline_function(in, field.default_code, error))
                return false;
            decl.fields.push_back(std::move(field));
        }
        proto->struct_decls.push_back(std::move(decl));
    }
    uint32_t enum_count = 0;
    if (!in.count(enum_count)) {
        error = in.error;
        return false;
    }
    proto->enum_decls.reserve(enum_count);
    for (uint32_t i = 0; i < enum_count; ++i) {
        skein::bc::EnumDeclMeta decl;
        uint8_t exported = 0;
        uint32_t variant_count = 0;
        if (!in.str(decl.module) || !in.str(decl.name) ||
            !in.u8(exported) || !in.count(variant_count)) {
            error = in.error;
            return false;
        }
        decl.exported = exported != 0;
        decl.variants.reserve(variant_count);
        for (uint32_t j = 0; j < variant_count; ++j) {
            skein::bc::EnumVariantMeta variant;
            uint32_t field_count = 0;
            if (!in.str(variant.name) || !in.count(field_count)) {
                error = in.error;
                return false;
            }
            variant.fields.reserve(field_count);
            for (uint32_t k = 0; k < field_count; ++k) {
                skein::bc::DeclFieldMeta field;
                if (!in.str(field.name) ||
                    !read_type_value(in, field.type, error) ||
                    !read_inline_function(in, field.default_code, error))
                    return false;
                variant.fields.push_back(std::move(field));
            }
            decl.variants.push_back(std::move(variant));
        }
        proto->enum_decls.push_back(std::move(decl));
    }
    uint32_t trait_count = 0;
    if (!in.count(trait_count)) {
        error = in.error;
        return false;
    }
    proto->trait_decls.reserve(trait_count);
    for (uint32_t i = 0; i < trait_count; ++i) {
        skein::bc::TraitDeclMeta decl;
        uint8_t exported = 0;
        uint32_t method_count = 0;
        if (!in.str(decl.module) || !in.str(decl.name) ||
            !in.u8(exported) || !in.count(method_count)) {
            error = in.error;
            return false;
        }
        decl.exported = exported != 0;
        decl.methods.reserve(method_count);
        for (uint32_t j = 0; j < method_count; ++j) {
            skein::bc::TraitMethodMeta method;
            uint32_t param_count = 0;
            if (!in.str(method.name) || !in.count(param_count)) {
                error = in.error;
                return false;
            }
            method.params.reserve(param_count);
            for (uint32_t k = 0; k < param_count; ++k) {
                skein::bc::TraitParamMeta param;
                uint8_t variadic = 0;
                uint8_t has_default = 0;
                if (!in.str(param.name) ||
                    !read_type_value(in, param.type, error) ||
                    !in.u8(variadic) || !in.u8(has_default)) {
                    error = in.error;
                    return false;
                }
                param.variadic = variadic != 0;
                param.has_default = has_default != 0;
                method.params.push_back(std::move(param));
            }
            if (!read_inline_function(in, method.code, error))
                return false;
            decl.methods.push_back(std::move(method));
        }
        proto->trait_decls.push_back(std::move(decl));
    }
    uint32_t impl_count = 0;
    if (!in.count(impl_count)) {
        error = in.error;
        return false;
    }
    proto->impl_decls.reserve(impl_count);
    for (uint32_t i = 0; i < impl_count; ++i) {
        skein::bc::ImplDeclMeta decl;
        uint8_t has_trait = 0;
        uint32_t method_count = 0;
        if (!in.str(decl.module) || !in.str(decl.type_name) ||
            !in.str(decl.trait_name) || !in.u8(has_trait) ||
            !in.count(method_count)) {
            error = in.error;
            return false;
        }
        decl.has_trait = has_trait != 0;
        decl.methods.reserve(method_count);
        for (uint32_t j = 0; j < method_count; ++j) {
            skein::bc::DeclMethodMeta method;
            if (!in.str(method.name) ||
                !read_inline_function(in, method.code, error))
                return false;
            decl.methods.push_back(std::move(method));
        }
        proto->impl_decls.push_back(std::move(decl));
    }

    uint32_t defer_count = 0;
    if (!in.count(defer_count)) {
        error = in.error;
        return false;
    }
    proto->defer_protos.reserve(defer_count);
    for (uint32_t i = 0; i < defer_count; ++i) {
        uint8_t present = 0;
        if (!in.u8(present)) {
            error = in.error;
            return false;
        }
        if (!present) {
            proto->defer_protos.push_back(nullptr);
            continue;
        }
        std::shared_ptr<skein::bc::Proto> defer;
        if (!read_proto(in, defer, error))
            return false;
        proto->defer_protos.push_back(std::move(defer));
    }
    uint32_t defer_capture_count = 0;
    if (!in.count(defer_capture_count)) {
        error = in.error;
        return false;
    }
    proto->defer_captures.reserve(defer_capture_count);
    for (uint32_t i = 0; i < defer_capture_count; ++i) {
        uint32_t capture_count = 0;
        if (!in.count(capture_count)) {
            error = in.error;
            return false;
        }
        std::vector<skein::bc::DeferCapture> captures;
        captures.reserve(capture_count);
        for (uint32_t j = 0; j < capture_count; ++j) {
            skein::bc::DeferCapture capture;
            uint8_t kind = 0;
            if (!in.str(capture.name) || !in.u8(kind) ||
                !in.u32(capture.index)) {
                error = in.error;
                return false;
            }
            capture.kind = (skein::bc::DeferCaptureKind)kind;
            captures.push_back(std::move(capture));
        }
        proto->defer_captures.push_back(std::move(captures));
    }

    if (proto->locs.size() != proto->code.size()) {
        error = "thread payload location table does not match bytecode";
        return false;
    }
    std::string verify_error;
    if (!skein::bc::verify(*proto, verify_error)) {
        error = "invalid thread payload bytecode: " + verify_error;
        return false;
    }
    return true;
}

void collect_transfer(
    const thread_transfer_value &value,
    std::unordered_map<const thread_transfer_function *, uint32_t> &ids,
    std::vector<std::shared_ptr<thread_transfer_function>> &order,
    int depth, bool &ok, std::string &error) {
    if (!ok)
        return;
    if (depth > kMaxDepth) {
        error = "thread payload function nesting is too deep";
        ok = false;
        return;
    }
    if (value.kind == thread_transfer_value::Kind::Function) {
        if (!value.function) {
            error = "invalid thread payload function";
            ok = false;
            return;
        }
        if (ids.count(value.function.get()))
            return;
        ids.emplace(value.function.get(), (uint32_t)order.size());
        order.push_back(value.function);
        for (const thread_transfer_value &capture :
             value.function->captures) {
            collect_transfer(capture, ids, order, depth + 1, ok, error);
            if (!ok)
                return;
        }
    } else if (value.kind == thread_transfer_value::Kind::Namespace) {
        for (const auto &field : value.fields) {
            collect_transfer(field.second, ids, order, depth + 1, ok, error);
            if (!ok)
                return;
        }
    } else if (value.kind == thread_transfer_value::Kind::Heap) {
        if (!value.heap) {
            error = "invalid thread payload heap";
            ok = false;
            return;
        }
        collect_transfer(value.heap->less, ids, order, depth + 1, ok, error);
        if (!ok)
            return;
        for (const thread_transfer_heap_node &node : value.heap->nodes) {
            collect_transfer(node.value, ids, order, depth + 1, ok, error);
            if (!ok)
                return;
        }
    } else if (value.kind == thread_transfer_value::Kind::Container) {
        if (!value.container) {
            error = "invalid thread payload container";
            ok = false;
            return;
        }
        for (const thread_transfer_value &item : value.container->items) {
            collect_transfer(item, ids, order, depth + 1, ok, error);
            if (!ok)
                return;
        }
        for (const auto &entry : value.container->entries) {
            collect_transfer(entry.first, ids, order, depth + 1, ok, error);
            if (!ok)
                return;
            collect_transfer(entry.second, ids, order, depth + 1, ok, error);
            if (!ok)
                return;
        }
    }
}

} // namespace

bool skein_vm_serialize_thread_payload(
    const std::shared_ptr<struct thread_transfer_function> &entry,
    const std::vector<std::pair<std::string, thread_transfer_value>> &globals,
    const std::vector<struct thread_transfer_module> &modules,
    std::string &bytes, std::string &error) {
    if (!entry || !entry->proto) {
        error = "invalid thread payload entry";
        return false;
    }
    g_inline_function_payloads.clear();
    struct inline_map_guard {
        ~inline_map_guard() { g_inline_function_payloads.clear(); }
    } inline_guard;
    std::unordered_map<const thread_transfer_function *, uint32_t> ids;
    std::vector<std::shared_ptr<thread_transfer_function>> order;
    thread_transfer_value entry_value;
    entry_value.kind = thread_transfer_value::Kind::Function;
    entry_value.function = entry;
    bool ok = true;
    collect_transfer(entry_value, ids, order, 0, ok, error);
    if (!ok)
        return false;
    for (const auto &item : globals) {
        collect_transfer(item.second, ids, order, 0, ok, error);
        if (!ok)
            return false;
    }
    for (const auto &item : modules) {
        collect_transfer(item.value, ids, order, 0, ok, error);
        if (!ok)
            return false;
    }

    type_map types;
    std::vector<type_record> type_order;
    std::vector<std::pair<std::string, thread_transfer_value>> all_globals =
        globals;
    std::unordered_set<std::string> global_names;
    for (const auto &item : globals)
        global_names.insert(item.first);
    type_collect_context context{&all_globals, &global_names};

    auto collect_all_types = [&]() -> bool {
        for (const auto &function : order) {
            if (!collect_proto_types(*function->proto, types, type_order, 0,
                                     error, &context))
                return false;
            for (const thread_transfer_value &capture :
                 function->captures) {
                if (!collect_transfer_types(capture, types, type_order, 0,
                                            error, &context))
                    return false;
            }
        }
        for (const auto &entry : g_inline_function_payloads) {
            const std::shared_ptr<thread_transfer_function> &function =
                entry.second;
            if (!function || !function->proto)
                continue;
            if (!collect_proto_types(*function->proto, types, type_order, 0,
                                     error, &context))
                return false;
            for (const thread_transfer_value &capture : function->captures) {
                if (!collect_transfer_types(capture, types, type_order, 0,
                                            error, &context))
                    return false;
            }
        }
        for (std::size_t i = 0; i < all_globals.size(); ++i) {
            if (!collect_transfer_types(all_globals[i].second, types,
                                        type_order, 0, error, &context))
                return false;
        }
        for (const auto &item : modules) {
            if (!collect_transfer_types(item.value, types, type_order, 0,
                                        error, &context))
                return false;
        }
        for (std::size_t i = 0; i < type_order.size(); ++i) {
            const type_record &record = type_order[i];
            for (const auto &method : record.methods) {
                if (!method.second)
                    continue;
                for (const thread_transfer_value &capture :
                     method.second->captures) {
                    if (!collect_transfer_types(capture, types, type_order, 0,
                                                error, &context))
                        return false;
                }
            }
            for (const auto &field_default : record.field_defaults) {
                if (!field_default.second)
                    continue;
                for (const thread_transfer_value &capture :
                     field_default.second->captures) {
                    if (!collect_transfer_types(capture, types, type_order, 0,
                                                error, &context))
                        return false;
                }
            }
        }
        return true;
    };

    std::size_t last_order = 0;
    std::size_t last_types = 0;
    std::size_t last_globals = 0;
    for (int pass = 0; pass < 8; ++pass) {
        for (const auto &function : order) {
            if (!prepare_inline_functions(*function->proto, &context, error))
                return false;
        }
        std::vector<std::shared_ptr<thread_transfer_function>> inline_functions;
        inline_functions.reserve(g_inline_function_payloads.size());
        for (const auto &entry : g_inline_function_payloads)
            inline_functions.push_back(entry.second);
        for (const auto &function : inline_functions) {
            if (!prepare_inline_functions(*function->proto, &context, error))
                return false;
        }
        if (!collect_all_types())
            return false;
        for (const type_record &record : type_order) {
            thread_transfer_value wrapper;
            wrapper.kind = thread_transfer_value::Kind::Function;
            for (const auto &method : record.methods) {
                wrapper.function = method.second;
                collect_transfer(wrapper, ids, order, 0, ok, error);
                if (!ok)
                    return false;
            }
            for (const auto &field_default : record.field_defaults) {
                wrapper.function = field_default.second;
                collect_transfer(wrapper, ids, order, 0, ok, error);
                if (!ok)
                    return false;
            }
        }
        for (const auto &item : all_globals) {
            collect_transfer(item.second, ids, order, 0, ok, error);
            if (!ok)
                return false;
        }
        if (order.size() == last_order && type_order.size() == last_types &&
            all_globals.size() == last_globals)
            break;
        last_order = order.size();
        last_types = type_order.size();
        last_globals = all_globals.size();
    }

    writer out;
    out.u32(kPayloadMagic);
    out.u32(kPayloadVersion);
    out.u32(kEndianMarker);
    if (!write_type_metadata(out, type_order, ids, error))
        return false;
    out.u32((uint32_t)order.size());

    /* Function metadata first so captures can reference any function. */
    for (const auto &function : order) {
        out.str(function->name);
        if (!write_proto(out, *function->proto, error))
            return false;
    }
    for (const auto &function : order) {
        out.u32((uint32_t)function->captures.size());
        for (const thread_transfer_value &capture : function->captures) {
            if (!write_transfer(out, capture, ids, 0, error))
                return false;
        }
        out.u32((uint32_t)function->defaults.size());
        for (const thread_transfer_value &value : function->defaults) {
            if (!write_transfer(out, value, ids, 0, error))
                return false;
        }
        out.u8(function->pattern_proto ? 1 : 0);
        if (function->pattern_proto &&
            !write_proto(out, *function->pattern_proto, error))
            return false;
        out.u32((uint32_t)function->pattern_roots.size());
        for (uint32_t root : function->pattern_roots)
            out.u32(root);
    }

    auto entry_it = ids.find(entry.get());
    if (entry_it == ids.end()) {
        error = "thread payload entry is missing";
        return false;
    }
    out.u32(entry_it->second);

    out.u32((uint32_t)all_globals.size());
    for (const auto &item : all_globals) {
        out.str(item.first);
        if (!write_transfer(out, item.second, ids, 0, error))
            return false;
    }
    out.u32((uint32_t)modules.size());
    for (const auto &item : modules) {
        out.str(item.key);
        out.str(item.path);
        out.str(item.base);
        if (!write_transfer(out, item.value, ids, 0, error))
            return false;
    }
    bytes = std::move(out.data);
    return true;
}

bool skein_vm_deserialize_thread_payload(
    const std::string &bytes,
    std::shared_ptr<struct thread_transfer_function> &entry,
    std::vector<std::pair<std::string, thread_transfer_value>> &globals,
    std::vector<struct thread_transfer_module> &modules,
    std::vector<struct thread_transfer_type> &types,
    std::string &error) {
    reader in(bytes);
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t endian = 0;
    if (!in.u32(magic) || !in.u32(version) || !in.u32(endian)) {
        error = in.error;
        return false;
    }
    if (magic != kPayloadMagic) {
        error = "invalid thread payload header";
        return false;
    }
    if (version != kPayloadVersion) {
        error = "unsupported thread payload version";
        return false;
    }
    if (endian != kEndianMarker) {
        error = "unsupported thread payload byte order";
        return false;
    }
    if (!read_type_metadata(in, types, error))
        return false;

    uint32_t function_count = 0;
    if (!in.count(function_count)) {
        error = in.error;
        return false;
    }
    std::vector<std::shared_ptr<thread_transfer_function>> functions;
    functions.reserve(function_count);
    for (uint32_t i = 0; i < function_count; ++i) {
        std::shared_ptr<thread_transfer_function> function =
            std::make_shared<thread_transfer_function>();
        std::shared_ptr<skein::bc::Proto> proto;
        if (!in.str(function->name) || !read_proto(in, proto, error)) {
            if (error.empty())
                error = in.error;
            return false;
        }
        function->proto = std::move(proto);
        functions.push_back(std::move(function));
    }
    for (uint32_t i = 0; i < function_count; ++i) {
        uint32_t capture_count = 0;
        if (!in.count(capture_count)) {
            error = in.error;
            return false;
        }
        functions[i]->captures.reserve(capture_count);
        for (uint32_t j = 0; j < capture_count; ++j) {
            thread_transfer_value capture;
            if (!read_transfer(in, functions, capture, 0, error))
                return false;
            functions[i]->captures.push_back(std::move(capture));
        }
        uint32_t default_count = 0;
        if (!in.count(default_count)) {
            error = in.error;
            return false;
        }
        functions[i]->defaults.reserve(default_count);
        for (uint32_t j = 0; j < default_count; ++j) {
            thread_transfer_value value;
            if (!read_transfer(in, functions, value, 0, error))
                return false;
            functions[i]->defaults.push_back(std::move(value));
        }
        uint8_t has_patterns = 0;
        if (!in.u8(has_patterns)) {
            error = in.error;
            return false;
        }
        if (has_patterns) {
            std::shared_ptr<skein::bc::Proto> pattern_proto;
            if (!read_proto(in, pattern_proto, error))
                return false;
            functions[i]->pattern_proto = std::move(pattern_proto);
        }
        uint32_t root_count = 0;
        if (!in.count(root_count)) {
            error = in.error;
            return false;
        }
        functions[i]->pattern_roots.reserve(root_count);
        for (uint32_t j = 0; j < root_count; ++j) {
            uint32_t root = 0;
            if (!in.u32(root)) {
                error = in.error;
                return false;
            }
            functions[i]->pattern_roots.push_back(root);
        }
    }
    for (thread_transfer_type &type : types) {
        for (thread_transfer_type_field &field : type.fields) {
            if (!field.has_default)
                continue;
            if (field.default_index >= functions.size()) {
                error = "thread payload field default index is out of range";
                return false;
            }
            field.default_function = functions[field.default_index];
        }
        for (thread_transfer_type_variant &variant : type.variants) {
            for (thread_transfer_type_field &field : variant.fields) {
                if (!field.has_default)
                    continue;
                if (field.default_index >= functions.size()) {
                    error =
                        "thread payload field default index is out of range";
                    return false;
                }
                field.default_function = functions[field.default_index];
            }
        }
        if (type.method_indices.size() != type.methods.size()) {
            error = "thread payload method table is inconsistent";
            return false;
        }
        for (std::size_t i = 0; i < type.methods.size(); ++i) {
            uint32_t index = type.method_indices[i];
            if (index >= functions.size()) {
                error = "thread payload method index is out of range";
                return false;
            }
            type.methods[i].second = functions[index];
        }
    }

    uint32_t entry_index = 0;
    if (!in.u32(entry_index)) {
        error = in.error;
        return false;
    }
    if (entry_index >= functions.size()) {
        error = "thread payload entry index is out of range";
        return false;
    }
    entry = functions[entry_index];

    uint32_t global_count = 0;
    if (!in.count(global_count)) {
        error = in.error;
        return false;
    }
    globals.clear();
    globals.reserve(global_count);
    for (uint32_t i = 0; i < global_count; ++i) {
        std::string name;
        thread_transfer_value value;
        if (!in.str(name) || !read_transfer(in, functions, value, 0, error)) {
            if (error.empty())
                error = in.error;
            return false;
        }
        globals.emplace_back(std::move(name), std::move(value));
    }

    uint32_t module_count = 0;
    if (!in.count(module_count)) {
        error = in.error;
        return false;
    }
    modules.clear();
    modules.reserve(module_count);
    for (uint32_t i = 0; i < module_count; ++i) {
        thread_transfer_module module;
        if (!in.str(module.key) || !in.str(module.path) ||
            !in.str(module.base) ||
            !read_transfer(in, functions, module.value, 0, error)) {
            if (error.empty())
                error = in.error;
            return false;
        }
        modules.push_back(std::move(module));
    }
    return true;
}

bool skein_vm_serialize_message_payload(const struct value_t *value,
                                        std::string &bytes,
                                        std::string &error) {
    writer out;
    if (!write_value(out, value, 0, error))
        return false;
    bytes = std::move(out.data);
    return true;
}

bool skein_vm_deserialize_message_payload(const std::string &bytes,
                                          struct value_t *&value,
                                          std::string &error) {
    reader in(bytes);
    value_owner owned;
    if (!read_value(in, owned, 0, error))
        return false;
    value = owned.release();
    return true;
}
