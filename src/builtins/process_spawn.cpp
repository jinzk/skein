#include "process_spawn.h"

#include "ast.h"
#include "async_support.h"
#include "builtin_ids.h"
#include "builtins_io.h"
#include "dict.h"
#include "event_loop.h"
#include "platform.h"
#include "runtime_state.h"
#include "task.h"
#include "thread_channel.h"
#include "valuetype.h"
#include "vm.h"

#include <uv.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct process_resource;
struct process_pipe;

struct pipe_owner {
    std::shared_ptr<process_resource> process;
    process_pipe *pipe = nullptr;
};

struct pipe_async_op;

struct process_pipe {
    uv_pipe_t handle;
    process_resource *process = nullptr;
    int kind = 0;  // 0 = stdin, 1 = stdout, 2 = stderr
    bool open = true;
    bool reading = false;
    bool read_done = false;
    bool read_eof = false;
    int read_status = 0;
    std::size_t read_requested = 0;
    std::string read_buffer;
    std::string read_result;
    bool writing = false;
    bool write_done = false;
    int write_status = 0;
    uv_write_t write_request;
    std::string write_data;
    pipe_async_op *pending_read = nullptr;
    pipe_async_op *pending_write = nullptr;
};

struct pipe_async_op : skein_async::state {
    std::shared_ptr<process_resource> process;
    process_pipe *pipe = nullptr;
    bool is_write = false;
    std::size_t requested = 0;
    bool eof = false;
    int status = 0;
    std::string buffer;
    std::string result;
    std::string write_data;
    uv_write_t write_request;
};

struct pipe_write_context {
    process_pipe *pipe = nullptr;
    pipe_async_op *op = nullptr;
};

struct process_resource {
    uv_process_t process;
    bool process_initialized = false;
    bool exited = false;
    long long exit_status = -1;
    int term_signal = 0;
    process_pipe *stdin_pipe = nullptr;
    process_pipe *stdout_pipe = nullptr;
    process_pipe *stderr_pipe = nullptr;

    ~process_resource() {
        delete stdin_pipe;
        delete stdout_pipe;
        delete stderr_pipe;
    }
};

std::vector<std::shared_ptr<process_resource>> &process_registry() {
    static thread_local std::vector<std::shared_ptr<process_resource>> registry;
    return registry;
}

std::string describe_process(const void *data) {
    const process_resource *process =
        static_cast<const process_resource *>(data);
    if (!process || !process->process_initialized)
        return "<process>";
    return "<process: " + std::to_string((long long)process->process.pid) +
           (process->exited ? " exited>" : ">");
}

std::string describe_pipe(const void *data) {
    const process_pipe *pipe = static_cast<const process_pipe *>(data);
    if (!pipe || !pipe->open)
        return "<process pipe closed>";
    return pipe->kind == 0 ? "<process stdin>"
                           : (pipe->kind == 1 ? "<process stdout>"
                                              : "<process stderr>");
}

const native_type kProcessType = {
    "process", describe_process, nullptr,
};

const native_type kProcessPipeType = {
    "process_pipe", describe_pipe, nullptr,
};

void close_pipe(process_pipe *pipe) {
    if (!pipe || !pipe->open)
        return;
    pipe->open = false;
    pipe->reading = false;
    pipe->writing = false;
    if (pipe->pending_read) {
        pipe_async_op *op = pipe->pending_read;
        pipe->pending_read = nullptr;
        op->pipe = nullptr;
        skein_async::async_fail(op, "read_async: pipe closed");
    }
    if (pipe->pending_write) {
        pipe_async_op *op = pipe->pending_write;
        pipe->pending_write = nullptr;
        op->pipe = nullptr;
        skein_async::async_fail(op, "write_async: pipe closed");
    }
    if (!uv_is_closing((uv_handle_t *)&pipe->handle))
        uv_close((uv_handle_t *)&pipe->handle, nullptr);
}

void close_process_handles(process_resource *process) {
    if (!process)
        return;
    close_pipe(process->stdin_pipe);
    close_pipe(process->stdout_pipe);
    close_pipe(process->stderr_pipe);
    if (process->process_initialized &&
        !uv_is_closing((uv_handle_t *)&process->process)) {
        uv_close((uv_handle_t *)&process->process, nullptr);
    }
}

void on_process_exit(uv_process_t *handle, int64_t exit_status,
                     int term_signal) {
    process_resource *process =
        static_cast<process_resource *>(handle->data);
    if (!process)
        return;
    process->exited = true;
    process->exit_status = (long long)exit_status;
    process->term_signal = term_signal;
}

void on_pipe_alloc(uv_handle_t *handle, std::size_t suggested_size,
                   uv_buf_t *buffer) {
    process_pipe *pipe = static_cast<process_pipe *>(handle->data);
    if (!pipe || !pipe->reading) {
        buffer->base = nullptr;
        buffer->len = 0;
        return;
    }
    std::size_t size = suggested_size;
    if (pipe->read_requested > 0 && size > pipe->read_requested)
        size = pipe->read_requested;
    if (size == 0)
        size = 1;
    pipe->read_buffer.resize(size);
    buffer->base = pipe->read_buffer.data();
    buffer->len = pipe->read_buffer.size();
}

