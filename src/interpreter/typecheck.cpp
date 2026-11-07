#include "typecheck.h"

#include "typecheck_builtins.h"
#include "typecheck_state.h"
#include "typecheck_types.h"

#include "ast.h"
#include "ast_analysis.h"
#include "diagnostics.h"
#include "scopestack.h"
#include "static_type.h"
#include "structtype.h"
#include "valuetype.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skein {
namespace typecheck {

using namespace detail;

namespace {

Mode g_mode = Mode::Warn;

struct Checker : public BuiltinContext {
    std::string source;
    std::string module;
    std::unordered_map<std::string, FunctionSig> functions;
    std::unordered_map<std::string, StaticTypeInfo> static_types;
    std::vector<const TypeNode *> return_types;
    std::unordered_set<std::string> warnings;
    bool error_ = false;

    void report_error(int line, int column, const char *message) {
        if (error_)
            return;
        diag_report(line, column, "type error", message);
        error_ = true;
    }

    void report_warning(int line, int column, const char *message) {
        if (error_)
            return;
        std::string key = std::to_string(line);
        key += ':';
        key += std::to_string(column);
        key += ':';
        key += message ? message : "";
        if (!warnings.insert(key).second)
            return;
        diag_report_warning(line, column, "type warning", message);
    }

    explicit Checker(const std::string &source_name,
                     const std::string &module_name)
        : source(source_name), module(module_name) {}

    VarInfo lookup_var(const std::string &name, Env &env) {
        if (const VarInfo *info = env.lookup(name))
            return *info;
        struct symbol *sym = lookupsym(name.c_str());
        VarInfo info;
        if (!sym) {
            info.type = type_any();
            return info;
        }
        info.type = type_from_constraint(&sym->type);
        info.fixed = sym->type.kind != TYPE_ANY;
        return info;
    }

    const TypeNode *lookup_name(const std::string &name, Env &env) {
        return lookup_var(name, env).type;
    }

    bool warn_or_error(const TypeNode *actual, const TypeNode *expected,
                       int line, int column, bool explicit_cast,
                       bool force_element) {
        if (!force_element && !needs_static_check(expected))
            return true;
        if (!expected || expected->kind == TYPE_ANY)
            return true;

        MatchResult result = match_type(actual, expected);
        if (result == MatchResult::Ok)
            return true;

        std::string actual_text = type_display(actual);
        std::string expected_text = type_display(expected);
        if (type_contains_vars(actual) && expected &&
            !type_contains_vars(expected)) {
            TypeBindings bindings;
            bind_type(expected, actual, bindings);
            const TypeNode *instantiated =
                instantiate_type(actual, bindings);
            if (instantiated != actual)
                actual_text = type_display(instantiated);
        }
        char message[512];
        if (result == MatchResult::Mismatch) {
            std::snprintf(message, sizeof(message),
                          "expected %s, got %s", expected_text.c_str(),
                          actual_text.c_str());
            report_error(line, column, message);
            return false;
        }

        if (explicit_cast || g_mode == Mode::Off || g_mode == Mode::Loose)
            return true;

        std::snprintf(message, sizeof(message),
                      "implicit %s assigned to %s; use 'as %s' if intentional",
                      actual_text.c_str(), expected_text.c_str(),
                      expected_text.c_str());
        if (g_mode == Mode::Strict) {
            report_error(line, column, message);
            return false;
        }
        report_warning(line, column, message);
        return true;
    }

    bool check_return_expr(struct ast *expr, const TypeNode *expected,
                           Env &env, int line, int column) {
        if (!expected || expected->kind == TYPE_ANY) {
            if (expr)
                infer(expr, env);
            return true;
        }
        if (needs_static_check(expected))
            return check_expr(expr, expected, env, false, true);

        const TypeNode *actual =
            expr ? infer(expr, env) : type_builtin(TYPE_NULL);
        MatchResult result = match_type(actual, expected);
        if (result != MatchResult::Mismatch)
            return true;

        char message[512];
        std::snprintf(message, sizeof(message), "expected %s, got %s",
                      type_display(expected), type_display(actual));
        report_error(line, column, message);
        return false;
    }

    bool check_expr(struct ast *expr, const TypeNode *expected, Env &env,
                    bool explicit_cast = false, bool force_element = false) {
        if (!expr)
            return true;
        if (!force_element && !needs_static_check(expected)) {
            infer(expr, env);
            return true;
        }

        if (expr->nodetype == NODE_CAST) {
            struct cast_node *cast = (struct cast_node *)expr;
            const TypeNode *target = type_from_constraint(cast->type);
            if (!check_expr(cast->value, target, env, true,
                            needs_static_check(target)))
                return false;
            return warn_or_error(target, expected, expr->line, expr->column,
                                 needs_static_check(target), force_element);
        }

        if (expected && type_is_function_signature(expected) &&
            expr->nodetype == 'Z') {
            struct funcdef *def = (struct funcdef *)expr;
            check_funcdef(def, env);
            const TypeNode *actual =
                function_type_from_def(def, env, expected);
            return warn_or_error(actual, expected, expr->line,
                                 expr->column, explicit_cast,
                                 force_element);
        }

        if (expected && expected->kind == TYPE_LIST &&
            expr->nodetype == 'l') {
            const TypeNode *element =
                expected->args.empty() ? type_any() : expected->args[0];
            for (struct list_node *item = (struct list_node *)expr; item;
                 item = (struct list_node *)item->next) {
                if (!check_expr(item->value, element, env, explicit_cast,
                                true))
                    return false;
            }
            return true;
        }

        if (expected && expected->kind == TYPE_DICT &&
            expr->nodetype == 'd') {
            const TypeNode *key_type =
                expected->args.empty() ? type_any() : expected->args[0];
            const TypeNode *value_type =
                expected->args.size() < 2 ? type_any() : expected->args[1];
            for (struct dict_node *entry = (struct dict_node *)expr; entry;
                 entry = (struct dict_node *)entry->next) {
                if (!check_expr(entry->key, key_type, env, explicit_cast,
                                true) ||
                    !check_expr(entry->value, value_type, env, explicit_cast,
                                true))
                    return false;
            }
            return true;
        }

        if (expected && expected->kind == TYPE_LIST &&
            expr->nodetype == '+') {
            if (!check_expr(expr->l, expected, env, explicit_cast))
                return false;
            return check_expr(expr->r, expected, env, explicit_cast);
        }

        if (expr->nodetype == 'C') {
            const TypeNode *result = type_any();
            if (!check_call(expr, expected, env, result) || error_)
                return false;
            return warn_or_error(result, expected, expr->line,
                                 expr->column, explicit_cast,
                                 force_element);
        }

        const TypeNode *actual = infer(expr, env);
        return warn_or_error(actual, expected, expr->line, expr->column,
                             explicit_cast, force_element);
    }

    bool check_lvalue(struct ast *target, struct ast *value, Env &env,
                      bool compound) {
        if (!target)
            return true;
        const TypeNode *target_type = type_any();
        if (!resolve_lvalue(target, env, target_type))
            return false;
        if (!value)
            return true;
        bool force_element =
            target->nodetype == 'G' || needs_static_check(target_type);
        (void)compound;
        return check_expr(value, target_type, env, false, force_element);
    }

    bool resolve_lvalue(struct ast *node, Env &env,
                        const TypeNode *&out_type) {
        if (!node) {
            out_type = type_any();
            return true;
        }
        if (node->nodetype == 'N') {
            out_type = lookup_name(((struct symref *)node)->name, env);
            return true;
        }
        if (node->nodetype == NODE_CAST) {
            report_error(node->line, node->column,
                         "cannot assign through 'as'");
            out_type = type_any();
            return false;
        }
        if (node->nodetype == 'j') {
            const TypeNode *base_type = type_any();
            if (!resolve_lvalue(node->l, env, base_type))
                return false;
            out_type = field_type(base_type, node, env);
            return true;
        }
        if (node->nodetype == 'G') {
            const TypeNode *base_type = type_any();
            if (!resolve_lvalue(node->l, env, base_type))
                return false;
            if (base_type->kind == TYPE_DICT) {
                const TypeNode *key_type =
                    base_type->args.empty() ? type_any()
                                            : base_type->args[0];
                if (!check_expr(node->r, key_type, env, false, true))
                    return false;
                out_type = base_type->args.size() < 2
                               ? type_any()
                               : base_type->args[1];
                return true;
            }
            if (base_type->kind == TYPE_LIST ||
                base_type->kind == TYPE_DEQUE) {
                if (!check_expr(node->r, type_builtin(TYPE_INT), env, false,
                                false))
                    return false;
                out_type = base_type->args.empty() ? type_any()
                                                   : base_type->args[0];
                return true;
            }
            infer(node->l, env);
            infer(node->r, env);
            out_type = type_any();
            return true;
        }
        infer(node, env);
        out_type = type_any();
        return true;
    }

