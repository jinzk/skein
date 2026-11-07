#ifndef SKEIN_TYPECHECK_TYPES_H
#define SKEIN_TYPECHECK_TYPES_H

#include "static_type.h"

#include <string>
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
    const TypeNode *rest = nullptr;
    const TypeNode *result = nullptr;
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
FunctionSig method_signature_from_code(const struct function_code *code);
FunctionSig method_signature_from_def(const struct funcdef *def);
struct ast *find_single_return(struct ast *node, int &count);

bool lookup_named_type(const TypeNode *type,
                       struct structtype_lookup_result &out);
MatchResult match_type(const TypeNode *actual, const TypeNode *expected);

} // namespace detail
} // namespace typecheck
} // namespace skein

#endif