void on_pipe_read(uv_stream_t *stream, ssize_t nread,
                  const uv_buf_t *buffer) {
    (void)buffer;
    process_pipe *pipe = static_cast<process_pipe *>(stream->data);
    if (!pipe || !pipe->reading)
        return;
    if (nread == 0)
        return;
    pipe_async_op *op = pipe->pending_read;
    if (op) {
        if (nread > 0) {
            std::size_t size = (std::size_t)nread;
            if (op->requested > 0 && size > op->requested)
                size = op->requested;
            op->result.assign(pipe->read_buffer.data(), size);
            op->status = 0;
        } else if (nread == UV_EOF) {
            op->result.clear();
            op->status = 0;
            op->eof = true;
        } else {
            op->result.clear();
            op->status = (int)nread;
        }
        pipe->reading = false;
        pipe->pending_read = nullptr;
        uv_read_stop(stream);
        op->pipe = nullptr;
        if (op->status < 0) {
            skein_async::async_fail(
                op, std::string("read_async: ") +
                        uv_strerror(op->status));
        } else if (op->eof && op->result.empty()) {
            skein_async::async_complete(op, newnull());
        } else {
            skein_async::async_complete(
                op, newbytesval(std::move(op->result)));
        }
        return;
    }

    if (nread > 0) {
        std::size_t size = (std::size_t)nread;
        if (pipe->read_requested > 0 && size > pipe->read_requested)
            size = pipe->read_requested;
        pipe->read_result.assign(pipe->read_buffer.data(), size);
        pipe->read_status = 0;
    } else if (nread == UV_EOF) {
        pipe->read_result.clear();
        pipe->read_status = 0;
        pipe->read_eof = true;
    } else {
        pipe->read_result.clear();
        pipe->read_status = (int)nread;
    }
    pipe->reading = false;
    uv_read_stop(stream);
    pipe->read_done = true;
}

void on_pipe_write(uv_write_t *request, int status) {
    pipe_write_context *context =
        static_cast<pipe_write_context *>(request->data);
    if (!context)
        return;
    process_pipe *pipe = context->pipe;
    pipe_async_op *op = context->op;
    delete context;
    if (!pipe)
        return;

    if (op) {
        pipe->pending_write = nullptr;
        pipe->writing = false;
        op->pipe = nullptr;
        const bool finished = op->finished;
        if (finished) {
            delete op;
            return;
        }
        if (status < 0) {
            skein_async::async_fail(
                op, std::string("write_async: ") + uv_strerror(status));
        } else {
            skein_async::async_complete(
                op, newintval((long long)op->write_data.size()));
        }
        return;
    }

    pipe->write_status = status;
    pipe->writing = false;
    pipe->write_done = true;
}

bool pipe_read(process_pipe *pipe, std::size_t max_bytes,
               long long timeout_ms, std::string &output, bool &eof,
               std::string &error) {
    if (!pipe || !pipe->open) {
        error = "process pipe is closed";
        return false;
    }
    if (pipe->reading) {
        error = "process pipe already has a pending read";
        return false;
    }
    if (pipe->read_eof) {
        output.clear();
        eof = true;
        return true;
    }
    if (max_bytes == 0) {
        output.clear();
        eof = false;
        return true;
    }
    pipe->read_requested = max_bytes;
    pipe->read_done = false;
    pipe->read_result.clear();
    pipe->read_buffer.clear();
    pipe->reading = true;
    const int rc = uv_read_start((uv_stream_t *)&pipe->handle, on_pipe_alloc,
                                 on_pipe_read);
    if (rc < 0) {
        pipe->reading = false;
        error = uv_strerror(rc);
        return false;
    }
    event_timeout timeout = make_event_timeout(timeout_ms);
    const event_wait_result result = event_wait(
        pipe->read_done,
        [pipe] {
            uv_read_stop((uv_stream_t *)&pipe->handle);
            pipe->reading = false;
            pipe->read_done = true;
        },
        timeout);
    pipe->reading = false;
    if (result == EVENT_WAIT_TIMEOUT) {
        error = "read timed out";
        return false;
    }
    if (result != EVENT_WAIT_OK)
        return false;
    if (pipe->read_status < 0) {
        error = uv_strerror(pipe->read_status);
        return false;
    }
    output = std::move(pipe->read_result);
    eof = pipe->read_eof && output.empty();
    return true;
}

