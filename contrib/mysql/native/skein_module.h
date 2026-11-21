#ifndef SKEIN_MODULE_H
#define SKEIN_MODULE_H

/*
 * Skein native module ABI, version 3.
 *
 * This header is pure C and is the only contract between the host and a
 * native module.  Modules never link against runtime symbols: the host
 * passes a `skein_api` table to `skein_module::init`, and the module stores
 * it.  Values are opaque handles owned by the host; modules create and read
 * them only through the API table.
 *
 * The table is append-only: v1 added new_list/new_dict/as_userdata, v2 added
 * call_task/task_cancelled, and v3 added new_string_len.  Check `size` before
 * using a field that may not exist in an older host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SKEIN_MODULE_ABI_VERSION 3

#if defined(_WIN32)
#define SKEIN_MODULE_EXPORT __declspec(dllexport)
#else
#define SKEIN_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct skein_context skein_context; /* opaque, host-owned */
typedef struct skein_value skein_value;     /* opaque, host-owned value */

typedef enum skein_status {
    SKEIN_OK = 0,
    SKEIN_ERROR = 1
} skein_status;

typedef void (*skein_visit_fn)(void *ctx, const skein_value *value);

/*
 * A native type exposed through userdata values.  `get_field` and
 * `set_field` implement `value.field` on the script side; `visit_children`
 * lets the garbage collector see script values owned by `data`.
 */
typedef struct skein_native_type {
    const char *name;

    /* The returned string is copied by the host and must stay valid until
     * the next call for the same value. */
    const char *(*describe)(void *data);

    /* Called when the last reference to the userdata is released. */
    void (*destroy)(void *data);

    /* Report errors through api->error and return NULL / SKEIN_ERROR. */
    skein_value *(*get_field)(skein_context *ctx, void *data,
                              const char *name);
    skein_status (*set_field)(skein_context *ctx, void *data,
                              const char *name, const skein_value *value);

    void (*visit_children)(void *data, void *ctx, skein_visit_fn fn);
} skein_native_type;

/*
 * Native function signature.  Arguments are borrowed for the duration of the
 * call.  On success the function stores a newly created value in *result and
 * transfers ownership to the host.  On failure it calls api->error and
 * returns SKEIN_ERROR; *result is ignored.
 */
typedef skein_status (*skein_native_fn)(
    skein_context *ctx,
    const skein_value *const *args,
    size_t argc,
    skein_value **result);

typedef struct skein_function {
    const char *name;
    skein_native_fn call;
} skein_function;

typedef enum skein_constant_kind {
    SKEIN_CONST_INT,
    SKEIN_CONST_FLOAT,
    SKEIN_CONST_STRING,
    SKEIN_CONST_BOOL,
    SKEIN_CONST_NULL
} skein_constant_kind;

typedef struct skein_constant {
    const char *name;
    skein_constant_kind kind;
    union {
        int64_t i;
        double f;
        const char *s;
        bool b;
    } value;
} skein_constant;

/* ABI v2 async task callbacks.  See `skein_api::call_task` below. */
typedef void (*skein_task_work_fn)(void *data);
typedef skein_value *(*skein_task_finish_fn)(skein_context *ctx, void *data);
typedef void (*skein_task_destroy_fn)(void *data);

/*
 * Value API.  The table is append-only: new entries are added at the end and
 * `abi_version` is bumped; existing entries never change meaning.
 */
