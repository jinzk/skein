#ifndef CALC_IO_H
#define CALC_IO_H

#include "valuetype.h"

#include <vector>

struct value_t *call_io_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
