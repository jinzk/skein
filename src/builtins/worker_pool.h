#ifndef SKEIN_WORKER_POOL_H
#define SKEIN_WORKER_POOL_H

#include "valuetype.h"

#include <vector>

struct value_t *call_worker_builtin(
    int functype, const std::vector<struct value_t *> &args);

/* Kill all worker processes but keep the pool value usable for cleanup. */
void worker_pool_force_shutdown(struct value_t *pool);

#endif
