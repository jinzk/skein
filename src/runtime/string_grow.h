#ifndef SKEIN_STRING_GROW_H
#define SKEIN_STRING_GROW_H

#include <cstddef>

struct value_t;

/* Append `rhs` (string or char) to a string value.
 *
 * `extra_owners` is the number of additional live references the caller knows
 * about (for the VM the receiver is also on the stack, so 1).  Anything above
 * that means an alias exists and the buffer is copied first. */
bool string_append_in_place(struct value_t *target,
                            const struct value_t *rhs,
                            size_t extra_owners = 0);

/* Keep a cached `value` pointer in sync when `old_value` and `new_value`
 * share the same string buffer (used by assignment after in-place growth). */
void string_refresh_cache(struct value_t *old_value,
                          const struct value_t *new_value);

#endif
