#ifndef SKEIN_USER_MACROS_H
#define SKEIN_USER_MACROS_H

#include "ast.h"

#include <string>
#include <vector>

/* Names written as `#name` in a template body are encoded as ordinary NAME
 * tokens with this non-lexable prefix.  This lets every existing grammar slot
 * that accepts an identifier accept a placeholder without duplicating rules. */
#define USER_MACRO_PARAM_PREFIX "\001macroparam:"
/* `#name...` in an argument list is lexed separately so expansion can splice
 * a whole expression sequence rather than produce one invalid expression. */
#define USER_MACRO_EXPAND_PREFIX "\001macroexpand:"

enum class user_macro_param_kind {
    expr,
    ident,
    expr_list,
    block,
};

struct user_macro_param {
    user_macro_param_kind kind;
    std::string name;
    int line = 0;
    int column = 0;
};

struct user_macro_template {
    std::string name;
    std::vector<user_macro_param> params;
    struct ast *body = NULL;
    int line = 0;
    int column = 0;
};

void user_macros_reset();
void user_macros_push();
void user_macros_pop();
void user_macros_set_template_mode(bool enabled);

/* Implemented by the parser, which owns the parameter list of the macro
 * declaration currently being parsed.  The lexer uses it to distinguish a
 * block fragment statement from an ordinary placeholder expression. */
bool macro_param_is_block(const char *name);

bool user_macro_define(const char *name,
                       const std::vector<user_macro_param> &params,
                       struct ast *body, int line, int column);

struct ast *expand_macro(struct ast *callee, struct ast *args,
                         struct ast *block, int line, int column,
                         bool generator);
struct ast *expand_deferred_macro(struct ast *call);

#endif
