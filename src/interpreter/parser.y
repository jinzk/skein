%{

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

#include "ast.h"
#include "decorators.h"
#include "dict.h"
#include "diagnostics.h"
#include "eval_context.h"
#include "repl.h"
#include "runtime_state.h"
#include "scopestack.h"
#include "gc.h"
#include "eval.h"
#include "vm.h"
#include "static_type.h"
#include "typecheck.h"

extern int yylex();
extern int yyparse();

static int flow_keyword_line = 0;
static int flow_keyword_column = 0;
static std::vector<int> func_yield_counts;

extern int prev_token_line;

static void check_do_while_line() {
    if (prev_token_line != 0 && g_token_line != prev_token_line) {
        diag_set_location(g_token_line, g_token_column);
        yyerror("syntax error while must be on the same line as the "
                "preceding '}'");
    }
}

static void start_function() {
    func_yield_counts.push_back(0);
}

static void note_yield() {
    if (!func_yield_counts.empty())
        ++func_yield_counts.back();
}

static bool finish_function() {
    bool generator =
        !func_yield_counts.empty() && func_yield_counts.back() > 0;
    if (!func_yield_counts.empty())
        func_yield_counts.pop_back();
    return generator;
}

static char *join_import_path(const char *left, const char *right) {
    size_t left_len = std::strlen(left);
    size_t right_len = std::strlen(right);
    char *path = (char *)std::malloc(left_len + right_len + 2);
    if (!path) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(path, left, left_len);
    path[left_len] = '.';
    std::memcpy(path + left_len + 1, right, right_len + 1);
    return path;
}

static char *join_import_prefix(const char *prefix, char *path) {
    size_t prefix_len = std::strlen(prefix);
    size_t path_len = std::strlen(path);
    char *result = (char *)std::malloc(prefix_len + path_len + 1);
    if (!result) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(result, prefix, prefix_len);
    std::memcpy(result + prefix_len, path, path_len + 1);
    std::free(path);
    return result;
}

static char *copy_text(const char *text) {
    size_t len = std::strlen(text) + 1;
    char *copy = (char *)std::malloc(len);
    if (!copy) {
        yyerror("out of space");
        std::exit(0);
    }
    std::memcpy(copy, text, len);
    return copy;
}

/* Python-style slicing is lowered to a hidden builtin call so both the AST
 * interpreter and the bytecode VM share one implementation.  Omitted bounds
 * are passed as null values. */
static struct ast *newslice(struct ast *container, struct ast *start,
                            struct ast *stop, struct ast *step) {
    struct ast *args = newast('L', step ? step : newnullast(), NULL);
    args = newast('L', stop ? stop : newnullast(), args);
    args = newast('L', start ? start : newnullast(), args);
    args = newast('L', container, args);
    return newcall(newbuiltin(B_slice), args);
}

static struct ast *with_decorators(struct ast *method,
                                   struct ast *decorators) {
    if (method)
        ((struct struct_member *)method)->decorators = decorators;
    return method;
}

/* -------------------------------------------------------------------------
 * extern lib bindings: build a manifest dict at parse time, then hand it to
 * the FFI builtin as a normal call expression.
 * ---------------------------------------------------------------------- */

struct val;

static struct ast *ffi_value_node(struct value_t *value) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    a->line = 0;
    a->column = 0;
    a->val = value;
    return (struct ast *)a;
}

static struct value_t *ffi_dict_new() {
    return (struct value_t *)newdictval();
}

static struct value_t *ffi_dict_set(struct value_t *dict, const char *key,
                                    struct value_t *value) {
    dict_append_item((struct dict_value_t *)dict, newstrval(key), value);
    return dict;
}

static struct value_t *ffi_dict_value(const struct value_t *dict,
                                      const char *key) {
    if (!dict || dict->type != T_dict)
        return NULL;
    const struct dict_value_t *d = (const struct dict_value_t *)dict;
    for (const auto &entry : d->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((const struct string_value_t *)entry.first)->value,
                        key) == 0)
            return entry.second;
    }
    return NULL;
}

static const char *ffi_dict_string(const struct value_t *dict,
                                   const char *key) {
    struct value_t *value = ffi_dict_value(dict, key);
    if (!value || value->type != T_string)
        return NULL;
    return ((const struct string_value_t *)value)->value;
}

static struct ast *ffi_items_new() {
    return ffi_value_node((struct value_t *)newlistval());
}

static struct ast *ffi_list_append(struct ast *list_node,
                                   struct ast *item) {
    struct list_value_t *list =
        (struct list_value_t *)((struct val *)list_node)->val;
    list->payload->items.push_back(dupval(((struct val *)item)->val));
    return list_node;
}

static struct ast *ffi_params_new() {
    struct value_t *dict = ffi_dict_new();
    ffi_dict_set(dict, "types", (struct value_t *)newlistval());
    ffi_dict_set(dict, "names", (struct value_t *)newlistval());
    return ffi_value_node(dict);
}

static struct value_t *ffi_params_list(struct ast *params, const char *key) {
    return ffi_dict_value(((struct val *)params)->val, key);
}

static void ffi_params_push(struct ast *params, struct value_t *type,
                            char *name) {
    struct value_t *types = ffi_params_list(params, "types");
    struct value_t *names = ffi_params_list(params, "names");
    ((struct list_value_t *)types)->payload->items.push_back(type);
    ((struct list_value_t *)names)->payload->items.push_back(
        newstrval(name ? name : ""));
    if (name)
        std::free(name);
}

static struct ast *ffi_param_one(struct ast *type, char *name) {
    struct value_t *dict = ffi_dict_new();
    ffi_dict_set(dict, "type", dupval(((struct val *)type)->val));
    ffi_dict_set(dict, "name", newstrval(name ? name : ""));
    if (name)
        std::free(name);
    treefree(type);
    return ffi_value_node(dict);
}

static void ffi_param_append(struct ast *params, struct ast *param) {
    struct value_t *dict = ((struct val *)param)->val;
    struct value_t *type = ffi_dict_value(dict, "type");
    struct value_t *name = ffi_dict_value(dict, "name");
    const char *text = (name && name->type == T_string)
                           ? ((struct string_value_t *)name)->value
                           : "";
    ffi_params_push(params, dupval(type), copy_text(text));
}

static struct ast *ffi_annotations_new() {
    return ffi_value_node((struct value_t *)newlistval());
}

static struct ast *ffi_annotation_node(char *kind, char *name,
                                       long long index, bool has_index) {
    struct value_t *dict = ffi_dict_new();
    ffi_dict_set(dict, "kind", newstrval(kind));
    if (has_index)
        ffi_dict_set(dict, "index", newintval(index));
    else
        ffi_dict_set(dict, "name", newstrval(name ? name : ""));
    std::free(kind);
    if (name)
        std::free(name);
    return ffi_value_node(dict);
}

static void ffi_annotation_append(struct ast *annotations,
                                  struct ast *annotation) {
    struct list_value_t *list =
        (struct list_value_t *)((struct val *)annotations)->val;
    list->payload->items.push_back(dupval(((struct val *)annotation)->val));
}

static struct ast *ffi_fields_new() {
    return ffi_value_node(ffi_dict_new());
}

static struct ast *ffi_type_pointer(struct ast *inner) {
    struct value_t *dict = ffi_dict_new();
    ffi_dict_set(dict, "kind", newstrval("pointer"));
    ffi_dict_set(dict, "to", dupval(((struct val *)inner)->val));
    treefree(inner);
    return ffi_value_node(dict);
}

static struct ast *ffi_type_array(struct ast *inner, long long count) {
    struct value_t *dict = ffi_dict_new();
    ffi_dict_set(dict, "kind", newstrval("array"));
    ffi_dict_set(dict, "element", dupval(((struct val *)inner)->val));
    ffi_dict_set(dict, "count", newintval(count));
    treefree(inner);
    return ffi_value_node(dict);
}

static struct value_t *ffi_struct_decl(struct ast *fields) {
    struct value_t *dict = ffi_dict_new();
    ffi_dict_set(dict, "kind", newstrval("struct"));
    ffi_dict_set(dict, "fields", dupval(((struct val *)fields)->val));
    treefree(fields);
    return dict;
}

static struct value_t *ffi_callback_decl(struct ast *params,
                                         struct ast *result) {
    struct value_t *dict = ffi_dict_new();
    ffi_dict_set(dict, "kind", newstrval("callback"));
    ffi_dict_set(dict, "result", dupval(((struct val *)result)->val));
    ffi_dict_set(dict, "args",
                 dupval(ffi_params_list(params, "types")));
    treefree(params);
    treefree(result);
    return dict;
}

static long long ffi_param_index(struct ast *params, const char *name) {
    struct value_t *names = ffi_params_list(params, "names");
    if (!names || names->type != T_list)
        return -1;
    struct list_value_t *list = (struct list_value_t *)names;
    for (size_t i = 0; i < list->payload->items.size(); ++i) {
        struct value_t *entry = list->payload->items[i];
        if (entry->type == T_string &&
            std::strcmp(((struct string_value_t *)entry)->value, name) == 0)
            return (long long)i;
    }
    return -1;
}

static struct value_t *ffi_function_decl(struct ast *params,
                                         struct ast *result,
                                         struct ast *annotations) {
    struct value_t *dict = ffi_dict_new();
    ffi_dict_set(dict, "result", dupval(((struct val *)result)->val));
    ffi_dict_set(dict, "args",
                 dupval(ffi_params_list(params, "types")));

    struct value_t *consumes = NULL;
    struct value_t *retains = NULL;
    if (annotations) {
        struct list_value_t *list =
            (struct list_value_t *)((struct val *)annotations)->val;
        for (struct value_t *annotation : list->payload->items) {
            const char *kind = ffi_dict_string(annotation, "kind");
            if (!kind)
                continue;
            if (std::strcmp(kind, "owns") == 0) {
                const char *free_name =
                    ffi_dict_string(annotation, "name");
                if (!free_name) {
                    yyerror("ffi binding: owns expects a free function name");
                    continue;
                }
                struct value_t *owned = ffi_dict_new();
                ffi_dict_set(owned, "free", newstrval(free_name));
                ffi_dict_set(dict, "returns_owned", owned);
                continue;
            }
            long long index = -1;
            struct value_t *index_value =
                ffi_dict_value(annotation, "index");
            if (index_value && index_value->type == T_int) {
                index = ((struct int_value_t *)index_value)->value;
            } else {
                const char *name = ffi_dict_string(annotation, "name");
                if (name)
                    index = ffi_param_index(params, name);
            }
            struct value_t *types = ffi_params_list(params, "types");
            size_t count =
                types && types->type == T_list
                    ? ((struct list_value_t *)types)->payload->items.size()
                    : 0;
            if (index < 0 || (size_t)index >= count) {
                yyerror("ffi binding: annotation refers to an unknown "
                        "parameter");
                continue;
            }
            if (std::strcmp(kind, "consumes") == 0) {
                if (!consumes)
                    consumes = (struct value_t *)newlistval();
                ((struct list_value_t *)consumes)
                    ->payload->items.push_back(newintval(index));
            } else if (std::strcmp(kind, "retains") == 0) {
                if (!retains)
                    retains = (struct value_t *)newlistval();
                ((struct list_value_t *)retains)
                    ->payload->items.push_back(newintval(index));
            } else {
                yyerror("ffi binding: unknown annotation");
            }
        }
    }
    if (consumes)
        ffi_dict_set(dict, "consumes", consumes);
    if (retains)
        ffi_dict_set(dict, "retains", retains);
    treefree(params);
    treefree(result);
    if (annotations)
        treefree(annotations);
    return dict;
}

