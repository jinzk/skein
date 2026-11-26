#include "typeinfo.h"

#include "structtype.h"
#include "symbol.h"
#include "type_descriptor.h"
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
    RESOLVED_PROTOCOL,
    RESOLVED_BUILTIN,
};

struct resolved_type {
    resolved_kind kind = RESOLVED_BUILTIN;
    const struct_def *structure = NULL;
    const enum_def *enumeration = NULL;
    const protocol_def *protocol = NULL;
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

resolve_status resolve_type_name_text(const char *importer_source,
                                      const char *importer_package,
                                      const std::string &text,
                                      resolved_type &out,
                                      std::string &error) {
    out = resolved_type();
    error.clear();
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
        importer_source, importer_package, text.c_str(), lookup, error);
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
        case STRUCTTYPE_LOOKUP_PROTOCOL:
            out.kind = RESOLVED_PROTOCOL;
            out.protocol = lookup.protocol;
            break;
        default:
            error = "invalid type " + text;
            return RESOLVE_ERROR;
    }
    return RESOLVE_FOUND;
}

resolve_status resolve_type_arg(const char *importer_source,
                                const char *importer_package,
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
        if (!type_def_visible(importer_source, importer_package, sv->def)) {
            error = "type " + sv->def->package + "." + sv->def->name +
                    " is not visible";
            return RESOLVE_NOT_VISIBLE;
        }
        out.kind = RESOLVED_STRUCT;
        out.structure = sv->def;
        out.input_name = sv->def->package + "." + sv->def->name;
        return RESOLVE_FOUND;
    }

    if (arg->type == T_enum) {
        const enum_value_t *ev = (const enum_value_t *)arg;
        if (!ev->def) {
            error = "invalid enum value";
            return RESOLVE_ERROR;
        }
        if (!type_def_visible(importer_source, importer_package, ev->def)) {
            error = "type " + ev->def->package + "." + ev->def->name +
                    " is not visible";
            return RESOLVE_NOT_VISIBLE;
        }
        out.kind = RESOLVED_ENUM;
        out.enumeration = ev->def;
        out.input_name = ev->def->package + "." + ev->def->name;
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
    return resolve_type_name_text(importer_source, importer_package, text,
                                  out, error);
}

