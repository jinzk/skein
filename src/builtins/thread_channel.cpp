#include "thread_channel.h"

#include "builtin_ids.h"
#include "builtins.h"
#include "cycle.h"
#include "dict.h"
#include "error_report.h"
#include "eval.h"
#include "eval_context.h"
#include "event_loop.h"
#include "fiber.h"
#include "package.h"
#include "runtime_context.h"
#include "runtime_state.h"
#include "scopestack.h"
#include "symbol.h"
#include "task.h"
#include "vm.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct message_state {
    struct value_t *payload = nullptr;
    /* Owned dict of name -> handle userdata. */
    struct value_t *handles = nullptr;

    ~message_state() {
        freeval(payload);
        payload = nullptr;
        freeval(handles);
        handles = nullptr;
    }
};

struct channel_state : std::enable_shared_from_this<channel_state> {
    explicit channel_state(long long cap) : capacity(cap) {}

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<struct value_t *> queue;
    long long capacity = 0; /* -1 means unbounded */
    bool closed = false;

    ~channel_state() {
        for (struct value_t *value : queue)
            freeval(value);
        queue.clear();
    }
};

extern const struct native_type kMessageType;
extern const struct native_type kChannelType;

bool is_handle_value(struct value_t *value) {
    if (!value || value->type != T_userdata)
        return false;
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    return userdata->meta && userdata->meta->name &&
           std::strcmp(userdata->meta->name, "thread_channel") == 0;
}

struct value_t *copy_data_value(struct value_t *value,
                                std::unordered_set<const void *> &visiting,
                                std::string &error);

struct value_t *copy_dict_value(struct value_t *value,
                                std::unordered_set<const void *> &visiting,
                                std::string &error) {
    const void *payload = value_payload(value);
    if (payload && !visiting.insert(payload).second) {
        error = "Message payload contains a cycle";
        return nullptr;
    }
    struct dict_value_t *source = (struct dict_value_t *)value;
    struct value_t *copy = (struct value_t *)newdictval();
    for (const auto &entry : source->payload->items) {
        struct value_t *key =
            copy_data_value(entry.first, visiting, error);
        if (!key) {
            freeval(copy);
            if (payload)
                visiting.erase(payload);
            return nullptr;
        }
        struct value_t *item =
            copy_data_value(entry.second, visiting, error);
        if (!item) {
            freeval(key);
            freeval(copy);
            if (payload)
                visiting.erase(payload);
            return nullptr;
        }
        dict_append_item((struct dict_value_t *)copy, key, item);
    }
    if (payload)
        visiting.erase(payload);
    return copy;
}

struct value_t *copy_data_value(struct value_t *value,
                                std::unordered_set<const void *> &visiting,
                                std::string &error) {
    if (!value) {
        error = "Message payload is null";
        return nullptr;
    }
    switch (value->type) {
        case T_null:
        case T_bool:
        case T_int:
        case T_float:
        case T_char:
        case T_byte:
        case T_string:
        case T_bytes:
            return dupval(value);
        case T_error: {
            struct error_value_t *error = (struct error_value_t *)value;
            return newerrorval(error->kind, error->message);
        }
        case T_result: {
            struct result_value_t *result = (struct result_value_t *)value;
            struct value_t *copy = copy_data_value(
                result->ok ? result->value : result->error, visiting, error);
            if (!copy)
                return nullptr;
            return result->ok ? newresultok(copy) : newresulterr(copy);
        }
        case T_list: {
            const void *payload = value_payload(value);
            if (payload && !visiting.insert(payload).second) {
                error = "Message payload contains a cycle";
                return nullptr;
            }
            struct list_value_t *source = (struct list_value_t *)value;
            struct value_t *copy = (struct value_t *)newlistval();
            struct list_value_t *list = (struct list_value_t *)copy;
            list->payload->items.reserve(source->payload->items.size());
            for (struct value_t *item : source->payload->items) {
                struct value_t *child =
                    copy_data_value(item, visiting, error);
                if (!child) {
                    freeval(copy);
                    if (payload)
                        visiting.erase(payload);
                    return nullptr;
                }
                list->payload->items.push_back(child);
            }
            if (payload)
                visiting.erase(payload);
            return copy;
        }
        case T_dict:
            return copy_dict_value(value, visiting, error);
        default:
            error = std::string("Message payload does not support ") +
                    typename_of(value);
            return nullptr;
    }
}

