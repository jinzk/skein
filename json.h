#ifndef SKEIN_JSON_H
#define SKEIN_JSON_H

#include "valuetype.h"

#include <vector>

struct value_t *call_json_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
