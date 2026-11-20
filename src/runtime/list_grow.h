#ifndef SKEIN_LIST_GROW_H
#define SKEIN_LIST_GROW_H

#include <cstdint>

struct value_t;

/* Copy-on-write growth used by LIST_APPEND_SELF / LIST_CONCAT_SELF.
 *
 * The caller must hold one reference to the target list; the second reference
 * allowed by the fast path is the target variable.  On success the arguments
 * (and, for concat, the right list's items) are moved into the target and the
 * function returns true.  On false nothing is modified and the caller keeps
 * ownership of the operands. */
bool list_grow_append(struct value_t *list, struct value_t **args,
                      uint32_t argc);
bool list_grow_concat(struct value_t *left, struct value_t *right);

#endif
