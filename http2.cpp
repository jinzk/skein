#include "http2.h"

#include "ast.h"
#include "eval.h"
#include "fiber.h"
#include "io.h"
#include "net.h"
#include "task.h"
#ifdef CALC_ENABLE_TLS
#include "tls.h"
#endif
#include "valuetype.h"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <cctype>
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

std::string lower_ascii(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char c = (unsigned char)value[i];
        value[i] = (char)std::tolower(c);
    }
    return value;
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
    std::size_t max_body_bytes = DEFAULT_MAX_BODY_BYTES;
    out_body request_body;

    /* server request state */
    struct server_request {
        std::string method;
        std::string path;
        std::string scheme;
        std::string authority;
        std::vector<header_pair> headers;
        std::string body;
        bool complete = false;
        bool responded = false;
    };
    std::map<int32_t, server_request> requests;
    std::vector<int32_t> ready;
    std::map<int32_t, out_body> response_bodies;
};

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

int client_on_data(nghttp2_session *, uint8_t, int32_t stream_id,
                   const uint8_t *data, size_t len, void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (stream_id != connection->stream_id)
        return 0;
    if (connection->response_body.size() + len > connection->max_body_bytes) {
        connection->failure = "response body exceeds limit";
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    connection->response_body.append((const char *)data, len);
    return 0;
}

int client_on_stream_close(nghttp2_session *, int32_t stream_id,
                           uint32_t error_code, void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (stream_id != connection->stream_id)
        return 0;
    connection->done = true;
    if (error_code != NGHTTP2_NO_ERROR && connection->failure.empty())
        connection->failure = "HTTP/2 stream closed with error " +
                              std::to_string(error_code);
    return 0;
}

int client_on_header2(nghttp2_session *, const nghttp2_frame *frame,
                      const uint8_t *name, size_t namelen,
                      const uint8_t *value, size_t valuelen, uint8_t,
                      void *user_data) {
    h2_connection *connection = (h2_connection *)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->hd.stream_id != connection->stream_id)
        return 0;
    std::string key((const char *)name, namelen);
    std::string text((const char *)value, valuelen);
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
    if (request.body.size() + len > connection->max_body_bytes) {
        connection->failure = "request body exceeds limit";
        return NGHTTP2_ERR_CALLBACK_FAILURE;
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

    int connect_type =
        async ? B_net_tcp_connect_async : B_net_tcp_connect;
    std::vector<value_t *> connect_args;
    connect_args.push_back(newstrval(url.host.c_str()));
    connect_args.push_back(newintval(url.port));
    connect_args.push_back(newintval(timeout_ms));
    value_t *client = call_net(connect_type, connect_args);
    if (!client)
        return NULL;
    if (async) {
        client = await_task_value(client);
        if (!client)
            return NULL;
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
            return NULL;
        }
        freeval(client);

        std::vector<value_t *> alpn_args;
        alpn_args.push_back(dupval(stream));
        value_t *selected = call_tls(B_tls_alpn, alpn_args);
        if (!selected) {
            close_socket(stream);
            freeval(stream);
            return NULL;
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
            return NULL;
        }
#else
        yyerror("std.http2: https is not supported (TLS disabled)");
        close_socket(client);
        freeval(client);
        return NULL;
#endif
    }

    h2_connection connection;
    connection.stream = stream;
    connection.async = async;
    connection.timeout_ms = timeout_ms;
    value_t *max_body = dict_get(options, "max_body_bytes");
    if (max_body) {
        if (max_body->type != T_int ||
            ((int_value_t *)max_body)->value <= 0) {
            yyerror("std.http2: max_body_bytes must be a positive integer");
            close_socket(stream);
            freeval(stream);
            return NULL;
        }
        connection.max_body_bytes = (std::size_t)((int_value_t *)max_body)->value;
    }

    nghttp2_session_callbacks *callbacks = NULL;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        yyerror("std.http2: cannot create nghttp2 callbacks");
        close_socket(stream);
        freeval(stream);
        return NULL;
    }
    setup_callbacks(callbacks, true);
    int rv = nghttp2_session_client_new(&connection.session, callbacks,
                                        &connection);
    nghttp2_session_callbacks_del(callbacks);
    if (rv != 0) {
        yyerror("std.http2: cannot create nghttp2 session");
        close_socket(stream);
        freeval(stream);
        return NULL;
    }
    nghttp2_submit_settings(connection.session, NGHTTP2_FLAG_NONE, NULL, 0);

    std::vector<header_pair> request_headers;
    header_pair method_header = {":method", method};
    header_pair scheme_header = {":scheme", url.tls ? "https" : "http"};
    header_pair authority_header = {":authority", url.authority};
    header_pair path_header = {":path", url.path};
    request_headers.push_back(method_header);
    request_headers.push_back(scheme_header);
    request_headers.push_back(authority_header);
    request_headers.push_back(path_header);
    if (!collect_request_headers(headers, request_headers)) {
        nghttp2_session_del(connection.session);
        close_socket(stream);
        freeval(stream);
        return NULL;
    }

    std::vector<nghttp2_nv> nva;
    build_nv(request_headers, nva);

    connection.request_body.data = body;
    nghttp2_data_provider provider;
    nghttp2_data_provider *provider_ptr = NULL;
    if (!body.empty()) {
        provider.source.ptr = &connection.request_body;
        provider.read_callback = body_read_callback;
        provider_ptr = &provider;
    }

    connection.stream_id = nghttp2_submit_request(
        connection.session, NULL, nva.data(), nva.size(), provider_ptr,
        NULL);
    if (connection.stream_id < 0) {
        yyerror("std.http2: cannot submit request");
        nghttp2_session_del(connection.session);
        close_socket(stream);
        freeval(stream);
        return NULL;
    }

    bool failed = false;
    while (!connection.done) {
        rv = nghttp2_session_send(connection.session);
        if (rv != 0) {
            connection.failure = "nghttp2 send failed";
            failed = true;
            break;
        }
        value_t *chunk = h2_read(&connection);
        if (!chunk) {
            failed = true;
            break;
        }
        if (chunk->type == T_null) {
            freeval(chunk);
            connection.failure = "connection closed before response";
            failed = true;
            break;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            connection.failure = "read returned non-bytes";
            failed = true;
            break;
        }
        const std::string &data = ((bytes_value_t *)chunk)->value;
        ssize_t consumed = nghttp2_session_mem_recv(
            connection.session, (const uint8_t *)data.data(), data.size());
        freeval(chunk);
        if (consumed < 0) {
            connection.failure = "nghttp2 receive failed";
            failed = true;
            break;
        }
    }

    rv = nghttp2_session_send(connection.session);
    nghttp2_session_del(connection.session);
    close_socket(stream);
    freeval(stream);

    if (failed || !connection.failure.empty()) {
        yyerror("std.http2: %s", connection.failure.empty()
                                     ? "request failed"
                                     : connection.failure.c_str());
        return NULL;
    }
    return make_response(connection);
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
    value_t *body = args.size() >= 2 ? args[1] : NULL;
    value_t *options = args.size() == 3 ? args[2] : NULL;
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
    value_t *body = args.size() >= 2 ? args[1] : NULL;
    value_t *options = args.size() == 3 ? args[2] : NULL;
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
                      value_t *stream) {
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
    if (!body_value(dict_get(response, "body"), body, "std.http2.serve"))
        return false;

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

    out_body &body_slot = connection.response_bodies[stream_id];
    body_slot.data = body;
    body_slot.offset = 0;
    nghttp2_data_provider provider;
    nghttp2_data_provider *provider_ptr = NULL;
    if (!body.empty()) {
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
                break;
            }
            freeval(client);
#else
            yyerror("std.http2.serve: TLS is disabled");
            close_socket(client);
            freeval(client);
            break;
#endif
        }

        h2_connection connection;
        connection.stream = stream;
        connection.async = false;
        connection.timeout_ms = 0;
        connection.max_body_bytes = DEFAULT_MAX_BODY_BYTES;

        nghttp2_session_callbacks *callbacks = NULL;
        if (nghttp2_session_callbacks_new(&callbacks) != 0) {
            close_socket(stream);
            freeval(stream);
            break;
        }
        setup_callbacks(callbacks, false);
        int rv = nghttp2_session_server_new(&connection.session, callbacks,
                                            &connection);
        nghttp2_session_callbacks_del(callbacks);
        if (rv != 0) {
            close_socket(stream);
            freeval(stream);
            break;
        }
        nghttp2_submit_settings(connection.session, NGHTTP2_FLAG_NONE, NULL,
                                0);

        bool connection_open = true;
        long long connection_requests = 0;
        while (connection_open &&
               (max_requests == 0 || served < max_requests)) {
            rv = nghttp2_session_send(connection.session);
            if (rv != 0)
                break;

            while (!connection.ready.empty()) {
                int32_t stream_id = connection.ready.front();
                connection.ready.erase(connection.ready.begin());
                std::map<int32_t, h2_connection::server_request>::iterator it =
                    connection.requests.find(stream_id);
                if (it == connection.requests.end() || it->second.responded)
                    continue;

                value_t *request = make_request(it->second, stream);
                value_t *response =
                    call_value_args(args[1],
                                    std::vector<value_t *>{request});
                if (!response) {
                    connection_open = false;
                    break;
                }
                bool ok = submit_server_response(connection, stream_id,
                                                 response);
                freeval(response);
                if (!ok) {
                    connection_open = false;
                    break;
                }
                ++served;
                ++connection_requests;
                if ((max_requests != 0 && served >= max_requests) ||
                    (max_per_connection != 0 &&
                     connection_requests >= max_per_connection))
                    connection_open = false;
            }
            if (!connection_open)
                break;

            rv = nghttp2_session_send(connection.session);
            if (rv != 0)
                break;

            bool timed_out = false;
            bool clean_eof = false;
            value_t *chunk = NULL;
            {
                std::vector<value_t *> read_args;
                read_args.push_back(dupval(stream));
                read_args.push_back(newintval(8192));
                read_args.push_back(newintval(idle_timeout_ms));
                eval_capture_snapshot snapshot;
                eval_capture_begin(snapshot);
                chunk = call_io(B_io_read, read_args);
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
                    if (timed_out)
                        break;
                    break;
                }
                if (chunk && chunk->type == T_null) {
                    clean_eof = true;
                    freeval(chunk);
                    chunk = NULL;
                }
            }
            if (timed_out || clean_eof)
                break;
            if (!chunk)
                break;
            if (!feed_chunk(connection, chunk))
                break;
        }

        nghttp2_submit_goaway(
            connection.session, NGHTTP2_FLAG_NONE,
            nghttp2_session_get_last_proc_stream_id(connection.session),
            NGHTTP2_NO_ERROR, NULL, 0);
        nghttp2_session_send(connection.session);
        nghttp2_session_del(connection.session);
        close_socket(stream);
        freeval(stream);
    }
    return newintval(served);
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

void http2_shutdown() {}
