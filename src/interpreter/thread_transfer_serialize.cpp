#include "vm.h"

#include "dict.h"
#include "structtype.h"
#include "symbol.h"
#include "verifier.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kPayloadMagic = 0x48545453u; /* "STTH" */
constexpr uint32_t kPayloadVersion = 1u;
constexpr uint32_t kEndianMarker = 0x01020304u;
constexpr uint32_t kMaxCount = 1000000u;
constexpr uint32_t kMaxString = 64u * 1024u * 1024u;
constexpr int kMaxDepth = 64;

struct value_deleter {
    void operator()(struct value_t *value) const { freeval(value); }
};

using owned_value = std::shared_ptr<struct value_t>;
using owned_type = std::shared_ptr<struct type_constraint>;

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

bool read_value(reader &in, owned_value &value, int depth,
                std::string &error);

bool read_value_inner(reader &in, owned_value &value, int depth,
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
            value = owned_value(newnull(), value_deleter());
            return true;
        case value_tag::Bool: {
            uint8_t raw = 0;
            if (!in.u8(raw)) {
                error = in.error;
                return false;
            }
            value = owned_value(newboolval(raw != 0), value_deleter());
            return true;
        }
        case value_tag::Int: {
            int64_t raw = 0;
            if (!in.i64(raw)) {
                error = in.error;
                return false;
            }
            value = owned_value(newintval((long long)raw), value_deleter());
            return true;
        }
        case value_tag::Float: {
            double raw = 0;
            if (!in.f64(raw)) {
                error = in.error;
                return false;
            }
            value = owned_value(newfloatval(raw), value_deleter());
            return true;
        }
        case value_tag::Char: {
            uint32_t raw = 0;
            if (!in.u32(raw)) {
                error = in.error;
                return false;
            }
            value = owned_value(newcharval(raw), value_deleter());
            return true;
        }
        case value_tag::Byte: {
            uint32_t raw = 0;
            if (!in.u32(raw)) {
                error = in.error;
                return false;
            }
            value = owned_value(newbyteval(raw), value_deleter());
            return true;
        }
        case value_tag::String: {
            std::string raw;
            if (!in.str(raw)) {
                error = in.error;
                return false;
            }
            value = owned_value(newstrval(raw.c_str()), value_deleter());
            return true;
        }
        case value_tag::Bytes: {
            std::string raw;
            if (!in.str(raw)) {
                error = in.error;
                return false;
            }
            value = owned_value(newbytesval(std::move(raw)), value_deleter());
            return true;
        }
        case value_tag::Error: {
            std::string kind;
            std::string message;
            if (!in.str(kind) || !in.str(message)) {
                error = in.error;
                return false;
            }
            value = owned_value(newerrorval(kind, message), value_deleter());
            return true;
        }
        case value_tag::Result: {
            uint8_t ok = 0;
            if (!in.u8(ok)) {
                error = in.error;
                return false;
            }
            owned_value nested;
            if (!read_value(in, nested, depth + 1, error))
                return false;
            struct value_t *raw = nested.get();
            nested.reset();
            value = owned_value(ok ? newresultok(raw)
                                   : newresulterr(raw),
                                value_deleter());
            return true;
        }
        case value_tag::List: {
            uint32_t count = 0;
            if (!in.count(count)) {
                error = in.error;
                return false;
            }
            struct list_value_t *list = newlistval();
            value = owned_value((struct value_t *)list, value_deleter());
            for (uint32_t i = 0; i < count; ++i) {
                owned_value item;
                if (!read_value(in, item, depth + 1, error))
                    return false;
                list->payload->items.push_back(item.get());
                item.reset();
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
            value = owned_value((struct value_t *)dict, value_deleter());
            dict->payload->is_namespace = namespace_flag != 0;
            for (uint32_t i = 0; i < count; ++i) {
                owned_value key;
                owned_value item;
                if (!read_value(in, key, depth + 1, error) ||
                    !read_value(in, item, depth + 1, error)) {
                    return false;
                }
                struct value_t *raw_key = key.get();
                struct value_t *raw_item = item.get();
                key.reset();
                item.reset();
                dict_append_item(dict, raw_key, raw_item);
            }
            return true;
        }
    }
    error = "invalid thread payload value tag";
    return false;
}

