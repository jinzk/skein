#include "filetype.h"

#include "async_support.h"
#include "event_loop.h"
#include "task.h"

#include <uv.h>

#include <limits>
#include <memory>
#include <string>
#include <utility>

static std::string describe_file(const void *data) {
    const file_resource *resource = static_cast<const file_resource *>(data);
    if (!resource)
        return "<file>";
    std::string text = "<file: " + resource->path;
    if (!platform_file_is_open(resource->file))
        text += " (closed)";
    text += ">";
    return text;
}

static bool file_stream_read(void *data, std::size_t max_bytes,
                             long long timeout_ms, std::string &out,
                             bool &eof, std::string &error) {
    (void)timeout_ms;
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        error = "file is closed";
        return false;
    }
    if (resource->async_pending) {
        error = "file has a pending async operation";
        return false;
    }
    if (!platform_file_read(resource->file, max_bytes, out, error))
        return false;
    resource->eof = out.empty() && platform_file_eof(resource->file);
    eof = resource->eof;
    return true;
}

static bool file_stream_write(void *data, const std::string &bytes,
                              long long timeout_ms, std::size_t &written,
                              std::string &error) {
    (void)timeout_ms;
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        error = "file is closed";
        return false;
    }
    if (resource->async_pending) {
        error = "file has a pending async operation";
        return false;
    }
    if (!platform_file_write(resource->file, bytes, error))
        return false;
    resource->eof = false;
    written = bytes.size();
    return true;
}

static bool file_stream_flush(void *data, std::string &error) {
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        error = "file is closed";
        return false;
    }
    if (resource->async_pending) {
        error = "file has a pending async operation";
        return false;
    }
    return platform_file_flush(resource->file, error);
}

static bool file_stream_close(void *data, bool &was_open,
                              std::string &error) {
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        was_open = false;
        return true;
    }
    if (resource->async_pending) {
        error = "file has a pending async operation";
        return false;
    }
    was_open = true;
    platform_file_close(resource->file, error);
    return error.empty();
}

static bool file_stream_eof(void *data, bool &eof, std::string &error) {
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        error = "file is closed";
        return false;
    }
    eof = resource->eof || platform_file_eof(resource->file);
    return true;
}

static bool file_stream_seekable(void *data) {
    (void)data;
    return true;
}

static bool file_stream_seek(void *data, long long offset, int whence,
                             long long &position, std::string &error) {
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        error = "file is closed";
        return false;
    }
    if (resource->async_pending) {
        error = "file has a pending async operation";
        return false;
    }
    if (!platform_file_seek(resource->file, offset, whence, error))
        return false;
    resource->eof = false;
    position = platform_file_tell(resource->file, error);
    return position >= 0;
}

static bool file_stream_tell(void *data, long long &position,
                             std::string &error) {
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        error = "file is closed";
        return false;
    }
    if (resource->async_pending) {
        error = "file has a pending async operation";
        return false;
    }
    position = platform_file_tell(resource->file, error);
    return position >= 0;
}

namespace {

struct file_async_op : skein_async::state {
    uv_fs_t request = {};
    platform_file_ptr file;
    std::shared_ptr<void> owner;
    file_resource *resource = NULL;
    std::string buffer;
};

void finish_file_async(file_async_op *op) {
    if (op->resource)
        op->resource->async_pending = false;
    uv_fs_req_cleanup(&op->request);
}

void on_file_read_async(uv_fs_t *request) {
    file_async_op *op = static_cast<file_async_op *>(request->data);
    if (!op)
        return;
    if (op->finished) {
        finish_file_async(op);
        delete op;
        return;
    }

    ssize_t result = request->result;
    if (result > 0) {
        op->buffer.resize((std::size_t)result);
        if (op->resource)
            op->resource->eof = false;
        finish_file_async(op);
        skein_async::async_complete(op, newbytesval(std::move(op->buffer)));
        return;
    }
    if (result == 0) {
        if (op->resource)
            op->resource->eof = true;
        finish_file_async(op);
        skein_async::async_complete(op, newnull());
        return;
    }
    std::string error =
        std::string("read_async: ") + uv_strerror((int)result);
    finish_file_async(op);
    skein_async::async_fail(op, error);
}

void on_file_write_async(uv_fs_t *request) {
    file_async_op *op = static_cast<file_async_op *>(request->data);
    if (!op)
        return;
    if (op->finished) {
        finish_file_async(op);
        delete op;
        return;
    }

    ssize_t result = request->result;
    if (result >= 0) {
        if (op->resource)
            op->resource->eof = false;
        finish_file_async(op);
        skein_async::async_complete(op, newintval((long long)result));
        return;
    }
    std::string error =
        std::string("write_async: ") + uv_strerror((int)result);
    finish_file_async(op);
    skein_async::async_fail(op, error);
}

value_t *file_stream_read_async(void *data,
                                const std::shared_ptr<void> &owner,
                                std::size_t max_bytes, long long timeout_ms,
                                std::string &error) {
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        error = "file is closed";
        return NULL;
    }
    if (resource->async_pending) {
        error = "file has a pending async operation";
        return NULL;
    }
    if (max_bytes == 0 ||
        max_bytes > (std::size_t)std::numeric_limits<unsigned int>::max()) {
        error = "read_async size is too large";
        return NULL;
    }