    bool lookup_type_node(const TypeNode *type,
                          structtype_lookup_result &out) {
        return lookup_named_type(type, out);
    }

    static std::string static_type_key(const char *source,
                                       const char *name) {
        std::string key = source ? source : "";
        key += '\x1f';
        key += name ? name : "";
        return key;
    }

    StaticTypeInfo *find_static_info(const TypeNode *type) {
        if (!type || type->kind != TYPE_NAMED)
            return NULL;
        auto it = static_types.find(
            static_type_key(type->module.c_str(), type->name.c_str()));
        return it == static_types.end() ? NULL : &it->second;
    }

    const TypeNode *static_field_type(const TypeNode *type,
                                      const char *field) {
        StaticTypeInfo *info = find_static_info(type);
        if (!info)
            return type_any();
        auto it = info->fields.find(field ? field : "");
        return it == info->fields.end() ? type_any() : it->second;
    }

    struct funcdef *static_method(const TypeNode *type, const char *name) {
        StaticTypeInfo *info = find_static_info(type);
        if (!info)
            return NULL;
        auto it = info->methods.find(name ? name : "");
        return it == info->methods.end() ? NULL : it->second;
    }

    const TypeNode *method_value_type(const TypeNode *base_type,
                                      const char *name) {
        if (!base_type || base_type->kind != TYPE_NAMED || !name)
            return NULL;

        structtype_lookup_result lookup;
        if (lookup_type_node(base_type, lookup)) {
            const type_def *def =
                lookup.structure
                    ? static_cast<const type_def *>(lookup.structure)
                    : static_cast<const type_def *>(lookup.enumeration);
            if (def) {
                auto it = def->methods.find(name);
                if (it != def->methods.end() && it->second &&
                    it->second->code)
                    return function_type_from_sig(
                        method_signature_from_code(
                            it->second->code.get()));
            }
        }

        struct funcdef *method = static_method(base_type, name);
        if (method)
            return function_type_from_sig(
                method_signature_from_def(method));
        return NULL;
    }

    const TypeNode *field_type(const TypeNode *base_type,
                               const struct ast *field_node, Env &env) {
        (void)env;
        if (!base_type || base_type->kind != TYPE_NAMED)
            return type_any();
        const struct ast *right = field_node->r;
        if (!right || right->nodetype != 'V')
            return type_any();
        const struct value_t *value = ((const struct val *)right)->val;
        if (!value || value->type != T_string)
            return type_any();
        const char *field =
            ((const struct string_value_t *)value)->value;
        structtype_lookup_result lookup;
        if (!lookup_type_node(base_type, lookup) || !lookup.structure)
            return static_field_type(base_type, field);
        struct_def *def = lookup.structure;
        auto it = def->field_index.find(field);
        if (it == def->field_index.end())
            return static_field_type(base_type, field);
        return type_from_constraint(&def->fields[(size_t)it->second].type);
    }

    bool check_direct_argument(struct ast *expr, const TypeNode *expected,
                               Env &env, int line, int column) {
        if (!expected || expected->kind == TYPE_ANY) {
            infer(expr, env);
            return true;
        }
        if (needs_static_check(expected))
            return check_expr(expr, expected, env, false, true);

        const TypeNode *actual = infer(expr, env);
        if (match_type(actual, expected) != MatchResult::Mismatch)
            return true;
        char message[512];
        std::snprintf(message, sizeof(message), "expected %s, got %s",
                      type_display(expected), type_display(actual));
        report_error(line, column, message);
        return false;
    }

    bool check_call_args(const FunctionSig &sig,
                         const std::vector<struct ast *> &args,
                         Env &env, bool check_concrete = false) {
        bool has_spread = false;
        for (struct ast *arg : args) {
            if (arg && arg->nodetype == 'x') {
                has_spread = true;
                break;
            }
        }
        if (has_spread) {
            for (struct ast *arg : args)
                infer(arg, env);
            return true;
        }
        if (!sig.variadic && args.size() > sig.params.size()) {
            std::size_t extra = sig.params.size();
            char message[256];
            if (sig.optional_params > 0) {
                std::snprintf(
                    message, sizeof(message),
                    "function expects at most %zu argument(s), got %zu",
                    sig.params.size(), args.size());
            } else {
                std::snprintf(message, sizeof(message),
                              "function expects %zu argument(s), got %zu",
                              sig.params.size(), args.size());
            }
            report_error(args[extra] ? args[extra]->line : 0,
                         args[extra] ? args[extra]->column : 0, message);
            return false;
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            const TypeNode *param =
                i < sig.params.size()
                    ? sig.params[i]
                    : (sig.variadic ? sig.rest : type_any());
            if (check_concrete) {
                if (!check_direct_argument(args[i], param, env,
                                           args[i] ? args[i]->line : 0,
                                           args[i] ? args[i]->column : 0))
                    return false;
            } else if (!check_expr(args[i], param, env, false, false)) {
                return false;
            }
        }
        return true;
    }

    bool check_method_call(struct ast *callee,
                           const std::vector<struct ast *> &args,
                           Env &env, const TypeNode *&result) {
        if (!callee || callee->nodetype != 'j')
            return false;
        const struct ast *right = callee->r;
        if (!right || right->nodetype != 'V')
            return false;
        const struct value_t *field_value = ((const struct val *)right)->val;
        if (!field_value || field_value->type != T_string)
            return false;
        const char *method_name =
            ((const struct string_value_t *)field_value)->value;

        const TypeNode *receiver = infer(callee->l, env);
        FunctionSig sig;
        bool found = false;
        structtype_lookup_result lookup;
        if (lookup_type_node(receiver, lookup)) {
            const type_def *def =
                lookup.structure
                    ? static_cast<const type_def *>(lookup.structure)
                    : static_cast<const type_def *>(lookup.enumeration);
            if (def) {
                auto it = def->methods.find(method_name);
                if (it != def->methods.end() && it->second &&
                    it->second->code) {
                    sig = method_signature_from_code(
                        it->second->code.get());
                    found = true;
                }
            }
        }
        if (!found) {
            struct funcdef *method = static_method(receiver, method_name);
            if (method) {
                sig = method_signature_from_def(method);
                found = true;
            }
        }
        if (!found)
            return false;

        if (!check_call_args(sig, args, env, true))
            return false;
        result = signature_result_type(sig);
        return true;
    }

    bool check_namespace_call(const std::string &path,
                              const std::vector<struct ast *> &args,
                              Env &env, const TypeNode *&result) {
        struct value_t *value = NULL;
        if (!resolve_namespace_value(path, value) || !value)
            return false;
        if (value->type != T_func)
            return false;
        struct func_value_t *function = (struct func_value_t *)value;
        if (!function->def || !function->def->code)
            return false;
        FunctionSig sig = signature_from_symlist(function->def->code->sl);
        sig.result = function->def->code->ret_type
                         ? type_from_constraint(
                               function->def->code->ret_type.get())
                         : type_any();
        sig.is_async = function->def->code->is_async;
        sig.is_generator = function->def->code->is_generator;
        if (!check_call_args(sig, args, env, true))
            return false;
        result = signature_result_type(sig);
        return true;
    }

