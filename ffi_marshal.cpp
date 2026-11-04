#include "ffi_marshal.h"

#include "dict.h"
#include "ffi_value.h"
#include "value_ops.h"

#include <cstdint>
#include <cstring>
#include <limits>

namespace ffi {
namespace {

template <typename T>
T load(const unsigned char *src) {
    T value{};
    std::memcpy(&value, src, sizeof(T));
    return value;
}

template <typename T>
void store(unsigned char *dst, T value) {
    std::memcpy(dst, &value, sizeof(T));
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    if (alignment <= 1)
        return value;
    return (value + alignment - 1) / alignment * alignment;
}

bool is_pointer_value(const struct value_t *v) {
    return v && v->type == T_userdata &&
           ((const userdata_value_t *)v)->meta == &pointer_native_type;
}

bool is_buffer_value(const struct value_t *v) {
    return v && v->type == T_userdata &&
           ((const userdata_value_t *)v)->meta == &buffer_native_type;
}

bool check_integer_range(kind_t kind, long long value, std::string &error) {
    switch (kind) {
        case kind_t::i8:
            if (value < -128 || value > 127) {
                error = "value does not fit in i8";
                return false;
            }
            return true;
        case kind_t::u8:
            if (value < 0 || value > 255) {
                error = "value does not fit in u8";
                return false;
            }
            return true;
        case kind_t::i16:
            if (value < -32768 || value > 32767) {
                error = "value does not fit in i16";
                return false;
            }
            return true;
        case kind_t::u16:
            if (value < 0 || value > 65535) {
                error = "value does not fit in u16";
                return false;
            }
            return true;
        case kind_t::i32:
            if (value < std::numeric_limits<std::int32_t>::min() ||
                value > std::numeric_limits<std::int32_t>::max()) {
                error = "value does not fit in i32";
                return false;
            }
            return true;
        case kind_t::u32:
            if (value < 0 ||
                (unsigned long long)value >
                    std::numeric_limits<std::uint32_t>::max()) {
                error = "value does not fit in u32";
                return false;
            }
            return true;
        case kind_t::u64:
        case kind_t::usize:
            if (value < 0) {
                error = "value cannot be negative";
                return false;
            }
            return true;
        default:
            return true;
    }
}

void store_integer(kind_t kind, unsigned char *dst, long long value) {
    switch (kind) {
        case kind_t::i8:
            store<std::int8_t>(dst, (std::int8_t)value);
            break;
        case kind_t::u8:
            store<std::uint8_t>(dst, (std::uint8_t)value);
            break;
        case kind_t::i16:
            store<std::int16_t>(dst, (std::int16_t)value);
            break;
        case kind_t::u16:
            store<std::uint16_t>(dst, (std::uint16_t)value);
            break;
        case kind_t::i32:
            store<std::int32_t>(dst, (std::int32_t)value);
            break;
        case kind_t::u32:
            store<std::uint32_t>(dst, (std::uint32_t)value);
            break;
        case kind_t::i64:
            store<std::int64_t>(dst, (std::int64_t)value);
            break;
        case kind_t::u64:
            store<std::uint64_t>(dst, (std::uint64_t)value);
            break;
        case kind_t::usize:
            store<std::size_t>(dst, (std::size_t)value);
            break;
        case kind_t::isize:
            store<std::ptrdiff_t>(dst, (std::ptrdiff_t)value);
            break;
        default:
            break;
    }
}

} // namespace

keepalive::~keepalive() {
    for (struct value_t *v : values)
        freeval(v);
}

memory_block::memory_block(std::size_t size, std::size_t alignment)
    : keep(std::make_shared<keepalive>()) {
    this->size = size ? size : 1;
    const std::size_t align = alignment ? alignment : 1;
    raw = std::make_shared<std::vector<unsigned char>>(this->size + align);
    const std::uintptr_t base = (std::uintptr_t)raw->data();
    const std::uintptr_t aligned = align_up(base, align);
    data = raw->data() + (aligned - base);
    std::memset(data, 0, this->size);
}

struct value_t *read_value(const type_ptr &t, const unsigned char *src,
                           std::shared_ptr<void> owner, std::string *error,
                           std::shared_ptr<keepalive> keep) {
    if (!t || !src) {
        if (error)
            *error = "missing ffi type or data";
        return NULL;
    }

    switch (t->kind) {
        case kind_t::i8:
            return newintval((long long)load<std::int8_t>(src));
        case kind_t::u8:
            return newintval((long long)load<std::uint8_t>(src));
        case kind_t::i16:
            return newintval((long long)load<std::int16_t>(src));
        case kind_t::u16:
            return newintval((long long)load<std::uint16_t>(src));
        case kind_t::i32:
            return newintval((long long)load<std::int32_t>(src));
        case kind_t::u32:
            return newintval((long long)load<std::uint32_t>(src));
        case kind_t::i64:
            return newintval((long long)load<std::int64_t>(src));
        case kind_t::u64: {
            std::uint64_t value = load<std::uint64_t>(src);
            if (value >
                (std::uint64_t)std::numeric_limits<long long>::max()) {
                if (error)
                    *error = "u64 value exceeds the integer range";
                return NULL;
            }
            return newintval((long long)value);
        }
        case kind_t::f32:
            return newfloatval((double)load<float>(src));
        case kind_t::f64:
            return newfloatval(load<double>(src));
        case kind_t::usize: {
            std::size_t value = load<std::size_t>(src);
            if (value >
                (std::size_t)std::numeric_limits<long long>::max()) {
                if (error)
                    *error = "usize value exceeds the integer range";
                return NULL;
            }
            return newintval((long long)value);
        }
        case kind_t::isize:
            return newintval((long long)load<std::ptrdiff_t>(src));
        case kind_t::cstring: {
            const char *value = load<const char *>(src);
            return value ? newstrval(value) : newnull();
        }
        case kind_t::bytes:
        case kind_t::buffer: {
            type_ptr byte = scalar("u8");
            type_ptr pointer = make_pointer(byte);
            return new_pointer_value(
                pointer, (unsigned char *)load<void *>(src), owner, keep);
        }
        case kind_t::pointer:
            return new_pointer_value(
                t->element, (unsigned char *)load<void *>(src), owner, keep);
        case kind_t::opaque:
        case kind_t::callback:
            return new_pointer_value(NULL, (unsigned char *)load<void *>(src),
                                     owner, keep);
        case kind_t::record:
        case kind_t::array:
            return new_object_value(t, (unsigned char *)src, owner, keep);
        case kind_t::void_:
            return newnull();
    }
    return newnull();
}

struct value_t *to_data(const type_ptr &t, const unsigned char *src,
                        std::shared_ptr<void> owner, std::string *error,
                        std::shared_ptr<keepalive> keep) {
    if (!t || !src) {
        if (error)
            *error = "missing ffi type or data";
        return NULL;
    }
    if (t->kind == kind_t::record) {
        struct dict_value_t *dict = newdictval();
        for (const field &f : t->fields) {
            struct value_t *key = newstrval(f.name.c_str());
            struct value_t *value =
                to_data(f.type, src + f.offset, owner, error, keep);
            if (!value) {
                freeval(key);
                freeval((struct value_t *)dict);
                return NULL;
            }
            dict_append_item(dict, key, value);
        }
        return (struct value_t *)dict;
    }
    if (t->kind == kind_t::array) {
        struct list_value_t *list = newlistval();
        for (std::size_t i = 0; i < t->count; ++i) {
            struct value_t *value = to_data(t->element,
                                           src + i * t->element->size,
                                           owner, error, keep);
            if (!value) {
                freeval((struct value_t *)list);
                return NULL;
            }
            list->payload->items.push_back(value);
        }
        return (struct value_t *)list;
    }
    return read_value(t, src, owner, error, keep);
}

bool write_value(const type_ptr &t, unsigned char *dst, struct value_t *v,
                 keepalive &keep, std::string &error) {
    return write_value(t, dst, v, keep, error, 0);
}

bool write_value(const type_ptr &t, unsigned char *dst, struct value_t *v,
                 keepalive &keep, std::string &error, int depth) {
    if (!t || !dst) {
        error = "missing ffi type or data";
        return false;
    }
    if (depth > 64) {
        error = "ffi value nesting is too deep";
        return false;
    }
    if (!v) {
        error = "missing value";
        return false;
    }

    if (is_integer(t->kind)) {
        if (v->type != T_int) {
            error = "expected an integer, got " + std::string(typename_of(v));
            return false;
        }
        long long number = ((int_value_t *)v)->value;
        if (!check_integer_range(t->kind, number, error))
            return false;
        store_integer(t->kind, dst, number);
        return true;
    }

    if (is_float(t->kind)) {
        if (v->type != T_int && v->type != T_float) {
            error = "expected a number, got " + std::string(typename_of(v));
            return false;
        }
        double number = as_double(v);
        if (t->kind == kind_t::f32)
            store<float>(dst, (float)number);
        else
            store<double>(dst, number);
        return true;
    }

    switch (t->kind) {
        case kind_t::cstring: {
            if (v->type == T_null) {
                store<const char *>(dst, nullptr);
                return true;
            }
            if (v->type != T_string) {
                error = "expected a string, got " +
                        std::string(typename_of(v));
                return false;
            }
            struct value_t *copy = dupval(v);
            if (!copy) {
                error = "cannot copy string";
                return false;
            }
            keep.values.push_back(copy);
            store<const char *>(dst,
                                ((string_value_t *)copy)->value);
            return true;
        }
        case kind_t::bytes: {
            if (v->type == T_null) {
                store<const char *>(dst, nullptr);
                return true;
            }
            if (v->type != T_bytes) {
                error = "expected bytes, got " + std::string(typename_of(v));
                return false;
            }
            struct value_t *copy = dupval(v);
            if (!copy) {
                error = "cannot copy bytes";
                return false;
            }
            keep.values.push_back(copy);
            store<const char *>(dst,
                               ((bytes_value_t *)copy)->value.c_str());
            return true;
        }
        case kind_t::buffer: {
            if (v->type == T_null) {
                store<void *>(dst, nullptr);
                return true;
            }
            if (!is_buffer_value(v)) {
                error = "expected an ffi buffer";
                return false;
            }
            std::shared_ptr<buffer_handle> buffer =
                std::static_pointer_cast<buffer_handle>(
                    ((userdata_value_t *)v)->data);
            if (!buffer || buffer->freed) {
                error = "ffi buffer is freed";
                return false;
            }
            keep.owners.push_back(buffer);
            store<void *>(dst, buffer->bytes.data());
            return true;
        }
        case kind_t::pointer:
        case kind_t::opaque:
        case kind_t::callback: {
            if (v->type == T_null) {
                store<void *>(dst, nullptr);
                return true;
            }
            if (!is_pointer_value(v)) {
                error = "expected an ffi pointer";
                return false;
            }
            std::shared_ptr<pointer_handle> pointer =
                std::static_pointer_cast<pointer_handle>(
                    ((userdata_value_t *)v)->data);
            if (!pointer) {
                error = "invalid ffi pointer";
                return false;
            }
            keep.owners.push_back(pointer);
            store<void *>(dst, pointer->address);
            return true;
        }
        case kind_t::record: {
            if (v->type == T_dict) {
                std::memset(dst, 0, t->size);
                struct dict_value_t *dict = (struct dict_value_t *)v;
                for (const auto &entry : dict->payload->items) {
                    if (entry.first->type != T_string) {
                        error = "struct field names must be strings";
                        return false;
                    }
                    const char *name = ((string_value_t *)entry.first)->value;
                    const field *f = find_field(*t, name);
                    if (!f) {
                        error = t->name + " has no field " + name;
                        return false;
                    }
                    if (!write_value(f->type, dst + f->offset, entry.second,
                                     keep, error, depth + 1))
                        return false;
                }
                return true;
            }
            if (v->type != T_userdata ||
                ((userdata_value_t *)v)->meta != &object_native_type) {
                error = "expected " + t->name + ", got " +
                        std::string(typename_of(v));
                return false;
            }
            std::shared_ptr<object_handle> object =
                std::static_pointer_cast<object_handle>(
                    ((userdata_value_t *)v)->data);
            if (!object || !object->type || object->type.get() != t.get()) {
                error = "expected " + t->name + ", got a different struct";
                return false;
            }
            std::memcpy(dst, object->address, t->size);
            return true;
        }
        case kind_t::array: {
            if (v->type == T_list) {
                struct list_value_t *list = (struct list_value_t *)v;
                if (list->payload->items.size() != t->count) {
                    error = "expected " + std::to_string(t->count) +
                            " elements, got " +
                            std::to_string(list->payload->items.size());
                    return false;
                }
                std::memset(dst, 0, t->size);
                for (std::size_t i = 0; i < t->count; ++i) {
                    if (!write_value(t->element,
                                     dst + i * t->element->size,
                                     list->payload->items[i], keep, error,
                                     depth + 1))
                        return false;
                }
                return true;
            }
            if (v->type != T_userdata ||
                ((userdata_value_t *)v)->meta != &array_native_type) {
                error = "expected a list or ffi array, got " +
                        std::string(typename_of(v));
                return false;
            }
            std::shared_ptr<object_handle> object =
                std::static_pointer_cast<object_handle>(
                    ((userdata_value_t *)v)->data);
            if (!object || !object->type || object->type.get() != t.get()) {
                error = "expected " + t->name + ", got a different array";
                return false;
            }
            std::memcpy(dst, object->address, t->size);
            return true;
        }
        case kind_t::void_:
            error = "void is not a value type";
            return false;
        default:
            break;
    }
    error = "cannot write " + std::string(kind_name(t->kind)) + " values";
    return false;
}

namespace {

struct arg_holder {
    std::shared_ptr<memory_block> block;
    keepalive keep;
};

} // namespace

bool prepare_arg(const type_ptr &t, struct value_t *v, arg_slot &out,
                 std::string &error) {
    if (!t) {
        error = "missing argument type";
        return false;
    }
    if (t->kind == kind_t::void_) {
        error = "void is not a valid argument type";
        return false;
    }
    if (t->size == 0 || !t->cif) {
        error = t->name + " cannot be passed by value";
        return false;
    }

    std::shared_ptr<arg_holder> holder = std::make_shared<arg_holder>();
    holder->block = std::make_shared<memory_block>(t->size, t->align);
    if (!write_value(t, holder->block->data, v, holder->keep, error))
        return false;
    out.value = holder->block->data;
    out.keep = holder;
    return true;
}

} // namespace ffi