resolve_status resolve_protocol_arg(const char *importer_source,
                                 const char *importer_package,
                                 struct value_t *arg, const protocol_def *&out,
                                 std::string &error) {
    out = NULL;
    error.clear();
    if (!arg || arg->type != T_string) {
        error = "std.meta.implements expects a proto name string";
        return RESOLVE_ERROR;
    }

    std::string text = ((const string_value_t *)arg)->value;
    if (type_constraint_canonical_builtin_name(text.c_str())) {
        error = "type " + text + " is not a proto";
        return RESOLVE_ERROR;
    }

    structtype_lookup_result lookup;
    enum structtype_lookup_status status = structtype_lookup_type(
        importer_source, importer_package, text.c_str(), lookup, error);
    switch (status) {
        case STRUCTTYPE_LOOKUP_FOUND:
            break;
        case STRUCTTYPE_LOOKUP_NOT_FOUND:
            error = "unknown proto " + text;
            return RESOLVE_NOT_FOUND;
        case STRUCTTYPE_LOOKUP_AMBIGUOUS:
        case STRUCTTYPE_LOOKUP_NOT_VISIBLE:
        case STRUCTTYPE_LOOKUP_ERROR:
            return RESOLVE_ERROR;
    }

    if (lookup.kind != STRUCTTYPE_LOOKUP_PROTOCOL) {
        error = "type " + text + " is not a proto";
        return RESOLVE_ERROR;
    }
    out = lookup.protocol;
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

std::string protocol_name(const protocol_def *protocol) {
    if (!protocol)
        return std::string();
    if (protocol->package.empty())
        return protocol->name;
    return protocol->package + "." + protocol->name;
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
    return (struct value_t *)dict;
}

struct value_t *type_methods(const type_def *definition) {
    struct list_value_t *methods = newlistval();
    if (!definition)
        return (struct value_t *)methods;

    std::vector<std::string> names = structtype_method_names(definition);
    std::sort(names.begin(), names.end());

    for (const std::string &name : names) {
        std::shared_ptr<function_def> method =
            structtype_method(definition, name);
        if (!method)
            continue;
        list_add(methods, method_info_from_code(name, method.get()));
    }
    return (struct value_t *)methods;
}

struct value_t *protocol_methods(const protocol_def *definition) {
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

struct value_t *type_protocols(const type_def *definition) {
    struct list_value_t *protocols = newlistval();
    if (!definition)
        return (struct value_t *)protocols;

    std::vector<const protocol_def *> matched =
        structtype_protocols(definition);
    std::vector<std::string> names;
    names.reserve(matched.size());
    for (const protocol_def *protocol : matched) {
        if (!protocol)
            continue;
        names.push_back(protocol_name(protocol));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const std::string &name : names)
        list_add(protocols, newstrval(name.c_str()));
    return (struct value_t *)protocols;
}

struct value_t *builtin_protocols_list(const char *name) {
    struct list_value_t *protocols = newlistval();
    std::vector<const protocol_def *> matched =
        builtin_type_protocols(name);
    std::vector<std::string> names;
    names.reserve(matched.size());
    for (const protocol_def *protocol : matched) {
        if (protocol)
            names.push_back(protocol_name(protocol));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const std::string &protocol : names)
        list_add(protocols, newstrval(protocol.c_str()));
    return (struct value_t *)protocols;
}

struct value_t *value_builtin_protocols(const struct value_t *value) {
    struct list_value_t *protocols = newlistval();
    if (!value)
        return (struct value_t *)protocols;
    std::vector<const protocol_def *> matched;
    if (protocol_def *protocol =
            structtype_find_protocol_by_name(nullptr, "Iterable")) {
        if (protocol->builtin_kind == PROTOCOL_BUILTIN_ITERABLE &&
            value_satisfies_builtin_protocol(value, protocol))
            matched.push_back(protocol);
    }
    if (protocol_def *protocol =
            structtype_find_protocol_by_name(nullptr, "Hashable")) {
        if (protocol->builtin_kind == PROTOCOL_BUILTIN_HASHABLE &&
            value_satisfies_builtin_protocol(value, protocol))
            matched.push_back(protocol);
    }
    std::vector<std::string> names;
    names.reserve(matched.size());
    for (const protocol_def *protocol : matched)
        names.push_back(protocol_name(protocol));
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const std::string &protocol : names)
        list_add(protocols, newstrval(protocol.c_str()));
    return (struct value_t *)protocols;
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
                              const std::string &package,
                              const std::string &module,
                              const std::string &source,
                              bool exported,
                              struct value_t *fields,
                              struct value_t *variants,
                              struct value_t *methods,
                              struct value_t *protocols) {
    struct dict_value_t *dict = newdictval();
    dict_add(dict, "kind", newstrval(kind));
    dict_add(dict, "name", newstrval(name));
    dict_add(dict, "package", newstrval(package.c_str()));
    dict_add(dict, "module", newstrval(module.c_str()));
    dict_add(dict, "source", newstrval(source.c_str()));
    dict_add(dict, "exported", newboolval(exported));
    dict_add(dict, "fields", fields);
    dict_add(dict, "variants", variants);
    dict_add(dict, "methods", methods);
    dict_add(dict, "protocols", protocols);
    return (struct value_t *)dict;
}

struct value_t *make_type_info(const resolved_type &type) {
    switch (type.kind) {
        case RESOLVED_BUILTIN:
            return new_type_dict("builtin", type.builtin_name, "", "", "",
                                 true, new_string_list(), new_string_list(),
                                 new_string_list(),
                                 builtin_protocols_list(type.builtin_name));
        case RESOLVED_STRUCT: {
            const struct_def *def = type.structure;
            return new_type_dict("struct", def->name.c_str(), def->package,
                                 def->module, def->source, def->exported,
                                 struct_fields(def), new_string_list(),
                                 type_methods(def), type_protocols(def));
        }
        case RESOLVED_ENUM: {
            const enum_def *def = type.enumeration;
            return new_type_dict("enum", def->name.c_str(), def->package,
                                 def->module, def->source, def->exported,
                                 new_string_list(), enum_variants(def),
                                 type_methods(def), type_protocols(def));
        }
        case RESOLVED_PROTOCOL: {
            const protocol_def *def = type.protocol;
            return new_type_dict("proto", def->name.c_str(), def->package,
                                 def->module, def->source, def->exported,
                                 new_string_list(), new_string_list(),
                                 protocol_methods(def), new_string_list());
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

struct builtin_generic_def {
    const char *name;
    const char *canonical;
    std::vector<std::string> params;
    bool variadic;
    int min_args;
    int max_args;
    std::vector<std::string> defaults;
};

const std::vector<builtin_generic_def> &builtin_generics() {
    static const std::vector<builtin_generic_def> table = {
        {"list", "list", {"T"}, false, 1, 1, {}},
        {"dict", "dict", {"K", "V"}, false, 2, 2, {}},
        {"set", "set", {"T"}, false, 1, 1, {}},
        {"deque", "deque", {"T"}, false, 1, 1, {}},
        {"heap", "heap", {"T"}, false, 1, 1, {}},
        {"task", "task", {"T"}, false, 1, 1, {}},
        {"generator", "generator", {"T", "S"}, false, 1, 2, {"", "any"}},
        {"pair", "pair", {"A", "B"}, false, 2, 2, {}},
        {"result", "result", {"T", "E"}, false, 2, 2, {}},
        {"ref", "ref", {"T"}, false, 1, 1, {}},
        {"weak", "weak", {"T"}, false, 1, 1, {}},
        {"tuple", "tuple", {}, true, 0, -1, {}},
        {"union", "union", {}, true, 1, -1, {}},
    };
    return table;
}

const builtin_generic_def *find_builtin_generic(const std::string &name) {
    for (const builtin_generic_def &def : builtin_generics()) {
        if (name == def.name)
            return &def;
    }
    return NULL;
}

bool builtin_generic_arguments(const builtin_generic_def &def,
                               const type_descriptor &desc,
                               std::vector<std::string> &args,
                               bool &defaulted, std::string &error,
                               bool allow_incomplete = false) {
    const int count = (int)desc.args.size();
    if ((!def.variadic && count > def.max_args) ||
        (def.variadic && def.max_args >= 0 && count > def.max_args)) {
        if (def.min_args == def.max_args) {
            error = "type " + std::string(def.canonical) + " expects " +
                    std::to_string(def.min_args) +
                    " type argument(s), got " + std::to_string(count);
        } else if (def.variadic) {
            error = "type " + std::string(def.canonical) + " expects at most " +
                    std::to_string(def.max_args) +
                    " type argument(s), got " + std::to_string(count);
        } else {
            error = "type " + std::string(def.canonical) + " expects " +
                    std::to_string(def.min_args) + " to " +
                    std::to_string(def.max_args) +
                    " type argument(s), got " + std::to_string(count);
        }
        return false;
    }
    if (count < def.min_args && !allow_incomplete) {
        if (def.variadic) {
            error = "type " + std::string(def.canonical) + " expects at least " +
                    std::to_string(def.min_args) +
                    " type argument(s), got " + std::to_string(count);
        } else {
            error = "type " + std::string(def.canonical) + " expects " +
                    std::to_string(def.min_args) +
                    " type argument(s), got " + std::to_string(count);
        }
        return false;
    }
    args.clear();
    for (const type_descriptor_ptr &arg : desc.args)
        args.push_back(type_descriptor_display(*arg));
    if (!def.variadic && (int)args.size() < def.max_args) {
        for (std::size_t i = args.size();
             i < def.params.size() && i < def.defaults.size(); ++i) {
            if (!def.defaults[i].empty()) {
                args.push_back(def.defaults[i]);
                defaulted = true;
            }
        }
    }
    return true;
}

bool builtin_generic_instantiated(const builtin_generic_def &def,
                                  std::size_t argc) {
    if (def.variadic)
        return (int)argc >= def.min_args;
    return (int)argc == def.max_args;
}

const std::vector<std::string> *generic_params_of(
    const resolved_type &type) {
    if (type.kind == RESOLVED_STRUCT)
        return &type.structure->generic_params;
    if (type.kind == RESOLVED_ENUM)
        return &type.enumeration->generic_params;
    if (type.kind == RESOLVED_PROTOCOL)
        return &type.protocol->generic_params;
    if (type.kind == RESOLVED_BUILTIN && type.builtin_name) {
        const builtin_generic_def *def =
            find_builtin_generic(type.builtin_name);
        if (def)
            return &def->params;
    }
    return NULL;
}

const std::vector<std::string> *generic_defaults_of(
    const resolved_type &type) {
    if (type.kind == RESOLVED_STRUCT)
        return &type.structure->generic_defaults;
    if (type.kind == RESOLVED_ENUM)
        return &type.enumeration->generic_defaults;
    return NULL;
}

struct value_t *string_list_of(const std::vector<std::string> &items) {
    struct list_value_t *list = newlistval();
    for (const std::string &item : items)
        list_add(list, newstrval(item.c_str()));
    return (struct value_t *)list;
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
    if (type.kind == RESOLVED_PROTOCOL)
        return protocol_methods(type.protocol);
    const type_def *definition = as_type_def(type);
    if (definition)
        return type_methods(definition);
    return new_string_list();
}

struct value_t *resolved_protocols(const resolved_type &type) {
    if (type.kind == RESOLVED_BUILTIN)
        return builtin_protocols_list(type.builtin_name);
    const type_def *definition = as_type_def(type);
    if (definition)
        return type_protocols(definition);
    return new_string_list();
}

} // namespace

struct value_t *typeinfo_type_info(const char *importer_source,
                                   const char *importer_package,
                                   struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (type_arg && type_arg->type == T_string) {
        std::string text = ((struct string_value_t *)type_arg)->value;
        for (std::size_t pos = 0;
             (pos = text.find("::", pos)) != std::string::npos;)
            text.replace(pos, 2, ".");
        type_descriptor desc;
        std::string descriptor_error;
        if (parse_type_descriptor(text, desc, descriptor_error)) {
            std::string base = desc.package.empty()
                                   ? desc.name
                                   : desc.package + "." + desc.name;
            if (!report_resolve_error(
                    resolve_type_name_text(importer_source, importer_package,
                                           base, type, error),
                    error))
                return NULL;
            return make_type_info(type);
        }
    }
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_package, type_arg,
                                               type, error),
                              error))
        return NULL;
    return make_type_info(type);
}

