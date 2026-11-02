#ifndef CALC_HTTP_H
#define CALC_HTTP_H

#include "valuetype.h"

#include <vector>

struct value_t *call_http_builtin(
    int functype, const std::vector<struct value_t *> &args);

void http_shutdown();

#endif
