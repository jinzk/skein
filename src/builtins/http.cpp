#include "http.h"

#include "runtime_context.h"
#include "ast.h"
#include "compression.h"
#include "cookie.h"
#include "eval.h"
#include "fiber.h"
#include "http1_parser.h"
#include "io.h"
#include "net.h"
#include "repl.h"
#include "task.h"
#ifdef SKEIN_ENABLE_TLS
#include "tls.h"
#endif
#include "valuetype.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

value_t *await_task_value(value_t *task_value);

void free_args(std::vector<value_t *> &args) {
    for (size_t i = 0; i < args.size(); ++i)
        freeval(args[i]);
    args.clear();
}

value_t *call_net(int functype, std::vector<value_t *> args) {
    value_t *result = call_net_builtin(functype, args);
    free_args(args);
    return result;
}

value_t *call_io(int functype, std::vector<value_t *> args) {
    value_t *result = call_io_builtin(functype, args);
    free_args(args);
    return result;
}

#ifdef SKEIN_ENABLE_TLS
value_t *call_tls(int functype, std::vector<value_t *> args) {
    value_t *result = call_tls_builtin(functype, args);
    free_args(args);
    return result;
}
#endif

void close_socket(value_t *socket) {
    if (!socket)
        return;
    std::vector<value_t *> args;
    args.push_back(dupval(socket));
    value_t *result = call_io_builtin(B_io_close, args);
    free_args(args);
    freeval(result);
}

struct pooled_connection {
    value_t *stream = NULL;
    std::string key;
    std::chrono::steady_clock::time_point last_used;
    bool in_use = false;
};

std::vector<std::unique_ptr<pooled_connection>> &connection_pool() {
    static std::vector<std::unique_ptr<pooled_connection>> pool;
    return pool;
}

void pool_discard(pooled_connection *connection) {
    if (!connection)
        return;
    std::vector<std::unique_ptr<pooled_connection>> &pool = connection_pool();
    for (size_t i = 0; i < pool.size(); ++i) {
        if (pool[i].get() != connection)
            continue;
        close_socket(connection->stream);
        freeval(connection->stream);
        pool.erase(pool.begin() + (std::ptrdiff_t)i);
        return;
    }
}

void pool_expire() {
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<pooled_connection>> &pool = connection_pool();
    for (size_t i = 0; i < pool.size();) {
        if (!pool[i]->in_use &&
            now - pool[i]->last_used > std::chrono::seconds(30)) {
            close_socket(pool[i]->stream);
            freeval(pool[i]->stream);
            pool.erase(pool.begin() + (std::ptrdiff_t)i);
            continue;
        }
        ++i;
    }
}

pooled_connection *pool_acquire(const std::string &key) {
    pool_expire();
    std::vector<std::unique_ptr<pooled_connection>> &pool = connection_pool();
    for (size_t i = 0; i < pool.size(); ++i) {
        if (!pool[i]->in_use && pool[i]->key == key) {
            pool[i]->in_use = true;
            return pool[i].get();
        }
    }
    return NULL;
}

pooled_connection *pool_add(const std::string &key, value_t *stream) {
    std::unique_ptr<pooled_connection> connection(new pooled_connection());
    connection->stream = stream;
    connection->key = key;
    connection->in_use = true;
    connection->last_used = std::chrono::steady_clock::now();
    pooled_connection *result = connection.get();
    connection_pool().push_back(std::move(connection));
    return result;
}

void pool_release(pooled_connection *connection) {
    if (!connection)
        return;
    connection->in_use = false;
    connection->last_used = std::chrono::steady_clock::now();
}

void pool_shutdown() {
    std::vector<std::unique_ptr<pooled_connection>> &pool = connection_pool();
    for (size_t i = 0; i < pool.size(); ++i) {
        close_socket(pool[i]->stream);
        freeval(pool[i]->stream);
    }
    pool.clear();
}

value_t *dict_get(value_t *dict, const char *key) {
    if (!dict || dict->type != T_dict)
        return NULL;
    dict_value_t *value = (dict_value_t *)dict;
    for (size_t i = 0; i < value->payload->items.size(); ++i) {
        value_t *item_key = value->payload->items[i].first;
        if (item_key->type != T_string)
            continue;
        if (std::strcmp(((string_value_t *)item_key)->value, key) == 0)
            return value->payload->items[i].second;
    }
    return NULL;
}

bool option_bool(value_t *options, const char *key, bool fallback) {
    value_t *value = dict_get(options, key);
    if (!value || value->type != T_bool)
        return fallback;
    return ((bool_value_t *)value)->value;
}

std::string option_string(value_t *options, const char *key) {
    value_t *value = dict_get(options, key);
    if (!value || value->type != T_string)
        return std::string();
    return ((string_value_t *)value)->value;
}

void dict_set(value_t *dict, const std::string &key, value_t *value) {
    if (!dict || dict->type != T_dict) {
        freeval(value);
        return;
    }
    dict_value_t *object = (dict_value_t *)dict;
    for (size_t i = 0; i < object->payload->items.size(); ++i) {
        value_t *item_key = object->payload->items[i].first;
        if (item_key->type == T_string &&
            key == ((string_value_t *)item_key)->value) {
            freeval(object->payload->items[i].second);
            object->payload->items[i].second = value;
            return;
        }
    }
    object->payload->items.push_back(
        std::make_pair(newstrval(key.c_str()), value));
}

bool positive_size_option(value_t *options, const char *key,
                          std::size_t &out) {
    value_t *value = dict_get(options, key);
    if (!value)
        return true;
    if (value->type != T_int ||
        ((int_value_t *)value)->value <= 0) {
        yyerror("std.http: %s must be a positive integer", key);
        return false;
    }
    out = (std::size_t)((int_value_t *)value)->value;
    return true;
}

bool callable_value(value_t *value) {
    return value && (value->type == T_func || value->type == T_builtin ||
                     value->type == T_method);
}

std::string lower_ascii(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char c = (unsigned char)value[i];
        value[i] = (char)std::tolower(c);
    }
    return value;
}

bool header_exists(value_t *headers, const char *name) {
    if (!headers || headers->type != T_dict)
        return false;
    std::string target = lower_ascii(name);
    dict_value_t *dict = (dict_value_t *)headers;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        value_t *key = dict->payload->items[i].first;
        if (key->type != T_string)
            continue;
        if (lower_ascii(((string_value_t *)key)->value) == target)
            return true;
    }
    return false;
}

std::string header_value(value_t *headers, const char *name) {
    if (!headers || headers->type != T_dict)
        return std::string();
    std::string target = lower_ascii(name);
    dict_value_t *dict = (dict_value_t *)headers;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        value_t *key = dict->payload->items[i].first;
        value_t *value = dict->payload->items[i].second;
        if (key->type == T_string && value->type == T_string &&
            lower_ascii(((string_value_t *)key)->value) == target)
            return ((string_value_t *)value)->value;
    }
    return std::string();
}

bool headers_request_close(value_t *headers) {
    std::string connection = lower_ascii(header_value(headers, "Connection"));
    return connection.find("close") != std::string::npos;
}

bool append_headers(std::string &out, value_t *headers, std::string &error) {
    if (!headers || headers->type == T_null)
        return true;
    if (headers->type != T_dict) {
        error = "headers must be a dict";
        return false;
    }
    dict_value_t *dict = (dict_value_t *)headers;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        value_t *key = dict->payload->items[i].first;
        value_t *value = dict->payload->items[i].second;
        if (key->type != T_string || value->type != T_string) {
            error = "header names and values must be strings";
            return false;
        }
        out += ((string_value_t *)key)->value;
        out += ": ";
        out += ((string_value_t *)value)->value;
        out += "\r\n";
    }
    return true;
}

bool body_value(value_t *value, std::string &out, const char *who) {
    if (!value || value->type == T_null) {
        out.clear();
        return true;
    }
    if (value->type == T_string) {
        out = ((string_value_t *)value)->value;
        return true;
    }
    if (value->type == T_bytes) {
        out = ((bytes_value_t *)value)->value;
        return true;
    }
    yyerror("%s body must be string or bytes", who);
    return false;
}

const char *reason_phrase(int status) {
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "";
    }
}

struct url_parts {
    std::string host;
    int port = 80;
    std::string path = "/";
    bool tls = false;
};


std::string connection_key(const url_parts &url, bool verify,
                           const std::string &ca_file) {
    std::string key = url.tls ? "https://" : "http://";
    key += url.host;
    key += ":";
    key += std::to_string(url.port);
    if (url.tls) {
        key += verify ? "|verify" : "|noverify";
        key += "|";
        key += ca_file;
    }
    return key;
}


bool parse_url(const std::string &url, url_parts &parts, std::string &error) {
    size_t scheme_length = 0;
    if (url.rfind("http://", 0) == 0) {
        parts.tls = false;
        parts.port = 80;
        scheme_length = 7;
    } else if (url.rfind("https://", 0) == 0) {
        parts.tls = true;
        parts.port = 443;
        scheme_length = 8;
    } else {
        error = "only http:// and https:// URLs are supported";
        return false;
    }
    std::string rest = url.substr(scheme_length);
    if (rest.empty()) {
        error = "URL is missing a host";
        return false;
    }

    size_t slash = rest.find('/');
    std::string authority =
        slash == std::string::npos ? rest : rest.substr(0, slash);
    parts.path =
        slash == std::string::npos ? "/" : rest.substr(slash);
    if (parts.path.empty())
        parts.path = "/";

    if (authority.empty()) {
        error = "URL is missing a host";
        return false;
    }
    if (authority.find('@') != std::string::npos) {
        error = "userinfo in URLs is not supported";
        return false;
    }

    std::string port_text;
    if (authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) {
            error = "invalid IPv6 host";
            return false;
        }
        parts.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                error = "invalid URL authority";
                return false;
            }
            port_text = authority.substr(close + 2);
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            if (authority.find(':') != colon) {
                error = "IPv6 hosts must use brackets";
                return false;
            }
            parts.host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
        } else {
            parts.host = authority;
        }
    }

    if (parts.host.empty()) {
        error = "URL is missing a host";
        return false;
    }
    if (!port_text.empty()) {
        char *end = NULL;
        errno = 0;
        long value = std::strtol(port_text.c_str(), &end, 10);
        if (errno != 0 || !end || *end != '\0' || value <= 0 ||
            value > 65535) {
            error = "invalid URL port";
            return false;
        }
        parts.port = (int)value;
    }
    return true;
}

