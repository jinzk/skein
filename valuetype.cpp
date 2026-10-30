#include "valuetype.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static char *dupstr(const char *s) {
    if (!s) s = "";
    size_t len = std::strlen(s) + 1;
    char *p = (char *)std::malloc(len);
    if (!p) {
        std::fprintf(stderr, "out of space\n");
        std::exit(1);
    }
    std::memcpy(p, s, len);
    return p;
}

struct value_t *newreturn() {
    struct value_t *v = new value_t;
    v->type = T_return;
    return v;
}

struct value_t *newbreak() {
    struct value_t *v = new value_t;
    v->type = T_break;
    return v;
}

struct value_t *newcontinue() {
    struct value_t *v = new value_t;
    v->type = T_continue;
    return v;
}

struct value_t *newintval(int i) {
    struct int_value_t *v = new int_value_t;
    v->type = T_int;
    v->value = i;
    return (struct value_t *)v;
}

struct value_t *newfloatval(double f) {
    struct float_value_t *v = new float_value_t;
    v->type = T_float;
    v->value = f;
    return (struct value_t *)v;
}

struct value_t *newstrval(const char *s) {
    struct string_value_t *v = new string_value_t;
    v->type = T_string;
    v->value = dupstr(s);
    return (struct value_t *)v;
}

struct value_t *newfuncval(struct ast *ast, struct symlist *sl) {
    struct func_value_t *v = new func_value_t;
    v->type = T_func;
    v->def = std::shared_ptr<function_def>(new function_def{ast, sl},
                                          free_function_def);
    return (struct value_t *)v;
}

struct list_value_t *newlistval() {
    struct list_value_t *v = new list_value_t;
    v->type = T_list;
    return v;
}

struct dict_value_t *newdictval() {
    struct dict_value_t *v = new dict_value_t;
    v->type = T_dict;
    return v;
}

struct value_t *dupval(struct value_t *v) {
    if (!v) return NULL;
    switch (v->type) {
        case T_int:
            return newintval(((struct int_value_t *)v)->value);
        case T_float:
            return newfloatval(((struct float_value_t *)v)->value);
        case T_string:
            return newstrval(((struct string_value_t *)v)->value);
        case T_list: {
            struct list_value_t *src = (struct list_value_t *)v;
            struct list_value_t *dst = newlistval();
            dst->value.reserve(src->value.size());
            for (size_t i = 0; i < src->value.size(); ++i)
                dst->value.push_back(dupval(src->value[i]));
            return (struct value_t *)dst;
        }
        case T_dict: {
            struct dict_value_t *src = (struct dict_value_t *)v;
            struct dict_value_t *dst = newdictval();
            dst->value.reserve(src->value.size());
            for (size_t i = 0; i < src->value.size(); ++i)
                dst->value.push_back(std::make_pair(dupval(src->value[i].first),
                                                    dupval(src->value[i].second)));
            return (struct value_t *)dst;
        }
        case T_func: {
            struct func_value_t *src = (struct func_value_t *)v;
            struct func_value_t *dst = new func_value_t;
            dst->type = T_func;
            dst->def = src->def;
            return (struct value_t *)dst;
        }
        case T_return:
            return newreturn();
        case T_break:
            return newbreak();
        case T_continue:
            return newcontinue();
        default:
            return NULL;
    }
}

void freeval(struct value_t *v) {
    if (!v) return;
    switch (v->type) {
        case T_int:
            delete (struct int_value_t *)v;
            break;
        case T_float:
            delete (struct float_value_t *)v;
            break;
        case T_string: {
            struct string_value_t *s = (struct string_value_t *)v;
            std::free(s->value);
            delete s;
            break;
        }
        case T_list: {
            struct list_value_t *l = (struct list_value_t *)v;
            for (size_t i = 0; i < l->value.size(); ++i)
                freeval(l->value[i]);
            delete l;
            break;
        }
        case T_dict: {
            struct dict_value_t *d = (struct dict_value_t *)v;
            for (size_t i = 0; i < d->value.size(); ++i) {
                freeval(d->value[i].first);
                freeval(d->value[i].second);
            }
            delete d;
            break;
        }
        case T_func:
            delete (struct func_value_t *)v;
            break;
        case T_return:
        case T_break:
        case T_continue:
            delete v;
            break;
        default:
            break;
    }
}

const char *typename_of(const struct value_t *v) {
    if (!v) return "null";
    switch (v->type) {
        case T_int: return "int";
        case T_float: return "float";
        case T_string: return "string";
        case T_list: return "list";
        case T_dict: return "dict";
        case T_func: return "function";
        case T_class: return "class";
        case T_return: return "return";
        case T_break: return "break";
        case T_continue: return "continue";
        default: return "unknown";
    }
}

void printvalue(struct value_t *v) {
    if (!v) {
        std::printf("null");
        return;
    }
    switch (v->type) {
        case T_int:
            std::printf("%d", ((struct int_value_t *)v)->value);
            break;
        case T_float:
            std::printf("%.4g", ((struct float_value_t *)v)->value);
            break;
        case T_string:
            std::printf("%s", ((struct string_value_t *)v)->value);
            break;
        case T_list: {
            struct list_value_t *l = (struct list_value_t *)v;
            std::printf("[");
            for (size_t i = 0; i < l->value.size(); ++i) {
                if (i) std::printf(", ");
                printvalue(l->value[i]);
            }
            std::printf("]");
            break;
        }
        case T_dict: {
            struct dict_value_t *d = (struct dict_value_t *)v;
            std::printf("{");
            for (size_t i = 0; i < d->value.size(); ++i) {
                if (i) std::printf(", ");
                printvalue(d->value[i].first);
                std::printf(": ");
                printvalue(d->value[i].second);
            }
            std::printf("}");
            break;
        }
        case T_func:
            std::printf("<function>");
            break;
        case T_class:
            std::printf("<class>");
            break;
        case T_return:
            std::printf("<return>");
            break;
        case T_break:
            std::printf("<break>");
            break;
        case T_continue:
            std::printf("<continue>");
            break;
        default:
            std::printf("<unknown>");
            break;
    }
}
