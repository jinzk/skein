#include "ffi_callbacks.h"

#include "eval.h"
#include "ffi_marshal.h"
#include "runtime_context.h"
#include "valuetype.h"

#include <ffi.h>

#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace ffi {

struct callback_handle {
    type_ptr signature;
    ffi_closure *closure = nullptr;
    void *code = nullptr;
    ffi_cif cif{};
    std::vector<ffi_type *> arg_types;
    struct value_t *function = nullptr;
    bool failed = false;
    std::string error;

    ~callback_handle() {
        if (function)
            freeval(function);
        if (closure)
            ffi_closure_free(closure);
    }
};

struct callback_call_scope {
    std::thread::id thread;
    std::shared_ptr<keepalive> keep;
    std::string error;
    bool failed = false;
    callback_call_scope *previous = nullptr;
};

namespace {

thread_local callback_call_scope *tls_scope = nullptr;
thread_local int tls_call_depth = 0;

constexpr int MAX_CALLBACK_DEPTH = 32;

std::string describe_callback(const void *data) {
    const callback_handle *handle = (const callback_handle *)data;
    if (!handle || !handle->signature)
        return "<ffi_callback>";
    return "<ffi_callback " + handle->signature->name + ">";
}

void callback_for_each_child(
    const void *data,
    const std::function<void(const struct value_t *)> &fn) {
    const callback_handle *handle = (const callback_handle *)data;
    if (handle && handle->function)
        fn(handle->function);
}

void zero_return(ffi_cif *cif, void *ret) {
    if (!ret || !cif || !cif->rtype || cif->rtype->type == FFI_TYPE_VOID)
        return;
    std::size_t size = cif->rtype->size;
    if (cif->rtype->type != FFI_TYPE_STRUCT && size < sizeof(ffi_arg))
        size = sizeof(ffi_arg);
    std::memset(ret, 0, size);
}

void record_failure(callback_handle *handle, callback_call_scope *scope,
                    const std::string &message) {
    if (handle) {
        handle->failed = true;
        if (handle->error.empty())
            handle->error = message;
    }
    if (scope) {
        scope->failed = true;
        if (scope->error.empty())
            scope->error = message;
    }
}

extern "C" void ffi_callback_entry(ffi_cif *cif, void *ret, void **args,
                                   void *user_data) {
    callback_handle *handle = (callback_handle *)user_data;
    try {
        callback_call_scope *scope = tls_scope;
        if (!scope) {
            if (handle) {
                record_failure(
                    handle, nullptr,
                    skein_context_current()
                        ? "ffi callback invoked outside an active ffi.call"
                        : "ffi callback invoked from a non-interpreter thread");
            }
            zero_return(cif, ret);
            return;
        }
        if (tls_call_depth > MAX_CALLBACK_DEPTH) {
            record_failure(handle, scope,
                           "ffi callback recursion limit exceeded");
            zero_return(cif, ret);
            return;
        }
        if (!handle || !handle->function || handle->failed ||
            !handle->signature) {
            zero_return(cif, ret);
            return;
        }

        std::vector<struct value_t *> call_args;
        const std::size_t count = handle->signature->params.size();
        call_args.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            struct value_t *value = read_value(
                handle->signature->params[i],
                (const unsigned char *)args[i], nullptr);
            if (!value) {
                for (struct value_t *arg : call_args)
                    freeval(arg);
                record_failure(handle, scope,
                               "ffi callback argument conversion failed");
                zero_return(cif, ret);
                return;
            }
            call_args.push_back(value);
        }

        eval_capture_snapshot snapshot;
        eval_capture_begin(snapshot);
        struct value_t *result =
            skein_call_function(handle->function, std::move(call_args));
        eval_capture_result captured = eval_capture_end(snapshot);

        if (captured.failed || !result) {
            std::string message = captured.failed
                                      ? error_message_of(captured.error)
                                      : std::string("ffi callback failed");
            freeval(captured.error);
            freeval(result);
            record_failure(handle, scope, message);
            zero_return(cif, ret);
            return;
        }
        freeval(captured.error);

        if (handle->signature->result->kind == kind_t::void_) {
            freeval(result);
            zero_return(cif, ret);
            return;
        }

        std::string error;
        keepalive &keep = *scope->keep;
        if (!write_return(handle->signature->result, ret, result, keep,
                          error)) {
            freeval(result);
            record_failure(handle, scope, "ffi callback result: " + error);
            zero_return(cif, ret);
            return;
        }
        freeval(result);
    } catch (...) {
        if (handle)
            record_failure(handle, tls_scope, "ffi callback failed");
        zero_return(cif, ret);
    }
}

std::shared_ptr<callback_handle> get_callback(const struct value_t *value) {
    if (!value || value->type != T_userdata ||
        ((const userdata_value_t *)value)->meta != &callback_native_type)
        return nullptr;
    return std::static_pointer_cast<callback_handle>(
        ((const userdata_value_t *)value)->data);
}

} // namespace

const native_type callback_native_type = {
    "ffi_callback", describe_callback, nullptr, nullptr, nullptr,
    callback_for_each_child};

callback_scope_guard::callback_scope_guard() {
    scope_ = new callback_call_scope();
    scope_->thread = std::this_thread::get_id();
    scope_->keep = std::make_shared<keepalive>();
    scope_->previous = tls_scope;
    tls_scope = scope_;
    ++tls_call_depth;
}

callback_scope_guard::~callback_scope_guard() {
    tls_scope = scope_->previous;
    --tls_call_depth;
    delete scope_;
}

bool callback_scope_guard::failed() const { return scope_->failed; }

const std::string &callback_scope_guard::error() const {
    return scope_->error;
}

struct value_t *new_callback_value(const type_ptr &signature,
                                   struct value_t *function,
                                   std::string &error) {
    if (!signature || signature->kind != kind_t::callback) {
        error = "expected a callback signature";
        return NULL;
    }
    if (!function) {
        error = "missing callback function";
        return NULL;
    }
    std::shared_ptr<callback_handle> handle =
        std::make_shared<callback_handle>();
    handle->signature = signature;
    handle->function = dupval(function);
    handle->arg_types.reserve(signature->params.size());
    for (const type_ptr &param : signature->params)
        handle->arg_types.push_back(param->cif);

    handle->closure = (ffi_closure *)ffi_closure_alloc(
        sizeof(ffi_closure), &handle->code);
    if (!handle->closure) {
        error = "cannot allocate a callback closure";
        return NULL;
    }
    if (ffi_prep_cif(&handle->cif, FFI_DEFAULT_ABI,
                     (unsigned int)signature->params.size(),
                     signature->result->cif,
                     handle->arg_types.empty()
                         ? nullptr
                         : handle->arg_types.data()) != FFI_OK) {
        error = "cannot prepare the callback interface";
        return NULL;
    }
    if (ffi_prep_closure_loc(handle->closure, &handle->cif,
                             ffi_callback_entry, handle.get(),
                             handle->code) != FFI_OK) {
        error = "cannot prepare the callback closure";
        return NULL;
    }
    return newuserdataval(&callback_native_type, handle);
}

bool is_callback_value(const struct value_t *value) {
    return get_callback(value) != nullptr;
}

void *callback_code(const struct value_t *value) {
    std::shared_ptr<callback_handle> handle = get_callback(value);
    if (!handle || !handle->code)
        return nullptr;
    return handle->code;
}

void callback_prepare(struct value_t *value) {
    std::shared_ptr<callback_handle> handle = get_callback(value);
    if (!handle)
        return;
    handle->failed = false;
    handle->error.clear();
}

} // namespace ffi
