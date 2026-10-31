#include "structtype.h"
#include "diagnostics.h"

#include <cstring>
#include <unordered_map>
#include <utility>

namespace {

std::unordered_map<std::string, std::unique_ptr<struct_def>> struct_types;
std::unordered_map<std::string, std::unique_ptr<trait_def>> trait_types;

} // namespace

struct_payload::~struct_payload() {
    for (size_t i = 0; i < fields.size(); ++i)
        freeval(fields[i]);
}

void structtype_shutdown() {
    struct_types.clear();
    trait_types.clear();
}

struct_def *structtype_find_struct(const char *name) {
    if (!name) return NULL;
    auto it = struct_types.find(name);
    return it == struct_types.end() ? NULL : it->second.get();
}

trait_def *structtype_find_trait(const char *name) {
    if (!name) return NULL;
    auto it = trait_types.find(name);
    return it == trait_types.end() ? NULL : it->second.get();
}

bool structtype_type_exists(const char *name) {
    if (!name) return false;
    return struct_types.count(name) != 0 || trait_types.count(name) != 0;
}

bool structtype_create_struct(const char *name, std::vector<field_def> fields,
                              struct_def *&out, std::string &error) {
    out = NULL;
    if (!name || !*name) {
        error = "invalid struct name";
        return false;
    }
    if (structtype_type_exists(name)) {
        error = std::string("type repeated ") + name;
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
    def->module = diag_current_source_name();
    def->fields = std::move(fields);
    out = def.get();
    struct_types.emplace(out->name, std::move(def));
    return true;
}

bool structtype_create_trait(const char *name, std::vector<method_sig> methods,
                             trait_def *&out, std::string &error) {
    out = NULL;
    if (!name || !*name) {
        error = "invalid trait name";
        return false;
    }
    if (structtype_type_exists(name)) {
        error = std::string("type repeated ") + name;
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
    def->module = diag_current_source_name();
    def->methods = std::move(methods);
    out = def.get();
    trait_types.emplace(out->name, std::move(def));
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

bool named_type_matches(const char *name, const struct value_t *value) {
    if (!name || !value || value->type != T_struct)
        return false;

    const struct struct_value_t *sv = (const struct struct_value_t *)value;
    if (!sv->def)
        return false;

    if (sv->def->name == name)
        return true;

    trait_def *trait = structtype_find_trait(name);
    if (!trait)
        return false;
    return structtype_has_trait(sv->def, trait);
}
