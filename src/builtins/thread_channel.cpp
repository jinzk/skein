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

#include <algorithm>
#include <atomic>

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

struct channel_waiter {
    std::condition_variable cv;
    std::atomic<bool> ready{false};
};

struct channel_state : std::enable_shared_from_this<channel_state> {
    explicit channel_state(long long cap) : capacity(cap) {}

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<struct value_t *> queue;
    std::vector<channel_waiter *> recv_waiters;
    std::vector<channel_waiter *> send_waiters;
    long long capacity = 0; /* -1 means unbounded */
    bool closed = false;

    ~channel_state() {
        for (struct value_t *value : queue)
            freeval(value);
        queue.clear();
    }
};

void notify_recv_waiters(channel_state &state) {
    for (channel_waiter *waiter : state.recv_waiters) {
        waiter->ready.store(true, std::memory_order_release);
        waiter->cv.notify_all();
    }
}

void notify_send_waiters(channel_state &state) {
    for (channel_waiter *waiter : state.send_waiters) {
        waiter->ready.store(true, std::memory_order_release);
        waiter->cv.notify_all();
    }
}

void remove_waiter(std::vector<channel_waiter *> &waiters,
                   channel_waiter *waiter) {
    waiters.erase(
        std::remove(waiters.begin(), waiters.end(), waiter),
        waiters.end());
}

void channel_fiber_sleep(long long ms) {
    if (ms <= 0)
        ms = 1;
    struct value_t *value = newintval(ms);
    struct value_t *sleep_value =
        call_task_builtin(B_task_sleep, {value});
    freeval(value);
    if (!sleep_value)
        return;
    task_ptr task;
    if (get_task_value(sleep_value, task)) {
        struct value_t *result = fiber_await_task(task);
        freeval(result);
    }
    freeval(sleep_value);
}

extern const struct native_type kMessageType;
extern const struct native_type kChannelType;

bool get_channel_state(struct value_t *value,
                       std::shared_ptr<channel_state> &state);

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

bool validate_data_value(struct value_t *value,
                         std::unordered_set<const void *> &visiting,
                         std::string &error) {
    if (!value) {
        error = "Message payload is null";
        return false;
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
        case T_error:
            return true;
        case T_result: {
            struct result_value_t *result = (struct result_value_t *)value;
            return validate_data_value(
                result->ok ? result->value : result->error, visiting, error);
        }
        case T_list: {
            const void *payload = value_payload(value);
            if (payload && !visiting.insert(payload).second) {
                error = "Message payload contains a cycle";
                return false;
            }
            struct list_value_t *list = (struct list_value_t *)value;
            for (struct value_t *item : list->payload->items) {
                if (!validate_data_value(item, visiting, error)) {
                    if (payload)
                        visiting.erase(payload);
                    return false;
                }
            }
            if (payload)
                visiting.erase(payload);
            return true;
        }
        case T_dict: {
            const void *payload = value_payload(value);
            if (payload && !visiting.insert(payload).second) {
                error = "Message payload contains a cycle";
                return false;
            }
            struct dict_value_t *dict = (struct dict_value_t *)value;
            for (const auto &entry : dict->payload->items) {
                if (!validate_data_value(entry.first, visiting, error) ||
                    !validate_data_value(entry.second, visiting, error)) {
                    if (payload)
                        visiting.erase(payload);
                    return false;
                }
            }
            if (payload)
                visiting.erase(payload);
            return true;
        }
        default:
            error = std::string("Message payload does not support ") +
                    typename_of(value);
            return false;
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
    if (std::strcmp(name, "payload") == 0)
        return dupval(state->payload);
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
    bool blocking, long long timeout_ms = 0) {
    std::shared_ptr<message_state> message_data;
    if (!get_message_state(message, message_data)) {
        yyerror("std.thread.Channel.send expects a Message");
        return nullptr;
    }

    if (blocking && fiber_in_fiber()) {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->closed) {
                    yyerror("thread.closed: channel is closed");
                    return nullptr;
                }
                if (state->capacity < 0 ||
                    state->queue.size() < (size_t)state->capacity) {
                    state->queue.push_back(dupval(message));
                    notify_recv_waiters(*state);
                    state->cv.notify_all();
                    return newnull();
                }
            }
            if (g_interrupted)
                return nullptr;
            if (timeout_ms > 0) {
                const long long elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
                if (elapsed >= timeout_ms) {
                    yyerror("thread.timeout: channel send timed out");
                    return nullptr;
                }
            }
            channel_fiber_sleep(1);
        }
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    channel_waiter waiter;
    if (blocking) {
        /* NOTE: this blocks the current OS thread.  The fiber-aware
         * transport will replace this with a task wait. */
        state->send_waiters.push_back(&waiter);
        auto ready = [&state] {
            return state->closed || state->capacity < 0 ||
                   state->queue.size() < (size_t)state->capacity;
        };
        bool signaled = true;
        if (timeout_ms > 0) {
            signaled = waiter.cv.wait_for(
                lock, std::chrono::milliseconds(timeout_ms), ready);
        } else {
            waiter.cv.wait(lock, ready);
        }
        remove_waiter(state->send_waiters, &waiter);
        if (!signaled && !ready()) {
            yyerror("thread.timeout: channel send timed out");
            return nullptr;
        }
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
    notify_recv_waiters(*state);
    state->cv.notify_all();
    return blocking ? newnull() : newboolval(true);
}

enum class channel_recv_result {
    message,
    closed,
    timeout,
};

