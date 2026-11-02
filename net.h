#ifndef SKEIN_NET_H
#define SKEIN_NET_H

#include "valuetype.h"

#include <vector>

struct value_t *call_net_builtin(
    int functype, const std::vector<struct value_t *> &args);

void net_shutdown();

#endif
