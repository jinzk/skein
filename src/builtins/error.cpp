#include "error.h"

#include "ast.h"
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
        default:
            yyerror("unknown error function");
            return NULL;
    }
}
