#ifndef SKEIN_TYPECHECK_STATE_H
#define SKEIN_TYPECHECK_STATE_H

#include "static_type.h"

#include <string>
#include <unordered_map>

struct funcdef;
struct function_code;

namespace skein {
namespace typecheck {
namespace detail {

struct VarInfo {
    const TypeNode *type = nullptr;
    /* Original declared type when this binding is a narrowing overlay. */
    const TypeNode *declared = nullptr;
    bool fixed = false;
    bool exported = false;
};

using ConstTypeMap = std::unordered_map<std::string, const TypeNode *>;

struct StaticTypeInfo {
    bool is_enum = false;
    ConstTypeMap fields;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, const TypeNode *>>
        variants;
    std::unordered_map<std::string, struct funcdef *> methods;
};

struct Env {
    Env *parent = nullptr;
    std::unordered_map<std::string, VarInfo> vars;

    VarInfo *lookup(const std::string &name) {
        for (Env *env = this; env; env = env->parent) {
            auto it = env->vars.find(name);
            if (it != env->vars.end())
                return &it->second;
        }
        return nullptr;
    }

    const VarInfo *lookup(const std::string &name) const {
        for (const Env *env = this; env; env = env->parent) {
            auto it = env->vars.find(name);
            if (it != env->vars.end())
                return &it->second;
        }
        return nullptr;
    }

    Env *owner_of(const std::string &name) {
        for (Env *env = this; env; env = env->parent) {
            if (env->vars.find(name) != env->vars.end())
                return env;
        }
        return nullptr;
    }

    void define(const std::string &name, const TypeNode *type,
                bool fixed = false) {
        VarInfo info;
        info.type = type ? type : type_any();
        info.fixed = fixed;
        vars[name] = info;
    }
};

/* Imported namespace aliases, keyed by source name. */
extern std::unordered_map<
    std::string, std::unordered_map<std::string, std::string>>
    g_import_aliases;

/* Top-level inferred bindings, keyed by source name. */
extern std::unordered_map<
    std::string, std::unordered_map<std::string, VarInfo>>
    g_persistent_bindings;

/* Exported variable types, keyed by logical package name. */
extern std::unordered_map<
    std::string, std::unordered_map<std::string, VarInfo>>
    g_exported_types;

/* Inferred generator element and send types, keyed by function identity. */
extern std::unordered_map<std::string, const TypeNode *>
    g_generator_element_types;
extern std::unordered_map<std::string, const TypeNode *>
    g_generator_send_type_map;

} // namespace detail
} // namespace typecheck
} // namespace skein

#endif
