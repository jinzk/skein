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

std::string function_hidden_name(const char *kind,
                                 const struct funcdef *func, int index) {
    std::string name = "$";
    name += kind ? kind : "hidden";
    name += "$func$";
    name += func && func->name ? func->name : "?";
    name += "$";
    name += std::to_string(func ? func->line : 0);
    name += "$";
    name += std::to_string(func ? func->column : 0);
    if (index > 0) {
        name += "$";
        name += std::to_string(index);
    }
    return name;
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
    copy->display = source->display;
    copy->detail = source->detail;
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
        /*
         * The hidden body is bound as an ordinary function, so remember the
         * impl type for static checking.  This keeps body type inference
         * (including yield/send types) equivalent to the original method
         * without adding a runtime parameter check.
         */
        if (body->sl && body->sl->sym && body->sl->sym->name &&
            std::strcmp(body->sl->sym->name, "self") == 0 &&
            !body->sl->type)
            body->method_self_type =
                new_named_type_constraint(impl->type_name, impl->module);

        bool body_is_generator = body->is_generator != 0;
        struct ast *wrapper_body = build_wrapper_body(
            decorator_name, body->sl, method->line, method->column);
        struct funcdef *wrapper = (struct funcdef *)newfunc(
            NULL, wrapper_params, wrapper_body, false, false);
        wrapper->line = method->line;
        wrapper->column = method->column;
        if (body_is_generator) {
            wrapper->decorated_generator_source = body;
            if (body->ret_type &&
                body->ret_type->kind == TYPE_GENERATOR)
                wrapper->ret_type =
                    copy_type_constraint(body->ret_type);
        }

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
        method->func = (struct ast *)wrapper;
        free_decorator_wrappers(old_decorators);
    }

    struct ast *result = (struct ast *)impl;
    for (size_t i = prelude.size(); i > 0; --i)
        result = newast('L', prelude[i - 1], result);
    return result;
}

struct ast *lower_function_decorators(struct ast *decorators,
                                      struct funcdef *func) {
    if (!decorators || !func || !func->name)
        return (struct ast *)func;

    std::vector<struct ast *> expressions;
    collect_decorator_expressions(decorators, expressions);
    if (expressions.empty())
        return (struct ast *)func;

    /*
     * Evaluate decorator expressions before binding the function name.
     * This matches Python's evaluation order and avoids a function named
     * like the decorator shadowing the decorator expression.
     */
    std::vector<struct ast *> prelude;
    std::vector<std::string> decorator_names;
    for (size_t i = 0; i < expressions.size(); ++i) {
        std::string name =
            function_hidden_name("d", func, static_cast<int>(i) + 1);
        decorator_names.push_back(name);
        struct ast *binding =
            newvar(copy_text(name), expressions[i], false, false,
                   new_type_constraint(TYPE_CALLABLE));
        binding->line = func->line;
        binding->column = func->column;
        prelude.push_back(binding);
    }

    std::string decorated_name = function_hidden_name("dec", func, 0);
    struct ast *current = make_ref(func->name);
    for (size_t i = decorator_names.size(); i > 0; --i) {
        current = call_with_argument(make_ref(decorator_names[i - 1]),
                                     current);
    }

    struct ast *decorated_binding =
        newvar(copy_text(decorated_name), current, false, false,
               new_type_constraint(TYPE_CALLABLE));
    decorated_binding->line = func->line;
    decorated_binding->column = func->column;

    struct ast *assign = newasgn(newref(copy_text(func->name)),
                                 make_ref(decorated_name));
    assign->line = func->line;
    assign->column = func->column;

    struct ast *result = newast('L', assign, newnullast());
    result = newast('L', decorated_binding, result);
    result = newast('L', (struct ast *)func, result);
    for (size_t i = prelude.size(); i > 0; --i)
        result = newast('L', prelude[i - 1], result);

    free_decorator_wrappers(decorators);
    return result;
}