    bool check_polymorphic_call(const TypeNode *callee_type,
                                const std::vector<struct ast *> &args,
                                Env &env, const TypeNode *&result) {
        TypeBindings bindings;
        int count = type_function_param_count(callee_type);
        bool has_spread = false;
        for (struct ast *arg : args) {
            if (arg && arg->nodetype == 'x') {
                has_spread = true;
                break;
            }
        }
        if (has_spread) {
            for (struct ast *arg : args)
                infer(arg, env);
            result = instantiate_type(type_function_result(callee_type),
                                      bindings);
            return true;
        }
        const TypeNode *compose_type =
            builtin_polymorphic_type("std.fn.compose");
        const TypeNode *pipe_type =
            builtin_polymorphic_type("std.fn.pipe");
        const char *binary_name = callee_type == compose_type
                                      ? "std.fn.compose"
                                      : (callee_type == pipe_type
                                             ? "std.fn.pipe"
                                             : nullptr);
        if (binary_name && args.size() != 2) {
            char message[256];
            std::snprintf(message, sizeof(message),
                          "%s expects exactly 2 argument(s), got %zu",
                          binary_name, args.size());
            int line = args.size() > 2
                           ? args[2]->line
                           : (args.empty() ? 0 : args[0]->line);
            int column = args.size() > 2
                             ? args[2]->column
                             : (args.empty() ? 0 : args[0]->column);
            report_error(line, column, message);
            return false;
        }
        if (!type_function_is_variadic(callee_type) &&
            args.size() > static_cast<std::size_t>(count)) {
            std::size_t extra = static_cast<std::size_t>(count);
            char message[256];
            if (type_function_optional_params(callee_type) > 0) {
                std::snprintf(
                    message, sizeof(message),
                    "function expects at most %d argument(s), got %zu",
                    count, args.size());
            } else {
                std::snprintf(message, sizeof(message),
                              "function expects %d argument(s), got %zu",
                              count, args.size());
            }
            report_error(args[extra] ? args[extra]->line : 0,
                         args[extra] ? args[extra]->column : 0, message);
            return false;
        }

        std::vector<const TypeNode *> actual_types(args.size(), NULL);
        for (std::size_t i = 0; i < args.size(); ++i) {
            struct ast *arg = args[i];
            const TypeNode *pattern =
                i < static_cast<std::size_t>(count)
                    ? type_function_param(callee_type, i)
                    : NULL;
            if (arg->nodetype == 'Z' && pattern &&
                type_is_function_signature(pattern)) {
                const TypeNode *expected_pattern =
                    instantiate_type(pattern, bindings);
                struct funcdef *def = (struct funcdef *)arg;
                check_funcdef(def, env);
                actual_types[i] = function_type_from_def(
                    def, env, expected_pattern);
            } else {
                actual_types[i] = infer(arg, env);
            }
            if (pattern &&
                !bind_type(actual_types[i], pattern, bindings)) {
                const TypeNode *expected_pattern =
                    instantiate_type(pattern, bindings);
                char message[512];
                std::snprintf(message, sizeof(message),
                              "expected %s, got %s",
                              type_display(expected_pattern),
                              type_display(actual_types[i]));
                report_error(arg->line, arg->column, message);
                return false;
            }
        }

        for (std::size_t i = 0; i < args.size(); ++i) {
            const TypeNode *pattern =
                i < static_cast<std::size_t>(count)
                    ? type_function_param(callee_type, i)
                    : NULL;
            const TypeNode *expected_pattern =
                pattern ? instantiate_type(pattern, bindings) : type_any();
            if (!check_expr(args[i], expected_pattern, env, false,
                            needs_static_check(expected_pattern)))
                return false;
        }
        result = instantiate_type(type_function_result(callee_type),
                                  bindings);
        return true;
    }

    bool check_call(struct ast *expr, const TypeNode *expected, Env &env,
                    const TypeNode *&result) {
        struct ufncall *call = (struct ufncall *)expr;
        result = type_any();
        std::vector<struct ast *> args;
        collect_call_args(call->args, args);

        std::string path;
        bool has_path = flatten_path(call->callee, path);
        std::string canonical =
            has_path ? canonical_builtin_path(path, source)
                     : std::string();
        bool spread = false;
        for (struct ast *arg : args) {
            if (arg && arg->nodetype == 'x') {
                spread = true;
                break;
            }
        }

        if (!spread &&
            (canonical == "std.fn.compose" ||
             canonical == "std.fn.pipe") &&
            args.size() != 2) {
            char message[256];
            std::snprintf(message, sizeof(message),
                          "%s expects exactly 2 argument(s), got %zu",
                          canonical.c_str(), args.size());
            int line = args.size() > 2 ? args[2]->line
                                       : (call->callee ? call->callee->line
                                                       : 0);
            int column = args.size() > 2
                             ? args[2]->column
                             : (call->callee ? call->callee->column : 0);
            report_error(line, column, message);
            return false;
        }

        /*
         * Enforce the maximum arity of known builtin templates even when the
         * call is otherwise deferred to the runtime builtin.  Optional
         * trailing parameters are included in the template, so this only
         * rejects genuinely extra arguments.
         */
        if (!canonical.empty()) {
            const TypeNode *polymorphic =
                builtin_polymorphic_type(canonical);
            int maximum = type_function_param_count(polymorphic);
            if (!spread && maximum >= 0 && !type_function_is_variadic(
                                              polymorphic) &&
                args.size() > static_cast<std::size_t>(maximum)) {
                std::size_t extra = static_cast<std::size_t>(maximum);
                char message[256];
                if (type_function_optional_params(polymorphic) > 0) {
                    std::snprintf(
                        message, sizeof(message),
                        "function expects at most %d argument(s), got %zu",
                        maximum, args.size());
                } else {
                    std::snprintf(
                        message, sizeof(message),
                        "function expects %d argument(s), got %zu",
                        maximum, args.size());
                }
                report_error(args[extra] ? args[extra]->line : 0,
                             args[extra] ? args[extra]->column : 0,
                             message);
                return false;
            }
        }

        /*
         * Container builtin wrappers in std.list/std.dict/... carry better
         * hand-written signatures than their generic adapter function
         * values, so try the builtin table before using a function value's
         * own signature.
         */
        bool allow_builtin_check =
            has_path &&
            (canonical.rfind("std.iter.", 0) == 0 ||
             needs_static_check(expected) ||
             (!args.empty() &&
              expr_has_declared_container(args[0], env)));
        if (allow_builtin_check) {
            const TypeNode *builtin_result = type_any();
            if (check_builtin(*this, canonical, args, expected, env,
                              builtin_result)) {
                result = builtin_result;
                return true;
            }
        }

        if (canonical == "std.fn.compose" || canonical == "std.fn.pipe") {
            for (struct ast *arg : args) {
                const TypeNode *actual = infer(arg, env);
                if (actual->kind == TYPE_FUNCTION ||
                    actual->kind == TYPE_BUILTIN ||
                    actual->kind == TYPE_CALLABLE ||
                    actual->kind == TYPE_ANY)
                    continue;
                char message[512];
                std::snprintf(message, sizeof(message),
                              "%s expects callables, got %s",
                              canonical.c_str(), type_display(actual));
                report_error(arg->line, arg->column, message);
                return false;
            }
        }

        if (call->callee) {
            const TypeNode *callee_type = infer(call->callee, env);
            if (type_is_function_signature(callee_type)) {
                if (type_contains_vars(callee_type)) {
                    /*
                     * For a direct std.list/std.dict/... call without enough
                     * annotation context, keep the historical behaviour:
                     * defer invalid base types to the runtime builtin.
                     */
                    bool container_builtin_path =
                        canonical.rfind("std.list.", 0) == 0 ||
                        canonical.rfind("std.dict.", 0) == 0 ||
                        canonical.rfind("std.set.", 0) == 0 ||
                        canonical.rfind("std.deque.", 0) == 0 ||
                        canonical.rfind("std.heap.", 0) == 0;
                    if (!container_builtin_path || allow_builtin_check) {
                        if (!check_polymorphic_call(callee_type, args, env,
                                                    result))
                            return false;
                        return true;
                    }
                } else {
                    FunctionSig sig;
                    int count = type_function_param_count(callee_type);
                    for (int i = 0; i < count; ++i)
                        sig.params.push_back(
                            type_function_param(callee_type,
                                                (std::size_t)i));
                    sig.result = type_function_result(callee_type);
                    sig.variadic = type_function_is_variadic(callee_type);
                    sig.optional_params =
                        type_function_optional_params(callee_type);
                    if (sig.variadic)
                        sig.rest = type_function_rest(callee_type);
                    if (!check_call_args(sig, args, env, true))
                        return false;
                    result = sig.result ? sig.result : type_any();
                    return true;
                }
            }
        }

        if (has_path && path.find('.') == std::string::npos) {
            FunctionSig sig;
            bool found = false;
            auto it = functions.find(path);
            if (it != functions.end()) {
                sig = it->second;
                found = true;
            } else {
                struct symbol *sym = lookupsym(path.c_str());
                if (sym && sym->val && sym->val->type == T_func) {
                    struct func_value_t *fn =
                        (struct func_value_t *)sym->val;
                    if (fn->def && fn->def->code) {
                        sig = signature_from_symlist(fn->def->code->sl);
                        sig.result =
                            fn->def->code->ret_type
                                ? type_from_constraint(
                                      fn->def->code->ret_type.get())
                                : type_any();
                        sig.is_async = fn->def->code->is_async;
                        sig.is_generator =
                            fn->def->code->is_generator;
                        found = true;
                    }
                }
            }
            if (found) {
                if (!check_call_args(sig, args, env, true))
                    return false;
                result = signature_result_type(sig);
                return true;
            }
        }

        if (has_path && path.find('.') != std::string::npos) {
            if (check_namespace_call(path, args, env, result))
                return true;
        }

        if (call->callee && call->callee->nodetype == 'j') {
            if (check_method_call(call->callee, args, env, result))
                return true;
        }

        if (call->callee)
            infer(call->callee, env);
        for (struct ast *arg : args)
            infer(arg, env);
        result = type_any();
        return true;
    }

