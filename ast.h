#ifndef _AST_H_
#define _AST_H_
#include "symbol.h"
#include "valuetype.h"

extern int yylineno;
void yyerror(const char *s,...);

enum bifs {
    B_sqrt = 1,
    B_exp,
    B_log,
    B_abs,
    B_print,
    B_list,
    B_dict,
    B_int,
    B_float,
    B_str,
    B_format,
    B_command,
    B_command_status,
    B_read_file,
    B_write_file,
    B_append_file,
    B_file_exists,
    B_file_open,
    B_file_read,
    B_file_write,
    B_file_seek,
    B_file_tell,
    B_file_eof,
    B_file_close,
    B_file_flush,
    B_input,
    B_split,
    B_split_ws,
    B_trim,
    B_find,
    B_substr,
    B_bytes_from_string,
    B_bytes_to_string,
    B_bytes_len,
    B_bytes_get,
    B_bytes_slice,
    B_bytes_concat,
    B_bytes_hex,
    B_bytes_from_hex,
    B_bytes_to_int,
    B_int_to_bytes,
    B_read_bytes,
    B_write_bytes,
    B_append_bytes,
    B_file_read_bytes,
    B_file_write_bytes
};

struct ast {
    int nodetype;
    struct ast *l;
    struct ast *r;
};

struct fnret {
    int nodetype;
    struct ast *v;
};

struct nbreak {
    int nodetype;
};

struct ncontinue {
    int nodetype;
};

struct nscope {
    int nodetype;
    struct ast *ast;
};

struct funcdef {
    int nodetype;
    char *name;
    struct symlist *sl;
    struct ast *ast;
    int moved; /* body/params transferred to a runtime function value */
};

struct ufncall {
    int nodetype;
    struct ast *callee;
    struct ast *args;
};

struct flow {
    int nodetype;
    struct ast *cond;
    struct ast *tl;
    struct ast *el;
};

struct forloop {
    int nodetype;
    struct ast *init;
    struct ast *cond;
    struct ast *step;
    struct ast *body;
};

struct val {
    int nodetype;
    value_t *val;
};

struct list_node {
    int nodetype;
    ast *value;
    ast *next;
};

struct dict_node {
    int nodetype;
    ast *key;
    ast *value;
    ast *next;
};

struct symdef {
    int nodetype;
    char *name;
    struct ast *v;
};

struct symref {
    int nodetype;
    char *name;
};

struct symasgn {
    int nodetype;
    struct ast *target;
    struct ast *v;
};

struct ast *newast(int nodetype, struct ast *l, struct ast *r);
struct ast *newcmp(int cmptype, struct ast *l, struct ast *r);
struct ast *newcall(struct ast *callee, struct ast *l);
struct ast *newref(char *s);
struct ast *newasgn(struct ast *target, struct ast *v);
struct ast *newscope(struct ast *v);
struct ast *newint(long long i);
struct ast *newfloat(double d);
struct ast *newstr(char *s);
struct ast *newbuiltin(int functype);
struct ast *newnullast();
struct ast *newflow(int nodetype, struct ast *cond, struct ast *tl, struct ast *tr);
struct ast *newfor(struct ast *init, struct ast *cond, struct ast *step, struct ast *body);
struct ast *newsname(char *s);
struct ast *newvar(char *s, struct ast *v);

struct ast *newret(struct ast *v);
struct ast *newbrk();
struct ast *newctn();

struct ast *newfunc(char *s, struct symlist *sl, struct ast *ast);
std::shared_ptr<function_code> get_function_code(struct funcdef *node);
void release_function_code(struct funcdef *node);


struct ast *newlist(struct ast *v, struct ast *l);
struct ast *newdict(struct ast *k, struct ast *v, struct ast *l);


void treefree(struct ast *);

#endif