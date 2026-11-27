#ifndef SKEIN_BUILTINS_ORDERING_H
#define SKEIN_BUILTINS_ORDERING_H

#include <vector>

struct value_t;

/* True when the value has a structural std.ops.Comparable op_cmp method. */
bool value_has_op_cmp(const struct value_t *value);

/* True for a non-empty set of values that all expose op_cmp. */
bool values_have_op_cmp(const std::vector<struct value_t *> &values);

/* Call left.op_cmp(right) and return its sign in `order`. */
bool compare_with_op_cmp(const struct value_t *left,
                         const struct value_t *right, long long &order,
                         const char *name);

#endif
