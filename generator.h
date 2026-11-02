#ifndef SKEIN_GENERATOR_H
#define SKEIN_GENERATOR_H

#include "valuetype.h"

#include <vector>

struct value_t *call_generator_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
