#include "ast.h"
#include "scopestack.h"
#include <stdio.h>
#include <stdlib.h>

struct ast *newast(int nodetype, struct ast *l, struct ast *r) {
    struct ast *a = (struct ast *)malloc(sizeof(struct ast));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = nodetype;
    a->l = l;
    a->r = r;
    return a;
}

struct ast *newint(int i) {
    struct val *a = (struct val *)malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'V';
    a->val = newintval(i);
    return (struct ast *)a;
}

struct ast *newfloat(double d) {
    struct val *a = (struct val *)malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'V';
    a->val = newfloatval(d);
    return (struct ast *)a;
}


struct ast *newstr(char *s) {
    
    struct val *a = (struct val *)malloc(sizeof(struct val));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'V';
    a->val = newstrval(s);
    return (struct ast *)a;
}

struct ast *newcmp(int cmptype, struct ast *l, struct ast *r) {
    struct ast *a = (struct ast *)malloc(sizeof(struct ast));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = '0' + cmptype;
    a->l = l;
    a->r = r;
    return a;
}

struct ast *newfunc(int functype, struct ast *l) {
    struct fncall *a = (struct fncall *)malloc(sizeof(struct fncall));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'F';
    a->args = l;
    a->functype = (enum bifs)functype;
    return (struct ast *)a;
}

struct ast *newcall(char *s, struct ast *l) {
    struct ufncall *a = (struct ufncall *)malloc(sizeof(struct ufncall));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'C';
    a->args = l;
    a->name = s;
    return (struct ast *)a;
}

struct ast *newref(char *s) {
    struct symref *a = (struct symref *)malloc(sizeof(struct symref));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'N';
    a->name = s;
    return (struct ast *)a;
}

struct ast *newasgn(char *s, struct ast *v) {
    struct symasgn *a = (struct symasgn *)malloc(sizeof(struct symasgn));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = '=';
    a->name = s;
    a->v = v;
    return (struct ast *)a;
}

struct ast *newscope(struct ast *v) {
    struct nscope *a = (struct nscope *)malloc(sizeof(struct nscope));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'X';
    a->ast = v;
    return (struct ast *)a;
}

struct ast *newvar(char *s, struct ast *v) {
    struct symdef *a = (struct symdef *)malloc(sizeof(struct symdef));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'D';
    a->name = s;
    a->v = v;
    return (struct ast *)a;
}

struct ast *newret(struct ast *v) {
    struct fnret *a = (struct fnret *)malloc(sizeof(struct fnret));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'R';
    a->v = v;
    return (struct ast *)a;
}

struct ast *newbrk() {
    struct nbreak *a = (struct nbreak *)malloc(sizeof(struct nbreak));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'B';
    return (struct ast *)a;
}

struct ast *newctn() {
    struct ncontinue *a = (struct ncontinue *)malloc(sizeof(struct ncontinue));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'O';
    return (struct ast *)a;
}

struct ast *newflow(int nodetype, struct ast *cond, struct ast *tl, struct ast *el) {
    struct flow *a = (struct flow *)malloc(sizeof(struct flow));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = nodetype;
    a->cond = cond;
    a->tl = tl;
    a->el = el;
    return (struct ast *)a;
}


struct ast *newfunc(char *s, struct symlist *sl, struct ast *ast) {
    struct funcdef *a = (struct funcdef *)malloc(sizeof(struct funcdef));
    if (!a) {
        yyerror("out of space");
        exit(0);
    }
    a->nodetype = 'Z';
    a->name = s;
    a->sl = sl;
    a->ast = ast;
    return (struct ast *)a;
}

struct ast *newlist(struct ast *v, struct ast *l) {
    struct list_node *p = (struct list_node *)malloc(sizeof(struct list_node));
    p->nodetype = 'l';
    p->value = v;
    p->next = l;
    return (struct ast *)p;
}

struct ast *newdict(struct ast *k, struct ast *v, struct ast *l) {
    struct dict_node *p = (struct dict_node *)malloc(sizeof(struct dict_node));
    p->nodetype = 'd';
    p->value = v;
    p->key = k;
    p->next = l;
    return (struct ast *)p;
}

void treefree(struct ast *a) {
    if (!a) return;
    switch(a->nodetype) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '1': case '2': case '3': case '4': case '5': case '6': 
        case 'L': treefree(a->r);  treefree(a->l); break;
        case 'V': case 'N': case 'B': case 'O': break;
        case 'Z': break;
        case '=': treefree(((struct symasgn *)a)->v);break;
        case 'D': treefree(((struct symdef *)a)->v);break;
        case 'R': treefree(((struct fnret *)a)->v);break;
        case 'I': case 'W': {
            treefree(((struct flow *)a)->cond);
            if(((struct flow *)a)->tl) 
                treefree(((struct flow *)a)->tl);
            if(((struct flow *)a)->el) 
                treefree(((struct flow *)a)->el);
            break;
        }
        case 'C': treefree(((struct ufncall *)a)->args); break;
        case 'F': treefree(((struct fncall *)a)->args); break;
        case 'X': treefree(((struct nscope *)a)->ast); break;
        case 'l': treefree(((struct list_node *)a)->next); break;
        case 'd': treefree(((struct dict_node *)a)->next); break;
        default: printf("inernal error: bad node %c\n", a->nodetype);
    }
    free(a);
}


