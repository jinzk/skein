#include "structtype.h"

#include "builtin_ids.h"
#include "dict.h"
#include "refvalue.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace {

std::unordered_map<std::string, std::unique_ptr<struct_def>> struct_types;
std::unordered_map<std::string, std::unique_ptr<protocol_def>> protocol_types;
std::unordered_map<std::string, std::unique_ptr<enum_def>> enum_types;
std::unordered_map<std::string, struct_def *> logical_structs;
std::unordered_map<std::string, protocol_def *> logical_protocols;
std::unordered_map<std::string, enum_def *> logical_enums;
std::unordered_map<std::string, std::string> source_aliases;
std::unordered_map<std::string, std::string> type_alias_names;

/*
 * Methods attached by a thread payload are thread-local overrides.  Keeping
 * them out of the shared type definition avoids mutating process-global method
 * tables after worker threads have started.
 */
thread_local std::unordered_map<
    const type_def *,
    std::unordered_map<std::string, std::shared_ptr<function_def>>>
    thread_methods;
thread_local uint64_t thread_method_generation = 0;

/* Registry mutations are rare and recursive; method lookups are hot. */
std::recursive_mutex type_registry_mutex;
std::shared_mutex type_method_mutex;

std::string type_key(const char *source, const char *name) {
    std::string key = source ? source : "";
    key.push_back('\x1f');
    key += name ? name : "";
    return key;
}

std::string alias_key(const char *importer, const char *alias) {
    return type_key(importer, alias);
}

struct resolved_type {
    struct_def *structure;
    protocol_def *protocol;
    enum_def *enumeration;
};

bool split_qualified_name(const char *name, std::string &prefix,
                          std::string &base) {
    if (!name) return false;
    std::string text(name);
    size_t sep = text.rfind("::");
    size_t sep_len = 2;
    size_t dot = text.rfind('.');
    if (dot != std::string::npos &&
        (sep == std::string::npos || dot > sep)) {
        sep = dot;
        sep_len = 1;
    }
    if (sep == std::string::npos || sep == 0 ||
        sep + sep_len >= text.size())
        return false;
    prefix = text.substr(0, sep);
    base = text.substr(sep + sep_len);
    return true;
}

std::string resolve_qualified_module(const char *importer,
                                      const std::string &prefix) {
    auto it = source_aliases.find(alias_key(importer, prefix.c_str()));
    if (it != source_aliases.end())
        return it->second;
    return prefix;
}

resolved_type resolve_named(const char *source, const char *name,
                            bool require_exported) {
    resolved_type result{NULL, NULL, NULL};
    if (!name) return result;

    auto sit = struct_types.find(type_key(source, name));
    if (sit != struct_types.end()) {
        result.structure = sit->second.get();
        return result;
    }
    auto tit = protocol_types.find(type_key(source, name));
    if (tit != protocol_types.end()) {
        result.protocol = tit->second.get();
        return result;
    }
    auto eit = enum_types.find(type_key(source, name));
    if (eit != enum_types.end()) {
        result.enumeration = eit->second.get();
        return result;
    }

    auto alias_it = source_aliases.find(alias_key(source, name));
    if (alias_it != source_aliases.end()) {
        const std::string &target = alias_it->second;
        std::string original = name;
        auto name_it = type_alias_names.find(alias_key(source, name));
        if (name_it != type_alias_names.end())
            original = name_it->second;
        auto lsit =
            logical_structs.find(type_key(target.c_str(), original.c_str()));
        if (lsit != logical_structs.end() &&
            (!require_exported || lsit->second->exported)) {
            result.structure = lsit->second;
            return result;
        }
        auto ltit =
            logical_protocols.find(type_key(target.c_str(), original.c_str()));
        if (ltit != logical_protocols.end()) {
            result.protocol = ltit->second;
            return result;
        }
        auto leit =
            logical_enums.find(type_key(target.c_str(), original.c_str()));
        if (leit != logical_enums.end() &&
            (!require_exported || leit->second->exported)) {
            result.enumeration = leit->second;
            return result;
        }
    }

    if (std::strcmp(name, "Callable") == 0 ||
        std::strcmp(name, "Hashable") == 0 ||
        std::strcmp(name, "Iterable") == 0 ||
        std::strcmp(name, "RpcClient") == 0 ||
        std::strcmp(name, "RpcJob") == 0 ||
        std::strcmp(name, "RpcServer") == 0) {
        auto prelude = logical_protocols.find(type_key("std.traits", name));
        if (prelude != logical_protocols.end() && prelude->second &&
            prelude->second->builtin_kind != PROTOCOL_BUILTIN_NONE) {
            result.protocol = prelude->second;
            return result;
        }
    }

    std::string prefix;
    std::string base;
    if (!split_qualified_name(name, prefix, base))
        return result;

    std::string target = resolve_qualified_module(source, prefix);
    auto lsit = logical_structs.find(type_key(target.c_str(), base.c_str()));
    if (lsit != logical_structs.end() &&
        (!require_exported || lsit->second->exported)) {
        result.structure = lsit->second;
        return result;
    }
    auto ltit = logical_protocols.find(type_key(target.c_str(), base.c_str()));
    /* Protocols are structural contracts and are always visible by package
     * path; the `export` modifier does not apply to them. */
    if (ltit != logical_protocols.end()) {
        result.protocol = ltit->second;
        return result;
    }
    auto leit = logical_enums.find(type_key(target.c_str(), base.c_str()));
    if (leit != logical_enums.end() &&
        (!require_exported || leit->second->exported)) {
        result.enumeration = leit->second;
    }
    return result;
}

bool constraint_is_hashable(const struct type_constraint &type,
                            std::unordered_set<const type_def *> &active,
                            bool &unknown);

bool type_def_is_hashable_inner(const type_def *def,
                                std::unordered_set<const type_def *> &active) {
    if (!def)
        return false;
    if (!active.insert(def).second)
        return false;

    std::shared_ptr<function_def> hash = structtype_method(def, "dict_hash");
    if (hash) {
        bool ok = hash->code && !hash->code->variadic &&
                  hash->code->nparams == 1 && hash->code->ret_type &&
                  hash->code->ret_type->kind == TYPE_INT &&
                  !hash->code->ret_type->nullable;
        if (ok) {
            std::shared_ptr<function_def> eq =
                structtype_method(def, "key_eq");
            if (eq) {
                ok = eq->code && !eq->code->variadic &&
                     eq->code->nparams == 2 && eq->code->ret_type &&
                     eq->code->ret_type->kind == TYPE_BOOL &&
                     !eq->code->ret_type->nullable;
            }
        }
        active.erase(def);
        return ok;
    }

    bool unknown = false;
    auto inspect_fields = [&](const std::vector<field_def> &fields) {
        for (const field_def &field : fields) {
            if (!constraint_is_hashable(field.type, active, unknown))
                return false;
        }
        return true;
    };

    bool result = true;
    if (def->kind == TYPE_OWNER_STRUCT) {
        const struct_def *structure = static_cast<const struct_def *>(def);
        result = inspect_fields(structure->fields);
    } else {
        const enum_def *enumeration = static_cast<const enum_def *>(def);
        for (const variant_def &variant : enumeration->variants) {
            if (!inspect_fields(variant.fields)) {
                result = false;
                break;
            }
        }
    }
    active.erase(def);
    return result;
}

bool constraint_is_hashable(const struct type_constraint &type,
                            std::unordered_set<const type_def *> &active,
                            bool &unknown) {
    switch (type.kind) {
        case TYPE_INT:
        case TYPE_FLOAT:
        case TYPE_NUMBER:
        case TYPE_BOOL:
        case TYPE_STRING:
        case TYPE_CHAR:
        case TYPE_BYTE:
        case TYPE_BYTES:
        case TYPE_NULL:
            return true;

        case TYPE_PAIR:
        case TYPE_TUPLE:
        case TYPE_UNION:
            /* Runtime constraints do not retain element/member details.
             * Treat them as conservatively hashable; the actual dict/set
             * value check still verifies nested mutable values. */
            unknown = true;
            return true;

        case TYPE_ANY:
        case TYPE_VAR:
            unknown = true;
            return true;

        case TYPE_NAMED: {
            resolved_type resolved =
                resolve_named(type.source, type.name, true);
            if (resolved.structure)
                return type_def_is_hashable_inner(resolved.structure, active);
            if (resolved.enumeration)
                return type_def_is_hashable_inner(resolved.enumeration, active);
            unknown = true;
            return true;
        }

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
            return false;
    }
    unknown = true;
    return true;
}

