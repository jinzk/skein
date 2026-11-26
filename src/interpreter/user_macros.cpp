#include "user_macros.h"

#include "diagnostics.h"
#include "macros.h"
#include "structeval.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::size_t kParamPrefixLen = sizeof(USER_MACRO_PARAM_PREFIX) - 1;
constexpr std::size_t kExpandPrefixLen = sizeof(USER_MACRO_EXPAND_PREFIX) - 1;

struct macro_entry {
    std::string name;
    user_macro_template body;
};

thread_local std::vector<macro_entry> g_macros;
thread_local std::vector<std::vector<macro_entry>> g_saved_macros;
thread_local bool g_template_mode = false;
thread_local int g_next_expansion_id = 1;
thread_local int g_expansion_depth = 0;

enum class clone_context {
    expr,
    name,
    stmt,
    arg,
};

struct param_value {
    user_macro_param_kind kind = user_macro_param_kind::expr;
    struct ast *expr = NULL;
    std::string ident;
    std::vector<struct ast *> exprs;
    struct ast *block = NULL;
};

struct expansion_state {
    const user_macro_template *macro = NULL;
    std::unordered_map<std::string, param_value> values;
    std::unordered_set<std::string> declared;
    int expansion_id = 0;
    bool failed = false;
    bool validate_only = false;
};

char *copy_string(const char *text) {
    if (!text)
        return NULL;
    const std::size_t len = std::strlen(text) + 1;
    char *copy = (char *)std::malloc(len);
    if (!copy) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(copy, text, len);
    return copy;
}

bool placeholder_of(const char *name, std::string *out) {
    if (!name || std::strncmp(name, USER_MACRO_PARAM_PREFIX,
                              kParamPrefixLen) != 0)
        return false;
    if (out)
        *out = name + kParamPrefixLen;
    return true;
}

bool expand_placeholder_of(const char *name, std::string *out) {
    if (!name || std::strncmp(name, USER_MACRO_EXPAND_PREFIX,
                              kExpandPrefixLen) != 0)
        return false;
    if (out)
        *out = name + kExpandPrefixLen;
    return true;
}

bool is_expand_node(const struct ast *node, std::string *name = NULL) {
    return node && node->nodetype == 'N' &&
           expand_placeholder_of(((const struct symref *)node)->name, name);
}

bool is_internal_name(const char *name) {
    if (!name)
        return false;
    if (name[0] == '#')
        return true;
    return placeholder_of(name, NULL) || expand_placeholder_of(name, NULL);
}

std::string hygienic_name(int id, const char *name) {
    return "#m" + std::to_string(id) + "_" + (name ? name : "");
}

struct type_constraint *dup_type(const struct type_constraint *type) {
    if (!type)
        return NULL;
    struct type_constraint *copy = (struct type_constraint *)std::malloc(
        sizeof(struct type_constraint));
    if (!copy) {
        yyerror("out of space");
        std::exit(0);
    }
    *copy = copy_type_constraint(type);
    return copy;
}

void report_at(int line, int column, const char *message,
               const char *detail = NULL) {
    diag_set_location(line > 0 ? line : 0, column > 0 ? column : 0);
    if (detail)
        yyerror("%s: %s", message, detail);
    else
        yyerror("%s", message);
}

template <typename T>
static T *copy_node(const T *source) {
    T *copy = (T *)ast_node_alloc(sizeof(T));
    if (!copy) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(copy, source, sizeof(T));
    return copy;
}

struct ast *clone_node(const struct ast *node, expansion_state &state,
                       clone_context context);

expansion_state fragment_state_for(const expansion_state &state) {
    expansion_state fragment = state;
    fragment.expansion_id = 0;
    fragment.declared.clear();
    return fragment;
}

std::string resolve_placeholder_or_name(const char *name, expansion_state &state,
                                        clone_context context, int line,
                                        int column) {
    std::string param_name;
    if (placeholder_of(name, &param_name)) {
        auto it = state.values.find(param_name);
        if (it == state.values.end()) {
            report_at(line, column, "unknown macro placeholder",
                      param_name.c_str());
            state.failed = true;
            return param_name;
        }
        const param_value &value = it->second;
        if (value.kind == user_macro_param_kind::ident)
            return value.ident;
        if (context == clone_context::name ||
            context == clone_context::stmt) {
            report_at(line, column, "macro fragment cannot be used here",
                      param_name.c_str());
            state.failed = true;
            return param_name;
        }
        return name ? name : "";
    }
    if (state.expansion_id > 0 &&
        state.declared.find(name ? name : "") != state.declared.end())
        return hygienic_name(state.expansion_id, name);
    return name ? name : "";
}

struct ast *splice_arg_expansion(const char *param_name,
                                 expansion_state &state, int line,
                                 int column) {
    auto it = state.values.find(param_name ? param_name : "");
    if (it == state.values.end() ||
        it->second.kind != user_macro_param_kind::expr_list) {
        report_at(line, column, "expr... placeholder is not valid here",
                  param_name ? param_name : "");
        state.failed = true;
        return NULL;
    }
    expansion_state fragment_state = fragment_state_for(state);
    struct ast *result = NULL;
    struct ast **tail = &result;
    for (struct ast *item : it->second.exprs) {
        struct ast *copy = clone_node(item, fragment_state,
                                      clone_context::expr);
        *tail = newast('L', copy, NULL);
        tail = &((*tail)->r);
    }
    return result;
}

struct ast *clone_call_arg_list(const struct list_node *list,
                                expansion_state &state) {
    if (!list)
        return NULL;
    std::string param_name;
    struct ast *value = NULL;
    if (is_expand_node(list->value, &param_name)) {
        value = splice_arg_expansion(param_name.c_str(), state,
                                     list->value->line, list->value->column);
    } else {
        value = clone_node(list->value, state, clone_context::arg);
    }
    struct ast *next = clone_call_arg_list(
        (const struct list_node *)list->next, state);
    if (!value)
        value = newnullast();
    struct ast *result = newast('L', value, next);
    result->line = list->line;
    result->column = list->column;
    return result;
}

