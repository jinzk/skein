#include "net.h"

#include "ast.h"
#include "async_support.h"
#include "dict.h"
#include "event_loop.h"
#include "platform.h"
#include "repl.h"
#include "task.h"
#include "valuetype.h"

#include <uv.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace {

void on_tcp_close(uv_handle_t *handle) {
    delete (uv_tcp_t *)handle;
}

struct resolve_state {
    bool done = false;
    int status = 0;
    struct addrinfo *addresses = NULL;
    uv_getaddrinfo_t request = {};
    event_wait_result wait_result = EVENT_WAIT_OK;
};

void on_resolve(uv_getaddrinfo_t *request, int status,
                struct addrinfo *addresses) {
    resolve_state *state = (resolve_state *)request->data;
    state->status = status;
    state->addresses = addresses;
    state->done = true;
}

bool getaddrinfo_async(const char *node, const char *service,
                       const struct addrinfo *hints,
                       const event_timeout &timeout, resolve_state &state) {
    if (!event_loop_init()) {
        state.status = UV_ENOMEM;
        return false;
    }
    state.request.data = &state;
    int rc = uv_getaddrinfo(event_loop_handle(), &state.request, on_resolve,
                            node, service, hints);
    if (rc < 0) {
        state.status = rc;
        return false;
    }
    state.wait_result = event_wait(state.done, [&state] {
        uv_cancel((uv_req_t *)&state.request);
    }, timeout);
    if (state.wait_result != EVENT_WAIT_OK) {
        if (state.addresses)
            uv_freeaddrinfo(state.addresses);
        state.addresses = NULL;
        return false;
    }
    if (state.status < 0) {
        if (state.addresses)
            uv_freeaddrinfo(state.addresses);
        state.addresses = NULL;
        return false;
    }
    return true;
}

using async_state = skein_async::state;
using skein_async::async_complete;
using skein_async::async_fail;
using skein_async::async_cancel;
using skein_async::async_timeout;
using skein_async::start_async_timeout;
using skein_async::prepare_async;
using skein_async::async_timeout_error;

struct net_socket;
struct accept_async_op;
struct recv_async_op;

struct accept_state {
    bool done = false;
    int status = 0;
    std::shared_ptr<net_socket> client;
};

struct net_socket : std::enable_shared_from_this<net_socket> {
    uv_tcp_t *handle = NULL;
    bool listening = false;
    accept_state *pending_accept = NULL;
    std::deque<std::shared_ptr<net_socket>> pending_clients;
    std::deque<accept_async_op *> pending_async_accepts;
    recv_async_op *pending_async_read = NULL;

    bool reading = false;
    bool read_done = false;
    bool read_eof = false;
    bool read_failed = false;
    int read_status = 0;
    int read_error = 0;
    size_t read_requested = 0;
    std::string read_buffer;
    std::string read_result;

    ~net_socket() {
        close();
    }

    void close() {
        if (!handle)
            return;
        uv_tcp_t *old = handle;
        handle = NULL;
        old->data = NULL;
        if (!uv_is_closing((uv_handle_t *)old))
            uv_close((uv_handle_t *)old, on_tcp_close);
    }
};

struct accept_async_op : async_state {
    std::shared_ptr<net_socket> listener;
};

struct recv_async_op : async_state {
    std::shared_ptr<net_socket> socket;
    bool stream_mode = false;
};

void remove_accept_async(accept_async_op *op) {
    if (!op || !op->listener)
        return;
    std::deque<accept_async_op *> &queue =
        op->listener->pending_async_accepts;
    queue.erase(std::remove(queue.begin(), queue.end(), op), queue.end());
}

void cancel_accept_async(accept_async_op *op) {
    remove_accept_async(op);
}

void fail_async_accept(net_socket *listener, const std::string &error) {
    if (!listener || listener->pending_async_accepts.empty())
        return;
    accept_async_op *op = listener->pending_async_accepts.front();
    listener->pending_async_accepts.pop_front();
    async_fail(op, error);
}

void cancel_recv_async(recv_async_op *op) {
    if (!op || !op->socket)
        return;
    net_socket *socket = op->socket.get();
    if (socket->pending_async_read == op)
        socket->pending_async_read = NULL;
    if (socket->handle)
        uv_read_stop((uv_stream_t *)socket->handle);
    socket->reading = false;
    socket->read_done = true;
}

std::shared_ptr<net_socket> new_socket() {
    if (!event_loop_init())
        return NULL;
    std::shared_ptr<net_socket> socket = std::make_shared<net_socket>();
    socket->handle = new uv_tcp_t;
    int rc = uv_tcp_init(event_loop_handle(), socket->handle);
    if (rc < 0) {
        delete socket->handle;
        socket->handle = NULL;
        return NULL;
    }
    socket->handle->data = socket.get();
    return socket;
}

std::string net_describe(const void *data) {
    const net_socket *socket = (const net_socket *)data;
    if (!socket || !socket->handle)
        return "<tcp_socket closed>";
    return "<tcp_socket>";
}

bool net_stream_read(void *data, std::size_t max_bytes,
                     long long timeout_ms, std::string &out, bool &eof,
                     std::string &error);
bool net_stream_write(void *data, const std::string &bytes,
                      long long timeout_ms, std::size_t &written,
                      std::string &error);
bool net_stream_flush(void *data, std::string &error);
bool net_stream_close(void *data, bool &was_open, std::string &error);
bool net_stream_eof(void *data, bool &eof, std::string &error);
bool net_stream_seekable(void *data);
value_t *net_stream_read_async(void *data,
                               const std::shared_ptr<void> &owner,
                               std::size_t max_bytes, long long timeout_ms,
                               std::string &error);
value_t *net_stream_write_async(void *data,
                                const std::shared_ptr<void> &owner,
                                const std::string &bytes,
                                long long timeout_ms, std::string &error);

const struct stream_ops net_stream_ops = {
    net_stream_read,
    net_stream_write,
    net_stream_flush,
    net_stream_close,
    net_stream_eof,
    net_stream_seekable,
    NULL,
    NULL,
    net_stream_read_async,
    net_stream_write_async,
};

const struct native_type net_socket_type = {
    "tcp_socket", net_describe, &net_stream_ops
};

const struct native_type net_listener_type = {
    "tcp_listener", net_describe
};

value_t *new_socket_value(std::shared_ptr<net_socket> socket) {
    return newuserdataval(&net_socket_type, socket);
}

value_t *new_listener_value(std::shared_ptr<net_socket> socket) {
    return newuserdataval(&net_listener_type, socket);
}

