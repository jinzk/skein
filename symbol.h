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
    TYPE_NAMED,
    TYPE_INVALID
};

struct type_constraint {
    enum type_constraint_kind kind;
    /* Interned; non-NULL only for TYPE_NAMED. */
    const char *name;
    const char *module;
};

struct symbol {
    char *name;
    size_t name_len;
    struct value_t *val;
    struct type_constraint type;
    bool exported;
    bool is_const;
    /* True for the implicit receiver binding of a read-only method. */
    bool is_self;
};

struct symlist {
    struct symbol *sym;
    struct type_constraint *type;
    struct symlist *next;
    bool variadic;
};

struct symbol *newsym(const char *name);
struct symlist *newsymlist(struct symbol *sym, struct type_constraint *type,
                           struct symlist *next, bool variadic = false);
void symlistfree(struct symlist *sl);

struct type_constraint *new_type_constraint(enum type_constraint_kind kind);
struct type_constraint *new_named_type_constraint(const char *name,
                                                  const char *module);
void free_type_constraint(struct type_constraint *type);
bool type_constraint_matches(const struct type_constraint *type,
                             const struct value_t *value);
bool type_constraint_equal(const struct type_constraint *a,
                           const struct type_constraint *b);
const char *type_constraint_name(const struct type_constraint *type);

#endif