#include "label_analysis.h"

#include "ast.h"

namespace skein {
namespace labels {

TargetInfo target_info(const struct ast *stmt) {
    while (stmt && stmt->nodetype == 'g')
        stmt = ((const struct label_node *)stmt)->stmt;
    TargetInfo info;
    if (!stmt)
        return info;
    switch (stmt->nodetype) {
        case 'F': /* for-in */
        case 'P': /* C-style for */
        case 'W': /* while */
        case 'E': /* do-while */
            info.breakable = true;
            info.continuable = true;
            break;
        case 'S': /* switch */
            info.breakable = true;
            break;
        default:
            break;
    }
    return info;
}

LabelInfo label_info(const struct ast *label_node) {
    LabelInfo info;
    if (!label_node)
        return info;
    info.line = label_node->line;
    info.column = label_node->column;
    if (label_node->nodetype == 'g')
        info.target =
            target_info(((const struct label_node *)label_node)->stmt);
    return info;
}

bool accepts(const TargetInfo &target, bool is_break) {
    return is_break ? target.breakable : target.continuable;
}

std::string target_error(bool is_break, const std::string &name,
                         bool exists) {
    if (!exists) {
        return std::string(is_break ? "break" : "continue") + " target " +
               name + " not found in this function";
    }
    if (is_break)
        return "break target " + name +
               " is not an enclosing loop or switch";
    return "continue target " + name + " is not an enclosing loop";
}

} // namespace labels
} // namespace skein
