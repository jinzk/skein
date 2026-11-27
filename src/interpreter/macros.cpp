#include "macros.h"

#include "ast.h"
#include "builtin_ids.h"
#include "user_macros.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

char *copy_text(const std::string &text) {
    const std::size_t size = text.size() + 1;
    char *copy = (char *)std::malloc(size);
    if (!copy) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(copy, text.c_str(), size);
    return copy;
}

void set_location(struct ast *node, int line, int column) {
    if (!node)
        return;
    node->line = line;
    node->column = column;
}

bool macro_path(const struct ast *node, std::string &path) {
    if (!node)
        return false;
    if (node->nodetype == 'N') {
        const struct symref *ref = (const struct symref *)node;
        if (!ref->name)
            return false;
        path = ref->name;
        return true;
    }
    if (node->nodetype == 'j') {
        std::string base;
        if (!macro_path(node->l, base))
            return false;
        const struct val *field = (const struct val *)node->r;
        if (!field || field->nodetype != 'V' || !field->val ||
            field->val->type != T_string)
            return false;
        path = base + "." + ((const string_value_t *)field->val)->value;
        return true;
    }
    return false;
}

void collect_args(struct ast *node, std::vector<struct ast *> &out) {
    if (!node)
        return;
    if (node->nodetype == 'L') {
        collect_args(node->l, out);
        collect_args(node->r, out);
        return;
    }
    out.push_back(node);
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

std::vector<struct ast *> take_args(struct ast *args) {
    std::vector<struct ast *> values;
    collect_args(args, values);
    free_list_nodes(args);
    return values;
}

bool any_spread(const std::vector<struct ast *> &values) {
    for (struct ast *value : values) {
        if (value && value->nodetype == 'x')
            return true;
    }
    return false;
}

void free_values(const std::vector<struct ast *> &values) {
    for (struct ast *value : values)
        treefree(value);
}

struct ast *build_args(const std::vector<struct ast *> &values,
                       std::size_t begin) {
    if (begin >= values.size())
        return NULL;
    struct ast *result = values[begin];
    for (std::size_t i = begin + 1; i < values.size(); ++i)
        result = newast('L', result, values[i]);
    return result;
}

struct ast *builtin_call(int functype, struct ast *args, int line,
                         int column) {
    struct ast *call = newcall(newbuiltin(functype), args);
    set_location(call, line, column);
    return call;
}

struct ast *member_call(struct ast *base, const char *field,
                        struct ast *args, int line, int column) {
    struct ast *member = newast('j', base, newstr(field));
    set_location(member, line, column);
    struct ast *call = newcall(member, args);
    set_location(call, line, column);
    return call;
}

struct ast *string_arg(const char *text, int line, int column) {
    struct ast *value = newstr(text);
    set_location(value, line, column);
    return value;
}

bool literal_string(struct ast *node, std::string &text) {
    if (!node || node->nodetype != 'V')
        return false;
    const struct val *value = (const struct val *)node;
    if (!value->val || value->val->type != T_string)
        return false;
    text = ((const string_value_t *)value->val)->value;
    return true;
}

bool known_level(const std::string &level) {
    return level == "trace" || level == "debug" || level == "info" ||
           level == "warn" || level == "error";
}

/* Deliberately retained convenience bindings.  These are the only bare macro
 * names resolved by default; logging macros have no bare alias and must use
 * their std.log.*! canonical paths. */
std::string default_macro_path(const std::string &name) {
    static const struct {
        const char *alias;
        const char *canonical;
    } bindings[] = {
        {"capture", "std.result.capture"},
        {"fail", "std.error.fail"},
        {"recover", "std.error.recover"},
        {"pass", "std.error.pass"},
        {"wait", "std.task.wait"},
        {"iter", "std.utils.iter"},
    };
    for (const auto &entry : bindings) {
        if (name == entry.alias)
            return entry.canonical;
    }
    return name;
}

int level_builtin(const std::string &macro_name) {
    if (macro_name == "std.log.trace")
        return B_log_trace;
    if (macro_name == "std.log.debug")
        return B_log_debug;
    if (macro_name == "std.log.info")
        return B_log_info;
    if (macro_name == "std.log.warn")
        return B_log_warn;
    if (macro_name == "std.log.error")
        return B_log_error;
    return 0;
}

struct ast *fail_expansion(const char *name, const char *message,
                           struct ast *callee, struct ast *args,
                           struct ast *block) {
    yyerror("macro %s %s", name, message);
    treefree(callee);
    treefree(args);
    treefree(block);
    return NULL;
}

struct ast *wrap_enabled_call(int level_builtin_id, const char *level,
                              struct ast *args, int line, int column) {
    struct ast *enabled = builtin_call(B_log_enabled,
                                       string_arg(level, line, column),
                                       line, column);
    struct ast *body_call =
        builtin_call(level_builtin_id, args, line, column);
    struct ast *body = newscope(body_call);
    set_location(body, line, column);
    /* A ternary node is an expression, so the guard works in both statement
     * and expression positions and typechecking still visits the call. */
    struct ast *branch =
        newflow('T', enabled, body, newnullast());
    set_location(branch, line, column);
    set_location(branch->r, line, column);
    return branch;
}

struct ast *control_flow_macro(const std::string &name, struct ast *args,
                               struct ast *block, std::size_t arg_count,
                               int line, int column) {
    if (block)
        return fail_expansion(name.c_str(), "does not accept a block", NULL,
                              args, block);
    if (arg_count > 1)
        return fail_expansion(name.c_str(), "expects 0 or 1 arguments", NULL,
                              args, block);

    std::vector<struct ast *> values = take_args(args);
    if (any_spread(values)) {
        free_values(values);
        return fail_expansion(name.c_str(), "does not accept spread arguments",
                              NULL, NULL, NULL);
    }
    struct ast *value = values.empty() ? NULL : values[0];
    struct ast *result = NULL;
    if (name == "std.error.fail")
        result = newfail(value);
    else if (name == "std.error.recover")
        result = newrecover(value);
    else
        result = newpass(value);
    set_location(result, line, column);
    return result;
}

struct ast *wait_macro(struct ast *args, struct ast *block,
                       std::size_t arg_count, int line, int column) {
    if (block)
        return fail_expansion("wait", "does not accept a block", NULL, args,
                              block);
    if (arg_count != 1)
        return fail_expansion("wait", "expects 1 expression argument", NULL,
                              args, block);

    std::vector<struct ast *> values = take_args(args);
    if (any_spread(values)) {
        free_values(values);
        return fail_expansion("wait", "does not accept spread arguments", NULL,
                              NULL, NULL);
    }
    struct ast *result = new_wait_call(values[0]);
    set_location(result, line, column);
    return result;
}

} // namespace

