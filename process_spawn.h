#ifndef SKEIN_PROCESS_SPAWN_H
#define SKEIN_PROCESS_SPAWN_H

#include "valuetype.h"

#include <vector>

struct value_t *call_process_spawn_builtin(
    int functype, const std::vector<struct value_t *> &args);

void process_spawn_shutdown();

#endif