bool get_socket(struct value_t *value, std::shared_ptr<net_socket> &socket) {
    if (!value || value->type != T_userdata) {
        yyerror("expected a tcp socket, got %s", typename_of(value));
        return false;
    }
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    if (userdata->meta != &net_socket_type &&
        userdata->meta != &net_listener_type) {
        yyerror("expected a tcp socket, got %s", typename_of(value));
        return false;
    }
    socket = std::static_pointer_cast<net_socket>(userdata->data);
    if (!socket || !socket->handle) {
        yyerror("tcp socket is closed");
        return false;
    }
    return true;
}

bool get_port(struct value_t *value, int &port) {
    if (!value || value->type != T_int) {
        yyerror("port must be an integer");
        return false;
    }
    long long n = ((struct int_value_t *)value)->value;
    if (n < 0 || n > 65535) {
        yyerror("port must be in 0..65535");
        return false;
    }
    port = (int)n;
    return true;
}

std::string service_string(int port) {
    return std::to_string(port);
}

bool get_optional_timeout(const std::vector<value_t *> &args, size_t index,
                          long long &timeout_ms) {
    timeout_ms = 0;
    if (args.size() <= index)
        return true;
    if (args[index]->type != T_int) {
        yyerror("timeout_ms must be an integer");
        return false;
    }
    long long value = ((struct int_value_t *)args[index])->value;
    if (value < 0) {
        yyerror("timeout_ms must be non-negative");
        return false;
    }
    timeout_ms = value;
    return true;
}

bool get_optional_family(const std::vector<value_t *> &args, size_t index,
                         int &family) {
    family = AF_UNSPEC;
    if (args.size() <= index)
        return true;
    if (args[index]->type != T_string) {
        yyerror("family must be a string");
        return false;
    }
    const char *text = ((struct string_value_t *)args[index])->value;
    if (std::strcmp(text, "ipv4") == 0) {
        family = AF_INET;
    } else if (std::strcmp(text, "ipv6") == 0) {
        family = AF_INET6;
    } else if (std::strcmp(text, "any") == 0 || text[0] == '\0') {
        family = AF_UNSPEC;
    } else {
        yyerror("family must be \"any\", \"ipv4\", or \"ipv6\"");
        return false;
    }
    return true;
}

void report_timeout(const char *operation, long long timeout_ms) {
    yyerror("%s: timed out after %lld ms", operation, timeout_ms);
}

value_t *address_list_from_addrinfo(struct addrinfo *addresses) {
    struct list_value_t *list = newlistval();
    for (struct addrinfo *address = addresses; address;
         address = address->ai_next) {
        char host[64];
        int rc = -1;
        if (address->ai_family == AF_INET) {
            rc = uv_ip4_name((const struct sockaddr_in *)address->ai_addr,
                             host, sizeof host);
        } else if (address->ai_family == AF_INET6) {
            rc = uv_ip6_name((const struct sockaddr_in6 *)address->ai_addr,
                             host, sizeof host);
        }
        if (rc != 0)
            continue;

        bool duplicate = false;
        for (size_t i = 0; i < list->payload->items.size(); ++i) {
            if (std::strcmp(
                    ((struct string_value_t *)list->payload->items[i])->value,
                    host) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            list->payload->items.push_back(newstrval(host));
    }
    return (value_t *)list;
}

value_t *address_value(const struct sockaddr *address) {
    char host[64];
    const char *family = NULL;
    int port = 0;
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *ipv4 =
            (const struct sockaddr_in *)address;
        if (uv_ip4_name(ipv4, host, sizeof host) != 0)
            return NULL;
        port = (int)ntohs(ipv4->sin_port);
        family = "ipv4";
    } else if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 =
            (const struct sockaddr_in6 *)address;
        if (uv_ip6_name(ipv6, host, sizeof host) != 0)
            return NULL;
        port = (int)ntohs(ipv6->sin6_port);
        family = "ipv6";
    } else {
        return NULL;
    }

    struct dict_value_t *result = newdictval();
    result->payload->items.push_back(std::make_pair(
        newstrval("address"), newstrval(host)));
    result->payload->items.push_back(std::make_pair(
        newstrval("port"), newintval(port)));
    result->payload->items.push_back(std::make_pair(
        newstrval("family"), newstrval(family)));
    return (value_t *)result;
}

struct connect_state {
    bool done = false;
    int status = 0;
    uv_connect_t request = {};
};

void on_connect(uv_connect_t *request, int status) {
    connect_state *state = (connect_state *)request->data;
    state->status = status;
    state->done = true;
}

struct write_state {
    bool done = false;
    int status = 0;
    uv_write_t request = {};
    std::string data;
};

void on_write(uv_write_t *request, int status) {
    write_state *state = (write_state *)request->data;
    state->status = status;
    state->done = true;
}

struct shutdown_state {
    bool done = false;
    int status = 0;
    uv_shutdown_t request = {};
};

void on_socket_shutdown(uv_shutdown_t *request, int status) {
    shutdown_state *state = (shutdown_state *)request->data;
    state->status = status;
    state->done = true;
}

void on_connection(uv_stream_t *server, int status) {
    net_socket *listener = (net_socket *)server->data;
    if (!listener)
        return;

    if (status < 0) {
        if (listener->pending_accept &&
            !listener->pending_accept->done) {
            listener->pending_accept->status = status;
            listener->pending_accept->done = true;
        } else {
            fail_async_accept(
                listener, std::string("accept_async: ") +
                              uv_strerror(status));
        }
        return;
    }

    std::shared_ptr<net_socket> client = new_socket();
    if (!client) {
        if (listener->pending_accept &&
            !listener->pending_accept->done) {
            listener->pending_accept->status = UV_ENOMEM;
            listener->pending_accept->done = true;
        } else {
            fail_async_accept(listener, "accept_async: out of memory");
        }
        return;
    }

    int rc = uv_accept(server, (uv_stream_t *)client->handle);
    if (rc < 0) {
        client->close();
        if (listener->pending_accept &&
            !listener->pending_accept->done) {
            listener->pending_accept->status = rc;
            listener->pending_accept->done = true;
        } else {
            fail_async_accept(listener, std::string("accept_async: ") +
                                            uv_strerror(rc));
        }
        return;
    }

    if (listener->pending_accept && !listener->pending_accept->done) {
        listener->pending_accept->client = client;
        listener->pending_accept->status = 0;
        listener->pending_accept->done = true;
    } else if (!listener->pending_async_accepts.empty()) {
        accept_async_op *op = listener->pending_async_accepts.front();
        listener->pending_async_accepts.pop_front();
        async_complete(op, new_socket_value(client));
    } else if (listener->pending_clients.size() < 16) {
        listener->pending_clients.push_back(std::move(client));
    } else {
        client->close();
    }
}

