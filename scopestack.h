#ifndef _SCOPESTACK_H_
#define _SCOPESTACK_H_
#include "valuetype.h"

struct scope_t {
    struct symbol *func;
    value_t *ret;
    struct symlist *syms;
};

struct scopestack {
    struct scope_t *scope;
    struct scopestack *last;
};

struct scope_t *topscope();
void pushscope(struct symbol *);
void initscope();
void popscope();
struct symbol *lookupsym(char *);
bool isrepeated(char *);
void savesym(struct symbol *);
void retval(struct value_t *);

#endif