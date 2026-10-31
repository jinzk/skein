#ifndef _SYMBOL_H_
#define _SYMBOL_H_
#include "valuetype.h"
extern int yylineno;
void yyerror(const char *s, ...);

enum type_constraint_kind {
    TYPE_ANY,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_NUMBER,
    TYPE_BOOL,
    TYPE_STRING,
    TYPE_BYTES,
    TYPE_LIST,
    TYPE_DICT,
    TYPE_FUNCTION,
    TYPE_BUILTIN,
    TYPE_CALLABLE,
    TYPE_NULL,
    TYPE_USERDATA,
    TYPE_INVALID
};

struct type_constraint {
    enum type_constraint_kind kind;
};

struct symbol {
    char *name;
    size_t name_len;
    struct value_t *val;
    struct type_constraint type;
    bool exported;
    bool is_const;
};

struct symlist {
    struct symbol *sym;
    struct type_constraint *type;
    struct symlist *next;
};

struct symbol *newsym(const char *name);
struct symlist *newsymlist(struct symbol *sym, struct type_constraint *type,
                           struct symlist *next);
void symlistfree(struct symlist *sl);

struct type_constraint *new_type_constraint(enum type_constraint_kind kind);
void free_type_constraint(struct type_constraint *type);
bool type_constraint_matches(const struct type_constraint *type,
                             const struct value_t *value);
const char *type_constraint_name(const struct type_constraint *type);

#endif