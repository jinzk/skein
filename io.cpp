#include "io.h"

#include "ast.h"
#include "repl.h"
#include "valuetype.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

struct stream_view {
    const stream_ops *ops = NULL;
    void *data = NULL;
};

bool get_stream(value_t *value, stream_view &view, const char *who) {
    if (!value || value->type != T_userdata) {
        yyerror("%s expects a stream, got %s", who, typename_of(value));
        return false;
    }
    userdata_value_t *userdata = (userdata_value_t *)value;
    if (!userdata->meta || !userdata->meta->stream || !userdata->data) {
        yyerror("%s expects a stream, got %s", who, typename_of(value));
        return false;
    }
    view.ops = userdata->meta->stream;
    view.data = userdata->data.get();
    return true;
}

bool get_timeout(const std::vector<value_t *> &args, std::size_t index,
                 long long &timeout_ms, const char *who) {
    timeout_ms = 0;
    if (args.size() <= index)
        return true;
    if (args[index]->type != T_int ||
        ((int_value_t *)args[index])->value < 0) {
        yyerror("%s timeout must be a non-negative integer", who);
        return false;
    }
    timeout_ms = ((int_value_t *)args[index])->value;
    return true;
}

bool get_size(value_t *value, std::size_t &size, bool allow_zero,
              const char *who) {
    if (!value || value->type != T_int ||
        ((int_value_t *)value)->value < 0) {
        yyerror("%s expects a non-negative size", who);
        return false;
    }
    long long requested = ((int_value_t *)value)->value;
    if (!allow_zero && requested == 0) {
        yyerror("%s size must be greater than zero", who);
        return false;
    }
    size = (std::size_t)requested;
    return true;
}

bool get_data(value_t *value, std::string &data, const char *who) {
    if (value && value->type == T_string) {
        data = ((string_value_t *)value)->value;
        return true;
    }
    if (value && value->type == T_bytes) {
        data = ((bytes_value_t *)value)->value;
        return true;
    }
    yyerror("%s data must be string or bytes, got %s", who,
            typename_of(value));
    return false;
}

void report_stream_error(const char *who, const std::string &error) {
    if (g_interrupted)
        return;
    yyerror("%s: %s", who, error.empty() ? "stream error" : error.c_str());
}

bool write_all_data(const stream_view &stream, const std::string &data,
                    long long timeout_ms, std::size_t &written,
                    const char *who) {
    written = 0;
    while (written < data.size()) {
        std::size_t count = 0;
        std::string error;
        if (!stream.ops->write(stream.data, data.substr(written), timeout_ms,
                               count, error)) {
            report_stream_error(who, error);
            return false;
        }
        if (count == 0) {
            report_stream_error(who, "write returned 0 bytes");
            return false;
        }
        written += count;
        if (written > data.size()) {
            report_stream_error(who, "write returned too many bytes");
            return false;
        }
    }
    return true;
}

value_t *io_read(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.io.read expects 2 or 3 arguments");
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, "std.io.read"))
        return NULL;
    std::size_t max_bytes = 0;
    if (!get_size(args[1], max_bytes, false, "std.io.read"))
        return NULL;
    long long timeout_ms = 0;
    if (!get_timeout(args, 2, timeout_ms, "std.io.read"))
        return NULL;

    std::string out;
    std::string error;
    bool eof = false;
    if (!stream.ops->read(stream.data, max_bytes, timeout_ms, out, eof,
                          error)) {
        report_stream_error("std.io.read", error);
        return NULL;
    }
    if (eof)
        return newnull();
    return newbytesval(std::move(out));
}

value_t *io_write(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.io.write expects 2 or 3 arguments");
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, "std.io.write"))
        return NULL;
    std::string data;
    if (!get_data(args[1], data, "std.io.write"))
        return NULL;
    long long timeout_ms = 0;
    if (!get_timeout(args, 2, timeout_ms, "std.io.write"))
        return NULL;

    std::size_t written = 0;
    std::string error;
    if (!stream.ops->write(stream.data, data, timeout_ms, written, error)) {
        report_stream_error("std.io.write", error);
        return NULL;
    }
    return newintval((long long)written);
}

