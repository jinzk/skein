#include "ast_analysis.h"

#include "ast.h"

#include <cstring>

namespace skein {
namespace ast_analysis {

namespace {

void add_pattern_binding_name(std::vector<std::string> &names,
                              const char *name) {
    if (!name || !*name || std::strcmp(name, "_") == 0)
        return;
    for (const std::string &existing : names) {
        if (existing == name)
            return;
    }
    names.push_back(name);
}

} // namespace

void collect_pattern_bindings(const struct ast *node,
                              std::vector<std::string> &names) {
    if (!node)
        return;
    const struct pattern_node *pattern =
        (const struct pattern_node *)node;
    switch (pattern->nodetype) {
        case NODE_PATTERN_BINDING:
            add_pattern_binding_name(names, pattern->name);
            return;
        case NODE_PATTERN_REST:
            add_pattern_binding_name(names, pattern->name);
            return;
        case NODE_PATTERN_AS:
            collect_pattern_bindings(pattern->items, names);
            add_pattern_binding_name(names, pattern->name);
            return;
        case NODE_PATTERN_OR:
        case NODE_PATTERN_LIST:
            for (struct pattern_node *item =
                     (struct pattern_node *)pattern->items;
                 item; item = (struct pattern_node *)item->next) {
                collect_pattern_bindings((const struct ast *)item, names);
            }
            return;
        case NODE_PATTERN_CONTAINER:
            for (struct pattern_field *field = pattern->fields; field;
                 field = field->next) {
                if (field->pattern)
                    collect_pattern_bindings(field->pattern, names);
                else
                    add_pattern_binding_name(names, field->rest_name);
            }
            return;
        default:
            return;
    }
}

void collect_referenced_names(const struct ast *node,
                              std::unordered_set<std::string> &names) {
    if (!node)
        return;
    switch (node->nodetype) {
        case 'N': {
            struct symref *ref = (struct symref *)node;
            if (ref->name)
                names.insert(ref->name);
            return;
        }
        case 'D':
            collect_referenced_names(((struct symdef *)node)->v, names);
            return;
        case NODE_CAST:
            collect_referenced_names(
                ((struct cast_node *)node)->value, names);
            return;
        case 'Z': {
            struct funcdef *def = (struct funcdef *)node;
            for (struct symlist *sl = def->sl; sl; sl = sl->next) {
                collect_referenced_names(symlist_default_ast(sl), names);
                collect_referenced_names(symlist_pattern_ast(sl), names);
            }
            collect_referenced_names(def->ast, names);
            return;
        }
        case 'F': {
            struct forinloop *loop = (struct forinloop *)node;
            collect_referenced_names(loop->iterable, names);
            collect_referenced_names(loop->body, names);
            return;
        }
        case 'o': {
            struct on_error_node *handler = (struct on_error_node *)node;
            collect_referenced_names(handler->call, names);
            collect_referenced_names(handler->body, names);
            return;
        }
        case '7': {
            struct match_node *match = (struct match_node *)node;
            collect_referenced_names(match->expr, names);
            for (struct match_arm *arm =
                     (struct match_arm *)match->arms;
                 arm; arm = (struct match_arm *)arm->next) {
                collect_referenced_names(arm->guard, names);
                collect_referenced_names(arm->body, names);
            }
            return;
        }
        case NODE_DESTRUCTURE:
            collect_referenced_names(
                ((struct destructure_node *)node)->value, names);
            return;
        case 'J':
        case 'Y':
        case 'H':
        case 't':
        case '<':
        case '>':
        case 'K':
            return;
        case 'X':
            collect_referenced_names(((struct nscope *)node)->ast, names);
            return;
        case 'L':
            collect_referenced_names(node->l, names);
            collect_referenced_names(node->r, names);
            return;
        case 'I':
        case 'T': {
            struct flow *f = (struct flow *)node;
            collect_referenced_names(f->cond, names);
            collect_referenced_names(f->tl, names);
            collect_referenced_names(f->el, names);
            return;
        }
        case 'W': {
            struct flow *f = (struct flow *)node;
            collect_referenced_names(f->cond, names);
            collect_referenced_names(f->tl, names);
            return;
        }
        case 'E': {
            struct flow *f = (struct flow *)node;
            collect_referenced_names(f->tl, names);
            collect_referenced_names(f->cond, names);
            return;
        }
        case 'P': {
            struct forloop *f = (struct forloop *)node;
            collect_referenced_names(f->init, names);
            collect_referenced_names(f->cond, names);
            collect_referenced_names(f->step, names);
            collect_referenced_names(f->body, names);
            return;
        }
        case 'S': {
            struct switch_node *s = (struct switch_node *)node;
            collect_referenced_names(s->expr, names);
            for (struct case_node *c = (struct case_node *)s->cases; c;
                 c = (struct case_node *)c->next) {
                collect_referenced_names(c->values, names);
                collect_referenced_names(c->body, names);
            }
            return;
        }
        case 'R':
            collect_referenced_names(((struct fnret *)node)->v, names);
            return;
        case 'C': {
            struct ufncall *call = (struct ufncall *)node;
            collect_referenced_names(call->callee, names);
            collect_referenced_names(call->args, names);
            return;
        }
        case '=': {
            struct symasgn *asgn = (struct symasgn *)node;
            collect_referenced_names(asgn->target, names);
            collect_referenced_names(asgn->v, names);
            return;
        }
        case 'a': {
            struct compound_asgn *asgn = (struct compound_asgn *)node;
            collect_referenced_names(asgn->target, names);
            collect_referenced_names(asgn->v, names);
            return;
        }
        case 'U':
            collect_referenced_names(
                ((struct incdec_node *)node)->target, names);
            return;
        case 'u':
        case 'v':
        case 'p':
            collect_referenced_names(
                ((struct control_stmt *)node)->value, names);
            return;
        case 'b':
        case 'w':
            collect_referenced_names(node->l, names);
            return;
        case 'g':
            collect_referenced_names(
                ((struct label_node *)node)->stmt, names);
            return;
        case 'l': {
            for (struct list_node *item = (struct list_node *)node; item;
                 item = (struct list_node *)item->next)
                collect_referenced_names(item->value, names);
            return;
        }
        case 'd': {
            for (struct dict_node *entry = (struct dict_node *)node;
                 entry; entry = (struct dict_node *)entry->next) {
                collect_referenced_names(entry->key, names);
                collect_referenced_names(entry->value, names);
            }
            return;
        }
        case 'k':
            collect_referenced_names(
                ((struct is_node *)node)->value, names);
            return;
        case 'x':
            collect_referenced_names(
                ((struct spread_node *)node)->value, names);
            return;
        case 'y':
        case '?': {
            struct struct_init *init =
                node->nodetype == 'y'
                    ? ((struct struct_lit_node *)node)->fields
                    : ((struct enum_lit_node *)node)->fields;
            for (; init; init = init->next)
                collect_referenced_names(init->value, names);
            return;
        }
        case 'n':
            collect_referenced_names(node->l, names);
            return;
        case 'V':
        case 'B':
        case 'O':
        case '#':
            return;
        default:
            collect_referenced_names(node->l, names);
            collect_referenced_names(node->r, names);
            return;
    }
}

} // namespace ast_analysis
} // namespace skein