bool type_def_is_hashable(const type_def *def) {
    if (!def)
        return false;
    std::unordered_set<const type_def *> active;
    return type_def_is_hashable_inner(def, active);
}

bool builtin_name_is(const char *name,
                     std::initializer_list<const char *> candidates) {
    if (!name)
        return false;
    for (const char *candidate : candidates) {
        if (std::strcmp(name, candidate) == 0)
            return true;
    }
    return false;
}

static bool named_type_visible_impl(const char *source, const char *name) {
    if (!name) return false;
    std::string key = type_key(source, name);
    auto sit = struct_types.find(key);
    if (sit != struct_types.end())
        return true;
    auto tit = protocol_types.find(key);
    if (tit != protocol_types.end())
        return true;
    auto eit = enum_types.find(key);
    if (eit != enum_types.end())
        return true;
    resolved_type resolved = resolve_named(source, name, true);
    return resolved.structure != NULL || resolved.protocol != NULL ||
           resolved.enumeration != NULL;
}

} // namespace

bool named_type_visible(const char *source, const char *name) {
    return named_type_visible_impl(source, name);
}

namespace {

bool protocol_signature_type_visible(const char *source, const char *name) {
    if (!name)
        return false;
    std::string key = type_key(source, name);
    auto sit = struct_types.find(key);
    if (sit != struct_types.end())
        return sit->second->exported;
    auto tit = protocol_types.find(key);
    if (tit != protocol_types.end())
        return true;
    auto eit = enum_types.find(key);
    if (eit != enum_types.end())
        return eit->second->exported;
    resolved_type resolved = resolve_named(source, name, true);
    return resolved.structure != NULL || resolved.protocol != NULL ||
           resolved.enumeration != NULL;
}

} // namespace

struct_payload::~struct_payload() {
    for (size_t i = 0; i < fields.size(); ++i)
        freeval(fields[i]);
}

void structtype_shutdown() {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    std::unique_lock<std::shared_mutex> method_lock(type_method_mutex);
    struct_types.clear();
    protocol_types.clear();
    enum_types.clear();
    logical_structs.clear();
    logical_protocols.clear();
    logical_enums.clear();
    source_aliases.clear();
}

void structtype_visit_functions(
    const std::function<void(const std::shared_ptr<function_def> &)> &visit) {
    if (!visit)
        return;
    std::vector<std::shared_ptr<function_def>> functions;
    {
        std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
        std::shared_lock<std::shared_mutex> method_lock(type_method_mutex);
        for (auto &entry : struct_types) {
            struct_def *def = entry.second.get();
            for (auto &method : def->methods)
                if (method.second)
                    functions.push_back(method.second);
            for (const field_def &field : def->fields)
                if (field.default_value)
                    functions.push_back(field.default_value);
        }
        for (auto &entry : enum_types) {
            enum_def *def = entry.second.get();
            for (auto &method : def->methods)
                if (method.second)
                    functions.push_back(method.second);
            for (const variant_def &variant : def->variants) {
                for (const field_def &field : variant.fields)
                    if (field.default_value)
                        functions.push_back(field.default_value);
            }
        }
        for (auto &entry : protocol_types) {
            for (auto &method : entry.second->defaults)
                if (method.second)
                    functions.push_back(method.second);
        }
    }
    for (auto &entry : thread_methods) {
        for (auto &method : entry.second)
            if (method.second)
                functions.push_back(method.second);
    }
    for (const auto &function : functions)
        visit(function);
}

struct_def *structtype_find_struct(const char *source, const char *name) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!name) return NULL;
    auto it = struct_types.find(type_key(source, name));
    return it == struct_types.end() ? NULL : it->second.get();
}

struct_def *structtype_find_struct_by_name(const char *package,
                                           const char *name,
                                           bool *ambiguous) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (ambiguous) *ambiguous = false;
    if (!name) return NULL;
    if (package && *package) {
        auto it = logical_structs.find(type_key(package, name));
        return it == logical_structs.end() ? NULL : it->second;
    }
    struct_def *found = NULL;
    for (auto &entry : logical_structs) {
        if (!entry.second || entry.second->name != name)
            continue;
        if (found && found != entry.second) {
            if (ambiguous) *ambiguous = true;
            return NULL;
        }
        found = entry.second;
    }
    return found;
}

bool builtin_type_name_is_hashable(const char *name) {
    return builtin_name_is(name, {"int", "float", "number", "bool", "string",
                                  "char", "byte", "bytes", "null", "pair",
                                  "tuple"});
}

bool builtin_type_name_is_iterable(const char *name) {
    return builtin_name_is(name, {"list", "dict", "set", "deque", "heap",
                                  "string", "bytes", "generator", "pair",
                                  "tuple"});
}

bool builtin_type_name_is_callable(const char *name) {
    return builtin_name_is(name, {"function", "builtin", "callable"});
}

bool builtin_type_satisfies_protocol(const char *name,
                                     const protocol_def *protocol) {
    if (!protocol)
        return false;
    if (protocol->builtin_kind == PROTOCOL_BUILTIN_HASHABLE)
        return builtin_type_name_is_hashable(name);
    if (protocol->builtin_kind == PROTOCOL_BUILTIN_ITERABLE)
        return builtin_type_name_is_iterable(name);
    if (protocol->builtin_kind == PROTOCOL_BUILTIN_CALLABLE)
        return builtin_type_name_is_callable(name);
    return false;
}

std::vector<const protocol_def *> builtin_type_protocols(const char *name) {
    std::vector<const protocol_def *> result;
    if (builtin_type_name_is_iterable(name)) {
        protocol_def *protocol =
            structtype_find_protocol_by_name(nullptr, "Iterable");
        if (protocol && protocol->builtin_kind == PROTOCOL_BUILTIN_ITERABLE)
            result.push_back(protocol);
    }
    if (builtin_type_name_is_hashable(name)) {
        protocol_def *protocol =
            structtype_find_protocol_by_name(nullptr, "Hashable");
        if (protocol && protocol->builtin_kind == PROTOCOL_BUILTIN_HASHABLE)
            result.push_back(protocol);
    }
    if (builtin_type_name_is_callable(name)) {
        protocol_def *protocol =
            structtype_find_protocol_by_name(nullptr, "Callable");
        if (protocol && protocol->builtin_kind == PROTOCOL_BUILTIN_CALLABLE)
            result.push_back(protocol);
    }
    return result;
}

bool value_is_iterable(const struct value_t *value) {
    if (!value)
        return false;
    switch (value->type) {
        case T_list:
        case T_dict:
        case T_set:
        case T_deque:
        case T_heap:
        case T_string:
        case T_bytes:
            return true;
        case T_userdata: {
            const userdata_value_t *userdata =
                (const userdata_value_t *)value;
            return userdata->meta && userdata->meta->name &&
                   std::strcmp(userdata->meta->name, "generator") == 0;
        }
        default:
            return false;
    }
}

bool value_is_callable(const struct value_t *value) {
    if (!value)
        return false;
    return value->type == T_func || value->type == T_builtin ||
           value->type == T_method || value->type == T_native;
}