bool read_message(value_t *socket, bool request, long long timeout_ms,
                  http1::message &message, bool *clean_eof = NULL,
                  bool *timed_out = NULL, value_t *on_body = NULL,
                  std::size_t max_body_bytes = 16 * 1024 * 1024,
                  std::size_t max_header_bytes = 64 * 1024,
                  std::size_t max_headers = 100) {
    if (clean_eof)
        *clean_eof = false;
    if (timed_out)
        *timed_out = false;
    http1::parser_options parser_options;
    parser_options.max_body_bytes = max_body_bytes;
    parser_options.max_header_bytes = max_header_bytes;
    parser_options.max_headers = max_headers;
    http1::parser parser(request, parser_options);
    bool sink_failed = false;
    if (on_body) {
        parser.set_body_sink([&](const char *data, std::size_t length) {
            value_t *chunk = newbytesval(std::string(data, length));
            value_t *result =
                skein_call_function(on_body, std::vector<value_t *>{chunk});
            if (!result) {
                sink_failed = true;
                return false;
            }
            freeval(result);
            return true;
        });
    }
    for (;;) {
        if (!parser.messages().empty()) {
            message = parser.messages()[0];
            return true;
        }

        std::vector<value_t *> args;
        args.push_back(dupval(socket));
        args.push_back(newintval(8192));
        args.push_back(newintval(timeout_ms));
        value_t *chunk = NULL;
        if (timed_out) {
            eval_capture_snapshot snapshot;
            eval_capture_begin(snapshot);
            chunk = call_io(B_io_read, args);
            eval_capture_result captured = eval_capture_end(snapshot);
            if (captured.failed) {
                std::string message = error_message_of(captured.error);
                freeval(captured.error);
                if (message.find("timed out") != std::string::npos) {
                    *timed_out = true;
                    return true;
                }
                yyerror("http: %s", message.c_str());
                return false;
            }
        } else {
            chunk = call_io(B_io_read, args);
        }
        if (!chunk)
            return false;

        if (chunk->type == T_null) {
            freeval(chunk);
            if (!parser.finish()) {
                if (sink_failed)
                    return false;
                if (!parser.error().empty())
                    yyerror("http: incomplete message: %s",
                            parser.error().c_str());
                else
                    yyerror("http: incomplete message");
                return false;
            }
            break;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            yyerror("http: read returned non-bytes");
            return false;
        }

        std::string data = ((bytes_value_t *)chunk)->value;
        freeval(chunk);
        if (!parser.execute(data)) {
            if (sink_failed)
                return false;
            yyerror("http: parse error: %s", parser.error().c_str());
            return false;
        }
    }

    if (parser.messages().empty()) {
        if (clean_eof && parser.error().empty()) {
            *clean_eof = true;
            return true;
        }
        yyerror("http: incomplete message");
        return false;
    }
    message = parser.messages()[0];
    return true;
}

value_t *headers_dict(const http1::message &message) {
    value_t *headers = (value_t *)newdictval();
    for (size_t i = 0; i < message.headers.size(); ++i) {
        std::string name = lower_ascii(message.headers[i].name);
        dict_set(headers, name, newstrval(message.headers[i].value.c_str()));
    }
    return headers;
}

value_t *set_cookie_list(const std::vector<http1::header> &headers);

value_t *make_response(const http1::message &message) {
    value_t *response = (value_t *)newdictval();
    dict_set(response, "status", newintval(message.status_code));
    dict_set(response, "status_text",
             newstrval(message.status_text.c_str()));
    dict_set(response, "headers", headers_dict(message));
    dict_set(response, "body", newbytesval(message.body));
    dict_set(response, "http_major", newintval(message.http_major));
    dict_set(response, "http_minor", newintval(message.http_minor));
    dict_set(response, "set_cookie", set_cookie_list(message.headers));
    return response;
}

value_t *make_request(value_t *client, const http1::message &message) {
    value_t *request = (value_t *)newdictval();
    dict_set(request, "method", newstrval(message.method.c_str()));
    dict_set(request, "path", newstrval(message.url.c_str()));
    dict_set(request, "headers", headers_dict(message));
    dict_set(request, "body", newbytesval(message.body));
    dict_set(request, "http_major", newintval(message.http_major));
    dict_set(request, "http_minor", newintval(message.http_minor));
    dict_set(request, "keep_alive", newboolval(message.keep_alive));
    dict_set(request, "socket", dupval(client));
    return request;
}

struct http1_pull_body : request_body {
    value_t *stream = NULL;
    http1::parser parser;
    std::deque<std::string> chunks;
    bool complete = false;
    bool failed = false;
    std::string error;
    long long timeout_ms = 0;

    http1_pull_body(value_t *socket, long long timeout,
                    const http1::parser_options &options)
        : stream(dupval(socket)), parser(true, options),
          timeout_ms(timeout) {
        parser.set_body_sink([this](const char *data, std::size_t length) {
            chunks.push_back(std::string(data, length));
            return true;
        });
    }

    ~http1_pull_body() {
        freeval(stream);
        stream = NULL;
    }

    bool next(value_t *&value, bool &done) override;
};

void fail_pull_body(http1_pull_body *body, const std::string &message) {
    body->failed = true;
    body->error = message;
    yyerror("std.http.serve: %s", message.c_str());
}

bool read_pull_body_chunk(http1_pull_body *body) {
    std::vector<value_t *> args;
    args.push_back(dupval(body->stream));
    args.push_back(newintval(8192));
    args.push_back(newintval(body->timeout_ms));
    value_t *chunk =
        await_task_value(call_io(B_io_read_async, args));
    if (!chunk)
        return false;

    if (chunk->type == T_null) {
        freeval(chunk);
        if (!body->parser.finish()) {
            std::string message = body->parser.error().empty()
                                      ? "incomplete request body"
                                      : body->parser.error();
            fail_pull_body(body, message);
            return false;
        }
        if (body->parser.messages().empty()) {
            fail_pull_body(body, "incomplete request body");
            return false;
        }
        body->complete = true;
        return true;
    }
    if (chunk->type != T_bytes) {
        freeval(chunk);
        fail_pull_body(body, "read returned non-bytes");
        return false;
    }

    std::string data = ((bytes_value_t *)chunk)->value;
    freeval(chunk);
    if (!body->parser.execute(data)) {
        std::string message = body->parser.error().empty()
                                  ? "request parse error"
                                  : body->parser.error();
        fail_pull_body(body, message);
        return false;
    }
    if (!body->parser.messages().empty())
        body->complete = true;
    return true;
}

bool http1_pull_body::next(value_t *&value, bool &done) {
    done = false;
    while (chunks.empty()) {
        if (failed) {
            yyerror("std.http.serve: %s", error.c_str());
            return false;
        }
        if (complete) {
            done = true;
            value = newnull();
            return true;
        }
        if (!read_pull_body_chunk(this))
            return false;
    }
    value = newbytesval(std::move(chunks.front()));
    chunks.pop_front();
    return true;
}

bool read_request_headers_async(
    value_t *socket, long long timeout_ms, http1::message &message,
    std::shared_ptr<http1_pull_body> &body, bool *clean_eof,
    const http1::parser_options &options) {
    if (clean_eof)
        *clean_eof = false;
    body = std::make_shared<http1_pull_body>(socket, timeout_ms, options);
    for (;;) {
        if (!body->parser.messages().empty()) {
            message = body->parser.messages()[0];
            body->complete = true;
            return true;
        }
        if (body->parser.headers_complete()) {
            message = body->parser.current_message();
            return true;
        }

        std::vector<value_t *> args;
        args.push_back(dupval(socket));
        args.push_back(newintval(8192));
        args.push_back(newintval(timeout_ms));
        value_t *chunk =
            await_task_value(call_io(B_io_read_async, args));
        if (!chunk)
            return false;

        if (chunk->type == T_null) {
            freeval(chunk);
            if (!body->parser.finish()) {
                yyerror("http: incomplete message: %s",
                        body->parser.error().c_str());
                return false;
            }
            if (body->parser.messages().empty()) {
                if (clean_eof && body->parser.error().empty()) {
                    *clean_eof = true;
                    return false;
                }
                yyerror("http: incomplete message");
                return false;
            }
            message = body->parser.messages()[0];
            body->complete = true;
            return true;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            yyerror("http: read returned non-bytes");
            return false;
        }

        std::string data = ((bytes_value_t *)chunk)->value;
        freeval(chunk);
        if (!body->parser.execute(data)) {
            yyerror("http: parse error: %s",
                    body->parser.error().c_str());
            return false;
        }
    }
}

value_t *make_pull_request(
    value_t *stream, const http1::message &message,
    const std::shared_ptr<http1_pull_body> &body) {
    value_t *request = make_request(stream, message);
    dict_set(request, "body_reader", new_request_body_value(body));
    return request;
}

std::string build_response(int status, value_t *headers,
                           const std::string &body, bool keep_alive,
                           std::string &error, bool chunked = false) {
    std::string response = "HTTP/1.1 ";
    response += std::to_string(status);
    response += " ";
    response += reason_phrase(status);
    response += "\r\n";
    if (chunked) {
        if (!header_exists(headers, "Content-Length") &&
            !header_exists(headers, "Transfer-Encoding"))
            response += "Transfer-Encoding: chunked\r\n";
    } else if (!body.empty() && !header_exists(headers, "Content-Length")) {
        response += "Content-Length: ";
        response += std::to_string(body.size());
        response += "\r\n";
    }
    if (!header_exists(headers, "Connection"))
        response += keep_alive ? "Connection: keep-alive\r\n"
                               : "Connection: close\r\n";
    if (!append_headers(response, headers, error))
        return std::string();
    response += "\r\n";
    if (!chunked)
        response += body;
    return response;
}

std::string build_request(const std::string &method, const url_parts &url,
                          value_t *headers, const std::string &body,
                          bool keep_alive, bool chunked = false,
                          bool upgrade = false) {
    std::string request;
    request += method;
    request += " ";
    request += url.path;
    request += " HTTP/1.1\r\n";

    if (!header_exists(headers, "Host")) {
        request += "Host: ";
        request += url.host;
        if (url.port != (url.tls ? 443 : 80)) {
            request += ":";
            request += std::to_string(url.port);
        }
        request += "\r\n";
    }
    if (!header_exists(headers, "Connection")) {
        if (upgrade)
            request += "Connection: Upgrade\r\n";
        else
            request += keep_alive ? "Connection: keep-alive\r\n"
                                  : "Connection: close\r\n";
    }
    if (chunked) {
        if (!header_exists(headers, "Content-Length") &&
            !header_exists(headers, "Transfer-Encoding"))
            request += "Transfer-Encoding: chunked\r\n";
    } else if (!body.empty() && !header_exists(headers, "Content-Length")) {
        request += "Content-Length: ";
        request += std::to_string(body.size());
        request += "\r\n";
    }

    std::string error;
    if (!append_headers(request, headers, error)) {
        yyerror("std.http: %s", error.c_str());
        return std::string();
    }
    request += "\r\n";
    if (!chunked)
        request += body;
    return request;
}

bool async_write(value_t *socket, const std::string &data,
                 long long timeout_ms);

std::string hex_size(std::size_t size) {
    char buffer[2 * sizeof(std::size_t) + 1];
    static const char digits[] = "0123456789abcdef";
    std::size_t pos = sizeof(buffer) - 1;
    buffer[pos] = '\0';
    if (size == 0)
        buffer[--pos] = '0';
    while (size) {
        buffer[--pos] = digits[size & 0xf];
        size >>= 4;
    }
    return std::string(buffer + pos);
}

bool write_stream_data(value_t *stream, const std::string &data,
                       long long timeout_ms, bool async) {
    if (async)
        return async_write(stream, data, timeout_ms);
    std::vector<value_t *> args;
    args.push_back(dupval(stream));
    args.push_back(newbytesval(data));
    args.push_back(newintval(timeout_ms));
    value_t *written = call_io(B_io_write_all, args);
    if (!written)
        return false;
    freeval(written);
    return true;
}

bool write_chunked_body(value_t *stream, const generator_ptr &generator,
                        long long timeout_ms, bool async) {
    for (;;) {
        value_t *value = NULL;
        bool done = false;
        if (!generator_next_value(generator, NULL, value, done))
            return false;
        if (done) {
            freeval(value);
            break;
        }
        std::string chunk;
        if (value) {
            if (value->type == T_string)
                chunk = ((string_value_t *)value)->value;
            else if (value->type == T_bytes)
                chunk = ((bytes_value_t *)value)->value;
            else {
                freeval(value);
                yyerror("std.http: body_stream must yield string or bytes");
                return false;
            }
            freeval(value);
        }
        if (chunk.empty())
            continue;
        std::string frame = hex_size(chunk.size()) + "\r\n" + chunk + "\r\n";
        if (!write_stream_data(stream, frame, timeout_ms, async))
            return false;
    }
    return write_stream_data(stream, "0\r\n\r\n", timeout_ms, async);
}

bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

value_t *copy_headers(value_t *headers) {
    value_t *output = (value_t *)newdictval();
    if (!headers || headers->type != T_dict)
        return output;
    dict_value_t *dict = (dict_value_t *)headers;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        value_t *key = dict->payload->items[i].first;
        value_t *value = dict->payload->items[i].second;
        if (key->type == T_string && value->type == T_string)
            dict_set(output, ((string_value_t *)key)->value, dupval(value));
    }
    return output;
}

value_t *redirect_headers(value_t *headers, bool cross_origin) {
    value_t *output = (value_t *)newdictval();
    if (!headers || headers->type != T_dict)
        return output;
    dict_value_t *dict = (dict_value_t *)headers;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        value_t *key = dict->payload->items[i].first;
        value_t *value = dict->payload->items[i].second;
        if (key->type != T_string || value->type != T_string)
            continue;
        std::string name = lower_ascii(((string_value_t *)key)->value);
        if (name == "host" || name == "content-length" ||
            name == "transfer-encoding" || name == "connection" ||
            name == "upgrade" ||
            (cross_origin && (name == "authorization" || name == "cookie")))
            continue;
        dict_set(output, name, dupval(value));
    }
    return output;
}

bool same_http_origin(const std::string &left, const std::string &right) {
    url_parts a, b;
    std::string error;
    if (!parse_url(left, a, error) || !parse_url(right, b, error))
        return false;
    return a.tls == b.tls && a.host == b.host && a.port == b.port;
}

std::string resolve_redirect_url(const std::string &base,
                                 const std::string &location) {
    std::string target = location;
    size_t fragment = target.find('#');
    if (fragment != std::string::npos)
        target = target.substr(0, fragment);
    if (target.empty())
        return base;
    if (target.rfind("http://", 0) == 0 ||
        target.rfind("https://", 0) == 0)
        return target;

    url_parts parts;
    std::string error;
    if (!parse_url(base, parts, error))
        return target;
    std::string scheme = parts.tls ? "https" : "http";
    std::string authority = parts.host;
    bool default_port = (parts.tls && parts.port == 443) ||
                        (!parts.tls && parts.port == 80);
    if (!default_port)
        authority += ":" + std::to_string(parts.port);
    if (target.rfind("//", 0) == 0)
        return scheme + ":" + target;
    if (target[0] == '/')
        return scheme + "://" + authority + target;
    if (target[0] == '?')
        return scheme + "://" + authority + parts.path + target;

    std::string directory = parts.path;
    size_t slash = directory.rfind('/');
    directory = slash == std::string::npos
                    ? "/"
                    : directory.substr(0, slash + 1);
    std::string path = directory + target;
    std::vector<std::string> segments;
    size_t i = 0;
    while (i <= path.size()) {
        size_t next = path.find('/', i);
        std::string segment =
            path.substr(i, next == std::string::npos ? std::string::npos
                                                     : next - i);
        if (segment == "..") {
            if (!segments.empty())
                segments.pop_back();
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(segment);
        }
        if (next == std::string::npos)
            break;
        i = next + 1;
    }
    std::string normalized = "/";
    for (size_t j = 0; j < segments.size(); ++j) {
        if (j > 0)
            normalized += "/";
        normalized += segments[j];
    }
    if (!segments.empty() && !path.empty() && path.back() == '/')
        normalized += "/";
    return scheme + "://" + authority + normalized;
}

value_t *redirect_options(value_t *base, value_t *headers,
                          bool keep_body_stream) {
    value_t *output = (value_t *)newdictval();
    if (base && base->type == T_dict) {
        dict_value_t *dict = (dict_value_t *)base;
        for (size_t i = 0; i < dict->payload->items.size(); ++i) {
            value_t *key = dict->payload->items[i].first;
            value_t *value = dict->payload->items[i].second;
            if (key->type != T_string)
                continue;
            std::string name = ((string_value_t *)key)->value;
            if (name == "headers" || name == "body" ||
                name == "body_stream" || name == "follow_redirects" ||
                name == "max_redirects")
                continue;
            dict_set(output, name, dupval(value));
        }
    }
    dict_set(output, "headers", dupval(headers));
    if (keep_body_stream) {
        value_t *stream = dict_get(base, "body_stream");
        if (stream)
            dict_set(output, "body_stream", dupval(stream));
    }
    return output;
}

void dict_delete(value_t *dict, const std::string &key) {
    if (!dict || dict->type != T_dict)
        return;
    dict_value_t *object = (dict_value_t *)dict;
    for (size_t i = 0; i < object->payload->items.size(); ++i) {
        value_t *item_key = object->payload->items[i].first;
        if (item_key->type == T_string &&
            key == ((string_value_t *)item_key)->value) {
            freeval(object->payload->items[i].first);
            freeval(object->payload->items[i].second);
            object->payload->items.erase(
                object->payload->items.begin() + (std::ptrdiff_t)i);
            return;
        }
    }
}

bool apply_response_decompression(value_t *response) {
    value_t *headers = dict_get(response, "headers");
    std::string encoding = header_value(headers, "Content-Encoding");
    if (encoding.empty() || lower_ascii(encoding) == "identity")
        return true;
    value_t *body = dict_get(response, "body");
    if (!body || body->type != T_bytes)
        return true;
    std::string output;
    std::string error;
    if (!decompress_content_encoding(
            encoding, ((bytes_value_t *)body)->value, output, error)) {
        yyerror("std.http: %s", error.c_str());
        return false;
    }
    dict_set(response, "body", newbytesval(output));
    dict_set(response, "decompressed", newboolval(true));
    if (headers && headers->type == T_dict) {
        dict_delete(headers, "Content-Encoding");
        dict_delete(headers, "Content-Length");
        dict_set(headers, "Content-Length",
                 newstrval(std::to_string(output.size()).c_str()));
    }
    return true;
}

value_t *set_cookie_list(const std::vector<http1::header> &headers) {
    value_t *list = (value_t *)newlistval();
    list_value_t *items = (list_value_t *)list;
    for (size_t i = 0; i < headers.size(); ++i) {
        if (lower_ascii(headers[i].name) != "set-cookie")
            continue;
        items->payload->items.push_back(
            newstrval(headers[i].value.c_str()));
    }
    return list;
}

