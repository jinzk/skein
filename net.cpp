#include "net.h"

#include "ast.h"
#include "repl.h"
#include "valuetype.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
using net_fd = SOCKET;
static const net_fd NET_INVALID = INVALID_SOCKET;
#else
using net_fd = int;
static const net_fd NET_INVALID = -1;
#endif

namespace {

struct net_socket {
    net_fd fd = NET_INVALID;
    bool closed = false;

    ~net_socket() {
        if (!closed && fd != NET_INVALID) {
#ifdef _WIN32
            closesocket(fd);
#else
            ::close(fd);
#endif
        }
    }
};

std::string net_describe(const void *data) {
    const net_socket *socket = (const net_socket *)data;
    if (!socket || socket->closed)
        return "<tcp_socket closed>";
    return "<tcp_socket>";
}

const struct native_type net_socket_type = {
    "tcp_socket", net_describe
};

#ifdef _WIN32
void ensure_winsock() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

std::string net_error_string() {
    return "winsock error " + std::to_string(WSAGetLastError());
}

void net_close_socket(net_fd fd) {
    if (fd != NET_INVALID)
        closesocket(fd);
}

bool net_interrupted() {
    return WSAGetLastError() == WSAEINTR;
}
#else
void ensure_winsock() {}

std::string net_error_string() {
    return std::strerror(errno);
}

void net_close_socket(net_fd fd) {
    if (fd != NET_INVALID)
        ::close(fd);
}

bool net_interrupted() {
    return errno == EINTR;
}
#endif

struct value_t *new_socket_value(net_fd fd) {
    std::shared_ptr<net_socket> socket = std::make_shared<net_socket>();
    socket->fd = fd;
    return newuserdataval(&net_socket_type, socket);
}

bool get_socket(struct value_t *value, std::shared_ptr<net_socket> &socket) {
    if (!value || value->type != T_userdata) {
        yyerror("expected a tcp socket, got %s", typename_of(value));
        return false;
    }
    struct userdata_value_t *userdata =
        (struct userdata_value_t *)value;
    if (userdata->meta != &net_socket_type) {
        yyerror("expected a tcp socket, got %s", typename_of(value));
        return false;
    }
    socket = std::static_pointer_cast<net_socket>(userdata->data);
    if (!socket || socket->closed || socket->fd == NET_INVALID) {
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

    ensure_winsock();
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    std::string service = service_string(port);
    int rc = getaddrinfo(((struct string_value_t *)args[0])->value,
                         service.c_str(), &hints, &addresses);
    if (rc != 0) {
        yyerror("tcp_connect: cannot resolve host");
        return NULL;
    }

    net_fd fd = NET_INVALID;
    for (struct addrinfo *address = addresses; address;
         address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd == NET_INVALID)
            continue;
        if (connect(fd, address->ai_addr, (int)address->ai_addrlen) == 0)
            break;
        net_close_socket(fd);
        fd = NET_INVALID;
    }
    freeaddrinfo(addresses);
    if (fd == NET_INVALID) {
        yyerror("tcp_connect: %s", net_error_string().c_str());
        return NULL;
    }
    return new_socket_value(fd);
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

    ensure_winsock();
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo *addresses = NULL;
    std::string service = service_string(port);
    const char *host = ((struct string_value_t *)args[0])->value;
    int rc = getaddrinfo(host[0] ? host : NULL, service.c_str(), &hints,
                         &addresses);
    if (rc != 0) {
        yyerror("tcp_listen: cannot resolve bind address");
        return NULL;
    }

    net_fd fd = NET_INVALID;
    for (struct addrinfo *address = addresses; address;
         address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd == NET_INVALID)
            continue;
        int enabled = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&enabled,
                   sizeof enabled);
        if (bind(fd, address->ai_addr, (int)address->ai_addrlen) == 0 &&
            listen(fd, 16) == 0)
            break;
        net_close_socket(fd);
        fd = NET_INVALID;
    }
    freeaddrinfo(addresses);
    if (fd == NET_INVALID) {
        yyerror("tcp_listen: %s", net_error_string().c_str());
        return NULL;
    }
    return new_socket_value(fd);
}

value_t *net_accept(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.net.accept expects 1 argument");
        return NULL;
    }
    std::shared_ptr<net_socket> listener;
    if (!get_socket(args[0], listener))
        return NULL;

    for (;;) {
        if (g_interrupted)
            return NULL;
        struct sockaddr_storage address = {};
        socklen_t length = sizeof address;
        net_fd fd = accept(listener->fd, (struct sockaddr *)&address, &length);
        if (fd != NET_INVALID)
            return new_socket_value(fd);
        if (net_interrupted())
            continue;
        yyerror("accept: %s", net_error_string().c_str());
        return NULL;
    }
}

value_t *net_send(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.net.send expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;

    const char *data = NULL;
    size_t length = 0;
    if (args[1]->type == T_string) {
        data = ((struct string_value_t *)args[1])->value;
        length = std::strlen(data);
    } else if (args[1]->type == T_bytes) {
        const std::string &bytes = ((struct bytes_value_t *)args[1])->value;
        data = bytes.data();
        length = bytes.size();
    } else {
        yyerror("send data must be string or bytes");
        return NULL;
    }

    size_t sent = 0;
    while (sent < length) {
        if (g_interrupted)
            break;
        size_t remaining = length - sent;
        int chunk = (int)std::min(
            remaining,
            (size_t)std::numeric_limits<int>::max());
        int n = send(socket->fd, data + sent, chunk, 0);
        if (n < 0) {
            if (net_interrupted())
                continue;
            yyerror("send: %s", net_error_string().c_str());
            return NULL;
        }
        if (n == 0)
            break;
        sent += (size_t)n;
    }
    return newintval((long long)sent);
}

value_t *net_recv(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.net.recv expects 2 arguments");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
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

    std::string buffer((size_t)requested, '\0');
    for (;;) {
        if (g_interrupted)
            return NULL;
        int n = recv(socket->fd, &buffer[0], (int)requested, 0);
        if (n < 0) {
            if (net_interrupted())
                continue;
            yyerror("recv: %s", net_error_string().c_str());
            return NULL;
        }
        buffer.resize((size_t)n);
        return newbytesval(std::move(buffer));
    }
}

value_t *net_close(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.net.close expects 1 argument");
        return NULL;
    }
    std::shared_ptr<net_socket> socket;
    if (!get_socket(args[0], socket))
        return NULL;
    net_close_socket(socket->fd);
    socket->fd = NET_INVALID;
    socket->closed = true;
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
    ensure_winsock();
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    int rc = getaddrinfo(((struct string_value_t *)args[0])->value, NULL,
                         &hints, &addresses);
    if (rc != 0) {
        yyerror("resolve: cannot resolve host");
        return NULL;
    }
    struct list_value_t *list = newlistval();
    for (struct addrinfo *address = addresses; address;
         address = address->ai_next) {
        char host[NI_MAXHOST];
        if (getnameinfo(address->ai_addr, (socklen_t)address->ai_addrlen,
                        host, sizeof host, NULL, 0, NI_NUMERICHOST) == 0) {
            bool duplicate = false;
            for (size_t i = 0; i < list->payload->items.size(); ++i)
                if (std::strcmp(
                        ((struct string_value_t *)
                             list->payload->items[i])->value,
                        host) == 0) {
                    duplicate = true;
                    break;
                }
            if (!duplicate)
                list->payload->items.push_back(newstrval(host));
        }
    }
    freeaddrinfo(addresses);
    return (struct value_t *)list;
}

} // namespace

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
