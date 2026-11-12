#include "pattern.h"

#include "ast.h"
#include "eval.h"
#include "eval_helpers.h"
#include "structtype.h"
#include "symbol.h"
#include "valuetype.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

void free_pattern_bindings(std::vector<pattern_binding> &bindings) {
    for (auto &binding : bindings)
        freeval(binding.value);
    bindings.clear();
}

bool bind_pattern_value(std::vector<pattern_binding> &bindings,
                               const char *name, struct value_t *value) {
    if (!name || !*name || std::strcmp(name, "_") == 0)
        return true;
    for (auto &binding : bindings) {
        if (binding.name == name) {
            yyerror("pattern binds %s more than once", name);
            return false;
        }
    }
    pattern_binding binding;
    binding.name = name;
    binding.value = dupval(value);
    bindings.push_back(std::move(binding));
    return true;
}

static int dict_string_key_index(struct dict_value_t *dict, const char *key) {
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        struct value_t *item_key = dict->payload->items[i].first;
        if (item_key && item_key->type == T_string &&
            std::strcmp(((struct string_value_t *)item_key)->value, key) == 0)
            return (int)i;
    }
    return -1;
}

static bool pattern_has_key(struct pattern_field *fields, const char *key) {
    for (struct pattern_field *field = fields; field; field = field->next) {
        if (field->key && std::strcmp(field->key, key) == 0)
            return true;
    }
    return false;
}

