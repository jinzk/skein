#ifndef CALC_WEBSOCKET_H
#define CALC_WEBSOCKET_H

#include "valuetype.h"

#include <vector>

struct value_t *call_websocket_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
