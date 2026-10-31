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
    B_sin,
    B_cos,
    B_tan,
    B_asin,
    B_acos,
    B_atan,
    B_atan2,
    B_pow,
    B_floor,
    B_ceil,
    B_round,
    B_trunc,
    B_log2,
    B_log10,
    B_hypot,
    B_min,
    B_max,
    B_len,
    B_range,
    B_list_slice,
    B_list_contains,
    B_list_index,
    B_list_reverse,
    B_list_sort,
    B_list_append,
    B_list_map,
    B_list_filter,
    B_list_reduce,
    B_dict_get,
    B_dict_has,
    B_dict_keys,
    B_dict_values,
    B_dict_items,
    B_dict_delete,
    B_dict_merge,
    B_string_join,
    B_string_replace,
    B_string_starts_with,
    B_string_ends_with,
    B_string_contains,
    B_string_repeat,
    B_string_trim_start,
    B_string_trim_end,
    B_print,
    B_list,
    B_dict,
    B_int,
    B_float,
    B_bool,
    B_str,
    B_kind_of,
    B_type_name,
    B_type_module,
    B_type_source,
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
    B_file_write_bytes,
    B_net_tcp_connect,
    B_net_tcp_listen,
    B_net_accept,
    B_net_send,
    B_net_recv,
    B_net_close,
    B_net_resolve,
    B_net_shutdown,
    B_net_local_addr,
    B_net_peer_addr,
    B_ffi_open,
    B_ffi_close,
    B_ffi_call,
    B_ffi_buffer,
    B_ffi_buffer_len,
    B_ffi_buffer_bytes,
    B_ffi_buffer_free,
    B_task_sleep,
    B_task_yield,
    B_task_wait,
    B_task_wait_all,
    B_task_cancel,
    B_task_done
};

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
struct ast *newstr(char *s);
struct ast *newbuiltin(int functype);
struct ast *newnullast();
struct ast *newflow(int nodetype, struct ast *cond, struct ast *tl, struct ast *tr);
struct ast *newfor(struct ast *init, struct ast *cond, struct ast *step, struct ast *body);
struct ast *newforin(char *name, char *value_name, struct ast *iterable,
                     struct ast *body);
struct ast *newswitch(struct ast *expr, struct ast *cases);
struct ast *newcase(struct ast *values, struct ast *body, struct ast *next);
struct ast *append_case(struct ast *head, struct ast *node);
struct ast *newsname(char *s);
struct ast *newvar(char *s, struct ast *v, bool exported = false,
                   bool is_const = false, struct type_constraint *type = NULL);

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
                    bool exported = false);
struct ast *newpackage(char *name, struct ast *body);
struct ast *newimport(char *path, char *alias);
void push_func_location(int line, int column);
void clear_func_locations();
std::shared_ptr<function_code> get_function_code(struct funcdef *node);
void release_function_code(struct funcdef *node);


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
struct ast *newstructinit(char *name, struct ast *value);
struct ast *append_struct_init(struct ast *head, struct ast *init);


void treefree(struct ast *);

#endif