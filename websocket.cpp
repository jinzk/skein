#include "websocket.h"

#include "ast.h"
#include "eval.h"
#include "fiber.h"
#include "http.h"
#include "io.h"
#include "task.h"
#include "valuetype.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

const std::size_t WS_MAX_PAYLOAD = 16 * 1024 * 1024;
const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void free_args(std::vector<value_t *> &args) {
    for (size_t i = 0; i < args.size(); ++i)
        freeval(args[i]);
    args.clear();
}

value_t *call_io(int functype, std::vector<value_t *> args) {
    value_t *result = call_io_builtin(functype, args);
    free_args(args);
    return result;
}

value_t *call_http(int functype, std::vector<value_t *> args) {
    value_t *result = call_http_builtin(functype, args);
    free_args(args);
    return result;
}

void close_socket(value_t *socket) {
    if (!socket)
        return;
    value_t *result = call_io(B_io_close, {dupval(socket)});
    freeval(result);
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
    for (size_t i = 0; i < value.size(); ++i)
        value[i] = (char)std::tolower((unsigned char)value[i]);
    return value;
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

std::string base64_encode(const std::string &input) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= input.size()) {
        uint32_t value = ((uint32_t)(unsigned char)input[i] << 16) |
                         ((uint32_t)(unsigned char)input[i + 1] << 8) |
                         (uint32_t)(unsigned char)input[i + 2];
        output.push_back(table[(value >> 18) & 0x3f]);
        output.push_back(table[(value >> 12) & 0x3f]);
        output.push_back(table[(value >> 6) & 0x3f]);
        output.push_back(table[value & 0x3f]);
        i += 3;
    }
    if (i < input.size()) {
        uint32_t value = (uint32_t)(unsigned char)input[i] << 16;
        if (i + 1 < input.size())
            value |= (uint32_t)(unsigned char)input[i + 1] << 8;
        output.push_back(table[(value >> 18) & 0x3f]);
        output.push_back(table[(value >> 12) & 0x3f]);
        output.push_back(i + 1 < input.size() ? table[(value >> 6) & 0x3f]
                                              : '=');
        output.push_back('=');
    }
    return output;
}

