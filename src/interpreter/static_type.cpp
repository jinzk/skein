#include "static_type.h"

#include "diagnostics.h"
#include "runtime_state.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <unordered_map>

namespace skein {
namespace typecheck {

namespace {

const char *builtin_kind_name(enum type_constraint_kind kind) {
    switch (kind) {
        case TYPE_ANY: return "any";
        case TYPE_INT: return "int";
        case TYPE_FLOAT: return "float";
        case TYPE_NUMBER: return "number";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_CHAR: return "char";
        case TYPE_BYTE: return "byte";
        case TYPE_BYTES: return "bytes";
        case TYPE_LIST: return "list";
        case TYPE_DICT: return "dict";
        case TYPE_DEQUE: return "deque";
        case TYPE_HEAP: return "heap";
        case TYPE_SET: return "set";
        case TYPE_FUNCTION: return "function";
        case TYPE_BUILTIN: return "builtin";
        case TYPE_CALLABLE: return "callable";
        case TYPE_NULL: return "null";
        case TYPE_USERDATA: return "userdata";
        case TYPE_RESULT: return "result";
        case TYPE_ERROR: return "error";
        case TYPE_NAMED: return "invalid";
        case TYPE_VAR: return "T";
        case TYPE_TASK: return "task";
        case TYPE_GENERATOR: return "generator";
        case TYPE_SOCKET: return "socket";
        case TYPE_LISTENER: return "listener";
        case TYPE_WEBSOCKET: return "websocket";
        case TYPE_STREAM: return "stream";
        case TYPE_PAIR: return "pair";
        case TYPE_UNION: return "union";
        case TYPE_INVALID:
        default: return "invalid";
    }
}

std::unordered_map<std::string, std::unique_ptr<TypeNode>> &type_pool() {
    static std::unordered_map<std::string, std::unique_ptr<TypeNode>> pool;
    return pool;
}

std::string node_key(enum type_constraint_kind kind, const std::string &name,
                     const std::string &module, bool nullable,
                     const std::vector<const TypeNode *> &args,
                     int optional_params, bool variadic,
                     const TypeNode *rest) {
    std::string key;
    key.reserve(32);
    key += std::to_string(static_cast<int>(kind));
    key += '|';
    key += module;
    key += '|';
    key += name;
    key += nullable ? "|1" : "|0";
    key += '|';
    key += std::to_string(optional_params);
    key += variadic ? "|v1" : "|v0";
    key += '|';
    key += rest ? node_key(rest->kind, rest->name, rest->module,
                           rest->nullable, rest->args,
                           rest->optional_params, rest->variadic,
                           rest->rest)
                : std::string("-");
    for (const TypeNode *arg : args) {
        key += '|';
        key += arg ? node_key(arg->kind, arg->name, arg->module,
                              arg->nullable, arg->args,
                              arg->optional_params, arg->variadic,
                              arg->rest)
                   : std::string("any");
    }
    return key;
}

std::string function_arg_display(const TypeNode *type) {
    if (!type)
        return "any";
    if (type_is_function_signature(type))
        return "(" + type->display + ")";
    return type->display;
}

std::string node_display(enum type_constraint_kind kind,
                         const std::string &name,
                         const std::vector<const TypeNode *> &args,
                         bool nullable, bool variadic,
                         const TypeNode *rest) {
    std::string text;
    if (kind == TYPE_VAR) {
        text = name.empty() ? "T" : name;
    } else if (kind == TYPE_NAMED) {
        text = name.empty() ? "invalid" : name;
    } else if ((kind == TYPE_CALLABLE || kind == TYPE_FUNCTION ||
                kind == TYPE_BUILTIN) && !args.empty()) {
        if (!variadic && args.size() == 2 && args[0] &&
            type_is_function_signature(args[0])) {
            text = function_arg_display(args[0]);
        } else {
            text = "(";
            std::size_t fixed = args.size() - 1;
            for (std::size_t i = 0; i < fixed; ++i) {
                if (i != 0) text += ", ";
                text += function_arg_display(args[i]);
            }
            if (variadic) {
                if (fixed != 0) text += ", ";
                text += "...";
                if (rest)
                    text += function_arg_display(rest);
            }
            text += ")";
        }
        text += " -> ";
        text += function_arg_display(args.back());
        if (nullable)
            text = "(" + text + ")?";
        return text;
    } else if (type_container_arity(kind) > 0 && !args.empty()) {
        text = builtin_kind_name(kind);
        text += '[';
        std::size_t count =
            (kind == TYPE_GENERATOR && args.size() == 2 &&
             type_is_any(args[1]))
                ? 1
                : args.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (i != 0) text += ", ";
            text += args[i] ? args[i]->display : "any";
        }
        text += ']';
    } else {
        text = builtin_kind_name(kind);
    }
    if (nullable)
        text += '?';
    return text;
}

const TypeNode *intern_type(enum type_constraint_kind kind,
                            const std::string &name,
                            const std::string &module, bool nullable,
                            const std::vector<const TypeNode *> &args,
                            int optional_params = 0,
                            bool variadic = false,
                            const TypeNode *rest = nullptr) {
    std::string key = node_key(kind, name, module, nullable, args,
                               optional_params, variadic, rest);
    auto &pool = type_pool();
    auto it = pool.find(key);
    if (it != pool.end())
        return it->second.get();

    std::unique_ptr<TypeNode> node(new TypeNode());
    node->kind = kind;
    node->name = name;
    node->module = module;
    node->nullable = nullable;
    node->args = args;
    node->optional_params = optional_params;
    node->variadic = variadic;
    node->rest = rest;
    node->display = node_display(kind, name, args, nullable, variadic, rest);
    const TypeNode *raw = node.get();
    pool.emplace(std::move(key), std::move(node));
    return raw;
}

} // namespace

const TypeNode *type_any() {
    return type_builtin(TYPE_ANY);
}

const TypeNode *type_builtin(enum type_constraint_kind kind, bool nullable) {
    return intern_type(kind, std::string(), std::string(), nullable,
                       std::vector<const TypeNode *>());
}

const TypeNode *type_named(const char *module, const char *name,
                           bool nullable) {
    return intern_type(TYPE_NAMED, name ? name : "",
                       module ? module : "", nullable,
                       std::vector<const TypeNode *>());
}

const TypeNode *type_variable(const char *name) {
    return intern_type(TYPE_VAR, name ? name : "", std::string(), false,
                       std::vector<const TypeNode *>());
}

const TypeNode *type_container(enum type_constraint_kind kind,
                               const std::vector<const TypeNode *> &args,
                               bool nullable) {
    return intern_type(kind, std::string(), std::string(), nullable, args);
}

const TypeNode *make_function_type(
    enum type_constraint_kind kind,
    const std::vector<const TypeNode *> &args, int optional_params,
    bool variadic, bool nullable, const TypeNode *rest) {
    return intern_type(kind, std::string(), std::string(), nullable, args,
                       optional_params, variadic, rest);
}

const TypeNode *type_nullable(const TypeNode *type) {
    if (!type || type->nullable)
        return type ? type : type_any();
    return intern_type(type->kind, type->name, type->module, true,
                       type->args, type->optional_params, type->variadic,
                       type->rest);
}

const TypeNode *type_from_constraint(const struct type_constraint *type) {
    if (!type)
        return type_any();
    if (type->detail) {
        const TypeNode *node = static_cast<const TypeNode *>(type->detail);
        if (type->nullable && !node->nullable)
            return type_nullable(node);
        return node;
    }
    return intern_type(type->kind, type->name ? type->name : "",
                       type->module ? type->module : "", type->nullable,
                       std::vector<const TypeNode *>());
}

bool type_is_any(const TypeNode *type) {
    return !type || type->kind == TYPE_ANY;
}

int type_container_arity(enum type_constraint_kind kind) {
    switch (kind) {
        case TYPE_LIST:
        case TYPE_SET:
        case TYPE_DEQUE:
        case TYPE_HEAP:
        case TYPE_TASK:
            return 1;
        case TYPE_DICT:
        case TYPE_RESULT:
            return 2;
        case TYPE_GENERATOR:
        case TYPE_PAIR:
        case TYPE_UNION:
            return 2;
        default:
            return -1;
    }
}

bool type_is_container(const TypeNode *type) {
    return type && type_container_arity(type->kind) > 0;
}

bool type_is_function_signature(const TypeNode *type) {
    return type && (type->kind == TYPE_CALLABLE ||
                    type->kind == TYPE_FUNCTION ||
                    type->kind == TYPE_BUILTIN) &&
           !type->args.empty();
}

int type_function_param_count(const TypeNode *type) {
    if (!type_is_function_signature(type))
        return -1;
    return static_cast<int>(type->args.size() - 1);
}

int type_function_optional_params(const TypeNode *type) {
    if (!type_is_function_signature(type))
        return 0;
    return type->optional_params;
}

bool type_function_is_variadic(const TypeNode *type) {
    return type_is_function_signature(type) && type->variadic;
}

const TypeNode *type_function_rest(const TypeNode *type) {
    if (!type_is_function_signature(type) || !type->variadic)
        return type_any();
    return type->rest ? type->rest : type_any();
}

const TypeNode *type_function_param(const TypeNode *type,
                                     std::size_t index) {
    if (!type_is_function_signature(type) || index + 1 >= type->args.size())
        return type_any();
    return type->args[index];
}

const TypeNode *type_function_result(const TypeNode *type) {
    if (!type_is_function_signature(type))
        return type_any();
    return type->args.back();
}

bool type_constraint_has_arguments(const struct type_constraint *type) {
    if (!type || !type->detail)
        return false;
    const TypeNode *node = static_cast<const TypeNode *>(type->detail);
    return !node->args.empty();
}

bool type_constraint_is_function_type(const struct type_constraint *type) {
    if (!type || !type->detail)
        return false;
    const TypeNode *node = static_cast<const TypeNode *>(type->detail);
    return node->kind == TYPE_CALLABLE && !node->args.empty();
}

const TypeNode *type_element(const TypeNode *type, std::size_t index) {
    if (!type || index >= type->args.size())
        return type_any();
    return type->args[index] ? type->args[index] : type_any();
}

const char *type_display(const TypeNode *type) {
    return type ? type->display.c_str() : "any";
}

bool type_equal(const TypeNode *a, const TypeNode *b) {
    return a == b;
}

struct type_constraint *type_apply_generic(struct type_constraint *base,
                                           struct type_constraint *arg1,
                                           struct type_constraint *arg2) {
    if (!base) {
        free_type_constraint(arg1);
        free_type_constraint(arg2);
        return NULL;
    }