struct value_t *copy_handles_value(struct value_t *handles) {
    struct value_t *copy = (struct value_t *)newdictval();
    if (!handles || handles->type != T_dict)
        return copy;
    struct dict_value_t *source = (struct dict_value_t *)handles;
    for (const auto &entry : source->payload->items) {
        dict_append_item((struct dict_value_t *)copy, dupval(entry.first),
                         dupval(entry.second));
    }
    return copy;
}

bool get_message_state(struct value_t *value,
                       std::shared_ptr<message_state> &state) {
    if (!value || value->type != T_userdata)
        return false;
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    if (userdata->meta != &kMessageType || !userdata->data)
        return false;
    state = std::static_pointer_cast<message_state>(userdata->data);
    return true;
}

struct value_t *new_message_value(std::shared_ptr<message_state> state) {
    return newuserdataval(&kMessageType, std::move(state));
}

struct value_t *new_channel_value(std::shared_ptr<channel_state> state) {
    return newuserdataval(&kChannelType, std::move(state));
}

std::string message_describe(const void *) {
    return "<thread_message>";
}

struct value_t *message_get_field(void *data, const char *name) {
    message_state *state = static_cast<message_state *>(data);
    if (!state || !name) {
        yyerror("thread.Message has no such field");
        return nullptr;
    }
    if (std::strcmp(name, "payload") == 0) {
        std::unordered_set<const void *> visiting;
        std::string error;
        struct value_t *copy =
            copy_data_value(state->payload, visiting, error);
        if (!copy) {
            yyerror("%s", error.c_str());
            return nullptr;
        }
        return copy;
    }
    if (std::strcmp(name, "handles") == 0)
        return copy_handles_value(state->handles);
    yyerror("thread.Message has no field %s", name);
    return nullptr;
}

const struct native_type kMessageType = {
    "thread_message",
    message_describe,
    nullptr,
    message_get_field,
    nullptr,
    nullptr,
};

std::string channel_describe(const void *) {
    return "<thread_channel>";
}

