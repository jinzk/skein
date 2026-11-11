#include "http.h"

#include "runtime_context.h"
#include "ast.h"
#include "compression.h"
#include "cookie.h"
#include "eval.h"
#include "fiber.h"
#include "event_loop.h"
#include "http1_parser.h"
#include "builtins_io.h"
#include "net.h"
#include "package.h"
#include "parser_lock.h"
#include "platform.h"
#include "process_spawn.h"
#include "repl.h"
#include "runtime_state.h"
#include "symbol.h"
#include "task.h"
#include "thread_channel.h"
#include "worker_pool.h"
#ifdef SKEIN_ENABLE_HTTP2
#include "http2.h"
#endif
#ifdef SKEIN_ENABLE_TLS
#include "tls.h"
#endif
#include "valuetype.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

value_t *await_task_value(value_t *task_value);
std::string http_platform_name();
bool request_unsupported_feature(value_t *request,
                                 std::string &message, bool allow_sse,
                                 bool allow_websocket = false);
std::shared_ptr<http_shared_state> map_shared_state_file(
    const std::string &path, bool create, std::string &error);

constexpr long long kHttpShutdownForceGraceMs = 5000;

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
    static thread_local std::vector<std::unique_ptr<pooled_connection>> pool;
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
                           const std::string &ca_file,
                           const std::string &cert_file,
                           const std::string &key_file) {
    std::string key = url.tls ? "https://" : "http://";
    key += url.host;
    key += ":";
    key += std::to_string(url.port);
    if (url.tls) {
        key += verify ? "|verify" : "|noverify";
        key += "|";
        key += ca_file;
        key += "|";
        key += cert_file;
        key += "|";
        key += key_file;
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
    value_t *peer = call_net(B_net_peer_addr, {dupval(client)});
    if (peer && peer->type == T_dict) {
        value_t *address = dict_get(peer, "address");
        value_t *port = dict_get(peer, "port");
        if (address)
            dict_set(request, "remote_addr", dupval(address));
        if (port)
            dict_set(request, "remote_port", dupval(port));
    }
    freeval(peer);
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
    std::string tls_cert_file;
    std::string tls_key_file;
    value_t *tls_options = dict_get(options, "tls");
    if (url.tls) {
        if (tls_options && tls_options->type != T_dict) {
            yyerror("std.http: tls options must be a dict");
            return NULL;
        }
        if (tls_options) {
            tls_verify = option_bool(tls_options, "verify", true);
            tls_ca_file = option_string(tls_options, "ca_file");
            tls_cert_file = option_string(tls_options, "cert_file");
            tls_key_file = option_string(tls_options, "key_file");
        }
    }

    std::string key =
        connection_key(url, tls_verify, tls_ca_file, tls_cert_file,
                       tls_key_file);
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
    std::string tls_cert_file;
    std::string tls_key_file;
    if (url.tls) {
        value_t *tls_options = dict_get(options, "tls");
        if (tls_options && tls_options->type != T_dict) {
            yyerror("std.http: tls options must be a dict");
            return NULL;
        }
        if (tls_options) {
            tls_verify = option_bool(tls_options, "verify", true);
            tls_ca_file = option_string(tls_options, "ca_file");
            tls_cert_file = option_string(tls_options, "cert_file");
            tls_key_file = option_string(tls_options, "key_file");
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
    std::string key =
        connection_key(url, tls_verify, tls_ca_file, tls_cert_file,
                       tls_key_file);
    bool idempotent = method == "GET" || method == "HEAD" ||
                      method == "OPTIONS" || method == "TRACE";
    bool retryable = idempotent && !on_body && !chunked;
    value_t *on_body_owned = on_body ? dupval(on_body) : NULL;
    task_ptr task = fiber_start_async(
        [host, port, request, timeout_ms, use_tls, tls_verify,
         tls_ca_file, tls_cert_file, tls_key_file, key, retryable,
         on_body_owned, max_body_bytes,
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
                if (!tls_cert_file.empty())
                    dict_set(tls_options, "cert_file",
                             newstrval(tls_cert_file.c_str()));
                if (!tls_key_file.empty())
                    dict_set(tls_options, "key_file",
                             newstrval(tls_key_file.c_str()));
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
                    if (!tls_cert_file.empty())
                        dict_set(tls_options, "cert_file",
                                 newstrval(tls_cert_file.c_str()));
                    if (!tls_key_file.empty())
                        dict_set(tls_options, "key_file",
                                 newstrval(tls_key_file.c_str()));
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

#ifdef SKEIN_ENABLE_TLS
value_t *build_server_tls_options(value_t *server) {
    value_t *client_ca = dict_get(server, "client_ca_file");
    value_t *client_auth = dict_get(server, "client_auth");
    value_t *alpn = dict_get(server, "alpn");
    if (!client_ca && !client_auth && !alpn)
        return NULL;
    value_t *options = (value_t *)newdictval();
    if (client_ca)
        dict_set(options, "client_ca_file", dupval(client_ca));
    if (client_auth)
        dict_set(options, "client_auth", dupval(client_auth));
    if (alpn)
        dict_set(options, "alpn", dupval(alpn));
    return options;
}
#endif

value_t *http_listen_tls(const std::vector<value_t *> &args) {
    if (args.size() < 4 || args.size() > 6 || args[0]->type != T_string ||
        args[1]->type != T_int || args[2]->type != T_string ||
        args[3]->type != T_string) {
        yyerror("std.http.listen_tls expects host, port, cert file, key file, "
                "optional timeout, and optional options");
        return NULL;
    }
#ifndef SKEIN_ENABLE_TLS
    yyerror("std.http.listen_tls: TLS is disabled");
    return NULL;
#else
    long long timeout_ms = 0;
    value_t *tls_options = NULL;
    if (args.size() >= 5) {
        if (args[4]->type == T_int) {
            if (((int_value_t *)args[4])->value < 0) {
                yyerror("std.http.listen_tls timeout must be a non-negative "
                        "integer");
                return NULL;
            }
            timeout_ms = ((int_value_t *)args[4])->value;
            if (args.size() == 6) {
                if (args[5]->type != T_dict) {
                    yyerror("std.http.listen_tls options must be a dict");
                    return NULL;
                }
                tls_options = args[5];
            }
        } else if (args[4]->type == T_dict) {
            if (args.size() == 6) {
                yyerror("std.http.listen_tls options must be the last "
                        "argument");
                return NULL;
            }
            tls_options = args[4];
        } else {
            yyerror("std.http.listen_tls expects an optional timeout or "
                    "options dict");
            return NULL;
        }
    }
    if (tls_options) {
        value_t *client_ca = dict_get(tls_options, "client_ca_file");
        value_t *client_auth = dict_get(tls_options, "client_auth");
        value_t *alpn = dict_get(tls_options, "alpn");
        if (client_ca && client_ca->type != T_string) {
            yyerror("std.http.listen_tls client_ca_file must be a string");
            return NULL;
        }
        if (client_auth && client_auth->type != T_string) {
            yyerror("std.http.listen_tls client_auth must be a string");
            return NULL;
        }
        if (alpn && alpn->type != T_list) {
            yyerror("std.http.listen_tls alpn must be a list");
            return NULL;
        }
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
    if (tls_options) {
        value_t *client_ca = dict_get(tls_options, "client_ca_file");
        value_t *client_auth = dict_get(tls_options, "client_auth");
        value_t *alpn = dict_get(tls_options, "alpn");
        if (client_ca)
            dict_set(server, "client_ca_file", dupval(client_ca));
        if (client_auth)
            dict_set(server, "client_auth", dupval(client_auth));
        if (alpn)
            dict_set(server, "alpn", dupval(alpn));
    }
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
        value_t *tls_options = build_server_tls_options(args[0]);
        if (tls_options)
            tls_args.push_back(tls_options);
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
#ifdef SKEIN_ENABLE_TLS
    value_t *tls_options =
        cert_file.empty() ? NULL : build_server_tls_options(args[0]);
#else
    value_t *tls_options = NULL;
#endif
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
                                       key_file, tls_options, max_header_bytes,
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
            if (tls_options)
                tls_args.push_back(tls_options);
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

struct http_connection_registry {
    std::mutex mutex;
    std::vector<value_t *> streams;
};

struct http_server_state {
    long long max_requests = 0;
    long long max_per_connection = 100;
    long long idle_timeout_ms = 0;
    long long max_connections = 0;
    bool stream_request_body = false;
    value_t *on_request_body = NULL;
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    bool http2_enabled = false;
    long long h2_max_concurrent_streams = 16;
    bool unsupported_501 = false;
    std::string unsupported_message;
    std::shared_ptr<http_shared_state> shared;
    /* Shared with reuse_port worker copies; set when a shutdown deadline is hit. */
    std::shared_ptr<std::atomic<bool>> force_stop =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<http_connection_registry> tracked_connections =
        std::make_shared<http_connection_registry>();
};

bool http_force_stopping(const http_server_state *state) {
    return state && state->force_stop && state->force_stop->load();
}

void http_close_tracked_connections(
    const std::shared_ptr<http_server_state> &state) {
    if (!state || !state->tracked_connections)
        return;
    std::vector<value_t *> streams;
    {
        std::lock_guard<std::mutex> lock(
            state->tracked_connections->mutex);
        streams = state->tracked_connections->streams;
    }
    for (value_t *stream : streams)
        close_socket(stream);
}

void handle_http_server_connection(value_t *stream, value_t *handler,
                                   http_server_state *state) {
    bool connection_open = true;
    long long connection_requests = 0;
    std::chrono::steady_clock::time_point last_activity =
        std::chrono::steady_clock::now();
    while (connection_open && !state->shared->stopping.load() &&
           !http_force_stopping(state) &&
           (state->max_requests == 0 ||
            state->shared->served.load() < state->max_requests)) {
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
            if (state->shared->stopping.load() ||
                http_force_stopping(state))
                break;
            if (state->idle_timeout_ms != 0 &&
                std::chrono::steady_clock::now() - last_activity >=
                    std::chrono::milliseconds(state->idle_timeout_ms))
                break;
            continue;
        }
        if (clean_eof || !ok)
            break;

        long long ticket = state->shared->claimed.fetch_add(1) + 1;
        if (state->max_requests != 0 && ticket > state->max_requests) {
            state->shared->stopping.store(true);
            std::string error;
            value_t *headers = (value_t *)newdictval();
            std::string text = build_response(
                503, headers, "server is shutting down", false, error, false);
            freeval(headers);
            if (!text.empty())
                write_stream_data(stream, text, 0, true);
            connection_open = false;
            break;
        }

        last_activity = std::chrono::steady_clock::now();
        state->shared->served.fetch_add(1);
        ++connection_requests;
        if (state->max_requests != 0 &&
            state->shared->served.load() >= state->max_requests)
            state->shared->stopping.store(true);

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
        if (status == 101) {
            /* The handler already wrote the switching-protocols response. */
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
            state->shared->served.load() >= state->max_requests)
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

using http_connection_fn = std::function<void(
    value_t *, value_t *, http_server_state *)>;

task_ptr start_http_server(value_t *socket, value_t *handler,
                           std::shared_ptr<http_server_state> state,
                           const std::string &cert_file,
                           const std::string &key_file,
                           const std::string &client_ca_file = {},
                           const std::string &client_auth = {},
                           const std::vector<std::string> &alpn = {},
                           const http_connection_fn &connection_fn = {}) {
    value_t *socket_copy = dupval(socket);
    value_t *handler_copy = dupval(handler);
    task_ptr server_task = fiber_start_async(
        [socket_copy, handler_copy, state, cert_file, key_file, client_ca_file,
         client_auth, alpn, connection_fn]() {
        std::vector<task_ptr> connections;
        auto reserve_connection = [state]() -> bool {
            if (state->max_connections <= 0)
                return true;
            long long active =
                state->shared->active_connections.load();
            while (active < state->max_connections) {
                if (state->shared->active_connections
                        .compare_exchange_weak(active, active + 1))
                    return true;
            }
            return false;
        };
        auto release_connection = [state]() {
            if (state->max_connections > 0)
                state->shared->active_connections.fetch_sub(1);
        };
        for (;;) {
            if (state->shared->stopping.load())
                break;
            if (state->max_requests != 0 &&
                state->shared->served.load() >= state->max_requests) {
                state->shared->stopping.store(true);
                break;
            }
            for (size_t i = 0; i < connections.size();) {
                if (task_get_state(connections[i]) != TASK_STATE_PENDING)
                    connections.erase(connections.begin()
                                      + (std::ptrdiff_t)i);
                else
                    ++i;
            }

            std::vector<value_t *> accept_args;
            accept_args.push_back(dupval(socket_copy));
            accept_args.push_back(newintval(50));
            value_t *accept_task =
                call_net(B_net_accept_async, accept_args);
            if (!accept_task) {
                break;
            }

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
                if (!alpn.empty() || !client_ca_file.empty() ||
                    !client_auth.empty()) {
                    value_t *options = (value_t *)newdictval();
                    if (!alpn.empty()) {
                        value_t *protocols = (value_t *)newlistval();
                        for (const std::string &protocol : alpn)
                            ((list_value_t *)protocols)
                                ->payload->items.push_back(
                                    newstrval(protocol.c_str()));
                        dict_set(options, "alpn", protocols);
                    }
                    if (!client_ca_file.empty())
                        dict_set(options, "client_ca_file",
                                 newstrval(client_ca_file.c_str()));
                    if (!client_auth.empty())
                        dict_set(options, "client_auth",
                                 newstrval(client_auth.c_str()));
                    tls_args.push_back(options);
                }
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

            bool reserved = false;
            if (state->max_connections > 0) {
                while (!reserved) {
                    if (state->shared->stopping.load())
                        break;
                    reserved = reserve_connection();
                    if (!reserved) {
                        value_t *sleep_task = call_task_builtin(
                            B_task_sleep, {newintval(20)});
                        value_t *slept = await_task_value(sleep_task);
                        freeval(slept);
                    }
                }
            } else {
                reserved = true;
            }
            if (!reserved) {
                close_socket(stream);
                freeval(stream);
                continue;
            }

            task_ptr connection_task = fiber_start_async(
                [stream, handler_copy, state, connection_fn]() {
                struct connection_guard {
                    std::shared_ptr<http_server_state> state;
                    ~connection_guard() {
                        if (state && state->max_connections > 0)
                            state->shared->active_connections.fetch_sub(1);
                    }
                } guard{state};
                struct tracking_guard {
                    std::shared_ptr<http_connection_registry> registry;
                    value_t *tracked = nullptr;
                    ~tracking_guard() {
                        if (!registry || !tracked)
                            return;
                        {
                            std::lock_guard<std::mutex> lock(
                                registry->mutex);
                            auto &streams = registry->streams;
                            auto it = std::find(streams.begin(),
                                                streams.end(), tracked);
                            if (it != streams.end())
                                streams.erase(it);
                        }
                        freeval(tracked);
                    }
                } tracking{state->tracked_connections, nullptr};
                if (tracking.registry) {
                    tracking.tracked = dupval(stream);
                    std::lock_guard<std::mutex> lock(
                        tracking.registry->mutex);
                    tracking.registry->streams.push_back(tracking.tracked);
                }
                if (connection_fn)
                    connection_fn(stream, handler_copy, state.get());
                else
                    handle_http_server_connection(stream, handler_copy,
                                                  state.get());
                return (value_t *)NULL;
            });
            if (!connection_task) {
                close_socket(stream);
                freeval(stream);
                release_connection();
                break;
            }
            connections.push_back(connection_task);
        }

        state->shared->stopping.store(true);
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
        freeval(socket_copy);
        freeval(handler_copy);
        return newintval(state->shared->served.load());
    });
    if (!server_task) {
        freeval(socket_copy);
        freeval(handler_copy);
    }
    return server_task;
}

value_t *http_serve(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 5 || args[0]->type != T_dict ||
        (args[1]->type != T_func && args[1]->type != T_builtin &&
         args[1]->type != T_method && args[1]->type != T_native)) {
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
    std::string client_ca_file;
    std::string client_auth;
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
    value_t *client_ca_value = dict_get(args[0], "client_ca_file");
    value_t *client_auth_value = dict_get(args[0], "client_auth");
    if (client_ca_value) {
        if (client_ca_value->type != T_string) {
            yyerror("std.http.serve: invalid client_ca_file");
            return NULL;
        }
        client_ca_file = ((string_value_t *)client_ca_value)->value;
    }
    if (client_auth_value) {
        if (client_auth_value->type != T_string) {
            yyerror("std.http.serve: invalid client_auth");
            return NULL;
        }
        client_auth = ((string_value_t *)client_auth_value)->value;
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
    state.shared = std::make_shared<http_shared_state>();

    value_t *handler = args[1];
    std::shared_ptr<http_server_state> shared_state =
        std::make_shared<http_server_state>(state);
    task_ptr server_task =
        start_http_server(socket, handler, shared_state, cert_file, key_file,
                          client_ca_file, client_auth);
    if (!server_task)
        return NULL;
    return await_task_value(new_task_value(std::move(server_task)));
}

std::string escape_skein_string(const std::string &text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string make_worker_bootstrap(const std::string &script,
                                  const std::string &entry) {
    std::string source = R"(import std;

let pool = std.thread.pool(1, "@SCRIPT@", {"entry": "@ENTRY@"});
std.io.write_all(std.io.stdout, bytes_from_string(
    "{\"ready\":true,\"version\":1,\"protocol\":\"hybrid\"}\n"));
std.io.flush(std.io.stdout);

while (true) {
    let line = std.io.read_line(std.io.stdin);
    if (line == null) { break; }
    let request = std.json.parse(bytes_to_string(line));
    let body = bytes_from_string("");
    if (request["body_len"] > 0) {
        body = std.io.read_exact(std.io.stdin, request["body_len"]);
    }
    let req = {
        "method": request["params"][0],
        "path": request["params"][1],
        "headers": request["params"][2],
        "body": body,
        "http_major": 0,
        "http_minor": 0,
        "keep_alive": false,
        "remote_addr": null,
        "remote_port": 0
    };
    if (len(request["params"]) > 3) {
        req["http_major"] = request["params"][3];
    }
    if (len(request["params"]) > 4) {
        req["http_minor"] = request["params"][4];
    }
    if (len(request["params"]) > 5) {
        req["keep_alive"] = request["params"][5];
    }
    if (len(request["params"]) > 6) {
        req["remote_addr"] = request["params"][6];
    }
    if (len(request["params"]) > 7) {
        req["remote_port"] = request["params"][7];
    }
    let future = pool.submit(req);
    let result = future.wait();
    if (result["ok"]) {
        let response = result["value"];
        let body_value = response["body"];
        let response_body = bytes_from_string("");
        if (std.type.name(body_value) == "bytes") {
            response_body = body_value;
        } else if (body_value != null) {
            response_body = bytes_from_string(str(body_value));
        }
        let reply = {"id": request["id"], "ok": true,
                     "result": {"status": response["status"],
                                "headers": response["headers"]},
                     "body_len": len(response_body)};
        std.io.write_all(std.io.stdout,
            bytes_from_string(std.json.stringify(reply) + "\n"));
        std.io.write_all(std.io.stdout, response_body);
        std.io.flush(std.io.stdout);
    } else {
        let error_text = "worker handler failed";
        if (result["error"] != null) {
            error_text = str(result["error"]);
        }
        let reply = {"id": request["id"], "ok": false,
                     "error": error_text};
        std.io.write_all(std.io.stdout,
            bytes_from_string(std.json.stringify(reply) + "\n"));
        std.io.flush(std.io.stdout);
    }
}
)";
    const std::string script_marker = "@SCRIPT@";
    const std::string entry_marker = "@ENTRY@";
    auto replace_all = [&source](const std::string &marker,
                                 const std::string &value) {
        size_t pos = 0;
        while ((pos = source.find(marker, pos)) != std::string::npos) {
            source.replace(pos, marker.size(), value);
            pos += value.size();
        }
    };
    replace_all(script_marker, escape_skein_string(script));
    replace_all(entry_marker, escape_skein_string(entry));
    return source;
}

bool write_worker_bootstrap(const std::string &source, std::string &path,
                            std::string &error) {
    std::string directory;
    if (!platform_get_env("TEMP", directory) &&
        !platform_get_env("TMPDIR", directory))
        directory = ".";
    static std::atomic<unsigned long long> counter{0};
    unsigned long long serial = counter.fetch_add(1);
    unsigned long long stamp =
        (unsigned long long)std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
    path = directory + "/skein-http-worker-" + std::to_string(stamp) +
           "-" + std::to_string(serial) + ".skein";
    if (!platform_write_file(path, source, false, error))
        return false;
    return true;
}

struct worker_bootstrap_file {
    std::string path;
    ~worker_bootstrap_file() {
        if (!path.empty())
            std::remove(path.c_str());
    }
};

#if SKEIN_HAS_REUSE_PORT
std::string make_reuse_port_bootstrap(
    const std::string &host, long long port,
    const std::string &script, const std::string &entry,
    long long max_requests, long long max_per_connection,
    long long idle_timeout_ms, long long max_connections,
    std::size_t max_header_bytes, std::size_t max_headers,
    std::size_t max_body_bytes, long long h2_max_concurrent_streams,
    bool stream_request_body, const std::string &shared_path,
    const std::string &cert_file, const std::string &key_file,
    const std::string &client_ca_file, const std::string &client_auth,
    bool unsupported_501, const std::string &unsupported_message) {
    std::string source = "import std;\n";
    source += "let s = {\"host\": \"" + escape_skein_string(host) +
              "\", \"port\": " + std::to_string(port) + "};\n";
    if (!cert_file.empty()) {
        source += "s[\"cert\"] = \"" + escape_skein_string(cert_file) +
                  "\";\n";
        source += "s[\"key\"] = \"" + escape_skein_string(key_file) +
                  "\";\n";
    }
    if (!client_ca_file.empty()) {
        source += "s[\"client_ca_file\"] = \"" +
                  escape_skein_string(client_ca_file) + "\";\n";
    }
    if (!client_auth.empty()) {
        source += "s[\"client_auth\"] = \"" +
                  escape_skein_string(client_auth) + "\";\n";
    }
    source += "let ts = std.http.serve_threads(s, 1, \"" +
              escape_skein_string(script) + "\", {\n";
    source += "  \"strategy\": \"reuse_port\",\n";
    source += "  \"entry\": \"" + escape_skein_string(entry) + "\",\n";
    source += "  \"max_requests\": " + std::to_string(max_requests) +
              ",\n";
    source += "  \"max_per_connection\": " +
              std::to_string(max_per_connection) + ",\n";
    source += "  \"idle_timeout_ms\": " +
              std::to_string(idle_timeout_ms) + ",\n";
    source += "  \"max_connections\": " +
              std::to_string(max_connections) + ",\n";
    source += "  \"max_header_bytes\": " +
              std::to_string(max_header_bytes) + ",\n";
    source += "  \"max_headers\": " + std::to_string(max_headers) +
              ",\n";
    source += "  \"max_body_bytes\": " +
              std::to_string(max_body_bytes) + ",\n";
    source += "  \"h2_max_concurrent_streams\": " +
              std::to_string(h2_max_concurrent_streams) + ",\n";
    source += "  \"stream_request_body\": ";
    source += stream_request_body ? "true" : "false";
    source += ",\n";
    source += "  \"shared_state_path\": \"" +
              escape_skein_string(shared_path) + "\",\n";
    source += "  \"unsupported_501\": ";
    source += unsupported_501 ? "true" : "false";
    source += ",\n";
    source += "  \"unsupported_message\": \"" +
              escape_skein_string(unsupported_message) + "\"\n";
    source += "});\n";
    source += "println(\"ready\");\n";
    source += "std.io.flush(std.io.stdout);\n";
    source += "let served = ts.join();\n";
    source += "println(\"served=\" + str(served));\n";
    return source;
}
#endif

struct http_capabilities {
    bool http1 = true;
    bool http2 = false;
    bool tls = false;
    bool keep_alive = true;
    bool stream_request_body = false;
    bool stream_response = false;
    bool websocket = false;
    bool sse = false;
    bool raw_socket = false;
    bool reuse_port = false;
    bool dispatch = true;
    std::string strategy;
    std::vector<std::string> strategies;
    std::string platform;
};

http_capabilities make_capabilities(const std::string &strategy,
                                    bool tls, bool http2,
                                    bool stream_body) {
    http_capabilities caps;
    caps.strategy = strategy;
    caps.tls = tls;
    caps.http2 = http2;
    caps.stream_request_body = stream_body;
    caps.stream_response = strategy == "reuse_port";
    caps.websocket = strategy == "reuse_port";
    caps.reuse_port =
        strategy == "reuse_port" && SKEIN_HAS_REUSE_PORT != 0;
    caps.dispatch = true;
    caps.platform = http_platform_name();
    caps.strategies.push_back("dispatch");
#if SKEIN_HAS_REUSE_PORT
    caps.strategies.push_back("reuse_port");
#endif
    return caps;
}

bool capability_enabled(const http_capabilities &caps,
                        const std::string &name) {
    if (name == "http1") return caps.http1;
    if (name == "http2") return caps.http2;
    if (name == "tls") return caps.tls;
    if (name == "keep_alive") return caps.keep_alive;
    if (name == "stream_request_body") return caps.stream_request_body;
    if (name == "stream_response") return caps.stream_response;
    if (name == "websocket") return caps.websocket;
    if (name == "sse") return caps.sse;
    if (name == "raw_socket") return caps.raw_socket;
    if (name == "reuse_port") return caps.reuse_port;
    if (name == "dispatch") return caps.dispatch;
    return false;
}

value_t *capabilities_value(const http_capabilities &caps) {
    value_t *result = (value_t *)newdictval();
    dict_set(result, "schema", newintval(1));
    dict_set(result, "platform", newstrval(caps.platform.c_str()));
    dict_set(result, "strategy", newstrval(caps.strategy.c_str()));
    value_t *strategies = (value_t *)newlistval();
    for (const std::string &name : caps.strategies)
        ((list_value_t *)strategies)
            ->payload->items.push_back(newstrval(name.c_str()));
    dict_set(result, "strategies", strategies);
    dict_set(result, "http1", newboolval(caps.http1));
    dict_set(result, "http2", newboolval(caps.http2));
    dict_set(result, "tls", newboolval(caps.tls));
    dict_set(result, "keep_alive", newboolval(caps.keep_alive));
    dict_set(result, "stream_request_body",
             newboolval(caps.stream_request_body));
    dict_set(result, "stream_response",
             newboolval(caps.stream_response));
    dict_set(result, "websocket", newboolval(caps.websocket));
    dict_set(result, "sse", newboolval(caps.sse));
    dict_set(result, "raw_socket", newboolval(caps.raw_socket));
    dict_set(result, "reuse_port", newboolval(caps.reuse_port));
    dict_set(result, "dispatch", newboolval(caps.dispatch));
    return result;
}

bool await_task_timeout(const task_ptr &task, long long timeout_ms) {
    if (!task)
        return true;
    if (task_get_state(task) != TASK_STATE_PENDING) {
        value_t *result = fiber_await_task(task);
        freeval(result);
        return true;
    }
    if (timeout_ms <= 0) {
        value_t *result = fiber_await_task(task);
        freeval(result);
        return true;
    }
    if (fiber_in_fiber()) {
        auto ready = [task]() {
            return task_get_state(task) != TASK_STATE_PENDING;
        };
        if (!fiber_wait_for(ready, timeout_ms))
            return false;
        if (task_get_state(task) == TASK_STATE_PENDING)
            return false;
    } else {
        if (!event_loop_init())
            return false;
        event_timeout timeout = make_event_timeout(timeout_ms);
        while (task_get_state(task) == TASK_STATE_PENDING) {
            if (g_interrupted)
                return false;
            if (event_timeout_expired(timeout))
                return false;
            uv_run(event_loop_handle(), UV_RUN_ONCE);
            fiber_pump();
        }
    }
    value_t *result = fiber_await_task(task);
    freeval(result);
    return true;
}

struct process_server_state
    : std::enable_shared_from_this<process_server_state> {
    std::string strategy;
    bool tls = false;
    bool unsupported_501 = false;
    std::string unsupported_message;
    std::shared_ptr<http_shared_state> shared;
    long long workers = 0;
    long long max_requests = 0;
    std::string script;
    std::string entry = "handle";

    value_t *pool = nullptr;
    task_ptr server_task;
    std::shared_ptr<http_server_state> http;
    bool pool_shutdown = false;

    std::vector<value_t *> children;
    bool joined = false;
    std::mutex mutex;

    ~process_server_state();
};

bool process_server_stop_children(process_server_state *server, bool wait,
                                  long long timeout_ms = 0) {
    if (wait && timeout_ms > 0 && !server->children.empty()) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        for (;;) {
            bool all_exited = true;
            for (value_t *process : server->children) {
                long long status = 0;
                if (!process_try_wait(process, status)) {
                    all_exited = false;
                    break;
                }
            }
            if (all_exited)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (value_t *process : server->children) {
        if (wait) {
            value_t *status = call_process_spawn_builtin(
                B_process_wait, {process});
            if (status)
                freeval(status);
        } else {
            value_t *killed = call_process_spawn_builtin(
                B_process_kill, {process});
            if (killed)
                freeval(killed);
        }
        value_t *closed = call_process_spawn_builtin(
            B_process_close, {process});
        if (closed)
            freeval(closed);
        freeval(process);
    }
    server->children.clear();
    return true;
}

void process_server_close_pool(process_server_state *server) {
    if (!server->pool || server->pool_shutdown)
        return;
    server->pool_shutdown = true;
    value_t *result = call_worker_builtin(
        B_worker_shutdown, {server->pool});
    if (result)
        freeval(result);
    freeval(server->pool);
    server->pool = nullptr;
}

void process_server_force(process_server_state *server) {
    if (server->http && server->http->force_stop)
        server->http->force_stop->store(true);
    if (server->strategy == "reuse_port") {
        process_server_stop_children(server, false);
        return;
    }
    http_close_tracked_connections(server->http);
    if (server->pool && !server->pool_shutdown)
        worker_pool_force_shutdown(server->pool);
}

bool process_server_shutdown(process_server_state *server,
                             long long timeout_ms, long long &served) {
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        if (server->joined) {
            served = server->shared ? server->shared->served.load() : 0;
            return true;
        }
    }
    if (server->shared)
        server->shared->stopping.store(true);
    if (server->server_task) {
        bool stopped = await_task_timeout(server->server_task, timeout_ms);
        if (!stopped) {
            process_server_force(server);
            stopped = await_task_timeout(server->server_task,
                                         kHttpShutdownForceGraceMs);
        }
        if (!stopped)
            return false;
        server->server_task = nullptr;
    } else {
        bool stopped = process_server_stop_children(server, true,
                                                    timeout_ms);
        if (!stopped)
            process_server_stop_children(server, false);
    }
    process_server_close_pool(server);
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        server->joined = true;
    }
    served = server->shared ? server->shared->served.load() : 0;
    return true;
}

process_server_state::~process_server_state() {
    if (shared)
        shared->stopping.store(true);
    process_server_stop_children(this, false);
    process_server_close_pool(this);
}

bool process_server_join(process_server_state *server, long long timeout_ms,
                         long long &served) {
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        if (server->joined) {
            served = server->shared ? server->shared->served.load() : 0;
            return true;
        }
    }
    if (server->server_task) {
        if (!await_task_timeout(server->server_task, timeout_ms))
            return false;
        server->server_task = nullptr;
    }
    if (!process_server_stop_children(server, true, timeout_ms))
        return false;
    process_server_close_pool(server);
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        server->joined = true;
    }
    served = server->shared ? server->shared->served.load() : 0;
    return true;
}

value_t *process_server_build_stats(process_server_state *server) {
    value_t *stats = nullptr;
    long long workers = server->workers;
    bool joined = false;
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        joined = server->joined;
    }
    if (server->pool) {
        stats = call_worker_builtin(B_worker_stats, {server->pool});
        if (!stats)
            return nullptr;
        long long available = 0;
        value_t *available_value = dict_get(stats, "available");
        if (available_value && available_value->type == T_int)
            available = ((int_value_t *)available_value)->value;
        dict_set(stats, "size", newintval(workers));
        dict_set(stats, "workers", newintval(workers));
        dict_set(stats, "ready", newintval(available));
        dict_set(stats, "alive", newintval(available));
        dict_set(stats, "dead",
                 newintval(workers > available ? workers - available : 0));
        dict_set(stats, "restart_limit", newintval(0));
        dict_set(stats, "submitted", newintval(0));
        dict_set(stats, "completed",
                 newintval(server->shared ? server->shared->served.load()
                                          : 0));
        dict_set(stats, "failed", newintval(0));
        dict_set(stats, "active", newintval(0));
        dict_set(stats, "queued", newintval(0));
    } else {
        stats = (value_t *)newdictval();
        long long alive = joined ? 0 : workers;
        dict_set(stats, "size", newintval(workers));
        dict_set(stats, "workers", newintval(workers));
        dict_set(stats, "ready", newintval(workers));
        dict_set(stats, "alive", newintval(alive));
        dict_set(stats, "dead", newintval(joined ? workers : 0));
        dict_set(stats, "restarts", newintval(0));
        dict_set(stats, "restart_limit", newintval(0));
        dict_set(stats, "submitted",
                 newintval(server->shared ? server->shared->claimed.load()
                                          : 0));
        dict_set(stats, "completed",
                 newintval(server->shared ? server->shared->served.load()
                                          : 0));
        dict_set(stats, "failed", newintval(0));
        dict_set(stats, "active", newintval(0));
        dict_set(stats, "queued", newintval(0));
    }
    dict_set(stats, "served",
             newintval(server->shared ? server->shared->served.load() : 0));
    dict_set(stats, "stopping",
             newboolval(server->shared ? server->shared->stopping.load()
                                       : false));
    dict_set(stats, "connections",
             newintval(server->shared
                           ? server->shared->active_connections.load()
                           : 0));
    dict_set(stats, "strategy", newstrval(server->strategy.c_str()));
    dict_set(stats, "max_requests", newintval(server->max_requests));
    return stats;
}

std::string process_server_describe(const void *) {
    return "<process_server>";
}

value_t *process_server_get_field(void *data, const char *name) {
    process_server_state *raw = static_cast<process_server_state *>(data);
    if (!raw || !name) {
        yyerror("std.http.ProcessServer has no such field");
        return nullptr;
    }
    std::shared_ptr<process_server_state> server = raw->shared_from_this();

    if (std::strcmp(name, "stop") == 0) {
        return newnativeval(
            [server](const std::vector<value_t *> &args) {
                if (!args.empty()) {
                    yyerror("ProcessServer.stop expects no arguments");
                    return (value_t *)nullptr;
                }
                if (server->shared)
                    server->shared->stopping.store(true);
                return newnull();
            },
            server);
    }
    if (std::strcmp(name, "join") == 0) {
        return newnativeval(
            [server](const std::vector<value_t *> &args) {
                long long timeout_ms = 0;
                if (args.size() > 1) {
                    yyerror("ProcessServer.join expects 0 or 1 argument");
                    return (value_t *)nullptr;
                }
                if (args.size() == 1) {
                    if (args[0]->type != T_int ||
                        ((int_value_t *)args[0])->value < 0) {
                        yyerror("ProcessServer.join timeout must be a "
                                "non-negative integer");
                        return (value_t *)nullptr;
                    }
                    timeout_ms = ((int_value_t *)args[0])->value;
                }
                long long served = 0;
                if (!process_server_join(server.get(), timeout_ms,
                                         served)) {
                    yyerror("std.http.ProcessServer.join timed out");
                    return (value_t *)nullptr;
                }
                return newintval(served);
            },
            server);
    }
    if (std::strcmp(name, "shutdown") == 0) {
        return newnativeval(
            [server](const std::vector<value_t *> &args) {
                long long timeout_ms = 0;
                if (args.size() > 1) {
                    yyerror("ProcessServer.shutdown expects 0 or 1 "
                            "argument");
                    return (value_t *)nullptr;
                }
                if (args.size() == 1) {
                    if (args[0]->type != T_int ||
                        ((int_value_t *)args[0])->value < 0) {
                        yyerror("ProcessServer.shutdown timeout must be a "
                                "non-negative integer");
                        return (value_t *)nullptr;
                    }
                    timeout_ms = ((int_value_t *)args[0])->value;
                }
                long long served = 0;
                if (!process_server_shutdown(server.get(), timeout_ms,
                                             served)) {
                    yyerror("std.http.ProcessServer.shutdown timed out");
                    return (value_t *)nullptr;
                }
                return newintval(served);
            },
            server);
    }
    if (std::strcmp(name, "stats") == 0) {
        return newnativeval(
            [server](const std::vector<value_t *> &args) {
                if (!args.empty()) {
                    yyerror("ProcessServer.stats expects no arguments");
                    return (value_t *)nullptr;
                }
                return process_server_build_stats(server.get());
            },
            server);
    }
    if (std::strcmp(name, "strategy") == 0)
        return newstrval(server->strategy.c_str());
    if (std::strcmp(name, "served") == 0)
        return newintval(server->shared ? server->shared->served.load() : 0);
    if (std::strcmp(name, "capabilities") == 0) {
        bool http2 = false;
#ifdef SKEIN_ENABLE_HTTP2
        http2 = server->strategy == "reuse_port";
#endif
        bool stream_body = server->strategy == "reuse_port";
        return capabilities_value(make_capabilities(
            server->strategy, server->tls, http2, stream_body));
    }
    yyerror("std.http.ProcessServer has no field %s", name);
    return nullptr;
}

const native_type kProcessServerType = {
    "process_server",
    process_server_describe,
    nullptr,
    process_server_get_field,
    nullptr,
    nullptr,
};

value_t *http_serve_processes(const std::vector<value_t *> &args) {
    if (args.size() < 3 || args.size() > 6 || args[0]->type != T_dict ||
        args[1]->type != T_int ||
        ((int_value_t *)args[1])->value <= 0 ||
        args[2]->type != T_string) {
        yyerror("std.http.serve_processes expects server, worker count, "
                "script and optional options or serve limits");
        return NULL;
    }

    value_t *options = nullptr;
    if (args.size() > 3 && args[3]->type == T_dict) {
        if (args.size() > 4) {
            yyerror("std.http.serve_processes options cannot be combined "
                    "with positional serve limits");
            return NULL;
        }
        options = args[3];
    }

    long long workers = ((int_value_t *)args[1])->value;

    std::string strategy = "auto";
    if (options) {
        value_t *value = dict_get(options, "strategy");
        if (value) {
            if (value->type != T_string) {
                yyerror("std.http.serve_processes strategy must be a string");
                return NULL;
            }
            strategy = ((string_value_t *)value)->value;
        }
    }
    if (strategy == "auto") {
#if SKEIN_HAS_REUSE_PORT
        strategy = "reuse_port";
#else
        strategy = "dispatch";
#endif
    }
#if !SKEIN_HAS_REUSE_PORT
    if (strategy == "reuse_port") {
        yyerror("std.http.serve_processes: reuse_port is not supported "
                "on this platform");
        return NULL;
    }
#endif
    if (strategy != "dispatch" && strategy != "reuse_port") {
        yyerror("std.http.serve_processes strategy must be "
                "\"auto\", \"dispatch\" or \"reuse_port\"");
        return NULL;
    }

    std::string on_unsupported = "error";
    std::vector<std::string> required;
    if (options) {
        value_t *value = dict_get(options, "on_unsupported");
        if (value) {
            if (value->type != T_string) {
                yyerror("std.http.serve_processes on_unsupported must be a "
                        "string");
                return NULL;
            }
            on_unsupported = ((string_value_t *)value)->value;
        }
        value_t *require_value = dict_get(options, "require");
        if (require_value && require_value->type != T_null) {
            if (require_value->type == T_string) {
                required.push_back(
                    ((string_value_t *)require_value)->value);
            } else if (require_value->type == T_list) {
                for (value_t *item :
                     ((list_value_t *)require_value)->payload->items) {
                    if (!item || item->type != T_string) {
                        yyerror("std.http.serve_processes require entries "
                                "must be strings");
                        return NULL;
                    }
                    required.push_back(
                        ((string_value_t *)item)->value);
                }
            } else {
                yyerror("std.http.serve_processes require must be a "
                        "string or list of strings");
                return NULL;
            }
        }
    }
    if (on_unsupported != "error" && on_unsupported != "501" &&
        on_unsupported != "warn") {
        yyerror("std.http.serve_processes on_unsupported must be "
                "\"error\", \"501\" or \"warn\"");
        return NULL;
    }

    std::string entry = "handle";
    if (options) {
        value_t *value = dict_get(options, "entry");
        if (value) {
            if (value->type != T_string) {
                yyerror("std.http.serve_processes entry must be a string");
                return NULL;
            }
            entry = ((string_value_t *)value)->value;
        }
    }

    auto read_option = [&options](const char *key, long long &out) -> bool {
        if (!options)
            return true;
        value_t *value = dict_get(options, key);
        if (!value)
            return true;
        if (value->type != T_int ||
            ((int_value_t *)value)->value < 0) {
            yyerror("std.http.serve_processes %s must be a non-negative "
                    "integer", key);
            return false;
        }
        out = ((int_value_t *)value)->value;
        return true;
    };

    long long max_requests = 0;
    long long max_per_connection = 100;
    long long idle_timeout_ms = 0;
    if (options) {
        if (!read_option("max_requests", max_requests) ||
            !read_option("max_per_connection", max_per_connection) ||
            !read_option("idle_timeout_ms", idle_timeout_ms))
            return NULL;
    } else {
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i]->type != T_int ||
                ((int_value_t *)args[i])->value < 0) {
                yyerror("std.http.serve_processes limits must be "
                        "non-negative integers");
                return NULL;
            }
            long long value = ((int_value_t *)args[i])->value;
            if (i == 3)
                max_requests = value;
            else if (i == 4)
                max_per_connection = value;
            else
                idle_timeout_ms = value;
        }
    }

    long long worker_timeout_ms = 30000;
    {
        value_t *value = options ? dict_get(options, "worker_timeout_ms")
                                 : nullptr;
        if (!value)
            value = dict_get(args[0], "worker_timeout_ms");
        if (value) {
            if (value->type != T_int ||
                ((int_value_t *)value)->value < 0) {
                yyerror("std.http.serve_processes worker_timeout_ms must "
                        "be a non-negative integer");
                return NULL;
            }
            worker_timeout_ms = ((int_value_t *)value)->value;
        }
    }

    bool process_tls = false;
    {
        value_t *cert = dict_get(args[0], "cert");
        value_t *key = dict_get(args[0], "key");
        process_tls = cert && key;
    }
    bool reuse_mode = strategy == "reuse_port";
    bool http2_capable = false;
#ifdef SKEIN_ENABLE_HTTP2
    http2_capable = reuse_mode;
#endif
    auto process_capability = [&](const std::string &name) {
        if (name == "http1" || name == "keep_alive")
            return true;
        if (name == "dispatch")
            return !reuse_mode;
        if (name == "tls")
            return process_tls;
        if (name == "reuse_port")
            return reuse_mode;
        if (name == "http2")
            return http2_capable;
        if (name == "websocket")
            return reuse_mode;
        if (name == "stream_request_body" || name == "stream_response")
            return reuse_mode;
        return false;
    };
    std::string missing;
    for (const std::string &name : required) {
        if (process_capability(name))
            continue;
        if (!missing.empty())
            missing += ", ";
        missing += name;
    }
    bool unsupported_501 = false;
    std::string unsupported_message;
    if (!missing.empty()) {
        if (on_unsupported == "error") {
            const std::string message =
                "std.http.serve_processes: missing capabilities: " + missing;
            eval_raise(newerrorval("http.unsupported", message.c_str()));
            return NULL;
        }
        if (on_unsupported == "warn") {
            std::fprintf(stderr,
                         "std.http.serve_processes: missing capabilities: "
                         "%s\n", missing.c_str());
            std::fflush(stderr);
        } else {
            unsupported_501 = true;
            unsupported_message =
                "unsupported capabilities: " + missing;
        }
    }

    long long max_connections = 0;
    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    bool stream_request_body = false;
    long long h2_max_concurrent_streams = 16;
    if (options) {
        if (!read_option("max_connections", max_connections))
            return NULL;
        if (!positive_size_option(options, "max_header_bytes",
                                  max_header_bytes) ||
            !positive_size_option(options, "max_headers", max_headers) ||
            !positive_size_option(options, "max_body_bytes",
                                  max_body_bytes))
            return NULL;
        value_t *stream_value = dict_get(options, "stream_request_body");
        if (stream_value) {
            if (stream_value->type != T_bool) {
                yyerror("std.http.serve_processes stream_request_body "
                        "must be a bool");
                return NULL;
            }
            stream_request_body =
                ((bool_value_t *)stream_value)->value;
        }
        if (stream_request_body && strategy != "reuse_port") {
            yyerror("std.http.serve_processes: stream_request_body requires "
                    "reuse_port");
            return NULL;
        }
        if (!read_option("h2_max_concurrent_streams",
                         h2_max_concurrent_streams))
            return NULL;
        dict_set(args[0], "max_connections", newintval(max_connections));
        dict_set(args[0], "max_header_bytes",
                 newintval((long long)max_header_bytes));
        dict_set(args[0], "max_headers",
                 newintval((long long)max_headers));
        dict_set(args[0], "max_body_bytes",
                 newintval((long long)max_body_bytes));
        dict_set(args[0], "worker_timeout_ms",
                 newintval(worker_timeout_ms));
    } else {
        value_t *max_connections_value =
            dict_get(args[0], "max_connections");
        if (max_connections_value) {
            if (max_connections_value->type != T_int ||
                ((int_value_t *)max_connections_value)->value < 0) {
                yyerror("std.http.serve_processes max_connections must be "
                        "a non-negative integer");
                return NULL;
            }
            max_connections =
                ((int_value_t *)max_connections_value)->value;
        }
        if (!positive_size_option(args[0], "max_header_bytes",
                                  max_header_bytes) ||
            !positive_size_option(args[0], "max_headers", max_headers) ||
            !positive_size_option(args[0], "max_body_bytes",
                                  max_body_bytes))
            return NULL;
    }

    value_t *caps = (value_t *)newdictval();
    dict_set(caps, "schema", newintval(1));
    dict_set(caps, "platform",
             newstrval(http_platform_name().c_str()));
    dict_set(caps, "strategy", newstrval(strategy.c_str()));
    value_t *strategies = (value_t *)newlistval();
    ((list_value_t *)strategies)
        ->payload->items.push_back(newstrval("dispatch"));
#if SKEIN_HAS_REUSE_PORT
    ((list_value_t *)strategies)
        ->payload->items.push_back(newstrval("reuse_port"));
#endif
    dict_set(caps, "strategies", strategies);
    dict_set(caps, "http1", newboolval(true));
    dict_set(caps, "http2", newboolval(http2_capable));
    dict_set(caps, "tls", newboolval(process_tls));
    dict_set(caps, "keep_alive", newboolval(true));
    dict_set(caps, "stream_request_body",
             newboolval(reuse_mode));
    dict_set(caps, "stream_response", newboolval(reuse_mode));
    dict_set(caps, "websocket", newboolval(reuse_mode));
    dict_set(caps, "sse", newboolval(false));
    dict_set(caps, "raw_socket", newboolval(false));
    dict_set(caps, "reuse_port", newboolval(reuse_mode));
    dict_set(caps, "dispatch", newboolval(!reuse_mode));
    dict_set(args[0], "strategy",
             newstrval(strategy.c_str()));
    dict_set(args[0], "platform",
             newstrval(http_platform_name().c_str()));
    dict_set(args[0], "capabilities", caps);

    (void)workers;
    (void)stream_request_body;

    std::string user_script = ((string_value_t *)args[2])->value;

#if SKEIN_HAS_REUSE_PORT
    if (reuse_mode) {
        value_t *host_value = dict_get(args[0], "host");
        value_t *port_value = dict_get(args[0], "port");
        std::string host = "127.0.0.1";
        if (host_value && host_value->type == T_string)
            host = ((string_value_t *)host_value)->value;
        long long port = 0;
        if (port_value && port_value->type == T_int)
            port = ((int_value_t *)port_value)->value;
        value_t *listener = dict_get(args[0], "socket");
        if (port <= 0 && listener) {
            value_t *addr =
                call_net(B_net_local_addr, {dupval(listener)});
            if (addr && addr->type == T_dict) {
                value_t *p = dict_get(addr, "port");
                if (p && p->type == T_int)
                    port = ((int_value_t *)p)->value;
            }
            freeval(addr);
        }
        if (port <= 0) {
            std::vector<value_t *> listen_args{
                newstrval(host.c_str()), newintval(0), newintval(0)};
            value_t *temp = call_net(B_net_tcp_listen, listen_args);
            if (temp) {
                value_t *addr =
                    call_net(B_net_local_addr, {dupval(temp)});
                if (addr && addr->type == T_dict) {
                    value_t *p = dict_get(addr, "port");
                    if (p && p->type == T_int)
                        port = ((int_value_t *)p)->value;
                }
                freeval(addr);
                call_net(B_net_close, {dupval(temp)});
                freeval(temp);
            }
        }
        if (port <= 0) {
            yyerror("std.http.serve_processes: cannot determine "
                    "reuse_port port");
            return NULL;
        }
        if (listener)
            call_net(B_net_close, {dupval(listener)});

        std::string cert_file;
        std::string key_file;
        std::string client_ca_file;
        std::string client_auth;
        value_t *cert = dict_get(args[0], "cert");
        value_t *key = dict_get(args[0], "key");
        if (cert && key && cert->type == T_string &&
            key->type == T_string) {
            cert_file = ((string_value_t *)cert)->value;
            key_file = ((string_value_t *)key)->value;
        }
        value_t *client_ca = dict_get(args[0], "client_ca_file");
        value_t *client_auth_value = dict_get(args[0], "client_auth");
        if (client_ca && client_ca->type == T_string)
            client_ca_file = ((string_value_t *)client_ca)->value;
        if (client_auth_value && client_auth_value->type == T_string)
            client_auth = ((string_value_t *)client_auth_value)->value;

        std::string temp_dir;
        if (!platform_get_env("TMPDIR", temp_dir) &&
            !platform_get_env("TEMP", temp_dir))
            temp_dir = ".";
        static std::atomic<unsigned long long> sequence{0};
        unsigned long long stamp =
            (unsigned long long)std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        unsigned long long serial = sequence.fetch_add(1);
        std::string base = "skein-http-proc-" +
                           std::to_string(stamp) + "-" +
                           std::to_string(serial);
        std::string shared_path = temp_dir + "/" + base + ".shm";
        std::string bootstrap_path = temp_dir + "/" + base + ".skein";
        std::string shared_error;
        std::shared_ptr<http_shared_state> shared =
            map_shared_state_file(shared_path, true, shared_error);
        if (!shared) {
            yyerror("std.http.serve_processes: %s", shared_error.c_str());
            return NULL;
        }
        struct file_cleanup {
            std::string shared;
            std::string bootstrap;
            ~file_cleanup() {
                if (!shared.empty())
                    std::remove(shared.c_str());
                if (!bootstrap.empty())
                    std::remove(bootstrap.c_str());
            }
        } cleanup_files{shared_path, bootstrap_path};

        std::string source = make_reuse_port_bootstrap(
            host, port, user_script, entry, max_requests,
            max_per_connection, idle_timeout_ms, max_connections,
            max_header_bytes, max_headers, max_body_bytes,
            h2_max_concurrent_streams, stream_request_body, shared_path,
            cert_file, key_file, client_ca_file, client_auth,
            unsupported_501, unsupported_message);
        std::string write_error;
        if (!platform_write_file(bootstrap_path, source, false,
                                 write_error)) {
            yyerror("std.http.serve_processes: cannot write reuse_port "
                    "bootstrap: %s", write_error.c_str());
            return NULL;
        }

        std::string executable = platform_executable_path();
        if (executable.empty()) {
            yyerror("std.http.serve_processes: cannot locate interpreter");
            return NULL;
        }
        std::vector<value_t *> children;
        auto stop_children = [&children]() {
            for (value_t *process : children) {
                call_process_spawn_builtin(B_process_kill, {process});
                call_process_spawn_builtin(B_process_close, {process});
                freeval(process);
            }
            children.clear();
        };
        for (long long i = 0; i < workers; ++i) {
            value_t *file = newstrval(executable.c_str());
            value_t *spawn_options = (value_t *)newdictval();
            value_t *args_list = (value_t *)newlistval();
            ((list_value_t *)args_list)->payload->items.push_back(
                newstrval(bootstrap_path.c_str()));
            dict_set(spawn_options, "args", args_list);
            std::vector<value_t *> spawn_args{file, spawn_options};
            value_t *process = call_process_spawn_builtin(
                B_process_spawn_command, spawn_args);
            freeval(file);
            freeval(spawn_options);
            if (!process) {
                stop_children();
                yyerror("std.http.serve_processes: cannot start worker "
                        "process");
                return NULL;
            }
            children.push_back(process);
            value_t *stdout_stream = dict_get(process, "stdout");
            value_t *line =
                call_io(B_io_read_line, {dupval(stdout_stream)});
            if (!line) {
                stop_children();
                yyerror("std.http.serve_processes: worker did not report "
                        "ready");
                return NULL;
            }
            std::string text;
            if (line->type == T_bytes)
                text = ((bytes_value_t *)line)->value;
            else if (line->type == T_string)
                text = ((string_value_t *)line)->value;
            freeval(line);
            if (text.find("ready") == std::string::npos) {
                stop_children();
                yyerror("std.http.serve_processes: worker failed: %s",
                        text.c_str());
                return NULL;
            }
        }
        std::shared_ptr<process_server_state> server =
            std::make_shared<process_server_state>();
        server->strategy = strategy;
        server->tls = !cert_file.empty();
        server->unsupported_501 = unsupported_501;
        server->unsupported_message = unsupported_message;
        server->shared = shared;
        server->workers = workers;
        server->max_requests = max_requests;
        server->script = user_script;
        server->entry = entry;
        server->children = std::move(children);
        children.clear();
        return newuserdataval(&kProcessServerType, server);
    }
#endif

    std::string bootstrap_source =
        make_worker_bootstrap(user_script, entry);
    worker_bootstrap_file bootstrap;
    std::string bootstrap_error;
    if (!write_worker_bootstrap(bootstrap_source, bootstrap.path,
                                bootstrap_error)) {
        yyerror("std.http.serve_processes: cannot write worker bootstrap: "
                "%s", bootstrap_error.c_str());
        return NULL;
    }

    value_t *worker_count = dupval(args[1]);
    value_t *script = newstrval(bootstrap.path.c_str());
    value_t *pool_options = (value_t *)newdictval();
    dict_set(pool_options, "protocol", newstrval("hybrid"));
    std::vector<value_t *> pool_args{worker_count, script, pool_options};
    value_t *pool = call_worker_builtin(B_worker_pool, pool_args);
    freeval(worker_count);
    freeval(script);
    freeval(pool_options);
    if (!pool)
        return NULL;

    std::string cert_file;
    std::string key_file;
    std::string client_ca_file;
    std::string client_auth;
    value_t *cert = dict_get(args[0], "cert");
    value_t *key = dict_get(args[0], "key");
    if (cert && key) {
        if (cert->type != T_string || key->type != T_string) {
            freeval(pool);
            yyerror("std.http.serve_processes: invalid TLS server "
                    "certificate");
            return NULL;
        }
        cert_file = ((string_value_t *)cert)->value;
        key_file = ((string_value_t *)key)->value;
    }
    value_t *client_ca = dict_get(args[0], "client_ca_file");
    value_t *client_auth_value = dict_get(args[0], "client_auth");
    if (client_ca) {
        if (client_ca->type != T_string) {
            freeval(pool);
            yyerror("std.http.serve_processes: invalid client_ca_file");
            return NULL;
        }
        client_ca_file = ((string_value_t *)client_ca)->value;
    }
    if (client_auth_value) {
        if (client_auth_value->type != T_string) {
            freeval(pool);
            yyerror("std.http.serve_processes: invalid client_auth");
            return NULL;
        }
        client_auth = ((string_value_t *)client_auth_value)->value;
    }

    std::shared_ptr<process_server_state> server =
        std::make_shared<process_server_state>();
    server->strategy = strategy;
    server->tls = !cert_file.empty();
    server->unsupported_501 = unsupported_501;
    server->unsupported_message = unsupported_message;
    server->shared = std::make_shared<http_shared_state>();
    server->workers = workers;
    server->max_requests = max_requests;
    server->script = user_script;
    server->entry = entry;
    server->pool = dupval(pool);
    freeval(pool);

    value_t *handler = newnativeval(
        [server, worker_timeout_ms, unsupported_501,
         unsupported_message](
            const std::vector<value_t *> &call_args) -> value_t * {
            auto error_response = [](int status, const std::string &message) {
                value_t *response = (value_t *)newdictval();
                dict_set(response, "status", newintval(status));
                dict_set(response, "headers", (value_t *)newdictval());
                dict_set(response, "body", newbytesval(message));
                return response;
            };
            if (call_args.size() != 1 || !call_args[0] ||
                call_args[0]->type != T_dict)
                return error_response(400, "invalid HTTP request");
            value_t *request = call_args[0];
            if (unsupported_501)
                return error_response(501, unsupported_message);
            std::string feature;
            if (request_unsupported_feature(request, feature, false))
                return error_response(501, feature);
            if (dict_get(request, "body_reader"))
                return error_response(
                    501, "serve_processes does not support stream_request_body");

            std::string body;
            if (!body_value(dict_get(request, "body"), body,
                            "std.http.serve_processes"))
                return NULL;

            value_t *params = (value_t *)newlistval();
            list_value_t *items = (list_value_t *)params;
            items->payload->items.push_back(dupval(dict_get(request, "method")));
            items->payload->items.push_back(dupval(dict_get(request, "path")));
            items->payload->items.push_back(dupval(dict_get(request, "headers")));
            value_t *major = dict_get(request, "http_major");
            items->payload->items.push_back(
                major ? dupval(major) : newintval(0));
            value_t *minor = dict_get(request, "http_minor");
            items->payload->items.push_back(
                minor ? dupval(minor) : newintval(0));
            value_t *keep_alive = dict_get(request, "keep_alive");
            items->payload->items.push_back(
                keep_alive ? dupval(keep_alive) : newboolval(false));
            value_t *remote_addr = dict_get(request, "remote_addr");
            items->payload->items.push_back(
                remote_addr ? dupval(remote_addr) : newnull());
            value_t *remote_port = dict_get(request, "remote_port");
            items->payload->items.push_back(
                remote_port ? dupval(remote_port) : newintval(0));
            value_t *method = newstrval("handle");
            value_t *body_arg = newbytesval(body);
            std::vector<value_t *> call{server->pool, method, params,
                                        body_arg};
            value_t *timeout_value = nullptr;
            if (worker_timeout_ms > 0) {
                timeout_value = newintval(worker_timeout_ms);
                call.push_back(timeout_value);
            }
            value_t *worker_result =
                call_worker_builtin(B_worker_call_bytes, call);
            freeval(method);
            freeval(params);
            freeval(body_arg);
            if (timeout_value)
                freeval(timeout_value);
            if (!worker_result)
                return error_response(502, "worker call failed");
            if (worker_result->type != T_result) {
                freeval(worker_result);
                return error_response(502, "invalid worker result");
            }
            result_value_t *result = (result_value_t *)worker_result;
            if (!result->ok) {
                std::string message = "worker error";
                if (result->error &&
                    result->error->type == T_error)
                    message = ((error_value_t *)result->error)->message;
                freeval(worker_result);
                int status = 502;
                if (message.find("does not support generator") !=
                        std::string::npos ||
                    message.find("stream_response") !=
                        std::string::npos)
                    status = 501;
                else if (message.find("timeout") != std::string::npos)
                    status = 504;
                else if (message.find("pool_failed") != std::string::npos ||
                         message.find("closed") != std::string::npos)
                    status = 503;
                return error_response(status, message);
            }
            value_t *payload = result->value;
            value_t *meta = dict_get(payload, "value");
            value_t *reply_body = dict_get(payload, "body");
            if (!meta || meta->type != T_dict) {
                freeval(worker_result);
                return error_response(502, "worker must return metadata dict");
            }
            value_t *status = dict_get(meta, "status");
            long long status_code = 200;
            if (status && status->type == T_int)
                status_code = ((int_value_t *)status)->value;
            if (status_code < 100 || status_code > 999)
                status_code = 502;
            value_t *headers = dict_get(meta, "headers");
            value_t *response = (value_t *)newdictval();
            dict_set(response, "status", newintval(status_code));
            dict_set(response, "headers",
                     headers && headers->type == T_dict
                         ? dupval(headers)
                         : (value_t *)newdictval());
            dict_set(response, "body",
                     reply_body && reply_body->type == T_bytes
                         ? dupval(reply_body)
                         : newbytesval(""));
            freeval(worker_result);
            return response;
        },
        server);

    std::shared_ptr<http_server_state> state =
        std::make_shared<http_server_state>();
    state->max_requests = max_requests;
    state->max_per_connection = max_per_connection;
    state->idle_timeout_ms = idle_timeout_ms;
    state->max_connections = max_connections;
    state->stream_request_body = false;
    state->max_header_bytes = max_header_bytes;
    state->max_headers = max_headers;
    state->max_body_bytes = max_body_bytes;
    state->shared = server->shared;
    state->unsupported_501 = unsupported_501;
    state->unsupported_message = unsupported_message;

    value_t *socket = dict_get(args[0], "socket");
    if (!socket) {
        process_server_close_pool(server.get());
        yyerror("std.http.serve_processes expects a std.http server for "
                "dispatch");
        return NULL;
    }

    task_ptr task = start_http_server(socket, handler, state, cert_file,
                                      key_file, client_ca_file, client_auth);
    freeval(handler);
    if (!task) {
        process_server_close_pool(server.get());
        yyerror("std.http.serve_processes could not start the server");
        return NULL;
    }
    server->http = state;
    server->server_task = task;
    return newuserdataval(&kProcessServerType, server);
}

value_t *thread_server_response(int status, const std::string &message) {
    value_t *response = (value_t *)newdictval();
    value_t *headers = (value_t *)newdictval();
    dict_set(headers, "Content-Type",
             newstrval("text/plain; charset=utf-8"));
    dict_set(response, "status", newintval(status));
    dict_set(response, "headers", headers);
    dict_set(response, "body", newbytesval(message));
    return response;
}

std::string http_platform_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "unix";
#endif
}

bool request_unsupported_feature(value_t *request,
                                 std::string &message, bool allow_sse,
                                 bool allow_websocket) {
    value_t *headers = dict_get(request, "headers");
    std::string upgrade =
        lower_ascii(header_value(headers, "Upgrade"));
    if (!allow_websocket &&
        upgrade.find("websocket") != std::string::npos) {
        message = "websocket is not supported";
        return true;
    }
    if (allow_sse)
        return false;
    std::string accept =
        lower_ascii(header_value(headers, "Accept"));
    if (accept.find("text/event-stream") != std::string::npos) {
        message = "sse is not supported";
        return true;
    }
    return false;
}

#if SKEIN_HAS_REUSE_PORT

struct reuse_port_server_state
    : std::enable_shared_from_this<reuse_port_server_state> {
    std::shared_ptr<http_shared_state> shared;
    std::shared_ptr<http_server_state> config;
    std::string host;
    int port = 0;
    std::string cert_file;
    std::string key_file;
    std::string client_ca_file;
    std::string client_auth;
    std::string script;
    std::string entry = "handle";
    bool worker_vm = false;
    bool http2_enabled = false;
    bool unsupported_501 = false;
    std::string unsupported_message;
    std::size_t worker_count = 0;
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t ready = 0;
    bool startup_failed = false;
    std::string startup_error;
    bool joined = false;
    std::atomic<long long> alive{0};
    std::atomic<long long> active{0};
    std::atomic<long long> completed{0};
    std::atomic<long long> failed{0};

    ~reuse_port_server_state() {
        stop_server();
        join_server();
    }

    void stop_server() {
        if (shared)
            shared->stopping.store(true);
    }

    bool wait_server(long long timeout_ms) {
        if (timeout_ms <= 0)
            return true;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        while (alive.load() > 0) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    void join_server() {
        bool should_join = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!joined) {
                joined = true;
                should_join = true;
            }
        }
        if (!should_join)
            return;
        for (std::thread &thread : threads) {
            if (thread.joinable())
                thread.join();
        }
    }

    long long alive_workers() const { return alive.load(); }
    long long active_requests() const { return active.load(); }
    long long completed_requests() const { return completed.load(); }
    long long failed_requests() const { return failed.load(); }
    std::size_t workers() const { return worker_count; }
    std::shared_ptr<http_shared_state> shared_state() const {
        return shared;
    }
    std::size_t max_requests() const {
        return config ? (std::size_t)config->max_requests : 0;
    }
};

void reuse_port_worker_fail(
    const std::shared_ptr<reuse_port_server_state> &server,
    const std::string &error) {
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        if (!server->startup_failed) {
            server->startup_failed = true;
            server->startup_error = error;
        }
    }
    server->cv.notify_all();
}