struct ast *clone_call_args(const struct ast *args, expansion_state &state) {
    std::string param_name;
    if (is_expand_node(args, &param_name))
        return splice_arg_expansion(param_name.c_str(), state, args->line,
                                    args->column);
    return clone_node(args, state, clone_context::arg);
}

struct symlist *clone_symlist(const struct symlist *list,
                              expansion_state &state) {
    if (!list)
        return NULL;
    struct symlist *head = NULL;
    struct symlist *tail = NULL;
    for (const struct symlist *item = list; item; item = item->next) {
        const char *raw = item->sym && item->sym->name ? item->sym->name : "";
        std::string name = resolve_placeholder_or_name(
            raw, state, clone_context::name, 0, 0);
        struct symbol *sym = newsym(copy_string(name.c_str()));
        struct symlist *copy = newsymlist(sym, dup_type(item->type), NULL,
                                          item->variadic);
        struct ast *default_value =
            (struct ast *)take_symlist_default_ast((struct symlist *)item);
        if (default_value) {
            set_symlist_default_ast(copy,
                                    clone_node(default_value, state,
                                               clone_context::expr));
            set_symlist_default_ast((struct symlist *)item, default_value);
        }
        struct ast *pattern =
            (struct ast *)symlist_pattern_ast((struct symlist *)item);
        if (pattern) {
            set_symlist_pattern_ast(copy,
                                    clone_node(pattern, state,
                                               clone_context::expr));
            set_symlist_pattern_ast((struct symlist *)item, pattern);
        }
        if (tail)
            tail->next = copy;
        else
            head = copy;
        tail = copy;
    }
    return head;
}

struct ast *clone_pair(const struct ast *node, expansion_state &state,
                       clone_context context) {
    struct ast *copy = newast(node->nodetype,
                              clone_node(node->l, state, context),
                              clone_node(node->r, state, context));
    copy->line = node->line;
    copy->column = node->column;
    return copy;
}

struct ast *clone_control(const struct control_stmt *node,
                          expansion_state &state) {
    struct control_stmt *copy = copy_node(node);
    copy->value = clone_node(node->value, state, clone_context::expr);
    return (struct ast *)copy;
}

