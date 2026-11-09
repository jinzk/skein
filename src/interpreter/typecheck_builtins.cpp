#include "typecheck_builtins.h"

#include "ast.h"
#include "builtin_ids.h"
#include "typecheck_types.h"

#include <cstdio>
#include <initializer_list>

namespace skein {
namespace typecheck {
namespace detail {

namespace {

const TypeNode *container_type(const TypeNode *type,
                               enum type_constraint_kind kind,
                               std::size_t arity) {
    if (type && type->kind == kind)
        return type;
    std::vector<const TypeNode *> args(arity, type_any());
    return type_container(kind, args);
}

const TypeNode *expected_element(const TypeNode *expected,
                                 enum type_constraint_kind kind,
                                 std::size_t index) {
    if (expected && expected->kind == kind &&
        index < expected->args.size())
        return expected->args[index];
    return type_any();
}

} // namespace

bool check_builtin(BuiltinContext &context, const std::string &path,
               const std::vector<struct ast *> &args,
               const TypeNode *expected, Env &env,
               const TypeNode *&result) {
    auto arg_type = [&](std::size_t i) -> const TypeNode * {
        return i < args.size() ? context.infer(args[i], env) : type_any();
    };
    auto check_arg = [&](std::size_t i, const TypeNode *type,
                         bool force) -> bool {
        return i >= args.size() ||
               context.check_expr(args[i], type, env, false, force);
    };
    auto function_type = [](std::initializer_list<const TypeNode *> params,
                            const TypeNode *return_type) {
        std::vector<const TypeNode *> types(params);
        types.push_back(return_type);
        return type_container(TYPE_CALLABLE, types);
    };
    auto require_kind = [&](std::size_t i,
                            enum type_constraint_kind kind) -> bool {
        if (i >= args.size())
            return true;
        const TypeNode *actual = context.infer(args[i], env);
        if (actual->kind == TYPE_ANY || actual->kind == kind)
            return true;
        const TypeNode *wanted = type_builtin(kind);
        char message[256];
        std::snprintf(message, sizeof(message), "expected %s, got %s",
                      type_display(wanted), type_display(actual));
        context.report_error(args[i]->line, args[i]->column, message);
        return false;
    };

    if (path == "std.generator.next" ||
        path == "std.generator.done" ||
        path == "std.generator.close") {
        const TypeNode *actual =
            args.empty() ? type_any() : context.infer(args[0], env);
        if (actual->kind == TYPE_GENERATOR) {
            const TypeNode *element = type_element(actual, 0);
            if (path == "std.generator.next") {
                if (args.size() > 1) {
                    const TypeNode *send = type_element(actual, 1);
                    if (!type_is_any(send) &&
                        !context.check_expr(args[1], send, env, false,
                                            true))
                        return true;
                }
                result = type_container(
                    TYPE_DICT, {type_builtin(TYPE_STRING), element});
            } else {
                result = type_builtin(TYPE_BOOL);
            }
        } else {
            /* Keep unknown/null generator values gradual. */
            result = type_any();
        }
        return true;
    }

    if (path == "std.iter.take" || path == "std.iter.drop") {
        const TypeNode *element = args.empty()
                                      ? type_any()
                                      : context.infer_iterable_element(
                                            args[0], false, env);
        result = type_container(TYPE_GENERATOR, {element, type_any()});
        return true;
    }

    if (path == "std.iter.enumerate") {
        const TypeNode *element = args.empty()
                                      ? type_any()
                                      : context.infer_iterable_element(
                                            args[0], false, env);
        result = type_container(
            TYPE_GENERATOR,
            {type_container(TYPE_PAIR,
                            {type_builtin(TYPE_INT), element}),
             type_any()});
        return true;
    }

    if (path == "std.iter.collect") {
        const TypeNode *element = type_any();
        if (!args.empty())
            element = context.infer_iterable_element(args[0], false, env);
        result = type_container(TYPE_LIST, {element});
        return true;
    }

    if (path == "std.iter.map" || path == "std.iter.filter") {
        bool is_map = path == "std.iter.map";
        const TypeNode *element = type_any();
        if (!args.empty())
            element = context.infer_iterable_element(args[0], false, env);
        const TypeNode *mapped = element;
        if (args.size() > 1) {
            struct ast *mapper = args[1];
            const TypeNode *expected = function_type(
                {element}, is_map ? type_any() : type_builtin(TYPE_BOOL));
            if (!context.check_expr(mapper, expected, env, false, false))
                return true;
            if (is_map) {
                if (mapper->nodetype == 'Z') {
                    mapped = context.infer_lambda_result(mapper, element,
                                                         env);
                } else {
                    const TypeNode *mapper_type =
                        context.infer(mapper, env);
                    if (type_is_function_signature(mapper_type)) {
                        mapped = type_function_result(mapper_type);
                    } else if (mapper->nodetype == 'N') {
                        const char *mapper_name =
                            ((struct symref *)mapper)->name;
                        mapped = context.named_function_result(
                            mapper_name ? mapper_name : "");
                    }
                }
            }
        }
        result = type_container(TYPE_GENERATOR, {mapped, type_any()});
        return true;
    }

    if (path == "std.set.new" || path == "std.deque.new" ||
        path == "std.heap.new") {
        enum type_constraint_kind kind =
            path == "std.set.new"
                ? TYPE_SET
                : (path == "std.deque.new" ? TYPE_DEQUE : TYPE_HEAP);
        if (expected && expected->kind == kind) {
            result = expected;
        } else {
            result = type_container(kind, {type_any()});
        }
        return true;
    }

    if (path.rfind("std.list.", 0) == 0) {
        std::string name = path.substr(9);
        if (!require_kind(0, TYPE_LIST))
            return true;
        const TypeNode *list_type = container_type(arg_type(0),
                                                   TYPE_LIST, 1);
        const TypeNode *input_element =
            list_type->args.empty() ? type_any() : list_type->args[0];
        const TypeNode *element =
            expected_element(expected, TYPE_LIST, 0);
        if (element->kind == TYPE_ANY)
            element = input_element;
        /* map/zip input element types are independent from result types. */
        if (name == "map" || name == "zip")
            element = input_element;
        const TypeNode *target_list =
            type_container(TYPE_LIST, {element});
        if (!check_arg(0, target_list, false))
            return true;

        if (name == "len") {
            result = type_builtin(TYPE_INT);
            return true;
        }
        if (name == "append") {
            if (!check_arg(1, element, true))
                return true;
            result = target_list;
            return true;
        }
        if (name == "slice") {
            if (!check_arg(1, type_builtin(TYPE_INT), false) ||
                !check_arg(2, type_builtin(TYPE_INT), false))
                return true;
            result = target_list;
            return true;
        }
        if (name == "contains" || name == "index") {
            if (!check_arg(1, element, true))
                return true;
            result = type_builtin(name == "contains" ? TYPE_BOOL
                                                      : TYPE_INT);
            return true;
        }
        if (name == "reverse" || name == "sort" ||
            name == "sort_desc" || name == "unique" ||
            name == "top_k") {
            if (name == "top_k" && !check_arg(1, type_builtin(TYPE_INT),
                                              false))
                return true;
            result = target_list;
            return true;
        }
        if (name == "filter") {
            if (args.size() > 1 &&
                !context.check_expr(
                    args[1], function_type({element}, type_builtin(TYPE_BOOL)),
                    env, false, false))
                return true;
            result = target_list;
            return true;
        }
        if (name == "sort_by" || name == "unique_by") {
            if (args.size() > 1 &&
                !context.check_expr(args[1],
                                    function_type({element}, type_any()),
                                    env, false, false))
                return true;
            result = target_list;
            return true;
        }
        if (name == "is_sorted") {
            result = type_builtin(TYPE_BOOL);
            return true;
        }
        if (name == "map") {
            const TypeNode *mapped = type_any();
            if (args.size() > 1) {
                struct ast *mapper = args[1];
                if (mapper->nodetype == 'Z') {
                    const TypeNode *expected =
                        function_type({element}, type_any());
                    if (!context.check_expr(mapper, expected, env, false,
                                            false))
                        return true;
                    mapped = context.infer_lambda_result(mapper, element, env);
                } else {
                    const TypeNode *mapper_type =
                        context.infer(mapper, env);
                    if (type_is_function_signature(mapper_type)) {
                        const TypeNode *expected =
                            function_type({element}, type_any());
                        if (match_type(mapper_type, expected) ==
                            MatchResult::Mismatch) {
                            char message[512];
                            std::snprintf(
                                message, sizeof(message),
                                "expected %s, got %s",
                                type_display(expected),
                                type_display(mapper_type));
                            context.report_error(mapper->line,
                                                 mapper->column,
                                                 message);
                            return true;
                        }
                        mapped = type_function_result(mapper_type);
                    } else if (mapper->nodetype == 'N') {
                        const char *mapper_name =
                            ((struct symref *)mapper)->name;
                        mapped = context.named_function_result(
                            mapper_name ? mapper_name : "");
                    }
                }
            }
            result = type_container(TYPE_LIST, {mapped});
            return true;
        }
        if (name == "reduce") {
            if (args.size() > 2) {
                const TypeNode *accumulator =
                    context.infer(args[2], env);
                const TypeNode *expected =
                    function_type({accumulator, element}, accumulator);
                if (!context.check_expr(args[1], expected, env, false,
                                        false))
                    return true;
                result = accumulator;
            }
            return true;
        }
        if (name == "group_by") {
            const TypeNode *key = type_any();
            if (args.size() > 1) {
                struct ast *mapper = args[1];
                if (mapper->nodetype == 'Z') {
                    key = context.infer_lambda_result(mapper, element, env);
                } else {
                    const TypeNode *mapper_type =
                        context.infer(mapper, env);
                    if (type_is_function_signature(mapper_type))
                        key = type_function_result(mapper_type);
                }
            }
            result = type_container(TYPE_DICT, {key, target_list});
            return true;
        }
        if (name == "chunk" || name == "window") {
            if (!check_arg(1, type_builtin(TYPE_INT), false))
                return true;
            result = type_container(
                TYPE_LIST,
                {type_container(TYPE_LIST, {element})});
            return true;
        }
        if (name == "zip") {
            if (!require_kind(1, TYPE_LIST))
                return true;
            const TypeNode *second_list =
                container_type(arg_type(1), TYPE_LIST, 1);
            const TypeNode *second_element =
                second_list->args.empty() ? type_any()
                                          : second_list->args[0];
            if (!check_arg(
                    1, type_container(TYPE_LIST, {second_element}),
                    false))
                return true;
            result = type_container(
                TYPE_LIST,
                {type_container(TYPE_PAIR, {element, second_element})});
            return true;
        }
        if (name == "binary_search") {
            if (!check_arg(1, element, true))
                return true;
            result = type_container(
                TYPE_DICT, {type_builtin(TYPE_STRING), type_any()});
            return true;
        }
        result = type_any();
        return true;
    }

    if (path.rfind("std.dict.", 0) == 0) {
        std::string name = path.substr(9);
        if (!require_kind(0, TYPE_DICT))
            return true;
        const TypeNode *dict_type = container_type(arg_type(0),
                                                   TYPE_DICT, 2);
        const TypeNode *key = expected_element(expected, TYPE_DICT, 0);
        const TypeNode *value = expected_element(expected, TYPE_DICT, 1);
        if (key->kind == TYPE_ANY && dict_type->args.size() > 0)
            key = dict_type->args[0];
        if (value->kind == TYPE_ANY && dict_type->args.size() > 1)
            value = dict_type->args[1];
        const TypeNode *target_dict =
            type_container(TYPE_DICT, {key, value});
        if (!check_arg(0, target_dict, false))
            return true;

        if (name == "len") {
            result = type_builtin(TYPE_INT);
            return true;
        }
        if (name == "keys") {
            result = type_container(TYPE_LIST, {key});
            return true;
        }
        if (name == "values") {
            result = type_container(TYPE_LIST, {value});
            return true;
        }
        if (name == "items") {
            result = type_container(
                TYPE_LIST,
                {type_container(TYPE_PAIR, {key, value})});
            return true;
        }
        if (name == "get") {
            if (!check_arg(1, key, true))
                return true;
            if (args.size() > 2) {
                if (!check_arg(2, value, true))
                    return true;
                result = value;
            } else {
                result = type_nullable(value);
            }
            return true;
        }
        if (name == "has") {
            if (!check_arg(1, key, true))
                return true;
            result = type_builtin(TYPE_BOOL);
            return true;
        }
        if (name == "delete") {
            if (!check_arg(1, key, true))
                return true;
            result = target_dict;
            return true;
        }
        if (name == "merge") {
            if (!require_kind(1, TYPE_DICT) ||
                !check_arg(1, target_dict, false))
                return true;
            result = target_dict;
            return true;
        }
        result = type_any();
        return true;
    }

    if (path.rfind("std.set.", 0) == 0) {
        std::string name = path.substr(8);
        if (name == "from_list") {
            if (!require_kind(0, TYPE_LIST))
                return true;
            const TypeNode *element =
                expected_element(expected, TYPE_SET, 0);
            const TypeNode *list_type = container_type(arg_type(0),
                                                       TYPE_LIST, 1);
            if (element->kind == TYPE_ANY && list_type->args.size() > 0)
                element = list_type->args[0];
            if (!check_arg(0,
                           type_container(TYPE_LIST, {element}), false))
                return true;
            result = type_container(TYPE_SET, {element});
            return true;
        }
        if (!require_kind(0, TYPE_SET))
            return true;
        const TypeNode *set_type = container_type(arg_type(0), TYPE_SET,
                                                  1);
        const TypeNode *element =
            expected_element(expected, TYPE_SET, 0);
        if (element->kind == TYPE_ANY && set_type->args.size() > 0)
            element = set_type->args[0];
        const TypeNode *target_set =
            type_container(TYPE_SET, {element});
        if (!check_arg(0, target_set, false))
            return true;
        if (name == "add" || name == "remove") {
            if (!check_arg(1, element, true))
                return true;
            result = target_set;
            return true;
        }
        if (name == "has") {
            if (!check_arg(1, element, true))
                return true;
            result = type_builtin(TYPE_BOOL);
            return true;
        }
        if (name == "values") {
            result = type_container(TYPE_LIST, {element});
            return true;
        }
        if (name == "size") {
            result = type_builtin(TYPE_INT);
            return true;
        }
        if (name == "union" || name == "intersect" ||
            name == "difference") {
            if (!require_kind(1, TYPE_SET) ||
                !check_arg(1, target_set, false))
                return true;
            result = target_set;
            return true;
        }
        if (name == "is_subset") {
            if (!require_kind(1, TYPE_SET) ||
                !check_arg(1, target_set, false))
                return true;
            result = type_builtin(TYPE_BOOL);
            return true;
        }
        result = type_any();
        return true;
    }

    if (path.rfind("std.deque.", 0) == 0) {
        std::string name = path.substr(10);
        if (name == "from_list") {
            if (!require_kind(0, TYPE_LIST))
                return true;
            const TypeNode *element =
                expected_element(expected, TYPE_DEQUE, 0);
            const TypeNode *list_type = container_type(arg_type(0),
                                                       TYPE_LIST, 1);
            if (element->kind == TYPE_ANY && list_type->args.size() > 0)
                element = list_type->args[0];
            if (!check_arg(0,
                           type_container(TYPE_LIST, {element}), false))
                return true;
            result = type_container(TYPE_DEQUE, {element});
            return true;
        }
        if (!require_kind(0, TYPE_DEQUE))
            return true;
        const TypeNode *deque_type =
            container_type(arg_type(0), TYPE_DEQUE, 1);
        const TypeNode *element =
            expected_element(expected, TYPE_DEQUE, 0);
        if (element->kind == TYPE_ANY && deque_type->args.size() > 0)
            element = deque_type->args[0];
        const TypeNode *target_deque =
            type_container(TYPE_DEQUE, {element});
        if (!check_arg(0, target_deque, false))
            return true;
        if (name == "push_front" || name == "push_back") {
            if (!check_arg(1, element, true))
                return true;
            result = target_deque;
            return true;
        }
        if (name == "to_list") {
            result = type_container(TYPE_LIST, {element});
            return true;
        }
        if (name == "size") {
            result = type_builtin(TYPE_INT);
            return true;
        }
        if (name == "peek_front" || name == "peek_back") {
            result = type_nullable(element);
            return true;
        }
        result = type_any();
        return true;
    }

    if (path.rfind("std.heap.", 0) == 0) {
        std::string name = path.substr(9);
        if (name == "from_list") {
            if (!require_kind(0, TYPE_LIST))
                return true;
            const TypeNode *element =
                expected_element(expected, TYPE_HEAP, 0);
            const TypeNode *list_type = container_type(arg_type(0),
                                                       TYPE_LIST, 1);
            if (element->kind == TYPE_ANY && list_type->args.size() > 0)
                element = list_type->args[0];
            if (!check_arg(0,
                           type_container(TYPE_LIST, {element}), false))
                return true;
            result = type_container(TYPE_HEAP, {element});
            return true;
        }
        if (!require_kind(0, TYPE_HEAP))
            return true;
        const TypeNode *heap_type = container_type(arg_type(0),
                                                   TYPE_HEAP, 1);
        const TypeNode *element =
            expected_element(expected, TYPE_HEAP, 0);
        if (element->kind == TYPE_ANY && heap_type->args.size() > 0)
            element = heap_type->args[0];
        const TypeNode *target_heap =
            type_container(TYPE_HEAP, {element});
        if (!check_arg(0, target_heap, false))
            return true;
        if (name == "push") {
            if (!check_arg(1, element, true))
                return true;
            result = target_heap;
            return true;
        }
        if (name == "to_list") {
            result = type_container(TYPE_LIST, {element});
            return true;
        }
        if (name == "size") {
            result = type_builtin(TYPE_INT);
            return true;
        }
        if (name == "peek") {
            result = type_nullable(element);
            return true;
        }
        result = type_any();
        return true;
    }

    return false;
}



namespace {

const TypeNode *builtin_sig(
    std::initializer_list<const TypeNode *> params,
    const TypeNode *result) {
    std::vector<const TypeNode *> args(params);
    args.push_back(result);
    return type_container(TYPE_BUILTIN, args);
}

const TypeNode *num() { return type_builtin(TYPE_NUMBER); }
const TypeNode *integer() { return type_builtin(TYPE_INT); }
const TypeNode *real() { return type_builtin(TYPE_FLOAT); }
const TypeNode *boolean() { return type_builtin(TYPE_BOOL); }
const TypeNode *text() { return type_builtin(TYPE_STRING); }
const TypeNode *any_type() { return type_any(); }

const TypeNode *poly_var(const char *name) { return type_variable(name); }

const TypeNode *poly_list(const TypeNode *element) {
    return type_container(TYPE_LIST, {element});
}

const TypeNode *poly_dict(const TypeNode *key, const TypeNode *value) {
    return type_container(TYPE_DICT, {key, value});
}

const TypeNode *poly_set(const TypeNode *element) {
    return type_container(TYPE_SET, {element});
}

const TypeNode *poly_deque(const TypeNode *element) {
    return type_container(TYPE_DEQUE, {element});
}

const TypeNode *poly_heap(const TypeNode *element) {
    return type_container(TYPE_HEAP, {element});
}

const TypeNode *poly_task(const TypeNode *result) {
    return type_container(TYPE_TASK, {result});
}

const TypeNode *poly_generator(const TypeNode *element,
                               const TypeNode *send = nullptr) {
    return type_container(
        TYPE_GENERATOR,
        {element, send ? send : static_cast<const TypeNode *>(type_any())});
}

const TypeNode *poly_builtin(
    std::initializer_list<const TypeNode *> params,
    const TypeNode *result, int optional_params = 0,
    bool variadic = false) {
    std::vector<const TypeNode *> args(params);
    args.push_back(result);
    return make_function_type(TYPE_BUILTIN, args, optional_params, variadic);
}

const TypeNode *poly_callback(
    std::initializer_list<const TypeNode *> params,
    const TypeNode *result) {
    std::vector<const TypeNode *> args(params);
    args.push_back(result);
    return type_container(TYPE_CALLABLE, args);
}

} // namespace

const TypeNode *builtin_polymorphic_type(const std::string &path) {
    const TypeNode *T = poly_var("T");
    const TypeNode *U = poly_var("U");
    const TypeNode *K = poly_var("K");
    const TypeNode *V = poly_var("V");
    const TypeNode *A = poly_var("A");
    const TypeNode *B = poly_var("B");
    const TypeNode *C = poly_var("C");

    const TypeNode *list_t = poly_list(T);
    const TypeNode *dict_kv = poly_dict(K, V);
    const TypeNode *set_t = poly_set(T);
    const TypeNode *deque_t = poly_deque(T);
    const TypeNode *heap_t = poly_heap(T);
    const TypeNode *integer_type = integer();
    const TypeNode *bool_type = boolean();

    if (path == "std.list.len")
        return poly_builtin({list_t}, integer_type);
    if (path == "std.list.slice")
        return poly_builtin({list_t, integer_type, integer_type}, list_t,
                            1);
    if (path == "std.list.contains")
        return poly_builtin({list_t, T}, bool_type);
    if (path == "std.list.index")
        return poly_builtin({list_t, T}, integer_type);
    if (path == "std.list.reverse")
        return poly_builtin({list_t}, list_t);
    if (path == "std.list.sort")
        return poly_builtin({list_t, any_type()}, list_t, 1);
    if (path == "std.list.sort_desc" || path == "std.list.unique")
        return poly_builtin({list_t}, list_t);
    if (path == "std.list.is_sorted")
        return poly_builtin({list_t, any_type()}, bool_type, 1);
    if (path == "std.list.append")
        return poly_builtin({list_t, T}, list_t);
    if (path == "std.list.top_k")
        return poly_builtin({list_t, integer_type, any_type()}, list_t,
                            1);
    if (path == "std.list.map")
        return poly_builtin({list_t, poly_callback({T}, U)},
                            poly_list(U));
    if (path == "std.list.filter")
        return poly_builtin({list_t, poly_callback({T}, bool_type)},
                            list_t);
    if (path == "std.list.reduce")
        return poly_builtin(
            {list_t, poly_callback({A, T}, A), A}, A, 1);
    if (path == "std.list.sort_by")
        return poly_builtin(
            {list_t, poly_callback({T}, U), any_type()}, list_t, 1);
    if (path == "std.list.unique_by")
        return poly_builtin({list_t, poly_callback({T}, U)}, list_t);
    if (path == "std.list.group_by")
        return poly_builtin({list_t, poly_callback({T}, K)},
                            poly_dict(K, list_t));
    if (path == "std.list.chunk")
        return poly_builtin({list_t, integer_type},
                            poly_list(poly_list(T)));
    if (path == "std.list.window")
        return poly_builtin({list_t, integer_type, integer_type},
                            poly_list(poly_list(T)), 1);
    if (path == "std.list.zip")
        return poly_builtin(
            {list_t, poly_list(U)},
            poly_list(type_container(TYPE_PAIR, {T, U})), 0, true);
    if (path == "std.list.binary_search")
        return poly_builtin(
            {list_t, T, any_type()},
            poly_dict(text(), any_type()), 1);

    if (path == "std.iter.map")
        return poly_builtin({list_t, poly_callback({T}, U)},
                            any_type());
    if (path == "std.iter.filter")
        return poly_builtin({list_t, poly_callback({T}, bool_type)},
                            any_type());
    if (path == "std.iter.for_each")
        return poly_builtin({any_type(), any_type()},
                            type_builtin(TYPE_NULL));

    if (path == "std.fn.compose")
        return poly_builtin(
            {poly_callback({B}, C), poly_callback({A}, B)},
            poly_callback({A}, C));
    if (path == "std.fn.pipe")
        return poly_builtin(
            {poly_callback({A}, B), poly_callback({B}, C)},
            poly_callback({A}, C));

    if (path == "std.task.wait")
        return poly_builtin({poly_task(T), integer_type}, T, 1);
    if (path == "std.task.wait_all")
        return poly_builtin(
            {poly_list(poly_task(T)), integer_type}, poly_list(T), 1);
    if (path == "std.task.done" || path == "std.task.cancel")
        return poly_builtin({poly_task(T)}, bool_type);
    if (path == "std.task.sleep")
        return poly_builtin({integer_type},
                            poly_task(type_builtin(TYPE_NULL)));
    if (path == "std.task.yield")
        return poly_builtin({}, poly_task(type_builtin(TYPE_NULL)));

    const TypeNode *error_type = type_builtin(TYPE_ERROR);
    const TypeNode *result_t =
        type_container(TYPE_RESULT, {T, error_type});
    if (path == "std.task.catch")
        return poly_builtin({poly_task(T)}, poly_task(result_t));
    if (path == "std.task.try_wait")
        return poly_builtin({poly_task(T), integer_type}, result_t, 1);
    if (path == "std.task.try_wait_all")
        return poly_builtin(
            {poly_list(poly_task(T)), integer_type},
            type_container(TYPE_RESULT, {poly_list(T), error_type}), 1);
    if (path == "std.task.timeout")
        return poly_builtin({poly_task(T), integer_type}, result_t);
    if (path == "std.task.retry")
        return poly_builtin(
            {poly_callback({}, any_type()),
             poly_dict(text(), any_type())},
            poly_task(any_type()), 1);
    if (path == "std.task.rate_limit")
        return poly_builtin({type_builtin(TYPE_NUMBER),
                             type_builtin(TYPE_NUMBER)},
                            type_builtin(TYPE_USERDATA));
    if (path == "std.task.acquire")
        return poly_builtin({type_builtin(TYPE_USERDATA), integer_type},
                            bool_type, 1);
    if (path == "std.task.bounded_parallel_map")
        return poly_builtin(
            {poly_list(T), integer_type, poly_callback({T}, any_type())},
            poly_list(type_container(TYPE_RESULT,
                                     {any_type(), error_type})));

    if (path == "std.generator.next")
        return poly_builtin(
            {poly_generator(T, U), U},
            poly_dict(text(), T), 1);
    if (path == "std.generator.done" ||
        path == "std.generator.close")
        return poly_builtin({poly_generator(T)}, bool_type);

    const TypeNode *userdata_type = type_builtin(TYPE_USERDATA);
    const TypeNode *socket_type = type_builtin(TYPE_SOCKET);
    const TypeNode *listener_type = type_builtin(TYPE_LISTENER);
    const TypeNode *websocket_type = type_builtin(TYPE_WEBSOCKET);
    const TypeNode *stream_type = type_builtin(TYPE_STREAM);
    const TypeNode *bytes_type = type_builtin(TYPE_BYTES);
    const TypeNode *null_type = type_builtin(TYPE_NULL);
    const TypeNode *dict_any = poly_dict(text(), any_type());

    if (path == "std.net.tcp_connect_async")
        return poly_builtin(
            {text(), integer_type, integer_type, text()},
            poly_task(socket_type), 2);
    if (path == "std.net.tcp_listen_async")
        return poly_builtin(
            {text(), integer_type, integer_type, text(), dict_any},
            poly_task(listener_type), 2);
    if (path == "std.net.accept_async")
        return poly_builtin({listener_type, integer_type},
                            poly_task(socket_type), 1);
    if (path == "std.net.send_async")
        return poly_builtin({socket_type, any_type(), integer_type},
                            poly_task(integer_type), 1);
    if (path == "std.net.recv_async")
        return poly_builtin(
            {socket_type, integer_type, integer_type},
            poly_task(bytes_type), 1);
    if (path == "std.net.resolve_async")
        return poly_builtin({text(), integer_type, text()},
                            poly_task(poly_list(dict_any)), 2);
    if (path == "std.net.shutdown_async")
        return poly_builtin({socket_type, integer_type},
                            poly_task(null_type), 1);

    if (path == "std.net.tcp_connect")
        return poly_builtin(
            {text(), integer_type, integer_type, text()}, socket_type, 2);
    if (path == "std.net.tcp_listen")
        return poly_builtin(
            {text(), integer_type, integer_type, text()}, listener_type, 2);
    if (path == "std.net.accept")
        return poly_builtin({listener_type, integer_type}, socket_type, 1);
    if (path == "std.net.send")
        return poly_builtin({socket_type, any_type(), integer_type},
                            integer_type, 1);
    if (path == "std.net.recv")
        return poly_builtin({socket_type, integer_type, integer_type},
                            bytes_type, 1);
    if (path == "std.net.resolve")
        return poly_builtin({text(), integer_type, text()},
                            poly_list(dict_any), 2);
    if (path == "std.net.shutdown")
        return poly_builtin({socket_type, integer_type}, null_type, 1);
    if (path == "std.net.local_addr" || path == "std.net.peer_addr")
        return poly_builtin({socket_type}, dict_any);

    if (path == "std.io.read_async")
        return poly_builtin({stream_type, integer_type, integer_type},
                            poly_task(bytes_type), 1);
    if (path == "std.io.write_async")
        return poly_builtin({stream_type, any_type(), integer_type},
                            poly_task(integer_type), 1);
    if (path == "std.io.buffered")
        return poly_builtin({stream_type, integer_type}, stream_type, 1);
    if (path == "std.fs.open")
        return poly_builtin({text(), text()}, stream_type);

    if (path == "std.http.request_async")
        return poly_builtin({text(), text(), dict_any},
                            poly_task(dict_any), 1);
    if (path == "std.http.get_async")
        return poly_builtin({text(), dict_any}, poly_task(dict_any), 1);
    if (path == "std.http.post_async")
        return poly_builtin({text(), any_type(), dict_any},
                            poly_task(dict_any), 1);
    if (path == "std.http.accept_async")
        return poly_builtin({dict_any, integer_type},
                            poly_task(dict_any), 1);

    if (path == "std.http2.request_async")
        return poly_builtin({text(), text(), dict_any},
                            poly_task(dict_any), 1);
    if (path == "std.http2.get_async")
        return poly_builtin({text(), dict_any}, poly_task(dict_any), 1);
    if (path == "std.http2.post_async")
        return poly_builtin({text(), any_type(), dict_any},
                            poly_task(dict_any), 2);
    if (path == "std.http2.request_many_async")
        return poly_builtin({poly_list(any_type()), dict_any},
                            poly_task(poly_list(dict_any)), 1);

    if (path == "std.websocket.connect_async")
        return poly_builtin({text(), dict_any},
                            poly_task(websocket_type), 1);
    if (path == "std.websocket.send_async")
        return poly_builtin({websocket_type, any_type(), integer_type},
                            poly_task(bool_type), 1);
    if (path == "std.websocket.receive_async")
        return poly_builtin({websocket_type, integer_type},
                            poly_task(dict_any), 1);

    if (path == "std.dict.len")
        return poly_builtin({dict_kv}, integer_type);
    if (path == "std.dict.has")
        return poly_builtin({dict_kv, K}, bool_type);
    if (path == "std.dict.keys")
        return poly_builtin({dict_kv}, poly_list(K));
    if (path == "std.dict.values")
        return poly_builtin({dict_kv}, poly_list(V));
    if (path == "std.dict.items")
        return poly_builtin(
            {dict_kv},
            poly_list(type_container(TYPE_PAIR, {K, V})));
    if (path == "std.dict.delete")
        return poly_builtin({dict_kv, K}, dict_kv);
    if (path == "std.dict.merge")
        return poly_builtin({dict_kv, dict_kv}, dict_kv);

    if (path == "std.set.from_list")
        return poly_builtin({list_t}, set_t);
    if (path == "std.set.add" || path == "std.set.remove")
        return poly_builtin({set_t, T}, set_t);
    if (path == "std.set.has")
        return poly_builtin({set_t, T}, bool_type);
    if (path == "std.set.size")
        return poly_builtin({set_t}, integer_type);
    if (path == "std.set.values")
        return poly_builtin({set_t}, poly_list(T));
    if (path == "std.set.union" || path == "std.set.intersect" ||
        path == "std.set.difference")
        return poly_builtin({set_t, set_t}, set_t);
    if (path == "std.set.is_subset")
        return poly_builtin({set_t, set_t}, bool_type);

    if (path == "std.deque.from_list")
        return poly_builtin({list_t}, deque_t);
    if (path == "std.deque.size")
        return poly_builtin({deque_t}, integer_type);
    if (path == "std.deque.push_front" || path == "std.deque.push_back")
        return poly_builtin({deque_t, T}, deque_t);
    if (path == "std.deque.peek_front" ||
        path == "std.deque.peek_back" ||
        path == "std.deque.pop_front" ||
        path == "std.deque.pop_back")
        return poly_builtin({deque_t}, type_nullable(T));
    if (path == "std.deque.to_list")
        return poly_builtin({deque_t}, poly_list(T));

    if (path == "std.heap.from_list")
        return poly_builtin(
            {list_t, poly_callback({T, T}, bool_type)}, heap_t);
    if (path == "std.heap.size")
        return poly_builtin({heap_t}, integer_type);
    if (path == "std.heap.push")
        return poly_builtin({heap_t, T}, heap_t);
    if (path == "std.heap.peek" || path == "std.heap.pop")
        return poly_builtin({heap_t}, type_nullable(T));
    if (path == "std.heap.to_list")
        return poly_builtin({heap_t}, poly_list(T));

    return NULL;
}

const TypeNode *builtin_function_type(int functype) {
    switch (functype) {
        case B_sqrt:
        case B_exp:
        case B_log:
        case B_abs:
        case B_sin:
        case B_cos:
        case B_tan:
        case B_asin:
        case B_acos:
        case B_atan:
        case B_log2:
        case B_log10:
        case B_math_cbrt:
        case B_math_degrees:
        case B_math_radians:
            return builtin_sig({num()}, num());
        case B_floor:
        case B_ceil:
        case B_round:
        case B_trunc:
            return builtin_sig({num()}, integer());
        case B_atan2:
        case B_pow:
        case B_hypot:
        case B_math_fmod:
        case B_math_mod:
            return builtin_sig({num(), num()}, num());
        case B_math_clamp:
            return builtin_sig({num(), num(), num()}, num());
        case B_math_sign:
            return builtin_sig({num()}, num());
        case B_math_is_nan:
        case B_math_is_inf:
        case B_math_is_finite:
            return builtin_sig({num()}, boolean());
        case B_math_gcd:
        case B_math_lcm:
            return builtin_sig({integer(), integer()}, integer());
        case B_len:
            return builtin_sig({any_type()}, integer());
        case B_int:
            return builtin_sig({any_type()}, integer());
        case B_float:
            return builtin_sig({any_type()}, real());
        case B_bool:
            return builtin_sig({any_type()}, boolean());
        case B_str:
            return builtin_sig({any_type()}, text());
        case B_char:
            return builtin_sig({any_type()}, type_builtin(TYPE_CHAR));
        case B_byte:
            return builtin_sig({any_type()}, type_builtin(TYPE_BYTE));
        case B_kind_of:
        case B_type_name:
        case B_type_module:
        case B_type_source:
            return builtin_sig({any_type()}, text());
        case B_string_replace:
            return builtin_sig({text(), text(), text()}, text());
        case B_string_starts_with:
        case B_string_ends_with:
        case B_string_contains:
            return builtin_sig({text(), text()}, boolean());
        case B_string_repeat:
            return builtin_sig({text(), integer()}, text());
        case B_string_trim_start:
        case B_string_trim_end:
        case B_trim:
            return builtin_sig({text()}, text());
        case B_split:
            return builtin_sig(
                {text(), text()},
                type_container(TYPE_LIST, {text()}));
        case B_split_ws:
            return builtin_sig(
                {text()},
                type_container(TYPE_LIST, {text()}));
        case B_find:
            return builtin_sig({text(), text()}, integer());
        case B_bytes_from_string:
            return builtin_sig({text()}, type_builtin(TYPE_BYTES));
        case B_bytes_to_string:
            return builtin_sig({type_builtin(TYPE_BYTES)}, text());
        case B_bytes_from_char:
            return builtin_sig({type_builtin(TYPE_CHAR)},
                               type_builtin(TYPE_BYTES));
        case B_char_from_bytes:
            return builtin_sig({type_builtin(TYPE_BYTES)},
                               type_builtin(TYPE_CHAR));
        case B_bytes_len:
            return builtin_sig({type_builtin(TYPE_BYTES)}, integer());
        case B_bytes_get:
            return builtin_sig({type_builtin(TYPE_BYTES), integer()},
                               type_builtin(TYPE_BYTE));
        case B_bytes_concat:
            return builtin_sig(
                {type_builtin(TYPE_BYTES), type_builtin(TYPE_BYTES)},
                type_builtin(TYPE_BYTES));
        case B_bytes_hex:
            return builtin_sig({type_builtin(TYPE_BYTES)}, text());
        case B_bytes_from_hex:
            return builtin_sig({text()}, type_builtin(TYPE_BYTES));
        case B_int_to_bytes:
            return builtin_sig({integer(), integer(), text()},
                               type_builtin(TYPE_BYTES));
        case B_read_file:
            return builtin_sig({text()}, text());
        case B_write_file:
        case B_append_file:
            return builtin_sig({text(), text()}, type_builtin(TYPE_NULL));
        case B_file_exists:
            return builtin_sig({text()}, boolean());
        case B_read_bytes:
            return builtin_sig({text()}, type_builtin(TYPE_BYTES));
        case B_write_bytes:
        case B_append_bytes:
            return builtin_sig({text(), type_builtin(TYPE_BYTES)},
                               type_builtin(TYPE_NULL));
        default:
            return NULL;
    }
}

} // namespace detail
} // namespace typecheck
} // namespace skein