struct ast *expand_builtin_macro(struct ast *callee, struct ast *args,
                                 struct ast *block, int line, int column,
                                 bool is_generator) {
    std::string name;
    if (!macro_path(callee, name)) {
        yyerror("macro name must be an identifier or dotted path");
        treefree(callee);
        treefree(args);
        treefree(block);
        return NULL;
    }
    name = default_macro_path(name);

    std::vector<struct ast *> checked_args;
    collect_args(args, checked_args);
    for (struct ast *value : checked_args) {
        if (value && value->nodetype == NODE_MACRO_TYPE_ARG) {
            yyerror("macro %s does not accept type arguments", name.c_str());
            treefree(callee);
            treefree(args);
            treefree(block);
            return NULL;
        }
    }
    const std::size_t arg_count = checked_args.size();

    treefree(callee);

    if (name == "std.utils.iter") {
        if (block)
            return fail_expansion(name.c_str(), "does not accept a block",
                                  NULL, args, block);
        if (arg_count != 1)
            return fail_expansion(name.c_str(),
                                  "expects exactly 1 expression argument",
                                  NULL, args, block);
        std::vector<struct ast *> values = take_args(args);
        if (any_spread(values)) {
            free_values(values);
            return fail_expansion(name.c_str(),
                                  "does not accept spread arguments", NULL,
                                  NULL, NULL);
        }
        return member_call(values[0], "iter", NULL, line, column);
    }

    if (name == "std.assert.that") {
        if (block)
            return fail_expansion(name.c_str(), "does not accept a block",
                                  NULL, args, block);
        if (arg_count < 1 || arg_count > 2)
            return fail_expansion(name.c_str(),
                                  "expects 1 or 2 expression arguments",
                                  NULL, args, block);
        std::vector<struct ast *> values = take_args(args);
        if (any_spread(values)) {
            free_values(values);
            return fail_expansion(name.c_str(),
                                  "does not accept spread arguments", NULL,
                                  NULL, NULL);
        }
        return builtin_call(B_assert_that, build_args(values, 0), line,
                            column);
    }

    if (name == "std.assert.schema") {
        if (block)
            return fail_expansion(name.c_str(), "does not accept a block",
                                  NULL, args, block);
        if (arg_count != 2)
            return fail_expansion(name.c_str(),
                                  "expects 2 expression arguments", NULL,
                                  args, block);
        std::vector<struct ast *> values = take_args(args);
        if (any_spread(values)) {
            free_values(values);
            return fail_expansion(name.c_str(),
                                  "does not accept spread arguments", NULL,
                                  NULL, NULL);
        }
        return builtin_call(B_assert_schema, build_args(values, 0), line,
                            column);
    }

    const int level_id = level_builtin(name);
    if (level_id != 0) {
        if (block)
            return fail_expansion(name.c_str(), "does not accept a block",
                                  NULL, args, block);
        if (arg_count < 1 || arg_count > 2)
            return fail_expansion(name.c_str(),
                                  "expects 1 or 2 expression arguments",
                                  NULL, args, block);
        const std::string level =
            name.rfind("std.log.", 0) == 0 ? name.substr(8) : name;
        std::vector<struct ast *> values = take_args(args);
        if (any_spread(values)) {
            free_values(values);
            return fail_expansion(name.c_str(),
                                  "does not accept spread arguments", NULL,
                                  NULL, NULL);
        }
        return wrap_enabled_call(level_id, level.c_str(),
                                 build_args(values, 0), line, column);
    }

    if (name == "std.log.log_to") {
        if (block)
            return fail_expansion(name.c_str(), "does not accept a block",
                                  NULL, args, block);
        if (arg_count < 3 || arg_count > 4)
            return fail_expansion(name.c_str(),
                                  "expects 3 or 4 expression arguments",
                                  NULL, args, block);
        std::vector<struct ast *> values = take_args(args);
        if (any_spread(values)) {
            free_values(values);
            return fail_expansion(name.c_str(),
                                  "does not accept spread arguments", NULL,
                                  NULL, NULL);
        }
        std::string level;
        if (!literal_string(values[1], level) || !known_level(level)) {
            free_values(values);
            return fail_expansion(name.c_str(),
                                  "expects a literal log level", NULL, NULL,
                                  NULL);
        }

        const std::string hidden =
            "$hidden$macro$log_to$" + std::to_string(line) + "$" +
            std::to_string(column);
        struct ast *binding = newvar(copy_text(hidden), values[0]);
        set_location(binding, line, column);
        struct ast *enabled = member_call(newref(copy_text(hidden)), "enabled",
                                          string_arg(level.c_str(), line,
                                                     column),
                                          line, column);
        std::vector<struct ast *> log_values;
        /* Reuse the level literal for logger.log; the guard keeps its own copy. */
        log_values.push_back(values[1]);
        for (std::size_t i = 2; i < values.size(); ++i)
            log_values.push_back(values[i]);
        struct ast *body_call = member_call(newref(copy_text(hidden)), "log",
                                            build_args(log_values, 0), line,
                                            column);
        struct ast *body = newscope(body_call);
        set_location(body, line, column);
        struct ast *branch = newflow('T', enabled, body, newnullast());
        set_location(branch, line, column);
        set_location(branch->r, line, column);
        struct ast *sequence = newast('L', binding, branch);
        set_location(sequence, line, column);
        struct ast *scope = newscope(sequence);
        set_location(scope, line, column);
        return scope;
    }

    if (name == "std.result.capture") {
        if (!block)
            return fail_expansion(name.c_str(), "expects a block", NULL,
                                  args, NULL);
        if (arg_count != 0)
            return fail_expansion(name.c_str(),
                                  "does not accept expression arguments",
                                  NULL, args, block);
        treefree(args);
        set_location(block, line, column);
        struct ast *thunk =
            newfunc(NULL, NULL, block, false, is_generator, NULL);
        set_location(thunk, line, column);
        return builtin_call(B_result_capture, thunk, line, column);
    }

    if (name == "std.error.fail" || name == "std.error.recover" ||
        name == "std.error.pass")
        return control_flow_macro(name, args, block, arg_count, line, column);

    if (name == "std.task.wait")
        return wait_macro(args, block, arg_count, line, column);

    /* Syntax-only tools skip macro resolution, so an imported macro is not
     * bound yet.  Keep the call as a deferred node instead of reporting a
     * false unknown-macro diagnostic. */
    if (!g_resolve_imported_macros)
        return new_user_macro_call(newref(copy_text(name.c_str())), args,
                                   block, line, column, is_generator);
    return fail_expansion(name.c_str(),
                          "is not a known built-in or user-defined macro", NULL,
                          args, block);
}