struct ast *clone_node(const struct ast *node, expansion_state &state,
                       clone_context context) {
    if (!node)
        return NULL;
    switch (node->nodetype) {
        case 'N': {
            const struct symref *ref = (const struct symref *)node;
            std::string param_name;
            if (expand_placeholder_of(ref->name, &param_name)) {
                report_at(node->line, node->column,
                          "expr... fragment must be used in an argument list",
                          param_name.c_str());
                state.failed = true;
                return newref(copy_string(param_name.c_str()));
            }
            if (placeholder_of(ref->name, &param_name)) {
                auto it = state.values.find(param_name);
                if (it == state.values.end()) {
                    report_at(node->line, node->column,
                              "unknown macro placeholder", param_name.c_str());
                    state.failed = true;
                    return newref(copy_string(param_name.c_str()));
                }
                const param_value &value = it->second;
                switch (value.kind) {
                    case user_macro_param_kind::ident: {
                        struct ast *result =
                            newref(copy_string(value.ident.c_str()));
                        result->line = node->line;
                        result->column = node->column;
                        return result;
                    }
                    case user_macro_param_kind::expr: {
                        expansion_state fragment = fragment_state_for(state);
                        struct ast *result = clone_node(
                            value.expr, fragment, clone_context::expr);
                        return result;
                    }
                    case user_macro_param_kind::block: {
                        if (context != clone_context::stmt) {
                            report_at(node->line, node->column,
                                      "block fragment must be used as a statement",
                                      param_name.c_str());
                            state.failed = true;
                            return newref(copy_string(param_name.c_str()));
                        }
                        expansion_state fragment = fragment_state_for(state);
                        return clone_node(value.block, fragment,
                                          clone_context::stmt);
                    }
                    case user_macro_param_kind::expr_list:
                        report_at(node->line, node->column,
                                  "expr... fragment must be used in an argument list",
                                  param_name.c_str());
                        state.failed = true;
                        return newref(copy_string(param_name.c_str()));
                }
            }
            std::string name = resolve_placeholder_or_name(
                ref->name, state, context, node->line, node->column);
            struct ast *result = newref(copy_string(name.c_str()));
            result->line = node->line;
            result->column = node->column;
            return result;
        }
        case 'V': {
            const struct val *literal = (const struct val *)node;
            struct ast *result = newvalnode(dupval(literal->val));
            result->line = node->line;
            result->column = node->column;
            return result;
        }
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 'A':
        case 'Q':
        case 's':
        case 'r':
        case 'j':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '0':
        case NODE_FUNCTION_BODY:
        case NODE_OPTIONAL_CHAIN:
        case NODE_REF_TARGET:
            return clone_pair(node, state, clone_context::expr);
        case 'M':
        case '!':
        case '~':
        case 'b':
        case 'w':
        case 'n':
            return clone_control((const struct control_stmt *)node, state);
        case 'G': {
            struct index_node *copy = copy_node(
                (const struct index_node *)node);
            copy->l = clone_node(node->l, state, clone_context::expr);
            copy->r = clone_node(node->r, state, clone_context::expr);
            return (struct ast *)copy;
        }
        case NODE_BRACKET_APPLY: {
            const struct bracket_apply_node *source =
                (const struct bracket_apply_node *)node;
            struct bracket_apply_node *copy = copy_node(source);
            copy->base = clone_node(source->base, state, clone_context::expr);
            copy->suffix_value = clone_node(source->suffix_value, state,
                                            clone_context::expr);
            return (struct ast *)copy;
        }
        case 'L': {
            if (context == clone_context::arg)
                return clone_call_arg_list((const struct list_node *)node,
                                           state);
            const struct list_node *source = (const struct list_node *)node;
            struct list_node *copy = copy_node(source);
            copy->value = clone_node(source->value, state, context);
            copy->next = clone_node(source->next, state, context);
            return (struct ast *)copy;
        }
        case 'C': {
            const struct ufncall *source = (const struct ufncall *)node;
            struct ufncall *copy = copy_node(source);
            copy->callee = clone_node(source->callee, state,
                                      clone_context::expr);
            copy->args = clone_call_args(source->args, state);
            return (struct ast *)copy;
        }
        case '=': {
            const struct symasgn *source = (const struct symasgn *)node;
            struct symasgn *copy = copy_node(source);
            copy->target = clone_node(source->target, state,
                                      clone_context::expr);
            copy->v = clone_node(source->v, state, clone_context::expr);
            return (struct ast *)copy;
        }
        case 'a': {
            const struct compound_asgn *source =
                (const struct compound_asgn *)node;
            struct compound_asgn *copy = copy_node(source);
            copy->target = clone_node(source->target, state,
                                      clone_context::expr);
            copy->v = clone_node(source->v, state, clone_context::expr);
            return (struct ast *)copy;
        }
        case 'U': {
            const struct incdec_node *source =
                (const struct incdec_node *)node;
            struct incdec_node *copy = copy_node(source);
            copy->target = clone_node(source->target, state,
                                      clone_context::expr);
            return (struct ast *)copy;
        }
        case 'D': {
            const struct symdef *source = (const struct symdef *)node;
            struct symdef *copy = copy_node(source);
            std::string name = resolve_placeholder_or_name(
                source->name, state, clone_context::name, node->line,
                node->column);
            copy->name = copy_string(name.c_str());
            copy->v = clone_node(source->v, state, clone_context::expr);
            copy->type = dup_type(source->type);
            return (struct ast *)copy;
        }
        case 'R': {
            const struct fnret *source = (const struct fnret *)node;
            struct fnret *copy = copy_node(source);
            copy->v = clone_node(source->v, state, clone_context::expr);
            return (struct ast *)copy;
        }
        case 'X': {
            const struct nscope *source = (const struct nscope *)node;
            struct nscope *copy = copy_node(source);
            copy->ast = clone_node(source->ast, state, clone_context::stmt);
            return (struct ast *)copy;
        }
        case 'l':
        case NODE_TUPLE: {
            const struct list_node *source = (const struct list_node *)node;
            struct list_node *copy = copy_node(source);
            copy->value = clone_node(source->value, state, context);
            copy->next = clone_node(source->next, state, context);
            return (struct ast *)copy;
        }
        case 'd': {
            const struct dict_node *source = (const struct dict_node *)node;
            struct dict_node *copy = copy_node(source);
            copy->key = clone_node(source->key, state, clone_context::expr);
            copy->value = clone_node(source->value, state,
                                     clone_context::expr);
            copy->next = clone_node(source->next, state, clone_context::expr);
            return (struct ast *)copy;
        }
        case 'I':
        case 'W':
        case 'E':
        case 'T': {
            const struct flow *source = (const struct flow *)node;
            struct flow *copy = copy_node(source);
            copy->cond = clone_node(source->cond, state, clone_context::expr);
            copy->tl = clone_node(source->tl, state, clone_context::stmt);
            copy->el = clone_node(source->el, state, clone_context::stmt);
            return (struct ast *)copy;
        }
        case 'P': {
            const struct forloop *source = (const struct forloop *)node;
            struct forloop *copy = copy_node(source);
            copy->init = clone_node(source->init, state, clone_context::stmt);
            copy->cond = clone_node(source->cond, state, clone_context::expr);
            copy->step = clone_node(source->step, state, clone_context::expr);
            copy->body = clone_node(source->body, state, clone_context::stmt);
            return (struct ast *)copy;
        }
        case 'F': {
            const struct forinloop *source = (const struct forinloop *)node;
            struct forinloop *copy = copy_node(source);
            copy->name = copy_string(resolve_placeholder_or_name(
                source->name, state, clone_context::name, node->line,
                node->column).c_str());
            copy->value_name = copy_string(resolve_placeholder_or_name(
                source->value_name, state, clone_context::name, node->line,
                node->column).c_str());
            copy->iterable = clone_node(source->iterable, state,
                                        clone_context::expr);
            copy->body = clone_node(source->body, state,
                                    clone_context::stmt);
            return (struct ast *)copy;
        }
        case 'S': {
            const struct switch_node *source =
                (const struct switch_node *)node;
            struct switch_node *copy = copy_node(source);
            copy->expr = clone_node(source->expr, state, clone_context::expr);
            copy->cases = clone_node(source->cases, state,
                                     clone_context::stmt);
            return (struct ast *)copy;
        }
        case 'c': {
            const struct case_node *source = (const struct case_node *)node;
            struct case_node *copy = copy_node(source);
            copy->values = clone_node(source->values, state,
                                      clone_context::expr);
            copy->body = clone_node(source->body, state,
                                    clone_context::stmt);
            copy->next = clone_node((const struct ast *)source->next, state,
                                    clone_context::stmt);
            return (struct ast *)copy;
        }
        case 'o': {
            const struct on_error_node *source =
                (const struct on_error_node *)node;
            struct on_error_node *copy = copy_node(source);
            copy->call = clone_node(source->call, state, clone_context::expr);
            copy->name = copy_string(resolve_placeholder_or_name(
                source->name, state, clone_context::name, node->line,
                node->column).c_str());
            copy->body = clone_node(source->body, state,
                                    clone_context::stmt);
            return (struct ast *)copy;
        }
        case 'k': {
            const struct is_node *source = (const struct is_node *)node;
            struct is_node *copy = copy_node(source);
            copy->value = clone_node(source->value, state,
                                     clone_context::expr);
            copy->type = dup_type(source->type);
            return (struct ast *)copy;
        }
        case NODE_CAST: {
            const struct cast_node *source = (const struct cast_node *)node;
            struct cast_node *copy = copy_node(source);
            copy->value = clone_node(source->value, state,
                                     clone_context::expr);
            copy->type = dup_type(source->type);
            return (struct ast *)copy;
        }
        case 'x': {
            const struct spread_node *source =
                (const struct spread_node *)node;
            struct spread_node *copy = copy_node(source);
            copy->value = clone_node(source->value, state,
                                     clone_context::expr);
            return (struct ast *)copy;
        }
        case 'Z': {
            const struct funcdef *source = (const struct funcdef *)node;
            struct funcdef *copy = copy_node(source);
            if (source->generic_params) {
                report_at(node->line, node->column,
                          "macro template cannot generate generic syntax");
                state.failed = true;
            }
            copy->name = source->name
                             ? copy_string(resolve_placeholder_or_name(
                                   source->name, state, clone_context::name,
                                   node->line, node->column).c_str())
                             : NULL;
            copy->sl = clone_symlist(source->sl, state);
            copy->ast = clone_node(source->ast, state, clone_context::stmt);
            copy->ret_type = dup_type(source->ret_type);
            copy->method_self_type = dup_type(source->method_self_type);
            copy->generic_params = NULL;
            copy->moved = 0;
            copy->decorated_generator_source = NULL;
            return (struct ast *)copy;
        }
        case 'g': {
            const struct label_node *source = (const struct label_node *)node;
            struct label_node *copy = copy_node(source);
            copy->name = copy_string(resolve_placeholder_or_name(
                source->name, state, clone_context::name, node->line,
                node->column).c_str());
            copy->stmt = clone_node(source->stmt, state, clone_context::stmt);
            return (struct ast *)copy;
        }
        case NODE_GOTO: {
            const struct goto_node *source = (const struct goto_node *)node;
            struct goto_node *copy = copy_node(source);
            copy->name = copy_string(resolve_placeholder_or_name(
                source->name, state, clone_context::name, node->line,
                node->column).c_str());
            return (struct ast *)copy;
        }
        case 'B':
        case 'O': {
            const struct nbreak *source = (const struct nbreak *)node;
            struct nbreak *copy = copy_node(source);
            copy->label = source->label
                              ? copy_string(resolve_placeholder_or_name(
                                    source->label, state, clone_context::name,
                                    node->line, node->column).c_str())
                              : NULL;
            return (struct ast *)copy;
        }
        case NODE_USER_MACRO_CALL: {
            const struct user_macro_call_node *source =
                (const struct user_macro_call_node *)node;
            if (state.validate_only) {
                const char *callee_name =
                    source->callee && source->callee->nodetype == 'N'
                        ? ((const struct symref *)source->callee)->name
                        : NULL;
                if (callee_name && state.macro &&
                    std::strcmp(callee_name,
                                state.macro->name.c_str()) == 0) {
                    report_at(node->line, node->column,
                              "recursive user macro calls are not allowed");
                    state.failed = true;
                }
                return new_user_macro_call(
                    clone_node(source->callee, state, clone_context::expr),
                    clone_call_args(source->args, state),
                    clone_node(source->block, state, clone_context::stmt),
                    node->line, node->column, source->generator != 0);
            }
            /* Substitute this macro's fragments into the deferred call before
             * expanding the nested macro. */
            struct ast *deferred = new_user_macro_call(
                clone_node(source->callee, state, clone_context::expr),
                clone_call_args(source->args, state),
                clone_node(source->block, state, clone_context::stmt),
                node->line, node->column, source->generator != 0);
            struct ast *result = expand_deferred_macro(deferred);
            if (!result)
                state.failed = true;
            return result;
        }
        default:
            report_at(node->line, node->column,
                      "macro template contains unsupported syntax");
            state.failed = true;
            return NULL;
    }
}

