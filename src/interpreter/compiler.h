#ifndef SKEIN_COMPILER_H
#define SKEIN_COMPILER_H

#include "bytecode.h"

#include <memory>
#include <string>
#include <vector>

struct ast;
struct function_code;

/*
 * Compiled destructuring patterns for a function's parameter list.
 *
 * `roots[i]` is the pattern index for parameter `i` in `proto->patterns`,
 * or UINT32_MAX when that parameter has no pattern.  The set is cached on
 * `function_code` so the source AST can be released independently of when
 * the function is first called.
 */
struct compiled_pattern_set {
    skein::bc::ProtoPtr proto;
    std::vector<uint32_t> roots;
    bool failed = false;
};

/*
 * Compile a top-level statement or a function body into bytecode.
 *
 * The compiler has no AST escape hatch.  Unsupported nodes report failure by
 * returning nullptr; the caller then runs the whole statement/function with
 * the AST interpreter.
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

/*
 * Best-effort declaration check for field defaults: compile the expression
 * and reject free-name loads that refer to the receiver or a sibling field.
 * Expressions the compiler cannot handle are left to run-time checking.
 */
bool skein_check_default_references(
    struct function_code *code, struct ast *expression,
    const std::vector<std::string> &forbidden,
    const std::string &module, std::string &error);

/* Compile every destructuring parameter pattern for `code`. */
std::shared_ptr<const compiled_pattern_set>
skein_compile_parameter_patterns(struct function_code *code,
                                 const std::string &module,
                                 std::string &error);

#endif
