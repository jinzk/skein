#ifndef CALC_JSON_H
#define CALC_JSON_H

#include "valuetype.h"

#include <vector>

struct value_t *call_json_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