void collect_declared(const struct ast *node,
                      std::unordered_set<std::string> &names) {
    if (!node)
        return;
    switch (node->nodetype) {
        case 'D': {
            const char *name = ((const struct symdef *)node)->name;
            if (name && !placeholder_of(name, NULL))
                names.insert(name);
            collect_declared(((const struct symdef *)node)->v, names);
            break;
        }
        case 'Z': {
            const struct funcdef *func = (const struct funcdef *)node;
            if (func->name && !placeholder_of(func->name, NULL))
                names.insert(func->name);
            for (const struct symlist *param = func->sl; param;
                 param = param->next) {
                const char *name = param->sym ? param->sym->name : NULL;
                if (name && !placeholder_of(name, NULL))
                    names.insert(name);
            }
            collect_declared(func->ast, names);
            break;
        }
        case 'F': {
            const struct forinloop *loop = (const struct forinloop *)node;
            if (loop->name && !placeholder_of(loop->name, NULL))
                names.insert(loop->name);
            if (loop->value_name && !placeholder_of(loop->value_name, NULL))
                names.insert(loop->value_name);
            collect_declared(loop->iterable, names);
            collect_declared(loop->body, names);
            break;
        }
        case 'o': {
            const struct on_error_node *handler =
                (const struct on_error_node *)node;
            if (handler->name && !placeholder_of(handler->name, NULL))
                names.insert(handler->name);
            collect_declared(handler->call, names);
            collect_declared(handler->body, names);
            break;
        }
        case 'g': {
            const struct label_node *label = (const struct label_node *)node;
            if (label->name && !placeholder_of(label->name, NULL))
                names.insert(label->name);
            collect_declared(label->stmt, names);
            break;
        }
        case NODE_GOTO:
        case 'B':
        case 'O':
            break;
        case 'C': {
            const struct ufncall *call = (const struct ufncall *)node;
            collect_declared(call->callee, names);
            collect_declared(call->args, names);
            break;
        }
        case 'L':
        case NODE_TUPLE:
            collect_declared(node->l, names);
            collect_declared(node->r, names);
            break;
        case 'l':
            collect_declared(node->l, names);
            collect_declared(node->r, names);
            break;
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 'A':
        case 'Q':
        case 's':
        case 'r':
        case 'j':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case NODE_FUNCTION_BODY:
        case NODE_OPTIONAL_CHAIN:
        case NODE_REF_TARGET:
            collect_declared(node->l, names);
            collect_declared(node->r, names);
            break;
        default:
            break;
    }
}

