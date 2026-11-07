#ifndef SKEIN_AST_ANALYSIS_H
#define SKEIN_AST_ANALYSIS_H

#include <string>
#include <unordered_set>
#include <vector>

struct ast;

namespace skein {
namespace ast_analysis {

/* Collect names bound by a match/destructuring pattern. */
void collect_pattern_bindings(const struct ast *node,
                              std::vector<std::string> &names);

/* Collect every identifier referenced by an expression/statement tree. */
void collect_referenced_names(const struct ast *node,
                              std::unordered_set<std::string> &names);

} // namespace ast_analysis
} // namespace skein

#endif
