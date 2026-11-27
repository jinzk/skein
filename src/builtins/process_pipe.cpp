#include "process_pipe.h"

#include "builtin_ids.h"
#include "builtins_io.h"
#include "fiber.h"
#include "process_spawn.h"
#include "task.h"
#include "valuetype.h"

#include <algorithm>
#include <chrono>
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

process_pipe::~process_pipe() {
    close(true, true);
}

process_pipe::process_pipe(process_pipe &&other) noexcept
    : process(other.process),
      stdin_stream(other.stdin_stream),
      stdout_stream(other.stdout_stream),
      pending(std::move(other.pending)) {
    other.process = nullptr;
    other.stdin_stream = nullptr;
    other.stdout_stream = nullptr;
}

process_pipe &process_pipe::operator=(process_pipe &&other) noexcept {
    if (this == &other)
        return *this;
    close(true, true);
    process = other.process;
    stdin_stream = other.stdin_stream;
    stdout_stream = other.stdout_stream;
    pending = std::move(other.pending);
    other.process = nullptr;
    other.stdin_stream = nullptr;
    other.stdout_stream = nullptr;
    return *this;
}

bool process_pipe::spawn(const char *executable, struct value_t *options,
                         std::string &error) {
    close(true, true);
    if (!executable || !*executable) {
        error = "cannot locate process executable";
        return false;
    }
    value_t *executable_value = newstrval(executable);
    value_t *process = call_process_spawn_builtin(
        B_process_spawn_command, {executable_value, options});
    freeval(executable_value);
    if (!process) {
        error = "cannot spawn process";
        return false;
    }
    value_t *stdin_stream = nullptr;
    value_t *stdout_stream = nullptr;
    if (process->type == T_dict) {
        dict_value_t *dict = (dict_value_t *)process;
        for (auto &entry : dict->payload->items) {
            if (!entry.first || entry.first->type != T_string)
                continue;
            const char *key =
                ((string_value_t *)entry.first)->value;
            if (std::string(key) == "stdin")
                stdin_stream = entry.second;
            else if (std::string(key) == "stdout")
                stdout_stream = entry.second;
        }
    }
    if (!stdin_stream || !stdout_stream) {
        freeval(process);
        error = "process has no pipes";
        return false;
    }
    this->process = process;
    this->stdin_stream = stdin_stream;
    this->stdout_stream = stdout_stream;
    pending.clear();
    return true;
}

bool process_pipe::usable() const {
    return process && stdin_stream && stdout_stream;
}

bool process_pipe::write(const std::string &data) {
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

bool process_pipe::flush() {
    if (!stdin_stream)
        return false;
    value_t *result = call_io(B_io_flush, {stdin_stream});
    if (!result)
        return false;
    freeval(result);
    return true;
}

value_t *process_pipe::read_line(long long timeout_ms, std::string &error) {
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

value_t *process_pipe::read_exact(std::size_t size, long long timeout_ms,
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

void process_pipe::close(bool kill_if_running, bool force) {
    if (stdin_stream) {
        value_t *result = call_io(B_io_close, {stdin_stream});
        if (result)
            freeval(result);
        stdin_stream = nullptr;
    }
    if (process) {
        if (!force) {
            value_t *timeout = newintval(1000);
            value_t *wait_result =
                call_process_spawn_builtin(B_process_wait,
                                           {process, timeout});
            freeval(timeout);
            if (wait_result)
                freeval(wait_result);
        }
        if (kill_if_running) {
            std::vector<value_t *> kill_args{process};
            if (force)
                kill_args.push_back(newintval(9));
            value_t *kill_result = call_process_spawn_builtin(
                B_process_kill, kill_args);
            if (kill_args.size() > 1)
                freeval(kill_args[1]);
            if (kill_result)
                freeval(kill_result);
        }
        value_t *close_result = call_process_spawn_builtin(
            B_process_close, {process});
        if (close_result)
            freeval(close_result);
        freeval(process);
        process = nullptr;
    }
    stdout_stream = nullptr;
    pending.clear();
}