    file_async_op *op = new file_async_op;
    task_ptr task;
    if (!skein_async::prepare_async(
            op, task, timeout_ms,
            skein_async::async_timeout_error("read_async", timeout_ms))) {
        delete op;
        error = "read_async: cannot initialize event loop";
        return NULL;
    }
    op->file = resource->file;
    op->owner = owner;
    op->resource = resource;
    op->buffer.resize(max_bytes);
    op->callback_after_cancel = true;
    op->cancel_operation = [op] {
        int rc = uv_cancel((uv_req_t *)&op->request);
        if (rc == 0 && op->resource)
            op->resource->async_pending = false;
    };
    op->request.data = op;
    resource->async_pending = true;

    uv_buf_t buffer = uv_buf_init(
        op->buffer.data(), (unsigned int)op->buffer.size());
    int rc = uv_fs_read(event_loop_handle(), &op->request,
                        platform_file_fd(op->file), &buffer, 1, -1,
                        on_file_read_async);
    if (rc < 0) {
        resource->async_pending = false;
        skein_async::async_fail(op, std::string("read_async: ") +
                                       uv_strerror(rc));
    }
    return new_task_value(std::move(task));
}

value_t *file_stream_write_async(void *data,
                                 const std::shared_ptr<void> &owner,
                                 const std::string &bytes,
                                 long long timeout_ms,
                                 std::string &error) {
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        error = "file is closed";
        return NULL;
    }
    if (resource->async_pending) {
        error = "file has a pending async operation";
        return NULL;
    }
    if (bytes.empty()) {
        task_ptr task = task_create();
        task_complete(task, newintval(0));
        return new_task_value(std::move(task));
    }
    if (bytes.size() >
        (std::size_t)std::numeric_limits<unsigned int>::max()) {
        error = "write_async data is too large";
        return NULL;
    }

    file_async_op *op = new file_async_op;
    task_ptr task;
    if (!skein_async::prepare_async(
            op, task, timeout_ms,
            skein_async::async_timeout_error("write_async", timeout_ms))) {
        delete op;
        error = "write_async: cannot initialize event loop";
        return NULL;
    }
    op->file = resource->file;
    op->owner = owner;
    op->resource = resource;
    op->buffer = bytes;
    op->callback_after_cancel = true;
    op->cancel_operation = [op] {
        int rc = uv_cancel((uv_req_t *)&op->request);
        if (rc == 0 && op->resource)
            op->resource->async_pending = false;
    };
    op->request.data = op;
    resource->async_pending = true;

    uv_buf_t buffer = uv_buf_init(
        op->buffer.data(), (unsigned int)op->buffer.size());
    int rc = uv_fs_write(event_loop_handle(), &op->request,
                         platform_file_fd(op->file), &buffer, 1, -1,
                         on_file_write_async);
    if (rc < 0) {
        resource->async_pending = false;
        skein_async::async_fail(op, std::string("write_async: ") +
                                       uv_strerror(rc));
    }
    return new_task_value(std::move(task));
}

} // namespace

static const struct stream_ops file_stream_ops = {
    file_stream_read,
    file_stream_write,
    file_stream_flush,
    file_stream_close,
    file_stream_eof,
    file_stream_seekable,
    file_stream_seek,
    file_stream_tell,
    file_stream_read_async,
    file_stream_write_async,
};

const struct native_type file_native_type = {
    "file",
    describe_file,
    &file_stream_ops,
};

bool is_file_value(const struct value_t *value) {
    if (!value || value->type != T_userdata)
        return false;
    const userdata_value_t *userdata =
        (const userdata_value_t *)value;
    return userdata->meta == &file_native_type;
}

std::shared_ptr<file_resource> file_value(const struct value_t *value) {
    if (!is_file_value(value))
        return nullptr;
    const userdata_value_t *userdata =
        (const userdata_value_t *)value;
    return std::static_pointer_cast<file_resource>(userdata->data);
}

struct value_t *new_file_value(platform_file_ptr file,
                               const std::string &path) {
    std::shared_ptr<file_resource> resource =
        std::make_shared<file_resource>();
    resource->file = std::move(file);
    resource->path = path;
    return newuserdataval(&file_native_type, resource);
}

std::string normalize_file_mode(const std::string &mode, std::string &error) {
    error.clear();
    if (mode.empty()) {
        error = "invalid file mode";
        return std::string();
    }

    char first = mode[0];
    if (first != 'r' && first != 'w' && first != 'a') {
        error = "invalid file mode";
        return std::string();
    }

    bool plus = false;
    for (size_t i = 1; i < mode.size(); ++i) {
        char c = mode[i];
        if (c == '+') {
            if (plus) {
                error = "invalid file mode";
                return std::string();
            }
            plus = true;
        } else if (c != 'b' && c != 't') {
            error = "invalid file mode";
            return std::string();
        }
    }

    std::string normalized;
    normalized += first;
    if (plus)
        normalized += '+';
    normalized += 'b';
    return normalized;
}
