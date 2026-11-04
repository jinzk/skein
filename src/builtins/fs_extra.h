#ifndef SKEIN_FS_EXTRA_H
#define SKEIN_FS_EXTRA_H

#include "valuetype.h"

#include <vector>

struct value_t *call_fs_extra_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
