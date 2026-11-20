#include "log.h"

#include "ast.h"
#include "builtins_internal.h"
#include "valuetype.h"

#include <cerrno>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

struct log_field {
    std::string key;
    std::string text;
    std::string json;
};

struct log_sink {
    std::mutex mutex;
    std::string output = "stderr";
    std::string on_error = "fail";
    bool flush_each = true;
    FILE *file = nullptr;

    ~log_sink() {
        if (file)
            std::fclose(file);
    }

    FILE *target() const {
        if (file)
            return file;
        return output == "stdout" ? stdout : stderr;
    }

    bool write_record(const std::string &record, std::string &error) {
        std::lock_guard<std::mutex> lock(mutex);
        FILE *stream = target();
        if (!stream) {
            error = "log output is not open";
            return false;
        }
        const std::size_t written =
            std::fwrite(record.data(), 1, record.size(), stream);
        const bool flushed = !flush_each || std::fflush(stream) == 0;
        if (written != record.size()) {
            error = std::strerror(errno);
            return false;
        }
        if (!flushed) {
            error = std::strerror(errno);
            return false;
        }
        return true;
    }
};

struct log_state : std::enable_shared_from_this<log_state> {
    std::shared_ptr<log_sink> sink;
    std::atomic<int> level{static_cast<int>(LogLevel::Info)};
    bool json = false;
    bool utc = false;
    std::string name;
    std::vector<log_field> fields;
    struct value_t *writer = nullptr;

    log_state() = default;
    log_state(const log_state &other)
        : std::enable_shared_from_this<log_state>(),
          sink(other.sink),
          level(other.level.load(std::memory_order_relaxed)),
          json(other.json),
          utc(other.utc),
          name(other.name),
          fields(other.fields),
          writer(other.writer ? dupval(other.writer) : nullptr) {}
    log_state &operator=(const log_state &) = delete;
    ~log_state() {
        if (writer)
            freeval(writer);
    }
};

const char *level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

std::string lowercase(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return text;
}

bool parse_level(const std::string &text, LogLevel &level) {
    const std::string value = lowercase(text);
    if (value == "trace")
        level = LogLevel::Trace;
    else if (value == "debug")
        level = LogLevel::Debug;
    else if (value == "info")
        level = LogLevel::Info;
    else if (value == "warn" || value == "warning")
        level = LogLevel::Warn;
    else if (value == "error")
        level = LogLevel::Error;
    else
        return false;
    return true;
}

void append_json_string(std::string &out, const std::string &text) {
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    out.push_back('"');
}

std::string bytes_hex(const std::string &data) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char c : data) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0F]);
    }
    return out;
}

void append_json_value(std::string &out, const struct value_t *value,
                       int depth) {
    if (!value) {
        out += "null";
        return;
    }
    if (depth > 8) {
        out += "\"<depth limit>\"";
        return;
    }
    switch (value->type) {
        case T_null:
            out += "null";
            break;
        case T_bool:
            out += ((const bool_value_t *)value)->value ? "true" : "false";
            break;
        case T_int:
            out += std::to_string(((const int_value_t *)value)->value);
            break;
        case T_float: {
            const double number = ((const float_value_t *)value)->value;
            if (std::isfinite(number)) {
                char buffer[32];
                std::snprintf(buffer, sizeof buffer, "%.17g", number);
                out += buffer;
            } else {
                append_json_string(out, value_to_string(value));
            }
            break;
        }
        case T_string:
            append_json_string(
                out, ((const string_value_t *)value)->value
                         ? ((const string_value_t *)value)->value
                         : "");
            break;
        case T_char: {
            std::string encoded;
            if (utf8_encode(((const char_value_t *)value)->value, encoded))
                append_json_string(out, encoded);
            else
                out += "null";
            break;
        }
        case T_byte:
            out += std::to_string(((const byte_value_t *)value)->value);
            break;
        case T_bytes:
            append_json_string(
                out, "hex:" + bytes_hex(((const bytes_value_t *)value)
                                            ->value.read()));
            break;
        case T_list: {
            const auto &items = ((const list_value_t *)value)->payload->items;
            out.push_back('[');
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i)
                    out.push_back(',');
                append_json_value(out, items[i], depth + 1);
            }
            out.push_back(']');
            break;
        }
        case T_dict: {
            const auto &items = ((const dict_value_t *)value)->payload->items;
            out.push_back('{');
            bool first = true;
            for (const auto &item : items) {
                std::string key;
                if (item.first && item.first->type == T_string) {
                    key = ((const string_value_t *)item.first)->value;
                } else {
                    key = value_to_string(item.first);
                }
                if (!first)
                    out.push_back(',');
                first = false;
                append_json_string(out, key);
                out.push_back(':');
                append_json_value(out, item.second, depth + 1);
            }
            out.push_back('}');
            break;
        }
        case T_error: {
            const error_value_t *error = (const error_value_t *)value;
            append_json_string(out, error->kind + ": " + error->message);
            break;
        }
        default:
            append_json_string(out, value_to_string(value));
            break;
    }
}