value_t *make_reuse_handler(
    const std::shared_ptr<reuse_port_server_state> &server,
    value_t *handler, std::size_t worker_index) {
    return newnativeval(
        [server, handler, worker_index](
            const std::vector<value_t *> &args) -> value_t * {
            if (args.empty() || !args[0] || args[0]->type != T_dict)
                return nullptr;
            if (server->unsupported_501)
                return thread_server_response(
                    501, server->unsupported_message);
            server->active.fetch_add(1);
            value_t *source = args[0];
            std::string unsupported;
            if (request_unsupported_feature(source, unsupported, true,
                                            true)) {
                server->active.fetch_sub(1);
                return thread_server_response(501, unsupported);
            }
            value_t *request = (value_t *)newdictval();
            const char *fields[] = {
                "method", "path", "headers", "body", "http_major",
                "http_minor", "keep_alive", "body_reader", "socket",
                "connection_id", "scheme", "authority"};
            for (const char *field : fields) {
                value_t *value = dict_get(source, field);
                if (value)
                    dict_set(request, field, dupval(value));
            }
            value_t *socket = dict_get(source, "socket");
            if (socket) {
                value_t *peer =
                    call_net(B_net_peer_addr, {dupval(socket)});
                if (peer && peer->type == T_dict) {
                    value_t *address = dict_get(peer, "address");
                    value_t *port = dict_get(peer, "port");
                    if (address)
                        dict_set(request, "remote_addr",
                                 dupval(address));
                    if (port)
                        dict_set(request, "remote_port",
                                 dupval(port));
                }
                freeval(peer);
            }
            dict_set(request, "worker_id",
                     newintval((long long)worker_index));

            value_t *response =
                skein_call_function(handler, {request});
            if (response && is_task_value(response))
                response = await_task_value(response);
            if (response)
                server->completed.fetch_add(1);
            else
                server->failed.fetch_add(1);
            server->active.fetch_sub(1);
            return response;
        },
        server);
}

