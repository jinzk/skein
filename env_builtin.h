#ifndef SKEIN_ENV_BUILTIN_H
#define SKEIN_ENV_BUILTIN_H

#include "valuetype.h"

#include <vector>

struct value_t *call_env_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
