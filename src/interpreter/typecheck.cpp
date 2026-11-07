#include "typecheck.h"

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
#include <vector>

namespace skein {
namespace typecheck {

namespace {

Mode g_mode = Mode::Warn;

std::unordered_map<std::string,
                   std::unordered_map<std::string, std::string>>
    g_import_aliases;

const TypeNode *value_static_type(const struct value_t *value) {
    if (!value)
        return type_any();
    switch (value->type) {
        case T_int: return type_builtin(TYPE_INT);
        case T_float: return type_builtin(TYPE_FLOAT);
        case T_bool: return type_builtin(TYPE_BOOL);
        case T_string: return type_builtin(TYPE_STRING);
        case T_char: return type_builtin(TYPE_CHAR);
        case T_byte: return type_builtin(TYPE_BYTE);
        case T_bytes: return type_builtin(TYPE_BYTES);
        case T_null: return type_builtin(TYPE_NULL);
        case T_list: return type_container(TYPE_LIST, {type_any()});
        case T_dict:
            return type_container(TYPE_DICT,
                                  {type_any(), type_any()});
        case T_set: return type_container(TYPE_SET, {type_any()});
        case T_deque: return type_container(TYPE_DEQUE, {type_any()});
        case T_heap: return type_container(TYPE_HEAP, {type_any()});
        case T_result:
            return type_container(TYPE_RESULT,
                                  {type_any(), type_any()});
        case T_error: return type_builtin(TYPE_ERROR);
        case T_func: return type_builtin(TYPE_FUNCTION);
        case T_builtin: return type_builtin(TYPE_BUILTIN);
        default: return type_any();
    }
}

const TypeNode *strip_nullable(const TypeNode *type) {
    if (!type || !type->nullable)
        return type;
    if (type_container_arity(type->kind) > 0)
        return type_container(type->kind, type->args, false);
    if (type->kind == TYPE_NAMED)
        return type_named(type->module.c_str(), type->name.c_str(), false);
    return type_builtin(type->kind, false);
}

bool has_concrete_container_arg(const TypeNode *type) {
    if (!type)
        return false;
    int arity = type_container_arity(type->kind);
    if (arity <= 0)
        return false;
    for (const TypeNode *arg : type->args) {
        if (!arg || arg->kind == TYPE_ANY)
            continue;
        if (type_container_arity(arg->kind) > 0) {
            if (has_concrete_container_arg(arg))
                return true;
            continue;
        }
        return true;
    }
    return false;
}

bool needs_static_check(const TypeNode *type) {
    return type && type->kind != TYPE_ANY &&
           has_concrete_container_arg(type);
}

const TypeNode *join_types(const TypeNode *a, const TypeNode *b) {
    if (!a)
        return b ? b : type_any();
    if (!b)
        return a;
    if (a == b)
        return a;
    if (a->kind == TYPE_ANY || b->kind == TYPE_ANY)
        return type_any();
    if ((a->kind == TYPE_INT || a->kind == TYPE_FLOAT ||
         a->kind == TYPE_NUMBER) &&
        (b->kind == TYPE_INT || b->kind == TYPE_FLOAT ||
         b->kind == TYPE_NUMBER))
        return type_builtin(TYPE_NUMBER);
    if (a->kind == TYPE_NAMED && b->kind == TYPE_NAMED &&
        a->name == b->name && a->module == b->module)
        return a;
    if (a->kind == b->kind && type_container_arity(a->kind) > 0 &&
        a->args.size() == b->args.size()) {
        std::vector<const TypeNode *> args;
        args.reserve(a->args.size());
        for (std::size_t i = 0; i < a->args.size(); ++i)
            args.push_back(join_types(a->args[i], b->args[i]));
        return type_container(a->kind, args, a->nullable || b->nullable);
    }
    return type_any();
}

bool flatten_path(const struct ast *node, std::string &out) {
    if (!node)
        return false;
    if (node->nodetype == 'N') {
        out = ((const struct symref *)node)->name
                  ? ((const struct symref *)node)->name
                  : "";
        return true;
    }
    if (node->nodetype == 'j') {
        const struct ast *right = node->r;
        if (!right || right->nodetype != 'V')
            return false;
        const struct value_t *value = ((const struct val *)right)->val;
        if (!value || value->type != T_string)
            return false;
        std::string base;
        if (!flatten_path(node->l, base))
            return false;
        out = base;
        out += '.';
        out += ((const struct string_value_t *)value)->value;
        return true;
    }
    return false;
}

void collect_call_args(struct ast *args, std::vector<struct ast *> &out) {
    if (!args)
        return;
    if (args->nodetype == 'L') {
        collect_call_args(args->l, out);
        collect_call_args(args->r, out);
        return;
    }
    out.push_back(args);
}

std::string last_path_component(const std::string &path) {
    std::size_t dot = path.rfind('.');
    if (dot == std::string::npos)
        return path;
    return path.substr(dot + 1);
}

std::string canonical_builtin_path(const std::string &path,
                                   const std::string &source) {
    if (path.empty())
        return path;
    std::size_t dot = path.find('.');
    std::string first =
        dot == std::string::npos ? path : path.substr(0, dot);
    std::string rest =
        dot == std::string::npos ? std::string() : path.substr(dot);

    if (first == "std")
        return path;

    auto source_it = g_import_aliases.find(source);
    if (source_it != g_import_aliases.end()) {
        auto alias_it = source_it->second.find(first);
        if (alias_it != source_it->second.end())
            return alias_it->second + rest;
    }

    if (first == "list" || first == "dict" || first == "set" ||
        first == "deque" || first == "heap")
        return "std." + path;
    return path;
}

struct FunctionSig {
    std::vector<const TypeNode *> params;
    bool variadic = false;
    const TypeNode *rest = nullptr;
};

FunctionSig signature_from_symlist(struct symlist *sl) {
    FunctionSig sig;
    for (struct symlist *item = sl; item; item = item->next) {
        const TypeNode *type = type_from_constraint(item->type);
        if (item->variadic) {
            sig.variadic = true;
            sig.rest = type;
        } else {
            sig.params.push_back(type);
        }
    }
    return sig;
}

struct Env {
    Env *parent = nullptr;
    std::unordered_map<std::string, const TypeNode *> vars;