void reuse_port_worker_main(
    std::shared_ptr<reuse_port_server_state> server,
    std::size_t worker_index,
    std::shared_ptr<const thread_env_snapshot> env_snapshot) {
    thread_env_install(env_snapshot);
    server->alive.fetch_add(1);
    std::string error;
    thread_worker_begin(server->worker_vm, error);

    struct value_t *handler = nullptr;
    bool loaded = false;
    try {
        package_configure(server->script, {});
        if (parse_worker_source(server->script, error)) {
            handler = lookup_global_symbol(server->entry.c_str());
            if (!callable_value(handler))
                error = "worker entry function not found: " +
                        server->entry;
            else
                loaded = true;
        }
    } catch (const std::exception &e) {
        error = e.what();
    } catch (...) {
        error = "worker startup failed";
    }

    if (!loaded) {
        reuse_port_worker_fail(server, error);
        thread_worker_end();
        server->alive.fetch_sub(1);
        return;
    }

    std::vector<value_t *> listen_args;
    listen_args.push_back(newstrval(server->host.c_str()));
    listen_args.push_back(newintval(server->port));
    listen_args.push_back(newintval(0));
    listen_args.push_back(newstrval("any"));
    value_t *options = (value_t *)newdictval();
    dict_set(options, "reuse_port", newboolval(true));
    listen_args.push_back(options);
    value_t *listener = call_net(B_net_tcp_listen, listen_args);
    if (!listener) {
        reuse_port_worker_fail(
            server, "could not bind reuse_port listener");
        thread_worker_end();
        server->alive.fetch_sub(1);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(server->mutex);
        ++server->ready;
    }
    server->cv.notify_all();

    std::shared_ptr<http_server_state> state =
        std::make_shared<http_server_state>(*server->config);
    state->shared = server->shared;
    state->tracked_connections =
        std::make_shared<http_connection_registry>();
    value_t *wrapped = make_reuse_handler(server, handler, worker_index);

    std::vector<std::string> alpn;
    if (!server->cert_file.empty()) {
        alpn.push_back("http/1.1");
        if (server->http2_enabled)
            alpn.insert(alpn.begin(), "h2");
    }

    http_connection_fn connection_fn;
#ifdef SKEIN_ENABLE_HTTP2
    if (server->http2_enabled) {
        auto serve_h2 = [](value_t *stream, value_t *handler,
                           http_server_state *state) {
            h2_connection_config config;
            config.max_requests = state->max_requests;
            config.max_per_connection = state->max_per_connection;
            config.idle_timeout_ms = state->idle_timeout_ms;
            config.max_body_bytes = state->max_body_bytes;
            config.stream_request_body = state->stream_request_body;
            config.on_request_body = nullptr;
            config.max_concurrent_streams =
                state->h2_max_concurrent_streams;
            http2_serve_connection(stream, handler, config, state->shared);
        };
        if (!server->cert_file.empty()) {
            connection_fn = [serve_h2](value_t *stream, value_t *handler,
                                       http_server_state *state) {
                std::string protocol;
                value_t *selected =
                    call_tls(B_tls_alpn, {dupval(stream)});
                if (selected && selected->type == T_string)
                    protocol = ((string_value_t *)selected)->value;
                freeval(selected);
                if (protocol == "h2") {
                    serve_h2(stream, handler, state);
                    return;
                }
                handle_http_server_connection(stream, handler, state);
            };
        } else {
            connection_fn = [serve_h2](value_t *stream, value_t *handler,
                                       http_server_state *state) {
                static const std::string preface =
                    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
                std::string peeked;
                bool h2 = false;
                for (int attempt = 0; attempt < 50; ++attempt) {
                    std::string chunk;
                    if (!net_peek(stream, chunk, preface.size(), 50))
                        break;
                    if (chunk.size() > preface.size())
                        chunk.resize(preface.size());
                    peeked = std::move(chunk);
                    if (preface.compare(0, peeked.size(), peeked) != 0)
                        break;
                    if (peeked.size() == preface.size()) {
                        h2 = true;
                        break;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }
                if (h2) {
                    serve_h2(stream, handler, state);
                    return;
                }
                handle_http_server_connection(stream, handler, state);
            };
        }
    }
#endif

    task_ptr task = start_http_server(
        listener, wrapped, state, server->cert_file, server->key_file,
        server->client_ca_file, server->client_auth, alpn, connection_fn);
    if (task) {
        value_t *result = fiber_await_task(task);
        freeval(result);
    }
    freeval(wrapped);
    freeval(listener);
    thread_worker_end();
    server->alive.fetch_sub(1);
}

std::shared_ptr<reuse_port_server_state> create_reuse_port_server(
    const std::string &host, int port, const std::string &script,
    const std::string &entry, std::size_t workers, bool worker_vm,
    const std::string &cert_file, const std::string &key_file,
    const std::string &client_ca_file, const std::string &client_auth,
    std::shared_ptr<http_server_state> config, std::string &error) {
    std::shared_ptr<reuse_port_server_state> server =
        std::make_shared<reuse_port_server_state>();
    server->shared = config->shared;
    server->config = std::move(config);
    server->http2_enabled = server->config->http2_enabled;
    server->unsupported_501 = server->config->unsupported_501;
    server->unsupported_message = server->config->unsupported_message;
    server->host = host;
    server->port = port;
    server->script = script;
    server->entry = entry;
    server->worker_vm = worker_vm;
    server->worker_count = workers;
    server->cert_file = cert_file;
    server->key_file = key_file;
    server->client_ca_file = client_ca_file;
    server->client_auth = client_auth;

    try {
        for (std::size_t i = 0; i < workers; ++i)
            server->threads.emplace_back(reuse_port_worker_main, server, i,
                                         thread_env_capture());
    } catch (const std::exception &) {
        error = "could not start reuse_port workers";
        server->stop_server();
        server->join_server();
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(server->mutex);
    {
        parser_lock_suspend parse_suspend;
        server->cv.wait(lock, [&server, workers] {
            return server->startup_failed || server->ready >= workers;
        });
    }
    if (server->startup_failed) {
        error = server->startup_error;
        lock.unlock();
        server->stop_server();
        server->join_server();
        return nullptr;
    }
    return server;
}

#endif

struct thread_server_state : std::enable_shared_from_this<thread_server_state> {
    std::shared_ptr<thread_pool_link> pool;
    std::shared_ptr<http_server_state> http;
    task_ptr server_task;
    std::string script;
    std::string strategy = "pool";
    long long worker_timeout_ms = 30000;
    bool joined = false;
    bool tls = false;
    bool unsupported_501 = false;
    std::string unsupported_message;
    std::mutex mutex;
    value_t *on_request_body = nullptr;
#if SKEIN_HAS_REUSE_PORT
    std::shared_ptr<reuse_port_server_state> reuse;
#endif

    ~thread_server_state() {
        freeval(on_request_body);
        on_request_body = nullptr;
#if SKEIN_HAS_REUSE_PORT
        if (reuse) {
            reuse->stop_server();
            reuse->join_server();
        }
#endif
    }
};

std::string thread_server_describe(const void *) {
    return "<thread_server>";
}

bool thread_server_wait(thread_server_state *server, long long timeout_ms) {
#if SKEIN_HAS_REUSE_PORT
    if (server->reuse)
        return server->reuse->wait_server(timeout_ms);
#endif
    if (!server->server_task)
        return true;
    return await_task_timeout(server->server_task, timeout_ms);
}

void thread_server_finish(thread_server_state *server) {
#if SKEIN_HAS_REUSE_PORT
    if (server->reuse) {
        server->reuse->join_server();
        return;
    }
#endif
    server->server_task = nullptr;
    thread_pool_link_join(server->pool);
}

void thread_server_force(thread_server_state *server) {
    if (server->http && server->http->force_stop)
        server->http->force_stop->store(true);
#if SKEIN_HAS_REUSE_PORT
    if (server->reuse)
        return;
#endif
    http_close_tracked_connections(server->http);
}

bool thread_server_shutdown(thread_server_state *server, long long timeout_ms,
                            long long &served) {
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        if (server->joined) {
            served = server->http && server->http->shared
                         ? server->http->shared->served.load()
                         : 0;
            return true;
        }
    }
    if (server->http && server->http->shared)
        server->http->shared->stopping.store(true);
    if (!thread_server_wait(server, timeout_ms)) {
        thread_server_force(server);
        if (!thread_server_wait(server, kHttpShutdownForceGraceMs))
            return false;
    }
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        server->joined = true;
    }
    thread_server_finish(server);
    served = server->http && server->http->shared
                 ? server->http->shared->served.load()
                 : 0;
    return true;
}

