#include "typeinfo.h"

#include "structtype.h"
#include "symbol.h"
#include "valuetype.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

const char *builtin_type_for_value(const struct value_t *value) {
    if (!value)
        return NULL;
    switch (value->type) {
        case T_int: return "int";
        case T_float: return "float";
        case T_bool: return "bool";
        case T_char: return "char";
        case T_byte: return "byte";
        case T_bytes: return "bytes";
        case T_list: return "list";
        case T_dict: return "dict";
        case T_deque: return "deque";
        case T_heap: return "heap";
        case T_set: return "set";
        case T_func: return "function";
        case T_builtin: return "builtin";
        case T_userdata: return "userdata";
        case T_result: return "result";
        case T_error: return "error";
        case T_null: return "null";
        default: return NULL;
    }
}

enum resolved_kind {
    RESOLVED_STRUCT,
    RESOLVED_ENUM,
    RESOLVED_TRAIT,
    RESOLVED_BUILTIN,
};

struct resolved_type {
    resolved_kind kind = RESOLVED_BUILTIN;
    const struct_def *structure = NULL;
    const enum_def *enumeration = NULL;
    const trait_def *trait = NULL;
    const char *builtin_name = NULL;
    std::string input_name;
};

enum resolve_status {
    RESOLVE_FOUND,
    RESOLVE_NOT_FOUND,
    RESOLVE_NOT_VISIBLE,
    RESOLVE_ERROR,
};

bool type_def_visible(const char *importer_source,
                      const char *importer_module,
                      const type_def *def) {
    if (!def)
        return false;
    if (def->exported)
        return true;
    if (importer_source && def->module == importer_source)
        return true;
    if (importer_module && def->module_name == importer_module)
        return true;
    return false;
}

resolve_status resolve_type_arg(const char *importer_source,
                                const char *importer_module,
                                struct value_t *arg, resolved_type &out,
                                std::string &error) {
    out = resolved_type();
    error.clear();

    if (!arg) {
        error = "std.meta expects a type argument";
        return RESOLVE_ERROR;
    }

    if (arg->type == T_struct) {
        const struct_value_t *sv = (const struct_value_t *)arg;
        if (!sv->def) {
            error = "invalid struct value";
            return RESOLVE_ERROR;
        }
        if (!type_def_visible(importer_source, importer_module, sv->def)) {
            error = "type " + sv->def->module_name + "::" + sv->def->name +
                    " is not visible";
            return RESOLVE_NOT_VISIBLE;
        }
        out.kind = RESOLVED_STRUCT;
        out.structure = sv->def;
        out.input_name = sv->def->module_name + "::" + sv->def->name;
        return RESOLVE_FOUND;
    }

    if (arg->type == T_enum) {
        const enum_value_t *ev = (const enum_value_t *)arg;
        if (!ev->def) {
            error = "invalid enum value";
            return RESOLVE_ERROR;
        }
        if (!type_def_visible(importer_source, importer_module, ev->def)) {
            error = "type " + ev->def->module_name + "::" + ev->def->name +
                    " is not visible";
            return RESOLVE_NOT_VISIBLE;
        }
        out.kind = RESOLVED_ENUM;
        out.enumeration = ev->def;
        out.input_name = ev->def->module_name + "::" + ev->def->name;
        return RESOLVE_FOUND;
    }

    if (arg->type != T_string) {
        const char *canonical = builtin_type_for_value(arg);
        if (canonical) {
            out.kind = RESOLVED_BUILTIN;
            out.builtin_name = canonical;
            out.input_name = canonical;
            return RESOLVE_FOUND;
        }
        error = std::string("value of type ") + typename_of(arg) +
                " is not a named type";
        return RESOLVE_ERROR;
    }

    std::string text = ((const string_value_t *)arg)->value;
    const char *canonical =
        type_constraint_canonical_builtin_name(text.c_str());
    if (canonical) {
        out.kind = RESOLVED_BUILTIN;
        out.builtin_name = canonical;
        out.input_name = canonical;
        return RESOLVE_FOUND;
    }

    structtype_lookup_result lookup;
    enum structtype_lookup_status status = structtype_lookup_type(
        importer_source, importer_module, text.c_str(), lookup, error);
    switch (status) {
        case STRUCTTYPE_LOOKUP_FOUND:
            break;
        case STRUCTTYPE_LOOKUP_NOT_FOUND:
            return RESOLVE_NOT_FOUND;
        case STRUCTTYPE_LOOKUP_NOT_VISIBLE:
            return RESOLVE_NOT_VISIBLE;
        case STRUCTTYPE_LOOKUP_AMBIGUOUS:
        case STRUCTTYPE_LOOKUP_ERROR:
            return RESOLVE_ERROR;
    }

    out.input_name = text;
    switch (lookup.kind) {
        case STRUCTTYPE_LOOKUP_STRUCT:
            out.kind = RESOLVED_STRUCT;
            out.structure = lookup.structure;
            break;
        case STRUCTTYPE_LOOKUP_ENUM:
            out.kind = RESOLVED_ENUM;
            out.enumeration = lookup.enumeration;
            break;
        case STRUCTTYPE_LOOKUP_TRAIT:
            out.kind = RESOLVED_TRAIT;
            out.trait = lookup.trait;
            break;
        default:
            error = "invalid type " + text;
            return RESOLVE_ERROR;
    }
    return RESOLVE_FOUND;
}

