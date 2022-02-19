#ifndef _VALUETYPE_H_
#define _VALUETYPE_H_

#include <vector>
#include <utility>

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
    int value;
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
    struct ast *ast;
    struct symlist *sl;
};

struct value_t *newreturn();
struct value_t *newbreak();
struct value_t *newcontinue();
struct value_t *newintval(int i);
struct value_t *newfloatval(double f);
struct value_t *newstrval(char *s);
struct value_t *newfuncval(struct ast *ast, struct symlist *sl);
struct value_t *dupval(struct value_t *);

struct list_value_t *newlistval();
struct dict_value_t *newdictval();

#endif