uint32_t rotl32(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::string sha1(const std::string &input) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string message = input;
    uint64_t bit_length = (uint64_t)input.size() * 8;
    message.push_back((char)0x80);
    while ((message.size() % 64) != 56)
        message.push_back((char)0);
    for (int i = 7; i >= 0; --i)
        message.push_back((char)((bit_length >> (i * 8)) & 0xff));

    for (size_t offset = 0; offset < message.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const unsigned char *p =
                (const unsigned char *)message.data() + offset + i * 4;
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::string digest;
    digest.reserve(20);
    uint32_t values[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        digest.push_back((char)((values[i] >> 24) & 0xff));
        digest.push_back((char)((values[i] >> 16) & 0xff));
        digest.push_back((char)((values[i] >> 8) & 0xff));
        digest.push_back((char)(values[i] & 0xff));
    }
    return digest;
}

std::string websocket_accept(const std::string &key) {
    return base64_encode(sha1(key + WS_GUID));
}

std::string random_bytes(std::size_t count) {
    std::random_device device;
    std::string output(count, '\0');
    for (std::size_t i = 0; i < count; ++i)
        output[i] = (char)(device() & 0xff);
    return output;
}

struct ws_connection {
    value_t *stream = NULL;
    bool client = false;
    bool closed = false;
    bool stream_closed = false;
    std::string fragment;
    int fragment_opcode = 0;

    ~ws_connection() {
        if (stream) {
            if (!stream_closed)
                close_socket(stream);
            freeval(stream);
            stream = NULL;
        }
    }
};

std::string ws_describe(const void *) {
    return "<websocket>";
}

const native_type ws_type = {
    "websocket", ws_describe
};

bool get_ws(value_t *value, std::shared_ptr<ws_connection> &connection) {
    if (!value || value->type != T_userdata) {
        yyerror("expected a websocket connection");
        return false;
    }
    userdata_value_t *userdata = (userdata_value_t *)value;
    if (userdata->meta != &ws_type) {
        yyerror("expected a websocket connection");
        return false;
    }
    connection = std::static_pointer_cast<ws_connection>(userdata->data);
    if (!connection) {
        yyerror("invalid websocket connection");
        return false;
    }
    return true;
}

bool ws_write(ws_connection *connection, const std::string &data,
              long long timeout_ms, bool async) {
    if (!async) {
        value_t *written = call_io(B_io_write_all,
                                   {dupval(connection->stream),
                                    newbytesval(data),
                                    newintval(timeout_ms)});
        if (!written)
            return false;
        freeval(written);
        return true;
    }
    value_t *written = await_task_value(
        call_io(B_io_write_async, {dupval(connection->stream),
                                   newbytesval(data),
                                   newintval(timeout_ms)}));
    if (!written)
        return false;
    freeval(written);
    return true;
}

value_t *ws_read_exact(ws_connection *connection, std::size_t count,
                       long long timeout_ms, bool async) {
    if (!async)
        return call_io(B_io_read_exact,
                       {dupval(connection->stream), newintval(count),
                        newintval(timeout_ms)});
    std::string output;
    while (output.size() < count) {
        std::size_t remaining = count - output.size();
        value_t *chunk = await_task_value(
            call_io(B_io_read_async,
                    {dupval(connection->stream), newintval(remaining),
                     newintval(timeout_ms)}));
        if (!chunk)
            return NULL;
        if (chunk->type == T_null) {
            freeval(chunk);
            yyerror("websocket: unexpected EOF");
            return NULL;
        }
        if (chunk->type != T_bytes) {
            freeval(chunk);
            yyerror("websocket: read returned non-bytes");
            return NULL;
        }
        output += ((bytes_value_t *)chunk)->value;
        freeval(chunk);
    }
    return newbytesval(std::move(output));
}

struct ws_frame {
    bool fin = true;
    int opcode = 0;
    bool masked = false;
    std::string payload;
};

bool ws_read_frame(ws_connection *connection, long long timeout_ms,
                   bool async, ws_frame &frame) {
    value_t *header = ws_read_exact(connection, 2, timeout_ms, async);
    if (!header)
        return false;
    std::string bytes = ((bytes_value_t *)header)->value;
    freeval(header);
    unsigned char first = (unsigned char)bytes[0];
    unsigned char second = (unsigned char)bytes[1];
    if (first & 0x70) {
        yyerror("websocket: reserved bits are set");
        return false;
    }
    frame.fin = (first & 0x80) != 0;
    frame.opcode = first & 0x0f;
    frame.masked = (second & 0x80) != 0;
    uint64_t length = second & 0x7f;
    if (length == 126) {
        value_t *extended = ws_read_exact(connection, 2, timeout_ms, async);
        if (!extended)
            return false;
        std::string data = ((bytes_value_t *)extended)->value;
        freeval(extended);
        length = ((uint64_t)(unsigned char)data[0] << 8) |
                 (uint64_t)(unsigned char)data[1];
    } else if (length == 127) {
        value_t *extended = ws_read_exact(connection, 8, timeout_ms, async);
        if (!extended)
            return false;
        std::string data = ((bytes_value_t *)extended)->value;
        freeval(extended);
        length = 0;
        for (int i = 0; i < 8; ++i)
            length = (length << 8) | (uint64_t)(unsigned char)data[i];
    }
    if (length > WS_MAX_PAYLOAD) {
        yyerror("websocket: frame exceeds payload limit");
        return false;
    }
    if (frame.opcode >= 0x8 && (!frame.fin || length > 125)) {
        yyerror("websocket: invalid control frame");
        return false;
    }
    if (!connection->client && !frame.masked) {
        yyerror("websocket: client frame is not masked");
        return false;
    }
    std::string mask;
    if (frame.masked) {
        value_t *mask_value = ws_read_exact(connection, 4, timeout_ms, async);
        if (!mask_value)
            return false;
        mask = ((bytes_value_t *)mask_value)->value;
        freeval(mask_value);
    }
    if (length > 0) {
        value_t *payload =
            ws_read_exact(connection, (std::size_t)length, timeout_ms, async);
        if (!payload)
            return false;
        frame.payload = ((bytes_value_t *)payload)->value;
        freeval(payload);
    } else {
        frame.payload.clear();
    }
    if (frame.masked) {
        for (size_t i = 0; i < frame.payload.size(); ++i)
            frame.payload[i] ^= mask[i % 4];
    }
    return true;
}

bool ws_write_frame(ws_connection *connection, int opcode,
                    const std::string &payload, bool fin,
                    long long timeout_ms, bool async) {
    std::string frame;
    frame.push_back((char)((fin ? 0x80 : 0) | (opcode & 0x0f)));
    bool masked = connection->client;
    std::size_t length = payload.size();
    unsigned char mask_bit = masked ? 0x80 : 0;
    if (length <= 125) {
        frame.push_back((char)(mask_bit | (unsigned char)length));
    } else if (length <= 65535) {
        frame.push_back((char)(mask_bit | 126));
        frame.push_back((char)((length >> 8) & 0xff));
        frame.push_back((char)(length & 0xff));
    } else {
        frame.push_back((char)(mask_bit | 127));
        for (int i = 7; i >= 0; --i)
            frame.push_back((char)((length >> (i * 8)) & 0xff));
    }
    std::string data = payload;
    if (masked) {
        std::string key = random_bytes(4);
        frame += key;
        for (size_t i = 0; i < data.size(); ++i)
            data[i] ^= key[i % 4];
    }
    frame += data;
    return ws_write(connection, frame, timeout_ms, async);
}

value_t *ws_message(int opcode, const std::string &payload) {
    value_t *message = (value_t *)newdictval();
    if (opcode == 0x1) {
        dict_set(message, "type", newstrval("text"));
        dict_set(message, "data", newstrval(payload.c_str()));
    } else {
        dict_set(message, "type", newstrval("binary"));
        dict_set(message, "data", newbytesval(payload));
    }
    return message;
}

value_t *ws_receive_impl(ws_connection *connection, long long timeout_ms,
                         bool async) {
    if (connection->closed)
        return newnull();
    for (;;) {
        ws_frame frame;
        if (!ws_read_frame(connection, timeout_ms, async, frame))
            return NULL;
        if (frame.opcode == 0x8) {
            if (!connection->closed)
                ws_write_frame(connection, 0x8, frame.payload, true,
                               timeout_ms, async);
            connection->closed = true;
            value_t *message = (value_t *)newdictval();
            dict_set(message, "type", newstrval("close"));
            int code = 1005;
            std::string reason;
            if (frame.payload.size() >= 2) {
                code = ((unsigned char)frame.payload[0] << 8) |
                       (unsigned char)frame.payload[1];
                reason = frame.payload.substr(2);
            }
            dict_set(message, "code", newintval(code));
            dict_set(message, "reason", newstrval(reason.c_str()));
            return message;
        }
        if (frame.opcode == 0x9) {
            if (!ws_write_frame(connection, 0xA, frame.payload, true,
                                timeout_ms, async))
                return NULL;
            continue;
        }
        if (frame.opcode == 0xA) {
            value_t *message = (value_t *)newdictval();
            dict_set(message, "type", newstrval("pong"));
            dict_set(message, "data", newbytesval(frame.payload));
            return message;
        }
        if (frame.opcode == 0x1 || frame.opcode == 0x2) {
            if (!frame.fin) {
                connection->fragment = frame.payload;
                connection->fragment_opcode = frame.opcode;
                continue;
            }
            return ws_message(frame.opcode, frame.payload);
        }
        if (frame.opcode == 0x0) {
            if (connection->fragment_opcode == 0) {
                yyerror("websocket: unexpected continuation frame");
                return NULL;
            }
            connection->fragment += frame.payload;
            if (connection->fragment.size() > WS_MAX_PAYLOAD) {
                yyerror("websocket: message exceeds payload limit");
                return NULL;
            }
            if (frame.fin) {
                int opcode = connection->fragment_opcode;
                std::string payload = connection->fragment;
                connection->fragment.clear();
                connection->fragment_opcode = 0;
                return ws_message(opcode, payload);
            }
            continue;
        }
        yyerror("websocket: unknown opcode %d", frame.opcode);
        return NULL;
    }
}

value_t *ws_connect_impl(const std::string &url, value_t *options,
                         bool async) {
    std::string target = url;
    if (target.rfind("ws://", 0) == 0)
        target = "http://" + target.substr(5);
    else if (target.rfind("wss://", 0) == 0)
        target = "https://" + target.substr(6);
    value_t *user_headers = dict_get(options, "headers");
    if (user_headers && user_headers->type != T_dict) {
        yyerror("std.websocket.connect headers must be a dict");
        return NULL;
    }
    std::string key = base64_encode(random_bytes(16));
    value_t *headers = (value_t *)newdictval();
    if (user_headers && user_headers->type == T_dict) {
        dict_value_t *dict = (dict_value_t *)user_headers;
        for (size_t i = 0; i < dict->payload->items.size(); ++i) {
            value_t *key = dict->payload->items[i].first;
            value_t *value = dict->payload->items[i].second;
            if (key->type != T_string || value->type != T_string) {
                freeval(headers);
                yyerror("std.websocket.connect headers must be strings");
                return NULL;
            }
            dict_set(headers, ((string_value_t *)key)->value,
                     dupval(value));
        }
    }
    dict_set(headers, "Upgrade", newstrval("websocket"));
    dict_set(headers, "Sec-WebSocket-Key", newstrval(key.c_str()));
    dict_set(headers, "Sec-WebSocket-Version", newstrval("13"));

    value_t *http_options = (value_t *)newdictval();
    dict_set(http_options, "upgrade", newboolval(true));
    dict_set(http_options, "headers", headers);
    value_t *timeout_value = dict_get(options, "timeout_ms");
    if (timeout_value)
        dict_set(http_options, "timeout_ms", dupval(timeout_value));
    value_t *tls_options = dict_get(options, "tls");
    if (tls_options)
        dict_set(http_options, "tls", dupval(tls_options));

    value_t *response =
        call_http(async ? B_http_get_async : B_http_get,
                  {newstrval(target.c_str()), http_options});
    if (async)
        response = await_task_value(response);
    if (!response)
        return NULL;
    value_t *status = dict_get(response, "status");
    value_t *upgraded = dict_get(response, "upgraded");
    value_t *socket = dict_get(response, "socket");
    value_t *response_headers = dict_get(response, "headers");
    bool ok = status && status->type == T_int &&
              ((int_value_t *)status)->value == 101 && upgraded &&
              upgraded->type == T_bool &&
              ((bool_value_t *)upgraded)->value && socket;
    if (ok) {
        std::string accept = header_value(response_headers,
                                          "Sec-WebSocket-Accept");
        if (accept != websocket_accept(key))
            ok = false;
    }
    if (!ok) {
        yyerror("std.websocket.connect: handshake rejected");
        freeval(response);
        return NULL;
    }
    value_t *stream = dupval(socket);
    freeval(response);
    std::shared_ptr<ws_connection> connection =
        std::make_shared<ws_connection>();
    connection->stream = stream;
    connection->client = true;
    return newuserdataval(&ws_type, connection);
}

value_t *ws_accept_impl(value_t *request) {
    if (!request || request->type != T_dict) {
        yyerror("std.websocket.accept expects an HTTP request");
        return NULL;
    }
    value_t *headers = dict_get(request, "headers");
    std::string upgrade = lower_ascii(header_value(headers, "Upgrade"));
    std::string key = header_value(headers, "Sec-WebSocket-Key");
    std::string version = header_value(headers, "Sec-WebSocket-Version");
    if (upgrade.find("websocket") == std::string::npos || key.empty() ||
        version != "13") {
        yyerror("std.websocket.accept: invalid websocket handshake");
        return NULL;
    }
    value_t *response_headers = (value_t *)newdictval();
    dict_set(response_headers, "Sec-WebSocket-Accept",
             newstrval(websocket_accept(key).c_str()));
    value_t *stream =
        call_http(B_http_upgrade, {dupval(request), response_headers});
    if (!stream)
        return NULL;
    std::shared_ptr<ws_connection> connection =
        std::make_shared<ws_connection>();
    connection->stream = stream;
    connection->client = false;
    return newuserdataval(&ws_type, connection);
}

bool ws_payload(value_t *value, std::string &payload, const char *who) {
    if (!value || (value->type != T_string && value->type != T_bytes)) {
        yyerror("%s expects string or bytes", who);
        return false;
    }
    if (value->type == T_string)
        payload = ((string_value_t *)value)->value;
    else
        payload = ((bytes_value_t *)value)->value;
    return true;
}

value_t *ws_send_impl(const std::vector<value_t *> &args, bool async) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.websocket.send expects connection, data, and optional "
                "timeout");
        return NULL;
    }
    std::shared_ptr<ws_connection> connection;
    if (!get_ws(args[0], connection))
        return NULL;
    if (connection->closed) {
        yyerror("std.websocket.send: connection is closed");
        return NULL;
    }
    std::string payload;
    if (!ws_payload(args[1], payload, "std.websocket.send"))
        return NULL;
    int opcode = args[1]->type == T_string ? 0x1 : 0x2;
    long long timeout_ms = 0;
    if (args.size() == 3) {
        if (args[2]->type != T_int || ((int_value_t *)args[2])->value < 0) {
            yyerror("std.websocket.send timeout must be a non-negative "
                    "integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)args[2])->value;
    }
    if (!ws_write_frame(connection.get(), opcode, payload, true,
                        timeout_ms, async))
        return NULL;
    return newboolval(true);
}

value_t *ws_receive_builtin(const std::vector<value_t *> &args, bool async) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.websocket.receive expects connection and optional "
                "timeout");
        return NULL;
    }
    std::shared_ptr<ws_connection> connection;
    if (!get_ws(args[0], connection))
        return NULL;
    long long timeout_ms = 0;
    if (args.size() == 2) {
        if (args[1]->type != T_int || ((int_value_t *)args[1])->value < 0) {
            yyerror("std.websocket.receive timeout must be a non-negative "
                    "integer");
            return NULL;
        }
        timeout_ms = ((int_value_t *)args[1])->value;
    }
    return ws_receive_impl(connection.get(), timeout_ms, async);
}