bool validate_node(const struct ast *node, const user_macro_template &macro,
                   clone_context context) {
    if (!node)
        return true;
    if (node->nodetype == NODE_USER_MACRO_CALL) {
        const struct user_macro_call_node *call =
            (const struct user_macro_call_node *)node;
        const char *name = call->callee && call->callee->nodetype == 'N'
                               ? ((const struct symref *)call->callee)->name
                               : NULL;
        if (name && std::strcmp(name, macro.name.c_str()) == 0) {
            report_at(node->line, node->column,
                      "recursive user macro calls are not allowed");
            return false;
        }
        return validate_node(call->callee, macro, clone_context::expr) &&
               validate_node(call->args, macro, clone_context::arg) &&
               validate_node(call->block, macro, clone_context::stmt);
    }
    if (node->nodetype == 'G') {
        const struct index_node *index = (const struct index_node *)node;
        if (index->generic_candidate || index->generic_function_value) {
            report_at(node->line, node->column,
                      "macro template cannot generate generic syntax");
            return false;
        }
    }
    std::string param_name;
    if (is_expand_node(node, &param_name)) {
        if (context != clone_context::arg) {
            report_at(node->line, node->column,
                      "expr... fragment must be used in an argument list",
                      param_name.c_str());
            return false;
        }
        bool valid = false;
        for (const auto &param : macro.params)
            if (param.name == param_name &&
                param.kind == user_macro_param_kind::expr_list)
                valid = true;
        if (!valid) {
            report_at(node->line, node->column,
                      "expr... placeholder must refer to an expr... parameter",
                      param_name.c_str());
            return false;
        }
        return true;
    }
    if (node->nodetype == 'N' &&
        placeholder_of(((const struct symref *)node)->name, &param_name)) {
        bool found = false;
        user_macro_param_kind kind = user_macro_param_kind::expr;
        for (const auto &param : macro.params) {
            if (param.name == param_name) {
                found = true;
                kind = param.kind;
                break;
            }
        }
        if (!found) {
            report_at(node->line, node->column, "unknown macro placeholder",
                      param_name.c_str());
            return false;
        }
        if (kind == user_macro_param_kind::block &&
            context != clone_context::stmt) {
            report_at(node->line, node->column,
                      "block fragment must be used as a statement",
                      param_name.c_str());
            return false;
        }
        if (kind == user_macro_param_kind::expr_list) {
            report_at(node->line, node->column,
                      "expr... fragment must be written as #name...",
                      param_name.c_str());
            return false;
        }
        return true;
    }

    bool ok = true;
    switch (node->nodetype) {
        case 'D': {
            const struct symdef *def = (const struct symdef *)node;
            if (placeholder_of(def->name, &param_name)) {
                bool is_ident = false;
                for (const auto &param : macro.params)
                    if (param.name == param_name &&
                        param.kind == user_macro_param_kind::ident)
                        is_ident = true;
                if (!is_ident) {
                    report_at(node->line, node->column,
                              "only an ident fragment can be used as a name",
                              param_name.c_str());
                    ok = false;
                }
            }
            ok = validate_node(def->v, macro, clone_context::expr) && ok;
            break;
        }
        case 'Z': {
            const struct funcdef *func = (const struct funcdef *)node;
            if (func->generic_params) {
                report_at(node->line, node->column,
                          "macro template cannot generate generic syntax");
                ok = false;
            }
            for (const struct symlist *param = func->sl; param;
                 param = param->next) {
                const char *name = param->sym ? param->sym->name : NULL;
                if (placeholder_of(name, &param_name)) {
                    bool is_ident = false;
                    for (const auto &item : macro.params)
                        if (item.name == param_name &&
                            item.kind == user_macro_param_kind::ident)
                            is_ident = true;
                    if (!is_ident) {
                        report_at(node->line, node->column,
                                  "only an ident fragment can be used as a name",
                                  param_name.c_str());
                        ok = false;
                    }
                }
            }
            ok = validate_node(func->ast, macro, clone_context::stmt) && ok;
            break;
        }
        case 'F': {
            const struct forinloop *loop = (const struct forinloop *)node;
            const char *names[2] = {loop->name, loop->value_name};
            for (const char *name : names) {
                if (!placeholder_of(name, &param_name))
                    continue;
                bool is_ident = false;
                for (const auto &item : macro.params)
                    if (item.name == param_name &&
                        item.kind == user_macro_param_kind::ident)
                        is_ident = true;
                if (!is_ident) {
                    report_at(node->line, node->column,
                              "only an ident fragment can be used as a name",
                              param_name.c_str());
                    ok = false;
                }
            }
            ok = validate_node(loop->iterable, macro, clone_context::expr) &&
                 validate_node(loop->body, macro, clone_context::stmt) && ok;
            break;
        }
        case 'o': {
            const struct on_error_node *handler =
                (const struct on_error_node *)node;
            if (placeholder_of(handler->name, &param_name)) {
                bool is_ident = false;
                for (const auto &item : macro.params)
                    if (item.name == param_name &&
                        item.kind == user_macro_param_kind::ident)
                        is_ident = true;
                if (!is_ident) {
                    report_at(node->line, node->column,
                              "only an ident fragment can be used as a name",
                              param_name.c_str());
                    ok = false;
                }
            }
            ok = validate_node(handler->call, macro, clone_context::expr) &&
                 validate_node(handler->body, macro, clone_context::stmt) && ok;
            break;
        }
        case 'C': {
            const struct ufncall *call = (const struct ufncall *)node;
            ok = validate_node(call->callee, macro, clone_context::expr) &&
                 validate_node(call->args, macro, clone_context::arg) && ok;
            break;
        }
        case 'L': {
            if (context == clone_context::arg && is_expand_node(node->l) &&
                node->r) {
                report_at(node->l->line, node->l->column,
                          "expr... fragment must be the last argument");
                return false;
            }
            ok = validate_node(node->l, macro, context) &&
                 validate_node(node->r, macro, context) && ok;
            break;
        }
        case 'X':
        case 'l':
            ok = validate_node(node->l, macro, context) &&
                 validate_node(node->r, macro, context) && ok;
            break;
        case 'V':
            break;
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 'A':
        case 'Q':
        case 's':
        case 'r':
        case 'j':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '0':
        case NODE_FUNCTION_BODY:
        case NODE_OPTIONAL_CHAIN:
        case NODE_REF_TARGET:
        case 'M':
        case '!':
        case '~':
        case 'b':
        case 'w':
        case 'n':
        case 'G':
        case NODE_BRACKET_APPLY:
        case '=':
        case 'a':
        case 'U':
        case 'R':
        case 'd':
        case NODE_TUPLE:
        case 'I':
        case 'W':
        case 'E':
        case 'T':
        case 'P':
        case 'S':
        case 'c':
        case 'k':
        case NODE_CAST:
        case 'x':
        case 'g':
        case NODE_GOTO:
        case 'B':
        case 'O':
            ok = validate_node(node->l, macro, clone_context::expr) &&
                 validate_node(node->r, macro, clone_context::expr) && ok;
            break;
        default:
            report_at(node->line, node->column,
                      "macro template contains unsupported syntax");
            return false;
    }
    return ok;
}

