#include "ffi_types.h"

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

namespace ffi {
namespace {

std::size_t align_up(std::size_t value, std::size_t alignment) {
    if (alignment <= 1)
        return value;
    return (value + alignment - 1) / alignment * alignment;
}

type_ptr make_scalar_type(const char *name, kind_t kind, ffi_type *cif) {
    std::shared_ptr<type> t = std::make_shared<type>();
    t->kind = kind;
    t->name = name;
    t->cif = cif;
    if (kind == kind_t::void_) {
        t->size = 0;
        t->align = 1;
    } else {
        t->size = cif->size;
        t->align = cif->alignment ? cif->alignment : 1;
    }
    return t;
}

const std::unordered_map<std::string, type_ptr> &scalar_table() {
    static const std::unordered_map<std::string, type_ptr> table = [] {
        std::unordered_map<std::string, type_ptr> map;
        map.emplace("i8", make_scalar_type("i8", kind_t::i8, &ffi_type_sint8));
        map.emplace("u8", make_scalar_type("u8", kind_t::u8, &ffi_type_uint8));
        map.emplace("i16",
                    make_scalar_type("i16", kind_t::i16, &ffi_type_sint16));
        map.emplace("u16",
                    make_scalar_type("u16", kind_t::u16, &ffi_type_uint16));
        map.emplace("i32",
                    make_scalar_type("i32", kind_t::i32, &ffi_type_sint32));
        map.emplace("u32",
                    make_scalar_type("u32", kind_t::u32, &ffi_type_uint32));
        map.emplace("i64",
                    make_scalar_type("i64", kind_t::i64, &ffi_type_sint64));
        map.emplace("u64",
                    make_scalar_type("u64", kind_t::u64, &ffi_type_uint64));
        map.emplace("f32",
                    make_scalar_type("f32", kind_t::f32, &ffi_type_float));
        map.emplace("f64",
                    make_scalar_type("f64", kind_t::f64, &ffi_type_double));
        map.emplace("usize",
                    make_scalar_type("usize", kind_t::usize,
                                     sizeof(std::size_t) == 8
                                         ? &ffi_type_uint64
                                         : &ffi_type_uint32));
        map.emplace("isize",
                    make_scalar_type("isize", kind_t::isize,
                                     sizeof(std::size_t) == 8
                                         ? &ffi_type_sint64
                                         : &ffi_type_sint32));
        map.emplace("cstring",
                    make_scalar_type("cstring", kind_t::cstring,
                                     &ffi_type_pointer));
        map.emplace("bytes",
                    make_scalar_type("bytes", kind_t::bytes,
                                     &ffi_type_pointer));
        map.emplace("buffer",
                    make_scalar_type("buffer", kind_t::buffer,
                                     &ffi_type_pointer));
        map.emplace("void",
                    make_scalar_type("void", kind_t::void_, &ffi_type_void));
        std::shared_ptr<type> ptr = std::make_shared<type>();
        ptr->kind = kind_t::pointer;
        ptr->name = "ptr";
        ptr->cif = &ffi_type_pointer;
        ptr->size = sizeof(void *);
        ptr->align = alignof(void *);
        map.emplace("ptr", std::move(ptr));
        return map;
    }();
    return table;
}

/* Ask libffi to lay the aggregate out and compare it with the layout we
 * computed.  A mismatch means the declaration does not follow the platform C
 * ABI; silently trusting either side would corrupt memory. */
bool verify_layout(type &t, std::string &error) {
#if defined(FFI_VERSION_NUMBER) && FFI_VERSION_NUMBER >= 30300
    if (!t.owned_cif)
        return true;
    const std::size_t count = t.cif_elements.empty()
                                  ? 0
                                  : t.cif_elements.size() - 1;
    std::vector<std::size_t> offsets(count, 0);
    if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, t.owned_cif.get(),
                               offsets.empty() ? nullptr : offsets.data()) !=
        FFI_OK) {
        error = "cannot compute the platform layout of " + t.name;
        return false;
    }
    if (t.owned_cif->size != 0 && t.owned_cif->size != t.size) {
        error = "layout of " + t.name + " does not match the platform C ABI";
        return false;
    }
    if (t.owned_cif->alignment != 0 &&
        (std::size_t)t.owned_cif->alignment != t.align) {
        error = "alignment of " + t.name +
                " does not match the platform C ABI";
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t expected =
            t.kind == kind_t::record
                ? t.fields[i].offset
                : i * (t.element ? t.element->size : 0);
        if (offsets[i] != expected) {
            error = "field layout of " + t.name +
                    " does not match the platform C ABI";
            return false;
        }
    }
#else
    (void)t;
    (void)error;
#endif
    return true;
}

} // namespace

