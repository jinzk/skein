#ifndef SKEIN_FFI_MARSHAL_H
#define SKEIN_FFI_MARSHAL_H

#include "ffi_types.h"
#include "valuetype.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ffi {

/* Script values and owners that C memory still points at.  A keepalive is
 * shared by every view of the same memory so a string written through a
 * nested view cannot be freed while the root value is alive. */
struct keepalive {
    std::vector<struct value_t *> values;
    std::vector<std::shared_ptr<void>> owners;

    ~keepalive();
};

/* Byte buffer aligned for every C scalar and standard-layout struct.  The
 * keepalive travels with the memory owner. */
struct memory_block {
    std::shared_ptr<std::vector<unsigned char>> raw;
    unsigned char *data = nullptr;
    std::size_t size = 0;
    std::shared_ptr<keepalive> keep;

    memory_block(std::size_t size, std::size_t alignment);
};

/* Convert C memory into a script value.  `owner` keeps the memory alive when
 * the result is a view (struct/array/pointer); `keep` is shared with that
 * view. */
struct value_t *read_value(const type_ptr &t, const unsigned char *src,
                           std::shared_ptr<void> owner,
                           std::string *error = nullptr,
                           std::shared_ptr<keepalive> keep = nullptr);

/* Convert a value into plain data (dicts/lists, no views). */
struct value_t *to_data(const type_ptr &t, const unsigned char *src,
                        std::shared_ptr<void> owner,
                        std::string *error = nullptr,
                        std::shared_ptr<keepalive> keep = nullptr);

/* Write a script value into C memory. */
bool write_value(const type_ptr &t, unsigned char *dst, struct value_t *v,
                 keepalive &keep, std::string &error);
bool write_value(const type_ptr &t, unsigned char *dst, struct value_t *v,
                 keepalive &keep, std::string &error, int depth);

struct arg_slot {
    void *value = nullptr;
    std::shared_ptr<void> keep;
};

/* Prepare one libffi argument.  Storage stays alive through `keep`. */
bool prepare_arg(const type_ptr &t, struct value_t *v, arg_slot &out,
                 std::string &error);

} // namespace ffi

#endif
