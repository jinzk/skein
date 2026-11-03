#ifndef SKEIN_PROCESS_BUILTIN_H
#define SKEIN_PROCESS_BUILTIN_H

#include "valuetype.h"

#include <vector>

struct value_t *call_process_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
