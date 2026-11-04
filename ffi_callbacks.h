#ifndef SKEIN_FFI_CALLBACKS_H
#define SKEIN_FFI_CALLBACKS_H

#include "ffi_marshal.h"
#include "ffi_types.h"
#include "valuetype.h"

#include <atomic>
#include <memory>
#include <string>

namespace ffi {

struct callback_call_scope;
struct async_call_context;

/* Installed on a worker thread while an asynchronous FFI call runs. */
class async_scope {
public:
    explicit async_scope(
        std::shared_ptr<std::atomic<bool>> failed = nullptr);
    ~async_scope();

    async_scope(const async_scope &) = delete;
    async_scope &operator=(const async_scope &) = delete;

private:
    async_call_context *context_ = nullptr;
};

/*
 * Installed around one `ffi.call` execution.  Closures only run while a scope
 * is active on the interpreter thread; everything else is recorded as an
 * error and reported once the C call returns.
 */
class callback_scope_guard {
public:
    callback_scope_guard();
    ~callback_scope_guard();

    callback_scope_guard(const callback_scope_guard &) = delete;
    callback_scope_guard &operator=(const callback_scope_guard &) = delete;

    bool failed() const;
    const std::string &error() const;

private:
    callback_call_scope *scope_;
};

extern const native_type callback_native_type;

struct value_t *new_callback_value(const type_ptr &signature,
                                   struct value_t *function,
                                   std::string &error);

bool is_callback_value(const struct value_t *value);
void *callback_code(const struct value_t *value);

/* Reset per-call state before a closure is handed to C again. */
void callback_prepare(struct value_t *value);

} // namespace ffi

#endif