struct value_t *channel_send_value(
    const std::shared_ptr<channel_state> &state, struct value_t *message,
    bool blocking) {
    std::shared_ptr<message_state> message_data;
    if (!get_message_state(message, message_data)) {
        yyerror("std.thread.Channel.send expects a Message");
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (blocking) {
        /* NOTE: this blocks the current OS thread.  The fiber-aware
         * transport will replace this with a task wait. */
        state->cv.wait(lock, [&state] {
            return state->closed || state->capacity < 0 ||
                   state->queue.size() < (size_t)state->capacity;
        });
    } else if (state->closed ||
               (state->capacity >= 0 &&
                state->queue.size() >= (size_t)state->capacity)) {
        return newboolval(false);
    }

    if (state->closed) {
        yyerror("thread.closed: channel is closed");
        return nullptr;
    }
    state->queue.push_back(dupval(message));
    state->cv.notify_all();
    return blocking ? newnull() : newboolval(true);
}

struct value_t *channel_recv_value(
    const std::shared_ptr<channel_state> &state, long long timeout_ms) {
    std::unique_lock<std::mutex> lock(state->mutex);
    if (timeout_ms > 0) {
        if (!state->cv.wait_for(
                lock, std::chrono::milliseconds(timeout_ms),
                [&state] { return state->closed || !state->queue.empty(); })) {
            yyerror("std.thread.Channel.recv timed out");
            return nullptr;
        }
    } else {
        state->cv.wait(lock, [&state] {
            return state->closed || !state->queue.empty();
        });
    }
    if (state->queue.empty())
        return newnull();
    struct value_t *message = state->queue.front();
    state->queue.pop_front();
    return message;
}

struct value_t *channel_try_recv_value(
    const std::shared_ptr<channel_state> &state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    struct value_t *result = (struct value_t *)newdictval();
    struct dict_value_t *dict = (struct dict_value_t *)result;
    if (!state->queue.empty()) {
        struct value_t *message = state->queue.front();
        state->queue.pop_front();
        dict_append_item(dict, newstrval("ok"), newboolval(true));
        dict_append_item(dict, newstrval("closed"),
                         newboolval(state->closed));
        dict_append_item(dict, newstrval("value"), message);
        return result;
    }
    dict_append_item(dict, newstrval("ok"), newboolval(false));
    dict_append_item(dict, newstrval("closed"),
                     newboolval(state->closed));
    return result;
}

struct value_t *channel_get_field(void *data, const char *name) {
    channel_state *raw = static_cast<channel_state *>(data);
    if (!raw || !name) {
        yyerror("thread.Channel has no such field");
        return nullptr;
    }
    std::shared_ptr<channel_state> state = raw->shared_from_this();

    if (std::strcmp(name, "send") == 0) {
        return newnativeval(
            [state](const std::vector<struct value_t *> &args) {
                if (args.size() != 1) {
                    yyerror("Channel.send expects 1 argument");
                    return (struct value_t *)nullptr;
                }
                return channel_send_value(state, args[0], true);
            },
            state);
    }
    if (std::strcmp(name, "try_send") == 0) {
        return newnativeval(
            [state](const std::vector<struct value_t *> &args) {
                if (args.size() != 1) {
                    yyerror("Channel.try_send expects 1 argument");
                    return (struct value_t *)nullptr;
                }
                return channel_send_value(state, args[0], false);
            },
            state);
    }
    if (std::strcmp(name, "recv") == 0) {
        return newnativeval(
            [state](const std::vector<struct value_t *> &args) {
                long long timeout_ms = 0;
                if (args.size() > 1) {
                    yyerror("Channel.recv expects 0 or 1 argument");
                    return (struct value_t *)nullptr;
                }
                if (args.size() == 1) {
                    if (args[0]->type != T_int ||
                        ((int_value_t *)args[0])->value < 0) {
                        yyerror("Channel.recv timeout must be a "
                                "non-negative integer");
                        return (struct value_t *)nullptr;
                    }
                    timeout_ms = ((int_value_t *)args[0])->value;
                }
                return channel_recv_value(state, timeout_ms);
            },
            state);
    }
    if (std::strcmp(name, "try_recv") == 0) {
        return newnativeval(
            [state](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("Channel.try_recv expects no arguments");
                    return (struct value_t *)nullptr;
                }
                return channel_try_recv_value(state);
            },
            state);
    }
    if (std::strcmp(name, "close") == 0) {
        return newnativeval(
            [state](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("Channel.close expects no arguments");
                    return (struct value_t *)nullptr;
                }
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->closed = true;
                }
                state->cv.notify_all();
                return newnull();
            },
            state);
    }
    if (std::strcmp(name, "closed") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return newboolval(state->closed);
    }
    if (std::strcmp(name, "capacity") == 0)
        return newintval(state->capacity);
    if (std::strcmp(name, "length") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return newintval((long long)state->queue.size());
    }

    yyerror("thread.Channel has no field %s", name);
    return nullptr;
}

const struct native_type kChannelType = {
    "thread_channel",
    channel_describe,
    nullptr,
    channel_get_field,
    nullptr,
    nullptr,
};

/* ------------------------------------------------------------------ */
/* Thread pool                                                         */
/* ------------------------------------------------------------------ */

