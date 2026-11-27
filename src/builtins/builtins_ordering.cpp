#include "builtins_ordering.h"

#include "builtins_internal.h"

namespace {

const struct type_def *comparable_def(const struct value_t *value) {
    if (!value)
        return NULL;
    if (value->type == T_struct)
        return ((const struct struct_value_t *)value)->def;
    if (value->type == T_enum)
        return ((const struct enum_value_t *)value)->def;
    return NULL;
}

bool comparable_method(const struct value_t *value,
                       std::shared_ptr<function_def> &method) {
    const struct type_def *def = comparable_def(value);
    if (!def)
        return false;
    method = structtype_method(def, "op_cmp");
    return method != NULL;
}

}  // namespace

bool value_has_op_cmp(const struct value_t *value) {
    std::shared_ptr<function_def> method;
    return comparable_method(value, method);
}

bool values_have_op_cmp(const std::vector<struct value_t *> &values) {
    if (values.empty())
        return false;
    std::shared_ptr<function_def> method;
    for (struct value_t *value : values) {
        if (!comparable_method(value, method))
            return false;
    }
    return true;
}

bool compare_with_op_cmp(const struct value_t *left,
                         const struct value_t *right, long long &order,
                         const char *name) {
    std::shared_ptr<function_def> method;
    if (!comparable_method(left, method)) {
        yyerror("%s left value is not Comparable", name);
        return false;
    }
    if (!skein_context_current() || !skein_context_current()->call) {
        yyerror("%s requires an interpreter context", name);
        return false;
    }

    struct value_t *callee =
        newmethodval(dupval(const_cast<struct value_t *>(left)), method);
    if (!callee)
        return false;
    struct value_t *result = skein_call_function(
        callee, {dupval(const_cast<struct value_t *>(right))});
    freeval(callee);
    if (!result)
        return false;
    if (result->type != T_int) {
        yyerror("%s comparator must return int, got %s", name,
                typename_of(result));
        freeval(result);
        return false;
    }
    order = ((struct int_value_t *)result)->value;
    freeval(result);
    return true;
}
