#include "typecheck.h"

#include "typecheck_builtins.h"
#include "typecheck_state.h"
#include "typecheck_types.h"

#include "ast.h"
#include "ast_analysis.h"
#include "builtin_ids.h"
#include "diagnostics.h"
#include "scopestack.h"
#include "static_type.h"
#include "structtype.h"
#include "type_text_parser.h"
#include "valuetype.h"

#include <cstdio>
#include <cstdlib>
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

struct GenericConstraintMeta {
    enum generic_constraint_kind kind = GENERIC_CONSTRAINT_SINGLE;
    enum generic_constraint_category category =
        GENERIC_CONSTRAINT_UNSPECIFIED;
    std::vector<const TypeNode *> items;
};

struct GenericFunctionMeta {
    std::string name;
    std::vector<std::string> params;
    std::vector<GenericConstraintMeta> constraints;
    std::vector<const TypeNode *> defaults;
};

thread_local std::unordered_map<std::string, GenericFunctionMeta>
    g_generic_function_params;

struct GenericTypeMeta {
    std::vector<std::string> params;
    std::vector<GenericConstraintMeta> constraints;
    std::vector<const TypeNode *> defaults;
    ConstTypeMap fields;
    std::unordered_map<std::string, ConstTypeMap> variants;
};

thread_local std::unordered_map<std::string, GenericTypeMeta>
    g_generic_type_meta;

std::string generator_identity(const struct funcdef *def) {
    std::string key = diag_current_package_name();
    key += '|';
    if (def && def->name && *def->name) {
        key += def->name;
    } else {
        key += std::to_string(def && def->ast ? def->ast->line : 0);
        key += ':';
        key += std::to_string(def && def->ast ? def->ast->column : 0);
    }
    return key;
}

std::string generator_identity(const struct function_code *code) {
    std::string key = code ? code->package : "";
    key += '|';
    if (code && !code->name.empty()) {
        key += code->name;
    } else {
        key += std::to_string(code ? code->def_line : 0);
        key += ':';
        key += std::to_string(code ? code->def_column : 0);
    }
    return key;
}

std::string generator_location_identity(const struct funcdef *def) {
    std::string key = diag_current_package_name();
    key += '|';
    key += std::to_string(def && def->ast ? def->ast->line : 0);
    key += ':';
    key += std::to_string(def && def->ast ? def->ast->column : 0);
    return key;
}

std::string generator_location_identity(const struct function_code *code) {
    std::string key = code ? code->package : "";
    key += '|';
    key += std::to_string(code ? code->def_line : 0);
    key += ':';
    key += std::to_string(code ? code->def_column : 0);
    return key;
}

struct Checker : public BuiltinContext {
    std::string source;
    std::string package;
    std::unordered_map<std::string, FunctionSig> functions;
    std::unordered_map<std::string, GenericFunctionMeta> generic_functions;
    std::unordered_map<std::string, StaticTypeInfo> static_types;
    std::unordered_map<const struct ast *, std::vector<const TypeNode *>>
        generic_literal_args;
    std::vector<const TypeNode *> return_types;
    std::vector<std::vector<const TypeNode *>> yield_types;
    std::vector<const TypeNode *> generator_send_types;
    std::vector<const TypeNode *> generator_yield_expected;
    std::unordered_map<const struct funcdef *, const TypeNode *>
        generator_elements;
    std::unordered_map<const struct funcdef *, const TypeNode *>
        generator_sends;
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

    void warn_launch_on_error(const struct ast *node) {
        if (!node || node->nodetype != 'o')
            return;
        const struct on_error_node *handler =
            (const struct on_error_node *)node;
        const struct ast *call = handler->call;
        if (!call || call->nodetype != 'C')
            return;
        const struct ufncall *uf = (const struct ufncall *)call;
        if (!uf->launch || uf->wait_sugar)
            return;
        std::string key = "launch-on-error:";
        key += std::to_string(handler->line);
        key += ':';
        key += std::to_string(handler->column);
        if (!warnings.insert(key).second)
            return;
        diag_report_warning(
            handler->line, handler->column, "launch",
            "on error does not catch failures from the launched function; "
            "attach it to wait instead: wait t on error(e) { ... } or "
            "wait launch f() on error(e) { ... }");
    }

    explicit Checker(const std::string &source_name,
                     const std::string &module_name)
        : source(source_name), package(module_name) {}

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

    struct NarrowFacts {
        std::unordered_map<std::string, std::vector<const TypeNode *>>
            positive;
        std::unordered_map<std::string, std::vector<const TypeNode *>>
            negative;
        std::unordered_map<std::string, const TypeNode *> originals;
    };

    static bool simple_name(const struct ast *node, std::string &name) {
        if (!node || node->nodetype != 'N')
            return false;
        const char *raw = ((const struct symref *)node)->name;
        if (!raw)
            return false;
        name = raw;
        return true;
    }

    bool callee_is_builtin(const std::string &name, int functype,
                           Env &env) {
        if (env.lookup(name) || functions.find(name) != functions.end())
            return false;
        struct symbol *sym = lookupsym(name.c_str());
        if (!sym || !sym->val || sym->val->type != T_builtin)
            return false;
        struct builtin_value_t *builtin =
            (struct builtin_value_t *)sym->val;
        return builtin->functype == functype;
    }

    static bool narrow_path(const struct ast *node, std::string &path) {
        if (!node)
            return false;
        if (node->nodetype == 'N') {
            const char *raw = ((const struct symref *)node)->name;
            if (!raw)
                return false;
            path = raw;
            return true;
        }
        if (node->nodetype == 'j')
            return flatten_path(node, path);
        return false;
    }

    static std::string path_root(const std::string &path) {
        std::size_t dot = path.find('.');
        return dot == std::string::npos ? path : path.substr(0, dot);
    }

    static void invalidate_path_facts(Env &env,
                                      const std::string &path) {
        for (Env *scope = &env; scope; scope = scope->parent) {
            auto &paths = scope->narrowed_paths;
            for (auto it = paths.begin(); it != paths.end();) {
                const std::string &key = it->first;
                bool same = key == path;
                bool child = key.size() > path.size() &&
                             key.compare(0, path.size(), path) == 0 &&
                             key[path.size()] == '.';
                if (same || child)
                    it = paths.erase(it);
                else
                    ++it;
            }
        }
    }

    static void invalidate_root_facts(Env &env,
                                      const std::string &root) {
        for (Env *scope = &env; scope; scope = scope->parent) {
            auto &paths = scope->narrowed_paths;
            for (auto it = paths.begin(); it != paths.end();) {
                if (path_root(it->first) == root)
                    it = paths.erase(it);
                else
                    ++it;
            }
        }
    }

    void invalidate_lvalue_facts(struct ast *target, Env &env) {
        if (!target)
            return;
        std::string path;
        if (narrow_path(target, path)) {
            invalidate_path_facts(env, path);
            return;
        }
        std::string root;
        if (simple_name(lvalue_root(target), root))
            invalidate_root_facts(env, root);
    }

    void invalidate_method_receiver(struct ast *callee, Env &env) {
        if (!callee || callee->nodetype != 'j')
            return;
        std::string path;
        if (narrow_path(callee, path))
            invalidate_root_facts(env, path_root(path));
    }

    static const TypeNode *non_null_type(const TypeNode *type) {
        if (!type || !type->nullable)
            return type;
        if (type_container_arity(type->kind) > 0)
            return type_container(type->kind, type->args, false);
        if (type->kind == TYPE_NAMED)
            return type_named(type->source.c_str(), type->name.c_str(),
                              false);
        return type_builtin(type->kind, false);
    }

    const TypeNode *narrow_include(const TypeNode *type,
                                   const TypeNode *pattern) {
        if (!type || type->kind == TYPE_ANY)
            return pattern;
        if (type->nullable) {
            if (pattern && pattern->kind == TYPE_NULL)
                return type_builtin(TYPE_NULL);
            return narrow_include(non_null_type(type), pattern);
        }
        if (type->kind == TYPE_UNION) {
            std::vector<const TypeNode *> kept;
            for (const TypeNode *member : type->args) {
                if (!member || member->kind == TYPE_ANY)
                    return pattern;
                MatchResult forward = match_type(member, pattern);
                MatchResult reverse = match_type(pattern, member);
                if (forward != MatchResult::Mismatch ||
                    reverse != MatchResult::Mismatch)
                    kept.push_back(narrow_include(member, pattern));
            }
            if (kept.empty())
                return type;
            if (kept.size() == 1)
                return kept[0];
            return type_container(TYPE_UNION, kept);
        }
        if (match_type(type, pattern) == MatchResult::Ok)
            return type;
        if (match_type(pattern, type) == MatchResult::Ok)
            return pattern;
        if (match_type(type, pattern) == MatchResult::Unknown)
            return pattern;
        return type;
    }

    const TypeNode *narrow_exclude(const TypeNode *type,
                                   const TypeNode *pattern) {
        if (!type || type->kind == TYPE_ANY || type->nullable)
            return type;
        if (type->kind == TYPE_UNION) {
            std::vector<const TypeNode *> kept;
            for (const TypeNode *member : type->args) {
                if (!member || member->kind == TYPE_ANY) {
                    kept.push_back(member);
                    continue;
                }
                if (match_type(member, pattern) != MatchResult::Ok)
                    kept.push_back(member);
            }
            if (kept.empty() || kept.size() == type->args.size())
                return type;
            if (kept.size() == 1)
                return kept[0];
            return type_container(TYPE_UNION, kept);
        }
        return type;
    }

    void apply_narrow_facts(Env &target, const Env &source,
                            const NarrowFacts &facts) {
        auto apply = [&](const std::unordered_map<
                             std::string,
                             std::vector<const TypeNode *>> &entries,
                         bool include) {
            for (const auto &entry : entries) {
                const std::string &path = entry.first;
                bool simple = path.find('.') == std::string::npos;
                const VarInfo *original = source.lookup(path);
                const TypeNode *type = nullptr;
                auto original_it = facts.originals.find(path);
                if (original_it != facts.originals.end())
                    type = original_it->second;
                else if (original)
                    type = original->type;
                if (!type)
                    continue;
                if (simple && (!original || !original->fixed))
                    continue;
                for (const TypeNode *pattern : entry.second)
                    type = include
                               ? narrow_include(type, pattern)
                               : narrow_exclude(type, pattern);
                if (simple) {
                    target.define(path, type, true);
                    target.vars[path].declared =
                        original->declared ? original->declared
                                           : original->type;
                } else {
                    VarInfo info;
                    info.type = type;
                    info.declared = original_it != facts.originals.end()
                                        ? original_it->second
                                        : (original ? original->type
                                                    : type_any());
                    info.fixed = true;
                    target.narrowed_paths[path] = info;
                }
            }
        };
        apply(facts.positive, true);
        apply(facts.negative, false);
    }

    static void merge_facts(NarrowFacts &target,
                            const NarrowFacts &source) {
        auto merge_map = [](
            std::unordered_map<std::string,
                               std::vector<const TypeNode *>> &dst,
            const std::unordered_map<
                std::string, std::vector<const TypeNode *>> &src) {
            for (const auto &entry : src) {
                auto &values = dst[entry.first];
                values.insert(values.end(), entry.second.begin(),
                              entry.second.end());
            }
        };
        merge_map(target.positive, source.positive);
        merge_map(target.negative, source.negative);
        for (const auto &entry : source.originals) {
            if (target.originals.find(entry.first) ==
                target.originals.end())
                target.originals[entry.first] = entry.second;
        }
    }

