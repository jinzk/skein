#ifndef SKEIN_ITER_H
#define SKEIN_ITER_H

#include <vector>

struct value_t;

struct value_t *call_iter_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
