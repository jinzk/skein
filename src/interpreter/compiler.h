#ifndef SKEIN_COMPILER_H
#define SKEIN_COMPILER_H

#include "bytecode.h"

#include <memory>
#include <string>

struct ast;
struct function_code;

/*
 * Compile a top-level statement or a function body into bytecode.
 *
 * Unsupported subtrees are emitted as Op::EvalAst when doing so cannot change
 * control-flow semantics.  If a subtree would require unsafe fallback (for
 * example a match expression containing break inside a bytecode loop), the
 * compiler reports failure by returning nullptr; the caller must then fall
 * back to the AST interpreter for the whole statement/function.
 */
skein::bc::ProtoPtr skein_compile_statement(struct ast *statement,
                                            const std::string &module,
                                            std::string &error);

skein::bc::ProtoPtr skein_compile_function_body(struct function_code *code,
                                                 const std::string &module,
                                                 std::string &error);

/* Compile one default-parameter expression in the function's parameter scope. */
skein::bc::ProtoPtr skein_compile_default_expression(
    struct function_code *code, struct ast *expression, int index,
    const std::string &module, std::string &error);

#endif
