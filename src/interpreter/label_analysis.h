#ifndef SKEIN_LABEL_ANALYSIS_H
#define SKEIN_LABEL_ANALYSIS_H

#include <string>

struct ast;

namespace skein {
namespace labels {

/* Control-flow capabilities of a label target.  A loop accepts both break
 * and continue; a switch accepts only break. */
struct TargetInfo {
    bool breakable = false;
    bool continuable = false;
};

struct LabelInfo {
    int line = 0;
    int column = 0;
    TargetInfo target;
};

/* Capabilities of the statement decorated by a label, unwrapping nested
 * labels such as `a: b: for ...`. */
TargetInfo target_info(const struct ast *stmt);

/* Line/column and target capabilities of a `'g'` label node. */
LabelInfo label_info(const struct ast *label_node);

bool accepts(const TargetInfo &target, bool is_break);

/* Shared diagnostics for break/continue targets.  `exists` distinguishes a
 * label that is present but not enclosing from one that is unknown. */
std::string target_error(bool is_break, const std::string &name, bool exists);

} // namespace labels
} // namespace skein

#endif
