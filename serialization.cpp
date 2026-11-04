#include "serialization.h"

#include "structtype.h"
#include "symbol.h"
#include "unicode.h"
#include "valuetype.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
                                           const std::string &path);
static struct value_t *decode_enum_object(struct value_t *value, int depth,
                                          const std::string &path);
static struct value_t *decode_struct_object(struct value_t *value,
                                            struct_def *def, int depth,
                                            const std::string &path);
static struct value_t *decode_value_for_type(
    struct value_t *value, const struct type_constraint *type, int depth,
    const std::string &path);

static std::string join_path(const std::string &base,
                             const std::string &name) {
    return base.empty() ? name : base + "." + name;
}

static const char *decode_path_label(const std::string &path) {
    return path.empty() ? "value" : path.c_str();
}

static struct value_t *decode_nested_value(struct value_t *value, int depth,
                                           const std::string &path) {
    if (!value)
        return NULL;
    if (depth > 512) {
        yyerror("maximum decode depth exceeded");
        return NULL;
    }

    if (value->type == T_dict) {
        struct dict_value_t *dict = (struct dict_value_t *)value;
        if (dict_lookup_value(dict, "$enum"))
            return decode_enum_object(value, depth, path);
        struct dict_value_t *out = newdictval();
        for (auto &entry : dict->payload->items) {
            struct value_t *key = dupval(entry.first);
            struct value_t *item = decode_nested_value(entry.second, depth + 1, path);
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
        struct list_value_t *out = newlistval();
        for (struct value_t *item : list->payload->items) {
            struct value_t *decoded = decode_nested_value(item, depth + 1, path);
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
                                          const std::string &path) {
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
    std::vector<std::string> parts;
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

    std::string module_name;
    std::string type_name;
    std::string variant_name;
    if (parts.size() == 2) {
        type_name = parts[0];
        variant_name = parts[1];
    } else if (parts.size() == 3) {
        module_name = parts[0];
        type_name = parts[1];
        variant_name = parts[2];
    } else {
        yyerror("invalid enum tag %s", tag.c_str());
        return NULL;
    }

    enum_def *def = structtype_find_enum_by_name(
        module_name.empty() ? NULL : module_name.c_str(),
        type_name.c_str());
    if (!def) {
        yyerror("unknown or ambiguous enum type %s", type_name.c_str());
        return NULL;
    }
    int variant_index = structtype_variant_index(def, variant_name);
    if (variant_index < 0) {
        yyerror("unknown enum variant %s::%s", type_name.c_str(),
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
        if (!field_value) {
            yyerror("missing field %s in enum %s", field.name.c_str(),
                    tag.c_str());
            return NULL;
        }
        struct value_t *decoded = decode_value_for_type(
            field_value, &field.type, depth + 1,
            join_path(path.empty() ? tag : path, field.name));
        if (!decoded)
            return NULL;
        payload->fields[i] = decoded;
    }

    return newenumval(def, variant_index, payload);
}

static struct_def *find_named_struct(const struct type_constraint *type) {
    if (!type || type->kind != TYPE_NAMED || !type->name)
        return NULL;
    struct_def *def = structtype_find_struct(type->module, type->name);
    if (def)
        return def;
    std::string name(type->name);
    size_t sep = name.find("::");
    if (sep == std::string::npos || sep == 0 || sep + 2 >= name.size())
        return NULL;
    return structtype_find_struct_by_name(
        name.substr(0, sep).c_str(), name.substr(sep + 2).c_str());
}

static enum_def *find_named_enum(const struct type_constraint *type) {
    if (!type || type->kind != TYPE_NAMED || !type->name)
        return NULL;
    enum_def *def = structtype_find_enum(type->module, type->name);
    if (def)
        return def;
    std::string name(type->name);
    size_t sep = name.find("::");
    if (sep == std::string::npos || sep == 0 || sep + 2 >= name.size())
        return NULL;
    return structtype_find_enum_by_name(
        name.substr(0, sep).c_str(), name.substr(sep + 2).c_str());
}

static struct value_t *decode_value_for_type(
    struct value_t *value, const struct type_constraint *type, int depth,
    const std::string &path) {
    if (!value)
        return NULL;

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
                return decode_struct_object(value, sdef, depth, path);
        }
        struct value_t *decoded = decode_nested_value(value, depth, path);
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
        struct value_t *decoded = decode_nested_value(value, depth, path);
        if (decoded && !type_constraint_matches(type, decoded)) {
            yyerror("%s expects %s, got %s", decode_path_label(path),
                    type_constraint_name(type), typename_of(decoded));
            freeval(decoded);
            return NULL;
        }
        return decoded;
    }

    struct value_t *decoded = decode_nested_value(value, depth, path);
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
                                            const std::string &path) {
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
        if (!field_value) {
            if (field.type.nullable) {
                payload->fields[i] = newnull();
                continue;
            }
            yyerror("missing field %s in %s", field.name.c_str(),
                    def->name.c_str());
            return NULL;
        }
        struct value_t *decoded = decode_value_for_type(
            field_value, &field.type, depth + 1,
            join_path(path, field.name));
        if (!decoded)
            return NULL;
        payload->fields[i] = decoded;
    }

    return newstructval(def, payload);
}

static struct value_t *struct_decode(struct value_t *value,
                                     struct value_t *type_value) {
    if (!value || value->type != T_dict) {
        yyerror("std.struct.decode expects an object");
        return NULL;
    }
    if (!type_value || type_value->type != T_string) {
        yyerror("std.struct.decode expects a type name string");
        return NULL;
    }

    std::string type_name = ((struct string_value_t *)type_value)->value;
    std::string module_name;
    size_t sep = type_name.find("::");
    if (sep != std::string::npos) {
        module_name = type_name.substr(0, sep);
        type_name = type_name.substr(sep + 2);
    }

    struct_def *def = structtype_find_struct_by_name(
        module_name.empty() ? NULL : module_name.c_str(),
        type_name.c_str());
    if (!def) {
        yyerror("unknown or ambiguous struct type %s", type_name.c_str());
        return NULL;
    }
    return decode_struct_object(value, def, 0, def->name);
}

struct value_t *enum_decode_value(struct value_t *value) {
    return decode_enum_object(value, 0, "");
}

struct value_t *struct_decode_value(struct value_t *value,
                                    struct value_t *type_value) {
    return struct_decode(value, type_value);
}