bool value_satisfies_builtin_protocol(const struct value_t *value,
                                      const protocol_def *protocol) {
    if (!value || !protocol)
        return false;
    if (protocol->builtin_kind == PROTOCOL_BUILTIN_HASHABLE)
        return dict_key_allowed(value);
    if (protocol->builtin_kind == PROTOCOL_BUILTIN_ITERABLE)
        return value_is_iterable(value);
    if (protocol->builtin_kind == PROTOCOL_BUILTIN_CALLABLE)
        return value_is_callable(value);
    if (protocol->builtin_kind == PROTOCOL_BUILTIN_RPC_CLIENT ||
        protocol->builtin_kind == PROTOCOL_BUILTIN_RPC_JOB ||
        protocol->builtin_kind == PROTOCOL_BUILTIN_RPC_SERVER) {
        if (value->type == T_struct) {
            const struct_value_t *sv = (const struct_value_t *)value;
            return sv->def &&
                   structtype_satisfies_protocol(sv->def, protocol);
        }
        if (value->type == T_enum) {
            const enum_value_t *ev = (const enum_value_t *)value;
            return ev->def &&
                   structtype_satisfies_protocol(ev->def, protocol);
        }
        if (value->type != T_userdata)
            return false;
        const userdata_value_t *userdata =
            (const userdata_value_t *)value;
        const char *type_name =
            userdata->meta ? userdata->meta->name : nullptr;
        if (!type_name)
            return false;
        if (protocol->builtin_kind == PROTOCOL_BUILTIN_RPC_CLIENT)
            return std::strcmp(type_name, "worker_pool") == 0;
        if (protocol->builtin_kind == PROTOCOL_BUILTIN_RPC_JOB)
            return std::strcmp(type_name, "worker_job") == 0;
        return false;
    }
    return false;
}

int builtin_iter_method_id(const struct value_t *value) {
    if (!value)
        return -1;
    if (value->type == T_dict)
        return B_iter_items;
    if (value_is_iterable(value))
        return B_iter_elements;
    return -1;
}

int builtin_length_method_id(const struct value_t *value) {
    if (!value)
        return -1;
    switch (value->type) {
        case T_char:
        case T_byte:
        case T_string:
        case T_bytes:
        case T_list:
        case T_dict:
        case T_set:
        case T_deque:
        case T_heap:
            return B_len;
        default:
            return -1;
    }
}

type_def *structtype_find_type(const char *source, const char *name) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!name) return NULL;
    auto sit = struct_types.find(type_key(source, name));
    if (sit != struct_types.end())
        return sit->second.get();
    auto eit = enum_types.find(type_key(source, name));
    if (eit != enum_types.end())
        return eit->second.get();
    return NULL;
}

protocol_def *structtype_find_protocol(const char *source, const char *name) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!name) return NULL;
    auto it = protocol_types.find(type_key(source, name));
    return it == protocol_types.end() ? NULL : it->second.get();
}

protocol_def *structtype_find_protocol_by_name(const char *package,
                                         const char *name,
                                         bool *ambiguous) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (ambiguous) *ambiguous = false;
    if (!name) return NULL;
    if (package && *package) {
        auto it = logical_protocols.find(type_key(package, name));
        return it == logical_protocols.end() ? NULL : it->second;
    }
    protocol_def *found = NULL;
    for (auto &entry : logical_protocols) {
        if (!entry.second || entry.second->name != name)
            continue;
        if (found && found != entry.second) {
            if (ambiguous) *ambiguous = true;
            return NULL;
        }
        found = entry.second;
    }
    return found;
}

protocol_def *structtype_resolve_protocol(const char *source, const char *name,
                                    bool require_exported,
                                    std::string &error) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    error.clear();
    if (!name) {
        error = "invalid proto name";
        return NULL;
    }

    protocol_def *local = structtype_find_protocol(source, name);
    if (local)
        return local;

    resolved_type resolved = resolve_named(source, name, require_exported);
    if (resolved.protocol)
        return resolved.protocol;

    resolved_type any = resolve_named(source, name, false);
    if (any.structure || any.enumeration) {
        error = std::string("type ") + name + " is not a proto";
        return NULL;
    }
    if (any.protocol && require_exported && !any.protocol->exported) {
        error = std::string("proto ") + name + " is not exported";
        return NULL;
    }
    error = std::string("unknown proto ") + name;
    return NULL;
}

enum_def *structtype_find_enum(const char *source, const char *name) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!name) return NULL;
    auto it = enum_types.find(type_key(source, name));
    return it == enum_types.end() ? NULL : it->second.get();
}

enum_def *structtype_find_enum_by_name(const char *package,
                                       const char *name,
                                       bool *ambiguous) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (ambiguous) *ambiguous = false;
    if (!name) return NULL;
    if (package && *package) {
        auto it = logical_enums.find(type_key(package, name));
        return it == logical_enums.end() ? NULL : it->second;
    }
    enum_def *found = NULL;
    for (auto &entry : logical_enums) {
        if (!entry.second || entry.second->name != name)
            continue;
        if (found && found != entry.second) {
            if (ambiguous) *ambiguous = true;
            return NULL;
        }
        found = entry.second;
    }
    return found;
}

bool structtype_is_exported_type(const char *package, const char *name) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!package || !*package || !name || !*name)
        return false;
    const std::string key = type_key(package, name);
    auto structure = logical_structs.find(key);
    if (structure != logical_structs.end() && structure->second &&
        structure->second->exported)
        return true;
    auto enumeration = logical_enums.find(key);
    if (enumeration != logical_enums.end() && enumeration->second &&
        enumeration->second->exported)
        return true;
    return logical_protocols.find(key) != logical_protocols.end();
}

static bool lookup_def_visible(const char *importer_source,
                               const char *importer_package,
                               const type_def *def) {
    if (!def)
        return false;
    if (def->exported)
        return true;
    if (importer_source && def->source == importer_source)
        return true;
    if (importer_package && def->package == importer_package)
        return true;
    return false;
}

static bool lookup_protocol_visible(const char *importer_source,
                                 const char *importer_package,
                                 const protocol_def *def) {
    /* Protocols are structural contracts and have no `export` modifier.  A
     * qualified reference through an imported package is always visible. */
    (void)importer_source;
    (void)importer_package;
    return def != NULL;
}