resolve_status resolve_trait_arg(const char *importer_source,
                                 const char *importer_module,
                                 struct value_t *arg, const trait_def *&out,
                                 std::string &error) {
    out = NULL;
    error.clear();
    if (!arg || arg->type != T_string) {
        error = "std.meta.implements expects a trait name string";
        return RESOLVE_ERROR;
    }

    std::string text = ((const string_value_t *)arg)->value;
    if (type_constraint_canonical_builtin_name(text.c_str())) {
        error = "type " + text + " is not a trait";
        return RESOLVE_ERROR;
    }

    structtype_lookup_result lookup;
    enum structtype_lookup_status status = structtype_lookup_type(
        importer_source, importer_module, text.c_str(), lookup, error);
    switch (status) {
        case STRUCTTYPE_LOOKUP_FOUND:
            break;
        case STRUCTTYPE_LOOKUP_NOT_FOUND:
            error = "unknown trait " + text;
            return RESOLVE_NOT_FOUND;
        case STRUCTTYPE_LOOKUP_AMBIGUOUS:
        case STRUCTTYPE_LOOKUP_NOT_VISIBLE:
        case STRUCTTYPE_LOOKUP_ERROR:
            return RESOLVE_ERROR;
    }

    if (lookup.kind != STRUCTTYPE_LOOKUP_TRAIT) {
        error = "type " + text + " is not a trait";
        return RESOLVE_ERROR;
    }
    out = lookup.trait;
    return RESOLVE_FOUND;
}

void dict_add(struct dict_value_t *dict, const char *key,
              struct value_t *value) {
    dict->payload->items.push_back(
        std::make_pair(newstrval(key), value));
}

void list_add(struct list_value_t *list, struct value_t *value) {
    list->payload->items.push_back(value);
}

struct value_t *new_string_list() {
    return (struct value_t *)newlistval();
}

std::string type_text(const struct type_constraint *type) {
    return std::string(type_constraint_name(type));
}

std::string trait_name(const trait_def *trait) {
    if (!trait)
        return std::string();
    if (trait->module_name.empty())
        return trait->name;
    return trait->module_name + "::" + trait->name;
}

struct value_t *field_info(const field_def &field, bool readonly) {
    struct dict_value_t *dict = newdictval();
    dict_add(dict, "name", newstrval(field.name.c_str()));
    dict_add(dict, "type", newstrval(type_text(&field.type).c_str()));
    dict_add(dict, "nullable", newboolval(field.type.nullable));
    dict_add(dict, "readonly", newboolval(readonly));
    dict_add(dict, "has_default", newboolval(field.has_default));
    return (struct value_t *)dict;
}

struct value_t *variant_info(const variant_def &variant) {
    struct dict_value_t *dict = newdictval();
    dict_add(dict, "name", newstrval(variant.name.c_str()));
    struct list_value_t *fields = newlistval();
    for (const field_def &field : variant.fields) {
        list_add(fields, field_info(field, true));
    }
    dict_add(dict, "fields", (struct value_t *)fields);
    return (struct value_t *)dict;
}

