#include "ast.h"
#include "scopestack.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>

struct ast *newast(int nodetype, struct ast *l, struct ast *r) {
    struct ast *a = (struct ast *)std::malloc(sizeof(struct ast));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = nodetype;
    a->l = l;
    a->r = r;
    return a;
}

struct ast *newint(long long i) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    a->val = newintval(i);
    return (struct ast *)a;
}

struct ast *newfloat(double d) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    a->val = newfloatval(d);
    return (struct ast *)a;
}

struct ast *newstr(char *s) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    a->val = newstrval(s);
    return (struct ast *)a;
}

struct ast *newbuiltin(int functype) {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    a->val = newbuiltinval(functype);
    return (struct ast *)a;
}

struct ast *newnullast() {
    struct val *a = (struct val *)std::malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'V';
    a->val = newnull();
    return (struct ast *)a;
}

struct ast *newcmp(int cmptype, struct ast *l, struct ast *r) {
    struct ast *a = (struct ast *)std::malloc(sizeof(struct ast));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = '0' + cmptype;
    a->l = l;
    a->r = r;
    return a;
}

struct ast *newcall(struct ast *callee, struct ast *l) {
    struct ufncall *a = (struct ufncall *)std::malloc(sizeof(struct ufncall));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'C';
    a->callee = callee;
    a->args = l;
    return (struct ast *)a;
}

struct ast *newref(char *s) {
    struct symref *a = (struct symref *)std::malloc(sizeof(struct symref));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'N';
    a->name = s;
    return (struct ast *)a;
}

struct ast *newasgn(struct ast *target, struct ast *v) {
    struct symasgn *a = (struct symasgn *)std::malloc(sizeof(struct symasgn));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = '=';
    a->target = target;
    a->v = v;
    return (struct ast *)a;
}

struct ast *newscope(struct ast *v) {
    struct nscope *a = (struct nscope *)std::malloc(sizeof(struct nscope));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'X';
    a->ast = v;
    return (struct ast *)a;
}

struct ast *newvar(char *s, struct ast *v) {
    struct symdef *a = (struct symdef *)std::malloc(sizeof(struct symdef));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'D';
    a->name = s;
    a->v = v;
    return (struct ast *)a;
}

struct ast *newret(struct ast *v) {
    struct fnret *a = (struct fnret *)std::malloc(sizeof(struct fnret));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'R';
    a->v = v;
    return (struct ast *)a;
}

struct ast *newbrk() {
    struct nbreak *a = (struct nbreak *)std::malloc(sizeof(struct nbreak));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'B';
    return (struct ast *)a;
}

struct ast *newctn() {
    struct ncontinue *a = (struct ncontinue *)std::malloc(sizeof(struct ncontinue));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'O';
    return (struct ast *)a;
}

struct ast *newflow(int nodetype, struct ast *cond, struct ast *tl, struct ast *el) {
    struct flow *a = (struct flow *)std::malloc(sizeof(struct flow));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = nodetype;
    a->cond = cond;
    a->tl = tl;
    a->el = el;
    return (struct ast *)a;
}

struct ast *newfor(struct ast *init, struct ast *cond, struct ast *step, struct ast *body) {
    struct forloop *a = (struct forloop *)std::malloc(sizeof(struct forloop));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'P';
    a->init = init;
    a->cond = cond;
    a->step = step;
    a->body = body;
    return (struct ast *)a;
}

struct ast *newfunc(char *s, struct symlist *sl, struct ast *ast) {
    struct funcdef *a = (struct funcdef *)std::malloc(sizeof(struct funcdef));
    if (!a) {
        yyerror("out of space");
        std::exit(0);
    }
    a->nodetype = 'Z';
    a->name = s;
    a->sl = sl;
    a->ast = ast;
    a->moved = 0;
    return (struct ast *)a;
}

void free_function_code(struct function_code *code) {
    if (!code) return;
    if (code->ast) treefree(code->ast);
    if (code->sl) symlistfree(code->sl);
    delete code;
}

