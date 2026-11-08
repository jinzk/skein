/*
 * Redirect C++ new/delete to mimalloc for the interpreter executable.
 *
 * mimalloc-new-delete.h must be included in exactly one translation unit.
 * The C allocator (malloc/free) is not redirected by this file; Skein's hot
 * paths use new/delete for value objects and standard containers.
 */
#include <mimalloc-new-delete.h>
