#include "http.h"

#include "ast.h"
#include "eval.h"
#include "fiber.h"
#include "http1_parser.h"
#include "io.h"
#include "net.h"
#include "repl.h"
#include "task.h"
#ifdef CALC_ENABLE_TLS
#include "tls.h"
#endif
#include "valuetype.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <memory>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

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

#ifdef CALC_ENABLE_TLS
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
                  bool *timed_out = NULL) {
    if (clean_eof)
        *clean_eof = false;
    if (timed_out)
        *timed_out = false;
    http1::parser parser(request);
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

value_t *make_response(const http1::message &message) {
    value_t *response = (value_t *)newdictval();
    dict_set(response, "status", newintval(message.status_code));
    dict_set(response, "status_text",
             newstrval(message.status_text.c_str()));
    dict_set(response, "headers", headers_dict(message));
    dict_set(response, "body", newbytesval(message.body));
    dict_set(response, "http_major", newintval(message.http_major));
    dict_set(response, "http_minor", newintval(message.http_minor));
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

std::string build_response(int status, value_t *headers,
                           const std::string &body, bool keep_alive,
                           std::string &error) {
    std::string response = "HTTP/1.1 ";
    response += std::to_string(status);
    response += " ";
    response += reason_phrase(status);
    response += "\r\n";
    if (!body.empty() && !header_exists(headers, "Content-Length")) {
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
    response += body;
    return response;
}

std::string build_request(const std::string &method, const url_parts &url,
                          value_t *headers, const std::string &body,
                          bool keep_alive) {
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
    if (!header_exists(headers, "Connection"))
        request += keep_alive ? "Connection: keep-alive\r\n"
                              : "Connection: close\r\n";
    if (!body.empty() && !header_exists(headers, "Content-Length")) {
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
    request += body;
    return request;
}

value_t *do_request(const std::string &method, const std::string &url_text,
                    value_t *options, value_t *body_override) {
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

    std::string body;
    value_t *body_value_option =
        body_override ? body_override : dict_get(options, "body");
    if (!body_value(body_value_option, body, "std.http"))
        return NULL;

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
    std::string request =
        build_request(method, url, headers, body, true);
    if (request.empty())
        return NULL;

    bool idempotent = method == "GET" || method == "HEAD" ||
                      method == "OPTIONS" || method == "TRACE";
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
#ifdef CALC_ENABLE_TLS
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
        http1::message message;
        if (ok)
            ok = read_message(stream, false, timeout_ms, message);

        eval_capture_result captured;
        if (capture_errors)
            captured = eval_capture_end(snapshot);

        if (!ok) {
            freeval(captured.error);
            pool_discard(connection);
            if (capture_errors && idempotent && !g_interrupted)
                continue;
            return NULL;
        }

        freeval(captured.error);
        if (message.keep_alive)
            pool_release(connection);
        else
            pool_discard(connection);
        return make_response(message);
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
                        long long timeout_ms, http1::message &message) {
    http1::parser parser(request);
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
            yyerror("http: parse error: %s", parser.error().c_str());
            return false;
        }
    }

    if (parser.messages().empty()) {
        yyerror("http: incomplete message");
        return false;
    }
    message = parser.messages()[0];
    return true;
}

value_t *do_request_async(const std::string &method,
                          const std::string &url_text, value_t *options,
                          value_t *body_override) {
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

    std::string body;
    value_t *body_option =
        body_override ? body_override : dict_get(options, "body");
    if (!body_value(body_option, body, "std.http"))
        return NULL;

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

    std::string request =
        build_request(method, url, headers, body, true);
    if (request.empty())
        return NULL;

    std::string host = url.host;
    int port = url.port;
    bool use_tls = url.tls;
    std::string key = connection_key(url, tls_verify, tls_ca_file);
    bool idempotent = method == "GET" || method == "HEAD" ||
                      method == "OPTIONS" || method == "TRACE";
    task_ptr task = fiber_start_async(
        [host, port, request, timeout_ms, use_tls, tls_verify,
         tls_ca_file, key, idempotent]() {
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
#ifdef CALC_ENABLE_TLS
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
            http1::message message;
            if (ok)
                ok = read_message_async(stream, false, timeout_ms, message);

            eval_capture_result captured;
            if (capture_errors)
                captured = eval_capture_end(snapshot);

            if (!ok) {
                freeval(captured.error);
                pool_discard(connection);
                if (capture_errors && idempotent && !g_interrupted)
                    continue;
                return (value_t *)NULL;
            }

            freeval(captured.error);
            if (message.keep_alive)
                pool_release(connection);
            else
                pool_discard(connection);
            return make_response(message);
        }
        return (value_t *)NULL;
    });
    if (!task)
        return NULL;
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
    value_t *options = args.size() == 3 ? args[2] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http.post_async options must be a dict");
        return NULL;
    }
    return do_request_async("POST", ((string_value_t *)args[0])->value,
                            options, args[1]);
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
    value_t *options = args.size() == 3 ? args[2] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http.post options must be a dict");
        return NULL;
    }
    return do_request("POST", ((string_value_t *)args[0])->value, options,
                      args[1]);
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
#ifndef CALC_ENABLE_TLS
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
#ifdef CALC_ENABLE_TLS
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
    if (!read_message(stream, true, timeout_ms, message)) {
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

    value_t *listener = dupval(socket);
    task_ptr task = fiber_start_async([listener, timeout_ms, cert_file,
                                       key_file]() {
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
#ifdef CALC_ENABLE_TLS
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
        if (!read_message_async(stream, true, timeout_ms, message)) {
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
    if (args.size() == 4 && !body_value(args[3], body, "std.http.respond"))
        return NULL;

    std::string error;
    std::string response =
        build_response((int)status, headers, body, false, error);
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
    if (args.size() == 4 &&
        !body_value(args[3], body, "std.http.respond_async"))
        return NULL;

    std::string error;
    std::string response =
        build_response((int)status, headers, body, false, error);
    if (response.empty()) {
        yyerror("std.http.respond_async: %s", error.c_str());
        return NULL;
    }

    value_t *client = dupval(socket);
    task_ptr task = fiber_start_async([client, response]() {
        bool ok = async_write(client, response, 0);
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

    long long served = 0;
    while (max_requests == 0 || served < max_requests) {
        std::vector<value_t *> accept_args;
        accept_args.push_back(dupval(socket));
        accept_args.push_back(newintval(0));
        value_t *client = call_net(B_net_accept, accept_args);
        if (!client)
            break;

        value_t *stream = client;
        if (!cert_file.empty()) {
#ifdef CALC_ENABLE_TLS
            std::vector<value_t *> tls_args;
            tls_args.push_back(dupval(client));
            tls_args.push_back(newstrval(cert_file.c_str()));
            tls_args.push_back(newstrval(key_file.c_str()));
            stream = call_tls(B_tls_server, tls_args);
            if (!stream) {
                close_socket(client);
                freeval(client);
                break;
            }
            freeval(client);
#else
            yyerror("std.http.serve: TLS is disabled");
            close_socket(client);
            freeval(client);
            break;
#endif
        }

        bool connection_open = true;
        long long connection_requests = 0;
        while (connection_open &&
               (max_requests == 0 || served < max_requests)) {
            http1::message message;
            bool clean_eof = false;
            bool timed_out = false;
            if (!read_message(stream, true, idle_timeout_ms, message,
                              &clean_eof, &timed_out))
                break;
            if (clean_eof || timed_out)
                break;

            value_t *request = make_request(stream, message);
            value_t *response =
                call_value_args(args[1], std::vector<value_t *>{request});
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
            if (!body_value(dict_get(response, "body"), body,
                            "std.http.serve")) {
                freeval(response);
                connection_open = false;
                break;
            }

            bool keep_alive =
                message.keep_alive && !headers_request_close(headers);
            if (max_requests != 0 && served + 1 >= max_requests)
                keep_alive = false;
            if (max_per_connection != 0 &&
                connection_requests + 1 >= max_per_connection)
                keep_alive = false;
            std::string error;
            std::string response_text = build_response(
                (int)status, headers, body, keep_alive, error);
            if (response_text.empty()) {
                yyerror("std.http.serve: %s", error.c_str());
                freeval(response);
                connection_open = false;
                break;
            }
            freeval(response);

            std::vector<value_t *> write_args;
            write_args.push_back(dupval(stream));
            write_args.push_back(newbytesval(response_text));
            write_args.push_back(newintval(0));
            value_t *written = call_io(B_io_write_all, write_args);
            if (!written) {
                connection_open = false;
                break;
            }
            freeval(written);
            ++served;
            ++connection_requests;
            if (!keep_alive)
                connection_open = false;
        }

        close_socket(stream);
        freeval(stream);
    }
    return newintval(served);
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
        case B_http_close:
            return http_close(args);
        default:
            yyerror("unknown http function");
            return NULL;
    }
}
