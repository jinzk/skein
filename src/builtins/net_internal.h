#ifndef SKEIN_BUILTINS_NET_INTERNAL_H
#define SKEIN_BUILTINS_NET_INTERNAL_H

#include <cstddef>
#include <vector>

#include "builtin_ids.h"
#include "builtins_io.h"
#include "valuetype.h"

namespace skein {
namespace builtins_internal {

/* Release every value in an argument vector and empty it.  Kept in an
 * internal namespace so it does not collide with the interpreter's own
 * free_args helpers. */
inline void free_args(std::vector<value_t *> &args) {
    for (std::size_t i = 0; i < args.size(); ++i)
        freeval(args[i]);
    args.clear();
}

/* Close a socket value through the io builtin.  The caller keeps ownership of
 * the socket itself; only the duplicated reference used for the call is
 * released. */
inline void close_socket(value_t *socket) {
    if (!socket)
        return;
    std::vector<value_t *> args;
    args.push_back(dupval(socket));
    value_t *result = call_io_builtin(B_io_close, args);
    free_args(args);
    freeval(result);
}

} // namespace builtins_internal
} // namespace skein

#endif
