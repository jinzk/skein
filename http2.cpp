#include "http2.h"

#include "ast.h"
#include "eval.h"
#include "fiber.h"
#include "io.h"
#include "net.h"
#include "repl.h"
#include "task.h"
#ifdef CALC_ENABLE_TLS
#include "tls.h"
#endif
#include "valuetype.h"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

const std::size_t DEFAULT_MAX_BODY_BYTES = 16 * 1024 * 1024;

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

value_t *dict_get(value_t *dict, const char *key) {
    if (!dict || dict->type != T_dict)
        return NULL;
    dict_value_t *object = (dict_value_t *)dict;
    for (size_t i = 0; i < object->payload->items.size(); ++i) {
        value_t *item_key = object->payload->items[i].first;
        if (item_key->type == T_string &&
            std::strcmp(((string_value_t *)item_key)->value, key) == 0)
            return object->payload->items[i].second;
    }
    return NULL;
}

bool build_h2_settings(value_t *settings, bool server,
                       std::vector<nghttp2_settings_entry> &entries) {
    entries.clear();
    if (!settings || settings->type == T_null)
        return true;
    if (settings->type != T_dict) {
        yyerror("std.http2: settings must be a dict");
        return false;
    }
    dict_value_t *dict = (dict_value_t *)settings;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        value_t *key = dict->payload->items[i].first;
        value_t *value = dict->payload->items[i].second;
        if (key->type != T_string) {
            yyerror("std.http2: settings keys must be strings");
            return false;
        }
        std::string name = ((string_value_t *)key)->value;
        nghttp2_settings_entry entry;
        if (name == "max_concurrent_streams") {
            if (value->type != T_int ||
                ((int_value_t *)value)->value <= 0) {
                yyerror("std.http2: max_concurrent_streams must be a "
                        "positive integer");
                return false;
            }
            entry.settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
            entry.value = (uint32_t)((int_value_t *)value)->value;
        } else if (name == "initial_window_size") {
            if (value->type != T_int ||
                ((int_value_t *)value)->value <= 0 ||
                ((int_value_t *)value)->value > 2147483647LL) {
                yyerror("std.http2: initial_window_size must be between 1 "
                        "and 2147483647");
                return false;
            }
            entry.settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
            entry.value = (uint32_t)((int_value_t *)value)->value;
        } else if (name == "max_frame_size") {
            if (value->type != T_int ||
                ((int_value_t *)value)->value < 16384 ||
                ((int_value_t *)value)->value > 16777215LL) {
                yyerror("std.http2: max_frame_size must be between 16384 "
                        "and 16777215");
                return false;
            }
            entry.settings_id = NGHTTP2_SETTINGS_MAX_FRAME_SIZE;
            entry.value = (uint32_t)((int_value_t *)value)->value;
        } else if (name == "max_header_list_size") {
            if (value->type != T_int ||
                ((int_value_t *)value)->value <= 0) {
                yyerror("std.http2: max_header_list_size must be a "
                        "positive integer");
                return false;
            }
            entry.settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE;
            entry.value = (uint32_t)((int_value_t *)value)->value;
        } else if (name == "enable_push") {
            if (server) {
                yyerror("std.http2: enable_push is only valid for clients");
                return false;
            }
            if (value->type != T_bool) {
                yyerror("std.http2: enable_push must be a bool");
                return false;
            }
            entry.settings_id = NGHTTP2_SETTINGS_ENABLE_PUSH;
            entry.value = ((bool_value_t *)value)->value ? 1 : 0;
        } else {
            yyerror("std.http2: unknown setting '%s'", name.c_str());
            return false;
        }
        entries.push_back(entry);
    }
    return true;
}

std::string h2_settings_tag(
    const std::vector<nghttp2_settings_entry> &settings) {
    if (settings.empty())
        return std::string();
    std::string tag = "|settings:";
    for (size_t i = 0; i < settings.size(); ++i) {
        tag += std::to_string((int)settings[i].settings_id);
        tag += "=";
        tag += std::to_string(settings[i].value);
        tag += ",";
    }
    return tag;
}

std::string lower_ascii(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char c = (unsigned char)value[i];
        value[i] = (char)std::tolower(c);
    }
    return value;
}

bool callable_value(value_t *value) {
    return value && (value->type == T_func || value->type == T_builtin ||
                     value->type == T_method);
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
    std::string authority;
    int port = 80;
    std::string path = "/";
    bool tls = false;
};

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
    parts.path = slash == std::string::npos ? "/" : rest.substr(slash);
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

    std::string host = authority;
    std::string port_text;
    if (authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) {
            error = "URL has an invalid IPv6 host";
            return false;
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                error = "URL has an invalid host";
                return false;
            }
            port_text = authority.substr(close + 2);
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
        }
    }
    if (host.empty()) {
        error = "URL is missing a host";
        return false;
    }
    if (!port_text.empty()) {
        char *end = NULL;
        long port = std::strtol(port_text.c_str(), &end, 10);
        if (!end || *end != '\0' || port <= 0 || port > 65535) {
            error = "URL has an invalid port";
            return false;
        }
        parts.port = (int)port;
    }
    parts.host = host;
    parts.authority = host;
    bool default_port = (parts.tls && parts.port == 443) ||
                        (!parts.tls && parts.port == 80);
    if (!default_port)
        parts.authority += ":" + std::to_string(parts.port);
    return true;
}

struct header_pair {
    std::string name;
    std::string value;
};

bool header_is_connection_specific(const std::string &name) {
    std::string lower = lower_ascii(name);
    return lower == "connection" || lower == "host" ||
           lower == "keep-alive" || lower == "proxy-connection" ||
           lower == "transfer-encoding" || lower == "upgrade";
}

struct out_body {
    std::string data;
    size_t offset = 0;
};

struct generator_state {
    generator_ptr generator;
    std::string chunk;
    size_t offset = 0;
};

struct h2_connection {
    value_t *stream = NULL;
    nghttp2_session *session = NULL;
    bool async = false;
    long long timeout_ms = 0;

    /* client response state */
    int32_t stream_id = -1;
    bool done = false;
    int status = 0;
    std::vector<header_pair> response_headers;
    std::string response_body;
    std::string failure;
    bool goaway = false;
    bool peer_settings_received = false;
    std::size_t max_body_bytes = DEFAULT_MAX_BODY_BYTES;
    std::size_t received_body_bytes = 0;
    value_t *on_body = NULL;
    out_body request_body;
    generator_ptr request_generator;
    std::string generator_chunk;
    std::size_t generator_offset = 0;

    /* client multiplexed request state */
    bool multi_mode = false;
    struct multi_response {
        bool done = false;
        int status = 0;
        std::vector<header_pair> headers;
        std::string body;
        std::string failure;
        value_t *on_body = NULL;
        std::size_t received_bytes = 0;
        std::size_t max_body_bytes = DEFAULT_MAX_BODY_BYTES;
    };
    std::map<int32_t, multi_response> multi_streams;
    std::vector<std::shared_ptr<out_body>> multi_request_bodies;
    std::map<int32_t, std::shared_ptr<generator_state>>
        multi_generators;

    /* client pool state */
    std::string key;
    bool in_use = false;
    std::chrono::steady_clock::time_point last_used;

    /* shared async connection state */
    bool shared = false;
    bool reader_running = false;
    bool closed = false;
    bool stream_closed = false;
    int active_streams = 0;
    task_ptr reader_task;
    bool session_busy = false;
    int users = 0;
    std::vector<task_ptr> session_waiters;
    std::map<int32_t, task_ptr> shared_tasks;
    std::map<int32_t, std::shared_ptr<out_body>> shared_body_owners;
    std::map<int32_t, std::shared_ptr<generator_state>>
        shared_generator_owners;

    /* server request state */
    struct server_request {
        std::string method;
        std::string path;
        std::string scheme;
        std::string authority;
        std::vector<header_pair> headers;
        std::string body;
        std::size_t received_bytes = 0;
        bool complete = false;
        bool responded = false;
    };
    std::map<int32_t, server_request> requests;
    std::vector<int32_t> ready;
    value_t *on_request_body = NULL;
    std::map<int32_t, out_body> response_bodies;
    std::map<int32_t, generator_state> response_streams;
};

void h2_release_connection(h2_connection *connection) {
    if (!connection)
        return;
    if (connection->session) {
        nghttp2_session_del(connection->session);
        connection->session = NULL;
    }
    if (connection->stream) {
        if (!connection->stream_closed)
            close_socket(connection->stream);
        freeval(connection->stream);
        connection->stream = NULL;
    }
}

struct h2_connection_deleter {
    void operator()(h2_connection *connection) const {
        h2_release_connection(connection);
        delete connection;
    }
};

typedef std::unique_ptr<h2_connection, h2_connection_deleter>
    h2_connection_ptr;

std::vector<h2_connection_ptr> &h2_pool() {
    static std::vector<h2_connection_ptr> pool;
    return pool;
}

std::map<std::string, task_ptr> &h2_connecting() {
    static std::map<std::string, task_ptr> connecting;
    return connecting;
}

void h2_pool_discard(h2_connection *connection) {
    if (!connection)
        return;
    if (connection->shared &&
        (connection->reader_running || connection->users > 0)) {
        connection->closed = true;
        if (connection->stream && !connection->stream_closed) {
            close_socket(connection->stream);
            connection->stream_closed = true;
        }
        return;
    }
    std::vector<h2_connection_ptr> &pool = h2_pool();
    for (size_t i = 0; i < pool.size(); ++i) {
        if (pool[i].get() != connection)
            continue;
        pool.erase(pool.begin() + (std::ptrdiff_t)i);
        return;
    }
}

void h2_pool_expire() {
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    std::vector<h2_connection_ptr> &pool = h2_pool();
    for (size_t i = 0; i < pool.size();) {
        if (pool[i]->shared) {
            bool idle = pool[i]->active_streams == 0 &&
                        !pool[i]->reader_running && pool[i]->users == 0;
            bool stale = pool[i]->closed || pool[i]->goaway ||
                         now - pool[i]->last_used >
                             std::chrono::seconds(30);
            if (idle && stale) {
                pool.erase(pool.begin() + (std::ptrdiff_t)i);
                continue;
            }
            ++i;
            continue;
        }
        if (!pool[i]->in_use && !pool[i]->goaway &&
            now - pool[i]->last_used > std::chrono::seconds(30)) {
            pool.erase(pool.begin() + (std::ptrdiff_t)i);
            continue;
        }
        ++i;
    }
}