type_ptr scalar(const std::string &name) {
    const auto &table = scalar_table();
    auto it = table.find(name);
    if (it == table.end())
        return nullptr;
    return it->second;
}

bool is_integer(kind_t kind) {
    switch (kind) {
        case kind_t::i8:
        case kind_t::u8:
        case kind_t::i16:
        case kind_t::u16:
        case kind_t::i32:
        case kind_t::u32:
        case kind_t::i64:
        case kind_t::u64:
        case kind_t::usize:
        case kind_t::isize:
            return true;
        default:
            return false;
    }
}

bool is_float(kind_t kind) {
    return kind == kind_t::f32 || kind == kind_t::f64;
}

bool is_pointer_like(kind_t kind) {
    switch (kind) {
        case kind_t::cstring:
        case kind_t::bytes:
        case kind_t::buffer:
        case kind_t::pointer:
        case kind_t::opaque:
        case kind_t::callback:
            return true;
        default:
            return false;
    }
}

bool is_aggregate(kind_t kind) {
    return kind == kind_t::record || kind == kind_t::array;
}

bool is_value_type(const type &t) {
    return t.kind != kind_t::void_ && t.cif != nullptr;
}

const char *kind_name(kind_t kind) {
    switch (kind) {
        case kind_t::i8: return "i8";
        case kind_t::u8: return "u8";
        case kind_t::i16: return "i16";
        case kind_t::u16: return "u16";
        case kind_t::i32: return "i32";
        case kind_t::u32: return "u32";
        case kind_t::i64: return "i64";
        case kind_t::u64: return "u64";
        case kind_t::f32: return "f32";
        case kind_t::f64: return "f64";
        case kind_t::usize: return "usize";
        case kind_t::isize: return "isize";
        case kind_t::cstring: return "cstring";
        case kind_t::bytes: return "bytes";
        case kind_t::buffer: return "buffer";
        case kind_t::void_: return "void";
        case kind_t::pointer: return "pointer";
        case kind_t::array: return "array";
        case kind_t::record: return "struct";
        case kind_t::opaque: return "opaque";
        case kind_t::callback: return "callback";
    }
    return "unknown";
}

type_ptr make_pointer(type_ptr pointee) {
    std::shared_ptr<type> t = std::make_shared<type>();
    t->kind = kind_t::pointer;
    t->element = std::move(pointee);
    t->cif = &ffi_type_pointer;
    t->size = sizeof(void *);
    t->align = alignof(void *);
    t->name = "pointer(" +
              std::string(t->element ? t->element->name : "void") + ")";
    return t;
}

type_ptr make_opaque(const std::string &name) {
    std::shared_ptr<type> t = std::make_shared<type>();
    t->kind = kind_t::opaque;
    t->name = name.empty() ? "opaque" : name;
    t->cif = &ffi_type_pointer;
    t->size = sizeof(void *);
    t->align = alignof(void *);
    return t;
}