enum structtype_lookup_status structtype_lookup_type(
    const char *importer_source, const char *importer_package,
    const char *name, struct structtype_lookup_result &out,
    std::string &error) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    out = structtype_lookup_result();
    error.clear();
    if (!name || !*name) {
        error = "invalid type name";
        return STRUCTTYPE_LOOKUP_ERROR;
    }

    std::string text(name);
    std::string prefix;
    std::string base;
    if (split_qualified_name(name, prefix, base)) {
        std::string target = resolve_qualified_module(importer_source, prefix);
        struct_def *structure = NULL;
        enum_def *enumeration = NULL;
        protocol_def *protocol = NULL;
        auto sit = logical_structs.find(type_key(target.c_str(), base.c_str()));
        if (sit != logical_structs.end())
            structure = sit->second;
        auto eit = logical_enums.find(type_key(target.c_str(), base.c_str()));
        if (eit != logical_enums.end())
            enumeration = eit->second;
        auto tit = logical_protocols.find(type_key(target.c_str(), base.c_str()));
        if (tit != logical_protocols.end())
            protocol = tit->second;

        int found = (structure ? 1 : 0) + (enumeration ? 1 : 0) +
                    (protocol ? 1 : 0);
        if (found > 1) {
            error = "ambiguous type " + text +
                    "; use a package-qualified name";
            return STRUCTTYPE_LOOKUP_AMBIGUOUS;
        }
        if (found == 0) {
            error = "unknown type " + text;
            return STRUCTTYPE_LOOKUP_NOT_FOUND;
        }

        bool visible =
            structure ? lookup_def_visible(importer_source, importer_package,
                                           structure)
                      : enumeration
                            ? lookup_def_visible(importer_source,
                                                 importer_package,
                                                 enumeration)
                            : lookup_protocol_visible(importer_source,
                                                   importer_package, protocol);
        if (!visible) {
            error = "type " + text + " is not visible";
            return STRUCTTYPE_LOOKUP_NOT_VISIBLE;
        }

        if (structure) {
            out.kind = STRUCTTYPE_LOOKUP_STRUCT;
            out.structure = structure;
        } else if (enumeration) {
            out.kind = STRUCTTYPE_LOOKUP_ENUM;
            out.enumeration = enumeration;
        } else {
            out.kind = STRUCTTYPE_LOOKUP_PROTOCOL;
            out.protocol = protocol;
        }
        return STRUCTTYPE_LOOKUP_FOUND;
    }

    /* A type declared in the caller's own source is always visible. */
    if (importer_source && *importer_source) {
        struct_def *structure = structtype_find_struct(importer_source, name);
        enum_def *enumeration = structtype_find_enum(importer_source, name);
        protocol_def *protocol = structtype_find_protocol(importer_source, name);
        int found = (structure ? 1 : 0) + (enumeration ? 1 : 0) +
                    (protocol ? 1 : 0);
        if (found > 1) {
            error = "ambiguous type " + text;
            return STRUCTTYPE_LOOKUP_AMBIGUOUS;
        }
        if (found == 1) {
            if (structure) {
                out.kind = STRUCTTYPE_LOOKUP_STRUCT;
                out.structure = structure;
            } else if (enumeration) {
                out.kind = STRUCTTYPE_LOOKUP_ENUM;
                out.enumeration = enumeration;
            } else {
                out.kind = STRUCTTYPE_LOOKUP_PROTOCOL;
                out.protocol = protocol;
            }
            return STRUCTTYPE_LOOKUP_FOUND;
        }
    }

    /* Types in the caller's logical source are also local. */
    if (importer_package && *importer_package) {
        struct_def *structure =
            structtype_find_struct_by_name(importer_package, name);
        enum_def *enumeration =
            structtype_find_enum_by_name(importer_package, name);
        protocol_def *protocol =
            structtype_find_protocol_by_name(importer_package, name);
        int found = (structure ? 1 : 0) + (enumeration ? 1 : 0) +
                    (protocol ? 1 : 0);
        if (found > 1) {
            error = "ambiguous type " + text;
            return STRUCTTYPE_LOOKUP_AMBIGUOUS;
        }
        if (found == 1) {
            if (structure) {
                out.kind = STRUCTTYPE_LOOKUP_STRUCT;
                out.structure = structure;
            } else if (enumeration) {
                out.kind = STRUCTTYPE_LOOKUP_ENUM;
                out.enumeration = enumeration;
            } else {
                out.kind = STRUCTTYPE_LOOKUP_PROTOCOL;
                out.protocol = protocol;
            }
            return STRUCTTYPE_LOOKUP_FOUND;
        }
    }

    /* A selective import binds the local name directly to an exported type. */
    auto alias_it = source_aliases.find(alias_key(importer_source, name));
    if (alias_it != source_aliases.end()) {
        const std::string &target = alias_it->second;
        std::string original = name;
        auto name_it =
            type_alias_names.find(alias_key(importer_source, name));
        if (name_it != type_alias_names.end())
            original = name_it->second;
        struct_def *structure =
            structtype_find_struct(target.c_str(), original.c_str());
        if (!structure)
            structure = structtype_find_struct_by_name(target.c_str(),
                                                       original.c_str(), NULL);
        enum_def *enumeration =
            structtype_find_enum(target.c_str(), original.c_str());
        if (!enumeration)
            enumeration = structtype_find_enum_by_name(target.c_str(),
                                                       original.c_str(), NULL);
        protocol_def *protocol =
            structtype_find_protocol(target.c_str(), original.c_str());
        if (!protocol)
            protocol = structtype_find_protocol_by_name(target.c_str(),
                                                        original.c_str(), NULL);
        int found = (structure ? 1 : 0) + (enumeration ? 1 : 0) +
                    (protocol ? 1 : 0);
        if (found > 1) {
            error = "ambiguous type " + text +
                    "; use a package-qualified name";
            return STRUCTTYPE_LOOKUP_AMBIGUOUS;
        }
        if (found == 1) {
            bool visible =
                structure
                    ? lookup_def_visible(importer_source, importer_package,
                                         structure)
                    : enumeration
                          ? lookup_def_visible(importer_source,
                                               importer_package, enumeration)
                          : lookup_protocol_visible(importer_source,
                                                    importer_package, protocol);
            if (!visible) {
                error = "type " + text + " is not visible";
                return STRUCTTYPE_LOOKUP_NOT_VISIBLE;
            }
            if (structure) {
                out.kind = STRUCTTYPE_LOOKUP_STRUCT;
                out.structure = structure;
            } else if (enumeration) {
                out.kind = STRUCTTYPE_LOOKUP_ENUM;
                out.enumeration = enumeration;
            } else {
                out.kind = STRUCTTYPE_LOOKUP_PROTOCOL;
                out.protocol = protocol;
            }
            return STRUCTTYPE_LOOKUP_FOUND;
        }
    }

    /* Finally, an unqualified name may refer to one visible exported type. */
    struct_def *structure = NULL;
    enum_def *enumeration = NULL;
    protocol_def *protocol = NULL;
    int found = 0;
    for (auto &entry : logical_structs) {
        struct_def *candidate = entry.second;
        if (!candidate || candidate->name != name || !candidate->exported)
            continue;
        if (structure && structure != candidate) {
            error = "ambiguous type " + text +
                    "; use a package-qualified name";
            return STRUCTTYPE_LOOKUP_AMBIGUOUS;
        }
        structure = candidate;
        ++found;
    }
    for (auto &entry : logical_enums) {
        enum_def *candidate = entry.second;
        if (!candidate || candidate->name != name || !candidate->exported)
            continue;
        if (enumeration && enumeration != candidate) {
            error = "ambiguous type " + text +
                    "; use a package-qualified name";
            return STRUCTTYPE_LOOKUP_AMBIGUOUS;
        }
        enumeration = candidate;
        ++found;
    }
    for (auto &entry : logical_protocols) {
        protocol_def *candidate = entry.second;
        if (!candidate || candidate->name != name)
            continue;
        if (protocol && protocol != candidate) {
            error = "ambiguous type " + text +
                    "; use a package-qualified name";
            return STRUCTTYPE_LOOKUP_AMBIGUOUS;
        }
        protocol = candidate;
        ++found;
    }

    if (found > 1) {
        error = "ambiguous type " + text +
                "; use a package-qualified name";
        return STRUCTTYPE_LOOKUP_AMBIGUOUS;
    }
    if (found == 0) {
        error = "unknown type " + text;
        return STRUCTTYPE_LOOKUP_NOT_FOUND;
    }

    if (structure) {
        out.kind = STRUCTTYPE_LOOKUP_STRUCT;
        out.structure = structure;
    } else if (enumeration) {
        out.kind = STRUCTTYPE_LOOKUP_ENUM;
        out.enumeration = enumeration;
    } else {
        out.kind = STRUCTTYPE_LOOKUP_PROTOCOL;
        out.protocol = protocol;
    }
    return STRUCTTYPE_LOOKUP_FOUND;
}

bool structtype_type_exists(const char *source, const char *name) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!name) return false;
    std::string key = type_key(source, name);
    if (struct_types.count(key) != 0 || protocol_types.count(key) != 0 ||
        enum_types.count(key) != 0)
        return true;
    resolved_type resolved = resolve_named(source, name, true);
    return resolved.structure != NULL || resolved.protocol != NULL ||
           resolved.enumeration != NULL;
}

bool structtype_prepare_struct(const char *source, const char *package,
                               const char *module, const char *name, bool exported,
                               struct_def *&out, std::string &error) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    out = NULL;
    if (!name || !*name) {
        error = "invalid struct name";
        return false;
    }
    if (structtype_type_exists(source, name)) {
        error = std::string("type repeated ") + name;
        return false;
    }
    std::string logical_key = type_key(package, name);
    if (logical_structs.count(logical_key) != 0 ||
        logical_protocols.count(logical_key) != 0 ||
        logical_enums.count(logical_key) != 0) {
        error = std::string("type repeated ") + name +
                " in package " + (package ? package : "");
        return false;
    }

    std::unique_ptr<struct_def> def(new struct_def);
    def->kind = TYPE_OWNER_STRUCT;
    def->name = name;
    def->source = source ? source : "";
    def->package = package ? package : def->source;
    def->module = module ? module : "";
    def->exported = exported;
    out = def.get();
    logical_structs.emplace(logical_key, out);
    struct_types.emplace(type_key(source, name), std::move(def));
    return true;
}

