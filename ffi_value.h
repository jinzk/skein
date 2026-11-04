#ifndef SKEIN_FFI_VALUE_H
#define SKEIN_FFI_VALUE_H

#include "dynamic_library.h"
#include "ffi_marshal.h"
#include "ffi_types.h"
#include "valuetype.h"

#include <memory>
#include <string>
#include <vector>

namespace ffi {

struct library_handle {
    std::shared_ptr<dynamic_library> library;
    bool closed = false;
};

struct buffer_handle {
    std::vector<unsigned char> bytes;
    std::size_t size = 0;
    bool freed = false;
};

struct type_handle {
    type_ptr type;
};

struct pointer_handle {
    type_ptr pointee;
    unsigned char *address = nullptr;
    std::shared_ptr<void> owner;
    std::shared_ptr<keepalive> keep;
};

/* A struct value or an array view.  `owner` keeps the backing memory (or the
 * library that owns it) alive for as long as this view exists.  `keep` is
 * shared with every view of the same memory. */
struct object_handle {
    type_ptr type;
    unsigned char *address = nullptr;
    std::shared_ptr<void> owner;
    std::shared_ptr<keepalive> keep;
};

extern const native_type library_native_type;
extern const native_type buffer_native_type;
extern const native_type type_native_type;
extern const native_type pointer_native_type;
extern const native_type object_native_type;
extern const native_type array_native_type;

struct value_t *new_library_value(std::shared_ptr<dynamic_library> library);
struct value_t *new_buffer_value(std::size_t size);
struct value_t *new_type_value(type_ptr type);
struct value_t *new_pointer_value(type_ptr pointee, unsigned char *address,
                                  std::shared_ptr<void> owner,
                                  std::shared_ptr<keepalive> keep = nullptr);
struct value_t *new_object_value(type_ptr type, unsigned char *address,
                                 std::shared_ptr<void> owner,
                                 std::shared_ptr<keepalive> keep = nullptr);

bool get_library(struct value_t *value,
                 std::shared_ptr<library_handle> &out);
bool get_buffer(struct value_t *value, std::shared_ptr<buffer_handle> &out);
bool get_type(struct value_t *value, type_ptr &out);
bool get_pointer(struct value_t *value, std::shared_ptr<pointer_handle> &out);
bool get_object(struct value_t *value, std::shared_ptr<object_handle> &out);

/* Read a type object or a scalar type name. */
bool resolve_type(struct value_t *value, type_ptr &out, std::string &error);

} // namespace ffi

#endif
