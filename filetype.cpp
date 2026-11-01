#include "filetype.h"

#include <memory>
#include <string>

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
    if (!platform_file_read(resource->file, max_bytes, out, error))
        return false;
    eof = out.empty() && platform_file_eof(resource->file);
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
    if (!platform_file_write(resource->file, bytes, error))
        return false;
    written = bytes.size();
    return true;
}

static bool file_stream_flush(void *data, std::string &error) {
    file_resource *resource = static_cast<file_resource *>(data);
    if (!resource || !platform_file_is_open(resource->file)) {
        error = "file is closed";
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
    eof = platform_file_eof(resource->file);
    return true;
}

static bool file_stream_seekable(void *data) {
    (void)data;
    return true;
}

static const struct stream_ops file_stream_ops = {
    file_stream_read,
    file_stream_write,
    file_stream_flush,
    file_stream_close,
    file_stream_eof,
    file_stream_seekable,
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
