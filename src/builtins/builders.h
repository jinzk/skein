#ifndef SKEIN_BUILDERS_H
#define SKEIN_BUILDERS_H

#include <vector>

struct value_t;

/* std.list.builder / std.string.builder.  Builders provide amortized O(1)
 * growth without copying the accumulated values on every push. */
struct value_t *call_builder_builtin(
    int functype, const std::vector<struct value_t *> &args);

#endif