void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    net_socket *socket = (net_socket *)handle->data;
    if (!socket || !socket->reading) {
        buf->base = NULL;
        buf->len = 0;
        return;
    }
    size_t size = suggested_size;
    if (size > socket->read_requested)
        size = socket->read_requested;
    if (size == 0)
        size = 1;
    socket->read_buffer.resize(size);
    buf->base = &socket->read_buffer[0];
    buf->len = socket->read_buffer.size();
}

void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *) {
    net_socket *socket = (net_socket *)stream->data;
    if (!socket || !socket->reading)
        return;

    // nread == 0 means EAGAIN/EWOULDBLOCK, not EOF. Keep reading.
    if (nread == 0)
        return;

    if (nread > 0) {
        size_t size = (size_t)nread;
        if (size > socket->read_requested)
            size = socket->read_requested;
        socket->read_result.assign(&socket->read_buffer[0], size);
        socket->read_status = 0;
    } else if (nread == UV_EOF) {
        socket->read_result.clear();
        socket->read_status = 0;
        socket->read_eof = true;
    } else {
        socket->read_result.clear();
        socket->read_status = (int)nread;
        socket->read_error = (int)nread;
        socket->read_failed = true;
    }

    socket->reading = false;
    uv_read_stop(stream);

    recv_async_op *op = socket->pending_async_read;
    if (!op) {
        socket->read_done = true;
        return;
    }

    socket->pending_async_read = NULL;
    if (socket->read_status < 0) {
        async_fail(op, std::string("recv_async: ") +
                           uv_strerror(socket->read_status));
    } else if (op->stream_mode && socket->read_eof &&
               socket->read_result.empty()) {
        async_complete(op, newnull());
    } else {
        async_complete(op, newbytesval(std::move(socket->read_result)));
    }
}

value_t *net_tcp_connect(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 4) {
        yyerror("std.net.tcp_connect expects 2 to 4 arguments");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("tcp_connect host must be a string");
        return NULL;
    }
    int port = 0;
    if (!get_port(args[1], port))
        return NULL;
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 2, timeout_ms))
        return NULL;
    int family = AF_UNSPEC;
    if (!get_optional_family(args, 3, family))
        return NULL;
    if (g_interrupted)
        return NULL;

    struct addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;

    event_timeout timeout = make_event_timeout(timeout_ms);
    std::string service = service_string(port);
    const char *host = ((struct string_value_t *)args[0])->value;
    resolve_state resolved;
    if (!getaddrinfo_async(host, service.c_str(), &hints, timeout,
                           resolved)) {
        if (g_interrupted)
            return NULL;
        if (resolved.wait_result == EVENT_WAIT_TIMEOUT) {
            report_timeout("tcp_connect", timeout_ms);
            return NULL;
        }
        yyerror("tcp_connect: %s", uv_strerror(resolved.status));
        return NULL;
    }

    int last_status = 0;
    event_wait_result wait_result = EVENT_WAIT_OK;
    for (struct addrinfo *address = resolved.addresses; address;
         address = address->ai_next) {
        if (g_interrupted) {
            wait_result = EVENT_WAIT_INTERRUPTED;
            break;
        }
        if (event_timeout_expired(timeout)) {
            wait_result = EVENT_WAIT_TIMEOUT;
            break;
        }

        std::shared_ptr<net_socket> socket = new_socket();
        if (!socket) {
            last_status = UV_ENOMEM;
            continue;
        }

        connect_state state;
        state.request.data = &state;
        int rc = uv_tcp_connect(&state.request, socket->handle,
                                address->ai_addr, on_connect);
        if (rc < 0) {
            last_status = rc;
            socket->close();
            continue;
        }

        wait_result = event_wait(state.done, [&socket] {
            socket->close();
        }, timeout);
        if (wait_result != EVENT_WAIT_OK) {
            socket->close();
            break;
        }
        if (state.status == 0) {
            uv_freeaddrinfo(resolved.addresses);
            return new_socket_value(socket);
        }
        last_status = state.status;
        socket->close();
    }

    uv_freeaddrinfo(resolved.addresses);
    if (wait_result == EVENT_WAIT_INTERRUPTED || g_interrupted)
        return NULL;
    if (wait_result == EVENT_WAIT_TIMEOUT || event_timeout_expired(timeout)) {
        report_timeout("tcp_connect", timeout_ms);
        return NULL;
    }
    yyerror("tcp_connect: %s",
            uv_strerror(last_status ? last_status : UV_EINVAL));
    return NULL;
}

value_t *net_tcp_listen(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 5) {
        yyerror("std.net.tcp_listen expects 2 to 5 arguments");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("tcp_listen host must be a string");
        return NULL;
    }
    int port = 0;
    if (!get_port(args[1], port))
        return NULL;
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 2, timeout_ms))
        return NULL;
    int family = AF_UNSPEC;
    if (!get_optional_family(args, 3, family))
        return NULL;
    bool reuse_port = false;
    if (args.size() == 5) {
        if (args[4]->type != T_dict) {
            yyerror("tcp_listen options must be a dict");
            return NULL;
        }
        value_t *flag = NULL;
        struct dict_value_t *options = (struct dict_value_t *)args[4];
        for (const auto &entry : options->payload->items) {
            if (entry.first->type == T_string &&
                std::strcmp(
                    ((struct string_value_t *)entry.first)->value,
                    "reuse_port") == 0) {
                flag = entry.second;
                break;
            }
        }
        if (flag) {
            if (flag->type != T_bool) {
                yyerror("tcp_listen reuse_port must be a bool");
                return NULL;
            }
            reuse_port = ((bool_value_t *)flag)->value;
        }
    }
    if (reuse_port && !SKEIN_HAS_REUSE_PORT) {
        yyerror("tcp_listen: reuse_port is not supported on this platform");
        return NULL;
    }
    if (g_interrupted)
        return NULL;

    struct addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    event_timeout timeout = make_event_timeout(timeout_ms);
    std::string service = service_string(port);
    const char *host = ((struct string_value_t *)args[0])->value;
    resolve_state resolved;
    if (!getaddrinfo_async(host[0] ? host : NULL, service.c_str(),
                           &hints, timeout, resolved)) {
        if (g_interrupted)
            return NULL;
        if (resolved.wait_result == EVENT_WAIT_TIMEOUT) {
            report_timeout("tcp_listen", timeout_ms);
            return NULL;
        }
        yyerror("tcp_listen: %s", uv_strerror(resolved.status));
        return NULL;
    }

    unsigned int bind_flags = 0;