value_t *do_request_once(const std::string &method,
                         const std::string &url_text, value_t *options,
                         value_t *body_override) {
    url_parts url;
    std::string error;
    if (!parse_url(url_text, url, error)) {
        yyerror("std.http: %s", error.c_str());
        return NULL;
    }

    value_t *headers = dict_get(options, "headers");
    if (headers && headers->type != T_dict) {
        yyerror("std.http: headers must be a dict");
        return NULL;
    }
    value_t *cookie_jar = dict_get(options, "cookie_jar");
    if (cookie_jar && cookie_jar->type != T_dict) {
        yyerror("std.http: cookie_jar must be a dict");
        return NULL;
    }
    bool decompress = option_bool(options, "decompress", false);
    value_t *decompress_value = dict_get(options, "decompress");
    if (decompress_value && decompress_value->type != T_bool) {
        yyerror("std.http: decompress must be a bool");
        return NULL;
    }
    value_t *request_headers = headers;
    value_t *owned_headers = NULL;
    if (cookie_jar && !header_exists(headers, "Cookie")) {
        std::string cookie = cookie_header_value(cookie_jar, url.host, url.path,
                                           url.tls);
        if (!cookie.empty()) {
            owned_headers = copy_headers(headers);
            dict_set(owned_headers, "Cookie",
                     newstrval(cookie.c_str()));
            request_headers = owned_headers;
        }
    }
    if (decompress && !header_exists(request_headers, "Accept-Encoding")) {
        std::string encodings = compression_accept_encoding();
        if (encodings.empty()) {
            yyerror("std.http: compression support is disabled");
            return NULL;
        }
        if (!owned_headers) {
            owned_headers = copy_headers(request_headers);
            request_headers = owned_headers;
        }
        dict_set(owned_headers, "Accept-Encoding",
                 newstrval(encodings.c_str()));
    }
    std::shared_ptr<value_t> headers_holder(owned_headers, freeval);

    std::string body;
    value_t *body_value_option =
        body_override ? body_override : dict_get(options, "body");
    if (!body_value(body_value_option, body, "std.http"))
        return NULL;

    value_t *body_stream_value = dict_get(options, "body_stream");
    generator_ptr body_generator;
    bool chunked = false;
    if (body_stream_value && body_stream_value->type != T_null) {
        if (!fiber_is_generator(body_stream_value)) {
            yyerror("std.http: body_stream must be a generator");
            return NULL;
        }
        if (!get_generator_value(body_stream_value, body_generator))
            return NULL;
        if (!body.empty()) {
            yyerror("std.http: body and body_stream cannot both be set");
            return NULL;
        }
        chunked = true;
    }

    long long timeout_ms = 0;
    value_t *timeout_value = dict_get(options, "timeout_ms");
    if (timeout_value) {
        if (timeout_value->type != T_int ||
            ((int_value_t *)timeout_value)->value < 0) {
            yyerror("std.http: timeout_ms must be a non-negative integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)timeout_value)->value;
    }

    value_t *on_body = dict_get(options, "on_body");
    if (on_body && on_body->type == T_null)
        on_body = NULL;
    if (on_body && !callable_value(on_body)) {
        yyerror("std.http: on_body must be a function");
        return NULL;
    }
    if (decompress && on_body) {
        yyerror("std.http: decompress cannot be combined with on_body");
        return NULL;
    }
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    value_t *max_body_value = dict_get(options, "max_body_bytes");
    if (max_body_value) {
        if (max_body_value->type != T_int ||
            ((int_value_t *)max_body_value)->value <= 0) {
            yyerror("std.http: max_body_bytes must be a positive integer");
            return NULL;
        }
        max_body_bytes =
            (std::size_t)((int_value_t *)max_body_value)->value;
    }
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    if (!positive_size_option(options, "max_header_bytes",
                              max_header_bytes) ||
        !positive_size_option(options, "max_headers", max_headers))
        return NULL;

    bool upgrade = false;
    value_t *upgrade_value = dict_get(options, "upgrade");
    if (upgrade_value) {
        if (upgrade_value->type != T_bool) {
            yyerror("std.http: upgrade must be a bool");
            return NULL;
        }
        upgrade = ((bool_value_t *)upgrade_value)->value;
    }

    bool tls_verify = true;
    std::string tls_ca_file;
    value_t *tls_options = dict_get(options, "tls");
    if (url.tls) {
        if (tls_options && tls_options->type != T_dict) {
            yyerror("std.http: tls options must be a dict");
            return NULL;
        }
        if (tls_options) {
            tls_verify = option_bool(tls_options, "verify", true);
            tls_ca_file = option_string(tls_options, "ca_file");
        }
    }

    std::string key = connection_key(url, tls_verify, tls_ca_file);
    std::string request = build_request(method, url, request_headers, body,
                                        true, chunked, upgrade);
    if (request.empty())
        return NULL;

    if (upgrade) {
        std::vector<value_t *> connect_args;
        connect_args.push_back(newstrval(url.host.c_str()));
        connect_args.push_back(newintval(url.port));
        connect_args.push_back(newintval(timeout_ms));
        value_t *client = call_net(B_net_tcp_connect, connect_args);
        if (!client)
            return NULL;
        value_t *stream = client;
        if (url.tls) {
#ifdef SKEIN_ENABLE_TLS
            std::vector<value_t *> tls_args;
            tls_args.push_back(dupval(client));
            tls_args.push_back(newstrval(url.host.c_str()));
            tls_args.push_back(tls_options ? dupval(tls_options)
                                           : newnull());
            stream = call_tls(B_tls_client, tls_args);
            if (!stream) {
                close_socket(client);
                freeval(client);
                return NULL;
            }
            freeval(client);
#else
            yyerror("std.http: https is not supported (TLS disabled)");
            close_socket(client);
            freeval(client);
            return NULL;
#endif
        }

        std::vector<value_t *> write_args;
        write_args.push_back(dupval(stream));
        write_args.push_back(newbytesval(request));
        write_args.push_back(newintval(timeout_ms));
        value_t *written = call_io(B_io_write_all, write_args);
        bool ok = written != NULL;
        if (written)
            freeval(written);
        http1::message message;
        if (ok)
            ok = read_message(stream, false, timeout_ms, message, NULL, NULL,
                              NULL, max_body_bytes, max_header_bytes,
                              max_headers);
        if (!ok) {
            close_socket(stream);
            freeval(stream);
            return NULL;
        }
        value_t *response = make_response(message);
        cookie_jar_update(cookie_jar, response, url.host, url.path,
                          url.tls);
        bool upgraded = message.status_code == 101;
        dict_set(response, "upgraded", newboolval(upgraded));
        if (upgraded) {
            dict_set(response, "socket", dupval(stream));
            freeval(stream);
            return response;
        }
        close_socket(stream);
        freeval(stream);
        return response;
    }

    bool idempotent = method == "GET" || method == "HEAD" ||
                      method == "OPTIONS" || method == "TRACE";
    bool retryable = idempotent && !on_body && !chunked;
    for (int attempt = 0; attempt < 2; ++attempt) {
        pooled_connection *connection =
            attempt == 0 ? pool_acquire(key) : NULL;
        bool reused = connection != NULL;
        value_t *stream = connection ? connection->stream : NULL;
        if (!connection) {
            std::vector<value_t *> connect_args;
            connect_args.push_back(newstrval(url.host.c_str()));
            connect_args.push_back(newintval(url.port));
            connect_args.push_back(newintval(timeout_ms));
            value_t *client = call_net(B_net_tcp_connect, connect_args);
            if (!client)
                return NULL;

            stream = client;
            if (url.tls) {
#ifdef SKEIN_ENABLE_TLS
                std::vector<value_t *> tls_args;
                tls_args.push_back(dupval(client));
                tls_args.push_back(newstrval(url.host.c_str()));
                tls_args.push_back(tls_options ? dupval(tls_options)
                                               : newnull());
                stream = call_tls(B_tls_client, tls_args);
                if (!stream) {
                    close_socket(client);
                    freeval(client);
                    return NULL;
                }
                freeval(client);
#else
                yyerror("std.http: https is not supported (TLS disabled)");
                close_socket(client);
                freeval(client);
                return NULL;
#endif
            }
            connection = pool_add(key, stream);
        }

        bool capture_errors = attempt == 0 && reused;
        eval_capture_snapshot snapshot;
        if (capture_errors)
            eval_capture_begin(snapshot);

        std::vector<value_t *> write_args;
        write_args.push_back(dupval(stream));
        write_args.push_back(newbytesval(request));
        write_args.push_back(newintval(timeout_ms));
        value_t *written = call_io(B_io_write_all, write_args);
        bool ok = written != NULL;
        if (written)
            freeval(written);
        if (ok && chunked)
            ok = write_chunked_body(stream, body_generator, timeout_ms, false);
        http1::message message;
        if (ok)
            ok = read_message(stream, false, timeout_ms, message, NULL, NULL,
                              on_body, max_body_bytes, max_header_bytes,
                              max_headers);

        eval_capture_result captured;
        if (capture_errors)
            captured = eval_capture_end(snapshot);

        if (!ok) {
            freeval(captured.error);
            pool_discard(connection);
            if (capture_errors && retryable && !g_interrupted)
                continue;
            return NULL;
        }

        freeval(captured.error);
        if (message.keep_alive)
            pool_release(connection);
        else
            pool_discard(connection);
        value_t *response = make_response(message);
        cookie_jar_update(cookie_jar, response, url.host, url.path,
                          url.tls);
        if (decompress && !apply_response_decompression(response)) {
            freeval(response);
            return NULL;
        }
        return response;
    }
    return NULL;
}

value_t *await_task_value(value_t *task_value) {
    if (!task_value)
        return NULL;
    task_ptr task;
    if (!get_task_value(task_value, task)) {
        freeval(task_value);
        return NULL;
    }
    freeval(task_value);
    return fiber_await_task(task);
}

value_t *async_connect(const std::string &host, int port,
                       long long timeout_ms) {
    std::vector<value_t *> args;
    args.push_back(newstrval(host.c_str()));
    args.push_back(newintval(port));
    args.push_back(newintval(timeout_ms));
    return await_task_value(call_net(B_net_tcp_connect_async, args));
}

bool async_write(value_t *socket, const std::string &data,
                 long long timeout_ms) {
    std::vector<value_t *> args;
    args.push_back(dupval(socket));
    args.push_back(newbytesval(data));
    args.push_back(newintval(timeout_ms));
    value_t *result =
        await_task_value(call_io(B_io_write_async, args));
    if (!result)
        return false;
    bool ok = result->type == T_int &&
              ((int_value_t *)result)->value == (long long)data.size();
    freeval(result);
    if (!ok) {
        yyerror("std.http: short async write");
        return false;
    }
    return true;
}

bool read_message_async(value_t *socket, bool request,
                        long long timeout_ms, http1::message &message,
                        value_t *on_body = NULL,
                        std::size_t max_body_bytes = 16 * 1024 * 1024,
                        bool *clean_eof = NULL,
                        std::size_t max_header_bytes = 64 * 1024,
                        std::size_t max_headers = 100) {
    if (clean_eof)
        *clean_eof = false;
    http1::parser_options parser_options;
    parser_options.max_body_bytes = max_body_bytes;
    parser_options.max_header_bytes = max_header_bytes;
    parser_options.max_headers = max_headers;
    http1::parser parser(request, parser_options);
    bool sink_failed = false;
    if (on_body) {
        parser.set_body_sink([&](const char *data, std::size_t length) {
            value_t *chunk = newbytesval(std::string(data, length));
            value_t *result =
                skein_call_function(on_body, std::vector<value_t *>{chunk});
            if (!result) {
                sink_failed = true;
                return false;
            }
            freeval(result);
            return true;
        });
    }
    for (;;) {
        if (!parser.messages().empty()) {
            message = parser.messages()[0];
            return true;
        }

        std::vector<value_t *> args;
        args.push_back(dupval(socket));
        args.push_back(newintval(8192));
        args.push_back(newintval(timeout_ms));
        value_t *chunk =
            await_task_value(call_io(B_io_read_async, args));
        if (!chunk)
            return false;

        if (chunk->type == T_null) {
            freeval(chunk);
            if (!parser.finish()) {
                if (sink_failed)
                    return false;
                if (!parser.error().empty())
                    yyerror("http: incomplete message: %s",
                            parser.error().c_str());
                else
                    yyerror("http: incomplete message");
                return false;
            }
            break;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            yyerror("http: read returned non-bytes");
            return false;
        }

        std::string data = ((bytes_value_t *)chunk)->value;
        freeval(chunk);
        if (!parser.execute(data)) {
            if (sink_failed)
                return false;
            yyerror("http: parse error: %s", parser.error().c_str());
            return false;
        }
    }

    if (parser.messages().empty()) {
        if (clean_eof && parser.error().empty()) {
            *clean_eof = true;
            return true;
        }
        yyerror("http: incomplete message");
        return false;
    }
    message = parser.messages()[0];
    return true;
}

value_t *do_request(const std::string &method, const std::string &url_text,
                    value_t *options, value_t *body_override) {
    value_t *follow_value = dict_get(options, "follow_redirects");
    if (follow_value && follow_value->type != T_bool) {
        yyerror("std.http: follow_redirects must be a bool");
        return NULL;
    }
    bool follow = follow_value &&
                  ((bool_value_t *)follow_value)->value;
    long long max_redirects = 5;
    value_t *max_value = dict_get(options, "max_redirects");
    if (max_value) {
        if (max_value->type != T_int ||
            ((int_value_t *)max_value)->value < 0) {
            yyerror("std.http: max_redirects must be a non-negative integer");
            return NULL;
        }
        max_redirects = ((int_value_t *)max_value)->value;
    }
    if (!follow)
        return do_request_once(method, url_text, options, body_override);

    value_t *headers = copy_headers(dict_get(options, "headers"));
    value_t *body = body_override ? dupval(body_override) : NULL;
    if (!body) {
        value_t *option_body = dict_get(options, "body");
        if (option_body && option_body->type != T_null)
            body = dupval(option_body);
    }
    value_t *stream_value = dict_get(options, "body_stream");
    bool keep_stream = stream_value && stream_value->type != T_null;
    std::string method_cur = method;
    std::string url_cur = url_text;
    int redirects = 0;

    for (;;) {
        value_t *hop = redirect_options(options, headers, keep_stream);
        value_t *response = do_request_once(method_cur, url_cur, hop, body);
        freeval(hop);
        if (!response) {
            freeval(headers);
            freeval(body);
            return NULL;
        }

        value_t *status_value = dict_get(response, "status");
        int status = status_value && status_value->type == T_int
                         ? (int)((int_value_t *)status_value)->value
                         : 0;
        std::string location;
        if (is_redirect_status(status))
            location = header_value(dict_get(response, "headers"),
                                    "Location");
        if (!is_redirect_status(status) || location.empty()) {
            freeval(headers);
            freeval(body);
            return response;
        }
        if (redirects >= max_redirects) {
            freeval(response);
            freeval(headers);
            freeval(body);
            yyerror("std.http: too many redirects");
            return NULL;
        }

        bool drop_body = status == 303 ||
                         ((status == 301 || status == 302) &&
                          method_cur == "POST");
        if (drop_body) {
            method_cur = "GET";
            freeval(body);
            body = NULL;
            keep_stream = false;
        } else if (keep_stream) {
            freeval(response);
            freeval(headers);
            freeval(body);
            yyerror("std.http: body_stream cannot be replayed across "
                    "redirects");
            return NULL;
        }

        std::string next_url = resolve_redirect_url(url_cur, location);
        bool cross_origin = !same_http_origin(url_cur, next_url);
        value_t *next_headers = redirect_headers(headers, cross_origin);
        freeval(headers);
        headers = next_headers;
        freeval(response);
        url_cur = next_url;
        ++redirects;
    }
}

value_t *do_request_async_once(const std::string &method,
                               const std::string &url_text,
                               value_t *options, value_t *body_override) {
    url_parts url;
    std::string error;
    if (!parse_url(url_text, url, error)) {
        yyerror("std.http: %s", error.c_str());
        return NULL;
    }
    bool tls_verify = true;
    std::string tls_ca_file;
    if (url.tls) {
        value_t *tls_options = dict_get(options, "tls");
        if (tls_options && tls_options->type != T_dict) {
            yyerror("std.http: tls options must be a dict");
            return NULL;
        }
        if (tls_options) {
            tls_verify = option_bool(tls_options, "verify", true);
            tls_ca_file = option_string(tls_options, "ca_file");
        }
    }

    value_t *headers = dict_get(options, "headers");
    if (headers && headers->type != T_dict) {
        yyerror("std.http: headers must be a dict");
        return NULL;
    }
    value_t *cookie_jar = dict_get(options, "cookie_jar");
    if (cookie_jar && cookie_jar->type != T_dict) {
        yyerror("std.http: cookie_jar must be a dict");
        return NULL;
    }
    bool decompress = option_bool(options, "decompress", false);
    value_t *decompress_value = dict_get(options, "decompress");
    if (decompress_value && decompress_value->type != T_bool) {
        yyerror("std.http: decompress must be a bool");
        return NULL;
    }
    value_t *request_headers = headers;
    value_t *owned_headers = NULL;
    if (cookie_jar && !header_exists(headers, "Cookie")) {
        std::string cookie = cookie_header_value(cookie_jar, url.host, url.path,
                                           url.tls);
        if (!cookie.empty()) {
            owned_headers = copy_headers(headers);
            dict_set(owned_headers, "Cookie",
                     newstrval(cookie.c_str()));
            request_headers = owned_headers;
        }
    }
    if (decompress && !header_exists(request_headers, "Accept-Encoding")) {
        std::string encodings = compression_accept_encoding();
        if (encodings.empty()) {
            yyerror("std.http: compression support is disabled");
            return NULL;
        }
        if (!owned_headers) {
            owned_headers = copy_headers(request_headers);
            request_headers = owned_headers;
        }
        dict_set(owned_headers, "Accept-Encoding",
                 newstrval(encodings.c_str()));
    }

    std::string body;
    value_t *body_option =
        body_override ? body_override : dict_get(options, "body");
    if (!body_value(body_option, body, "std.http"))
        return NULL;

    value_t *body_stream_value = dict_get(options, "body_stream");
    generator_ptr body_generator;
    bool chunked = false;
    if (body_stream_value && body_stream_value->type != T_null) {
        if (!fiber_is_generator(body_stream_value)) {
            yyerror("std.http: body_stream must be a generator");
            return NULL;
        }
        if (!get_generator_value(body_stream_value, body_generator))
            return NULL;
        if (!body.empty()) {
            yyerror("std.http: body and body_stream cannot both be set");
            return NULL;
        }
        chunked = true;
    }

    long long timeout_ms = 0;
    value_t *timeout_value = dict_get(options, "timeout_ms");
    if (timeout_value) {
        if (timeout_value->type != T_int ||
            ((int_value_t *)timeout_value)->value < 0) {
            yyerror("std.http: timeout_ms must be a non-negative integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)timeout_value)->value;
    }

    value_t *on_body = dict_get(options, "on_body");
    if (on_body && on_body->type == T_null)
        on_body = NULL;
    if (on_body && !callable_value(on_body)) {
        yyerror("std.http: on_body must be a function");
        return NULL;
    }
    if (decompress && on_body) {
        yyerror("std.http: decompress cannot be combined with on_body");
        return NULL;
    }
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    value_t *max_body_value = dict_get(options, "max_body_bytes");
    if (max_body_value) {
        if (max_body_value->type != T_int ||
            ((int_value_t *)max_body_value)->value <= 0) {
            yyerror("std.http: max_body_bytes must be a positive integer");
            return NULL;
        }
        max_body_bytes =
            (std::size_t)((int_value_t *)max_body_value)->value;
    }
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    if (!positive_size_option(options, "max_header_bytes",
                              max_header_bytes) ||
        !positive_size_option(options, "max_headers", max_headers))
        return NULL;

    bool upgrade = false;
    value_t *upgrade_value = dict_get(options, "upgrade");
    if (upgrade_value) {
        if (upgrade_value->type != T_bool) {
            yyerror("std.http: upgrade must be a bool");
            return NULL;
        }
        upgrade = ((bool_value_t *)upgrade_value)->value;
    }

    std::string request = build_request(method, url, request_headers, body,
                                        true, chunked, upgrade);
    freeval(owned_headers);
    if (request.empty())
        return NULL;

    std::string host = url.host;
    std::string request_path = url.path;
    value_t *jar_owned = cookie_jar ? dupval(cookie_jar) : NULL;
    int port = url.port;
    bool use_tls = url.tls;
    std::string key = connection_key(url, tls_verify, tls_ca_file);
    bool idempotent = method == "GET" || method == "HEAD" ||
                      method == "OPTIONS" || method == "TRACE";
    bool retryable = idempotent && !on_body && !chunked;
    value_t *on_body_owned = on_body ? dupval(on_body) : NULL;
    task_ptr task = fiber_start_async(
        [host, port, request, timeout_ms, use_tls, tls_verify,
         tls_ca_file, key, retryable, on_body_owned, max_body_bytes,
         body_generator, chunked, upgrade, jar_owned, request_path,
         decompress, max_header_bytes, max_headers]() {
        std::shared_ptr<value_t> on_body_holder;
        if (on_body_owned)
            on_body_holder =
                std::shared_ptr<value_t>(on_body_owned, freeval);
        std::shared_ptr<value_t> jar_holder;
        if (jar_owned)
            jar_holder = std::shared_ptr<value_t>(jar_owned, freeval);
        if (upgrade) {
            value_t *client = async_connect(host, port, timeout_ms);
            if (!client)
                return (value_t *)NULL;
            value_t *stream = client;
            if (use_tls) {
#ifdef SKEIN_ENABLE_TLS
                value_t *tls_options = (value_t *)newdictval();
                dict_set(tls_options, "verify", newboolval(tls_verify));
                if (!tls_ca_file.empty())
                    dict_set(tls_options, "ca_file",
                             newstrval(tls_ca_file.c_str()));
                std::vector<value_t *> tls_args;
                tls_args.push_back(dupval(client));
                tls_args.push_back(newstrval(host.c_str()));
                tls_args.push_back(tls_options);
                stream = call_tls(B_tls_client, tls_args);
                if (!stream) {
                    close_socket(client);
                    freeval(client);
                    return (value_t *)NULL;
                }
                freeval(client);
#else
                yyerror("std.http: https is not supported (TLS disabled)");
                close_socket(client);
                freeval(client);
                return (value_t *)NULL;
#endif
            }
            bool ok = async_write(stream, request, timeout_ms);
            http1::message message;
            if (ok)
                ok = read_message_async(
                    stream, false, timeout_ms, message, on_body_owned,
                    max_body_bytes, NULL, max_header_bytes, max_headers);
            if (!ok) {
                close_socket(stream);
                freeval(stream);
                return (value_t *)NULL;
            }
            value_t *response = make_response(message);
            cookie_jar_update(jar_owned, response, host, request_path,
                              use_tls);
            bool upgraded = message.status_code == 101;
            dict_set(response, "upgraded", newboolval(upgraded));
            if (upgraded) {
                dict_set(response, "socket", dupval(stream));
                freeval(stream);
                return response;
            }
            close_socket(stream);
            freeval(stream);
            return response;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            pooled_connection *connection =
                attempt == 0 ? pool_acquire(key) : NULL;
            bool reused = connection != NULL;
            if (!connection) {
                value_t *client = async_connect(host, port, timeout_ms);
                if (!client)
                    return (value_t *)NULL;

                value_t *stream = client;
                if (use_tls) {
#ifdef SKEIN_ENABLE_TLS
                    value_t *tls_options = (value_t *)newdictval();
                    dict_set(tls_options, "verify", newboolval(tls_verify));
                    if (!tls_ca_file.empty())
                        dict_set(tls_options, "ca_file",
                                 newstrval(tls_ca_file.c_str()));
                    std::vector<value_t *> tls_args;
                    tls_args.push_back(dupval(client));
                    tls_args.push_back(newstrval(host.c_str()));
                    tls_args.push_back(tls_options);
                    stream = call_tls(B_tls_client, tls_args);
                    if (!stream) {
                        close_socket(client);
                        freeval(client);
                        return (value_t *)NULL;
                    }
                    freeval(client);
#else
                    yyerror("std.http: https is not supported "
                            "(TLS disabled)");
                    close_socket(client);
                    freeval(client);
                    return (value_t *)NULL;
#endif
                }
                connection = pool_add(key, stream);
            }

            bool capture_errors = attempt == 0 && reused;
            eval_capture_snapshot snapshot;
            if (capture_errors)
                eval_capture_begin(snapshot);

            value_t *stream = connection->stream;
            bool ok = async_write(stream, request, timeout_ms);
            if (ok && chunked)
                ok = write_chunked_body(stream, body_generator, timeout_ms,
                                        true);
            http1::message message;
            if (ok)
                ok = read_message_async(
                    stream, false, timeout_ms, message, on_body_owned,
                    max_body_bytes, NULL, max_header_bytes, max_headers);

            eval_capture_result captured;
            if (capture_errors)
                captured = eval_capture_end(snapshot);

            if (!ok) {
                freeval(captured.error);
                pool_discard(connection);
                if (capture_errors && retryable && !g_interrupted)
                    continue;
                return (value_t *)NULL;
            }

            freeval(captured.error);
            if (message.keep_alive)
                pool_release(connection);
            else
                pool_discard(connection);
            value_t *response = make_response(message);
            cookie_jar_update(jar_owned, response, host, request_path,
                              use_tls);
            if (decompress && !apply_response_decompression(response)) {
                freeval(response);
                return (value_t *)NULL;
            }
            return response;
        }
        return (value_t *)NULL;
    });
    if (!task) {
        freeval(on_body_owned);
        freeval(jar_owned);
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *do_request_async(const std::string &method,
                          const std::string &url_text, value_t *options,
                          value_t *body_override) {
    value_t *follow_value = dict_get(options, "follow_redirects");
    if (follow_value && follow_value->type != T_bool) {
        yyerror("std.http: follow_redirects must be a bool");
        return NULL;
    }
    bool follow = follow_value &&
                  ((bool_value_t *)follow_value)->value;
    long long max_redirects = 5;
    value_t *max_value = dict_get(options, "max_redirects");
    if (max_value) {
        if (max_value->type != T_int ||
            ((int_value_t *)max_value)->value < 0) {
            yyerror("std.http: max_redirects must be a non-negative integer");
            return NULL;
        }
        max_redirects = ((int_value_t *)max_value)->value;
    }
    if (!follow)
        return do_request_async_once(method, url_text, options,
                                     body_override);

    std::string method_initial = method;
    std::string url_initial = url_text;
    value_t *owned_options = options ? dupval(options) : NULL;
    value_t *owned_body = body_override ? dupval(body_override) : NULL;
    task_ptr task = fiber_start_async(
        [method_initial, url_initial, owned_options, owned_body,
         max_redirects]() {
        auto releaser = [](value_t *value) { freeval(value); };
        typedef std::unique_ptr<value_t, decltype(releaser)> value_holder;
        value_holder options_holder(owned_options, releaser);
        value_holder body_holder(owned_body, releaser);
        value_t *headers =
            copy_headers(dict_get(owned_options, "headers"));
        value_holder headers_holder(headers, releaser);

        value_t *body = owned_body;
        if (!body) {
            value_t *option_body = dict_get(owned_options, "body");
            if (option_body && option_body->type != T_null)
                body = option_body;
        }
        value_t *stream_value = dict_get(owned_options, "body_stream");
        bool keep_stream = stream_value && stream_value->type != T_null;
        std::string method_cur = method_initial;
        std::string url_cur = url_initial;
        int redirects = 0;

        for (;;) {
            value_t *hop = redirect_options(
                owned_options, headers_holder.get(), keep_stream);
            value_t *task_value = do_request_async_once(
                method_cur, url_cur, hop, body);
            freeval(hop);
            value_t *response = await_task_value(task_value);
            if (!response)
                return (value_t *)NULL;

            value_t *status_value = dict_get(response, "status");
            int status = status_value && status_value->type == T_int
                             ? (int)((int_value_t *)status_value)->value
                             : 0;
            std::string location;
            if (is_redirect_status(status))
                location = header_value(dict_get(response, "headers"),
                                        "Location");
            if (!is_redirect_status(status) || location.empty())
                return response;
            if (redirects >= max_redirects) {
                freeval(response);
                yyerror("std.http: too many redirects");
                return (value_t *)NULL;
            }

            bool drop_body = status == 303 ||
                             ((status == 301 || status == 302) &&
                              method_cur == "POST");
            if (drop_body) {
                method_cur = "GET";
                body = NULL;
                body_holder.reset();
                keep_stream = false;
            } else if (keep_stream) {
                freeval(response);
                yyerror("std.http: body_stream cannot be replayed across "
                        "redirects");
                return (value_t *)NULL;
            }

            std::string next_url =
                resolve_redirect_url(url_cur, location);
            bool cross_origin = !same_http_origin(url_cur, next_url);
            value_t *next_headers =
                redirect_headers(headers_holder.get(), cross_origin);
            headers_holder.reset(next_headers);
            freeval(response);
            url_cur = next_url;
            ++redirects;
        }
    });
    if (!task) {
        freeval(owned_options);
        freeval(owned_body);
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *http_request_async(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.http.request_async expects 2 or 3 arguments");
        return NULL;
    }
    if (args[0]->type != T_string || args[1]->type != T_string) {
        yyerror("std.http.request_async expects method and URL strings");
        return NULL;
    }
    value_t *options = args.size() == 3 ? args[2] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http.request_async options must be a dict");
        return NULL;
    }
    return do_request_async(((string_value_t *)args[0])->value,
                            ((string_value_t *)args[1])->value, options,
                            NULL);
}

value_t *http_get_async(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_string) {
        yyerror("std.http.get_async expects a URL and optional options dict");
        return NULL;
    }
    value_t *options = args.size() == 2 ? args[1] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http.get_async options must be a dict");
        return NULL;
    }
    return do_request_async("GET", ((string_value_t *)args[0])->value,
                            options, NULL);
}

value_t *http_post_async(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 || args[0]->type != T_string) {
        yyerror("std.http.post_async expects a URL, body, and optional "
                "options dict");
        return NULL;
    }
    value_t *body = NULL;
    value_t *options = NULL;
    if (args.size() == 2) {
        if (args[1]->type == T_dict)
            options = args[1];
        else
            body = args[1];
    } else {
        body = args[1];
        options = args[2];
    }
    if (options && options->type != T_dict) {
        yyerror("std.http.post_async options must be a dict");
        return NULL;
    }
    return do_request_async("POST", ((string_value_t *)args[0])->value,
                            options, body);
}

value_t *http_request(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.http.request expects 2 or 3 arguments");
        return NULL;
    }
    if (args[0]->type != T_string || args[1]->type != T_string) {
        yyerror("std.http.request expects method and URL strings");
        return NULL;
    }
    value_t *options = args.size() == 3 ? args[2] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http.request options must be a dict");
        return NULL;
    }
    return do_request(((string_value_t *)args[0])->value,
                      ((string_value_t *)args[1])->value, options, NULL);
}

value_t *http_get(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_string) {
        yyerror("std.http.get expects a URL and optional options dict");
        return NULL;
    }
    value_t *options = args.size() == 2 ? args[1] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http.get options must be a dict");
        return NULL;
    }
    return do_request("GET", ((string_value_t *)args[0])->value, options,
                      NULL);
}