    static struct value_t *namespace_field(struct value_t *value,
                                           const std::string &name) {
        if (!value || value->type != T_dict)
            return NULL;
        struct dict_value_t *dict = (struct dict_value_t *)value;
        for (auto &entry : dict->payload->items) {
            if (entry.first && entry.first->type == T_string &&
                name == ((struct string_value_t *)entry.first)->value)
                return entry.second;
        }
        return NULL;
    }

    bool resolve_namespace_value(const std::string &path,
                                 struct value_t *&out) {
        out = NULL;
        std::size_t dot = path.find('.');
        if (dot == std::string::npos)
            return false;
        std::string root = path.substr(0, dot);
        struct symbol *sym = lookupsym(root.c_str());
        if (!sym || !sym->val || sym->val->type != T_dict)
            return false;
        struct dict_value_t *dict = (struct dict_value_t *)sym->val;
        if (!dict->payload || !dict->payload->is_namespace)
            return false;

        struct value_t *current = sym->val;
        std::size_t start = dot + 1;
        while (start <= path.size()) {
            std::size_t next = path.find('.', start);
            std::string component =
                next == std::string::npos
                    ? path.substr(start)
                    : path.substr(start, next - start);
            if (component.empty())
                return false;
            struct value_t *found = namespace_field(current, component);
            if (!found)
                return false;
            if (next == std::string::npos) {
                out = found;
                return true;
            }
            if (found->type != T_dict)
                return false;
            struct dict_value_t *next_dict =
                (struct dict_value_t *)found;
            if (!next_dict->payload || !next_dict->payload->is_namespace)
                return false;
            current = found;
            start = next + 1;
        }
        return false;
    }

    bool resolve_namespace_type(const std::string &path,
                                const TypeNode *&type,
                                bool *fixed = NULL) {
        type = NULL;
        if (fixed)
            *fixed = false;
        std::size_t dot = path.find('.');
        if (dot == std::string::npos)
            return false;
        std::string root = path.substr(0, dot);
        auto source_it = g_import_aliases.find(source);
        if (source_it == g_import_aliases.end())
            return false;
        auto alias_it = source_it->second.find(root);
        if (alias_it == source_it->second.end())
            return false;

        std::string module = alias_it->second;
        std::string rest = path.substr(dot + 1);
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= rest.size()) {
            std::size_t next = rest.find('.', start);
            parts.push_back(next == std::string::npos
                                ? rest.substr(start)
                                : rest.substr(start, next - start));
            if (next == std::string::npos)
                break;
            start = next + 1;
        }

        for (std::size_t split = 0; split < parts.size(); ++split) {
            std::string candidate_module = module;
            for (std::size_t i = 0; i < split; ++i) {
                candidate_module += '.';
                candidate_module += parts[i];
            }
            std::string name;
            for (std::size_t i = split; i < parts.size(); ++i) {
                if (!name.empty())
                    name += '.';
                name += parts[i];
            }
            auto module_it = g_exported_types.find(candidate_module);
            if (module_it == g_exported_types.end())
                continue;
            auto name_it = module_it->second.find(name);
            if (name_it != module_it->second.end() &&
                name_it->second.type) {
                type = name_it->second.type;
                if (fixed)
                    *fixed = name_it->second.fixed;
                return true;
            }
        }
        return false;
    }

    bool expr_fixed(struct ast *expr, Env &env) {
        if (!expr)
            return false;
        if (expr->nodetype == 'N') {
            const char *name = ((struct symref *)expr)->name;
            return lookup_var(name ? name : "", env).fixed;
        }
        if (expr->nodetype == 'j') {
            std::string path;
            const TypeNode *type = NULL;
            bool fixed = false;
            if (flatten_path(expr, path) &&
                resolve_namespace_type(path, type, &fixed))
                return fixed;
            return expr_fixed(expr->l, env);
        }
        if (expr->nodetype == 'G')
            return expr_fixed(expr->l, env);
        if (expr->nodetype == NODE_CAST) {
            struct cast_node *cast = (struct cast_node *)expr;
            return needs_static_check(type_from_constraint(cast->type));
        }
        return false;
    }

    bool expr_has_declared_container(struct ast *expr, Env &env) {
        if (!expr || !expr_fixed(expr, env))
            return false;
        if (expr->nodetype == 'N') {
            const char *name = ((struct symref *)expr)->name;
            return needs_static_check(lookup_name(name ? name : "", env));
        }
        if (expr->nodetype == 'j') {
            const TypeNode *base = infer(expr->l, env);
            return needs_static_check(field_type(base, expr, env));
        }
        if (expr->nodetype == NODE_CAST)
            return true;
        return false;
    }

    static struct ast *lvalue_root(struct ast *target) {
        while (target &&
               (target->nodetype == 'G' || target->nodetype == 'j'))
            target = target->l;
        return target;
    }

    void degrade_inferred_lvalue(struct ast *target, Env &env) {
        struct ast *root = lvalue_root(target);
        if (!root || root->nodetype != 'N')
            return;
        const char *name = ((struct symref *)root)->name;
        Env *owner = env.owner_of(name ? name : "");
        if (!owner)
            return;
        auto it = owner->vars.find(name ? name : "");
        if (it == owner->vars.end() || it->second.fixed)
            return;
        const TypeNode *type = it->second.type;
        int arity = type ? type_container_arity(type->kind) : -1;
        if (arity > 0) {
            std::vector<const TypeNode *> args((size_t)arity, type_any());
            type = type_container(type->kind, args);
        } else {
            type = type_any();
        }
        it->second.type = type;
    }

    bool check_assignment(struct ast *target, struct ast *value, Env &env,
                          bool compound) {
        if (!target)
            return true;
        if (target->nodetype == 'N') {
            const char *name = ((struct symref *)target)->name;
            Env *owner = env.owner_of(name ? name : "");
            VarInfo info = owner
                               ? owner->vars[name ? name : ""]
                               : lookup_var(name ? name : "", env);
            if (info.fixed) {
                return check_expr(value, info.type, env, false,
                                  needs_static_check(info.type));
            }
            const TypeNode *rhs =
                value ? infer(value, env) : type_any();
            if (owner) {
                VarInfo &assigned = owner->vars[name ? name : ""];
                assigned.type = owner == &env ? rhs : type_any();
                assigned.fixed = false;
                if (assigned.exported && !module.empty())
                    g_exported_types[module][name ? name : ""] = assigned;
            }
            return true;
        }

        bool fixed = expr_fixed(lvalue_root(target), env);
        if (fixed)
            return check_lvalue(target, value, env, compound);

        if (value)
            infer(value, env);
        degrade_inferred_lvalue(target, env);
        return true;
    }

    const TypeNode *function_type_from_sig(const FunctionSig &sig) {
        std::vector<const TypeNode *> args = sig.params;
        args.push_back(signature_result_type(sig));
        return make_function_type(TYPE_FUNCTION, args, sig.optional_params,
                                  sig.variadic, false,
                                  sig.variadic ? sig.rest : nullptr);
    }

    const TypeNode *function_type_from_code(
        const struct function_code *code) {
        if (!code)
            return type_builtin(TYPE_FUNCTION);
        FunctionSig sig = signature_from_symlist(code->sl);
        sig.result = code->ret_type ? type_from_constraint(code->ret_type.get())
                                    : type_any();
        sig.is_async = code->is_async;
        sig.is_generator = code->is_generator;
        return function_type_from_sig(sig);
    }