value_t *thread_dispatch(
    const std::shared_ptr<thread_server_state> &server,
    const std::vector<value_t *> &args) {
    if (args.size() != 1 || !args[0] || args[0]->type != T_dict)
        return thread_server_response(400, "invalid HTTP request");
    if (server->unsupported_501)
        return thread_server_response(501,
                                      server->unsupported_message);
    value_t *request = args[0];
    std::string unsupported;
    if (request_unsupported_feature(request, unsupported, false))
        return thread_server_response(501, unsupported);
    if (dict_get(request, "body_reader"))
        return thread_server_response(
            501, "stream_request_body is not supported");

    value_t *socket = dict_get(request, "socket");
    value_t *payload = (value_t *)newdictval();
    const char *fields[] = {"method", "path", "headers", "body",
                            "http_major", "http_minor", "keep_alive"};
    for (const char *field : fields) {
        value_t *field_value = dict_get(request, field);
        if (field_value)
            dict_set(payload, field, dupval(field_value));
    }
    if (socket) {
        value_t *peer = call_net(B_net_peer_addr, {dupval(socket)});
        if (peer && peer->type == T_dict) {
            value_t *address = dict_get(peer, "address");
            value_t *port = dict_get(peer, "port");
            if (address)
                dict_set(payload, "remote_addr", dupval(address));
            if (port)
                dict_set(payload, "remote_port", dupval(port));
        }
        freeval(peer);
    }

    std::shared_ptr<thread_future_link> future;
    std::string error;
    if (!thread_pool_link_submit(server->pool, payload,
                                 server->worker_timeout_ms, future,
                                 error)) {
        freeval(payload);
        int status = 502;
        if (error.find("thread.pool_failed") != std::string::npos)
            status = 503;
        else if (error.find("thread.timeout") != std::string::npos)
            status = 504;
        return thread_server_response(status, error);
    }
    freeval(payload);

    value_t *envelope = nullptr;
    thread_future_status wait_status = thread_future_link_wait(
        future, server->worker_timeout_ms, envelope, error);
    if (wait_status == thread_future_status::pending) {
        thread_future_link_cancel(future);
        freeval(envelope);
        return thread_server_response(504, "worker timed out");
    }
    if (wait_status == thread_future_status::canceled ||
        wait_status == thread_future_status::closed) {
        freeval(envelope);
        return thread_server_response(
            503, error.empty() ? "worker unavailable" : error);
    }

    bool ok = false;
    value_t *ok_value = envelope ? dict_get(envelope, "ok") : nullptr;
    if (ok_value && ok_value->type == T_bool)
        ok = ((bool_value_t *)ok_value)->value;
    if (!ok) {
        std::string kind;
        std::string message = "worker failed";
        value_t *err = envelope ? dict_get(envelope, "error") : nullptr;
        if (err) {
            kind = error_kind_of(err);
            message = error_message_of(err);
        }
        freeval(envelope);
        int status = 502;
        if (kind.find("pool_failed") != std::string::npos)
            status = 503;
        else if (kind.find("timeout") != std::string::npos)
            status = 504;
        else if (message.find("does not support generator") !=
                 std::string::npos)
            status = 501;
        return thread_server_response(status, message);
    }

    value_t *value = dict_get(envelope, "value");
    if (!value || value->type != T_dict) {
        freeval(envelope);
        return thread_server_response(
            502, "worker returned an invalid response");
    }
    long long status_code = 200;
    value_t *status_value = dict_get(value, "status");
    if (status_value && status_value->type == T_int)
        status_code = ((int_value_t *)status_value)->value;
    if (status_code < 100 || status_code > 999)
        status_code = 502;
    value_t *headers = dict_get(value, "headers");
    value_t *body = dict_get(value, "body");
    value_t *response = (value_t *)newdictval();
    dict_set(response, "status", newintval(status_code));
    dict_set(response, "headers",
             headers && headers->type == T_dict
                 ? dupval(headers)
                 : (value_t *)newdictval());
    dict_set(response, "body",
             body ? dupval(body) : newbytesval(""));
    freeval(envelope);
    return response;
}

