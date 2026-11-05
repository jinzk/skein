#include "structtype.h"

#include <cstring>
#include <unordered_map>
#include <utility>

namespace {

std::unordered_map<std::string, std::unique_ptr<struct_def>> struct_types;
std::unordered_map<std::string, std::unique_ptr<trait_def>> trait_types;
std::unordered_map<std::string, std::unique_ptr<enum_def>> enum_types;
std::unordered_map<std::string, struct_def *> logical_structs;
std::unordered_map<std::string, trait_def *> logical_traits;
std::unordered_map<std::string, enum_def *> logical_enums;
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
    enum_def *enumeration;
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
    resolved_type result{NULL, NULL, NULL};
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
    auto eit = enum_types.find(type_key(module, name));
    if (eit != enum_types.end()) {
        result.enumeration = eit->second.get();
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
        return result;
    }
    auto leit = logical_enums.find(type_key(target.c_str(), base.c_str()));
    if (leit != logical_enums.end() &&
        (!require_exported || leit->second->exported))
        result.enumeration = leit->second;
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
    enum_types.clear();
    logical_structs.clear();
    logical_traits.clear();
    logical_enums.clear();
    module_aliases.clear();
}

struct_def *structtype_find_struct(const char *module, const char *name) {
    if (!name) return NULL;
    auto it = struct_types.find(type_key(module, name));
    return it == struct_types.end() ? NULL : it->second.get();
}

struct_def *structtype_find_struct_by_name(const char *module_name,
                                           const char *name) {
    if (!name) return NULL;
    if (module_name && *module_name) {
        auto it = logical_structs.find(type_key(module_name, name));
        return it == logical_structs.end() ? NULL : it->second;
    }
    struct_def *found = NULL;
    for (auto &entry : logical_structs) {
        if (!entry.second || entry.second->name != name)
            continue;
        if (found && found != entry.second)
            return NULL;
        found = entry.second;
    }
    return found;
}

trait_def *structtype_find_trait(const char *module, const char *name) {
    if (!name) return NULL;
    auto it = trait_types.find(type_key(module, name));
    return it == trait_types.end() ? NULL : it->second.get();
}

enum_def *structtype_find_enum(const char *module, const char *name) {
    if (!name) return NULL;
    auto it = enum_types.find(type_key(module, name));
    return it == enum_types.end() ? NULL : it->second.get();
}

enum_def *structtype_find_enum_by_name(const char *module_name,
                                       const char *name) {
    if (!name) return NULL;
    if (module_name && *module_name) {
        auto it = logical_enums.find(type_key(module_name, name));
        return it == logical_enums.end() ? NULL : it->second;
    }
    enum_def *found = NULL;
    for (auto &entry : logical_enums) {
        if (!entry.second || entry.second->name != name)
            continue;
        if (found && found != entry.second)
            return NULL;
        found = entry.second;
    }
    return found;
}

bool structtype_type_exists(const char *module, const char *name) {
    if (!name) return false;
    std::string key = type_key(module, name);
    if (struct_types.count(key) != 0 || trait_types.count(key) != 0 ||
        enum_types.count(key) != 0)
        return true;
    resolved_type resolved = resolve_named(module, name, true);
    return resolved.structure != NULL || resolved.trait != NULL ||
           resolved.enumeration != NULL;
}

bool structtype_prepare_struct(const char *module, const char *module_name,
                               const char *name, bool exported,
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
        logical_traits.count(logical_key) != 0 ||
        logical_enums.count(logical_key) != 0) {
        error = std::string("type repeated ") + name +
                " in module " + (module_name ? module_name : "");
        return false;
    }

    std::unique_ptr<struct_def> def(new struct_def);
    def->name = name;
    def->module = module ? module : "";
    def->module_name = module_name ? module_name : def->module;
    def->exported = exported;
    out = def.get();
    logical_structs.emplace(logical_key, out);
    struct_types.emplace(type_key(module, name), std::move(def));
    return true;
}

bool structtype_finalize_struct(struct_def *def,
                                std::vector<field_def> fields,
                                std::string &error) {
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

bool structtype_create_struct(const char *module, const char *module_name,
                              const char *name, bool exported,
                              std::vector<field_def> fields,
                              struct_def *&out, std::string &error) {
    if (!structtype_prepare_struct(module, module_name, name, exported, out,
                                   error))
        return false;
    if (!structtype_finalize_struct(out, std::move(fields), error)) {
        structtype_rollback_struct(out);
        out = NULL;
        return false;
    }
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

bool structtype_prepare_enum(const char *module, const char *module_name,
                             const char *name, bool exported,
                             enum_def *&out, std::string &error) {
    out = NULL;
    if (!name || !*name) {
        error = "invalid enum name";
        return false;
    }
    if (structtype_type_exists(module, name)) {
        error = std::string("type repeated ") + name;
        return false;
    }
    std::string logical_key = type_key(module_name, name);
    if (logical_structs.count(logical_key) != 0 ||
        logical_traits.count(logical_key) != 0 ||
        logical_enums.count(logical_key) != 0) {
        error = std::string("type repeated ") + name +
                " in module " + (module_name ? module_name : "");
        return false;
    }

    std::unique_ptr<enum_def> def(new enum_def);
    def->name = name;
    def->module = module ? module : "";
    def->module_name = module_name ? module_name : def->module;
    def->exported = exported;
    out = def.get();
    logical_enums.emplace(logical_key, out);
    enum_types.emplace(type_key(module, name), std::move(def));
    return true;
}

bool structtype_finalize_enum(enum_def *def, std::vector<variant_def> variants,
                              std::string &error) {
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

bool structtype_create_enum(const char *module, const char *module_name,
                            const char *name, bool exported,
                            std::vector<variant_def> variants,
                            enum_def *&out, std::string &error) {
    if (!structtype_prepare_enum(module, module_name, name, exported, out,
                                 error))
        return false;
    if (!structtype_finalize_enum(out, std::move(variants), error)) {
        structtype_rollback_enum(out);
        out = NULL;
        return false;
    }
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
    if (!module || !name || !value)
        return false;

    if (value->type == T_enum) {
        const struct enum_value_t *ev = (const struct enum_value_t *)value;
        if (!ev->def)
            return false;
        if (ev->def->module == module && ev->def->name == name)
            return true;
        resolved_type resolved = resolve_named(module, name, true);
        return resolved.enumeration && ev->def == resolved.enumeration;
    }

    if (value->type != T_struct)
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
