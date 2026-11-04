#ifndef SKEIN_TIME_BUILTIN_H
#define SKEIN_TIME_BUILTIN_H

#include "valuetype.h"

#include <vector>

struct value_t *call_time_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
