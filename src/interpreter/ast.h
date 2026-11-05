#ifndef _AST_H_
#define _AST_H_
#include "symbol.h"
#include "builtin_ids.h"
#include "valuetype.h"

extern int yylineno;
void yyerror(const char *s,...);

/* AST node tags introduced by match/enum/destructuring.
 *
 * The parser still uses single-character tags for historical reasons; keep
 * this table in sync with ast.cpp's treefree and eval.cpp's dispatch.
 *
 *   '7'  match expression
 *   '8'  match arm
 *   '9'  literal pattern
 *   'e'  binding pattern (name == NULL is a wildcard)
 *   'h'  container pattern (name == NULL: dict; name != NULL: struct/enum)
 *   'z'  list pattern
 *   ':'  rest pattern
 *   '@'  as-pattern (whole-value binding)
 *   '`'  destructuring declaration
 *   '<'  enum declaration
 *   '>'  enum variant
 *   '?'  enum literal
 */
constexpr int NODE_MATCH = '7';
constexpr int NODE_MATCH_ARM = '8';
constexpr int NODE_PATTERN_LITERAL = '9';
constexpr int NODE_PATTERN_BINDING = 'e';
constexpr int NODE_PATTERN_CONTAINER = 'h';
constexpr int NODE_PATTERN_LIST = 'z';
constexpr int NODE_PATTERN_REST = ':';
constexpr int NODE_PATTERN_AS = '@';
constexpr int NODE_PATTERN_NEVER = '#';
constexpr int NODE_PATTERN_TYPE = 0x100;
constexpr int NODE_DESTRUCTURE = '`';
constexpr int NODE_ENUM_DECL = '<';
constexpr int NODE_ENUM_VARIANT = '>';
constexpr int NODE_ENUM_LITERAL = '?';


struct ast {
    int nodetype;
    int line;
    int column;
    struct ast *l;
    struct ast *r;
};

struct fnret {
    int nodetype;
    int line;
    int column;
    struct ast *v;
};

struct nbreak {
    int nodetype;
    int line;
    int column;
    char *label;
};

struct ncontinue {
    int nodetype;
    int line;
    int column;
    char *label;
};

struct label_node {
    int nodetype;
    int line;
    int column;
    char *name;
    struct ast *stmt;
};

struct control_stmt {
    int nodetype;
    int line;
    int column;
    struct ast *value;
};

struct on_error_node {
    int nodetype;
    int line;
    int column;
    struct ast *call;
    char *name;
    struct ast *body;
};

struct is_node {
    int nodetype;
    int line;
    int column;
    struct ast *value;
    struct type_constraint *type;
};

struct spread_node {
    int nodetype;
    int line;
    int column;
    struct ast *value;
};

struct nscope {
    int nodetype;
    int line;
    int column;
    struct ast *ast;
};

struct funcdef {
    int nodetype;
    int line;
    int column;
    char *name;
    struct symlist *sl;
    struct ast *ast;
    int moved; /* body/params transferred to a runtime function value */
    int exported;
    int is_async;
    int is_generator;
};

struct ufncall {
    int nodetype;
    int line;
    int column;
    struct ast *callee;
    struct ast *args;
};

struct flow {
    int nodetype;
    int line;
    int column;
    struct ast *cond;
    struct ast *tl;
    struct ast *el;
};

struct forloop {
    int nodetype;
    int line;
    int column;
    struct ast *init;
    struct ast *cond;
    struct ast *step;
    struct ast *body;
};

struct forinloop {
    int nodetype;
    int line;
    int column;
    char *name;
    char *value_name;
    struct ast *iterable;
    struct ast *body;
};

struct switch_node {
    int nodetype;
    int line;
    int column;
    struct ast *expr;
    struct ast *cases;
};

struct case_node {
    int nodetype;
    int line;
    int column;
    struct ast *values;
    struct ast *body;
    struct ast *next;
};

/* Pattern matching nodes.
 *
 * nodetype:
 *   '7' match_node
 *   '8' match_arm
 *   '9' literal pattern      (literal)
 *   'e' binding pattern      (name == NULL => wildcard)
 *   'h' container pattern    (name == NULL => dict; name != NULL => struct)
 *   'z' list pattern         (items)
 *   ':' rest pattern         (name == NULL => ignore rest)
 */
struct pattern_field {
    char *key;                 /* NULL for a rest field */
    char *rest_name;           /* rest binding name, may be NULL */
    struct ast *pattern;       /* NULL for a rest field */
    struct pattern_field *next;
};

