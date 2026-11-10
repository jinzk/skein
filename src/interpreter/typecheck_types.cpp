#include "typecheck_types.h"

#include "ast.h"
#include "diagnostics.h"
#include "structtype.h"
#include "typecheck_state.h"
#include "valuetype.h"

#include <cstddef>
#include <cstring>

namespace skein {
namespace typecheck {
namespace detail {

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
        case T_dict: {
            const struct dict_value_t *dict =
                (const struct dict_value_t *)value;
            if (dict->payload && dict->payload->is_namespace)
                return type_any();
            return type_container(TYPE_DICT,
                                  {type_any(), type_any()});
        }
        case T_struct: {
            const struct struct_value_t *structure =
                (const struct struct_value_t *)value;
            if (structure->def)
                return type_named(structure->def->module.c_str(),
                                  structure->def->name.c_str());
            return type_any();
        }
        case T_enum: {
            const struct enum_value_t *enumeration =
                (const struct enum_value_t *)value;
            if (enumeration->def)
                return type_named(enumeration->def->module.c_str(),
                                  enumeration->def->name.c_str());
            return type_any();
        }
        case T_set: return type_container(TYPE_SET, {type_any()});
        case T_deque: return type_container(TYPE_DEQUE, {type_any()});
        case T_heap: return type_container(TYPE_HEAP, {type_any()});
        case T_result:
            return type_container(TYPE_RESULT,
                                  {type_any(), type_any()});
        case T_error: return type_builtin(TYPE_ERROR);
        case T_func: return type_builtin(TYPE_FUNCTION);
        case T_builtin: return type_builtin(TYPE_BUILTIN);
        case T_method:
        case T_native: return type_builtin(TYPE_CALLABLE);
        case T_userdata: {
            const struct userdata_value_t *userdata =
                (const struct userdata_value_t *)value;
            const char *name =
                userdata->meta ? userdata->meta->name : NULL;
            if (name && std::strcmp(name, "task") == 0)
                return type_container(TYPE_TASK, {type_any()});
            if (name && std::strcmp(name, "generator") == 0)
                return type_container(TYPE_GENERATOR,
                                      {type_any(), type_any()});
            if (name && std::strcmp(name, "tcp_socket") == 0)
                return type_builtin(TYPE_SOCKET);
            if (name && std::strcmp(name, "tcp_listener") == 0)
                return type_builtin(TYPE_LISTENER);
            if (name && std::strcmp(name, "websocket") == 0)
                return type_builtin(TYPE_WEBSOCKET);
            if (userdata->meta && userdata->meta->stream)
                return type_builtin(TYPE_STREAM);
            return type_builtin(TYPE_USERDATA);
        }
        default: return type_any();
    }
}

namespace {

const TypeNode *strip_nullable(const TypeNode *type) {
    if (!type || !type->nullable)
        return type;
    if (type_container_arity(type->kind) > 0)
        return type_container(type->kind, type->args, false);
    if (type->kind == TYPE_NAMED)
        return type_named(type->module.c_str(), type->name.c_str(), false);
    if (type->kind == TYPE_VAR)
        return type_variable(type->name.c_str());
    if (type_is_function_signature(type))
        return make_function_type(type->kind, type->args,
                                  type->optional_params, type->variadic,
                                  false, type->rest);
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

MatchResult combine_result(MatchResult a, MatchResult b) {
    if (a == MatchResult::Mismatch || b == MatchResult::Mismatch)
        return MatchResult::Mismatch;
    if (a == MatchResult::Unknown || b == MatchResult::Unknown)
        return MatchResult::Unknown;
    return MatchResult::Ok;
}

MatchResult match_function_signature(const TypeNode *actual,
                                     const TypeNode *expected) {
    std::size_t actual_params = actual->args.size() - 1;
    std::size_t expected_params = expected->args.size() - 1;
    if (actual_params > expected_params) {
        /*
         * Optional trailing parameters are static-only metadata: a template
         * can be assigned to a shorter function type when the extra
         * parameters may be omitted.  Required parameter counts must still
         * agree.
         */
        std::size_t optional =
            static_cast<std::size_t>(type_function_optional_params(actual));
        if (actual_params - expected_params > optional)
            return MatchResult::Mismatch;
    }

    MatchResult result = MatchResult::Ok;
    std::size_t common =
        actual_params < expected_params ? actual_params : expected_params;
    for (std::size_t i = 0; i < common; ++i) {
        /* Parameters are contravariant: every expected input must be
         * accepted by the actual function. */
        result = combine_result(
            result, match_type(expected->args[i], actual->args[i]));
        if (result == MatchResult::Mismatch)
            return result;
    }
    if (expected_params > actual_params) {
        if (!actual->variadic)
            return MatchResult::Mismatch;
        const TypeNode *rest = type_function_rest(actual);
        for (std::size_t i = actual_params; i < expected_params; ++i) {
            result = combine_result(
                result, match_type(expected->args[i], rest));
            if (result == MatchResult::Mismatch)
                return result;
        }
    }
    result = combine_result(
        result, match_type(actual->args.back(), expected->args.back()));
    return result;
}

} // namespace

bool needs_static_check(const TypeNode *type) {
    if (!type || type->kind == TYPE_ANY)
        return false;
    if (type->kind == TYPE_SOCKET || type->kind == TYPE_LISTENER ||
        type->kind == TYPE_WEBSOCKET || type->kind == TYPE_STREAM)
        return true;
    if (has_concrete_container_arg(type))
        return true;
    if (type_is_function_signature(type)) {
        for (const TypeNode *arg : type->args) {
            if (arg && arg->kind != TYPE_ANY)
                return true;
        }
    }
    return false;
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
    if (a->kind == TYPE_INVALID || b->kind == TYPE_INVALID)
        return type_any();
    if ((a->kind == TYPE_INT || a->kind == TYPE_FLOAT ||
         a->kind == TYPE_NUMBER) &&
        (b->kind == TYPE_INT || b->kind == TYPE_FLOAT ||
         b->kind == TYPE_NUMBER))
        return type_builtin(TYPE_NUMBER);
    if (a->kind == TYPE_NAMED && b->kind == TYPE_NAMED &&
        a->name == b->name && a->module == b->module)
        return a;

    if (a->kind == TYPE_UNION || b->kind == TYPE_UNION) {
        auto contains = [](const TypeNode *type, const TypeNode *item) {
            for (const TypeNode *arg : type->args) {
                if (arg == item)
                    return true;
            }
            return false;
        };
        if (a->kind == TYPE_UNION && contains(a, b))
            return a;
        if (b->kind == TYPE_UNION && contains(b, a))
            return b;
        std::vector<const TypeNode *> args;
        auto add_unique = [&](const TypeNode *type) {
            if (type->kind == TYPE_UNION) {
                for (const TypeNode *arg : type->args) {
                    bool found = false;
                    for (const TypeNode *existing : args) {
                        if (existing == arg) {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        args.push_back(arg);
                }
                return;
            }
            for (const TypeNode *existing : args) {
                if (existing == type)
                    return;
            }
            args.push_back(type);
        };
        add_unique(a);
        add_unique(b);
        return type_container(TYPE_UNION, args);
    }

    if (a->kind == b->kind && type_container_arity(a->kind) > 0 &&
        a->args.size() == b->args.size()) {
        std::vector<const TypeNode *> args;
        args.reserve(a->args.size());
        for (std::size_t i = 0; i < a->args.size(); ++i)
            args.push_back(join_types(a->args[i], b->args[i]));
        return type_container(a->kind, args, a->nullable || b->nullable);
    }
    return type_container(TYPE_UNION, {a, b});
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

FunctionSig signature_from_symlist(struct symlist *sl, bool skip_self) {
    FunctionSig sig;
    bool skipped = false;
    for (struct symlist *item = sl; item; item = item->next) {
        if (skip_self && !skipped) {
            skipped = true;
            continue;
        }
        const TypeNode *type = type_from_constraint(item->type);
        if (item->variadic) {
            sig.variadic = true;
            sig.rest = type;
        } else {
            sig.params.push_back(type);
            if (item->has_default)
                ++sig.optional_params;
        }
    }
    return sig;
}

const TypeNode *signature_result_type(const FunctionSig &sig) {
    const TypeNode *result = sig.result ? sig.result : type_any();
    if (sig.is_generator) {
        if (result->kind == TYPE_GENERATOR)
            return result;
        return type_container(
            TYPE_GENERATOR,
            {result, sig.generator_send ? sig.generator_send : type_any()});
    }
    return result;
}

struct ast *find_single_return(struct ast *node, int &count) {
    if (!node)
        return NULL;
    if (node->nodetype == 'R') {
        ++count;
        return count == 1 ? ((struct fnret *)node)->v : NULL;
    }
    if (node->nodetype == 'L') {
        struct ast *left = find_single_return(node->l, count);
        struct ast *right = find_single_return(node->r, count);
        return count == 1 ? (left ? left : right) : NULL;
    }
    if (node->nodetype == 'X')
        return find_single_return(((struct nscope *)node)->ast, count);
    return NULL;
}

FunctionSig method_signature_from_code(const struct function_code *code) {
    FunctionSig sig;
    if (!code)
        return sig;
    sig = signature_from_symlist(code->sl, true);
    sig.result = code->ret_type
                     ? type_from_constraint(code->ret_type.get())
                     : type_any();
    sig.is_generator = code->is_generator;
    return sig;
}

FunctionSig method_signature_from_def(const struct funcdef *def) {
    FunctionSig sig;
    if (!def)
        return sig;
    sig = signature_from_symlist(def->sl, true);
    sig.result = def->ret_type ? type_from_constraint(def->ret_type)
                               : type_any();
    sig.is_generator = def->is_generator;
    return sig;
}

bool lookup_named_type(const TypeNode *type,
                       structtype_lookup_result &out) {
    if (!type || type->kind != TYPE_NAMED)
        return false;
    out = structtype_lookup_result();

    const char *logical_module = diag_current_module_name().c_str();
    struct_def *structure =
        structtype_find_struct(type->module.c_str(), type->name.c_str());
    if (!structure)
        structure = structtype_find_struct_by_name(
            logical_module, type->name.c_str(), NULL);
    if (structure) {
        out.kind = STRUCTTYPE_LOOKUP_STRUCT;
        out.structure = structure;
        return true;
    }
    enum_def *enumeration =
        structtype_find_enum(type->module.c_str(), type->name.c_str());
    if (!enumeration)
        enumeration = structtype_find_enum_by_name(
            logical_module, type->name.c_str(), NULL);
    if (enumeration) {
        out.kind = STRUCTTYPE_LOOKUP_ENUM;
        out.enumeration = enumeration;
        return true;
    }
    trait_def *trait =
        structtype_find_trait(type->module.c_str(), type->name.c_str());
    if (!trait)
        trait = structtype_find_trait_by_name(
            logical_module, type->name.c_str(), NULL);
    if (trait) {
        out.kind = STRUCTTYPE_LOOKUP_TRAIT;
        out.trait = trait;
        return true;
    }

    std::string error;
    return structtype_lookup_type(
               type->module.c_str(), diag_current_module_name().c_str(),
               type->name.c_str(), out, error) ==
           STRUCTTYPE_LOOKUP_FOUND;
}

MatchResult match_type(const TypeNode *actual, const TypeNode *expected) {
    if (!expected || expected->kind == TYPE_ANY)
        return MatchResult::Ok;
    if (!actual)
        actual = type_any();
    if (actual->kind == TYPE_ANY || actual->kind == TYPE_INVALID)
        return MatchResult::Unknown;

    if (type_contains_vars(expected) || type_contains_vars(actual)) {
        if (type_contains_vars(expected) && type_contains_vars(actual))
            return MatchResult::Unknown;
        TypeBindings bindings;
        if (type_contains_vars(expected)) {
            if (!bind_type(actual, expected, bindings))
                return MatchResult::Mismatch;
            const TypeNode *instantiated =
                instantiate_type(expected, bindings);
            if (instantiated != expected)
                return match_type(actual, instantiated);
            return MatchResult::Ok;
        }
        if (!bind_type(expected, actual, bindings))
            return MatchResult::Mismatch;
        const TypeNode *instantiated = instantiate_type(actual, bindings);
        if (instantiated != actual)
            return match_type(instantiated, expected);
        return MatchResult::Unknown;
    }

    if (actual->kind == TYPE_UNION) {
        MatchResult result = MatchResult::Ok;
        for (const TypeNode *member : actual->args) {
            result = combine_result(result,
                                    match_type(member, expected));
            if (result == MatchResult::Mismatch)
                return MatchResult::Mismatch;
        }
        return result;
    }
    if (expected->kind == TYPE_UNION) {
        bool unknown = false;
        for (const TypeNode *member : expected->args) {
            MatchResult result = match_type(actual, member);
            if (result == MatchResult::Ok)
                return MatchResult::Ok;
            if (result == MatchResult::Unknown)
                unknown = true;
        }
        return unknown ? MatchResult::Unknown : MatchResult::Mismatch;
    }

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
    if (actual->kind == TYPE_NULL || actual->kind == TYPE_VOID) {
        return expected->kind == TYPE_NULL || expected->kind == TYPE_VOID
                   ? MatchResult::Ok
                   : MatchResult::Mismatch;
    }
    if (expected->kind == TYPE_VOID)
        return MatchResult::Mismatch;

    if (expected->kind == TYPE_NUMBER) {
        if (actual->kind == TYPE_INT || actual->kind == TYPE_FLOAT ||
            actual->kind == TYPE_NUMBER)
            return MatchResult::Ok;
        return MatchResult::Mismatch;
    }
    if (expected->kind == TYPE_CALLABLE) {
        if (expected->args.empty()) {
            if (actual->kind == TYPE_FUNCTION ||
                actual->kind == TYPE_BUILTIN ||
                actual->kind == TYPE_CALLABLE)
                return MatchResult::Ok;
            return MatchResult::Mismatch;
        }
        if (type_is_function_signature(actual))
            return match_function_signature(actual, expected);
        if (actual->kind == TYPE_FUNCTION ||
            actual->kind == TYPE_BUILTIN ||
            actual->kind == TYPE_CALLABLE)
            return MatchResult::Unknown;
        return MatchResult::Mismatch;
    }
    if (expected->kind == TYPE_FUNCTION) {
        if (actual->kind == TYPE_FUNCTION ||
            actual->kind == TYPE_CALLABLE)
            return MatchResult::Ok;
        return MatchResult::Mismatch;
    }
    if (expected->kind == TYPE_BUILTIN) {
        if (actual->kind == TYPE_BUILTIN)
            return MatchResult::Ok;
        if (actual->kind == TYPE_CALLABLE)
            return MatchResult::Unknown;
        return MatchResult::Mismatch;
    }
    if (expected->kind == TYPE_STREAM && actual->kind == TYPE_SOCKET)
        return MatchResult::Ok;
    if (expected->kind == TYPE_SOCKET && actual->kind == TYPE_LISTENER)
        return MatchResult::Ok;
    if ((expected->kind == TYPE_PAIR &&
         actual->kind == TYPE_TUPLE) ||
        (expected->kind == TYPE_TUPLE &&
         actual->kind == TYPE_PAIR)) {
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
    if (actual->kind != expected->kind)
        return MatchResult::Mismatch;

    if (actual->kind == TYPE_NAMED) {
        if (structtype_named_types_equal(actual->module.c_str(),
                                         actual->name.c_str(),
                                         expected->module.c_str(),
                                         expected->name.c_str()))
            return MatchResult::Ok;
        structtype_lookup_result expected_lookup;
        if (lookup_named_type(expected, expected_lookup) &&
            expected_lookup.trait) {
            structtype_lookup_result actual_lookup;
            if (!lookup_named_type(actual, actual_lookup))
                return MatchResult::Unknown;
            const type_def *actual_type =
                actual_lookup.structure
                    ? static_cast<const type_def *>(actual_lookup.structure)
                    : static_cast<const type_def *>(actual_lookup.enumeration);
            if (!actual_type)
                return MatchResult::Unknown;
            for (const trait_def *trait : actual_type->traits) {
                if (trait == expected_lookup.trait)
                    return MatchResult::Ok;
            }
            return MatchResult::Mismatch;
        }
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

bool type_contains_vars(const TypeNode *type) {
    if (!type)
        return false;
    if (type->kind == TYPE_VAR)
        return true;
    for (const TypeNode *arg : type->args) {
        if (type_contains_vars(arg))
            return true;
    }
    return false;
}

const TypeNode *instantiate_type(const TypeNode *type,
                                 const TypeBindings &bindings) {
    if (!type)
        return type_any();
    if (type->kind == TYPE_VAR) {
        auto it = bindings.find(type->name);
        const TypeNode *bound =
            it == bindings.end() || !it->second ? type_any() : it->second;
        if (type->nullable && !bound->nullable)
            return type_nullable(bound);
        return bound;
    }
    if (type->args.empty())
        return type;

    std::vector<const TypeNode *> args;
    args.reserve(type->args.size());
    bool changed = false;
    for (const TypeNode *arg : type->args) {
        const TypeNode *instantiated = instantiate_type(arg, bindings);
        args.push_back(instantiated);
        if (instantiated != arg)
            changed = true;
    }
    const TypeNode *rest =
        type->rest ? instantiate_type(type->rest, bindings) : nullptr;
    if (rest != type->rest)
        changed = true;
    if (!changed)
        return type;
    if (type->kind == TYPE_CALLABLE || type->kind == TYPE_FUNCTION ||
        type->kind == TYPE_BUILTIN) {
        return make_function_type(type->kind, args, type->optional_params,
                                  type->variadic, type->nullable, rest);
    }
    return type_container(type->kind, args, type->nullable);
}

bool bind_type(const TypeNode *actual, const TypeNode *pattern,
               TypeBindings &bindings) {
    if (!pattern)
        return true;
    if (!actual)
        actual = type_any();

    if (pattern->kind == TYPE_VAR) {
        if (pattern->name.empty())
            return true;
        auto it = bindings.find(pattern->name);
        if (it == bindings.end()) {
            bindings[pattern->name] = actual;
            return true;
        }
        const TypeNode *bound = it->second;
        if (type_is_any(bound)) {
            if (!type_is_any(actual))
                bindings[pattern->name] = actual;
            return true;
        }
        if (type_is_any(actual))
            return true;
        return match_type(actual, bound) != MatchResult::Mismatch;
    }

    if (pattern->kind == TYPE_ANY || actual->kind == TYPE_ANY)
        return true;

    if (pattern->nullable) {
        if (actual->kind == TYPE_NULL)
            return true;
        const TypeNode *a = actual->nullable ? strip_nullable(actual) : actual;
        return bind_type(a, strip_nullable(pattern), bindings);
    }
    if (actual->nullable || actual->kind == TYPE_NULL)
        return false;

    if (type_is_function_signature(pattern)) {
        if (!type_is_function_signature(actual)) {
            if (actual->kind == TYPE_FUNCTION ||
                actual->kind == TYPE_BUILTIN ||
                actual->kind == TYPE_CALLABLE)
                return true;
            return false;
        }
        std::size_t pattern_params = pattern->args.size() - 1;
        std::size_t actual_params = actual->args.size() - 1;
        if (pattern->variadic) {
            std::size_t common =
                actual_params < pattern_params ? actual_params
                                               : pattern_params;
            for (std::size_t i = 0; i < common; ++i) {
                if (!bind_type(actual->args[i], pattern->args[i],
                               bindings))
                    return false;
            }
        } else {
            if (actual_params > pattern_params) {
                std::size_t actual_optional =
                    static_cast<std::size_t>(actual->optional_params);
                if (actual_params - pattern_params > actual_optional)
                    return false;
            }
            std::size_t common =
                actual_params < pattern_params ? actual_params
                                               : pattern_params;
            for (std::size_t i = 0; i < common; ++i) {
                if (!bind_type(actual->args[i], pattern->args[i],
                               bindings))
                    return false;
            }
            if (actual_params < pattern_params) {
                std::size_t pattern_optional = static_cast<std::size_t>(
                    pattern->optional_params);
                if (actual->variadic) {
                    const TypeNode *rest = type_function_rest(actual);
                    for (std::size_t i = actual_params;
                         i < pattern_params; ++i) {
                        if (!bind_type(rest, pattern->args[i],
                                       bindings))
                            return false;
                    }
                } else if (pattern_params - actual_params >
                           pattern_optional) {
                    return false;
                }
            }
        }
        return bind_type(actual->args.back(), pattern->args.back(),
                         bindings);
    }

    int arity = type_container_arity(pattern->kind);
    if (arity > 0) {
        if (actual->kind != pattern->kind)
            return false;
        if (pattern->args.empty() || actual->args.empty())
            return true;
        if (actual->args.size() != pattern->args.size())
            return false;
        for (std::size_t i = 0; i < pattern->args.size(); ++i) {
            if (!bind_type(actual->args[i], pattern->args[i], bindings))
                return false;
        }
        return true;
    }

    return match_type(actual, pattern) != MatchResult::Mismatch;
}

} // namespace detail
} // namespace typecheck
} // namespace skein