value_t *http_post(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 || args[0]->type != T_string) {
        yyerror("std.http.post expects a URL, body, and optional options dict");
        return NULL;
    }
    value_t *body = NULL;
    value_t *options = NULL;
    if (args.size() == 2) {
        if (args[1]->type == T_dict)
            options = args[1];
        else
            body = args[1];
    } else {
        body = args[1];
        options = args[2];
    }
    if (options && options->type != T_dict) {
        yyerror("std.http.post options must be a dict");
        return NULL;
    }
    return do_request("POST", ((string_value_t *)args[0])->value, options,
                      body);
}

value_t *http_listen(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 || args[0]->type != T_string ||
        args[1]->type != T_int) {
        yyerror("std.http.listen expects host, port, and optional timeout");
        return NULL;
    }
    long long port = ((int_value_t *)args[1])->value;
    if (port < 0 || port > 65535) {
        yyerror("std.http.listen port must be between 0 and 65535");
        return NULL;
    }
    long long timeout_ms = 0;
    if (args.size() == 3) {
        if (args[2]->type != T_int ||
            ((int_value_t *)args[2])->value < 0) {
            yyerror("std.http.listen timeout must be a non-negative integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)args[2])->value;
    }

    std::vector<value_t *> listen_args;
    listen_args.push_back(dupval(args[0]));
    listen_args.push_back(newintval(port));
    listen_args.push_back(newintval(timeout_ms));
    value_t *listener = call_net(B_net_tcp_listen, listen_args);
    if (!listener)
        return NULL;

    std::vector<value_t *> addr_args;
    addr_args.push_back(dupval(listener));
    value_t *addr = call_net(B_net_local_addr, addr_args);
    if (!addr) {
        close_socket(listener);
        freeval(listener);
        return NULL;
    }

    value_t *server = (value_t *)newdictval();
    dict_set(server, "socket", listener);
    dict_set(server, "host", dupval(args[0]));
    value_t *actual_port = dict_get(addr, "port");
    if (actual_port)
        dict_set(server, "port", dupval(actual_port));
    else
        dict_set(server, "port", newintval(port));
    freeval(addr);
    return server;
}

value_t *http_listen_tls(const std::vector<value_t *> &args) {
    if (args.size() < 4 || args.size() > 5 || args[0]->type != T_string ||
        args[1]->type != T_int || args[2]->type != T_string ||
        args[3]->type != T_string) {
        yyerror("std.http.listen_tls expects host, port, cert file, key file, "
                "and optional timeout");
        return NULL;
    }
#ifndef SKEIN_ENABLE_TLS
    yyerror("std.http.listen_tls: TLS is disabled");
    return NULL;
#else
    long long timeout_ms = 0;
    if (args.size() == 5) {
        if (args[4]->type != T_int ||
            ((int_value_t *)args[4])->value < 0) {
            yyerror("std.http.listen_tls timeout must be a non-negative "
                    "integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)args[4])->value;
    }

    std::vector<value_t *> listen_args;
    listen_args.push_back(dupval(args[0]));
    listen_args.push_back(dupval(args[1]));
    listen_args.push_back(newintval(timeout_ms));
    value_t *server = http_listen(listen_args);
    free_args(listen_args);
    if (!server)
        return NULL;
    dict_set(server, "cert", dupval(args[2]));
    dict_set(server, "key", dupval(args[3]));
    return server;
#endif
}

value_t *http_accept(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_dict) {
        yyerror("std.http.accept expects a server handle and optional timeout");
        return NULL;
    }
    value_t *socket = dict_get(args[0], "socket");
    if (!socket) {
        yyerror("std.http.accept expects a std.http server");
        return NULL;
    }
    long long timeout_ms = 0;
    if (args.size() == 2) {
        if (args[1]->type != T_int ||
            ((int_value_t *)args[1])->value < 0) {
            yyerror("std.http.accept timeout must be a non-negative integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)args[1])->value;
    }
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    if (!positive_size_option(args[0], "max_header_bytes",
                              max_header_bytes) ||
        !positive_size_option(args[0], "max_headers", max_headers) ||
        !positive_size_option(args[0], "max_body_bytes", max_body_bytes))
        return NULL;

    std::vector<value_t *> accept_args;
    accept_args.push_back(dupval(socket));
    accept_args.push_back(newintval(timeout_ms));
    value_t *client = call_net(B_net_accept, accept_args);
    if (!client)
        return NULL;

    value_t *stream = client;
    value_t *cert = dict_get(args[0], "cert");
    value_t *key = dict_get(args[0], "key");
    if (cert || key) {
        if (!cert || !key || cert->type != T_string ||
            key->type != T_string) {
            yyerror("std.http.accept: invalid TLS server certificate");
            close_socket(client);
            freeval(client);
            return NULL;
        }
#ifdef SKEIN_ENABLE_TLS
        std::vector<value_t *> tls_args;
        tls_args.push_back(dupval(client));
        tls_args.push_back(dupval(cert));
        tls_args.push_back(dupval(key));
        stream = call_tls(B_tls_server, tls_args);
        if (!stream) {
            close_socket(client);
            freeval(client);
            return NULL;
        }
        freeval(client);
#else
        yyerror("std.http.accept: TLS is disabled");
        close_socket(client);
        freeval(client);
        return NULL;
#endif
    }

    http1::message message;
    if (!read_message(stream, true, timeout_ms, message, NULL, NULL, NULL,
                      max_body_bytes, max_header_bytes, max_headers)) {
        close_socket(stream);
        freeval(stream);
        return NULL;
    }

    value_t *request = make_request(stream, message);
    freeval(stream);
    return request;
}

value_t *http_accept_async(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_dict) {
        yyerror("std.http.accept_async expects a server handle and optional "
                "timeout");
        return NULL;
    }
    value_t *socket = dict_get(args[0], "socket");
    if (!socket) {
        yyerror("std.http.accept_async expects a std.http server");
        return NULL;
    }
    std::string cert_file;
    std::string key_file;
    value_t *cert_value = dict_get(args[0], "cert");
    value_t *key_value = dict_get(args[0], "key");
    if (cert_value || key_value) {
        if (!cert_value || !key_value ||
            cert_value->type != T_string || key_value->type != T_string) {
            yyerror("std.http.accept_async: invalid TLS server certificate");
            return NULL;
        }
        cert_file = ((string_value_t *)cert_value)->value;
        key_file = ((string_value_t *)key_value)->value;
    }
    long long timeout_ms = 0;
    if (args.size() == 2) {
        if (args[1]->type != T_int ||
            ((int_value_t *)args[1])->value < 0) {
            yyerror("std.http.accept_async timeout must be a non-negative "
                    "integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)args[1])->value;
    }
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    if (!positive_size_option(args[0], "max_header_bytes",
                              max_header_bytes) ||
        !positive_size_option(args[0], "max_headers", max_headers) ||
        !positive_size_option(args[0], "max_body_bytes", max_body_bytes))
        return NULL;

    value_t *listener = dupval(socket);
    task_ptr task = fiber_start_async([listener, timeout_ms, cert_file,
                                       key_file, max_header_bytes,
                                       max_headers, max_body_bytes]() {
        std::vector<value_t *> accept_args;
        accept_args.push_back(dupval(listener));
        accept_args.push_back(newintval(timeout_ms));
        value_t *client = await_task_value(
            call_net(B_net_accept_async, accept_args));
        if (!client) {
            freeval(listener);
            return (value_t *)NULL;
        }

        value_t *stream = client;
        if (!cert_file.empty()) {
#ifdef SKEIN_ENABLE_TLS
            std::vector<value_t *> tls_args;
            tls_args.push_back(dupval(client));
            tls_args.push_back(newstrval(cert_file.c_str()));
            tls_args.push_back(newstrval(key_file.c_str()));
            stream = call_tls(B_tls_server, tls_args);
            if (!stream) {
                close_socket(client);
                freeval(client);
                freeval(listener);
                return (value_t *)NULL;
            }
            freeval(client);
#else
            yyerror("std.http.accept_async: TLS is disabled");
            close_socket(client);
            freeval(client);
            freeval(listener);
            return (value_t *)NULL;
#endif
        }

        http1::message message;
        if (!read_message_async(stream, true, timeout_ms, message, NULL,
                                max_body_bytes, NULL, max_header_bytes,
                                max_headers)) {
            close_socket(stream);
            freeval(stream);
            freeval(listener);
            return (value_t *)NULL;
        }

        value_t *request = make_request(stream, message);
        freeval(stream);
        freeval(listener);
        return request;
    });
    if (!task) {
        freeval(listener);
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *http_respond(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 4 || args[0]->type != T_dict ||
        args[1]->type != T_int) {
        yyerror("std.http.respond expects request, status, optional headers, "
                "optional body");
        return NULL;
    }
    value_t *socket = dict_get(args[0], "socket");
    if (!socket) {
        yyerror("std.http.respond expects an HTTP request handle");
        return NULL;
    }
    long long status = ((int_value_t *)args[1])->value;
    if (status < 100 || status > 999) {
        yyerror("std.http.respond status is invalid");
        return NULL;
    }

    value_t *headers = args.size() >= 3 ? args[2] : NULL;
    if (headers && headers->type != T_dict && headers->type != T_null) {
        yyerror("std.http.respond headers must be a dict or null");
        return NULL;
    }
    std::string body;
    generator_ptr body_generator;
    bool chunked = false;
    if (args.size() == 4) {
        if (fiber_is_generator(args[3])) {
            if (!get_generator_value(args[3], body_generator))
                return NULL;
            chunked = true;
        } else if (!body_value(args[3], body, "std.http.respond")) {
            return NULL;
        }
    }

    std::string error;
    std::string response =
        build_response((int)status, headers, body, false, error, chunked);
    if (response.empty()) {
        yyerror("std.http.respond: %s", error.c_str());
        return NULL;
    }

    std::vector<value_t *> write_args;
    write_args.push_back(dupval(socket));
    write_args.push_back(newbytesval(response));
    write_args.push_back(newintval(0));
    value_t *written = call_io(B_io_write_all, write_args);
    if (!written) {
        close_socket(socket);
        return NULL;
    }
    freeval(written);
    if (chunked &&
        !write_chunked_body(socket, body_generator, 0, false)) {
        close_socket(socket);
        return NULL;
    }
    close_socket(socket);
    return newboolval(true);
}

value_t *http_respond_async(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 4 || args[0]->type != T_dict ||
        args[1]->type != T_int) {
        yyerror("std.http.respond_async expects request, status, optional "
                "headers, optional body");
        return NULL;
    }
    value_t *socket = dict_get(args[0], "socket");
    if (!socket) {
        yyerror("std.http.respond_async expects an HTTP request handle");
        return NULL;
    }
    long long status = ((int_value_t *)args[1])->value;
    if (status < 100 || status > 999) {
        yyerror("std.http.respond_async status is invalid");
        return NULL;
    }

    value_t *headers = args.size() >= 3 ? args[2] : NULL;
    if (headers && headers->type != T_dict && headers->type != T_null) {
        yyerror("std.http.respond_async headers must be a dict or null");
        return NULL;
    }
    std::string body;
    generator_ptr body_generator;
    bool chunked = false;
    if (args.size() == 4) {
        if (fiber_is_generator(args[3])) {
            if (!get_generator_value(args[3], body_generator))
                return NULL;
            chunked = true;
        } else if (!body_value(args[3], body, "std.http.respond_async")) {
            return NULL;
        }
    }

    std::string error;
    std::string response =
        build_response((int)status, headers, body, false, error, chunked);
    if (response.empty()) {
        yyerror("std.http.respond_async: %s", error.c_str());
        return NULL;
    }

    value_t *client = dupval(socket);
    task_ptr task = fiber_start_async(
        [client, response, body_generator, chunked]() {
        bool ok = async_write(client, response, 0);
        if (ok && chunked)
            ok = write_chunked_body(client, body_generator, 0, true);
        close_socket(client);
        freeval(client);
        if (!ok)
            return (value_t *)NULL;
        return newboolval(true);
    });
    if (!task) {
        close_socket(client);
        freeval(client);
        return NULL;
    }
    return new_task_value(std::move(task));
}

void drain_http1_pull_body(const std::shared_ptr<http1_pull_body> &body) {
    if (!body || body->complete || body->failed)
        return;
    eval_capture_snapshot snapshot;
    eval_capture_begin(snapshot);
    for (;;) {
        value_t *chunk = NULL;
        bool done = false;
        if (!body->next(chunk, done)) {
            freeval(chunk);
            break;
        }
        freeval(chunk);
        if (done || body->complete || body->failed)
            break;
    }
    eval_capture_result captured = eval_capture_end(snapshot);
    freeval(captured.error);
}

struct http_server_state {
    long long max_requests = 0;
    long long max_per_connection = 100;
    long long idle_timeout_ms = 0;
    long long max_connections = 0;
    long long served = 0;
    bool stopping = false;
    bool stream_request_body = false;
    value_t *on_request_body = NULL;
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
};

void handle_http_server_connection(value_t *stream, value_t *handler,
                                   http_server_state *state) {
    bool connection_open = true;
    long long connection_requests = 0;
    std::chrono::steady_clock::time_point last_activity =
        std::chrono::steady_clock::now();
    while (connection_open && !state->stopping &&
           (state->max_requests == 0 ||
            state->served < state->max_requests)) {
        http1::message message;
        std::shared_ptr<http1_pull_body> pull_body;
        bool clean_eof = false;
        bool timed_out = false;
        bool ok = false;
        long long read_timeout = 100;
        if (state->idle_timeout_ms != 0)
            read_timeout = std::min<long long>(
                read_timeout, state->idle_timeout_ms);
        {
            eval_capture_snapshot snapshot;
            eval_capture_begin(snapshot);
            if (state->stream_request_body) {
                http1::parser_options options;
                options.max_body_bytes = state->max_body_bytes;
                options.max_header_bytes = state->max_header_bytes;
                options.max_headers = state->max_headers;
                ok = read_request_headers_async(
                    stream, read_timeout, message, pull_body, &clean_eof,
                    options);
            } else {
                ok = read_message_async(
                    stream, true, read_timeout, message,
                    state->on_request_body, state->max_body_bytes,
                    &clean_eof, state->max_header_bytes,
                    state->max_headers);
            }
            eval_capture_result captured = eval_capture_end(snapshot);
            if (!ok) {
                std::string text;
                if (captured.error && captured.error->type == T_error)
                    text = ((error_value_t *)captured.error)->message;
                freeval(captured.error);
                if (text.find("timed out") != std::string::npos)
                    timed_out = true;
                else if (!text.empty())
                    yyerror("std.http.serve: %s", text.c_str());
            }
        }
        if (timed_out) {
            if (state->stopping)
                break;
            if (state->idle_timeout_ms != 0 &&
                std::chrono::steady_clock::now() - last_activity >=
                    std::chrono::milliseconds(state->idle_timeout_ms))
                break;
            continue;
        }
        if (clean_eof || !ok)
            break;
        last_activity = std::chrono::steady_clock::now();
        ++state->served;
        ++connection_requests;
        if (state->max_requests != 0 &&
            state->served >= state->max_requests)
            state->stopping = true;

        value_t *request = state->stream_request_body
                               ? make_pull_request(stream, message, pull_body)
                               : make_request(stream, message);
        if (!request) {
            connection_open = false;
            break;
        }
        value_t *response =
            skein_call_function(handler, std::vector<value_t *>{request});
        if (response && is_task_value(response))
            response = await_task_value(response);
        if (!response) {
            connection_open = false;
            break;
        }
        if (response->type != T_dict) {
            yyerror("std.http.serve handler must return a response dict");
            freeval(response);
            connection_open = false;
            break;
        }

        value_t *status_value = dict_get(response, "status");
        if (!status_value || status_value->type != T_int) {
            yyerror("std.http.serve response status must be an integer");
            freeval(response);
            connection_open = false;
            break;
        }
        long long status = ((int_value_t *)status_value)->value;
        if (status < 100 || status > 999) {
            yyerror("std.http.serve response status is invalid");
            freeval(response);
            connection_open = false;
            break;
        }
        value_t *headers = dict_get(response, "headers");
        if (headers && headers->type != T_dict &&
            headers->type != T_null) {
            yyerror("std.http.serve response headers must be a dict");
            freeval(response);
            connection_open = false;
            break;
        }
        std::string body;
        generator_ptr body_generator;
        bool chunked = false;
        value_t *body_stream_value = dict_get(response, "body_stream");
        if (body_stream_value && body_stream_value->type != T_null) {
            if (!fiber_is_generator(body_stream_value) ||
                !get_generator_value(body_stream_value, body_generator)) {
                freeval(response);
                connection_open = false;
                break;
            }
            if (!body_value(dict_get(response, "body"), body,
                            "std.http.serve")) {
                freeval(response);
                connection_open = false;
                break;
            }
            if (!body.empty()) {
                yyerror("std.http.serve: body and body_stream cannot both "
                        "be set");
                freeval(response);
                connection_open = false;
                break;
            }
            chunked = true;
        } else if (!body_value(dict_get(response, "body"), body,
                               "std.http.serve")) {
            freeval(response);
            connection_open = false;
            break;
        }

        bool keep_alive =
            message.keep_alive && !headers_request_close(headers);
        if (state->stream_request_body &&
            (!pull_body || !pull_body->complete))
            keep_alive = false;
        if (state->max_requests != 0 &&
            state->served >= state->max_requests)
            keep_alive = false;
        if (state->max_per_connection != 0 &&
            connection_requests >= state->max_per_connection)
            keep_alive = false;

        std::string error;
        std::string response_text = build_response(
            (int)status, headers, body, keep_alive, error, chunked);
        if (response_text.empty()) {
            yyerror("std.http.serve: %s", error.c_str());
            freeval(response);
            connection_open = false;
            break;
        }
        freeval(response);

        if (!write_stream_data(stream, response_text, 0, true)) {
            connection_open = false;
            break;
        }
        if (chunked &&
            !write_chunked_body(stream, body_generator, 0, true)) {
            connection_open = false;
            break;
        }
        if (state->stream_request_body && !keep_alive && pull_body)
            drain_http1_pull_body(pull_body);
        if (!keep_alive)
            connection_open = false;
    }
    close_socket(stream);
    freeval(stream);
}

value_t *http_serve(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 5 || args[0]->type != T_dict ||
        (args[1]->type != T_func && args[1]->type != T_builtin &&
         args[1]->type != T_method)) {
        yyerror("std.http.serve expects server, handler, optional "
                "max_requests, max_per_connection, idle_timeout_ms");
        return NULL;
    }
    value_t *socket = dict_get(args[0], "socket");
    if (!socket) {
        yyerror("std.http.serve expects a std.http server");
        return NULL;
    }
    long long max_requests = 0;
    long long max_per_connection = 100;
    long long idle_timeout_ms = 0;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i]->type != T_int ||
            ((int_value_t *)args[i])->value < 0) {
            yyerror("std.http.serve limits must be non-negative integers");
            return NULL;
        }
        long long value = ((int_value_t *)args[i])->value;
        if (i == 2)
            max_requests = value;
        else if (i == 3)
            max_per_connection = value;
        else
            idle_timeout_ms = value;
    }

    std::string cert_file;
    std::string key_file;
    value_t *cert_value = dict_get(args[0], "cert");
    value_t *key_value = dict_get(args[0], "key");
    if (cert_value || key_value) {
        if (!cert_value || !key_value ||
            cert_value->type != T_string || key_value->type != T_string) {
            yyerror("std.http.serve: invalid TLS server certificate");
            return NULL;
        }
        cert_file = ((string_value_t *)cert_value)->value;
        key_file = ((string_value_t *)key_value)->value;
    }

    value_t *on_request_body = dict_get(args[0], "on_request_body");
    if (on_request_body && on_request_body->type == T_null)
        on_request_body = NULL;
    if (on_request_body && !callable_value(on_request_body)) {
        yyerror("std.http.serve on_request_body must be a function");
        return NULL;
    }
    bool stream_request_body = false;
    value_t *stream_body_value =
        dict_get(args[0], "stream_request_body");
    if (stream_body_value && stream_body_value->type != T_null) {
        if (stream_body_value->type != T_bool) {
            yyerror("std.http.serve stream_request_body must be a bool");
            return NULL;
        }
        stream_request_body =
            ((bool_value_t *)stream_body_value)->value;
    }
    if (stream_request_body && on_request_body) {
        yyerror("std.http.serve cannot combine stream_request_body with "
                "on_request_body");
        return NULL;
    }

    long long max_connections = 0;
    value_t *max_connections_value = dict_get(args[0], "max_connections");
    if (max_connections_value) {
        if (max_connections_value->type != T_int ||
            ((int_value_t *)max_connections_value)->value < 0) {
            yyerror("std.http.serve max_connections must be a "
                    "non-negative integer");
            return NULL;
        }
        max_connections = ((int_value_t *)max_connections_value)->value;
    }
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    if (!positive_size_option(args[0], "max_header_bytes",
                              max_header_bytes) ||
        !positive_size_option(args[0], "max_headers", max_headers) ||
        !positive_size_option(args[0], "max_body_bytes", max_body_bytes))
        return NULL;

    http_server_state state;
    state.max_requests = max_requests;
    state.max_per_connection = max_per_connection;
    state.idle_timeout_ms = idle_timeout_ms;
    state.max_connections = max_connections;
    state.stream_request_body = stream_request_body;
    state.on_request_body = on_request_body;
    state.max_header_bytes = max_header_bytes;
    state.max_headers = max_headers;
    state.max_body_bytes = max_body_bytes;

    value_t *handler = args[1];
    task_ptr server_task = fiber_start_async(
        [socket, handler, &state, cert_file, key_file]() {
        std::vector<task_ptr> connections;
        for (;;) {
            if (state.stopping)
                break;
            if (state.max_requests != 0 &&
                state.served >= state.max_requests) {
                state.stopping = true;
                break;
            }
            for (size_t i = 0; i < connections.size();) {
                if (task_get_state(connections[i]) != TASK_STATE_PENDING)
                    connections.erase(connections.begin()
                                      + (std::ptrdiff_t)i);
                else
                    ++i;
            }
            if (state.max_connections > 0 &&
                (long long)connections.size() >=
                    state.max_connections) {
                value_t *sleep_task = call_task_builtin(
                    B_task_sleep, {newintval(20)});
                value_t *slept = await_task_value(sleep_task);
                freeval(slept);
                continue;
            }

            std::vector<value_t *> accept_args;
            accept_args.push_back(dupval(socket));
            accept_args.push_back(newintval(50));
            value_t *accept_task =
                call_net(B_net_accept_async, accept_args);
            if (!accept_task)
                break;

            eval_capture_snapshot snapshot;
            eval_capture_begin(snapshot);
            value_t *client = await_task_value(accept_task);
            eval_capture_result captured = eval_capture_end(snapshot);
            if (!client) {
                bool timed_out = false;
                if (captured.failed && captured.error &&
                    captured.error->type == T_error) {
                    std::string message =
                        ((error_value_t *)captured.error)->message;
                    timed_out =
                        message.find("timed out") != std::string::npos;
                }
                if (timed_out) {
                    freeval(captured.error);
                    continue;
                }
                if (captured.failed && captured.error)
                    eval_raise(captured.error);
                else
                    freeval(captured.error);
                break;
            }
            freeval(captured.error);

            value_t *stream = client;
            if (!cert_file.empty()) {
#ifdef SKEIN_ENABLE_TLS
                std::vector<value_t *> tls_args;
                tls_args.push_back(dupval(client));
                tls_args.push_back(newstrval(cert_file.c_str()));
                tls_args.push_back(newstrval(key_file.c_str()));
                stream = call_tls(B_tls_server, tls_args);
                if (!stream) {
                    close_socket(client);
                    freeval(client);
                    continue;
                }
                freeval(client);
#else
                yyerror("std.http.serve: TLS is disabled");
                close_socket(client);
                freeval(client);
                continue;
#endif
            }

            task_ptr connection_task = fiber_start_async(
                [stream, handler, &state]() {
                handle_http_server_connection(stream, handler, &state);
                return (value_t *)NULL;
            });
            if (!connection_task) {
                close_socket(stream);
                freeval(stream);
                break;
            }
            connections.push_back(connection_task);
        }

        state.stopping = true;
        for (size_t i = 0; i < connections.size(); ++i) {
            if (task_get_state(connections[i]) != TASK_STATE_PENDING)
                continue;
            eval_capture_snapshot snapshot;
            eval_capture_begin(snapshot);
            value_t *result = fiber_await_task(connections[i]);
            eval_capture_result captured = eval_capture_end(snapshot);
            freeval(result);
            freeval(captured.error);
        }
        connections.clear();
        return newintval(state.served);
    });
    if (!server_task)
        return NULL;
    return await_task_value(new_task_value(std::move(server_task)));
}

value_t *http_upgrade(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_dict) {
        yyerror("std.http.upgrade expects a request and optional headers");
        return NULL;
    }
    value_t *socket = dict_get(args[0], "socket");
    if (!socket) {
        yyerror("std.http.upgrade expects an HTTP request handle");
        return NULL;
    }
    value_t *headers = args.size() == 2 ? args[1] : NULL;
    if (headers && headers->type != T_dict && headers->type != T_null) {
        yyerror("std.http.upgrade headers must be a dict or null");
        return NULL;
    }

    std::string upgrade = header_value(headers, "Upgrade");
    if (upgrade.empty())
        upgrade = header_value(dict_get(args[0], "headers"), "Upgrade");
    if (upgrade.empty()) {
        yyerror("std.http.upgrade requires an Upgrade header");
        return NULL;
    }

    std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Upgrade: ";
    response += upgrade;
    response += "\r\n";
    if (headers && headers->type == T_dict) {
        dict_value_t *dict = (dict_value_t *)headers;
        for (size_t i = 0; i < dict->payload->items.size(); ++i) {
            value_t *key = dict->payload->items[i].first;
            value_t *value = dict->payload->items[i].second;
            if (key->type != T_string || value->type != T_string) {
                yyerror("std.http.upgrade header names and values must be "
                        "strings");
                return NULL;
            }
            std::string name = ((string_value_t *)key)->value;
            std::string lower = lower_ascii(name);
            if (lower == "connection" || lower == "upgrade" ||
                lower == "content-length" ||
                lower == "transfer-encoding")
                continue;
            response += name;
            response += ": ";
            response += ((string_value_t *)value)->value;
            response += "\r\n";
        }
    }
    response += "\r\n";

    std::vector<value_t *> write_args;
    write_args.push_back(dupval(socket));
    write_args.push_back(newbytesval(response));
    write_args.push_back(newintval(0));
    value_t *written = call_io(B_io_write_all, write_args);
    if (!written)
        return NULL;
    freeval(written);
    return dupval(socket);
}

