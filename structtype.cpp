#include "structtype.h"

#include <cstring>
#include <unordered_map>
#include <utility>

namespace {

std::unordered_map<std::string, std::unique_ptr<struct_def>> struct_types;
std::unordered_map<std::string, std::unique_ptr<trait_def>> trait_types;
std::unordered_map<std::string, struct_def *> logical_structs;
std::unordered_map<std::string, trait_def *> logical_traits;
std::unordered_map<std::string, std::string> module_aliases;

std::string type_key(const char *module, const char *name) {
    std::string key = module ? module : "";
    key.push_back('\x1f');
    key += name ? name : "";
    return key;
}

std::string alias_key(const char *importer, const char *alias) {
    return type_key(importer, alias);
}

struct resolved_type {
    struct_def *structure;
    trait_def *trait;
};

bool split_qualified_name(const char *name, std::string &prefix,
                          std::string &base) {
    if (!name) return false;
    std::string text(name);
    size_t sep = text.rfind("::");
    if (sep == std::string::npos || sep == 0 || sep + 2 >= text.size())
        return false;
    prefix = text.substr(0, sep);
    base = text.substr(sep + 2);
    return true;
}

std::string resolve_qualified_module(const char *importer,
                                      const std::string &prefix) {
    auto it = module_aliases.find(alias_key(importer, prefix.c_str()));
    if (it != module_aliases.end())
        return it->second;
    return prefix;
}

resolved_type resolve_named(const char *module, const char *name,
                            bool require_exported) {
    resolved_type result{NULL, NULL};
    if (!name) return result;

    auto sit = struct_types.find(type_key(module, name));
    if (sit != struct_types.end()) {
        result.structure = sit->second.get();
        return result;
    }
    auto tit = trait_types.find(type_key(module, name));
    if (tit != trait_types.end()) {
        result.trait = tit->second.get();
        return result;
    }

    std::string prefix;
    std::string base;
    if (!split_qualified_name(name, prefix, base))
        return result;

    std::string target = resolve_qualified_module(module, prefix);
    auto lsit = logical_structs.find(type_key(target.c_str(), base.c_str()));
    if (lsit != logical_structs.end() &&
        (!require_exported || lsit->second->exported)) {
        result.structure = lsit->second;
        return result;
    }
    auto ltit = logical_traits.find(type_key(target.c_str(), base.c_str()));
    if (ltit != logical_traits.end() &&
        (!require_exported || ltit->second->exported)) {
        result.trait = ltit->second;
    }
    return result;
}

} // namespace

struct_payload::~struct_payload() {
    for (size_t i = 0; i < fields.size(); ++i)
        freeval(fields[i]);
}

void structtype_shutdown() {
    struct_types.clear();
    trait_types.clear();
    logical_structs.clear();
    logical_traits.clear();
    module_aliases.clear();
}

struct_def *structtype_find_struct(const char *module, const char *name) {
    if (!name) return NULL;
    auto it = struct_types.find(type_key(module, name));
    return it == struct_types.end() ? NULL : it->second.get();
}

trait_def *structtype_find_trait(const char *module, const char *name) {
    if (!name) return NULL;
    auto it = trait_types.find(type_key(module, name));
    return it == trait_types.end() ? NULL : it->second.get();
}

bool structtype_type_exists(const char *module, const char *name) {
    if (!name) return false;
    std::string key = type_key(module, name);
    if (struct_types.count(key) != 0 || trait_types.count(key) != 0)
        return true;
    resolved_type resolved = resolve_named(module, name, true);
    return resolved.structure != NULL || resolved.trait != NULL;
}

