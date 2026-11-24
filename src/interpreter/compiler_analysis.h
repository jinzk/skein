#ifndef SKEIN_COMPILER_ANALYSIS_H
#define SKEIN_COMPILER_ANALYSIS_H

#include <string>
#include <unordered_set>

struct ast;

namespace skein {
namespace compiler_analysis {

/* True when the statement itself introduces a local binding. */
bool statement_declares_local(const struct ast *node);

/* Conservative checks used to choose the compiler's slot fast path. */
bool has_local_declarations(const struct ast *node);
bool can_use_slots(const struct ast *node);

/* Names introduced by the current block/function scope, excluding nested
 * function bodies.  Patterns and imports are included. */
void collect_local_declared_names(const struct ast *node,
                                  std::unordered_set<std::string> &names);

} // namespace compiler_analysis
} // namespace skein

#endif