struct value_t *typeinfo_fields(const char *importer_source,
                                const char *importer_package,
                                struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_package, type_arg,
                                               type, error),
                              error))
        return NULL;
    return resolved_fields(type);
}

struct value_t *typeinfo_variants(const char *importer_source,
                                  const char *importer_package,
                                  struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_package, type_arg,
                                               type, error),
                              error))
        return NULL;
    return resolved_variants(type);
}

struct value_t *typeinfo_methods(const char *importer_source,
                                 const char *importer_package,
                                 struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_package, type_arg,
                                               type, error),
                              error))
        return NULL;
    return resolved_methods(type);
}

struct value_t *typeinfo_protocols(const char *importer_source,
                                const char *importer_package,
                                struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_package, type_arg,
                                               type, error),
                              error))
        return NULL;
    if (type.kind == RESOLVED_BUILTIN && type_arg &&
        type_arg->type != T_string)
        return value_builtin_protocols(type_arg);
    return resolved_protocols(type);
}

struct value_t *typeinfo_implements(const char *importer_source,
                                    const char *importer_package,
                                    struct value_t *type_arg,
                                    struct value_t *protocol_arg) {
    resolved_type type;
    std::string error;
    if (!report_resolve_error(resolve_type_arg(importer_source,
                                               importer_package, type_arg,
                                               type, error),
                              error))
        return NULL;

    const protocol_def *protocol = NULL;
    if (!report_resolve_error(resolve_protocol_arg(importer_source,
                                                importer_package, protocol_arg,
                                                protocol, error),
                              error))
        return NULL;

    if (type.kind == RESOLVED_BUILTIN) {
        if (type_arg && type_arg->type != T_string &&
            protocol->builtin_kind != PROTOCOL_BUILTIN_NONE)
            return newboolval(
                value_satisfies_builtin_protocol(type_arg, protocol));
        return newboolval(
            builtin_type_satisfies_protocol(type.builtin_name, protocol));
    }

    const type_def *definition = as_type_def(type);
    if (!definition)
        return newboolval(false);
    return newboolval(structtype_has_protocol(definition, protocol));
}