value_t *thread_server_get_field(void *data, const char *name) {
    thread_server_state *raw = static_cast<thread_server_state *>(data);
    if (!raw || !name) {
        yyerror("std.http.ThreadServer has no such field");
        return nullptr;
    }
    std::shared_ptr<thread_server_state> server = raw->shared_from_this();

    if (std::strcmp(name, "stop") == 0) {
        return newnativeval(
            [server](const std::vector<value_t *> &args) {
                if (!args.empty()) {
                    yyerror("ThreadServer.stop expects no arguments");
                    return (value_t *)nullptr;
                }
                std::lock_guard<std::mutex> lock(server->mutex);
                if (server->http && server->http->shared)
                    server->http->shared->stopping.store(true);
                return newnull();
            },
            server);
    }
    if (std::strcmp(name, "join") == 0) {
        return newnativeval(
            [server](const std::vector<value_t *> &args) {
                long long timeout_ms = 0;
                if (args.size() > 1) {
                    yyerror("ThreadServer.join expects 0 or 1 argument");
                    return (value_t *)nullptr;
                }
                if (args.size() == 1) {
                    if (args[0]->type != T_int ||
                        ((int_value_t *)args[0])->value < 0) {
                        yyerror("ThreadServer.join timeout must be a "
                                "non-negative integer");
                        return (value_t *)nullptr;
                    }
                    timeout_ms = ((int_value_t *)args[0])->value;
                }
                bool should_join = false;
                {
                    std::lock_guard<std::mutex> lock(server->mutex);
                    should_join = !server->joined;
                }
                if (should_join) {
                    if (!thread_server_wait(server.get(), timeout_ms)) {
                        yyerror("std.http.ThreadServer.join timed out");
                        return (value_t *)nullptr;
                    }
                    {
                        std::lock_guard<std::mutex> lock(server->mutex);
                        server->joined = true;
                    }
                    thread_server_finish(server.get());
                }
                long long served =
                    server->http && server->http->shared
                        ? server->http->shared->served.load()
                        : 0;
                return newintval(served);
            },
            server);
    }
    if (std::strcmp(name, "shutdown") == 0) {
        return newnativeval(
            [server](const std::vector<value_t *> &args) {
                long long timeout_ms = 0;
                if (args.size() > 1) {
                    yyerror("ThreadServer.shutdown expects 0 or 1 argument");
                    return (value_t *)nullptr;
                }
                if (args.size() == 1) {
                    if (args[0]->type != T_int ||
                        ((int_value_t *)args[0])->value < 0) {
                        yyerror("ThreadServer.shutdown timeout must be a "
                                "non-negative integer");
                        return (value_t *)nullptr;
                    }
                    timeout_ms = ((int_value_t *)args[0])->value;
                }
                long long served = 0;
                if (!thread_server_shutdown(server.get(), timeout_ms,
                                            served)) {
                    yyerror("std.http.ThreadServer.shutdown timed out");
                    return (value_t *)nullptr;
                }
                return newintval(served);
            },
            server);
    }
    if (std::strcmp(name, "stats") == 0) {
        return newnativeval(
            [server](const std::vector<value_t *> &args) {
                if (!args.empty()) {
                    yyerror("ThreadServer.stats expects no arguments");
                    return (value_t *)nullptr;
                }
                value_t *stats = nullptr;
#if SKEIN_HAS_REUSE_PORT
                if (server->reuse) {
                    std::shared_ptr<http_shared_state> shared =
                        server->reuse->shared_state();
                    long long workers =
                        (long long)server->reuse->workers();
                    long long alive = server->reuse->alive_workers();
                    stats = (value_t *)newdictval();
                    dict_set(stats, "size", newintval(workers));
                    dict_set(stats, "workers", newintval(workers));
                    dict_set(stats, "ready", newintval(alive));
                    dict_set(stats, "alive", newintval(alive));
                    dict_set(stats, "dead",
                             newintval(workers > alive ? workers - alive : 0));
                    dict_set(stats, "restarts", newintval(0));
                    dict_set(stats, "restart_limit", newintval(0));
                    dict_set(stats, "submitted",
                             newintval(shared
                                           ? shared->claimed.load()
                                           : 0));
                    dict_set(stats, "completed",
                             newintval(
                                 server->reuse->completed_requests()));
                    dict_set(stats, "failed",
                             newintval(server->reuse->failed_requests()));
                    dict_set(stats, "active",
                             newintval(server->reuse->active_requests()));
                    dict_set(stats, "queued", newintval(0));
                    dict_set(stats, "served",
                             newintval(shared
                                           ? shared->served.load()
                                           : 0));
                    dict_set(stats, "stopping",
                             newboolval(shared
                                            ? shared->stopping.load()
                                            : false));
                    dict_set(stats, "connections",
                             newintval(shared
                                           ? shared->active_connections.load()
                                           : 0));
                    dict_set(stats, "strategy",
                             newstrval(server->strategy.c_str()));
                    dict_set(stats, "max_requests",
                             newintval(
                                 (long long)server->reuse->max_requests()));
                    return stats;
                }
#endif
                stats = thread_pool_link_stats(server->pool);
                if (!stats)
                    return (value_t *)nullptr;
                dict_set(stats, "served",
                         newintval(server->http && server->http->shared
                                       ? server->http->shared->served.load()
                                       : 0));
                dict_set(stats, "stopping",
                         newboolval(server->http && server->http->shared
                                        ? server->http->shared->stopping.load()
                                        : false));
                dict_set(stats, "connections",
                         newintval(server->http && server->http->shared
                                       ? server->http->shared
                                             ->active_connections.load()
                                       : 0));
                dict_set(stats, "strategy",
                         newstrval(server->strategy.c_str()));
                dict_set(stats, "max_requests",
                         newintval(server->http
                                       ? server->http->max_requests
                                       : 0));
                return stats;
            },
            server);
    }
    if (std::strcmp(name, "strategy") == 0)
        return newstrval(server->strategy.c_str());
    if (std::strcmp(name, "served") == 0)
        return newintval(server->http && server->http->shared
                             ? server->http->shared->served.load()
                             : 0);
    if (std::strcmp(name, "capabilities") == 0) {
        bool http2 =
            server->http && server->http->http2_enabled;
        bool stream_body =
            server->strategy == "reuse_port" && server->http &&
            server->http->stream_request_body;
        return capabilities_value(make_capabilities(
            server->strategy, server->tls, http2, stream_body));
    }

    yyerror("std.http.ThreadServer has no field %s", name);
    return nullptr;
}