void structtype_set_generic_params(
    type_def *def, std::vector<std::string> params,
    std::vector<std::string> defaults) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!def)
        return;
    def->generic_params = std::move(params);
    def->generic_defaults = std::move(defaults);
    if (def->generic_defaults.size() != def->generic_params.size())
        def->generic_defaults.resize(def->generic_params.size());
}

bool structtype_finalize_struct(struct_def *def,
                                std::vector<field_def> fields,
                                std::string &error) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!def) {
        error = "invalid struct";
        return false;
    }
    std::unordered_map<std::string, bool> seen;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name.empty()) {
            error = "invalid field name";
            return false;
        }
        if (!seen.emplace(fields[i].name, true).second) {
            error = "field repeated " + fields[i].name;
            return false;
        }
    }
    def->fields = std::move(fields);
    def->field_index.clear();
    def->field_index.reserve(def->fields.size());
    for (size_t i = 0; i < def->fields.size(); ++i)
        def->field_index.emplace(def->fields[i].name, (int)i);
    return true;
}

void structtype_rollback_struct(struct_def *def) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!def)
        return;
    for (auto it = logical_structs.begin(); it != logical_structs.end(); ++it) {
        if (it->second == def) {
            logical_structs.erase(it);
            break;
        }
    }
    for (auto it = struct_types.begin(); it != struct_types.end(); ++it) {
        if (it->second.get() == def) {
            struct_types.erase(it);
            break;
        }
    }
}

bool structtype_create_struct(const char *source, const char *package,
                              const char *module, const char *name, bool exported,
                              std::vector<field_def> fields,
                              struct_def *&out, std::string &error) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!structtype_prepare_struct(source, package, module, name, exported, out,
                                   error))
        return false;
    if (!structtype_finalize_struct(out, std::move(fields), error)) {
        structtype_rollback_struct(out);
        out = NULL;
        return false;
    }
    return true;
}

bool structtype_create_protocol(const char *source, const char *package,
                             const char *module, const char *name, bool exported,
                             std::vector<method_sig> methods,
                             protocol_def *&out, std::string &error,
                             std::vector<std::string> generic_params) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    out = NULL;
    if (!name || !*name) {
        error = "invalid proto name";
        return false;
    }
    const bool std_traits_source =
        (package && std::strcmp(package, "std.traits") == 0) ||
        (source && (std::strcmp(source, "<std.traits>") == 0 ||
                    std::strcmp(source, "std.traits") == 0));
    if (std_traits_source &&
        (std::strcmp(name, "Callable") == 0 ||
         std::strcmp(name, "Hashable") == 0 ||
         std::strcmp(name, "Iterable") == 0 ||
         std::strcmp(name, "RpcClient") == 0 ||
         std::strcmp(name, "RpcJob") == 0 ||
         std::strcmp(name, "RpcServer") == 0)) {
        if (protocol_def *existing =
                structtype_find_protocol_by_name("std.traits", name)) {
            out = existing;
            return true;
        }
    }
    if (structtype_type_exists(source, name)) {
        error = std::string("type repeated ") + name;
        return false;
    }
    std::string logical_key = type_key(package, name);
    if (logical_structs.count(logical_key) != 0 ||
        logical_protocols.count(logical_key) != 0 ||
        logical_enums.count(logical_key) != 0) {
        error = std::string("type repeated ") + name +
                " in package " + (package ? package : "");
        return false;
    }

    std::unordered_map<std::string, bool> seen;
    for (size_t i = 0; i < methods.size(); ++i) {
        if (methods[i].name.empty()) {
            error = "invalid proto method name";
            return false;
        }
        if (!seen.emplace(methods[i].name, true).second) {
            error = "proto method repeated " + methods[i].name;
            return false;
        }
        for (size_t j = 0; j < methods[i].params.size(); ++j) {
            const type_constraint &param = methods[i].params[j];
            if (param.kind != TYPE_NAMED)
                continue;
            if (!protocol_signature_type_visible(source, param.name)) {
                error = "exported proto " + std::string(name) +
                        " method " + methods[i].name +
                        " uses non-exported type " +
                        (param.name ? param.name : "invalid");
                return false;
            }
        }
        const type_constraint *result = methods[i].return_type.get();
        if (result && result->kind == TYPE_NAMED &&
            !protocol_signature_type_visible(source, result->name)) {
            error = "exported proto " + std::string(name) +
                    " method " + methods[i].name +
                    " uses non-exported return type " +
                    (result->name ? result->name : "invalid");
            return false;
        }
    }

    std::unordered_set<std::string> seen_params;
    for (const std::string &param : generic_params) {
        if (param.empty()) {
            error = "invalid proto type parameter";
            return false;
        }
        if (!seen_params.insert(param).second) {
            error = "proto type parameter repeated " + param;
            return false;
        }
    }

    std::unique_ptr<protocol_def> def(new protocol_def);
    def->name = name;
    def->source = source ? source : "";
    def->package = package ? package : def->source;
    def->module = module ? module : "";
    def->exported = exported;
    def->generic_params = std::move(generic_params);
    def->methods = std::move(methods);
    const bool std_traits =
        (def->package == "std.traits") ||
        (def->source == "<std.traits>") || (def->source == "std.traits");
    if (std_traits) {
        if (def->name == "Hashable")
            def->builtin_kind = PROTOCOL_BUILTIN_HASHABLE;
        else if (def->name == "Iterable")
            def->builtin_kind = PROTOCOL_BUILTIN_ITERABLE;
        else if (def->name == "Callable")
            def->builtin_kind = PROTOCOL_BUILTIN_CALLABLE;
        else if (def->name == "RpcClient")
            def->builtin_kind = PROTOCOL_BUILTIN_RPC_CLIENT;
        else if (def->name == "RpcJob")
            def->builtin_kind = PROTOCOL_BUILTIN_RPC_JOB;
        else if (def->name == "RpcServer")
            def->builtin_kind = PROTOCOL_BUILTIN_RPC_SERVER;
    }
    out = def.get();
    logical_protocols.emplace(logical_key, out);
    protocol_types.emplace(type_key(source, name), std::move(def));
    return true;
}

void structtype_install_builtin_protocols() {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    auto install = [](const char *name,
                      std::vector<method_sig> methods,
                      std::vector<std::string> generic_params) {
        if (structtype_find_protocol_by_name("std.traits", name))
            return;
        protocol_def *out = NULL;
        std::string error;
        structtype_create_protocol("<std.traits>", "std.traits", "", name,
                                   true, std::move(methods), out, error,
                                   std::move(generic_params));
    };

    install("Hashable", {}, {});
    install("Callable", {}, {});

    auto constraint = [](type_constraint_kind kind) {
        type_constraint c{};
        c.kind = kind;
        c.name = NULL;
        c.source = NULL;
        c.nullable = false;
        return c;
    };
    auto method = [&](const char *name,
                      std::vector<type_constraint> params,
                      type_constraint_kind result_kind,
                      bool has_result) {
        method_sig sig;
        sig.name = name;
        sig.params = std::move(params);
        sig.required_params = (int)sig.params.size();
        if (has_result)
            sig.return_type = std::make_shared<const type_constraint>(
                constraint(result_kind));
        return sig;
    };
    install("RpcJob",
            {method("wait",
                    {constraint(TYPE_ANY), constraint(TYPE_INT)},
                    TYPE_ANY, false)},
            {});
    install("RpcClient",
            {method("call",
                    {constraint(TYPE_ANY), constraint(TYPE_STRING),
                     constraint(TYPE_LIST)},
                    TYPE_ANY, false),
             method("submit",
                    {constraint(TYPE_ANY), constraint(TYPE_STRING),
                     constraint(TYPE_LIST)},
                    TYPE_ANY, false)},
            {});
    install("RpcServer",
            {method("serve", {constraint(TYPE_ANY)}, TYPE_INT, true)},
            {});

    method_sig iter;
    iter.name = "iter";
    type_constraint self;
    self.kind = TYPE_ANY;
    iter.params.push_back(self);
    iter.required_params = 1;
    type_constraint result;
    result.kind = TYPE_GENERATOR;
    iter.return_type = std::make_shared<const type_constraint>(result);
    install("Iterable", {iter}, {"T"});
}