bool match_pattern(struct ast *pattern_ast, struct value_t *value,
                          std::vector<pattern_binding> &bindings) {
    if (!pattern_ast || !value)
        return false;
    struct pattern_node *pattern = (struct pattern_node *)pattern_ast;

    switch (pattern->nodetype) {
        case NODE_PATTERN_LITERAL: {
            struct value_t *literal = eval(pattern->literal);
            if (!literal)
                return false;
            bool equal = value_equals(literal, value);
            freeval(literal);
            return equal;
        }
        case NODE_PATTERN_BINDING:
            return bind_pattern_value(bindings, pattern->name, value);
        case NODE_PATTERN_TYPE:
            return type_constraint_matches(pattern->type, value);
        case NODE_PATTERN_OR: {
            size_t mark = bindings.size();
            for (struct pattern_node *item = (struct pattern_node *)pattern->items;
                 item; item = (struct pattern_node *)item->next) {
                if (match_pattern((struct ast *)item, value, bindings))
                    return true;
                for (size_t i = mark; i < bindings.size(); ++i)
                    freeval(bindings[i].value);
                bindings.resize(mark);
            }
            return false;
        }
        case NODE_PATTERN_RANGE:
            return value->type == T_int &&
                   ((struct int_value_t *)value)->value >= pattern->range_start &&
                   ((struct int_value_t *)value)->value <= pattern->range_end;
        case NODE_PATTERN_REST:
            yyerror("rest pattern is only allowed at the end of a list "
                    "or dict pattern");
            return false;
        case NODE_PATTERN_AS:
            if (!match_pattern(pattern->items, value, bindings))
                return false;
            return bind_pattern_value(bindings, pattern->name, value);
        case NODE_PATTERN_NEVER:
            return false;
        case NODE_PATTERN_LIST: {
            if (value->type != T_list)
                return false;
            struct list_value_t *list = (struct list_value_t *)value;
            size_t index = 0;
            for (struct ast *item_ast = pattern->items; item_ast;
                 item_ast = ((struct pattern_node *)item_ast)->next) {
                struct pattern_node *item = (struct pattern_node *)item_ast;
                if (item->nodetype == NODE_PATTERN_REST) {
                    if (item->next) {
                        yyerror("rest pattern must be last");
                        return false;
                    }
                    struct list_value_t *rest = newlistval();
                    while (index < list->payload->items.size())
                        rest->payload->items.push_back(
                            dupval(list->payload->items[index++]));
                    bool ok = bind_pattern_value(bindings, item->name,
                                                 (struct value_t *)rest);
                    freeval((struct value_t *)rest);
                    return ok;
                }
                if (index >= list->payload->items.size())
                    return false;
                if (!match_pattern(item_ast, list->payload->items[index],
                                   bindings))
                    return false;
                ++index;
            }
            return index == list->payload->items.size();
        }
        case NODE_PATTERN_CONTAINER: {
            if (pattern->variant) {
                if (value->type != T_enum)
                    return false;
                struct enum_value_t *instance = (struct enum_value_t *)value;
                enum_def *def =
                    structtype_find_enum(pattern->source, pattern->name);
                if (!def || instance->def != def)
                    return false;
                int variant_index =
                    structtype_variant_index(def, pattern->variant);
                if (variant_index < 0) {
                    yyerror("unknown enum variant %s.%s", def->name.c_str(),
                            pattern->variant);
                    return false;
                }
                if (instance->variant != variant_index)
                    return false;
                const struct variant_def &variant =
                    def->variants[(size_t)variant_index];
                std::vector<bool> seen(variant.fields.size(), false);
                for (struct pattern_field *field = pattern->fields; field;
                     field = field->next) {
                    if (!field->key)
                        continue;
                    int index =
                        structtype_variant_field_index(&variant, field->key);
                    if (index < 0) {
                        yyerror("unknown field %s in %s.%s", field->key,
                                def->name.c_str(), variant.name.c_str());
                        return false;
                    }
                    if (seen[(size_t)index]) {
                        yyerror("field %s repeated in pattern", field->key);
                        return false;
                    }
                    seen[(size_t)index] = true;
                    if (!match_pattern(field->pattern,
                                       instance->payload->fields[(size_t)index],
                                       bindings))
                        return false;
                }
                for (struct pattern_field *field = pattern->fields; field;
                     field = field->next) {
                    if (field->key)
                        continue;
                    if (field->rest_name) {
                        struct dict_value_t *rest = newdictval();
                        for (size_t i = 0; i < variant.fields.size(); ++i) {
                            if (seen[i])
                                continue;
                            rest->payload->items.push_back(std::make_pair(
                                newstrval(variant.fields[i].name.c_str()),
                                dupval(instance->payload->fields[i])));
                        }
                        bool ok = bind_pattern_value(
                            bindings, field->rest_name,
                            (struct value_t *)rest);
                        freeval((struct value_t *)rest);
                        if (!ok)
                            return false;
                    }
                    break;
                }
                return true;
            }
            if (pattern->name) {
                if (value->type != T_struct)
                    return false;
                struct struct_value_t *instance = (struct struct_value_t *)value;
                struct_def *def =
                    structtype_find_struct(pattern->source, pattern->name);
                if (!def || instance->def != def)
                    return false;
                std::vector<bool> seen(def->fields.size(), false);
                for (struct pattern_field *field = pattern->fields; field;
                     field = field->next) {
                    if (!field->key)
                        continue;
                    int index = structtype_field_index(def, field->key);
                    if (index < 0) {
                        yyerror("unknown field %s in %s", field->key,
                                def->name.c_str());
                        return false;
                    }
                    if (seen[(size_t)index]) {
                        yyerror("field %s repeated in pattern", field->key);
                        return false;
                    }
                    seen[(size_t)index] = true;
                    if (!match_pattern(field->pattern,
                                       instance->payload->fields[(size_t)index],
                                       bindings))
                        return false;
                }
                for (struct pattern_field *field = pattern->fields; field;
                     field = field->next) {
                    if (field->key)
                        continue;
                    if (field->rest_name) {
                        struct dict_value_t *rest = newdictval();
                        for (size_t i = 0; i < def->fields.size(); ++i) {
                            if (seen[i])
                                continue;
                            rest->payload->items.push_back(std::make_pair(
                                newstrval(def->fields[i].name.c_str()),
                                dupval(instance->payload->fields[i])));
                        }
                        bool ok = bind_pattern_value(
                            bindings, field->rest_name,
                            (struct value_t *)rest);
                        freeval((struct value_t *)rest);
                        if (!ok)
                            return false;
                    }
                    break;
                }
                return true;
            }

            if (value->type != T_dict)
                return false;
            struct dict_value_t *dict = (struct dict_value_t *)value;
            for (struct pattern_field *field = pattern->fields; field;
                 field = field->next) {
                if (!field->key)
                    continue;
                int index = dict_string_key_index(dict, field->key);
                if (index < 0)
                    return false;
                if (!match_pattern(field->pattern,
                                   dict->payload->items[(size_t)index].second,
                                   bindings))
                    return false;
            }
            for (struct pattern_field *field = pattern->fields; field;
                 field = field->next) {
                if (field->key)
                    continue;
                if (field->rest_name) {
                    struct dict_value_t *rest = newdictval();
                    for (auto &entry : dict->payload->items) {
                        bool listed = false;
                        if (entry.first && entry.first->type == T_string)
                            listed = pattern_has_key(
                                pattern->fields,
                                ((struct string_value_t *)entry.first)->value);
                        if (!listed) {
                            rest->payload->items.push_back(std::make_pair(
                                dupval(entry.first), dupval(entry.second)));
                        }
                    }
                    bool ok = bind_pattern_value(
                        bindings, field->rest_name, (struct value_t *)rest);
                    freeval((struct value_t *)rest);
                    if (!ok)
                        return false;
                }
                break;
            }
            return true;
        }
        default:
            return false;
    }
}