std::string timestamp(bool utc) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto since_epoch =
        duration_cast<milliseconds>(now.time_since_epoch()).count();
    long long millis = since_epoch % 1000;
    if (millis < 0)
        millis += 1000;
    const std::time_t seconds = system_clock::to_time_t(now);
    std::tm value{};
#ifdef _WIN32
    if (utc)
        gmtime_s(&value, &seconds);
    else
        localtime_s(&value, &seconds);
#else
    if (utc)
        gmtime_r(&seconds, &value);
    else
        localtime_r(&seconds, &value);
#endif
    char base[32];
    std::strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &value);
    char zone[8] = "";
    if (utc) {
        std::snprintf(zone, sizeof zone, "Z");
    } else {
        std::strftime(zone, sizeof zone, "%z", &value);
        if (std::strlen(zone) == 5) {
            char expanded[8];
            expanded[0] = zone[0];
            expanded[1] = zone[1];
            expanded[2] = zone[2];
            expanded[3] = ':';
            expanded[4] = zone[3];
            expanded[5] = zone[4];
            expanded[6] = '\0';
            std::memcpy(zone, expanded, sizeof expanded);
        }
    }
    char out[64];
    std::snprintf(out, sizeof out, "%s.%03d%s", base, (int)millis, zone);
    return out;
}

bool dict_lookup(const struct value_t *dict, const char *key,
                 struct value_t **out) {
    if (!dict || dict->type != T_dict)
        return false;
    for (const auto &item : ((const dict_value_t *)dict)->payload->items) {
        if (!item.first || item.first->type != T_string)
            continue;
        const char *name = ((const string_value_t *)item.first)->value;
        if (name && std::strcmp(name, key) == 0) {
            *out = item.second;
            return true;
        }
    }
    return false;
}

bool read_string_option(const struct value_t *options, const char *key,
                        std::string &out) {
    struct value_t *value = nullptr;
    if (!dict_lookup(options, key, &value))
        return true;
    if (!value || value->type != T_string) {
        yyerror("std.log.new: %s must be a string", key);
        return false;
    }
    out = ((const string_value_t *)value)->value;
    return true;
}

bool read_bool_option(const struct value_t *options, const char *key,
                      bool &out) {
    struct value_t *value = nullptr;
    if (!dict_lookup(options, key, &value))
        return true;
    if (!value || value->type != T_bool) {
        yyerror("std.log.new: %s must be a bool", key);
        return false;
    }
    out = ((const bool_value_t *)value)->value;
    return true;
}

bool make_field(const struct value_t *key, const struct value_t *value,
                log_field &field) {
    if (!key || key->type != T_string) {
        yyerror("std.log: field keys must be strings");
        return false;
    }
    field.key = ((const string_value_t *)key)->value;
    field.text = value_to_string(value);
    field.json.clear();
    append_json_value(field.json, value, 0);
    return true;
}