struct value_t *channel_recv_value_status(
    const std::shared_ptr<channel_state> &state, long long timeout_ms,
    channel_recv_result &result) {
    result = channel_recv_result::closed;
    if (fiber_in_fiber()) {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->queue.empty()) {
                    result = channel_recv_result::message;
                    struct value_t *message = state->queue.front();
                    state->queue.pop_front();
                    notify_send_waiters(*state);
                    return message;
                }
                if (state->closed)
                    return newnull();
            }
            if (g_interrupted)
                return nullptr;
            long long sleep_ms = 1;
            if (timeout_ms > 0) {
                const long long elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
                const long long remaining = timeout_ms - elapsed;
                if (remaining <= 0) {
                    result = channel_recv_result::timeout;
                    return nullptr;
                }
                sleep_ms = remaining < 1 ? remaining : 1;
            }
            channel_fiber_sleep(sleep_ms);
        }
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->queue.empty() && !state->closed) {
        channel_waiter waiter;
        state->recv_waiters.push_back(&waiter);
        auto ready = [&state] {
            return state->closed || !state->queue.empty();
        };
        bool signaled = true;
        if (timeout_ms > 0) {
            signaled = waiter.cv.wait_for(
                lock, std::chrono::milliseconds(timeout_ms), ready);
        } else {
            waiter.cv.wait(lock, ready);
        }
        remove_waiter(state->recv_waiters, &waiter);
        if (!signaled && state->queue.empty()) {
            result = channel_recv_result::timeout;
            return nullptr;
        }
    }
    if (state->queue.empty())
        return newnull();
    result = channel_recv_result::message;
    struct value_t *message = state->queue.front();
    state->queue.pop_front();
    notify_send_waiters(*state);
    return message;
}

struct value_t *channel_recv_value(
    const std::shared_ptr<channel_state> &state, long long timeout_ms) {
    channel_recv_result result = channel_recv_result::closed;
    struct value_t *message =
        channel_recv_value_status(state, timeout_ms, result);
    if (result == channel_recv_result::timeout) {
        yyerror("std.thread.Channel.recv timed out");
        return nullptr;
    }
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
        notify_send_waiters(*state);
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

bool collect_channels(struct value_t *value,
                      std::vector<std::shared_ptr<channel_state>> &channels,
                      std::string &error) {
    if (!value || value->type != T_list) {
        error = "expected a list of channels";
        return false;
    }
    struct list_value_t *list = (struct list_value_t *)value;
    for (struct value_t *item : list->payload->items) {
        std::shared_ptr<channel_state> channel;
        if (!get_channel_state(item, channel)) {
            error = "expected a list of channels";
            return false;
        }
        channels.push_back(std::move(channel));
    }
    if (channels.empty()) {
        error = "channel list must not be empty";
        return false;
    }
    return true;
}

void unregister_select_waiter(
    const std::vector<std::shared_ptr<channel_state>> &channels,
    channel_waiter *waiter) {
    for (const auto &channel : channels) {
        std::lock_guard<std::mutex> lock(channel->mutex);
        remove_waiter(channel->recv_waiters, waiter);
    }
}

struct value_t *select_value_result(size_t index, struct value_t *message) {
    struct value_t *result = (struct value_t *)newdictval();
    dict_append_item((struct dict_value_t *)result, newstrval("index"),
                     newintval((long long)index));
    dict_append_item((struct dict_value_t *)result, newstrval("value"),
                     message);
    return result;
}

struct value_t *select_closed_result(size_t index) {
    struct value_t *result = (struct value_t *)newdictval();
    dict_append_item((struct dict_value_t *)result, newstrval("index"),
                     newintval((long long)index));
    dict_append_item((struct dict_value_t *)result, newstrval("closed"),
                     newboolval(true));
    return result;
}

struct value_t *select_timeout_result() {
    struct value_t *result = (struct value_t *)newdictval();
    dict_append_item((struct dict_value_t *)result, newstrval("index"),
                     newintval(-1));
    dict_append_item((struct dict_value_t *)result, newstrval("timeout"),
                     newboolval(true));
    return result;
}

/* Poll the channels without registering a waiter. */
struct value_t *select_poll(
    const std::vector<std::shared_ptr<channel_state>> &channels,
    size_t start) {
    const size_t count = channels.size();
    for (size_t k = 0; k < count; ++k) {
        size_t i = (start + k) % count;
        struct value_t *message = nullptr;
        bool closed = false;
        {
            std::lock_guard<std::mutex> lock(channels[i]->mutex);
            if (!channels[i]->queue.empty()) {
                message = channels[i]->queue.front();
                channels[i]->queue.pop_front();
                notify_send_waiters(*channels[i]);
            } else if (channels[i]->closed) {
                closed = true;
            }
        }
        if (message)
            return select_value_result(i, message);
        if (closed)
            return select_closed_result(i);
    }
    return nullptr;
}

struct value_t *channel_select(
    const std::vector<std::shared_ptr<channel_state>> &channels,
    long long timeout_ms) {
    static thread_local size_t cursor = 0;
    const size_t count = channels.size();
    const size_t start = cursor++ % count;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              timeout_ms > 0 ? timeout_ms : 0);

    if (fiber_in_fiber()) {
        for (;;) {
            if (struct value_t *ready = select_poll(channels, start))
                return ready;
            if (timeout_ms > 0 &&
                std::chrono::steady_clock::now() >= deadline)
                return select_timeout_result();
            if (g_interrupted)
                return nullptr;
            channel_fiber_sleep(1);
        }
    }

    for (;;) {
        if (struct value_t *ready = select_poll(channels, start))
            return ready;
        if (timeout_ms > 0 &&
            std::chrono::steady_clock::now() >= deadline)
            return select_timeout_result();

        channel_waiter waiter;
        for (const auto &channel : channels) {
            std::lock_guard<std::mutex> lock(channel->mutex);
            channel->recv_waiters.push_back(&waiter);
        }

        if (struct value_t *ready = select_poll(channels, start)) {
            unregister_select_waiter(channels, &waiter);
            return ready;
        }

        std::mutex wait_mutex;
        std::unique_lock<std::mutex> wait_lock(wait_mutex);
        if (timeout_ms > 0) {
            waiter.cv.wait_until(wait_lock, deadline, [&waiter] {
                return waiter.ready.load(std::memory_order_acquire);
            });
        } else {
            waiter.cv.wait(wait_lock, [&waiter] {
                return waiter.ready.load(std::memory_order_acquire);
            });
        }
        wait_lock.unlock();
        unregister_select_waiter(channels, &waiter);
    }
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

enum class push_result {
    sent,
    full,
    closed,
};

push_result channel_push_impl(const std::shared_ptr<channel_state> &state,
                              struct value_t *message) {
    if (!state || !message)
        return push_result::closed;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed)
        return push_result::closed;
    if (state->capacity >= 0 &&
        state->queue.size() >= (size_t)state->capacity)
        return push_result::full;
    state->queue.push_back(dupval(message));
    notify_recv_waiters(*state);
    state->cv.notify_all();
    return push_result::sent;
}