bool structtype_prepare_enum(const char *source, const char *package,
                             const char *module, const char *name, bool exported,
                             enum_def *&out, std::string &error) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    out = NULL;
    if (!name || !*name) {
        error = "invalid enum name";
        return false;
    }
    if (structtype_type_exists(source, name)) {
        error = std::string("type repeated ") + name;
        return false;
    }
    std::string logical_key = type_key(package, name);
    if (logical_structs.count(logical_key) != 0 ||
        logical_protocols.count(logical_key) != 0 ||
        logical_enums.count(logical_key) != 0) {
        error = std::string("type repeated ") + name +
                " in package " + (package ? package : "");
        return false;
    }

    std::unique_ptr<enum_def> def(new enum_def);
    def->kind = TYPE_OWNER_ENUM;
    def->name = name;
    def->source = source ? source : "";
    def->package = package ? package : def->source;
    def->module = module ? module : "";
    def->exported = exported;
    out = def.get();
    logical_enums.emplace(logical_key, out);
    enum_types.emplace(type_key(source, name), std::move(def));
    return true;
}

bool structtype_finalize_enum(enum_def *def, std::vector<variant_def> variants,
                              std::string &error) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!def) {
        error = "invalid enum";
        return false;
    }
    std::unordered_map<std::string, bool> seen_variants;
    for (size_t i = 0; i < variants.size(); ++i) {
        if (variants[i].name.empty()) {
            error = "invalid enum variant name";
            return false;
        }
        if (!seen_variants.emplace(variants[i].name, true).second) {
            error = "enum variant repeated " + variants[i].name;
            return false;
        }
        std::unordered_map<std::string, bool> seen_fields;
        for (size_t j = 0; j < variants[i].fields.size(); ++j) {
            const std::string &field = variants[i].fields[j].name;
            if (field.empty()) {
                error = "invalid field name in variant " + variants[i].name;
                return false;
            }
            if (!seen_fields.emplace(field, true).second) {
                error = "field repeated " + field + " in variant " +
                        variants[i].name;
                return false;
            }
        }
    }
    def->variants = std::move(variants);
    def->variant_index.clear();
    def->variant_index.reserve(def->variants.size());
    for (size_t i = 0; i < def->variants.size(); ++i) {
        variant_def &variant = def->variants[i];
        def->variant_index.emplace(variant.name, (int)i);
        variant.field_index.clear();
        variant.field_index.reserve(variant.fields.size());
        for (size_t j = 0; j < variant.fields.size(); ++j)
            variant.field_index.emplace(variant.fields[j].name, (int)j);
    }
    return true;
}

void structtype_rollback_enum(enum_def *def) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!def)
        return;
    for (auto it = logical_enums.begin(); it != logical_enums.end(); ++it) {
        if (it->second == def) {
            logical_enums.erase(it);
            break;
        }
    }
    for (auto it = enum_types.begin(); it != enum_types.end(); ++it) {
        if (it->second.get() == def) {
            enum_types.erase(it);
            break;
        }
    }
}

bool structtype_create_enum(const char *source, const char *package,
                            const char *module, const char *name, bool exported,
                            std::vector<variant_def> variants,
                            enum_def *&out, std::string &error) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!structtype_prepare_enum(source, package, module, name, exported, out,
                                 error))
        return false;
    if (!structtype_finalize_enum(out, std::move(variants), error)) {
        structtype_rollback_enum(out);
        out = NULL;
        return false;
    }
    return true;
}

bool structtype_add_method(type_def *def, const std::string &name,
                           std::shared_ptr<function_def> code,
                           std::string &error) {
    std::lock_guard<std::recursive_mutex> registry_lock(type_registry_mutex);
    std::unique_lock<std::shared_mutex> method_lock(type_method_mutex);
    if (!def || name.empty() || !code) {
        error = "invalid method";
        return false;
    }
    if (def->kind == TYPE_OWNER_STRUCT) {
        struct_def *structure = static_cast<struct_def *>(def);
        if (structtype_field_index(structure, name) >= 0) {
            error = "field and method have the same name " + name;
            return false;
        }
    } else if (def->kind == TYPE_OWNER_ENUM) {
        enum_def *enumeration = static_cast<enum_def *>(def);
        for (size_t i = 0; i < enumeration->variants.size(); ++i) {
            const variant_def &variant = enumeration->variants[i];
            if (structtype_variant_field_index(&variant, name) >= 0) {
                error = "field and method have the same name " + name +
                        " in " + def->name + "." + variant.name;
                return false;
            }
        }
    } else {
        error = "invalid type owner";
        return false;
    }
    if (def->methods.count(name) != 0) {
        /*
         * A later explicit impl may replace a method that was supplied by a
         * protocol default.  Explicit duplicates are still rejected.
         */
        if (def->default_methods.erase(name) != 0) {
            def->methods[name] = std::move(code);
            def->explicit_methods.insert(name);
            return true;
        }
        error = "method repeated " + name;
        return false;
    }
    def->methods.emplace(name, std::move(code));
    def->explicit_methods.insert(name);
    return true;
}

bool structtype_set_default_method(type_def *def, const std::string &name,
                                   std::shared_ptr<function_def> code,
                                   const protocol_def *protocol,
                                   std::string &error) {
    std::lock_guard<std::recursive_mutex> registry_lock(type_registry_mutex);
    std::unique_lock<std::shared_mutex> method_lock(type_method_mutex);
    if (!def || !code || !protocol || name.empty()) {
        error = "invalid proto default method";
        return false;
    }
    if (def->explicit_methods.count(name) != 0)
        return true;
    if (def->methods.count(name) != 0)
        return true;
    def->methods.emplace(name, std::move(code));
    def->default_methods.emplace(name, protocol);
    return true;
}

std::shared_ptr<function_def> structtype_method(const type_def *def,
                                                const std::string &name) {
    std::shared_lock<std::shared_mutex> lock(type_method_mutex);
    if (!def)
        return NULL;
    auto type = thread_methods.find(def);
    if (type != thread_methods.end()) {
        auto method = type->second.find(name);
        if (method != type->second.end())
            return method->second;
    }
    auto method = def->methods.find(name);
    return method == def->methods.end() ? NULL : method->second;
}

bool structtype_is_constructor(const function_def *method) {
    return method && method->code && !method->code->is_method;
}

bool structtype_find_iter_method(const type_def *def,
                                 std::shared_ptr<function_def> &out) {
    if (!def)
        return false;
    std::shared_ptr<function_def> method = structtype_method(def, "iter");
    if (!method || !method->code)
        return false;
    const function_code *code = method->code.get();
    if (code->variadic || code->nparams != 1 || !code->ret_type)
        return false;
    if (code->ret_type->kind != TYPE_GENERATOR)
        return false;
    out = std::move(method);
    return true;
}

std::shared_ptr<function_def> structtype_constructor(const type_def *def) {
    if (!def || def->kind != TYPE_OWNER_STRUCT)
        return NULL;
    std::shared_ptr<function_def> method = structtype_method(def, "new");
    return structtype_is_constructor(method.get()) ? method : NULL;
}

struct value_t *structtype_constructor_value(const type_def *def) {
    std::shared_ptr<function_def> method = structtype_constructor(def);
    if (!method || !method->code)
        return NULL;
    std::vector<std::shared_ptr<struct upvalue_cell>> upvalues =
        method->upvalues;
    return newfuncval(method->code, method->closure, std::move(upvalues));
}