typedef struct skein_api {
    uint32_t abi_version;
    size_t size; /* sizeof(skein_api) for the host build */

    /* ---- errors ---- */
    skein_status (*error)(skein_context *ctx, const char *kind,
                          const char *message);

    /* ---- inspection ---- */
    bool (*is_null)(const skein_value *value);
    bool (*as_bool)(const skein_value *value, bool *out);
    bool (*as_int)(const skein_value *value, int64_t *out);
    bool (*as_float)(const skein_value *value, double *out);
    const char *(*as_string)(const skein_value *value);   /* borrowed */
    const void *(*as_bytes)(const skein_value *value,
                            size_t *len);                 /* borrowed */

    /* ---- construction (result owned by the module until returned) ---- */
    skein_value *(*new_null)(skein_context *ctx);
    skein_value *(*new_bool)(skein_context *ctx, bool value);
    skein_value *(*new_int)(skein_context *ctx, int64_t value);
    skein_value *(*new_float)(skein_context *ctx, double value);
    skein_value *(*new_string)(skein_context *ctx, const char *value);
    skein_value *(*new_bytes)(skein_context *ctx, const void *data,
                              size_t len);
    skein_value *(*new_userdata)(skein_context *ctx,
                                 const skein_native_type *type, void *data);

    /* ---- containers ---- */
    size_t (*list_len)(const skein_value *value);
    skein_value *(*list_get)(const skein_value *value, size_t index);
    bool (*list_push)(skein_context *ctx, skein_value *list,
                      skein_value *value); /* transfers value */
    size_t (*dict_len)(const skein_value *value);
    skein_value *(*dict_get)(const skein_value *value, const char *key);
    bool (*dict_set)(skein_context *ctx, skein_value *dict, const char *key,
                     skein_value *value); /* transfers value */

    /* ---- lifetime and calls ---- */
    skein_value *(*dup_value)(skein_context *ctx, const skein_value *value);
    void (*free_value)(skein_context *ctx, skein_value *value);
    /* Returns an owned value; the module must call free_value when done. */
    skein_value *(*call)(skein_context *ctx, const skein_value *callee,
                         const skein_value *const *args, size_t argc);

    /*
     * Added after the initial v1 table.  Modules compiled against this header
     * may use them; check `size` before using fields past the original call
     * entry when loading into an older host.
     */
    skein_value *(*new_list)(skein_context *ctx);
    skein_value *(*new_dict)(skein_context *ctx);
    /* `type` must be non-null; on success stores the module payload. */
    bool (*as_userdata)(const skein_value *value,
                        const skein_native_type *type, void **data);

    /*
     * ABI v2: asynchronous exports.
     *
     * `work` runs on a worker thread and must not call any skein_api function
     * (the value API is not thread-safe).  When it returns, `finish` runs on
     * the owner thread; its result completes a standard Skein task, which the
     * script can wait on with `task.wait()` or `std.task.wait`.  `destroy`
     * releases `data` on the owner thread and may be null.
     *
     * `task_cancelled` is safe to poll from the worker thread.
     */
    skein_value *(*call_task)(skein_context *ctx, skein_task_work_fn work,
                              skein_task_finish_fn finish,
                              skein_task_destroy_fn destroy, void *data);
    bool (*task_cancelled)(skein_context *ctx, skein_value *task);

    /*
     * ABI v3: length-aware string construction.  Builds a string from an
     * explicit (data, len) buffer, so the input need not be NUL-terminated and
     * embedded NUL bytes are stored in the value's buffer.  Note that script
     * string operations still use UTF-8/NUL-terminated semantics; use bytes
     * values for arbitrary binary data.  Check `size` before using this field
     * on older hosts.
     */
    skein_value *(*new_string_len)(skein_context *ctx, const char *data,
                                   size_t len);
} skein_api;

typedef struct skein_module {
    uint32_t abi_version;

    /* Suggested import path, for example "example.math".  The host owns the
     * internal registry key; modules never declare one. */
    const char *name;

    /* Called once, on first import.  Must not retain `ctx`. */
    skein_status (*init)(skein_context *ctx, const skein_api *api);

    /* Called before a dynamically loaded module is unloaded. */
    void (*fini)(skein_context *ctx);

    const skein_function *functions;
    size_t function_count;

    const skein_native_type *const *types;
    size_t type_count;

    const skein_constant *constants;
    size_t constant_count;
} skein_module;

/* Dynamic modules export exactly this symbol. */
SKEIN_MODULE_EXPORT const skein_module *skein_module_descriptor(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