value_t *io_write_all(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.io.write_all expects 2 or 3 arguments");
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, "std.io.write_all"))
        return NULL;
    std::string data;
    if (!get_data(args[1], data, "std.io.write_all"))
        return NULL;
    long long timeout_ms = 0;
    if (!get_timeout(args, 2, timeout_ms, "std.io.write_all"))
        return NULL;

    std::size_t written = 0;
    if (!write_all_data(stream, data, timeout_ms, written, "std.io.write_all"))
        return NULL;
    return newintval((long long)written);
}

value_t *io_flush(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.io.flush expects 1 argument");
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, "std.io.flush"))
        return NULL;
    if (!stream.ops->flush) {
        yyerror("std.io.flush: stream is not flushable");
        return NULL;
    }
    std::string error;
    if (!stream.ops->flush(stream.data, error)) {
        report_stream_error("std.io.flush", error);
        return NULL;
    }
    return newnull();
}

value_t *io_close(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.io.close expects 1 argument");
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, "std.io.close"))
        return NULL;
    if (!stream.ops->close) {
        yyerror("std.io.close: stream is not closable");
        return NULL;
    }
    bool was_open = false;
    std::string error;
    if (!stream.ops->close(stream.data, was_open, error)) {
        report_stream_error("std.io.close", error);
        return NULL;
    }
    return newboolval(was_open);
}

value_t *io_eof(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.io.eof expects 1 argument");
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, "std.io.eof"))
        return NULL;
    if (!stream.ops->eof) {
        yyerror("std.io.eof: stream does not support eof");
        return NULL;
    }
    bool eof = false;
    std::string error;
    if (!stream.ops->eof(stream.data, eof, error)) {
        report_stream_error("std.io.eof", error);
        return NULL;
    }
    return newboolval(eof);
}

value_t *io_capability(const std::vector<value_t *> &args, int functype) {
    const char *name =
        functype == B_io_readable ? "std.io.readable" :
        functype == B_io_writable ? "std.io.writable" :
        "std.io.seekable";
    if (args.size() != 1) {
        yyerror("%s expects 1 argument", name);
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, name))
        return NULL;
    bool result = false;
    if (functype == B_io_readable)
        result = stream.ops->read != NULL;
    else if (functype == B_io_writable)
        result = stream.ops->write != NULL;
    else
        result = stream.ops->seekable && stream.ops->seekable(stream.data);
    return newboolval(result);
}

value_t *io_read_exact(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.io.read_exact expects 2 or 3 arguments");
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, "std.io.read_exact"))
        return NULL;
    std::size_t wanted = 0;
    if (!get_size(args[1], wanted, true, "std.io.read_exact"))
        return NULL;
    long long timeout_ms = 0;
    if (!get_timeout(args, 2, timeout_ms, "std.io.read_exact"))
        return NULL;

    std::string out;
    out.reserve(wanted);
    while (out.size() < wanted) {
        std::size_t remaining = wanted - out.size();
        std::size_t request = std::min<std::size_t>(remaining, 64 * 1024);
        std::string chunk;
        std::string error;
        bool eof = false;
        if (!stream.ops->read(stream.data, request, timeout_ms, chunk, eof,
                              error)) {
            report_stream_error("std.io.read_exact", error);
            return NULL;
        }
        if (eof) {
            if (g_interrupted)
                return NULL;
            yyerror("std.io.read_exact: unexpected EOF after %zu of %zu bytes",
                    out.size(), wanted);
            return NULL;
        }
        if (chunk.empty()) {
            if (g_interrupted)
                return NULL;
            yyerror("std.io.read_exact: read returned no data");
            return NULL;
        }
        out += chunk;
    }
    return newbytesval(std::move(out));
}