macro_entry *find_macro(const char *name) {
    if (!name)
        return NULL;
    for (auto &item : g_macros)
        if (item.name == name)
            return &item;
    return NULL;
}

void collect_macro_calls(const struct ast *node,
                         std::vector<std::string> &out) {
    if (!node)
        return;
    if (node->nodetype == NODE_USER_MACRO_CALL) {
        const struct user_macro_call_node *call =
            (const struct user_macro_call_node *)node;
        if (call->callee && call->callee->nodetype == 'N') {
            const char *name = ((const struct symref *)call->callee)->name;
            if (name && !is_internal_name(name))
                out.push_back(name);
        }
        collect_macro_calls(call->callee, out);
        collect_macro_calls(call->args, out);
        collect_macro_calls(call->block, out);
        return;
    }
    switch (node->nodetype) {
        case 'C': {
            const struct ufncall *call = (const struct ufncall *)node;
            collect_macro_calls(call->callee, out);
            collect_macro_calls(call->args, out);
            break;
        }
        case 'L':
        case NODE_TUPLE:
        case 'l':
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 'A':
        case 'Q':
        case 's':
        case 'r':
        case 'j':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '0':
        case NODE_FUNCTION_BODY:
        case NODE_OPTIONAL_CHAIN:
        case NODE_REF_TARGET:
        case 'G':
            collect_macro_calls(node->l, out);
            collect_macro_calls(node->r, out);
            break;
        case NODE_BRACKET_APPLY: {
            const struct bracket_apply_node *apply =
                (const struct bracket_apply_node *)node;
            collect_macro_calls(apply->base, out);
            collect_macro_calls(apply->suffix_value, out);
            break;
        }
        case '=': {
            const struct symasgn *asgn = (const struct symasgn *)node;
            collect_macro_calls(asgn->target, out);
            collect_macro_calls(asgn->v, out);
            break;
        }
        case 'a': {
            const struct compound_asgn *asgn =
                (const struct compound_asgn *)node;
            collect_macro_calls(asgn->target, out);
            collect_macro_calls(asgn->v, out);
            break;
        }
        case 'U':
            collect_macro_calls(((const struct incdec_node *)node)->target,
                                out);
            break;
        case 'D':
            collect_macro_calls(((const struct symdef *)node)->v, out);
            break;
        case 'R':
            collect_macro_calls(((const struct fnret *)node)->v, out);
            break;
        case 'X':
            collect_macro_calls(((const struct nscope *)node)->ast, out);
            break;
        case 'd': {
            const struct dict_node *dict = (const struct dict_node *)node;
            collect_macro_calls(dict->key, out);
            collect_macro_calls(dict->value, out);
            collect_macro_calls(dict->next, out);
            break;
        }
        case 'I':
        case 'W':
        case 'E':
        case 'T': {
            const struct flow *flow_node = (const struct flow *)node;
            collect_macro_calls(flow_node->cond, out);
            collect_macro_calls(flow_node->tl, out);
            collect_macro_calls(flow_node->el, out);
            break;
        }
        case 'P': {
            const struct forloop *loop = (const struct forloop *)node;
            collect_macro_calls(loop->init, out);
            collect_macro_calls(loop->cond, out);
            collect_macro_calls(loop->step, out);
            collect_macro_calls(loop->body, out);
            break;
        }
        case 'F': {
            const struct forinloop *loop = (const struct forinloop *)node;
            collect_macro_calls(loop->iterable, out);
            collect_macro_calls(loop->body, out);
            break;
        }
        case 'S': {
            const struct switch_node *sw = (const struct switch_node *)node;
            collect_macro_calls(sw->expr, out);
            collect_macro_calls(sw->cases, out);
            break;
        }
        case 'c': {
            const struct case_node *case_node = (const struct case_node *)node;
            collect_macro_calls(case_node->values, out);
            collect_macro_calls(case_node->body, out);
            collect_macro_calls(case_node->next, out);
            break;
        }
        case 'o': {
            const struct on_error_node *handler =
                (const struct on_error_node *)node;
            collect_macro_calls(handler->call, out);
            collect_macro_calls(handler->body, out);
            break;
        }
        case 'k':
            collect_macro_calls(((const struct is_node *)node)->value, out);
            break;
        case NODE_CAST:
            collect_macro_calls(((const struct cast_node *)node)->value, out);
            break;
        case 'x':
            collect_macro_calls(((const struct spread_node *)node)->value,
                                out);
            break;
        case 'Z': {
            const struct funcdef *func = (const struct funcdef *)node;
            for (const struct symlist *param = func->sl; param;
                 param = param->next) {
                struct symlist *mutable_param = (struct symlist *)param;
                collect_macro_calls(symlist_default_ast(mutable_param), out);
                collect_macro_calls(symlist_pattern_ast(mutable_param), out);
            }
            collect_macro_calls(func->ast, out);
            break;
        }
        case 'g':
            collect_macro_calls(((const struct label_node *)node)->stmt, out);
            break;
        case 'M':
        case '!':
        case '~':
        case 'b':
        case 'w':
        case 'n':
            collect_macro_calls(((const struct control_stmt *)node)->value,
                                out);
            break;
        default:
            break;
    }
}

