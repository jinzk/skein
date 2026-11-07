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
    std::string display;
};

const TypeNode *type_any();
const TypeNode *type_from_constraint(const struct type_constraint *type);
const TypeNode *type_builtin(enum type_constraint_kind kind,
                             bool nullable = false);
const TypeNode *type_named(const char *module, const char *name,
                           bool nullable = false);
const TypeNode *type_container(enum type_constraint_kind kind,
                               const std::vector<const TypeNode *> &args,
                               bool nullable = false);
const TypeNode *type_nullable(const TypeNode *type);

bool type_is_any(const TypeNode *type);
bool type_is_container(const TypeNode *type);
bool type_constraint_has_arguments(const struct type_constraint *type);
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
struct type_constraint *type_apply_nullable(struct type_constraint *base);

} // namespace typecheck
} // namespace skein

#endif