    const TypeNode *lookup(const std::string &name) const {
        for (const Env *env = this; env; env = env->parent) {
            auto it = env->vars.find(name);
            if (it != env->vars.end())
                return it->second;
        }
        return nullptr;
    }

    void define(const std::string &name, const TypeNode *type) {
        vars[name] = type ? type : type_any();
    }
};

enum class MatchResult {
    Ok,
    Unknown,
    Mismatch,
};

MatchResult combine_result(MatchResult a, MatchResult b) {
    if (a == MatchResult::Mismatch || b == MatchResult::Mismatch)
        return MatchResult::Mismatch;
    if (a == MatchResult::Unknown || b == MatchResult::Unknown)
        return MatchResult::Unknown;
    return MatchResult::Ok;
}

MatchResult match_type(const TypeNode *actual, const TypeNode *expected) {
    if (!expected || expected->kind == TYPE_ANY)
        return MatchResult::Ok;
    if (!actual)
        actual = type_any();
    if (actual->kind == TYPE_ANY || actual->kind == TYPE_INVALID)
        return MatchResult::Unknown;

    if (expected->nullable) {
        if (actual->kind == TYPE_NULL)
            return MatchResult::Ok;
        const TypeNode *a = actual->nullable ? strip_nullable(actual)
                                             : actual;
        const TypeNode *e = strip_nullable(expected);
        return match_type(a, e);
    }
    if (actual->nullable)
        return MatchResult::Mismatch;
    if (actual->kind == TYPE_NULL)
        return MatchResult::Mismatch;

    if (expected->kind == TYPE_NUMBER) {
        if (actual->kind == TYPE_INT || actual->kind == TYPE_FLOAT ||
            actual->kind == TYPE_NUMBER)
            return MatchResult::Ok;
        return MatchResult::Mismatch;
    }
    if (actual->kind != expected->kind)
        return MatchResult::Mismatch;

    if (actual->kind == TYPE_NAMED) {
        if (actual->name == expected->name &&
            actual->module == expected->module)
            return MatchResult::Ok;
        return MatchResult::Mismatch;
    }

    int arity = type_container_arity(expected->kind);
    if (arity > 0) {
        if (expected->args.empty())
            return MatchResult::Ok;
        if (actual->args.empty()) {
            MatchResult result = MatchResult::Ok;
            for (const TypeNode *arg : expected->args) {
                if (arg && arg->kind != TYPE_ANY)
                    result = combine_result(result, MatchResult::Unknown);
            }
            return result;
        }
        if (actual->args.size() != expected->args.size())
            return MatchResult::Mismatch;
        MatchResult result = MatchResult::Ok;
        for (std::size_t i = 0; i < expected->args.size(); ++i) {
            result = combine_result(
                result, match_type(actual->args[i], expected->args[i]));
            if (result == MatchResult::Mismatch)
                return result;
        }
        return result;
    }

    return MatchResult::Ok;
}

struct Checker {
    std::string source;
    std::string module;
    std::unordered_map<std::string, FunctionSig> functions;
    bool error_ = false;

