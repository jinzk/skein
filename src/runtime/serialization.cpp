#include "serialization.h"

#include "runtime_context.h"
#include "structtype.h"
#include "symbol.h"
#include "type_descriptor.h"
#include "unicode.h"
#include "valuetype.h"

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using type_bindings =
    std::unordered_map<std::string, type_descriptor_ptr>;

static struct value_t *dict_lookup_value(struct dict_value_t *dict,
                                         const char *key) {
    if (!dict || !key)
        return NULL;
    for (auto &entry : dict->payload->items) {
        if (entry.first && entry.first->type == T_string &&
            std::strcmp(((struct string_value_t *)entry.first)->value,
                        key) == 0)
            return entry.second;
    }
    return NULL;
}

static struct value_t *decode_nested_value(struct value_t *value, int depth,
                                           const std::string &path,
                                           bool defaults);
static struct value_t *decode_enum_object(struct value_t *value, int depth,
                                          const std::string &path,
                                          struct enum_def *expected,
                                          bool defaults,
                                          const type_bindings *bindings);
static struct value_t *decode_struct_object(struct value_t *value,
                                            struct_def *def, int depth,
                                            const std::string &path,
                                            bool defaults,
                                            const type_bindings *bindings);
static struct value_t *decode_value_for_type(
    struct value_t *value, const struct type_constraint *type, int depth,
    const std::string &path, bool defaults,
    const type_bindings *bindings);
static struct value_t *decode_value_for_descriptor(
    struct value_t *value, const type_descriptor &desc, int depth,
    const std::string &path, bool defaults);
static struct value_t *eval_field_default(const field_def &field,
                                          const char *owner,
                                          const char *field_name,
                                          const std::string &path);

static std::string join_path(const std::string &base,
                             const std::string &name) {
    return base.empty() ? name : base + "." + name;
}

static const char *decode_path_label(const std::string &path) {
    return path.empty() ? "value" : path.c_str();
}

static struct value_t *eval_field_default(const field_def &field,
                                          const char *owner,
                                          const char *field_name,
                                          const std::string &path) {
    if (!field.default_value || !field.default_value->code) {
        yyerror("field %s of %s has no default value",
                field_name ? field_name : "?",
                owner ? owner : "?");
        return NULL;
    }
    if (!skein_context_current() || !skein_context_current()->call) {
        yyerror("field default evaluation requires an interpreter context");
        return NULL;
    }

    struct func_value_t callee;
    callee.type = T_func;
    callee.def = field.default_value;
    struct value_t *value =
        skein_call_function((struct value_t *)&callee, {});
    if (!value)
        return NULL;
    if (value->type == T_return || value->type == T_break ||
        value->type == T_continue) {
        yyerror("default value for field %s of %s cannot return control flow",
                field_name ? field_name : "?",
                owner ? owner : "?");
        freeval(value);
        return NULL;
    }
    if (!type_constraint_matches(&field.type, value)) {
        yyerror("%s expects %s, got %s", decode_path_label(path),
                type_constraint_name(&field.type), typename_of(value));
        freeval(value);
        return NULL;
    }
    return value;
}

static struct value_t *decode_nested_value(struct value_t *value, int depth,
                                           const std::string &path,
                                           bool defaults) {
    if (!value)
        return NULL;
    if (depth > 512) {
        yyerror("maximum decode depth exceeded");
        return NULL;
    }

    if (value->type == T_dict) {
        struct dict_value_t *dict = (struct dict_value_t *)value;
        if (dict_lookup_value(dict, "$enum"))
            return decode_enum_object(value, depth, path, NULL, defaults,
                                      nullptr);
        struct dict_value_t *out = newdictval();
        for (auto &entry : dict->payload->items) {
            struct value_t *key = dupval(entry.first);
            struct value_t *item = decode_nested_value(
                entry.second, depth + 1, path, defaults);
            if (!item) {
                freeval(key);
                freeval((struct value_t *)out);
                return NULL;
            }
            out->payload->items.push_back(std::make_pair(key, item));
        }
        return (struct value_t *)out;
    }

    if (value->type == T_list) {
        struct list_value_t *list = (struct list_value_t *)value;
        struct list_value_t *out =
            list->payload && list->payload->frozen ? newtupleval()
                                                   : newlistval();
        for (struct value_t *item : list->payload->items) {
            struct value_t *decoded = decode_nested_value(
                item, depth + 1, path, defaults);
            if (!decoded) {
                freeval((struct value_t *)out);
                return NULL;
            }
            out->payload->items.push_back(decoded);
        }
        return (struct value_t *)out;
    }

    return dupval(value);
}

