#include "decorators.h"

#include "ast.h"
#include "diagnostics.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

char *copy_text(const std::string &text) {
    char *copy = (char *)std::malloc(text.size() + 1);
    if (!copy) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(copy, text.c_str(), text.size() + 1);
    return copy;
}

const char *member_name(const struct struct_member *member) {
    return member && member->name ? member->name : "?";
}

void set_method_location(const struct struct_member *member) {
    if (member)
        diag_set_location(member->line, member->column);
}

bool has_decorators(const struct struct_member *member) {
    return member && member->nodetype == 'm' && member->decorators != NULL;
}

void collect_decorator_expressions(struct ast *node,
                                   std::vector<struct ast *> &out) {
    if (!node)
        return;
    if (node->nodetype == 'L') {
        collect_decorator_expressions(node->l, out);
        collect_decorator_expressions(node->r, out);
    } else {
        out.push_back(node);
    }
}

/*
 * Free only the 'L' wrapper nodes used to chain decorator expressions.  The
 * expression nodes themselves are moved into the generated decorator calls.
 */
void free_decorator_wrappers(struct ast *node) {
    if (!node || node->nodetype != 'L')
        return;
    free_decorator_wrappers(node->l);
    free_decorator_wrappers(node->r);
    std::free(node);
}

std::string hidden_name(const char *kind, const struct impl_node *impl,
                        const struct struct_member *member, int index) {
    std::string name = "$";
    name += kind ? kind : "hidden";
    name += "$";
    if (impl && impl->type_name)
        name += impl->type_name;
    name += "$";
    name += member_name(member);
    name += "$";
    name += std::to_string(member ? member->line : 0);
    name += "$";
    name += std::to_string(member ? member->column : 0);
    name += "$";
    name += std::to_string(index);
    return name;
}

struct type_constraint *copy_type_constraint(
    const struct type_constraint *source) {
    if (!source)
        return NULL;
    struct type_constraint *copy = new_type_constraint(source->kind);
    copy->name = source->name;
    copy->module = source->module;
    copy->nullable = source->nullable;
    return copy;
}

struct symlist *copy_symlist(const struct symlist *source) {
    struct symlist *head = NULL;
    struct symlist *tail = NULL;
    for (const struct symlist *param = source; param; param = param->next) {
        struct symbol *sym = newsym(
            param->sym && param->sym->name ? param->sym->name : "");
        struct symlist *copy = newsymlist(
            sym, copy_type_constraint(param->type), NULL, param->variadic);
        if (tail)
            tail->next = copy;
        else
            head = copy;
        tail = copy;
    }
    return head;
}

struct ast *make_ref(const std::string &name) {
    return newref(copy_text(name));
}

struct ast *call_with_argument(struct ast *callee, struct ast *argument) {
    return newcall(callee, newast('L', argument, NULL));
}

bool validate_decorated_method(const struct struct_member *member) {
    const struct funcdef *func = (const struct funcdef *)member->func;
    if (!func || func->nodetype != 'Z' || func->name != NULL) {
        set_method_location(member);
        yyerror("invalid decorated method %s", member_name(member));
        return false;
    }
    if (!func->sl || !func->sl->sym || !func->sl->sym->name ||
        std::strcmp(func->sl->sym->name, "self") != 0 ||
        func->sl->variadic) {
        set_method_location(member);
        yyerror("decorated method %s must start with self",
                member_name(member));
        return false;
    }
    if (func->sl->has_default) {
        set_method_location(member);
        yyerror("decorated method %s cannot have a default self parameter",
                member_name(member));
        return false;
    }
    if (func->is_generator) {
        set_method_location(member);
        yyerror("decorated method %s cannot be a generator",
                member_name(member));
        return false;
    }
    if (func->is_async) {
        set_method_location(member);
        yyerror("decorated method %s cannot be async", member_name(member));
        return false;
    }
    for (const struct symlist *param = func->sl; param; param = param->next) {
        if (param->has_pattern || symlist_pattern_ast(
                                      const_cast<struct symlist *>(param))) {
            set_method_location(member);
            yyerror("decorated method %s parameters cannot use "
                    "destructuring patterns",
                    member_name(member));
            return false;
        }
        if (param->has_default &&
            !symlist_default_ast(const_cast<struct symlist *>(param))) {
            set_method_location(member);
            yyerror("decorated method %s has an unavailable default value",
                    member_name(member));
            return false;
        }
    }
    return true;
}

