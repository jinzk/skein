#ifndef SKEIN_ERROR_H
#define SKEIN_ERROR_H

#include "valuetype.h"

#include <vector>

struct value_t *call_error_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
