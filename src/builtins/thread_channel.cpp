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
#include "gc.h"
#include "package.h"
#include "parser_lock.h"
#include "platform.h"
#include "runtime_context.h"
#include "runtime_state.h"
#include "scopestack.h"
#include "std_namespace.h"
#include "structtype.h"
#include "symbol.h"
#include "task.h"
#include "vm.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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
    /* Shared cancellation flag for thread-pool jobs. */
    std::shared_ptr<std::atomic<bool>> canceled;

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
    std::vector<void *> recv_fiber_wakes;
    std::vector<void *> send_fiber_wakes;
    long long capacity = 0; /* -1 means unbounded, 0 means rendezvous */
    bool closed = false;
    long long total_sends = 0;
    long long total_recvs = 0;
    long long send_timeouts = 0;
    long long recv_timeouts = 0;

    ~channel_state() {
        for (struct value_t *value : queue)
            freeval(value);
        queue.clear();
    }
};

struct duplex_state {
    std::shared_ptr<channel_state> parent_to_child;
    std::shared_ptr<channel_state> child_to_parent;
};

struct duplex_endpoint_state
    : std::enable_shared_from_this<duplex_endpoint_state> {
    std::shared_ptr<duplex_state> duplex;
    bool parent = true;

    const std::shared_ptr<channel_state> &out() const {
        return parent ? duplex->parent_to_child : duplex->child_to_parent;
    }
    const std::shared_ptr<channel_state> &in() const {
        return parent ? duplex->child_to_parent : duplex->parent_to_child;
    }
};

std::mutex thread_cycle_registry_mutex;
std::vector<std::weak_ptr<message_state>> thread_message_registry;
std::vector<std::weak_ptr<channel_state>> thread_channel_registry;
std::vector<std::weak_ptr<duplex_state>> thread_duplex_registry;
std::vector<std::weak_ptr<duplex_endpoint_state>> thread_endpoint_registry;

std::mutex thread_runtime_gate;
long long active_thread_runtimes = 0;
std::atomic<long long> thread_gc_collected{0};

void thread_runtime_enter() {
    std::lock_guard<std::mutex> lock(thread_runtime_gate);
    ++active_thread_runtimes;
}

void thread_runtime_leave() {
    std::lock_guard<std::mutex> lock(thread_runtime_gate);
    --active_thread_runtimes;
}

void collect_thread_cycles_entry();

void ensure_thread_cycle_collector() {
    static std::once_flag once;
    std::call_once(once, [] {
        gc_set_thread_cycle_collector(&collect_thread_cycles_entry);
    });
}

template <typename T>
void prune_registry(std::vector<std::weak_ptr<T>> &registry) {
    if (registry.size() < 1024)
        return;
    size_t write = 0;
    for (size_t i = 0; i < registry.size(); ++i) {
        if (!registry[i].expired())
            registry[write++] = registry[i];
    }
    registry.resize(write);
}

std::shared_ptr<message_state> make_message_state() {
    ensure_thread_cycle_collector();
    std::shared_ptr<message_state> state =
        std::make_shared<message_state>();
    {
        std::lock_guard<std::mutex> lock(thread_cycle_registry_mutex);
        thread_message_registry.push_back(state);
        prune_registry(thread_message_registry);
    }
    return state;
}

std::shared_ptr<channel_state> make_channel_state(long long capacity) {
    ensure_thread_cycle_collector();
    std::shared_ptr<channel_state> state =
        std::make_shared<channel_state>(capacity);
    {
        std::lock_guard<std::mutex> lock(thread_cycle_registry_mutex);
        thread_channel_registry.push_back(state);
        prune_registry(thread_channel_registry);
    }
    return state;
}

std::shared_ptr<duplex_state> make_duplex_state(long long capacity) {
    ensure_thread_cycle_collector();
    std::shared_ptr<duplex_state> state =
        std::make_shared<duplex_state>();
    state->parent_to_child = make_channel_state(capacity);
    state->child_to_parent = make_channel_state(capacity);
    {
        std::lock_guard<std::mutex> lock(thread_cycle_registry_mutex);
        thread_duplex_registry.push_back(state);
        prune_registry(thread_duplex_registry);
    }
    return state;
}

std::shared_ptr<duplex_endpoint_state> make_duplex_endpoint_state() {
    ensure_thread_cycle_collector();
    std::shared_ptr<duplex_endpoint_state> state =
        std::make_shared<duplex_endpoint_state>();
    {
        std::lock_guard<std::mutex> lock(thread_cycle_registry_mutex);
        thread_endpoint_registry.push_back(state);
        prune_registry(thread_endpoint_registry);
    }
    return state;
}

struct value_t *thread_debug_counts(
    const std::vector<struct value_t *> &args) {
    if (!args.empty()) {
        yyerror("std.thread.debug_counts expects no arguments");
        return nullptr;
    }
    auto count_live = [](auto &registry) {
        long long live = 0;
        size_t write = 0;
        for (size_t i = 0; i < registry.size(); ++i) {
            if (registry[i].lock()) {
                ++live;
                registry[write++] = registry[i];
            }
        }
        registry.resize(write);
        return live;
    };
    long long messages = 0;
    long long channels = 0;
    long long duplexes = 0;
    long long endpoints = 0;
    {
        std::lock_guard<std::mutex> lock(thread_cycle_registry_mutex);
        messages = count_live(thread_message_registry);
        channels = count_live(thread_channel_registry);
        duplexes = count_live(thread_duplex_registry);
        endpoints = count_live(thread_endpoint_registry);
    }
    struct value_t *result = (struct value_t *)newdictval();
    dict_append_item((struct dict_value_t *)result, newstrval("messages"),
                     newintval(messages));
    dict_append_item((struct dict_value_t *)result, newstrval("channels"),
                     newintval(channels));
    dict_append_item((struct dict_value_t *)result, newstrval("duplexes"),
                     newintval(duplexes));
    dict_append_item((struct dict_value_t *)result, newstrval("endpoints"),
                     newintval(endpoints));
    dict_append_item((struct dict_value_t *)result, newstrval("collected"),
                     newintval(thread_gc_collected.load()));
    return result;
}

struct value_t *thread_collect(
    const std::vector<struct value_t *> &args) {
    if (!args.empty()) {
        yyerror("std.thread.collect expects no arguments");
        return nullptr;
    }
    collect_thread_cycles();
    return newnull();
}

void wake_fiber_handles(const std::vector<void *> &handles) {
    for (void *handle : handles)
        event_loop_wake(handle);
}

void add_fiber_handle(std::vector<void *> &handles, void *handle) {
    if (handle)
        handles.push_back(handle);
}

void remove_fiber_handle(std::vector<void *> &handles, void *handle) {
    if (!handle)
        return;
    auto it = std::find(handles.begin(), handles.end(), handle);
    if (it != handles.end())
        handles.erase(it);
}

void notify_recv_waiters(channel_state &state) {
    for (channel_waiter *waiter : state.recv_waiters) {
        waiter->ready.store(true, std::memory_order_release);
        waiter->cv.notify_all();
    }
    wake_fiber_handles(state.recv_fiber_wakes);
}

void notify_recv_waiter(channel_state &state) {
    if (!state.recv_waiters.empty()) {
        channel_waiter *waiter = state.recv_waiters.front();
        waiter->ready.store(true, std::memory_order_release);
        waiter->cv.notify_all();
        return;
    }
    if (!state.recv_fiber_wakes.empty())
        event_loop_wake(state.recv_fiber_wakes.front());
}

void notify_send_waiters(channel_state &state) {
    for (channel_waiter *waiter : state.send_waiters) {
        waiter->ready.store(true, std::memory_order_release);
        waiter->cv.notify_all();
    }
    wake_fiber_handles(state.send_fiber_wakes);
}

void notify_send_waiter(channel_state &state) {
    if (!state.send_waiters.empty()) {
        channel_waiter *waiter = state.send_waiters.front();
        waiter->ready.store(true, std::memory_order_release);
        waiter->cv.notify_all();
        return;
    }
    if (!state.send_fiber_wakes.empty())
        event_loop_wake(state.send_fiber_wakes.front());
}

bool channel_can_enqueue_locked(const channel_state &state) {
    if (state.capacity < 0)
        return true;
    if (state.capacity == 0)
        return state.queue.empty() && (!state.recv_waiters.empty() ||
                                       !state.recv_fiber_wakes.empty());
    return state.queue.size() < (size_t)state.capacity;
}

bool channel_has_receiver_locked(const channel_state &state) {
    return !state.recv_waiters.empty() || !state.recv_fiber_wakes.empty();
}

