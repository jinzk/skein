#ifndef SKEIN_RANDOM_BUILTIN_H
#define SKEIN_RANDOM_BUILTIN_H

#include "valuetype.h"

#include <vector>

struct value_t *call_random_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
