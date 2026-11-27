#include "process_io.h"

#include "ast.h"
#include "builtin_ids.h"
#include "builtins_io.h"
#include "fiber.h"
#include "process_spawn.h"
#include "task.h"
#include "valuetype.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

value_t *call_io(int functype, std::vector<value_t *> args) {
    return call_io_builtin(functype, args);
}

value_t *wait_async_task(value_t *task, long long timeout_ms,
                         std::string &error,
                         const char *timeout_message) {
    if (!task) {
        error = "cannot start process pipe operation";
        return nullptr;
    }
    if (fiber_in_fiber()) {
        task_ptr pending_task;
        if (!get_task_value(task, pending_task)) {
            freeval(task);
            error = "invalid process pipe task";
            return nullptr;
        }
        auto ready = [pending_task]() {
            return task_get_state(pending_task) != TASK_STATE_PENDING;
        };
        if (!fiber_wait_for(ready, timeout_ms)) {
            value_t *cancelled = call_task_builtin(B_task_cancel, {task});
            if (cancelled)
                freeval(cancelled);
            freeval(task);
            error = timeout_message;
            return nullptr;
        }
    }

    value_t *wait_timeout =
        timeout_ms > 0 ? newintval(timeout_ms) : nullptr;
    std::vector<value_t *> wait_args{task};
    if (wait_timeout)
        wait_args.push_back(wait_timeout);
    value_t *wait_result = call_task_builtin(B_task_try_wait, wait_args);
    if (wait_timeout)
        freeval(wait_timeout);
    freeval(task);
    if (!wait_result) {
        error = "cannot wait for process pipe operation";
        return nullptr;
    }
    result_value_t *result = (result_value_t *)wait_result;
    if (!result->ok) {
        if (result->error && result->error->type == T_error)
            error = ((error_value_t *)result->error)->message;
        else
            error = "process pipe operation failed";
        freeval(wait_result);
        return nullptr;
    }
    value_t *chunk = result->value;
    result->value = nullptr;
    freeval(wait_result);
    if (!chunk) {
        error = "process pipe operation failed";
        return nullptr;
    }
    return chunk;
}

value_t *read_async_chunk(value_t *stream, long long max_bytes,
                          long long timeout_ms, std::string &error,
                          const char *timeout_message) {
    value_t *size = newintval(max_bytes);
    value_t *timeout = timeout_ms > 0 ? newintval(timeout_ms) : nullptr;
    std::vector<value_t *> args{stream, size};
    if (timeout)
        args.push_back(timeout);
    value_t *task = call_io(B_io_read_async, args);
    freeval(size);
    if (timeout)
        freeval(timeout);
    if (!task) {
        error = "cannot start process pipe read";
        return nullptr;
    }
    return wait_async_task(task, timeout_ms, error, timeout_message);
}

} // namespace


process_io::process_io(process_io &&other) noexcept
    : stdin_stream(other.stdin_stream),
      stdout_stream(other.stdout_stream),
      pending(std::move(other.pending)) {
    other.stdin_stream = nullptr;
    other.stdout_stream = nullptr;
}

process_io &process_io::operator=(process_io &&other) noexcept {
    if (this == &other)
        return *this;
    stdin_stream = other.stdin_stream;
    stdout_stream = other.stdout_stream;
    pending = std::move(other.pending);
    other.stdin_stream = nullptr;
    other.stdout_stream = nullptr;
    return *this;
}

bool process_io::attach(value_t *process, std::string &error) {
    stdin_stream = nullptr;
    stdout_stream = nullptr;
    pending.clear();
    if (!process || process->type != T_dict) {
        error = "process pipe expects a process handle";
        return false;
    }
    dict_value_t *dict = (dict_value_t *)process;
    for (auto &entry : dict->payload->items) {
        if (!entry.first || entry.first->type != T_string)
            continue;
        const char *key = ((string_value_t *)entry.first)->value;
        if (std::strcmp(key, "stdin") == 0)
            stdin_stream = entry.second;
        else if (std::strcmp(key, "stdout") == 0)
            stdout_stream = entry.second;
    }
    if (!stdin_stream || !stdout_stream) {
        stdin_stream = nullptr;
        stdout_stream = nullptr;
        error = "process has no stdio pipes";
        return false;
    }
    return true;
}

bool process_io::usable() const {
    return stdin_stream && stdout_stream;
}