static struct ast *ffi_item_node(const char *section, char *name,
                                 struct value_t *decl) {
    struct value_t *dict = ffi_dict_new();
    ffi_dict_set(dict, "section", newstrval(section));
    ffi_dict_set(dict, "name", newstrval(name));
    ffi_dict_set(dict, "decl", decl);
    std::free(name);
    return ffi_value_node(dict);
}

static struct value_t *ffi_build_manifest(const char *binding_name,
                                          struct ast *items_node) {
    struct list_value_t *items =
        (struct list_value_t *)((struct val *)items_node)->val;
    struct value_t *types = ffi_dict_new();
    struct value_t *functions = ffi_dict_new();
    struct value_t *constants = ffi_dict_new();
    for (struct value_t *item : items->payload->items) {
        const char *section = ffi_dict_string(item, "section");
        struct value_t *name = ffi_dict_value(item, "name");
        struct value_t *decl = ffi_dict_value(item, "decl");
        if (!section || !name || !decl)
            continue;
        if (std::strcmp(section, "type") == 0)
            dict_append_item((struct dict_value_t *)types, dupval(name),
                             dupval(decl));
        else if (std::strcmp(section, "function") == 0)
            dict_append_item((struct dict_value_t *)functions, dupval(name),
                             dupval(decl));
        else if (std::strcmp(section, "constant") == 0)
            dict_append_item((struct dict_value_t *)constants, dupval(name),
                             dupval(decl));
    }
    struct value_t *manifest = ffi_dict_new();
    ffi_dict_set(manifest, "ffi", newintval(1));
    ffi_dict_set(manifest, "library", newstrval(binding_name));
    ffi_dict_set(manifest, "types", types);
    ffi_dict_set(manifest, "functions", functions);
    ffi_dict_set(manifest, "constants", constants);
    return manifest;
}

static struct ast *ffi_binding_node(char *name, const char *path,
                                    struct value_t *manifest) {
    struct ast *args = newast(
        'L', ffi_value_node(newstrval(path)),
        newast('L', ffi_value_node(manifest), NULL));
    return newvar(name, newcall(newbuiltin(B_ffi_bind), args));
}

static struct value_t *ffi_empty_manifest(const char *binding_name) {
    struct value_t *manifest = ffi_dict_new();
    ffi_dict_set(manifest, "ffi", newintval(1));
    ffi_dict_set(manifest, "library", newstrval(binding_name));
    ffi_dict_set(manifest, "types", ffi_dict_new());
    ffi_dict_set(manifest, "functions", ffi_dict_new());
    ffi_dict_set(manifest, "constants", ffi_dict_new());
    return manifest;
}

static const char *builtin_field_name(int functype) {
    switch (functype) {
        case B_sqrt: return "sqrt";
        case B_exp: return "exp";
        case B_log: return "log";
        case B_abs: return "abs";
        case B_sin: return "sin";
        case B_cos: return "cos";
        case B_tan: return "tan";
        case B_asin: return "asin";
        case B_acos: return "acos";
        case B_atan: return "atan";
        case B_atan2: return "atan2";
        case B_pow: return "pow";
        case B_floor: return "floor";
        case B_ceil: return "ceil";
        case B_round: return "round";
        case B_trunc: return "trunc";
        case B_log2: return "log2";
        case B_log10: return "log10";
        case B_hypot: return "hypot";
        case B_min: return "min";
        case B_max: return "max";
        case B_len: return "len";
        case B_range: return "range";
        case B_print: return "print";
        case B_println: return "println";
        case B_list: return "list";
        case B_dict: return "dict";
        case B_int: return "int";
        case B_float: return "float";
        case B_bool: return "bool";
        case B_str: return "str";
        case B_char: return "char";
        case B_byte: return "byte";
        case B_kind_of: return "kind_of";
        case B_type_name: return "type_name";
        case B_type_package: return "type_package";
        case B_type_module_name: return "type_module_name";
        case B_type_source: return "type_source";
        case B_format: return "format";
        case B_command: return "command";
        case B_command_status: return "command_status";
        case B_read_file: return "read_file";
        case B_write_file: return "write_file";
        case B_append_file: return "append_file";
        case B_file_exists: return "file_exists";
        case B_file_open: return "file_open";
        case B_file_read: return "file_read";
        case B_file_write: return "file_write";
        case B_file_seek: return "file_seek";
        case B_file_tell: return "file_tell";
        case B_file_eof: return "file_eof";
        case B_file_close: return "file_close";
        case B_file_flush: return "file_flush";
        case B_input: return "input";
        case B_split: return "split";
        case B_split_ws: return "split_ws";
        case B_trim: return "trim";
        case B_find: return "find";
        case B_substr: return "substr";
        case B_bytes_from_string: return "bytes_from_string";
        case B_bytes_to_string: return "bytes_to_string";
        case B_bytes_len: return "bytes_len";
        case B_bytes_get: return "bytes_get";
        case B_bytes_slice: return "bytes_slice";
        case B_bytes_concat: return "bytes_concat";
        case B_bytes_hex: return "bytes_hex";
        case B_bytes_from_hex: return "bytes_from_hex";
        case B_bytes_to_int: return "bytes_to_int";
        case B_int_to_bytes: return "int_to_bytes";
        case B_read_bytes: return "read_bytes";
        case B_write_bytes: return "write_bytes";
        case B_append_bytes: return "append_bytes";
        case B_file_read_bytes: return "file_read_bytes";
        case B_file_write_bytes: return "file_write_bytes";
        default: return NULL;
    }
}

static struct type_constraint *type_constraint_from_name(const char *name) {
    if (!name)
        return new_type_constraint(TYPE_INVALID);
    enum type_constraint_kind kind;
    if (type_constraint_kind_for_builtin_name(name, kind))
        return new_type_constraint(kind);
    return new_named_type_constraint(name,
                                     diag_current_source_name().c_str());
}

static int destructure_param_id = 0;

static struct symlist *new_destructure_param(struct ast *pattern) {
    char name[32];
    std::snprintf(name, sizeof(name), "#%d", ++destructure_param_id);
    struct symbol *symbol = newsym(name);
    struct symlist *params = newsymlist(symbol, NULL, NULL);
    set_symlist_pattern_ast(params, pattern);
    return params;
}

static bool is_placeholder_arg(struct ast *arg) {
    return arg && arg->nodetype == 'N' &&
           std::strcmp(((struct symref *)arg)->name, "$") == 0;
}

static struct ast *replace_single_placeholder(
    struct ast *arg, const std::vector<std::string> &names, size_t &index) {
    if (!is_placeholder_arg(arg))
        return arg;
    treefree(arg);
    return newref(copy_text(names[index++].c_str()));
}

static struct ast *replace_placeholder_args(
    struct ast *args, const std::vector<std::string> &names,
    size_t &index) {
    if (!args)
        return NULL;
    if (args->nodetype == 'L') {
        struct list_node *node = (struct list_node *)args;
        node->value = replace_single_placeholder(
            node->value, names, index);
        node->next = replace_placeholder_args(node->next, names, index);
        return args;
    }
    return replace_single_placeholder(args, names, index);
}

static struct ast *new_call_with_placeholders(
    struct ast *callee, struct ast *args) {
    size_t holes = 0;
    for (struct ast *item = args; item;) {
        if (item->nodetype == 'L') {
            struct list_node *node = (struct list_node *)item;
            if (is_placeholder_arg(node->value))
                ++holes;
            item = node->next;
        } else {
            if (is_placeholder_arg(item))
                ++holes;
            break;
        }
    }
    if (holes == 0)
        return newcall(callee, args);

    std::vector<std::string> names;
    names.reserve(holes);
    for (size_t i = 0; i < holes; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "#%d", ++destructure_param_id);
        names.emplace_back(name);
    }

    size_t index = 0;
    args = replace_placeholder_args(args, names, index);

    std::vector<struct symlist *> params;
    params.reserve(holes);
    for (const std::string &name : names)
        params.push_back(newsymlist(newsym(name.c_str()), NULL, NULL));
    for (size_t i = 0; i + 1 < params.size(); ++i)
        params[i]->next = params[i + 1];

    return newfunc(NULL, params.empty() ? NULL : params[0],
                   newret(newcall(callee, args)), false, false);
}

/*
 * `launch` binds to a call expression, but `launch f(x).wait()` must launch
 * `f(x)` and then call `.wait()` on the task instead of launching the whole
 * chain.  Attribute calls whose receiver is itself a call keep the historical
 * tight-binding behaviour by descending to the receiver call.
 */
static struct ast *launch_target_call(struct ast *node) {
    struct ast *current = node;
    while (current && current->nodetype == 'C') {
        struct ast *callee = ((struct ufncall *)current)->callee;
        if (!callee || callee->nodetype != 'j' || !callee->l ||
            callee->l->nodetype != 'C')
            break;
        current = callee->l;
    }
    return current;
}

/*
 * `struct Name with Trait, pkg.Other { ... }` and the enum form lower to
 * the declaration followed by `impl Trait for Name;` statements.  `with` is
 * not a reserved word: it is parsed as a NAME and validated here, so code
 * can still use `with` as an ordinary identifier elsewhere.
 */