    const TypeNode *function_type_from_def(
        struct funcdef *def, Env &env,
        const TypeNode *expected = nullptr) {
        if (!def)
            return type_builtin(TYPE_FUNCTION);
        FunctionSig sig = signature_from_symlist(def->sl);
        sig.is_async = def->is_async;
        sig.is_generator = def->is_generator;
        if (def->is_generator) {
            sig.result = type_any();
            return function_type_from_sig(sig);
        }

        bool use_expected =
            expected && type_is_function_signature(expected) &&
            (std::size_t)type_function_param_count(expected) ==
                sig.params.size();
        std::vector<const TypeNode *> params;
        std::size_t index = 0;
        for (struct symlist *item = def->sl; item; item = item->next) {
            const TypeNode *type = type_from_constraint(item->type);
            if (item->variadic) {
                if (type->kind == TYPE_ANY && use_expected &&
                    type_function_is_variadic(expected))
                    type = type_function_rest(expected);
                sig.rest = type;
                continue;
            }
            if (type->kind == TYPE_ANY && use_expected)
                type = type_function_param(expected, index);
            params.push_back(type);
            ++index;
        }
        sig.params = params;

        if (def->ret_type) {
            sig.result = type_from_constraint(def->ret_type);
            return function_type_from_sig(sig);
        }

        int return_count = 0;
        struct ast *return_expr =
            find_single_return(def->ast, return_count);
        if (!return_expr || return_count != 1) {
            sig.result = type_any();
            return function_type_from_sig(sig);
        }

        Env local;
        local.parent = &env;
        index = 0;
        for (struct symlist *item = def->sl; item; item = item->next) {
            const char *name =
                item->sym && item->sym->name ? item->sym->name : "";
            if (item->variadic) {
                local.define(name, type_container(
                                       TYPE_LIST,
                                       {sig.rest ? sig.rest : type_any()}));
            } else {
                local.define(name, params[index++]);
            }
        }
        sig.result = infer(return_expr, local);
        return function_type_from_sig(sig);
    }

    const TypeNode *infer_iterable_element(struct ast *iterable,
                                           bool value_part,
                                           Env &env) {
        return iterable_element(infer(iterable, env), value_part);
    }

    const TypeNode *named_function_result(const char *name) {
        std::string key = name ? name : "";
        auto it = functions.find(key);
        if (it != functions.end())
            return signature_result_type(it->second);
        struct symbol *sym = lookupsym(key.c_str());
        if (sym && sym->val && sym->val->type == T_func) {
            struct func_value_t *fn = (struct func_value_t *)sym->val;
            if (fn->def && fn->def->code) {
                FunctionSig sig =
                    signature_from_symlist(fn->def->code->sl);
                sig.result = fn->def->code->ret_type
                                 ? type_from_constraint(
                                       fn->def->code->ret_type.get())
                                 : type_any();
                sig.is_async = fn->def->code->is_async;
                sig.is_generator = fn->def->code->is_generator;
                return signature_result_type(sig);
            }
        }
        return type_any();
    }

    const TypeNode *infer_lambda_result(struct ast *function,
                                        const TypeNode *element,
                                        Env &env) {
        if (!function || function->nodetype != 'Z')
            return type_any();
        struct funcdef *def = (struct funcdef *)function;
        check_funcdef(def, env);
        if (def->is_generator)
            return type_any();
        auto finish = [&](const TypeNode *result) {
            return def->is_async ? type_container(TYPE_TASK, {result})
                                 : result;
        };
        if (def->ret_type)
            return finish(type_from_constraint(def->ret_type));
        if (!def->ast)
            return type_any();
        int return_count = 0;
        struct ast *return_expr =
            find_single_return(def->ast, return_count);
        if (!return_expr || return_count != 1)
            return type_any();

        Env local;
        local.parent = &env;
        bool first = true;
        for (struct symlist *item = def->sl; item; item = item->next) {
            const char *name =
                item->sym && item->sym->name ? item->sym->name : "";
            const TypeNode *type = type_from_constraint(item->type);
            if (type->kind == TYPE_ANY) {
                if (first && !item->variadic)
                    type = element ? element : type_any();
            }
            if (item->variadic)
                type = type_container(TYPE_LIST, {type});
            local.define(name, type);
            if (!item->variadic)
                first = false;
        }
        return finish(infer(return_expr, local));
    }

    void bind_pattern_types(const struct ast *node,
                            const TypeNode *value, Env &env,
                            bool fixed = false) {
        if (!node)
            return;
        const struct pattern_node *pattern =
            (const struct pattern_node *)node;
        switch (pattern->nodetype) {
            case NODE_PATTERN_BINDING:
            case NODE_PATTERN_REST:
                if (pattern->name && *pattern->name &&
                    std::strcmp(pattern->name, "_") != 0)
                    env.define(pattern->name, value ? value : type_any(),
                               fixed);
                return;
            case NODE_PATTERN_AS: {
                if (pattern->items)
                    bind_pattern_types(pattern->items, value, env, fixed);
                if (pattern->name && *pattern->name &&
                    std::strcmp(pattern->name, "_") != 0) {
                    const struct pattern_node *inner =
                        (const struct pattern_node *)pattern->items;
                    if (inner &&
                        inner->nodetype == NODE_PATTERN_TYPE) {
                        const TypeNode *type =
                            type_from_constraint(inner->type);
                        if (value &&
                            match_type(value, type) !=
                                MatchResult::Mismatch)
                            type = value;
                        env.define(pattern->name, type, true);
                    } else {
                        env.define(pattern->name,
                                   value ? value : type_any(), fixed);
                    }
                }
                return;
            }
            case NODE_PATTERN_LIST: {
                const TypeNode *element = type_any();
                const TypeNode *rest = value ? value : type_any();
                if (value && value->kind == TYPE_LIST) {
                    element = value->args.empty() ? type_any()
                                                  : value->args[0];
                    rest = value;
                }
                for (const struct pattern_node *item =
                         (const struct pattern_node *)pattern->items;
                     item; item = (const struct pattern_node *)item->next) {
                    if (item->nodetype == NODE_PATTERN_REST)
                        bind_pattern_types((const struct ast *)item, rest,
                                           env, fixed);
                    else
                        bind_pattern_types((const struct ast *)item, element,
                                           env, fixed);
                }
                return;
            }
            case NODE_PATTERN_CONTAINER: {
                if (pattern->name) {
                    const TypeNode *pattern_type =
                        type_named(pattern->module, pattern->name);
                    structtype_lookup_result lookup;
                    ConstTypeMap *static_fields = NULL;
                    StaticTypeInfo *info =
                        find_static_info(pattern_type);
                    struct variant_def *variant = NULL;
                    struct_def *structure = NULL;

                    if (lookup_named_type(pattern_type, lookup)) {
                        if (lookup.enumeration && pattern->variant) {
                            auto vit =
                                lookup.enumeration->variant_index.find(
                                    pattern->variant);
                            if (vit !=
                                lookup.enumeration->variant_index.end())
                                variant = &lookup.enumeration->variants[
                                    (size_t)vit->second];
                            else if (info && info->is_enum)
                                static_fields = &info->variants[
                                    pattern->variant];
                        } else if (lookup.structure) {
                            structure = lookup.structure;
                        }
                    } else if (info) {
                        if (info->is_enum && pattern->variant) {
                            auto vit = info->variants.find(
                                pattern->variant);
                            if (vit != info->variants.end())
                                static_fields = &vit->second;
                        } else if (!info->is_enum) {
                            static_fields = &info->fields;
                        }
                    }

                    for (struct pattern_field *field = pattern->fields;
                         field; field = field->next) {
                        const TypeNode *field_type = type_any();
                        if (field->pattern && field->key) {
                            if (variant) {
                                auto fit = variant->field_index.find(
                                    field->key);
                                if (fit != variant->field_index.end())
                                    field_type = type_from_constraint(
                                        &variant->fields[
                                             (size_t)fit->second]
                                             .type);
                            } else if (structure) {
                                auto fit = structure->field_index.find(
                                    field->key);
                                if (fit != structure->field_index.end())
                                    field_type = type_from_constraint(
                                        &structure->fields[
                                             (size_t)fit->second]
                                             .type);
                            } else if (static_fields) {
                                auto fit = static_fields->find(field->key);
                                if (fit != static_fields->end())
                                    field_type = fit->second;
                            }
                            bind_pattern_types(field->pattern, field_type,
                                               env, fixed);
                        } else if (field->rest_name) {
                            env.define(field->rest_name, type_any(), fixed);
                        }
                    }
                    break;
                }

                const TypeNode *field_type = type_any();
                const TypeNode *rest_type = value ? value : type_any();
                if (value && value->kind == TYPE_DICT) {
                    field_type = value->args.size() < 2
                                     ? type_any()
                                     : value->args[1];
                    rest_type = value;
                }
                for (struct pattern_field *field = pattern->fields; field;
                     field = field->next) {
                    if (field->pattern)
                        bind_pattern_types(field->pattern, field_type, env,
                                           fixed);
                    else if (field->rest_name)
                        env.define(field->rest_name, rest_type, fixed);
                }
                return;
            }
            case NODE_PATTERN_OR:
                for (const struct pattern_node *item =
                         (const struct pattern_node *)pattern->items;
                     item; item = (const struct pattern_node *)item->next)
                    bind_pattern_types((const struct ast *)item, value, env,
                                       fixed);
                return;
            case NODE_PATTERN_TYPE:
            case NODE_PATTERN_LITERAL:
            case NODE_PATTERN_RANGE:
            case NODE_PATTERN_NEVER:
            default:
                return;
        }
    }