const native_type kThreadServerType = {
    "thread_server",
    thread_server_describe,
    nullptr,
    thread_server_get_field,
    nullptr,
    nullptr,
};

std::shared_ptr<http_shared_state> map_shared_state_file(
    const std::string &path, bool create, std::string &error) {
#if defined(_WIN32)
    (void)path;
    (void)create;
    error = "shared state is not supported on this platform";
    return nullptr;
#else
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        error = "cannot open shared state: " +
                std::string(std::strerror(errno));
        return nullptr;
    }
    if (::ftruncate(fd, (off_t)sizeof(http_shared_state)) != 0) {
        int saved = errno;
        ::close(fd);
        error = "cannot size shared state: " +
                std::string(std::strerror(saved));
        return nullptr;
    }
    void *memory = ::mmap(nullptr, sizeof(http_shared_state),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    ::close(fd);
    if (memory == MAP_FAILED) {
        error = "cannot map shared state: " +
                std::string(std::strerror(saved));
        return nullptr;
    }
    auto *state = reinterpret_cast<http_shared_state *>(memory);
    if (create)
        new (state) http_shared_state();
    return std::shared_ptr<http_shared_state>(
        state, [](http_shared_state *value) {
            ::munmap(value, sizeof(http_shared_state));
        });
#endif
}

