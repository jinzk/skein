#ifndef CALC_TASK_H
#define CALC_TASK_H

#include "valuetype.h"

#include <vector>

struct value_t *call_task_builtin(
    int functype, const std::vector<struct value_t *> &args);

void task_shutdown();

#endif
