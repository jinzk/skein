#ifndef SKEIN_TYPECHECK_BUILTINS_H
#define SKEIN_TYPECHECK_BUILTINS_H

#include "static_type.h"
#include "typecheck_state.h"

#include <string>
#include <vector>

struct ast;

namespace skein {
namespace typecheck {
namespace detail {

struct BuiltinContext {
    virtual ~BuiltinContext() {}

    virtual const TypeNode *infer(struct ast *expr, Env &env) = 0;
    virtual bool check_expr(struct ast *expr, const TypeNode *expected,
                            Env &env, bool explicit_cast = false,
                            bool force_element = false) = 0;
    virtual const TypeNode *infer_lambda_result(struct ast *function,
                                                const TypeNode *element,
                                                Env &env) = 0;
    virtual const TypeNode *named_function_result(const char *name) = 0;
    virtual void report_error(int line, int column,
                              const char *message) = 0;
};

bool check_builtin(BuiltinContext &context, const std::string &path,
                   const std::vector<struct ast *> &args,
                   const TypeNode *expected, Env &env,
                   const TypeNode *&result);

/* Static signature for a simple (non-container) builtin, or NULL when the
 * builtin is variadic/optional and has no precise first-class type yet. */
const TypeNode *builtin_function_type(int functype);

/*
 * Polymorphic first-class signature for a container builtin path such as
 * "std.list.append".  Returns NULL when the path is not a known container
 * builtin.
 */
const TypeNode *builtin_polymorphic_type(const std::string &canonical_path);

} // namespace detail
} // namespace typecheck
} // namespace skein

#endif