bool channel_try_push(const std::shared_ptr<channel_state> &state,
                      struct value_t *message) {
    if (!state || !message)
        return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed)
        return false;
    if (state->capacity >= 0 &&
        state->queue.size() >= (size_t)state->capacity)
        return false;
    state->queue.push_back(dupval(message));
    state->cv.notify_all();
    return true;
}

void channel_close_state(const std::shared_ptr<channel_state> &state) {
    if (!state)
        return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->closed = true;
    }
    state->cv.notify_all();
}

bool get_channel_state(struct value_t *value,
                       std::shared_ptr<channel_state> &state) {
    if (!value || value->type != T_userdata)
        return false;
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    if (userdata->meta != &kChannelType || !userdata->data)
        return false;
    state = std::static_pointer_cast<channel_state>(userdata->data);
    return true;
}

struct value_t *make_message_owned(struct value_t *payload,
                                   struct value_t *handles) {
    std::shared_ptr<message_state> state = std::make_shared<message_state>();
    state->payload = payload;
    state->handles = handles ? handles : (struct value_t *)newdictval();
    return new_message_value(std::move(state));
}

void dict_set_owned(struct value_t *dict, const char *key,
                    struct value_t *value) {
    if (!dict || dict->type != T_dict)
        return;
    struct dict_value_t *object = (struct dict_value_t *)dict;
    for (auto &entry : object->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((struct string_value_t *)entry.first)->value,
                        key) == 0) {
            freeval(entry.second);
            entry.second = value;
            return;
        }
    }
    dict_append_item(object, newstrval(key), value);
}

struct value_t *dict_get_handle(struct value_t *dict, const char *key) {
    if (!dict || dict->type != T_dict)
        return nullptr;
    struct dict_value_t *object = (struct dict_value_t *)dict;
    for (const auto &entry : object->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((struct string_value_t *)entry.first)->value,
                        key) == 0)
            return entry.second;
    }
    return nullptr;
}

struct pool_state;
struct future_state;

struct pool_state : std::enable_shared_from_this<pool_state> {
    std::shared_ptr<channel_state> jobs;
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    size_t size = 0;
    size_t ready = 0;
    std::string script;
    std::string entry = "handle";
    std::string startup_error;
    bool closing = false;
    bool joined = false;
    long long submitted = 0;
    long long completed = 0;
    long long failed = 0;

    ~pool_state() {
        if (jobs)
            channel_close_state(jobs);
        for (std::thread &thread : threads) {
            if (thread.joinable())
                thread.join();
        }
    }
};

struct future_state : std::enable_shared_from_this<future_state> {
    std::shared_ptr<channel_state> reply;
};

extern const struct native_type kPoolType;
extern const struct native_type kFutureType;

struct value_t *new_pool_value(std::shared_ptr<pool_state> state) {
    return newuserdataval(&kPoolType, std::move(state));
}

struct value_t *new_future_value(std::shared_ptr<future_state> state) {
    return newuserdataval(&kFutureType, std::move(state));
}

struct value_t *pool_worker_call(
    struct value_t *callee, std::vector<struct value_t *> args) {
    return call_value_args(callee, std::move(args));
}

struct value_t *pool_worker_call_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    return call_builtin(functype, args);
}

bool callable_value(struct value_t *value) {
    if (!value)
        return false;
    return value->type == T_func || value->type == T_builtin ||
           value->type == T_method || value->type == T_native;
}

bool load_worker_script(pool_state &pool, struct value_t *&handler,
                        std::string &error) {
    handler = nullptr;
    package_configure(pool.script, {});
    if (!parse_worker_source(pool.script, error))
        return false;
    handler = lookup_global_symbol(pool.entry.c_str());
    if (!callable_value(handler)) {
        error = "worker entry function not found: " + pool.entry;
        return false;
    }
    return true;
}