    bool check_condition(struct ast *cond, Env &env,
                         NarrowFacts &positive, NarrowFacts &negative) {
        if (!cond)
            return true;
        if (cond->nodetype == 'k') {
            const struct is_node *node = (const struct is_node *)cond;
            std::string path;
            if (narrow_path(node->value, path) && node->type) {
                bool simple = path.find('.') == std::string::npos;
                const TypeNode *original = NULL;
                if (simple) {
                    const VarInfo *info = env.lookup(path);
                    if (info && info->fixed)
                        original = info->type;
                } else {
                    original = infer(node->value, env);
                }
                const TypeNode *pattern =
                    type_from_constraint(node->type);
                if (original && pattern->kind != TYPE_INVALID) {
                    positive.positive[path].push_back(pattern);
                    negative.negative[path].push_back(pattern);
                    positive.originals[path] = original;
                    negative.originals[path] = original;
                }
            }
            return check_expr(cond, type_any(), env, false, false);
        }
        if (cond->nodetype == '!') {
            return check_condition(cond->l, env, negative, positive);
        }
        if (cond->nodetype == 'A') {
            NarrowFacts left_positive;
            NarrowFacts left_negative;
            if (!check_condition(cond->l, env, left_positive,
                                 left_negative))
                return false;
            Env right_env;
            right_env.parent = &env;
            apply_narrow_facts(right_env, env, left_positive);
            NarrowFacts right_positive;
            NarrowFacts right_negative;
            if (!check_condition(cond->r, right_env, right_positive,
                                 right_negative))
                return false;
            merge_facts(positive, left_positive);
            merge_facts(positive, right_positive);
            /* A false && may come from either side, so narrow nothing. */
            return true;
        }
        if (cond->nodetype == 'Q') {
            NarrowFacts left_positive;
            NarrowFacts left_negative;
            if (!check_condition(cond->l, env, left_positive,
                                 left_negative))
                return false;
            Env right_env;
            right_env.parent = &env;
            apply_narrow_facts(right_env, env, left_negative);
            NarrowFacts right_positive;
            NarrowFacts right_negative;
            if (!check_condition(cond->r, right_env, right_positive,
                                 right_negative))
                return false;
            merge_facts(negative, left_negative);
            merge_facts(negative, right_negative);
            /* A true || may come from either side, so narrow nothing. */
            return true;
        }
        return check_expr(cond, type_any(), env, false, false);
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

    /*
     * Implicit struct-to-ref conversion is limited to fresh values: a struct
     * literal or a Type.new(...) constructor call.  A variable that already
     * holds a struct is a value object and must use ref(expr) explicitly.
     */
    bool fresh_struct_expr(const struct ast *expr, Env &env) {
        if (!expr)
            return false;
        if (expr->nodetype == 'y')
            return !((const struct struct_lit_node *)expr)->is_ref;
        if (expr->nodetype != 'C')
            return false;
        const struct ufncall *call = (const struct ufncall *)expr;
        const struct ast *callee = call->callee;
        if (!callee || callee->nodetype != 'j')
            return false;
        const struct ast *base = callee->l;
        const struct ast *field = callee->r;
        if (!base || base->nodetype != 'N' || !field ||
            field->nodetype != 'V')
            return false;
        const struct value_t *name = ((const struct val *)field)->val;
        if (!name || name->type != T_string ||
            std::strcmp(((const struct string_value_t *)name)->value,
                        "new") != 0)
            return false;
        const char *base_name = ((const struct symref *)base)->name;
        return base_name && env.owner_of(base_name) == nullptr;
    }

    bool check_expr(struct ast *expr, const TypeNode *expected, Env &env,
                    bool explicit_cast = false, bool force_element = false,
                    bool no_implicit_ref = false) {
        if (!expr)
            return true;
        if (!no_implicit_ref && expected && expected->kind == TYPE_REF &&
            !expected->args.empty() && expected->args[0] &&
            expected->args[0]->kind == TYPE_NAMED &&
            fresh_struct_expr(expr, env)) {
            return check_expr(expr, expected->args[0], env, explicit_cast,
                              true, no_implicit_ref);
        }
        if (expr->nodetype == 'o')
            warn_launch_on_error(expr);
        if (expr->nodetype == 'w' && expected &&
            expected->kind != TYPE_ANY &&
            !generator_send_types.empty()) {
            const TypeNode *send = generator_send_types.back();
            if (type_is_any(send)) {
                generator_send_types.back() = expected;
            } else if (match_type(send, expected) ==
                       MatchResult::Mismatch) {
                return warn_or_error(send, expected, expr->line,
                                     expr->column, false, true);
            }
        }
        if (!force_element && !needs_static_check(expected)) {
            infer(expr, env);
            return true;
        }

        if (expr->nodetype == NODE_CAST) {
            struct cast_node *cast = (struct cast_node *)expr;
            const TypeNode *target = type_from_constraint(cast->type);
            if (!check_expr(cast->value, target, env, true,
                            needs_static_check(target), no_implicit_ref))
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

        if (expected &&
            (expected->kind == TYPE_PAIR ||
             expected->kind == TYPE_TUPLE)) {
            if (expr->nodetype == 'l') {
                report_error(
                    expr->line, expr->column,
                    expected->kind == TYPE_PAIR
                        ? "pair expects a tuple literal; use (a, b)"
                        : "tuple expects a tuple literal; use (a, b)");
                return false;
            }
            if (expr->nodetype == NODE_TUPLE) {
                std::size_t index = 0;
                for (struct list_node *item = (struct list_node *)expr;
                     item; item = (struct list_node *)item->next, ++index) {
                    const TypeNode *element =
                        index < expected->args.size()
                            ? expected->args[index]
                            : type_any();
                    if (!check_expr(item->value, element, env,
                                    explicit_cast, true, true))
                        return false;
                }
                if (index != expected->args.size()) {
                    char message[128];
                    std::snprintf(message, sizeof(message),
                                  "%s expects %zu element(s), got %zu",
                                  expected->kind == TYPE_PAIR ? "pair"
                                                              : "tuple",
                                  expected->args.size(), index);
                    report_error(expr->line, expr->column, message);
                    return false;
                }
                return true;
            }
        }

        if (expected && expected->kind == TYPE_LIST &&
            expr->nodetype == 'l') {
            const TypeNode *element =
                expected->args.empty() ? type_any() : expected->args[0];
            for (struct list_node *item = (struct list_node *)expr; item;
                 item = (struct list_node *)item->next) {
                if (!check_expr(item->value, element, env, explicit_cast,
                                true, true))
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
                                true, true) ||
                    !check_expr(entry->value, value_type, env, explicit_cast,
                                true, true))
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
            if (((struct ufncall *)expr)->launch && result) {
                if (result->kind == TYPE_TASK) {
                    char message[512];
                    std::snprintf(
                        message, sizeof(message),
                        "launch cannot start a call that already returns %s; "
                        "call it directly and use .wait()",
                        type_display(result));
                    report_error(expr->line, expr->column, message);
                    return false;
                }
                result = type_container(TYPE_TASK, {result});
            }
            invalidate_method_receiver(
                ((struct ufncall *)expr)->callee, env);
            return warn_or_error(result, expected, expr->line,
                                 expr->column, explicit_cast,
                                 force_element);
        }

        const TypeNode *actual = infer(expr, env);
        return warn_or_error(actual, expected, expr->line, expr->column,
                             explicit_cast, force_element);
    }

    static bool compound_type_known(const TypeNode *type) {
        return type && type->kind != TYPE_ANY &&
               type->kind != TYPE_INVALID && type->kind != TYPE_VAR &&
               type->kind != TYPE_UNION;
    }

    static bool compound_number(const TypeNode *type) {
        return type && (type->kind == TYPE_INT ||
                        type->kind == TYPE_FLOAT ||
                        type->kind == TYPE_NUMBER);
    }

    static bool compound_chars(const TypeNode *type) {
        return type && (type->kind == TYPE_STRING ||
                        type->kind == TYPE_CHAR);
    }

    static bool compound_sequence(const TypeNode *type) {
        return type && (type->kind == TYPE_LIST ||
                        type->kind == TYPE_PAIR ||
                        type->kind == TYPE_TUPLE);
    }

    static const TypeNode *compound_number_result(const TypeNode *left,
                                                  const TypeNode *right) {
        if (left->kind == TYPE_NUMBER || right->kind == TYPE_NUMBER)
            return type_builtin(TYPE_NUMBER);
        if (left->kind == TYPE_FLOAT || right->kind == TYPE_FLOAT)
            return type_builtin(TYPE_FLOAT);
        return type_builtin(TYPE_INT);
    }

    static const TypeNode *compound_sequence_element(const TypeNode *type) {
        if (type->kind == TYPE_LIST)
            return type->args.empty() ? type_any() : type->args[0];
        if (type->args.empty())
            return type_any();
        const TypeNode *joined = type->args[0];
        for (std::size_t i = 1; i < type->args.size(); ++i)
            joined = join_types(joined, type->args[i]);
        return joined;
    }

    static const TypeNode *compound_sequence_result(const TypeNode *left,
                                                    const TypeNode *right) {
        if (left->kind == TYPE_LIST || right->kind == TYPE_LIST) {
            return type_container(
                TYPE_LIST,
                {join_types(compound_sequence_element(left),
                            compound_sequence_element(right))});
        }
        if (left->args.empty() || right->args.empty())
            return type_any();
        std::vector<const TypeNode *> args = left->args;
        args.insert(args.end(), right->args.begin(), right->args.end());
        return type_container(TYPE_TUPLE, args);
    }

    /*
     * Compute the static result type of `target op value` for a compound
     * assignment.  Unknown types (any, generics, unions) stay permissive; a
     * definite mismatch is reported with the same wording the runtime uses
     * for the corresponding binary operator.
     */
    static const TypeNode *compound_result_type(
        int op, const TypeNode *left, const TypeNode *right, bool &invalid,
        std::string &message) {
        invalid = false;
        if (!compound_type_known(left) || !compound_type_known(right))
            return type_any();
        const std::string left_name = type_display(left);
        const std::string right_name = type_display(right);

        if (op == '&' || op == '|' || op == '^' || op == 's' || op == 'r') {
            if (left->kind == TYPE_INT && right->kind == TYPE_INT)
                return type_builtin(TYPE_INT);
            /* `number` may hold an int at runtime; leave that to the VM. */
            if (left->kind == TYPE_NUMBER || right->kind == TYPE_NUMBER)
                return type_any();
            invalid = true;
            message = (op == 's' || op == 'r')
                          ? "shift operator expects ints, got "
                          : "bitwise operator expects ints, got ";
            message += left_name;
            message += " and ";
            message += right_name;
            return nullptr;
        }

        if (op == '+') {
            if (compound_number(left) && compound_number(right))
                return compound_number_result(left, right);
            if (compound_chars(left) && compound_chars(right))
                return type_builtin(TYPE_STRING);
            if (compound_sequence(left) && compound_sequence(right))
                return compound_sequence_result(left, right);
            invalid = true;
            message = "cannot add ";
            message += left_name;
            message += " and ";
            message += right_name;
            return nullptr;
        }

        if (op == '-' || op == '*' || op == '/' || op == '%') {
            if (compound_number(left) && compound_number(right))
                return compound_number_result(left, right);
            invalid = true;
            if (op == '/') {
                message = "cannot divide ";
                message += left_name;
                message += " by ";
                message += right_name;
            } else if (op == '%') {
                message = "cannot take ";
                message += left_name;
                message += " modulo ";
                message += right_name;
            } else {
                message = "cannot use ";
                message.push_back(static_cast<char>(op));
                message += " on ";
                message += left_name;
                message += " and ";
                message += right_name;
            }
            return nullptr;
        }

        return type_any();
    }

    bool check_compound_op(struct ast *target, struct ast *value,
                           const TypeNode *target_type, int op,
                           bool enforce_target, Env &env,
                           const TypeNode *&result_out) {
        result_out = type_any();
        if (!value)
            return true;
        const TypeNode *rhs = infer(value, env);
        bool invalid = false;
        std::string message;
        const TypeNode *result =
            compound_result_type(op, target_type, rhs, invalid, message);
        if (invalid) {
            report_error(target->line, target->column, message.c_str());
            return false;
        }
        result_out = result;
        if (!enforce_target || !target_type ||
            target_type->kind == TYPE_ANY)
            return true;
        if (match_type(result, target_type) == MatchResult::Mismatch) {
            char diagnostic[512];
            std::snprintf(diagnostic, sizeof(diagnostic),
                          "expected %s, got %s",
                          type_display(target_type), type_display(result));
            report_error(target->line, target->column, diagnostic);
            return false;
        }
        return true;
    }

    bool check_lvalue(struct ast *target, struct ast *value, Env &env,
                      bool compound, int op = 0) {
        if (!target)
            return true;
        const TypeNode *target_type = type_any();
        if (!resolve_lvalue(target, env, target_type))
            return false;
        if (!value)
            return true;
        if (compound) {
            const TypeNode *result = type_any();
            return check_compound_op(target, value, target_type, op, true,
                                     env, result);
        }
        bool force_element =
            target->nodetype == 'G' || needs_static_check(target_type);
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
        if (node->nodetype == NODE_REF_TARGET) {
            const TypeNode *base = infer(node->l, env);
            out_type = base && base->kind == TYPE_REF && !base->args.empty()
                           ? base->args[0]
                           : type_any();
            return true;
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
        return detail::generic_defaults_key(source, name);
    }

    const TypeNode *unwrap_ref_type(const TypeNode *type) {
        if (!type)
            return type;
        if (type->kind == TYPE_REF || type->kind == TYPE_WEAK) {
            const TypeNode *inner = type_element(type, 0);
            return inner ? unwrap_ref_type(inner) : type;
        }
        if (type->nullable) {
            const TypeNode *inner =
                type_container(type->kind, type->args, false);
            if (inner && inner != type)
                return unwrap_ref_type(inner);
        }
        return type;
    }

    StaticTypeInfo *find_static_info(const TypeNode *type) {
        type = unwrap_ref_type(type);
        if (!type || type->kind != TYPE_NAMED)
            return NULL;
        auto it = static_types.find(
            static_type_key(type->source.c_str(), type->name.c_str()));
        return it == static_types.end() ? NULL : &it->second;
    }

    const TypeNode *static_field_type(const TypeNode *type,
                                      const char *field) {
        StaticTypeInfo *info = find_static_info(type);
        const ConstTypeMap *fields = NULL;
        const std::vector<std::string> *params = NULL;
        if (info) {
            fields = &info->fields;
            params = &info->generic_params;
        } else if (type && type->kind == TYPE_NAMED) {
            auto meta = g_generic_type_meta.find(
                static_type_key(type->source.c_str(), type->name.c_str()));
            if (meta != g_generic_type_meta.end()) {
                fields = &meta->second.fields;
                params = &meta->second.params;
            }
        }
        if (!fields)
            return type_any();
        auto it = fields->find(field ? field : "");
        if (it == fields->end())
            return type_any();
        if (!params || params->empty() || !type ||
            type->args.size() != params->size())
            return it->second;
        TypeBindings bindings;
        for (std::size_t i = 0; i < params->size(); ++i)
            bindings[(*params)[i]] = type->args[i];
        return instantiate_type(it->second, bindings);
    }

    GenericConstraintMeta generic_constraint_meta(
        const struct generic_constraint *constraint) {
        GenericConstraintMeta meta;
        if (!constraint)
            return meta;
        meta.kind = constraint->kind;
        meta.category = constraint->category;
        for (struct generic_constraint_item *item = constraint->items;
             item; item = item->next) {
            if (item->type)
                meta.items.push_back(type_from_constraint(item->type));
        }
        return meta;
    }

    bool type_is_protocol_name(const TypeNode *type) {
        if (!type || type->kind != TYPE_NAMED)
            return false;
        structtype_lookup_result lookup;
        return lookup_named_type(type, lookup) && lookup.protocol;
    }

    bool check_type_argument_is_type(const TypeNode *arg, int line,
                                     int column) {
        if (!type_is_protocol_name(arg))
            return true;
        std::string message =
            std::string("proto ") + type_display(arg) +
            " cannot be used as a type argument";
        report_error(line, column, message.c_str());
        return false;
    }

    bool type_satisfies_generic_constraint(
        const TypeNode *arg, const GenericConstraintMeta &constraint) {
        if (!arg || arg->kind == TYPE_ANY || type_contains_vars(arg))
            return true;
        if (constraint.items.empty())
            return true;

        if (arg->kind == TYPE_UNION) {
            for (const TypeNode *member : arg->args) {
                if (!type_satisfies_generic_constraint(member, constraint))
                    return false;
            }
            return true;
        }

        auto is_hashable = [](const TypeNode *type) {
            return key_contract(type) != KeyContract::No;
        };
        auto is_iterable = [](const TypeNode *type) {
            return iterable_contract(type) != IterableContract::No;
        };
        auto protocol_lookup = [&](const TypeNode *item,
                                   structtype_lookup_result &out) {
            return lookup_named_type(item, out) && out.protocol;
        };
        auto protocol_matches = [&](const TypeNode *item,
                                    const structtype_lookup_result &lookup) {
            std::unordered_map<std::string, struct type_constraint> bindings;
            if (match_protocol(arg, lookup.protocol, &bindings) !=
                ProtocolMatch::Yes)
                return false;
            if (item->args.empty())
                return true;
            if (item->args.size() !=
                lookup.protocol->generic_params.size())
                return false;
            for (std::size_t i = 0; i < item->args.size(); ++i) {
                auto binding =
                    bindings.find(lookup.protocol->generic_params[i]);
                if (binding == bindings.end())
                    return false;
                const TypeNode *bound =
                    type_from_constraint(&binding->second);
                if (!type_equal(bound, item->args[i]))
                    return false;
            }
            return true;
        };

        if (constraint.kind == GENERIC_CONSTRAINT_OR) {
            if (constraint.category == GENERIC_CONSTRAINT_PROTOCOL)
                return false;
            for (const TypeNode *item : constraint.items) {
                if (!item)
                    continue;
                if (item->kind == TYPE_ANY)
                    return true;
                if (item->kind == TYPE_NAMED &&
                    (item->name == "hashable" || item->name == "iterable"))
                    return false;
                structtype_lookup_result protocol;
                if (protocol_lookup(item, protocol))
                    return false;
                if (type_equal(arg, item))
                    return true;
            }
            return false;
        }

        for (const TypeNode *item : constraint.items) {
            if (!item)
                continue;
            if (item->kind == TYPE_ANY)
                continue;
            if (item->kind == TYPE_NAMED && item->name == "hashable") {
                if (!is_hashable(arg))
                    return false;
                continue;
            }
            if (item->kind == TYPE_NAMED && item->name == "iterable") {
                if (!is_iterable(arg))
                    return false;
                continue;
            }
            structtype_lookup_result protocol;
            if (protocol_lookup(item, protocol)) {
                if (!protocol_matches(item, protocol))
                    return false;
                continue;
            }
            if (constraint.category == GENERIC_CONSTRAINT_PROTOCOL)
                return false;
            if (!type_equal(arg, item))
                return false;
        }
        return true;
    }

    bool validate_generic_constraint(const GenericConstraintMeta &meta,
                                     const char *param, int line,
                                     int column) {
        if (meta.kind == GENERIC_CONSTRAINT_AND) {
            for (const TypeNode *item : meta.items) {
                if (!item)
                    continue;
                if (item->kind == TYPE_ANY) {
                    report_error(line, column,
                                 "'any' cannot be used with '&'");
                    return false;
                }
                if (item->kind == TYPE_NAMED &&
                    (item->name == "hashable" ||
                     item->name == "iterable"))
                    continue;
                structtype_lookup_result lookup;
                if (lookup_named_type(item, lookup) && lookup.protocol)
                    continue;
                std::string message =
                    std::string("concrete type ") + type_display(item) +
                    " cannot be used with '&' in the constraint for " +
                    (param ? param : "type parameter");
                report_error(line, column, message.c_str());
                return false;
            }
        } else if (meta.kind == GENERIC_CONSTRAINT_OR) {
            for (const TypeNode *item : meta.items) {
                if (!item)
                    continue;
                if (item->kind == TYPE_ANY) {
                    report_error(line, column,
                                 "'any' cannot be used with '|'");
                    return false;
                }
                if (item->kind == TYPE_NAMED &&
                    (item->name == "hashable" ||
                     item->name == "iterable")) {
                    std::string message =
                        std::string("'") + item->name +
                        "' cannot be used with '|'";
                    report_error(line, column, message.c_str());
                    return false;
                }
                structtype_lookup_result lookup;
                if (lookup_named_type(item, lookup) && lookup.protocol) {
                    std::string message =
                        std::string("proto ") + type_display(item) +
                        " cannot be used with '|'";
                    report_error(line, column, message.c_str());
                    return false;
                }
            }
        }
        return true;
    }

    bool collect_generic_param_meta(
        struct generic_param *params, std::vector<std::string> &names,
        std::vector<GenericConstraintMeta> &constraints,
        std::vector<const TypeNode *> &defaults, int line, int column) {
        std::unordered_set<std::string> seen;
        bool saw_default = false;
        for (struct generic_param *param = params; param;
             param = param->next) {
            if (!param->name || !*param->name)
                continue;
            if (!seen.insert(param->name).second) {
                std::string message =
                    std::string("generic type parameter repeated ") +
                    param->name;
                report_error(line, column, message.c_str());
                return false;
            }
            const TypeNode *default_type =
                param->default_type
                    ? type_from_constraint(param->default_type)
                    : nullptr;
            if (default_type)
                saw_default = true;
            else if (saw_default) {
                report_error(line, column,
                             "default type parameters must be trailing");
                return false;
            }
            GenericConstraintMeta meta =
                generic_constraint_meta(param->constraint);
            if (!validate_generic_constraint(meta, param->name, line,
                                             column))
                return false;
            if (default_type &&
                !type_satisfies_generic_constraint(default_type, meta)) {
                std::string message =
                    std::string("default type ") +
                    type_display(default_type) +
                    " does not satisfy the constraint for " +
                    param->name;
                report_error(line, column, message.c_str());
                return false;
            }
            names.push_back(param->name);
            constraints.push_back(std::move(meta));
            defaults.push_back(default_type);
        }
        return true;
    }

    void register_generic_defaults(
        const char *source, const char *name,
        const std::vector<std::string> &params,
        const std::vector<const TypeNode *> &defaults) {
        std::string key = static_type_key(source, name);
        if (params.empty() || defaults.size() != params.size()) {
            g_generic_default_types.erase(key);
            return;
        }
        for (const TypeNode *default_type : defaults) {
            if (!default_type) {
                g_generic_default_types.erase(key);
                return;
            }
        }
        g_generic_default_types[key] = defaults;
    }

    const std::vector<GenericConstraintMeta> *generic_constraints_for(
        const char *source, const char *name) {
        std::string key = static_type_key(source, name);
        auto it = static_types.find(key);
        if (it != static_types.end() && !it->second.generic_params.empty()) {
            /* GenericTypeMeta carries the normalized constraints. */
        }
        auto meta = g_generic_type_meta.find(key);
        if (meta == g_generic_type_meta.end() ||
            meta->second.constraints.empty())
            return NULL;
        return &meta->second.constraints;
    }

    bool check_generic_type_arguments(
        const TypeNode *type, const std::vector<const TypeNode *> &args,
        int line, int column) {
        if (!type || type->kind != TYPE_NAMED)
            return true;
        for (const TypeNode *arg : args) {
            if (!check_type_argument_is_type(arg, line, column))
                return false;
        }
        const std::vector<GenericConstraintMeta> *constraints =
            generic_constraints_for(type->source.c_str(), type->name.c_str());
        if (!constraints)
            return true;
        for (std::size_t i = 0;
             i < args.size() && i < constraints->size(); ++i) {
            if (type_satisfies_generic_constraint(args[i], (*constraints)[i]))
                continue;
            std::string message =
                std::string("type argument ") + type_display(args[i]) +
                " does not satisfy generic constraint for " + type->name;
            report_error(line, column, message.c_str());
            return false;
        }
        return true;
    }

    bool generic_bindings_for(const TypeNode *type,
                              TypeBindings &bindings) {
        if (!type || type->kind != TYPE_NAMED || type->args.empty())
            return false;
        const std::vector<std::string> *params = NULL;
        StaticTypeInfo *info = find_static_info(type);
        if (info && !info->generic_params.empty())
            params = &info->generic_params;
        else {
            auto meta = g_generic_type_meta.find(
                static_type_key(type->source.c_str(), type->name.c_str()));
            if (meta != g_generic_type_meta.end() &&
                !meta->second.params.empty())
                params = &meta->second.params;
        }
        if (!params || params->size() != type->args.size())
            return false;
        for (std::size_t i = 0; i < params->size(); ++i)
            bindings[(*params)[i]] = type->args[i];
        return true;
    }

    struct funcdef *static_method(const TypeNode *type, const char *name) {
        StaticTypeInfo *info = find_static_info(type);
        if (!info)
            return NULL;
        auto it = info->methods.find(name ? name : "");
        if (it == info->methods.end())
            return NULL;
        struct funcdef *method = it->second;
        if (method && method->name && std::strcmp(method->name, "new") == 0)
            return NULL;
        return method;
    }

    const TypeNode *method_value_type(const TypeNode *base_type,
                                      const char *name) {
        if (!name)
            return NULL;
        if (base_type && base_type->kind == TYPE_TASK) {
            const TypeNode *element = type_element(base_type, 0);
            const TypeNode *result_type = type_container(
                TYPE_RESULT, {element, type_builtin(TYPE_ERROR)});
            if (std::strcmp(name, "wait") == 0)
                return make_function_type(TYPE_CALLABLE,
                                          {type_builtin(TYPE_INT), element},
                                          1);
            if (std::strcmp(name, "try_wait") == 0)
                return make_function_type(
                    TYPE_CALLABLE, {type_builtin(TYPE_INT), result_type}, 1);
            if (std::strcmp(name, "detach") == 0)
                return make_function_type(TYPE_CALLABLE, {base_type});
            if (std::strcmp(name, "detached") == 0 ||
                std::strcmp(name, "done") == 0 ||
                std::strcmp(name, "cancel") == 0)
                return make_function_type(TYPE_CALLABLE,
                                          {type_builtin(TYPE_BOOL)});
            if (std::strcmp(name, "catch") == 0)
                return make_function_type(
                    TYPE_CALLABLE,
                    {type_container(TYPE_TASK, {result_type})});
            return NULL;
        }
        base_type = unwrap_ref_type(base_type);
        if (!base_type || base_type->kind != TYPE_NAMED)
            return NULL;

        structtype_lookup_result lookup;
        if (lookup_type_node(base_type, lookup)) {
            const type_def *def =
                lookup.structure
                    ? static_cast<const type_def *>(lookup.structure)
                    : static_cast<const type_def *>(lookup.enumeration);
            if (def) {
                std::shared_ptr<function_def> method =
                    structtype_method(def, name);
                if (method && method->code &&
                    !structtype_is_constructor(method.get())) {
                    FunctionSig sig =
                        method_signature_from_code(method->code.get());
                    adjust_generator_result(sig, NULL, method->code.get());
                    TypeBindings bindings;
                    if (generic_bindings_for(base_type, bindings)) {
                        for (const TypeNode *&param : sig.params)
                            param = instantiate_type(param, bindings);
                        if (sig.result)
                            sig.result =
                                instantiate_type(sig.result, bindings);
                        if (sig.generator_send)
                            sig.generator_send = instantiate_type(
                                sig.generator_send, bindings);
                        if (sig.rest)
                            sig.rest = instantiate_type(sig.rest, bindings);
                    }
                    return function_type_from_sig(sig);
                }
            }
            if (lookup.protocol) {
                for (const method_sig &sig : lookup.protocol->methods) {
                    if (sig.name != name)
                        continue;
                    FunctionSig function_sig;
                    for (size_t i = 1; i < sig.params.size(); ++i)
                        function_sig.params.push_back(
                            type_from_constraint(&sig.params[i]));
                    function_sig.result =
                        sig.return_type
                            ? type_from_constraint(sig.return_type.get())
                            : type_any();
                    return function_type_from_sig(function_sig);
                }
            }
        }

        struct funcdef *method = static_method(base_type, name);
        if (method) {
            FunctionSig sig = method_signature_from_def(method);
            adjust_generator_result(sig, method, NULL);
            return function_type_from_sig(sig);
        }
        return NULL;
    }

    const TypeNode *field_type(const TypeNode *base_type,
                               const struct ast *field_node, Env &env) {
        (void)env;
        base_type = unwrap_ref_type(base_type);
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
                           Env &env, const TypeNode *&result,
                           bool wait_sugar) {
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
        std::string receiver_path;
        if (narrow_path(callee->l, receiver_path))
            invalidate_root_facts(env, path_root(receiver_path));

        if (wait_sugar && receiver && receiver->kind != TYPE_TASK &&
            receiver->kind != TYPE_ANY && receiver->kind != TYPE_VAR &&
            receiver->kind != TYPE_UNION &&
            receiver->kind != TYPE_INVALID) {
            char message[512];
            std::snprintf(message, sizeof(message),
                          "wait expects a task, got %s",
                          type_display(receiver));
            report_error(callee->line, callee->column, message);
            return false;
        }

        if (receiver && receiver->kind == TYPE_TASK) {
            const TypeNode *element = type_element(receiver, 0);
            const TypeNode *result_type = type_container(
                TYPE_RESULT, {element, type_builtin(TYPE_ERROR)});
            std::size_t max_args = 0;
            if (std::strcmp(method_name, "wait") == 0) {
                result = element;
                max_args = 1;
            } else if (std::strcmp(method_name, "try_wait") == 0) {
                result = result_type;
                max_args = 1;
            } else if (std::strcmp(method_name, "detach") == 0) {
                result = receiver;
            } else if (std::strcmp(method_name, "detached") == 0 ||
                       std::strcmp(method_name, "done") == 0 ||
                       std::strcmp(method_name, "cancel") == 0) {
                result = type_builtin(TYPE_BOOL);
            } else if (std::strcmp(method_name, "catch") == 0) {
                result = type_container(TYPE_TASK, {result_type});
            } else {
                return false;
            }
            if (args.size() > max_args) {
                char message[256];
                std::snprintf(message, sizeof(message),
                              "task.%s expects at most %zu argument(s)",
                              method_name, max_args);
                report_error(callee->line, callee->column, message);
                return false;
            }
            return true;
        }

        FunctionSig sig;
        bool found = false;
        const struct funcdef *method_def = NULL;
        const struct function_code *method_code = NULL;
        structtype_lookup_result lookup;
        if (lookup_type_node(receiver, lookup)) {
            const type_def *def =
                lookup.structure
                    ? static_cast<const type_def *>(lookup.structure)
                    : static_cast<const type_def *>(lookup.enumeration);
            if (def) {
                std::shared_ptr<function_def> method =
                    structtype_method(def, method_name);
                if (method && method->code &&
                    !structtype_is_constructor(method.get())) {
                    sig = method_signature_from_code(method->code.get());
                    method_code = method->code.get();
                    found = true;
                }
            }
            if (!found && lookup.protocol) {
                for (const method_sig &protocol_sig :
                     lookup.protocol->methods) {
                    if (protocol_sig.name != method_name)
                        continue;
                    for (size_t i = 1; i < protocol_sig.params.size(); ++i)
                        sig.params.push_back(
                            type_from_constraint(&protocol_sig.params[i]));
                    sig.result =
                        protocol_sig.return_type
                            ? type_from_constraint(
                                  protocol_sig.return_type.get())
                            : type_any();
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            struct funcdef *method = static_method(receiver, method_name);
            if (method) {
                sig = method_signature_from_def(method);
                method_def = method;
                found = true;
            }
        }
        if (!found)
            return false;

        adjust_generator_result(sig, method_def, method_code);
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
        sig.is_generator = function->def->code->is_generator;
        adjust_generator_result(sig, NULL, function->def->code.get());
        if (!check_call_args(sig, args, env, true))
            return false;
        result = signature_result_type(sig);
        return true;
    }

    bool check_polymorphic_call(const TypeNode *callee_type,
                                const std::vector<struct ast *> &args,
                                Env &env, const TypeNode *&result,
                                const GenericFunctionMeta *generic_meta = nullptr) {
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

        if (generic_meta) {
            for (std::size_t i = 0; i < generic_meta->params.size(); ++i) {
                const std::string &param = generic_meta->params[i];
                if (bindings.find(param) != bindings.end())
                    continue;
                if (i < generic_meta->defaults.size() &&
                    generic_meta->defaults[i])
                    bindings[param] = generic_meta->defaults[i];
            }
            for (std::size_t i = 0; i < generic_meta->params.size(); ++i) {
                auto binding = bindings.find(generic_meta->params[i]);
                if (binding == bindings.end() ||
                    i >= generic_meta->constraints.size())
                    continue;
                if (type_satisfies_generic_constraint(
                        binding->second, generic_meta->constraints[i]))
                    continue;
                int line = args.empty() ? 0 : args[0]->line;
                int column = args.empty() ? 0 : args[0]->column;
                std::string message =
                    std::string("type argument ") +
                    type_display(binding->second) +
                    " does not satisfy generic constraint for " +
                    generic_meta->name;
                report_error(line, column, message.c_str());
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

    bool direct_builtin_functype(const struct ast *callee, int &functype) {
        if (!callee || callee->nodetype != 'V')
            return false;
        const struct value_t *value = ((const struct val *)callee)->val;
        if (!value || value->type != T_builtin)
            return false;
        functype = ((const struct builtin_value_t *)value)->functype;
        return true;
    }

    struct BuiltinMethod {
        enum type_constraint_kind receiver;
        const char *name;
        const char *canonical;
        int functype;
    };

    static const BuiltinMethod *find_builtin_method(
        enum type_constraint_kind receiver, const std::string &name) {
        static const BuiltinMethod methods[] = {
            {TYPE_LIST, "len", "std.list.len", B_len},
            {TYPE_LIST, "iter", "std.list.iter", B_iter_elements},
            {TYPE_LIST, "slice", "std.list.slice", B_list_slice},
            {TYPE_LIST, "contains", "std.list.contains", B_list_contains},
            {TYPE_LIST, "index", "std.list.index", B_list_index},
            {TYPE_LIST, "reverse", "std.list.reverse", B_list_reverse},
            {TYPE_LIST, "sort", "std.list.sort", B_list_sort},
            {TYPE_LIST, "sort_by", "std.list.sort_by", B_list_sort_by},
            {TYPE_LIST, "sort_desc", "std.list.sort_desc", B_list_sort_desc},
            {TYPE_LIST, "is_sorted", "std.list.is_sorted",
             B_list_is_sorted},
            {TYPE_LIST, "append", "std.list.append", B_list_append},
            {TYPE_LIST, "map", "std.list.map", B_list_map},
            {TYPE_LIST, "filter", "std.list.filter", B_list_filter},
            {TYPE_LIST, "reduce", "std.list.reduce", B_list_reduce},
            {TYPE_LIST, "unique", "std.list.unique", B_list_unique},
            {TYPE_LIST, "unique_by", "std.list.unique_by",
             B_list_unique_by},
            {TYPE_LIST, "group_by", "std.list.group_by", B_list_group_by},
            {TYPE_LIST, "chunk", "std.list.chunk", B_list_chunk},
            {TYPE_LIST, "window", "std.list.window", B_list_window},
            {TYPE_LIST, "zip", "std.list.zip", B_list_zip},
            {TYPE_LIST, "top_k", "std.list.top_k", B_list_top_k},
            {TYPE_LIST, "binary_search", "std.list.binary_search",
             B_list_binary_search},

            {TYPE_DICT, "len", "std.dict.len", B_len},
            {TYPE_DICT, "get", "std.dict.get", B_dict_get},
            {TYPE_DICT, "has", "std.dict.has", B_dict_has},
            {TYPE_DICT, "keys", "std.dict.keys", B_dict_keys},
            {TYPE_DICT, "values", "std.dict.values", B_dict_values},
            {TYPE_DICT, "items", "std.dict.items", B_dict_items},
            {TYPE_DICT, "delete", "std.dict.delete", B_dict_delete},
            {TYPE_DICT, "merge", "std.dict.merge", B_dict_merge},
            {TYPE_DICT, "index_by", "std.dict.index_by",
             B_dict_index_by},
            {TYPE_DICT, "iter", "std.dict.iter", B_iter_items},
            {TYPE_DICT, "iter_keys", "std.dict.iter_keys", B_iter_keys},
            {TYPE_DICT, "iter_values", "std.dict.iter_values",
             B_iter_values},
            {TYPE_DICT, "iter_items", "std.dict.iter_items", B_iter_items},

            {TYPE_SET, "size", "std.set.size", B_set_size},
            {TYPE_SET, "iter", "std.set.iter", B_iter_elements},
            {TYPE_SET, "add", "std.set.add", B_set_add},
            {TYPE_SET, "remove", "std.set.remove", B_set_remove},
            {TYPE_SET, "has", "std.set.has", B_set_has},
            {TYPE_SET, "values", "std.set.values", B_set_values},
            {TYPE_SET, "union", "std.set.union", B_set_union},
            {TYPE_SET, "intersect", "std.set.intersect", B_set_intersect},
            {TYPE_SET, "difference", "std.set.difference",
             B_set_difference},
            {TYPE_SET, "is_subset", "std.set.is_subset", B_set_is_subset},

            {TYPE_DEQUE, "size", "std.deque.size", B_deque_size},
            {TYPE_DEQUE, "iter", "std.deque.iter", B_iter_elements},
            {TYPE_DEQUE, "push_front", "std.deque.push_front",
             B_deque_push_front},
            {TYPE_DEQUE, "push_back", "std.deque.push_back",
             B_deque_push_back},
            {TYPE_DEQUE, "peek_front", "std.deque.peek_front",
             B_deque_peek_front},
            {TYPE_DEQUE, "peek_back", "std.deque.peek_back",
             B_deque_peek_back},
            {TYPE_DEQUE, "pop_front", "std.deque.pop_front",
             B_deque_pop_front},
            {TYPE_DEQUE, "pop_back", "std.deque.pop_back",
             B_deque_pop_back},
            {TYPE_DEQUE, "to_list", "std.deque.to_list", B_deque_to_list},

            {TYPE_HEAP, "size", "std.heap.size", B_heap_size},
            {TYPE_HEAP, "iter", "std.heap.iter", B_iter_elements},
            {TYPE_HEAP, "push", "std.heap.push", B_heap_push},
            {TYPE_HEAP, "peek", "std.heap.peek", B_heap_peek},
            {TYPE_HEAP, "pop", "std.heap.pop", B_heap_pop},
            {TYPE_HEAP, "to_list", "std.heap.to_list", B_heap_to_list},

            {TYPE_STRING, "iter", "std.iter.elements", B_iter_elements},
            {TYPE_BYTES, "iter", "std.iter.elements", B_iter_elements},
            {TYPE_GENERATOR, "iter", "std.iter.elements", B_iter_elements},
            {TYPE_PAIR, "iter", "std.iter.elements", B_iter_elements},
            {TYPE_TUPLE, "iter", "std.iter.elements", B_iter_elements},
        };
        for (const BuiltinMethod &method : methods) {
            if (method.receiver == receiver && name == method.name)
                return &method;
        }
        return NULL;
    }

    static bool builtin_method_name(const struct ast *callee,
                                    std::string &name) {
        if (!callee || callee->nodetype != 'j' || !callee->r ||
            callee->r->nodetype != 'V')
            return false;
        const struct value_t *value = ((const struct val *)callee->r)->val;
        if (!value || value->type != T_string)
            return false;
        name = ((const struct string_value_t *)value)->value;
        return true;
    }

    bool check_explicit_generic_call(
        struct ufncall *call, struct index_node *bracket,
        const std::vector<struct ast *> &args, Env &env,
        const TypeNode *&result) {
        std::string name;
        if (!flatten_path(bracket->l, name) ||
            name.find('.') != std::string::npos) {
            report_error(bracket->line, bracket->column,
                         "explicit generic instantiation requires a "
                         "function name");
            return false;
        }
        auto generic = g_generic_function_params.find(name);
        if (generic == g_generic_function_params.end()) {
            auto local = generic_functions.find(name);
            if (local != generic_functions.end())
                generic = g_generic_function_params.emplace(
                    name, local->second).first;
        }
        FunctionSig sig;
        bool have_signature = false;
        auto function = functions.find(name);
        if (function != functions.end()) {
            sig = function->second;
            have_signature = true;
        } else {
            struct symbol *sym = lookupsym(name.c_str());
            if (sym && sym->val && sym->val->type == T_func) {
                struct func_value_t *fn = (struct func_value_t *)sym->val;
                if (fn->def && fn->def->code) {
                    sig = signature_from_symlist(fn->def->code->sl);
                    sig.result = fn->def->code->ret_type
                                     ? type_from_constraint(
                                           fn->def->code->ret_type.get())
                                     : type_any();
                    sig.is_generator = fn->def->code->is_generator;
                    have_signature = true;
                }
            }
        }
        if (generic == g_generic_function_params.end() || !have_signature) {
            report_error(bracket->line, bracket->column,
                         "explicit type arguments require a generic function");
            return false;
        }

        std::string text = diag_source_text(
            bracket->span.start_line, bracket->span.start_column + 1,
            bracket->span.end_line, bracket->span.end_column);
        std::vector<struct type_constraint *> constraints;
        std::string parse_error;
        if (!parse_type_list_text(text, source.c_str(), constraints,
                                  parse_error, true)) {
            report_error(bracket->line, bracket->column,
                         parse_error.empty()
                             ? "invalid explicit type arguments"
                             : parse_error.c_str());
            for (struct type_constraint *constraint : constraints)
                free_type_constraint(constraint);
            return false;
        }
        if (constraints.size() != generic->second.params.size()) {
            std::string message =
                "function " + name + " expects " +
                std::to_string(generic->second.params.size()) +
                " type argument(s), got " +
                std::to_string(constraints.size());
            report_error(bracket->line, bracket->column, message.c_str());
            for (struct type_constraint *constraint : constraints)
                free_type_constraint(constraint);
            return false;
        }

        TypeBindings bindings;
        for (std::size_t i = 0; i < constraints.size(); ++i) {
            const TypeNode *arg = type_from_constraint(constraints[i]);
            bindings[generic->second.params[i]] = arg;
            free_type_constraint(constraints[i]);
            if (!check_type_argument_is_type(arg, bracket->line,
                                             bracket->column))
                return false;
            if (i < generic->second.constraints.size() &&
                !type_satisfies_generic_constraint(
                    arg, generic->second.constraints[i])) {
                std::string message =
                    std::string("type argument ") + type_display(arg) +
                    " does not satisfy generic constraint for " + name;
                report_error(bracket->line, bracket->column,
                             message.c_str());
                return false;
            }
        }

        for (const TypeNode *&param : sig.params)
            param = instantiate_type(param, bindings);
        if (sig.result)
            sig.result = instantiate_type(sig.result, bindings);
        if (sig.rest)
            sig.rest = instantiate_type(sig.rest, bindings);
        if (!check_call_args(sig, args, env, true))
            return false;
        result = signature_result_type(sig);

        /* Type arguments are erased; the runtime call targets the function
         * directly once the semantic phase has validated them. */
        call->callee = bracket->l;
        bracket->l = NULL;
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
        if (has_path && path.find('.') == std::string::npos) {
            if (path == "list" && callee_is_builtin(path, B_list, env))
                canonical = "std.list.new";
            else if (path == "dict" && callee_is_builtin(path, B_dict, env))
                canonical = "std.dict.new";
        }
        bool spread = false;
        for (struct ast *arg : args) {
            if (arg && arg->nodetype == 'x') {
                spread = true;
                break;
            }
        }

        /*
         * Built-in container values expose their module functions as methods:
         * `xs.len()`, `d.iter_keys()`, `q.push_back(x)`, ...  Resolve from the
         * receiver's static container type and rewrite the call to the
         * corresponding builtin with the receiver as the first argument.
         */
        if (!spread && call->callee && call->callee->nodetype == 'j' &&
            call->callee->l) {
            std::string method;
            if (builtin_method_name(call->callee, method)) {
                const TypeNode *receiver =
                    infer(call->callee->l, env);
                if (receiver &&
                    (receiver->kind == TYPE_LIST ||
                     receiver->kind == TYPE_DICT ||
                     receiver->kind == TYPE_SET ||
                     receiver->kind == TYPE_DEQUE ||
                     receiver->kind == TYPE_HEAP ||
                     receiver->kind == TYPE_STRING ||
                     receiver->kind == TYPE_BYTES ||
                     receiver->kind == TYPE_GENERATOR ||
                     receiver->kind == TYPE_PAIR ||
                     receiver->kind == TYPE_TUPLE)) {
                    const BuiltinMethod *builtin =
                        find_builtin_method(receiver->kind, method);
                    if (builtin) {
                        std::vector<struct ast *> call_args;
                        call_args.push_back(call->callee->l);
                        call_args.insert(call_args.end(), args.begin(),
                                         args.end());
                        const TypeNode *builtin_result = type_any();
                        if (!check_builtin(*this, builtin->canonical,
                                           call_args, expected, env,
                                           builtin_result))
                            return false;
                        struct ast *receiver_expr = call->callee->l;
                        struct ast *field = call->callee->r;
                        call->callee->l = NULL;
                        call->callee->r = NULL;
                        treefree(field);
                        treefree(call->callee);
                        call->callee = newbuiltin(builtin->functype);
                        call->args =
                            newast('L', receiver_expr, call->args);
                        result = builtin_result;
                        return true;
                    }
                }
            }
        }

        if (!spread && call->callee &&
            call->callee->nodetype == 'G') {
            struct index_node *bracket =
                (struct index_node *)call->callee;
            if (bracket->generic_candidate)
                return check_explicit_generic_call(
                    call, bracket, args, env, result);
        }

        int direct_builtin = -1;
        if (!has_path &&
            direct_builtin_functype(call->callee, direct_builtin) &&
            direct_builtin == B_ref_wrap) {
            if (args.size() != 1) {
                char message[128];
                std::snprintf(message, sizeof(message),
                              "ref expects 1 argument, got %zu",
                              args.size());
                report_error(expr->line, expr->column, message);
                return false;
            }
            const TypeNode *argument = infer(args[0], env);
            if (argument->kind == TYPE_ANY) {
                result = type_any();
                return true;
            }
            if (argument->kind == TYPE_NAMED) {
                result = type_container(TYPE_REF, {argument});
                return true;
            }
            char message[256];
            std::snprintf(message, sizeof(message),
                          "ref expects a struct value, got %s",
                          type_display(argument));
            report_error(args[0]->line, args[0]->column, message);
            return false;
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
             canonical.rfind("std.generator.", 0) == 0 ||
             canonical == "std.set.from_list" ||
             canonical == "std.dict.index_by" ||
             canonical == "std.list.new" || canonical == "std.dict.new" ||
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
                        const GenericFunctionMeta *generic_meta = nullptr;
                        if (has_path &&
                            path.find('.') == std::string::npos) {
                            auto it = g_generic_function_params.find(path);
                            if (it == g_generic_function_params.end()) {
                                auto local = generic_functions.find(path);
                                if (local != generic_functions.end())
                                    it = g_generic_function_params
                                             .emplace(path, local->second)
                                             .first;
                            }
                            if (it != g_generic_function_params.end())
                                generic_meta = &it->second;
                        }
                        if (!check_polymorphic_call(callee_type, args, env,
                                                    result, generic_meta))
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
            if (check_method_call(call->callee, args, env, result,
                                  call->wait_sugar != 0))
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

        std::string package = alias_it->second;
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
            std::string candidate_module = package;
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
        /* The inner type of a ref/weak handle is needed to resolve fields
         * after an assignment; degrading it to `any` loses that knowledge. */
        if (type && (type->kind == TYPE_REF || type->kind == TYPE_WEAK))
            return;
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
                          bool compound, int op = 0) {
        if (!target)
            return true;
        invalidate_lvalue_facts(target, env);
        if (target->nodetype == 'N') {
            const char *name = ((struct symref *)target)->name;
            Env *owner = env.owner_of(name ? name : "");
            VarInfo info = owner
                               ? owner->vars[name ? name : ""]
                               : lookup_var(name ? name : "", env);
            if (info.fixed) {
                const TypeNode *declared_target =
                    info.declared ? info.declared : info.type;
                bool ok;
                if (compound) {
                    const TypeNode *result = type_any();
                    ok = check_compound_op(target, value, declared_target,
                                           op, true, env, result);
                } else {
                    ok = check_expr(value, declared_target, env, false,
                                    needs_static_check(declared_target));
                }
                if (info.declared && owner)
                    owner->vars[name ? name : ""].type =
                        info.declared;
                return ok;
            }
            if (compound) {
                const TypeNode *result = type_any();
                if (!check_compound_op(target, value, info.type, op, false,
                                       env, result))
                    return false;
                if (owner) {
                    VarInfo &assigned = owner->vars[name ? name : ""];
                    assigned.type = owner == &env ? result : type_any();
                    assigned.declared = nullptr;
                    assigned.fixed = false;
                    if (assigned.exported && !package.empty())
                        g_exported_types[package][name ? name : ""] =
                            assigned;
                }
                return true;
            }
            const TypeNode *rhs =
                value ? infer(value, env) : type_any();
            if (owner) {
                VarInfo &assigned = owner->vars[name ? name : ""];
                assigned.type = owner == &env ? rhs : type_any();
                assigned.declared = nullptr;
                assigned.fixed = false;
                if (assigned.exported && !package.empty())
                    g_exported_types[package][name ? name : ""] = assigned;
            }
            return true;
        }

        bool fixed = expr_fixed(lvalue_root(target), env);
        if (fixed)
            return check_lvalue(target, value, env, compound, op);

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

    const TypeNode *inferred_generator_type(const struct funcdef *def,
                                            bool send) {
        if (!def)
            return NULL;
        if (send) {
            auto it = generator_sends.find(def);
            if (it != generator_sends.end())
                return it->second;
        } else {
            auto it = generator_elements.find(def);
            if (it != generator_elements.end())
                return it->second;
        }
        std::string key = generator_identity(def);
        std::string location = generator_location_identity(def);
        if (send) {
            auto it = g_generator_send_type_map.find(key);
            if (it == g_generator_send_type_map.end())
                it = g_generator_send_type_map.find(location);
            return it == g_generator_send_type_map.end() ? NULL
                                                         : it->second;
        }
        auto it = g_generator_element_types.find(key);
        if (it == g_generator_element_types.end())
            it = g_generator_element_types.find(location);
        return it == g_generator_element_types.end() ? NULL : it->second;
    }

    /*
     * The forwarder for a decorated generator method is an ordinary function
     * returning a generator.  Recover the yield/send types from the hidden
     * method body and store them as the forwarder's static return annotation
     * so later statements in the same source see generator[T, S].
     */
    void annotate_decorated_generator(struct funcdef *def) {
        if (!def || !def->decorated_generator_source || def->ret_type)
            return;
        const struct funcdef *source = def->decorated_generator_source;
        const TypeNode *element =
            inferred_generator_type(source, false);
        const TypeNode *send = inferred_generator_type(source, true);
        struct type_constraint *annotation =
            new_type_constraint(TYPE_GENERATOR);
        const TypeNode *node = type_container(
            TYPE_GENERATOR, {element ? element : type_any(),
                             send ? send : type_any()});
        annotation->display = node->display.c_str();
        annotation->arity = 0;
        annotation->detail = node;
        def->ret_type = annotation;
    }

    void adjust_generator_result(FunctionSig &sig,
                                 const struct funcdef *def,
                                 const struct function_code *code) {
        if (!sig.is_generator)
            return;
        const TypeNode *element = NULL;
        const TypeNode *send = NULL;
        if (def) {
            auto it = generator_elements.find(def);
            if (it != generator_elements.end())
                element = it->second;
            auto send_it = generator_sends.find(def);
            if (send_it != generator_sends.end())
                send = send_it->second;
        }
        if (code) {
            std::string key = generator_identity(code);
            std::string location = generator_location_identity(code);
            if (!element) {
                auto it = g_generator_element_types.find(key);
                if (it == g_generator_element_types.end())
                    it = g_generator_element_types.find(location);
                if (it != g_generator_element_types.end())
                    element = it->second;
            }
            if (!send) {
                auto it = g_generator_send_type_map.find(key);
                if (it == g_generator_send_type_map.end())
                    it = g_generator_send_type_map.find(location);
                if (it != g_generator_send_type_map.end())
                    send = it->second;
            }
        }
        if (element)
            sig.result = element;
        if (send)
            sig.generator_send = send;
    }

    const TypeNode *function_type_from_code(
        const struct function_code *code) {
        if (!code)
            return type_builtin(TYPE_FUNCTION);
        FunctionSig sig = signature_from_symlist(code->sl);
        sig.result = code->ret_type ? type_from_constraint(code->ret_type.get())
                                    : type_any();
        sig.is_generator = code->is_generator;
        adjust_generator_result(sig, NULL, code);
        return function_type_from_sig(sig);
    }

    const TypeNode *function_type_from_def(
        struct funcdef *def, Env &env,
        const TypeNode *expected = nullptr) {
        if (!def)
            return type_builtin(TYPE_FUNCTION);
        FunctionSig sig = signature_from_symlist(def->sl);
        sig.is_generator = def->is_generator;
        if (def->is_generator) {
            auto it = generator_elements.find(def);
            sig.result = it == generator_elements.end()
                             ? type_any()
                             : it->second;
            auto send = generator_sends.find(def);
            sig.generator_send = send == generator_sends.end()
                                     ? type_any()
                                     : send->second;
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
                sig.is_generator = fn->def->code->is_generator;
                adjust_generator_result(sig, NULL, fn->def->code.get());
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
        if (def->ret_type)
            return type_from_constraint(def->ret_type);
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
        return infer(return_expr, local);
    }

    void bind_pattern_types(const struct ast *node,
                            const TypeNode *value, Env &env,
                            bool fixed = false,
                            bool allow_unproven_tuple = false) {
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
                    bind_pattern_types(pattern->items, value, env, fixed,
                                       allow_unproven_tuple);
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
            case NODE_PATTERN_TUPLE: {
                std::size_t arity = 0;
                for (const struct pattern_node *item =
                         (const struct pattern_node *)pattern->items;
                     item; item = (const struct pattern_node *)item->next)
                    ++arity;
                /*
                 * A nullable pair/tuple (union[T, null]) still has a unique
                 * tuple shape.  A union with another non-null member is not
                 * proven at this point and stays rejected.
                 */
                const TypeNode *tuple_value = value;
                if (tuple_value && tuple_value->kind == TYPE_UNION) {
                    const TypeNode *found = nullptr;
                    bool ambiguous = false;
                    for (const TypeNode *member : tuple_value->args) {
                        if (!member || member->kind == TYPE_NULL)
                            continue;
                        if (member->kind == TYPE_PAIR ||
                            member->kind == TYPE_TUPLE) {
                            if (found) {
                                ambiguous = true;
                                break;
                            }
                            found = member;
                        } else {
                            ambiguous = true;
                            break;
                        }
                    }
                    tuple_value = ambiguous ? nullptr : found;
                }
                const bool proven =
                    tuple_value && (tuple_value->kind == TYPE_PAIR ||
                                    tuple_value->kind == TYPE_TUPLE);
                if (!proven && !allow_unproven_tuple) {
                    char message[192];
                    std::snprintf(
                        message, sizeof(message),
                        "tuple pattern requires a pair or tuple, got %s",
                        value ? type_display(value) : "null");
                    report_error(node->line, node->column, message);
                }
                if (proven && tuple_value->args.size() != arity) {
                    char message[192];
                    std::snprintf(
                        message, sizeof(message),
                        "tuple pattern expects %zu element(s), got %zu",
                        arity, tuple_value->args.size());
                    report_error(node->line, node->column, message);
                }
                std::size_t index = 0;
                for (const struct pattern_node *item =
                         (const struct pattern_node *)pattern->items;
                     item;
                     item = (const struct pattern_node *)item->next,
                     ++index) {
                    const TypeNode *element =
                        proven && index < tuple_value->args.size()
                            ? tuple_value->args[index]
                            : type_any();
                    bind_pattern_types((const struct ast *)item, element,
                                       env, fixed, allow_unproven_tuple);
                }
                return;
            }
            case NODE_PATTERN_LIST: {
                if (value && (value->kind == TYPE_PAIR ||
                              value->kind == TYPE_TUPLE)) {
                    std::size_t index = 0;
                    const TypeNode *rest_element =
                        type_element(value, 0);
                    for (std::size_t i = 1; i < value->args.size(); ++i)
                        rest_element =
                            join_types(rest_element, value->args[i]);
                    const TypeNode *rest =
                        type_container(TYPE_LIST, {rest_element});
                    for (const struct pattern_node *item =
                             (const struct pattern_node *)pattern->items;
                         item;
                         item = (const struct pattern_node *)item->next) {
                        if (item->nodetype == NODE_PATTERN_REST) {
                            bind_pattern_types((const struct ast *)item,
                                               rest, env, fixed,
                                               allow_unproven_tuple);
                        } else {
                            const TypeNode *element =
                                index < value->args.size()
                                    ? value->args[index]
                                    : type_any();
                            bind_pattern_types((const struct ast *)item,
                                               element, env, fixed,
                                               allow_unproven_tuple);
                            ++index;
                        }
                    }
                    return;
                }
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
                                           env, fixed,
                                           allow_unproven_tuple);
                    else
                        bind_pattern_types((const struct ast *)item, element,
                                           env, fixed,
                                           allow_unproven_tuple);
                }
                return;
            }
            case NODE_PATTERN_CONTAINER: {
                if (pattern->name) {
                    const TypeNode *pattern_type =
                        type_named(pattern->source, pattern->name);
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
                                               env, fixed,
                                               allow_unproven_tuple);
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
                                           fixed, allow_unproven_tuple);
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
                                       fixed, allow_unproven_tuple);
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
        const char *source = nullptr;
        struct struct_init *fields = nullptr;
        struct_def *def = nullptr;
        enum_def *edef = nullptr;
        struct variant_def *variant = nullptr;
        const char *variant_name = nullptr;
        bool has_type_args = false;
        struct source_span type_args_span = {};

        if (expr->nodetype == 'y') {
            struct struct_lit_node *lit =
                (struct struct_lit_node *)expr;
            name = lit->name;
            source = lit->source;
            fields = lit->fields;
            has_type_args = lit->has_type_args;
            type_args_span = lit->type_args_span;
            def = structtype_find_struct(source, name);
        } else if (expr->nodetype == '?') {
            struct enum_lit_node *lit = (struct enum_lit_node *)expr;
            name = lit->name;
            source = lit->source;
            fields = lit->fields;
            variant_name = lit->variant;
            has_type_args = lit->has_type_args;
            type_args_span = lit->type_args_span;
            edef = structtype_find_enum(source, name);
            if (edef && lit->variant) {
                auto it = edef->variant_index.find(lit->variant);
                if (it != edef->variant_index.end())
                    variant = &edef->variants[(size_t)it->second];
            }
        }

        const TypeNode *literal_type = type_named(source, name);
        StaticTypeInfo *info = find_static_info(literal_type);
        ConstTypeMap *static_fields = NULL;
        const std::vector<std::string> *generic_params = NULL;
        const std::vector<const TypeNode *> *generic_defaults = NULL;
        if (info) {
            if (info->is_enum && variant_name) {
                auto it = info->variants.find(variant_name);
                if (it != info->variants.end())
                    static_fields = &it->second;
            } else if (!info->is_enum) {
                static_fields = &info->fields;
            }
            if (!info->generic_params.empty()) {
                generic_params = &info->generic_params;
                generic_defaults = &info->generic_defaults;
            }
        }
        if (!static_fields || !generic_params) {
            auto meta = g_generic_type_meta.find(
                static_type_key(source, name));
            if (meta != g_generic_type_meta.end()) {
                if (!static_fields) {
                    if (variant_name) {
                        auto variant_fields =
                            meta->second.variants.find(variant_name);
                        if (variant_fields != meta->second.variants.end())
                            static_fields = &variant_fields->second;
                    } else if (!meta->second.fields.empty()) {
                        static_fields = &meta->second.fields;
                    }
                }
                if (!meta->second.params.empty()) {
                    generic_params = &meta->second.params;
                    generic_defaults = &meta->second.defaults;
                }
            }
        }

        const bool generic_literal =
            generic_params && !generic_params->empty();
        TypeBindings bindings;
        std::vector<const TypeNode *> explicit_args;
        bool explicit_bindings = false;
        if (has_type_args) {
            if (!generic_params) {
                std::string message =
                    std::string("type ") + (name ? name : "invalid") +
                    " does not accept type arguments";
                report_error(expr->line, expr->column, message.c_str());
                return false;
            }
            std::string text = diag_source_text(
                type_args_span.start_line,
                type_args_span.start_column + 1,
                type_args_span.end_line,
                type_args_span.end_column);
            std::vector<struct type_constraint *> constraints;
            std::string parse_error;
            if (!parse_type_list_text(text, source, constraints,
                                      parse_error, true)) {
                report_error(expr->line, expr->column,
                             parse_error.empty() ? "invalid type arguments"
                                                 : parse_error.c_str());
                for (struct type_constraint *constraint : constraints)
                    free_type_constraint(constraint);
                return false;
            }
            if (constraints.size() != generic_params->size()) {
                std::string message =
                    std::string("type ") + (name ? name : "invalid") +
                    " expects " + std::to_string(generic_params->size()) +
                    " type argument(s), got " +
                    std::to_string(constraints.size());
                report_error(expr->line, expr->column, message.c_str());
                for (struct type_constraint *constraint : constraints)
                    free_type_constraint(constraint);
                return false;
            }
            for (std::size_t i = 0; i < constraints.size(); ++i) {
                const TypeNode *arg = type_from_constraint(constraints[i]);
                if (!check_type_argument_is_type(arg, expr->line,
                                                 expr->column))
                    return false;
                explicit_args.push_back(arg);
                bindings[(*generic_params)[i]] = arg;
                free_type_constraint(constraints[i]);
            }
            explicit_bindings = true;
        }
        auto check_initializer = [&](struct ast *value,
                                     const TypeNode *expected) -> bool {
            if (!value)
                return true;
            if (!expected) {
                infer(value, env);
                return true;
            }
            const TypeNode *actual = infer(value, env);
            if (generic_literal && !explicit_bindings &&
                type_contains_vars(expected)) {
                if (!bind_type(actual, expected, bindings)) {
                    char message[512];
                    std::snprintf(
                        message, sizeof(message), "expected %s, got %s",
                        type_display(instantiate_type(expected, bindings)),
                        type_display(actual));
                    report_error(value->line, value->column, message);
                    return false;
                }
                expected = instantiate_type(expected, bindings);
            } else if (explicit_bindings && type_contains_vars(expected)) {
                expected = instantiate_type(expected, bindings);
            }
            if (explicit_bindings)
                return warn_or_error(actual, expected, value->line,
                                     value->column, false, true);
            return check_expr(value, expected, env, false,
                              needs_static_check(expected));
        };

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
                if (!check_initializer(
                        init->value, type_from_constraint(field_type)))
                    return false;
                continue;
            }
            if (static_fields) {
                auto it = static_fields->find(init->name);
                if (it != static_fields->end()) {
                    if (!check_initializer(init->value, it->second))
                        return false;
                    continue;
                }
            }
            if (init->value)
                infer(init->value, env);
        }

        if (generic_literal) {
            std::vector<const TypeNode *> args;
            if (explicit_bindings) {
                args = explicit_args;
            } else {
                for (std::size_t i = 0; i < generic_params->size(); ++i) {
                    const std::string &param = (*generic_params)[i];
                    auto it = bindings.find(param);
                    if (it != bindings.end()) {
                        args.push_back(it->second);
                    } else if (generic_defaults &&
                               i < generic_defaults->size() &&
                               (*generic_defaults)[i]) {
                        args.push_back((*generic_defaults)[i]);
                    } else {
                        args.push_back(type_any());
                    }
                }
            }
            if (!check_generic_type_arguments(literal_type, args,
                                              expr->line, expr->column))
                return false;
            generic_literal_args[expr] = args;
        }
        return true;
    }

    bool check_type_contracts(const TypeNode *type, int line, int column) {
        if (!type)
            return true;

        bool ok = true;
        if (type->kind == TYPE_NAMED && !type->args.empty()) {
            std::string key =
                static_type_key(type->source.c_str(), type->name.c_str());
            const std::vector<std::string> *params = NULL;
            auto it = static_types.find(key);
            if (it != static_types.end())
                params = &it->second.generic_params;
            else {
                auto meta = g_generic_type_meta.find(key);
                if (meta != g_generic_type_meta.end())
                    params = &meta->second.params;
            }
            if (params) {
                if (params->empty()) {
                    std::string message =
                        std::string("type ") + type->name +
                        " does not accept type arguments";
                    report_error(line, column, message.c_str());
                    ok = false;
                } else if (type->args.size() != params->size()) {
                    std::string message =
                        std::string("type ") + type->name + " expects " +
                        std::to_string(params->size()) +
                        " type argument(s), got " +
                        std::to_string(type->args.size());
                    report_error(line, column, message.c_str());
                    ok = false;
                } else {
                    ok = check_generic_type_arguments(
                             type, type->args, line, column) &&
                         ok;
                }
            }
        }
        if (type->kind == TYPE_DICT && !type->args.empty()) {
            const TypeNode *key = type->args[0];
            if (key_contract(key) == KeyContract::No) {
                std::string message =
                    std::string("dict key type ") + type_display(key) +
                    " is not hashable; remove mutable fields or implement "
                    "std.dict_key.DictKey";
                report_error(line, column, message.c_str());
                ok = false;
            }
        } else if (type->kind == TYPE_SET && !type->args.empty()) {
            const TypeNode *element = type->args[0];
            if (key_contract(element) == KeyContract::No) {
                std::string message =
                    std::string("set element type ") + type_display(element) +
                    " is not hashable; remove mutable fields or implement "
                    "std.dict_key.DictKey";
                report_error(line, column, message.c_str());
                ok = false;
            }
        }

        for (const TypeNode *arg : type->args) {
            if (!check_type_contracts(arg, line, column))
                ok = false;
        }
        return ok;
    }

    bool check_funcdef(struct funcdef *def, Env &env,
                       const TypeNode *self_type = nullptr) {
        if (!def)
            return true;
        annotate_decorated_generator(def);
        FunctionSig sig;
        if (def->sl)
            sig = signature_from_symlist(def->sl);
        sig.result = def->ret_type ? type_from_constraint(def->ret_type)
                                   : type_any();
        sig.is_generator = def->is_generator;
        if (def->name) {
            functions[def->name] = sig;
            if (def->generic_params) {
                GenericFunctionMeta meta;
                meta.name = def->name;
                if (!collect_generic_param_meta(
                        def->generic_params, meta.params,
                        meta.constraints, meta.defaults, def->line,
                        def->column))
                    return false;
                if (!meta.params.empty()) {
                    generic_functions[def->name] = meta;
                    g_generic_function_params[def->name] = meta;
                }
            }
        }

        if (def->ret_type &&
            !check_type_contracts(type_from_constraint(def->ret_type),
                                  def->line, def->column))
            return false;

        for (struct symlist *item = def->sl; item; item = item->next) {
            if (item->type &&
                !check_type_contracts(type_from_constraint(item->type),
                                      def->line, def->column))
                return false;
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

        const TypeNode *effective_self = self_type;
        if (!effective_self && def->method_self_type)
            effective_self =
                type_from_constraint(def->method_self_type);
        Env local;
        local.parent = &env;
        for (struct symlist *item = def->sl; item; item = item->next) {
            const char *name =
                item->sym && item->sym->name ? item->sym->name : "";
            const TypeNode *type = type_from_constraint(item->type);
            if (item->variadic)
                type = type_container(TYPE_LIST, {type});
            const bool annotated = item->type != NULL;
            bool fixed = annotated;
            if (effective_self && std::string(name) == "self") {
                type = effective_self;
                fixed = true;
            }
            struct ast *pattern = symlist_pattern_ast(item);
            if (pattern && !annotated) {
                if (const TypeNode *implied =
                        tuple_pattern_param_type(item))
                    type = implied;
            }
            invalidate_root_facts(local, name);
            local.define(name, type, fixed);
            if (pattern)
                bind_pattern_types(pattern, type, local, fixed,
                                   !annotated);
        }

        /*
         * A decorated generator method forwarder returns whatever the
         * decorator chain produced.  Its signature is the original
         * generator[T, S], but the forwarding call itself is dynamically
         * typed, so do not type-check the return expression against it.
         */
        const TypeNode *return_expected =
            (!def->decorated_generator_source && def->ret_type)
                ? type_from_constraint(def->ret_type)
                : NULL;
        bool is_generator = def->is_generator != 0;
        const TypeNode *declared_generator = NULL;
        if (is_generator && return_expected &&
            return_expected->kind == TYPE_GENERATOR) {
            declared_generator = return_expected;
            return_expected = NULL;
        }
        const TypeNode *declared_yield =
            declared_generator ? type_element(declared_generator, 0)
                               : NULL;
        const TypeNode *declared_send =
            declared_generator ? type_element(declared_generator, 1)
                               : NULL;
        return_types.push_back(return_expected);
        FunctionSig *sig_entry = NULL;
        if (def->name) {
            auto it = functions.find(def->name);
            if (it != functions.end())
                sig_entry = &it->second;
        }
        if (is_generator) {
            yield_types.emplace_back();
            generator_send_types.push_back(declared_send
                                               ? declared_send
                                               : type_any());
            generator_yield_expected.push_back(declared_yield
                                                   ? declared_yield
                                                   : NULL);
        }
        bool ok = check_stmt(def->ast, local);
        if (is_generator) {
            const TypeNode *element = declared_yield;
            if (!element) {
                element = type_any();
                bool first = true;
                for (const TypeNode *type : yield_types.back()) {
                    element = first ? type : join_types(element, type);
                    first = false;
                }
            }
            const TypeNode *send = generator_send_types.back();
            yield_types.pop_back();
            generator_send_types.pop_back();
            generator_yield_expected.pop_back();
            generator_elements[def] = element;
            generator_sends[def] = send;
            std::string key = generator_identity(def);
            std::string location = generator_location_identity(def);
            g_generator_element_types[key] = element;
            g_generator_send_type_map[key] = send;
            g_generator_element_types[location] = element;
            g_generator_send_type_map[location] = send;
            if (ok && !error_) {
                std::shared_ptr<function_code> code =
                    get_function_code(def);
                if (code) {
                    std::string code_key = generator_identity(code.get());
                    std::string code_location =
                        generator_location_identity(code.get());
                    g_generator_element_types[code_key] = element;
                    g_generator_send_type_map[code_key] = send;
                    g_generator_element_types[code_location] = element;
                    g_generator_send_type_map[code_location] = send;
                }
            }
            if (sig_entry) {
                sig_entry->result = element;
                sig_entry->generator_send = send;
            }
        }
        return_types.pop_back();
        return ok;
    }

    bool check_struct_decl(struct struct_node *decl, Env &env) {
        const TypeNode *self_type =
            type_named(decl->source, decl->name);
        StaticTypeInfo &info = static_types[
            static_type_key(decl->source, decl->name)];
        info.is_enum = false;
        info.generic_params.clear();
        info.generic_defaults.clear();
        std::vector<GenericConstraintMeta> struct_constraints;
        std::vector<const TypeNode *> struct_defaults;
        if (!collect_generic_param_meta(
                decl->generic_params, info.generic_params,
                struct_constraints, struct_defaults, decl->line,
                decl->column))
            return false;
        register_generic_defaults(decl->source, decl->name,
                                  info.generic_params, struct_defaults);
        for (struct struct_member *member = decl->members; member;
             member = member->next) {
            if (member->name && member->type && !member->func) {
                const TypeNode *field_type =
                    type_from_constraint(member->type);
                if (!check_type_contracts(field_type, member->line,
                                          member->column))
                    return false;
                info.fields[member->name] = field_type;
            }
            if (member->name && member->func &&
                member->func->nodetype == 'Z')
                info.methods[member->name] =
                    (struct funcdef *)member->func;
        }
        GenericTypeMeta &meta =
            g_generic_type_meta[static_type_key(decl->source, decl->name)];
        info.generic_defaults = struct_defaults;
        meta.params = info.generic_params;
        meta.constraints = struct_constraints;
        meta.defaults = std::move(struct_defaults);
        meta.fields = info.fields;
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
            static_type_key(decl->source, decl->name)];
        info.is_enum = true;
        info.generic_params.clear();
        info.generic_defaults.clear();
        std::vector<GenericConstraintMeta> enum_constraints;
        std::vector<const TypeNode *> enum_defaults;
        if (!collect_generic_param_meta(
                decl->generic_params, info.generic_params,
                enum_constraints, enum_defaults, decl->line,
                decl->column))
            return false;
        register_generic_defaults(decl->source, decl->name,
                                  info.generic_params, enum_defaults);
        for (struct enum_variant *variant = decl->variants; variant;
             variant = variant->next) {
            auto &fields = info.variants[variant->name
                                             ? variant->name
                                             : ""];
            for (struct struct_member *field = variant->fields; field;
                 field = field->next) {
                if (field->name && field->type) {
                    const TypeNode *field_type =
                        type_from_constraint(field->type);
                    if (!check_type_contracts(field_type, field->line,
                                              field->column))
                        return false;
                    fields[field->name] = field_type;
                }
            }
        }
        GenericTypeMeta &meta =
            g_generic_type_meta[static_type_key(decl->source, decl->name)];
        info.generic_defaults = enum_defaults;
        meta.params = info.generic_params;
        meta.constraints = enum_constraints;
        meta.defaults = std::move(enum_defaults);
        meta.variants = info.variants;
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
            type_named(impl->source, impl->type_name);
        StaticTypeInfo &info = static_types[
            static_type_key(impl->source, impl->type_name)];
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
                if (value_part)
                    return iterable->args[1];
                return type_container(
                    TYPE_PAIR, {iterable->args[0], iterable->args[1]});
            case TYPE_STRING:
                return type_builtin(TYPE_CHAR);
            case TYPE_BYTES:
                return type_builtin(TYPE_BYTE);
            case TYPE_GENERATOR:
                return iterable->args.empty() ? type_any()
                                              : iterable->args[0];
            default:
                return type_any();
        }
    }

    static char *duplicate_text(const char *text) {
        const char *source = text ? text : "";
        std::size_t size = std::strlen(source) + 1;
        char *copy = (char *)std::malloc(size);
        if (!copy)
            return NULL;
        std::memcpy(copy, source, size);
        return copy;
    }

    void rewrite_pair_forin(struct forinloop *f) {
        static thread_local int pair_id = 0;
        char hidden[32];
        std::snprintf(hidden, sizeof(hidden), "#forpair%d", ++pair_id);

        struct ast *pattern = newpatterntuple(append_patternitem(
            newpatternname(duplicate_text(f->name)),
            newpatternname(duplicate_text(f->value_name))));
        struct ast *destructure = newdestructure(
            pattern, newref(duplicate_text(hidden)), false);
        destructure->line = f->line;
        destructure->column = f->column;
        f->body = newscope(newast('L', destructure, f->body));
        std::free(f->name);
        std::free(f->value_name);
        f->name = duplicate_text(hidden);
        f->value_name = NULL;
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
                    if (!check_type_contracts(declared, def->line,
                                              def->column))
                        return false;
                    if (def->v &&
                        !check_expr(def->v, declared, env, false,
                                    needs_static_check(declared)))
                        return false;
                } else {
                    declared = def->v ? infer(def->v, env) : type_any();
                }
                const char *name = def->name ? def->name : "";
                invalidate_root_facts(env, name ? name : "");
                env.define(name, declared, fixed);
                if (def->exported) {
                    env.vars[name].exported = true;
                    if (!package.empty()) {
                        VarInfo info;
                        info.type = declared;
                        info.fixed = fixed;
                        info.exported = true;
                        g_exported_types[package][name] = info;
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
                return check_assignment(asgn->target, asgn->v, env, true,
                                        asgn->op);
            }
            case 'U': {
                struct incdec_node *inc = (struct incdec_node *)stmt;
                invalidate_lvalue_facts(inc->target, env);
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
                NarrowFacts positive;
                NarrowFacts negative;
                if (!check_condition(f->cond, env, positive, negative))
                    return false;
                Env then_env;
                then_env.parent = &env;
                apply_narrow_facts(then_env, env, positive);
                if (!check_stmt(f->tl, then_env))
                    return false;
                Env else_env;
                else_env.parent = &env;
                apply_narrow_facts(else_env, env, negative);
                return check_stmt(f->el, else_env);
            }
            case 'W': {
                struct flow *f = (struct flow *)stmt;
                NarrowFacts positive;
                NarrowFacts negative;
                if (!check_condition(f->cond, env, positive, negative))
                    return false;
                Env child;
                child.parent = &env;
                apply_narrow_facts(child, env, positive);
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
                if (f->value_name) {
                    if (iterable->kind == TYPE_DICT) {
                        const TypeNode *key =
                            iterable->args.empty() ? type_any()
                                                   : iterable->args[0];
                        const TypeNode *value =
                            iterable->args.size() > 1 ? iterable->args[1]
                                                      : type_any();
                        child.define(f->name ? f->name : "", key, fixed);
                        child.define(f->value_name, value, fixed);
                        return check_stmt(f->body, child);
                    }
                    if (iterable->kind == TYPE_ANY) {
                        child.define(f->name ? f->name : "",
                                     type_any(), fixed);
                        child.define(f->value_name, type_any(), fixed);
                        return check_stmt(f->body, child);
                    }
                    const TypeNode *element =
                        iterable_element(iterable, false);
                    if (!element ||
                        (element->kind != TYPE_PAIR &&
                         element->kind != TYPE_TUPLE)) {
                        std::string message =
                            std::string("two-variable for-in over ") +
                            type_display(iterable) +
                            " requires pair/tuple elements";
                        report_error(f->line, f->column, message.c_str());
                        return false;
                    }
                    if (element->args.size() != 2) {
                        std::string message =
                            std::string("two-variable for-in requires a "
                                        "pair or 2-tuple, got ") +
                            type_display(element);
                        report_error(f->line, f->column, message.c_str());
                        return false;
                    }
                    child.define(f->name ? f->name : "",
                                 element->args[0], fixed);
                    child.define(f->value_name, element->args[1], fixed);
                    bool ok = check_stmt(f->body, child);
                    if (ok)
                        rewrite_pair_forin(f);
                    return ok;
                }
                child.define(f->name ? f->name : "",
                             iterable_element(iterable, false), fixed);
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
                std::string subject_path;
                bool has_subject_path =
                    narrow_path(match->expr, subject_path);
                bool simple_subject =
                    has_subject_path &&
                    subject_path.find('.') == std::string::npos;
                const VarInfo *subject_info =
                    simple_subject ? env.lookup(subject_path) : NULL;
                if (simple_subject &&
                    (!subject_info || !subject_info->fixed)) {
                    has_subject_path = false;
                    subject_info = NULL;
                }
                const TypeNode *subject_original =
                    subject_info ? subject_info->type : subject;
                std::vector<const TypeNode *> excluded;
                for (struct match_arm *arm =
                         (struct match_arm *)match->arms;
                     arm; arm = (struct match_arm *)arm->next) {
                    Env child;
                    child.parent = &env;
                    const struct pattern_node *type_pattern = NULL;
                    if (arm->pattern) {
                        const struct ast *inner = arm->pattern;
                        while (inner &&
                               ((const struct pattern_node *)inner)
                                       ->nodetype == NODE_PATTERN_AS)
                            inner = ((const struct pattern_node *)inner)
                                        ->items;
                        if (inner &&
                            ((const struct pattern_node *)inner)
                                    ->nodetype == NODE_PATTERN_TYPE) {
                            type_pattern =
                                (const struct pattern_node *)inner;
                        }
                    }
                    if (has_subject_path) {
                        const TypeNode *narrowed = subject_original;
                        for (const TypeNode *pattern : excluded)
                            narrowed =
                                narrow_exclude(narrowed, pattern);
                        if (type_pattern && type_pattern->type) {
                            const TypeNode *pattern =
                                type_from_constraint(type_pattern->type);
                            if (pattern->kind != TYPE_INVALID)
                                narrowed =
                                    narrow_include(narrowed, pattern);
                        }
                        if (narrowed != subject_original) {
                            if (simple_subject) {
                                child.define(subject_path, narrowed, true);
                                child.vars[subject_path].declared =
                                    subject_info->declared
                                        ? subject_info->declared
                                        : subject_info->type;
                            } else {
                                VarInfo info;
                                info.type = narrowed;
                                info.declared = subject_original;
                                info.fixed = true;
                                child.narrowed_paths[subject_path] = info;
                            }
                        }
                    }
                    if (arm->pattern)
                        bind_pattern_types(arm->pattern, subject, child,
                                           fixed);
                    if (!check_expr(arm->guard, type_any(), child, false,
                                    false) ||
                        !check_stmt(arm->body, child))
                        return false;
                    if (has_subject_path && type_pattern &&
                        type_pattern->type && !arm->guard) {
                        const TypeNode *pattern =
                            type_from_constraint(type_pattern->type);
                        if (pattern->kind != TYPE_INVALID)
                            excluded.push_back(pattern);
                    }
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
            case 't': {
                /*
                 * Protocol type parameters are stored with the runtime
                 * protocol definition and used for structural method
                 * matching.  Method-level type parameters are not allowed.
                 */
                return true;
            }
            case 'J': {
                struct import_node *imp = (struct import_node *)stmt;
                std::string path = imp->path ? imp->path : "";
                if (!path.empty() && path[0] == '\x02')
                    path.erase(0, 1);
                if (imp->selections) {
                    for (const struct import_selection *sel = imp->selections;
                         sel; sel = sel->next) {
                        const char *binding =
                            sel->alias ? sel->alias : sel->name;
                        if (!binding || !*binding)
                            continue;
                        const TypeNode *type = type_any();
                        bool fixed = false;
                        const std::string &selected =
                            sel->name ? sel->name : "";
                        auto pkg_it = g_exported_types.find(path);
                        if (pkg_it != g_exported_types.end()) {
                            auto type_it = pkg_it->second.find(selected);
                            if (type_it != pkg_it->second.end()) {
                                type = type_it->second.type;
                                fixed = type_it->second.fixed;
                            }
                        }
                        env.define(binding, type, fixed);
                        g_selective_imports[source][binding] = path;
                        g_selective_import_names[source][binding] = selected;
                    }
                    return true;
                }
                std::string alias =
                    imp->alias ? imp->alias : last_path_component(path);
                if (!alias.empty())
                    g_import_aliases[source][alias] = path;
                return true;
            }
            case 'o': {
                struct on_error_node *handler =
                    (struct on_error_node *)stmt;
                warn_launch_on_error(stmt);
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
            case NODE_FUNCTION_BODY:
                return check_stmt(stmt->l, env);
            case NODE_GOTO:
                return true;
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
            case NODE_TUPLE: {
                std::vector<const TypeNode *> args;
                for (struct list_node *item = (struct list_node *)expr;
                     item; item = (struct list_node *)item->next)
                    args.push_back(infer(item->value, env));
                if (args.size() == 2)
                    return type_container(TYPE_PAIR, args);
                if (args.size() > 2)
                    return type_container(TYPE_TUPLE, args);
                return type_any();
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
                struct index_node *index = (struct index_node *)expr;
                if (index->generic_function_value) {
                    std::string name;
                    if (!flatten_path(index->l, name) ||
                        name.find('.') != std::string::npos) {
                        report_error(expr->line, expr->column,
                                     "explicit generic instantiation "
                                     "requires a function name");
                        return type_any();
                    }
                    auto generic = g_generic_function_params.find(name);
                    if (generic == g_generic_function_params.end()) {
                        auto local = generic_functions.find(name);
                        if (local != generic_functions.end())
                            generic = g_generic_function_params
                                          .emplace(name, local->second)
                                          .first;
                    }
                    const TypeNode *callee = infer(index->l, env);
                    if (generic == g_generic_function_params.end() ||
                        !type_is_function_signature(callee)) {
                        report_error(expr->line, expr->column,
                                     "explicit type arguments require a "
                                     "generic function");
                        return type_any();
                    }
                    std::string text = diag_source_text(
                        index->span.start_line,
                        index->span.start_column + 1,
                        index->span.end_line, index->span.end_column);
                    std::vector<struct type_constraint *> constraints;
                    std::string parse_error;
                    if (!parse_type_list_text(text, source.c_str(),
                                              constraints, parse_error,
                                              true)) {
                        report_error(
                            expr->line, expr->column,
                            parse_error.empty()
                                ? "invalid explicit type arguments"
                                : parse_error.c_str());
                        for (struct type_constraint *constraint :
                             constraints)
                            free_type_constraint(constraint);
                        return type_any();
                    }
                    if (constraints.size() !=
                        generic->second.params.size()) {
                        std::string message =
                            "function " + name + " expects " +
                            std::to_string(generic->second.params.size()) +
                            " type argument(s), got " +
                            std::to_string(constraints.size());
                        report_error(expr->line, expr->column,
                                     message.c_str());
                        for (struct type_constraint *constraint :
                             constraints)
                            free_type_constraint(constraint);
                        return type_any();
                    }
                    TypeBindings bindings;
                    for (std::size_t i = 0; i < constraints.size(); ++i) {
                        const TypeNode *arg =
                            type_from_constraint(constraints[i]);
                        bindings[generic->second.params[i]] = arg;
                        free_type_constraint(constraints[i]);
                        if (!check_type_argument_is_type(
                                arg, expr->line, expr->column))
                            return type_any();
                        if (i < generic->second.constraints.size() &&
                            !type_satisfies_generic_constraint(
                                arg, generic->second.constraints[i])) {
                            std::string message =
                                std::string("type argument ") +
                                type_display(arg) +
                                " does not satisfy generic constraint "
                                "for " + name;
                            report_error(expr->line, expr->column,
                                         message.c_str());
                            return type_any();
                        }
                    }
                    return instantiate_type(callee, bindings);
                }
                const TypeNode *base = infer(expr->l, env);
                infer(expr->r, env);
                if (base->kind == TYPE_PAIR ||
                    base->kind == TYPE_TUPLE) {
                    const struct ast *index_node = expr->r;
                    if (index_node && index_node->nodetype == 'V') {
                        const struct value_t *value =
                            ((const struct val *)index_node)->val;
                        if (value && value->type == T_int) {
                            long long index =
                                ((const struct int_value_t *)value)->value;
                            if (index >= 0 &&
                                static_cast<std::size_t>(index) <
                                    base->args.size())
                                return base->args[(std::size_t)index];
                            return type_any();
                        }
                    }
                    const TypeNode *joined = type_element(base, 0);
                    for (std::size_t i = 1; i < base->args.size(); ++i)
                        joined = join_types(joined, base->args[i]);
                    return joined;
                }
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
            case NODE_REF_TARGET: {
                const TypeNode *base = infer(expr->l, env);
                if (base && base->kind == TYPE_REF && !base->args.empty())
                    return base->args[0];
                return type_any();
            }
            case NODE_OPTIONAL_CHAIN: {
                struct optional_chain_node *chain =
                    (struct optional_chain_node *)expr;
                const TypeNode *base =
                    unwrap_ref_type(infer(chain->l, env));
                const TypeNode *field =
                    field_type(base, (const struct ast *)expr, env);
                const struct ast *right = chain->r;
                if (field->kind == TYPE_ANY && right &&
                    right->nodetype == 'V') {
                    const struct value_t *value =
                        ((const struct val *)right)->val;
                    if (value && value->type == T_string) {
                        const TypeNode *method = method_value_type(
                            base,
                            ((const struct string_value_t *)value)->value);
                        if (method)
                            field = method;
                    }
                }
                return type_nullable(field);
            }
            case 'j': {
                const TypeNode *base = infer(expr->l, env);
                std::string path;
                bool has_path = flatten_path(expr, path);
                if (has_path) {
                    if (const VarInfo *fact = env.lookup_path(path))
                        return fact->type ? fact->type : type_any();
                }
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
                        const char *field_name =
                            ((const struct string_value_t *)value)->value;
                        /*
                         * `Type.new` is a constructor: the receiver is a type
                         * name, not a value, so the generic member paths above
                         * cannot see it.
                         */
                        if (base->kind == TYPE_ANY && expr->l &&
                            expr->l->nodetype == 'N' &&
                            std::strcmp(field_name, "new") == 0) {
                            const char *base_name =
                                ((const struct symref *)expr->l)->name;
                            const TypeNode *named =
                                base_name && env.owner_of(base_name) == nullptr
                                    ? type_named(source.c_str(), base_name)
                                    : nullptr;
                            structtype_lookup_result lookup;
                            if (named && lookup_type_node(named, lookup) &&
                                lookup.kind == STRUCTTYPE_LOOKUP_STRUCT) {
                                std::shared_ptr<function_def> constructor =
                                    structtype_constructor(lookup.structure);
                                if (constructor && constructor->code)
                                    return function_type_from_code(
                                        constructor->code.get());
                            }
                        }
                        const TypeNode *method =
                            method_value_type(base, field_name);
                        if (method)
                            return method;
                    }
                }
                return field;
            }
            case 'C': {
                const TypeNode *result = type_any();
                check_call(expr, type_any(), env, result);
                if (((struct ufncall *)expr)->launch && result) {
                    if (result->kind == TYPE_TASK) {
                        char message[512];
                        std::snprintf(
                            message, sizeof(message),
                            "launch cannot start a call that already returns "
                            "%s; call it directly and use .wait()",
                            type_display(result));
                        report_error(expr->line, expr->column, message);
                        return type_any();
                    }
                    result = type_container(TYPE_TASK, {result});
                }
                invalidate_method_receiver(
                    ((struct ufncall *)expr)->callee, env);
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
                const TypeNode *named = type_named(lit->source, lit->name);
                auto inferred = generic_literal_args.find(expr);
                if (inferred != generic_literal_args.end())
                    named = type_named_generic(lit->source, lit->name,
                                               inferred->second);
                if (lit->is_ref)
                    return type_container(TYPE_REF, {named});
                return named;
            }
            case '?': {
                struct enum_lit_node *lit =
                    (struct enum_lit_node *)expr;
                check_struct_literal(expr, env);
                const TypeNode *named =
                    type_named(lit->source, lit->name);
                auto inferred = generic_literal_args.find(expr);
                if (inferred != generic_literal_args.end())
                    named = type_named_generic(lit->source, lit->name,
                                               inferred->second);
                return named;
            }
            case '=': {
                struct symasgn *asgn = (struct symasgn *)expr;
                check_assignment(asgn->target, asgn->v, env, false);
                return infer(asgn->v, env);
            }
            case 'a': {
                struct compound_asgn *asgn =
                    (struct compound_asgn *)expr;
                check_assignment(asgn->target, asgn->v, env, true,
                                 asgn->op);
                return infer(asgn->target, env);
            }
            case 'U': {
                struct incdec_node *inc = (struct incdec_node *)expr;
                invalidate_lvalue_facts(inc->target, env);
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
            case 'w': {
                const TypeNode *value =
                    expr->l ? infer(expr->l, env)
                            : type_builtin(TYPE_NULL);
                if (!yield_types.empty()) {
                    if (expr->l && !generator_yield_expected.empty() &&
                        generator_yield_expected.back() &&
                        !type_is_any(generator_yield_expected.back())) {
                        check_expr(expr->l,
                                   generator_yield_expected.back(), env,
                                   false, true);
                    }
                    yield_types.back().push_back(value);
                }
                if (!generator_send_types.empty())
                    return generator_send_types.back();
                return type_any();
            }
            case 'n':
                if (expr->l)
                    infer(expr->l, env);
                return type_any();
            case 'X':
                check_stmt(((struct nscope *)expr)->ast, env);
                return type_any();
            case NODE_MATCH: {
                const struct match_node *match =
                    (const struct match_node *)expr;
                const TypeNode *subject = infer(match->expr, env);
                bool fixed = expr_fixed(match->expr, env);
                for (const struct match_arm *arm =
                         (const struct match_arm *)match->arms;
                     arm; arm = (const struct match_arm *)arm->next) {
                    if (!arm->pattern)
                        continue;
                    Env child;
                    child.parent = &env;
                    bind_pattern_types(arm->pattern, subject, child, fixed);
                }
                return type_any();
            }
            case 'o': {
                struct on_error_node *handler =
                    (struct on_error_node *)expr;
                warn_launch_on_error(expr);
                /*
                 * Check a protected do-block.  Plain expression handlers keep
                 * their existing lax treatment, but a statement block should
                 * still report type errors in its body.
                 */
                if (handler->call && handler->call->nodetype == 'X') {
                    check_expr(handler->call, type_any(), env, false,
                               false);
                    Env child;
                    child.parent = &env;
                    child.define(handler->name ? handler->name : "",
                                 type_any());
                    check_stmt(handler->body, child);
                }
                return type_any();
            }
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

namespace {
thread_local bool g_suppressed = false;
}

suppress_scope::suppress_scope(bool enabled)
    : active_(enabled), previous_(g_suppressed) {
    if (active_)
        g_suppressed = true;
}

suppress_scope::~suppress_scope() {
    if (active_)
        g_suppressed = previous_;
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
    if (g_suppressed || g_mode == Mode::Off || !statement)
        return true;
    Checker checker(diag_current_source_name(),
                    diag_current_package_name());
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

void forget_source(const std::string &source_name) {
    g_import_aliases.erase(source_name);
    g_selective_imports.erase(source_name);
    g_selective_import_names.erase(source_name);
    g_persistent_bindings.erase(source_name);
    g_exported_types.erase(source_name);
}

} // namespace typecheck
} // namespace skein