value_t *http_cookies(const std::vector<value_t *> &args) {
    if (args.size() != 1 || args[0]->type != T_dict) {
        yyerror("std.http.cookies expects an HTTP response");
        return NULL;
    }
    value_t *list = dict_get(args[0], "set_cookie");
    value_t *output = (value_t *)newlistval();
    if (!list || list->type != T_list)
        return output;
    list_value_t *source = (list_value_t *)list;
    list_value_t *target = (list_value_t *)output;
    for (size_t i = 0; i < source->payload->items.size(); ++i) {
        value_t *raw = source->payload->items[i];
        if (!raw || raw->type != T_string)
            continue;
        bool ok = false;
        value_t *cookie = parse_cookie_header(
            ((string_value_t *)raw)->value, std::string(), std::string(),
            ok);
        if (ok)
            target->payload->items.push_back(cookie);
        else
            freeval(cookie);
    }
    return output;
}

value_t *http_close(const std::vector<value_t *> &args) {
    if (args.size() != 1 || args[0]->type != T_dict) {
        yyerror("std.http.close expects a server or request handle");
        return NULL;
    }
    value_t *socket = dict_get(args[0], "socket");
    if (!socket) {
        yyerror("std.http.close expects a std.http handle");
        return NULL;
    }
    std::vector<value_t *> close_args;
    close_args.push_back(dupval(socket));
    value_t *closed = call_io(B_io_close, close_args);
    if (!closed)
        return NULL;
    return closed;
}

} // namespace

