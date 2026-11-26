#include "error.h"

#include "ast.h"
#include "dict.h"
#include "eval.h"
#include "valuetype.h"
#include "value_ops.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {

bool get_error_value(value_t *value, error_value_t *&error) {
    if (!value || value->type != T_error) {
        yyerror("expected an error, got %s", typename_of(value));
        return false;
    }
    error = (error_value_t *)value;
    return true;
}

value_t *error_new(const std::vector<value_t *> &args) {
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.error.new expects 1 or 2 arguments");
        return NULL;
    }

    std::string kind = "error";
    value_t *message_value = args[0];
    if (args.size() == 2) {
        if (args[0]->type != T_string) {
            yyerror("std.error.new kind must be a string, got %s",
                    typename_of(args[0]));
            return NULL;
        }
        kind = ((string_value_t *)args[0])->value;
        message_value = args[1];
    }

    return newerrorval(kind, value_to_string(message_value));
}

value_t *error_kind(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.error.kind expects 1 argument");
        return NULL;
    }
    error_value_t *error = NULL;
    if (!get_error_value(args[0], error))
        return NULL;
    return newstrval(error->kind.c_str());
}

value_t *error_message(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.error.message expects 1 argument");
        return NULL;
    }
    error_value_t *error = NULL;
    if (!get_error_value(args[0], error))
        return NULL;
    return newstrval(error->message.c_str());
}

value_t *error_location(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.error.location expects 1 argument");
        return NULL;
    }
    error_value_t *error = NULL;
    if (!get_error_value(args[0], error))
        return NULL;
    if (error->line <= 0)
        return newnull();
    dict_value_t *location = newdictval();
    dict_append_item(location, newstrval("source"),
                     newstrval(error->source.c_str()));
    dict_append_item(location, newstrval("line"),
                     newintval(error->line));
    dict_append_item(location, newstrval("column"),
                     newintval(error->column));
    return (value_t *)location;
}

value_t *error_trace(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.error.trace expects 1 argument");
        return NULL;
    }
    error_value_t *error = NULL;
    if (!get_error_value(args[0], error))
        return NULL;
    list_value_t *frames = newlistval();
    frames->payload->items.reserve(error->trace.size());
    for (const error_frame &frame : error->trace) {
        dict_value_t *entry = newdictval();
        dict_append_item(entry, newstrval("source"),
                         newstrval(frame.source.c_str()));
        dict_append_item(entry, newstrval("function"),
                         newstrval(frame.function.c_str()));
        dict_append_item(entry, newstrval("line"), newintval(frame.line));
        dict_append_item(entry, newstrval("column"),
                         newintval(frame.column));
        frames->payload->items.push_back((value_t *)entry);
    }
    return (value_t *)frames;
}

value_t *error_format_trace(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.error.format_trace expects 1 argument");
        return NULL;
    }
    error_value_t *error = NULL;
    if (!get_error_value(args[0], error))
        return NULL;
    std::string text;
    if (!error->source.empty()) {
        text += error->source;
        text += ':';
    }
    if (error->line > 0) {
        text += std::to_string(error->line);
        if (error->column > 0) {
            text += ':';
            text += std::to_string(error->column);
        }
        text += ": ";
    }
    text += error->kind;
    text += ": ";
    text += error->message;
    for (const error_frame &frame : error->trace) {
        text += "\n  in ";
        text += frame.function;
        text += "()";
        if (frame.line > 0) {
            text += " called from line ";
            text += std::to_string(frame.line);
        }
    }
    return newstrval(text.c_str());
}

constexpr int kMaxSchemaDepth = 32;

bool known_schema_type(const std::string &name) {
    return name == "any" || name == "null" || name == "bool" ||
           name == "int" || name == "float" || name == "number" ||
           name == "string" || name == "char" || name == "byte" ||
           name == "bytes" || name == "list" || name == "dict" ||
           name == "set" || name == "deque" || name == "heap" ||
           name == "error" || name == "result" || name == "function" ||
           name == "func" || name == "builtin" || name == "method" ||
           name == "callable" || name == "userdata" || name == "struct" ||
           name == "enum";
}

bool matches_schema_type(const value_t *value, const std::string &name) {
    if (name == "any")
        return true;
    if (!value)
        return name == "null";
    switch (value->type) {
        case T_null:
            return name == "null";
        case T_bool:
            return name == "bool";
        case T_int:
            return name == "int" || name == "number";
        case T_float:
            return name == "float" || name == "number";
        case T_string:
            return name == "string";
        case T_char:
            return name == "char";
        case T_byte:
            return name == "byte";
        case T_bytes:
            return name == "bytes";
        case T_list:
            return name == "list";
        case T_dict:
            return name == "dict";
        case T_set:
            return name == "set";
        case T_deque:
            return name == "deque";
        case T_heap:
            return name == "heap";
        case T_error:
            return name == "error";
        case T_result:
            return name == "result";
        case T_func:
            return name == "function" || name == "func" ||
                   name == "callable";
        case T_builtin:
        case T_native:
            return name == "builtin" || name == "callable";
        case T_method:
            return name == "method" || name == "callable";
        case T_userdata:
            return name == "userdata";
        case T_struct:
            return name == "struct";
        case T_enum:
            return name == "enum";
        default:
            return false;
    }
}