value_t *ws_ping_builtin(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.websocket.ping expects connection and optional data");
        return NULL;
    }
    std::shared_ptr<ws_connection> connection;
    if (!get_ws(args[0], connection))
        return NULL;
    std::string payload;
    if (args.size() == 2 &&
        !ws_payload(args[1], payload, "std.websocket.ping"))
        return NULL;
    if (payload.size() > 125) {
        yyerror("std.websocket.ping payload must be at most 125 bytes");
        return NULL;
    }
    if (!ws_write_frame(connection.get(), 0x9, payload, true, 0, false))
        return NULL;
    return newboolval(true);
}

value_t *ws_close_builtin(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 3) {
        yyerror("std.websocket.close expects connection, optional code and "
                "reason");
        return NULL;
    }
    std::shared_ptr<ws_connection> connection;
    if (!get_ws(args[0], connection))
        return NULL;
    long long code = 1000;
    std::string reason;
    if (args.size() >= 2) {
        if (args[1]->type != T_int) {
            yyerror("std.websocket.close code must be an integer");
            return NULL;
        }
        code = ((int_value_t *)args[1])->value;
    }
    if (args.size() == 3) {
        if (args[2]->type != T_string) {
            yyerror("std.websocket.close reason must be a string");
            return NULL;
        }
        reason = ((string_value_t *)args[2])->value;
    }
    if (reason.size() > 123) {
        yyerror("std.websocket.close reason is too long");
        return NULL;
    }
    if (!connection->closed) {
        std::string payload;
        payload.push_back((char)((code >> 8) & 0xff));
        payload.push_back((char)(code & 0xff));
        payload += reason;
        if (!ws_write_frame(connection.get(), 0x8, payload, true, 0,
                            false))
            return NULL;
        connection->closed = true;
    }
    close_socket(connection->stream);
    freeval(connection->stream);
    connection->stream = NULL;
    connection->stream_closed = true;
    return newboolval(true);
}

