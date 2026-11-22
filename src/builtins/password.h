#ifndef SKEIN_PASSWORD_H
#define SKEIN_PASSWORD_H

#include "valuetype.h"

#include <vector>

struct value_t *call_password_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
