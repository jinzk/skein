#ifndef SKEIN_ENCODING_H
#define SKEIN_ENCODING_H

#include "valuetype.h"

#include <vector>

struct value_t *call_encoding_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
