#ifndef _AST_H_
#define _AST_H_
#include "symbol.h"
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
constexpr int NODE_DESTRUCTURE = '`';
constexpr int NODE_ENUM_DECL = '<';
constexpr int NODE_ENUM_VARIANT = '>';
constexpr int NODE_ENUM_LITERAL = '?';

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
    B_list_sort_by,
    B_list_sort_desc,
    B_list_is_sorted,
    B_list_append,
    B_list_map,
    B_list_filter,
    B_list_reduce,
    B_list_unique,
    B_list_unique_by,
    B_list_group_by,
    B_list_chunk,
    B_list_window,
    B_list_zip,
    B_list_top_k,
    B_list_binary_search,
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
    B_char,
    B_byte,
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
    B_bytes_from_char,
    B_char_from_bytes,
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
    B_net_tcp_connect_async,
    B_net_tcp_listen_async,
    B_net_accept_async,
    B_net_send_async,
    B_net_recv_async,
    B_net_resolve_async,
    B_net_shutdown_async,
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
    B_task_done,
    B_task_catch,
    B_task_try_wait,
    B_task_try_wait_all,
    B_task_retry,
    B_task_timeout,
    B_task_rate_limit,
    B_task_acquire,
    B_task_bounded_parallel_map,
    B_generator_next,
    B_generator_done,
    B_generator_close,
    B_result_ok,
    B_result_err,
    B_result_is_ok,
    B_result_is_err,
    B_result_unwrap,
    B_result_unwrap_or,
    B_result_value,
    B_result_error,
    B_result_capture,
    B_error_new,
    B_error_kind,
    B_error_message,
    B_io_read,
    B_io_write,
    B_io_write_all,
    B_io_flush,
    B_io_close,
    B_io_eof,
    B_io_readable,
    B_io_writable,
    B_io_seekable,
    B_io_seek,
    B_io_tell,
    B_io_read_exact,
    B_io_read_all,
    B_io_read_line,
    B_io_copy,
    B_io_buffered,
    B_io_read_async,
    B_io_write_async,
    B_http_request,
    B_http_get,
    B_http_post,
    B_http_request_async,
    B_http_get_async,
    B_http_post_async,
    B_http_listen,
    B_http_listen_tls,
    B_http_accept,
    B_http_accept_async,
    B_http_respond,
    B_http_respond_async,
    B_http_serve,
    B_http_upgrade,
    B_http_cookies,
    B_http_read_body,
    B_http_close,
    B_http2_request,
    B_http2_get,
    B_http2_post,
    B_http2_request_async,
    B_http2_get_async,
    B_http2_post_async,
    B_http2_request_many,
    B_http2_request_many_async,
    B_http2_listen,
    B_http2_listen_tls,
    B_http2_serve,
    B_tls_client,
    B_tls_server,
    B_tls_alpn,
    B_ws_connect,
    B_ws_connect_async,
    B_ws_accept,
    B_ws_send,
    B_ws_send_async,
    B_ws_receive,
    B_ws_receive_async,
    B_ws_ping,
    B_ws_close,
    B_json_parse,
    B_json_try_parse,
    B_json_stringify,
    B_enum_decode,
    B_enum_tag,
    B_struct_decode,
    B_lru_new,
    B_lru_get,
    B_lru_peek,
    B_lru_put,
    B_lru_delete,
    B_lru_has,
    B_lru_keys,
    B_lru_values,
    B_lru_len,
    B_lru_capacity,
    B_lru_clear,
    B_encoding_decode,
    B_encoding_encode,
    B_encoding_supports,
    B_unicode_normalize,
    B_unicode_is_normalized,
    B_unicode_version,
    B_read_text,
    B_write_text,
    B_regex_match,
    B_regex_search,
    B_regex_find_all,
    B_regex_replace,
    B_regex_split,
    B_time_now,
    B_time_now_ms,
    B_time_monotonic_ms,
    B_time_utc_offset,
    B_time_format,
    B_time_parse,
    B_time_sleep,
    B_random_seed,
    B_random_int,
    B_random_float,
    B_random_bytes,
    B_random_choice,
    B_random_shuffle,
    B_random_uuid,
    B_encoding_base64_encode,
    B_encoding_base64_decode,
    B_encoding_url_encode,
    B_encoding_url_decode,
    B_fs_listdir,
    B_fs_mkdir,
    B_fs_remove,
    B_fs_rename,
    B_fs_stat,
    B_fs_cwd,
    B_fs_chdir,
    B_fs_path_join,
    B_fs_path_basename,
    B_fs_path_dirname,
    B_fs_path_extname,
    B_fs_path_abs,
    B_fs_path_normalize,
    B_env_get,
    B_env_has,
    B_env_set,
    B_env_unset,
    B_env_all,
    B_process_run,
    B_crypto_sha256,
    B_crypto_hmac_sha256,
    B_compress_gzip,
    B_compress_gunzip,
    B_compress_zlib,
    B_compress_unzlib,
    B_compress_deflate,
    B_compress_inflate,
    B_process_spawn,
    B_process_wait,
    B_process_kill,
    B_process_close,
    B_process_pid,
    B_worker_pool,
    B_worker_call,
    B_worker_map,
    B_worker_submit,
    B_worker_wait,
    B_worker_shutdown,
    B_worker_stats
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
struct ast *newstr(char *s);
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