static struct pattern_node *pattern_unwrap_as(struct ast *pattern_ast) {
    struct pattern_node *pattern = (struct pattern_node *)pattern_ast;
    while (pattern && pattern->nodetype == NODE_PATTERN_AS)
        pattern = (struct pattern_node *)pattern->items;
    return pattern;
}

static bool pattern_covers_all(enum_def *def, struct value_t *subject,
                              struct pattern_node *pattern) {
    pattern = pattern_unwrap_as((struct ast *)pattern);
    if (!pattern)
        return false;
    if (pattern->nodetype == NODE_PATTERN_BINDING)
        return true;
    if (pattern->nodetype == NODE_PATTERN_TYPE &&
        type_constraint_matches(pattern->type, subject))
        return true;
    if (pattern->nodetype == NODE_PATTERN_OR) {
        for (struct pattern_node *item = (struct pattern_node *)pattern->items;
             item; item = (struct pattern_node *)item->next) {
            if (pattern_covers_all(def, subject, item))
                return true;
        }
    }
    return false;
}

static void collect_enum_variants(enum_def *def,
                                  struct pattern_node *pattern,
                                  std::vector<bool> &covered);

static bool pattern_enum_variant(enum_def *def,
                                 struct pattern_node *pattern,
                                 int &variant) {
    if (!def || !pattern)
        return false;
    if (pattern->nodetype == NODE_PATTERN_CONTAINER && pattern->variant) {
        enum_def *pattern_def =
            structtype_find_enum(pattern->source, pattern->name);
        if (pattern_def != def)
            return false;
        variant = structtype_variant_index(def, pattern->variant);
        return variant >= 0;
    }
    if (pattern->nodetype == NODE_PATTERN_LITERAL && pattern->literal &&
        pattern->literal->nodetype == NODE_ENUM_LITERAL) {
        struct enum_lit_node *literal =
            (struct enum_lit_node *)pattern->literal;
        enum_def *pattern_def =
            structtype_find_enum(literal->source, literal->name);
        if (pattern_def != def)
            return false;
        variant = structtype_variant_index(def, literal->variant);
        return variant >= 0;
    }
    return false;
}

static void collect_enum_variants(enum_def *def,
                                  struct pattern_node *pattern,
                                  std::vector<bool> &covered) {
    pattern = pattern_unwrap_as((struct ast *)pattern);
    if (!pattern)
        return;
    if (pattern->nodetype == NODE_PATTERN_OR) {
        for (struct pattern_node *item = (struct pattern_node *)pattern->items;
             item; item = (struct pattern_node *)item->next)
            collect_enum_variants(def, item, covered);
        return;
    }
    int variant = -1;
    if (pattern_enum_variant(def, pattern, variant))
        covered[(size_t)variant] = true;
}

bool match_exhaustive_check(struct ast *arms, struct value_t *subject) {
    if (!subject || subject->type != T_enum)
        return true;

    struct enum_value_t *instance = (struct enum_value_t *)subject;
    enum_def *def = instance->def;
    if (!def || def->variants.empty())
        return true;

    std::vector<bool> covered(def->variants.size(), false);
    for (struct ast *arm_ast = arms; arm_ast;
         arm_ast = ((struct match_arm *)arm_ast)->next) {
        struct match_arm *arm = (struct match_arm *)arm_ast;
        if (!arm->pattern)
            return true; /* default */
        if (arm->guard)
            continue; /* a guarded arm is not a guarantee */

        struct pattern_node *pattern = pattern_unwrap_as(arm->pattern);
        if (!pattern)
            continue;
        if (pattern_covers_all(def, subject, pattern))
            return true;
        collect_enum_variants(def, pattern, covered);
    }

    std::string missing;
    for (size_t i = 0; i < def->variants.size(); ++i) {
        if (covered[i])
            continue;
        if (!missing.empty())
            missing += ", ";
        missing += def->name + "." + def->variants[i].name;
    }
    if (missing.empty())
        return true;

    yyerror("match is not exhaustive: missing %s", missing.c_str());
    return false;
}