struct value_t *method_info_from_code(const std::string &name,
                                       const function_def *definition) {
    struct dict_value_t *dict = newdictval();
    dict_add(dict, "name", newstrval(name.c_str()));

    struct list_value_t *params = newlistval();
    bool has_defaults = false;
    const struct function_code *code =
        definition ? definition->code.get() : NULL;
    const struct symlist *param = code ? code->sl : NULL;
    /* Methods store the receiver as the first parameter.  Script calls do not
     * pass it explicitly, so it is not part of the reflected parameter list. */
    if (param)
        param = param->next;
    for (; param; param = param->next) {
        list_add(params, newstrval(type_text(param->type).c_str()));
        if (param->has_default)
            has_defaults = true;
    }
    dict_add(dict, "params", (struct value_t *)params);
    dict_add(dict, "variadic", newboolval(code ? code->variadic : false));
    dict_add(dict, "has_defaults", newboolval(has_defaults));
    dict_add(dict, "async", newboolval(code ? code->is_async : false));
    return (struct value_t *)dict;
}

struct value_t *method_info_from_sig(const method_sig &sig) {
    struct dict_value_t *dict = newdictval();
    dict_add(dict, "name", newstrval(sig.name.c_str()));

    struct list_value_t *params = newlistval();
    /* method_sig.params includes the receiver at index 0. */
    for (size_t i = 1; i < sig.params.size(); ++i) {
        list_add(params,
                 newstrval(type_text(&sig.params[i]).c_str()));
    }
    dict_add(dict, "params", (struct value_t *)params);
    dict_add(dict, "variadic", newboolval(false));
    dict_add(dict, "has_defaults", newboolval(false));
    dict_add(dict, "async", newboolval(sig.is_async));
    return (struct value_t *)dict;
}

struct value_t *type_methods(const type_def *definition) {
    struct list_value_t *methods = newlistval();
    if (!definition)
        return (struct value_t *)methods;

    std::vector<std::string> names;
    names.reserve(definition->methods.size());
    for (const auto &entry : definition->methods) {
        if (entry.second)
            names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());

    for (const std::string &name : names) {
        auto it = definition->methods.find(name);
        if (it == definition->methods.end() || !it->second)
            continue;
        list_add(methods, method_info_from_code(name, it->second.get()));
    }
    return (struct value_t *)methods;
}

struct value_t *trait_methods(const trait_def *definition) {
    struct list_value_t *methods = newlistval();
    if (!definition)
        return (struct value_t *)methods;

    std::vector<const method_sig *> signatures;
    signatures.reserve(definition->methods.size());
    for (const method_sig &sig : definition->methods)
        signatures.push_back(&sig);
    std::sort(signatures.begin(), signatures.end(),
              [](const method_sig *a, const method_sig *b) {
                  return a->name < b->name;
              });
    for (const method_sig *sig : signatures)
        list_add(methods, method_info_from_sig(*sig));
    return (struct value_t *)methods;
}

struct value_t *type_traits(const type_def *definition) {
    struct list_value_t *traits = newlistval();
    if (!definition)
        return (struct value_t *)traits;

    std::vector<std::string> names;
    names.reserve(definition->traits.size());
    for (const trait_def *trait : definition->traits) {
        if (!trait)
            continue;
        names.push_back(trait_name(trait));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const std::string &name : names)
        list_add(traits, newstrval(name.c_str()));
    return (struct value_t *)traits;
}

struct value_t *struct_fields(const struct_def *definition) {
    struct list_value_t *fields = newlistval();
    if (!definition)
        return (struct value_t *)fields;
    for (const field_def &field : definition->fields)
        list_add(fields, field_info(field, false));
    return (struct value_t *)fields;
}

struct value_t *enum_variants(const enum_def *definition) {
    struct list_value_t *variants = newlistval();
    if (!definition)
        return (struct value_t *)variants;
    for (const variant_def &variant : definition->variants)
        list_add(variants, variant_info(variant));
    return (struct value_t *)variants;
}

struct value_t *new_type_dict(const char *kind, const char *name,
                              const std::string &module,
                              const std::string &source,
                              bool exported,
                              struct value_t *fields,
                              struct value_t *variants,
                              struct value_t *methods,
                              struct value_t *traits) {
    struct dict_value_t *dict = newdictval();
    dict_add(dict, "kind", newstrval(kind));
    dict_add(dict, "name", newstrval(name));
    dict_add(dict, "module", newstrval(module.c_str()));
    dict_add(dict, "source", newstrval(source.c_str()));
    dict_add(dict, "exported", newboolval(exported));
    dict_add(dict, "fields", fields);
    dict_add(dict, "variants", variants);
    dict_add(dict, "methods", methods);
    dict_add(dict, "traits", traits);
    return (struct value_t *)dict;
}