static struct value_t *decode_enum_object(struct value_t *value, int depth,
                                          const std::string &path,
                                          struct enum_def *expected,
                                          bool defaults,
                                          const type_bindings *bindings) {
    if (!value || value->type != T_dict) {
        yyerror("std.enum.decode expects an enum object");
        return NULL;
    }
    if (depth > 512) {
        yyerror("maximum decode depth exceeded");
        return NULL;
    }

    struct dict_value_t *dict = (struct dict_value_t *)value;
    struct value_t *tag_value = dict_lookup_value(dict, "$enum");
    if (!tag_value || tag_value->type != T_string) {
        yyerror("std.enum.decode expects an enum object");
        return NULL;
    }

    std::string tag = ((struct string_value_t *)tag_value)->value;

    /*
     * Current tags use `module.Type.Variant`; module names may themselves
     * contain dots, so split the suffix first.  The old `module::Type::Variant`
     * form is still accepted for data written by earlier versions.
     */
    std::vector<std::string> parts;
    if (tag.find("::") != std::string::npos) {
        size_t start = 0;
        for (;;) {
            size_t sep = tag.find("::", start);
            if (sep == std::string::npos) {
                parts.push_back(tag.substr(start));
                break;
            }
            parts.push_back(tag.substr(start, sep - start));
            start = sep + 2;
        }
    } else {
        size_t start = 0;
        for (;;) {
            size_t sep = tag.find('.', start);
            if (sep == std::string::npos) {
                parts.push_back(tag.substr(start));
                break;
            }
            parts.push_back(tag.substr(start, sep - start));
            start = sep + 1;
        }
    }

    std::string package_name;
    std::string type_name;
    std::string variant_name;
    if (parts.size() == 2) {
        type_name = parts[0];
        variant_name = parts[1];
    } else if (parts.size() >= 3) {
        variant_name = parts.back();
        type_name = parts[parts.size() - 2];
        for (size_t i = 0; i + 2 < parts.size(); ++i) {
            if (!package_name.empty())
                package_name += ".";
            package_name += parts[i];
        }
    } else {
        yyerror("invalid enum tag %s", tag.c_str());
        return NULL;
    }

    enum_def *def = expected;
    if (def) {
        if (type_name != def->name ||
            (!package_name.empty() && package_name != def->package)) {
            yyerror("enum tag %s does not match expected %s.%s",
                    tag.c_str(), def->package.c_str(),
                    def->name.c_str());
            return NULL;
        }
    } else {
        def = structtype_find_enum_by_name(
            package_name.empty() ? NULL : package_name.c_str(),
            type_name.c_str());
        if (!def) {
            yyerror("unknown or ambiguous enum type %s", type_name.c_str());
            return NULL;
        }
    }
    int variant_index = structtype_variant_index(def, variant_name);
    if (variant_index < 0) {
        yyerror("unknown enum variant %s.%s", type_name.c_str(),
                variant_name.c_str());
        return NULL;
    }
    const struct variant_def &variant =
        def->variants[(size_t)variant_index];

    struct value_t *payload_value = dict_lookup_value(dict, "payload");
    if (!payload_value || payload_value->type != T_dict) {
        yyerror("enum %s expects a payload object", tag.c_str());
        return NULL;
    }
    struct dict_value_t *payload_dict =
        (struct dict_value_t *)payload_value;

    std::shared_ptr<struct_payload> payload =
        std::make_shared<struct_payload>();
    payload->fields.resize(variant.fields.size(), NULL);

    for (size_t i = 0; i < variant.fields.size(); ++i) {
        const struct field_def &field = variant.fields[i];
        struct value_t *field_value =
            dict_lookup_value(payload_dict, field.name.c_str());
        const std::string field_path =
            join_path(path.empty() ? tag : path, field.name);
        if (!field_value) {
            if (defaults && field.has_default) {
                payload->fields[i] = eval_field_default(
                    field, def->name.c_str(), field.name.c_str(),
                    field_path);
                if (!payload->fields[i])
                    return NULL;
                continue;
            }
            if (field.type.nullable) {
                payload->fields[i] = newnull();
                continue;
            }
            yyerror("missing field %s in enum %s", field.name.c_str(),
                    tag.c_str());
            return NULL;
        }
        struct value_t *decoded = decode_value_for_type(
            field_value, &field.type, depth + 1, field_path, defaults,
            bindings);
        if (!decoded)
            return NULL;
        payload->fields[i] = decoded;
    }

    return newenumval(def, variant_index, payload);
}