value_t *http_serve_threads(const std::vector<value_t *> &args) {
    if (args.size() < 3 || args.size() > 4 ||
        args[0]->type != T_dict || args[1]->type != T_int ||
        ((int_value_t *)args[1])->value <= 0 ||
        args[2]->type != T_string) {
        yyerror("std.http.serve_threads expects server, workers, script "
                "and optional options");
        return NULL;
    }
    value_t *socket = dict_get(args[0], "socket");
    long long workers = ((int_value_t *)args[1])->value;
    std::string script = ((string_value_t *)args[2])->value;

    value_t *options = nullptr;
    if (args.size() == 4) {
        if (args[3]->type != T_dict) {
            yyerror("std.http.serve_threads options must be a dict");
            return NULL;
        }
        options = args[3];
    }
    auto option = [&options, &args](const char *key) -> value_t * {
        value_t *value = options ? dict_get(options, key) : nullptr;
        if (!value)
            value = dict_get(args[0], key);
        return value;
    };
    auto read_non_negative = [&option](const char *key,
                                       long long &out) -> bool {
        value_t *value = option(key);
        if (!value)
            return true;
        if (value->type != T_int ||
            ((int_value_t *)value)->value < 0) {
            yyerror("std.http.serve_threads %s must be a non-negative "
                    "integer", key);
            return false;
        }
        out = ((int_value_t *)value)->value;
        return true;
    };

    std::string entry = "handle";
    value_t *entry_value = options ? dict_get(options, "entry") : nullptr;
    if (entry_value) {
        if (entry_value->type != T_string) {
            yyerror("std.http.serve_threads entry must be a string");
            return NULL;
        }
        entry = ((string_value_t *)entry_value)->value;
    }
    long long queue_capacity = 1024;
    value_t *queue = options ? dict_get(options, "queue") : nullptr;
    if (queue) {
        if (queue->type == T_int && ((int_value_t *)queue)->value > 0)
            queue_capacity = ((int_value_t *)queue)->value;
        else if (queue->type == T_string &&
                 std::strcmp(((string_value_t *)queue)->value,
                             "unbounded") == 0)
            queue_capacity = -1;
        else {
            yyerror("std.http.serve_threads queue must be positive or "
                    "\"unbounded\"");
            return NULL;
        }
    }
    long long restart_limit = 0;
    if (!read_non_negative("restart_limit", restart_limit))
        return NULL;
    bool worker_vm = false;
    value_t *worker_vm_value =
        options ? dict_get(options, "worker_vm") : nullptr;
    if (worker_vm_value) {
        if (worker_vm_value->type != T_bool) {
            yyerror("std.http.serve_threads worker_vm must be a bool");
            return NULL;
        }
        worker_vm = ((bool_value_t *)worker_vm_value)->value;
    }

    long long max_requests = 0;
    long long max_per_connection = 100;
    long long idle_timeout_ms = 0;
    long long max_connections = 0;
    long long worker_timeout_ms = 30000;
    long long h2_max_concurrent_streams = 16;
    if (!read_non_negative("max_requests", max_requests) ||
        !read_non_negative("max_per_connection", max_per_connection) ||
        !read_non_negative("idle_timeout_ms", idle_timeout_ms) ||
        !read_non_negative("max_connections", max_connections) ||
        !read_non_negative("worker_timeout_ms", worker_timeout_ms) ||
        !read_non_negative("h2_max_concurrent_streams",
                           h2_max_concurrent_streams))
        return NULL;

    std::size_t max_header_bytes = 64 * 1024;
    std::size_t max_headers = 100;
    std::size_t max_body_bytes = 16 * 1024 * 1024;
    if (!positive_size_option(args[0], "max_header_bytes",
                              max_header_bytes) ||
        !positive_size_option(args[0], "max_headers", max_headers) ||
        !positive_size_option(args[0], "max_body_bytes", max_body_bytes))
        return NULL;
    if (options &&
        (!positive_size_option(options, "max_header_bytes",
                               max_header_bytes) ||
         !positive_size_option(options, "max_headers", max_headers) ||
         !positive_size_option(options, "max_body_bytes",
                               max_body_bytes)))
        return NULL;

    bool stream_request_body = false;
    value_t *stream_body_value = option("stream_request_body");
    if (stream_body_value && stream_body_value->type != T_null) {
        if (stream_body_value->type != T_bool) {
            yyerror("std.http.serve_threads stream_request_body must be "
                    "a bool");
            return NULL;
        }
        stream_request_body =
            ((bool_value_t *)stream_body_value)->value;
    }

    value_t *on_request_body = dict_get(args[0], "on_request_body");
    if (on_request_body && on_request_body->type == T_null)
        on_request_body = nullptr;
    if (on_request_body && !callable_value(on_request_body)) {
        yyerror("std.http.serve_threads on_request_body must be a "
                "function");
        return NULL;
    }

    std::string cert_file;
    std::string key_file;
    std::string client_ca_file;
    std::string client_auth;
    value_t *cert_value = dict_get(args[0], "cert");
    value_t *key_value = dict_get(args[0], "key");
    if (cert_value || key_value) {
        if (!cert_value || !key_value ||
            cert_value->type != T_string || key_value->type != T_string) {
            yyerror("std.http.serve_threads: invalid TLS certificate");
            return NULL;
        }
        cert_file = ((string_value_t *)cert_value)->value;
        key_file = ((string_value_t *)key_value)->value;
    }
    value_t *client_ca_value = dict_get(args[0], "client_ca_file");
    value_t *client_auth_value = dict_get(args[0], "client_auth");
    if (client_ca_value) {
        if (client_ca_value->type != T_string) {
            yyerror("std.http.serve_threads: invalid client_ca_file");
            return NULL;
        }
        client_ca_file = ((string_value_t *)client_ca_value)->value;
    }
    if (client_auth_value) {
        if (client_auth_value->type != T_string) {
            yyerror("std.http.serve_threads: invalid client_auth");
            return NULL;
        }
        client_auth = ((string_value_t *)client_auth_value)->value;
    }

    std::string strategy = "auto";
    value_t *strategy_value = option("strategy");
    if (strategy_value) {
        if (strategy_value->type != T_string) {
            yyerror("std.http.serve_threads strategy must be a string");
            return NULL;
        }
        strategy = ((string_value_t *)strategy_value)->value;
    }
    if (strategy == "auto") {
#if SKEIN_HAS_REUSE_PORT
        strategy = "reuse_port";
#else
        strategy = "dispatch";
#endif
    }
    if (strategy != "dispatch" && strategy != "reuse_port") {
        yyerror("std.http.serve_threads strategy must be "
                "\"auto\", \"dispatch\" or \"reuse_port\"");
        return NULL;
    }
#if !SKEIN_HAS_REUSE_PORT
    if (strategy == "reuse_port") {
        yyerror("std.http.serve_threads: reuse_port is not supported "
                "on this platform");
        return NULL;
    }
#endif
    if (strategy == "reuse_port" && on_request_body) {
        yyerror("std.http.serve_threads: on_request_body is not "
                "supported by reuse_port");
        return NULL;
    }
    if (stream_request_body && strategy != "reuse_port") {
        yyerror("std.http.serve_threads: stream_request_body requires "
                "reuse_port");
        return NULL;
    }

    std::string on_unsupported = "error";
    value_t *on_unsupported_value = option("on_unsupported");
    if (on_unsupported_value) {
        if (on_unsupported_value->type != T_string) {
            yyerror("std.http.serve_threads on_unsupported must be a "
                    "string");
            return NULL;
        }
        on_unsupported =
            ((string_value_t *)on_unsupported_value)->value;
    }
    if (on_unsupported != "error" && on_unsupported != "501" &&
        on_unsupported != "warn") {
        yyerror("std.http.serve_threads on_unsupported must be "
                "\"error\", \"501\" or \"warn\"");
        return NULL;
    }

    std::vector<std::string> required;
    value_t *require_value = option("require");
    if (require_value && require_value->type != T_null) {
        if (require_value->type == T_string) {
            required.push_back(
                ((string_value_t *)require_value)->value);
        } else if (require_value->type == T_list) {
            for (value_t *item :
                 ((list_value_t *)require_value)->payload->items) {
                if (!item || item->type != T_string) {
                    yyerror("std.http.serve_threads require entries "
                            "must be strings");
                    return NULL;
                }
                required.push_back(
                    ((string_value_t *)item)->value);
            }
        } else {
            yyerror("std.http.serve_threads require must be a string "
                    "or list of strings");
            return NULL;
        }
    }

    bool http2_capable = false;
#ifdef SKEIN_ENABLE_HTTP2
    http2_capable = strategy == "reuse_port";
#endif
    http_capabilities caps = make_capabilities(
        strategy, !cert_file.empty(), http2_capable,
        strategy == "reuse_port" ? stream_request_body : false);
    std::string missing;
    for (const std::string &name : required) {
        if (capability_enabled(caps, name))
            continue;
        if (!missing.empty())
            missing += ", ";
        missing += name;
    }
    bool unsupported_501 = false;
    std::string unsupported_message;
    if (!missing.empty()) {
        if (on_unsupported == "error") {
            const std::string message =
                "std.http.serve_threads: missing capabilities: " + missing;
            eval_raise(newerrorval("http.unsupported", message.c_str()));
            return NULL;
        }
        if (on_unsupported == "warn") {
            std::fprintf(stderr,
                         "std.http.serve_threads: missing capabilities: "
                         "%s\n", missing.c_str());
            std::fflush(stderr);
        } else {
            unsupported_501 = true;
            unsupported_message =
                "unsupported capabilities: " + missing;
        }
    }

    std::shared_ptr<http_server_state> state =
        std::make_shared<http_server_state>();
    state->max_requests = max_requests;
    state->max_per_connection = max_per_connection;
    state->idle_timeout_ms = idle_timeout_ms;
    state->max_connections = max_connections;
    state->stream_request_body =
        strategy == "reuse_port" ? stream_request_body : false;
    state->max_header_bytes = max_header_bytes;
    state->max_headers = max_headers;
    state->max_body_bytes = max_body_bytes;
    state->h2_max_concurrent_streams = h2_max_concurrent_streams;
    state->unsupported_501 = unsupported_501;
    state->unsupported_message = unsupported_message;
#ifdef SKEIN_ENABLE_HTTP2
    state->http2_enabled =
        strategy == "reuse_port";
#else
    state->http2_enabled = false;
#endif
    value_t *shared_path_value =
        options ? dict_get(options, "shared_state_path") : nullptr;
    if (shared_path_value) {
        if (shared_path_value->type != T_string) {
            yyerror("std.http.serve_threads shared_state_path must be a "
                    "string");
            return NULL;
        }
        std::string shared_error;
        state->shared = map_shared_state_file(
            ((string_value_t *)shared_path_value)->value, false,
            shared_error);
        if (!state->shared) {
            yyerror("std.http.serve_threads: %s", shared_error.c_str());
            return NULL;
        }
    } else {
        state->shared = std::make_shared<http_shared_state>();
    }
    if (options) {
        value_t *unsupported = dict_get(options, "unsupported_501");
        if (unsupported) {
            if (unsupported->type != T_bool) {
                yyerror("std.http.serve_threads unsupported_501 must be "
                        "a bool");
                return NULL;
            }
            state->unsupported_501 =
                ((bool_value_t *)unsupported)->value;
        }
        value_t *unsupported_text =
            dict_get(options, "unsupported_message");
        if (unsupported_text) {
            if (unsupported_text->type != T_string) {
                yyerror("std.http.serve_threads unsupported_message "
                        "must be a string");
                return NULL;
            }
            state->unsupported_message =
                ((string_value_t *)unsupported_text)->value;
        }
    }

    std::shared_ptr<thread_server_state> server =
        std::make_shared<thread_server_state>();
    server->http = state;
    server->script = script;
    server->strategy = strategy;
    server->worker_timeout_ms = worker_timeout_ms;
    server->tls = !cert_file.empty();
    server->unsupported_501 = state->unsupported_501;
    server->unsupported_message = state->unsupported_message;
    if (args[0]->type == T_dict) {
        dict_set(args[0], "strategy", newstrval(strategy.c_str()));
        dict_set(args[0], "platform",
                 newstrval(http_platform_name().c_str()));
        dict_set(args[0], "capabilities", capabilities_value(caps));
    }

    std::string error;
#if SKEIN_HAS_REUSE_PORT
    if (strategy == "reuse_port") {
        value_t *host_value = dict_get(args[0], "host");
        value_t *port_value = dict_get(args[0], "port");
        if (!host_value || host_value->type != T_string ||
            !port_value || port_value->type != T_int) {
            yyerror("std.http.serve_threads: reuse_port requires an "
                    "explicit host and port");
            return NULL;
        }
        std::string host = ((string_value_t *)host_value)->value;
        long long port = ((int_value_t *)port_value)->value;
        if (socket)
            call_net(B_net_close, {dupval(socket)});
        server->reuse = create_reuse_port_server(
            host, (int)port, script, entry, (std::size_t)workers,
            worker_vm, cert_file, key_file, client_ca_file, client_auth,
            state, error);
        if (!server->reuse) {
            yyerror("std.http.serve_threads: %s", error.c_str());
            return NULL;
        }
        return newuserdataval(&kThreadServerType, server);
    }
#endif

    if (!socket) {
        yyerror("std.http.serve_threads expects a std.http server for "
                "dispatch");
        return NULL;
    }
    std::shared_ptr<thread_pool_link> pool = thread_pool_link_create(
        (std::size_t)workers, script, entry, queue_capacity, restart_limit,
        worker_vm, error);
    if (!pool) {
        yyerror("std.http.serve_threads: %s", error.c_str());
        return NULL;
    }
    server->pool = pool;
    if (on_request_body)
        server->on_request_body = dupval(on_request_body);
    state->on_request_body = server->on_request_body;

    value_t *handler = newnativeval(
        [server](const std::vector<value_t *> &call_args) {
            return thread_dispatch(server, call_args);
        },
        server);

    task_ptr task =
        start_http_server(socket, handler, state, cert_file, key_file,
                          client_ca_file, client_auth);
    freeval(handler);
    if (!task) {
        thread_pool_link_close(pool);
        thread_pool_link_join(pool);
        yyerror("std.http.serve_threads could not start the server");
        return NULL;
    }
    server->server_task = task;
    return newuserdataval(&kThreadServerType, server);
}