value_t *io_read_all(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.io.read_all expects 1 or 2 arguments");
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, "std.io.read_all"))
        return NULL;
    std::size_t limit = 16 * 1024 * 1024;
    if (args.size() == 2 &&
        !get_size(args[1], limit, false, "std.io.read_all"))
        return NULL;

    std::string out;
    while (out.size() < limit) {
        std::size_t request =
            std::min<std::size_t>(limit - out.size(), 64 * 1024);
        std::string chunk;
        std::string error;
        bool eof = false;
        if (!stream.ops->read(stream.data, request, 0, chunk, eof, error)) {
            report_stream_error("std.io.read_all", error);
            return NULL;
        }
        if (eof)
            break;
        if (chunk.empty()) {
            if (g_interrupted)
                return NULL;
            yyerror("std.io.read_all: read returned no data");
            return NULL;
        }
        out += chunk;
    }
    return newbytesval(std::move(out));
}

value_t *io_read_line(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.io.read_line expects 1 or 2 arguments");
        return NULL;
    }
    stream_view stream;
    if (!get_stream(args[0], stream, "std.io.read_line"))
        return NULL;
    std::size_t limit = 4096;
    if (args.size() == 2 &&
        !get_size(args[1], limit, false, "std.io.read_line"))
        return NULL;

    std::string out;
    for (;;) {
        if (out.size() >= limit) {
            yyerror("std.io.read_line: line exceeds limit");
            return NULL;
        }
        std::string chunk;
        std::string error;
        bool eof = false;
        if (!stream.ops->read(stream.data, 1, 0, chunk, eof, error)) {
            report_stream_error("std.io.read_line", error);
            return NULL;
        }
        if (eof) {
            if (out.empty())
                return newnull();
            break;
        }
        if (chunk.empty()) {
            if (g_interrupted)
                return NULL;
            yyerror("std.io.read_line: read returned no data");
            return NULL;
        }
        char c = chunk[0];
        if (c == '\n')
            break;
        out += c;
    }
    if (!out.empty() && out.back() == '\r')
        out.pop_back();
    return newbytesval(std::move(out));
}

value_t *io_copy(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.io.copy expects 2 or 3 arguments");
        return NULL;
    }
    stream_view destination;
    stream_view source;
    if (!get_stream(args[0], destination, "std.io.copy destination"))
        return NULL;
    if (!get_stream(args[1], source, "std.io.copy source"))
        return NULL;
    std::size_t limit = 0;
    if (args.size() == 3 &&
        !get_size(args[2], limit, true, "std.io.copy"))
        return NULL;

    std::size_t total = 0;
    for (;;) {
        if (limit != 0 && total >= limit)
            break;
        std::size_t request = 64 * 1024;
        if (limit != 0)
            request = std::min(request, limit - total);
        std::string chunk;
        std::string error;
        bool eof = false;
        if (!source.ops->read(source.data, request, 0, chunk, eof, error)) {
            report_stream_error("std.io.copy", error);
            return NULL;
        }
        if (eof)
            break;
        if (chunk.empty()) {
            if (g_interrupted)
                return NULL;
            yyerror("std.io.copy: read returned no data");
            return NULL;
        }
        std::size_t written = 0;
        if (!write_all_data(destination, chunk, 0, written, "std.io.copy"))
            return NULL;
        total += written;
    }
    return newintval((long long)total);
}

} // namespace

struct value_t *call_io_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_io_read:
            return io_read(args);
        case B_io_write:
            return io_write(args);
        case B_io_write_all:
            return io_write_all(args);
        case B_io_flush:
            return io_flush(args);
        case B_io_close:
            return io_close(args);
        case B_io_eof:
            return io_eof(args);
        case B_io_readable:
        case B_io_writable:
        case B_io_seekable:
            return io_capability(args, functype);
        case B_io_read_exact:
            return io_read_exact(args);
        case B_io_read_all:
            return io_read_all(args);
        case B_io_read_line:
            return io_read_line(args);
        case B_io_copy:
            return io_copy(args);
        default:
            yyerror("unknown io function");
            return NULL;
    }
}