void worker_thread_main(pool_state *pool) {
    struct skein_context hooks = {
        pool_worker_call,
        pool_worker_call_builtin,
        runtime_error_report,
        nullptr,
    };
    skein_context_set(&hooks);
    skein_vm_set_enabled(false);
    g_interactive = false;
    g_show_prompt = false;

    eval_context context;
    set_active_eval_context(&context);
    initscope();
    eval_begin_root_defers();

    struct value_t *handler = nullptr;
    std::string error;
    bool loaded = load_worker_script(*pool, handler, error);

    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        if (!loaded)
            pool->startup_error = error;
        ++pool->ready;
    }
    pool->cv.notify_all();

    if (loaded) {
        for (;;) {
            struct value_t *job = channel_recv_value(pool->jobs, 0);
            if (!job)
                break;
            if (job->type == T_null) {
                freeval(job);
                break;
            }
            std::shared_ptr<message_state> message;
            if (!get_message_state(job, message)) {
                freeval(job);
                continue;
            }
            struct value_t *payload = nullptr;
            std::unordered_set<const void *> visiting;
            std::string copy_error;
            payload = copy_data_value(message->payload, visiting, copy_error);
            if (!payload) {
                std::lock_guard<std::mutex> lock(pool->mutex);
                ++pool->failed;
                freeval(job);
                continue;
            }

            struct value_t *reply_value =
                dict_get_handle(message->handles, "reply");
            std::shared_ptr<channel_state> reply;
            if (!get_channel_state(reply_value, reply)) {
                freeval(payload);
                freeval(job);
                continue;
            }

            eval_capture_snapshot snapshot;
            eval_capture_begin(snapshot);
            struct value_t *result =
                skein_call_function(handler, {payload});
            if (result && is_task_value(result)) {
                task_ptr task;
                if (get_task_value(result, task)) {
                    freeval(result);
                    result = fiber_await_task(task);
                }
            }
            eval_capture_result captured = eval_capture_end(snapshot);

            struct value_t *envelope = (struct value_t *)newdictval();
            bool ok = !captured.failed && result;
            if (ok) {
                dict_append_item((struct dict_value_t *)envelope,
                                 newstrval("ok"), newboolval(true));
                dict_append_item((struct dict_value_t *)envelope,
                                 newstrval("value"), result);
                result = nullptr;
            } else {
                struct value_t *err = captured.error;
                captured.error = nullptr;
                if (!err)
                    err = newerrorval("worker", "worker handler failed");
                dict_append_item((struct dict_value_t *)envelope,
                                 newstrval("ok"), newboolval(false));
                dict_append_item((struct dict_value_t *)envelope,
                                 newstrval("error"), err);
                freeval(result);
            }

            struct value_t *reply_message =
                make_message_owned(envelope, (struct value_t *)newdictval());
            channel_try_push(reply, reply_message);
            freeval(reply_message);

            {
                std::lock_guard<std::mutex> lock(pool->mutex);
                if (ok)
                    ++pool->completed;
                else
                    ++pool->failed;
            }
            freeval(job);
            freeval(captured.error);
        }
    }

    eval_run_root_defers();
    fiber_disable_generator_warnings();
    task_shutdown();
    fiber_shutdown();
    event_loop_shutdown();
    ast_thread_shutdown();
    skein_vm_shutdown();
    set_active_eval_context(nullptr);
    skein_context_set(nullptr);
}

struct value_t *pool_submit(pool_state &pool, struct value_t *value) {
    std::shared_ptr<message_state> source;
    struct value_t *payload = nullptr;
    struct value_t *handles = (struct value_t *)newdictval();
    std::unordered_set<const void *> visiting;
    std::string error;

    if (get_message_state(value, source)) {
        payload = copy_data_value(source->payload, visiting, error);
        freeval(handles);
        handles = copy_handles_value(source->handles);
    } else {
        payload = copy_data_value(value, visiting, error);
    }
    if (!payload) {
        freeval(handles);
        yyerror("%s", error.c_str());
        return nullptr;
    }

