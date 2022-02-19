#include "eval.h"
#include "scopestack.h"
#include "valuetype.h"
#include <stdio.h>
#include <math.h>
#include <utility>

static struct value_t *callbuildin(struct fncall *f) {
    struct ast *ret = NULL;
    struct value_t *v= eval(f->args);
    if (v->type == T_string) {
        if (f->functype == B_print)
            printf("%s\n", ((string_value_t *)v)->value);
        else
            yyerror("errer call");
    } else {
        double vv = 0;
        if (v->type == T_int) vv = ((int_value_t *)v)->value;
        if (v->type == T_float) vv = ((float_value_t *)v)->value;
        switch (f->functype) {
            case B_sqrt: return newfloatval(sqrt(vv));
            case B_exp: return newfloatval(exp(vv));
            case B_log: return newfloatval(log(vv));
            case B_abs: return newfloatval(vv >= 0 ? vv : -vv);
            case B_print: printf("= %4.4g\n", vv); break;
            default: yyerror("Unknown built-in function %d", f->functype); break;
        }
    }
    return NULL;
}

static struct value_t *calluser(struct ufncall *f) {
    struct symbol *fn = lookupsym(f->name);
    if (fn == NULL) {
        yyerror("function not found %s", f->name);
        return NULL;
    }
    struct symlist *sl;
    struct ast *args = f->args;
    value_t *v;
    int nargs;
    sl = ((struct func_value_t *)fn->val)->sl;
    symlist *sll = NULL;
    for (nargs = 0; sl; sl = sl->next) {
        struct symbol *sym = newsym(sl->sym->name);
        if (!args) {
            yyerror("args error %s", f->name);
            return NULL;
        }
        if (args->nodetype == 'L') {
            sym->val = eval(args->l);
            args = args->r;
        } else {
            sym->val = eval(args);
            args = NULL;
        }
        struct symlist *slx = (struct symlist *)malloc(sizeof(struct symlist));
        slx->sym = sym;
        slx->next = sll;
        sll = slx;
        nargs++;
    }

    pushscope(fn);
    symlist *slp = sll;
    while (slp) {
        savesym(slp->sym);
        slp = slp->next;
    }
    eval(((struct func_value_t *)fn->val)->ast);
    v = topscope()->ret;
    popscope();
    return v;
}