static struct ast *lower_type_with(char *name, char *with_token,
                                   char *traits, void *members,
                                   bool is_enum, bool exported) {
    bool with_ok =
        with_token && std::strcmp(with_token, "with") == 0;
    if (!with_ok) {
        diag_set_location(g_token_line, g_token_column);
        yyerror("expected 'with' after type name");
    }
    std::free(with_token);

    std::vector<std::string> names;
    if (traits) {
        std::string text(traits);
        std::free(traits);
        size_t start = 0;
        for (;;) {
            size_t sep = text.find('\x1f', start);
            std::string item = text.substr(
                start, sep == std::string::npos ? std::string::npos
                                                : sep - start);
            if (!item.empty())
                names.push_back(item);
            if (sep == std::string::npos)
                break;
            start = sep + 1;
        }
    }

    std::string source = diag_current_source_name();
    struct ast *decl = NULL;
    if (is_enum) {
        decl = newenumdecl(copy_text(name), copy_text(source.c_str()),
                           (struct enum_variant *)members, exported);
    } else {
        decl = newstructdecl(copy_text(name), copy_text(source.c_str()),
                             (struct struct_member *)members, exported);
    }
    struct ast *tail = NULL;
    for (size_t i = names.size(); i > 0; --i) {
        struct ast *impl = newimpl(
            copy_text(name), copy_text(names[i - 1].c_str()),
            copy_text(source.c_str()), NULL);
        tail = tail ? newast('L', impl, tail) : impl;
    }
    std::free(name);
    if (!with_ok || !tail)
        return decl;
    return newast('L', decl, tail);
}

static char *concat_with_trait(char *head, char *next) {
    std::string joined = head ? std::string(head) : std::string();
    if (!joined.empty())
        joined.push_back('\x1f');
    if (next)
        joined += next;
    std::free(head);
    std::free(next);
    return copy_text(joined.c_str());
}

static struct type_constraint *type_constraint_from_builtin(int functype) {
    switch (functype) {
        case B_int: return new_type_constraint(TYPE_INT);
        case B_float: return new_type_constraint(TYPE_FLOAT);
        case B_bool: return new_type_constraint(TYPE_BOOL);
        case B_str: return new_type_constraint(TYPE_STRING);
        case B_char: return new_type_constraint(TYPE_CHAR);
        case B_byte: return new_type_constraint(TYPE_BYTE);
        case B_list: return new_type_constraint(TYPE_LIST);
        case B_dict: return new_type_constraint(TYPE_DICT);
        default: {
            const char *name = builtin_field_name(functype);
            yyerror("unknown type name %s", name ? name : "builtin");
            return new_type_constraint(TYPE_INVALID);
        }
    }
}

static struct type_constraint *make_generic_type(struct type_constraint *base,
                                                 struct type_constraint *arg1,
                                                 struct type_constraint *arg2) {
    return skein::typecheck::type_apply_generic(base, arg1, arg2);
}

static struct type_constraint *make_nullable_type(
    struct type_constraint *base) {
    return skein::typecheck::type_apply_nullable(base);
}

struct parsed_type_list {
    struct type_constraint *type;
    struct parsed_type_list *next;
};

static struct parsed_type_list *new_type_list(struct type_constraint *type,
                                               struct parsed_type_list *next) {
    struct parsed_type_list *node =
        (struct parsed_type_list *)std::malloc(
            sizeof(struct parsed_type_list));
    if (!node) {
        yyerror("out of space");
        std::exit(0);
    }
    node->type = type;
    node->next = next;
    return node;
}

static void free_type_list(struct parsed_type_list *list) {
    while (list) {
        struct parsed_type_list *next = list->next;
        free_type_constraint(list->type);
        std::free(list);
        list = next;
    }
}

static struct type_constraint *make_generic_type_list(
    struct type_constraint *base, struct parsed_type_list *list) {
    std::vector<struct type_constraint *> args;
    for (struct parsed_type_list *item = list; item; item = item->next)
        args.push_back(item->type);
    struct type_constraint *type =
        skein::typecheck::type_apply_generic_list(base, args);
    /* type_apply_generic_list takes ownership of the constraint nodes. */
    while (list) {
        struct parsed_type_list *next = list->next;
        std::free(list);
        list = next;
    }
    return type;
}

static struct ast *append_tuple_explist(struct ast *chain,
                                        struct ast *items) {
    if (!items)
        return chain;
    if (items->nodetype == 'L') {
        chain = append_tuple_explist(chain, items->l);
        chain = append_tuple_explist(chain, items->r);
        std::free(items);
        return chain;
    }
    struct ast *node = newlist(items, NULL);
    if (!chain)
        return node;
    struct list_node *tail = (struct list_node *)chain;
    while (tail->next)
        tail = (struct list_node *)tail->next;
    tail->next = node;
    return chain;
}

static struct ast *make_tuple_lambda(struct ast *tuple, struct ast *body) {
    struct symlist *head = NULL;
    struct symlist *tail = NULL;
    for (struct list_node *item = (struct list_node *)tuple; item;
         item = (struct list_node *)item->next) {
        if (!item->value || item->value->nodetype != 'N') {
            yyerror("lambda parameter must be a name");
            treefree(tuple);
            treefree(body);
            return newnullast();
        }
        const char *name = ((struct symref *)item->value)->name;
        struct symlist *param =
            newsymlist(newsym(name ? name : ""), NULL, NULL);
        if (tail)
            tail->next = param;
        else
            head = param;
        tail = param;
    }
    treefree(tuple);
    return newfunc(NULL, head, newret(body), false,
                   finish_function());
}

static struct type_constraint *make_function_type(
    struct parsed_type_list *params, struct type_constraint *result) {
    std::vector<struct type_constraint *> types;
    for (struct parsed_type_list *item = params; item; item = item->next)
        types.push_back(item->type);
    struct type_constraint *type =
        skein::typecheck::type_apply_function(types, result);
    /* type_apply_function takes ownership of the constraint nodes. */
    while (params) {
        struct parsed_type_list *next = params->next;
        std::free(params);
        params = next;
    }
    return type;
}

static struct type_constraint *runtime_type_or_error(
    struct type_constraint *type, const char *where) {
    const char *message = NULL;
    char buffer[128];
    if (skein::typecheck::type_constraint_is_function_type(type)) {
        std::snprintf(buffer, sizeof(buffer),
                      "function type signatures are not allowed in %s",
                      where);
        message = buffer;
    } else if (skein::typecheck::type_constraint_has_arguments(type)) {
        std::snprintf(buffer, sizeof(buffer),
                      "generic type arguments are not allowed in %s", where);
        message = buffer;
    }
    if (message) {
        diag_report(g_token_line, g_token_column, "type error", message);
        g_statement_invalid = true;
    }
    return type;
}
%}

%define parse.error custom
%define parse.lac full

%union {
    struct ast *a;
    struct symlist *sl;
    struct type_constraint *type;
    struct parsed_type_list *tl;
    struct pattern_field *pf;
    double d;
    long long i;
    int fn;
    char *str;
}

%define api.token.prefix {SKEIN_TOK_}

%token <d> FLOAT "float"
%token <i> INT "integer"
%token <str> NAME "identifier"
%token <fn> BUILTIN "built-in function"
%token <str> XSTRING "string"
%token <i> CHAR "character"
%token EOL "end of line"
%token IF "if" ELSE "else" WHILE "while" DO "do" FOR "for" FUNC "func" LET "let" CONST "const"
%token YIELD "yield" DEFER "defer"
%token LAUNCH "launch"
%token STRUCT "struct" IMPL "impl" TRAIT "trait" ENUM "enum"
%token EXTERN "extern"
%token SWITCH "switch" CASE "case" DEFAULT "default" ARROW "=>"
%token MATCH "match"
%token RETURN "return" BREAK "break" CONTINUE "continue"
%token FAIL "fail" ON "on" ERROR "error" RECOVER "recover" PASS "pass"
%token IS "is" LABEL "label"
%token COLONCOLON "::"
%token PLUS_ASSIGN "+=" MINUS_ASSIGN "-=" MUL_ASSIGN "*=" DIV_ASSIGN "/="
%token MOD_ASSIGN "%=" AND_ASSIGN "&=" OR_ASSIGN "|=" XOR_ASSIGN "^="
%token SHL_ASSIGN "<<=" SHR_ASSIGN ">>="
%token NULLVAL "null" TRUE "true" FALSE "false"
%token IMPORT "import" PACKAGE "package" EXPORT "export" IN "in" AS "as"
%token LOGAND "&&" LOGOR "||" SHL "<<" SHR ">>"
%token INC "++" DEC "--"
%token ELLIPSIS "..."
%token DOTDOT ".."
%token PIPE "|>"
%token PIPE_APPLY "|>>"
%token PLACEHOLDER "$"
%token RARROW "->"
%token EMPTY_DICT_CLOSE "empty dictionary"
%token EMPTY_BLOCK_CLOSE "empty block"
%token BODY_LBRACE "body"
%token HEADER_LPAREN "header ("
%token DO_WHILE_LPAREN "do while ("

%right ARROW RARROW
%right '=' PLUS_ASSIGN MINUS_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%right AND_ASSIGN OR_ASSIGN XOR_ASSIGN SHL_ASSIGN SHR_ASSIGN
%right '?'
%left LOGOR
%left LOGAND
%left '|'
%precedence AS
%left '^'
%left '&'
%nonassoc <fn> CMP
%precedence IS
%left SHL SHR
%left '+' '-'
%left '*' '/' '%'
%left PIPE PIPE_APPLY
%precedence UMINUS NOT BITNOT
%right INC DEC
%precedence '['
%precedence '.'
%precedence '('
%precedence ON
%precedence NOELSE
%precedence ELSE

%type <a> exp value stmt list scope explist arg top_expression top_statement line valuelist pairlist
%type <a> slice_bound
%type <a> forinit forcond forend
%type <a> switch_body switch_cases switch_case case_values
%type <a> match_arms match_arm pattern pattern_list pattern_items value_pattern match_values
%type <a> tuple_body
%type <pf> pattern_fields pattern_field_items pattern_field
%type <a> let_pattern
%type <a> struct_decl impl_decl trait_decl struct_members struct_member
%type <a> enum_decl enum_variants enum_variant variant_fields variant_field
%type <a> func_decl
%type <a> impl_methods trait_methods trait_method method_decl method_member
%type <a> decorators decorator decorator_expr
%type <a> struct_fields struct_field
%type <sl> param symlist
%type <type> type type_name func_ret_opt
%type <tl> type_list
%type <str> importref importpath fieldname impl_trait_name with_trait with_traits
%type <a> ffi_items ffi_item ffi_fields ffi_params ffi_param_list ffi_param ffi_type ffi_const_value ffi_annotations ffi_annotation_list ffi_annotation