    if (base->kind == TYPE_GENERATOR) {
        if (!arg1) {
            char message[256];
            std::snprintf(message, sizeof(message),
                          "type generator expects 1 or 2 type argument(s)");
            diag_report(g_token_line, g_token_column, "type error", message);
            g_statement_invalid = true;
            free_type_constraint(arg1);
            free_type_constraint(arg2);
            return base;
        }
        std::vector<const TypeNode *> args;
        args.push_back(type_from_constraint(arg1));
        args.push_back(arg2 ? type_from_constraint(arg2) : type_any());
        free_type_constraint(arg1);
        free_type_constraint(arg2);
        const TypeNode *node =
            type_container(TYPE_GENERATOR, args, base->nullable);
        base->display = node->display.c_str();
        base->detail = node;
        return base;
    }

    int arity = type_container_arity(base->kind);
    if (arity < 0) {
        char message[256];
        std::snprintf(message, sizeof(message),
                      "type %s does not accept type arguments",
                      type_constraint_name(base));
        diag_report(g_token_line, g_token_column, "type error", message);
        g_statement_invalid = true;
        free_type_constraint(arg1);
        free_type_constraint(arg2);
        return base;
    }
    if ((arity == 1 && (!arg1 || arg2)) ||
        (arity == 2 && (!arg1 || !arg2))) {
        char message[256];
        std::snprintf(message, sizeof(message),
                      "type %s expects %d type argument(s)",
                      type_constraint_name(base), arity);
        diag_report(g_token_line, g_token_column, "type error", message);
        g_statement_invalid = true;
        free_type_constraint(arg1);
        free_type_constraint(arg2);
        return base;
    }

