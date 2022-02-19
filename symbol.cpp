#include "symbol.h"
#include <string.h>
#include <stdlib.h>

struct symbol *newsym(char *name) {
    struct symbol *s = (struct symbol *)malloc(sizeof(struct symbol));
    if (!s) {
        yyerror("out of space");
        exit(0);
    }
    s->name = name;
    s->val = NULL;
    return s;
}

struct symlist *newsymlist(struct symbol *sym, struct symlist *next) {
    struct symlist *sl = (struct symlist *)malloc(sizeof(struct symlist));
    if (!sl) {
        yyerror("out of space");
        exit(0);
    }
    sl->sym = sym;
    sl->next = next;
    return sl;
}

void symlistfree(struct symlist *sl) {
    struct symlist *nsl;
    while (sl) {
        nsl = sl->next;
        free(sl);
        sl = nsl;
    }
}