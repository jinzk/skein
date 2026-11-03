#ifndef SKEIN_UNICODE_H
#define SKEIN_UNICODE_H

#include "valuetype.h"

#include <vector>

struct value_t *call_unicode_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
