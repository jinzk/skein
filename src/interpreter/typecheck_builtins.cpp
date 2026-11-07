#include "typecheck_builtins.h"

#include "ast.h"
#include "typecheck_types.h"

#include <cstdio>

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
        const TypeNode *element =
            expected_element(expected, TYPE_LIST, 0);
        if (element->kind == TYPE_ANY && !list_type->args.empty())
            element = list_type->args[0];
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
            name == "sort_desc" || name == "sort_by" ||
            name == "filter" || name == "unique" ||
            name == "unique_by" || name == "top_k") {
            if (name == "top_k" && !check_arg(1, type_builtin(TYPE_INT),
                                              false))
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
                    mapped = context.infer_lambda_result(mapper, element, env);
                } else if (mapper->nodetype == 'N') {
                    const char *mapper_name =
                        ((struct symref *)mapper)->name;
                    mapped = context.named_function_result(
                        mapper_name ? mapper_name : "");
                }
            }
            result = type_container(TYPE_LIST, {mapped});
            return true;
        }
        if (name == "reduce") {
            result = type_any();
            return true;
        }
        if (name == "group_by") {
            result = type_container(
                TYPE_DICT, {type_any(), target_list});
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
            if (!require_kind(1, TYPE_LIST) ||
                !check_arg(1, type_container(TYPE_LIST, {type_any()}),
                           false))
                return true;
            result = type_container(
                TYPE_LIST,
                {type_container(TYPE_LIST, {type_any()})});
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
                {type_container(TYPE_LIST, {type_any()})});
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


} // namespace detail
} // namespace typecheck
} // namespace skein
