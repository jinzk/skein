#ifndef SKEIN_MACROS_H
#define SKEIN_MACROS_H

/*
 * Restricted AST-level expansion for the first batch of built-in macros.
 * The parser calls this before typechecking; macros never produce a new AST
 * node kind and never introduce a runtime opcode.
 */
struct ast;

struct ast *expand_builtin_macro(struct ast *callee, struct ast *args,
                                 struct ast *block, int line, int column,
                                 bool is_generator);

#endif