    bool check_struct_literal(struct ast *expr, Env &env) {
        const char *name = nullptr;
        const char *module = nullptr;
        struct struct_init *fields = nullptr;
        struct_def *def = nullptr;
        enum_def *edef = nullptr;
        struct variant_def *variant = nullptr;
        const char *variant_name = nullptr;

        if (expr->nodetype == 'y') {
            struct struct_lit_node *lit =
                (struct struct_lit_node *)expr;
            name = lit->name;
            module = lit->module;
            fields = lit->fields;
            def = structtype_find_struct(module, name);
        } else if (expr->nodetype == '?') {
            struct enum_lit_node *lit = (struct enum_lit_node *)expr;
            name = lit->name;
            module = lit->module;
            fields = lit->fields;
            variant_name = lit->variant;
            edef = structtype_find_enum(module, name);
            if (edef && lit->variant) {
                auto it = edef->variant_index.find(lit->variant);
                if (it != edef->variant_index.end())
                    variant = &edef->variants[(size_t)it->second];
            }
        }

        const TypeNode *literal_type = type_named(module, name);
        StaticTypeInfo *info = find_static_info(literal_type);
        ConstTypeMap *static_fields = NULL;
        if (info) {
            if (info->is_enum && variant_name) {
                auto it = info->variants.find(variant_name);
                if (it != info->variants.end())
                    static_fields = &it->second;
            } else if (!info->is_enum) {
                static_fields = &info->fields;
            }
        }

        for (struct struct_init *init = fields; init; init = init->next) {
            if (!init->name) {
                if (init->value)
                    infer(init->value, env);
                continue;
            }
            const struct type_constraint *field_type = nullptr;
            if (def) {
                auto it = def->field_index.find(init->name);
                if (it != def->field_index.end())
                    field_type =
                        &def->fields[(size_t)it->second].type;
            } else if (variant) {
                auto it = variant->field_index.find(init->name);
                if (it != variant->field_index.end())
                    field_type =
                        &variant->fields[(size_t)it->second].type;
            }
            if (field_type) {
                const TypeNode *expected =
                    type_from_constraint(field_type);
                if (!check_expr(init->value, expected, env, false,
                                needs_static_check(expected)))
                    return false;
                continue;
            }
            if (static_fields) {
                auto it = static_fields->find(init->name);
                if (it != static_fields->end()) {
                    if (!check_expr(init->value, it->second, env, false,
                                    needs_static_check(it->second)))
                        return false;
                    continue;
                }
            }
            if (init->value)
                infer(init->value, env);
        }
        return true;
    }

    bool check_funcdef(struct funcdef *def, Env &env,
                       const TypeNode *self_type = nullptr) {
        if (!def)
            return true;
        FunctionSig sig;
        if (def->sl)
            sig = signature_from_symlist(def->sl);
        sig.result = def->ret_type ? type_from_constraint(def->ret_type)
                                   : type_any();
        sig.is_async = def->is_async;
        sig.is_generator = def->is_generator;
        if (def->name)
            functions[def->name] = sig;

        for (struct symlist *item = def->sl; item; item = item->next) {
            struct ast *default_value = symlist_default_ast(item);
            if (!default_value)
                continue;
            const TypeNode *expected =
                type_from_constraint(item->type);
            if (item->variadic)
                expected = type_container(TYPE_LIST, {expected});
            if (!check_expr(default_value, expected, env, false,
                            needs_static_check(expected)))
                return false;
        }

        Env local;
        local.parent = &env;
        for (struct symlist *item = def->sl; item; item = item->next) {
            const char *name =
                item->sym && item->sym->name ? item->sym->name : "";
            const TypeNode *type = type_from_constraint(item->type);
            if (item->variadic)
                type = type_container(TYPE_LIST, {type});
            bool fixed = item->type != NULL;
            if (self_type && std::string(name) == "self") {
                type = self_type;
                fixed = true;
            }
            local.define(name, type, fixed);
            struct ast *pattern = symlist_pattern_ast(item);
            if (pattern)
                bind_pattern_types(pattern, type, local, fixed);
        }

        return_types.push_back(def->ret_type
                                   ? type_from_constraint(def->ret_type)
                                   : NULL);
        bool ok = check_stmt(def->ast, local);
        return_types.pop_back();
        return ok;
    }

    bool check_struct_decl(struct struct_node *decl, Env &env) {
        const TypeNode *self_type =
            type_named(decl->module, decl->name);
        StaticTypeInfo &info = static_types[
            static_type_key(decl->module, decl->name)];
        info.is_enum = false;
        for (struct struct_member *member = decl->members; member;
             member = member->next) {
            if (member->name && member->type && !member->func)
                info.fields[member->name] =
                    type_from_constraint(member->type);
            if (member->name && member->func &&
                member->func->nodetype == 'Z')
                info.methods[member->name] =
                    (struct funcdef *)member->func;
        }
        for (struct struct_member *member = decl->members; member;
             member = member->next) {
            if (member->func && member->func->nodetype == 'Z') {
                if (!check_funcdef((struct funcdef *)member->func, env,
                                   self_type))
                    return false;
                continue;
            }
            if (member->default_value) {
                const TypeNode *expected =
                    type_from_constraint(member->type);
                if (!check_expr(member->default_value, expected, env, false,
                                needs_static_check(expected)))
                    return false;
            }
        }
        return true;
    }

    bool check_enum_decl(struct enum_node *decl, Env &env) {
        StaticTypeInfo &info = static_types[
            static_type_key(decl->module, decl->name)];
        info.is_enum = true;
        for (struct enum_variant *variant = decl->variants; variant;
             variant = variant->next) {
            auto &fields = info.variants[variant->name
                                             ? variant->name
                                             : ""];
            for (struct struct_member *field = variant->fields; field;
                 field = field->next) {
                if (field->name && field->type)
                    fields[field->name] =
                        type_from_constraint(field->type);
            }
        }
        for (struct enum_variant *variant = decl->variants; variant;
             variant = variant->next) {
            for (struct struct_member *field = variant->fields; field;
                 field = field->next) {
                if (!field->default_value)
                    continue;
                const TypeNode *expected =
                    type_from_constraint(field->type);
                if (!check_expr(field->default_value, expected, env, false,
                                needs_static_check(expected)))
                    return false;
            }
        }
        return true;
    }

    bool check_impl_decl(struct impl_node *impl, Env &env) {
        const TypeNode *self_type =
            type_named(impl->module, impl->type_name);
        StaticTypeInfo &info = static_types[
            static_type_key(impl->module, impl->type_name)];
        for (struct struct_member *member = impl->methods; member;
             member = member->next) {
            if (member->func && member->func->nodetype == 'Z') {
                if (member->name)
                    info.methods[member->name] =
                        (struct funcdef *)member->func;
                if (!check_funcdef((struct funcdef *)member->func, env,
                                   self_type))
                    return false;
            }
        }
        return true;
    }

    const TypeNode *iterable_element(const TypeNode *iterable,
                                     bool value_part) {
        if (!iterable)
            return type_any();
        switch (iterable->kind) {
            case TYPE_LIST:
            case TYPE_SET:
            case TYPE_DEQUE:
            case TYPE_HEAP:
                return iterable->args.empty() ? type_any()
                                              : iterable->args[0];
            case TYPE_DICT:
                if (iterable->args.size() < 2)
                    return type_any();
                return iterable->args[value_part ? 1 : 0];
            case TYPE_STRING:
                return type_builtin(TYPE_CHAR);
            case TYPE_BYTES:
                return type_builtin(TYPE_BYTE);
            default:
                return type_any();
        }
    }