/*
 * A single `def` AST node can be executed more than once (a nested function
 * defined inside another function).  All executions share the same code
 * (body + parameter list), but each gets its own closure environment.
 */
static std::unordered_map<struct funcdef *, std::shared_ptr<function_code>>
    function_codes;

std::shared_ptr<function_code> get_function_code(struct funcdef *node) {
    std::unordered_map<struct funcdef *, std::shared_ptr<function_code>>::iterator
        it = function_codes.find(node);
    if (it != function_codes.end())
        return it->second;

    int nparams = 0;
    for (struct symlist *sl = node->sl; sl; sl = sl->next) ++nparams;
    std::shared_ptr<function_code> code(
        new function_code{node->ast, node->sl, nparams}, free_function_code);
    function_codes[node] = code;
    return code;
}

void release_function_code(struct funcdef *node) {
    function_codes.erase(node);
}

struct ast *newlist(struct ast *v, struct ast *l) {
    struct list_node *p = (struct list_node *)std::malloc(sizeof(struct list_node));
    if (!p) {
        yyerror("out of space");
        std::exit(0);
    }
    p->nodetype = 'l';
    p->value = v;
    p->next = l;
    return (struct ast *)p;
}

struct ast *newdict(struct ast *k, struct ast *v, struct ast *l) {
    struct dict_node *p = (struct dict_node *)std::malloc(sizeof(struct dict_node));
    if (!p) {
        yyerror("out of space");
        std::exit(0);
    }
    p->nodetype = 'd';
    p->value = v;
    p->key = k;
    p->next = l;
    return (struct ast *)p;
}

void treefree(struct ast *a) {
    if (!a) return;
    switch (a->nodetype) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 'A':
        case 'Q':
        case 's':
        case 'r':
        case 'G':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case 'L':
            treefree(a->r);
            treefree(a->l);
            break;
        case 'M':
        case '!':
        case '~':
            treefree(a->l);
            break;
        case 'V':
            freeval(((struct val *)a)->val);
            break;
        case 'N':
            std::free(((struct symref *)a)->name);
            break;
        case 'B':
        case 'O':
            break;
        case '=':
            treefree(((struct symasgn *)a)->target);
            treefree(((struct symasgn *)a)->v);
            break;
        case 'D':
            treefree(((struct symdef *)a)->v);
            std::free(((struct symdef *)a)->name);
            break;
        case 'R':
            treefree(((struct fnret *)a)->v);
            break;
        case 'Z': {
            /*
             * If the definition was evaluated, the body and parameter list
             * are owned by the runtime function value.  Otherwise (for
             * example, a duplicate definition error) release them here.
             */
            struct funcdef *f = (struct funcdef *)a;
            if (f->moved) {
                release_function_code(f);
            } else {
                treefree(f->ast);
                if (f->sl) symlistfree(f->sl);
            }
            std::free(f->name);
            break;
        }
        case 'I':
        case 'W': {
            struct flow *f = (struct flow *)a;
            treefree(f->cond);
            treefree(f->tl);
            treefree(f->el);
            break;
        }
        case 'P': {
            struct forloop *f = (struct forloop *)a;
            treefree(f->init);
            treefree(f->cond);
            treefree(f->step);
            treefree(f->body);
            break;
        }
        case 'C':
            treefree(((struct ufncall *)a)->callee);
            treefree(((struct ufncall *)a)->args);
            break;
        case 'X':
            treefree(((struct nscope *)a)->ast);
            break;
        case 'l': {
            struct list_node *p = (struct list_node *)a;
            treefree(p->value);
            treefree(p->next);
            break;
        }
        case 'd': {
            struct dict_node *p = (struct dict_node *)a;
            treefree(p->key);
            treefree(p->value);
            treefree(p->next);
            break;
        }
        default:
            std::fprintf(stderr, "internal error: bad node %c\n", a->nodetype);
            break;
    }
    std::free(a);
}