%destructor { treefree($$); } <a>
%destructor { free($$); } <str>
%destructor { symlistfree_full($$); } <sl>
%destructor { free_type_constraint($$); } <type>
%destructor { free_type_list($$); } <tl>

/* `NAME '.' NAME` is deliberately shifted for field/enum dot access instead
 * of reducing `NAME` to `exp` first.  The conflicts are resolved as shifts;
 * the no-parenthesis `for` header adds one more instance of the same
 * ambiguity, and this count keeps future grammar changes visible. */
%expect 3

%start program
%%

func_decl: funcstart NAME '(' symlist ')' func_ret_opt '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newfunc($2, $4, NULL, false, finish_function(), $6); }
    | funcstart NAME '(' symlist ')' func_ret_opt '{' list '}'
                                           { $$ = newfunc($2, $4, $8, false, finish_function(), $6); }
    | funcstart NAME '(' symlist ')' func_ret_opt RARROW exp ';'
                                           { $$ = newfunc($2, $4, newret($8), false, finish_function(), $6); }
    | EXPORT funcstart NAME '(' symlist ')' func_ret_opt '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newfunc($3, $5, NULL, true, finish_function(), $7); }
    | EXPORT funcstart NAME '(' symlist ')' func_ret_opt '{' list '}'
                                           { $$ = newfunc($3, $5, $9, true, finish_function(), $7); }
    | EXPORT funcstart NAME '(' symlist ')' func_ret_opt RARROW exp ';'
                                           { $$ = newfunc($3, $5, newret($9), true, finish_function(), $7); }
    ;

stmt: IF HEADER_LPAREN exp ')' scope %prec NOELSE   { $$ = newflow('I', $3, $5, NULL); }
    | IF HEADER_LPAREN exp ')' scope ELSE scope     { $$ = newflow('I', $3, $5, $7); }
    | IF exp BODY_LBRACE EMPTY_BLOCK_CLOSE %prec NOELSE
                                           { $$ = newflow('I', $2, newscope(NULL), NULL); }
    | IF exp BODY_LBRACE list '}' %prec NOELSE
                                           { $$ = newflow('I', $2, newscope($4), NULL); }
    | IF exp BODY_LBRACE EMPTY_BLOCK_CLOSE ELSE scope
                                           { $$ = newflow('I', $2, newscope(NULL), $6); }
    | IF exp BODY_LBRACE list '}' ELSE scope
                                           { $$ = newflow('I', $2, newscope($4), $7); }
    | WHILE HEADER_LPAREN exp ')' scope             { $$ = newflow('W', $3, $5, NULL); }
    | WHILE exp BODY_LBRACE EMPTY_BLOCK_CLOSE
                                           { $$ = newflow('W', $2, newscope(NULL), NULL); }
    | WHILE exp BODY_LBRACE list '}'
                                           { $$ = newflow('W', $2, newscope($4), NULL); }
    | DO scope eols do_while_start WHILE DO_WHILE_LPAREN exp ')'
                                           { $$ = newflow('E', $7, $2, NULL); }
    | DO scope eols do_while_start WHILE exp ';'
                                           { $$ = newflow('E', $6, $2, NULL); }
    | SWITCH HEADER_LPAREN exp ')' '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newswitch($3, NULL); }
    | SWITCH HEADER_LPAREN exp ')' '{' switch_body '}'
                                           { $$ = newswitch($3, $6); }
    | SWITCH exp BODY_LBRACE EMPTY_BLOCK_CLOSE
                                           { $$ = newswitch($2, NULL); }
    | SWITCH exp BODY_LBRACE switch_body '}'
                                           { $$ = newswitch($2, $4); }
    | FOR HEADER_LPAREN forinit ';' forcond ';' forend ')' scope
                                           { $$ = newfor($3, $5, $7, $9); }
    | FOR forinit ';' forcond ';' forend BODY_LBRACE EMPTY_BLOCK_CLOSE
                                           { $$ = newfor($2, $4, $6, newscope(NULL)); }
    | FOR forinit ';' forcond ';' forend BODY_LBRACE list '}'
                                           { $$ = newfor($2, $4, $6, newscope($8)); }
    | FOR HEADER_LPAREN LET NAME IN exp ')' scope   { $$ = newforin($4, NULL, $6, $8); }
    | FOR HEADER_LPAREN LET NAME ',' NAME IN exp ')' scope
                                           { $$ = newforin($4, $6, $8, $10); }
    | FOR NAME IN exp BODY_LBRACE EMPTY_BLOCK_CLOSE
                                           { $$ = newforin($2, NULL, $4, newscope(NULL)); }
    | FOR NAME IN exp BODY_LBRACE list '}'
                                           { $$ = newforin($2, NULL, $4, newscope($6)); }
    | FOR NAME ',' NAME IN exp BODY_LBRACE EMPTY_BLOCK_CLOSE
                                           { $$ = newforin($2, $4, $6, newscope(NULL)); }
    | FOR NAME ',' NAME IN exp BODY_LBRACE list '}'
                                           { $$ = newforin($2, $4, $6, newscope($8)); }
    | DEFER exp ';'                       { $$ = newdefer($2); }
    | func_decl
    | decorators func_decl                { $$ = lower_function_decorators($1, (struct funcdef *)$2); }
    | EXPORT LET NAME '=' exp ';'         { $$ = newvar($3, $5, true); }
    | EXPORT LET NAME ':' type '=' exp ';' {
        $$ = newvar($3, $7, true, false, $5);
      }
    | EXPORT LET NAME ':' type ';'        { $$ = newvar($3, NULL, true, false, $5); }
    | EXPORT CONST NAME '=' exp ';'       { $$ = newvar($3, $5, true, true); }
    | EXPORT CONST NAME ':' type '=' exp ';' {
        $$ = newvar($3, $7, true, true, $5);
      }
    | EXPORT NAME '=' exp ';'             { $$ = newvar($2, $4, true); }
    | EXPORT STRUCT NAME '{' empty_body  {
        $$ = newstructdecl($3,
                           copy_text(diag_current_source_name().c_str()),
                           NULL, true);
      }
    | EXPORT STRUCT NAME '{' struct_members '}' {
        $$ = newstructdecl($3,
                           copy_text(diag_current_source_name().c_str()),
                           (struct struct_member *)$5, true);
      }
    | EXPORT STRUCT NAME NAME with_traits '{' empty_body {
        $$ = lower_type_with($3, $4, $5, NULL, false, true);
      }
    | EXPORT STRUCT NAME NAME with_traits '{' struct_members '}' {
        $$ = lower_type_with($3, $4, $5,
                             (struct struct_member *)$7, false, true);
      }
    | EXPORT TRAIT NAME '{' empty_body   {
        $$ = newtraitdecl($3,
                          copy_text(diag_current_source_name().c_str()),
                          NULL, true);
      }
    | EXPORT TRAIT NAME '{' trait_methods '}' {
        $$ = newtraitdecl($3,
                          copy_text(diag_current_source_name().c_str()),
                          (struct trait_method *)$5, true);
      }
    | EXPORT ENUM NAME '{' empty_body    {
        $$ = newenumdecl($3,
                         copy_text(diag_current_source_name().c_str()),
                         NULL, true);
      }
    | EXPORT ENUM NAME '{' enum_variants '}' {
        $$ = newenumdecl($3,
                         copy_text(diag_current_source_name().c_str()),
                         (struct enum_variant *)$5, true);
      }
    | EXPORT ENUM NAME NAME with_traits '{' empty_body {
        $$ = lower_type_with($3, $4, $5, NULL, true, true);
      }
    | EXPORT ENUM NAME NAME with_traits '{' enum_variants '}' {
        $$ = lower_type_with($3, $4, $5,
                             (struct enum_variant *)$7, true, true);
      }
    | LABEL NAME ':' stmt                { $$ = newlabel($2, $4); }
    | struct_decl
    | impl_decl
    | trait_decl
    | enum_decl
    | PACKAGE NAME '{' EMPTY_BLOCK_CLOSE  { $$ = newpackage($2, NULL); }
    | PACKAGE NAME '{' list '}'           { $$ = newpackage($2, $4); }
    | IMPORT importref ';'                { $$ = newimport($2, NULL); }
    | IMPORT importref AS NAME ';'        { $$ = newimport($2, $4); }
    | LET NAME '=' exp ';'                { $$ = newvar($2, $4); }
    | LET NAME ':' type '=' exp ';'       { $$ = newvar($2, $6, false, false, $4); }
    | LET NAME ':' type ';'               { $$ = newvar($2, NULL, false, false, $4); }
    | LET let_pattern '=' exp ';'          { $$ = newdestructure($2, $4); }
    | CONST NAME '=' exp ';'              { $$ = newvar($2, $4, false, true); }
    | CONST NAME ':' type '=' exp ';'     { $$ = newvar($2, $6, false, true, $4); }
    | CONST let_pattern '=' exp ';'        { $$ = newdestructure($2, $4, true); }
    | EXTERN XSTRING NAME NAME NAME XSTRING '{' ffi_items '}' {
        if (std::strcmp($2, "c") != 0 || std::strcmp($3, "lib") != 0 ||
            std::strcmp($5, "from") != 0) {
            yyerror("expected: extern \"c\" lib NAME from \"path\" { ... }");
            free($2);
            free($3);
            free($5);
            free($6);
            free($4);
            treefree($8);
            $$ = NULL;
        } else {
            struct value_t *manifest = ffi_build_manifest($4, $8);
            treefree($8);
            struct ast *args = newast(
                'L', ffi_value_node(newstrval($6)),
                newast('L', ffi_value_node(manifest), NULL));
            $$ = newvar($4, newcall(newbuiltin(B_ffi_bind), args));
            free($2);
            free($3);
            free($5);
            free($6);
        }
    }
    | EXTERN XSTRING NAME NAME NAME XSTRING '{' EMPTY_DICT_CLOSE {
        if (std::strcmp($2, "c") != 0 || std::strcmp($3, "lib") != 0 ||
            std::strcmp($5, "from") != 0) {
            yyerror("expected: extern \"c\" lib NAME from \"path\" { ... }");
            free($2);
            free($3);
            free($5);
            free($6);
            free($4);
            $$ = NULL;
        } else {
            $$ = ffi_binding_node($4, $6, ffi_empty_manifest($4));
            free($2);
            free($3);
            free($5);
            free($6);
        }
    }
    | EXTERN XSTRING NAME NAME NAME XSTRING '{' EMPTY_BLOCK_CLOSE {
        if (std::strcmp($2, "c") != 0 || std::strcmp($3, "lib") != 0 ||
            std::strcmp($5, "from") != 0) {
            yyerror("expected: extern \"c\" lib NAME from \"path\" { ... }");
            free($2);
            free($3);
            free($5);
            free($6);
            free($4);
            $$ = NULL;
        } else {
            $$ = ffi_binding_node($4, $6, ffi_empty_manifest($4));
            free($2);
            free($3);
            free($5);
            free($6);
        }
    }
    ;