bool make_fields(const struct value_t *dict, std::vector<log_field> &fields) {
    if (!dict || dict->type != T_dict) {
        yyerror("std.log: fields must be a dict");
        return false;
    }
    for (const auto &item : ((const dict_value_t *)dict)->payload->items) {
        log_field field;
        if (!make_field(item.first, item.second, field))
            return false;
        fields.push_back(std::move(field));
    }
    return true;
}

void merge_fields(std::vector<log_field> &target,
                  const std::vector<log_field> &extra) {
    for (const log_field &field : extra) {
        bool replaced = false;
        for (log_field &existing : target) {
            if (existing.key == field.key) {
                existing = field;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            target.push_back(field);
    }
}

std::string format_text(const log_state &state, LogLevel level,
                        const std::string &message,
                        const std::vector<log_field> &fields) {
    std::string line = timestamp(state.utc);
    line.push_back(' ');
    const char *name = level_name(level);
    line += name;
    for (std::size_t i = std::strlen(name); i < 5; ++i)
        line.push_back(' ');
    line.push_back(' ');
    if (!state.name.empty()) {
        line += state.name;
        line += ": ";
    }
    line += message;
    for (const log_field &field : fields) {
        line.push_back(' ');
        line += field.key;
        line.push_back('=');
        line += field.text;
    }
    line.push_back('\n');
    return line;
}

std::string format_json(const log_state &state, LogLevel level,
                        const std::string &message,
                        const std::vector<log_field> &fields) {
    std::string line = "{\"time\":";
    append_json_string(line, timestamp(state.utc));
    line += ",\"level\":";
    append_json_string(line, lowercase(level_name(level)));
    line += ",\"logger\":";
    append_json_string(line, state.name);
    line += ",\"message\":";
    append_json_string(line, message);
    line += ",\"fields\":{";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i)
            line.push_back(',');
        append_json_string(line, fields[i].key);
        line.push_back(':');
        line += fields[i].json;
    }
    line += "}}\n";
    return line;
}

struct value_t *handle_write_failure(const std::shared_ptr<log_state> &state,
                                     const std::string &error);