    std::shared_ptr<channel_state> reply =
        std::make_shared<channel_state>(1);
    struct value_t *reply_value = new_channel_value(reply);
    dict_set_owned(handles, "reply", reply_value);

    struct value_t *message = make_message_owned(payload, handles);
    struct value_t *sent = channel_send_value(pool.jobs, message, true);
    freeval(message);
    if (!sent)
        return nullptr;

    std::shared_ptr<future_state> future =
        std::make_shared<future_state>();
    future->reply = reply;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        ++pool.submitted;
    }
    return new_future_value(std::move(future));
}

struct value_t *future_wait(struct future_state &future,
                            long long timeout_ms) {
    struct value_t *message =
        channel_recv_value(future.reply, timeout_ms);
    if (!message)
        return nullptr;
    std::shared_ptr<message_state> state;
    if (!get_message_state(message, state)) {
        freeval(message);
        return nullptr;
    }
    std::unordered_set<const void *> visiting;
    std::string error;
    struct value_t *payload =
        copy_data_value(state->payload, visiting, error);
    freeval(message);
    if (!payload) {
        yyerror("%s", error.c_str());
        return nullptr;
    }
    return payload;
}

struct value_t *future_try_wait(struct future_state &future) {
    struct value_t *raw = channel_try_recv_value(future.reply);
    if (!raw || raw->type != T_dict)
        return raw;
    struct dict_value_t *dict = (struct dict_value_t *)raw;
    int value_index = -1;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        struct value_t *key = dict->payload->items[i].first;
        if (key->type == T_string &&
            std::strcmp(((struct string_value_t *)key)->value, "value") == 0) {
            value_index = (int)i;
            break;
        }
    }
    if (value_index < 0)
        return raw;
    struct value_t *message = dict->payload->items[(size_t)value_index].second;
    std::shared_ptr<message_state> state;
    if (!get_message_state(message, state))
        return raw;
    std::unordered_set<const void *> visiting;
    std::string error;
    struct value_t *payload =
        copy_data_value(state->payload, visiting, error);
    if (!payload)
        return raw;
    freeval(dict->payload->items[(size_t)value_index].second);
    dict->payload->items[(size_t)value_index].second = payload;
    return raw;
}

std::string pool_describe(const void *) {
    return "<thread_pool>";
}

std::string future_describe(const void *) {
    return "<thread_future>";
}

struct value_t *pool_get_field(void *data, const char *name) {
    pool_state *raw = static_cast<pool_state *>(data);
    if (!raw || !name) {
        yyerror("thread.pool has no such field");
        return nullptr;
    }
    std::shared_ptr<pool_state> pool = raw->shared_from_this();