empty_body: EMPTY_DICT_CLOSE
    | EMPTY_BLOCK_CLOSE
    ;

ffi_items: %empty                         { $$ = ffi_items_new(); }
    | ffi_items ffi_item {
        $$ = ffi_list_append($1, $2);
        treefree($2);
      }
    ;

ffi_item: NAME NAME '{' ffi_fields '}' {
        if (std::strcmp($1, "type") != 0) {
            yyerror("expected type NAME { ... }");
            free($1);
            free($2);
            treefree($4);
            $$ = ffi_value_node(newnull());
        } else {
            $$ = ffi_item_node("type", $2, ffi_struct_decl($4));
            free($1);
        }
      }
    | NAME NAME '{' EMPTY_DICT_CLOSE {
        if (std::strcmp($1, "type") != 0) {
            yyerror("expected type NAME { ... }");
            free($1);
            free($2);
            $$ = ffi_value_node(newnull());
        } else {
            $$ = ffi_item_node("type", $2,
                               ffi_struct_decl(ffi_fields_new()));
            free($1);
        }
      }
    | NAME NAME '=' ffi_type ';' {
        if (std::strcmp($1, "type") != 0) {
            yyerror("expected type NAME = TYPE;");
            free($1);
            free($2);
            treefree($4);
            $$ = ffi_value_node(newnull());
        } else {
            $$ = ffi_item_node("type", $2,
                               dupval(((struct val *)$4)->val));
            free($1);
            treefree($4);
        }
      }
    | NAME NAME '(' ffi_params ')' ARROW ffi_type ';' {
        if (std::strcmp($1, "callback") != 0) {
            yyerror("expected callback NAME(...) -> TYPE;");
            free($1);
            free($2);
            treefree($4);
            treefree($7);
            $$ = ffi_value_node(newnull());
        } else {
            $$ = ffi_item_node("type", $2,
                               ffi_callback_decl($4, $7));
            free($1);
        }
      }
    | FUNC NAME '(' ffi_params ')' ARROW ffi_type ffi_annotations ';' {
        $$ = ffi_item_node("function", $2,
                           ffi_function_decl($4, $7, $8));
      }
    | FUNC NAME '(' ffi_params ')' ffi_annotations ';' {
        $$ = ffi_item_node("function", $2,
                           ffi_function_decl($4, newstr("void"), $6));
      }
    | CONST NAME ':' ffi_type '=' ffi_const_value ';' {
        treefree($4);
        $$ = ffi_item_node("constant", $2,
                           dupval(((struct val *)$6)->val));
        treefree($6);
      }
    ;

ffi_fields: %empty                        { $$ = ffi_fields_new(); }
    | ffi_fields NAME ':' ffi_type ';' {
        struct dict_value_t *dict =
            (struct dict_value_t *)((struct val *)$1)->val;
        dict_append_item(dict, newstrval($2),
                         dupval(((struct val *)$4)->val));
        treefree($4);
        free($2);
        $$ = $1;
      }
    ;

ffi_params: %empty                        { $$ = ffi_params_new(); }
    | ffi_param_list                      { $$ = $1; }
    ;

ffi_param_list: ffi_param {
        $$ = ffi_params_new();
        ffi_param_append($$, $1);
        treefree($1);
      }
    | ffi_param_list ',' ffi_param {
        ffi_param_append($1, $3);
        treefree($3);
        $$ = $1;
      }
    ;

ffi_param: ffi_type                       { $$ = ffi_param_one($1, NULL); }
    | NAME ':' ffi_type                   { $$ = ffi_param_one($3, $1); }
    ;

ffi_annotations: %empty                   { $$ = ffi_annotations_new(); }
    | ffi_annotation_list                 { $$ = $1; }
    ;

ffi_annotation_list: ffi_annotation {
        $$ = ffi_annotations_new();
        ffi_annotation_append($$, $1);
        treefree($1);
      }
    | ffi_annotation_list ',' ffi_annotation {
        ffi_annotation_append($1, $3);
        treefree($3);
        $$ = $1;
      }
    ;

ffi_annotation: NAME XSTRING              { $$ = ffi_annotation_node($1, $2, 0, false); }
    | NAME NAME                           { $$ = ffi_annotation_node($1, $2, 0, false); }
    | NAME INT                            { $$ = ffi_annotation_node($1, NULL, $2, true); }
    ;

ffi_type: NAME                            { $$ = ffi_value_node(newstrval($1)); free($1); }
    | '*' ffi_type                        { $$ = ffi_type_pointer($2); }
    | '[' ffi_type ';' INT ']'            { $$ = ffi_type_array($2, $4); }
    ;

ffi_const_value: INT                      { $$ = newint($1); }
    | '-' INT                             { $$ = newint(-$2); }
    | FLOAT                               { $$ = newfloat($1); }
    | XSTRING                             { $$ = newstr($1); free($1); }
    | TRUE                                { $$ = newbool(true); }
    | FALSE                               { $$ = newbool(false); }
    ;


let_pattern: NAME '{' pattern_fields '}'  {
        $$ = newpatternstruct($1,
                              copy_text(diag_current_source_name().c_str()),
                              $3);
      }
    | NAME '{' EMPTY_DICT_CLOSE           {
        $$ = newpatternstruct($1,
                              copy_text(diag_current_source_name().c_str()),
                              NULL);
      }
    | NAME '.' NAME                {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, NULL);
      }
    | NAME '.' NAME '{' pattern_fields '}' {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, $5);
      }
    | NAME '.' NAME '{' EMPTY_DICT_CLOSE {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, NULL);
      }
    | '{' pattern_fields '}'              { $$ = newpatterndict($2); }
    | '{' EMPTY_DICT_CLOSE                { $$ = newpatterndict(NULL); }
    | '[' pattern_list ']'                { $$ = newpatternlist($2); }
    ;

struct_decl: STRUCT NAME '{' empty_body  {
        $$ = newstructdecl($2,
                           copy_text(diag_current_source_name().c_str()),
                           NULL);
      }
    | STRUCT NAME '{' struct_members '}' {
        $$ = newstructdecl($2,
                           copy_text(diag_current_source_name().c_str()),
                           (struct struct_member *)$4);
      }
    | STRUCT NAME NAME with_traits '{' empty_body {
        $$ = lower_type_with($2, $3, $4, NULL, false, false);
      }
    | STRUCT NAME NAME with_traits '{' struct_members '}' {
        $$ = lower_type_with($2, $3, $4,
                             (struct struct_member *)$6, false, false);
      }
    ;

enum_decl: ENUM NAME '{' empty_body      {
        $$ = newenumdecl($2,
                         copy_text(diag_current_source_name().c_str()),
                         NULL);
      }
    | ENUM NAME '{' enum_variants '}'    {
        $$ = newenumdecl($2,
                         copy_text(diag_current_source_name().c_str()),
                         (struct enum_variant *)$4);
      }
    | ENUM NAME NAME with_traits '{' empty_body {
        $$ = lower_type_with($2, $3, $4, NULL, true, false);
      }
    | ENUM NAME NAME with_traits '{' enum_variants '}' {
        $$ = lower_type_with($2, $3, $4,
                             (struct enum_variant *)$6, true, false);
      }
    ;

enum_variants: enum_variant              { $$ = $1; }
    | enum_variants enum_variant         {
        $$ = (struct ast *)append_enumvariant(
            (struct enum_variant *)$1, (struct enum_variant *)$2);
      }
    ;

enum_variant: NAME ';'                   { $$ = newenumvariant($1, NULL, NULL); }
    | NAME '{' variant_fields '}'        {
        $$ = newenumvariant($1, (struct struct_member *)$3, NULL);
      }
    | NAME '{' variant_fields '}' ';'    {
        $$ = newenumvariant($1, (struct struct_member *)$3, NULL);
      }
    | NAME '{' EMPTY_DICT_CLOSE          {
        yyerror("enum variant %s has an empty payload; write %s;",
                $1, $1);
        $$ = newenumvariant($1, NULL, NULL);
      }
    | NAME '{' EMPTY_DICT_CLOSE ';'      {
        yyerror("enum variant %s has an empty payload; write %s;",
                $1, $1);
        $$ = newenumvariant($1, NULL, NULL);
      }
    ;

variant_fields: variant_field            { $$ = $1; }
    | variant_fields variant_field       {
        $$ = append_struct_member($1, $2);
      }
    ;

variant_field: NAME ':' type ';'         { $$ = newstructfield($1, $3); }
    | NAME ':' type '=' exp ';'           {
        $$ = newstructfield($1, $3);
        ((struct struct_member *)$$)->default_value = $5;
      }
    ;

struct_members: struct_member            { $$ = $1; }
    | struct_members struct_member       { $$ = append_struct_member($1, $2); }
    ;

struct_member: NAME ':' type ';'         { $$ = newstructfield($1, $3); }
    | NAME ':' type '=' exp ';'           {
        $$ = newstructfield($1, $3);
        ((struct struct_member *)$$)->default_value = $5;
      }
    | method_member                       { $$ = $1; }
    ;

decorators: decorator                    { $$ = $1; }
    | decorators decorator               { $$ = newast('L', $1, $2); }
    ;

decorator: '@' decorator_expr eols       { $$ = $2; }
    ;

/*
 * Decorator expressions deliberately support the common callable forms
 * (name, member access, call, index) instead of the full expression grammar.
 * This avoids a shift/reduce conflict with prefix operators such as `yield`.
 * Arbitrary expressions can be parenthesized: @(make_decorator(x)).
 */