bool flush_sink(log_sink &sink, std::string &error) {
    std::lock_guard<std::mutex> lock(sink.mutex);
    FILE *stream = sink.target();
    if (stream && std::fflush(stream) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

struct value_t *handle_write_failure(const std::shared_ptr<log_state> &state,
                                     const std::string &error) {
    if (state->sink->on_error == "ignore")
        return newnull();
    if (state->sink->on_error == "warn") {
        std::fprintf(stderr, "skein: log: %s\n", error.c_str());
        std::fflush(stderr);
        return newnull();
    }
    yyerror("std.log: %s", error.c_str());
    return NULL;
}

struct value_t *emit_record(const std::shared_ptr<log_state> &state,
                            LogLevel level, const std::string &message,
                            const std::vector<log_field> &extra) {
    if (static_cast<int>(level) <
        state->level.load(std::memory_order_relaxed))
        return newnull();
    std::vector<log_field> fields = state->fields;
    merge_fields(fields, extra);
    const std::string record =
        state->json ? format_json(*state, level, message, fields)
                    : format_text(*state, level, message, fields);
    if (state->writer) {
        std::vector<value_t *> args{newstrval(record.c_str())};
        value_t *result = skein_call_function(state->writer, std::move(args));
        if (!result)
            return NULL;
        freeval(result);
        return newnull();
    }
    std::string error;
    if (!state->sink->write_record(record, error))
        return handle_write_failure(state, error);
    return newnull();
}

struct value_t *log_message(const std::shared_ptr<log_state> &state,
                            LogLevel level, const std::vector<value_t *> &args,
                            const char *name) {
    if (args.size() != 1 && args.size() != 2) {
        yyerror("%s expects 1 or 2 arguments, got %d", name, (int)args.size());
        return NULL;
    }
    if (!args[0] || args[0]->type != T_string) {
        yyerror("%s message must be a string", name);
        return NULL;
    }
    std::vector<log_field> fields;
    if (args.size() == 2 && !make_fields(args[1], fields))
        return NULL;
    return emit_record(state, level,
                       ((const string_value_t *)args[0])->value, fields);
}

struct value_t *log_with_level(const std::shared_ptr<log_state> &state,
                               const std::vector<value_t *> &args,
                               const char *name) {
    if (args.size() != 2 && args.size() != 3) {
        yyerror("%s expects 2 or 3 arguments, got %d", name, (int)args.size());
        return NULL;
    }
    if (!args[0] || args[0]->type != T_string) {
        yyerror("%s level must be a string", name);
        return NULL;
    }
    LogLevel level;
    if (!parse_level(((const string_value_t *)args[0])->value, level)) {
        yyerror("%s unknown level \"%s\"", name,
                ((const string_value_t *)args[0])->value);
        return NULL;
    }
    if (!args[1] || args[1]->type != T_string) {
        yyerror("%s message must be a string", name);
        return NULL;
    }
    std::vector<log_field> fields;
    if (args.size() == 3 && !make_fields(args[2], fields))
        return NULL;
    return emit_record(state, level,
                       ((const string_value_t *)args[1])->value, fields);
}

std::shared_ptr<log_state> make_logger(const std::vector<value_t *> &args) {
    if (args.size() > 1) {
        yyerror("std.log.new expects 0 or 1 arguments, got %d",
                (int)args.size());
        return nullptr;
    }
    const struct value_t *options = args.empty() ? nullptr : args[0];
    if (options && options->type != T_dict) {
        yyerror("std.log.new expects an options dict, got %s",
                typename_of(options));
        return nullptr;
    }

    auto sink = std::make_shared<log_sink>();
    auto state = std::make_shared<log_state>();
    state->sink = sink;

    std::string level = "info";
    std::string format = "text";
    std::string output = "stderr";
    std::string path;
    std::string time = "local";
    std::string on_error = "fail";
    std::string name;
    bool append = true;
    bool flush_each = true;

    if (options) {
        static const char *const known[] = {
            "level", "format", "output", "path", "append",
            "time", "name", "fields", "flush", "on_error", "writer",
        };
        for (const auto &item : ((const dict_value_t *)options)
                                    ->payload->items) {
            if (!item.first || item.first->type != T_string) {
                yyerror("std.log.new: option names must be strings");
                return nullptr;
            }
            const char *key = ((const string_value_t *)item.first)->value;
            bool recognized = false;
            for (const char *candidate : known) {
                if (std::strcmp(key, candidate) == 0) {
                    recognized = true;
                    break;
                }
            }
            if (!recognized) {
                yyerror("std.log.new: unknown option \"%s\"", key);
                return nullptr;
            }
        }
        if (!read_string_option(options, "level", level) ||
            !read_string_option(options, "format", format) ||
            !read_string_option(options, "output", output) ||
            !read_string_option(options, "path", path) ||
            !read_string_option(options, "time", time) ||
            !read_string_option(options, "on_error", on_error) ||
            !read_string_option(options, "name", name) ||
            !read_bool_option(options, "append", append) ||
            !read_bool_option(options, "flush", flush_each))
            return nullptr;
        struct value_t *fields = nullptr;
        if (dict_lookup(options, "fields", &fields) &&
            !make_fields(fields, state->fields))
            return nullptr;
        struct value_t *writer = nullptr;
        if (dict_lookup(options, "writer", &writer)) {
            if (!writer || (writer->type != T_func &&
                            writer->type != T_native &&
                            writer->type != T_builtin)) {
                yyerror("std.log.new: writer must be a function");
                return nullptr;
            }
            struct value_t *conflict = nullptr;
            if (dict_lookup(options, "output", &conflict) ||
                dict_lookup(options, "path", &conflict)) {
                yyerror("std.log.new: writer cannot be combined with "
                        "output or path");
                return nullptr;
            }
            state->writer = dupval(writer);
        }
    }

    LogLevel parsed_level = LogLevel::Info;
    if (!parse_level(level, parsed_level)) {
        yyerror("std.log.new: unknown level \"%s\"", level.c_str());
        return nullptr;
    }
    state->level.store(static_cast<int>(parsed_level),
                       std::memory_order_relaxed);
    const std::string normalized_format = lowercase(format);
    if (normalized_format == "text")
        state->json = false;
    else if (normalized_format == "json")
        state->json = true;
    else {
        yyerror("std.log.new: unknown format \"%s\"", format.c_str());
        return nullptr;
    }
    const std::string normalized_time = lowercase(time);
    if (normalized_time == "local")
        state->utc = false;
    else if (normalized_time == "utc")
        state->utc = true;
    else {
        yyerror("std.log.new: unknown time \"%s\"", time.c_str());
        return nullptr;
    }
    const std::string normalized_error = lowercase(on_error);
    if (normalized_error != "fail" && normalized_error != "warn" &&
        normalized_error != "ignore") {
        yyerror("std.log.new: unknown on_error \"%s\"", on_error.c_str());
        return nullptr;
    }
    state->name = name;
    sink->output = lowercase(output);
    sink->on_error = normalized_error;
    sink->flush_each = flush_each;
    if (sink->output == "file") {
        if (path.empty()) {
            yyerror("std.log.new: output \"file\" requires a path");
            return nullptr;
        }
        sink->file = std::fopen(path.c_str(), append ? "ab" : "wb");
        if (!sink->file) {
            yyerror("std.log.new: cannot open %s: %s", path.c_str(),
                    std::strerror(errno));
            return nullptr;
        }
    } else if (sink->output == "stderr" || sink->output == "stdout") {
        if (!path.empty()) {
            yyerror("std.log.new: path is only valid with output \"file\"");
            return nullptr;
        }
    } else {
        yyerror("std.log.new: unknown output \"%s\"", output.c_str());
        return nullptr;
    }
    return state;
}

std::shared_ptr<log_state> default_logger() {
    static std::shared_ptr<log_state> state = [] {
        auto logger = std::make_shared<log_state>();
        logger->sink = std::make_shared<log_sink>();
        return logger;
    }();
    return state;
}

std::string logger_describe(const void *data) {
    const log_state *state = static_cast<const log_state *>(data);
    if (!state)
        return "logger";
    if (state->name.empty())
        return "logger";
    return "logger(" + state->name + ")";
}

struct value_t *logger_get_field(void *data, const char *name);

void logger_for_each_child(
    const void *data,
    const std::function<void(const struct value_t *)> &visit) {
    const log_state *state = static_cast<const log_state *>(data);
    if (state && state->writer)
        visit(state->writer);
}

const struct native_type kLoggerType = {
    "logger",
    logger_describe,
    nullptr,
    logger_get_field,
    nullptr,
    logger_for_each_child,
};

struct value_t *logger_sink_field(const std::shared_ptr<log_state> &state,
                                  const char *name) {
    if (std::strcmp(name, "trace") == 0) {
        return newnativeval([state](const std::vector<value_t *> &args) {
            return log_message(state, LogLevel::Trace, args,
                               "logger.trace");
        }, state);
    }
    if (std::strcmp(name, "debug") == 0) {
        return newnativeval([state](const std::vector<value_t *> &args) {
            return log_message(state, LogLevel::Debug, args,
                               "logger.debug");
        }, state);
    }
    if (std::strcmp(name, "info") == 0) {
        return newnativeval([state](const std::vector<value_t *> &args) {
            return log_message(state, LogLevel::Info, args, "logger.info");
        }, state);
    }
    if (std::strcmp(name, "warn") == 0) {
        return newnativeval([state](const std::vector<value_t *> &args) {
            return log_message(state, LogLevel::Warn, args, "logger.warn");
        }, state);
    }
    if (std::strcmp(name, "error") == 0) {
        return newnativeval([state](const std::vector<value_t *> &args) {
            return log_message(state, LogLevel::Error, args,
                               "logger.error");
        }, state);
    }
    if (std::strcmp(name, "log") == 0) {
        return newnativeval([state](const std::vector<value_t *> &args) {
            return log_with_level(state, args, "logger.log");
        }, state);
    }
    if (std::strcmp(name, "with") == 0) {
        return newnativeval([state](const std::vector<value_t *> &args) {
            if (args.size() != 1) {
                yyerror("logger.with expects 1 argument");
                return (value_t *)NULL;
            }
            std::vector<log_field> extra;
            if (!make_fields(args[0], extra))
                return (value_t *)NULL;
            auto derived = std::make_shared<log_state>(*state);
            merge_fields(derived->fields, extra);
            return newuserdataval(&kLoggerType, derived);
        }, state);
    }
    if (std::strcmp(name, "set_level") == 0) {
        return newnativeval([state](const std::vector<value_t *> &args) {
            if (args.size() != 1) {
                yyerror("logger.set_level expects 1 argument");
                return (value_t *)NULL;
            }
            if (!args[0] || args[0]->type != T_string) {
                yyerror("logger.set_level level must be a string");
                return (value_t *)NULL;
            }
            LogLevel level;
            if (!parse_level(((const string_value_t *)args[0])->value,
                             level)) {
                yyerror("logger.set_level unknown level \"%s\"",
                        ((const string_value_t *)args[0])->value);
                return (value_t *)NULL;
            }
            state->level.store(static_cast<int>(level),
                               std::memory_order_relaxed);
            return newnull();
        }, state);
    }
    if (std::strcmp(name, "flush") == 0) {
        return newnativeval([state](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("logger.flush expects 0 arguments");
                return (value_t *)NULL;
            }
            std::string error;
            if (!flush_sink(*state->sink, error))
                return handle_write_failure(state, error);
            return newnull();
        }, state);
    }
    yyerror("logger has no field %s", name);
    return NULL;
}

struct value_t *logger_get_field(void *data, const char *name) {
    log_state *state = static_cast<log_state *>(data);
    if (!state || !name) {
        yyerror("logger has no field");
        return NULL;
    }
    return logger_sink_field(state->shared_from_this(), name);
}

struct value_t *log_level_builtin(LogLevel level,
                                  const std::vector<value_t *> &args,
                                  const char *name) {
    return log_message(default_logger(), level, args, name);
}

} // namespace

