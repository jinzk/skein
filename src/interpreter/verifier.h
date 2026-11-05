#ifndef SKEIN_VERIFIER_H
#define SKEIN_VERIFIER_H

#include "bytecode.h"

#include <cstdint>
#include <string>

namespace skein {
namespace bc {

/*
 * Validate a single Proto before execution.
 *
 * Returns false and fills `error` with a stable diagnostic when the Proto is
 * malformed.  `max_stack` receives the maximum operand-stack depth reached by
 * the reachable control-flow graph.
 */
bool verify(const Proto &proto, std::string &error,
            uint32_t *max_stack = nullptr);

/* Verify a Proto and all Protos reachable through packages/defer/patterns. */
bool verify_tree(const Proto &proto, std::string &error);

} // namespace bc
} // namespace skein

#endif