h2_connection *h2_pool_acquire(const std::string &key) {
    h2_pool_expire();
    std::vector<h2_connection_ptr> &pool = h2_pool();
    for (size_t i = 0; i < pool.size(); ++i) {
        if (!pool[i]->shared && !pool[i]->in_use && !pool[i]->goaway &&
            pool[i]->key == key) {
            pool[i]->in_use = true;
            return pool[i].get();
        }
    }
    return NULL;
}

h2_connection *h2_pool_acquire_shared(const std::string &key) {
    h2_pool_expire();
    std::vector<h2_connection_ptr> &pool = h2_pool();
    for (size_t i = 0; i < pool.size(); ++i) {
        if (pool[i]->shared && pool[i]->key == key &&
            !pool[i]->closed && !pool[i]->goaway &&
            pool[i]->active_streams < 100)
            return pool[i].get();
    }
    return NULL;
}

h2_connection *h2_pool_add(const std::string &key,
                           h2_connection *connection) {
    connection->key = key;
    connection->in_use = true;
    connection->last_used = std::chrono::steady_clock::now();
    h2_pool().push_back(h2_connection_ptr(connection));
    return connection;
}

h2_connection *h2_pool_add_shared(const std::string &key,
                                  h2_connection *connection) {
    connection->key = key;
    connection->shared = true;
    connection->in_use = false;
    connection->last_used = std::chrono::steady_clock::now();
    h2_pool().push_back(h2_connection_ptr(connection));
    return connection;
}

void h2_pool_release(h2_connection *connection) {
    if (!connection)
        return;
    connection->in_use = false;
    connection->last_used = std::chrono::steady_clock::now();
}

void h2_pool_shutdown() {
    h2_pool().clear();
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

bool h2_write(h2_connection *connection, const std::string &data) {
    if (data.empty())
        return true;
    if (!connection->async) {
        std::vector<value_t *> args;
        args.push_back(dupval(connection->stream));
        args.push_back(newbytesval(data));
        args.push_back(newintval(connection->timeout_ms));
        value_t *written = call_io(B_io_write_all, args);
        if (!written)
            return false;
        freeval(written);
        return true;
    }
    std::vector<value_t *> args;
    args.push_back(dupval(connection->stream));
    args.push_back(newbytesval(data));
    args.push_back(newintval(connection->timeout_ms));
    value_t *task = call_io(B_io_write_async, args);
    value_t *written = await_task_value(task);
    if (!written)
        return false;
    freeval(written);
    return true;
}

value_t *h2_read(h2_connection *connection) {
    std::vector<value_t *> args;
    args.push_back(dupval(connection->stream));
    args.push_back(newintval(8192));
    args.push_back(newintval(connection->timeout_ms));
    if (!connection->async)
        return call_io(B_io_read, args);
    return await_task_value(call_io(B_io_read_async, args));
}

bool h2_session_lock(h2_connection *connection);
void h2_session_unlock(h2_connection *connection);

bool h2_session_lock(h2_connection *connection) {
    if (!connection->session_busy) {
        connection->session_busy = true;
        if (connection->closed) {
            h2_session_unlock(connection);
            return false;
        }
        return true;
    }
    task_ptr waiter = task_create();
    connection->session_waiters.push_back(waiter);
    value_t *result = fiber_await_task(waiter);
    if (!result) {
        if (task_get_state(waiter) == TASK_STATE_DONE)
            h2_session_unlock(connection);
        return false;
    }
    freeval(result);
    if (connection->closed) {
        h2_session_unlock(connection);
        return false;
    }
    return true;
}

void h2_session_unlock(h2_connection *connection) {
    while (!connection->session_waiters.empty()) {
        task_ptr waiter = connection->session_waiters.front();
        connection->session_waiters.erase(
            connection->session_waiters.begin());
        if (task_get_state(waiter) == TASK_STATE_PENDING) {
            task_complete(waiter, newboolval(true));
            return;
        }
    }
    connection->session_busy = false;
}

void h2_fail_shared_streams(h2_connection *connection,
                            const std::string &message) {
    for (std::map<int32_t, task_ptr>::iterator it =
             connection->shared_tasks.begin();
         it != connection->shared_tasks.end(); ++it) {
        if (task_get_state(it->second) == TASK_STATE_PENDING)
            task_fail(it->second, message);
    }
    connection->shared_tasks.clear();
    connection->multi_streams.clear();
    connection->shared_body_owners.clear();
    connection->shared_generator_owners.clear();
    connection->active_streams = 0;
}

void h2_shared_reader(h2_connection *connection) {
    for (;;) {
        if (!h2_session_lock(connection))
            break;
        bool stop = connection->closed ||
                    connection->active_streams == 0;
        if (stop)
            connection->reader_running = false;
        h2_session_unlock(connection);
        if (stop)
            break;
        value_t *chunk = h2_read(connection);
        if (!chunk) {
            h2_fail_shared_streams(connection,
                                   "HTTP/2 connection read failed");
            connection->closed = true;
            break;
        }
        if (chunk->type == T_null) {
            freeval(chunk);
            h2_fail_shared_streams(connection,
                                   "HTTP/2 connection closed");
            connection->closed = true;
            break;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            h2_fail_shared_streams(connection,
                                   "HTTP/2 read returned non-bytes");
            connection->closed = true;
            break;
        }
        std::string data = ((bytes_value_t *)chunk)->value;
        freeval(chunk);

        if (!h2_session_lock(connection))
            break;
        ssize_t consumed = nghttp2_session_mem_recv(
            connection->session, (const uint8_t *)data.data(),
            data.size());
        int rv = consumed >= 0 ? nghttp2_session_send(connection->session)
                               : -1;
        h2_session_unlock(connection);
        if (consumed < 0) {
            h2_fail_shared_streams(connection,
                                   "HTTP/2 receive failed");
            connection->closed = true;
            break;
        }
        if (rv != 0) {
            h2_fail_shared_streams(connection, "HTTP/2 send failed");
            connection->closed = true;
            break;
        }
    }
    if (connection->closed)
        h2_fail_shared_streams(connection, "HTTP/2 connection closed");
    connection->reader_running = false;
    connection->reader_task.reset();
    h2_pool_expire();
}

ssize_t send_callback(nghttp2_session *, const uint8_t *data, size_t length,
                      int, void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (!h2_write(connection, std::string((const char *)data, length)))
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    return (ssize_t)length;
}

ssize_t body_read_callback(nghttp2_session *, int32_t, uint8_t *buf,
                           size_t length, uint32_t *data_flags,
                           nghttp2_data_source *source, void *) {
    out_body *body = (out_body *)source->ptr;
    size_t remaining = body->data.size() - body->offset;
    size_t take = std::min(remaining, length);
    if (take > 0) {
        memcpy(buf, body->data.data() + body->offset, take);
        body->offset += take;
    }
    if (body->offset >= body->data.size())
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)take;
}