struct value_t *make_type_info(const resolved_type &type) {
    switch (type.kind) {
        case RESOLVED_BUILTIN:
            return new_type_dict("builtin", type.builtin_name, "", "", true,
                                 new_string_list(), new_string_list(),
                                 new_string_list(), new_string_list());
        case RESOLVED_STRUCT: {
            const struct_def *def = type.structure;
            return new_type_dict("struct", def->name.c_str(), def->module_name,
                                 def->module, def->exported,
                                 struct_fields(def), new_string_list(),
                                 type_methods(def), type_traits(def));
        }
        case RESOLVED_ENUM: {
            const enum_def *def = type.enumeration;
            return new_type_dict("enum", def->name.c_str(), def->module_name,
                                 def->module, def->exported, new_string_list(),
                                 enum_variants(def), type_methods(def),
                                 type_traits(def));
        }
        case RESOLVED_TRAIT: {
            const trait_def *def = type.trait;
            return new_type_dict("trait", def->name.c_str(), def->module_name,
                                 def->module, def->exported, new_string_list(),
                                 new_string_list(), trait_methods(def),
                                 new_string_list());
        }
    }
    return NULL;
}

const type_def *as_type_def(const resolved_type &type) {
    if (type.kind == RESOLVED_STRUCT)
        return static_cast<const type_def *>(type.structure);
    if (type.kind == RESOLVED_ENUM)
        return static_cast<const type_def *>(type.enumeration);
    return NULL;
}

bool report_resolve_error(resolve_status status, const std::string &error) {
    if (status == RESOLVE_FOUND)
        return true;
    yyerror("%s", error.c_str());
    return false;
}

struct value_t *resolved_fields(const resolved_type &type) {
    if (type.kind == RESOLVED_STRUCT)
        return struct_fields(type.structure);
    return new_string_list();
}

struct value_t *resolved_variants(const resolved_type &type) {
    if (type.kind == RESOLVED_ENUM)
        return enum_variants(type.enumeration);
    return new_string_list();
}

struct value_t *resolved_methods(const resolved_type &type) {
    if (type.kind == RESOLVED_TRAIT)
        return trait_methods(type.trait);
    const type_def *definition = as_type_def(type);
    if (definition)
        return type_methods(definition);
    return new_string_list();
}

struct value_t *resolved_traits(const resolved_type &type) {
    const type_def *definition = as_type_def(type);
    if (definition)
        return type_traits(definition);
    return new_string_list();
}

} // namespace

struct value_t *typeinfo_type_info(const char *importer_source,
                                   const char *importer_module,
                                   struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_module, type_arg,
                                               type, error),
                              error))
        return NULL;
    return make_type_info(type);
}

struct value_t *typeinfo_fields(const char *importer_source,
                                const char *importer_module,
                                struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_module, type_arg,
                                               type, error),
                              error))
        return NULL;
    return resolved_fields(type);
}

struct value_t *typeinfo_variants(const char *importer_source,
                                  const char *importer_module,
                                  struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_module, type_arg,
                                               type, error),
                              error))
        return NULL;
    return resolved_variants(type);
}

struct value_t *typeinfo_methods(const char *importer_source,
                                 const char *importer_module,
                                 struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_module, type_arg,
                                               type, error),
                              error))
        return NULL;
    return resolved_methods(type);
}

struct value_t *typeinfo_traits(const char *importer_source,
                                const char *importer_module,
                                struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_module, type_arg,
                                               type, error),
                              error))
        return NULL;
    return resolved_traits(type);
}

struct value_t *typeinfo_implements(const char *importer_source,
                                    const char *importer_module,
                                    struct value_t *type_arg,
                                    struct value_t *trait_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_module, type_arg,
                                               type, error),
                              error))
        return NULL;

    const trait_def *trait = NULL;
    if (!report_resolve_error(resolve_trait_arg(importer_source,
                                                importer_module, trait_arg,
                                                trait, error),
                              error))
        return NULL;

    const type_def *definition = as_type_def(type);
    if (!definition)
        return newboolval(false);
    return newboolval(structtype_has_trait(definition, trait));
}

struct value_t *typeinfo_has_type(const char *importer_source,
                                  const char *importer_module,
                                  struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    resolve_status status = resolve_type_arg(importer_source, importer_module,
                                             type_arg, type, error);
    if (status == RESOLVE_FOUND)
        return newboolval(true);
    if (status == RESOLVE_NOT_FOUND || status == RESOLVE_NOT_VISIBLE)
        return newboolval(false);
    yyerror("%s", error.c_str());
    return NULL;
}