    explicit Checker(const std::string &source_name,
                     const std::string &module_name)
        : source(source_name), module(module_name) {}

    const TypeNode *lookup_name(const std::string &name, Env &env) {
        if (const TypeNode *type = env.lookup(name))
            return type;
        struct symbol *sym = lookupsym(name.c_str());
        if (!sym)
            return type_any();
        return type_from_constraint(&sym->type);
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
        char message[512];
        if (result == MatchResult::Mismatch) {
            std::snprintf(message, sizeof(message),
                          "expected %s, got %s", expected_text.c_str(),
                          actual_text.c_str());
            diag_report(line, column, "type error", message);
            error_ = true;
            return false;
        }

        if (explicit_cast || g_mode == Mode::Off || g_mode == Mode::Loose)
            return true;

        std::snprintf(message, sizeof(message),
                      "implicit %s assigned to %s; use 'as %s' if intentional",
                      actual_text.c_str(), expected_text.c_str(),
                      expected_text.c_str());
        if (g_mode == Mode::Strict) {
            diag_report(line, column, "type error", message);
            error_ = true;
            return false;
        }
        diag_report_warning(line, column, "type warning", message);
        return true;
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
            diag_report(node->line, node->column, "type error",
                        "cannot assign through 'as'");
            error_ = true;
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
        struct_def *def = structtype_find_struct(base_type->module.c_str(),
                                                 base_type->name.c_str());
        if (!def)
            return type_any();
        auto it = def->field_index.find(field);
        if (it == def->field_index.end())
            return type_any();
        return type_from_constraint(&def->fields[(size_t)it->second].type);
    }

    bool check_call(struct ast *expr, const TypeNode *expected, Env &env,
                    const TypeNode *&result) {
        struct ufncall *call = (struct ufncall *)expr;
        result = type_any();
        std::vector<struct ast *> args;
        collect_call_args(call->args, args);

        std::string path;
        bool has_path = flatten_path(call->callee, path);
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
                        found = true;
                    }
                }
            }
            if (found) {
                bool has_spread = false;
                for (struct ast *arg : args) {
                    if (arg && arg->nodetype == 'x') {
                        has_spread = true;
                        break;
                    }
                }
                if (!has_spread) {
                    for (std::size_t i = 0; i < args.size(); ++i) {
                        const TypeNode *param =
                            i < sig.params.size()
                                ? sig.params[i]
                                : (sig.variadic ? sig.rest : type_any());
                        if (!check_expr(args[i], param, env, false, false))
                            return false;
                    }
                } else {
                    for (struct ast *arg : args)
                        infer(arg, env);
                }
                result = type_any();
                return true;
            }
        }

        if (has_path) {
            std::string canonical = canonical_builtin_path(path, source);
            const TypeNode *builtin_result = type_any();
            if (check_builtin(canonical, args, expected, env,
                              builtin_result)) {
                result = builtin_result;
                return true;
            }
        }

