#ifndef SKEIN_CRYPTO_BUILTIN_H
#define SKEIN_CRYPTO_BUILTIN_H

#include "valuetype.h"

#include <vector>

struct value_t *call_crypto_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
