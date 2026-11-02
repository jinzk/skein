#ifndef SKEIN_TLS_H
#define SKEIN_TLS_H

#include "valuetype.h"

#include <vector>

struct value_t *call_tls_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