static bool split_type_name(const std::string &text, std::string &prefix,
                            std::string &base) {
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

static struct_def *find_named_struct(const struct type_constraint *type) {
    if (!type || type->kind != TYPE_NAMED || !type->name)
        return NULL;
    struct_def *def = structtype_find_struct(type->source, type->name);
    if (def)
        return def;
    std::string name(type->name);
    std::string prefix;
    std::string base;
    if (!split_type_name(name, prefix, base))
        return NULL;
    return structtype_find_struct_by_name(prefix.c_str(), base.c_str());
}

static enum_def *find_named_enum(const struct type_constraint *type) {
    if (!type || type->kind != TYPE_NAMED || !type->name)
        return NULL;
    enum_def *def = structtype_find_enum(type->source, type->name);
    if (def)
        return def;
    std::string name(type->name);
    std::string prefix;
    std::string base;
    if (!split_type_name(name, prefix, base))
        return NULL;
    return structtype_find_enum_by_name(prefix.c_str(), base.c_str());
}

static struct value_t *decode_value_for_type(
    struct value_t *value, const struct type_constraint *type, int depth,
    const std::string &path, bool defaults,
    const type_bindings *bindings) {
    if (!value)
        return NULL;

    if (bindings && type && type->kind == TYPE_VAR && type->name) {
        auto binding = bindings->find(type->name);
        if (binding != bindings->end())
            return decode_value_for_descriptor(value, *binding->second,
                                               depth, path, defaults);
    }

    if (type && type->kind == TYPE_CHAR) {
        if (value->type == T_char)
            return dupval(value);
        if (value->type == T_string) {
            const char *text = ((struct string_value_t *)value)->value;
            size_t consumed = 0;
            unsigned int codepoint = 0;
            if (!utf8_decode_one(text, std::strlen(text), consumed,
                                 codepoint) ||
                text[consumed] != '\0') {
                yyerror("%s expects char, got string", decode_path_label(path));
                return NULL;
            }
            return newcharval(codepoint);
        }
        yyerror("%s expects char, got %s", decode_path_label(path),
                        typename_of(value));
        return NULL;
    }

    if (type && type->kind == TYPE_BYTE) {
        if (value->type == T_byte)
            return dupval(value);
        if (value->type == T_int) {
            long long number = ((struct int_value_t *)value)->value;
            if (number < 0 || number > 255) {
                yyerror("%s expects byte, got int out of range",
                        decode_path_label(path));
                return NULL;
            }
            return newbyteval((unsigned int)number);
        }
        yyerror("%s expects byte, got %s", decode_path_label(path),
                        typename_of(value));
        return NULL;
    }

    struct_def *sdef = find_named_struct(type);
    if (sdef) {
        if (value->type == T_dict) {
            struct dict_value_t *dict = (struct dict_value_t *)value;
            if (!dict_lookup_value(dict, "$enum"))
                return decode_struct_object(value, sdef, depth, path,
                                            defaults, bindings);
        }
        struct value_t *decoded = decode_nested_value(value, depth, path,
                                                      defaults);
        if (decoded && !type_constraint_matches(type, decoded)) {
            yyerror("%s expects %s, got %s", decode_path_label(path),
                    type_constraint_name(type), typename_of(decoded));
            freeval(decoded);
            return NULL;
        }
        return decoded;
    }

    enum_def *edef = find_named_enum(type);
    if (edef) {
        if (value->type == T_dict) {
            struct dict_value_t *dict = (struct dict_value_t *)value;
            if (dict_lookup_value(dict, "$enum"))
                return decode_enum_object(value, depth, path, edef,
                                          defaults, bindings);
        }
        struct value_t *decoded = decode_nested_value(value, depth, path,
                                                      defaults);
        if (decoded && !type_constraint_matches(type, decoded)) {
            yyerror("%s expects %s, got %s", decode_path_label(path),
                    type_constraint_name(type), typename_of(decoded));
            freeval(decoded);
            return NULL;
        }
        return decoded;
    }

    struct value_t *decoded = decode_nested_value(value, depth, path,
                                                  defaults);
    if (decoded && !type_constraint_matches(type, decoded)) {
        yyerror("%s expects %s, got %s", decode_path_label(path),
                type_constraint_name(type), typename_of(decoded));
        freeval(decoded);
        return NULL;
    }
    return decoded;
}

static struct value_t *decode_struct_object(struct value_t *value,
                                            struct_def *def, int depth,
                                            const std::string &path,
                                            bool defaults,
                                            const type_bindings *bindings) {
    if (!def) {
        yyerror("invalid struct type");
        return NULL;
    }
    if (!value || value->type != T_dict) {
        yyerror("std.struct.decode expects an object");
        return NULL;
    }
    if (depth > 512) {
        yyerror("maximum decode depth exceeded");
        return NULL;
    }

    struct dict_value_t *dict = (struct dict_value_t *)value;
    for (auto &entry : dict->payload->items) {
        if (!entry.first || entry.first->type != T_string) {
            yyerror("struct %s expects string keys", def->name.c_str());
            return NULL;
        }
        const char *key = ((struct string_value_t *)entry.first)->value;
        if (structtype_field_index(def, key) < 0) {
            yyerror("unknown field %s in %s", key, def->name.c_str());
            return NULL;
        }
    }

    std::shared_ptr<struct_payload> payload =
        std::make_shared<struct_payload>();
    payload->fields.resize(def->fields.size(), NULL);

    for (size_t i = 0; i < def->fields.size(); ++i) {
        const struct field_def &field = def->fields[i];
        struct value_t *field_value =
            dict_lookup_value(dict, field.name.c_str());
        const std::string field_path = join_path(path, field.name);
        if (!field_value) {
            if (defaults && field.has_default) {
                payload->fields[i] = eval_field_default(
                    field, def->name.c_str(), field.name.c_str(),
                    field_path);
                if (!payload->fields[i])
                    return NULL;
                continue;
            }
            if (field.type.nullable) {
                payload->fields[i] = newnull();
                continue;
            }
            yyerror("missing field %s in %s", field.name.c_str(),
                    def->name.c_str());
            return NULL;
        }
        struct value_t *decoded = decode_value_for_type(
            field_value, &field.type, depth + 1, field_path, defaults,
            bindings);
        if (!decoded)
            return NULL;
        payload->fields[i] = decoded;
    }

    return newstructval(def, payload);
}

static bool make_type_bindings(const type_descriptor &desc,
                               const std::vector<std::string> &params,
                               const std::vector<std::string> &defaults,
                               type_bindings &bindings,
                               std::string &error) {
    bindings.clear();
    if (!desc.args.empty()) {
        if (desc.args.size() != params.size()) {
            error = "type " + desc.name + " expects " +
                    std::to_string(params.size()) +
                    " type argument(s), got " +
                    std::to_string(desc.args.size());
            return false;
        }
        for (std::size_t i = 0; i < params.size(); ++i)
            bindings[params[i]] = desc.args[i];
        return true;
    }
    if (params.empty())
        return true;
    if (defaults.size() != params.size()) {
        error = "generic type " + desc.name + " requires " +
                std::to_string(params.size()) + " type argument(s)";
        return false;
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (defaults[i].empty()) {
            error = "generic type " + desc.name + " requires " +
                    std::to_string(params.size()) + " type argument(s)";
            return false;
        }
        type_descriptor_ptr default_desc =
            std::make_shared<type_descriptor>();
        if (!parse_type_descriptor(defaults[i], *default_desc, error)) {
            error = "invalid default type argument for " + params[i] +
                    ": " + error;
            return false;
        }
        bindings[params[i]] = std::move(default_desc);
    }
    return true;
}

static struct type_constraint descriptor_constraint(
    const type_descriptor &desc) {
    struct type_constraint constraint;
    constraint.kind = TYPE_INVALID;
    constraint.name = NULL;
    constraint.source = NULL;
    constraint.nullable = desc.nullable;
    constraint.arity = 0;
    constraint.display = NULL;
    constraint.detail = NULL;
    enum type_constraint_kind kind;
    if (type_constraint_kind_for_builtin_name(desc.name.c_str(), kind)) {
        constraint.kind = kind;
        return constraint;
    }
    constraint.kind = TYPE_NAMED;
    constraint.name = desc.name.c_str();
    constraint.source = desc.package.c_str();
    return constraint;
}

static struct value_t *decode_value_for_descriptor(
    struct value_t *value, const type_descriptor &desc, int depth,
    const std::string &path, bool defaults) {
    if (!value)
        return NULL;
    if (desc.nullable && value->type == T_null)
        return dupval(value);
    if (desc.name == "any")
        return decode_nested_value(value, depth, path, defaults);

    if (desc.name == "list" || desc.name == "deque" ||
        desc.name == "heap" || desc.name == "set") {
        struct type_constraint outer = descriptor_constraint(desc);
        if (!type_constraint_matches(&outer, value)) {
            yyerror("%s expects %s, got %s", decode_path_label(path),
                    type_descriptor_display(desc).c_str(),
                    typename_of(value));
            return NULL;
        }
        if (desc.name == "list" && value->type == T_list) {
            struct list_value_t *list = (struct list_value_t *)value;
            struct list_value_t *out =
                list->payload && list->payload->frozen ? newtupleval()
                                                       : newlistval();
            for (struct value_t *item : list->payload->items) {
                struct value_t *decoded =
                    !desc.args.empty()
                        ? decode_value_for_descriptor(item, *desc.args[0],
                                                       depth + 1, path,
                                                       defaults)
                        : decode_nested_value(item, depth + 1, path,
                                              defaults);
                if (!decoded) {
                    freeval((struct value_t *)out);
                    return NULL;
                }
                out->payload->items.push_back(decoded);
            }
            return (struct value_t *)out;
        }
        return decode_nested_value(value, depth, path, defaults);
    }

    if (desc.name == "dict") {
        struct type_constraint outer = descriptor_constraint(desc);
        if (!type_constraint_matches(&outer, value)) {
            yyerror("%s expects dict, got %s", decode_path_label(path),
                    typename_of(value));
            return NULL;
        }
        struct dict_value_t *dict = (struct dict_value_t *)value;
        struct dict_value_t *out = newdictval();
        for (auto &entry : dict->payload->items) {
            struct value_t *key =
                desc.args.size() > 0
                    ? decode_value_for_descriptor(entry.first, *desc.args[0],
                                                   depth + 1, path, defaults)
                    : decode_nested_value(entry.first, depth + 1, path,
                                          defaults);
            struct value_t *item =
                desc.args.size() > 1
                    ? decode_value_for_descriptor(entry.second, *desc.args[1],
                                                   depth + 1, path, defaults)
                    : decode_nested_value(entry.second, depth + 1, path,
                                          defaults);
            if (!key || !item) {
                freeval(key);
                freeval(item);
                freeval((struct value_t *)out);
                return NULL;
            }
            out->payload->items.push_back(std::make_pair(key, item));
        }
        return (struct value_t *)out;
    }

    if (desc.name == "pair" || desc.name == "tuple") {
        if (value->type != T_list) {
            yyerror("%s expects %s, got %s", decode_path_label(path),
                    desc.name.c_str(), typename_of(value));
            return NULL;
        }
        struct list_value_t *list = (struct list_value_t *)value;
        if (!list->payload || !list->payload->frozen ||
            list->payload->items.size() != desc.args.size()) {
            yyerror("%s expects %s with %d member(s)",
                    decode_path_label(path), desc.name.c_str(),
                    (int)desc.args.size());
            return NULL;
        }
        struct list_value_t *out = newtupleval();
        for (std::size_t i = 0; i < desc.args.size(); ++i) {
            struct value_t *decoded = decode_value_for_descriptor(
                list->payload->items[i], *desc.args[i], depth + 1, path,
                defaults);
            if (!decoded) {
                freeval((struct value_t *)out);
                return NULL;
            }
            out->payload->items.push_back(decoded);
        }
        return (struct value_t *)out;
    }

    enum type_constraint_kind kind;
    if (type_constraint_kind_for_builtin_name(desc.name.c_str(), kind)) {
        struct type_constraint constraint = descriptor_constraint(desc);
        return decode_value_for_type(value, &constraint, depth, path,
                                     defaults, nullptr);
    }

    struct_def *sdef = structtype_find_struct_by_name(
        desc.package.empty() ? NULL : desc.package.c_str(),
        desc.name.c_str());
    if (sdef) {
        type_bindings nested;
        std::string binding_error;
        if (!make_type_bindings(desc, sdef->generic_params,
                                sdef->generic_defaults, nested,
                                binding_error)) {
            yyerror("%s: %s", decode_path_label(path),
                    binding_error.c_str());
            return NULL;
        }
        if (value->type == T_dict) {
            struct dict_value_t *dict = (struct dict_value_t *)value;
            if (!dict_lookup_value(dict, "$enum"))
                return decode_struct_object(value, sdef, depth, path,
                                            defaults, &nested);
        }
        struct value_t *decoded = decode_nested_value(value, depth, path,
                                                      defaults);
        if (decoded && !named_type_matches(desc.package.c_str(),
                                           desc.name.c_str(), decoded)) {
            yyerror("%s expects %s, got %s", decode_path_label(path),
                    type_descriptor_display(desc).c_str(),
                    typename_of(decoded));
            freeval(decoded);
            return NULL;
        }
        return decoded;
    }

    enum_def *edef = structtype_find_enum_by_name(
        desc.package.empty() ? NULL : desc.package.c_str(),
        desc.name.c_str());
    if (edef) {
        type_bindings nested;
        std::string binding_error;
        if (!make_type_bindings(desc, edef->generic_params,
                                edef->generic_defaults, nested,
                                binding_error)) {
            yyerror("%s: %s", decode_path_label(path),
                    binding_error.c_str());
            return NULL;
        }
        if (value->type == T_dict) {
            struct dict_value_t *dict = (struct dict_value_t *)value;
            if (dict_lookup_value(dict, "$enum"))
                return decode_enum_object(value, depth, path, edef,
                                          defaults, &nested);
        }
        struct value_t *decoded = decode_nested_value(value, depth, path,
                                                      defaults);
        if (decoded && !named_type_matches(desc.package.c_str(),
                                           desc.name.c_str(), decoded)) {
            yyerror("%s expects %s, got %s", decode_path_label(path),
                    type_descriptor_display(desc).c_str(),
                    typename_of(decoded));
            freeval(decoded);
            return NULL;
        }
        return decoded;
    }

    yyerror("%s expects %s, got %s", decode_path_label(path),
            type_descriptor_display(desc).c_str(), typename_of(value));
    return NULL;
}

static struct value_t *make_typed_envelope(struct value_t *type_value,
                                           struct value_t *payload) {
    struct dict_value_t *env = newdictval();
    env->payload->items.push_back(std::make_pair(
        newstrval("$version"), newintval(1)));
    env->payload->items.push_back(std::make_pair(
        newstrval("$type"), dupval(type_value)));
    env->payload->items.push_back(std::make_pair(
        newstrval("payload"), dupval(payload)));
    return (struct value_t *)env;
}

struct value_t *struct_encode_value(struct value_t *value,
                                    struct value_t *type_value) {
    if (!type_value || type_value->type != T_string) {
        yyerror("std.struct.encode expects a type name string");
        return NULL;
    }
    if (!value || value->type != T_struct) {
        yyerror("std.struct.encode expects a struct value");
        return NULL;
    }
    std::string type_name = ((struct string_value_t *)type_value)->value;
    for (std::size_t pos = 0;
         (pos = type_name.find("::", pos)) != std::string::npos;)
        type_name.replace(pos, 2, ".");
    type_descriptor desc;
    std::string error;
    if (!parse_type_descriptor(type_name, desc, error)) {
        yyerror("std.struct.encode: %s", error.c_str());
        return NULL;
    }
    struct_def *def = structtype_find_struct_by_name(
        desc.package.empty() ? NULL : desc.package.c_str(),
        desc.name.c_str());
    if (!def) {
        yyerror("unknown or ambiguous struct type %s", desc.name.c_str());
        return NULL;
    }
    type_bindings bindings;
    if (!make_type_bindings(desc, def->generic_params,
                            def->generic_defaults, bindings, error)) {
        yyerror("std.struct.encode: %s", error.c_str());
        return NULL;
    }
    const struct struct_value_t *sv =
        (const struct struct_value_t *)value;
    if (sv->def != def) {
        yyerror("std.struct.encode value is %s, not %s",
                sv->def ? sv->def->name.c_str() : "invalid",
                def->name.c_str());
        return NULL;
    }
    return make_typed_envelope(type_value, value);
}

struct value_t *enum_encode_value(struct value_t *value,
                                  struct value_t *type_value) {
    if (!type_value || type_value->type != T_string) {
        yyerror("std.enum.encode expects a type name string");
        return NULL;
    }
    if (!value || value->type != T_enum) {
        yyerror("std.enum.encode expects an enum value");
        return NULL;
    }
    std::string type_name = ((struct string_value_t *)type_value)->value;
    for (std::size_t pos = 0;
         (pos = type_name.find("::", pos)) != std::string::npos;)
        type_name.replace(pos, 2, ".");
    type_descriptor desc;
    std::string error;
    if (!parse_type_descriptor(type_name, desc, error)) {
        yyerror("std.enum.encode: %s", error.c_str());
        return NULL;
    }
    enum_def *def = structtype_find_enum_by_name(
        desc.package.empty() ? NULL : desc.package.c_str(),
        desc.name.c_str());
    if (!def) {
        yyerror("unknown or ambiguous enum type %s", desc.name.c_str());
        return NULL;
    }
    type_bindings bindings;
    if (!make_type_bindings(desc, def->generic_params,
                            def->generic_defaults, bindings, error)) {
        yyerror("std.enum.encode: %s", error.c_str());
        return NULL;
    }
    const struct enum_value_t *ev = (const struct enum_value_t *)value;
    if (ev->def != def) {
        yyerror("std.enum.encode value is %s, not %s",
                ev->def ? ev->def->name.c_str() : "invalid",
                def->name.c_str());
        return NULL;
    }
    return make_typed_envelope(type_value, value);
}

static struct value_t *struct_decode(struct value_t *value,
                                     struct value_t *type_value,
                                     bool defaults) {
    if (value && value->type == T_dict) {
        struct dict_value_t *dict = (struct dict_value_t *)value;
        struct value_t *env_type = dict_lookup_value(dict, "$type");
        struct value_t *env_payload = dict_lookup_value(dict, "payload");
        struct value_t *env_version = dict_lookup_value(dict, "$version");
        if (env_version &&
            (env_version->type != T_int ||
             ((struct int_value_t *)env_version)->value != 1)) {
            yyerror("std.struct.decode unsupported envelope version");
            return NULL;
        }
        if (env_type && env_type->type == T_string && env_payload) {
            if (type_value && type_value->type == T_string) {
                const char *explicit_type =
                    ((struct string_value_t *)type_value)->value;
                const char *encoded_type =
                    ((struct string_value_t *)env_type)->value;
                if (std::strcmp(explicit_type, encoded_type) != 0) {
                    yyerror("std.struct.decode type %s does not match "
                            "envelope type %s",
                            explicit_type, encoded_type);
                    return NULL;
                }
            } else {
                type_value = env_type;
            }
            value = env_payload;
        }
    }
    if (!value ||
        (value->type != T_dict && value->type != T_struct)) {
        yyerror("std.struct.decode expects an object");
        return NULL;
    }
    if (!type_value || type_value->type != T_string) {
        yyerror("std.struct.decode expects a type name string");
        return NULL;
    }

    std::string type_name = ((struct string_value_t *)type_value)->value;
    for (std::size_t pos = 0;
         (pos = type_name.find("::", pos)) != std::string::npos;)
        type_name.replace(pos, 2, ".");
    type_descriptor desc;
    std::string descriptor_error;
    if (!parse_type_descriptor(type_name, desc, descriptor_error)) {
        yyerror("std.struct.decode: %s", descriptor_error.c_str());
        return NULL;
    }

    struct_def *def = structtype_find_struct_by_name(
        desc.package.empty() ? NULL : desc.package.c_str(),
        desc.name.c_str());
    if (!def) {
        yyerror("unknown or ambiguous struct type %s", desc.name.c_str());
        return NULL;
    }

    type_bindings bindings;
    if (!make_type_bindings(desc, def->generic_params,
                            def->generic_defaults, bindings,
                            descriptor_error)) {
        yyerror("std.struct.decode: %s", descriptor_error.c_str());
        return NULL;
    }
    if (value && value->type == T_struct) {
        const struct struct_value_t *sv =
            (const struct struct_value_t *)value;
        if (sv->def != def) {
            yyerror("std.struct.decode value is %s, not %s",
                    sv->def ? sv->def->name.c_str() : "invalid",
                    def->name.c_str());
            return NULL;
        }
        return dupval(value);
    }
    return decode_struct_object(value, def, 0, def->name, defaults,
                                &bindings);
}

struct value_t *enum_decode_value(struct value_t *value) {
    return enum_decode_value_ex(value, NULL, false);
}

struct value_t *enum_decode_value_ex(struct value_t *value,
                                     struct enum_def *expected,
                                     bool defaults) {
    if (!expected && value && value->type == T_dict) {
        struct dict_value_t *dict = (struct dict_value_t *)value;
        struct value_t *env_type = dict_lookup_value(dict, "$type");
        struct value_t *env_payload = dict_lookup_value(dict, "payload");
        if (env_type && env_type->type == T_string && env_payload)
            return enum_decode_typed_value(env_payload, env_type, defaults);
    }
    return decode_enum_object(value, 0, "", expected, defaults, nullptr);
}

struct value_t *enum_decode_typed_value(struct value_t *value,
                                        struct value_t *type_value,
                                        bool defaults) {
    if (!type_value || type_value->type != T_string) {
        yyerror("std.enum.decode expects a type name string");
        return NULL;
    }
    if (value && value->type == T_dict) {
        struct dict_value_t *dict = (struct dict_value_t *)value;
        struct value_t *env_type = dict_lookup_value(dict, "$type");
        struct value_t *env_payload = dict_lookup_value(dict, "payload");
        struct value_t *env_version = dict_lookup_value(dict, "$version");
        if (env_version &&
            (env_version->type != T_int ||
             ((struct int_value_t *)env_version)->value != 1)) {
            yyerror("std.enum.decode unsupported envelope version");
            return NULL;
        }
        if (env_type && env_type->type == T_string && env_payload) {
            const char *explicit_type =
                ((struct string_value_t *)type_value)->value;
            const char *encoded_type =
                ((struct string_value_t *)env_type)->value;
            if (std::strcmp(explicit_type, encoded_type) != 0) {
                yyerror("std.enum.decode type %s does not match envelope "
                        "type %s",
                        explicit_type, encoded_type);
                return NULL;
            }
            value = env_payload;
        }
    }
    std::string type_name = ((struct string_value_t *)type_value)->value;
    for (std::size_t pos = 0;
         (pos = type_name.find("::", pos)) != std::string::npos;)
        type_name.replace(pos, 2, ".");
    type_descriptor desc;
    std::string error;
    if (!parse_type_descriptor(type_name, desc, error)) {
        yyerror("std.enum.decode: %s", error.c_str());
        return NULL;
    }
    enum_def *def = structtype_find_enum_by_name(
        desc.package.empty() ? NULL : desc.package.c_str(),
        desc.name.c_str());
    if (!def) {
        yyerror("unknown or ambiguous enum type %s", desc.name.c_str());
        return NULL;
    }
    type_bindings bindings;
    if (!make_type_bindings(desc, def->generic_params,
                            def->generic_defaults, bindings, error)) {
        yyerror("std.enum.decode: %s", error.c_str());
        return NULL;
    }
    if (value && value->type == T_enum) {
        const struct enum_value_t *ev = (const struct enum_value_t *)value;
        if (ev->def != def) {
            yyerror("std.enum.decode value is %s, not %s",
                    ev->def ? ev->def->name.c_str() : "invalid",
                    def->name.c_str());
            return NULL;
        }
        return dupval(value);
    }
    return decode_enum_object(value, 0, "", def, defaults, &bindings);
}

struct value_t *struct_decode_value(struct value_t *value,
                                    struct value_t *type_value) {
    return struct_decode(value, type_value, false);
}

struct value_t *struct_decode_value_ex(struct value_t *value,
                                       struct value_t *type_value,
                                       bool defaults) {
    return struct_decode(value, type_value, defaults);
}
