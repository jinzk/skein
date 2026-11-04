#ifndef _BUILTINS_H_
#define _BUILTINS_H_

#include "valuetype.h"

#include <string>
#include <vector>

struct value_t *call_builtin(int functype,
                             const std::vector<struct value_t *> &args);

/* Register static native modules.  Returns false and fills `error` when a
 * registration is invalid. */
bool builtins_register_modules(std::string &error);

#endif