#if SKEIN_HAS_REUSE_PORT
    if (reuse_port)
        bind_flags |= UV_TCP_REUSEPORT;
#endif

    int last_status = 0;
    for (struct addrinfo *address = resolved.addresses; address;
         address = address->ai_next) {
        if (g_interrupted)
            break;
        if (event_timeout_expired(timeout))
            break;

        std::shared_ptr<net_socket> socket = new_socket();
        if (!socket) {
            last_status = UV_ENOMEM;
            continue;
        }

        int rc = uv_tcp_bind(socket->handle, address->ai_addr, bind_flags);
        if (rc == 0)
            rc = uv_listen((uv_stream_t *)socket->handle, 16,
                           on_connection);
        if (rc < 0) {
            last_status = rc;
            socket->close();
            continue;
        }

        socket->listening = true;
        uv_freeaddrinfo(resolved.addresses);
        return new_listener_value(socket);
    }

    uv_freeaddrinfo(resolved.addresses);
    if (g_interrupted)
        return NULL;
    if (event_timeout_expired(timeout)) {
        report_timeout("tcp_listen", timeout_ms);
        return NULL;
    }
    yyerror("tcp_listen: %s",
            uv_strerror(last_status ? last_status : UV_EINVAL));
    return NULL;
}

value_t *net_accept(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.net.accept expects 1 or 2 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> listener;
    if (!get_socket(args[0], listener))
        return NULL;
    if (!listener->listening) {
        yyerror("accept expects a listening tcp socket");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 1, timeout_ms))
        return NULL;

    if (g_interrupted)
        return NULL;

    if (!listener->pending_clients.empty()) {
        std::shared_ptr<net_socket> client =
            std::move(listener->pending_clients.front());
        listener->pending_clients.pop_front();
        return new_socket_value(std::move(client));
    }

    event_timeout timeout = make_event_timeout(timeout_ms);
    accept_state state;
    listener->pending_accept = &state;
    event_wait_result wait_result = event_wait(state.done,
        [&listener, &state] {
            listener->pending_accept = NULL;
            state.done = true;
        }, timeout);
    listener->pending_accept = NULL;

    if (wait_result != EVENT_WAIT_OK) {
        if (state.client)
            listener->pending_clients.push_front(std::move(state.client));
        if (wait_result == EVENT_WAIT_TIMEOUT)
            report_timeout("accept", timeout_ms);
        return NULL;
    }
    if (state.status < 0) {
        yyerror("accept: %s", uv_strerror(state.status));
        return NULL;
    }
    if (!state.client) {
        yyerror("accept: no client");
        return NULL;
    }
    return new_socket_value(std::move(state.client));
}

bool net_send_data(net_socket *socket, std::string data,
                   long long timeout_ms, std::size_t &written,
                   std::string &error) {
    written = 0;
    if (!socket || !socket->handle) {
        error = "tcp socket is closed";
        return false;
    }
    if (socket->listening) {
        error = "send expects a connected tcp socket";
        return false;
    }
    if (data.empty())
        return true;
    if (g_interrupted)
        return false;
    if (data.size() > (size_t)std::numeric_limits<unsigned int>::max()) {
        error = "send data is too large";
        return false;
    }

    write_state state;
    state.data = std::move(data);
    state.request.data = &state;
    uv_buf_t buffer = uv_buf_init(
        &state.data[0], (unsigned int)state.data.size());
    int rc = uv_write(&state.request, (uv_stream_t *)socket->handle,
                      &buffer, 1, on_write);
    if (rc < 0) {
        error = std::string("send: ") + uv_strerror(rc);
        return false;
    }

    event_timeout timeout = make_event_timeout(timeout_ms);
    event_wait_result wait_result = event_wait(state.done, [socket] {
        socket->close();
    }, timeout);
    if (wait_result == EVENT_WAIT_TIMEOUT) {
        error = "send: timed out after " + std::to_string(timeout_ms) + " ms";
        return false;
    }
    if (wait_result != EVENT_WAIT_OK)
        return false;
    if (state.status < 0) {
        error = std::string("send: ") + uv_strerror(state.status);
        return false;
    }
    written = state.data.size();
    return true;
}

value_t *net_send(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.net.send expects 2 or 3 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    if (socket->listening) {
        yyerror("send expects a connected tcp socket");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 2, timeout_ms))
        return NULL;

    std::string data;
    if (args[1]->type == T_string) {
        data = ((struct string_value_t *)args[1])->value;
    } else if (args[1]->type == T_bytes) {
        data = ((struct bytes_value_t *)args[1])->value;
    } else {
        yyerror("send data must be string or bytes");
        return NULL;
    }

    std::size_t written = 0;
    std::string error;
    if (!net_send_data(socket.get(), std::move(data), timeout_ms, written,
                       error)) {
        if (g_interrupted)
            return NULL;
        yyerror("%s", error.empty() ? "send failed" : error.c_str());
        return NULL;
    }
    return newintval((long long)written);
}

bool net_recv_data(net_socket *socket, std::size_t requested,
                   long long timeout_ms, std::string &out, bool &eof,
                   std::string &error) {
    out.clear();
    eof = false;
    if (!socket || !socket->handle) {
        error = "tcp socket is closed";
        return false;
    }
    if (socket->listening) {
        error = "recv expects a connected tcp socket";
        return false;
    }
    if (requested == 0)
        return true;
    if (requested > (std::size_t)std::numeric_limits<int>::max()) {
        error = "recv size is too large";
        return false;
    }
    if (socket->read_eof) {
        eof = true;
        return true;
    }
    if (socket->read_failed) {
        error = std::string("recv: ") + uv_strerror(socket->read_error);
        return false;
    }
    if (socket->reading || socket->pending_async_read) {
        error = "recv: another read is already pending";
        return false;
    }
    if (g_interrupted)
        return false;

    socket->read_done = false;
    socket->read_status = 0;
    socket->read_requested = requested;
    socket->read_buffer.clear();
    socket->read_result.clear();
    socket->reading = true;

    int rc = uv_read_start((uv_stream_t *)socket->handle, on_alloc, on_read);
    if (rc < 0) {
        socket->reading = false;
        error = std::string("recv: ") + uv_strerror(rc);
        return false;
    }

    event_timeout timeout = make_event_timeout(timeout_ms);
    event_wait_result wait_result = event_wait(socket->read_done, [socket] {
        uv_read_stop((uv_stream_t *)socket->handle);
        socket->reading = false;
        socket->read_done = true;
    }, timeout);
    socket->reading = false;
    if (wait_result == EVENT_WAIT_TIMEOUT) {
        error = "recv: timed out after " + std::to_string(timeout_ms) + " ms";
        return false;
    }
    if (wait_result != EVENT_WAIT_OK)
        return false;
    if (socket->read_status < 0) {
        error = std::string("recv: ") + uv_strerror(socket->read_status);
        return false;
    }
    out = std::move(socket->read_result);
    eof = socket->read_eof && out.empty();
    return true;
}

