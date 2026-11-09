#ifndef SKEIN_NET_H
#define SKEIN_NET_H

#include "valuetype.h"

#include <string>
#include <vector>

struct value_t *call_net_builtin(
    int functype, const std::vector<struct value_t *> &args);

/* Peek up to `size` bytes without consuming them.  Returns false on EOF or
 * when no data becomes available before `timeout_ms`. */
bool net_peek(struct value_t *socket, std::string &data, std::size_t size,
              long long timeout_ms);

void net_shutdown();

#endif
