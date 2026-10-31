#include "net.h"

#include "ast.h"
#include "repl.h"
#include "valuetype.h"

#include <uv.h>

#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct net_runtime {
    uv_loop_t loop;
    uv_timer_t ticker;
    bool initialized = false;
};

net_runtime &get_runtime() {
    static net_runtime runtime;
    return runtime;
}

void on_tcp_close(uv_handle_t *handle) {
    delete (uv_tcp_t *)handle;
}

void on_timer_close(uv_handle_t *) {}

bool ensure_runtime() {
    net_runtime &runtime = get_runtime();
    if (runtime.initialized)
        return true;
    if (uv_loop_init(&runtime.loop) != 0)
        return false;
    if (uv_timer_init(&runtime.loop, &runtime.ticker) != 0) {
        uv_loop_close(&runtime.loop);
        return false;
    }
    if (uv_timer_start(&runtime.ticker, [](uv_timer_t *) {}, 20, 20) != 0) {
        uv_close((uv_handle_t *)&runtime.ticker, on_timer_close);
        uv_run(&runtime.loop, UV_RUN_DEFAULT);
        uv_loop_close(&runtime.loop);
        return false;
    }
    runtime.initialized = true;
    return true;
}

bool net_wait(bool &done, const std::function<void()> &interrupt) {
    net_runtime &runtime = get_runtime();
    bool interrupted = false;
    while (!done) {
        if (g_interrupted && !interrupted) {
            interrupted = true;
            interrupt();
        }
        uv_run(&runtime.loop, UV_RUN_ONCE);
    }
    return !interrupted;
}

struct resolve_state {
    bool done = false;
    int status = 0;
    struct addrinfo *addresses = NULL;
    uv_getaddrinfo_t request = {};
};

void on_resolve(uv_getaddrinfo_t *request, int status,
                struct addrinfo *addresses) {
    resolve_state *state = (resolve_state *)request->data;
    state->status = status;
    state->addresses = addresses;
    state->done = true;
}