value_t *net_recv(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.net.recv expects 2 or 3 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    if (socket->listening) {
        yyerror("recv expects a connected tcp socket");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 2, timeout_ms))
        return NULL;
    if (args[1]->type != T_int ||
        ((struct int_value_t *)args[1])->value < 0) {
        yyerror("recv size must be a non-negative integer");
        return NULL;
    }
    long long requested = ((struct int_value_t *)args[1])->value;
    if (requested == 0)
        return newbytesval(std::string());
    std::string out;
    bool eof = false;
    std::string error;
    if (!net_recv_data(socket.get(), (std::size_t)requested, timeout_ms, out,
                       eof, error)) {
        if (g_interrupted)
            return NULL;
        yyerror("%s", error.empty() ? "recv failed" : error.c_str());
        return NULL;
    }
    return newbytesval(std::move(out));
}

bool net_stream_read(void *data, std::size_t max_bytes,
                     long long timeout_ms, std::string &out, bool &eof,
                     std::string &error) {
    return net_recv_data(static_cast<net_socket *>(data), max_bytes,
                         timeout_ms, out, eof, error);
}

bool net_stream_write(void *data, const std::string &bytes,
                      long long timeout_ms, std::size_t &written,
                      std::string &error) {
    return net_send_data(static_cast<net_socket *>(data), bytes, timeout_ms,
                         written, error);
}

bool net_stream_flush(void *data, std::string &error) {
    net_socket *socket = static_cast<net_socket *>(data);
    if (!socket || !socket->handle) {
        error = "tcp socket is closed";
        return false;
    }
    return true;
}

void close_net_socket(net_socket *socket) {
    if (!socket)
        return;
    socket->pending_accept = NULL;
    socket->pending_clients.clear();
    std::deque<accept_async_op *> accepts =
        std::move(socket->pending_async_accepts);
    socket->pending_async_accepts.clear();
    for (size_t i = 0; i < accepts.size(); ++i) {
        async_fail(accepts[i], "accept_async: listener closed");
    }
    recv_async_op *read_op = socket->pending_async_read;
    socket->pending_async_read = NULL;
    if (read_op) {
        if (socket->handle)
            uv_read_stop((uv_stream_t *)socket->handle);
        socket->reading = false;
        async_fail(read_op, "recv_async: socket closed");
    }
    socket->listening = false;
    socket->reading = false;
    socket->read_buffer.clear();
    socket->read_result.clear();
    socket->close();
}

bool net_stream_close(void *data, bool &was_open, std::string &error) {
    (void)error;
    net_socket *socket = static_cast<net_socket *>(data);
    was_open = socket && socket->handle != NULL;
    close_net_socket(socket);
    return true;
}

bool net_stream_eof(void *data, bool &eof, std::string &error) {
    net_socket *socket = static_cast<net_socket *>(data);
    if (!socket || !socket->handle) {
        error = "tcp socket is closed";
        return false;
    }
    eof = socket->read_eof;
    return true;
}

bool net_stream_seekable(void *data) {
    (void)data;
    return false;
}

value_t *net_close(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.net.close expects 1 argument");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    close_net_socket(socket.get());
    return newnull();
}

value_t *net_socket_shutdown(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.net.shutdown expects 1 or 2 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    if (socket->listening) {
        yyerror("shutdown expects a connected tcp socket");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 1, timeout_ms))
        return NULL;
    if (g_interrupted)
        return NULL;

    shutdown_state state;
    state.request.data = &state;
    int rc = uv_shutdown(&state.request, (uv_stream_t *)socket->handle,
                         on_socket_shutdown);
    if (rc < 0) {
        yyerror("shutdown: %s", uv_strerror(rc));
        return NULL;
    }

    event_timeout timeout = make_event_timeout(timeout_ms);
    event_wait_result wait_result = event_wait(state.done, [&socket] {
        socket->close();
    }, timeout);
    if (wait_result == EVENT_WAIT_TIMEOUT) {
        report_timeout("shutdown", timeout_ms);
        return NULL;
    }
    if (wait_result != EVENT_WAIT_OK)
        return NULL;
    if (state.status < 0) {
        yyerror("shutdown: %s", uv_strerror(state.status));
        return NULL;
    }
    return newnull();
}

value_t *net_address(const std::vector<value_t *> &args, bool peer) {
    if (args.size() != 1) {
        yyerror(peer ? "std.net.peer_addr expects 1 argument"
                     : "std.net.local_addr expects 1 argument");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;

    struct sockaddr_storage storage = {};
    int length = (int)sizeof storage;
    int rc = peer
        ? uv_tcp_getpeername(socket->handle,
                             (struct sockaddr *)&storage, &length)
        : uv_tcp_getsockname(socket->handle,
                             (struct sockaddr *)&storage, &length);
    if (rc < 0) {
        yyerror("%s: %s", peer ? "peer_addr" : "local_addr",
                uv_strerror(rc));
        return NULL;
    }
    value_t *result = address_value((const struct sockaddr *)&storage);
    if (!result) {
        yyerror("%s: unsupported address family",
                peer ? "peer_addr" : "local_addr");
        return NULL;
    }
    return result;
}

value_t *completed_task(value_t *value) {
    task_ptr task = task_create();
    task_complete(task, value);
    return new_task_value(std::move(task));
}

value_t *failed_task(const std::string &error) {
    task_ptr task = task_create();
    task_fail(task, error);
    return new_task_value(std::move(task));
}

struct resolve_async_op : async_state {
    uv_getaddrinfo_t request = {};
};

void on_resolve_async(uv_getaddrinfo_t *request, int status,
                      struct addrinfo *addresses) {
    resolve_async_op *op = (resolve_async_op *)request->data;
    if (op->finished) {
        if (addresses)
            uv_freeaddrinfo(addresses);
        delete op;
        return;
    }
    if (status < 0) {
        if (addresses)
            uv_freeaddrinfo(addresses);
        async_fail(op, std::string("resolve_async: ") +
                          uv_strerror(status));
        return;
    }
    value_t *list = address_list_from_addrinfo(addresses);
    if (addresses)
        uv_freeaddrinfo(addresses);
    async_complete(op, list);
}

value_t *net_resolve_async(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 3) {
        yyerror("std.net.resolve_async expects 1 to 3 arguments");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("resolve_async host must be a string");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 1, timeout_ms))
        return NULL;
    int family = AF_UNSPEC;
    if (!get_optional_family(args, 2, family))
        return NULL;
    if (g_interrupted)
        return NULL;

    struct addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;

    resolve_async_op *op = new resolve_async_op;
    task_ptr task;
    std::string timeout_error =
        async_timeout_error("resolve_async", timeout_ms);
    if (!prepare_async(op, task, timeout_ms, timeout_error)) {
        delete op;
        yyerror("resolve_async: cannot initialize event loop");
        return NULL;
    }
    op->callback_after_cancel = true;
    op->cancel_operation = [op] {
        uv_cancel((uv_req_t *)&op->request);
    };
    op->request.data = op;

    int rc = uv_getaddrinfo(event_loop_handle(), &op->request,
                            on_resolve_async,
                            ((struct string_value_t *)args[0])->value,
                            NULL, &hints);
    if (rc < 0)
        async_fail(op, std::string("resolve_async: ") + uv_strerror(rc));
    return new_task_value(std::move(task));
}