void structtype_set_thread_method(const type_def *def, const std::string &name,
                                  std::shared_ptr<function_def> method) {
    if (!def || !method)
        return;
    thread_methods[def][name] = std::move(method);
    ++thread_method_generation;
}

uint64_t structtype_method_generation() {
    return thread_method_generation;
}

std::shared_ptr<function_def> structtype_protocol_default(
    const protocol_def *protocol, const std::string &name) {
    std::shared_lock<std::shared_mutex> lock(type_method_mutex);
    if (!protocol)
        return NULL;
    auto method = protocol->defaults.find(name);
    return method == protocol->defaults.end() ? NULL : method->second;
}

std::vector<std::string> structtype_method_names(const type_def *def) {
    std::shared_lock<std::shared_mutex> lock(type_method_mutex);
    std::vector<std::string> names;
    if (!def)
        return names;
    names.reserve(def->methods.size());
    for (const auto &method : def->methods)
        if (method.second)
            names.push_back(method.first);
    auto overrides = thread_methods.find(def);
    if (overrides != thread_methods.end()) {
        for (const auto &method : overrides->second) {
            if (!method.second)
                continue;
            if (std::find(names.begin(), names.end(), method.first) ==
                names.end())
                names.push_back(method.first);
        }
    }
    return names;
}

bool structtype_add_protocol_default(protocol_def *protocol, const std::string &name,
                                  std::shared_ptr<function_def> code,
                                  std::string &error) {
    std::lock_guard<std::recursive_mutex> registry_lock(type_registry_mutex);
    std::unique_lock<std::shared_mutex> method_lock(type_method_mutex);
    if (!protocol || name.empty() || !code || !code->code) {
        error = "invalid proto default method";
        return false;
    }
    if (protocol->defaults.count(name) != 0) {
        error = "proto default method repeated " + name;
        return false;
    }
    code->code->is_method = true;
    code->code->name = name;
    protocol->defaults.emplace(name, std::move(code));
    return true;
}

namespace {

struct type_constraint any_type() {
    struct type_constraint type;
    type.kind = TYPE_ANY;
    type.name = NULL;
    type.source = NULL;
    type.nullable = false;
    type.arity = 0;
    type.display = NULL;
    type.detail = NULL;
    return type;
}

bool constraint_equal(const struct type_constraint *a,
                      const struct type_constraint *b) {
    if (!a || !b)
        return !a && !b;
    if (a->kind == TYPE_ANY && b->kind == TYPE_ANY)
        return true;
    return type_constraint_equal(a, b);
}

bool protocol_constraint_matches(
    const struct type_constraint &expected,
    const struct type_constraint &actual,
    const std::vector<std::string> &generic_params,
    std::unordered_map<std::string, struct type_constraint> &bindings) {
    if (expected.kind == TYPE_VAR && expected.name &&
        std::find(generic_params.begin(), generic_params.end(),
                  expected.name) != generic_params.end()) {
        auto it = bindings.find(expected.name);
        if (it == bindings.end()) {
            bindings.emplace(expected.name, actual);
            return true;
        }
        return constraint_equal(&it->second, &actual);
    }
    return constraint_equal(&expected, &actual);
}

bool protocol_method_signature_matches(
    const method_sig &sig, const struct function_code *code,
    const std::vector<std::string> &generic_params,
    std::unordered_map<std::string, struct type_constraint> &bindings,
    std::string &error) {
    if (!code) {
        error = "invalid method signature";
        return false;
    }
    if (sig.variadic || code->variadic) {
        error = "proto methods cannot be variadic";
        return false;
    }
    if (sig.params.empty() ||
        (size_t)code->nparams != sig.params.size()) {
        error = "method " + sig.name + " has the wrong number of parameters";
        return false;
    }

    int index = 0;
    for (struct symlist *param = code->sl; param;
         param = param->next, ++index) {
        if (index >= (int)sig.params.size())
            return false;
        const type_constraint &expected = sig.params[(size_t)index];
        const type_constraint actual =
            param->type ? *param->type : any_type();
        if (index == 0) {
            /* The receiver is always the implementing type and stays untyped. */
            if (param->sym && param->sym->name &&
                std::strcmp(param->sym->name, "self") != 0)
                return false;
            if (expected.kind != TYPE_ANY || actual.kind != TYPE_ANY)
                return false;
            continue;
        }
        if (!protocol_constraint_matches(expected, actual, generic_params,
                                         bindings)) {
            error = "method " + sig.name +
                    " does not match the proto signature";
            return false;
        }
    }
    if (index != (int)sig.params.size()) {
        error = "method " + sig.name + " has the wrong number of parameters";
        return false;
    }

    if (sig.return_type) {
        if (!code->ret_type ||
            !protocol_constraint_matches(*sig.return_type, *code->ret_type,
                                         generic_params, bindings)) {
            error = "method " + sig.name +
                    " does not match the proto return type";
            return false;
        }
    }
    return true;
}

} // namespace

bool structtype_method_signature_matches(const method_sig &sig,
                                          const struct function_code *code,
                                          std::string &error) {
    std::unordered_map<std::string, struct type_constraint> bindings;
    return protocol_method_signature_matches(sig, code, {}, bindings, error);
}

bool structtype_method_signature_matches(
    const method_sig &sig, const struct function_code *code,
    const std::vector<std::string> &generic_params, std::string &error) {
    std::unordered_map<std::string, struct type_constraint> bindings;
    return protocol_method_signature_matches(sig, code, generic_params,
                                             bindings, error);
}

bool structtype_satisfies_protocol(
    const type_def *def, const protocol_def *protocol,
    std::unordered_map<std::string, struct type_constraint> *bindings_out) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!def || !protocol)
        return false;
    if (protocol->builtin_kind == PROTOCOL_BUILTIN_HASHABLE) {
        if (bindings_out)
            bindings_out->clear();
        return type_def_is_hashable(def);
    }
    if (protocol->builtin_kind == PROTOCOL_BUILTIN_CALLABLE) {
        if (bindings_out)
            bindings_out->clear();
        return false;
    }
    std::unordered_map<std::string, struct type_constraint> bindings;
    for (const method_sig &sig : protocol->methods) {
        bool is_default = def->default_methods.count(sig.name) != 0;
        std::shared_ptr<function_def> method;
        if (!is_default) {
            method = structtype_method(def, sig.name);
            if (!method) {
                method = structtype_protocol_default(protocol, sig.name);
                if (!method)
                    return false;
                is_default = true;
            }
        } else {
            method = structtype_method(def, sig.name);
        }
        if (!method || !method->code)
            return false;
        if (is_default)
            continue;
        std::string error;
        if (!protocol_method_signature_matches(sig, method->code.get(),
                                               protocol->generic_params,
                                               bindings, error))
            return false;
    }
    if (bindings_out)
        *bindings_out = bindings;
    return true;
}

bool structtype_satisfies_protocol(const type_def *def,
                                   const protocol_def *protocol) {
    return structtype_satisfies_protocol(def, protocol, nullptr);
}

std::vector<const protocol_def *> structtype_protocols(const type_def *def) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    std::vector<const protocol_def *> found;
    if (!def)
        return found;
    for (auto &entry : protocol_types) {
        const protocol_def *protocol = entry.second.get();
        if (structtype_satisfies_protocol(def, protocol))
            found.push_back(protocol);
    }
    return found;
}