bool channel_try_push(const std::shared_ptr<channel_state> &state,
                      struct value_t *message) {
    return channel_push_impl(state, message) == push_result::sent;
}

void channel_close_state(const std::shared_ptr<channel_state> &state) {
    if (!state)
        return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->closed = true;
        notify_recv_waiters(*state);
        notify_send_waiters(*state);
    }
    state->cv.notify_all();
}

struct value_t *channel_try_broadcast(
    const std::vector<std::shared_ptr<channel_state>> &channels,
    struct value_t *message) {
    std::shared_ptr<message_state> message_data;
    if (!get_message_state(message, message_data)) {
        yyerror("std.thread.try_broadcast expects a Message");
        return nullptr;
    }
    struct value_t *result = (struct value_t *)newdictval();
    struct dict_value_t *dict = (struct dict_value_t *)result;
    struct value_t *sent = (struct value_t *)newlistval();
    struct value_t *full = (struct value_t *)newlistval();
    struct value_t *closed = (struct value_t *)newlistval();
    for (size_t i = 0; i < channels.size(); ++i) {
        push_result outcome = channel_push_impl(channels[i], message);
        struct value_t *index = newintval((long long)i);
        if (outcome == push_result::sent)
            ((struct list_value_t *)sent)->payload->items.push_back(index);
        else if (outcome == push_result::full)
            ((struct list_value_t *)full)->payload->items.push_back(index);
        else
            ((struct list_value_t *)closed)->payload->items.push_back(index);
    }
    dict_append_item(dict, newstrval("sent"), sent);
    dict_append_item(dict, newstrval("full"), full);
    dict_append_item(dict, newstrval("closed"), closed);
    return result;
}

struct value_t *channel_broadcast(
    const std::vector<std::shared_ptr<channel_state>> &channels,
    struct value_t *message) {
    for (const auto &channel : channels) {
        struct value_t *result =
            channel_send_value(channel, message, true);
        if (!result)
            return nullptr;
        freeval(result);
    }
    return newnull();
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

enum class worker_status {
    starting,
    alive,
    dead,
};

struct pool_worker_slot {
    worker_status status = worker_status::starting;
    long long restarts = 0;
};

struct pool_state : std::enable_shared_from_this<pool_state> {
    std::shared_ptr<channel_state> jobs;
    /* Every worker generation, joined when the pool shuts down. */
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<pool_worker_slot>> workers;
    std::mutex mutex;
    std::mutex threads_mutex;
    std::condition_variable cv;
    size_t size = 0;
    size_t ready = 0;
    std::string script;
    std::string entry = "handle";
    std::string startup_error;
    bool closing = false;
    bool joined = false;
    bool worker_vm = false;
    long long restart_limit = 0;
    long long submitted = 0;
    long long completed = 0;
    long long failed = 0;
    long long active = 0;

    bool all_workers_dead_locked() const {
        if (workers.empty())
            return true;
        for (const auto &worker : workers) {
            if (worker->status != worker_status::dead)
                return false;
        }
        return true;
    }

    void join_workers() {
        for (;;) {
            std::vector<std::thread> pending;
            {
                std::lock_guard<std::mutex> lock(threads_mutex);
                pending.swap(threads);
            }
            if (pending.empty())
                break;
            for (std::thread &thread : pending) {
                if (thread.joinable())
                    thread.join();
            }
            std::lock_guard<std::mutex> lock(threads_mutex);
            if (threads.empty())
                break;
        }
    }

    ~pool_state() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
        }
        if (jobs)
            channel_close_state(jobs);
        join_workers();
    }
};

