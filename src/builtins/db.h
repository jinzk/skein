#ifndef SKEIN_DB_H
#define SKEIN_DB_H

#include "valuetype.h"

#include <vector>

struct value_t *call_db_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
