#ifndef SKEIN_COMPRESS_BUILTIN_H
#define SKEIN_COMPRESS_BUILTIN_H

#include "valuetype.h"

#include <vector>

struct value_t *call_compress_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
