#include "scopestack.h"
#include "symbol.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static struct scopestack *scope_stack = NULL;
static struct scope_t *global_scope = NULL;

struct scope_t *topscope() {
    if (!scope_stack) return NULL;
    return scope_stack->scope;
}

void pushscope(struct symbol *func) {
    struct scope_t * s = (struct scope_t *)malloc(sizeof(struct scope_t));
    s->syms = NULL;
    s->ret = NULL;
    s->func = func;
    struct scopestack *p = (struct scopestack *)malloc(sizeof(struct scopestack));
    p->last = scope_stack;
    p->scope = s;
    scope_stack = p;
}

void popscope() {
    if (!scope_stack) return;
    struct scopestack *p = scope_stack;
    scope_stack = p->last;
    struct symlist *sp = p->scope->syms;
    while (sp) {
        struct symlist *s = sp;
        sp = sp->next;
        free(s->sym);
        free(s);
    }
    free(p->scope);
    free(p);
}

void initscope() {
    if (scope_stack) return;
    pushscope(NULL);
    global_scope = topscope();
}

struct symbol *lookupsym(char *sym) {
    struct scopestack *p = scope_stack;
    while (p) {
        struct symlist *sl = p->scope->syms;
        while (sl) {
            if(sl->sym->name && !strcmp(sl->sym->name, sym)) {
                return sl->sym;
            }
            sl = sl->next;
        }
        if (p->scope->func) {
            sl = global_scope->syms;
            while (sl) {
                if(sl->sym->name && !strcmp(sl->sym->name, sym)) {
                    return sl->sym;
                }
                sl = sl->next;
            }
            return NULL;
        }
        p = p->last;
    }
    return NULL;
}

bool isrepeated(char *sym) {
    struct scopestack *p = scope_stack;
    if (p) {
        struct symlist *sl = p->scope->syms;
        while (sl) {
            if(sl->sym->name && !strcmp(sl->sym->name, sym)) {
                return true;
            }
            sl = sl->next;
        }
    }
    return false;
}

void savesym(struct symbol *sp) {
    struct symlist *sl = (struct symlist *)malloc(sizeof(struct symlist));
    sl->sym = sp;
    sl->next = scope_stack->scope->syms;
    scope_stack->scope->syms = sl;

}

void retval(struct value_t *r) {
    struct scopestack *p = scope_stack;
    while (p) {
        if (p->scope->func) {
            p->scope->ret = r;
            break;
        }
        p = p->last;
    }
}
