#ifndef SKEIN_AST_ANALYSIS_H
#define SKEIN_AST_ANALYSIS_H

#include <string>
#include <unordered_set>
#include <vector>

struct ast;

namespace skein {
namespace ast_analysis {

/* True for top-level statements that primarily declare symbols/types.
 * Analysis-only modes keep evaluating these so imports, type definitions and
 * packages can populate the symbol and type tables. */
bool ast_is_declaration(const struct ast *node);

/* Collect names bound by a match/destructuring pattern. */
void collect_pattern_bindings(const struct ast *node,
                              std::vector<std::string> &names);

/* Collect every identifier referenced by an expression/statement tree. */
void collect_referenced_names(const struct ast *node,
                              std::unordered_set<std::string> &names);

} // namespace ast_analysis
} // namespace skein

#endif
