#ifndef CALC_NET_H
#define CALC_NET_H

#include "valuetype.h"

#include <vector>

struct value_t *call_net_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
