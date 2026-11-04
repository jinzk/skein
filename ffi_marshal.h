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

/* An allocation whose ownership can be handed to C.  `release` must make the
 * destructor skip freeing the memory. */
struct releasable {
    virtual ~releasable() = default;
    virtual void release() = 0;
};

/* Byte buffer aligned for every C scalar and standard-layout struct.  The
 * keepalive travels with the memory owner.  Backed by malloc so C code that
 * takes ownership can free it safely. */
struct memory_block : releasable {
    unsigned char *base = nullptr;
    unsigned char *data = nullptr;
    std::size_t size = 0;
    std::shared_ptr<keepalive> keep;
    bool released = false;

    memory_block(std::size_t size, std::size_t alignment);
    ~memory_block() override;
    void release() override;
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

/* Write a callback return value into the libffi return buffer.  Integer
 * results are widened to `ffi_arg`, matching what libffi reads back. */
bool write_return(const type_ptr &t, void *ret, struct value_t *v,
                  keepalive &keep, std::string &error);

struct arg_slot {
    void *value = nullptr;
    std::shared_ptr<void> keep;
};

/* Prepare one libffi argument.  Storage stays alive through `keep`. */
bool prepare_arg(const type_ptr &t, struct value_t *v, arg_slot &out,
                 std::string &error);

} // namespace ffi

#endif