bool macro_reaches(macro_entry *entry, const std::string &target,
                   std::unordered_set<std::string> &visited) {
    if (!entry || !visited.insert(entry->name).second)
        return false;
    std::vector<std::string> calls;
    collect_macro_calls(entry->body.body, calls);
    for (const std::string &call : calls) {
        if (call == target)
            return true;
        if (macro_reaches(find_macro(call.c_str()), target, visited))
            return true;
    }
    return false;
}

bool is_bare_name(const struct ast *callee, std::string *name) {
    if (!callee || callee->nodetype != 'N')
        return false;
    const char *raw = ((const struct symref *)callee)->name;
    if (!raw || is_internal_name(raw))
        return false;
    if (name)
        *name = raw;
    return true;
}

void collect_args(const struct ast *node, std::vector<struct ast *> &out) {
    if (!node)
        return;
    if (node->nodetype == 'L') {
        collect_args(node->l, out);
        collect_args(node->r, out);
        return;
    }
    out.push_back((struct ast *)node);
}

void free_list_nodes(struct ast *node) {
    if (!node || node->nodetype != 'L')
        return;
    if (node->l && node->l->nodetype == 'L')
        free_list_nodes(node->l);
    if (node->r && node->r->nodetype == 'L')
        free_list_nodes(node->r);
    node->l = NULL;
    node->r = NULL;
    treefree(node);
}

void free_param_values(std::unordered_map<std::string, param_value> &values) {
    for (auto &entry : values) {
        treefree(entry.second.expr);
        for (struct ast *item : entry.second.exprs)
            treefree(item);
        treefree(entry.second.block);
    }
    values.clear();
}

struct ast *expand_macro_impl(struct ast *callee, struct ast *args,
                              struct ast *block, int line, int column,
                              bool generator);

struct ast *expand_user_macro(macro_entry *entry, struct ast *args,
                              struct ast *block, int line, int column) {
    const user_macro_template &macro = entry->body;
    expansion_state state;
    state.macro = &macro;
    state.expansion_id = g_next_expansion_id++;
    /* Call-site fragments keep their own names, so clone them without the
     * template's hygiene renaming state. */
    expansion_state fragment_state = fragment_state_for(state);

    std::vector<struct ast *> arg_values;
    collect_args(args, arg_values);
    const std::size_t positional = macro.params.size() -
        (!macro.params.empty() &&
         macro.params.back().kind == user_macro_param_kind::block ? 1 : 0);
    const bool variadic = !macro.params.empty() &&
        macro.params.back().kind == user_macro_param_kind::expr_list;
    const bool expects_block = !macro.params.empty() &&
        macro.params.back().kind == user_macro_param_kind::block;

    bool ok = true;
    if (expects_block) {
        if (!block) {
            report_at(line, column, "macro expects a block argument",
                      macro.name.c_str());
            ok = false;
        }
    } else if (block) {
        report_at(line, column, "macro does not accept a block",
                  macro.name.c_str());
        ok = false;
        treefree(block);
        block = NULL;
    }

    const std::size_t required = variadic ? positional - 1 : positional;
    if (variadic) {
        if (arg_values.size() < required) {
            report_at(line, column, "macro expects more expression arguments",
                      macro.name.c_str());
            ok = false;
        }
    } else if (arg_values.size() != required) {
        report_at(line, column, "macro expects a different number of arguments",
                  macro.name.c_str());
        ok = false;
    }

    for (std::size_t i = 0; i < macro.params.size() && ok; ++i) {
        const user_macro_param &param = macro.params[i];
        if (param.kind == user_macro_param_kind::block)
            break;
        param_value value;
        value.kind = param.kind;
        if (param.kind == user_macro_param_kind::expr_list) {
            for (std::size_t j = i; j < arg_values.size(); ++j) {
                struct ast *copy = clone_node(arg_values[j], fragment_state,
                                              clone_context::expr);
                value.exprs.push_back(copy);
            }
        } else if (i < arg_values.size()) {
            struct ast *arg = arg_values[i];
            if (arg->nodetype == 'x') {
                report_at(line, column,
                          "macro does not accept spread arguments",
                          macro.name.c_str());
                ok = false;
                break;
            }
            if (param.kind == user_macro_param_kind::ident) {
                if (arg->nodetype != 'N' || is_internal_name(
                        ((const struct symref *)arg)->name)) {
                    report_at(line, column,
                              "ident fragment expects a plain identifier",
                              param.name.c_str());
                    ok = false;
                    break;
                }
                value.ident = ((const struct symref *)arg)->name;
                treefree(arg);
                arg_values[i] = NULL;
            } else {
                value.expr = clone_node(arg, fragment_state,
                                        clone_context::expr);
            }
        }
        state.values[param.name] = std::move(value);
    }

    if (expects_block && block) {
        param_value value;
        value.kind = user_macro_param_kind::block;
        value.block = clone_node(block, fragment_state, clone_context::stmt);
        state.values[macro.params.back().name] = std::move(value);
        block = NULL;
    }

    free_list_nodes(args);
    args = NULL;
    if (!ok || state.failed) {
        free_param_values(state.values);
        return NULL;
    }

    collect_declared(macro.body, state.declared);
    struct ast *result = clone_node(macro.body, state, clone_context::stmt);
    if (state.failed) {
        treefree(result);
        free_param_values(state.values);
        return NULL;
    }
    free_param_values(state.values);
    return result;
}