    bool check_stmt(struct ast *stmt, Env &env) {
        if (!stmt)
            return true;
        switch (stmt->nodetype) {
            case 'D': {
                struct symdef *def = (struct symdef *)stmt;
                const TypeNode *declared = type_any();
                bool fixed = false;
                if (def->type) {
                    declared = type_from_constraint(def->type);
                    fixed = true;
                    if (def->v &&
                        !check_expr(def->v, declared, env, false,
                                    needs_static_check(declared)))
                        return false;
                } else {
                    declared = def->v ? infer(def->v, env) : type_any();
                }
                const char *name = def->name ? def->name : "";
                env.define(name, declared, fixed);
                if (def->exported) {
                    env.vars[name].exported = true;
                    if (!module.empty()) {
                        VarInfo info;
                        info.type = declared;
                        info.fixed = fixed;
                        info.exported = true;
                        g_exported_types[module][name] = info;
                    }
                }
                return true;
            }
            case NODE_DESTRUCTURE: {
                struct destructure_node *d =
                    (struct destructure_node *)stmt;
                bool fixed = expr_fixed(d->value, env);
                const TypeNode *value_type = infer(d->value, env);
                bind_pattern_types(d->pattern, value_type, env, fixed);
                return true;
            }
            case '=': {
                struct symasgn *asgn = (struct symasgn *)stmt;
                return check_assignment(asgn->target, asgn->v, env, false);
            }
            case 'a': {
                struct compound_asgn *asgn =
                    (struct compound_asgn *)stmt;
                return check_assignment(asgn->target, asgn->v, env, true);
            }
            case 'U': {
                struct incdec_node *inc = (struct incdec_node *)stmt;
                const TypeNode *type = type_any();
                if (!resolve_lvalue(inc->target, env, type))
                    return false;
                return true;
            }
            case 'X': {
                Env child;
                child.parent = &env;
                return check_stmt(((struct nscope *)stmt)->ast, child);
            }
            case 'L':
                return check_stmt(stmt->l, env) &&
                       check_stmt(stmt->r, env);
            case 'I':
            case 'T': {
                struct flow *f = (struct flow *)stmt;
                if (!check_expr(f->cond, type_any(), env, false, false))
                    return false;
                Env then_env;
                then_env.parent = &env;
                if (!check_stmt(f->tl, then_env))
                    return false;
                Env else_env;
                else_env.parent = &env;
                return check_stmt(f->el, else_env);
            }
            case 'W': {
                struct flow *f = (struct flow *)stmt;
                if (!check_expr(f->cond, type_any(), env, false, false))
                    return false;
                Env child;
                child.parent = &env;
                return check_stmt(f->tl, child);
            }
            case 'E': {
                struct flow *f = (struct flow *)stmt;
                Env child;
                child.parent = &env;
                if (!check_stmt(f->tl, child))
                    return false;
                return check_expr(f->cond, type_any(), env, false, false);
            }
            case 'P': {
                struct forloop *f = (struct forloop *)stmt;
                Env child;
                child.parent = &env;
                return check_stmt(f->init, child) &&
                       check_expr(f->cond, type_any(), child, false,
                                  false) &&
                       check_stmt(f->step, child) &&
                       check_stmt(f->body, child);
            }
            case 'F': {
                struct forinloop *f = (struct forinloop *)stmt;
                bool fixed = expr_fixed(f->iterable, env);
                const TypeNode *iterable = infer(f->iterable, env);
                Env child;
                child.parent = &env;
                child.define(f->name ? f->name : "",
                             iterable_element(iterable, false), fixed);
                if (f->value_name)
                    child.define(f->value_name,
                                 iterable_element(iterable, true), fixed);
                return check_stmt(f->body, child);
            }
            case 'S': {
                struct switch_node *s = (struct switch_node *)stmt;
                if (!check_expr(s->expr, type_any(), env, false, false))
                    return false;
                for (struct case_node *c = (struct case_node *)s->cases; c;
                     c = (struct case_node *)c->next) {
                    Env child;
                    child.parent = &env;
                    if (!check_stmt(c->body, child))
                        return false;
                }
                return true;
            }
            case NODE_MATCH: {
                struct match_node *match = (struct match_node *)stmt;
                bool fixed = expr_fixed(match->expr, env);
                const TypeNode *subject = infer(match->expr, env);
                for (struct match_arm *arm =
                         (struct match_arm *)match->arms;
                     arm; arm = (struct match_arm *)arm->next) {
                    Env child;
                    child.parent = &env;
                    if (arm->pattern)
                        bind_pattern_types(arm->pattern, subject, child,
                                           fixed);
                    if (!check_expr(arm->guard, type_any(), child, false,
                                    false) ||
                        !check_stmt(arm->body, child))
                        return false;
                }
                return true;
            }
            case 'R': {
                struct fnret *ret = (struct fnret *)stmt;
                const TypeNode *expected =
                    return_types.empty() ? NULL : return_types.back();
                if (!expected) {
                    if (ret->v)
                        infer(ret->v, env);
                    return true;
                }
                return check_return_expr(ret->v, expected, env, stmt->line,
                                         stmt->column);
            }
            case 'Z':
                return check_funcdef((struct funcdef *)stmt, env);
            case 'Y':
                return check_struct_decl((struct struct_node *)stmt, env);
            case '<':
                return check_enum_decl((struct enum_node *)stmt, env);
            case 'H':
                return check_impl_decl((struct impl_node *)stmt, env);
            case 'J': {
                struct import_node *imp = (struct import_node *)stmt;
                std::string path = imp->path ? imp->path : "";
                std::string alias =
                    imp->alias ? imp->alias : last_path_component(path);
                if (!alias.empty())
                    g_import_aliases[source][alias] = path;
                return true;
            }
            case 'o': {
                struct on_error_node *handler =
                    (struct on_error_node *)stmt;
                if (!check_expr(handler->call, type_any(), env, false,
                                false))
                    return false;
                Env child;
                child.parent = &env;
                child.define(handler->name ? handler->name : "",
                             type_any());
                return check_stmt(handler->body, child);
            }
            case 'n':
                return check_expr(stmt->l, type_any(), env, false, false);
            case 'u':
            case 'v':
            case 'p':
                return check_expr(((struct control_stmt *)stmt)->value,
                                  type_any(), env, false, false);
            case 'g':
                return check_stmt(((struct label_node *)stmt)->stmt, env);
            case 'K':
                return check_stmt(((struct package_node *)stmt)->body, env);
            default:
                return check_expr(stmt, type_any(), env, false, false);
        }
    }