struct future_state : std::enable_shared_from_this<future_state> {
    std::shared_ptr<channel_state> reply;
    std::atomic<bool> canceled{false};
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

void send_worker_crash(const std::shared_ptr<channel_state> &reply,
                       const std::string &reason) {
    if (!reply)
        return;
    struct value_t *envelope = (struct value_t *)newdictval();
    dict_append_item((struct dict_value_t *)envelope, newstrval("ok"),
                     newboolval(false));
    dict_append_item((struct dict_value_t *)envelope, newstrval("error"),
                     newerrorval("thread.worker_crashed", reason));
    struct value_t *message =
        make_message_owned(envelope, (struct value_t *)newdictval());
    channel_try_push(reply, message);
    freeval(message);
}

void spawn_worker_thread(pool_state *pool, size_t slot_index);

void worker_thread_main(pool_state *pool, size_t slot_index) {
    struct skein_context hooks = {
        pool_worker_call,
        pool_worker_call_builtin,
        runtime_error_report,
        nullptr,
    };
    skein_context_set(&hooks);
    skein_vm_set_enabled(pool->worker_vm);
    g_interactive = false;
    g_show_prompt = false;

    eval_context context;
    set_active_eval_context(&context);
    initscope();
    eval_begin_root_defers();

    struct value_t *handler = nullptr;
    std::string error;
    bool loaded = false;
    bool crashed = false;
    std::string crash_reason;
    try {
        loaded = load_worker_script(*pool, handler, error);
    } catch (const std::exception &e) {
        error = e.what();
        crash_reason = error;
        crashed = true;
    } catch (...) {
        error = "worker startup failed";
        crash_reason = error;
        crashed = true;
    }

    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool_worker_slot &slot = *pool->workers[slot_index];
        if (loaded) {
            slot.status = worker_status::alive;
        } else {
            slot.status = worker_status::dead;
            if (!crashed)
                pool->startup_error = error;
        }
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

            {
                std::lock_guard<std::mutex> lock(pool->mutex);
                ++pool->active;
            }
            bool job_crashed = false;
            try {
                eval_capture_snapshot snapshot;
                eval_capture_begin(snapshot);
                struct value_t *result =
                    skein_call_function(handler, {payload});
                payload = nullptr;
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
                    make_message_owned(envelope,
                                       (struct value_t *)newdictval());
                channel_try_push(reply, reply_message);
                freeval(reply_message);

                {
                    std::lock_guard<std::mutex> lock(pool->mutex);
                    if (ok)
                        ++pool->completed;
                    else
                        ++pool->failed;
                }
                freeval(captured.error);
            } catch (const std::exception &e) {
                job_crashed = true;
                crash_reason = e.what();
                send_worker_crash(reply, crash_reason);
                std::lock_guard<std::mutex> lock(pool->mutex);
                ++pool->failed;
            } catch (...) {
                job_crashed = true;
                crash_reason = "unknown worker exception";
                send_worker_crash(reply, crash_reason);
                std::lock_guard<std::mutex> lock(pool->mutex);
                ++pool->failed;
            }
            {
                std::lock_guard<std::mutex> lock(pool->mutex);
                if (pool->active > 0)
                    --pool->active;
            }
            freeval(job);
            if (job_crashed) {
                crashed = true;
                break;
            }
        }
    }

    bool restart = false;
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool_worker_slot &slot = *pool->workers[slot_index];
        slot.status = worker_status::dead;
        if (crashed && !pool->closing &&
            pool->restart_limit > slot.restarts) {
            ++slot.restarts;
            slot.status = worker_status::starting;
            restart = true;
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

    if (restart) {
        try {
            spawn_worker_thread(pool, slot_index);
        } catch (const std::exception &) {
            std::lock_guard<std::mutex> lock(pool->mutex);
            pool->workers[slot_index]->status = worker_status::dead;
        }
    }
}

void spawn_worker_thread(pool_state *pool, size_t slot_index) {
    std::thread thread(worker_thread_main, pool, slot_index);
    std::lock_guard<std::mutex> lock(pool->threads_mutex);
    pool->threads.push_back(std::move(thread));
}

bool pool_submit_raw(pool_state &pool, struct value_t *value,
                     long long timeout_ms,
                     std::shared_ptr<future_state> &future,
                     std::string &error) {
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (pool.closing) {
            error = "thread.closed: pool is closed";
            return false;
        }
        if (pool.all_workers_dead_locked()) {
            error = "thread.pool_failed: all workers have exited";
            return false;
        }
    }

    std::shared_ptr<message_state> source;
    struct value_t *payload = nullptr;
    struct value_t *handles = (struct value_t *)newdictval();
    std::unordered_set<const void *> visiting;

    if (get_message_state(value, source)) {
        payload = copy_data_value(source->payload, visiting, error);
        freeval(handles);
        handles = copy_handles_value(source->handles);
    } else {
        payload = copy_data_value(value, visiting, error);
    }
    if (!payload) {
        freeval(handles);
        return false;
    }

    std::shared_ptr<channel_state> reply =
        std::make_shared<channel_state>(1);
    struct value_t *reply_value = new_channel_value(reply);
    dict_set_owned(handles, "reply", reply_value);

    struct value_t *message = make_message_owned(payload, handles);
    struct value_t *sent =
        channel_send_value(pool.jobs, message, true, timeout_ms);
    freeval(message);
    if (!sent) {
        if (error.empty())
            error = "thread.timeout: pool submit timed out";
        return false;
    }

    future = std::make_shared<future_state>();
    future->reply = reply;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        ++pool.submitted;
    }
    return true;
}

struct value_t *pool_submit(pool_state &pool, struct value_t *value,
                            long long timeout_ms) {
    std::shared_ptr<future_state> future;
    std::string error;
    if (!pool_submit_raw(pool, value, timeout_ms, future, error)) {
        yyerror("%s", error.c_str());
        return nullptr;
    }
    return new_future_value(std::move(future));
}