bool process_io::write(const std::string &data) {
    if (!stdin_stream)
        return false;
    std::size_t offset = 0;
    while (offset < data.size()) {
        value_t *bytes = newbytesval(data.substr(offset));
        value_t *task = call_io(B_io_write_async, {stdin_stream, bytes});
        freeval(bytes);
        if (!task)
            return false;
        std::string error;
        value_t *result =
            wait_async_task(task, 0, error, "process pipe write timed out");
        if (!result)
            return false;
        if (result->type != T_int) {
            freeval(result);
            return false;
        }
        const long long written = ((int_value_t *)result)->value;
        freeval(result);
        if (written <= 0 ||
            (std::size_t)written > data.size() - offset)
            return false;
        offset += (std::size_t)written;
    }
    return true;
}

bool process_io::flush() {
    if (!stdin_stream)
        return false;
    value_t *result = call_io(B_io_flush, {stdin_stream});
    if (!result)
        return false;
    freeval(result);
    return true;
}

value_t *process_io::read_line(long long timeout_ms, std::string &error) {
    if (!stdout_stream) {
        error = "process pipe has no stdout";
        return nullptr;
    }
    std::string buffer;
    if (!pending.empty()) {
        buffer.swap(pending);
        const std::size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            pending = buffer.substr(newline + 1);
            buffer.resize(newline);
            return newstrval(buffer.c_str());
        }
    }
    if (timeout_ms <= 0 && !fiber_in_fiber()) {
        value_t *line = call_io(B_io_read_line, {stdout_stream});
        if (!line)
            return nullptr;
        if (line->type == T_null) {
            if (buffer.empty())
                return line;
            freeval(line);
            return newstrval(buffer.c_str());
        }
        std::string text;
        if (line->type == T_bytes)
            text = ((bytes_value_t *)line)->value;
        else if (line->type == T_string)
            text = ((string_value_t *)line)->value;
        freeval(line);
        buffer += text;
        return newstrval(buffer.c_str());
    }

    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        long long remaining = 0;
        if (timeout_ms > 0) {
            const auto now = std::chrono::steady_clock::now();
            const long long elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - start)
                    .count();
            remaining = timeout_ms - elapsed;
            if (remaining <= 0) {
                error = "process pipe read timed out";
                return nullptr;
            }
        }
        value_t *chunk = read_async_chunk(
            stdout_stream, 65536, remaining, error,
            "process pipe read timed out");
        if (!chunk)
            return nullptr;
        if (chunk->type == T_null) {
            freeval(chunk);
            break;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            error = "process pipe expected bytes";
            return nullptr;
        }
        const std::string &bytes = ((bytes_value_t *)chunk)->value;
        const std::size_t newline = bytes.find('\n');
        if (newline != std::string::npos) {
            buffer.append(bytes.data(), newline);
            pending = bytes.substr(newline + 1);
            freeval(chunk);
            break;
        }
        buffer += bytes;
        freeval(chunk);
    }
    return newstrval(buffer.c_str());
}

value_t *process_io::read_exact(std::size_t size, long long timeout_ms,
                                  std::string &error) {
    if (size == 0)
        return newbytesval(std::string());
    std::string buffered;
    if (!pending.empty()) {
        const std::size_t take = std::min(size, pending.size());
        buffered.assign(pending.data(), take);
        pending.erase(0, take);
    }
    const auto start = std::chrono::steady_clock::now();
    while (buffered.size() < size) {
        const std::size_t missing = size - buffered.size();
        long long remaining = 0;
        if (timeout_ms > 0) {
            const auto now = std::chrono::steady_clock::now();
            const long long elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - start)
                    .count();
            remaining = timeout_ms - elapsed;
            if (remaining <= 0) {
                error = "process pipe body read timed out";
                return nullptr;
            }
        }
        const long long request =
            (long long)std::min<std::size_t>(missing, 65536);
        value_t *chunk = read_async_chunk(
            stdout_stream, request, remaining, error,
            "process pipe body read timed out");
        if (!chunk)
            return nullptr;
        if (chunk->type == T_null) {
            freeval(chunk);
            error = "process closed its stdout";
            return nullptr;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            error = "process pipe expected bytes";
            return nullptr;
        }
        const std::string &bytes = ((bytes_value_t *)chunk)->value;
        if (bytes.empty()) {
            freeval(chunk);
            error = "process pipe body read failed";
            return nullptr;
        }
        buffered.append(bytes.data(),
                        std::min<std::size_t>(bytes.size(), missing));
        freeval(chunk);
    }
    return newbytesval(std::move(buffered));
}

void process_io::close_streams() {
    if (stdin_stream) {
        value_t *result = call_io(B_io_close, {stdin_stream});
        if (result)
            freeval(result);
        stdin_stream = nullptr;
    }
    stdout_stream = nullptr;
    pending.clear();
}