bool pipe_write(process_pipe *pipe, const std::string &data,
                long long timeout_ms, std::size_t &written,
                std::string &error) {
    if (!pipe || !pipe->open) {
        error = "process pipe is closed";
        return false;
    }
    if (pipe->writing) {
        error = "process pipe already has a pending write";
        return false;
    }
    if (data.empty()) {
        written = 0;
        return true;
    }
    pipe->write_data = data;
    pipe->write_done = false;
    pipe->write_status = 0;
    pipe->writing = true;
    uv_buf_t buffer = uv_buf_init(pipe->write_data.data(),
                                  (unsigned int)pipe->write_data.size());
    pipe_write_context *context = new pipe_write_context;
    context->pipe = pipe;
    context->op = nullptr;
    pipe->write_request.data = context;
    const int rc = uv_write(&pipe->write_request,
                            (uv_stream_t *)&pipe->handle, &buffer, 1,
                            on_pipe_write);
    if (rc < 0) {
        delete context;
        pipe->writing = false;
        error = uv_strerror(rc);
        return false;
    }
    event_timeout timeout = make_event_timeout(timeout_ms);
    const event_wait_result result = event_wait(
        pipe->write_done,
        [pipe] {
            uv_cancel((uv_req_t *)&pipe->write_request);
            pipe->writing = false;
            pipe->write_done = true;
        },
        timeout);
    pipe->writing = false;
    if (result == EVENT_WAIT_TIMEOUT) {
        error = "write timed out";
        return false;
    }
    if (result != EVENT_WAIT_OK)
        return false;
    if (pipe->write_status < 0) {
        error = uv_strerror(pipe->write_status);
        return false;
    }
    written = data.size();
    return true;
}

value_t *pipe_read_async(process_pipe *pipe,
                         const std::shared_ptr<process_resource> &process,
                         std::size_t max_bytes, long long timeout_ms,
                         std::string &error) {
    if (!pipe || !pipe->open) {
        error = "process pipe is closed";
        return nullptr;
    }
    if (pipe->reading || pipe->pending_read) {
        error = "process pipe already has a pending read";
        return nullptr;
    }
    pipe_async_op *op = new pipe_async_op;
    task_ptr task;
    if (!skein_async::prepare_async(
            op, task, timeout_ms,
            skein_async::async_timeout_error("read_async", timeout_ms))) {
        delete op;
        error = "read_async: cannot initialize event loop";
        return nullptr;
    }
    op->process = process;
    op->pipe = pipe;
    op->requested = max_bytes;
    op->callback_after_cancel = false;
    op->cancel_operation = [op] {
        if (op->pipe) {
            uv_read_stop((uv_stream_t *)&op->pipe->handle);
            op->pipe->reading = false;
            op->pipe->pending_read = nullptr;
            op->pipe = nullptr;
        }
    };
    pipe->pending_read = op;
    pipe->reading = true;
    pipe->read_requested = max_bytes;
    pipe->read_buffer.clear();
    const int rc = uv_read_start((uv_stream_t *)&pipe->handle, on_pipe_alloc,
                                 on_pipe_read);
    if (rc < 0) {
        pipe->pending_read = nullptr;
        pipe->reading = false;
        op->pipe = nullptr;
        skein_async::async_fail(op, std::string("read_async: ") +
                                       uv_strerror(rc));
    }
    return new_task_value(std::move(task));
}

value_t *pipe_write_async(process_pipe *pipe,
                          const std::shared_ptr<process_resource> &process,
                          const std::string &bytes, long long timeout_ms,
                          std::string &error) {
    if (!pipe || !pipe->open) {
        error = "process pipe is closed";
        return nullptr;
    }
    if (pipe->writing || pipe->pending_write) {
        error = "process pipe already has a pending write";
        return nullptr;
    }
    if (bytes.empty()) {
        task_ptr task = task_create();
        task_complete(task, newintval(0));
        return new_task_value(std::move(task));
    }
    pipe_async_op *op = new pipe_async_op;
    task_ptr task;
    if (!skein_async::prepare_async(
            op, task, timeout_ms,
            skein_async::async_timeout_error("write_async", timeout_ms))) {
        delete op;
        error = "write_async: cannot initialize event loop";
        return nullptr;
    }
    op->process = process;
    op->pipe = pipe;
    op->is_write = true;
    op->write_data = bytes;
    op->callback_after_cancel = true;
    op->cancel_operation = [op] {
        if (op->pipe) {
            uv_cancel((uv_req_t *)&op->write_request);
            op->pipe->writing = false;
            op->pipe->pending_write = nullptr;
            op->pipe = nullptr;
        }
    };
    pipe->pending_write = op;
    pipe->writing = true;
    uv_buf_t buffer = uv_buf_init(op->write_data.data(),
                                  (unsigned int)op->write_data.size());
    pipe_write_context *context = new pipe_write_context;
    context->pipe = pipe;
    context->op = op;
    op->write_request.data = context;
    const int rc = uv_write(&op->write_request, (uv_stream_t *)&pipe->handle,
                            &buffer, 1, on_pipe_write);
    if (rc < 0) {
        delete context;
        pipe->pending_write = nullptr;
        pipe->writing = false;
        op->pipe = nullptr;
        skein_async::async_fail(op, std::string("write_async: ") +
                                       uv_strerror(rc));
    }
    return new_task_value(std::move(task));
}