struct value_t *call_log_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_log_new: {
            std::shared_ptr<log_state> state = make_logger(args);
            if (!state)
                return NULL;
            return newuserdataval(&kLoggerType, state);
        }
        case B_log_trace:
            return log_level_builtin(LogLevel::Trace, args, "std.log.trace");
        case B_log_debug:
            return log_level_builtin(LogLevel::Debug, args, "std.log.debug");
        case B_log_info:
            return log_level_builtin(LogLevel::Info, args, "std.log.info");
        case B_log_warn:
            return log_level_builtin(LogLevel::Warn, args, "std.log.warn");
        case B_log_error:
            return log_level_builtin(LogLevel::Error, args, "std.log.error");
        case B_log_flush: {
            if (!args.empty()) {
                yyerror("std.log.flush expects 0 arguments");
                return NULL;
            }
            std::shared_ptr<log_state> state = default_logger();
            std::string error;
            if (!flush_sink(*state->sink, error))
                return handle_write_failure(state, error);
            return newnull();
        }
        case B_log_set_level: {
            if (args.size() != 1) {
                yyerror("std.log.set_level expects 1 argument");
                return NULL;
            }
            if (!args[0] || args[0]->type != T_string) {
                yyerror("std.log.set_level level must be a string");
                return NULL;
            }
            LogLevel level;
            if (!parse_level(((const string_value_t *)args[0])->value,
                             level)) {
                yyerror("std.log.set_level unknown level \"%s\"",
                        ((const string_value_t *)args[0])->value);
                return NULL;
            }
            default_logger()->level.store(static_cast<int>(level),
                                          std::memory_order_relaxed);
            return newnull();
        }
        default:
            yyerror("unknown log function");
            return NULL;
    }
}