value_t *http_thread_capabilities(
    const std::vector<value_t *> &args) {
    if (args.size() > 1 ||
        (args.size() == 1 && args[0]->type != T_dict)) {
        yyerror("std.http.thread_capabilities expects optional options "
                "dict");
        return NULL;
    }
    std::string strategy = "auto";
    if (args.size() == 1) {
        value_t *value = dict_get(args[0], "strategy");
        if (value) {
            if (value->type != T_string) {
                yyerror("std.http.thread_capabilities strategy must be "
                        "a string");
                return NULL;
            }
            strategy = ((string_value_t *)value)->value;
        }
    }
    if (strategy != "auto" && strategy != "dispatch" &&
        strategy != "reuse_port") {
        yyerror("std.http.thread_capabilities strategy must be "
                "\"auto\", \"dispatch\" or \"reuse_port\"");
        return NULL;
    }
    if (strategy == "auto") {
#if SKEIN_HAS_REUSE_PORT
        strategy = "reuse_port";
#else
        strategy = "dispatch";
#endif
    }
#if !SKEIN_HAS_REUSE_PORT
    if (strategy == "reuse_port") {
        yyerror("std.http.thread_capabilities: reuse_port is not "
                "supported on this platform");
        return NULL;
    }
#endif
    bool tls = false;
#ifdef SKEIN_ENABLE_TLS
    tls = true;
#endif
    bool http2 = false;
#ifdef SKEIN_ENABLE_HTTP2
    http2 = strategy == "reuse_port";
#endif
    http_capabilities caps = make_capabilities(
        strategy, tls, http2, strategy == "reuse_port");
    return capabilities_value(caps);
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
        case B_http_serve_processes:
            return http_serve_processes(args);
        case B_http_serve_threads:
            return http_serve_threads(args);
        case B_http_upgrade:
            return http_upgrade(args);
        case B_http_cookies:
            return http_cookies(args);
        case B_http_read_body:
            return http_read_body(args);
        case B_http_close:
            return http_close(args);
        case B_http_thread_capabilities:
            return http_thread_capabilities(args);
        default:
            yyerror("unknown http function");
            return NULL;
    }
}
