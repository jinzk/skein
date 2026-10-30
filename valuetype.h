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
};

struct function_def {
    std::shared_ptr<function_code> code;
    std::shared_ptr<scope_t> closure;
};

enum val_type {
    T_int = 1,
    T_float,
    T_string,
    T_list,
    T_dict,
    T_func,
    T_class,
    T_return,
    T_break,
    T_continue
};

struct value_t {
    enum val_type type;
};

struct int_value_t {
    enum val_type type;
    long long value;
};

struct float_value_t {
    enum val_type type;
    double value;
};

struct string_value_t {
    enum val_type type;
    char *value;
};

struct list_value_t {
    enum val_type type;
    std::vector<value_t *> value;
};

struct dict_value_t {
    enum val_type type;
    std::vector<std::pair<value_t *, value_t *>> value;
};

struct func_value_t {
    enum val_type type;
    std::shared_ptr<function_def> def;
};



struct value_t *newreturn();
struct value_t *newbreak();
struct value_t *newcontinue();
struct value_t *newintval(long long i);
struct value_t *newfloatval(double f);
struct value_t *newstrval(const char *s);
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
