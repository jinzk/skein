#include "symbol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

struct symbol *newsym(const char *name) {
    if (!name) name = "";
    size_t len = std::strlen(name);
    struct symbol *s =
        (struct symbol *)std::malloc(sizeof(struct symbol) + len + 1);
    if (!s) {
        yyerror("out of space");
        std::exit(0);
    }
    s->name = (char *)(s + 1);
    std::memcpy(s->name, name, len + 1);
    s->name_len = len;
    s->val = NULL;
    s->exported = false;
    return s;
}

struct symlist *newsymlist(struct symbol *sym, struct symlist *next) {
    struct symlist *sl = (struct symlist *)std::malloc(sizeof(struct symlist));
    if (!sl) {
        yyerror("out of space");
        std::exit(0);
    }
    sl->sym = sym;
    sl->next = next;
    return sl;
}

void symlistfree(struct symlist *sl) {
    while (sl) {
        struct symlist *next = sl->next;
        if (sl->sym) {
            std::free(sl->sym);
        }
        std::free(sl);
        sl = next;
    }
}
