#ifndef _BUILTINS_H_
#define _BUILTINS_H_

#include "valuetype.h"

#include <vector>

struct value_t *call_builtin(int functype,
                             const std::vector<struct value_t *> &args);

#endif
