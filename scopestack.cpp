#include "scopestack.h"
#include "symbol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static struct scopestack *scope_stack = NULL;
static struct scope_t *global_scope = NULL;

struct scope_t *topscope() {
    if (!scope_stack) return NULL;
    return scope_stack->scope;
}

void pushscope(struct symbol *func) {
    struct scope_t *s = (struct scope_t *)std::malloc(sizeof(struct scope_t));
    if (!s) {
        std::fprintf(stderr, "out of space\n");
        std::exit(1);
    }
    s->syms = NULL;
    s->ret = NULL;
    s->func = func;
    struct scopestack *p = (struct scopestack *)std::malloc(sizeof(struct scopestack));
    if (!p) {
        std::fprintf(stderr, "out of space\n");
        std::exit(1);
    }
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
        struct symbol *sym = s->sym;
        if (sym) {
            freeval(sym->val);
            std::free(sym->name);
            std::free(sym);
        }
        std::free(s);
    }
    /* scope->ret is owned by the caller (function call result), not freed here. */
    std::free(p->scope);
    std::free(p);
}

void initscope() {
    if (scope_stack) return;
    pushscope(NULL);
    global_scope = topscope();
}

void donescope() {
    while (scope_stack) popscope();
    global_scope = NULL;
}

struct symbol *lookupsym(char *sym) {
    struct scopestack *p = scope_stack;
    while (p) {
        struct symlist *sl = p->scope->syms;
        while (sl) {
            if (sl->sym->name && !std::strcmp(sl->sym->name, sym))
                return sl->sym;
            sl = sl->next;
        }
        if (p->scope->func) {
            if (!global_scope) return NULL;
            sl = global_scope->syms;
            while (sl) {
                if (sl->sym->name && !std::strcmp(sl->sym->name, sym))
                    return sl->sym;
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
            if (sl->sym->name && !std::strcmp(sl->sym->name, sym))
                return true;
            sl = sl->next;
        }
    }
    return false;
}

void savesym(struct symbol *sp) {
    if (!scope_stack) return;
    struct symlist *sl = (struct symlist *)std::malloc(sizeof(struct symlist));
    if (!sl) {
        std::fprintf(stderr, "out of space\n");
        std::exit(1);
    }
    sl->sym = sp;
    sl->next = scope_stack->scope->syms;
    scope_stack->scope->syms = sl;
}

bool retval(struct value_t *r) {
    struct scopestack *p = scope_stack;
    while (p) {
        if (p->scope->func) {
            p->scope->ret = r;
            return true;
        }
        p = p->last;
    }
    return false;
}
