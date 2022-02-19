#include "valuetype.h"
#include <stdlib.h>

struct value_t *newreturn() {
    struct value_t *v = (struct value_t *)malloc(sizeof(struct value_t));
    v->type = T_return;
    return v;
}
struct value_t *newbreak() {
    struct value_t *v = (struct value_t *)malloc(sizeof(struct value_t));
    v->type = T_break;
    return v;
}
struct value_t *newcontinue() {
    struct value_t *v = (struct value_t *)malloc(sizeof(struct value_t));
    v->type = T_continue;
    return v;
}

struct value_t *newintval(int i) {
    struct int_value_t *v = (struct int_value_t *)malloc(sizeof(struct int_value_t));
    v->type = T_int;
    v->value = i;
    return (struct value_t *)v;
}

struct value_t *newfloatval(double f) {
    struct float_value_t *v = (struct float_value_t *)malloc(sizeof(struct float_value_t));
    v->type = T_float;
    v->value = f;
    return (struct value_t *)v;
}

struct value_t *newstrval(char *s) {
    struct string_value_t *v = (struct string_value_t *)malloc(sizeof(struct string_value_t));
    v->type = T_string;
    v->value = s;
    return (struct value_t *)v;
}

struct value_t *newfuncval(struct ast *ast, struct symlist *sl) {
    struct func_value_t *v = (struct func_value_t *)malloc(sizeof(struct func_value_t));
    v->type = T_func;
    v->ast = ast;
    v->sl = sl;
    return (struct value_t *)v;
}

struct list_value_t *newlistval() {
    struct list_value_t *v = (struct list_value_t *)malloc(sizeof(struct list_value_t));
    v->type = T_list;
    return v;
}
struct dict_value_t *newdictval() {
    struct dict_value_t *v = (struct dict_value_t *)malloc(sizeof(struct dict_value_t));
    v->type = T_dict;
    return v;
}

struct value_t *dupval(struct value_t *v) {
    struct value_t *ret = NULL;
    switch(v->type) {
        case T_int: ret = newintval(((struct int_value_t *)v)->value); break;
        case T_float: ret = newfloatval(((struct float_value_t *)v)->value); break;
        case T_string: ret = newstrval(((struct string_value_t *)v)->value); break;
        case T_list: ret = v; break;
        case T_dict: ret = v; break;
        case T_func: ret = v; break;
        default: break;
    }
    return ret;
}