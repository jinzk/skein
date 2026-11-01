#ifndef CALC_RESULT_H
#define CALC_RESULT_H

#include "valuetype.h"

#include <vector>

struct value_t *call_result_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
