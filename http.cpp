#include "http.h"

#include "ast.h"
#include "http1_parser.h"
#include "io.h"
#include "net.h"
#include "valuetype.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
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

void close_socket(value_t *socket) {
    if (!socket)
        return;
    std::vector<value_t *> args;
    args.push_back(dupval(socket));
    value_t *result = call_io_builtin(B_io_close, args);
    free_args(args);
    freeval(result);
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
};

bool parse_url(const std::string &url, url_parts &parts, std::string &error) {
    if (url.rfind("http://", 0) != 0) {
        error = "only http:// URLs are supported";
        return false;
    }
    std::string rest = url.substr(7);
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
                  http1::message &message) {
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
        value_t *chunk = call_io(B_io_read, args);
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

std::string build_request(const std::string &method, const url_parts &url,
                          value_t *headers, const std::string &body) {
    std::string request;
    request += method;
    request += " ";
    request += url.path;
    request += " HTTP/1.1\r\n";

    if (!header_exists(headers, "Host")) {
        request += "Host: ";
        request += url.host;
        if (url.port != 80) {
            request += ":";
            request += std::to_string(url.port);
        }
        request += "\r\n";
    }
    if (!header_exists(headers, "Connection"))
        request += "Connection: close\r\n";
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

    std::vector<value_t *> connect_args;
    connect_args.push_back(newstrval(url.host.c_str()));
    connect_args.push_back(newintval(url.port));
    connect_args.push_back(newintval(timeout_ms));
    value_t *client = call_net(B_net_tcp_connect, connect_args);
    if (!client)
        return NULL;

    std::string request = build_request(method, url, headers, body);
    if (request.empty()) {
        close_socket(client);
        freeval(client);
        return NULL;
    }

    std::vector<value_t *> write_args;
    write_args.push_back(dupval(client));
    write_args.push_back(newbytesval(request));
    write_args.push_back(newintval(timeout_ms));
    value_t *written = call_io(B_io_write_all, write_args);
    if (!written) {
        close_socket(client);
        freeval(client);
        return NULL;
    }
    freeval(written);

    http1::message message;
    bool ok = read_message(client, false, timeout_ms, message);
    close_socket(client);
    freeval(client);
    if (!ok)
        return NULL;
    return make_response(message);
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

    http1::message message;
    if (!read_message(client, true, timeout_ms, message)) {
        close_socket(client);
        freeval(client);
        return NULL;
    }

    value_t *request = (value_t *)newdictval();
    dict_set(request, "method", newstrval(message.method.c_str()));
    dict_set(request, "path", newstrval(message.url.c_str()));
    dict_set(request, "headers", headers_dict(message));
    dict_set(request, "body", newbytesval(message.body));
    dict_set(request, "http_major", newintval(message.http_major));
    dict_set(request, "http_minor", newintval(message.http_minor));
    dict_set(request, "socket", client);
    return request;
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

    std::string response = "HTTP/1.1 ";
    response += std::to_string(status);
    response += " ";
    response += reason_phrase((int)status);
    response += "\r\n";
    if (!body.empty() && !header_exists(headers, "Content-Length")) {
        response += "Content-Length: ";
        response += std::to_string(body.size());
        response += "\r\n";
    }
    if (!header_exists(headers, "Connection"))
        response += "Connection: close\r\n";
    std::string error;
    if (!append_headers(response, headers, error)) {
        yyerror("std.http.respond: %s", error.c_str());
        return NULL;
    }
    response += "\r\n";
    response += body;

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

struct value_t *call_http_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_http_request:
            return http_request(args);
        case B_http_get:
            return http_get(args);
        case B_http_post:
            return http_post(args);
        case B_http_listen:
            return http_listen(args);
        case B_http_accept:
            return http_accept(args);
        case B_http_respond:
            return http_respond(args);
        case B_http_close:
            return http_close(args);
        default:
            yyerror("unknown http function");
            return NULL;
    }
}
