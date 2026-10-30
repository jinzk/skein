#ifndef _SYMBOL_H_
#define _SYMBOL_H_
#include "valuetype.h"
extern int yylineno;
void yyerror(const char *s, ...);

struct symbol {
    char *name;
    size_t name_len;
    struct value_t *val;
    bool exported;
};

struct symlist {
    struct symbol *sym;
    struct symlist *next;
};

struct symbol *newsym(const char *name);
struct symlist *newsymlist(struct symbol *sym, struct symlist *next);
void symlistfree(struct symlist *sl);

#endif