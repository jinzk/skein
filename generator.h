#ifndef CALC_GENERATOR_H
#define CALC_GENERATOR_H

#include "valuetype.h"

#include <vector>

struct value_t *call_generator_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
