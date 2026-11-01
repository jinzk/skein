#include "result.h"

#include "ast.h"
#include "eval.h"
#include "eval_context.h"
#include "repl.h"
#include "valuetype.h"

#include <string>
#include <utility>
#include <vector>

namespace {

bool get_result(value_t *value, result_value_t *&result) {
    if (!value || value->type != T_result) {
        yyerror("expected a result, got %s", typename_of(value));
        return false;
    }
    result = (result_value_t *)value;
    return true;
}

value_t *result_ok(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.result.ok expects 1 argument");
        return NULL;
    }
    return newresultok(dupval(args[0]));
}

value_t *result_err(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.result.err expects 1 argument");
        return NULL;
    }
    return newresulterr(normalize_error(dupval(args[0]), "error"));
}

value_t *result_is_ok(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.result.is_ok expects 1 argument");
        return NULL;
    }
    result_value_t *result = NULL;
    if (!get_result(args[0], result))
        return NULL;
    return newboolval(result->ok);
}

value_t *result_is_err(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.result.is_err expects 1 argument");
        return NULL;
    }
    result_value_t *result = NULL;
    if (!get_result(args[0], result))
        return NULL;
    return newboolval(!result->ok);
}

value_t *result_unwrap(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.result.unwrap expects 1 argument");
        return NULL;
    }
    result_value_t *result = NULL;
    if (!get_result(args[0], result))
        return NULL;
    if (!result->ok) {
        eval_raise(dupval(result->error));
        return NULL;
    }
    return dupval(result->value);
}

value_t *result_unwrap_or(const std::vector<value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.result.unwrap_or expects 2 arguments");
        return NULL;
    }
    result_value_t *result = NULL;
    if (!get_result(args[0], result))
        return NULL;
    return result->ok ? dupval(result->value) : dupval(args[1]);
}

value_t *result_value(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.result.value expects 1 argument");
        return NULL;
    }
    result_value_t *result = NULL;
    if (!get_result(args[0], result))
        return NULL;
    return result->ok ? dupval(result->value) : newnull();
}

value_t *result_error(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.result.error expects 1 argument");
        return NULL;
    }
    result_value_t *result = NULL;
    if (!get_result(args[0], result))
        return NULL;
    return result->ok ? newnull() : dupval(result->error);
}

value_t *result_capture(const std::vector<value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.result.capture expects 1 argument");
        return NULL;
    }
    if (args[0]->type != T_func && args[0]->type != T_builtin &&
        args[0]->type != T_method) {
        yyerror("std.result.capture expects a callable");
        return NULL;
    }

    eval_capture_snapshot snapshot;
    eval_capture_begin(snapshot);
    value_t *result = call_value_args(args[0], {});
    eval_capture_result captured = eval_capture_end(snapshot);

    if (g_interrupted) {
        freeval(result);
        freeval(captured.error);
        return NULL;
    }
    if (captured.failed) {
        freeval(result);
        return newresulterr(captured.error);
    }
    if (active_eval_context().control == EVAL_CONTROL_CLOSE) {
        freeval(result);
        freeval(captured.error);
        return NULL;
    }
    if (!result)
        return newresulterr_message("runtime", "runtime error");
    return newresultok(result);
}

} // namespace

struct value_t *call_result_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_result_ok:
            return result_ok(args);
        case B_result_err:
            return result_err(args);
        case B_result_is_ok:
            return result_is_ok(args);
        case B_result_is_err:
            return result_is_err(args);
        case B_result_unwrap:
            return result_unwrap(args);
        case B_result_unwrap_or:
            return result_unwrap_or(args);
        case B_result_value:
            return result_value(args);
        case B_result_error:
            return result_error(args);
        case B_result_capture:
            return result_capture(args);
        default:
            yyerror("unknown result function");
            return NULL;
    }
}