bool pipe_stream_read(void *data, std::size_t max_bytes,
                      long long timeout_ms, std::string &out, bool &eof,
                      std::string &error) {
    pipe_owner *owner = static_cast<pipe_owner *>(data);
    return pipe_read(owner ? owner->pipe : nullptr, max_bytes, timeout_ms,
                     out, eof, error);
}

bool pipe_stream_write(void *data, const std::string &bytes,
                       long long timeout_ms, std::size_t &written,
                       std::string &error) {
    pipe_owner *owner = static_cast<pipe_owner *>(data);
    return pipe_write(owner ? owner->pipe : nullptr, bytes, timeout_ms,
                      written, error);
}

bool pipe_stream_flush(void *data, std::string &error) {
    (void)data;
    error.clear();
    return true;
}

bool pipe_stream_close(void *data, bool &was_open, std::string &error) {
    pipe_owner *owner = static_cast<pipe_owner *>(data);
    process_pipe *pipe = owner ? owner->pipe : nullptr;
    was_open = pipe && pipe->open;
    close_pipe(pipe);
    error.clear();
    return true;
}

bool pipe_stream_eof(void *data, bool &eof, std::string &error) {
    pipe_owner *owner = static_cast<pipe_owner *>(data);
    process_pipe *pipe = owner ? owner->pipe : nullptr;
    if (!pipe || !pipe->open) {
        error = "process pipe is closed";
        return false;
    }
    eof = pipe->read_eof;
    return true;
}

bool pipe_stream_seekable(void *data) {
    (void)data;
    return false;
}

bool pipe_stream_seek(void *data, long long offset, int whence,
                      long long &position, std::string &error) {
    (void)data;
    (void)offset;
    (void)whence;
    (void)position;
    error = "process pipe is not seekable";
    return false;
}

bool pipe_stream_tell(void *data, long long &position, std::string &error) {
    (void)data;
    (void)position;
    error = "process pipe is not seekable";
    return false;
}

value_t *pipe_stream_read_async(void *data,
                                const std::shared_ptr<void> &owner,
                                std::size_t max_bytes, long long timeout_ms,
                                std::string &error) {
    (void)owner;
    pipe_owner *pipe_owner_value = static_cast<pipe_owner *>(data);
    if (!pipe_owner_value)
        return nullptr;
    return pipe_read_async(pipe_owner_value->pipe,
                           pipe_owner_value->process, max_bytes, timeout_ms,
                           error);
}

value_t *pipe_stream_write_async(void *data,
                                 const std::shared_ptr<void> &owner,
                                 const std::string &bytes,
                                 long long timeout_ms,
                                 std::string &error) {
    (void)owner;
    pipe_owner *pipe_owner_value = static_cast<pipe_owner *>(data);
    if (!pipe_owner_value)
        return nullptr;
    return pipe_write_async(pipe_owner_value->pipe,
                            pipe_owner_value->process, bytes, timeout_ms,
                            error);
}

const stream_ops kPipeStreamOps = {
    pipe_stream_read,
    pipe_stream_write,
    pipe_stream_flush,
    pipe_stream_close,
    pipe_stream_eof,
    pipe_stream_seekable,
    pipe_stream_seek,
    pipe_stream_tell,
    pipe_stream_read_async,
    pipe_stream_write_async,
};

const native_type kProcessPipeStreamType = {
    "process_pipe", describe_pipe, &kPipeStreamOps,
};

value_t *new_process_value(std::shared_ptr<process_resource> process) {
    userdata_value_t *value = new userdata_value_t;
    value->type = T_userdata;
    value->meta = &kProcessType;
    value->data = std::move(process);
    return (value_t *)value;
}

value_t *new_pipe_value(std::shared_ptr<process_resource> process,
                        process_pipe *pipe) {
    userdata_value_t *value = new userdata_value_t;
    value->type = T_userdata;
    value->meta = &kProcessPipeStreamType;
    value->data = std::make_shared<pipe_owner>(
        pipe_owner{std::move(process), pipe});
    return (value_t *)value;
}

const char *process_name(int functype) {
    switch (functype) {
        case B_process_spawn_command: return "std.process.spawn_command";
        case B_process_spawn_entry: return "std.process.spawn_entry";
        case B_process_run_entry: return "std.process.run_entry";
        case B_process_spawn_function: return "std.process.spawn_function";
        case B_process_run_function: return "std.process.run_function";
        case B_process_result: return "std.process.result";
        case B_process_wait: return "std.process.wait";
        case B_process_kill: return "std.process.kill";
        case B_process_close: return "std.process.close";
        case B_process_pid: return "std.process.pid";
        default: return "std.process";
    }
}