decorator_expr: NAME                     { $$ = newref($1); }
    | BUILTIN                            { $$ = newbuiltin($1); }
    | decorator_expr '(' explist ')'     { $$ = newcall($1, $3); }
    | decorator_expr '[' exp ']'         { $$ = newast('G', $1, $3); }
    | decorator_expr '.' fieldname       { $$ = newast('j', $1, newstr($3)); free($3); }
    | '(' exp ')'                        { $$ = $2; }
    ;

method_decl: funcstart NAME '(' symlist ')' func_ret_opt '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newstructmethod($2, newfunc(NULL, $4, NULL, false, finish_function(), $6)); }
    | funcstart NAME '(' symlist ')' func_ret_opt '{' list '}'
                                           { $$ = newstructmethod($2, newfunc(NULL, $4, $8, false, finish_function(), $6)); }
    | funcstart NAME '(' symlist ')' func_ret_opt RARROW exp ';'
                                           { $$ = newstructmethod($2, newfunc(NULL, $4, newret($8), false, finish_function(), $6)); }
    ;

method_member: method_decl               { $$ = $1; }
    | decorators method_decl             { $$ = with_decorators($2, $1); }
    ;

impl_decl: IMPL NAME '{' empty_body      {
        $$ = lower_impl_decorators((struct impl_node *)newimpl(
            $2, NULL, copy_text(diag_current_source_name().c_str()), NULL));
      }
    | IMPL NAME '{' impl_methods '}'     {
        $$ = lower_impl_decorators((struct impl_node *)newimpl(
            $2, NULL, copy_text(diag_current_source_name().c_str()),
            (struct struct_member *)$4));
      }
    | IMPL impl_trait_name FOR NAME '{' empty_body  {
        $$ = lower_impl_decorators((struct impl_node *)newimpl(
            $4, $2, copy_text(diag_current_source_name().c_str()), NULL));
      }
    | IMPL impl_trait_name FOR NAME '{' impl_methods '}' {
        $$ = lower_impl_decorators((struct impl_node *)newimpl(
            $4, $2, copy_text(diag_current_source_name().c_str()),
            (struct struct_member *)$6));
      }
    | IMPL impl_trait_name FOR NAME ';'  {
        $$ = lower_impl_decorators((struct impl_node *)newimpl(
            $4, $2, copy_text(diag_current_source_name().c_str()), NULL));
      }
    ;

impl_trait_name: NAME                  { $$ = $1; }
    | NAME '.' NAME            {
        std::string qualified = std::string($1) + "." + $3;
        $$ = copy_text(qualified.c_str());
        free($1);
        free($3);
      }
    ;

impl_methods: method_member              { $$ = $1; }
    | impl_methods method_member         { $$ = append_struct_member($1, $2); }
    ;

trait_decl: TRAIT NAME '{' empty_body    {
        $$ = newtraitdecl($2,
                          copy_text(diag_current_source_name().c_str()),
                          NULL);
      }
    | TRAIT NAME '{' trait_methods '}'   {
        $$ = newtraitdecl($2,
                          copy_text(diag_current_source_name().c_str()),
                          (struct trait_method *)$4);
      }
    ;

trait_methods: trait_method              { $$ = $1; }
    | trait_methods trait_method         { $$ = append_trait_method($1, $2); }
    ;

/*
 * Trait methods may be signatures (`;`) or provide a default body.  The
 * default body is kept as a function AST and compiled with the trait; an
 * impl that omits the method falls back to this definition.
 */
trait_method: funcstart NAME '(' symlist ')' ';' {
        $$ = newtraitmethod($2, $4, NULL);
        (void)finish_function();
      }
    | funcstart NAME '(' symlist ')' func_ret_opt '{' EMPTY_BLOCK_CLOSE {
        $$ = newtraitmethod(
            $2, NULL,
            newfunc(NULL, $4, NULL, false, finish_function(), $6));
      }
    | funcstart NAME '(' symlist ')' func_ret_opt '{' list '}' {
        $$ = newtraitmethod(
            $2, NULL,
            newfunc(NULL, $4, $8, false, finish_function(), $6));
      }
    | funcstart NAME '(' symlist ')' func_ret_opt RARROW exp ';' {
        $$ = newtraitmethod(
            $2, NULL,
            newfunc(NULL, $4, newret($8), false, finish_function(),
                    $6));
      }
    ;

with_trait: NAME                         { $$ = $1; }
    | NAME '.' NAME {
        std::string qualified = std::string($1) + "." + $3;
        free($1);
        free($3);
        $$ = copy_text(qualified.c_str());
      }
    ;

with_traits: with_trait                  { $$ = $1; }
    | with_traits ',' with_trait         { $$ = concat_with_trait($1, $3); }
    ;

struct_fields: struct_field              { $$ = $1; }
    | struct_field ',' struct_fields     { $$ = append_struct_init($1, $3); }
    ;

struct_field: NAME ':' exp               { $$ = newstructinit($1, $3); }
    | ELLIPSIS exp                       { $$ = newstructinit(NULL, $2); }
    ;

importref: importpath                     { $$ = $1; }
    | '.' importpath                      { $$ = join_import_prefix(".", $2); }
    | '.' '.' importpath                  { $$ = join_import_prefix("..", $3); }
    | DOTDOT importpath                    { $$ = join_import_prefix("..", $2); }
    | '@' importpath                      { $$ = join_import_prefix("@", $2); }
    ;

importpath: NAME                          { $$ = $1; }
    | importpath '.' NAME                 {
        $$ = join_import_path($1, $3);
        free($1);
        free($3);
      }
    | importpath '.' BUILTIN               {
        const char *name = builtin_field_name($3);
        if (!name) {
            yyerror("invalid import path");
            $$ = copy_text("");
            free($1);
        } else {
            $$ = join_import_path($1, name);
            free($1);
        }
      }
    ;

fieldname: NAME                           { $$ = $1; }
    | YIELD                                { $$ = copy_text("yield"); }
    | MATCH                                { $$ = copy_text("match"); }
    | ENUM                                 { $$ = copy_text("enum"); }
    | STRUCT                               { $$ = copy_text("struct"); }
    | ERROR                                { $$ = copy_text("error"); }
    | BUILTIN {
        const char *name = builtin_field_name($1);
        if (name) {
            $$ = copy_text(name);
        } else {
            yyerror("invalid field name");
            $$ = copy_text("");
        }
      }
    ;

line: RETURN exp                          { $$ = newret($2); }
    | RETURN                              { $$ = newret(NULL); }
    | breakstart                          {
        $$ = newbrk(NULL, flow_keyword_line, flow_keyword_column);
      }
    | breakstart NAME                     {
        $$ = newbrk($2, flow_keyword_line, flow_keyword_column);
      }
    | continuestart                       {
        $$ = newctn(NULL, flow_keyword_line, flow_keyword_column);
      }
    | continuestart NAME                  {
        $$ = newctn($2, flow_keyword_line, flow_keyword_column);
      }
    | FAIL exp                            { $$ = newfail($2); }
    | FAIL                                { $$ = newfail(NULL); }
    | RECOVER exp                         { $$ = newrecover($2); }
    | RECOVER                             { $$ = newrecover(NULL); }
    | PASS exp                            { $$ = newpass($2); }
    | exp                                 { $$ = $1; }
    ;

breakstart: BREAK {
        flow_keyword_line = g_token_line;
        flow_keyword_column = g_token_column;
      }
    ;

continuestart: CONTINUE {
        flow_keyword_line = g_token_line;
        flow_keyword_column = g_token_column;
      }
    ;

scope: stmt                               { $$ = $1; }
     | line ';'                           { $$ = $1; }
     | ';'                                { $$ = newscope(NULL); }
     | '{' EMPTY_BLOCK_CLOSE              { $$ = newscope(NULL); }
     | '{' list '}'                       { $$ = newscope($2); }
     ;

do_while_start: %empty { check_do_while_line(); };

list: %empty                              { $$ = NULL; }
    | scope list {
        if ($2 == NULL)
            $$ = $1;
        else
            $$ = newast('L', $1, $2);
      }
    ;