struct ast *expand_macro_impl(struct ast *callee, struct ast *args,
                              struct ast *block, int line, int column,
                              bool generator) {
    if (g_expansion_depth > 64) {
        yyerror("macro expansion nesting is too deep");
        treefree(callee);
        treefree(args);
        treefree(block);
        return NULL;
    }
    ++g_expansion_depth;
    std::string name;
    if (is_bare_name(callee, &name)) {
        macro_entry *entry = find_macro(name.c_str());
        if (entry) {
            struct ast *result = expand_user_macro(entry, args, block,
                                                   line, column);
            treefree(callee);
            --g_expansion_depth;
            return result;
        }
    }
    struct ast *result = expand_builtin_macro(callee, args, block, line,
                                              column, generator);
    --g_expansion_depth;
    return result;
}

} // namespace

void user_macros_reset() {
    g_macros.clear();
}

void user_macros_push() {
    g_saved_macros.push_back(g_macros);
}

void user_macros_pop() {
    if (g_saved_macros.empty()) {
        g_macros.clear();
        return;
    }
    g_macros = std::move(g_saved_macros.back());
    g_saved_macros.pop_back();
}

void user_macros_set_template_mode(bool enabled) {
    g_template_mode = enabled;
}

bool user_macro_define(const char *name,
                       const std::vector<user_macro_param> &params,
                       struct ast *body, int line, int column) {
    if (!name || !*name) {
        report_at(line, column, "macro name must be an identifier");
        treefree(body);
        return false;
    }
    if (find_macro(name)) {
        report_at(line, column, "duplicate macro definition", name);
        treefree(body);
        return false;
    }

    user_macro_template macro;
    macro.name = name;
    macro.params = params;
    macro.body = body;
    macro.line = line;
    macro.column = column;

    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const user_macro_param &param = params[i];
        if (!seen.insert(param.name).second) {
            report_at(param.line, param.column, "duplicate macro parameter",
                      param.name.c_str());
            treefree(body);
            return false;
        }
        if (param.kind == user_macro_param_kind::block &&
            i + 1 != params.size()) {
            report_at(param.line, param.column,
                      "block fragment must be the last parameter",
                      param.name.c_str());
            treefree(body);
            return false;
        }
        if (param.kind == user_macro_param_kind::expr_list &&
            i + 1 != params.size()) {
            report_at(param.line, param.column,
                      "expr... fragment must be the last parameter",
                      param.name.c_str());
            treefree(body);
            return false;
        }
    }

    expansion_state validation;
    validation.macro = &macro;
    validation.validate_only = true;
    for (const auto &param : params) {
        param_value value;
        value.kind = param.kind;
        switch (param.kind) {
            case user_macro_param_kind::expr:
                value.expr = newnullast();
                break;
            case user_macro_param_kind::ident:
                value.ident = param.name;
                break;
            case user_macro_param_kind::expr_list:
                value.exprs.push_back(newnullast());
                break;
            case user_macro_param_kind::block:
                value.block = newscope(NULL);
                break;
        }
        validation.values[param.name] = std::move(value);
    }
    collect_declared(body, validation.declared);
    struct ast *validation_copy = clone_node(body, validation,
                                             clone_context::stmt);
    treefree(validation_copy);
    free_param_values(validation.values);
    if (validation.failed) {
        treefree(body);
        return false;
    }

    std::vector<std::string> calls;
    collect_macro_calls(body, calls);
    std::unordered_set<std::string> visited;
    for (const std::string &call : calls) {
        if (call == name)
            continue;
        if (macro_reaches(find_macro(call.c_str()), name, visited)) {
            report_at(line, column, "recursive user macro calls are not allowed");
            treefree(body);
            return false;
        }
    }

    macro_entry entry;
    entry.name = name;
    entry.body = std::move(macro);
    g_macros.push_back(std::move(entry));
    return true;
}

struct ast *expand_macro(struct ast *callee, struct ast *args,
                         struct ast *block, int line, int column,
                         bool generator) {
    if (g_template_mode) {
        return new_user_macro_call(callee, args, block, line, column,
                                   generator);
    }
    return expand_macro_impl(callee, args, block, line, column, generator);
}

struct ast *expand_deferred_macro(struct ast *call) {
    if (!call || call->nodetype != NODE_USER_MACRO_CALL)
        return call;
    struct user_macro_call_node *node = (struct user_macro_call_node *)call;
    ++g_expansion_depth;
    struct ast *result = expand_macro_impl(node->callee, node->args,
                                           node->block, node->line,
                                           node->column, node->generator != 0);
    --g_expansion_depth;
    ast_node_free(node);
    return result;
}
