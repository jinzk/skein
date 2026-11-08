#include "thread_channel.h"

#include "builtin_ids.h"
#include "cycle.h"
#include "dict.h"
#include "symbol.h"
#include "value_ops.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
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
        default:
            yyerror("unknown thread function");
            return nullptr;
    }
}