value_t *ws_connect_builtin(const std::vector<value_t *> &args,
                            bool async) {
    if (args.size() < 1 || args.size() > 2 || args[0]->type != T_string) {
        yyerror("std.websocket.connect expects URL and optional options");
        return NULL;
    }
    value_t *options = args.size() == 2 ? args[1] : NULL;
    if (options && options->type != T_dict) {
        yyerror("std.websocket.connect options must be a dict");
        return NULL;
    }
    std::string url = ((string_value_t *)args[0])->value;
    if (!async)
        return ws_connect_impl(url, options, false);
    value_t *owned_options = options ? dupval(options) : NULL;
    task_ptr task = fiber_start_async([url, owned_options]() {
        std::shared_ptr<value_t> holder(owned_options, freeval);
        return ws_connect_impl(url, owned_options, true);
    });
    if (!task) {
        freeval(owned_options);
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *ws_send_builtin(const std::vector<value_t *> &args, bool async) {
    if (!async)
        return ws_send_impl(args, false);
    std::vector<value_t *> owned;
    for (size_t i = 0; i < args.size(); ++i)
        owned.push_back(dupval(args[i]));
    task_ptr task = fiber_start_async([owned]() {
        std::vector<value_t *> values = owned;
        value_t *result = ws_send_impl(values, true);
        for (size_t i = 0; i < values.size(); ++i)
            freeval(values[i]);
        return result;
    });
    if (!task) {
        for (size_t i = 0; i < owned.size(); ++i)
            freeval(owned[i]);
        return NULL;
    }
    return new_task_value(std::move(task));
}

value_t *ws_receive_builtin_async(const std::vector<value_t *> &args) {
    std::vector<value_t *> owned;
    for (size_t i = 0; i < args.size(); ++i)
        owned.push_back(dupval(args[i]));
    task_ptr task = fiber_start_async([owned]() {
        std::vector<value_t *> values = owned;
        value_t *result = ws_receive_builtin(values, true);
        for (size_t i = 0; i < values.size(); ++i)
            freeval(values[i]);
        return result;
    });
    if (!task) {
        for (size_t i = 0; i < owned.size(); ++i)
            freeval(owned[i]);
        return NULL;
    }
    return new_task_value(std::move(task));
}

} // namespace

value_t *call_websocket_builtin(
    int functype, const std::vector<value_t *> &args) {
    switch (functype) {
        case B_ws_connect:
            return ws_connect_builtin(args, false);
        case B_ws_connect_async:
            return ws_connect_builtin(args, true);
        case B_ws_accept:
            if (args.size() != 1) {
                yyerror("std.websocket.accept expects an HTTP request");
                return NULL;
            }
            return ws_accept_impl(args[0]);
        case B_ws_send:
            return ws_send_builtin(args, false);
        case B_ws_send_async:
            return ws_send_builtin(args, true);
        case B_ws_receive:
            return ws_receive_builtin(args, false);
        case B_ws_receive_async:
            return ws_receive_builtin_async(args);
        case B_ws_ping:
            return ws_ping_builtin(args);
        case B_ws_close:
            return ws_close_builtin(args);
        default:
            yyerror("unknown std.websocket function");
            return NULL;
    }
}