struct pattern_node {
    int nodetype;
    int line;
    int column;
    char *name;
    char *module;
    char *variant;
    struct ast *literal;
    struct type_constraint *type;
    struct pattern_field *fields;
    struct ast *items;
    struct ast *next;
};

struct match_arm {
    int nodetype;
    int line;
    int column;
    struct ast *pattern;
    struct ast *guard;
    struct ast *body;
    struct ast *next;
};

struct match_node {
    int nodetype;
    int line;
    int column;
    struct ast *expr;
    struct ast *arms;
};

struct val {
    int nodetype;
    int line;
    int column;
    value_t *val;
};

struct list_node {
    int nodetype;
    int line;
    int column;
    ast *value;
    ast *next;
};

struct dict_node {
    int nodetype;
    int line;
    int column;
    ast *key;
    ast *value;
    ast *next;
};

struct symdef {
    int nodetype;
    int line;
    int column;
    char *name;
    struct ast *v;
    struct type_constraint *type;
    int exported;
    int is_const;
};

struct destructure_node {
    int nodetype;
    int line;
    int column;
    struct ast *pattern;
    struct ast *value;
    int is_const;
};

struct struct_member {
    int nodetype;
    int line;
    int column;
    char *name;
    struct type_constraint *type;
    struct ast *func;
    struct struct_member *next;
};

struct struct_node {
    int nodetype;
    int line;
    int column;
    char *name;
    char *module;
    struct struct_member *members;
    int exported;
};

struct impl_node {
    int nodetype;
    int line;
    int column;
    char *type_name;
    char *trait_name;
    char *module;
    struct struct_member *methods;
};

struct trait_method {
    int nodetype;
    int line;
    int column;
    char *name;
    struct symlist *params;
    struct trait_method *next;
};

struct trait_node {
    int nodetype;
    int line;
    int column;
    char *name;
    char *module;
    struct trait_method *methods;
    int exported;
};

struct struct_init {
    int nodetype;
    int line;
    int column;
    char *name;
    struct ast *value;
    struct struct_init *next;
};

struct struct_lit_node {
    int nodetype;
    int line;
    int column;
    char *name;
    char *module;
    struct struct_init *fields;
};

struct enum_variant {
    int nodetype;
    int line;
    int column;
    char *name;
    struct struct_member *fields;
    struct enum_variant *next;
};

struct enum_node {
    int nodetype;
    int line;
    int column;
    char *name;
    char *module;
    struct enum_variant *variants;
    int exported;
};

struct enum_lit_node {
    int nodetype;
    int line;
    int column;
    char *name;
    char *module;
    char *variant;
    struct struct_init *fields;
};

struct package_node {
    int nodetype;
    int line;
    int column;
    char *name;
    struct ast *body;
};

struct import_node {
    int nodetype;
    int line;
    int column;
    char *path;
    char *alias;
    char *base;
};

struct symref {
    int nodetype;
    int line;
    int column;
    char *name;
};

struct symasgn {
    int nodetype;
    int line;
    int column;
    struct ast *target;
    struct ast *v;
};

struct compound_asgn {
    int nodetype;
    int line;
    int column;
    struct ast *target;
    int op;
    struct ast *v;
};

struct incdec_node {
    int nodetype;
    int line;
    int column;
    struct ast *target;
    int delta;
    int prefix;
};

struct ast *newast(int nodetype, struct ast *l, struct ast *r);
struct ast *newcmp(int cmptype, struct ast *l, struct ast *r);
struct ast *newcall(struct ast *callee, struct ast *l);
struct ast *newref(char *s);
struct ast *newasgn(struct ast *target, struct ast *v);
struct ast *newcompound(struct ast *target, int op, struct ast *v);
struct ast *newincdec(struct ast *target, int delta, bool prefix);
struct ast *newscope(struct ast *v);
struct ast *newint(long long i);
struct ast *newfloat(double d);
struct ast *newbool(bool b);
struct ast *newstr(const char *s);
struct ast *newchar(long long c);
struct ast *newbuiltin(int functype);
struct ast *newnullast();
struct ast *newflow(int nodetype, struct ast *cond, struct ast *tl, struct ast *tr);
struct ast *newfor(struct ast *init, struct ast *cond, struct ast *step, struct ast *body);
struct ast *newforin(char *name, char *value_name, struct ast *iterable,
                     struct ast *body);