        if (call->callee)
            infer(call->callee, env);
        for (struct ast *arg : args)
            infer(arg, env);
        result = type_any();
        return true;
    }

    const TypeNode *container_type(const TypeNode *type,
                                   enum type_constraint_kind kind,
                                   std::size_t arity) {
        if (type && type->kind == kind)
            return type;
        std::vector<const TypeNode *> args(arity, type_any());
        return type_container(kind, args);
    }

    const TypeNode *expected_element(const TypeNode *expected,
                                     enum type_constraint_kind kind,
                                     std::size_t index) {
        if (expected && expected->kind == kind &&
            index < expected->args.size())
            return expected->args[index];
        return type_any();
    }

    bool expr_has_declared_container(struct ast *expr, Env &env) {
        if (!expr)
            return false;
        if (expr->nodetype == 'N') {
            const char *name = ((struct symref *)expr)->name;
            return needs_static_check(lookup_name(name ? name : "", env));
        }
        if (expr->nodetype == 'j') {
            const TypeNode *base = infer(expr->l, env);
            return needs_static_check(field_type(base, expr, env));
        }
        return false;
    }

    bool check_builtin(const std::string &path,
                       const std::vector<struct ast *> &args,
                       const TypeNode *expected, Env &env,
                       const TypeNode *&result) {
        /*
         * Only run builtin signature checks when a generic annotation is
         * involved.  Unannotated code keeps the pre-existing runtime
         * behaviour and error text.
         */
        if (!needs_static_check(expected) &&
            (args.empty() ||
             !expr_has_declared_container(args[0], env)))
            return false;
        auto arg_type = [&](std::size_t i) -> const TypeNode * {
            return i < args.size() ? infer(args[i], env) : type_any();
        };
        auto check_arg = [&](std::size_t i, const TypeNode *type,
                             bool force) -> bool {
            return i >= args.size() ||
                   check_expr(args[i], type, env, false, force);
        };
        auto require_kind = [&](std::size_t i,
                                enum type_constraint_kind kind) -> bool {
            if (i >= args.size())
                return true;
            const TypeNode *actual = infer(args[i], env);
            if (actual->kind == TYPE_ANY || actual->kind == kind)
                return true;
            const TypeNode *wanted = type_builtin(kind);
            char message[256];
            std::snprintf(message, sizeof(message), "expected %s, got %s",
                          type_display(wanted), type_display(actual));
            diag_report(args[i]->line, args[i]->column, "type error",
                        message);
            error_ = true;
            return false;
        };

        if (path == "std.set.new" || path == "std.deque.new" ||
            path == "std.heap.new") {
            enum type_constraint_kind kind =
                path == "std.set.new"
                    ? TYPE_SET
                    : (path == "std.deque.new" ? TYPE_DEQUE : TYPE_HEAP);
            if (expected && expected->kind == kind) {
                result = expected;
            } else {
                result = type_container(kind, {type_any()});
            }
            return true;
        }

        if (path.rfind("std.list.", 0) == 0) {
            std::string name = path.substr(9);
            if (!require_kind(0, TYPE_LIST))
                return true;
            const TypeNode *list_type = container_type(arg_type(0),
                                                       TYPE_LIST, 1);
            const TypeNode *element =
                expected_element(expected, TYPE_LIST, 0);
            if (element->kind == TYPE_ANY && !list_type->args.empty())
                element = list_type->args[0];
            const TypeNode *target_list =
                type_container(TYPE_LIST, {element});
            if (!check_arg(0, target_list, false))
                return true;

            if (name == "len") {
                result = type_builtin(TYPE_INT);
                return true;
            }
            if (name == "append") {
                if (!check_arg(1, element, true))
                    return true;
                result = target_list;
                return true;
            }
            if (name == "slice") {
                if (!check_arg(1, type_builtin(TYPE_INT), false) ||
                    !check_arg(2, type_builtin(TYPE_INT), false))
                    return true;
                result = target_list;
                return true;
            }
            if (name == "contains" || name == "index") {
                if (!check_arg(1, element, true))
                    return true;
                result = type_builtin(name == "contains" ? TYPE_BOOL
                                                          : TYPE_INT);
                return true;
            }
            if (name == "reverse" || name == "sort" ||
                name == "sort_desc" || name == "sort_by" ||
                name == "filter" || name == "unique" ||
                name == "unique_by" || name == "top_k") {
                if (name == "top_k" && !check_arg(1, type_builtin(TYPE_INT),
                                                  false))
                    return true;
                result = target_list;
                return true;
            }
            if (name == "is_sorted") {
                result = type_builtin(TYPE_BOOL);
                return true;
            }
            if (name == "map") {
                result = type_container(TYPE_LIST, {type_any()});
                return true;
            }
            if (name == "reduce") {
                result = type_any();
                return true;
            }
            if (name == "group_by") {
                result = type_container(
                    TYPE_DICT, {type_any(), target_list});
                return true;
            }
            if (name == "chunk" || name == "window") {
                if (!check_arg(1, type_builtin(TYPE_INT), false))
                    return true;
                result = type_container(
                    TYPE_LIST,
                    {type_container(TYPE_LIST, {element})});
                return true;
            }
            if (name == "zip") {
                result = type_container(
                    TYPE_LIST,
                    {type_container(TYPE_LIST, {type_any()})});
                return true;
            }
            if (name == "binary_search") {
                if (!check_arg(1, element, true))
                    return true;
                result = type_container(
                    TYPE_DICT, {type_builtin(TYPE_STRING), type_any()});
                return true;
            }
            result = type_any();
            return true;
        }

        if (path.rfind("std.dict.", 0) == 0) {
            std::string name = path.substr(9);
            if (!require_kind(0, TYPE_DICT))
                return true;
            const TypeNode *dict_type = container_type(arg_type(0),
                                                       TYPE_DICT, 2);
            const TypeNode *key = expected_element(expected, TYPE_DICT, 0);
            const TypeNode *value = expected_element(expected, TYPE_DICT, 1);
            if (key->kind == TYPE_ANY && dict_type->args.size() > 0)
                key = dict_type->args[0];
            if (value->kind == TYPE_ANY && dict_type->args.size() > 1)
                value = dict_type->args[1];
            const TypeNode *target_dict =
                type_container(TYPE_DICT, {key, value});
            if (!check_arg(0, target_dict, false))
                return true;

            if (name == "len") {
                result = type_builtin(TYPE_INT);
                return true;
            }
            if (name == "keys") {
                result = type_container(TYPE_LIST, {key});
                return true;
            }
            if (name == "values") {
                result = type_container(TYPE_LIST, {value});
                return true;
            }
            if (name == "items") {
                result = type_container(
                    TYPE_LIST,
                    {type_container(TYPE_LIST, {type_any()})});
                return true;
            }
            if (name == "get") {
                if (!check_arg(1, key, true) ||
                    !check_arg(2, value, true))
                    return true;
                result = type_nullable(value);
                return true;
            }
            if (name == "has") {
                if (!check_arg(1, key, true))
                    return true;
                result = type_builtin(TYPE_BOOL);
                return true;
            }
            if (name == "delete") {
                if (!check_arg(1, key, true))
                    return true;
                result = target_dict;
                return true;
            }
            if (name == "merge") {
                if (!require_kind(1, TYPE_DICT) ||
                    !check_arg(1, target_dict, false))
                    return true;
                result = target_dict;
                return true;
            }
            result = type_any();
            return true;
        }

        if (path.rfind("std.set.", 0) == 0) {
            std::string name = path.substr(8);
            if (name == "from_list") {
                if (!require_kind(0, TYPE_LIST))
                    return true;
                const TypeNode *element =
                    expected_element(expected, TYPE_SET, 0);
                const TypeNode *list_type = container_type(arg_type(0),
                                                           TYPE_LIST, 1);
                if (element->kind == TYPE_ANY && list_type->args.size() > 0)
                    element = list_type->args[0];
                if (!check_arg(0,
                               type_container(TYPE_LIST, {element}), false))
                    return true;
                result = type_container(TYPE_SET, {element});
                return true;
            }
            if (!require_kind(0, TYPE_SET))
                return true;
            const TypeNode *set_type = container_type(arg_type(0), TYPE_SET,
                                                      1);
            const TypeNode *element =
                expected_element(expected, TYPE_SET, 0);
            if (element->kind == TYPE_ANY && set_type->args.size() > 0)
                element = set_type->args[0];
            const TypeNode *target_set =
                type_container(TYPE_SET, {element});
            if (!check_arg(0, target_set, false))
                return true;
            if (name == "add" || name == "remove") {
                if (!check_arg(1, element, true))
                    return true;
                result = target_set;
                return true;
            }
            if (name == "has") {
                if (!check_arg(1, element, true))
                    return true;
                result = type_builtin(TYPE_BOOL);
                return true;
            }
            if (name == "values") {
                result = type_container(TYPE_LIST, {element});
                return true;
            }
            if (name == "size") {
                result = type_builtin(TYPE_INT);
                return true;
            }
            if (name == "union" || name == "intersect" ||
                name == "difference") {
                if (!require_kind(1, TYPE_SET) ||
                    !check_arg(1, target_set, false))
                    return true;
                result = target_set;
                return true;
            }
            if (name == "is_subset") {
                if (!require_kind(1, TYPE_SET) ||
                    !check_arg(1, target_set, false))
                    return true;
                result = type_builtin(TYPE_BOOL);
                return true;
            }
            result = type_any();
            return true;
        }

        if (path.rfind("std.deque.", 0) == 0) {
            std::string name = path.substr(10);
            if (name == "from_list") {
                if (!require_kind(0, TYPE_LIST))
                    return true;
                const TypeNode *element =
                    expected_element(expected, TYPE_DEQUE, 0);
                const TypeNode *list_type = container_type(arg_type(0),
                                                           TYPE_LIST, 1);
                if (element->kind == TYPE_ANY && list_type->args.size() > 0)
                    element = list_type->args[0];
                if (!check_arg(0,
                               type_container(TYPE_LIST, {element}), false))
                    return true;
                result = type_container(TYPE_DEQUE, {element});
                return true;
            }
            if (!require_kind(0, TYPE_DEQUE))
                return true;
            const TypeNode *deque_type =
                container_type(arg_type(0), TYPE_DEQUE, 1);
            const TypeNode *element =
                expected_element(expected, TYPE_DEQUE, 0);
            if (element->kind == TYPE_ANY && deque_type->args.size() > 0)
                element = deque_type->args[0];
            const TypeNode *target_deque =
                type_container(TYPE_DEQUE, {element});
            if (!check_arg(0, target_deque, false))
                return true;
            if (name == "push_front" || name == "push_back") {
                if (!check_arg(1, element, true))
                    return true;
                result = target_deque;
                return true;
            }
            if (name == "to_list") {
                result = type_container(TYPE_LIST, {element});
                return true;
            }
            if (name == "size") {
                result = type_builtin(TYPE_INT);
                return true;
            }
            if (name == "peek_front" || name == "peek_back") {
                result = type_nullable(element);
                return true;
            }
            result = type_any();
            return true;
        }

        if (path.rfind("std.heap.", 0) == 0) {
            std::string name = path.substr(9);
            if (name == "from_list") {
                if (!require_kind(0, TYPE_LIST))
                    return true;
                const TypeNode *element =
                    expected_element(expected, TYPE_HEAP, 0);
                const TypeNode *list_type = container_type(arg_type(0),
                                                           TYPE_LIST, 1);
                if (element->kind == TYPE_ANY && list_type->args.size() > 0)
                    element = list_type->args[0];
                if (!check_arg(0,
                               type_container(TYPE_LIST, {element}), false))
                    return true;
                result = type_container(TYPE_HEAP, {element});
                return true;
            }
            if (!require_kind(0, TYPE_HEAP))
                return true;
            const TypeNode *heap_type = container_type(arg_type(0),
                                                       TYPE_HEAP, 1);
            const TypeNode *element =
                expected_element(expected, TYPE_HEAP, 0);
            if (element->kind == TYPE_ANY && heap_type->args.size() > 0)
                element = heap_type->args[0];
            const TypeNode *target_heap =
                type_container(TYPE_HEAP, {element});
            if (!check_arg(0, target_heap, false))
                return true;
            if (name == "push") {
                if (!check_arg(1, element, true))
                    return true;
                result = target_heap;
                return true;
            }
            if (name == "to_list") {
                result = type_container(TYPE_LIST, {element});
                return true;
            }
            if (name == "size") {
                result = type_builtin(TYPE_INT);
                return true;
            }
            if (name == "peek") {
                result = type_nullable(element);
                return true;
            }
            result = type_any();
            return true;
        }

        return false;
    }

    bool check_struct_literal(struct ast *expr, Env &env) {
        const char *name = nullptr;
        const char *module = nullptr;
        struct struct_init *fields = nullptr;
        struct_def *def = nullptr;
        enum_def *edef = nullptr;
        struct variant_def *variant = nullptr;

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
            edef = structtype_find_enum(module, name);
            if (edef && lit->variant) {
                auto it = edef->variant_index.find(lit->variant);
                if (it != edef->variant_index.end())
                    variant = &edef->variants[(size_t)it->second];
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
            if (!field_type) {
                if (init->value)
                    infer(init->value, env);
                continue;
            }
            const TypeNode *expected =
                type_from_constraint(field_type);
            if (!check_expr(init->value, expected, env, false,
                            needs_static_check(expected)))
                return false;
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
            if (self_type && std::string(name) == "self")
                type = self_type;
            local.define(name, type);
        }

        return check_stmt(def->ast, local);
    }

    bool check_struct_decl(struct struct_node *decl, Env &env) {
        const TypeNode *self_type =
            type_named(decl->module, decl->name);
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
        for (struct struct_member *member = impl->methods; member;
             member = member->next) {
            if (member->func && member->func->nodetype == 'Z') {
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
                const TypeNode *declared =
                    def->type ? type_from_constraint(def->type)
                              : type_any();
                if (def->v && def->type) {
                    if (!check_expr(def->v, declared, env, false,
                                    needs_static_check(declared)))
                        return false;
                } else if (def->v) {
                    infer(def->v, env);
                }
                env.define(def->name ? def->name : "", declared);
                return true;
            }
            case NODE_DESTRUCTURE: {
                struct destructure_node *d =
                    (struct destructure_node *)stmt;
                const TypeNode *value_type = infer(d->value, env);
                (void)value_type;
                std::vector<std::string> names;
                skein::ast_analysis::collect_pattern_bindings(d->pattern,
                                                              names);
                for (const std::string &name : names)
                    env.define(name, type_any());
                return true;
            }
            case '=': {
                struct symasgn *asgn = (struct symasgn *)stmt;
                if (!check_lvalue(asgn->target, asgn->v, env, false))
                    return false;
                return true;
            }
            case 'a': {
                struct compound_asgn *asgn =
                    (struct compound_asgn *)stmt;
                if (!check_lvalue(asgn->target, asgn->v, env, true))
                    return false;
                return true;
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
                const TypeNode *iterable = infer(f->iterable, env);
                Env child;
                child.parent = &env;
                child.define(f->name ? f->name : "",
                             iterable_element(iterable, false));
                if (f->value_name)
                    child.define(f->value_name,
                                 iterable_element(iterable, true));
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
                if (!check_expr(match->expr, type_any(), env, false, false))
                    return false;
                for (struct match_arm *arm =
                         (struct match_arm *)match->arms;
                     arm; arm = (struct match_arm *)arm->next) {
                    Env child;
                    child.parent = &env;
                    std::vector<std::string> names;
                    if (arm->pattern) {
                        skein::ast_analysis::collect_pattern_bindings(
                            arm->pattern, names);
                        for (const std::string &name : names)
                            child.define(name, type_any());
                    }
                    if (!check_expr(arm->guard, type_any(), child, false,
                                    false) ||
                        !check_stmt(arm->body, child))
                        return false;
                }
                return true;
            }
            case 'R': {
                struct fnret *ret = (struct fnret *)stmt;
                if (ret->v)
                    infer(ret->v, env);
                return true;
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
                if (path.rfind("std", 0) == 0 &&
                    (path.size() == 3 || path[3] == '.')) {
                    std::string alias =
                        imp->alias ? imp->alias
                                   : last_path_component(path);
                    if (!alias.empty())
                        g_import_aliases[source][alias] = path;
                }
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
            case 'V':
                return value_static_type(((struct val *)expr)->val);
            case 'N': {
                const char *name = ((struct symref *)expr)->name;
                return lookup_name(name ? name : "", env);
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
                return field_type(base, expr, env);
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
            case 'Z':
                check_funcdef((struct funcdef *)expr, env);
                return type_builtin(TYPE_FUNCTION);
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
                check_lvalue(asgn->target, asgn->v, env, false);
                return infer(asgn->v, env);
            }
            case 'a': {
                struct compound_asgn *asgn =
                    (struct compound_asgn *)expr;
                check_lvalue(asgn->target, asgn->v, env, true);
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
                     right->kind == TYPE_NUMBER))
                    return type_builtin(TYPE_NUMBER);
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
                if (expr->r)
                    infer(expr->r, env);
                if (left->kind == TYPE_FLOAT)
                    return type_builtin(TYPE_FLOAT);
                if (left->kind == TYPE_NUMBER)
                    return type_builtin(TYPE_NUMBER);
                if (left->kind == TYPE_INT)
                    return type_builtin(TYPE_INT);
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
            case 'b':
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
    bool ok = checker.check_stmt(statement, env);
    return ok && !checker.error_;
}

} // namespace typecheck
} // namespace skein
