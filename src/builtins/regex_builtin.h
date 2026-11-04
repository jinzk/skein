#ifndef SKEIN_REGEX_BUILTIN_H
#define SKEIN_REGEX_BUILTIN_H

#include "valuetype.h"

#include <vector>

struct value_t *call_regex_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