bool read_value(reader &in, owned_value &value, int depth,
                std::string &error) {
    return read_value_inner(in, value, depth, error);
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
            owned_value data;
            if (!read_value(in, data, depth + 1, error))
                return false;
            value = thread_transfer_value();
            value.kind = thread_transfer_value::Kind::Data;
            value.data = std::move(data);
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
        default:
            error = "invalid thread payload transfer tag";
            return false;
    }
}

bool write_proto(writer &out, const skein::bc::Proto &proto,
                 std::string &error) {
    if (proto.has_closures || proto.uses_defers ||
        !proto.defer_protos.empty() || !proto.defer_captures.empty()) {
        error = "cannot serialize a Proto that uses closures or defer";
        return false;
    }
    if (!proto.packages.empty()) {
        error = "cannot serialize a Proto with nested packages";
        return false;
    }
    if (!proto.patterns.empty() || !proto.match_arms.empty()) {
        error = "cannot serialize a Proto with patterns";
        return false;
    }
    if (!proto.constructs.empty() || !proto.field_or_enum.empty()) {
        error = "cannot serialize a Proto with struct/enum construction";
        return false;
    }
    if (!proto.struct_decls.empty() || !proto.enum_decls.empty() ||
        !proto.trait_decls.empty() || !proto.impl_decls.empty()) {
        error = "cannot serialize a Proto with type declarations";
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
                error = "cannot serialize a Proto with nested functions";
                return false;
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

    out.u32((uint32_t)proto.call_descs.size());
    for (const skein::bc::CallDesc &call : proto.call_descs) {
        out.u32((uint32_t)call.spread.size());
        for (bool spread : call.spread)
            out.u8(spread ? 1 : 0);
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
    out.u8(proto.is_async ? 1 : 0);
    out.u8(proto.is_generator ? 1 : 0);
    out.u8(proto.is_method ? 1 : 0);
    out.u32(proto.nparams);
    out.u32(proto.required_params);
    out.u32(proto.nlocals);
    out.u32(proto.max_stack);

    out.u32((uint32_t)proto.stack_depth.size());
    for (int depth : proto.stack_depth)
        out.u32((uint32_t)depth);
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
                owned_value value;
                if (!read_value(in, value, 0, error))
                    return false;
                constant.value = std::move(value);
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
                error = "thread payload contains a nested function";
                return false;
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

    uint8_t flags[12] = {0};
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
    proto->is_async = flags[9] != 0;
    proto->is_generator = flags[10] != 0;
    proto->is_method = flags[11] != 0;

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
    }
}

} // namespace

bool skein_vm_serialize_thread_payload(
    const std::shared_ptr<struct thread_transfer_function> &entry,
    const std::vector<std::pair<std::string, thread_transfer_value>> &globals,
    const std::vector<std::pair<std::string, thread_transfer_value>> &modules,
    std::string &bytes, std::string &error) {
    if (!entry || !entry->proto) {
        error = "invalid thread payload entry";
        return false;
    }
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
        collect_transfer(item.second, ids, order, 0, ok, error);
        if (!ok)
            return false;
    }

    writer out;
    out.u32(kPayloadMagic);
    out.u32(kPayloadVersion);
    out.u32(kEndianMarker);
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
    }

    auto entry_it = ids.find(entry.get());
    if (entry_it == ids.end()) {
        error = "thread payload entry is missing";
        return false;
    }
    out.u32(entry_it->second);

    out.u32((uint32_t)globals.size());
    for (const auto &item : globals) {
        out.str(item.first);
        if (!write_transfer(out, item.second, ids, 0, error))
            return false;
    }
    out.u32((uint32_t)modules.size());
    for (const auto &item : modules) {
        out.str(item.first);
        if (!write_transfer(out, item.second, ids, 0, error))
            return false;
    }
    bytes = std::move(out.data);
    return true;
}

bool skein_vm_deserialize_thread_payload(
    const std::string &bytes,
    std::shared_ptr<struct thread_transfer_function> &entry,
    std::vector<std::pair<std::string, thread_transfer_value>> &globals,
    std::vector<std::pair<std::string, thread_transfer_value>> &modules,
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
        std::string name;
        thread_transfer_value value;
        if (!in.str(name) || !read_transfer(in, functions, value, 0, error)) {
            if (error.empty())
                error = in.error;
            return false;
        }
        modules.emplace_back(std::move(name), std::move(value));
    }
    return true;
}
