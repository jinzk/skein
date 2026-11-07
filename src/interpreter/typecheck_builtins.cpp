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

} // namespace

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