enum connect_async_phase {
    CONNECT_ASYNC_RESOLVE,
    CONNECT_ASYNC_CONNECT
};

struct connect_async_op : async_state {
    uv_getaddrinfo_t resolve_request = {};
    uv_connect_t connect_request = {};
    std::shared_ptr<net_socket> socket;
    struct addrinfo *addresses = NULL;
    struct addrinfo *current = NULL;
    int phase = CONNECT_ASYNC_RESOLVE;
    int last_status = 0;

    ~connect_async_op() override {
        if (addresses)
            uv_freeaddrinfo(addresses);
    }
};

void start_next_connect(connect_async_op *op);

void on_connect_async(uv_connect_t *request, int status) {
    connect_async_op *op = (connect_async_op *)request->data;
    if (op->finished) {
        delete op;
        return;
    }
    if (status == 0) {
        async_complete(op, new_socket_value(std::move(op->socket)));
        return;
    }
    op->last_status = status;
    if (op->socket) {
        op->socket->close();
        op->socket.reset();
    }
    op->current = op->current ? op->current->ai_next : NULL;
    start_next_connect(op);
}

void start_next_connect(connect_async_op *op) {
    while (!op->finished && op->current) {
        op->socket = new_socket();
        if (!op->socket) {
            op->last_status = UV_ENOMEM;
            op->current = op->current->ai_next;
            continue;
        }

        op->connect_request.data = op;
        int rc = uv_tcp_connect(&op->connect_request, op->socket->handle,
                                op->current->ai_addr, on_connect_async);
        if (rc == 0) {
            op->phase = CONNECT_ASYNC_CONNECT;
            return;
        }
        op->last_status = rc;
        op->socket->close();
        op->socket.reset();
        op->current = op->current->ai_next;
    }
    if (op->finished)
        return;
    async_fail(op, std::string("tcp_connect_async: ") +
                      uv_strerror(op->last_status ?
                                  op->last_status : UV_EINVAL));
}

void on_connect_async_resolve(uv_getaddrinfo_t *request, int status,
                              struct addrinfo *addresses) {
    connect_async_op *op = (connect_async_op *)request->data;
    if (op->finished) {
        if (addresses)
            uv_freeaddrinfo(addresses);
        delete op;
        return;
    }
    if (status < 0) {
        if (addresses)
            uv_freeaddrinfo(addresses);
        async_fail(op, std::string("tcp_connect_async: ") +
                          uv_strerror(status));
        return;
    }
    op->addresses = addresses;
    op->current = addresses;
    start_next_connect(op);
}

value_t *net_tcp_connect_async(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 4) {
        yyerror("std.net.tcp_connect_async expects 2 to 4 arguments");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("tcp_connect_async host must be a string");
        return NULL;
    }
    int port = 0;
    if (!get_port(args[1], port))
        return NULL;
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 2, timeout_ms))
        return NULL;
    int family = AF_UNSPEC;
    if (!get_optional_family(args, 3, family))
        return NULL;
    if (g_interrupted)
        return NULL;

    struct addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;

    connect_async_op *op = new connect_async_op;
    task_ptr task;
    std::string timeout_error =
        async_timeout_error("tcp_connect_async", timeout_ms);
    if (!prepare_async(op, task, timeout_ms, timeout_error)) {
        delete op;
        yyerror("tcp_connect_async: cannot initialize event loop");
        return NULL;
    }
    op->phase = CONNECT_ASYNC_RESOLVE;
    op->callback_after_cancel = true;
    op->cancel_operation = [op] {
        if (op->phase == CONNECT_ASYNC_RESOLVE) {
            uv_cancel((uv_req_t *)&op->resolve_request);
        } else if (op->socket) {
            op->socket->close();
        }
    };
    op->resolve_request.data = op;

    std::string service = service_string(port);
    int rc = uv_getaddrinfo(
        event_loop_handle(), &op->resolve_request,
        on_connect_async_resolve,
        ((struct string_value_t *)args[0])->value,
        service.c_str(), &hints);
    if (rc < 0)
        async_fail(op, std::string("tcp_connect_async: ") +
                          uv_strerror(rc));
    return new_task_value(std::move(task));
}

struct listen_async_op : async_state {
    uv_getaddrinfo_t request = {};
    struct addrinfo *addresses = NULL;

    ~listen_async_op() override {
        if (addresses)
            uv_freeaddrinfo(addresses);
    }
};

void on_listen_async_resolve(uv_getaddrinfo_t *request, int status,
                             struct addrinfo *addresses) {
    listen_async_op *op = (listen_async_op *)request->data;
    if (op->finished) {
        if (addresses)
            uv_freeaddrinfo(addresses);
        delete op;
        return;
    }
    if (status < 0) {
        if (addresses)
            uv_freeaddrinfo(addresses);
        async_fail(op, std::string("tcp_listen_async: ") +
                          uv_strerror(status));
        return;
    }
    op->addresses = addresses;

    int last_status = 0;
    for (struct addrinfo *address = addresses; address;
         address = address->ai_next) {
        std::shared_ptr<net_socket> socket = new_socket();
        if (!socket) {
            last_status = UV_ENOMEM;
            continue;
        }
        int rc = uv_tcp_bind(socket->handle, address->ai_addr, 0);
        if (rc == 0)
            rc = uv_listen((uv_stream_t *)socket->handle, 16,
                           on_connection);
        if (rc < 0) {
            last_status = rc;
            socket->close();
            continue;
        }
        socket->listening = true;
        async_complete(op, new_listener_value(std::move(socket)));
        return;
    }

    async_fail(op, std::string("tcp_listen_async: ") +
                      uv_strerror(last_status ? last_status : UV_EINVAL));
}