struct value_t *typeinfo_has_type(const char *importer_source,
                                  const char *importer_package,
                                  struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    resolve_status status = resolve_type_arg(importer_source, importer_package,
                                             type_arg, type, error);
    if (status == RESOLVE_FOUND)
        return newboolval(true);
    if (status == RESOLVE_NOT_FOUND || status == RESOLVE_NOT_VISIBLE)
        return newboolval(false);
    yyerror("%s", error.c_str());
    return NULL;
}

namespace {

bool descriptor_from_string_arg(struct value_t *arg, type_descriptor &desc,
                                std::string &text, std::string &error) {
    if (!arg || arg->type != T_string) {
        error = "expected a type descriptor string";
        return false;
    }
    text = ((struct string_value_t *)arg)->value;
    if (!parse_type_descriptor(text, desc, error)) {
        error = "invalid type descriptor: " + error;
        return false;
    }
    return true;
}

std::string descriptor_base_text(const type_descriptor &desc) {
    if (desc.package.empty())
        return desc.name;
    return desc.package + "." + desc.name;
}

bool descriptor_default_args(const resolved_type &type,
                             std::vector<std::string> &args) {
    const std::vector<std::string> *params = generic_params_of(type);
    const std::vector<std::string> *defaults = generic_defaults_of(type);
    if (!params || params->empty() || !defaults ||
        defaults->size() != params->size())
        return false;
    for (const std::string &value : *defaults) {
        if (value.empty())
            return false;
    }
    args = *defaults;
    return true;
}

} // namespace