bool structtype_create_struct(const char *module, const char *module_name,
                              const char *name, bool exported,
                              std::vector<field_def> fields,
                              struct_def *&out, std::string &error) {
    out = NULL;
    if (!name || !*name) {
        error = "invalid struct name";
        return false;
    }
    if (structtype_type_exists(module, name)) {
        error = std::string("type repeated ") + name;
        return false;
    }
    std::string logical_key = type_key(module_name, name);
    if (logical_structs.count(logical_key) != 0 ||
        logical_traits.count(logical_key) != 0) {
        error = std::string("type repeated ") + name +
                " in module " + (module_name ? module_name : "");
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

    std::unique_ptr<struct_def> def(new struct_def);
    def->name = name;
    def->module = module ? module : "";
    def->module_name = module_name ? module_name : def->module;
    def->exported = exported;
    def->fields = std::move(fields);
    out = def.get();
    logical_structs.emplace(logical_key, out);
    struct_types.emplace(type_key(module, name), std::move(def));
    return true;
}

bool structtype_create_trait(const char *module, const char *module_name,
                             const char *name, bool exported,
                             std::vector<method_sig> methods,
                             trait_def *&out, std::string &error) {
    out = NULL;
    if (!name || !*name) {
        error = "invalid trait name";
        return false;
    }
    if (structtype_type_exists(module, name)) {
        error = std::string("type repeated ") + name;
        return false;
    }
    std::string logical_key = type_key(module_name, name);
    if (logical_structs.count(logical_key) != 0 ||
        logical_traits.count(logical_key) != 0) {
        error = std::string("type repeated ") + name +
                " in module " + (module_name ? module_name : "");
        return false;
    }

    std::unordered_map<std::string, bool> seen;
    for (size_t i = 0; i < methods.size(); ++i) {
        if (methods[i].name.empty()) {
            error = "invalid trait method name";
            return false;
        }
        if (!seen.emplace(methods[i].name, true).second) {
            error = "trait method repeated " + methods[i].name;
            return false;
        }
    }

    std::unique_ptr<trait_def> def(new trait_def);
    def->name = name;
    def->module = module ? module : "";
    def->module_name = module_name ? module_name : def->module;
    def->exported = exported;
    def->methods = std::move(methods);
    out = def.get();
    logical_traits.emplace(logical_key, out);
    trait_types.emplace(type_key(module, name), std::move(def));
    return true;
}

bool structtype_add_method(struct_def *def, const std::string &name,
                           std::shared_ptr<function_def> code,
                           std::string &error) {
    if (!def || name.empty() || !code) {
        error = "invalid method";
        return false;
    }
    if (structtype_field_index(def, name) >= 0) {
        error = "field and method have the same name " + name;
        return false;
    }
    if (def->methods.count(name) != 0) {
        error = "method repeated " + name;
        return false;
    }
    def->methods.emplace(name, std::move(code));
    return true;
}

bool structtype_add_trait(struct_def *def, trait_def *trait,
                          std::string &error) {
    if (!def || !trait) {
        error = "invalid trait implementation";
        return false;
    }
    if (structtype_has_trait(def, trait)) {
        error = "trait repeated " + trait->name + " for " + def->name;
        return false;
    }
    def->traits.push_back(trait);
    return true;
}

bool structtype_has_trait(const struct_def *def, const trait_def *trait) {
    if (!def || !trait) return false;
    for (size_t i = 0; i < def->traits.size(); ++i)
        if (def->traits[i] == trait)
            return true;
    return false;
}

int structtype_field_index(const struct_def *def, const std::string &name) {
    if (!def) return -1;
    for (size_t i = 0; i < def->fields.size(); ++i)
        if (def->fields[i].name == name)
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

struct value_t *newmethodval(struct_def *def,
                             std::shared_ptr<struct_payload> receiver,
                             std::shared_ptr<function_def> method) {
    struct method_value_t *value = new method_value_t;
    value->type = T_method;
    value->def = def;
    value->receiver = std::move(receiver);
    value->method = std::move(method);
    return (struct value_t *)value;
}

void ensure_unique_struct(struct struct_value_t *value) {
    if (!value) return;
    if (!value->payload) {
        value->payload = std::make_shared<struct_payload>();
        return;
    }
    if (value->payload.use_count() == 1)
        return;

    std::shared_ptr<struct_payload> copy = std::make_shared<struct_payload>();
    copy->fields.reserve(value->payload->fields.size());
    for (size_t i = 0; i < value->payload->fields.size(); ++i)
        copy->fields.push_back(dupval(value->payload->fields[i]));
    value->payload = std::move(copy);
}

void structtype_register_alias(const char *importer, const char *alias,
                               const char *target) {
    if (!importer || !alias || !target || !*alias || !*target)
        return;
    module_aliases[alias_key(importer, alias)] = target;
}

bool named_type_matches(const char *module, const char *name,
                        const struct value_t *value) {
    if (!module || !name || !value || value->type != T_struct)
        return false;

    const struct struct_value_t *sv = (const struct struct_value_t *)value;
    if (!sv->def)
        return false;

    if (sv->def->module == module && sv->def->name == name)
        return true;

    resolved_type resolved = resolve_named(module, name, true);
    if (resolved.structure)
        return sv->def == resolved.structure;
    if (resolved.trait)
        return structtype_has_trait(sv->def, resolved.trait);
    return false;
}
