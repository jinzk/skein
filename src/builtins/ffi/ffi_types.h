#ifndef SKEIN_FFI_TYPES_H
#define SKEIN_FFI_TYPES_H

#include <ffi.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ffi {

/* Kinds of C values the generic FFI understands.  pack/bitfield layouts are
 * deliberately absent: they are handled by static C++ bindings. */
enum class kind_t {
    i8,
    u8,
    i16,
    u16,
    i32,
    u32,
    i64,
    u64,
    f32,
    f64,
    usize,
    isize,
    cstring,
    bytes,
    buffer,
    void_,
    pointer,
    array,
    record,
    opaque,
    callback
};

struct type;
using type_ptr = std::shared_ptr<const type>;

struct field {
    std::string name;
    type_ptr type;
    std::size_t offset = 0;
};

/* Immutable description of one C type.  Instances are shared by every value
 * of that type; layout is computed once, at declaration time. */
struct type {
    kind_t kind = kind_t::void_;
    std::string name;
    std::size_t size = 0;
    std::size_t align = 1;

    std::vector<field> fields;     /* record */
    type_ptr element;              /* pointer / array */
    std::size_t count = 0;         /* array */
    type_ptr result;               /* callback */
    std::vector<type_ptr> params;  /* callback */

    ffi_type *cif = nullptr;
    std::shared_ptr<ffi_type> owned_cif;
    std::vector<ffi_type *> cif_elements;
};

/* Scalar type by its script-visible spelling ("i32", "cstring", ...).
 * Returns null when the name is unknown. */
type_ptr scalar(const std::string &name);

bool is_integer(kind_t kind);
bool is_float(kind_t kind);
bool is_pointer_like(kind_t kind); /* cstring/bytes/buffer/pointer/opaque */
bool is_aggregate(kind_t kind);    /* record/array */
/* Whether the kind can be passed or returned by value. */
bool is_value_type(const type &t);

const char *kind_name(kind_t kind);

type_ptr make_pointer(type_ptr pointee);
type_ptr make_opaque(const std::string &name);
bool make_array(type_ptr element, std::size_t count, type_ptr &out,
                std::string &error);
bool make_record(const std::string &name,
                 const std::vector<std::pair<std::string, type_ptr>> &fields,
                 type_ptr &out, std::string &error);
bool make_callback(type_ptr result, std::vector<type_ptr> params,
                   type_ptr &out, std::string &error);

/* Look up a field by name; returns nullptr when absent. */
const field *find_field(const type &t, const std::string &name);

} // namespace ffi

#endif