struct value_t *typeinfo_generic_info(const char *importer_source,
                                      const char *importer_package,
                                      struct value_t *type_arg) {
    type_descriptor desc;
    std::string text;
    std::string error;
    if (!descriptor_from_string_arg(type_arg, desc, text, error)) {
        yyerror("std.meta.generic_info: %s", error.c_str());
        return NULL;
    }

    if (const builtin_generic_def *builtin =
            find_builtin_generic(desc.name)) {
        std::vector<std::string> args;
        bool defaulted = false;
        if (!builtin_generic_arguments(*builtin, desc, args, defaulted,
                                       error, true)) {
            yyerror("std.meta.generic_info: %s", error.c_str());
            return NULL;
        }
        struct dict_value_t *dict = newdictval();
        dict_add(dict, "type", newstrval(text.c_str()));
        dict_add(dict, "name", newstrval(builtin->canonical));
        dict_add(dict, "kind", newstrval("builtin"));
        dict_add(dict, "package", newstrval(""));
        dict_add(dict, "module", newstrval(""));
        dict_add(dict, "source", newstrval(""));
        dict_add(dict, "exported", newboolval(false));
        dict_add(dict, "params", string_list_of(builtin->params));
        dict_add(dict, "args", string_list_of(args));
        dict_add(dict, "instantiated",
                 newboolval(builtin_generic_instantiated(
                     *builtin, args.size())));
        dict_add(dict, "defaulted", newboolval(defaulted));
        dict_add(dict, "variadic", newboolval(builtin->variadic));
        return (struct value_t *)dict;
    }

    resolved_type type;
    std::string base = descriptor_base_text(desc);
    if (!report_resolve_error(
            resolve_type_name_text(importer_source, importer_package, base,
                                   type, error),
            error))
        return NULL;

    const std::vector<std::string> *params = generic_params_of(type);
    static const std::vector<std::string> empty_params;
    if (!params)
        params = &empty_params;

    std::vector<std::string> args;
    bool defaulted = false;
    if (!desc.args.empty()) {
        if (desc.args.size() != params->size()) {
            yyerror("type %s expects %d type argument(s), got %d",
                    desc.name.c_str(), (int)params->size(),
                    (int)desc.args.size());
            return NULL;
        }
        for (const type_descriptor_ptr &arg : desc.args)
            args.push_back(type_descriptor_display(*arg));
    } else {
        defaulted = descriptor_default_args(type, args);
    }

    const char *kind = "builtin";
    std::string package;
    std::string module;
    std::string source;
    bool exported = false;
    if (type.kind == RESOLVED_STRUCT) {
        kind = "struct";
        package = type.structure->package;
        module = type.structure->module;
        source = type.structure->source;
        exported = type.structure->exported;
    } else if (type.kind == RESOLVED_ENUM) {
        kind = "enum";
        package = type.enumeration->package;
        module = type.enumeration->module;
        source = type.enumeration->source;
        exported = type.enumeration->exported;
    } else if (type.kind == RESOLVED_PROTOCOL) {
        kind = "proto";
        package = type.protocol->package;
        module = type.protocol->module;
        source = type.protocol->source;
        exported = type.protocol->exported;
    }

    bool instantiated = args.size() == params->size();
    struct dict_value_t *dict = newdictval();
    dict_add(dict, "type", newstrval(text.c_str()));
    dict_add(dict, "name", newstrval(desc.name.c_str()));
    dict_add(dict, "kind", newstrval(kind));
    dict_add(dict, "package", newstrval(package.c_str()));
    dict_add(dict, "module", newstrval(module.c_str()));
    dict_add(dict, "source", newstrval(source.c_str()));
    dict_add(dict, "exported", newboolval(exported));
    dict_add(dict, "params", string_list_of(*params));
    dict_add(dict, "args", string_list_of(args));
    dict_add(dict, "instantiated", newboolval(instantiated));
    dict_add(dict, "defaulted", newboolval(defaulted));
    dict_add(dict, "variadic", newboolval(false));
    return (struct value_t *)dict;
}