/* Move default ASTs from the original parameter list to the wrapper list. */
void move_defaults(struct symlist *source, struct symlist *target) {
    struct symlist *from = source;
    struct symlist *to = target;
    while (from && to) {
        if (from->has_default) {
            struct ast *value = take_symlist_default_ast(from);
            if (value)
                set_symlist_default_ast(to, value);
        }
        from = from->next;
        to = to->next;
    }
}

struct ast *build_decorator_chain(
    const std::vector<struct ast *> &decorators,
    const std::string &body_name) {
    struct ast *current = make_ref(body_name);
    for (size_t i = decorators.size(); i > 0; --i)
        current = call_with_argument(decorators[i - 1], current);
    return current;
}

struct ast *build_wrapper_body(const std::string &decorator_name,
                               const struct symlist *params,
                               int line, int column) {
    std::vector<struct ast *> arguments;
    for (const struct symlist *param = params; param; param = param->next) {
        const char *name =
            param->sym && param->sym->name ? param->sym->name : "";
        struct ast *argument = make_ref(name);
        if (param->variadic)
            argument = newspread(argument);
        arguments.push_back(argument);
    }

    struct ast *argument_list = NULL;
    for (size_t i = arguments.size(); i > 0; --i)
        argument_list = newast('L', arguments[i - 1], argument_list);
    struct ast *call = newcall(make_ref(decorator_name), argument_list);
    call->line = line;
    call->column = column;
    struct ast *body = newret(call);
    body->line = line;
    body->column = column;
    return body;
}

} // namespace

struct ast *lower_impl_decorators(struct impl_node *impl) {
    if (!impl)
        return NULL;

    for (struct struct_member *method = impl->methods; method;
         method = method->next) {
        if (!has_decorators(method))
            continue;
        if (!validate_decorated_method(method))
            return (struct ast *)impl;
    }

    std::vector<struct ast *> prelude;
    int index = 0;
    for (struct struct_member *method = impl->methods; method;
         method = method->next) {
        if (!has_decorators(method))
            continue;
        ++index;

        struct funcdef *body = (struct funcdef *)method->func;
        std::string body_name =
            hidden_name("body", impl, method, index);
        std::string decorator_name =
            hidden_name("dec", impl, method, index);

        std::vector<struct ast *> decorators;
        collect_decorator_expressions(method->decorators, decorators);

        struct symlist *wrapper_params = copy_symlist(body->sl);
        move_defaults(body->sl, wrapper_params);

        body->is_method = 1;

        struct ast *wrapper_body = build_wrapper_body(
            decorator_name, body->sl, method->line, method->column);
        struct ast *wrapper = newfunc(NULL, wrapper_params, wrapper_body,
                                      false, false, false);
        wrapper->line = method->line;
        wrapper->column = method->column;

        struct ast *decorator_chain =
            build_decorator_chain(decorators, body_name);

        struct ast *body_let =
            newvar(copy_text(body_name), (struct ast *)body);
        struct ast *decorator_let =
            newvar(copy_text(decorator_name), decorator_chain, false, false,
                   new_type_constraint(TYPE_CALLABLE));
        decorator_let->line = method->line;
        decorator_let->column = method->column;

        prelude.push_back(body_let);
        prelude.push_back(decorator_let);

        struct ast *old_decorators = method->decorators;
        method->decorators = NULL;
        method->func = wrapper;
        free_decorator_wrappers(old_decorators);
    }

    struct ast *result = (struct ast *)impl;
    for (size_t i = prelude.size(); i > 0; --i)
        result = newast('L', prelude[i - 1], result);
    return result;
}
