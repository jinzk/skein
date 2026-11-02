#ifndef SKEIN_HTTP2_H
#define SKEIN_HTTP2_H

#include "valuetype.h"

#include <vector>

struct value_t *call_http2_builtin(
    int functype, const std::vector<struct value_t *> &args);

void http2_shutdown();

#endif