value_t *net_tcp_listen_async(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 4) {
        yyerror("std.net.tcp_listen_async expects 2 to 4 arguments");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("tcp_listen_async host must be a string");
        return NULL;
    }
    int port = 0;
    if (!get_port(args[1], port))
        return NULL;
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 2, timeout_ms))
        return NULL;
    int family = AF_UNSPEC;
    if (!get_optional_family(args, 3, family))
        return NULL;
    if (g_interrupted)
        return NULL;

    struct addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    listen_async_op *op = new listen_async_op;
    task_ptr task;
    std::string timeout_error =
        async_timeout_error("tcp_listen_async", timeout_ms);
    if (!prepare_async(op, task, timeout_ms, timeout_error)) {
        delete op;
        yyerror("tcp_listen_async: cannot initialize event loop");
        return NULL;
    }
    op->callback_after_cancel = true;
    op->cancel_operation = [op] {
        uv_cancel((uv_req_t *)&op->request);
    };
    op->request.data = op;

    std::string service = service_string(port);
    const char *host = ((struct string_value_t *)args[0])->value;
    int rc = uv_getaddrinfo(event_loop_handle(), &op->request,
                            on_listen_async_resolve,
                            host[0] ? host : NULL, service.c_str(), &hints);
    if (rc < 0)
        async_fail(op, std::string("tcp_listen_async: ") +
                          uv_strerror(rc));
    return new_task_value(std::move(task));
}

value_t *net_accept_async(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.net.accept_async expects 1 or 2 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> listener;
    if (!get_socket(args[0], listener))
        return NULL;
    if (!listener->listening) {
        yyerror("accept_async expects a listening tcp socket");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 1, timeout_ms))
        return NULL;
    if (g_interrupted)
        return NULL;

    if (!listener->pending_clients.empty()) {
        std::shared_ptr<net_socket> client =
            std::move(listener->pending_clients.front());
        listener->pending_clients.pop_front();
        return completed_task(new_socket_value(std::move(client)));
    }

    accept_async_op *op = new accept_async_op;
    task_ptr task;
    std::string timeout_error =
        async_timeout_error("accept_async", timeout_ms);
    if (!prepare_async(op, task, timeout_ms, timeout_error)) {
        delete op;
        yyerror("accept_async: cannot initialize event loop");
        return NULL;
    }
    op->listener = listener;
    op->callback_after_cancel = false;
    op->cancel_operation = [op] {
        cancel_accept_async(op);
    };
    listener->pending_async_accepts.push_back(op);
    return new_task_value(std::move(task));
}

struct send_async_op : async_state {
    uv_write_t request = {};
    std::string data;
    std::shared_ptr<net_socket> socket;
};

void on_send_async(uv_write_t *request, int status) {
    send_async_op *op = (send_async_op *)request->data;
    if (op->finished) {
        delete op;
        return;
    }
    if (status < 0) {
        async_fail(op, std::string("send_async: ") + uv_strerror(status));
        return;
    }
    async_complete(op, newintval((long long)op->data.size()));
}

value_t *net_send_async_task(std::shared_ptr<net_socket> socket,
                              std::string data, long long timeout_ms,
                              std::string &error) {
    if (!socket || !socket->handle) {
        error = "tcp socket is closed";
        return NULL;
    }
    if (socket->listening) {
        error = "send_async expects a connected tcp socket";
        return NULL;
    }
    if (data.empty())
        return completed_task(newintval(0));
    if (g_interrupted)
        return NULL;
    if (data.size() >
        (size_t)std::numeric_limits<unsigned int>::max()) {
        error = "send_async data is too large";
        return NULL;
    }

    send_async_op *op = new send_async_op;
    task_ptr task;
    std::string timeout_error =
        async_timeout_error("send_async", timeout_ms);
    if (!prepare_async(op, task, timeout_ms, timeout_error)) {
        delete op;
        error = "send_async: cannot initialize event loop";
        return NULL;
    }
    op->socket = socket;
    op->data = std::move(data);
    op->callback_after_cancel = true;
    op->cancel_operation = [op] {
        if (op->socket)
            op->socket->close();
    };
    op->request.data = op;

    uv_buf_t buffer = uv_buf_init(
        &op->data[0], (unsigned int)op->data.size());
    int rc = uv_write(&op->request, (uv_stream_t *)socket->handle,
                      &buffer, 1, on_send_async);
    if (rc < 0)
        async_fail(op, std::string("send_async: ") + uv_strerror(rc));
    return new_task_value(std::move(task));
}

value_t *net_send_async(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.net.send_async expects 2 or 3 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    if (socket->listening) {
        yyerror("send_async expects a connected tcp socket");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 2, timeout_ms))
        return NULL;

    std::string data;
    if (args[1]->type == T_string) {
        data = ((struct string_value_t *)args[1])->value;
    } else if (args[1]->type == T_bytes) {
        data = ((struct bytes_value_t *)args[1])->value;
    } else {
        yyerror("send_async data must be string or bytes");
        return NULL;
    }

    std::string error;
    value_t *task = net_send_async_task(socket, std::move(data),
                                        timeout_ms, error);
    if (!task && !error.empty())
        yyerror("%s", error.c_str());
    return task;
}

value_t *net_recv_async_task(std::shared_ptr<net_socket> socket,
                             std::size_t requested, long long timeout_ms,
                             bool stream_mode, std::string &error) {
    if (!socket || !socket->handle) {
        error = "tcp socket is closed";
        return NULL;
    }
    if (socket->listening) {
        error = "recv_async expects a connected tcp socket";
        return NULL;
    }
    if (requested == 0)
        return completed_task(newbytesval(std::string()));
    if (requested > (std::size_t)std::numeric_limits<int>::max()) {
        error = "recv_async size is too large";
        return NULL;
    }
    if (socket->read_eof) {
        if (stream_mode)
            return completed_task(newnull());
        return completed_task(newbytesval(std::string()));
    }
    if (socket->read_failed) {
        return failed_task(std::string("recv_async: ") +
                           uv_strerror(socket->read_error));
    }
    if (socket->reading || socket->pending_async_read) {
        error = "recv_async: another read is already pending";
        return NULL;
    }
    if (g_interrupted)
        return NULL;

    recv_async_op *op = new recv_async_op;
    task_ptr task;
    std::string timeout_error =
        async_timeout_error("recv_async", timeout_ms);
    if (!prepare_async(op, task, timeout_ms, timeout_error)) {
        delete op;
        error = "recv_async: cannot initialize event loop";
        return NULL;
    }
    op->socket = socket;
    op->stream_mode = stream_mode;
    op->callback_after_cancel = false;
    op->cancel_operation = [op] {
        cancel_recv_async(op);
    };

    socket->read_done = false;
    socket->read_status = 0;
    socket->read_requested = requested;
    socket->read_buffer.clear();
    socket->read_result.clear();
    socket->reading = true;
    socket->pending_async_read = op;

    int rc = uv_read_start((uv_stream_t *)socket->handle,
                           on_alloc, on_read);
    if (rc < 0) {
        socket->pending_async_read = NULL;
        socket->reading = false;
        async_fail(op, std::string("recv_async: ") + uv_strerror(rc));
    }
    return new_task_value(std::move(task));
}

