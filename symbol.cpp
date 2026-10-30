#include "symbol.h"

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

struct symbol *newsym(const char *name) {
    struct symbol *s = (struct symbol *)std::malloc(sizeof(struct symbol));
    if (!s) {
        yyerror("out of space");
        std::exit(0);
    }
    s->name = dupstr(name);
    s->name_len = std::strlen(s->name);
    s->val = NULL;
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
            std::free(sl->sym->name);
            std::free(sl->sym);
        }
        std::free(sl);
        sl = next;
    }
}
