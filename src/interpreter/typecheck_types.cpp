#include "typecheck_types.h"

#include "ast.h"
#include "diagnostics.h"
#include "structtype.h"
#include "typecheck_state.h"
#include "valuetype.h"

#include <cstddef>

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
        case T_userdata: return type_builtin(TYPE_USERDATA);
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

} // namespace

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
        }
    }
    return sig;
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
    return sig;
}

FunctionSig method_signature_from_def(const struct funcdef *def) {
    FunctionSig sig;
    if (!def)
        return sig;
    sig = signature_from_symlist(def->sl, true);
    sig.result = def->ret_type ? type_from_constraint(def->ret_type)
                               : type_any();
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
    if (actual->kind == TYPE_NULL) {
        return expected->kind == TYPE_NULL ? MatchResult::Ok
                                           : MatchResult::Mismatch;
    }

    if (expected->kind == TYPE_NUMBER) {
        if (actual->kind == TYPE_INT || actual->kind == TYPE_FLOAT ||
            actual->kind == TYPE_NUMBER)
            return MatchResult::Ok;
        return MatchResult::Mismatch;
    }
    if (expected->kind == TYPE_CALLABLE) {
        if (actual->kind == TYPE_FUNCTION ||
            actual->kind == TYPE_BUILTIN ||
            actual->kind == TYPE_CALLABLE)
            return MatchResult::Ok;
        return MatchResult::Mismatch;
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

} // namespace detail
} // namespace typecheck
} // namespace skein
