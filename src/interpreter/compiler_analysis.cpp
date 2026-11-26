#include "compiler_analysis.h"

#include "ast.h"
#include "ast_analysis.h"
#include "value_runtime.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace skein {
namespace compiler_analysis {

bool statement_declares_local(const struct ast *node) {
    if (!node)
        return false;
    if (node->nodetype == 'g')
        return statement_declares_local(
            ((const struct label_node *)node)->stmt);
    return node->nodetype == 'D' || node->nodetype == NODE_DESTRUCTURE;
}

bool has_local_declarations(const struct ast *node) {
    if (!node)
        return false;
    switch (node->nodetype) {
        case 'D':
        case 'F':
        case 'o':
        case '7':
        case 'J':
        case NODE_DESTRUCTURE:
            return true;
        case 'Z': {
            struct funcdef *def = (struct funcdef *)node;
            return def->name != NULL;
        }
        case 'Y':
        case 'H':
        case 't':
        case '<':
        case '>':
        case 'K':
        case 'n':
        case 'V':
        case 'N':
        case 'B':
        case 'O':
        case '#':
            return false;
        case 'X':
            return has_local_declarations(
                ((struct nscope *)node)->ast);
        case 'L':
            return has_local_declarations(node->l) ||
                   has_local_declarations(node->r);
        case 'I':
        case 'T': {
            struct flow *f = (struct flow *)node;
            return has_local_declarations(f->cond) ||
                   has_local_declarations(f->tl) ||
                   has_local_declarations(f->el);
        }
        case 'W': {
            struct flow *f = (struct flow *)node;
            return has_local_declarations(f->cond) ||
                   has_local_declarations(f->tl);
        }
        case 'E': {
            struct flow *f = (struct flow *)node;
            return has_local_declarations(f->tl) ||
                   has_local_declarations(f->cond);
        }
        case 'P': {
            struct forloop *f = (struct forloop *)node;
            return has_local_declarations(f->init) ||
                   has_local_declarations(f->cond) ||
                   has_local_declarations(f->step) ||
                   has_local_declarations(f->body);
        }
        case 'S': {
            struct switch_node *s = (struct switch_node *)node;
            if (has_local_declarations(s->expr))
                return true;
            for (struct case_node *c = (struct case_node *)s->cases; c;
                 c = (struct case_node *)c->next) {
                if (has_local_declarations(c->values) ||
                    has_local_declarations(c->body))
                    return true;
            }
            return false;
        }
        case 'R':
            return has_local_declarations(((struct fnret *)node)->v);
        case 'C': {
            struct ufncall *call = (struct ufncall *)node;
            return has_local_declarations(call->callee) ||
                   has_local_declarations(call->args);
        }
        case '=': {
            struct symasgn *asgn = (struct symasgn *)node;
            return has_local_declarations(asgn->target) ||
                   has_local_declarations(asgn->v);
        }
        case 'a': {
            struct compound_asgn *asgn = (struct compound_asgn *)node;
            return has_local_declarations(asgn->target) ||
                   has_local_declarations(asgn->v);
        }
        case 'U':
            return has_local_declarations(
                ((struct incdec_node *)node)->target);
        case 'u':
        case 'v':
        case 'p':
            return has_local_declarations(
                ((struct control_stmt *)node)->value);
        case 'b':
        case 'w':
            return has_local_declarations(node->l);
        case 'g':
            return has_local_declarations(
                ((struct label_node *)node)->stmt);
        case NODE_FUNCTION_BODY:
            return has_local_declarations(node->l);
        case NODE_GOTO:
            return false;
        case 'l':
    case NODE_TUPLE: {
            for (struct list_node *item = (struct list_node *)node; item;
                 item = (struct list_node *)item->next) {
                if (has_local_declarations(item->value))
                    return true;
            }
            return false;
        }
        case 'd': {
            for (struct dict_node *entry = (struct dict_node *)node;
                 entry; entry = (struct dict_node *)entry->next) {
                if (has_local_declarations(entry->key) ||
                    has_local_declarations(entry->value))
                    return true;
            }
            return false;
        }
        case 'k':
            return has_local_declarations(
                ((struct is_node *)node)->value);
        case NODE_CAST:
            return has_local_declarations(
                ((struct cast_node *)node)->value);
        case 'x':
            return has_local_declarations(
                ((struct spread_node *)node)->value);
        case 'y':
        case '?': {
            struct struct_init *init =
                node->nodetype == 'y'
                    ? ((struct struct_lit_node *)node)->fields
                    : ((struct enum_lit_node *)node)->fields;
            for (; init; init = init->next) {
                if (has_local_declarations(init->value))
                    return true;
            }
            return false;
        }
        default:
            return has_local_declarations(node->l) ||
                   has_local_declarations(node->r);
    }
}

bool can_use_slots(const struct ast *node) {
    if (!node)
        return true;
    switch (node->nodetype) {
        case 'Z': // nested function body is compiled separately
        case 'Y': // struct methods are compiled separately
        case 'H': // impl methods are compiled separately
        case 't': // protocol declaration has no body
        case '<': // enum declaration has no body
        case 'n': // defer expression is compiled separately
            return true;
        case 'o': {
            struct on_error_node *handler =
                (struct on_error_node *)node;
            return can_use_slots(handler->call) &&
                   can_use_slots(handler->body);
        }
        case '7': {
            struct match_node *match = (struct match_node *)node;
            if (!can_use_slots(match->expr))
                return false;
            for (struct match_arm *arm =
                     (struct match_arm *)match->arms;
                 arm; arm = (struct match_arm *)arm->next) {
                if (!can_use_slots(arm->guard) ||
                    !can_use_slots(arm->body))
                    return false;
            }
            return true;
        }
        case NODE_DESTRUCTURE:
            return can_use_slots(
                ((struct destructure_node *)node)->value);
        case 'J': // import binding
        case 'K': // package body is a separate compilation unit
            return true;
        case 'F':
            return true;
        case 'D':
            return can_use_slots(((struct symdef *)node)->v);
        case 'X':
            return can_use_slots(((struct nscope *)node)->ast);
        case 'L':
            return can_use_slots(node->l) &&
                   can_use_slots(node->r);
        case 'I':
        case 'T': {
            struct flow *f = (struct flow *)node;
            return can_use_slots(f->cond) &&
                   can_use_slots(f->tl) &&
                   can_use_slots(f->el);
        }
        case 'W': {
            struct flow *f = (struct flow *)node;
            return can_use_slots(f->cond) &&
                   can_use_slots(f->tl);
        }
        case 'E': {
            struct flow *f = (struct flow *)node;
            return can_use_slots(f->tl) &&
                   can_use_slots(f->cond);
        }
        case 'P': {
            struct forloop *f = (struct forloop *)node;
            return can_use_slots(f->init) &&
                   can_use_slots(f->cond) &&
                   can_use_slots(f->step) &&
                   can_use_slots(f->body);
        }
        case 'S': {
            struct switch_node *s = (struct switch_node *)node;
            if (!can_use_slots(s->expr))
                return false;
            for (struct case_node *c = (struct case_node *)s->cases; c;
                 c = (struct case_node *)c->next) {
                if (!can_use_slots(c->values) ||
                    !can_use_slots(c->body))
                    return false;
            }
            return true;
        }
        case 'R':
            return can_use_slots(((struct fnret *)node)->v);
        case 'C': {
            struct ufncall *call = (struct ufncall *)node;
            return can_use_slots(call->callee) &&
                   can_use_slots(call->args);
        }
        case '=': {
            struct symasgn *asgn = (struct symasgn *)node;
            return can_use_slots(asgn->target) &&
                   can_use_slots(asgn->v);
        }
        case 'a': {
            struct compound_asgn *asgn = (struct compound_asgn *)node;
            return can_use_slots(asgn->target) &&
                   can_use_slots(asgn->v);
        }
        case 'U':
            return can_use_slots(
                ((struct incdec_node *)node)->target);
        case 'u':
        case 'v':
        case 'p':
            return can_use_slots(
                ((struct control_stmt *)node)->value);
        case 'b':
        case 'w':
            return can_use_slots(node->l);
        case 'g':
            return can_use_slots(
                ((struct label_node *)node)->stmt);
        case NODE_FUNCTION_BODY:
            return can_use_slots(node->l);
        case NODE_GOTO:
            return true;
        case 'l':
    case NODE_TUPLE: {
            for (struct list_node *item = (struct list_node *)node; item;
                 item = (struct list_node *)item->next) {
                if (!can_use_slots(item->value))
                    return false;
            }
            return true;
        }
        case 'd': {
            for (struct dict_node *entry = (struct dict_node *)node;
                 entry; entry = (struct dict_node *)entry->next) {
                if (!can_use_slots(entry->key) ||
                    !can_use_slots(entry->value))
                    return false;
            }
            return true;
        }
        case 'k':
            return can_use_slots(
                ((struct is_node *)node)->value);
        case NODE_CAST:
            return can_use_slots(
                ((struct cast_node *)node)->value);
        case 'x':
            return can_use_slots(
                ((struct spread_node *)node)->value);
        case 'y':
        case '?': {
            struct struct_init *init =
                node->nodetype == 'y'
                    ? ((struct struct_lit_node *)node)->fields
                    : ((struct enum_lit_node *)node)->fields;
            for (; init; init = init->next) {
                if (!can_use_slots(init->value))
                    return false;
            }
            return true;
        }
        case 'V':
        case 'N':
        case 'B':
        case 'O':
        case '#':
        case '>':
            return true;
        default:
            return can_use_slots(node->l) &&
                   can_use_slots(node->r);
    }
}

void collect_local_declared_names(
    const struct ast *node,
    std::unordered_set<std::string> &names) {
    if (!node)
        return;
    switch (node->nodetype) {
        case 'D': {
            struct symdef *def = (struct symdef *)node;
            if (def->name)
                names.insert(def->name);
            collect_local_declared_names(def->v, names);
            return;
        }
        case 'Z': {
            struct funcdef *def = (struct funcdef *)node;
            if (def->name)
                names.insert(def->name);
            /* Nested function body has its own scope. */
            return;
        }
        case 'F': {
            struct forinloop *loop = (struct forinloop *)node;
            if (loop->name)
                names.insert(loop->name);
            if (loop->value_name)
                names.insert(loop->value_name);
            collect_local_declared_names(loop->iterable, names);
            collect_local_declared_names(loop->body, names);
            return;
        }
        case 'o': {
            struct on_error_node *handler =
                (struct on_error_node *)node;
            if (handler->name)
                names.insert(handler->name);
            collect_local_declared_names(handler->call, names);
            collect_local_declared_names(handler->body, names);
            return;
        }
        case '7': {
            struct match_node *match = (struct match_node *)node;
            collect_local_declared_names(match->expr, names);
            for (struct match_arm *arm =
                     (struct match_arm *)match->arms;
                 arm; arm = (struct match_arm *)arm->next) {
                if (arm->pattern) {
                    std::vector<std::string> bindings;
                    skein::ast_analysis::collect_pattern_bindings(arm->pattern, bindings);
                    for (const std::string &name : bindings)
                        names.insert(name);
                }
                collect_local_declared_names(arm->guard, names);
                collect_local_declared_names(arm->body, names);
            }
            return;
        }
        case NODE_DESTRUCTURE: {
            struct destructure_node *d =
                (struct destructure_node *)node;
            std::vector<std::string> bindings;
            skein::ast_analysis::collect_pattern_bindings(d->pattern, bindings);
            for (const std::string &name : bindings)
                names.insert(name);
            collect_local_declared_names(d->value, names);
            return;
        }
        case 'J': {
            struct import_node *imp = (struct import_node *)node;
            if (imp->selections) {
                for (const struct import_selection *sel = imp->selections;
                     sel; sel = sel->next) {
                    const char *binding = sel->alias ? sel->alias : sel->name;
                    if (binding && *binding)
                        names.insert(binding);
                }
                return;
            }
            const char *binding =
                imp->alias ? imp->alias
                           : import_binding_name(imp->path);
            if (binding && *binding)
                names.insert(binding);
            return;
        }
        case 'Y':
        case 'H':
        case 't':
        case '<':
        case '>':
        case 'K':
            return;
        case 'X':
            collect_local_declared_names(
                ((struct nscope *)node)->ast, names);
            return;
        case 'L':
            collect_local_declared_names(node->l, names);
            collect_local_declared_names(node->r, names);
            return;
        case 'I':
        case 'T': {
            struct flow *f = (struct flow *)node;
            collect_local_declared_names(f->cond, names);
            collect_local_declared_names(f->tl, names);
            collect_local_declared_names(f->el, names);
            return;
        }
        case 'W': {
            struct flow *f = (struct flow *)node;
            collect_local_declared_names(f->cond, names);
            collect_local_declared_names(f->tl, names);
            return;
        }
        case 'E': {
            struct flow *f = (struct flow *)node;
            collect_local_declared_names(f->tl, names);
            collect_local_declared_names(f->cond, names);
            return;
        }
        case 'P': {
            struct forloop *f = (struct forloop *)node;
            collect_local_declared_names(f->init, names);
            collect_local_declared_names(f->cond, names);
            collect_local_declared_names(f->step, names);
            collect_local_declared_names(f->body, names);
            return;
        }
        case 'S': {
            struct switch_node *s = (struct switch_node *)node;
            collect_local_declared_names(s->expr, names);
            for (struct case_node *c = (struct case_node *)s->cases; c;
                 c = (struct case_node *)c->next) {
                collect_local_declared_names(c->values, names);
                collect_local_declared_names(c->body, names);
            }
            return;
        }
        case 'R':
            collect_local_declared_names(((struct fnret *)node)->v,
                                         names);
            return;
        case 'C': {
            struct ufncall *call = (struct ufncall *)node;
            collect_local_declared_names(call->callee, names);
            collect_local_declared_names(call->args, names);
            return;
        }
        case '=': {
            struct symasgn *asgn = (struct symasgn *)node;
            collect_local_declared_names(asgn->target, names);
            collect_local_declared_names(asgn->v, names);
            return;
        }
        case 'a': {
            struct compound_asgn *asgn = (struct compound_asgn *)node;
            collect_local_declared_names(asgn->target, names);
            collect_local_declared_names(asgn->v, names);
            return;
        }
        case 'U':
            collect_local_declared_names(
                ((struct incdec_node *)node)->target, names);
            return;
        case 'u':
        case 'v':
        case 'p':
            collect_local_declared_names(
                ((struct control_stmt *)node)->value, names);
            return;
        case 'b':
        case 'w':
            collect_local_declared_names(node->l, names);
            return;
        case 'g':
            collect_local_declared_names(
                ((struct label_node *)node)->stmt, names);
            return;
        case NODE_FUNCTION_BODY:
            collect_local_declared_names(node->l, names);
            return;
        case NODE_GOTO:
            return;
        case 'l':
    case NODE_TUPLE: {
            for (struct list_node *item = (struct list_node *)node; item;
                 item = (struct list_node *)item->next)
                collect_local_declared_names(item->value, names);
            return;
        }
        case 'd': {
            for (struct dict_node *entry = (struct dict_node *)node;
                 entry; entry = (struct dict_node *)entry->next) {
                collect_local_declared_names(entry->key, names);
                collect_local_declared_names(entry->value, names);
            }
            return;
        }
        case 'k':
            collect_local_declared_names(
                ((struct is_node *)node)->value, names);
            return;
        case NODE_CAST:
            collect_local_declared_names(
                ((struct cast_node *)node)->value, names);
            return;
        case 'x':
            collect_local_declared_names(
                ((struct spread_node *)node)->value, names);
            return;
        case 'y':
        case '?': {
            struct struct_init *init =
                node->nodetype == 'y'
                    ? ((struct struct_lit_node *)node)->fields
                    : ((struct enum_lit_node *)node)->fields;
            for (; init; init = init->next)
                collect_local_declared_names(init->value, names);
            return;
        }
        case 'n':
            collect_local_declared_names(node->l, names);
            return;
        case 'V':
        case 'N':
        case 'B':
        case 'O':
        case '#':
            return;
        default:
            collect_local_declared_names(node->l, names);
            collect_local_declared_names(node->r, names);
            return;
    }
}

} // namespace compiler_analysis
} // namespace skein