    std::vector<const TypeNode *> args;
    args.push_back(type_from_constraint(arg1));
    if (arity == 2)
        args.push_back(type_from_constraint(arg2));
    free_type_constraint(arg1);
    free_type_constraint(arg2);

    const TypeNode *node = type_container(base->kind, args, base->nullable);
    base->display = node->display.c_str();
    base->detail = node;
    return base;
}

struct type_constraint *type_apply_generic_list(
    struct type_constraint *base,
    const std::vector<struct type_constraint *> &args) {
    if (!base) {
        for (struct type_constraint *arg : args)
            free_type_constraint(arg);
        return NULL;
    }

    if (base->kind == TYPE_UNION) {
        if (args.size() < 2) {
            char message[256];
            std::snprintf(message, sizeof(message),
                          "type union expects at least 2 type argument(s)");
            diag_report(g_token_line, g_token_column, "type error", message);
            g_statement_invalid = true;
            for (struct type_constraint *arg : args)
                free_type_constraint(arg);
            return base;
        }
        std::vector<const TypeNode *> members;
        std::function<void(const TypeNode *)> append =
            [&](const TypeNode *type) {
                if (type && type->kind == TYPE_UNION) {
                    for (const TypeNode *member : type->args)
                        append(member);
                    return;
                }
                members.push_back(type ? type : type_any());
            };
        for (struct type_constraint *arg : args)
            append(type_from_constraint(arg));
        for (struct type_constraint *arg : args)
            free_type_constraint(arg);
        const TypeNode *node =
            type_container(TYPE_UNION, members, base->nullable);
        base->display = node->display.c_str();
        base->detail = node;
        return base;
    }

    int arity = type_container_arity(base->kind);
    if (arity < 0 || args.size() != static_cast<std::size_t>(arity)) {
        char message[256];
        if (arity < 0) {
            std::snprintf(message, sizeof(message),
                          "type %s does not accept type arguments",
                          type_constraint_name(base));
        } else {
            std::snprintf(message, sizeof(message),
                          "type %s expects %d type argument(s)",
                          type_constraint_name(base), arity);
        }
        diag_report(g_token_line, g_token_column, "type error", message);
        g_statement_invalid = true;
        for (struct type_constraint *arg : args)
            free_type_constraint(arg);
        return base;
    }

    std::vector<const TypeNode *> members;
    members.reserve(args.size());
    for (struct type_constraint *arg : args)
        members.push_back(type_from_constraint(arg));
    for (struct type_constraint *arg : args)
        free_type_constraint(arg);
    const TypeNode *node =
        type_container(base->kind, members, base->nullable);
    base->display = node->display.c_str();
    base->detail = node;
    return base;
}

struct type_constraint *type_apply_function(
    const std::vector<struct type_constraint *> &params,
    struct type_constraint *result) {
    std::vector<const TypeNode *> args;
    args.reserve(params.size() + 1);
    for (struct type_constraint *param : params)
        args.push_back(type_from_constraint(param));
    args.push_back(type_from_constraint(result));

    const TypeNode *node = type_container(TYPE_CALLABLE, args);
    struct type_constraint *type = new_type_constraint(TYPE_CALLABLE);
    type->display = node->display.c_str();
    type->detail = node;

    for (struct type_constraint *param : params)
        free_type_constraint(param);
    free_type_constraint(result);
    return type;
}

struct type_constraint *type_apply_nullable(struct type_constraint *base) {
    if (!base)
        return NULL;
    const TypeNode *node = type_from_constraint(base);
    const TypeNode *nullable = type_nullable(node);
    base->nullable = true;
    base->display = nullable->display.c_str();
    base->detail = nullable;
    return base;
}

} // namespace typecheck
} // namespace skein