struct ast *newswitch(struct ast *expr, struct ast *cases);
struct ast *newcase(struct ast *values, struct ast *body, struct ast *next);
struct ast *append_case(struct ast *head, struct ast *node);
struct ast *newmatch(struct ast *expr, struct ast *arms);
struct ast *newmatcharm(struct ast *pattern, struct ast *guard,
                        struct ast *body, struct ast *next);
struct ast *append_matcharm(struct ast *head, struct ast *node);
struct ast *newpatternlit(struct ast *literal);
struct ast *newpatternname(char *name);
struct ast *newpatternrest(char *name);
struct ast *newpatternas(struct ast *pattern, char *name);
struct ast *newpatternnever();
struct ast *newpatterntype(struct type_constraint *type);
struct ast *newpatterndict(struct pattern_field *fields);
struct ast *newpatternlist(struct ast *items);
struct ast *newpatternstruct(char *name, char *module,
                             struct pattern_field *fields);
struct ast *newpatternvariant(char *name, char *module, char *variant,
                              struct pattern_field *fields);
struct pattern_field *newpatternfield(char *key, struct ast *pattern,
                                      struct pattern_field *next);
struct pattern_field *newpatternrestfield(char *name,
                                          struct pattern_field *next);
struct pattern_field *append_patternfield(struct pattern_field *head,
                                          struct pattern_field *node);
struct ast *append_patternitem(struct ast *head, struct ast *node);
struct ast *newsname(char *s);
struct ast *newvar(char *s, struct ast *v, bool exported = false,
                   bool is_const = false, struct type_constraint *type = NULL);
struct ast *newdestructure(struct ast *pattern, struct ast *value,
                           bool is_const = false);

struct ast *newret(struct ast *v);
struct ast *newbrk(char *label = NULL, int line = 0, int column = 0);
struct ast *newctn(char *label = NULL, int line = 0, int column = 0);
struct ast *newlabel(char *name, struct ast *stmt);
struct ast *newfail(struct ast *v);
struct ast *newrecover(struct ast *v);
struct ast *newpass(struct ast *v);
struct ast *newonerror(struct ast *call, char *name, struct ast *body);
struct ast *newis(struct ast *value, struct type_constraint *type);
struct ast *newspread(struct ast *value);

struct ast *newfunc(char *s, struct symlist *sl, struct ast *ast,
                    bool exported = false, bool is_async = false,
                    bool is_generator = false);
struct ast *newawait(struct ast *value);
struct ast *newyield(struct ast *value);
struct ast *newdefer(struct ast *value);
struct ast *newpackage(char *name, struct ast *body);
struct ast *newimport(char *path, char *alias);
void push_func_location(int line, int column);
void clear_func_locations();
std::shared_ptr<function_code> get_function_code(struct funcdef *node);
void release_function_code(struct funcdef *node);
/* Free a parameter list, including any default-value expression ASTs.
 * symlistfree() itself stays runtime-only and does not inspect AST nodes. */
void symlistfree_full(struct symlist *sl);


struct ast *newlist(struct ast *v, struct ast *l);
struct ast *newdict(struct ast *k, struct ast *v, struct ast *l);

struct ast *newstructdecl(char *name, char *module,
                          struct struct_member *members,
                          bool exported = false);
struct ast *newstructfield(char *name, struct type_constraint *type);
struct ast *newstructmethod(char *name, struct ast *func);
struct ast *append_struct_member(struct ast *head, struct ast *member);
struct ast *newimpl(char *type_name, char *trait_name, char *module,
                    struct struct_member *methods);
struct ast *newtraitdecl(char *name, char *module,
                         struct trait_method *methods,
                         bool exported = false);
struct ast *newtraitmethod(char *name, struct symlist *params);
struct ast *append_trait_method(struct ast *head, struct ast *method);
struct ast *newstructlit(char *name, char *module,
                         struct struct_init *fields);
struct ast *newenumdecl(char *name, char *module,
                        struct enum_variant *variants,
                        bool exported = false);
struct ast *newenumvariant(char *name, struct struct_member *fields,
                           struct enum_variant *next);
struct enum_variant *append_enumvariant(struct enum_variant *head,
                                        struct enum_variant *node);
struct ast *newenumlit(char *name, char *module, char *variant,
                       struct struct_init *fields);
struct ast *newstructinit(char *name, struct ast *value);
struct ast *append_struct_init(struct ast *head, struct ast *init);


void treefree(struct ast *);

#endif