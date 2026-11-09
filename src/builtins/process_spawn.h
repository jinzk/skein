#ifndef SKEIN_PROCESS_SPAWN_H
#define SKEIN_PROCESS_SPAWN_H

#include "valuetype.h"

#include <vector>

struct value_t *call_process_spawn_builtin(
    int functype, const std::vector<struct value_t *> &args);

/* Poll a process without waiting or killing it. Returns true when it exited. */
bool process_try_wait(struct value_t *process, long long &status);

void process_spawn_shutdown();
void process_spawn_finish();

#endif