thread_future_status future_wait_raw(future_state &future,
                                     long long timeout_ms,
                                     struct value_t *&payload,
                                     std::string &error) {
    payload = nullptr;
    if (future.canceled.load())
        return thread_future_status::canceled;

    channel_recv_result result = channel_recv_result::closed;
    struct value_t *message =
        channel_recv_value_status(future.reply, timeout_ms, result);
    if (future.canceled.load()) {
        freeval(message);
        return thread_future_status::canceled;
    }
    if (result == channel_recv_result::timeout) {
        freeval(message);
        return thread_future_status::pending;
    }
    if (!message || message->type == T_null) {
        freeval(message);
        return thread_future_status::closed;
    }
    std::shared_ptr<message_state> state;
    if (!get_message_state(message, state)) {
        freeval(message);
        error = "invalid future reply";
        return thread_future_status::closed;
    }
    payload = dupval(state->payload);
    freeval(message);
    return thread_future_status::ready;
}

struct value_t *future_wait(struct future_state &future,
                            long long timeout_ms) {
    struct value_t *payload = nullptr;
    std::string error;
    thread_future_status status =
        future_wait_raw(future, timeout_ms, payload, error);
    if (status == thread_future_status::canceled) {
        freeval(payload);
        yyerror("thread.cancelled: future was canceled");
        return nullptr;
    }
    if (status == thread_future_status::pending) {
        yyerror("thread.timeout: future wait timed out");
        return nullptr;
    }
    if (status == thread_future_status::closed) {
        freeval(payload);
        if (!error.empty())
            yyerror("%s", error.c_str());
        return nullptr;
    }
    return payload;
}

struct value_t *future_try_wait(struct future_state &future) {
    if (future.canceled.load()) {
        struct value_t *result = (struct value_t *)newdictval();
        dict_append_item((struct dict_value_t *)result, newstrval("ok"),
                         newboolval(false));
        dict_append_item((struct dict_value_t *)result, newstrval("closed"),
                         newboolval(true));
        dict_append_item((struct dict_value_t *)result, newstrval("canceled"),
                         newboolval(true));
        return result;
    }
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
    struct value_t *payload = dupval(state->payload);
    freeval(dict->payload->items[(size_t)value_index].second);
    dict->payload->items[(size_t)value_index].second = payload;
    return raw;
}

std::string pool_describe(const void *) {
    return "<thread_pool>";
}