bool getaddrinfo_async(const char *node, const char *service,
                       const struct addrinfo *hints, resolve_state &state) {
    if (!ensure_runtime()) {
        state.status = UV_ENOMEM;
        return false;
    }
    state.request.data = &state;
    int rc = uv_getaddrinfo(&get_runtime().loop, &state.request, on_resolve,
                            node, service, hints);
    if (rc < 0) {
        state.status = rc;
        return false;
    }
    bool ok = net_wait(state.done, [&state] {
        uv_cancel((uv_req_t *)&state.request);
    });
    if (!ok) {
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

struct net_socket;

struct accept_state {
    bool done = false;
    int status = 0;
    std::shared_ptr<net_socket> client;
};

struct net_socket {
    uv_tcp_t *handle = NULL;
    bool listening = false;
    accept_state *pending_accept = NULL;
    std::deque<std::shared_ptr<net_socket>> pending_clients;

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

std::shared_ptr<net_socket> new_socket() {
    if (!ensure_runtime())
        return NULL;
    std::shared_ptr<net_socket> socket = std::make_shared<net_socket>();
    socket->handle = new uv_tcp_t;
    int rc = uv_tcp_init(&get_runtime().loop, socket->handle);
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

const struct native_type net_socket_type = {
    "tcp_socket", net_describe
};

value_t *new_socket_value(std::shared_ptr<net_socket> socket) {
    return newuserdataval(&net_socket_type, socket);
}

bool get_socket(struct value_t *value, std::shared_ptr<net_socket> &socket) {
    if (!value || value->type != T_userdata) {
        yyerror("expected a tcp socket, got %s", typename_of(value));
        return false;
    }
    struct userdata_value_t *userdata = (struct userdata_value_t *)value;
    if (userdata->meta != &net_socket_type) {
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

void on_connection(uv_stream_t *server, int status) {
    net_socket *listener = (net_socket *)server->data;
    if (!listener)
        return;

    if (status < 0) {
        if (listener->pending_accept &&
            !listener->pending_accept->done) {
            listener->pending_accept->status = status;
            listener->pending_accept->done = true;
        }
        return;
    }

    std::shared_ptr<net_socket> client = new_socket();
    if (!client) {
        if (listener->pending_accept &&
            !listener->pending_accept->done) {
            listener->pending_accept->status = UV_ENOMEM;
            listener->pending_accept->done = true;
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
        }
        return;
    }

    if (listener->pending_accept && !listener->pending_accept->done) {
        listener->pending_accept->client = client;
        listener->pending_accept->status = 0;
        listener->pending_accept->done = true;
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
    socket->read_done = true;
}

value_t *net_tcp_connect(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.net.tcp_connect expects 2 arguments");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("tcp_connect host must be a string");
        return NULL;
    }
    int port = 0;
    if (!get_port(args[1], port))
        return NULL;
    if (g_interrupted)
        return NULL;

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string service = service_string(port);
    const char *host = ((struct string_value_t *)args[0])->value;
    resolve_state resolved;
    if (!getaddrinfo_async(host, service.c_str(), &hints, resolved)) {
        if (!g_interrupted)
            yyerror("tcp_connect: %s", uv_strerror(resolved.status));
        return NULL;
    }

    int last_status = 0;
    for (struct addrinfo *address = resolved.addresses; address;
         address = address->ai_next) {
        if (g_interrupted)
            break;

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

        bool ok = net_wait(state.done, [&socket] {
            socket->close();
        });
        if (!ok) {
            uv_freeaddrinfo(resolved.addresses);
            return NULL;
        }
        if (state.status == 0) {
            uv_freeaddrinfo(resolved.addresses);
            return new_socket_value(socket);
        }
        last_status = state.status;
        socket->close();
    }

    uv_freeaddrinfo(resolved.addresses);
    if (!g_interrupted) {
        yyerror("tcp_connect: %s",
                uv_strerror(last_status ? last_status : UV_EINVAL));
    }
    return NULL;
}

value_t *net_tcp_listen(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.net.tcp_listen expects 2 arguments");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("tcp_listen host must be a string");
        return NULL;
    }
    int port = 0;
    if (!get_port(args[1], port))
        return NULL;
    if (g_interrupted)
        return NULL;

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    std::string service = service_string(port);
    const char *host = ((struct string_value_t *)args[0])->value;
    resolve_state resolved;
    if (!getaddrinfo_async(host[0] ? host : NULL, service.c_str(),
                           &hints, resolved)) {
        if (!g_interrupted)
            yyerror("tcp_listen: %s", uv_strerror(resolved.status));
        return NULL;
    }

    int last_status = 0;
    for (struct addrinfo *address = resolved.addresses; address;
         address = address->ai_next) {
        if (g_interrupted)
            break;

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
        uv_freeaddrinfo(resolved.addresses);
        return new_socket_value(socket);
    }

    uv_freeaddrinfo(resolved.addresses);
    if (!g_interrupted) {
        yyerror("tcp_listen: %s",
                uv_strerror(last_status ? last_status : UV_EINVAL));
    }
    return NULL;
}

value_t *net_accept(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.net.accept expects 1 argument");
        return NULL;
    }
    std::shared_ptr<net_socket> listener;
    if (!get_socket(args[0], listener))
        return NULL;
    if (!listener->listening) {
        yyerror("accept expects a listening tcp socket");
        return NULL;
    }

    if (g_interrupted)
        return NULL;

    if (!listener->pending_clients.empty()) {
        std::shared_ptr<net_socket> client =
            std::move(listener->pending_clients.front());
        listener->pending_clients.pop_front();
        return new_socket_value(std::move(client));
    }

    accept_state state;
    listener->pending_accept = &state;
    bool ok = net_wait(state.done, [&listener, &state] {
        listener->pending_accept = NULL;
        state.done = true;
    });
    listener->pending_accept = NULL;

    if (!ok) {
        if (state.client)
            listener->pending_clients.push_front(std::move(state.client));
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

value_t *net_send(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.net.send expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    if (socket->listening) {
        yyerror("send expects a connected tcp socket");
        return NULL;
    }

    std::string data;
    if (args[1]->type == T_string) {
        data = ((struct string_value_t *)args[1])->value;
    } else if (args[1]->type == T_bytes) {
        data = ((struct bytes_value_t *)args[1])->value;
    } else {
        yyerror("send data must be string or bytes");
        return NULL;
    }

    if (data.empty())
        return newintval(0);
    if (g_interrupted)
        return NULL;
    if (data.size() >
        (size_t)std::numeric_limits<unsigned int>::max()) {
        yyerror("send data is too large");
        return NULL;
    }

    write_state state;
    state.data = std::move(data);
    state.request.data = &state;
    uv_buf_t buffer = uv_buf_init(
        &state.data[0], (unsigned int)state.data.size());
    int rc = uv_write(&state.request, (uv_stream_t *)socket->handle,
                      &buffer, 1, on_write);
    if (rc < 0) {
        yyerror("send: %s", uv_strerror(rc));
        return NULL;
    }

    bool ok = net_wait(state.done, [&socket] {
        socket->close();
    });
    if (!ok)
        return NULL;
    if (state.status < 0) {
        yyerror("send: %s", uv_strerror(state.status));
        return NULL;
    }
    return newintval((long long)state.data.size());
}

value_t *net_recv(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.net.recv expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    if (socket->listening) {
        yyerror("recv expects a connected tcp socket");
        return NULL;
    }
    if (args[1]->type != T_int ||
        ((struct int_value_t *)args[1])->value < 0) {
        yyerror("recv size must be a non-negative integer");
        return NULL;
    }
    long long requested = ((struct int_value_t *)args[1])->value;
    if (requested == 0)
        return newbytesval(std::string());
    if (requested > std::numeric_limits<int>::max()) {
        yyerror("recv size is too large");
        return NULL;
    }
    if (socket->read_eof)
        return newbytesval(std::string());
    if (socket->read_failed) {
        yyerror("recv: %s", uv_strerror(socket->read_error));
        return NULL;
    }
    if (g_interrupted)
        return NULL;

    socket->read_done = false;
    socket->read_status = 0;
    socket->read_requested = (size_t)requested;
    socket->read_buffer.clear();
    socket->read_result.clear();
    socket->reading = true;

    int rc = uv_read_start((uv_stream_t *)socket->handle, on_alloc, on_read);
    if (rc < 0) {
        socket->reading = false;
        yyerror("recv: %s", uv_strerror(rc));
        return NULL;
    }

    bool ok = net_wait(socket->read_done, [&socket] {
        uv_read_stop((uv_stream_t *)socket->handle);
        socket->reading = false;
        socket->read_done = true;
    });
    socket->reading = false;
    if (!ok)
        return NULL;
    if (socket->read_status < 0) {
        yyerror("recv: %s", uv_strerror(socket->read_status));
        return NULL;
    }
    return newbytesval(std::move(socket->read_result));
}

value_t *net_close(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.net.close expects 1 argument");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    socket->pending_accept = NULL;
    socket->pending_clients.clear();
    socket->listening = false;
    socket->reading = false;
    socket->read_buffer.clear();
    socket->read_result.clear();
    socket->close();
    return newnull();
}

value_t *net_resolve(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.net.resolve expects 1 argument");
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("resolve host must be a string");
        return NULL;
    }
    if (g_interrupted)
        return NULL;

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    resolve_state resolved;
    if (!getaddrinfo_async(
            ((struct string_value_t *)args[0])->value, NULL,
            &hints, resolved)) {
        if (!g_interrupted)
            yyerror("resolve: %s", uv_strerror(resolved.status));
        return NULL;
    }

    struct list_value_t *list = newlistval();
    for (struct addrinfo *address = resolved.addresses; address;
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
    uv_freeaddrinfo(resolved.addresses);
    return (struct value_t *)list;
}

} // namespace

void net_shutdown() {
    net_runtime &runtime = get_runtime();
    if (!runtime.initialized)
        return;

    uv_timer_stop(&runtime.ticker);
    uv_walk(&runtime.loop, [](uv_handle_t *handle, void *) {
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
    if (!uv_is_closing((uv_handle_t *)&runtime.ticker))
        uv_close((uv_handle_t *)&runtime.ticker, on_timer_close);

    uv_run(&runtime.loop, UV_RUN_DEFAULT);
    uv_loop_close(&runtime.loop);
    runtime.initialized = false;
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
        default:
            yyerror("unknown network function");
            return NULL;
    }
}