bool get_process(const value_t *value, std::shared_ptr<process_resource> &out) {
    if (!value)
        return false;
    if (value->type == T_userdata) {
        const userdata_value_t *userdata = (const userdata_value_t *)value;
        if (userdata->meta == &kProcessType) {
            out = std::static_pointer_cast<process_resource>(userdata->data);
            return true;
        }
    }
    if (value->type == T_dict) {
        const dict_value_t *dict = (const dict_value_t *)value;
        for (const auto &entry : dict->payload->items) {
            if (entry.first->type == T_string &&
                std::strcmp(((const string_value_t *)entry.first)->value,
                            "handle") == 0) {
                return get_process(entry.second, out);
            }
        }
    }
    return false;
}

void append_u32(std::string &out, uint32_t value) {
    out.push_back((char)(value & 0xFF));
    out.push_back((char)((value >> 8) & 0xFF));
    out.push_back((char)((value >> 16) & 0xFF));
    out.push_back((char)((value >> 24) & 0xFF));
}

void free_value_list(std::vector<value_t *> &values) {
    for (value_t *value : values)
        freeval(value);
    values.clear();
}

value_t *process_dict_get(value_t *dict, const char *key) {
    if (!dict || dict->type != T_dict)
        return nullptr;
    for (const auto &entry :
         ((dict_value_t *)dict)->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((string_value_t *)entry.first)->value, key) == 0)
            return entry.second;
    }
    return nullptr;
}

void process_dict_set(value_t *dict, const char *key, value_t *value) {
    if (!dict || dict->type != T_dict)
        return;
    dict_append_item((dict_value_t *)dict, newstrval(key), value);
}

long long process_option_timeout(value_t *options) {
    if (!options || options->type != T_dict)
        return 0;
    value_t *timeout = process_dict_get(options, "timeout_ms");
    if (!timeout || timeout->type != T_int)
        return 0;
    return ((int_value_t *)timeout)->value;
}

value_t *spawn_entry_process(const value_t *entry, const value_t *message,
                               value_t *options, std::string &error) {
    if (!entry || entry->type != T_bytes) {
        error = "expected encoded entry bytes";
        return nullptr;
    }
    value_t *payload = thread_message_payload((value_t *)message);
    if (!payload) {
        error = "expected a Message";
        return nullptr;
    }
    std::string payload_bytes;
    if (!skein_vm_serialize_message_payload(payload, payload_bytes, error)) {
        freeval(payload);
        return nullptr;
    }
    freeval(payload);

    const std::string &entry_bytes =
        ((const bytes_value_t *)entry)->value.read();
    std::string request;
    append_u32(request, 0x52504B53u); /* SKPR */
    append_u32(request, 1u);
    append_u32(request, (uint32_t)entry_bytes.size());
    request += entry_bytes;
    append_u32(request, (uint32_t)payload_bytes.size());
    request += payload_bytes;

    value_t *spawn_options = (value_t *)newdictval();
    value_t *arguments = (value_t *)newlistval();
    ((list_value_t *)arguments)->payload->items.push_back(
        newstrval("--run-entry"));
    process_dict_set(spawn_options, "args", arguments);
    bool has_stderr = false;
    if (options && options->type == T_dict) {
        const char *keys[] = {"env", "cwd", "stderr"};
        for (const char *key : keys) {
            value_t *value = process_dict_get(options, key);
            if (value) {
                if (std::strcmp(key, "stderr") == 0)
                    has_stderr = true;
                process_dict_set(spawn_options, key, dupval(value));
            }
        }
    }
    if (!has_stderr)
        process_dict_set(spawn_options, "stderr", newstrval("inherit"));
    std::string executable = platform_executable_path();
    if (executable.empty()) {
        freeval(spawn_options);
        error = "cannot locate the interpreter executable";
        return nullptr;
    }
    value_t *file = newstrval(executable.c_str());
    std::vector<value_t *> spawn_args{file, spawn_options};
    value_t *process = call_process_spawn_builtin(B_process_spawn_command, spawn_args);
    free_value_list(spawn_args);
    if (!process) {
        error = "cannot spawn encoded process";
        return nullptr;
    }

    value_t *stdin_pipe = process_dict_get(process, "stdin");
    if (!stdin_pipe) {
        value_t *kill_args[] = {dupval(process)};
        std::vector<value_t *> kill{kill_args[0]};
        call_process_spawn_builtin(B_process_kill, kill);
        free_value_list(kill);
        freeval(process);
        error = "encoded process has no stdin pipe";
        return nullptr;
    }
    std::vector<value_t *> write_args{dupval(stdin_pipe),
                                     newbytesval(request), newintval(0)};
    value_t *written = call_io_builtin(B_io_write_all, write_args);
    free_value_list(write_args);
    if (!written) {
        std::vector<value_t *> kill{dupval(process)};
        call_process_spawn_builtin(B_process_kill, kill);
        free_value_list(kill);
        freeval(process);
        error = "cannot write encoded process request";
        return nullptr;
    }
    freeval(written);
    std::vector<value_t *> close_args{dupval(stdin_pipe)};
    value_t *closed = call_io_builtin(B_io_close, close_args);
    free_value_list(close_args);
    freeval(closed);
    return process;
}