    if (std::strcmp(name, "submit") == 0) {
        return newnativeval(
            [pool](const std::vector<struct value_t *> &args) {
                if (args.size() != 1) {
                    yyerror("pool.submit expects 1 argument");
                    return (struct value_t *)nullptr;
                }
                {
                    std::lock_guard<std::mutex> lock(pool->mutex);
                    if (pool->closing) {
                        yyerror("thread.closed: pool is closed");
                        return (struct value_t *)nullptr;
                    }
                }
                return pool_submit(*pool, args[0]);
            },
            pool);
    }
    if (std::strcmp(name, "close") == 0) {
        return newnativeval(
            [pool](const std::vector<struct value_t *> &) {
                {
                    std::lock_guard<std::mutex> lock(pool->mutex);
                    pool->closing = true;
                }
                channel_close_state(pool->jobs);
                return newnull();
            },
            pool);
    }
    if (std::strcmp(name, "join") == 0) {
        return newnativeval(
            [pool](const std::vector<struct value_t *> &) {
                {
                    std::lock_guard<std::mutex> lock(pool->mutex);
                    if (pool->joined)
                        return newnull();
                    pool->joined = true;
                }
                channel_close_state(pool->jobs);
                for (std::thread &thread : pool->threads) {
                    if (thread.joinable())
                        thread.join();
                }
                return newnull();
            },
            pool);
    }
    if (std::strcmp(name, "stats") == 0) {
        return newnativeval(
            [pool](const std::vector<struct value_t *> &) {
                struct value_t *stats = (struct value_t *)newdictval();
                struct dict_value_t *dict = (struct dict_value_t *)stats;
                std::lock_guard<std::mutex> lock(pool->mutex);
                dict_append_item(dict, newstrval("size"),
                                 newintval((long long)pool->size));
                dict_append_item(dict, newstrval("ready"),
                                 newintval((long long)pool->ready));
                dict_append_item(dict, newstrval("submitted"),
                                 newintval(pool->submitted));
                dict_append_item(dict, newstrval("completed"),
                                 newintval(pool->completed));
                dict_append_item(dict, newstrval("failed"),
                                 newintval(pool->failed));
                long long queued = 0;
                if (pool->jobs) {
                    std::lock_guard<std::mutex> jobs_lock(
                        pool->jobs->mutex);
                    queued = (long long)pool->jobs->queue.size();
                }
                dict_append_item(dict, newstrval("queued"),
                                 newintval(queued));
                return stats;
            },
            pool);
    }
    if (std::strcmp(name, "size") == 0)
        return newintval((long long)pool->size);

    yyerror("thread.pool has no field %s", name);
    return nullptr;
}

struct value_t *future_get_field(void *data, const char *name) {
    future_state *raw = static_cast<future_state *>(data);
    if (!raw || !name) {
        yyerror("thread.future has no such field");
        return nullptr;
    }
    std::shared_ptr<future_state> future = raw->shared_from_this();

    if (std::strcmp(name, "wait") == 0) {
        return newnativeval(
            [future](const std::vector<struct value_t *> &args) {
                long long timeout_ms = 0;
                if (args.size() > 1) {
                    yyerror("future.wait expects 0 or 1 argument");
                    return (struct value_t *)nullptr;
                }
                if (args.size() == 1) {
                    if (args[0]->type != T_int ||
                        ((int_value_t *)args[0])->value < 0) {
                        yyerror("future.wait timeout must be a "
                                "non-negative integer");
                        return (struct value_t *)nullptr;
                    }
                    timeout_ms = ((int_value_t *)args[0])->value;
                }
                return future_wait(*future, timeout_ms);
            },
            future);
    }
    if (std::strcmp(name, "try_wait") == 0) {
        return newnativeval(
            [future](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("future.try_wait expects no arguments");
                    return (struct value_t *)nullptr;
                }
                return future_try_wait(*future);
            },
            future);
    }

    yyerror("thread.future has no field %s", name);
    return nullptr;
}

const struct native_type kPoolType = {
    "thread_pool",
    pool_describe,
    nullptr,
    pool_get_field,
    nullptr,
    nullptr,
};

const struct native_type kFutureType = {
    "thread_future",
    future_describe,
    nullptr,
    future_get_field,
    nullptr,
    nullptr,
};

