#ifndef SKEIN_USER_MACROS_H
#define SKEIN_USER_MACROS_H

#include "ast.h"

#include <string>
#include <vector>

/* False for syntax-only tools (for example formatter validation) that must
 * not resolve or load imported packages. */
extern thread_local bool g_resolve_imported_macros;

/* Names written as `#name` in a template body are encoded as ordinary NAME
 * tokens with this non-lexable prefix.  This lets every existing grammar slot
 * that accepts an identifier accept a placeholder without duplicating rules. */
#define USER_MACRO_PARAM_PREFIX "\001macroparam:"
/* `#name...` in an argument list is lexed separately so expansion can splice
 * a whole expression sequence rather than produce one invalid expression. */
#define USER_MACRO_EXPAND_PREFIX "\001macroexpand:"
/* `#N` in a macro call's trailing block is encoded as an ordinary NAME token
 * with this prefix so existing grammar slots accept it without new rules.
 * It is resolved positionally against that call's arguments, never by name. */
#define USER_MACRO_ARG_PREFIX "\001macroarg:"

enum class user_macro_param_kind {
    expr,
    ident,
    expr_list,
    block,
    type,
};

struct user_macro_param {
    user_macro_param_kind kind;
    std::string name;
    int line = 0;
    int column = 0;
};

struct user_macro_template {
    std::string name;
    std::string source;
    std::vector<user_macro_param> params;
    struct ast *body = NULL;
    int line = 0;
    int column = 0;
};

void user_macros_reset();
void user_macros_push();
void user_macros_pop();
void user_macros_set_template_mode(bool enabled);
void user_macros_set_defer_block_calls(bool enabled);
bool user_macro_arg_placeholder(const char *name, std::string *digits);
bool user_macros_in_placeholder_context();
bool user_macro_callee_is_defined(struct ast *callee);

/* Cross-package macro exports.  The export key is the canonical source path
 * of the package being parsed; it is set only for the nested parse that reads
 * exported templates, never for ordinary evaluation. */
void user_macros_set_export_key(const char *key);
std::string user_macros_export_key();
void user_macros_clear_exports();
bool user_macros_has_exports(const char *key);
bool user_macro_import_exported(const char *key, const char *name,
                                const char *alias, int line, int column);

/* Implemented by the parser, which owns the parameter list of the macro
 * declaration currently being parsed.  The lexer uses it to distinguish a
 * block fragment statement from an ordinary placeholder expression. */
bool macro_param_is_block(const char *name);

bool user_macro_define(const char *name,
                       const std::vector<user_macro_param> &params,
                       struct ast *body, int line, int column);

/* Read-only view of user macros visible on the current thread, for check-only
 * tools such as LSP completion and hover. */
struct user_macro_info {
    std::string name;
    std::string signature;
    std::string source;
    int line = 0;
    int column = 0;
    bool imported = false;
};

std::vector<user_macro_info> user_macros_snapshot();

struct ast *expand_macro(struct ast *callee, struct ast *args,
                         struct ast *block, int line, int column,
                         bool generator);
struct ast *expand_deferred_macro(struct ast *call);

#endif