// ---------------------------------------------------------------------------
// Public std.process.spawn_pipe / std.process.io value
// ---------------------------------------------------------------------------

struct process_io_value : std::enable_shared_from_this<process_io_value> {
    value_t *process = nullptr;
    bool owns_process = false;
    process_io io;

    ~process_io_value() { close(); }

    void close() {
        io.close_streams();
        if (process) {
            if (owns_process) {
                value_t *timeout = newintval(1000);
                value_t *wait_result = call_process_spawn_builtin(
                    B_process_wait, {process, timeout});
                freeval(timeout);
                if (wait_result)
                    freeval(wait_result);
                value_t *close_result = call_process_spawn_builtin(
                    B_process_close, {process});
                if (close_result)
                    freeval(close_result);
            }
            freeval(process);
            process = nullptr;
        }
    }
};

std::string process_io_describe(const void *data) {
    const process_io_value *state =
        static_cast<const process_io_value *>(data);
    return state && state->process ? "<process_io>"
                                   : "<process_io: closed>";
}

value_t *io_method(value_t *self,
                   const std::shared_ptr<process_io_value> &state,
                   const char *name) {
    auto method = [state, self](
        std::function<value_t *(const std::vector<value_t *> &)> call) {
        value_t *fn = newnativeval(std::move(call), state);
        native_add_child(fn, self);
        return fn;
    };
    if (std::strcmp(name, "write") == 0) {
        return method([state](const std::vector<value_t *> &args) {
            if (args.size() != 1) {
                yyerror("process_io.write expects 1 argument");
                return (value_t *)nullptr;
            }
            std::string data;
            if (args[0]->type == T_bytes)
                data = ((bytes_value_t *)args[0])->value;
            else if (args[0]->type == T_string)
                data = ((string_value_t *)args[0])->value;
            else {
                yyerror("process_io.write expects bytes or string");
                return (value_t *)nullptr;
            }
            if (!state->io.write(data)) {
                yyerror("process_io.write failed");
                return (value_t *)nullptr;
            }
            return newintval((long long)data.size());
        });
    }
    if (std::strcmp(name, "flush") == 0) {
        return method([state](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("process_io.flush expects no arguments");
                return (value_t *)nullptr;
            }
            if (!state->io.flush()) {
                yyerror("process_io.flush failed");
                return (value_t *)nullptr;
            }
            return newnull();
        });
    }
    if (std::strcmp(name, "read_line") == 0) {
        return method([state](const std::vector<value_t *> &args) {
            if (args.size() > 1) {
                yyerror("process_io.read_line expects 0 or 1 argument");
                return (value_t *)nullptr;
            }
            long long timeout = 0;
            if (!args.empty()) {
                if (args[0]->type != T_int || ((int_value_t *)args[0])->value < 0) {
                    yyerror("process_io.read_line timeout must be non-negative");
                    return (value_t *)nullptr;
                }
                timeout = ((int_value_t *)args[0])->value;
            }
            std::string error;
            value_t *line = state->io.read_line(timeout, error);
            if (!line) {
                if (!error.empty())
                    yyerror("%s", error.c_str());
                return (value_t *)nullptr;
            }
            return line;
        });
    }
    if (std::strcmp(name, "read_exact") == 0) {
        return method([state](const std::vector<value_t *> &args) {
            if (args.empty() || args.size() > 2 ||
                args[0]->type != T_int ||
                ((int_value_t *)args[0])->value < 0) {
                yyerror("process_io.read_exact expects size[, timeout_ms]");
                return (value_t *)nullptr;
            }
            long long timeout = 0;
            if (args.size() == 2) {
                if (args[1]->type != T_int ||
                    ((int_value_t *)args[1])->value < 0) {
                    yyerror("process_io.read_exact timeout must be non-negative");
                    return (value_t *)nullptr;
                }
                timeout = ((int_value_t *)args[1])->value;
            }
            std::string error;
            value_t *data = state->io.read_exact(
                (std::size_t)((int_value_t *)args[0])->value, timeout, error);
            if (!data) {
                if (!error.empty())
                    yyerror("%s", error.c_str());
                return (value_t *)nullptr;
            }
            return data;
        });
    }
    if (std::strcmp(name, "close") == 0) {
        return method([state](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("process_io.close expects no arguments");
                return (value_t *)nullptr;
            }
            state->close();
            return newnull();
        });
    }
    if (std::strcmp(name, "wait") == 0) {
        return method([state](const std::vector<value_t *> &args) {
            if (args.size() > 1 || !state->process) {
                yyerror("process_io.wait expects 0 or 1 argument");
                return (value_t *)nullptr;
            }
            std::vector<value_t *> wait_args{dupval(state->process)};
            if (!args.empty())
                wait_args.push_back(dupval(args[0]));
            value_t *result =
                call_process_spawn_builtin(B_process_wait, wait_args);
            for (value_t *arg : wait_args)
                freeval(arg);
            return result;
        });
    }
    if (std::strcmp(name, "kill") == 0) {
        return method([state](const std::vector<value_t *> &args) {
            if (args.size() > 1 || !state->process) {
                yyerror("process_io.kill expects 0 or 1 argument");
                return (value_t *)nullptr;
            }
            std::vector<value_t *> kill_args{dupval(state->process)};
            if (!args.empty())
                kill_args.push_back(dupval(args[0]));
            value_t *result =
                call_process_spawn_builtin(B_process_kill, kill_args);
            for (value_t *arg : kill_args)
                freeval(arg);
            return result;
        });
    }
    if (std::strcmp(name, "pid") == 0) {
        return method([state](const std::vector<value_t *> &args) {
            if (!args.empty() || !state->process) {
                yyerror("process_io.pid expects no arguments");
                return (value_t *)nullptr;
            }
            std::vector<value_t *> pid_args{dupval(state->process)};
            value_t *result =
                call_process_spawn_builtin(B_process_pid, pid_args);
            for (value_t *arg : pid_args)
                freeval(arg);
            return result;
        });
    }
    if (std::strcmp(name, "process") == 0) {
        return method([state](const std::vector<value_t *> &args) {
            if (!args.empty() || !state->process) {
                yyerror("process_io.process expects no arguments");
                return (value_t *)nullptr;
            }
            return dupval(state->process);
        });
    }
    yyerror("process_io has no field %s", name);
    return (value_t *)nullptr;
}

