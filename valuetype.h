#ifndef _VALUETYPE_H_
#define _VALUETYPE_H_

#include <memory>
#include <string>
#include <vector>
#include <utility>

struct ast;
struct symlist;
struct scope_t;

struct function_code {
    struct ast *ast;
    struct symlist *sl;
    int nparams;
    std::string name;
    bool is_method;
    bool variadic;
    bool is_async;
    bool is_generator;
};

struct function_def {
    std::shared_ptr<function_code> code;
    std::shared_ptr<scope_t> closure;
};

enum val_type {
    T_int = 1,
    T_float,
    T_bool,
    T_string,
    T_bytes,
    T_list,
    T_dict,
    T_func,
    T_builtin,
    T_userdata,
    T_struct,
    T_method,
    T_null,
    T_class,
    T_return,
    T_break,
    T_continue
};

struct value_t {
    enum val_type type;
};

struct control_value_t {
    enum val_type type;
    char *label;
};

struct int_value_t {
    enum val_type type;
    long long value;
};

struct float_value_t {
    enum val_type type;
    double value;
};

struct bool_value_t {
    enum val_type type;
    bool value;
};

struct string_value_t {
    enum val_type type;
    char *value;
};

struct bytes_value_t {
    enum val_type type;
    std::string value;
};

struct list_payload {
    std::vector<value_t *> items;
    ~list_payload();
};

struct dict_payload {
    std::vector<std::pair<value_t *, value_t *>> items;
    ~dict_payload();
};

struct list_value_t {
    enum val_type type;
    std::shared_ptr<list_payload> payload;
};

struct dict_value_t {
    enum val_type type;
    std::shared_ptr<dict_payload> payload;
};

struct func_value_t {
    enum val_type type;
    std::shared_ptr<function_def> def;
};

struct builtin_value_t {
    enum val_type type;
    int functype;
};

struct native_type {
    const char *name;
    /* Return a human-readable representation of one instance. */
    std::string (*describe)(const void *data);
};

struct userdata_value_t {
    enum val_type type;
    const struct native_type *meta;
    std::shared_ptr<void> data;
};

struct value_t *newreturn();
struct value_t *newbreak(const char *label = NULL);
struct value_t *newcontinue(const char *label = NULL);
struct value_t *newintval(long long i);
struct value_t *newfloatval(double f);
struct value_t *newboolval(bool b);
struct value_t *newstrval(const char *s);
struct value_t *newbytesval(std::string data);
struct value_t *newbuiltinval(int functype);
struct value_t *newuserdataval(const struct native_type *meta,
                               std::shared_ptr<void> data);
struct value_t *newnull();
struct value_t *newfuncval(std::shared_ptr<function_code> code,
                           std::shared_ptr<struct scope_t> closure);
void free_function_code(struct function_code *code);
struct value_t *dupval(struct value_t *v);

struct list_value_t *newlistval();
struct dict_value_t *newdictval();

/* Free a value and everything it owns. NULL is safe. */
void freeval(struct value_t *v);

/* Print a value in a human-readable form (no trailing newline). */
void printvalue(struct value_t *v);

/* Convert a value to its human-readable string form. */
std::string value_to_string(const struct value_t *v);

const char *typename_of(const struct value_t *v);

#endif
