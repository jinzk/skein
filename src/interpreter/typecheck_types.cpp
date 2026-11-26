#include "typecheck_types.h"

#include "ast.h"
#include "diagnostics.h"
#include "refvalue.h"
#include "static_type.h"
#include "structtype.h"
#include "typecheck_state.h"
#include "valuetype.h"

#include <cstddef>
#include <cstring>
#include <unordered_set>

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
                return type_named(structure->def->source.c_str(),
                                  structure->def->name.c_str());
            return type_any();
        }
        case T_enum: {
            const struct enum_value_t *enumeration =
                (const struct enum_value_t *)value;
            if (enumeration->def)
                return type_named(enumeration->def->source.c_str(),
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
            if (is_ref_value(value) || is_weak_value(value)) {
                struct_def *def = ref_value_def(value);
                const TypeNode *inner =
                    def ? type_named(def->source.c_str(), def->name.c_str())
                        : type_any();
                return type_container(
                    is_ref_value(value) ? TYPE_REF : TYPE_WEAK, {inner});
            }
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
        return type_named(type->source.c_str(), type->name.c_str(), false);
    if (type->kind == TYPE_VAR)
        return type_variable(type->name.c_str());
    if (type_is_function_signature(type))
        return make_function_type(type->kind, type->args,
                                  type->optional_params, type->variadic,
                                  false, type->rest);
    return type_builtin(type->kind, false);
}

const TypeNode *normalize_nullable(const TypeNode *type) {
    if (type && type->nullable)
        return type_nullable(type);
    return type;
}

const TypeNode *strip_null_members(const TypeNode *type) {
    if (!type || type->kind != TYPE_UNION)
        return type;
    std::vector<const TypeNode *> members;
    for (const TypeNode *member : type->args) {
        if (member && member->kind == TYPE_NULL)
            continue;
        members.push_back(member);
    }
    if (members.size() == type->args.size())
        return type;
    if (members.empty())
        return type_builtin(TYPE_NULL);
    if (members.size() == 1)
        return members[0];
    return type_container(TYPE_UNION, members);
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
    if (type->kind == TYPE_NAMED) {
        if (!type->args.empty())
            return true;
        structtype_lookup_result lookup;
        if (lookup_named_type(type, lookup) && lookup.protocol &&
            lookup.protocol->builtin_kind != PROTOCOL_BUILTIN_NONE)
            return true;
    }
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
        return b ? normalize_nullable(b) : type_any();
    if (!b)
        return normalize_nullable(a);
    a = normalize_nullable(a);
    b = normalize_nullable(b);
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
        a->name == b->name && a->source == b->source)
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

const TypeNode *tuple_pattern_param_type(struct symlist *item) {
    if (!item || item->type || item->variadic)
        return nullptr;
    const struct pattern_node *pattern =
        (const struct pattern_node *)symlist_pattern_ast(item);
    if (!pattern || pattern->nodetype != NODE_PATTERN_TUPLE)
        return nullptr;
    std::size_t count = 0;
    for (const struct pattern_node *node =
             (const struct pattern_node *)pattern->items;
         node; node = (const struct pattern_node *)node->next)
        ++count;
    if (count < 2)
        return nullptr;
    std::vector<const TypeNode *> args(count, type_any());
    return type_container(count == 2 ? TYPE_PAIR : TYPE_TUPLE, args);
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
        if (!item->type) {
            if (const TypeNode *implied = tuple_pattern_param_type(item))
                type = implied;
        }
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

    const char *logical_module = diag_current_package_name().c_str();
    const char *resolved_source = type->source.c_str();
    std::string selected_source;
    std::string selected_name = type->name;
    auto source_it = g_selective_imports.find(type->source);
    if (source_it != g_selective_imports.end()) {
        auto alias_it = source_it->second.find(type->name);
        if (alias_it != source_it->second.end()) {
            selected_source = alias_it->second;
            resolved_source = selected_source.c_str();
            auto name_it = g_selective_import_names.find(type->source);
            if (name_it != g_selective_import_names.end()) {
                auto original_it = name_it->second.find(type->name);
                if (original_it != name_it->second.end())
                    selected_name = original_it->second;
            }
        }
    }

    struct_def *structure =
        structtype_find_struct(resolved_source, selected_name.c_str());
    if (!structure)
        structure = structtype_find_struct_by_name(logical_module,
                                                   selected_name.c_str(), NULL);
    if (!structure && !selected_source.empty())
        structure = structtype_find_struct_by_name(NULL, selected_name.c_str(),
                                                   NULL);
    if (structure) {
        out.kind = STRUCTTYPE_LOOKUP_STRUCT;
        out.structure = structure;
        return true;
    }
    enum_def *enumeration =
        structtype_find_enum(resolved_source, selected_name.c_str());
    if (!enumeration)
        enumeration = structtype_find_enum_by_name(logical_module,
                                                   selected_name.c_str(), NULL);
    if (!enumeration && !selected_source.empty())
        enumeration = structtype_find_enum_by_name(NULL, selected_name.c_str(),
                                                   NULL);
    if (enumeration) {
        out.kind = STRUCTTYPE_LOOKUP_ENUM;
        out.enumeration = enumeration;
        return true;
    }
    protocol_def *protocol =
        structtype_find_protocol(resolved_source, selected_name.c_str());
    if (!protocol)
        protocol = structtype_find_protocol_by_name(logical_module,
                                                    selected_name.c_str(), NULL);
    if (!protocol && !selected_source.empty())
        protocol = structtype_find_protocol_by_name(NULL, selected_name.c_str(),
                                                    NULL);
    if (protocol) {
        out.kind = STRUCTTYPE_LOOKUP_PROTOCOL;
        out.protocol = protocol;
        return true;
    }

    std::string error;
    return structtype_lookup_type(resolved_source,
                                  diag_current_package_name().c_str(),
                                  selected_name.c_str(), out, error) ==
           STRUCTTYPE_LOOKUP_FOUND;
}

namespace {

bool key_method_matches(const type_def *def, const char *name, int nparams,
                        enum type_constraint_kind result_kind) {
    std::shared_ptr<function_def> method = structtype_method(def, name);
    if (!method || !method->code)
        return false;
    const function_code *code = method->code.get();
    if (code->variadic || code->nparams != nparams || !code->ret_type)
        return false;
    const TypeNode *result = type_from_constraint(code->ret_type.get());
    return result && result->kind == result_kind && !result->nullable;
}

struct KeyContractState {
    std::unordered_set<const type_def *> active;
};

KeyContract key_contract_inner(const TypeNode *type,
                               KeyContractState &state);

KeyContract key_contract_named(const TypeNode *type,
                               KeyContractState &state) {
    structtype_lookup_result lookup;
    if (!lookup_named_type(type, lookup))
        return KeyContract::Unknown;
    if (lookup.kind == STRUCTTYPE_LOOKUP_PROTOCOL)
        return KeyContract::Unknown;
    const type_def *def = lookup.kind == STRUCTTYPE_LOOKUP_STRUCT
                              ? static_cast<const type_def *>(lookup.structure)
                              : static_cast<const type_def *>(lookup.enumeration);
    if (!def)
        return KeyContract::Unknown;

    std::shared_ptr<function_def> hash = structtype_method(def, "dict_hash");
    if (hash) {
        if (!key_method_matches(def, "dict_hash", 1, TYPE_INT))
            return KeyContract::No;
        std::shared_ptr<function_def> eq = structtype_method(def, "key_eq");
        if (eq && !key_method_matches(def, "key_eq", 2, TYPE_BOOL))
            return KeyContract::No;
        return KeyContract::Yes;
    }

    if (!state.active.insert(def).second)
        return KeyContract::No;

    KeyContract result = KeyContract::Yes;
    auto inspect_fields = [&](const std::vector<field_def> &fields) {
        for (const field_def &field : fields) {
            KeyContract field_contract =
                key_contract_inner(type_from_constraint(&field.type), state);
            if (field_contract == KeyContract::No) {
                result = KeyContract::No;
                return;
            }
            if (field_contract == KeyContract::Unknown)
                result = KeyContract::Unknown;
        }
    };

    if (lookup.kind == STRUCTTYPE_LOOKUP_STRUCT) {
        inspect_fields(lookup.structure->fields);
    } else {
        for (const variant_def &variant : lookup.enumeration->variants)
            inspect_fields(variant.fields);
    }

    state.active.erase(def);
    return result;
}

KeyContract key_contract_inner(const TypeNode *type,
                               KeyContractState &state) {
    if (!type || type->kind == TYPE_ANY || type->kind == TYPE_VAR)
        return KeyContract::Unknown;

    switch (type->kind) {
        case TYPE_ANY:
        case TYPE_VAR:
            return KeyContract::Unknown;

        case TYPE_INT:
        case TYPE_FLOAT:
        case TYPE_NUMBER:
        case TYPE_BOOL:
        case TYPE_STRING:
        case TYPE_CHAR:
        case TYPE_BYTE:
        case TYPE_BYTES:
        case TYPE_NULL:
            return KeyContract::Yes;

        case TYPE_TUPLE:
        case TYPE_PAIR: {
            KeyContract result = KeyContract::Yes;
            for (const TypeNode *arg : type->args) {
                KeyContract item = key_contract_inner(arg, state);
                if (item == KeyContract::No)
                    return KeyContract::No;
                if (item == KeyContract::Unknown)
                    result = KeyContract::Unknown;
            }
            return result;
        }

        case TYPE_UNION: {
            KeyContract result = KeyContract::Yes;
            for (const TypeNode *arg : type->args) {
                KeyContract item = key_contract_inner(arg, state);
                if (item == KeyContract::No)
                    return KeyContract::No;
                if (item == KeyContract::Unknown)
                    result = KeyContract::Unknown;
            }
            return result;
        }

        case TYPE_NAMED:
            return key_contract_named(type, state);

        case TYPE_LIST:
        case TYPE_DICT:
        case TYPE_SET:
        case TYPE_DEQUE:
        case TYPE_HEAP:
        case TYPE_REF:
        case TYPE_WEAK:
        case TYPE_RESULT:
        case TYPE_ERROR:
        case TYPE_FUNCTION:
        case TYPE_BUILTIN:
        case TYPE_CALLABLE:
        case TYPE_USERDATA:
        case TYPE_TASK:
        case TYPE_GENERATOR:
        case TYPE_SOCKET:
        case TYPE_LISTENER:
        case TYPE_WEBSOCKET:
        case TYPE_STREAM:
        case TYPE_VOID:
        case TYPE_INVALID:
            return KeyContract::No;
    }
    return KeyContract::Unknown;
}

} // namespace

KeyContract key_contract(const TypeNode *type) {
    KeyContractState state;
    return key_contract_inner(type, state);
}

namespace {

struct type_constraint constraint_from_static_type(const TypeNode *type) {
    struct type_constraint result;
    result.kind = type ? type->kind : TYPE_ANY;
    result.name = (type && !type->name.empty()) ? type->name.c_str() : NULL;
    result.source =
        (type && !type->source.empty()) ? type->source.c_str() : NULL;
    result.nullable = type ? type->nullable : false;
    result.arity = (type && (type->kind == TYPE_PAIR ||
                             type->kind == TYPE_TUPLE))
                       ? static_cast<int>(type->args.size())
                       : 0;
    result.display = NULL;
    result.detail = type;
    return result;
}

void bind_protocol_param(
    std::unordered_map<std::string, struct type_constraint> *bindings,
    const struct protocol_def *protocol, std::size_t index,
    const TypeNode *type) {
    if (!bindings || !protocol || index >= protocol->generic_params.size())
        return;
    (*bindings)[protocol->generic_params[index]] =
        constraint_from_static_type(type);
}

struct IterableContractState {
    std::unordered_set<const type_def *> active;
};

IterableContract iterable_contract_inner(const TypeNode *type,
                                         IterableContractState &state);

IterableContract iterable_contract_named(const TypeNode *type,
                                         IterableContractState &state) {
    structtype_lookup_result lookup;
    if (!lookup_named_type(type, lookup))
        return IterableContract::Unknown;
    if (lookup.kind == STRUCTTYPE_LOOKUP_PROTOCOL)
        return IterableContract::Unknown;
    const type_def *def =
        lookup.kind == STRUCTTYPE_LOOKUP_STRUCT
            ? static_cast<const type_def *>(lookup.structure)
            : static_cast<const type_def *>(lookup.enumeration);
    if (!def)
        return IterableContract::Unknown;

    std::shared_ptr<function_def> method = structtype_method(def, "iter");
    if (!method || !method->code)
        return IterableContract::No;
    const function_code *code = method->code.get();
    if (code->variadic || code->nparams != 1)
        return IterableContract::No;
    if (!code->ret_type) {
        /* The method exists but has no declared return type.  Treat it as
         * dynamically iterable; the runtime still performs the real check. */
        return IterableContract::Unknown;
    }
    if (!state.active.insert(def).second)
        return IterableContract::No;

    TypeBindings bindings;
    if (!type->args.empty() && !def->generic_params.empty() &&
        type->args.size() == def->generic_params.size()) {
        for (std::size_t i = 0; i < def->generic_params.size(); ++i)
            bindings[def->generic_params[i]] = type->args[i];
    }
    const TypeNode *result =
        type_from_constraint(code->ret_type.get());
    if (!bindings.empty())
        result = instantiate_type(result, bindings);
    IterableContract contract = iterable_contract_inner(result, state);
    state.active.erase(def);
    return contract;
}

IterableContract iterable_contract_inner(const TypeNode *type,
                                         IterableContractState &state) {
    if (!type)
        return IterableContract::Unknown;
    switch (type->kind) {
        case TYPE_LIST:
        case TYPE_DICT:
        case TYPE_SET:
        case TYPE_DEQUE:
        case TYPE_HEAP:
        case TYPE_STRING:
        case TYPE_BYTES:
        case TYPE_GENERATOR:
        case TYPE_PAIR:
        case TYPE_TUPLE:
            return IterableContract::Yes;

        case TYPE_UNION: {
            bool unknown = false;
            for (const TypeNode *member : type->args) {
                IterableContract item = iterable_contract_inner(member, state);
                if (item == IterableContract::No)
                    return IterableContract::No;
                if (item == IterableContract::Unknown)
                    unknown = true;
            }
            return unknown ? IterableContract::Unknown
                           : IterableContract::Yes;
        }

        case TYPE_NAMED:
            return iterable_contract_named(type, state);

        case TYPE_ANY:
        case TYPE_VAR:
            return IterableContract::Unknown;

        default:
            return IterableContract::No;
    }
}

ProtocolMatch iterable_protocol_match(
    const TypeNode *actual, const struct protocol_def *protocol,
    std::unordered_map<std::string, struct type_constraint> *bindings) {
    switch (actual->kind) {
        case TYPE_LIST:
        case TYPE_SET:
        case TYPE_DEQUE:
        case TYPE_HEAP:
        case TYPE_GENERATOR:
            bind_protocol_param(bindings, protocol, 0,
                                !actual->args.empty() ? actual->args[0]
                                                      : type_any());
            return ProtocolMatch::Yes;

        case TYPE_DICT: {
            const TypeNode *key =
                !actual->args.empty() ? actual->args[0] : type_any();
            const TypeNode *value =
                actual->args.size() > 1 ? actual->args[1] : type_any();
            bind_protocol_param(bindings, protocol, 0,
                                type_container(TYPE_PAIR, {key, value}));
            return ProtocolMatch::Yes;
        }

        case TYPE_STRING:
            bind_protocol_param(bindings, protocol, 0,
                                type_builtin(TYPE_CHAR));
            return ProtocolMatch::Yes;

        case TYPE_BYTES:
            bind_protocol_param(bindings, protocol, 0,
                                type_builtin(TYPE_BYTE));
            return ProtocolMatch::Yes;

        case TYPE_PAIR:
        case TYPE_TUPLE:
            bind_protocol_param(bindings, protocol, 0, type_any());
            return ProtocolMatch::Yes;

        default:
            break;
    }

    if (actual->kind != TYPE_NAMED)
        return ProtocolMatch::No;
    structtype_lookup_result lookup;
    if (!lookup_named_type(actual, lookup))
        return ProtocolMatch::Unknown;
    if (lookup.kind == STRUCTTYPE_LOOKUP_PROTOCOL)
        return ProtocolMatch::Unknown;
    const type_def *def =
        lookup.kind == STRUCTTYPE_LOOKUP_STRUCT
            ? static_cast<const type_def *>(lookup.structure)
            : static_cast<const type_def *>(lookup.enumeration);
    if (!def)
        return ProtocolMatch::Unknown;
    std::unordered_map<std::string, struct type_constraint> local;
    if (!structtype_satisfies_protocol(def, protocol, &local))
        return ProtocolMatch::No;
    if (bindings)
        *bindings = std::move(local);
    return ProtocolMatch::Yes;
}

} // namespace

IterableContract iterable_contract(const TypeNode *type) {
    IterableContractState state;
    return iterable_contract_inner(type, state);
}

ProtocolMatch match_protocol(
    const TypeNode *actual, const struct protocol_def *protocol,
    std::unordered_map<std::string, struct type_constraint> *bindings) {
    if (!actual || !protocol)
        return ProtocolMatch::No;
    if (actual->kind == TYPE_ANY || actual->kind == TYPE_VAR)
        return ProtocolMatch::Unknown;
    if (actual->kind == TYPE_UNION) {
        bool unknown = false;
        for (const TypeNode *member : actual->args) {
            ProtocolMatch item = match_protocol(member, protocol, nullptr);
            if (item == ProtocolMatch::No)
                return ProtocolMatch::No;
            if (item == ProtocolMatch::Unknown)
                unknown = true;
        }
        return unknown ? ProtocolMatch::Unknown : ProtocolMatch::Yes;
    }

    switch (protocol->builtin_kind) {
        case PROTOCOL_BUILTIN_HASHABLE:
            return key_contract(actual) == KeyContract::No
                       ? ProtocolMatch::No
                       : ProtocolMatch::Yes;
        case PROTOCOL_BUILTIN_ITERABLE:
            return iterable_protocol_match(actual, protocol, bindings);
        case PROTOCOL_BUILTIN_NONE:
            break;
    }

    if (actual->kind == TYPE_REF || actual->kind == TYPE_WEAK) {
        const TypeNode *inner = type_element(actual, 0);
        return inner ? match_protocol(inner, protocol, bindings)
                     : ProtocolMatch::Unknown;
    }
    if (actual->kind != TYPE_NAMED)
        return ProtocolMatch::No;
    structtype_lookup_result lookup;
    if (!lookup_named_type(actual, lookup))
        return ProtocolMatch::Unknown;
    if (lookup.kind == STRUCTTYPE_LOOKUP_PROTOCOL)
        return ProtocolMatch::Unknown;
    const type_def *def =
        lookup.kind == STRUCTTYPE_LOOKUP_STRUCT
            ? static_cast<const type_def *>(lookup.structure)
            : static_cast<const type_def *>(lookup.enumeration);
    if (!def)
        return ProtocolMatch::Unknown;
    std::unordered_map<std::string, struct type_constraint> local;
    if (!structtype_satisfies_protocol(def, protocol, &local))
        return ProtocolMatch::No;
    if (bindings)
        *bindings = std::move(local);
    return ProtocolMatch::Yes;
}

namespace {

thread_local int g_type_match_depth = 0;

struct TypeMatchDepthGuard {
    TypeMatchDepthGuard() { ++g_type_match_depth; }
    ~TypeMatchDepthGuard() { --g_type_match_depth; }
};

} // namespace

MatchResult match_type(const TypeNode *actual, const TypeNode *expected) {
    TypeMatchDepthGuard depth_guard;
    if (g_type_match_depth > 512)
        return MatchResult::Unknown;
    if (!expected)
        expected = type_any();
    if (!actual)
        actual = type_any();
    expected = normalize_nullable(expected);
    actual = normalize_nullable(actual);
    if (expected->kind == TYPE_ANY)
        return MatchResult::Ok;
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
    if (actual->kind == TYPE_REF && expected->kind == TYPE_NAMED)
        return match_type(type_element(actual, 0), expected);
    if (actual->kind == TYPE_REF && expected->kind == TYPE_WEAK) {
        /* A weak target may be initialized from a strong ref; runtime
         * converts the handle while keeping the target type. */
        const TypeNode *actual_inner = type_element(actual, 0);
        const TypeNode *expected_inner = type_element(expected, 0);
        if (actual_inner && expected_inner)
            return match_type(actual_inner, expected_inner);
        return MatchResult::Ok;
    }
    if (actual->kind == TYPE_REF && expected->kind == TYPE_REF &&
        !actual->args.empty() && !expected->args.empty()) {
        const TypeNode *actual_inner = type_element(actual, 0);
        const TypeNode *expected_inner = type_element(expected, 0);
        if (expected_inner && expected_inner->kind == TYPE_NAMED &&
            actual_inner && actual_inner->kind == TYPE_NAMED) {
            structtype_lookup_result expected_lookup;
            if (lookup_named_type(expected_inner, expected_lookup) &&
                expected_lookup.protocol) {
                structtype_lookup_result actual_lookup;
                if (!lookup_named_type(actual_inner, actual_lookup))
                    return MatchResult::Unknown;
                const type_def *actual_type =
                    actual_lookup.structure
                        ? static_cast<const type_def *>(actual_lookup.structure)
                        : static_cast<const type_def *>(
                              actual_lookup.enumeration);
                if (!actual_type)
                    return MatchResult::Unknown;
                return structtype_satisfies_protocol(actual_type,
                                                     expected_lookup.protocol)
                           ? MatchResult::Ok
                           : MatchResult::Mismatch;
            }
        }
    }
    if (expected->kind == TYPE_NAMED) {
        structtype_lookup_result expected_lookup;
        if (lookup_named_type(expected, expected_lookup) &&
            expected_lookup.protocol) {
            if (actual->kind == TYPE_NAMED &&
                structtype_named_types_equal(
                    actual->source.c_str(), actual->name.c_str(),
                    expected->source.c_str(), expected->name.c_str()))
                return MatchResult::Ok;
            std::unordered_map<std::string, struct type_constraint> bindings;
            ProtocolMatch matched = match_protocol(
                actual, expected_lookup.protocol,
                expected_lookup.protocol->builtin_kind !=
                        PROTOCOL_BUILTIN_NONE
                    ? &bindings
                    : nullptr);
            if (matched == ProtocolMatch::No)
                return MatchResult::Mismatch;
            if (matched == ProtocolMatch::Unknown)
                return MatchResult::Unknown;
            if (expected_lookup.protocol->builtin_kind !=
                    PROTOCOL_BUILTIN_NONE &&
                !expected->args.empty()) {
                if (expected->args.size() !=
                    expected_lookup.protocol->generic_params.size())
                    return MatchResult::Mismatch;
                MatchResult result = MatchResult::Ok;
                for (std::size_t i = 0; i < expected->args.size(); ++i) {
                    auto binding = bindings.find(
                        expected_lookup.protocol->generic_params[i]);
                    if (binding == bindings.end())
                        return MatchResult::Unknown;
                    const TypeNode *bound =
                        type_from_constraint(&binding->second);
                    result = combine_result(
                        result, match_type(bound, expected->args[i]));
                    if (result == MatchResult::Mismatch)
                        return result;
                }
                return result;
            }
            return MatchResult::Ok;
        }
    }
    if (actual->kind != expected->kind)
        return MatchResult::Mismatch;

    if (actual->kind == TYPE_NAMED) {
        if (structtype_named_types_equal(actual->source.c_str(),
                                         actual->name.c_str(),
                                         expected->source.c_str(),
                                         expected->name.c_str())) {
            if (actual->args.size() != expected->args.size())
                return MatchResult::Mismatch;
            for (std::size_t i = 0; i < actual->args.size(); ++i) {
                if (!type_equal(actual->args[i], expected->args[i]))
                    return MatchResult::Mismatch;
            }
            return MatchResult::Ok;
        }
        structtype_lookup_result expected_lookup;
        if (lookup_named_type(expected, expected_lookup) &&
            expected_lookup.protocol) {
            structtype_lookup_result actual_lookup;
            if (!lookup_named_type(actual, actual_lookup))
                return MatchResult::Unknown;
            const type_def *actual_type =
                actual_lookup.structure
                    ? static_cast<const type_def *>(actual_lookup.structure)
                    : static_cast<const type_def *>(actual_lookup.enumeration);
            if (!actual_type)
                return MatchResult::Unknown;
            if (structtype_satisfies_protocol(actual_type,
                                              expected_lookup.protocol))
                return MatchResult::Ok;
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
    if (type->kind == TYPE_NAMED) {
        return type_named_generic(type->source.c_str(),
                                  type->name.c_str(), args,
                                  type->nullable);
    }
    if (type->kind == TYPE_CALLABLE || type->kind == TYPE_FUNCTION ||
        type->kind == TYPE_BUILTIN) {
        return make_function_type(type->kind, args, type->optional_params,
                                  type->variadic, type->nullable, rest);
    }
    return type_container(type->kind, args, type->nullable);
}

bool bind_type(const TypeNode *actual, const TypeNode *pattern,
               TypeBindings &bindings) {
    TypeMatchDepthGuard depth_guard;
    if (g_type_match_depth > 512)
        return false;
    if (!pattern)
        return true;
    if (!actual)
        actual = type_any();
    pattern = normalize_nullable(pattern);
    actual = normalize_nullable(actual);

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

    if (pattern->kind == TYPE_UNION) {
        bool pattern_has_null = false;
        for (const TypeNode *member : pattern->args) {
            if (member && member->kind == TYPE_NULL)
                pattern_has_null = true;
        }
        if (actual->kind == TYPE_NULL)
            return pattern_has_null;
        if (actual->kind == TYPE_UNION) {
            if (pattern_has_null) {
                const TypeNode *actual_base = strip_null_members(actual);
                const TypeNode *pattern_base = strip_null_members(pattern);
                if (actual_base != actual || pattern_base != pattern)
                    return bind_type(actual_base, pattern_base, bindings);
            }
        } else {
            for (const TypeNode *member : pattern->args) {
                if (!member || member->kind == TYPE_NULL ||
                    member->kind == TYPE_ANY)
                    continue;
                TypeBindings trial = bindings;
                if (bind_type(actual, member, trial)) {
                    bindings = trial;
                    return true;
                }
            }
            return false;
        }
    }

    if (pattern->nullable) {
        if (actual->kind == TYPE_NULL)
            return true;
        const TypeNode *a = actual->nullable ? strip_nullable(actual) : actual;
        a = strip_null_members(a);
        return bind_type(a, strip_nullable(pattern), bindings);
    }
    if (actual->nullable || actual->kind == TYPE_NULL)
        return false;

    if (pattern->kind == TYPE_NAMED && actual->kind == TYPE_NAMED) {
        bool same_named = structtype_named_types_equal(
            actual->source.c_str(), actual->name.c_str(),
            pattern->source.c_str(), pattern->name.c_str());
        if (!same_named)
            return type_contains_vars(pattern)
                       ? false
                       : match_type(actual, pattern) !=
                             MatchResult::Mismatch;
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