ssize_t generator_read_callback(nghttp2_session *, int32_t, uint8_t *buf,
                                size_t length, uint32_t *data_flags,
                                nghttp2_data_source *, void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (!connection->request_generator) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    for (;;) {
        if (connection->generator_offset <
            connection->generator_chunk.size()) {
            size_t remaining = connection->generator_chunk.size() -
                               connection->generator_offset;
            size_t take = std::min(remaining, length);
            memcpy(buf, connection->generator_chunk.data() +
                            connection->generator_offset, take);
            connection->generator_offset += take;
            return (ssize_t)take;
        }
        value_t *value = NULL;
        bool done = false;
        if (!generator_next_value(connection->request_generator, NULL, value,
                                  done))
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        if (done) {
            freeval(value);
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        connection->generator_chunk.clear();
        connection->generator_offset = 0;
        if (value) {
            if (value->type == T_string)
                connection->generator_chunk =
                    ((string_value_t *)value)->value;
            else if (value->type == T_bytes)
                connection->generator_chunk =
                    ((bytes_value_t *)value)->value;
            else {
                freeval(value);
                yyerror("std.http2: body_stream must yield string or bytes");
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            freeval(value);
        }
    }
}

ssize_t generator_state_read_callback(nghttp2_session *, int32_t,
                                       uint8_t *buf, size_t length,
                                       uint32_t *data_flags,
                                       nghttp2_data_source *source, void *) {
    generator_state *state = (generator_state *)source->ptr;
    if (!state || !state->generator) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    for (;;) {
        if (state->offset < state->chunk.size()) {
            size_t remaining = state->chunk.size() - state->offset;
            size_t take = std::min(remaining, length);
            memcpy(buf, state->chunk.data() + state->offset, take);
            state->offset += take;
            return (ssize_t)take;
        }
        value_t *value = NULL;
        bool done = false;
        if (!generator_next_value(state->generator, NULL, value, done))
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        if (done) {
            freeval(value);
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        state->chunk.clear();
        state->offset = 0;
        if (value) {
            if (value->type == T_string)
                state->chunk = ((string_value_t *)value)->value;
            else if (value->type == T_bytes)
                state->chunk = ((bytes_value_t *)value)->value;
            else {
                freeval(value);
                yyerror("std.http2: body_stream must yield string or bytes");
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            freeval(value);
        }
    }
}

value_t *make_multi_response(h2_connection::multi_response &state);

int client_on_data(nghttp2_session *, uint8_t, int32_t stream_id,
                   const uint8_t *data, size_t len, void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (connection->multi_mode) {
        std::map<int32_t, h2_connection::multi_response>::iterator it =
            connection->multi_streams.find(stream_id);
        if (it == connection->multi_streams.end())
            return 0;
        if (it->second.received_bytes + len >
            it->second.max_body_bytes) {
            it->second.failure = "response body exceeds limit";
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        it->second.received_bytes += len;
        if (it->second.on_body) {
            value_t *chunk =
                newbytesval(std::string((const char *)data, len));
            value_t *result = call_value_args(
                it->second.on_body, std::vector<value_t *>{chunk});
            if (!result) {
                it->second.failure = "on_body callback failed";
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            freeval(result);
            return 0;
        }
        it->second.body.append((const char *)data, len);
        return 0;
    }
    if (stream_id != connection->stream_id)
        return 0;
    if (connection->received_body_bytes + len >
        connection->max_body_bytes) {
        connection->failure = "response body exceeds limit";
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    connection->received_body_bytes += len;
    if (connection->on_body) {
        value_t *chunk = newbytesval(std::string((const char *)data, len));
        value_t *result = call_value_args(
            connection->on_body, std::vector<value_t *>{chunk});
        if (!result) {
            connection->failure = "on_body callback failed";
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        freeval(result);
        return 0;
    }
    connection->response_body.append((const char *)data, len);
    return 0;
}

int client_on_stream_close(nghttp2_session *, int32_t stream_id,
                           uint32_t error_code, void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (connection->multi_mode) {
        std::map<int32_t, h2_connection::multi_response>::iterator it =
            connection->multi_streams.find(stream_id);
        if (it == connection->multi_streams.end())
            return 0;
        it->second.done = true;
        if (error_code != NGHTTP2_NO_ERROR && it->second.failure.empty())
            it->second.failure = "HTTP/2 stream closed with error " +
                                 std::to_string(error_code);
        if (connection->shared) {
            std::map<int32_t, task_ptr>::iterator task_it =
                connection->shared_tasks.find(stream_id);
            if (task_it != connection->shared_tasks.end()) {
                task_ptr pending = task_it->second;
                connection->shared_tasks.erase(task_it);
                if (task_get_state(pending) == TASK_STATE_PENDING) {
                    if (!it->second.failure.empty())
                        task_fail(pending, it->second.failure);
                    else
                        task_complete(pending,
                                      make_multi_response(it->second));
                }
            }
            connection->shared_body_owners.erase(stream_id);
            connection->shared_generator_owners.erase(stream_id);
            if (connection->active_streams > 0)
                --connection->active_streams;
            connection->last_used = std::chrono::steady_clock::now();
        }
        return 0;
    }
    if (stream_id != connection->stream_id)
        return 0;
    connection->done = true;
    if (error_code != NGHTTP2_NO_ERROR && connection->failure.empty())
        connection->failure = "HTTP/2 stream closed with error " +
                              std::to_string(error_code);
    return 0;
}

int client_on_frame_recv(nghttp2_session *, const nghttp2_frame *frame,
                         void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (frame->hd.type == NGHTTP2_GOAWAY)
        connection->goaway = true;
    if (frame->hd.type == NGHTTP2_SETTINGS &&
        !(frame->hd.flags & NGHTTP2_FLAG_ACK))
        connection->peer_settings_received = true;
    return 0;
}

int client_on_header2(nghttp2_session *, const nghttp2_frame *frame,
                      const uint8_t *name, size_t namelen,
                      const uint8_t *value, size_t valuelen, uint8_t,
                      void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS)
        return 0;
    std::string key((const char *)name, namelen);
    std::string text((const char *)value, valuelen);
    if (connection->multi_mode) {
        std::map<int32_t, h2_connection::multi_response>::iterator it =
            connection->multi_streams.find(frame->hd.stream_id);
        if (it == connection->multi_streams.end())
            return 0;
        if (key == ":status") {
            it->second.status = std::atoi(text.c_str());
            return 0;
        }
        if (!key.empty() && key[0] == ':')
            return 0;
        header_pair pair;
        pair.name = lower_ascii(key);
        pair.value = text;
        it->second.headers.push_back(pair);
        return 0;
    }
    if (frame->hd.stream_id != connection->stream_id)
        return 0;
    if (key == ":status") {
        connection->status = std::atoi(text.c_str());
        return 0;
    }
    if (!key.empty() && key[0] == ':')
        return 0;
    header_pair pair;
    pair.name = lower_ascii(key);
    pair.value = text;
    connection->response_headers.push_back(pair);
    return 0;
}

int server_on_begin_headers(nghttp2_session *, const nghttp2_frame *frame,
                            void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
    connection->requests[frame->hd.stream_id];
    return 0;
}

int server_on_header(nghttp2_session *, const nghttp2_frame *frame,
                     const uint8_t *name, size_t namelen,
                     const uint8_t *value, size_t valuelen, uint8_t,
                     void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS)
        return 0;
    h2_connection::server_request &request =
        connection->requests[frame->hd.stream_id];
    std::string key((const char *)name, namelen);
    std::string text((const char *)value, valuelen);
    if (key == ":method")
        request.method = text;
    else if (key == ":path")
        request.path = text;
    else if (key == ":scheme")
        request.scheme = text;
    else if (key == ":authority")
        request.authority = text;
    else if (!key.empty() && key[0] == ':')
        ; /* ignore unknown pseudo headers */
    else {
        header_pair pair;
        pair.name = lower_ascii(key);
        pair.value = text;
        request.headers.push_back(pair);
    }
    return 0;
}

int server_on_data(nghttp2_session *, uint8_t, int32_t stream_id,
                   const uint8_t *data, size_t len, void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    h2_connection::server_request &request = connection->requests[stream_id];
    if (request.received_bytes + len > connection->max_body_bytes) {
        connection->failure = "request body exceeds limit";
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    request.received_bytes += len;
    if (connection->on_request_body) {
        value_t *chunk =
            newbytesval(std::string((const char *)data, len));
        value_t *result = call_value_args(
            connection->on_request_body, std::vector<value_t *>{chunk});
        if (!result) {
            connection->failure = "on_request_body callback failed";
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        freeval(result);
        return 0;
    }
    request.body.append((const char *)data, len);
    return 0;
}

int server_on_frame_recv(nghttp2_session *, const nghttp2_frame *frame,
                         void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS &&
        frame->hd.type != NGHTTP2_DATA)
        return 0;
    if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        return 0;
    int32_t stream_id = frame->hd.stream_id;
    h2_connection::server_request &request = connection->requests[stream_id];
    if (request.complete || request.responded)
        return 0;
    request.complete = true;
    connection->ready.push_back(stream_id);
    return 0;
}

int server_on_stream_close(nghttp2_session *, int32_t stream_id,
                           uint32_t, void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    connection->requests.erase(stream_id);
    connection->response_bodies.erase(stream_id);
    connection->response_streams.erase(stream_id);
    connection->multi_generators.erase(stream_id);
    return 0;
}

void setup_callbacks(nghttp2_session_callbacks *callbacks, bool client) {
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    if (client) {
        nghttp2_session_callbacks_set_on_header_callback(
            callbacks, client_on_header2);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            callbacks, client_on_data);
        nghttp2_session_callbacks_set_on_stream_close_callback(
            callbacks, client_on_stream_close);
        nghttp2_session_callbacks_set_on_frame_recv_callback(
            callbacks, client_on_frame_recv);
    } else {
        nghttp2_session_callbacks_set_on_begin_headers_callback(
            callbacks, server_on_begin_headers);
        nghttp2_session_callbacks_set_on_header_callback(
            callbacks, server_on_header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            callbacks, server_on_data);
        nghttp2_session_callbacks_set_on_frame_recv_callback(
            callbacks, server_on_frame_recv);
        nghttp2_session_callbacks_set_on_stream_close_callback(
            callbacks, server_on_stream_close);
    }
}

bool build_nv(const std::vector<header_pair> &headers,
              std::vector<nghttp2_nv> &nva) {
    nva.clear();
    nva.reserve(headers.size());
    for (size_t i = 0; i < headers.size(); ++i) {
        nghttp2_nv nv;
        nv.name = (uint8_t *)headers[i].name.data();
        nv.value = (uint8_t *)headers[i].value.data();
        nv.namelen = headers[i].name.size();
        nv.valuelen = headers[i].value.size();
        nv.flags = NGHTTP2_NV_FLAG_NONE;
        nva.push_back(nv);
    }
    return true;
}

value_t *headers_dict(const std::vector<header_pair> &headers) {
    value_t *result = (value_t *)newdictval();
    for (size_t i = 0; i < headers.size(); ++i)
        dict_set(result, headers[i].name,
                 newstrval(headers[i].value.c_str()));
    return result;
}

value_t *make_response(const h2_connection &connection) {
    value_t *response = (value_t *)newdictval();
    dict_set(response, "status", newintval(connection.status));
    dict_set(response, "status_text",
             newstrval(reason_phrase(connection.status)));
    dict_set(response, "headers", headers_dict(connection.response_headers));
    dict_set(response, "body", newbytesval(connection.response_body));
    dict_set(response, "http_major", newintval(2));
    dict_set(response, "http_minor", newintval(0));
    dict_set(response, "protocol", newstrval("h2"));
    return response;
}

bool collect_request_headers(value_t *headers,
                             std::vector<header_pair> &out) {
    if (!headers || headers->type == T_null)
        return true;
    if (headers->type != T_dict) {
        yyerror("std.http2: headers must be a dict");
        return false;
    }
    dict_value_t *dict = (dict_value_t *)headers;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        value_t *key = dict->payload->items[i].first;
        value_t *value = dict->payload->items[i].second;
        if (key->type != T_string || value->type != T_string) {
            yyerror("std.http2: header names and values must be strings");
            return false;
        }
        std::string name = ((string_value_t *)key)->value;
        if (name.empty() || name[0] == ':') {
            yyerror("std.http2: header names must not be pseudo headers");
            return false;
        }
        if (header_is_connection_specific(name))
            continue;
        header_pair pair;
        pair.name = lower_ascii(name);
        pair.value = ((string_value_t *)value)->value;
        out.push_back(pair);
    }
    return true;
}

std::string h2_connection_key(const url_parts &url, bool verify,
                              const std::string &ca_file) {
    std::string key = url.tls ? "h2s://" : "h2://";
    key += url.host;
    key += ":";
    key += std::to_string(url.port);
    if (url.tls) {
        key += verify ? "|verify" : "|noverify";
        if (!ca_file.empty()) {
            key += "|";
            key += ca_file;
        }
    }
    return key;
}

bool open_client_connection(
    h2_connection *connection, const url_parts &url, bool async,
    bool tls_verify, const std::string &tls_ca_file, long long timeout_ms,
    const std::vector<nghttp2_settings_entry> &settings) {
    connection->async = async;
    connection->timeout_ms = timeout_ms;
    connection->goaway = false;

    int connect_type =
        async ? B_net_tcp_connect_async : B_net_tcp_connect;
    std::vector<value_t *> connect_args;
    connect_args.push_back(newstrval(url.host.c_str()));
    connect_args.push_back(newintval(url.port));
    connect_args.push_back(newintval(timeout_ms));
    value_t *client = call_net(connect_type, connect_args);
    if (!client)
        return false;
    if (async) {
        client = await_task_value(client);
        if (!client)
            return false;
    }

    value_t *stream = client;
    if (url.tls) {
#ifdef CALC_ENABLE_TLS
        value_t *tls_options = (value_t *)newdictval();
        dict_set(tls_options, "verify", newboolval(tls_verify));
        if (!tls_ca_file.empty())
            dict_set(tls_options, "ca_file", newstrval(tls_ca_file.c_str()));
        value_t *alpn = (value_t *)newlistval();
        ((list_value_t *)alpn)->payload->items.push_back(newstrval("h2"));
        dict_set(tls_options, "alpn", alpn);
        std::vector<value_t *> tls_args;
        tls_args.push_back(dupval(client));
        tls_args.push_back(newstrval(url.host.c_str()));
        tls_args.push_back(tls_options);
        stream = call_tls(B_tls_client, tls_args);
        if (!stream) {
            close_socket(client);
            freeval(client);
            return false;
        }
        freeval(client);

        std::vector<value_t *> alpn_args;
        alpn_args.push_back(dupval(stream));
        value_t *selected = call_tls(B_tls_alpn, alpn_args);
        if (!selected) {
            close_socket(stream);
            freeval(stream);
            return false;
        }
        std::string protocol;
        if (selected->type == T_string)
            protocol = ((string_value_t *)selected)->value;
        freeval(selected);
        if (protocol != "h2") {
            yyerror("std.http2: server did not negotiate h2 (got '%s')",
                    protocol.c_str());
            close_socket(stream);
            freeval(stream);
            return false;
        }
#else
        yyerror("std.http2: https is not supported (TLS disabled)");
        close_socket(client);
        freeval(client);
        return false;
#endif
    }

    nghttp2_session_callbacks *callbacks = NULL;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        yyerror("std.http2: cannot create nghttp2 callbacks");
        close_socket(stream);
        freeval(stream);
        return false;
    }
    setup_callbacks(callbacks, true);
    int rv = nghttp2_session_client_new(&connection->session, callbacks,
                                        connection);
    nghttp2_session_callbacks_del(callbacks);
    if (rv != 0) {
        yyerror("std.http2: cannot create nghttp2 session");
        close_socket(stream);
        freeval(stream);
        return false;
    }
    nghttp2_submit_settings(
        connection->session, NGHTTP2_FLAG_NONE,
        settings.empty() ? NULL : settings.data(), settings.size());
    connection->stream = stream;
    return true;
}

value_t *exchange_once(h2_connection *connection,
                       const std::string &method, const url_parts &url,
                       value_t *headers, const std::string &body,
                       std::size_t max_body_bytes, value_t *on_body,
                       const generator_ptr &body_generator) {
    connection->stream_id = -1;
    connection->done = false;
    connection->status = 0;
    connection->failure.clear();
    connection->response_headers.clear();
    connection->response_body.clear();
    connection->max_body_bytes = max_body_bytes;
    connection->received_body_bytes = 0;
    connection->on_body = on_body;
    connection->request_generator = body_generator;
    connection->generator_chunk.clear();
    connection->generator_offset = 0;
    connection->request_body.data = body;
    connection->request_body.offset = 0;

    std::vector<header_pair> request_headers;
    header_pair method_header = {":method", method};
    header_pair scheme_header = {":scheme", url.tls ? "https" : "http"};
    header_pair authority_header = {":authority", url.authority};
    header_pair path_header = {":path", url.path};
    request_headers.push_back(method_header);
    request_headers.push_back(scheme_header);
    request_headers.push_back(authority_header);
    request_headers.push_back(path_header);
    if (!collect_request_headers(headers, request_headers))
        return NULL;

    std::vector<nghttp2_nv> nva;
    build_nv(request_headers, nva);

    nghttp2_data_provider provider;
    nghttp2_data_provider *provider_ptr = NULL;
    if (body_generator) {
        provider.source.ptr = NULL;
        provider.read_callback = generator_read_callback;
        provider_ptr = &provider;
    } else if (!body.empty()) {
        provider.source.ptr = &connection->request_body;
        provider.read_callback = body_read_callback;
        provider_ptr = &provider;
    }

    connection->stream_id = nghttp2_submit_request(
        connection->session, NULL, nva.data(), nva.size(), provider_ptr,
        NULL);
    if (connection->stream_id < 0) {
        yyerror("std.http2: cannot submit request");
        return NULL;
    }

    int rv = 0;
    while (!connection->done) {
        rv = nghttp2_session_send(connection->session);
        if (rv != 0) {
            connection->failure = "nghttp2 send failed";
            break;
        }
        if (connection->goaway && !connection->done) {
            connection->failure = "server sent GOAWAY";
            break;
        }
        value_t *chunk = h2_read(connection);
        if (!chunk)
            return NULL;
        if (chunk->type == T_null) {
            freeval(chunk);
            connection->failure = "connection closed before response";
            break;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            connection->failure = "read returned non-bytes";
            break;
        }
        const std::string &data = ((bytes_value_t *)chunk)->value;
        ssize_t consumed = nghttp2_session_mem_recv(
            connection->session, (const uint8_t *)data.data(), data.size());
        freeval(chunk);
        if (consumed < 0) {
            connection->failure = "nghttp2 receive failed";
            break;
        }
    }

    if (nghttp2_session_send(connection->session) != 0 &&
        connection->failure.empty())
        connection->failure = "nghttp2 send failed";

    connection->on_body = NULL;
    connection->request_generator.reset();
    if (!connection->failure.empty()) {
        yyerror("std.http2: %s", connection->failure.c_str());
        return NULL;
    }
    return make_response(*connection);
}

struct shared_user_guard {
    h2_connection *connection;
    explicit shared_user_guard(h2_connection *value)
        : connection(value) {
        if (connection)
            ++connection->users;
    }
    ~shared_user_guard() {
        if (connection && --connection->users == 0)
            h2_pool_expire();
    }
};

value_t *exchange_shared(const std::string &method, const url_parts &url,
                         value_t *headers, const std::string &body,
                         std::size_t max_body_bytes, value_t *on_body,
                         const generator_ptr &body_generator,
                         bool tls_verify,
                         const std::string &tls_ca_file,
                         long long timeout_ms,
                         const std::vector<nghttp2_settings_entry> &settings,
                         bool *used_existing) {
    std::vector<header_pair> request_headers;
    header_pair method_header = {":method", method};
    header_pair scheme_header = {":scheme", url.tls ? "https" : "http"};
    header_pair authority_header = {":authority", url.authority};
    header_pair path_header = {":path", url.path};
    request_headers.push_back(method_header);
    request_headers.push_back(scheme_header);
    request_headers.push_back(authority_header);
    request_headers.push_back(path_header);
    if (!collect_request_headers(headers, request_headers))
        return NULL;

    std::string key = h2_connection_key(url, tls_verify, tls_ca_file);
    key += h2_settings_tag(settings);
    bool reused = false;
    h2_connection *connection = h2_pool_acquire_shared(key);
    if (connection)
        reused = true;
    if (!connection) {
        std::map<std::string, task_ptr>::iterator gate_it =
            h2_connecting().find(key);
        if (gate_it != h2_connecting().end()) {
            value_t *gate_result = fiber_await_task(gate_it->second);
            freeval(gate_result);
            connection = h2_pool_acquire_shared(key);
            if (connection)
                reused = true;
        }
    }
    if (!connection) {
        task_ptr gate = task_create();
        h2_connecting()[key] = gate;
        connection = new h2_connection();
        bool opened = open_client_connection(connection, url, true,
                                             tls_verify, tls_ca_file,
                                             timeout_ms, settings);
        if (opened) {
            connection->shared = true;
            h2_pool_add_shared(key, connection);
        }
        h2_connecting().erase(key);
        task_complete(gate, newboolval(opened));
        if (!opened) {
            h2_release_connection(connection);
            delete connection;
            return NULL;
        }
    }

    if (used_existing)
        *used_existing = reused;
    shared_user_guard user_guard(connection);
    connection->async = true;
    connection->multi_mode = true;
    connection->max_body_bytes = max_body_bytes;
    connection->timeout_ms = 0;
    connection->last_used = std::chrono::steady_clock::now();

    std::vector<nghttp2_nv> nva;
    build_nv(request_headers, nva);

    std::shared_ptr<out_body> body_owner;
    std::shared_ptr<generator_state> generator_owner;
    nghttp2_data_provider provider;
    nghttp2_data_provider *provider_ptr = NULL;
    if (body_generator) {
        generator_owner = std::make_shared<generator_state>();
        generator_owner->generator = body_generator;
        provider.source.ptr = generator_owner.get();
        provider.read_callback = generator_state_read_callback;
        provider_ptr = &provider;
    } else if (!body.empty()) {
        body_owner = std::make_shared<out_body>();
        body_owner->data = body;
        provider.source.ptr = body_owner.get();
        provider.read_callback = body_read_callback;
        provider_ptr = &provider;
    }

    if (!h2_session_lock(connection))
        return NULL;

    int32_t stream_id = nghttp2_submit_request(
        connection->session, NULL, nva.data(), nva.size(), provider_ptr,
        NULL);
    if (stream_id < 0) {
        h2_session_unlock(connection);
        yyerror("std.http2: cannot submit request");
        return NULL;
    }

    task_ptr stream_task = task_create();
    h2_connection::multi_response &stream_state =
        connection->multi_streams[stream_id];
    stream_state.on_body = on_body;
    stream_state.max_body_bytes = max_body_bytes;
    connection->shared_tasks[stream_id] = stream_task;
    if (body_owner)
        connection->shared_body_owners[stream_id] = body_owner;
    if (generator_owner)
        connection->shared_generator_owners[stream_id] = generator_owner;
    ++connection->active_streams;

    if (!connection->reader_running && !connection->closed) {
        connection->reader_running = true;
        connection->reader_task = fiber_start_async([connection]() {
            h2_shared_reader(connection);
            return (value_t *)NULL;
        });
        if (!connection->reader_task) {
            connection->reader_running = false;
            task_fail(stream_task, "cannot start HTTP/2 reader");
        }
    }

    int rv = nghttp2_session_send(connection->session);
    h2_session_unlock(connection);

    if (rv != 0) {
        connection->shared_tasks.erase(stream_id);
        connection->multi_streams.erase(stream_id);
        connection->shared_body_owners.erase(stream_id);
        connection->shared_generator_owners.erase(stream_id);
        if (connection->active_streams > 0)
            --connection->active_streams;
        h2_pool_discard(connection);
        yyerror("std.http2: nghttp2 send failed");
        return NULL;
    }

    value_t *response = fiber_await_task(stream_task);

    connection->shared_tasks.erase(stream_id);
    connection->multi_streams.erase(stream_id);
    connection->shared_body_owners.erase(stream_id);
    connection->shared_generator_owners.erase(stream_id);
    if (connection->closed || connection->goaway)
        h2_pool_discard(connection);
    return response;
}

value_t *exchange(const std::string &method, const std::string &url_text,
                  value_t *options, value_t *body_override, bool async) {
    url_parts url;
    std::string error;
    if (!parse_url(url_text, url, error)) {
        yyerror("std.http2: %s", error.c_str());
        return NULL;
    }

    value_t *headers = dict_get(options, "headers");
    std::string body;
    value_t *body_option =
        body_override ? body_override : dict_get(options, "body");
    if (!body_value(body_option, body, "std.http2"))
        return NULL;

    value_t *body_stream_value = dict_get(options, "body_stream");
    generator_ptr body_generator;
    if (body_stream_value && body_stream_value->type != T_null) {
        if (!fiber_is_generator(body_stream_value)) {
            yyerror("std.http2: body_stream must be a generator");
            return NULL;
        }
        if (!get_generator_value(body_stream_value, body_generator))
            return NULL;
        if (!body.empty()) {
            yyerror("std.http2: body and body_stream cannot both be set");
            return NULL;
        }
    }

    long long timeout_ms = 0;
    value_t *timeout_value = dict_get(options, "timeout_ms");
    if (timeout_value) {
        if (timeout_value->type != T_int ||
            ((int_value_t *)timeout_value)->value < 0) {
            yyerror("std.http2: timeout_ms must be a non-negative integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)timeout_value)->value;
    }

    bool tls_verify = true;
    std::string tls_ca_file;
    if (url.tls) {
        value_t *tls_options = dict_get(options, "tls");
        if (tls_options && tls_options->type != T_dict) {
            yyerror("std.http2: tls options must be a dict");
            return NULL;
        }
        if (tls_options) {
            tls_verify = option_bool(tls_options, "verify", true);
            tls_ca_file = option_string(tls_options, "ca_file");
        }
    }

    value_t *on_body = dict_get(options, "on_body");
    if (on_body && on_body->type == T_null)
        on_body = NULL;
    if (on_body && !callable_value(on_body)) {
        yyerror("std.http2: on_body must be a function");
        return NULL;
    }

    std::size_t max_body_bytes = DEFAULT_MAX_BODY_BYTES;
    value_t *max_body = dict_get(options, "max_body_bytes");
    if (max_body) {
        if (max_body->type != T_int ||
            ((int_value_t *)max_body)->value <= 0) {
            yyerror("std.http2: max_body_bytes must be a positive integer");
            return NULL;
        }
        max_body_bytes =
            (std::size_t)((int_value_t *)max_body)->value;
    }

    std::vector<nghttp2_settings_entry> settings;
    if (!build_h2_settings(dict_get(options, "settings"), false, settings))
        return NULL;

    std::string key = h2_connection_key(url, tls_verify, tls_ca_file);
    key += h2_settings_tag(settings);
    bool idempotent = method == "GET" || method == "HEAD" ||
                      method == "OPTIONS" || method == "TRACE";
    bool retryable = idempotent && !on_body && !body_generator;

    if (async) {
        if (!idempotent || on_body || body_generator)
            return exchange_shared(method, url, headers, body,
                                   max_body_bytes, on_body, body_generator,
                                   tls_verify, tls_ca_file, timeout_ms,
                                   settings, NULL);
        bool reused = false;
        eval_capture_snapshot snapshot;
        eval_capture_begin(snapshot);
        value_t *response =
            exchange_shared(method, url, headers, body, max_body_bytes,
                            on_body, body_generator, tls_verify,
                            tls_ca_file, timeout_ms, settings, &reused);
        eval_capture_result captured = eval_capture_end(snapshot);
        if (response) {
            freeval(captured.error);
            return response;
        }
        if (!reused) {
            if (captured.failed)
                eval_raise(captured.error);
            else
                yyerror("std.http2: request failed");
            return NULL;
        }
        freeval(captured.error);
        return exchange_shared(method, url, headers, body, max_body_bytes,
                               on_body, body_generator, tls_verify,
                               tls_ca_file, timeout_ms, settings, NULL);
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        h2_connection *connection =
            attempt == 0 ? h2_pool_acquire(key) : NULL;
        bool reused = connection != NULL;
        if (!connection) {
            connection = new h2_connection();
            if (!open_client_connection(connection, url, async, tls_verify,
                                        tls_ca_file, timeout_ms, settings)) {
                h2_release_connection(connection);
                delete connection;
                return NULL;
            }
            h2_pool_add(key, connection);
        } else {
            connection->async = async;
            connection->timeout_ms = timeout_ms;
        }

        bool capture_errors = attempt == 0 && reused;
        eval_capture_snapshot snapshot;
        if (capture_errors)
            eval_capture_begin(snapshot);

        value_t *response =
            exchange_once(connection, method, url, headers, body,
                          max_body_bytes, on_body, body_generator);

        eval_capture_result captured;
        if (capture_errors)
            captured = eval_capture_end(snapshot);

        if (!response) {
            freeval(captured.error);
            h2_pool_discard(connection);
            if (capture_errors && retryable && !g_interrupted)
                continue;
            return NULL;
        }

        freeval(captured.error);
        if (connection->goaway)
            h2_pool_discard(connection);
        else
            h2_pool_release(connection);
        return response;
    }
    return NULL;
}

value_t *make_multi_response(h2_connection::multi_response &state) {
    value_t *response = (value_t *)newdictval();
    dict_set(response, "status", newintval(state.status));
    dict_set(response, "status_text",
             newstrval(reason_phrase(state.status)));
    dict_set(response, "headers", headers_dict(state.headers));
    dict_set(response, "body", newbytesval(state.body));
    dict_set(response, "http_major", newintval(2));
    dict_set(response, "http_minor", newintval(0));
    dict_set(response, "protocol", newstrval("h2"));
    return response;
}

value_t *exchange_many(value_t *requests, value_t *options, bool async) {
    if (!requests || requests->type != T_list) {
        yyerror("std.http2.request_many expects a list of requests");
        return NULL;
    }
    list_value_t *request_list = (list_value_t *)requests;
    if (request_list->payload->items.empty())
        return (value_t *)newlistval();

    long long timeout_ms = 0;
    value_t *timeout_value = dict_get(options, "timeout_ms");
    if (timeout_value) {
        if (timeout_value->type != T_int ||
            ((int_value_t *)timeout_value)->value < 0) {
            yyerror("std.http2: timeout_ms must be a non-negative integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)timeout_value)->value;
    }

    bool tls_verify = true;
    std::string tls_ca_file;
    value_t *tls_options = dict_get(options, "tls");
    if (tls_options) {
        if (tls_options->type != T_dict) {
            yyerror("std.http2: tls options must be a dict");
            return NULL;
        }
        tls_verify = option_bool(tls_options, "verify", true);
        tls_ca_file = option_string(tls_options, "ca_file");
    }

    std::size_t max_body_bytes = DEFAULT_MAX_BODY_BYTES;
    value_t *max_body = dict_get(options, "max_body_bytes");
    if (max_body) {
        if (max_body->type != T_int ||
            ((int_value_t *)max_body)->value <= 0) {
            yyerror("std.http2: max_body_bytes must be a positive integer");
            return NULL;
        }
        max_body_bytes =
            (std::size_t)((int_value_t *)max_body)->value;
    }

    std::vector<nghttp2_settings_entry> settings;
    if (!build_h2_settings(dict_get(options, "settings"), false, settings))
        return NULL;

    std::vector<std::string> methods;
    std::vector<url_parts> urls;
    std::vector<value_t *> headers;
    std::vector<std::string> bodies;
    std::vector<value_t *> on_bodies;
    std::vector<generator_ptr> body_generators;
    std::string key;
    for (size_t i = 0; i < request_list->payload->items.size(); ++i) {
        value_t *item = request_list->payload->items[i];
        if (!item || item->type != T_dict) {
            yyerror("std.http2.request_many items must be dicts");
            return NULL;
        }
        value_t *url_value = dict_get(item, "url");
        if (!url_value || url_value->type != T_string) {
            yyerror("std.http2.request_many item is missing a URL");
            return NULL;
        }
        url_parts url;
        std::string error;
        if (!parse_url(((string_value_t *)url_value)->value, url, error)) {
            yyerror("std.http2: %s", error.c_str());
            return NULL;
        }
        std::string item_key =
            h2_connection_key(url, tls_verify, tls_ca_file);
        if (i == 0)
            key = item_key;
        else if (item_key != key) {
            yyerror("std.http2.request_many requires all requests to "
                    "share one origin");
            return NULL;
        }

        std::string method = "GET";
        value_t *method_value = dict_get(item, "method");
        if (method_value) {
            if (method_value->type != T_string) {
                yyerror("std.http2.request_many method must be a string");
                return NULL;
            }
            method = ((string_value_t *)method_value)->value;
        }
        value_t *item_headers = dict_get(item, "headers");
        if (item_headers && item_headers->type != T_dict &&
            item_headers->type != T_null) {
            yyerror("std.http2.request_many headers must be a dict");
            return NULL;
        }
        std::string body;
        if (!body_value(dict_get(item, "body"), body,
                        "std.http2.request_many"))
            return NULL;

        value_t *item_on_body = dict_get(item, "on_body");
        if (item_on_body && item_on_body->type == T_null)
            item_on_body = NULL;
        if (item_on_body && !callable_value(item_on_body)) {
            yyerror("std.http2.request_many on_body must be a function");
            return NULL;
        }
        generator_ptr item_generator;
        value_t *item_stream = dict_get(item, "body_stream");
        if (item_stream && item_stream->type != T_null) {
            if (!fiber_is_generator(item_stream) ||
                !get_generator_value(item_stream, item_generator))
                return NULL;
            if (!body.empty()) {
                yyerror("std.http2.request_many: body and body_stream "
                        "cannot both be set");
                return NULL;
            }
        }

        methods.push_back(method);
        urls.push_back(url);
        headers.push_back(item_headers);
        bodies.push_back(body);
        on_bodies.push_back(item_on_body);
        body_generators.push_back(item_generator);
    }

    key += h2_settings_tag(settings);
    h2_connection *connection = h2_pool_acquire(key);
    if (!connection) {
        connection = new h2_connection();
        if (!open_client_connection(connection, urls[0], async, tls_verify,
                                    tls_ca_file, timeout_ms, settings)) {
            h2_release_connection(connection);
            delete connection;
            return NULL;
        }
        h2_pool_add(key, connection);
    } else {
        connection->async = async;
        connection->timeout_ms = timeout_ms;
    }

    if (!connection->peer_settings_received) {
        bool ok = nghttp2_session_send(connection->session) == 0;
        while (ok && !connection->peer_settings_received) {
            value_t *chunk = h2_read(connection);
            if (!chunk) {
                ok = false;
                break;
            }
            if (chunk->type == T_null) {
                freeval(chunk);
                ok = false;
                break;
            }
            if (chunk->type != T_bytes) {
                freeval(chunk);
                ok = false;
                break;
            }
            std::string data = ((bytes_value_t *)chunk)->value;
            freeval(chunk);
            ssize_t consumed = nghttp2_session_mem_recv(
                connection->session, (const uint8_t *)data.data(),
                data.size());
            if (consumed < 0) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            h2_pool_discard(connection);
            yyerror("std.http2: failed to receive peer settings");
            return NULL;
        }
    }

    connection->multi_mode = true;
    connection->multi_streams.clear();
    connection->multi_request_bodies.clear();
    connection->max_body_bytes = max_body_bytes;
    connection->goaway = false;

    std::vector<int32_t> stream_order;
    bool failed = false;
    for (size_t i = 0; i < methods.size(); ++i) {
        std::vector<header_pair> request_headers;
        header_pair method_header = {":method", methods[i]};
        header_pair scheme_header = {":scheme",
                                     urls[i].tls ? "https" : "http"};
        header_pair authority_header = {":authority", urls[i].authority};
        header_pair path_header = {":path", urls[i].path};
        request_headers.push_back(method_header);
        request_headers.push_back(scheme_header);
        request_headers.push_back(authority_header);
        request_headers.push_back(path_header);
        if (!collect_request_headers(headers[i], request_headers)) {
            failed = true;
            break;
        }
        std::vector<nghttp2_nv> nva;
        build_nv(request_headers, nva);

        std::shared_ptr<out_body> body_owner;
        std::shared_ptr<generator_state> generator_owner;
        nghttp2_data_provider provider;
        nghttp2_data_provider *provider_ptr = NULL;
        if (body_generators[i]) {
            generator_owner = std::make_shared<generator_state>();
            generator_owner->generator = body_generators[i];
            provider.source.ptr = generator_owner.get();
            provider.read_callback = generator_state_read_callback;
            provider_ptr = &provider;
        } else if (!bodies[i].empty()) {
            body_owner = std::make_shared<out_body>();
            body_owner->data = bodies[i];
            body_owner->offset = 0;
            connection->multi_request_bodies.push_back(body_owner);
            provider.source.ptr = body_owner.get();
            provider.read_callback = body_read_callback;
            provider_ptr = &provider;
        }
        int32_t stream_id = nghttp2_submit_request(
            connection->session, NULL, nva.data(), nva.size(),
            provider_ptr, NULL);
        if (stream_id < 0) {
            yyerror("std.http2.request_many: cannot submit request");
            failed = true;
            break;
        }
        h2_connection::multi_response &stream_state =
            connection->multi_streams[stream_id];
        stream_state.on_body = on_bodies[i];
        stream_state.max_body_bytes = max_body_bytes;
        if (generator_owner)
            connection->multi_generators[stream_id] = generator_owner;
        stream_order.push_back(stream_id);
    }

    while (!failed) {
        int rv = nghttp2_session_send(connection->session);
        if (rv != 0) {
            connection->failure = "nghttp2 send failed";
            failed = true;
            break;
        }
        bool all_done = true;
        for (size_t i = 0; i < stream_order.size(); ++i) {
            if (!connection->multi_streams[stream_order[i]].done) {
                all_done = false;
                break;
            }
        }
        if (all_done)
            break;
        if (connection->goaway) {
            connection->failure = "server sent GOAWAY";
            failed = true;
            break;
        }
        value_t *chunk = h2_read(connection);
        if (!chunk) {
            failed = true;
            break;
        }
        if (chunk->type == T_null) {
            freeval(chunk);
            connection->failure = "connection closed before response";
            failed = true;
            break;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            connection->failure = "read returned non-bytes";
            failed = true;
            break;
        }
        const std::string &data = ((bytes_value_t *)chunk)->value;
        ssize_t consumed = nghttp2_session_mem_recv(
            connection->session, (const uint8_t *)data.data(), data.size());
        freeval(chunk);
        if (consumed < 0) {
            connection->failure = "nghttp2 receive failed";
            failed = true;
            break;
        }
    }
    if (!failed)
        nghttp2_session_send(connection->session);

    if (!failed) {
        for (size_t i = 0; i < stream_order.size(); ++i) {
            h2_connection::multi_response &state =
                connection->multi_streams[stream_order[i]];
            if (!state.failure.empty()) {
                connection->failure = state.failure;
                failed = true;
                break;
            }
        }
    }

    bool goaway = connection->goaway;
    connection->multi_mode = false;
    connection->multi_request_bodies.clear();
    connection->multi_generators.clear();

    std::string failure = connection->failure;
    if (failed || !failure.empty()) {
        connection->multi_streams.clear();
        h2_pool_discard(connection);
        yyerror("std.http2: %s",
                failure.empty() ? "request failed" : failure.c_str());
        return NULL;
    }

    value_t *results = (value_t *)newlistval();
    list_value_t *result_list = (list_value_t *)results;
    for (size_t i = 0; i < stream_order.size(); ++i) {
        result_list->payload->items.push_back(
            make_multi_response(connection->multi_streams[stream_order[i]]));
    }
    connection->multi_streams.clear();
    if (goaway)
        h2_pool_discard(connection);
    else
        h2_pool_release(connection);
    return results;
}

value_t *http2_request(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 || args[0]->type != T_string ||
        args[1]->type != T_string) {
        yyerror("std.http2.request expects method, URL, and optional options");
        return NULL;
    }
    value_t *options = args.size() == 3 ? args[2] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http2: options must be a dict");
        return NULL;
    }
    return exchange(((string_value_t *)args[0])->value,
                    ((string_value_t *)args[1])->value, options, NULL, false);
}

value_t *http2_get(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_string) {
        yyerror("std.http2.get expects URL and optional options");
        return NULL;
    }
    value_t *options = args.size() == 2 ? args[1] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http2: options must be a dict");
        return NULL;
    }
    return exchange("GET", ((string_value_t *)args[0])->value, options, NULL,
                    false);
}

value_t *http2_post(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 3 || args[0]->type != T_string) {
        yyerror("std.http2.post expects URL, optional body, and options");
        return NULL;
    }
    value_t *body = NULL;
    value_t *options = NULL;
    if (args.size() == 2) {
        if (args[1]->type == T_dict)
            options = args[1];
        else
            body = args[1];
    } else if (args.size() == 3) {
        body = args[1];
        options = args[2];
    }
    if (options && options->type != T_dict) {
        yyerror("std.http2: options must be a dict");
        return NULL;
    }
    return exchange("POST", ((string_value_t *)args[0])->value, options, body,
                    false);
}

value_t *http2_request_async(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 || args[0]->type != T_string ||
        args[1]->type != T_string) {
        yyerror("std.http2.request_async expects method, URL, and optional "
                "options");
        return NULL;
    }
    value_t *options = args.size() == 3 ? args[2] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http2: options must be a dict");
        return NULL;
    }
    std::string method = ((string_value_t *)args[0])->value;
    std::string url = ((string_value_t *)args[1])->value;
    value_t *owned_options = options ? dupval(options) : NULL;
    task_ptr task = fiber_start_async([method, url, owned_options]() {
        std::unique_ptr<value_t, void (*)(value_t *)> holder(
            owned_options, freeval);
        return exchange(method, url, owned_options, NULL, true);
    });
    if (!task) {
        freeval(owned_options);
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *http2_get_async(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_string) {
        yyerror("std.http2.get_async expects URL and optional options");
        return NULL;
    }
    value_t *options = args.size() == 2 ? args[1] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http2: options must be a dict");
        return NULL;
    }
    std::string url = ((string_value_t *)args[0])->value;
    value_t *owned_options = options ? dupval(options) : NULL;
    task_ptr task = fiber_start_async([url, owned_options]() {
        std::unique_ptr<value_t, void (*)(value_t *)> holder(
            owned_options, freeval);
        return exchange("GET", url, owned_options, NULL, true);
    });
    if (!task) {
        freeval(owned_options);
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *http2_post_async(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 3 || args[0]->type != T_string) {
        yyerror("std.http2.post_async expects URL, optional body, and "
                "options");
        return NULL;
    }
    std::string url = ((string_value_t *)args[0])->value;
    value_t *body = NULL;
    value_t *options = NULL;
    if (args.size() == 2) {
        if (args[1]->type == T_dict)
            options = args[1];
        else
            body = args[1];
    } else if (args.size() == 3) {
        body = args[1];
        options = args[2];
    }
    if (options && options->type != T_dict) {
        yyerror("std.http2: options must be a dict");
        return NULL;
    }
    value_t *owned_body = body ? dupval(body) : NULL;
    value_t *owned_options = options ? dupval(options) : NULL;
    task_ptr task = fiber_start_async([url, owned_body, owned_options]() {
        std::unique_ptr<value_t, void (*)(value_t *)> body_holder(
            owned_body, freeval);
        std::unique_ptr<value_t, void (*)(value_t *)> options_holder(
            owned_options, freeval);
        return exchange("POST", url, owned_options, owned_body, true);
    });
    if (!task) {
        freeval(owned_body);
        freeval(owned_options);
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *http2_request_many(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_list) {
        yyerror("std.http2.request_many expects a request list and optional "
                "options");
        return NULL;
    }
    value_t *options = args.size() == 2 ? args[1] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http2: options must be a dict");
        return NULL;
    }
    return exchange_many(args[0], options, false);
}

value_t *http2_request_many_async(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_list) {
        yyerror("std.http2.request_many_async expects a request list and "
                "optional options");
        return NULL;
    }
    value_t *options = args.size() == 2 ? args[1] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.http2: options must be a dict");
        return NULL;
    }
    value_t *owned_requests = dupval(args[0]);
    value_t *owned_options = options ? dupval(options) : NULL;
    task_ptr task = fiber_start_async([owned_requests, owned_options]() {
        std::unique_ptr<value_t, void (*)(value_t *)> requests_holder(
            owned_requests, freeval);
        std::unique_ptr<value_t, void (*)(value_t *)> options_holder(
            owned_options, freeval);
        return exchange_many(owned_requests, owned_options, true);
    });
    if (!task) {
        freeval(owned_requests);
        freeval(owned_options);
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *http2_listen(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3 || args[0]->type != T_string ||
        args[1]->type != T_int) {
        yyerror("std.http2.listen expects host, port, and optional timeout");
        return NULL;
    }
    long long port = ((int_value_t *)args[1])->value;
    if (port < 0 || port > 65535) {
        yyerror("std.http2.listen port must be between 0 and 65535");
        return NULL;
    }
    long long timeout_ms = 0;
    if (args.size() == 3) {
        if (args[2]->type != T_int ||
            ((int_value_t *)args[2])->value < 0) {
            yyerror("std.http2.listen timeout must be a non-negative "
                    "integer");
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

value_t *http2_listen_tls(const std::vector<value_t *> &args) {
    if (args.size() < 4 || args.size() > 5 || args[0]->type != T_string ||
        args[1]->type != T_int || args[2]->type != T_string ||
        args[3]->type != T_string) {
        yyerror("std.http2.listen_tls expects host, port, cert file, key "
                "file, and optional timeout");
        return NULL;
    }
#ifndef CALC_ENABLE_TLS
    yyerror("std.http2.listen_tls: TLS is disabled");
    return NULL;
#else
    std::vector<value_t *> listen_args;
    listen_args.push_back(dupval(args[0]));
    listen_args.push_back(dupval(args[1]));
    if (args.size() == 5)
        listen_args.push_back(dupval(args[4]));
    value_t *server = http2_listen(listen_args);
    free_args(listen_args);
    if (!server)
        return NULL;
    dict_set(server, "cert", dupval(args[2]));
    dict_set(server, "key", dupval(args[3]));
    return server;
#endif
}

value_t *make_request(const h2_connection::server_request &request,
                      value_t *stream, long long connection_id) {
    value_t *object = (value_t *)newdictval();
    dict_set(object, "method", newstrval(request.method.c_str()));
    dict_set(object, "path", newstrval(request.path.c_str()));
    dict_set(object, "scheme", newstrval(request.scheme.c_str()));
    dict_set(object, "authority", newstrval(request.authority.c_str()));
    dict_set(object, "headers", headers_dict(request.headers));
    dict_set(object, "body", newbytesval(request.body));
    dict_set(object, "http_major", newintval(2));
    dict_set(object, "http_minor", newintval(0));
    dict_set(object, "socket", dupval(stream));
    dict_set(object, "connection_id", newintval(connection_id));
    return object;
}

bool feed_chunk(h2_connection &connection, value_t *chunk) {
    if (!chunk || chunk->type != T_bytes) {
        freeval(chunk);
        yyerror("std.http2: read returned non-bytes");
        return false;
    }
    const std::string &data = ((bytes_value_t *)chunk)->value;
    ssize_t consumed = nghttp2_session_mem_recv(
        connection.session, (const uint8_t *)data.data(), data.size());
    freeval(chunk);
    if (consumed < 0) {
        yyerror("std.http2: nghttp2 receive failed");
        return false;
    }
    return true;
}

bool submit_server_response(h2_connection &connection, int32_t stream_id,
                            value_t *response) {
    if (response->type != T_dict) {
        yyerror("std.http2.serve handler must return a response dict");
        return false;
    }
    value_t *status_value = dict_get(response, "status");
    if (!status_value || status_value->type != T_int) {
        yyerror("std.http2.serve response status must be an integer");
        return false;
    }
    long long status = ((int_value_t *)status_value)->value;
    if (status < 100 || status > 999) {
        yyerror("std.http2.serve response status is invalid");
        return false;
    }
    value_t *headers = dict_get(response, "headers");
    if (headers && headers->type != T_dict && headers->type != T_null) {
        yyerror("std.http2.serve response headers must be a dict");
        return false;
    }
    std::string body;
    generator_ptr body_generator;
    value_t *body_stream_value = dict_get(response, "body_stream");
    if (body_stream_value && body_stream_value->type != T_null) {
        if (!fiber_is_generator(body_stream_value) ||
            !get_generator_value(body_stream_value, body_generator))
            return false;
        if (!body_value(dict_get(response, "body"), body,
                        "std.http2.serve"))
            return false;
        if (!body.empty()) {
            yyerror("std.http2.serve: body and body_stream cannot both be "
                    "set");
            return false;
        }
    } else if (!body_value(dict_get(response, "body"), body,
                           "std.http2.serve")) {
        return false;
    }

    std::vector<header_pair> response_headers;
    header_pair status_header = {":status", std::to_string(status)};
    response_headers.push_back(status_header);
    if (headers && headers->type == T_dict) {
        dict_value_t *dict = (dict_value_t *)headers;
        for (size_t i = 0; i < dict->payload->items.size(); ++i) {
            value_t *key = dict->payload->items[i].first;
            value_t *value = dict->payload->items[i].second;
            if (key->type != T_string || value->type != T_string) {
                yyerror("std.http2.serve header names and values must be "
                        "strings");
                return false;
            }
            std::string name = ((string_value_t *)key)->value;
            if (name.empty() || name[0] == ':') {
                yyerror("std.http2.serve header names must not be pseudo "
                        "headers");
                return false;
            }
            if (header_is_connection_specific(name))
                continue;
            header_pair pair;
            pair.name = lower_ascii(name);
            pair.value = ((string_value_t *)value)->value;
            response_headers.push_back(pair);
        }
    }

    std::vector<nghttp2_nv> nva;
    build_nv(response_headers, nva);

    nghttp2_data_provider provider;
    nghttp2_data_provider *provider_ptr = NULL;
    if (body_generator) {
        generator_state &state = connection.response_streams[stream_id];
        state.generator = body_generator;
        state.chunk.clear();
        state.offset = 0;
        provider.source.ptr = &state;
        provider.read_callback = generator_state_read_callback;
        provider_ptr = &provider;
    } else if (!body.empty()) {
        out_body &body_slot = connection.response_bodies[stream_id];
        body_slot.data = body;
        body_slot.offset = 0;
        provider.source.ptr = &body_slot;
        provider.read_callback = body_read_callback;
        provider_ptr = &provider;
    }
    int rv = nghttp2_submit_response(connection.session, stream_id,
                                     nva.data(), nva.size(), provider_ptr);
    if (rv != 0) {
        yyerror("std.http2.serve: cannot submit response");
        return false;
    }
    connection.requests[stream_id].responded = true;
    return true;
}

struct h2_server_state {
    long long max_requests = 0;
    long long max_per_connection = 100;
    long long idle_timeout_ms = 0;
    long long served = 0;
    bool stopping = false;
    value_t *on_request_body = NULL;
    std::vector<nghttp2_settings_entry> settings;
};

void handle_h2_server_connection(value_t *stream, long long connection_id,
                                 value_t *handler,
                                 h2_server_state *state) {
        h2_connection connection;
        connection.stream = stream;
        connection.async = true;
        connection.timeout_ms = 0;
        connection.max_body_bytes = DEFAULT_MAX_BODY_BYTES;
        connection.on_request_body = state->on_request_body;

        nghttp2_session_callbacks *callbacks = NULL;
        if (nghttp2_session_callbacks_new(&callbacks) != 0) {
            close_socket(stream);
            freeval(stream);
            return;
        }
        setup_callbacks(callbacks, false);
        int rv = nghttp2_session_server_new(&connection.session, callbacks,
                                            &connection);
        nghttp2_session_callbacks_del(callbacks);
        if (rv != 0) {
            close_socket(stream);
            freeval(stream);
            return;
        }
        nghttp2_submit_settings(
            connection.session, NGHTTP2_FLAG_NONE,
            state->settings.empty() ? NULL : state->settings.data(),
            state->settings.size());

        bool connection_open = true;
        bool draining = false;
        bool goaway_sent = false;
        int32_t last_served_stream_id = 0;
        long long connection_requests = 0;
        std::map<int32_t, task_ptr> handler_tasks;
        std::chrono::steady_clock::time_point last_activity =
            std::chrono::steady_clock::now();
        while (connection_open) {
            if (!draining &&
                (state->stopping ||
                 (state->max_requests != 0 &&
                  state->served >= state->max_requests)))
                draining = true;

            rv = nghttp2_session_send(connection.session);
            if (rv != 0) {
                connection.failure = "nghttp2 send failed";
                connection_open = false;
                break;
            }

            while (!draining && !connection.ready.empty()) {
                if (state->max_requests != 0 &&
                    state->served + (long long)handler_tasks.size() >=
                        state->max_requests) {
                    draining = true;
                    break;
                }
                if (state->max_per_connection != 0 &&
                    connection_requests +
                            (long long)handler_tasks.size() >=
                        state->max_per_connection) {
                    draining = true;
                    break;
                }
                int32_t stream_id = connection.ready.front();
                connection.ready.erase(connection.ready.begin());
                std::map<int32_t, h2_connection::server_request>::iterator it =
                    connection.requests.find(stream_id);
                if (it == connection.requests.end() || it->second.responded)
                    continue;

                value_t *request =
                    make_request(it->second, stream, connection_id);
                task_ptr handler_task = fiber_start_async(
                    [handler, request]() {
                    value_t *result =
                        call_value_args(handler,
                                        std::vector<value_t *>{request});
                    if (result && is_task_value(result))
                        result = await_task_value(result);
                    return result;
                });
                if (!handler_task) {
                    freeval(request);
                    connection.failure = "cannot start handler";
                    connection_open = false;
                    break;
                }
                handler_tasks[stream_id] = handler_task;
            }
            if (!connection_open)
                break;

            if (draining && !goaway_sent) {
                int32_t last_stream_id = last_served_stream_id;
                for (std::map<int32_t, task_ptr>::iterator it =
                         handler_tasks.begin();
                     it != handler_tasks.end(); ++it)
                    if (it->first > last_stream_id)
                        last_stream_id = it->first;
                nghttp2_submit_goaway(connection.session,
                                      NGHTTP2_FLAG_NONE, last_stream_id,
                                      NGHTTP2_NO_ERROR, NULL, 0);
                goaway_sent = true;
            }

            for (std::map<int32_t, task_ptr>::iterator it =
                     handler_tasks.begin();
                 it != handler_tasks.end();) {
                task_state_code handler_state =
                    task_get_state(it->second);
                if (handler_state == TASK_STATE_PENDING) {
                    ++it;
                    continue;
                }
                int32_t stream_id = it->first;
                if (handler_state != TASK_STATE_DONE) {
                    connection.failure = task_error_message(it->second);
                    if (connection.failure.empty())
                        connection.failure = "handler failed";
                    connection_open = false;
                    break;
                }
                value_t *response = task_copy_result(it->second);
                bool ok = submit_server_response(connection, stream_id,
                                                 response);
                freeval(response);
                it = handler_tasks.erase(it);
                if (!ok) {
                    connection_open = false;
                    break;
                }
                ++state->served;
                ++connection_requests;
                if (stream_id > last_served_stream_id)
                    last_served_stream_id = stream_id;
                if (state->max_requests != 0 &&
                    state->served >= state->max_requests) {
                    state->stopping = true;
                    draining = true;
                }
                if (state->max_per_connection != 0 &&
                    connection_requests >=
                        state->max_per_connection) {
                    connection_open = false;
                    break;
                }
            }
            if (!connection_open)
                break;

            rv = nghttp2_session_send(connection.session);
            if (rv != 0)
                break;

            if (draining && handler_tasks.empty())
                break;

            long long read_timeout = 100;
            if (!handler_tasks.empty() || draining)
                read_timeout = 20;
            if (state->idle_timeout_ms != 0)
                read_timeout = std::min<long long>(
                    read_timeout, state->idle_timeout_ms);
            connection.timeout_ms = read_timeout;

            bool timed_out = false;
            value_t *chunk = NULL;
            {
                eval_capture_snapshot snapshot;
                eval_capture_begin(snapshot);
                chunk = h2_read(&connection);
                eval_capture_result captured = eval_capture_end(snapshot);
                if (captured.failed) {
                    std::string message;
                    if (captured.error && captured.error->type == T_error)
                        message =
                            ((error_value_t *)captured.error)->message;
                    freeval(captured.error);
                    if (message.find("timed out") != std::string::npos)
                        timed_out = true;
                    else if (!message.empty())
                        yyerror("std.http2.serve: %s", message.c_str());
                }
            }
            if (timed_out) {
                if (state->stopping)
                    draining = true;
                if (draining && handler_tasks.empty())
                    break;
                if (handler_tasks.empty() &&
                    state->idle_timeout_ms != 0 &&
                    std::chrono::steady_clock::now() - last_activity >=
                        std::chrono::milliseconds(
                            state->idle_timeout_ms))
                    break;
                continue;
            }
            if (!chunk)
                break;
            if (chunk->type == T_null) {
                freeval(chunk);
                break;
            }
            if (!feed_chunk(connection, chunk))
                break;
            last_activity = std::chrono::steady_clock::now();
        }

        for (std::map<int32_t, task_ptr>::iterator it =
                 handler_tasks.begin();
             it != handler_tasks.end(); ++it) {
            if (task_get_state(it->second) == TASK_STATE_PENDING) {
                std::vector<value_t *> cancel_args;
                cancel_args.push_back(new_task_value(it->second));
                value_t *cancelled =
                    call_task_builtin(B_task_cancel, cancel_args);
                freeval(cancelled);
            }
        }
        handler_tasks.clear();

        if (!goaway_sent) {
            int32_t last_stream_id = last_served_stream_id;
            if (last_stream_id == 0)
                last_stream_id =
                    nghttp2_session_get_last_proc_stream_id(
                        connection.session);
            nghttp2_submit_goaway(connection.session, NGHTTP2_FLAG_NONE,
                                  last_stream_id, NGHTTP2_NO_ERROR, NULL,
                                  0);
        }
        nghttp2_session_send(connection.session);
        nghttp2_session_del(connection.session);
        close_socket(stream);
        freeval(stream);
}

value_t *http2_serve(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 5 || args[0]->type != T_dict ||
        (args[1]->type != T_func && args[1]->type != T_builtin &&
         args[1]->type != T_method)) {
        yyerror("std.http2.serve expects server, handler, optional "
                "max_requests, max_per_connection, idle_timeout_ms");
        return NULL;
    }
    value_t *socket = dict_get(args[0], "socket");
    if (!socket) {
        yyerror("std.http2.serve expects a std.http2 server");
        return NULL;
    }

    long long max_requests = 0;
    long long max_per_connection = 100;
    long long idle_timeout_ms = 0;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i]->type != T_int ||
            ((int_value_t *)args[i])->value < 0) {
            yyerror("std.http2.serve limits must be non-negative integers");
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
            yyerror("std.http2.serve: invalid TLS server certificate");
            return NULL;
        }
        cert_file = ((string_value_t *)cert_value)->value;
        key_file = ((string_value_t *)key_value)->value;
    }

    value_t *on_request_body = dict_get(args[0], "on_request_body");
    if (on_request_body && on_request_body->type == T_null)
        on_request_body = NULL;
    if (on_request_body && !callable_value(on_request_body)) {
        yyerror("std.http2.serve on_request_body must be a function");
        return NULL;
    }

    std::vector<nghttp2_settings_entry> settings;
    if (!build_h2_settings(dict_get(args[0], "settings"), true, settings))
        return NULL;

    h2_server_state state;
    state.max_requests = max_requests;
    state.max_per_connection = max_per_connection;
    state.idle_timeout_ms = idle_timeout_ms;
    state.on_request_body = on_request_body;
    state.settings = settings;

    value_t *handler = args[1];
    task_ptr server_task = fiber_start_async(
        [socket, handler, &state, cert_file, key_file]() {
        long long next_connection_id = 1;
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
#ifdef CALC_ENABLE_TLS
                value_t *tls_options = (value_t *)newdictval();
                value_t *alpn = (value_t *)newlistval();
                ((list_value_t *)alpn)->payload->items.push_back(
                    newstrval("h2"));
                dict_set(tls_options, "alpn", alpn);
                std::vector<value_t *> tls_args;
                tls_args.push_back(dupval(client));
                tls_args.push_back(newstrval(cert_file.c_str()));
                tls_args.push_back(newstrval(key_file.c_str()));
                tls_args.push_back(tls_options);
                stream = call_tls(B_tls_server, tls_args);
                if (!stream) {
                    close_socket(client);
                    freeval(client);
                    continue;
                }
                freeval(client);
#else
                yyerror("std.http2.serve: TLS is disabled");
                close_socket(client);
                freeval(client);
                continue;
#endif
            }

            long long connection_id = next_connection_id++;
            task_ptr connection_task = fiber_start_async(
                [stream, connection_id, handler, &state]() {
                handle_h2_server_connection(stream, connection_id, handler,
                                            &state);
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

} // namespace

value_t *call_http2_builtin(int functype,
                            const std::vector<value_t *> &args) {
    switch (functype) {
        case B_http2_request:
            return http2_request(args);
        case B_http2_get:
            return http2_get(args);
        case B_http2_post:
            return http2_post(args);
        case B_http2_request_async:
            return http2_request_async(args);
        case B_http2_get_async:
            return http2_get_async(args);
        case B_http2_post_async:
            return http2_post_async(args);
        case B_http2_request_many:
            return http2_request_many(args);
        case B_http2_request_many_async:
            return http2_request_many_async(args);
        case B_http2_listen:
            return http2_listen(args);
        case B_http2_listen_tls:
            return http2_listen_tls(args);
        case B_http2_serve:
            return http2_serve(args);
        default:
            yyerror("unknown std.http2 function");
            return NULL;
    }
}

void http2_shutdown() {
    h2_pool_shutdown();
}