struct value_t *pool_stats_value(pool_state &pool) {
    struct value_t *stats = (struct value_t *)newdictval();
    struct dict_value_t *dict = (struct dict_value_t *)stats;
    std::lock_guard<std::mutex> lock(pool.mutex);
    long long alive = 0;
    long long dead = 0;
    long long restarts = 0;
    for (const auto &worker : pool.workers) {
        if (worker->status == worker_status::alive)
            ++alive;
        else if (worker->status == worker_status::dead)
            ++dead;
        restarts += worker->restarts;
    }
    dict_append_item(dict, newstrval("size"),
                     newintval((long long)pool.size));
    dict_append_item(dict, newstrval("workers"),
                     newintval((long long)pool.size));
    dict_append_item(dict, newstrval("ready"),
                     newintval((long long)pool.ready));
    dict_append_item(dict, newstrval("alive"), newintval(alive));
    dict_append_item(dict, newstrval("dead"), newintval(dead));
    dict_append_item(dict, newstrval("restarts"), newintval(restarts));
    dict_append_item(dict, newstrval("restart_limit"),
                     newintval(pool.restart_limit));
    dict_append_item(dict, newstrval("submitted"),
                     newintval(pool.submitted));
    dict_append_item(dict, newstrval("completed"),
                     newintval(pool.completed));
    dict_append_item(dict, newstrval("failed"),
                     newintval(pool.failed));
    dict_append_item(dict, newstrval("active"),
                     newintval(pool.active));
    long long queued = 0;
    if (pool.jobs) {
        std::lock_guard<std::mutex> jobs_lock(pool.jobs->mutex);
        queued = (long long)pool.jobs->queue.size();
    }
    dict_append_item(dict, newstrval("queued"), newintval(queued));
    return stats;
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
                if (args.empty() || args.size() > 2) {
                    yyerror("pool.submit expects value and optional timeout");
                    return (struct value_t *)nullptr;
                }
                long long timeout_ms = 0;
                if (args.size() == 2) {
                    if (args[1]->type != T_int ||
                        ((int_value_t *)args[1])->value < 0) {
                        yyerror("pool.submit timeout must be a "
                                "non-negative integer");
                        return (struct value_t *)nullptr;
                    }
                    timeout_ms = ((int_value_t *)args[1])->value;
                }
                return pool_submit(*pool, args[0], timeout_ms);
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
                    pool->closing = true;
                }
                channel_close_state(pool->jobs);
                pool->join_workers();
                return newnull();
            },
            pool);
    }
    if (std::strcmp(name, "stats") == 0) {
        return newnativeval(
            [pool](const std::vector<struct value_t *> &) {
                return pool_stats_value(*pool);
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
    if (std::strcmp(name, "cancel") == 0) {
        return newnativeval(
            [future](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("future.cancel expects no arguments");
                    return (struct value_t *)nullptr;
                }
                bool first = !future->canceled.exchange(true);
                if (first)
                    channel_close_state(future->reply);
                return newboolval(first);
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

std::shared_ptr<pool_state> create_pool_state(
    std::size_t size, const std::string &script, const std::string &entry,
    long long queue_capacity, long long restart_limit, bool worker_vm,
    std::string &error) {
    std::shared_ptr<pool_state> pool = std::make_shared<pool_state>();
    pool->size = size;
    pool->script = script;
    pool->entry = entry;
    pool->restart_limit = restart_limit;
    pool->worker_vm = worker_vm;
    pool->jobs = std::make_shared<channel_state>(queue_capacity);
    pool->workers.reserve(size);
    for (std::size_t i = 0; i < size; ++i)
        pool->workers.push_back(std::make_unique<pool_worker_slot>());

    auto fail = [&pool, &error](const std::string &message) {
        error = message;
        {
            std::lock_guard<std::mutex> guard(pool->mutex);
            pool->closing = true;
        }
        channel_close_state(pool->jobs);
        pool->join_workers();
    };

    try {
        for (std::size_t i = 0; i < pool->workers.size(); ++i)
            spawn_worker_thread(pool.get(), i);
    } catch (const std::exception &) {
        fail("could not start workers");
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(pool->mutex);
    pool->cv.wait(lock, [&pool] {
        return pool->ready >= pool->size || !pool->startup_error.empty();
    });
    if (!pool->startup_error.empty()) {
        std::string startup_error = pool->startup_error;
        lock.unlock();
        fail(startup_error);
        return nullptr;
    }
    if (pool->all_workers_dead_locked()) {
        lock.unlock();
        fail("all workers failed to start");
        return nullptr;
    }
    lock.unlock();
    return pool;
}

struct value_t *thread_pool(const std::vector<struct value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 ||
        args[0]->type != T_int ||
        ((int_value_t *)args[0])->value <= 0 ||
        args[1]->type != T_string) {
        yyerror("std.thread.pool expects size, script and optional options");
        return nullptr;
    }
    long long size = ((int_value_t *)args[0])->value;
    std::string script = ((string_value_t *)args[1])->value;
    std::string entry = "handle";
    long long queue_capacity = 1024;
    long long restart_limit = 0;
    bool worker_vm = false;

    if (args.size() == 3) {
        if (args[2]->type != T_dict) {
            yyerror("std.thread.pool options must be a dict");
            return nullptr;
        }
        struct value_t *entry_value = dict_get_handle(args[2], "entry");
        if (entry_value) {
            if (entry_value->type != T_string) {
                yyerror("std.thread.pool entry must be a string");
                return nullptr;
            }
            entry = ((string_value_t *)entry_value)->value;
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
        struct value_t *restart = dict_get_handle(args[2], "restart_limit");
        if (restart) {
            if (restart->type != T_int ||
                ((int_value_t *)restart)->value < 0) {
                yyerror("std.thread.pool restart_limit must be a "
                        "non-negative integer");
                return nullptr;
            }
            restart_limit = ((int_value_t *)restart)->value;
        }
        struct value_t *vm = dict_get_handle(args[2], "worker_vm");
        if (vm) {
            if (vm->type != T_bool) {
                yyerror("std.thread.pool worker_vm must be a bool");
                return nullptr;
            }
            worker_vm = ((bool_value_t *)vm)->value;
        }
    }

    std::string error;
    std::shared_ptr<pool_state> pool =
        create_pool_state((std::size_t)size, script, entry, queue_capacity,
                          restart_limit, worker_vm, error);
    if (!pool) {
        yyerror("std.thread.pool: %s", error.c_str());
        return nullptr;
    }
    return new_pool_value(std::move(pool));
}

/* ------------------------------------------------------------------ */
/* Single thread                                                       */
/* ------------------------------------------------------------------ */

struct thread_state : std::enable_shared_from_this<thread_state> {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool startup_done = false;
    std::string startup_error;
    bool done = false;
    bool detached = false;
    bool joined = false;
    bool canceled = false;
    long long id = 0;
    struct value_t *result = nullptr;

    ~thread_state() {
        if (thread.joinable())
            thread.detach();
        freeval(result);
        result = nullptr;
    }
};

extern const struct native_type kThreadType;

std::atomic<long long> next_thread_id{1};

struct value_t *new_thread_value(std::shared_ptr<thread_state> state) {
    return newuserdataval(&kThreadType, std::move(state));
}

struct value_t *thread_ok_envelope(struct value_t *value) {
    struct value_t *envelope = (struct value_t *)newdictval();
    struct dict_value_t *dict = (struct dict_value_t *)envelope;
    dict_append_item(dict, newstrval("ok"), newboolval(true));
    dict_append_item(dict, newstrval("value"), value ? value : newnull());
    return envelope;
}

struct value_t *thread_error_envelope(struct value_t *error) {
    struct value_t *envelope = (struct value_t *)newdictval();
    struct dict_value_t *dict = (struct dict_value_t *)envelope;
    dict_append_item(dict, newstrval("ok"), newboolval(false));
    dict_append_item(dict, newstrval("error"),
                     error ? error : newerrorval("thread", "thread failed"));
    return envelope;
}

void thread_entry_main(std::shared_ptr<thread_state> state,
                       const std::string &script, const std::string &entry,
                       struct value_t *argument) {
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

    package_configure(script, {});
    std::string error;
    struct value_t *handler = nullptr;
    bool loaded = parse_worker_source(script, error);
    if (loaded) {
        handler = lookup_global_symbol(entry.c_str());
        if (!callable_value(handler)) {
            loaded = false;
            error = "thread entry function not found: " + entry;
        }
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->startup_done = true;
        if (!loaded)
            state->startup_error = error;
    }
    state->cv.notify_all();

    if (!loaded) {
        freeval(argument);
        argument = nullptr;
    } else {
        eval_capture_snapshot snapshot;
        eval_capture_begin(snapshot);
        struct value_t *result =
            skein_call_function(handler, {argument});
        argument = nullptr;
        if (result && is_task_value(result)) {
            task_ptr task;
            if (get_task_value(result, task)) {
                freeval(result);
                result = fiber_await_task(task);
            }
        }
        eval_capture_result captured = eval_capture_end(snapshot);

        struct value_t *envelope = nullptr;
        if (captured.failed || !result) {
            struct value_t *err = captured.error;
            captured.error = nullptr;
            if (!err)
                err = newerrorval("thread", "thread function failed");
            std::unordered_set<const void *> visiting;
            std::string copy_error;
            struct value_t *safe_error =
                copy_data_value(err, visiting, copy_error);
            freeval(err);
            envelope = thread_error_envelope(
                safe_error ? safe_error : newerrorval("thread", copy_error));
        } else {
            std::unordered_set<const void *> visiting;
            std::string copy_error;
            struct value_t *safe_value =
                copy_data_value(result, visiting, copy_error);
            freeval(result);
            if (safe_value)
                envelope = thread_ok_envelope(safe_value);
            else
                envelope = thread_error_envelope(
                    newerrorval("thread", copy_error));
        }
        freeval(captured.error);

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result = envelope;
            state->done = true;
        }
        state->cv.notify_all();
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

struct value_t *thread_spawn(const std::vector<struct value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 ||
        args[0]->type != T_string || args[1]->type != T_userdata) {
        yyerror("std.thread.spawn expects script, Message and optional options");
        return nullptr;
    }
    std::shared_ptr<message_state> message;
    if (!get_message_state(args[1], message)) {
        yyerror("std.thread.spawn expects a Message");
        return nullptr;
    }
    std::string entry = "main";
    if (args.size() == 3) {
        if (args[2]->type != T_dict) {
            yyerror("std.thread.spawn options must be a dict");
            return nullptr;
        }
        struct value_t *entry_value = dict_get_handle(args[2], "entry");
        if (entry_value) {
            if (entry_value->type != T_string) {
                yyerror("std.thread.spawn entry must be a string");
                return nullptr;
            }
            entry = ((string_value_t *)entry_value)->value;
        }
    }

    std::shared_ptr<thread_state> state = std::make_shared<thread_state>();
    state->id = next_thread_id.fetch_add(1);
    struct value_t *argument = dupval(args[1]);
    try {
        state->thread = std::thread(thread_entry_main, state,
                                    ((string_value_t *)args[0])->value,
                                    entry, argument);
    } catch (const std::exception &) {
        freeval(argument);
        yyerror("std.thread.spawn could not start thread");
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&state] { return state->startup_done; });
    if (!state->startup_error.empty()) {
        std::string error = state->startup_error;
        lock.unlock();
        if (state->thread.joinable())
            state->thread.join();
        yyerror("std.thread.spawn: %s", error.c_str());
        return nullptr;
    }
    lock.unlock();
    return new_thread_value(std::move(state));
}

std::string thread_describe(const void *) {
    return "<thread>";
}

struct value_t *thread_get_field(void *data, const char *name) {
    thread_state *raw = static_cast<thread_state *>(data);
    if (!raw || !name) {
        yyerror("thread has no such field");
        return nullptr;
    }
    std::shared_ptr<thread_state> state = raw->shared_from_this();

    if (std::strcmp(name, "join") == 0) {
        return newnativeval(
            [state](const std::vector<struct value_t *> &args) {
                long long timeout_ms = 0;
                if (args.size() > 1) {
                    yyerror("thread.join expects 0 or 1 argument");
                    return (struct value_t *)nullptr;
                }
                if (args.size() == 1) {
                    if (args[0]->type != T_int ||
                        ((int_value_t *)args[0])->value < 0) {
                        yyerror("thread.join timeout must be a "
                                "non-negative integer");
                        return (struct value_t *)nullptr;
                    }
                    timeout_ms = ((int_value_t *)args[0])->value;
                }
                std::unique_lock<std::mutex> lock(state->mutex);
                if (!state->done) {
                    if (timeout_ms > 0) {
                        if (!state->cv.wait_for(
                                lock, std::chrono::milliseconds(timeout_ms),
                                [&state] { return state->done; })) {
                            yyerror("std.thread.join timed out");
                            return (struct value_t *)nullptr;
                        }
                    } else {
                        state->cv.wait(lock, [&state] { return state->done; });
                    }
                }
                if (!state->joined) {
                    state->joined = true;
                    lock.unlock();
                    if (state->thread.joinable())
                        state->thread.join();
                    lock.lock();
                }
                return state->result ? dupval(state->result) : newnull();
            },
            state);
    }
    if (std::strcmp(name, "detach") == 0) {
        return newnativeval(
            [state](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("thread.detach expects no arguments");
                    return (struct value_t *)nullptr;
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->detached) {
                    if (state->thread.joinable())
                        state->thread.detach();
                    state->detached = true;
                }
                return newnull();
            },
            state);
    }
    if (std::strcmp(name, "is_alive") == 0) {
        return newnativeval(
            [state](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("thread.is_alive expects no arguments");
                    return (struct value_t *)nullptr;
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                return newboolval(!state->done);
            },
            state);
    }
    if (std::strcmp(name, "cancel") == 0) {
        return newnativeval(
            [state](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("thread.cancel expects no arguments");
                    return (struct value_t *)nullptr;
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                state->canceled = true;
                return newboolval(true);
            },
            state);
    }
    if (std::strcmp(name, "id") == 0)
        return newintval(state->id);

    yyerror("thread has no field %s", name);
    return nullptr;
}

const struct native_type kThreadType = {
    "thread",
    thread_describe,
    nullptr,
    thread_get_field,
    nullptr,
    nullptr,
};



struct value_t *thread_message_from(const std::vector<struct value_t *> &args) {
    if (args.empty() || args.size() > 2) {
        yyerror("std.thread.Message.from expects payload and optional handles");
        return nullptr;
    }
    std::unordered_set<const void *> visiting;
    std::string error;
    if (!validate_data_value(args[0], visiting, error)) {
        yyerror("%s", error.c_str());
        return nullptr;
    }
    struct value_t *payload = dupval(args[0]);

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

struct thread_pool_link {
    std::shared_ptr<pool_state> pool;
};

struct thread_future_link {
    std::shared_ptr<future_state> future;
};

std::shared_ptr<thread_pool_link> thread_pool_link_create(
    std::size_t workers, const std::string &script, const std::string &entry,
    long long queue_capacity, long long restart_limit, bool worker_vm,
    std::string &error) {
    std::shared_ptr<pool_state> pool = create_pool_state(
        workers, script, entry, queue_capacity, restart_limit, worker_vm,
        error);
    if (!pool)
        return nullptr;
    std::shared_ptr<thread_pool_link> link =
        std::make_shared<thread_pool_link>();
    link->pool = std::move(pool);
    return link;
}

bool thread_pool_link_submit(
    const std::shared_ptr<thread_pool_link> &pool, struct value_t *payload,
    long long timeout_ms, std::shared_ptr<thread_future_link> &future,
    std::string &error) {
    if (!pool || !pool->pool) {
        error = "thread.pool_failed: pool is not available";
        return false;
    }
    std::shared_ptr<future_state> state;
    if (!pool_submit_raw(*pool->pool, payload, timeout_ms, state, error))
        return false;
    future = std::make_shared<thread_future_link>();
    future->future = std::move(state);
    return true;
}

thread_future_status thread_future_link_wait(
    const std::shared_ptr<thread_future_link> &future, long long timeout_ms,
    struct value_t *&payload, std::string &error) {
    payload = nullptr;
    if (!future || !future->future) {
        error = "thread.pool_failed: future is not available";
        return thread_future_status::closed;
    }
    return future_wait_raw(*future->future, timeout_ms, payload, error);
}

void thread_future_link_cancel(
    const std::shared_ptr<thread_future_link> &future) {
    if (!future || !future->future)
        return;
    bool first = !future->future->canceled.exchange(true);
    if (first)
        channel_close_state(future->future->reply);
}

void thread_pool_link_close(const std::shared_ptr<thread_pool_link> &pool) {
    if (!pool || !pool->pool)
        return;
    {
        std::lock_guard<std::mutex> lock(pool->pool->mutex);
        pool->pool->closing = true;
    }
    channel_close_state(pool->pool->jobs);
}

void thread_pool_link_join(const std::shared_ptr<thread_pool_link> &pool) {
    if (!pool || !pool->pool)
        return;
    {
        std::lock_guard<std::mutex> lock(pool->pool->mutex);
        pool->pool->closing = true;
        pool->pool->joined = true;
    }
    channel_close_state(pool->pool->jobs);
    pool->pool->join_workers();
}

struct value_t *thread_pool_link_stats(
    const std::shared_ptr<thread_pool_link> &pool) {
    if (!pool || !pool->pool) {
        yyerror("thread.pool_failed: pool is not available");
        return nullptr;
    }
    return pool_stats_value(*pool->pool);
}

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
        case B_thread_spawn:
            return thread_spawn(args);
        case B_thread_select: {
            if (args.empty() || args.size() > 2) {
                yyerror("std.thread.select expects endpoints and optional "
                        "timeout");
                return nullptr;
            }
            long long timeout_ms = 0;
            if (args.size() == 2) {
                if (args[1]->type != T_int ||
                    ((int_value_t *)args[1])->value < 0) {
                    yyerror("std.thread.select timeout must be a "
                            "non-negative integer");
                    return nullptr;
                }
                timeout_ms = ((int_value_t *)args[1])->value;
            }
            std::vector<std::shared_ptr<channel_state>> channels;
            std::string error;
            if (!collect_channels(args[0], channels, error)) {
                yyerror("std.thread.select: %s", error.c_str());
                return nullptr;
            }
            return channel_select(channels, timeout_ms);
        }
        case B_thread_try_broadcast:
        case B_thread_broadcast: {
            if (args.size() != 2) {
                yyerror("std.thread.broadcast expects endpoints and Message");
                return nullptr;
            }
            std::vector<std::shared_ptr<channel_state>> channels;
            std::string error;
            if (!collect_channels(args[0], channels, error)) {
                yyerror("std.thread.broadcast: %s", error.c_str());
                return nullptr;
            }
            if (functype == B_thread_try_broadcast)
                return channel_try_broadcast(channels, args[1]);
            return channel_broadcast(channels, args[1]);
        }
        default:
            yyerror("unknown thread function");
            return nullptr;
    }
}
