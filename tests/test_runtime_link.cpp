/*
 * Link-level guard: this executable links against skein_runtime only.
 * It must never depend on interpreter or builtins symbols.
 */
#include "value_ops.h"
#include "valuetype.h"

int main() {
    struct value_t *number = newintval(42);
    struct value_t *left = newstrval("skein");
    struct value_t *right = newstrval("skein");
    bool ok = number && left && right;
    ok = ok && is_numeric(number);
    ok = ok && value_equals(left, right);
    freeval(number);
    freeval(left);
    freeval(right);
    return ok ? 0 : 1;
}
