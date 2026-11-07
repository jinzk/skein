#include "builtins_internal.h"

#include "runtime_context.h"
#include "valuetype.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace {

bool callable_value(const struct value_t *value) {
    if (!value)
        return false;
    return value->type == T_func || value->type == T_builtin ||
           value->type == T_method || value->type == T_native;
}

struct native_capture {
    std::vector<struct value_t *> values;

    ~native_capture() {
        for (struct value_t *value : values)
            freeval(value);
    }
};

struct value_t *make_closure(
    std::function<struct value_t *(
        const std::vector<struct value_t *> &captured,
        const std::vector<struct value_t *> &args)> call,
    const std::vector<struct value_t *> &captured) {
    auto context = std::make_shared<native_capture>();
    context->values.reserve(captured.size());
    for (struct value_t *value : captured)
        context->values.push_back(dupval(value));

    struct value_t *result = newnativeval(
        [context, call](const std::vector<struct value_t *> &args) {
            return call(context->values, args);
        },
        context);

    struct native_function_value *native =
        (struct native_function_value *)result;
    native->children.reserve(context->values.size());
    for (struct value_t *value : context->values)
        native->children.push_back(dupval(value));
    return result;
}

std::vector<struct value_t *> duplicate_args(
    const std::vector<struct value_t *> &args) {
    std::vector<struct value_t *> copy;
    copy.reserve(args.size());
    for (struct value_t *value : args)
        copy.push_back(dupval(value));
    return copy;
}

struct value_t *compose_call(
    const std::vector<struct value_t *> &captured,
    const std::vector<struct value_t *> &args) {
    if (captured.empty())
        return NULL;
    std::vector<struct value_t *> first = duplicate_args(args);
    struct value_t *current =
        skein_call_function(captured.back(), std::move(first));
    if (!current)
        return NULL;
    for (int i = (int)captured.size() - 2; i >= 0; --i) {
        std::vector<struct value_t *> one;
        one.push_back(current);
        current = skein_call_function(captured[(size_t)i], std::move(one));
        if (!current)
            return NULL;
    }
    return current;
}

struct value_t *pipe_call(
    const std::vector<struct value_t *> &captured,
    const std::vector<struct value_t *> &args) {
    if (captured.empty())
        return NULL;
    std::vector<struct value_t *> first = duplicate_args(args);
    struct value_t *current =
        skein_call_function(captured.front(), std::move(first));
    if (!current)
        return NULL;
    for (size_t i = 1; i < captured.size(); ++i) {
        std::vector<struct value_t *> one;
        one.push_back(current);
        current = skein_call_function(captured[i], std::move(one));
        if (!current)
            return NULL;
    }
    return current;
}

struct value_t *fn_compose_builtin(const std::vector<struct value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.fn.compose expects exactly 2 functions");
        return NULL;
    }
    for (struct value_t *value : args) {
        if (!callable_value(value)) {
            yyerror("std.fn.compose expects callables, got %s",
                    typename_of(value));
            return NULL;
        }
    }
    return make_closure(compose_call, args);
}

struct value_t *fn_pipe_builtin(const std::vector<struct value_t *> &args) {
    if (args.size() != 2) {
        yyerror("std.fn.pipe expects exactly 2 functions");
        return NULL;
    }
    for (struct value_t *value : args) {
        if (!callable_value(value)) {
            yyerror("std.fn.pipe expects callables, got %s",
                    typename_of(value));
            return NULL;
        }
    }
    return make_closure(pipe_call, args);
}

struct value_t *fn_partial_builtin(const std::vector<struct value_t *> &args) {
    if (args.empty()) {
        yyerror("std.fn.partial expects a callable");
        return NULL;
    }
    if (!callable_value(args[0])) {
        yyerror("std.fn.partial expects a callable, got %s",
                typename_of(args[0]));
        return NULL;
    }
    return make_closure(
        [](const std::vector<struct value_t *> &captured,
           const std::vector<struct value_t *> &call_args) {
            if (captured.empty())
                return (struct value_t *)NULL;
            std::vector<struct value_t *> combined;
            combined.reserve(captured.size() + call_args.size());
            for (size_t i = 1; i < captured.size(); ++i)
                combined.push_back(dupval(captured[i]));
            for (struct value_t *value : call_args)
                combined.push_back(dupval(value));
            return skein_call_function(captured[0], std::move(combined));
        },
        args);
}

struct value_t *fn_identity_builtin(
    const std::vector<struct value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.fn.identity expects 1 argument");
        return NULL;
    }
    return dupval(args[0]);
}

struct value_t *fn_always_builtin(const std::vector<struct value_t *> &args) {
    if (args.size() != 1) {
        yyerror("std.fn.always expects 1 argument");
        return NULL;
    }
    return make_closure(
        [](const std::vector<struct value_t *> &captured,
           const std::vector<struct value_t *> &) {
            return captured.empty() ? (struct value_t *)NULL
                                    : dupval(captured[0]);
        },
        args);
}

} // namespace

struct value_t *call_fn_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_fn_compose:
            return fn_compose_builtin(args);
        case B_fn_pipe:
            return fn_pipe_builtin(args);
        case B_fn_partial:
            return fn_partial_builtin(args);
        case B_fn_identity:
            return fn_identity_builtin(args);
        case B_fn_always:
            return fn_always_builtin(args);
        default:
            yyerror("unknown std.fn function");
            return NULL;
    }
}
