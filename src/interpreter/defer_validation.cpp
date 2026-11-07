#include "compiler.h"

#include "ast.h"
#include "ast_analysis.h"
#include "value_runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using skein::bc::SourceLoc;

struct defer_declaration_site {
    std::string name;
    SourceLoc loc;
};

thread_local int defer_error_line = 0;
thread_local int defer_error_column = 0;

static SourceLoc defer_node_loc(const struct ast *node) {
    SourceLoc loc;
    if (node) {
        loc.line = node->line > 0 ? node->line : 1;
        loc.column = node->column > 0 ? node->column : 1;
    }
    return loc;
}

static bool defer_loc_after(const SourceLoc &later,
                            const SourceLoc &earlier) {
    return later.line > earlier.line ||
           (later.line == earlier.line && later.column > earlier.column);
}

static void collect_defer_declaration_sites(
    const struct ast *node,
    std::vector<defer_declaration_site> &sites) {
    if (!node)
        return;
    auto record = [&](const char *name) {
        if (name && *name)
            sites.push_back({name, defer_node_loc(node)});
    };
    switch (node->nodetype) {
        case 'D': {
            struct symdef *def = (struct symdef *)node;
            record(def->name);
            collect_defer_declaration_sites(def->v, sites);
            return;
        }
        case 'Z': {
            struct funcdef *def = (struct funcdef *)node;
            record(def->name);
            /* Nested function body is a separate scope. */
            return;
        }
        case 'F': {
            struct forinloop *loop = (struct forinloop *)node;
            record(loop->name);
            record(loop->value_name);
            collect_defer_declaration_sites(loop->iterable, sites);
            collect_defer_declaration_sites(loop->body, sites);
            return;
        }
        case 'o': {
            struct on_error_node *handler =
                (struct on_error_node *)node;
            record(handler->name);
            collect_defer_declaration_sites(handler->call, sites);
            collect_defer_declaration_sites(handler->body, sites);
            return;
        }
        case '7': {
            struct match_node *match = (struct match_node *)node;
            collect_defer_declaration_sites(match->expr, sites);
            for (struct match_arm *arm =
                     (struct match_arm *)match->arms;
                 arm; arm = (struct match_arm *)arm->next) {
                if (arm->pattern) {
                    std::vector<std::string> bindings;
                    skein::ast_analysis::collect_pattern_bindings(arm->pattern,
                                                       bindings);
                    for (const std::string &name : bindings)
                        sites.push_back({name, defer_node_loc(
                                                    (struct ast *)arm)});
                }
                collect_defer_declaration_sites(arm->guard, sites);
                collect_defer_declaration_sites(arm->body, sites);
            }
            return;
        }
        case NODE_DESTRUCTURE: {
            struct destructure_node *d =
                (struct destructure_node *)node;
            std::vector<std::string> bindings;
            skein::ast_analysis::collect_pattern_bindings(d->pattern, bindings);
            for (const std::string &name : bindings)
                sites.push_back({name, defer_node_loc(node)});
            collect_defer_declaration_sites(d->value, sites);
            return;
        }
        case 'J': {
            struct import_node *imp = (struct import_node *)node;
            record(imp->alias ? imp->alias
                              : import_binding_name(imp->path));
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
            collect_defer_declaration_sites(
                ((struct nscope *)node)->ast, sites);
            return;
        case 'L':
            collect_defer_declaration_sites(node->l, sites);
            collect_defer_declaration_sites(node->r, sites);
            return;
        case 'I':
        case 'T': {
            struct flow *f = (struct flow *)node;
            collect_defer_declaration_sites(f->cond, sites);
            collect_defer_declaration_sites(f->tl, sites);
            collect_defer_declaration_sites(f->el, sites);
            return;
        }
        case 'W': {
            struct flow *f = (struct flow *)node;
            collect_defer_declaration_sites(f->cond, sites);
            collect_defer_declaration_sites(f->tl, sites);
            return;
        }
        case 'E': {
            struct flow *f = (struct flow *)node;
            collect_defer_declaration_sites(f->tl, sites);
            collect_defer_declaration_sites(f->cond, sites);
            return;
        }
        case 'P': {
            struct forloop *f = (struct forloop *)node;
            collect_defer_declaration_sites(f->init, sites);
            collect_defer_declaration_sites(f->cond, sites);
            collect_defer_declaration_sites(f->step, sites);
            collect_defer_declaration_sites(f->body, sites);
            return;
        }
        case 'S': {
            struct switch_node *s = (struct switch_node *)node;
            collect_defer_declaration_sites(s->expr, sites);
            for (struct case_node *c = (struct case_node *)s->cases; c;
                 c = (struct case_node *)c->next) {
                collect_defer_declaration_sites(c->values, sites);
                collect_defer_declaration_sites(c->body, sites);
            }
            return;
        }
        case 'R':
            collect_defer_declaration_sites(((struct fnret *)node)->v,
                                            sites);
            return;
        case 'C': {
            struct ufncall *call = (struct ufncall *)node;
            collect_defer_declaration_sites(call->callee, sites);
            collect_defer_declaration_sites(call->args, sites);
            return;
        }
        case '=': {
            struct symasgn *asgn = (struct symasgn *)node;
            collect_defer_declaration_sites(asgn->target, sites);
            collect_defer_declaration_sites(asgn->v, sites);
            return;
        }
        case 'a': {
            struct compound_asgn *asgn = (struct compound_asgn *)node;
            collect_defer_declaration_sites(asgn->target, sites);
            collect_defer_declaration_sites(asgn->v, sites);
            return;
        }
        case 'U':
            collect_defer_declaration_sites(
                ((struct incdec_node *)node)->target, sites);
            return;
        case 'u':
        case 'v':
        case 'p':
            collect_defer_declaration_sites(
                ((struct control_stmt *)node)->value, sites);
            return;
        case 'b':
        case 'w':
            collect_defer_declaration_sites(node->l, sites);
            return;
        case 'g':
            collect_defer_declaration_sites(
                ((struct label_node *)node)->stmt, sites);
            return;
        case 'l': {
            for (struct list_node *item = (struct list_node *)node; item;
                 item = (struct list_node *)item->next)
                collect_defer_declaration_sites(item->value, sites);
            return;
        }
        case 'd': {
            for (struct dict_node *entry = (struct dict_node *)node;
                 entry;
                 entry = (struct dict_node *)entry->next) {
                collect_defer_declaration_sites(entry->key, sites);
                collect_defer_declaration_sites(entry->value, sites);
            }
            return;
        }
        case 'k':
            collect_defer_declaration_sites(
                ((struct is_node *)node)->value, sites);
            return;
        case 'x':
            collect_defer_declaration_sites(
                ((struct spread_node *)node)->value, sites);
            return;
        case 'y':
        case '?': {
            struct struct_init *init =
                node->nodetype == 'y'
                    ? ((struct struct_lit_node *)node)->fields
                    : ((struct enum_lit_node *)node)->fields;
            for (; init; init = init->next)
                collect_defer_declaration_sites(init->value, sites);
            return;
        }
        case 'n':
            collect_defer_declaration_sites(node->l, sites);
            return;
        case NODE_CAST:
            collect_defer_declaration_sites(
                ((struct cast_node *)node)->value, sites);
            return;
        case 'N':
        case 'V':
        case 'B':
        case 'O':
        case '#':
            return;
        default:
            collect_defer_declaration_sites(node->l, sites);
            collect_defer_declaration_sites(node->r, sites);
            return;
    }
}

static bool validate_defer_body(const struct ast *body,
                                std::string &error);

static void validate_defer_members(struct struct_member *member,
                                   std::string &error, bool &ok) {
    for (; member && ok; member = member->next) {
        if (!member->func || member->func->nodetype != 'Z')
            continue;
        struct funcdef *def = (struct funcdef *)member->func;
        if (!validate_defer_body(def->ast, error))
            ok = false;
    }
}

static bool validate_defer_walk(
    const struct ast *node,
    const std::vector<defer_declaration_site> &sites,
    std::string &error) {
    if (!node)
        return true;
    switch (node->nodetype) {
        case 'n': {
            std::unordered_set<std::string> referenced;
            skein::ast_analysis::collect_referenced_names(node->l, referenced);
            std::vector<std::string> later;
            for (const std::string &name : referenced) {
                if (name.empty())
                    continue;
                for (const defer_declaration_site &site : sites) {
                    if (site.name != name)
                        continue;
                    if (defer_loc_after(site.loc, defer_node_loc(node))) {
                        later.push_back(name);
                        break;
                    }
                }
            }
            if (!later.empty()) {
                std::sort(later.begin(), later.end());
                defer_error_line = node->line > 0 ? node->line : 1;
                defer_error_column =
                    node->column > 0 ? node->column : 1;
                error = "defer at ";
                error += std::to_string(defer_error_line);
                error += ":";
                error += std::to_string(defer_error_column);
                error += " references ";
                error += later[0];
                for (size_t i = 1; i < later.size(); ++i) {
                    error += (i + 1 == later.size()) ? " and " : ", ";
                    error += later[i];
                }
                error += " before its declaration";
                return false;
            }
            return validate_defer_walk(node->l, sites, error);
        }
        case 'D':
            return validate_defer_walk(
                ((struct symdef *)node)->v, sites, error);
        case 'Z': {
            struct funcdef *def = (struct funcdef *)node;
            return validate_defer_body(def->ast, error);
        }
        case 'Y': {
            struct struct_node *decl = (struct struct_node *)node;
            bool ok = true;
            validate_defer_members(decl->members, error, ok);
            return ok;
        }
        case 'H': {
            struct impl_node *impl = (struct impl_node *)node;
            bool ok = true;
            validate_defer_members(impl->methods, error, ok);
            return ok;
        }
        case 'K': {
            struct package_node *package = (struct package_node *)node;
            return validate_defer_body(package->body, error);
        }
        case NODE_DESTRUCTURE:
            return validate_defer_walk(
                ((struct destructure_node *)node)->value, sites, error);
        case 'J':
        case 't':
        case '<':
        case '>':
            return true;
        case 'X':
            return validate_defer_walk(
                ((struct nscope *)node)->ast, sites, error);
        case 'L':
            return validate_defer_walk(node->l, sites, error) &&
                   validate_defer_walk(node->r, sites, error);
        case 'I':
        case 'T': {
            struct flow *f = (struct flow *)node;
            return validate_defer_walk(f->cond, sites, error) &&
                   validate_defer_walk(f->tl, sites, error) &&
                   validate_defer_walk(f->el, sites, error);
        }
        case 'W': {
            struct flow *f = (struct flow *)node;
            return validate_defer_walk(f->cond, sites, error) &&
                   validate_defer_walk(f->tl, sites, error);
        }
        case 'E': {
            struct flow *f = (struct flow *)node;
            return validate_defer_walk(f->tl, sites, error) &&
                   validate_defer_walk(f->cond, sites, error);
        }
        case 'P': {
            struct forloop *f = (struct forloop *)node;
            return validate_defer_walk(f->init, sites, error) &&
                   validate_defer_walk(f->cond, sites, error) &&
                   validate_defer_walk(f->step, sites, error) &&
                   validate_defer_walk(f->body, sites, error);
        }
        case 'S': {
            struct switch_node *s = (struct switch_node *)node;
            if (!validate_defer_walk(s->expr, sites, error))
                return false;
            for (struct case_node *c = (struct case_node *)s->cases; c;
                 c = (struct case_node *)c->next) {
                if (!validate_defer_walk(c->values, sites, error) ||
                    !validate_defer_walk(c->body, sites, error))
                    return false;
            }
            return true;
        }
        case 'F': {
            struct forinloop *loop = (struct forinloop *)node;
            return validate_defer_walk(loop->iterable, sites, error) &&
                   validate_defer_walk(loop->body, sites, error);
        }
        case 'o': {
            struct on_error_node *handler =
                (struct on_error_node *)node;
            return validate_defer_walk(handler->call, sites, error) &&
                   validate_defer_walk(handler->body, sites, error);
        }
        case '7': {
            struct match_node *match = (struct match_node *)node;
            if (!validate_defer_walk(match->expr, sites, error))
                return false;
            for (struct match_arm *arm =
                     (struct match_arm *)match->arms;
                 arm; arm = (struct match_arm *)arm->next) {
                if (!validate_defer_walk(arm->guard, sites, error) ||
                    !validate_defer_walk(arm->body, sites, error))
                    return false;
            }
            return true;
        }
        case 'R':
            return validate_defer_walk(((struct fnret *)node)->v,
                                       sites, error);
        case 'C': {
            struct ufncall *call = (struct ufncall *)node;
            return validate_defer_walk(call->callee, sites, error) &&
                   validate_defer_walk(call->args, sites, error);
        }
        case '=': {
            struct symasgn *asgn = (struct symasgn *)node;
            return validate_defer_walk(asgn->target, sites, error) &&
                   validate_defer_walk(asgn->v, sites, error);
        }
        case 'a': {
            struct compound_asgn *asgn = (struct compound_asgn *)node;
            return validate_defer_walk(asgn->target, sites, error) &&
                   validate_defer_walk(asgn->v, sites, error);
        }
        case 'U':
            return validate_defer_walk(
                ((struct incdec_node *)node)->target, sites, error);
        case 'u':
        case 'v':
        case 'p':
            return validate_defer_walk(
                ((struct control_stmt *)node)->value, sites, error);
        case 'b':
        case 'w':
            return validate_defer_walk(node->l, sites, error);
        case 'g':
            return validate_defer_walk(
                ((struct label_node *)node)->stmt, sites, error);
        case 'l': {
            for (struct list_node *item = (struct list_node *)node; item;
                 item = (struct list_node *)item->next) {
                if (!validate_defer_walk(item->value, sites, error))
                    return false;
            }
            return true;
        }
        case 'd': {
            for (struct dict_node *entry = (struct dict_node *)node;
                 entry;
                 entry = (struct dict_node *)entry->next) {
                if (!validate_defer_walk(entry->key, sites, error) ||
                    !validate_defer_walk(entry->value, sites, error))
                    return false;
            }
            return true;
        }
        case 'k':
            return validate_defer_walk(
                ((struct is_node *)node)->value, sites, error);
        case 'x':
            return validate_defer_walk(
                ((struct spread_node *)node)->value, sites, error);
        case 'y':
        case '?': {
            struct struct_init *init =
                node->nodetype == 'y'
                    ? ((struct struct_lit_node *)node)->fields
                    : ((struct enum_lit_node *)node)->fields;
            for (; init; init = init->next) {
                if (!validate_defer_walk(init->value, sites, error))
                    return false;
            }
            return true;
        }
        case NODE_CAST:
            return validate_defer_walk(
                ((struct cast_node *)node)->value, sites, error);
        case 'N':
        case 'V':
        case 'B':
        case 'O':
        case '#':
            return true;
        default:
            return validate_defer_walk(node->l, sites, error) &&
                   validate_defer_walk(node->r, sites, error);
    }
}

static bool validate_defer_body(const struct ast *body,
                                std::string &error) {
    if (!body)
        return true;
    std::vector<defer_declaration_site> sites;
    collect_defer_declaration_sites(body, sites);
    return validate_defer_walk(body, sites, error);
}

} // namespace

bool skein_validate_defer_references(struct ast *statement,
                                     std::string &error,
                                     int *error_line,
                                     int *error_column) {
    defer_error_line = 0;
    defer_error_column = 0;
    if (error_line)
        *error_line = 0;
    if (error_column)
        *error_column = 0;
    if (!statement)
        return true;
    bool ok = false;
    switch (statement->nodetype) {
        case 'Z': {
            struct funcdef *def = (struct funcdef *)statement;
            ok = validate_defer_body(def->ast, error);
            break;
        }
        case 'Y': {
            struct struct_node *decl = (struct struct_node *)statement;
            ok = true;
            validate_defer_members(decl->members, error, ok);
            break;
        }
        case 'H': {
            struct impl_node *impl = (struct impl_node *)statement;
            ok = true;
            validate_defer_members(impl->methods, error, ok);
            break;
        }
        case 'K': {
            struct package_node *package =
                (struct package_node *)statement;
            ok = validate_defer_body(package->body, error);
            break;
        }
        default:
            ok = validate_defer_body(statement, error);
            break;
    }
    if (!ok) {
        if (error_line)
            *error_line = defer_error_line;
        if (error_column)
            *error_column = defer_error_column;
    }
    return ok;
}