    const TypeNode *infer(struct ast *expr, Env &env) {
        if (!expr)
            return type_any();
        switch (expr->nodetype) {
            case 'V': {
                struct value_t *value = ((struct val *)expr)->val;
                if (value && value->type == T_builtin) {
                    struct builtin_value_t *builtin =
                        (struct builtin_value_t *)value;
                    if (const TypeNode *signature =
                            builtin_function_type(builtin->functype))
                        return signature;
                }
                return value_static_type(value);
            }
            case 'N': {
                const char *name = ((struct symref *)expr)->name;
                std::string key = name ? name : "";
                /* A scoped variable shadows any same-named function, even
                 * when its inferred type has degraded to any. */
                if (const VarInfo *scoped = env.lookup(key))
                    return scoped->type ? scoped->type : type_any();
                VarInfo info = lookup_var(key, env);
                if (info.type &&
                    (info.type->kind != TYPE_ANY || info.fixed))
                    return info.type;
                auto it = functions.find(key);
                if (it != functions.end())
                    return function_type_from_sig(it->second);
                struct symbol *sym = lookupsym(key.c_str());
                if (sym && sym->val && sym->val->type == T_func) {
                    struct func_value_t *fn =
                        (struct func_value_t *)sym->val;
                    if (fn->def && fn->def->code)
                        return function_type_from_code(
                            fn->def->code.get());
                }
                if (sym && sym->val && sym->val->type == T_builtin) {
                    struct builtin_value_t *builtin =
                        (struct builtin_value_t *)sym->val;
                    if (const TypeNode *signature =
                            builtin_function_type(builtin->functype))
                        return signature;
                    return type_builtin(TYPE_BUILTIN);
                }
                return info.type ? info.type : type_any();
            }
            case 'l': {
                const TypeNode *element = nullptr;
                for (struct list_node *item = (struct list_node *)expr;
                     item; item = (struct list_node *)item->next) {
                    element = join_types(element, infer(item->value, env));
                }
                return type_container(TYPE_LIST,
                                      {element ? element : type_any()});
            }
            case 'd': {
                const TypeNode *key = nullptr;
                const TypeNode *value = nullptr;
                for (struct dict_node *entry = (struct dict_node *)expr;
                     entry; entry = (struct dict_node *)entry->next) {
                    key = join_types(key, infer(entry->key, env));
                    value = join_types(value, infer(entry->value, env));
                }
                return type_container(
                    TYPE_DICT,
                    {key ? key : type_any(), value ? value : type_any()});
            }
            case 'G': {
                const TypeNode *base = infer(expr->l, env);
                infer(expr->r, env);
                if (base->kind == TYPE_LIST || base->kind == TYPE_DEQUE)
                    return base->args.empty() ? type_any() : base->args[0];
                if (base->kind == TYPE_DICT)
                    return base->args.size() < 2 ? type_any()
                                                 : base->args[1];
                if (base->kind == TYPE_STRING)
                    return type_builtin(TYPE_CHAR);
                if (base->kind == TYPE_BYTES)
                    return type_builtin(TYPE_BYTE);
                return type_any();
            }
            case 'j': {
                const TypeNode *base = infer(expr->l, env);
                std::string path;
                bool has_path = flatten_path(expr, path);
                if (base->kind == TYPE_ANY && has_path) {
                    const TypeNode *polymorphic =
                        builtin_polymorphic_type(
                            canonical_builtin_path(path, source));
                    if (polymorphic)
                        return polymorphic;
                }
                if (base->kind == TYPE_ANY) {
                    const TypeNode *exported = NULL;
                    struct value_t *value = NULL;
                    if (has_path &&
                        resolve_namespace_type(path, exported))
                        return exported;
                    if (has_path &&
                        resolve_namespace_value(path, value)) {
                        if (value->type == T_func) {
                            struct func_value_t *fn =
                                (struct func_value_t *)value;
                            if (fn->def && fn->def->code)
                                return function_type_from_code(
                                    fn->def->code.get());
                        }
                        if (value->type == T_builtin) {
                            struct builtin_value_t *builtin =
                                (struct builtin_value_t *)value;
                            if (const TypeNode *signature =
                                    builtin_function_type(
                                        builtin->functype))
                                return signature;
                        }
                        return value_static_type(value);
                    }
                }
                const TypeNode *field = field_type(base, expr, env);
                if (field->kind != TYPE_ANY)
                    return field;
                const struct ast *right = expr->r;
                if (right && right->nodetype == 'V') {
                    const struct value_t *value =
                        ((const struct val *)right)->val;
                    if (value && value->type == T_string) {
                        const TypeNode *method = method_value_type(
                            base,
                            ((const struct string_value_t *)value)->value);
                        if (method)
                            return method;
                    }
                }
                return field;
            }
            case 'C': {
                const TypeNode *result = type_any();
                check_call(expr, type_any(), env, result);
                return result;
            }
            case NODE_CAST: {
                struct cast_node *cast = (struct cast_node *)expr;
                const TypeNode *target = type_from_constraint(cast->type);
                check_expr(cast->value, target, env, true,
                           needs_static_check(target));
                return target;
            }
            case 'Z': {
                struct funcdef *def = (struct funcdef *)expr;
                check_funcdef(def, env);
                return function_type_from_def(def, env);
            }
            case 'k':
                return type_builtin(TYPE_BOOL);
            case 'x':
                infer(((struct spread_node *)expr)->value, env);
                return type_any();
            case 'y': {
                struct struct_lit_node *lit =
                    (struct struct_lit_node *)expr;
                check_struct_literal(expr, env);
                return type_named(lit->module, lit->name);
            }
            case '?': {
                struct enum_lit_node *lit =
                    (struct enum_lit_node *)expr;
                check_struct_literal(expr, env);
                return type_named(lit->module, lit->name);
            }
            case '=': {
                struct symasgn *asgn = (struct symasgn *)expr;
                check_assignment(asgn->target, asgn->v, env, false);
                return infer(asgn->v, env);
            }
            case 'a': {
                struct compound_asgn *asgn =
                    (struct compound_asgn *)expr;
                check_assignment(asgn->target, asgn->v, env, true);
                return infer(asgn->target, env);
            }
            case 'U': {
                struct incdec_node *inc = (struct incdec_node *)expr;
                const TypeNode *type = type_any();
                resolve_lvalue(inc->target, env, type);
                return type;
            }
            case 'A':
            case 'Q':
            case '!':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
                infer(expr->l, env);
                infer(expr->r, env);
                return type_builtin(TYPE_BOOL);
            case '+': {
                const TypeNode *left = infer(expr->l, env);
                const TypeNode *right = infer(expr->r, env);
                if (left->kind == TYPE_LIST && right->kind == TYPE_LIST) {
                    const TypeNode *a = left->args.empty()
                                            ? type_any()
                                            : left->args[0];
                    const TypeNode *b = right->args.empty()
                                            ? type_any()
                                            : right->args[0];
                    return type_container(TYPE_LIST, {join_types(a, b)});
                }
                if ((left->kind == TYPE_INT || left->kind == TYPE_FLOAT ||
                     left->kind == TYPE_NUMBER) &&
                    (right->kind == TYPE_INT || right->kind == TYPE_FLOAT ||
                     right->kind == TYPE_NUMBER)) {
                    if (left->kind == TYPE_NUMBER ||
                        right->kind == TYPE_NUMBER)
                        return type_builtin(TYPE_NUMBER);
                    if (left->kind == TYPE_FLOAT ||
                        right->kind == TYPE_FLOAT)
                        return type_builtin(TYPE_FLOAT);
                    return type_builtin(TYPE_INT);
                }
                if (left->kind == TYPE_STRING || right->kind == TYPE_STRING ||
                    left->kind == TYPE_CHAR || right->kind == TYPE_CHAR)
                    return type_builtin(TYPE_STRING);
                return type_any();
            }
            case '-':
            case '*':
            case '/':
            case '%':
            case 'M': {
                const TypeNode *left = infer(expr->l, env);
                const TypeNode *right =
                    expr->r ? infer(expr->r, env) : NULL;
                if ((left->kind == TYPE_INT || left->kind == TYPE_FLOAT ||
                     left->kind == TYPE_NUMBER) &&
                    (!right || right->kind == TYPE_INT ||
                     right->kind == TYPE_FLOAT ||
                     right->kind == TYPE_NUMBER)) {
                    if (left->kind == TYPE_NUMBER ||
                        (right && right->kind == TYPE_NUMBER))
                        return type_builtin(TYPE_NUMBER);
                    if (left->kind == TYPE_FLOAT ||
                        (right && right->kind == TYPE_FLOAT))
                        return type_builtin(TYPE_FLOAT);
                    return type_builtin(TYPE_INT);
                }
                return type_any();
            }
            case '&':
            case '|':
            case '^':
            case 's':
            case 'r':
            case '~':
                infer(expr->l, env);
                if (expr->r)
                    infer(expr->r, env);
                return type_builtin(TYPE_INT);
            case 'T': {
                struct flow *flow = (struct flow *)expr;
                infer(flow->cond, env);
                const TypeNode *then_type = infer(flow->tl, env);
                const TypeNode *else_type = infer(flow->el, env);
                return join_types(then_type, else_type);
            }
            case 'b': {
                const TypeNode *value =
                    expr->l ? infer(expr->l, env) : type_any();
                if (value->kind == TYPE_TASK)
                    return type_element(value, 0);
                return type_any();
            }
            case 'w':
                if (expr->l)
                    infer(expr->l, env);
                return type_any();
            case 'n':
                if (expr->l)
                    infer(expr->l, env);
                return type_any();
            case 'L':
                return type_any();
            default:
                return type_any();
        }
    }
};

} // namespace

void set_mode(Mode mode) {
    g_mode = mode;
}

Mode mode() {
    return g_mode;
}

bool parse_mode(const char *text, Mode &mode) {
    if (!text)
        return false;
    if (std::strcmp(text, "off") == 0) {
        mode = Mode::Off;
        return true;
    }
    if (std::strcmp(text, "loose") == 0) {
        mode = Mode::Loose;
        return true;
    }
    if (std::strcmp(text, "warn") == 0) {
        mode = Mode::Warn;
        return true;
    }
    if (std::strcmp(text, "strict") == 0) {
        mode = Mode::Strict;
        return true;
    }
    return false;
}

const char *mode_name(Mode mode) {
    switch (mode) {
        case Mode::Off: return "off";
        case Mode::Loose: return "loose";
        case Mode::Warn: return "warn";
        case Mode::Strict: return "strict";
    }
    return "warn";
}

bool check_statement(struct ast *statement) {
    if (g_mode == Mode::Off || !statement)
        return true;
    Checker checker(diag_current_source_name(),
                    diag_current_module_name());
    Env env;
    auto saved = g_persistent_bindings.find(checker.source);
    if (saved != g_persistent_bindings.end())
        env.vars = saved->second;
    bool ok = checker.check_stmt(statement, env);
    if (ok && !checker.error_) {
        g_persistent_bindings[checker.source] = env.vars;
    }
    return ok && !checker.error_;
}

} // namespace typecheck
} // namespace skein