bool validate_schema(value_t *value, value_t *spec,
                     const std::string &path, std::string &error,
                     int depth) {
    if (depth > kMaxSchemaDepth) {
        error = "schema nesting is too deep at " + path;
        return false;
    }
    if (!spec) {
        error = "schema is null at " + path;
        return false;
    }

    if (spec->type == T_string) {
        const std::string type_spec =
            ((const string_value_t *)spec)->value;
        std::size_t begin = 0;
        bool any_match = false;
        while (begin <= type_spec.size()) {
            std::size_t end = type_spec.find('|', begin);
            if (end == std::string::npos)
                end = type_spec.size();
            std::string name = type_spec.substr(begin, end - begin);
            while (!name.empty() && name.front() == ' ')
                name.erase(name.begin());
            while (!name.empty() && name.back() == ' ')
                name.pop_back();
            if (!known_schema_type(name)) {
                error = "unknown schema type \"" + name + "\" at " + path;
                return false;
            }
            if (matches_schema_type(value, name))
                any_match = true;
            if (end == type_spec.size())
                break;
            begin = end + 1;
        }
        if (!any_match) {
            error = "expected " + type_spec + " at " + path + ", got " +
                    typename_of(value);
            return false;
        }
        return true;
    }

    if (spec->type == T_list) {
        if (!value || value->type != T_list) {
            error = "expected list at " + path + ", got " +
                    typename_of(value);
            return false;
        }
        const std::vector<value_t *> &items =
            ((const list_value_t *)spec)->payload->items;
        const std::vector<value_t *> &values =
            ((const list_value_t *)value)->payload->items;
        if (items.empty())
            return true;
        if (items.size() == 1) {
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (!validate_schema(values[i], items[0],
                                     path + "[" + std::to_string(i) + "]",
                                     error, depth + 1))
                    return false;
            }
            return true;
        }
        if (values.size() != items.size()) {
            error = "expected list of length " +
                    std::to_string(items.size()) + " at " + path +
                    ", got " + std::to_string(values.size());
            return false;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!validate_schema(values[i], items[i],
                                 path + "[" + std::to_string(i) + "]",
                                 error, depth + 1))
                return false;
        }
        return true;
    }

    if (spec->type == T_dict) {
        if (!value || value->type != T_dict) {
            error = "expected dict at " + path + ", got " +
                    typename_of(value);
            return false;
        }
        const dict_value_t *value_dict = (const dict_value_t *)value;
        const dict_value_t *spec_dict = (const dict_value_t *)spec;
        for (const auto &entry : spec_dict->payload->items) {
            if (!entry.first || entry.first->type != T_string) {
                error = "schema field names must be strings at " + path;
                return false;
            }
            const std::string key =
                ((const string_value_t *)entry.first)->value;
            const bool optional = !key.empty() && key.back() == '?';
            const std::string field =
                optional ? key.substr(0, key.size() - 1) : key;
            value_t *field_key = newstrval(field.c_str());
            const int index = dict_find_index(value_dict, field_key);
            freeval(field_key);
            if (index < 0) {
                if (optional)
                    continue;
                error = "missing field \"" + field + "\" at " + path;
                return false;
            }
            const std::string child_path =
                path.empty() ? field : path + "." + field;
            if (!validate_schema(value_dict->payload->items[index].second,
                                 entry.second, child_path, error, depth + 1))
                return false;
        }
        return true;
    }

    error = "schema at " + path +
            " must be a type name, list, or dict";
    return false;
}

void raise_assert(const std::string &message) {
    eval_raise(newerrorval("assert", message.c_str()));
}

} // namespace

struct value_t *call_error_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_error_new:
            return error_new(args);
        case B_error_kind:
            return error_kind(args);
        case B_error_message:
            return error_message(args);
        case B_error_trace:
            return error_trace(args);
        case B_error_location:
            return error_location(args);
        case B_error_format_trace:
            return error_format_trace(args);
        case B_assert_that: {
            if (args.size() < 1 || args.size() > 2) {
                yyerror("std.assert.that expects 1 or 2 arguments");
                return NULL;
            }
            if (truthy(args[0]))
                return newboolval(true);
            const std::string message =
                args.size() == 2 ? value_to_string(args[1])
                                  : std::string("assertion failed");
            raise_assert(message);
            return NULL;
        }
        case B_assert_schema: {
            if (args.size() != 2) {
                yyerror("std.assert.schema expects 2 arguments");
                return NULL;
            }
            std::string failure;
            if (!validate_schema(args[0], args[1], "value", failure, 0)) {
                raise_assert(failure);
                return NULL;
            }
            return newboolval(true);
        }
        default:
            yyerror("unknown error function");
            return NULL;
    }
}
