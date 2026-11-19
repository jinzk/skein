#include "error.h"

#include "ast.h"
#include "dict.h"
#include "valuetype.h"

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
        default:
            yyerror("unknown error function");
            return NULL;
    }
}
