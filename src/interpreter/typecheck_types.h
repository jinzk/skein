#ifndef SKEIN_TYPECHECK_TYPES_H
#define SKEIN_TYPECHECK_TYPES_H

#include "static_type.h"

#include <string>
#include <unordered_map>
#include <vector>

struct ast;
struct funcdef;
struct function_code;
struct structtype_lookup_result;
struct symlist;
struct value_t;

namespace skein {
namespace typecheck {
namespace detail {

struct FunctionSig {
    std::vector<const TypeNode *> params;
    bool variadic = false;
    bool is_async = false;
    bool is_generator = false;
    int optional_params = 0;
    const TypeNode *rest = nullptr;
    const TypeNode *result = nullptr;
    const TypeNode *generator_send = nullptr;
};

enum class MatchResult {
    Ok,
    Unknown,
    Mismatch,
};

const TypeNode *value_static_type(const struct value_t *value);
bool needs_static_check(const TypeNode *type);
const TypeNode *join_types(const TypeNode *a, const TypeNode *b);

bool flatten_path(const struct ast *node, std::string &out);
void collect_call_args(struct ast *args, std::vector<struct ast *> &out);
std::string last_path_component(const std::string &path);
std::string canonical_builtin_path(const std::string &path,
                                   const std::string &source);

FunctionSig signature_from_symlist(struct symlist *sl,
                                   bool skip_self = false);
const TypeNode *signature_result_type(const FunctionSig &sig);
FunctionSig method_signature_from_code(const struct function_code *code);
FunctionSig method_signature_from_def(const struct funcdef *def);
struct ast *find_single_return(struct ast *node, int &count);

bool lookup_named_type(const TypeNode *type,
                       struct structtype_lookup_result &out);
MatchResult match_type(const TypeNode *actual, const TypeNode *expected);

/*
 * Builtin polymorphic values use static-only type variables (for example
 * list.append has type (list[T], T) -> list[T]).  bind_type unifies an
 * inferred concrete type with a template, and instantiate_type replaces
 * variables with the bound types.
 */
using TypeBindings = std::unordered_map<std::string, const TypeNode *>;
bool type_contains_vars(const TypeNode *type);
bool bind_type(const TypeNode *actual, const TypeNode *pattern,
               TypeBindings &bindings);
const TypeNode *instantiate_type(const TypeNode *type,
                                 const TypeBindings &bindings);

} // namespace detail
} // namespace typecheck
} // namespace skein

#endif