value_t *process_io_get_field(value_t *self, void *data, const char *name) {
    process_io_value *raw = static_cast<process_io_value *>(data);
    if (!raw || !name) {
        yyerror("process_io has no field");
        return (value_t *)nullptr;
    }
    return io_method(self, raw->shared_from_this(), name);
}

void process_io_for_each_child(
    const void *data,
    const std::function<void(const struct value_t *child)> &visit) {
    const process_io_value *state =
        static_cast<const process_io_value *>(data);
    if (!state)
        return;
    if (state->process)
        visit(state->process);
    if (state->io.stdin_stream)
        visit(state->io.stdin_stream);
    if (state->io.stdout_stream)
        visit(state->io.stdout_stream);
}

/* Compatibility entry point for callers that only hold the payload. */
value_t *process_io_get_field_plain(void *data, const char *name) {
    return process_io_get_field(nullptr, data, name);
}

const native_type kProcessIoType = {
    "process_io",
    process_io_describe,
    nullptr,
    process_io_get_field_plain,
    nullptr,
    process_io_for_each_child,
    process_io_get_field,
};

value_t *new_process_io_value(std::shared_ptr<process_io_value> state) {
    return newuserdataval(&kProcessIoType, std::move(state));
}

value_t *call_process_io_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    if (functype == B_process_spawn_pipe) {
        if (args.empty() || args.size() > 2 ||
            args[0]->type != T_string) {
            yyerror("std.process.spawn_pipe expects command[, options]");
            return nullptr;
        }
        std::vector<value_t *> spawn_args{dupval(args[0])};
        if (args.size() == 2)
            spawn_args.push_back(dupval(args[1]));
        value_t *process = call_process_spawn_builtin(
            B_process_spawn_command, spawn_args);
        for (value_t *arg : spawn_args)
            freeval(arg);
        if (!process)
            return nullptr;
        auto state = std::make_shared<process_io_value>();
        state->process = process;
        state->owns_process = true;
        std::string error;
        if (!state->io.attach(process, error)) {
            yyerror("%s", error.c_str());
            state->close();
            return nullptr;
        }
        return new_process_io_value(std::move(state));
    }
    if (functype == B_process_io) {
        if (args.size() != 1) {
            yyerror("std.process.io expects 1 argument");
            return nullptr;
        }
        auto state = std::make_shared<process_io_value>();
        state->process = dupval(args[0]);
        state->owns_process = false;
        std::string error;
        if (!state->io.attach(state->process, error)) {
            yyerror("%s", error.c_str());
            return nullptr;
        }
        return new_process_io_value(std::move(state));
    }
    yyerror("unknown process_io builtin");
    return nullptr;
}
