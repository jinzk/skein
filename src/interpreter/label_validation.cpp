#include "compiler.h"

#include "ast.h"
#include "ast_analysis.h"
#include "diagnostics.h"
#include "label_analysis.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using skein::ast_analysis::AstWalkAction;

struct label_control_ref {
    bool is_break = false;
    std::string name;
    int line = 0;
    int column = 0;
};

struct enclosing_label_target {
    std::string name;
    skein::labels::TargetInfo target;
};

struct label_check_state {
    bool is_generator = false;
    bool duplicate = false;
    bool goto_in_generator = false;
    int error_line = 0;
    int error_column = 0;
    std::string error;
    std::unordered_map<std::string, skein::labels::LabelInfo> labels;
    std::vector<enclosing_label_target> enclosing_targets;
    std::vector<label_control_ref> unresolved_controls;
    std::unordered_set<std::string> used;
};

static bool is_function_boundary(const struct ast *node) {
    switch (node->nodetype) {
        case 'Z':
        case 'Y':
        case 'H':
        case 'K':
        case 't':
        case NODE_ENUM_DECL:
        case NODE_ENUM_VARIANT:
        case 'J':
            return true;
        default:
            return false;
    }
}

static AstWalkAction enter_label_node(label_check_state &state,
                                      const struct ast *node) {
    switch (node->nodetype) {
        case 'g': {
            struct label_node *label = (struct label_node *)node;
            std::string name = label->name ? label->name : "";
            if (name.empty())
                return AstWalkAction::Continue;

            skein::labels::LabelInfo info =
                skein::labels::label_info(node);
            if (state.labels.count(name)) {
                if (!state.duplicate) {
                    state.duplicate = true;
                    state.error = "duplicate label ";
                    state.error += name;
                    state.error_line = info.line > 0 ? info.line : 1;
                    state.error_column =
                        info.column > 0 ? info.column : 1;
                }
            } else {
                state.labels[name] = info;
            }
            state.enclosing_targets.push_back(
                enclosing_label_target{name, info.target});
            return AstWalkAction::Continue;
        }
        case NODE_GOTO: {
            struct goto_node *jump = (struct goto_node *)node;
            std::string name = jump->name ? jump->name : "";
            if (!name.empty())
                state.used.insert(name);
            if (state.is_generator && !state.goto_in_generator) {
                state.goto_in_generator = true;
                state.error = "goto is not allowed in generator functions";
                state.error_line = node->line > 0 ? node->line : 1;
                state.error_column =
                    node->column > 0 ? node->column : 1;
            }
            return AstWalkAction::Continue;
        }
        case 'B':
        case 'O': {
            bool is_break = node->nodetype == 'B';
            const char *label =
                is_break ? ((struct nbreak *)node)->label
                         : ((struct ncontinue *)node)->label;
            if (!label || !*label)
                return AstWalkAction::Continue;

            state.used.insert(label);
            bool found = false;
            for (size_t i = state.enclosing_targets.size(); i > 0; --i) {
                const enclosing_label_target &target =
                    state.enclosing_targets[i - 1];
                if (target.name == label &&
                    skein::labels::accepts(target.target, is_break)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                state.unresolved_controls.push_back(label_control_ref{
                    is_break, label, node->line, node->column});
            }
            return AstWalkAction::Continue;
        }
        default:
            if (is_function_boundary(node))
                return AstWalkAction::SkipChildren;
            return AstWalkAction::Continue;
    }
}

static void leave_label_node(label_check_state &state,
                             const struct ast *node) {
    if (node->nodetype != 'g')
        return;
    const struct label_node *label = (const struct label_node *)node;
    if (label->name && *label->name && !state.enclosing_targets.empty())
        state.enclosing_targets.pop_back();
}

} // namespace

void skein_check_function_labels(struct function_code *code,
                                 struct ast *body) {
    if (!code || !body)
        return;

    label_check_state state;
    state.is_generator = code->is_generator;

    auto enter = [&](const struct ast *node) {
        return enter_label_node(state, node);
    };
    auto leave = [&](const struct ast *node) {
        leave_label_node(state, node);
    };
    skein::ast_analysis::walk_ast(body, enter, leave);

    if (state.duplicate || state.goto_in_generator) {
        diag_set_location(state.error_line, state.error_column);
        yyerror("%s", state.error.c_str());
        return;
    }

    if (!state.unresolved_controls.empty()) {
        const label_control_ref &ref = state.unresolved_controls.front();
        bool exists = state.labels.contains(ref.name);
        std::string message =
            skein::labels::target_error(ref.is_break, ref.name, exists);
        diag_set_location(ref.line, ref.column);
        yyerror("%s", message.c_str());
        return;
    }

    std::vector<std::string> unused;
    for (const auto &entry : state.labels) {
        if (!state.used.contains(entry.first))
            unused.push_back(entry.first);
    }
    std::sort(unused.begin(), unused.end());
    for (const std::string &name : unused) {
        const auto &loc = state.labels[name];
        std::string message = "label " + name + " is never used";
        diag_report_warning(loc.line, loc.column, "warning",
                            message.c_str());
    }
}