bool structtype_refresh_protocol_defaults(type_def *def, std::string &error) {
    if (!def) {
        error = "invalid type for proto defaults";
        return false;
    }
    error.clear();

    /* Defaults can make a type satisfy another protocol, so iterate. */
    for (;;) {
        std::vector<const protocol_def *> candidates;
        {
            std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
            for (auto &entry : protocol_types) {
                const protocol_def *protocol = entry.second.get();
                if (structtype_satisfies_protocol(def, protocol))
                    candidates.push_back(protocol);
            }
        }

        std::unordered_map<std::string, std::vector<const protocol_def *>>
            providers;
        for (const protocol_def *protocol : candidates) {
            for (const method_sig &sig : protocol->methods) {
                if (protocol->defaults.count(sig.name) != 0)
                    providers[sig.name].push_back(protocol);
            }
        }

        bool changed = false;
        std::lock_guard<std::recursive_mutex> registry_lock(type_registry_mutex);
        std::unique_lock<std::shared_mutex> method_lock(type_method_mutex);
        for (auto &entry : providers) {
            const std::string &name = entry.first;
            if (def->explicit_methods.count(name) != 0)
                continue;
            if (entry.second.size() > 1) {
                error = "proto " + entry.second.front()->name +
                        " default method " + name +
                        " conflicts with another proto default; implement "
                        "it explicitly";
                return false;
            }
            if (def->methods.count(name) != 0)
                continue;
            const protocol_def *provider = entry.second.front();
            auto fallback = provider->defaults.find(name);
            if (fallback == provider->defaults.end() || !fallback->second)
                continue;
            def->methods.emplace(name, fallback->second);
            def->default_methods.emplace(name, provider);
            changed = true;
        }
        if (!changed)
            return true;
    }
}

bool structtype_refresh_all_protocol_defaults(std::string &error) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    for (auto &entry : struct_types) {
        if (!structtype_refresh_protocol_defaults(entry.second.get(), error))
            return false;
    }
    for (auto &entry : enum_types) {
        if (!structtype_refresh_protocol_defaults(entry.second.get(), error))
            return false;
    }
    return true;
}

bool structtype_has_protocol(const type_def *def, const protocol_def *protocol) {
    return structtype_satisfies_protocol(def, protocol);
}

int structtype_field_index(const struct_def *def, const std::string &name) {
    if (!def) return -1;
    if (def->field_index.size() == def->fields.size()) {
        auto it = def->field_index.find(name);
        return it == def->field_index.end() ? -1 : it->second;
    }
    for (size_t i = 0; i < def->fields.size(); ++i)
        if (def->fields[i].name == name)
            return (int)i;
    return -1;
}

int structtype_variant_index(const enum_def *def, const std::string &name) {
    if (!def) return -1;
    if (def->variant_index.size() == def->variants.size()) {
        auto it = def->variant_index.find(name);
        return it == def->variant_index.end() ? -1 : it->second;
    }
    for (size_t i = 0; i < def->variants.size(); ++i)
        if (def->variants[i].name == name)
            return (int)i;
    return -1;
}

int structtype_variant_field_index(const variant_def *variant,
                                   const std::string &name) {
    if (!variant) return -1;
    if (variant->field_index.size() == variant->fields.size()) {
        auto it = variant->field_index.find(name);
        return it == variant->field_index.end() ? -1 : it->second;
    }
    for (size_t i = 0; i < variant->fields.size(); ++i)
        if (variant->fields[i].name == name)
            return (int)i;
    return -1;
}

struct value_t *newstructval(struct_def *def,
                             std::shared_ptr<struct_payload> payload) {
    struct struct_value_t *value = new struct_value_t;
    value->type = T_struct;
    value->def = def;
    value->payload = payload ? std::move(payload)
                             : std::make_shared<struct_payload>();
    return (struct value_t *)value;
}

struct value_t *newenumval(enum_def *def, int variant,
                           std::shared_ptr<struct_payload> payload) {
    struct enum_value_t *value = new enum_value_t;
    value->type = T_enum;
    value->def = def;
    value->variant = variant;
    value->payload = payload ? std::move(payload)
                             : std::make_shared<struct_payload>();
    return (struct value_t *)value;
}

struct value_t *newmethodval(value_t *receiver,
                             std::shared_ptr<function_def> method) {
    struct method_value_t *value = new method_value_t;
    value->type = T_method;
    value->receiver = receiver;
    value->method = std::move(method);
    return (struct value_t *)value;
}

void ensure_unique_struct(struct struct_value_t *value) {
    if (!value) return;
    if (!value->payload) {
        value->payload = std::make_shared<struct_payload>();
        return;
    }
    if (value->payload.use_count() == 1) {
        value->payload->hash_valid = false;
        return;
    }

    std::shared_ptr<struct_payload> copy = std::make_shared<struct_payload>();
    copy->fields.reserve(value->payload->fields.size());
    for (size_t i = 0; i < value->payload->fields.size(); ++i)
        copy->fields.push_back(dupval(value->payload->fields[i]));
    value->payload = std::move(copy);
}

void structtype_register_alias(const char *importer, const char *alias,
                               const char *target) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!importer || !alias || !target || !*alias || !*target)
        return;
    source_aliases[alias_key(importer, alias)] = target;
}

void structtype_register_type_alias(const char *importer, const char *alias,
                                    const char *target,
                                    const char *type_name) {
    if (!type_name || !*type_name) {
        structtype_register_alias(importer, alias, target);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!importer || !alias || !target || !*alias || !*target)
        return;
    const std::string key = alias_key(importer, alias);
    source_aliases[key] = target;
    type_alias_names[key] = type_name;
}

bool named_type_matches(const char *source, const char *name,
                        const struct value_t *value) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!source || !name || !value)
        return false;

    if (is_ref_value(value)) {
        ref_object *object = ref_target(const_cast<struct value_t *>(value));
        if (!object || !object->def)
            return false;
        struct struct_value_t plain;
        plain.type = T_struct;
        plain.def = object->def;
        plain.payload = object->payload;
        return named_type_matches(source, name, (const struct value_t *)&plain);
    }

    resolved_type resolved = resolve_named(source, name, true);
    if (resolved.protocol &&
        resolved.protocol->builtin_kind != PROTOCOL_BUILTIN_NONE) {
        if (resolved.protocol->builtin_kind == PROTOCOL_BUILTIN_HASHABLE)
            return dict_key_allowed(value);
        if (resolved.protocol->builtin_kind == PROTOCOL_BUILTIN_CALLABLE ||
            resolved.protocol->builtin_kind == PROTOCOL_BUILTIN_RPC_CLIENT ||
            resolved.protocol->builtin_kind == PROTOCOL_BUILTIN_RPC_JOB ||
            resolved.protocol->builtin_kind == PROTOCOL_BUILTIN_RPC_SERVER)
            return value_satisfies_builtin_protocol(value, resolved.protocol);
        if (resolved.protocol->builtin_kind == PROTOCOL_BUILTIN_ITERABLE &&
            value->type != T_struct && value->type != T_enum)
            return value_is_iterable(value);
        /* Structs/enums still use their real iter method for Iterable. */
    }

    if (value->type == T_enum) {
        const struct enum_value_t *ev = (const struct enum_value_t *)value;
        if (!ev->def)
            return false;
        if (ev->def->source == source && ev->def->name == name)
            return true;
        if (resolved.enumeration)
            return ev->def == resolved.enumeration;
        if (resolved.protocol)
            return structtype_has_protocol(ev->def, resolved.protocol);
        return false;
    }

    if (value->type != T_struct)
        return false;

    const struct struct_value_t *sv = (const struct struct_value_t *)value;
    if (!sv->def)
        return false;

    if (sv->def->source == source && sv->def->name == name)
        return true;

    if (resolved.structure)
        return sv->def == resolved.structure;
    if (resolved.protocol)
        return structtype_has_protocol(sv->def, resolved.protocol);
    return false;
}

bool structtype_named_types_equal(const char *module_a, const char *name_a,
                                  const char *module_b, const char *name_b) {
    std::lock_guard<std::recursive_mutex> lock(type_registry_mutex);
    if (!name_a || !name_b)
        return false;
    if (std::strcmp(name_a, name_b) == 0 &&
        ((!module_a && !module_b) ||
         (module_a && module_b && std::strcmp(module_a, module_b) == 0)))
        return true;

    resolved_type a = resolve_named(module_a, name_a, false);
    resolved_type b = resolve_named(module_b, name_b, false);
    if (a.structure || b.structure)
        return a.structure && a.structure == b.structure;
    if (a.protocol || b.protocol)
        return a.protocol && a.protocol == b.protocol;
    if (a.enumeration || b.enumeration)
        return a.enumeration && a.enumeration == b.enumeration;
    return false;
}
