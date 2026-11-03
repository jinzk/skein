#ifndef SKEIN_CYCLE_H
#define SKEIN_CYCLE_H

#include "valuetype.h"

#include <unordered_set>
#include <vector>

/* Identify the shared payload of a container-like value. */
const void *value_payload(const struct value_t *v);

/* Whether `v` contains `target` through any nested container payload. */
bool value_reaches(const struct value_t *v, const void *target,
                   std::unordered_set<const void *> &visited);

/* Whether `v` reaches any of `targets`. */
bool value_reaches_any(const struct value_t *v,
                       const std::vector<const void *> &targets);

#endif
