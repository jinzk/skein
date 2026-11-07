#ifndef SKEIN_STATIC_TYPE_H
#define SKEIN_STATIC_TYPE_H

#include "symbol.h"

#include <cstddef>
#include <string>
#include <vector>

namespace skein {
namespace typecheck {

/*
 * Immutable, interned static type node.  Generic container arguments live
 * here, not in the runtime value representation.  Nodes are allocated once
 * per canonical type and live until process exit; pointer equality is type
 * equality.
 */
struct TypeNode {
    enum type_constraint_kind kind;
    std::string name;
    std::string module;
    bool nullable;
    std::vector<const TypeNode *> args;
    /* Function types only: trailing parameters that may be omitted and a
     * variadic call shape.  They are static-only and never affect runtime. */
    int optional_params = 0;
    bool variadic = false;
    std::string display;
};

const TypeNode *type_any();
const TypeNode *type_from_constraint(const struct type_constraint *type);
const TypeNode *type_builtin(enum type_constraint_kind kind,
                             bool nullable = false);
const TypeNode *type_named(const char *module, const char *name,
                           bool nullable = false);
const TypeNode *type_variable(const char *name);
const TypeNode *type_container(enum type_constraint_kind kind,
                               const std::vector<const TypeNode *> &args,
                               bool nullable = false);
const TypeNode *make_function_type(
    enum type_constraint_kind kind,
    const std::vector<const TypeNode *> &args, int optional_params = 0,
    bool variadic = false, bool nullable = false);
const TypeNode *type_nullable(const TypeNode *type);

bool type_is_any(const TypeNode *type);
bool type_is_container(const TypeNode *type);
bool type_is_function_signature(const TypeNode *type);
int type_function_param_count(const TypeNode *type);
int type_function_optional_params(const TypeNode *type);
bool type_function_is_variadic(const TypeNode *type);
const TypeNode *type_function_param(const TypeNode *type,
                                     std::size_t index);
const TypeNode *type_function_result(const TypeNode *type);
bool type_constraint_has_arguments(const struct type_constraint *type);
bool type_constraint_is_function_type(const struct type_constraint *type);
int type_container_arity(enum type_constraint_kind kind);
const TypeNode *type_element(const TypeNode *type, std::size_t index);
const char *type_display(const TypeNode *type);
bool type_equal(const TypeNode *a, const TypeNode *b);

/*
 * Attach generic/nullable metadata to a flat type_constraint produced by the
 * parser.  The argument constraints are consumed and freed.
 */
struct type_constraint *type_apply_generic(struct type_constraint *base,
                                           struct type_constraint *arg1,
                                           struct type_constraint *arg2);
struct type_constraint *type_apply_function(
    const std::vector<struct type_constraint *> &params,
    struct type_constraint *result);
struct type_constraint *type_apply_nullable(struct type_constraint *base);

} // namespace typecheck
} // namespace skein

#endif