bool channel_can_enqueue_nonblocking_locked(const channel_state &state) {
    if (state.capacity < 0)
        return true;
    if (state.capacity == 0)
        return state.queue.empty() && channel_has_receiver_locked(state);
    return state.queue.size() < (size_t)state.capacity;
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
extern const struct native_type kDuplexEndpointType;

void channel_close_state(const std::shared_ptr<channel_state> &state);

bool get_channel_state(struct value_t *value,
                       std::shared_ptr<channel_state> &state);

bool is_handle_value(struct value_t *value) {
    if (!value || value->type != T_userdata)
        return false;
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    return userdata->meta && userdata->meta->name &&
           (std::strcmp(userdata->meta->name, "thread_channel") == 0 ||
            std::strcmp(userdata->meta->name, "thread_duplex") == 0);
}

struct value_t *copy_data_value(struct value_t *value,
                                std::unordered_set<const void *> &visiting,
                                std::string &error);

bool validate_data_value(struct value_t *value,
                         std::unordered_set<const void *> &visiting,
                         std::string &error);

struct value_t *copy_data_value(struct value_t *value,
                                std::unordered_set<const void *> &visiting,
                                std::string &error) {
    if (!value) {
        error = "Message payload is null";
        return nullptr;
    }
    if (!validate_data_value(value, visiting, error))
        return nullptr;
    // Containers use copy-on-write payloads, so a validated data value can be
    // shared across threads instead of deep-copied. Mutations detach the
    // payload on the writing side (see ensure_unique_* helpers).
    return dupval(value);
}

bool validate_heap_comparator(struct value_t *value, std::string &error) {
    if (!value || value->type != T_func) {
        error = "heap comparator must be a function";
        return false;
    }
    const struct func_value_t *function =
        (const struct func_value_t *)value;
    if (!function->def || !function->def->code) {
        error = "heap comparator has no function code";
        return false;
    }
    if (!function->def->upvalues.empty()) {
        error = "heap comparator captures a closure";
        return false;
    }
    return true;
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
        case T_lru: {
            const void *payload = value_payload(value);
            if (payload && !visiting.insert(payload).second) {
                error = "Message payload contains a cycle";
                return false;
            }
            struct lru_value_t *lru = (struct lru_value_t *)value;
            for (const auto &entry : lru->payload->items) {
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
        case T_deque: {
            const void *payload = value_payload(value);
            if (payload && !visiting.insert(payload).second) {
                error = "Message payload contains a cycle";
                return false;
            }
            struct deque_value_t *deque = (struct deque_value_t *)value;
            std::vector<struct value_t *> items;
            deque_items_in_order(deque->payload.get(), items);
            for (struct value_t *item : items) {
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
        case T_set: {
            const void *payload = value_payload(value);
            if (payload && !visiting.insert(payload).second) {
                error = "Message payload contains a cycle";
                return false;
            }
            struct set_value_t *set = (struct set_value_t *)value;
            std::vector<struct value_t *> keys;
            set_values_in_order(set->payload.get(), keys);
            for (struct value_t *key : keys) {
                if (!validate_data_value(key, visiting, error)) {
                    if (payload)
                        visiting.erase(payload);
                    return false;
                }
            }
            if (payload)
                visiting.erase(payload);
            return true;
        }
        case T_struct: {
            const void *payload = value_payload(value);
            if (payload && !visiting.insert(payload).second) {
                error = "Message payload contains a cycle";
                return false;
            }
            struct struct_value_t *structure =
                (struct struct_value_t *)value;
            for (struct value_t *field : structure->payload->fields) {
                if (!validate_data_value(field, visiting, error)) {
                    if (payload)
                        visiting.erase(payload);
                    return false;
                }
            }
            if (payload)
                visiting.erase(payload);
            return true;
        }
        case T_enum: {
            const void *payload = value_payload(value);
            if (payload && !visiting.insert(payload).second) {
                error = "Message payload contains a cycle";
                return false;
            }
            struct enum_value_t *enumeration = (struct enum_value_t *)value;
            for (struct value_t *field : enumeration->payload->fields) {
                if (!validate_data_value(field, visiting, error)) {
                    if (payload)
                        visiting.erase(payload);
                    return false;
                }
            }
            if (payload)
                visiting.erase(payload);
            return true;
        }
        case T_heap: {
            const struct heap_value_t *heap =
                (const struct heap_value_t *)value;
            if (!heap->payload) {
                error = "invalid heap value";
                return false;
            }
            if (!validate_heap_comparator(heap->payload->less, error))
                return false;
            std::vector<struct value_t *> items;
            heap_items_level_order(heap->payload.get(), items);
            for (struct value_t *item : items) {
                if (!validate_data_value(item, visiting, error))
                    return false;
            }
            return true;
        }
        case T_func:
            error = "Message payload does not support standalone function "
                    "values";
            return false;
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
        void *wake_handle = event_loop_wake_handle();
        auto ready = [state] {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->closed || channel_can_enqueue_locked(*state);
        };
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->closed) {
                    yyerror("thread.closed: channel is closed");
                    return nullptr;
                }
                if (channel_can_enqueue_locked(*state)) {
                    state->queue.push_back(dupval(message));
                    ++state->total_sends;
                    notify_recv_waiter(*state);
                    state->cv.notify_all();
                    return newnull();
                }
                add_fiber_handle(state->send_fiber_wakes, wake_handle);
            }
            if (g_interrupted) {
                std::lock_guard<std::mutex> lock(state->mutex);
                remove_fiber_handle(state->send_fiber_wakes, wake_handle);
                return nullptr;
            }
            long long remaining = 0;
            if (timeout_ms > 0) {
                const long long elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
                remaining = timeout_ms - elapsed;
                if (remaining <= 0) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    remove_fiber_handle(state->send_fiber_wakes, wake_handle);
                    ++state->send_timeouts;
                    yyerror("thread.timeout: channel send timed out");
                    return nullptr;
                }
            }
            bool alive = true;
            if (wake_handle) {
                alive = fiber_wait_for(ready, remaining);
            } else {
                channel_fiber_sleep(1);
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                remove_fiber_handle(state->send_fiber_wakes, wake_handle);
            }
            if (!alive)
                return nullptr;
        }
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    channel_waiter waiter;
    if (blocking) {
        /* NOTE: this blocks the current OS thread.  The fiber-aware
         * transport will replace this with a task wait. */
        state->send_waiters.push_back(&waiter);
        auto ready = [&state] {
            return state->closed || channel_can_enqueue_locked(*state);
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
            ++state->send_timeouts;
            yyerror("thread.timeout: channel send timed out");
            return nullptr;
        }
    } else if (state->closed ||
               !channel_can_enqueue_nonblocking_locked(*state)) {
        return newboolval(false);
    }

    if (state->closed) {
        yyerror("thread.closed: channel is closed");
        return nullptr;
    }
    state->queue.push_back(dupval(message));
    ++state->total_sends;
    notify_recv_waiter(*state);
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
        void *wake_handle = event_loop_wake_handle();
        auto ready = [state] {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->closed || !state->queue.empty();
        };
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->queue.empty()) {
                    result = channel_recv_result::message;
                    struct value_t *message = state->queue.front();
                    state->queue.pop_front();
                    ++state->total_recvs;
                    notify_send_waiter(*state);
                    return message;
                }
                if (state->closed)
                    return newnull();
                add_fiber_handle(state->recv_fiber_wakes, wake_handle);
                if (state->capacity == 0)
                    notify_send_waiter(*state);
            }
            if (g_interrupted) {
                std::lock_guard<std::mutex> lock(state->mutex);
                remove_fiber_handle(state->recv_fiber_wakes, wake_handle);
                return nullptr;
            }
            long long remaining = 0;
            if (timeout_ms > 0) {
                const long long elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
                remaining = timeout_ms - elapsed;
                if (remaining <= 0) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    remove_fiber_handle(state->recv_fiber_wakes, wake_handle);
                    ++state->recv_timeouts;
                    result = channel_recv_result::timeout;
                    return nullptr;
                }
            }
            bool alive = true;
            if (wake_handle) {
                alive = fiber_wait_for(ready, remaining);
            } else {
                channel_fiber_sleep(1);
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                remove_fiber_handle(state->recv_fiber_wakes, wake_handle);
            }
            if (!alive)
                return nullptr;
        }
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->queue.empty() && !state->closed) {
        channel_waiter waiter;
        state->recv_waiters.push_back(&waiter);
        if (state->capacity == 0)
            notify_send_waiter(*state);
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
            ++state->recv_timeouts;
            result = channel_recv_result::timeout;
            return nullptr;
        }
    }
    if (state->queue.empty())
        return newnull();
    result = channel_recv_result::message;
    struct value_t *message = state->queue.front();
    state->queue.pop_front();
    ++state->total_recvs;
    notify_send_waiter(*state);
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
        ++state->total_recvs;
        notify_send_waiter(*state);
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
                      std::string &error, bool accept_endpoints = false) {
    if (!value || value->type != T_list) {
        error = "expected a list of channels";
        return false;
    }
    struct list_value_t *list = (struct list_value_t *)value;
    for (struct value_t *item : list->payload->items) {
        std::shared_ptr<channel_state> channel;
        if (get_channel_state(item, channel)) {
            channels.push_back(std::move(channel));
            continue;
        }
        if (accept_endpoints && item && item->type == T_userdata) {
            struct userdata_value_t *userdata =
                (struct userdata_value_t *)item;
            if (userdata->meta == &kDuplexEndpointType && userdata->data) {
                std::shared_ptr<duplex_endpoint_state> endpoint =
                    std::static_pointer_cast<duplex_endpoint_state>(
                        userdata->data);
                channels.push_back(endpoint->in());
                continue;
            }
        }
        error = accept_endpoints
                    ? "expected a list of channels or duplex endpoints"
                    : "expected a list of channels";
        return false;
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

void register_select_fiber_wakes(
    const std::vector<std::shared_ptr<channel_state>> &channels,
    void *handle) {
    if (!handle)
        return;
    for (const auto &channel : channels) {
        std::lock_guard<std::mutex> lock(channel->mutex);
        add_fiber_handle(channel->recv_fiber_wakes, handle);
        if (channel->capacity == 0)
            notify_send_waiter(*channel);
    }
}

void unregister_select_fiber_wakes(
    const std::vector<std::shared_ptr<channel_state>> &channels,
    void *handle) {
    if (!handle)
        return;
    for (const auto &channel : channels) {
        std::lock_guard<std::mutex> lock(channel->mutex);
        remove_fiber_handle(channel->recv_fiber_wakes, handle);
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
                ++channels[i]->total_recvs;
                notify_send_waiter(*channels[i]);
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
        void *wake_handle = event_loop_wake_handle();
        auto ready = [channels] {
            for (const auto &channel : channels) {
                std::lock_guard<std::mutex> lock(channel->mutex);
                if (!channel->queue.empty() || channel->closed)
                    return true;
            }
            return false;
        };
        for (;;) {
            if (struct value_t *ready_value = select_poll(channels, start))
                return ready_value;
            if (g_interrupted)
                return nullptr;
            long long remaining = 0;
            if (timeout_ms > 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    return select_timeout_result();
                remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - now).count();
                if (remaining <= 0)
                    remaining = 1;
            }
            bool alive = true;
            if (wake_handle) {
                register_select_fiber_wakes(channels, wake_handle);
                alive = fiber_wait_for(ready, remaining);
                unregister_select_fiber_wakes(channels, wake_handle);
            } else {
                channel_fiber_sleep(1);
            }
            if (!alive)
                return nullptr;
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
            if (channel->capacity == 0)
                notify_send_waiter(*channel);
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
                channel_close_state(state);
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
    if (std::strcmp(name, "waiters") == 0 ||
        std::strcmp(name, "send_waiters") == 0 ||
        std::strcmp(name, "recv_waiters") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (std::strcmp(name, "send_waiters") == 0)
            return newintval((long long)(state->send_waiters.size() +
                                         state->send_fiber_wakes.size()));
        if (std::strcmp(name, "recv_waiters") == 0)
            return newintval((long long)(state->recv_waiters.size() +
                                         state->recv_fiber_wakes.size()));
        return newintval((long long)(state->send_waiters.size() +
                                     state->recv_waiters.size() +
                                     state->send_fiber_wakes.size() +
                                     state->recv_fiber_wakes.size()));
    }
    if (std::strcmp(name, "total_sends") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return newintval(state->total_sends);
    }
    if (std::strcmp(name, "total_recvs") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return newintval(state->total_recvs);
    }
    if (std::strcmp(name, "send_timeouts") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return newintval(state->send_timeouts);
    }
    if (std::strcmp(name, "recv_timeouts") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return newintval(state->recv_timeouts);
    }
    if (std::strcmp(name, "stats") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        struct value_t *result = (struct value_t *)newdictval();
        struct dict_value_t *dict = (struct dict_value_t *)result;
        dict_append_item(dict, newstrval("capacity"),
                         newintval(state->capacity));
        dict_append_item(dict, newstrval("length"),
                         newintval((long long)state->queue.size()));
        dict_append_item(dict, newstrval("closed"),
                         newboolval(state->closed));
        dict_append_item(
            dict, newstrval("waiters"),
            newintval((long long)(state->send_waiters.size() +
                                  state->recv_waiters.size() +
                                  state->send_fiber_wakes.size() +
                                  state->recv_fiber_wakes.size())));
        dict_append_item(
            dict, newstrval("send_waiters"),
            newintval((long long)(state->send_waiters.size() +
                                  state->send_fiber_wakes.size())));
        dict_append_item(
            dict, newstrval("recv_waiters"),
            newintval((long long)(state->recv_waiters.size() +
                                  state->recv_fiber_wakes.size())));
        dict_append_item(dict, newstrval("total_sends"),
                         newintval(state->total_sends));
        dict_append_item(dict, newstrval("total_recvs"),
                         newintval(state->total_recvs));
        dict_append_item(dict, newstrval("send_timeouts"),
                         newintval(state->send_timeouts));
        dict_append_item(dict, newstrval("recv_timeouts"),
                         newintval(state->recv_timeouts));
        return result;
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

std::string duplex_endpoint_describe(const void *) {
    return "<thread_duplex>";
}

struct value_t *new_duplex_endpoint_value(
    std::shared_ptr<duplex_endpoint_state> state) {
    return newuserdataval(&kDuplexEndpointType, std::move(state));
}

struct value_t *duplex_endpoint_get_field(void *data, const char *name) {
    duplex_endpoint_state *raw =
        static_cast<duplex_endpoint_state *>(data);
    if (!raw || !name) {
        yyerror("std.thread.duplex has no such field");
        return nullptr;
    }
    std::shared_ptr<duplex_endpoint_state> endpoint = raw->shared_from_this();

    if (std::strcmp(name, "send") == 0) {
        return newnativeval(
            [endpoint](const std::vector<struct value_t *> &args) {
                if (args.size() != 1) {
                    yyerror("duplex.send expects 1 argument");
                    return (struct value_t *)nullptr;
                }
                return channel_send_value(endpoint->out(), args[0], true, 0);
            },
            endpoint);
    }
    if (std::strcmp(name, "try_send") == 0) {
        return newnativeval(
            [endpoint](const std::vector<struct value_t *> &args) {
                if (args.size() != 1) {
                    yyerror("duplex.try_send expects 1 argument");
                    return (struct value_t *)nullptr;
                }
                return channel_send_value(endpoint->out(), args[0], false);
            },
            endpoint);
    }
    if (std::strcmp(name, "recv") == 0) {
        return newnativeval(
            [endpoint](const std::vector<struct value_t *> &args) {
                long long timeout_ms = 0;
                if (args.size() > 1) {
                    yyerror("duplex.recv expects 0 or 1 argument");
                    return (struct value_t *)nullptr;
                }
                if (args.size() == 1) {
                    if (args[0]->type != T_int ||
                        ((struct int_value_t *)args[0])->value < 0) {
                        yyerror("duplex.recv timeout must be a "
                                "non-negative integer");
                        return (struct value_t *)nullptr;
                    }
                    timeout_ms = ((struct int_value_t *)args[0])->value;
                }
                return channel_recv_value(endpoint->in(), timeout_ms);
            },
            endpoint);
    }
    if (std::strcmp(name, "try_recv") == 0) {
        return newnativeval(
            [endpoint](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("duplex.try_recv expects no arguments");
                    return (struct value_t *)nullptr;
                }
                return channel_try_recv_value(endpoint->in());
            },
            endpoint);
    }
    if (std::strcmp(name, "close_send") == 0) {
        return newnativeval(
            [endpoint](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("duplex.close_send expects no arguments");
                    return (struct value_t *)nullptr;
                }
                channel_close_state(endpoint->out());
                return newnull();
            },
            endpoint);
    }
    if (std::strcmp(name, "close_recv") == 0) {
        return newnativeval(
            [endpoint](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("duplex.close_recv expects no arguments");
                    return (struct value_t *)nullptr;
                }
                channel_close_state(endpoint->in());
                return newnull();
            },
            endpoint);
    }
    if (std::strcmp(name, "close") == 0) {
        return newnativeval(
            [endpoint](const std::vector<struct value_t *> &args) {
                if (!args.empty()) {
                    yyerror("duplex.close expects no arguments");
                    return (struct value_t *)nullptr;
                }
                channel_close_state(endpoint->out());
                channel_close_state(endpoint->in());
                return newnull();
            },
            endpoint);
    }
    if (std::strcmp(name, "child") == 0) {
        std::shared_ptr<duplex_endpoint_state> peer =
            make_duplex_endpoint_state();
        peer->duplex = endpoint->duplex;
        peer->parent = !endpoint->parent;
        return new_duplex_endpoint_value(std::move(peer));
    }
    if (std::strcmp(name, "closed") == 0) {
        std::lock_guard<std::mutex> lock(endpoint->in()->mutex);
        return newboolval(endpoint->in()->closed);
    }
    if (std::strcmp(name, "capacity") == 0) {
        std::lock_guard<std::mutex> lock(endpoint->in()->mutex);
        return newintval(endpoint->in()->capacity);
    }
    if (std::strcmp(name, "length") == 0) {
        std::lock_guard<std::mutex> lock(endpoint->in()->mutex);
        return newintval(
            (long long)endpoint->in()->queue.size());
    }

    yyerror("std.thread.duplex has no field %s", name);
    return nullptr;
}

const struct native_type kDuplexEndpointType = {
    "thread_duplex",
    duplex_endpoint_describe,
    nullptr,
    duplex_endpoint_get_field,
    nullptr,
    nullptr,
};

struct value_t *thread_duplex(const std::vector<struct value_t *> &args) {
    if (args.size() != 1 || args[0]->type != T_int) {
        yyerror("std.thread.duplex expects a capacity integer");
        return nullptr;
    }
    long long capacity = ((struct int_value_t *)args[0])->value;
    if (capacity <= 0) {
        yyerror("std.thread.duplex capacity must be >= 1");
        return nullptr;
    }
    std::shared_ptr<duplex_endpoint_state> endpoint =
        make_duplex_endpoint_state();
    endpoint->duplex = make_duplex_state(capacity);
    endpoint->parent = true;
    return new_duplex_endpoint_value(std::move(endpoint));
}

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
    ++state->total_sends;
    notify_recv_waiter(*state);
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
    std::shared_ptr<message_state> state = make_message_state();
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

void dict_set(struct value_t *dict, const char *key,
              struct value_t *value) {
    dict_set_owned(dict, key, value);
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
    std::shared_ptr<std::atomic<bool>> canceled =
        std::make_shared<std::atomic<bool>>(false);
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

void worker_thread_main(
    pool_state *pool, size_t slot_index,
    std::shared_ptr<const thread_env_snapshot> env_snapshot) {
    thread_env_install(env_snapshot);
    std::string error;
    thread_worker_begin(pool->worker_vm, error);

    struct value_t *handler = nullptr;
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
            if (message->canceled && message->canceled->load()) {
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
            if (message->canceled && message->canceled->load()) {
                freeval(payload);
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
                    std::unordered_set<const void *> visiting;
                    std::string copy_error;
                    struct value_t *safe =
                        copy_data_value(result, visiting, copy_error);
                    freeval(result);
                    result = nullptr;
                    if (!safe) {
                        ok = false;
                        dict_append_item(
                            (struct dict_value_t *)envelope,
                            newstrval("ok"), newboolval(false));
                        dict_append_item(
                            (struct dict_value_t *)envelope,
                            newstrval("error"),
                            newerrorval("thread", copy_error));
                    } else {
                        dict_append_item(
                            (struct dict_value_t *)envelope,
                            newstrval("ok"), newboolval(true));
                        dict_append_item(
                            (struct dict_value_t *)envelope,
                            newstrval("value"), safe);
                    }
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
        if (pool->ready > 0)
            --pool->ready;
        if (crashed && !pool->closing &&
            pool->restart_limit > slot.restarts) {
            ++slot.restarts;
            slot.status = worker_status::starting;
            restart = true;
        }
    }

    thread_worker_end();

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
    std::thread thread(worker_thread_main, pool, slot_index,
                       thread_env_capture());
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

    std::shared_ptr<channel_state> reply = make_channel_state(1);
    struct value_t *reply_value = new_channel_value(reply);
    dict_set_owned(handles, "reply", reply_value);

    auto canceled = std::make_shared<std::atomic<bool>>(false);
    struct value_t *message = make_message_owned(payload, handles);
    std::shared_ptr<message_state> message_state;
    if (get_message_state(message, message_state))
        message_state->canceled = canceled;
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
    future->canceled = canceled;
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
    if (future.canceled->load())
        return thread_future_status::canceled;

    channel_recv_result result = channel_recv_result::closed;
    struct value_t *message =
        channel_recv_value_status(future.reply, timeout_ms, result);
    if (future.canceled->load()) {
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
    if (future.canceled->load()) {
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
                bool first = !future->canceled->exchange(true);
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
    pool->jobs = make_channel_state(queue_capacity);
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
    {
        parser_lock_suspend parse_suspend;
        pool->cv.wait(lock, [&pool] {
            return pool->ready >= pool->size || !pool->startup_error.empty();
        });
    }
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

struct value_t *thread_capabilities(
    const std::vector<struct value_t *> &args) {
    if (!args.empty()) {
        yyerror("std.thread.capabilities expects no arguments");
        return nullptr;
    }
    value_t *caps = (value_t *)newdictval();
    dict_set(caps, "schema", newintval(1));
#if defined(_WIN32)
    dict_set(caps, "platform", newstrval("windows"));
#elif defined(__linux__)
    dict_set(caps, "platform", newstrval("linux"));
#elif defined(__APPLE__)
    dict_set(caps, "platform", newstrval("macos"));
#elif defined(__FreeBSD__)
    dict_set(caps, "platform", newstrval("freebsd"));
#else
    dict_set(caps, "platform", newstrval("unix"));
#endif
    dict_set(caps, "strategy", newstrval("auto"));
    value_t *strategies = (value_t *)newlistval();
    ((list_value_t *)strategies)
        ->payload->items.push_back(newstrval("dispatch"));
#if SKEIN_HAS_REUSE_PORT
    ((list_value_t *)strategies)
        ->payload->items.push_back(newstrval("reuse_port"));
#endif
    dict_set(caps, "strategies", strategies);
    dict_set(caps, "reuse_port",
             newboolval(SKEIN_HAS_REUSE_PORT != 0));
    dict_set(caps, "dispatch", newboolval(true));
    dict_set(caps, "http1", newboolval(true));
#if defined(SKEIN_ENABLE_HTTP2)
    dict_set(caps, "http2",
             newboolval(SKEIN_HAS_REUSE_PORT != 0));
#else
    dict_set(caps, "http2", newboolval(false));
#endif
#if defined(SKEIN_ENABLE_TLS)
    dict_set(caps, "tls", newboolval(true));
#else
    dict_set(caps, "tls", newboolval(false));
#endif
    dict_set(caps, "keep_alive", newboolval(true));
    dict_set(caps, "stream_request_body",
             newboolval(SKEIN_HAS_REUSE_PORT != 0));
    dict_set(caps, "stream_response",
             newboolval(SKEIN_HAS_REUSE_PORT != 0));
    dict_set(caps, "websocket", newboolval(false));
    dict_set(caps, "sse", newboolval(false));
    dict_set(caps, "raw_socket", newboolval(false));
    return caps;
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
    std::shared_ptr<channel_state> channel;
    std::shared_ptr<duplex_endpoint_state> link;

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

static void collect_upvalue_use(const skein::bc::Proto &proto,
                                std::vector<bool> &used) {
    for (const skein::bc::Instr &ins : proto.code) {
        switch ((skein::bc::Op)ins.op) {
            case skein::bc::Op::LoadUpvalue:
            case skein::bc::Op::StoreUpvalue:
            case skein::bc::Op::CompoundUpvalue:
            case skein::bc::Op::IncDecUpvalue:
                if (ins.b < used.size())
                    used[ins.b] = true;
                break;
            default:
                break;
        }
    }
    std::unordered_map<std::string, std::size_t> by_name;
    for (std::size_t i = 0; i < proto.upvalues.size(); ++i)
        by_name.emplace(proto.upvalues[i], i);
    auto note_name = [&](const std::string &name) {
        auto it = by_name.find(name);
        if (it != by_name.end())
            used[it->second] = true;
    };
    for (const skein::bc::Instr &ins : proto.code) {
        switch ((skein::bc::Op)ins.op) {
            case skein::bc::Op::LoadName:
            case skein::bc::Op::StoreName:
            case skein::bc::Op::CompoundName:
            case skein::bc::Op::IncDecName:
                if (ins.b < proto.consts.size()) {
                    const skein::bc::Constant &constant = proto.consts[ins.b];
                    if (constant.kind == skein::bc::ConstantKind::Name)
                        note_name(constant.name);
                }
                break;
            default:
                break;
        }
    }
    for (const skein::bc::IndexDesc &index : proto.index_ops) {
        if (index.base_kind == skein::bc::IndexBaseKind::Upvalue &&
            index.base_index < used.size())
            used[index.base_index] = true;
        else if (index.base_kind == skein::bc::IndexBaseKind::Name)
            note_name(index.base_name);
    }
}

static void collect_global_names(const skein::bc::Proto &proto,
                                 std::vector<std::string> &names) {
    std::unordered_set<std::string> seen;
    auto note = [&](const std::string &name) {
        if (name.empty() || name == "std")
            return;
        if (seen.insert(name).second)
            names.push_back(name);
    };
    for (const skein::bc::Instr &ins : proto.code) {
        switch ((skein::bc::Op)ins.op) {
            case skein::bc::Op::LoadGlobal:
            case skein::bc::Op::StoreGlobal:
            case skein::bc::Op::CompoundGlobal:
            case skein::bc::Op::IncDecGlobal:
                if (ins.b < proto.consts.size()) {
                    const skein::bc::Constant &constant = proto.consts[ins.b];
                    if (constant.kind == skein::bc::ConstantKind::Name)
                        note(constant.name);
                }
                break;
            default:
                break;
        }
    }
    for (const skein::bc::IndexDesc &index : proto.index_ops) {
        if (index.base_kind == skein::bc::IndexBaseKind::Global)
            note(index.base_name);
    }
}

struct thread_value_deleter {
    void operator()(struct value_t *value) const { freeval(value); }
};

bool contains_transfer_special(
    const struct value_t *value,
    std::unordered_set<const void *> &visiting, int depth = 0) {
    if (!value || depth > 64)
        return false;
    switch (value->type) {
    case T_heap:
    case T_func:
    case T_native:
    case T_userdata:
        return true;
    case T_dict: {
        const struct dict_value_t *dict =
            (const struct dict_value_t *)value;
        if (dict->payload && dict->payload->is_namespace)
            return true;
        const void *identity = value_payload(value);
        if (identity && !visiting.insert(identity).second)
            return false;
        for (const auto &entry : dict->payload->items) {
            if (contains_transfer_special(entry.first, visiting, depth + 1) ||
                contains_transfer_special(entry.second, visiting,
                                          depth + 1)) {
                if (identity)
                    visiting.erase(identity);
                return true;
            }
        }
        if (identity)
            visiting.erase(identity);
        return false;
    }
    case T_list: {
        const struct list_value_t *list =
            (const struct list_value_t *)value;
        const void *identity = value_payload(value);
        if (identity && !visiting.insert(identity).second)
            return false;
        for (const struct value_t *item : list->payload->items) {
            if (contains_transfer_special(item, visiting, depth + 1)) {
                if (identity)
                    visiting.erase(identity);
                return true;
            }
        }
        if (identity)
            visiting.erase(identity);
        return false;
    }
    case T_lru: {
        const struct lru_value_t *lru = (const struct lru_value_t *)value;
        const void *identity = value_payload(value);
        if (identity && !visiting.insert(identity).second)
            return false;
        for (const auto &entry : lru->payload->items) {
            if (contains_transfer_special(entry.first, visiting, depth + 1) ||
                contains_transfer_special(entry.second, visiting,
                                          depth + 1)) {
                if (identity)
                    visiting.erase(identity);
                return true;
            }
        }
        if (identity)
            visiting.erase(identity);
        return false;
    }
    case T_deque: {
        const struct deque_value_t *deque =
            (const struct deque_value_t *)value;
        const void *identity = value_payload(value);
        if (identity && !visiting.insert(identity).second)
            return false;
        std::vector<struct value_t *> items;
        deque_items_in_order(deque->payload.get(), items);
        for (struct value_t *item : items) {
            if (contains_transfer_special(item, visiting, depth + 1)) {
                if (identity)
                    visiting.erase(identity);
                return true;
            }
        }
        if (identity)
            visiting.erase(identity);
        return false;
    }
    case T_set: {
        const struct set_value_t *set = (const struct set_value_t *)value;
        const void *identity = value_payload(value);
        if (identity && !visiting.insert(identity).second)
            return false;
        std::vector<struct value_t *> keys;
        set_values_in_order(set->payload.get(), keys);
        for (struct value_t *key : keys) {
            if (contains_transfer_special(key, visiting, depth + 1)) {
                if (identity)
                    visiting.erase(identity);
                return true;
            }
        }
        if (identity)
            visiting.erase(identity);
        return false;
    }
    case T_result: {
        const struct result_value_t *result =
            (const struct result_value_t *)value;
        return contains_transfer_special(
            result->ok ? result->value : result->error, visiting, depth + 1);
    }
    case T_struct: {
        const struct struct_value_t *structure =
            (const struct struct_value_t *)value;
        const void *identity = value_payload(value);
        if (identity && !visiting.insert(identity).second)
            return false;
        for (const struct value_t *field : structure->payload->fields) {
            if (contains_transfer_special(field, visiting, depth + 1)) {
                if (identity)
                    visiting.erase(identity);
                return true;
            }
        }
        if (identity)
            visiting.erase(identity);
        return false;
    }
    case T_enum: {
        const struct enum_value_t *enumeration =
            (const struct enum_value_t *)value;
        const void *identity = value_payload(value);
        if (identity && !visiting.insert(identity).second)
            return false;
        for (const struct value_t *field : enumeration->payload->fields) {
            if (contains_transfer_special(field, visiting, depth + 1)) {
                if (identity)
                    visiting.erase(identity);
                return true;
            }
        }
        if (identity)
            visiting.erase(identity);
        return false;
    }
    default:
        return false;
    }
}

struct thread_payload_builder {
    std::unordered_map<const function_def *,
                       std::shared_ptr<thread_transfer_function>>
        functions;
    std::vector<std::pair<std::string, thread_transfer_value>> globals;
    std::vector<thread_transfer_module> modules;
    std::unordered_set<std::string> global_names;
    std::unordered_set<std::string> module_keys;
    std::unordered_set<const void *> namespace_visiting;
    std::unordered_set<const void *> container_visiting;
    std::string error;

    bool transfer_value(struct value_t *value, const char *what,
                        thread_transfer_value &out) {
        out = thread_transfer_value();
        if (!value) {
            out.kind = thread_transfer_value::Kind::Null;
            return true;
        }
        if (value->type == T_dict) {
            const struct dict_value_t *dict =
                (const struct dict_value_t *)value;
            if (dict->payload && dict->payload->is_namespace)
                return transfer_namespace(value, out);
        }
        if (value->type == T_heap)
            return transfer_heap(value, out);
        if (value->type == T_func)
            return transfer_function(value, out);
        if (value->type == T_builtin) {
            out.kind = thread_transfer_value::Kind::Data;
            out.data = std::shared_ptr<struct value_t>(
                dupval(value), thread_value_deleter());
            return true;
        }
        {
            std::unordered_set<const void *> special_visiting;
            if (contains_transfer_special(value, special_visiting)) {
                if (value->type == T_list || value->type == T_dict ||
                    value->type == T_set || value->type == T_deque ||
                    value->type == T_lru)
                    return transfer_container(value, out);
                error = std::string(what ? what : "value") +
                        " contains a heap/function but is not a container";
                return false;
            }
        }
        std::unordered_set<const void *> visiting;
        std::string copy_error;
        struct value_t *copy =
            copy_data_value(value, visiting, copy_error);
        if (!copy) {
            if (value->type == T_list || value->type == T_dict ||
                value->type == T_set || value->type == T_deque ||
                value->type == T_lru)
                return transfer_container(value, out);
            error = std::string(what ? what : "value") + ": " + copy_error;
            return false;
        }
        out.kind = thread_transfer_value::Kind::Data;
        out.data = std::shared_ptr<struct value_t>(copy, thread_value_deleter());
        return true;
    }

    bool transfer_container(struct value_t *value,
                            thread_transfer_value &out) {
        const void *identity = value_payload(value);
        if (identity && !container_visiting.insert(identity).second) {
            error = "cyclic container";
            return false;
        }
        std::shared_ptr<thread_transfer_container> container =
            std::make_shared<thread_transfer_container>();
        bool ok = true;
        switch (value->type) {
        case T_list: {
            container->kind = thread_transfer_container::Kind::List;
            const struct list_value_t *list =
                (const struct list_value_t *)value;
            for (struct value_t *item : list->payload->items) {
                thread_transfer_value transfer;
                if (!transfer_value(item, "list item", transfer)) {
                    ok = false;
                    break;
                }
                container->items.push_back(std::move(transfer));
            }
            break;
        }
        case T_dict: {
            container->kind = thread_transfer_container::Kind::Dict;
            const struct dict_value_t *dict =
                (const struct dict_value_t *)value;
            for (const auto &entry : dict->payload->items) {
                thread_transfer_value key;
                if (!transfer_value(entry.first, "dict key", key)) {
                    ok = false;
                    break;
                }
                thread_transfer_value item;
                if (!transfer_value(entry.second, "dict value", item)) {
                    ok = false;
                    break;
                }
                container->entries.emplace_back(std::move(key),
                                                std::move(item));
            }
            break;
        }
        case T_set: {
            container->kind = thread_transfer_container::Kind::Set;
            const struct set_value_t *set =
                (const struct set_value_t *)value;
            std::vector<struct value_t *> keys;
            set_values_in_order(set->payload.get(), keys);
            for (struct value_t *key : keys) {
                thread_transfer_value transfer;
                if (!transfer_value(key, "set value", transfer)) {
                    ok = false;
                    break;
                }
                container->items.push_back(std::move(transfer));
            }
            break;
        }
        case T_deque: {
            container->kind = thread_transfer_container::Kind::Deque;
            const struct deque_value_t *deque =
                (const struct deque_value_t *)value;
            std::vector<struct value_t *> items;
            deque_items_in_order(deque->payload.get(), items);
            for (struct value_t *item : items) {
                thread_transfer_value transfer;
                if (!transfer_value(item, "deque item", transfer)) {
                    ok = false;
                    break;
                }
                container->items.push_back(std::move(transfer));
            }
            break;
        }
        case T_lru: {
            container->kind = thread_transfer_container::Kind::Lru;
            const struct lru_value_t *lru =
                (const struct lru_value_t *)value;
            container->capacity = lru->payload->capacity;
            for (const auto &entry : lru->payload->items) {
                thread_transfer_value key;
                if (!transfer_value(entry.first, "lru key", key)) {
                    ok = false;
                    break;
                }
                thread_transfer_value item;
                if (!transfer_value(entry.second, "lru value", item)) {
                    ok = false;
                    break;
                }
                container->entries.emplace_back(std::move(key),
                                                std::move(item));
            }
            break;
        }
        default:
            error = std::string("cannot transfer container ") +
                    typename_of(value);
            ok = false;
            break;
        }
        if (identity)
            container_visiting.erase(identity);
        if (!ok)
            return false;
        out = thread_transfer_value();
        out.kind = thread_transfer_value::Kind::Container;
        out.container = std::move(container);
        return true;
    }

    bool transfer_namespace(struct value_t *value,
                            thread_transfer_value &out) {
        struct dict_value_t *dict = (struct dict_value_t *)value;
        const void *identity = dict->payload.get();
        if (!namespace_visiting.insert(identity).second) {
            error = "cyclic module namespace";
            return false;
        }
        out = thread_transfer_value();
        out.kind = thread_transfer_value::Kind::Namespace;
        bool ok = true;
        for (const auto &item : dict->payload->items) {
            if (!item.first || item.first->type != T_string)
                continue;
            std::string name =
                ((const struct string_value_t *)item.first)->value;
            std::string what = "module export " + name;
            thread_transfer_value field;
            if (!transfer_value(item.second, what.c_str(), field)) {
                ok = false;
                break;
            }
            out.fields.emplace_back(std::move(name), std::move(field));
        }
        namespace_visiting.erase(identity);
        return ok;
    }

    bool transfer_heap(struct value_t *value,
                       thread_transfer_value &out) {
        const struct heap_value_t *heap =
            (const struct heap_value_t *)value;
        if (!heap->payload) {
            error = "invalid heap value";
            return false;
        }
        std::shared_ptr<thread_transfer_heap> payload =
            std::make_shared<thread_transfer_heap>();
        if (!transfer_value(heap->payload->less, "heap comparator",
                            payload->less))
            return false;
        payload->size = heap->payload->size;

        std::unordered_map<const void *, uint32_t> ids;
        std::vector<const struct heap_node *> order;
        auto collect = [&](auto &&self,
                           const std::shared_ptr<heap_node> &node) -> bool {
            if (!node || ids.count(node.get()))
                return true;
            ids.emplace(node.get(), (uint32_t)order.size());
            order.push_back(node.get());
            return self(self, node->left) && self(self, node->right);
        };
        if (!collect(collect, heap->payload->root))
            return false;
        payload->nodes.reserve(order.size());
        for (const struct heap_node *node : order) {
            thread_transfer_heap_node record;
            record.rank = (uint32_t)node->rank;
            if (!transfer_value(node->value, "heap value", record.value))
                return false;
            auto left = ids.find(node->left.get());
            if (left != ids.end())
                record.left = left->second;
            auto right = ids.find(node->right.get());
            if (right != ids.end())
                record.right = right->second;
            payload->nodes.push_back(std::move(record));
        }
        if (heap->payload->root) {
            auto root = ids.find(heap->payload->root.get());
            if (root == ids.end()) {
                error = "invalid heap root";
                return false;
            }
            payload->root = root->second;
        }
        out = thread_transfer_value();
        out.kind = thread_transfer_value::Kind::Heap;
        out.heap = std::move(payload);
        return true;
    }

    bool transfer_function(struct value_t *value,
                           thread_transfer_value &out) {
        const func_value_t *function = (const func_value_t *)value;
        if (!function->def || !function->def->code) {
            error = "function value has no function code";
            return false;
        }
        const std::shared_ptr<function_def> &def = function->def;
        auto existing = functions.find(def.get());
        if (existing != functions.end()) {
            out.kind = thread_transfer_value::Kind::Function;
            out.function = existing->second;
            return true;
        }
        skein::bc::ProtoPtr proto;
        std::string prepare_error;
        if (!skein_vm_prepare_thread_helper(value, proto, prepare_error)) {
            error = prepare_error;
            return false;
        }
        std::shared_ptr<thread_transfer_function> payload =
            std::make_shared<thread_transfer_function>();
        payload->name = def->code->name;
        payload->proto = std::move(proto);
        functions.emplace(def.get(), payload);
        out.kind = thread_transfer_value::Kind::Function;
        out.function = payload;
        if (!fill_captures(payload, def))
            return false;
        if (!collect_globals(payload))
            return false;
        return collect_imports(payload);
    }

    bool fill_captures(
        const std::shared_ptr<thread_transfer_function> &payload,
        const std::shared_ptr<function_def> &def) {
        const skein::bc::Proto &proto = *payload->proto;
        std::vector<bool> used(proto.upvalues.size(), false);
        collect_upvalue_use(proto, used);
        payload->captures.assign(proto.upvalues.size(),
                                 thread_transfer_value());
        for (std::size_t i = 0; i < used.size(); ++i) {
            if (!used[i])
                continue;
            struct value_t *captured = nullptr;
            if (i < def->upvalues.size()) {
                const std::shared_ptr<upvalue_cell> &cell = def->upvalues[i];
                if (cell)
                    captured = cell->slot ? *cell->slot : cell->value;
            }
            if (!captured && def->closure) {
                std::string_view name(proto.upvalues[i]);
                for (scope_t *scope = def->closure.get();
                     scope && !scope->is_global; scope = scope->parent.get()) {
                    struct symbol *sym = scope->syms.find(name);
                    if (sym) {
                        captured = sym->val;
                        break;
                    }
                }
            }
            std::string what = "captured " + proto.upvalues[i];
            if (!transfer_value(captured, what.c_str(), payload->captures[i]))
                return false;
        }
        return true;
    }

    bool collect_globals(
        const std::shared_ptr<thread_transfer_function> &payload) {
        std::vector<std::string> names;
        collect_global_names(*payload->proto, names);
        for (const std::string &name : names) {
            if (!global_names.insert(name).second)
                continue;
            struct value_t *value = lookup_global_symbol(name.c_str());
            if (!value)
                continue;
            std::string what = "global " + name;
            thread_transfer_value transfer;
            if (!transfer_value(value, what.c_str(), transfer))
                return false;
            globals.emplace_back(name, std::move(transfer));
        }
        return true;
    }

    bool collect_imports(
        const std::shared_ptr<thread_transfer_function> &payload) {
        for (const skein::bc::ImportDesc &import : payload->proto->imports) {
            if (import.path.empty() || import.path == "std")
                continue;
            std::string key;
            std::string import_error;
            if (!package_import_cache_key(
                    import.path.c_str(),
                    import.base.empty() ? nullptr : import.base.c_str(), key,
                    import_error)) {
                error = "import " + import.path + ": " + import_error;
                return false;
            }
            if (!module_keys.insert(key).second)
                continue;
            struct value_t *namespace_value =
                package_import(import.path.c_str(),
                               import.base.empty() ? nullptr
                                                   : import.base.c_str(),
                               import_error);
            if (!namespace_value) {
                error = "import " + import.path + ": " + import_error;
                return false;
            }
            std::string what = "module " + import.path;
            thread_transfer_value transfer;
            bool ok =
                transfer_value(namespace_value, what.c_str(), transfer);
            freeval(namespace_value);
            if (!ok)
                return false;
            thread_transfer_module module;
            module.key = std::move(key);
            module.path = import.path;
            module.base = import.base;
            module.value = std::move(transfer);
            modules.push_back(std::move(module));
        }
        return true;
    }

    bool build_entry(
        struct value_t *entry,
        std::shared_ptr<thread_transfer_function> &payload,
        std::vector<std::pair<std::string, thread_transfer_value>> &out,
        std::vector<thread_transfer_module> &out_modules) {
        skein::bc::ProtoPtr proto;
        if (!skein_vm_prepare_thread_proto(entry, proto, error))
            return false;
        const func_value_t *function = (const func_value_t *)entry;
        const std::shared_ptr<function_def> &def = function->def;
        payload = std::make_shared<thread_transfer_function>();
        payload->name = def->code->name;
        payload->proto = std::move(proto);
        functions.emplace(def.get(), payload);
        if (!fill_captures(payload, def))
            return false;
        if (!collect_globals(payload))
            return false;
        if (!collect_imports(payload))
            return false;
        out = std::move(globals);
        out_modules = std::move(modules);
        return true;
    }
};

void thread_proto_entry_main(
    std::shared_ptr<thread_state> state,
    std::shared_ptr<thread_transfer_function> entry,
    std::vector<std::pair<std::string, thread_transfer_value>> globals,
    std::vector<thread_transfer_module> modules,
    std::vector<thread_transfer_type> types,
    struct value_t *argument,
    std::shared_ptr<const thread_env_snapshot> env_snapshot) {
    thread_env_install(env_snapshot);
    std::string begin_error;
    thread_worker_begin(true, begin_error);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->startup_done = true;
        if (!entry)
            state->startup_error = "invalid thread entry proto";
    }
    state->cv.notify_all();
    if (!entry) {
        freeval(argument);
        thread_worker_end();
        return;
    }

    {
        struct value_t *std_namespace = skein::stdlib::build_namespace();
        if (std_namespace) {
            save_global_symbol("std", std_namespace);
            package_seed_import("builtin:std", dupval(std_namespace));
            package_seed_import_path("std", nullptr,
                                     dupval(std_namespace));
        }
    }
    std::shared_ptr<thread_transfer_context> transfer_context =
        skein_vm_thread_transfer_context_create();
    std::string setup_error;
    if (!skein_vm_apply_thread_types(transfer_context, types, setup_error))
        setup_error = "types: " + setup_error;
    types.clear();
    if (setup_error.empty()) {
        for (auto &item : modules) {
            struct value_t *value = nullptr;
            std::string error;
            if (!skein_vm_materialize_thread_value(transfer_context, item.value,
                                                   value, error)) {
                setup_error = "module " + item.path + ": " + error;
                break;
            }
            package_seed_import(item.key, dupval(value));
            package_seed_import_path(
                item.path.c_str(),
                item.base.empty() ? nullptr : item.base.c_str(), value);
        }
    }
    modules.clear();
    if (setup_error.empty()) {
        for (auto &item : globals) {
            struct value_t *value = nullptr;
            std::string error;
            if (!skein_vm_materialize_thread_value(
                    transfer_context, item.second, value, error)) {
                setup_error = "global " + item.first + ": " + error;
                break;
            }
            if (value)
                save_global_symbol(item.first.c_str(), value);
        }
    }
    globals.clear();

    eval_capture_snapshot snapshot;
    eval_capture_begin(snapshot);
    struct value_t *result = nullptr;
    std::string call_error;
    bool call_started = false;
    if (setup_error.empty()) {
        thread_transfer_value entry_value;
        entry_value.kind = thread_transfer_value::Kind::Function;
        entry_value.function = std::move(entry);
        struct value_t *function = nullptr;
        if (!skein_vm_materialize_thread_value(transfer_context, entry_value,
                                               function, call_error)) {
            call_error = "thread entry: " + call_error;
        } else {
            result = skein_call_function(function, {argument});
            call_started = true;
            freeval(function);
        }
    } else {
        call_error = setup_error;
    }
    if (!call_started)
        freeval(argument);
    eval_capture_result captured = eval_capture_end(snapshot);

    struct value_t *envelope = nullptr;
    if (captured.failed || !result) {
        struct value_t *err = captured.error;
        captured.error = nullptr;
        if (!err)
            err = newerrorval("thread", call_error.empty()
                                           ? "thread function failed"
                                           : call_error.c_str());
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
    transfer_context.reset();
    thread_worker_end();
}

void thread_entry_main(
    std::shared_ptr<thread_state> state, const std::string &script,
    const std::string &entry, struct value_t *argument,
    std::shared_ptr<const thread_env_snapshot> env_snapshot) {
    thread_env_install(env_snapshot);
    thread_runtime_enter();
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

    std::unique_ptr<eval_context> context =
        std::make_unique<eval_context>();
    set_active_eval_context(context.get());
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
    context.reset();
    value_runtime_thread_shutdown();
    thread_runtime_leave();
}

struct value_t *thread_spawn_function(
    const std::vector<struct value_t *> &args);

struct value_t *thread_spawn(const std::vector<struct value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 ||
        args[1]->type != T_userdata) {
        yyerror("std.thread.spawn expects script or thread func, Message "
                "and optional options");
        return nullptr;
    }
    if (args[0]->type == T_func) {
        const func_value_t *function = (const func_value_t *)args[0];
        if (!function->def || !function->def->code ||
            !function->def->code->is_thread_entry) {
            yyerror("std.thread.spawn function must be declared thread func");
            return nullptr;
        }
        if (args.size() != 2) {
            yyerror("std.thread.spawn thread func does not accept options");
            return nullptr;
        }
        return thread_spawn_function(args);
    }
    if (args[0]->type != T_string) {
        yyerror("std.thread.spawn expects script or thread func, Message "
                "and optional options");
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
                                    entry, argument,
                                    thread_env_capture());
    } catch (const std::exception &) {
        freeval(argument);
        yyerror("std.thread.spawn could not start thread");
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    {
        parser_lock_suspend parse_suspend;
        state->cv.wait(lock, [&state] { return state->startup_done; });
    }
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

static struct value_t *thread_spawn_payload(
    std::shared_ptr<thread_transfer_function> entry,
    std::vector<std::pair<std::string, thread_transfer_value>> globals,
    std::vector<thread_transfer_module> modules,
    std::vector<thread_transfer_type> types,
    struct value_t *message, const char *what) {
    std::shared_ptr<thread_state> state = std::make_shared<thread_state>();
    state->id = next_thread_id.fetch_add(1);
    struct value_t *argument = dupval(message);
    try {
        state->thread = std::thread(thread_proto_entry_main, state,
                                    std::move(entry), std::move(globals),
                                    std::move(modules), std::move(types),
                                    argument, thread_env_capture());
    } catch (const std::exception &) {
        freeval(argument);
        yyerror("%s could not start thread", what);
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&state] { return state->startup_done; });
    if (!state->startup_error.empty()) {
        std::string startup_error = state->startup_error;
        lock.unlock();
        if (state->thread.joinable())
            state->thread.join();
        yyerror("%s: %s", what, startup_error.c_str());
        return nullptr;
    }
    lock.unlock();
    return new_thread_value(std::move(state));
}

struct value_t *thread_spawn_function(
    const std::vector<struct value_t *> &args) {
    if (args.size() != 2 || args[0]->type != T_func ||
        args[1]->type != T_userdata) {
        yyerror("std.thread.spawn_function expects function and Message");
        return nullptr;
    }
    thread_payload_builder builder;
    std::shared_ptr<thread_transfer_function> entry;
    std::vector<std::pair<std::string, thread_transfer_value>> globals;
    std::vector<thread_transfer_module> modules;
    if (!builder.build_entry(args[0], entry, globals, modules)) {
        yyerror("std.thread.spawn_function: %s", builder.error.c_str());
        return nullptr;
    }
    return thread_spawn_payload(std::move(entry), std::move(globals),
                                std::move(modules), {},
                                args[1], "std.thread.spawn_function");
}

struct value_t *thread_encode(const std::vector<struct value_t *> &args) {
    if (args.size() != 1 || args[0]->type != T_func) {
        yyerror("std.thread.encode expects a function");
        return nullptr;
    }
    thread_payload_builder builder;
    std::shared_ptr<thread_transfer_function> entry;
    std::vector<std::pair<std::string, thread_transfer_value>> globals;
    std::vector<thread_transfer_module> modules;
    if (!builder.build_entry(args[0], entry, globals, modules)) {
        yyerror("std.thread.encode: %s", builder.error.c_str());
        return nullptr;
    }
    std::string bytes;
    std::string error;
    if (!skein_vm_serialize_thread_payload(entry, globals, modules, bytes,
                                           error)) {
        yyerror("std.thread.encode: %s", error.c_str());
        return nullptr;
    }
    return newbytesval(std::move(bytes));
}

struct value_t *thread_spawn_encoded(
    const std::vector<struct value_t *> &args) {
    if (args.size() != 2 || args[0]->type != T_bytes ||
        args[1]->type != T_userdata) {
        yyerror("std.thread.spawn_encoded expects bytes and Message");
        return nullptr;
    }
    const std::string &bytes =
        ((const struct bytes_value_t *)args[0])->value.read();
    std::shared_ptr<thread_transfer_function> entry;
    std::vector<std::pair<std::string, thread_transfer_value>> globals;
    std::vector<thread_transfer_module> modules;
    std::vector<thread_transfer_type> types;
    std::string error;
    if (!skein_vm_deserialize_thread_payload(bytes, entry, globals, modules,
                                             types, error)) {
        yyerror("std.thread.spawn_encoded: %s", error.c_str());
        return nullptr;
    }
    return thread_spawn_payload(std::move(entry), std::move(globals),
                                std::move(modules), std::move(types), args[1],
                                "std.thread.spawn_encoded");
}

struct value_t *thread_spawn_source(
    const std::vector<struct value_t *> &args) {    if (args.size() < 2 || args.size() > 3 ||
        args[0]->type != T_string || args[1]->type != T_userdata) {
        yyerror("std.thread.spawn_source expects source, Message and "
                "optional options");
        return nullptr;
    }
    const std::string &source = ((string_value_t *)args[0])->value;
    namespace fs = std::filesystem;
    static std::atomic<unsigned long long> source_counter{0};
    std::vector<fs::path> directories;
    const std::string cwd = thread_cwd();
    if (!cwd.empty())
        directories.emplace_back(fs::u8path(cwd));
    std::error_code fs_error;
    fs::path temp_dir = fs::temp_directory_path(fs_error);
    if (!fs_error && !temp_dir.empty())
        directories.push_back(temp_dir);
    if (directories.empty()) {
        yyerror("std.thread.spawn_source: no writable temporary directory");
        return nullptr;
    }

    fs::path path;
    std::string write_error;
    for (const fs::path &directory : directories) {
        for (int attempt = 0; attempt < 32; ++attempt) {
            const unsigned long long id =
                source_counter.fetch_add(1, std::memory_order_relaxed);
            path = directory /
                   ("skein-thread-source-" + std::to_string(id) +
                    ".skein");
            if (platform_write_file(path.u8string(), source, false,
                                    write_error))
                break;
            path.clear();
        }
        if (!path.empty())
            break;
    }
    if (path.empty()) {
        yyerror("std.thread.spawn_source: %s",
                write_error.c_str());
        return nullptr;
    }

    std::vector<struct value_t *> spawn_args;
    spawn_args.push_back(newstrval(path.u8string().c_str()));
    spawn_args.push_back(dupval(args[1]));
    if (args.size() == 3)
        spawn_args.push_back(dupval(args[2]));
    struct value_t *thread = thread_spawn(spawn_args);
    for (struct value_t *arg : spawn_args)
        freeval(arg);
    std::error_code remove_error;
    fs::remove(path, remove_error);
    return thread;
}

struct value_t *message_with_handle(
    const std::shared_ptr<message_state> &source, const char *name,
    struct value_t *handle) {
    std::shared_ptr<message_state> state = make_message_state();
    state->payload = dupval(source->payload);
    state->handles = (struct value_t *)newdictval();
    if (source->handles && source->handles->type == T_dict) {
        for (const auto &entry :
             ((struct dict_value_t *)source->handles)->payload->items) {
            dict_append_item((struct dict_value_t *)state->handles,
                             dupval(entry.first), dupval(entry.second));
        }
    }
    dict_append_item((struct dict_value_t *)state->handles,
                     newstrval(name), dupval(handle));
    return new_message_value(std::move(state));
}

std::shared_ptr<thread_state> thread_state_of(struct value_t *value) {
    if (!value || value->type != T_userdata)
        return nullptr;
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    if (userdata->meta != &kThreadType || !userdata->data)
        return nullptr;
    return std::static_pointer_cast<thread_state>(userdata->data);
}

struct value_t *thread_spawn_with_channel(
    const std::vector<struct value_t *> &args) {
    if (args.size() < 3 || args.size() > 4 ||
        args[0]->type != T_string || args[1]->type != T_userdata ||
        args[2]->type != T_int) {
        yyerror("std.thread.spawn_with_channel expects script, Message, "
                "capacity and optional options");
        return nullptr;
    }
    long long capacity = ((struct int_value_t *)args[2])->value;
    if (capacity <= 0) {
        yyerror("std.thread.spawn_with_channel capacity must be >= 1");
        return nullptr;
    }
    std::shared_ptr<message_state> message;
    if (!get_message_state(args[1], message)) {
        yyerror("std.thread.spawn_with_channel expects a Message");
        return nullptr;
    }

    std::shared_ptr<channel_state> channel =
        make_channel_state(capacity);
    struct value_t *channel_value = new_channel_value(channel);
    struct value_t *injected =
        message_with_handle(message, "channel", channel_value);
    freeval(channel_value);

    std::vector<struct value_t *> spawn_args;
    spawn_args.push_back(dupval(args[0]));
    spawn_args.push_back(injected);
    if (args.size() == 4)
        spawn_args.push_back(dupval(args[3]));
    struct value_t *thread = thread_spawn(spawn_args);
    freeval(spawn_args[0]);
    freeval(injected);
    if (spawn_args.size() == 3)
        freeval(spawn_args[2]);
    if (!thread)
        return nullptr;

    std::shared_ptr<thread_state> state = thread_state_of(thread);
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->channel = std::move(channel);
    }
    return thread;
}

struct value_t *thread_spawn_with_duplex(
    const std::vector<struct value_t *> &args) {
    if (args.size() < 3 || args.size() > 4 ||
        args[0]->type != T_string || args[1]->type != T_userdata ||
        args[2]->type != T_int) {
        yyerror("std.thread.spawn_with_duplex expects script, Message, "
                "capacity and optional options");
        return nullptr;
    }
    long long capacity = ((struct int_value_t *)args[2])->value;
    if (capacity <= 0) {
        yyerror("std.thread.spawn_with_duplex capacity must be >= 1");
        return nullptr;
    }
    std::shared_ptr<message_state> message;
    if (!get_message_state(args[1], message)) {
        yyerror("std.thread.spawn_with_duplex expects a Message");
        return nullptr;
    }

    std::shared_ptr<duplex_state> duplex =
        make_duplex_state(capacity);
    std::shared_ptr<duplex_endpoint_state> parent =
        make_duplex_endpoint_state();
    parent->duplex = duplex;
    parent->parent = true;
    std::shared_ptr<duplex_endpoint_state> child =
        make_duplex_endpoint_state();
    child->duplex = duplex;
    child->parent = false;

    struct value_t *child_value = new_duplex_endpoint_value(child);
    struct value_t *injected =
        message_with_handle(message, "link", child_value);
    freeval(child_value);

    std::vector<struct value_t *> spawn_args;
    spawn_args.push_back(dupval(args[0]));
    spawn_args.push_back(injected);
    if (args.size() == 4)
        spawn_args.push_back(dupval(args[3]));
    struct value_t *thread = thread_spawn(spawn_args);
    freeval(spawn_args[0]);
    freeval(injected);
    if (spawn_args.size() == 3)
        freeval(spawn_args[2]);
    if (!thread)
        return nullptr;

    std::shared_ptr<thread_state> state = thread_state_of(thread);
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->link = std::move(parent);
    }
    return thread;
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
    if (std::strcmp(name, "channel") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->channel ? new_channel_value(state->channel)
                              : newnull();
    }
    if (std::strcmp(name, "link") == 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->link ? new_duplex_endpoint_value(state->link)
                           : newnull();
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

    std::shared_ptr<message_state> state = make_message_state();
    state->payload = payload;
    state->handles = handles;
    return new_message_value(std::move(state));
}

struct value_t *thread_channel(long long capacity) {
    if (capacity < -1) {
        yyerror("std.thread.Channel capacity must be >= 0 or -1");
        return nullptr;
    }
    return new_channel_value(make_channel_state(capacity));
}

struct thread_gc_node {
    enum class kind { message, channel, duplex, endpoint };
    kind type = kind::message;
    void *identity = nullptr;
    std::shared_ptr<void> keepalive;
    long long refs = 0;
    long long internal_in = 0;
    bool marked = false;
    std::vector<thread_gc_node *> out;
};

void collect_thread_cycles_impl() {
    std::vector<std::unique_ptr<thread_gc_node>> nodes;
    std::unordered_map<const void *, thread_gc_node *> by_identity;

    auto add_registry = [&nodes, &by_identity](auto &registry,
                                               thread_gc_node::kind type) {
        size_t write = 0;
        for (size_t i = 0; i < registry.size(); ++i) {
            auto locked = registry[i].lock();
            if (!locked)
                continue;
            registry[write++] = registry[i];
            auto node = std::make_unique<thread_gc_node>();
            node->type = type;
            node->identity = locked.get();
            node->refs = locked.use_count();
            node->keepalive = locked;
            by_identity[node->identity] = node.get();
            nodes.push_back(std::move(node));
        }
        registry.resize(write);
    };

    {
        std::lock_guard<std::mutex> lock(thread_cycle_registry_mutex);
        add_registry(thread_message_registry, thread_gc_node::kind::message);
        add_registry(thread_channel_registry, thread_gc_node::kind::channel);
        add_registry(thread_duplex_registry, thread_gc_node::kind::duplex);
        add_registry(thread_endpoint_registry, thread_gc_node::kind::endpoint);
    }
    if (nodes.empty())
        return;

    auto node_for = [&by_identity](const void *identity) -> thread_gc_node * {
        auto it = by_identity.find(identity);
        return it == by_identity.end() ? nullptr : it->second;
    };
    auto add_edge = [&node_for](thread_gc_node *from, const void *identity) {
        thread_gc_node *to = node_for(identity);
        if (!to)
            return;
        from->out.push_back(to);
        ++to->internal_in;
    };

    for (auto &node : nodes) {
        switch (node->type) {
            case thread_gc_node::kind::message: {
                auto *state = static_cast<message_state *>(node->identity);
                if (!state->handles || state->handles->type != T_dict)
                    break;
                for (const auto &entry :
                     ((dict_value_t *)state->handles)->payload->items) {
                    value_t *value = entry.second;
                    if (!value || value->type != T_userdata)
                        continue;
                    auto *userdata = (userdata_value_t *)value;
                    if (!userdata->data)
                        continue;
                    if (userdata->meta == &kChannelType)
                        add_edge(node.get(),
                                 static_cast<channel_state *>(
                                     userdata->data.get()));
                    else if (userdata->meta == &kDuplexEndpointType)
                        add_edge(node.get(),
                                 static_cast<duplex_endpoint_state *>(
                                     userdata->data.get()));
                }
                break;
            }
            case thread_gc_node::kind::channel: {
                auto *state = static_cast<channel_state *>(node->identity);
                std::lock_guard<std::mutex> lock(state->mutex);
                for (value_t *value : state->queue) {
                    if (!value || value->type != T_userdata)
                        continue;
                    auto *userdata = (userdata_value_t *)value;
                    if (userdata->meta == &kMessageType && userdata->data)
                        add_edge(node.get(),
                                 static_cast<message_state *>(
                                     userdata->data.get()));
                }
                break;
            }
            case thread_gc_node::kind::duplex: {
                auto *state = static_cast<duplex_state *>(node->identity);
                if (state->parent_to_child)
                    add_edge(node.get(), state->parent_to_child.get());
                if (state->child_to_parent)
                    add_edge(node.get(), state->child_to_parent.get());
                break;
            }
            case thread_gc_node::kind::endpoint: {
                auto *state =
                    static_cast<duplex_endpoint_state *>(node->identity);
                if (state->duplex)
                    add_edge(node.get(), state->duplex.get());
                break;
            }
        }
    }

    std::vector<thread_gc_node *> stack;
    for (auto &node : nodes) {
        if (node->refs - 1 - node->internal_in > 0)
            stack.push_back(node.get());
    }
    while (!stack.empty()) {
        thread_gc_node *node = stack.back();
        stack.pop_back();
        if (node->marked)
            continue;
        node->marked = true;
        for (thread_gc_node *child : node->out)
            stack.push_back(child);
    }

    bool collected = false;
    for (auto &node : nodes) {
        if (node->marked)
            continue;
        collected = true;
        ++thread_gc_collected;
        switch (node->type) {
            case thread_gc_node::kind::message: {
                auto *state = static_cast<message_state *>(node->identity);
                value_t *handles = state->handles;
                state->handles = nullptr;
                value_t *payload = state->payload;
                state->payload = nullptr;
                freeval(handles);
                freeval(payload);
                break;
            }
            case thread_gc_node::kind::channel: {
                auto *state = static_cast<channel_state *>(node->identity);
                std::deque<value_t *> queued;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    queued.swap(state->queue);
                    state->closed = true;
                }
                for (value_t *value : queued)
                    freeval(value);
                break;
            }
            case thread_gc_node::kind::duplex: {
                auto *state = static_cast<duplex_state *>(node->identity);
                std::shared_ptr<channel_state> left =
                    std::move(state->parent_to_child);
                std::shared_ptr<channel_state> right =
                    std::move(state->child_to_parent);
                break;
            }
            case thread_gc_node::kind::endpoint: {
                auto *state =
                    static_cast<duplex_endpoint_state *>(node->identity);
                std::shared_ptr<duplex_state> duplex =
                    std::move(state->duplex);
                break;
            }
        }
    }
    (void)collected;
}

void collect_thread_cycles_entry() {
    std::unique_lock<std::mutex> lock(thread_runtime_gate);
    if (active_thread_runtimes != 0)
        return;
    collect_thread_cycles_impl();
}

}  // namespace

namespace {

constexpr uint32_t kProcessRequestMagic = 0x52504B53u; /* "SKPR" */
constexpr uint32_t kProcessRequestVersion = 1u;

bool take_u32(const std::string &data, std::size_t &pos, uint32_t &value) {
    if (pos + 4 > data.size())
        return false;
    value = (uint32_t)(unsigned char)data[pos] |
            ((uint32_t)(unsigned char)data[pos + 1] << 8) |
            ((uint32_t)(unsigned char)data[pos + 2] << 16) |
            ((uint32_t)(unsigned char)data[pos + 3] << 24);
    pos += 4;
    return true;
}

} // namespace

bool thread_run_encoded_blocking(const std::string &bytes,
                                 struct value_t *message,
                                 struct value_t *&result,
                                 std::string &error) {
    std::shared_ptr<thread_transfer_function> entry;
    std::vector<std::pair<std::string, thread_transfer_value>> globals;
    std::vector<thread_transfer_module> modules;
    std::vector<thread_transfer_type> types;
    if (!skein_vm_deserialize_thread_payload(bytes, entry, globals, modules,
                                             types, error))
        return false;
    struct value_t *argument = dupval(message);
    struct value_t *thread = thread_spawn_payload(
        std::move(entry), std::move(globals), std::move(modules),
        std::move(types), argument, "std.process.spawn_encoded");
    if (!thread) {
        error = "could not start encoded process entry";
        return false;
    }
    std::shared_ptr<thread_state> state = thread_state_of(thread);
    if (!state) {
        freeval(thread);
        error = "invalid encoded process entry";
        return false;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&state] { return state->done; });
    if (!state->joined) {
        state->joined = true;
        lock.unlock();
        if (state->thread.joinable())
            state->thread.join();
        lock.lock();
    }
    result = state->result ? dupval(state->result) : newnull();
    freeval(thread);
    return true;
}

bool thread_run_encoded_request(const std::string &request,
                                std::string &result, std::string &error) {
    std::size_t pos = 0;
    uint32_t magic = 0;
    uint32_t version = 0;
    if (!take_u32(request, pos, magic) || !take_u32(request, pos, version)) {
        error = "truncated process request";
        return false;
    }
    if (magic != kProcessRequestMagic) {
        error = "invalid process request header";
        return false;
    }
    if (version != kProcessRequestVersion) {
        error = "unsupported process request version";
        return false;
    }
    uint32_t entry_size = 0;
    if (!take_u32(request, pos, entry_size) ||
        entry_size > request.size() - pos) {
        error = "truncated process entry payload";
        return false;
    }
    std::string entry_bytes = request.substr(pos, entry_size);
    pos += entry_size;
    uint32_t payload_size = 0;
    if (!take_u32(request, pos, payload_size) ||
        payload_size > request.size() - pos) {
        error = "truncated process message payload";
        return false;
    }
    std::string payload_bytes = request.substr(pos, payload_size);
    struct value_t *payload = nullptr;
    if (!skein_vm_deserialize_message_payload(payload_bytes, payload, error))
        return false;
    std::shared_ptr<message_state> message_state = make_message_state();
    message_state->payload = payload;
    message_state->handles = (struct value_t *)newdictval();
    struct value_t *message = new_message_value(std::move(message_state));
    struct value_t *envelope = nullptr;
    bool ok = thread_run_encoded_blocking(entry_bytes, message, envelope,
                                          error);
    freeval(message);
    if (!ok)
        return false;
    ok = skein_vm_serialize_message_payload(envelope, result, error);
    freeval(envelope);
    return ok;
}

struct value_t *thread_message_payload(struct value_t *message) {
    std::shared_ptr<message_state> state;
    if (!get_message_state(message, state))
        return nullptr;
    return dupval(state->payload);
}

namespace {
thread_local std::unique_ptr<eval_context> worker_runtime_context;
thread_local skein_context worker_runtime_hooks;
}

bool thread_worker_begin(bool worker_vm, std::string &error) {
    thread_runtime_enter();
    (void)error;
    worker_runtime_hooks.call = pool_worker_call;
    worker_runtime_hooks.call_builtin = pool_worker_call_builtin;
    worker_runtime_hooks.error = runtime_error_report;
    worker_runtime_hooks.drain_background = nullptr;
    skein_context_set(&worker_runtime_hooks);
    skein_vm_set_enabled(worker_vm);
    g_interactive = false;
    g_show_prompt = false;
    worker_runtime_context = std::make_unique<eval_context>();
    set_active_eval_context(worker_runtime_context.get());
    initscope();
    eval_begin_root_defers();
    return true;
}

void thread_worker_end() {
    eval_run_root_defers();
    fiber_disable_generator_warnings();
    task_shutdown();
    fiber_shutdown();
    event_loop_shutdown();
    ast_thread_shutdown();
    skein_vm_shutdown();
    set_active_eval_context(nullptr);
    skein_context_set(nullptr);
    worker_runtime_context.reset();
    package_clear_import_seeds();
    value_runtime_thread_shutdown();
    thread_runtime_leave();
}

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
    bool first = !future->future->canceled->exchange(true);
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
        case B_thread_spawn_source:
            return thread_spawn_source(args);
        case B_thread_spawn_function:
            return thread_spawn_function(args);
        case B_thread_encode:
            return thread_encode(args);
        case B_thread_spawn_encoded:
            return thread_spawn_encoded(args);
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
            if (!collect_channels(args[0], channels, error, true)) {
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
        case B_thread_duplex:
            return thread_duplex(args);
        case B_thread_spawn_with_channel:
            return thread_spawn_with_channel(args);
        case B_thread_spawn_with_duplex:
            return thread_spawn_with_duplex(args);
        case B_thread_debug_counts:
            return thread_debug_counts(args);
        case B_thread_collect:
            return thread_collect(args);
        case B_thread_capabilities:
            return thread_capabilities(args);
        default:
            yyerror("unknown thread function");
            return nullptr;
    }
}