value_t *read_process_encoded_result(value_t *process, long long timeout_ms) {
    value_t *stdout_pipe = process_dict_get(process, "stdout");
    if (!stdout_pipe)
        return newerrorval("process", "process has no stdout pipe");
    std::vector<value_t *> read_args{dupval(stdout_pipe),
                                     newintval(64 * 1024 * 1024)};
    value_t *data = call_io_builtin(B_io_read_all, read_args);
    free_value_list(read_args);
    std::vector<value_t *> wait_args{dupval(process)};
    if (timeout_ms > 0)
        wait_args.push_back(newintval(timeout_ms));
    value_t *status = call_process_spawn_builtin(B_process_wait, wait_args);
    free_value_list(wait_args);
    if (!data || data->type != T_bytes) {
        freeval(data);
        freeval(status);
        return newerrorval("process",
                           "process did not return an encoded result");
    }
    std::string bytes = ((bytes_value_t *)data)->value.read();
    freeval(data);
    freeval(status);
    value_t *envelope = nullptr;
    std::string error;
    if (!skein_vm_deserialize_message_payload(bytes, envelope, error))
        return newerrorval("process", error.c_str());
    return envelope;
}

}  // namespace

bool process_try_wait(struct value_t *value, long long &status) {
    std::shared_ptr<process_resource> process;
    if (!get_process(value, process) || !process)
        return false;
    if (!process->exited && event_loop_initialized())
        uv_run(event_loop_handle(), UV_RUN_NOWAIT);
    if (!process->exited)
        return false;
    status = process->term_signal != 0
                 ? 128 + (long long)process->term_signal
                 : process->exit_status;
    return true;
}