namespace {

std::string request_body_describe(const void *) {
    return "<request body>";
}

const native_type request_body_type = {
    "request body", request_body_describe
};

} // namespace

value_t *new_request_body_value(std::shared_ptr<request_body> body) {
    return newuserdataval(&request_body_type, std::move(body));
}

bool get_request_body_value(value_t *value,
                            std::shared_ptr<request_body> &body) {
    if (!value || value->type != T_userdata ||
        ((userdata_value_t *)value)->meta != &request_body_type) {
        yyerror("expected a request body reader, got %s",
                typename_of(value));
        return false;
    }
    body = std::static_pointer_cast<request_body>(
        ((userdata_value_t *)value)->data);
    if (!body) {
        yyerror("invalid request body reader");
        return false;
    }
    return true;
}

value_t *http_read_body(const std::vector<value_t *> &args) {
    if (args.size() != 1 || args[0]->type != T_dict) {
        yyerror("std.http.read_body expects a request dict");
        return NULL;
    }
    value_t *reader_value = dict_get(args[0], "body_reader");
    if (!reader_value) {
        yyerror("std.http.read_body expects a streaming request body");
        return NULL;
    }
    std::shared_ptr<request_body> body;
    if (!get_request_body_value(reader_value, body))
        return NULL;
    value_t *value = NULL;
    bool done = false;
    if (!body->next(value, done)) {
        freeval(value);
        return NULL;
    }
    if (done) {
        freeval(value);
        return newnull();
    }
    return value ? value : newnull();
}

void http_shutdown() {
    pool_shutdown();
}

struct value_t *call_http_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_http_request:
            return http_request(args);
        case B_http_get:
            return http_get(args);
        case B_http_post:
            return http_post(args);
        case B_http_request_async:
            return http_request_async(args);
        case B_http_get_async:
            return http_get_async(args);
        case B_http_post_async:
            return http_post_async(args);
        case B_http_listen:
            return http_listen(args);
        case B_http_listen_tls:
            return http_listen_tls(args);
        case B_http_accept:
            return http_accept(args);
        case B_http_accept_async:
            return http_accept_async(args);
        case B_http_respond:
            return http_respond(args);
        case B_http_respond_async:
            return http_respond_async(args);
        case B_http_serve:
            return http_serve(args);
        case B_http_upgrade:
            return http_upgrade(args);
        case B_http_cookies:
            return http_cookies(args);
        case B_http_read_body:
            return http_read_body(args);
        case B_http_close:
            return http_close(args);
        default:
            yyerror("unknown http function");
            return NULL;
    }
}