struct value_t *typeinfo_type_args(const char *importer_source,
                                   const char *importer_package,
                                   struct value_t *type_arg) {
    if (!type_arg || type_arg->type != T_string)
        return newnull();
    type_descriptor desc;
    std::string text;
    std::string error;
    if (!descriptor_from_string_arg(type_arg, desc, text, error)) {
        yyerror("std.meta.type_args: %s", error.c_str());
        return NULL;
    }

    if (const builtin_generic_def *builtin =
            find_builtin_generic(desc.name)) {
        std::vector<std::string> args;
        bool defaulted = false;
        if (!builtin_generic_arguments(*builtin, desc, args, defaulted,
                                       error, true)) {
            yyerror("std.meta.type_args: %s", error.c_str());
            return NULL;
        }
        if (!builtin_generic_instantiated(*builtin, args.size()))
            return newnull();
        return string_list_of(args);
    }

    resolved_type type;
    std::string base = descriptor_base_text(desc);
    if (!report_resolve_error(
            resolve_type_name_text(importer_source, importer_package, base,
                                   type, error),
            error))
        return NULL;

    const std::vector<std::string> *params = generic_params_of(type);
    static const std::vector<std::string> empty_params;
    if (!params)
        params = &empty_params;

    std::vector<std::string> args;
    if (!desc.args.empty()) {
        if (desc.args.size() != params->size()) {
            yyerror("type %s expects %d type argument(s), got %d",
                    desc.name.c_str(), (int)params->size(),
                    (int)desc.args.size());
            return NULL;
        }
        for (const type_descriptor_ptr &arg : desc.args)
            args.push_back(type_descriptor_display(*arg));
    } else if (!params->empty() && !descriptor_default_args(type, args)) {
        return newnull();
    }

    return string_list_of(args);
}

struct value_t *typeinfo_type_params(const char *importer_source,
                                     const char *importer_package,
                                     struct value_t *type_arg) {
    resolved_type type;
    std::string error;
    if (type_arg && type_arg->type == T_string) {
        type_descriptor desc;
        std::string text;
        if (!descriptor_from_string_arg(type_arg, desc, text, error)) {
            yyerror("std.meta.type_params: %s", error.c_str());
            return NULL;
        }
        if (const builtin_generic_def *builtin =
                find_builtin_generic(desc.name))
            return string_list_of(builtin->params);
        std::string base = descriptor_base_text(desc);
        if (!report_resolve_error(
                resolve_type_name_text(importer_source, importer_package,
                                       base, type, error),
                error))
            return NULL;
    } else if (!report_resolve_error(
                   resolve_type_arg(importer_source, importer_package,
                                    type_arg, type, error),
                   error)) {
        return NULL;
    }

    const std::vector<std::string> *params = generic_params_of(type);
    static const std::vector<std::string> empty_params;
    return string_list_of(params ? *params : empty_params);
}