value_t *call_process_spawn_builtin(
    int functype, const std::vector<value_t *> &args) {
    const char *name = process_name(functype);
    if (functype == B_process_spawn_command) {
        if (args.empty() || args.size() > 2) {
            yyerror("%s expects 1 or 2 arguments, got %d", name,
                    (int)args.size());
            return NULL;
        }
        if (args[0]->type != T_string) {
            yyerror("%s expects a file string, got %s", name,
                    typename_of(args[0]));
            return NULL;
        }
        std::vector<std::string> arguments;
        std::vector<std::string> environment;
        std::vector<std::pair<std::string, std::string>> env_overrides;
        std::string cwd;
        bool inherit_stderr = false;
        if (args.size() == 2) {
            if (args[1]->type != T_dict) {
                yyerror("%s options must be a dict", name);
                return NULL;
            }
            dict_value_t *options = (dict_value_t *)args[1];
            for (const auto &entry : options->payload->items) {
                if (entry.first->type != T_string)
                    continue;
                const std::string key =
                    ((string_value_t *)entry.first)->value;
                if (key == "args") {
                    if (entry.second->type != T_list) {
                        yyerror("%s args must be a list", name);
                        return NULL;
                    }
                    for (value_t *item :
                         ((list_value_t *)entry.second)->payload->items) {
                        if (item->type != T_string) {
                            yyerror("%s args must contain strings", name);
                            return NULL;
                        }
                        arguments.push_back(
                            ((string_value_t *)item)->value);
                    }
                } else if (key == "env") {
                    if (entry.second->type != T_dict) {
                        yyerror("%s env must be a dict", name);
                        return NULL;
                    }
                    for (const auto &env_entry :
                         ((dict_value_t *)entry.second)->payload->items) {
                        if (env_entry.first->type != T_string ||
                            env_entry.second->type != T_string) {
                            yyerror("%s env keys and values must be strings",
                                    name);
                            return NULL;
                        }
                        env_overrides.emplace_back(
                            ((string_value_t *)env_entry.first)->value,
                            ((string_value_t *)env_entry.second)->value);
                    }
                } else if (key == "cwd") {
                    if (entry.second->type != T_string) {
                        yyerror("%s cwd must be a string", name);
                        return NULL;
                    }
                    cwd = ((string_value_t *)entry.second)->value;
                } else if (key == "stderr") {
                    if (entry.second->type != T_string) {
                        yyerror("%s stderr must be \"pipe\" or \"inherit\"",
                                name);
                        return NULL;
                    }
                    const std::string value =
                        ((string_value_t *)entry.second)->value;
                    if (value == "inherit") {
                        inherit_stderr = true;
                    } else if (value != "pipe") {
                        yyerror("%s stderr must be \"pipe\" or \"inherit\"",
                                name);
                        return NULL;
                    }
                }
            }
        }
        {
            std::vector<std::pair<std::string, std::string>> merged;
            thread_env_all(merged);
            for (const auto &override : env_overrides) {
                bool replaced = false;
                for (auto &entry : merged) {
                    if (entry.first == override.first) {
                        entry.second = override.second;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                    merged.push_back(override);
            }
            environment.reserve(merged.size());
            for (const auto &entry : merged)
                environment.push_back(entry.first + "=" + entry.second);
        }
        if (cwd.empty())
            cwd = thread_cwd();
        else
            cwd = thread_resolve_path(cwd);
        if (!event_loop_init()) {
            yyerror("%s: cannot initialize event loop", name);
            return NULL;
        }

        std::shared_ptr<process_resource> process =
            std::make_shared<process_resource>();
        process->process.data = process.get();
        process->stdin_pipe = new process_pipe;
        process->stdout_pipe = new process_pipe;
        process->stderr_pipe = inherit_stderr ? nullptr : new process_pipe;
        process->stdin_pipe->process = process.get();
        process->stdout_pipe->process = process.get();
        process->stdin_pipe->kind = 0;
        process->stdout_pipe->kind = 1;
        if (process->stderr_pipe) {
            process->stderr_pipe->process = process.get();
            process->stderr_pipe->kind = 2;
        }

        const std::string file = ((string_value_t *)args[0])->value;
        arguments.insert(arguments.begin(), file);
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for (std::string &argument : arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        std::vector<char *> envp;
        for (std::string &entry : environment)
            envp.push_back(entry.data());
        envp.push_back(nullptr);

        uv_pipe_t *pipes[3] = {&process->stdin_pipe->handle,
                               &process->stdout_pipe->handle,
                               process->stderr_pipe
                                   ? &process->stderr_pipe->handle
                                   : nullptr};
        for (int i = 0; i < 3; ++i) {
            if (!pipes[i])
                continue;
            if (uv_pipe_init(event_loop_handle(), pipes[i], 0) != 0) {
                close_process_handles(process.get());
                yyerror("%s: cannot create pipe", name);
                return NULL;
            }
        }
        process->stdin_pipe->handle.data = process->stdin_pipe;
        process->stdout_pipe->handle.data = process->stdout_pipe;
        if (process->stderr_pipe)
            process->stderr_pipe->handle.data = process->stderr_pipe;

        uv_stdio_container_t stdio[3];
        stdio[0].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_READABLE_PIPE);
        stdio[0].data.stream = (uv_stream_t *)&process->stdin_pipe->handle;
        stdio[1].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
        stdio[1].data.stream = (uv_stream_t *)&process->stdout_pipe->handle;
        if (inherit_stderr) {
            stdio[2].flags = UV_INHERIT_FD;
            stdio[2].data.fd = 2;
        } else {
            stdio[2].flags =
                (uv_stdio_flags)(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[2].data.stream =
                (uv_stream_t *)&process->stderr_pipe->handle;
        }

        uv_process_options_t options = {};
        options.exit_cb = on_process_exit;
        options.file = file.c_str();
        options.args = argv.data();
        options.env = environment.empty() ? nullptr : envp.data();
        options.cwd = cwd.empty() ? nullptr : cwd.c_str();
        options.stdio_count = 3;
        options.stdio = stdio;

        const int rc = uv_spawn(event_loop_handle(), &process->process,
                                &options);
        if (rc != 0) {
            close_process_handles(process.get());
            yyerror("%s: %s", name, uv_strerror(rc));
            return NULL;
        }
        process->process_initialized = true;
        process_registry().push_back(process);

        dict_value_t *result = newdictval();
        result->payload->items.push_back(std::make_pair(
            newstrval("pid"),
            newintval((long long)process->process.pid)));
        result->payload->items.push_back(std::make_pair(
            newstrval("stdin"),
            new_pipe_value(process, process->stdin_pipe)));
        result->payload->items.push_back(std::make_pair(
            newstrval("stdout"),
            new_pipe_value(process, process->stdout_pipe)));
        result->payload->items.push_back(std::make_pair(
            newstrval("stderr"),
            process->stderr_pipe
                ? new_pipe_value(process, process->stderr_pipe)
                : newnull()));
        result->payload->items.push_back(std::make_pair(
            newstrval("handle"), new_process_value(process)));
        return (value_t *)result;
    }

    if (functype == B_process_spawn_entry ||
        functype == B_process_run_entry) {
        if (args.size() < 2 || args.size() > 3 ||
            args[0]->type != T_bytes || args[1]->type != T_userdata) {
            yyerror("%s expects encoded bytes, Message, and optional options",
                    name);
            return NULL;
        }
        value_t *options = args.size() == 3 ? args[2] : nullptr;
        if (options && options->type != T_dict) {
            yyerror("%s options must be a dict", name);
            return NULL;
        }
        std::string error;
        value_t *process = spawn_entry_process(args[0], args[1], options,
                                                 error);
        if (!process) {
            yyerror("%s: %s", name, error.c_str());
            return NULL;
        }
        if (functype == B_process_spawn_entry)
            return process;
        value_t *envelope = read_process_encoded_result(
            process, process_option_timeout(options));
        freeval(process);
        return envelope;
    }

    if (functype == B_process_run_function ||
        functype == B_process_spawn_function) {
        if (args.size() < 2 || args.size() > 3 ||
            args[1]->type != T_userdata) {
            yyerror("%s expects a function, Message, and optional options",
                    name);
            return NULL;
        }
        std::string bytes;
        std::string error;
        if (!thread_entry_encode(args[0], bytes, error)) {
            yyerror("%s: %s", name, error.c_str());
            return NULL;
        }
        value_t *blob = newbytesval(std::move(bytes));
        std::vector<value_t *> entry_args{blob, dupval(args[1])};
        if (args.size() == 3) {
            if (args[2]->type != T_dict) {
                yyerror("%s options must be a dict", name);
                free_value_list(entry_args);
                return NULL;
            }
            entry_args.push_back(dupval(args[2]));
        }
        value_t *result = call_process_spawn_builtin(
            functype == B_process_run_function ? B_process_run_entry
                                               : B_process_spawn_entry,
            entry_args);
        free_value_list(entry_args);
        return result;
    }

    if (functype == B_process_result) {
        if (args.empty() || args.size() > 2) {
            yyerror("%s expects a process and optional timeout", name);
            return NULL;
        }
        long long timeout_ms = 0;
        if (args.size() == 2) {
            if (args[1]->type != T_int ||
                ((int_value_t *)args[1])->value < 0) {
                yyerror("%s timeout must be a non-negative integer", name);
                return NULL;
            }
            timeout_ms = ((int_value_t *)args[1])->value;
        }
        return read_process_encoded_result(args[0], timeout_ms);
    }

    if (functype == B_process_wait) {
        if (args.empty() || args.size() > 2) {
            yyerror("%s expects 1 or 2 arguments, got %d", name,
                    (int)args.size());
            return NULL;
        }
        std::shared_ptr<process_resource> process;
        if (!get_process(args[0], process) || !process) {
            yyerror("%s expects a process handle", name);
            return NULL;
        }
        long long timeout_ms = 0;
        if (args.size() == 2) {
            if (args[1]->type != T_int ||
                ((int_value_t *)args[1])->value < 0) {
                yyerror("%s timeout must be a non-negative integer", name);
                return NULL;
            }
            timeout_ms = ((int_value_t *)args[1])->value;
        }
        event_timeout timeout = make_event_timeout(timeout_ms);
        const event_wait_result result = event_wait(
            process->exited,
            [process] {
                if (process->process_initialized && !process->exited)
                    uv_process_kill(&process->process, SIGTERM);
            },
            timeout);
        if (result == EVENT_WAIT_TIMEOUT) {
            yyerror("%s timed out", name);
            return NULL;
        }
        if (result != EVENT_WAIT_OK)
            return NULL;
        if (process->term_signal != 0)
            return newintval(128 + process->term_signal);
        return newintval(process->exit_status);
    }

    if (functype == B_process_kill) {
        if (args.empty() || args.size() > 2) {
            yyerror("%s expects 1 or 2 arguments, got %d", name,
                    (int)args.size());
            return NULL;
        }
        std::shared_ptr<process_resource> process;
        if (!get_process(args[0], process) || !process) {
            yyerror("%s expects a process handle", name);
            return NULL;
        }
        int signal = SIGTERM;
        if (args.size() == 2) {
            if (args[1]->type != T_int) {
                yyerror("%s signal must be an integer", name);
                return NULL;
            }
            signal = (int)((int_value_t *)args[1])->value;
        }
        if (!process->process_initialized || process->exited) {
            return newnull();
        }
        uv_process_kill(&process->process, signal);
        return newnull();
    }

    if (functype == B_process_close) {
        if (args.size() != 1) {
            yyerror("%s expects 1 argument, got %d", name,
                    (int)args.size());
            return NULL;
        }
        std::shared_ptr<process_resource> process;
        if (!get_process(args[0], process) || !process) {
            yyerror("%s expects a process handle", name);
            return NULL;
        }
        close_process_handles(process.get());
        return newnull();
    }

    if (functype == B_process_pid) {
        if (args.size() != 1) {
            yyerror("%s expects 1 argument, got %d", name,
                    (int)args.size());
            return NULL;
        }
        std::shared_ptr<process_resource> process;
        if (!get_process(args[0], process) || !process ||
            !process->process_initialized) {
            yyerror("%s expects a process handle", name);
            return NULL;
        }
        return newintval((long long)process->process.pid);
    }

    yyerror("unknown process function");
    return NULL;
}

void process_spawn_shutdown() {
    auto &registry = process_registry();
    if (registry.empty())
        return;
    for (const auto &process : registry) {
        if (process->process_initialized && !process->exited)
            uv_process_kill(&process->process, SIGTERM);
        close_process_handles(process.get());
    }
    // Keep resources alive until the event loop is shut down; uv_close()
    // callbacks may still reference the handles.
}

void process_spawn_finish() {
    process_registry().clear();
}