exp: exp '+' exp                          { $$ = newast('+', $1, $3); }
    | exp '-' exp                         { $$ = newast('-', $1, $3); }
    | exp '*' exp                         { $$ = newast('*', $1, $3); }
    | exp '/' exp                         { $$ = newast('/', $1, $3); }
    | exp '%' exp                         { $$ = newast('%', $1, $3); }
    | exp LOGAND exp                      { $$ = newast('A', $1, $3); }
    | exp LOGOR exp                       { $$ = newast('Q', $1, $3); }
    | exp '&' exp                         { $$ = newast('&', $1, $3); }
    | exp '|' exp                         { $$ = newast('|', $1, $3); }
    | exp '^' exp                         { $$ = newast('^', $1, $3); }
    | exp SHL exp                         { $$ = newast('s', $1, $3); }
    | exp SHR exp                         { $$ = newast('r', $1, $3); }
    | exp PIPE exp                        {
        if ($3->nodetype == 'C') {
            struct ufncall *call = (struct ufncall *)$3;
            call->args = newast('L', $1, call->args);
            $$ = $3;
        } else {
            $$ = newcall($3, newast('L', $1, NULL));
        }
      }
    | exp PIPE_APPLY exp                  {
        $$ = newcall($3, newast('L', $1, NULL));
      }
    | NAME RARROW lambda_start exp         {
        $$ = newfunc(NULL, newsymlist(newsym($1), NULL, NULL),
                     newret($4), false, finish_function());
        free($1);
      }
    | '(' ')' RARROW lambda_start exp      {
        $$ = newfunc(NULL, NULL, newret($5), false,
                     finish_function());
      }
    | '(' tuple_body ')'                 { $$ = $2; }
    | '(' tuple_body ')' RARROW lambda_start exp {
        $$ = make_tuple_lambda($2, $6);
      }
    | '(' exp ')'                         { $$ = $2; }
    | MATCH HEADER_LPAREN exp ')' BODY_LBRACE EMPTY_BLOCK_CLOSE
                                           {
        yyerror("match requires at least one arm");
        $$ = newmatch($3, NULL);
      }
    | MATCH HEADER_LPAREN exp ')' BODY_LBRACE match_arms '}'
                                           { $$ = newmatch($3, $6); }
    | MATCH HEADER_LPAREN exp ',' match_values ')' BODY_LBRACE EMPTY_BLOCK_CLOSE
                                           {
        yyerror("match requires at least one arm");
        $$ = newmatch(newtuple($3, $5), NULL);
      }
    | MATCH HEADER_LPAREN exp ',' match_values ')' BODY_LBRACE match_arms '}'
                                           { $$ = newmatch(newtuple($3, $5), $8); }
    | MATCH exp BODY_LBRACE EMPTY_BLOCK_CLOSE
                                           {
        yyerror("match requires at least one arm");
        $$ = newmatch($2, NULL);
      }
    | MATCH exp BODY_LBRACE match_arms '}'
                                           { $$ = newmatch($2, $4); }
    | LAUNCH exp %prec UMINUS {
        struct ast *target = launch_target_call($2);
        if (!target || target->nodetype != 'C') {
            yyerror("launch expects a call expression");
            treefree($2);
            $$ = NULL;
        } else {
            ((struct ufncall *)target)->launch = 1;
            $$ = $2;
        }
      }
    | YIELD exp %prec '?'                  { note_yield(); $$ = newyield($2); }
    | YIELD %prec '?'                      { note_yield(); $$ = newyield(newnullast()); }
    | '-' exp %prec UMINUS                { $$ = newast('M', $2, NULL); }
    | '!' exp %prec NOT                   { $$ = newast('!', $2, NULL); }
    | '~' exp %prec BITNOT                { $$ = newast('~', $2, NULL); }
    | INC exp                             { $$ = newincdec($2, 1, true); }
    | DEC exp                             { $$ = newincdec($2, -1, true); }
    | exp INC                             { $$ = newincdec($1, 1, false); }
    | exp DEC                             { $$ = newincdec($1, -1, false); }
    | exp CMP exp                         { $$ = newcmp($2, $1, $3); }
    | exp IS type %prec IS                {
        $$ = newis($1, runtime_type_or_error($3, "'is'"));
      }
    | exp AS type %prec AS                { $$ = newcast($1, $3); }
    | exp '?' exp ':' exp %prec '?'       { $$ = newflow('T', $1, $3, $5); }
    | exp ON ERROR '(' NAME ')' '{' empty_body %prec ON {
        $$ = newonerror($1, $5, NULL);
      }
    | exp ON ERROR '(' NAME ')' '{' list '}' %prec ON {
        $$ = newonerror($1, $5, $8);
      }
    | NAME                                { $$ = newref($1); }
    | exp '=' exp                         { $$ = newasgn($1, $3); }
    | exp PLUS_ASSIGN exp                 { $$ = newcompound($1, '+', $3); }
    | exp MINUS_ASSIGN exp                { $$ = newcompound($1, '-', $3); }
    | exp MUL_ASSIGN exp                  { $$ = newcompound($1, '*', $3); }
    | exp DIV_ASSIGN exp                  { $$ = newcompound($1, '/', $3); }
    | exp MOD_ASSIGN exp                  { $$ = newcompound($1, '%', $3); }
    | exp AND_ASSIGN exp                  { $$ = newcompound($1, '&', $3); }
    | exp OR_ASSIGN exp                   { $$ = newcompound($1, '|', $3); }
    | exp XOR_ASSIGN exp                  { $$ = newcompound($1, '^', $3); }
    | exp SHL_ASSIGN exp                  { $$ = newcompound($1, 's', $3); }
    | exp SHR_ASSIGN exp                  { $$ = newcompound($1, 'r', $3); }
    | exp '(' explist ')'      { $$ = new_call_with_placeholders($1, $3); }
    | exp '[' exp ']'                     { $$ = newast('G', $1, $3); }
    | exp '[' slice_bound ':' slice_bound ']' {
        $$ = newslice($1, $3, $5, NULL);
      }
    | exp '[' slice_bound ':' slice_bound ':' slice_bound ']' {
        $$ = newslice($1, $3, $5, $7);
      }
    | exp '[' exp COLONCOLON exp ']' {
        $$ = newslice($1, $3, NULL, $5);
      }
    | exp '[' exp COLONCOLON ']' {
        $$ = newslice($1, $3, NULL, NULL);
      }
    | exp '[' COLONCOLON slice_bound ']' {
        $$ = newslice($1, NULL, NULL, $4);
      }
    | exp '.' fieldname                   { $$ = newast('j', $1, newstr($3)); free($3); }
    | '[' valuelist ']'                   { $$ = $2; }
    | '{' EMPTY_DICT_CLOSE                { $$ = newdict(NULL, NULL, NULL); }
    | '{' pairlist '}'                    { $$ = $2; }
    | NAME '{' empty_body                 {
        $$ = newstructlit($1,
                          copy_text(diag_current_source_name().c_str()),
                          NULL);
      }
    | NAME '{' struct_fields '}'          {
        $$ = newstructlit($1,
                          copy_text(diag_current_source_name().c_str()),
                          (struct struct_init *)$3);
      }
    | NAME '.' fieldname {
        $$ = newast('j', newref($1), newstr($3));
        free($3);
      }
    | NAME '.' NAME '{' struct_fields '}' {
        $$ = newenumlit($1,
                        copy_text(diag_current_source_name().c_str()),
                        $3, (struct struct_init *)$5);
      }
    | NAME '.' NAME '{' EMPTY_DICT_CLOSE {
        $$ = newenumlit($1,
                        copy_text(diag_current_source_name().c_str()),
                        $3, NULL);
      }
    | funcstart '(' symlist ')' func_ret_opt '{' EMPTY_BLOCK_CLOSE
                                           { $$ = newfunc(NULL, $3, NULL, false, finish_function(), $5); }
    | funcstart '(' symlist ')' func_ret_opt '{' list '}'
                                           { $$ = newfunc(NULL, $3, $7, false, finish_function(), $5); }
    | funcstart '(' symlist ')' func_ret_opt RARROW exp
                                           { $$ = newfunc(NULL, $3, newret($7), false, finish_function(), $5); }
    | value
    ;

slice_bound: %empty                       { $$ = NULL; }
    | exp                                 { $$ = $1; }
    ;

funcstart: FUNC {
        push_func_location(g_token_line, g_token_column);
        start_function();
    }
    ;

func_ret_opt: %empty                          { $$ = NULL; }
    | ':' type                                { $$ = $2; }
    ;

lambda_start: %empty {
        push_func_location(g_token_line, g_token_column);
        start_function();
    }
    ;


value: INT                                { $$ = newint($1); }
    | FLOAT                               { $$ = newfloat($1); }
    | TRUE                                { $$ = newbool(true); }
    | FALSE                               { $$ = newbool(false); }
    | XSTRING                             { $$ = newstr($1); free($1); }
    | CHAR                                { $$ = newchar($1); }
    | BUILTIN                             { $$ = newbuiltin($1); }
    | NULLVAL                             { $$ = newnullast(); }
    ;

valuelist: %empty                         { $$ = newlist(NULL, NULL); }
    | exp                                 { $$ = newlist($1, NULL); }
    | exp ',' valuelist                   { $$ = newlist($1, $3); }
    ;

pairlist: exp ':' exp                     { $$ = newdict($1, $3, NULL); }
    | exp ':' exp ',' pairlist            { $$ = newdict($1, $3, $5); }
    ;

forinit: %empty                           { $$ = NULL; }
    | exp                                 { $$ = $1; }
    | LET NAME '=' exp                    { $$ = newvar($2, $4); }
    | LET NAME ':' type '=' exp           { $$ = newvar($2, $6, false, false, $4); }
    ;

forcond: %empty                           { $$ = NULL; }
    | exp                                 { $$ = $1; }
    ;

forend: %empty                            { $$ = NULL; }
    | exp                                 { $$ = $1; }
    ;

eols: %empty
    | EOL eols
    ;

switch_body: switch_cases                 { $$ = $1; }
    | DEFAULT ARROW scope                 { $$ = newcase(NULL, $3, NULL); }
    | switch_cases DEFAULT ARROW scope    {
        $$ = append_case($1, newcase(NULL, $4, NULL));
      }
    ;

switch_cases: switch_case                 { $$ = $1; }
    | switch_cases switch_case            { $$ = append_case($1, $2); }
    ;

switch_case: CASE case_values ARROW scope { $$ = newcase($2, $4, NULL); }
    ;

case_values: exp                          { $$ = newlist($1, NULL); }
    | exp ',' case_values                 { $$ = newlist($1, $3); }
    ;

match_arms: match_arm                     { $$ = $1; }
    | match_arms match_arm                { $$ = append_matcharm($1, $2); }
    ;

match_arm: CASE pattern ARROW scope       { $$ = newmatcharm($2, NULL, $4, NULL); }
    | CASE pattern IF exp ARROW scope     { $$ = newmatcharm($2, $4, $6, NULL); }
    | DEFAULT ARROW scope                 { $$ = newmatcharm(NULL, NULL, $3, NULL); }
    ;

pattern: INT                              { $$ = newpatternlit(newint($1)); }
    | XSTRING                             { $$ = newpatternlit(newstr($1)); free($1); }
    | CHAR                                { $$ = newpatternlit(newchar($1)); }
    | TRUE                                { $$ = newpatternlit(newbool(true)); }
    | FALSE                               { $$ = newpatternlit(newbool(false)); }
    | NULLVAL                             { $$ = newpatternlit(newnullast()); }
    | NAME                                { $$ = newpatternname($1); }
    | IS type                             {
        $$ = newpatterntype(runtime_type_or_error($2, "patterns"));
      }
    | NAME '{' pattern_fields '}'         {
        $$ = newpatternstruct($1,
                              copy_text(diag_current_source_name().c_str()),
                              $3);
      }
    | NAME '{' EMPTY_DICT_CLOSE           {
        $$ = newpatternstruct($1,
                              copy_text(diag_current_source_name().c_str()),
                              NULL);
      }
    | NAME '.' NAME                {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, NULL);
      }
    | NAME '.' NAME '{' pattern_fields '}' {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, $5);
      }
    | NAME '.' NAME '{' EMPTY_DICT_CLOSE {
        $$ = newpatternvariant($1,
                               copy_text(diag_current_source_name().c_str()),
                               $3, NULL);
      }
    | '{' pattern_fields '}'              { $$ = newpatterndict($2); }
    | '{' EMPTY_DICT_CLOSE                { $$ = newpatterndict(NULL); }
    | '[' pattern_list ']'                { $$ = newpatternlist($2); }
    | pattern '|' pattern                  { $$ = newpatternor($1, $3); }
    | INT DOTDOT INT                       { $$ = newpatternrange($1, $3); }
    | '-' INT DOTDOT INT                   { $$ = newpatternrange(-$2, $4); }
    | INT DOTDOT '-' INT                   { $$ = newpatternrange($1, -$4); }
    | '-' INT DOTDOT '-' INT               { $$ = newpatternrange(-$2, -$5); }
    | pattern AS NAME                     { $$ = newpatternas($1, $3); }
    | CMP value_pattern                   {
        if ($1 != 4) {
            yyerror("only == is allowed in a value pattern");
            treefree($2);
            $$ = newpatternnever();
        } else {
            $$ = newpatternlit($2);
        }
      }
    ;

