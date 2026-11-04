#include "ffi_callbacks.h"

#include "eval.h"
#include "ffi_marshal.h"
#include "runtime_context.h"
#include "valuetype.h"

#include <ffi.h>

#include <atomic>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ffi {

struct callback_handle : public std::enable_shared_from_this<callback_handle> {
    type_ptr signature;
    ffi_closure *closure = nullptr;
    void *code = nullptr;
    ffi_cif cif{};
    std::vector<ffi_type *> arg_types;
    struct value_t *function = nullptr;
    std::atomic<bool> failed{false};
    std::string error;

    ~callback_handle() {
        if (function)
            freeval(function);
        if (closure)
            ffi_closure_free(closure);
    }
};

struct async_call_context {
    std::shared_ptr<keepalive> keep;
    std::shared_ptr<std::atomic<bool>> failed;
    async_call_context *previous = nullptr;
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
thread_local async_call_context *tls_async_call = nullptr;

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


struct pending_callback {
    std::shared_ptr<callback_handle> handle;
    bool error_only = false;
    std::string error;
    std::vector<std::shared_ptr<std::vector<unsigned char>>> args;
};

std::mutex &background_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::deque<pending_callback> &background_queue() {
    static std::deque<pending_callback> queue;
    return queue;
}

struct blocking_callback_job {
    std::shared_ptr<callback_handle> handle;
    std::shared_ptr<std::atomic<bool>> failed_flag;
    std::vector<std::shared_ptr<std::vector<unsigned char>>> args;
    std::vector<unsigned char> result;
    std::shared_ptr<keepalive> keep;
    bool done = false;
    bool failed = false;
    std::string error;
    std::mutex mutex;
    std::condition_variable cv;
};

std::deque<std::shared_ptr<blocking_callback_job>> &blocking_queue() {
    static std::deque<std::shared_ptr<blocking_callback_job>> queue;
    return queue;
}

std::shared_ptr<blocking_callback_job> make_blocking_job(
    callback_handle *handle, void **args, ffi_cif *cif) {
    std::shared_ptr<blocking_callback_job> job =
        std::make_shared<blocking_callback_job>();
    job->handle = handle->shared_from_this();
    job->keep = tls_async_call ? tls_async_call->keep
                               : std::make_shared<keepalive>();
    job->failed_flag = tls_async_call ? tls_async_call->failed : nullptr;
    const std::vector<type_ptr> &params = handle->signature->params;
    job->args.reserve(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        std::shared_ptr<std::vector<unsigned char>> block =
            std::make_shared<std::vector<unsigned char>>(
                params[i]->size ? params[i]->size : 1);
        if (params[i]->size)
            std::memcpy(block->data(), args[i], params[i]->size);
        job->args.push_back(std::move(block));
    }
    size_t size = 0;
    if (cif && cif->rtype && cif->rtype->type != FFI_TYPE_VOID) {
        size = cif->rtype->size;
        if (cif->rtype->type != FFI_TYPE_STRUCT && size < sizeof(ffi_arg))
            size = sizeof(ffi_arg);
    }
    job->result.assign(size ? size : 1, 0);
    {
        std::lock_guard<std::mutex> lock(background_mutex());
        blocking_queue().push_back(job);
    }
    return job;
}

void enqueue_background_error(callback_handle *handle,
                              const char *message) {
    if (!handle)
        return;
    std::shared_ptr<callback_handle> owned;
    try {
        owned = handle->shared_from_this();
    } catch (...) {
        return;
    }
    pending_callback job;
    job.handle = std::move(owned);
    job.error_only = true;
    job.error = message ? message : "ffi callback failed";
    std::lock_guard<std::mutex> lock(background_mutex());
    background_queue().push_back(std::move(job));
}

void enqueue_background_call(callback_handle *handle, void **args) {
    if (!handle || !handle->signature)
        return;
    std::shared_ptr<callback_handle> owned;
    try {
        owned = handle->shared_from_this();
    } catch (...) {
        return;
    }
    pending_callback job;
    job.handle = std::move(owned);
    const std::vector<type_ptr> &params = handle->signature->params;
    job.args.reserve(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        const type_ptr &type = params[i];
        std::shared_ptr<std::vector<unsigned char>> block =
            std::make_shared<std::vector<unsigned char>>(
                type->size ? type->size : 1);
        if (type->size)
            std::memcpy(block->data(), args[i], type->size);
        job.args.push_back(std::move(block));
    }
    std::lock_guard<std::mutex> lock(background_mutex());
    background_queue().push_back(std::move(job));
}

extern "C" void ffi_callback_entry(ffi_cif *cif, void *ret, void **args,
                                   void *user_data) {
    callback_handle *handle = (callback_handle *)user_data;
    try {
        callback_call_scope *scope = tls_scope;
        if (!scope) {
            if (!handle) {
                zero_return(cif, ret);
                return;
            }
            if (tls_async_call) {
                if (handle->failed || !handle->function) {
                    zero_return(cif, ret);
                    return;
                }
                std::shared_ptr<blocking_callback_job> job =
                    make_blocking_job(handle, args, cif);
                std::unique_lock<std::mutex> lock(job->mutex);
                job->cv.wait(lock, [&job] { return job->done; });
                lock.unlock();
                if (job->failed) {
                    zero_return(cif, ret);
                    return;
                }
                if (ret && cif && cif->rtype &&
                    cif->rtype->type != FFI_TYPE_VOID)
                    std::memcpy(ret, job->result.data(),
                                job->result.size());
                return;
            }
            if (skein_context_current() != nullptr) {
                record_failure(handle, nullptr,
                               "ffi callback invoked outside an active "
                               "ffi.call");
            } else if (!handle->signature ||
                       handle->signature->result->kind != kind_t::void_) {
                handle->failed = true;
                enqueue_background_error(
                    handle, "cross-thread callbacks must return void");
            } else {
                enqueue_background_call(handle, args);
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

async_scope::async_scope(std::shared_ptr<std::atomic<bool>> failed) {
    context_ = new async_call_context();
    context_->keep = std::make_shared<keepalive>();
    context_->failed = std::move(failed);
    context_->previous = tls_async_call;
    tls_async_call = context_;
}

async_scope::~async_scope() {
    tls_async_call = context_->previous;
    delete context_;
}

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


void drain_blocking_job(
    const std::shared_ptr<blocking_callback_job> &job) {
    if (!job->handle || !job->handle->function) {
        job->failed = true;
        job->error = "ffi callback is no longer available";
    } else {
        std::vector<value_t *> call_args;
        bool ok = true;
        const std::vector<type_ptr> &params = job->handle->signature->params;
        for (size_t i = 0; i < params.size(); ++i) {
            value_t *arg = read_value(params[i], job->args[i]->data(),
                                      job->args[i]);
            if (!arg) {
                ok = false;
                break;
            }
            call_args.push_back(arg);
        }
        if (!ok) {
            for (value_t *arg : call_args)
                freeval(arg);
            job->failed = true;
            job->error = "ffi callback argument conversion failed";
        } else {
            eval_capture_snapshot snapshot;
            eval_capture_begin(snapshot);
            value_t *result = skein_call_function(job->handle->function,
                                                  std::move(call_args));
            eval_capture_result captured = eval_capture_end(snapshot);
            if (captured.failed || !result) {
                job->failed = true;
                job->error = captured.failed
                                 ? error_message_of(captured.error)
                                 : std::string("ffi callback failed");
            } else if (job->handle->signature->result->kind !=
                       kind_t::void_) {
                std::string error;
                if (!write_return(job->handle->signature->result,
                                  job->result.data(), result, *job->keep,
                                  error)) {
                    job->failed = true;
                    job->error = error;
                }
            }
            freeval(captured.error);
            freeval(result);
        }
    }
    if (job->failed) {
        if (job->failed_flag)
            *job->failed_flag = true;
        yyerror("ffi cross-thread callback: %s", job->error.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->done = true;
    }
    job->cv.notify_all();
}

struct drain_guard {
    bool &flag;
    explicit drain_guard(bool &value) : flag(value) { flag = true; }
    ~drain_guard() { flag = false; }
};

void background_drain() {
    static thread_local bool draining = false;
    if (draining)
        return;
    std::deque<pending_callback> jobs;
    std::deque<std::shared_ptr<blocking_callback_job>> blocking;
    {
        std::lock_guard<std::mutex> lock(background_mutex());
        if (background_queue().empty() && blocking_queue().empty())
            return;
        jobs.swap(background_queue());
        blocking.swap(blocking_queue());
    }

    drain_guard guard(draining);
    for (const std::shared_ptr<blocking_callback_job> &job : blocking)
        drain_blocking_job(job);
    for (pending_callback &job : jobs) {
        if (job.error_only) {
            yyerror("ffi cross-thread callback: %s", job.error.c_str());
            continue;
        }
        std::shared_ptr<callback_handle> handle = job.handle;
        if (!handle || !handle->function)
            continue;

        std::vector<value_t *> call_args;
        bool ok = true;
        for (size_t i = 0; i < handle->signature->params.size(); ++i) {
            value_t *arg = read_value(handle->signature->params[i],
                                      job.args[i]->data(), job.args[i]);
            if (!arg) {
                ok = false;
                break;
            }
            call_args.push_back(arg);
        }
        if (!ok) {
            for (value_t *arg : call_args)
                freeval(arg);
            continue;
        }

        eval_capture_snapshot snapshot;
        eval_capture_begin(snapshot);
        value_t *result =
            skein_call_function(handle->function, std::move(call_args));
        eval_capture_result captured = eval_capture_end(snapshot);
        if (captured.failed || !result) {
            std::string message =
                captured.failed ? error_message_of(captured.error)
                                : std::string("ffi callback failed");
            freeval(captured.error);
            freeval(result);
            handle->failed = true;
            handle->error = message;
            yyerror("ffi cross-thread callback: %s", message.c_str());
        } else {
            freeval(captured.error);
            freeval(result);
        }
    }
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
