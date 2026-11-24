#include "stdio_stream.h"

#include "valuetype.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

static std::mutex g_stdio_output_mutex;
static bool g_stdout_line_dirty = false;

std::mutex &stdio_output_mutex() {
    return g_stdio_output_mutex;
}

static void note_stdout_bytes_unlocked(const char *data, std::size_t size) {
    if (size == 0)
        return;
    g_stdout_line_dirty = data[size - 1] != '\n';
}

void note_stdout_bytes_locked(const char *data, std::size_t size) {
    note_stdout_bytes_unlocked(data, size);
}

void note_stdout_bytes(const char *data, std::size_t size) {
    std::lock_guard<std::mutex> lock(stdio_output_mutex());
    note_stdout_bytes_unlocked(data, size);
}

void ensure_stdout_newline() {
    std::lock_guard<std::mutex> lock(stdio_output_mutex());
    if (!g_stdout_line_dirty)
        return;
    std::fputc('\n', stdout);
    std::fflush(stdout);
    g_stdout_line_dirty = false;
}

struct stdio_resource {
    std::FILE *file = nullptr;
    std::string name;
    bool readable = false;
    bool writable = false;
    bool open = true;
    bool eof = false;
};

static std::string describe_stdio(const void *data) {
    const stdio_resource *resource = static_cast<const stdio_resource *>(data);
    if (!resource)
        return "<stdio>";
    return "<" + resource->name + ">";
}

static bool stdio_read(void *data, std::size_t max_bytes,
                       long long timeout_ms, std::string &out, bool &eof,
                       std::string &error) {
    (void)timeout_ms;
    stdio_resource *resource = static_cast<stdio_resource *>(data);
    if (!resource || !resource->open || !resource->readable) {
        error = "stream is not readable";
        return false;
    }
    if (resource->eof) {
        eof = true;
        return true;
    }
    std::string buffer;
    buffer.resize(max_bytes);
    const std::size_t received =
        std::fread(buffer.data(), 1, max_bytes, resource->file);
    if (received > 0) {
        buffer.resize(received);
        out = std::move(buffer);
        eof = false;
        return true;
    }
    if (std::ferror(resource->file)) {
        error = std::strerror(errno);
        return false;
    }
    resource->eof = true;
    eof = true;
    return true;
}

static bool stdio_write(void *data, const std::string &bytes,
                        long long timeout_ms, std::size_t &written,
                        std::string &error) {
    (void)timeout_ms;
    stdio_resource *resource = static_cast<stdio_resource *>(data);
    if (!resource || !resource->open || !resource->writable) {
        error = "stream is not writable";
        return false;
    }
    std::lock_guard<std::mutex> lock(stdio_output_mutex());
    written = std::fwrite(bytes.data(), 1, bytes.size(), resource->file);
    if (written != bytes.size()) {
        error = std::strerror(errno);
        return false;
    }
    if (resource->file == stdout)
        note_stdout_bytes_locked(bytes.data(), written);
    resource->eof = false;
    return true;
}

static bool stdio_flush(void *data, std::string &error) {
    stdio_resource *resource = static_cast<stdio_resource *>(data);
    if (!resource || !resource->open || !resource->writable) {
        error = "stream is not writable";
        return false;
    }
    std::lock_guard<std::mutex> lock(stdio_output_mutex());
    if (std::fflush(resource->file) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

static bool stdio_close(void *data, bool &was_open, std::string &error) {
    stdio_resource *resource = static_cast<stdio_resource *>(data);
    was_open = resource && resource->open;
    if (resource)
        resource->open = false;
    error.clear();
    return true;
}

static bool stdio_eof(void *data, bool &eof, std::string &error) {
    stdio_resource *resource = static_cast<stdio_resource *>(data);
    if (!resource || !resource->open) {
        error = "stream is closed";
        return false;
    }
    eof = resource->eof || std::feof(resource->file);
    return true;
}

static bool stdio_seekable(void *data) {
    (void)data;
    return false;
}

static bool stdio_seek(void *data, long long offset, int whence,
                       long long &position, std::string &error) {
    (void)data;
    (void)offset;
    (void)whence;
    (void)position;
    error = "standard stream is not seekable";
    return false;
}

static bool stdio_tell(void *data, long long &position, std::string &error) {
    (void)data;
    (void)position;
    error = "standard stream is not seekable";
    return false;
}

static const stream_ops kStdioOps = {
    stdio_read, stdio_write, stdio_flush, stdio_close, stdio_eof,
    stdio_seekable, stdio_seek, stdio_tell, nullptr, nullptr,
};

static const native_type kStdioType = {
    "stdio", describe_stdio, &kStdioOps,
};

struct value_t *new_stdio_stream_value(std::FILE *file, const char *name,
                                       bool readable, bool writable) {
    std::shared_ptr<stdio_resource> resource = std::make_shared<stdio_resource>();
    resource->file = file;
    resource->name = name ? name : "stdio";
    resource->readable = readable;
    resource->writable = writable;
    userdata_value_t *value = new userdata_value_t;
    value->type = T_userdata;
    value->meta = &kStdioType;
    value->data = resource;
    return (value_t *)value;
}