struct value_t *thread_pool(const std::vector<struct value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 ||
        args[0]->type != T_int ||
        ((int_value_t *)args[0])->value <= 0 ||
        args[1]->type != T_string) {
        yyerror("std.thread.pool expects size, script and optional options");
        return nullptr;
    }
    long long size = ((int_value_t *)args[0])->value;
    std::shared_ptr<pool_state> pool = std::make_shared<pool_state>();
    pool->size = (size_t)size;
    pool->script = ((string_value_t *)args[1])->value;
    long long queue_capacity = 1024;

    if (args.size() == 3) {
        if (args[2]->type != T_dict) {
            yyerror("std.thread.pool options must be a dict");
            return nullptr;
        }
        struct value_t *entry = dict_get_handle(args[2], "entry");
        if (entry) {
            if (entry->type != T_string) {
                yyerror("std.thread.pool entry must be a string");
                return nullptr;
            }
            pool->entry = ((string_value_t *)entry)->value;
        }
        struct value_t *queue = dict_get_handle(args[2], "queue");
        if (queue) {
            if (queue->type == T_int &&
                ((int_value_t *)queue)->value > 0) {
                queue_capacity = ((int_value_t *)queue)->value;
            } else if (queue->type == T_string &&
                       std::strcmp(((string_value_t *)queue)->value,
                                   "unbounded") == 0) {
                queue_capacity = -1;
            } else {
                yyerror("std.thread.pool queue must be positive or "
                        "\"unbounded\"");
                return nullptr;
            }
        }
    }

    pool->jobs = std::make_shared<channel_state>(queue_capacity);
    try {
        for (long long i = 0; i < size; ++i)
            pool->threads.emplace_back(worker_thread_main, pool.get());
    } catch (const std::exception &) {
        channel_close_state(pool->jobs);
        for (std::thread &thread : pool->threads) {
            if (thread.joinable())
                thread.join();
        }
        yyerror("std.thread.pool could not start workers");
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(pool->mutex);
    pool->cv.wait(lock, [&pool] {
        return pool->ready == pool->size || !pool->startup_error.empty();
    });
    if (!pool->startup_error.empty()) {
        std::string error = pool->startup_error;
        lock.unlock();
        channel_close_state(pool->jobs);
        for (std::thread &thread : pool->threads) {
            if (thread.joinable())
                thread.join();
        }
        yyerror("std.thread.pool: %s", error.c_str());
        return nullptr;
    }
    lock.unlock();
    struct value_t *value = new_pool_value(std::move(pool));
    return value;
}


struct value_t *thread_message_from(const std::vector<struct value_t *> &args) {
    if (args.empty() || args.size() > 2) {
        yyerror("std.thread.Message.from expects payload and optional handles");
        return nullptr;
    }
    std::unordered_set<const void *> visiting;
    std::string error;
    struct value_t *payload = copy_data_value(args[0], visiting, error);
    if (!payload) {
        yyerror("%s", error.c_str());
        return nullptr;
    }

    struct value_t *handles = (struct value_t *)newdictval();
    if (args.size() == 2) {
        if (args[1]->type != T_dict) {
            freeval(payload);
            freeval(handles);
            yyerror("std.thread.Message.from handles must be a dict");
            return nullptr;
        }
        struct dict_value_t *source = (struct dict_value_t *)args[1];
        for (const auto &entry : source->payload->items) {
            if (entry.first->type != T_string ||
                !is_handle_value(entry.second)) {
                freeval(payload);
                freeval(handles);
                yyerror("Message handles must map strings to thread handles");
                return nullptr;
            }
            dict_append_item((struct dict_value_t *)handles,
                             dupval(entry.first), dupval(entry.second));
        }
    }

    std::shared_ptr<message_state> state = std::make_shared<message_state>();
    state->payload = payload;
    state->handles = handles;
    return new_message_value(std::move(state));
}

struct value_t *thread_channel(long long capacity) {
    if (capacity == 0 || capacity < -1) {
        yyerror("std.thread.Channel capacity must be >= 1 or -1");
        return nullptr;
    }
    return new_channel_value(std::make_shared<channel_state>(capacity));
}

}  // namespace

struct value_t *call_thread_channel_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_thread_message_from:
            return thread_message_from(args);
        case B_thread_channel: {
            if (args.size() != 1 || args[0]->type != T_int) {
                yyerror("std.thread.Channel expects a capacity integer");
                return nullptr;
            }
            return thread_channel(((int_value_t *)args[0])->value);
        }
        case B_thread_unbounded_channel:
            if (!args.empty()) {
                yyerror("std.thread.unbounded_channel expects no arguments");
                return nullptr;
            }
            return thread_channel(-1);
        case B_thread_pool:
            return thread_pool(args);
        default:
            yyerror("unknown thread function");
            return nullptr;
    }
}