value_t *net_recv_async(const std::vector<value_t *> &args) {
    if (args.size() < 2 || args.size() > 3) {
        yyerror("std.net.recv_async expects 2 or 3 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    if (socket->listening) {
        yyerror("recv_async expects a connected tcp socket");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 2, timeout_ms))
        return NULL;
    if (args[1]->type != T_int ||
        ((struct int_value_t *)args[1])->value < 0) {
        yyerror("recv_async size must be a non-negative integer");
        return NULL;
    }
    long long requested = ((struct int_value_t *)args[1])->value;

    std::string error;
    value_t *task = net_recv_async_task(socket, (std::size_t)requested,
                                        timeout_ms, false, error);
    if (!task && !error.empty())
        yyerror("%s", error.c_str());
    return task;
}

value_t *net_stream_read_async(void *data,
                               const std::shared_ptr<void> &owner,
                               std::size_t max_bytes, long long timeout_ms,
                               std::string &error) {
    (void)data;
    std::shared_ptr<net_socket> socket =
        std::static_pointer_cast<net_socket>(owner);
    return net_recv_async_task(std::move(socket), max_bytes, timeout_ms,
                               true, error);
}

value_t *net_stream_write_async(void *data,
                                const std::shared_ptr<void> &owner,
                                const std::string &bytes,
                                long long timeout_ms, std::string &error) {
    (void)data;
    std::shared_ptr<net_socket> socket =
        std::static_pointer_cast<net_socket>(owner);
    return net_send_async_task(std::move(socket), bytes, timeout_ms, error);
}

struct shutdown_async_op : async_state {
    uv_shutdown_t request = {};
    std::shared_ptr<net_socket> socket;
};

void on_shutdown_async(uv_shutdown_t *request, int status) {
    shutdown_async_op *op = (shutdown_async_op *)request->data;
    if (op->finished) {
        delete op;
        return;
    }
    if (status < 0) {
        async_fail(op, std::string("shutdown_async: ") +
                          uv_strerror(status));
        return;
    }
    async_complete(op, newnull());
}

value_t *net_shutdown_async(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.net.shutdown_async expects 1 or 2 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    if (socket->listening) {
        yyerror("shutdown_async expects a connected tcp socket");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 1, timeout_ms))
        return NULL;
    if (g_interrupted)
        return NULL;

    shutdown_async_op *op = new shutdown_async_op;
    task_ptr task;
    std::string timeout_error =
        async_timeout_error("shutdown_async", timeout_ms);
    if (!prepare_async(op, task, timeout_ms, timeout_error)) {
        delete op;
        yyerror("shutdown_async: cannot initialize event loop");
        return NULL;
    }
    op->socket = socket;
    op->callback_after_cancel = true;
    op->cancel_operation = [op] {
        if (op->socket)
            op->socket->close();
    };
    op->request.data = op;

    int rc = uv_shutdown(&op->request, (uv_stream_t *)socket->handle,
                         on_shutdown_async);
    if (rc < 0)
        async_fail(op, std::string("shutdown_async: ") +
                          uv_strerror(rc));
    return new_task_value(std::move(task));
}

value_t *net_resolve(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 3) {
        yyerror("std.net.resolve expects 1 to 3 arguments");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("resolve host must be a string");
        return NULL;
    }
    long long timeout_ms = 0;
    if (!get_optional_timeout(args, 1, timeout_ms))
        return NULL;
    int family = AF_UNSPEC;
    if (!get_optional_family(args, 2, family))
        return NULL;
    if (g_interrupted)
        return NULL;

    struct addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;

    event_timeout timeout = make_event_timeout(timeout_ms);
    resolve_state resolved;
    if (!getaddrinfo_async(
            ((struct string_value_t *)args[0])->value, NULL,
            &hints, timeout, resolved)) {
        if (g_interrupted)
            return NULL;
        if (resolved.wait_result == EVENT_WAIT_TIMEOUT) {
            report_timeout("resolve", timeout_ms);
            return NULL;
        }
        yyerror("resolve: %s", uv_strerror(resolved.status));
        return NULL;
    }

    value_t *list = address_list_from_addrinfo(resolved.addresses);
    uv_freeaddrinfo(resolved.addresses);
    return list;
}

} // namespace

void net_shutdown() {
    uv_loop_t *loop = event_loop_handle();
    if (!loop)
        return;
    uv_walk(loop, [](uv_handle_t *handle, void *) {
        if (handle->type != UV_TCP)
            return;
        net_socket *socket = (net_socket *)handle->data;
        if (socket) {
            socket->handle = NULL;
            handle->data = NULL;
        }
        if (!uv_is_closing(handle))
            uv_close(handle, on_tcp_close);
    }, NULL);
}

struct value_t *call_net_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_net_tcp_connect:
            return net_tcp_connect(args);
        case B_net_tcp_listen:
            return net_tcp_listen(args);
        case B_net_accept:
            return net_accept(args);
        case B_net_send:
            return net_send(args);
        case B_net_recv:
            return net_recv(args);
        case B_net_close:
            return net_close(args);
        case B_net_resolve:
            return net_resolve(args);
        case B_net_shutdown:
            return net_socket_shutdown(args);
        case B_net_local_addr:
            return net_address(args, false);
        case B_net_peer_addr:
            return net_address(args, true);
        case B_net_tcp_connect_async:
            return net_tcp_connect_async(args);
        case B_net_tcp_listen_async:
            return net_tcp_listen_async(args);
        case B_net_accept_async:
            return net_accept_async(args);
        case B_net_send_async:
            return net_send_async(args);
        case B_net_recv_async:
            return net_recv_async(args);
        case B_net_resolve_async:
            return net_resolve_async(args);
        case B_net_shutdown_async:
            return net_shutdown_async(args);
        default:
            yyerror("unknown network function");
            return NULL;
    }
}