struct value_t *eval(struct ast *a) {
    if (!a) return NULL;
    struct value_t *ret = NULL;
    switch(a->nodetype) {
        case 'V': ret = dupval(((struct val *)a)->val); break;
        case 'N': {
            symbol *s = lookupsym(((struct symref *)a)->name);
            if (!s) {
                yyerror("symbol not found %s", ((struct symref *)a)->name);
            } else {
                ret = dupval(s->val); 
            }
            break;
        }
        case 'D': {
            char *name = ((struct symdef *)a)->name;
            if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else {
                struct value_t *v = eval(((struct symdef *)a)->v);
                if (v) {
                    struct symbol *s = newsym(name);
                    s->val = v;
                    savesym(s);
                }
            }
            break;
        }
        case 'Z': {
            char *name = ((struct funcdef *)a)->name;
            if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else {
                struct symbol *sym = newsym(name);
                sym->val = newfuncval(((struct funcdef *)a)->ast, ((struct funcdef *)a)->sl);
                savesym(sym);
            }
            break;
        }
        case 'R': {
            value_t *v = eval(((struct fnret *)a)->v);
            retval(v);
            ret = newreturn();
            break;
        }
        case 'B': {
            ret = newbreak();
            break;
        }
        case 'O': {
            ret = newcontinue();
            break;
        }
        case 'X': {
            pushscope(NULL);
            ret = eval(((struct nscope *)a)->ast);
            popscope();
            break;
        }
        case '=': {
            symbol *s = lookupsym(((struct symasgn *)a)->name);
            if (!s) {
                yyerror("symbol not found %s", ((struct symref *)a)->name);
            } else {
                s->val = eval(((struct symasgn *)a)->v); 
                ret = dupval(s->val); 
            }
            break;
        }
        case 'l': {
            struct list_node *p = (struct list_node *)a;
            list_value_t *v = newlistval();
            while (p != NULL) {
                v->value.push_back(dupval(((struct val *)p->value)->val));
                p = (struct list_node *)(p->next);
            }
            ret = (value_t *)v;
            break;
        }
        case 'd': {
            struct dict_node *p = (struct dict_node *)a;
            dict_value_t *v = newdictval();
            while (p != NULL) {
                v->value.push_back(std::make_pair(dupval(((struct val *)p->key)->val),dupval(((struct val *)p->value)->val)));
                p = (struct dict_node *)(p->next);
            }
            ret = (value_t *)v;
            break;
        }
        case '+': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double d = (l->type == T_int) ? ((struct int_value_t *)l)->value : ((struct float_value_t *)l)->value;
                d += (r->type == T_int) ? ((struct int_value_t *)r)->value : ((struct float_value_t *)r)->value;
                ret = newfloatval(d);
            } else {
                ret = newintval(((struct int_value_t *)l)->value + ((struct int_value_t *)r)->value);
            }
            free(l);free(r);
            break;
        }
        case '-': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double d = (l->type == T_int) ? ((struct int_value_t *)l)->value : ((struct float_value_t *)l)->value;
                d -= (r->type == T_int) ? ((struct int_value_t *)r)->value : ((struct float_value_t *)r)->value;
                ret = newfloatval(d);
            } else {
                ret = newintval(((struct int_value_t *)l)->value - ((struct int_value_t *)r)->value);
            }
            free(l);free(r);
            break;
        }
        case '*': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double d = (l->type == T_int) ? ((struct int_value_t *)l)->value : ((struct float_value_t *)l)->value;
                d *= (r->type == T_int) ? ((struct int_value_t *)r)->value : ((struct float_value_t *)r)->value;
                ret = newfloatval(d);
            } else {
                ret = newintval(((struct int_value_t *)l)->value * ((struct int_value_t *)r)->value);
            }
            free(l);free(r);
            break;
        }
        case '/': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double d = (l->type == T_int) ? ((struct int_value_t *)l)->value : ((struct float_value_t *)l)->value;
                d /= (r->type == T_int) ? ((struct int_value_t *)r)->value : ((struct float_value_t *)r)->value;
                ret = newfloatval(d);
            } else {
                ret = newintval(((struct int_value_t *)l)->value / ((struct int_value_t *)r)->value);
            }
            free(l);free(r);
            break;
        }

        case '1': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double a = 0, b = 0;
                if (l->type == T_int) a = ((struct int_value_t *)l)->value;
                else a = ((struct float_value_t *)l)->value;
                if (r->type == T_int) b = ((struct int_value_t *)r)->value;
                else b = ((struct float_value_t *)r)->value;
                ret = newintval(a > b ? 1 : 0);
            } else {
                ret = newintval(((struct int_value_t *)l)->value > ((struct int_value_t *)r)->value ? 1 : 0);
            }
            free(l);free(r);
            break;
        }
        case '2': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double a = 0, b = 0;
                if (l->type == T_int) a = ((struct int_value_t *)l)->value;
                else a = ((struct float_value_t *)l)->value;
                if (r->type == T_int) b = ((struct int_value_t *)r)->value;
                else b = ((struct float_value_t *)r)->value;
                ret = newintval(a < b ? 1 : 0);
            } else {
                ret = newintval(((struct int_value_t *)l)->value < ((struct int_value_t *)r)->value ? 1 : 0);
            }
            free(l);free(r);
            break;
        }
        case '3': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double a = 0, b = 0;
                if (l->type == T_int) a = ((struct int_value_t *)l)->value;
                else a = ((struct float_value_t *)l)->value;
                if (r->type == T_int) b = ((struct int_value_t *)r)->value;
                else b = ((struct float_value_t *)r)->value;
                ret = newintval(a != b ? 1 : 0);
            } else {
                ret = newintval(((struct int_value_t *)l)->value != ((struct int_value_t *)r)->value ? 1 : 0);
            }
            free(l);free(r);
            break;
        }
        case '4': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double a = 0, b = 0;
                if (l->type == T_int) a = ((struct int_value_t *)l)->value;
                else a = ((struct float_value_t *)l)->value;
                if (r->type == T_int) b = ((struct int_value_t *)r)->value;
                else b = ((struct float_value_t *)r)->value;
                ret = newintval(a == b ? 1 : 0);
            } else {
                ret = newintval(((struct int_value_t *)l)->value == ((struct int_value_t *)r)->value ? 1 : 0);
            }
            free(l);free(r);
            break;
        }
        case '5': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double a = 0, b = 0;
                if (l->type == T_int) a = ((struct int_value_t *)l)->value;
                else a = ((struct float_value_t *)l)->value;
                if (r->type == T_int) b = ((struct int_value_t *)r)->value;
                else b = ((struct float_value_t *)r)->value;
                ret = newintval(a >= b ? 1 : 0);
            } else {
                ret = newintval(((struct int_value_t *)l)->value >= ((struct int_value_t *)r)->value ? 1 : 0);
            }
            free(l);free(r);
            break;
        }
        case '6': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l->type == T_float || r->type == T_float) {
                double a = 0, b = 0;
                if (l->type == T_int) a = ((struct int_value_t *)l)->value;
                else a = ((struct float_value_t *)l)->value;
                if (r->type == T_int) b = ((struct int_value_t *)r)->value;
                else b = ((struct float_value_t *)r)->value;
                ret = newintval(a <= b ? 1 : 0);
            } else {
                ret = newintval(((struct int_value_t *)l)->value <= ((struct int_value_t *)r)->value ? 1 : 0);
            }
            free(l);free(r);
            break;
        }

        case 'I': {
            if (((struct int_value_t *)eval(((struct flow *)a)->cond))->value) {
                if(((struct flow *)a)->tl) {
                    ret = eval(((struct flow *)a)->tl);
                }
            } else {
                if(((struct flow *)a)->el) {
                    ret = eval(((struct flow *)a)->el);
                }
            }
            break;
        }

        case 'W': {
            if(((struct flow *)a)->tl) {
                while(((struct int_value_t *)eval(((struct flow *)a)->cond))->value) {
                    ret = eval(((struct flow *)a)->tl);
                    if (ret && ret->type == T_break) break;
                    if (ret && ret->type == T_continue) {/*do nothing*/};
                }
            }
            break;
        }
        case 'L': {
            ret = eval(a->l);
            if (!ret || (ret->type != T_return && ret->type != T_break && ret->type != T_continue)) {
                ret = eval(a->r);
            }
            break;
        }

        case 'F': ret = callbuildin((struct fncall *)a); break;
        case 'C': ret = calluser((struct ufncall *)a); break;

        default: printf("inernal error: bad node %c\n", a->nodetype);
    }
    return ret;
}