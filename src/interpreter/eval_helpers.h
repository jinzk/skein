#ifndef _EVAL_HELPERS_H_
#define _EVAL_HELPERS_H_

#include "value_ops.h"
#include "valuetype.h"

#include <cstddef>
#include <string>
#include <vector>

struct value_t *new_int_checked(__int128 n);

struct value_t *call_value_args(struct value_t *callee,
                                std::vector<struct value_t *> args);

#endif