value_pattern: value                      { $$ = $1; }
    | NAME                                { $$ = newref($1); }
    | NAME '.' NAME                       {
        $$ = newenumlit($1,
                        copy_text(diag_current_source_name().c_str()),
                        $3, NULL);
      }
    ;

match_values: exp                         { $$ = newlist($1, NULL); }
    | exp ',' match_values                { $$ = newlist($1, $3); }
    ;

pattern_fields: %empty                    { $$ = NULL; }
    | pattern_field_items
    | pattern_field_items ',' ELLIPSIS    {
        $$ = append_patternfield($1, newpatternrestfield(NULL, NULL));
      }
    | pattern_field_items ',' ELLIPSIS NAME {
        $$ = append_patternfield($1, newpatternrestfield($4, NULL));
      }
    | ELLIPSIS                            { $$ = newpatternrestfield(NULL, NULL); }
    | ELLIPSIS NAME                       { $$ = newpatternrestfield($2, NULL); }
    ;

pattern_field_items: pattern_field        { $$ = $1; }
    | pattern_field_items ',' pattern_field {
        $$ = append_patternfield($1, $3);
      }
    ;

pattern_field: NAME                       {
        $$ = newpatternfield($1, newpatternname(copy_text($1)), NULL);
      }
    | NAME ':' pattern                    { $$ = newpatternfield($1, $3, NULL); }
    | XSTRING ':' pattern                 { $$ = newpatternfield($1, $3, NULL); }
    ;

pattern_list: %empty                      { $$ = NULL; }
    | pattern_items
    | pattern_items ',' ELLIPSIS          {
        $$ = append_patternitem($1, newpatternrest(NULL));
      }
    | pattern_items ',' ELLIPSIS NAME     {
        $$ = append_patternitem($1, newpatternrest($4));
      }
    | ELLIPSIS                            { $$ = newpatternrest(NULL); }
    | ELLIPSIS NAME                       { $$ = newpatternrest($2); }
    ;

pattern_items: pattern                    { $$ = $1; }
    | pattern_items ',' pattern           { $$ = append_patternitem($1, $3); }
    ;

tuple_body: exp ',' exp                   {
        $$ = newtuple($1, newlist($3, NULL));
      }
    | exp ',' exp ',' explist              {
        struct ast *tail = append_tuple_explist(
            newlist($3, NULL), $5);
        $$ = newtuple($1, tail);
      }
    ;

explist: %empty                            { $$ = NULL; }
    | arg
    | arg ',' explist                     { $$ = newast('L', $1, $3); }
    ;

arg: exp                                   { $$ = $1; }
    | ELLIPSIS exp                         { $$ = newspread($2); }
    | PLACEHOLDER                          { $$ = newref(copy_text("$")); }
    ;

param: NAME {
        struct symbol *s = newsym($1);
        free($1);
        $$ = newsymlist(s, NULL, NULL);
      }
    | NAME ':' type {
        struct symbol *s = newsym($1);
        free($1);
        $$ = newsymlist(s, $3, NULL);
      }
    | NAME '=' exp {
        struct symbol *s = newsym($1);
        free($1);
        $$ = newsymlist(s, NULL, NULL);
        set_symlist_default_ast($$, $3);
      }
    | NAME ':' type '=' exp {
        struct symbol *s = newsym($1);
        free($1);
        $$ = newsymlist(s, $3, NULL);
        set_symlist_default_ast($$, $5);
      }
    | ELLIPSIS NAME {
        struct symbol *s = newsym($2);
        free($2);
        $$ = newsymlist(s, NULL, NULL, true);
      }
    | ELLIPSIS NAME ':' type {
        struct symbol *s = newsym($2);
        free($2);
        $$ = newsymlist(s, $4, NULL, true);
      }
    | '[' pattern_list ']' {
        $$ = new_destructure_param(newpatternlist($2));
      }
    | '{' pattern_fields '}' {
        $$ = new_destructure_param(newpatterndict($2));
      }
    | NAME '{' pattern_fields '}' {
        $$ = new_destructure_param(newpatternstruct(
            $1, copy_text(diag_current_source_name().c_str()), $3));
      }
    | NAME '{' EMPTY_DICT_CLOSE {
        $$ = new_destructure_param(newpatternstruct(
            $1, copy_text(diag_current_source_name().c_str()), NULL));
      }
    | NAME '.' NAME '{' pattern_fields '}' {
        $$ = new_destructure_param(newpatternvariant(
            $1, copy_text(diag_current_source_name().c_str()), $3, $5));
      }
    | NAME '.' NAME '{' EMPTY_DICT_CLOSE {
        $$ = new_destructure_param(newpatternvariant(
            $1, copy_text(diag_current_source_name().c_str()), $3, NULL));
      }
    ;

symlist: %empty                           { $$ = NULL; }
    | param                               { $$ = $1; }
    | param ',' symlist                   {
        $1->next = $3;
        $$ = $1;
      }
    ;

/*
 * The `%prec AS` on the bare type_name rule makes a following '[' prefer the
 * generic-argument path.  Without it, `x as Foo[0]` is ambiguous between a
 * generic argument list and indexing the cast result.  The chosen convention
 * is `(x as Foo)[0]` for postfix access on a cast, so generic arguments win.
 */
type: type_name %prec AS                  { $$ = $1; }
    | type '?'                            { $$ = make_nullable_type($1); }
    | type_name '[' type ']'              {
        $$ = make_generic_type($1, $3, NULL);
      }
    | type_name '[' type ',' type ']'     {
        $$ = make_generic_type($1, $3, $5);
      }
    | type_name '[' type ',' type ',' type_list ']' {
        $$ = make_generic_type_list(
            $1, new_type_list($3, new_type_list($5, $7)));
      }
    | '(' type_list ')' RARROW type       {
        $$ = make_function_type($2, $5);
      }
    ;

type_list: %empty                         { $$ = NULL; }
    | type                                { $$ = new_type_list($1, NULL); }
    | type ',' type_list                  { $$ = new_type_list($1, $3); }
    ;

type_name: NAME                           {
        $$ = type_constraint_from_name($1);
        free($1);
      }
    | NAME '.' NAME                {
        std::string qualified = std::string($1) + "." + $3;
        $$ = new_named_type_constraint(
            qualified.c_str(), diag_current_source_name().c_str());
        free($1);
        free($3);
      }
    | BUILTIN                             { $$ = type_constraint_from_builtin($1); }
    | FUNC                                { $$ = new_type_constraint(TYPE_FUNCTION); }
    | ERROR                               { $$ = new_type_constraint(TYPE_ERROR); }
    | NULLVAL                             { $$ = new_type_constraint(TYPE_NULL); }
    ;

top_expression: exp { $$ = $1; }
    | list { $$ = $1; }
    ;

top_statement: top_expression EOL {
        func_yield_counts.clear();
        active_eval_context().runtime_error_count = 0;
        diag_set_location(0, 0);

        if (!g_statement_invalid && $1 &&
            !skein::typecheck::check_statement($1))
            g_statement_invalid = true;

        bool invalid = g_statement_invalid;
        g_statement_invalid = false;
        bool skip_eval = invalid || sys_exit_requested() ||
                         (!g_interactive && g_script_stop);
        struct value_t *rslt = NULL;

        if (!skip_eval) {
            if (!g_package_loading)
                maybe_collect_cycles();

            rslt = skein_vm_eval_statement($1);
            if (eval_control_pending())
                eval_finish_statement_control();
            if (g_interactive && rslt && rslt->type != T_null &&
                rslt->type != T_return && rslt->type != T_break &&
                rslt->type != T_continue) {
                repl_set_last_result(rslt);
            }
            if (rslt && !g_interrupted && rslt->type != T_null) {
                switch (rslt->type) {
                    case T_return:
                        yyerror("return outside function");
                        break;
                    case T_break:
                        diag_set_location(g_flow_line, g_flow_column);
                        yyerror("break outside loop");
                        break;
                    case T_continue:
                        diag_set_location(g_flow_line, g_flow_column);
                        yyerror("continue outside loop");
                        break;
                    default:
                        if (g_interactive) {
                            printf("= ");
                            printvalue(rslt);
                            printf("\n");
                            fflush(stdout);
                        }
                        break;
                }
            }
            freeval(rslt);
        }

        if ($1) treefree($1);
        $1 = NULL;
        $$ = NULL;
        if (g_interrupted || sys_exit_requested() ||
            (!g_interactive && g_script_stop))
            YYABORT;
    }
    | error EOL {
        yyerrok;
        g_statement_invalid = false;
        if (!g_interactive && g_script_stop)
            YYABORT;
        $$ = NULL;
    }
    ;

program: %empty
    | program top_statement { (void)$2; }
    ;
%%

void parser_begin_nested_parse() {
    yychar = SKEIN_TOK_YYEMPTY;
    yynerrs = 0;
}

int yyreport_syntax_error(const yypcontext_t *ctx) {
    if (diag_lexical_error_on_line(g_token_line)) {
        clear_func_locations();
        return 0;
    }

    enum { MAX_EXPECTED = 64, MAX_SHOWN = 6 };
    yysymbol_kind_t expected[MAX_EXPECTED];

    yysymbol_kind_t unexpected = yypcontext_token(ctx);
    std::string message;
    if (unexpected == YYSYMBOL_YYEMPTY) {
        message = "invalid syntax";
    } else {
        message = "unexpected ";
        message += yysymbol_name(unexpected);
    }

    int n = yypcontext_expected_tokens(ctx, expected, MAX_EXPECTED);
    if (n > 0) {
        int shown = n < MAX_SHOWN ? n : MAX_SHOWN;
        message += ", expecting ";
        for (int i = 0; i < shown; ++i) {
            if (i > 0)
                message += (i + 1 == shown && shown == n) ? " or " : ", ";
            message += yysymbol_name(expected[i]);
        }
        if (shown < n)
            message += ", ...";
    }

    diag_report(g_token_line, g_token_column, "syntax error",
                message.c_str());
    clear_func_locations();
    return 0;
}