bool make_array(type_ptr element, std::size_t count, type_ptr &out,
                std::string &error) {
    if (!element) {
        error = "array element type is missing";
        return false;
    }
    if (!is_value_type(*element)) {
        error = "array element type " + element->name +
                " cannot be passed by value";
        return false;
    }
    if (count == 0) {
        error = "array length must be positive";
        return false;
    }
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    if (element->size != 0 && count > max / element->size) {
        error = "array length is too large";
        return false;
    }

    std::shared_ptr<type> t = std::make_shared<type>();
    t->kind = kind_t::array;
    t->element = std::move(element);
    t->count = count;
    t->size = t->element->size * count;
    t->align = t->element->align;
    t->name = "[" + std::to_string(count) + " x " + t->element->name + "]";

    t->owned_cif = std::make_shared<ffi_type>();
    t->owned_cif->size = 0;
    t->owned_cif->alignment = 0;
    t->owned_cif->type = FFI_TYPE_STRUCT;
    t->cif_elements.assign(count, t->element->cif);
    t->cif_elements.push_back(nullptr);
    t->owned_cif->elements = t->cif_elements.data();
    t->cif = t->owned_cif.get();

    if (!verify_layout(*t, error))
        return false;
    out = std::move(t);
    return true;
}

bool make_record(const std::string &name,
                 const std::vector<std::pair<std::string, type_ptr>> &fields,
                 type_ptr &out, std::string &error) {
    if (fields.empty()) {
        error = "struct " + (name.empty() ? std::string("(anonymous)") : name) +
                " must have at least one field";
        return false;
    }

    std::shared_ptr<type> t = std::make_shared<type>();
    t->kind = kind_t::record;
    t->name = name.empty() ? "struct" : name;
    t->owned_cif = std::make_shared<ffi_type>();
    t->owned_cif->size = 0;
    t->owned_cif->alignment = 0;
    t->owned_cif->type = FFI_TYPE_STRUCT;

    std::size_t offset = 0;
    std::size_t max_align = 1;
    for (const auto &entry : fields) {
        if (!entry.second) {
            error = "field " + entry.first + " has no type";
            return false;
        }
        if (!is_value_type(*entry.second)) {
            error = "field " + entry.first + " of " + t->name +
                    " cannot be passed by value";
            return false;
        }
        for (const field &existing : t->fields) {
            if (existing.name == entry.first) {
                error = "duplicate field " + entry.first + " in " + t->name;
                return false;
            }
        }
        offset = align_up(offset, entry.second->align);
        field f;
        f.name = entry.first;
        f.type = entry.second;
        f.offset = offset;
        t->fields.push_back(std::move(f));
        t->cif_elements.push_back(entry.second->cif);
        offset += entry.second->size;
        if (entry.second->align > max_align)
            max_align = entry.second->align;
    }

    t->align = max_align;
    t->size = align_up(offset, max_align);
    if (t->size == 0)
        t->size = 1;
    t->cif_elements.push_back(nullptr);
    t->owned_cif->elements = t->cif_elements.data();
    t->cif = t->owned_cif.get();

    if (!verify_layout(*t, error))
        return false;
    out = std::move(t);
    return true;
}

bool make_callback(type_ptr result, std::vector<type_ptr> params,
                   type_ptr &out, std::string &error) {
    if (!result) {
        error = "callback result type is missing";
        return false;
    }
    if (result->kind != kind_t::void_ && !is_value_type(*result)) {
        error = "callback result type " + result->name +
                " cannot be passed by value";
        return false;
    }
    for (const type_ptr &param : params) {
        if (!param || !is_value_type(*param)) {
            error = "callback argument types cannot be passed by value";
            return false;
        }
    }
    std::shared_ptr<type> t = std::make_shared<type>();
    t->kind = kind_t::callback;
    t->result = std::move(result);
    t->params = std::move(params);
    t->cif = &ffi_type_pointer;
    t->size = sizeof(void *);
    t->align = alignof(void *);
    t->name = "callback";
    out = std::move(t);
    return true;
}

const field *find_field(const type &t, const std::string &name) {
    if (t.kind != kind_t::record)
        return nullptr;
    for (const field &f : t.fields) {
        if (f.name == name)
            return &f;
    }
    return nullptr;
}

} // namespace ffi
