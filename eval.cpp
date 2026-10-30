#include "eval.h"
#include "scopestack.h"
#include "symbol.h"
#include "valuetype.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool is_numeric(const struct value_t *v) {
    return v && (v->type == T_int || v->type == T_float);
}

static struct value_t *new_int_or_float(long long n) {
    if (n >= INT_MIN && n <= INT_MAX)
        return newintval((int)n);
    return newfloatval((double)n);
}

static double as_double(const struct value_t *v) {
    if (!v) return 0.0;
    if (v->type == T_int) return (double)((struct int_value_t *)v)->value;
    if (v->type == T_float) return ((struct float_value_t *)v)->value;
    return 0.0;
}

static bool truthy(const struct value_t *v) {
    if (!v) return false;
    switch (v->type) {
        case T_int: return ((struct int_value_t *)v)->value != 0;
        case T_float: return ((struct float_value_t *)v)->value != 0.0;
        case T_string: {
            const char *s = ((struct string_value_t *)v)->value;
            return s && *s;
        }
        case T_list: return !((struct list_value_t *)v)->value.empty();
        case T_dict: return !((struct dict_value_t *)v)->value.empty();
        case T_func: return true;
        default: return false;
    }
}

static bool value_equals(struct value_t *a, struct value_t *b) {
    if (!a || !b) return a == b;
    if (is_numeric(a) && is_numeric(b))
        return as_double(a) == as_double(b);
    if (a->type != b->type) return false;
    switch (a->type) {
        case T_string:
            return std::strcmp(((struct string_value_t *)a)->value,
                               ((struct string_value_t *)b)->value) == 0;
        case T_list: {
            struct list_value_t *la = (struct list_value_t *)a;
            struct list_value_t *lb = (struct list_value_t *)b;
            if (la->value.size() != lb->value.size()) return false;
            for (size_t i = 0; i < la->value.size(); ++i)
                if (!value_equals(la->value[i], lb->value[i])) return false;
            return true;
        }
        case T_dict: {
            struct dict_value_t *da = (struct dict_value_t *)a;
            struct dict_value_t *db = (struct dict_value_t *)b;
            if (da->value.size() != db->value.size()) return false;
            for (size_t i = 0; i < da->value.size(); ++i) {
                bool found = false;
                for (size_t j = 0; j < db->value.size(); ++j) {
                    if (value_equals(da->value[i].first, db->value[j].first)) {
                        if (!value_equals(da->value[i].second, db->value[j].second))
                            return false;
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        }
        case T_func: {
            struct func_value_t *fa = (struct func_value_t *)a;
            struct func_value_t *fb = (struct func_value_t *)b;
            return fa->def == fb->def;
        }
        default:
            return false;
    }
}

static struct value_t *do_compare(int op, struct value_t *l, struct value_t *r) {
    if (!l || !r) return NULL;

    if (!(is_numeric(l) && is_numeric(r)) &&
        !(l->type == T_string && r->type == T_string)) {
        if (op == 3 || op == 4) {
            bool eq = value_equals(l, r);
            return newintval((op == 4) ? (eq ? 1 : 0) : (eq ? 0 : 1));
        }
        yyerror("cannot compare %s and %s", typename_of(l), typename_of(r));
        return NULL;
    }

    int cmp;
    if (is_numeric(l) && is_numeric(r)) {
        double a = as_double(l), b = as_double(r);
        cmp = (a < b) ? -1 : (a > b ? 1 : 0);
    } else {
        cmp = std::strcmp(((struct string_value_t *)l)->value,
                          ((struct string_value_t *)r)->value);
        cmp = (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
    }

    switch (op) {
        case 1: return newintval(cmp > 0 ? 1 : 0);
        case 2: return newintval(cmp < 0 ? 1 : 0);
        case 3: return newintval(cmp != 0 ? 1 : 0);
        case 4: return newintval(cmp == 0 ? 1 : 0);
        case 5: return newintval(cmp >= 0 ? 1 : 0);
        case 6: return newintval(cmp <= 0 ? 1 : 0);
        default: return NULL;
    }
}

static void free_args(std::vector<struct value_t *> &args) {
    for (size_t i = 0; i < args.size(); ++i)
        freeval(args[i]);
    args.clear();
}

/*
 * Function arguments are represented as a right-nested 'L' chain.  Evaluate
 * them left-to-right without using eval('L'), because eval('L') intentionally
 * frees intermediate statement values.
 */
static bool collect_args(struct ast *a, std::vector<struct value_t *> &out) {
    if (!a) return true;
    if (a->nodetype == 'L') {
        if (!collect_args(a->l, out)) return false;
        return collect_args(a->r, out);
    }
    struct value_t *v = eval(a);
    if (!v) return false;
    if (v->type == T_return || v->type == T_break || v->type == T_continue) {
        freeval(v);
        yyerror("invalid argument expression");
        return false;
    }
    out.push_back(v);
    return true;
}

static struct value_t *callbuildin(struct fncall *f) {
    std::vector<struct value_t *> args;
    if (!collect_args(f->args, args)) {
        free_args(args);
        return NULL;
    }
    if (args.size() != 1) {
        yyerror("built-in function expects 1 argument, got %d", (int)args.size());
        free_args(args);
        return NULL;
    }

    struct value_t *arg = args[0];
    struct value_t *ret = NULL;
    switch (f->functype) {
        case B_print:
            printvalue(arg);
            std::printf("\n");
            break;
        case B_abs:
            if (arg->type == T_int) {
                long long n = ((struct int_value_t *)arg)->value;
                if (n < 0) n = -n;
                if (n > 2147483647LL)
                    ret = newfloatval((double)n);
                else
                    ret = newintval((int)n);
            } else if (arg->type == T_float) {
                ret = newfloatval(std::fabs(((struct float_value_t *)arg)->value));
            } else {
                yyerror("abs expects a number, got %s", typename_of(arg));
            }
            break;
        case B_sqrt:
        case B_exp:
        case B_log:
            if (!is_numeric(arg)) {
                yyerror("math function expects a number, got %s", typename_of(arg));
                break;
            }
            if (f->functype == B_sqrt)
                ret = newfloatval(std::sqrt(as_double(arg)));
            else if (f->functype == B_exp)
                ret = newfloatval(std::exp(as_double(arg)));
            else
                ret = newfloatval(std::log(as_double(arg)));
            break;
        default:
            yyerror("unknown built-in function %d", f->functype);
            break;
    }
    free_args(args);
    return ret;
}

static struct value_t *calluser(struct ufncall *f) {
    struct symbol *fn = lookupsym(f->name);
    if (!fn || !fn->val) {
        yyerror("function not found %s", f->name);
        return NULL;
    }
    if (fn->val->type != T_func) {
        yyerror("%s is not a function", f->name);
        return NULL;
    }

    struct func_value_t *fv = (struct func_value_t *)fn->val;
    /*
     * Keep the function definition alive for the whole call.  The body can
     * reassign/free the variable that holds this function value.
     */
    std::shared_ptr<function_def> def = fv->def;
    std::vector<struct value_t *> args;
    if (!collect_args(f->args, args)) {
        free_args(args);
        return NULL;
    }

    int nparams = 0;
    for (struct symlist *sl = def->sl; sl; sl = sl->next) ++nparams;
    if ((int)args.size() != nparams) {
        yyerror("function %s expects %d argument(s), got %d",
                f->name, nparams, (int)args.size());
        free_args(args);
        return NULL;
    }

    pushscope(fn);
    int i = 0;
    for (struct symlist *sl = def->sl; sl; sl = sl->next, ++i) {
        struct symbol *sym = newsym(sl->sym->name);
        sym->val = args[i];
        savesym(sym);
    }
    args.clear(); /* ownership transferred to the function scope */

    struct value_t *body_ret = eval(def->ast);
    struct value_t *result = topscope()->ret;
    topscope()->ret = NULL;

    if (body_ret) {
        if (body_ret->type == T_break)
            yyerror("break outside loop in function %s", f->name);
        else if (body_ret->type == T_continue)
            yyerror("continue outside loop in function %s", f->name);
        freeval(body_ret);
    }

    popscope();
    return result;
}

struct value_t *eval(struct ast *a) {
    if (!a) return NULL;
    struct value_t *ret = NULL;

    switch (a->nodetype) {
        case 'V':
            ret = dupval(((struct val *)a)->val);
            break;

        case 'N': {
            struct symbol *s = lookupsym(((struct symref *)a)->name);
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
            struct symlist *sl = ((struct funcdef *)a)->sl;
            bool bad = false;
            for (struct symlist *p = sl; p && !bad; p = p->next)
                for (struct symlist *q = p->next; q; q = q->next)
                    if (!std::strcmp(p->sym->name, q->sym->name)) {
                        yyerror("duplicate parameter %s in function %s",
                                p->sym->name, name);
                        bad = true;
                        break;
                    }
            if (bad)
                break;
            if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else {
                struct symbol *sym = newsym(name);
                sym->val = newfuncval(((struct funcdef *)a)->ast, sl);
                ((struct funcdef *)a)->moved = 1;
                savesym(sym);
            }
            break;
        }

        case 'R': {
            struct ast *value_ast = ((struct fnret *)a)->v;
            struct value_t *v = eval(value_ast);
            if (value_ast && !v)
                break;
            if (!retval(v)) {
                yyerror("return outside function");
                freeval(v);
                break;
            }
            ret = newreturn();
            break;
        }

        case 'B':
            ret = newbreak();
            break;

        case 'O':
            ret = newcontinue();
            break;

        case 'X': {
            pushscope(NULL);
            ret = eval(((struct nscope *)a)->ast);
            popscope();
            break;
        }

        case '=': {
            struct symasgn *asgn = (struct symasgn *)a;
            struct symbol *s = lookupsym(asgn->name);
            if (!s) {
                yyerror("symbol not found %s", asgn->name);
            } else {
                struct value_t *v = eval(asgn->v);
                if (v) {
                    freeval(s->val);
                    s->val = v;
                    ret = dupval(s->val);
                }
            }
            break;
        }

        case 'l': {
            struct list_node *p = (struct list_node *)a;
            struct list_value_t *v = newlistval();
            while (p != NULL) {
                if (p->value) {
                    struct value_t *item = eval(p->value);
                    if (!item) {
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    v->value.push_back(item);
                }
                p = (struct list_node *)p->next;
            }
            ret = (struct value_t *)v;
            break;
        }

        case 'd': {
            struct dict_node *p = (struct dict_node *)a;
            struct dict_value_t *v = newdictval();
            while (p != NULL) {
                if (p->key && p->value) {
                    struct value_t *key = eval(p->key);
                    struct value_t *val = eval(p->value);
                    if (!key || !val) {
                        freeval(key);
                        freeval(val);
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    v->value.push_back(std::make_pair(key, val));
                }
                p = (struct dict_node *)p->next;
            }
            ret = (struct value_t *)v;
            break;
        }

        case 'M': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            if (v->type == T_int) {
                long long n = ((struct int_value_t *)v)->value;
                ret = new_int_or_float(-n);
            } else if (v->type == T_float) {
                ret = newfloatval(-((struct float_value_t *)v)->value);
            } else {
                yyerror("cannot negate %s", typename_of(v));
            }
            freeval(v);
            break;
        }

        case '+': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_or_float(x + y);
            } else if (is_numeric(l) && is_numeric(r)) {
                ret = newfloatval(as_double(l) + as_double(r));
            } else if (l->type == T_string && r->type == T_string) {
                std::string s = ((struct string_value_t *)l)->value;
                s += ((struct string_value_t *)r)->value;
                ret = newstrval(s.c_str());
            } else if (l->type == T_list && r->type == T_list) {
                struct list_value_t *lv = newlistval();
                struct list_value_t *ll = (struct list_value_t *)l;
                struct list_value_t *rl = (struct list_value_t *)r;
                lv->value.reserve(ll->value.size() + rl->value.size());
                for (size_t i = 0; i < ll->value.size(); ++i)
                    lv->value.push_back(dupval(ll->value[i]));
                for (size_t i = 0; i < rl->value.size(); ++i)
                    lv->value.push_back(dupval(rl->value[i]));
                ret = (struct value_t *)lv;
            } else {
                yyerror("cannot add %s and %s", typename_of(l), typename_of(r));
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '-':
        case '*': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (!is_numeric(l) || !is_numeric(r)) {
                yyerror("cannot use %c on %s and %s", a->nodetype,
                        typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_or_float(a->nodetype == '-' ? x - y : x * y);
            } else {
                double x = as_double(l), y = as_double(r);
                ret = newfloatval(a->nodetype == '-' ? x - y : x * y);
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '/': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (!l || !r) {
                freeval(l);
                freeval(r);
                break;
            }
            if (!is_numeric(l) || !is_numeric(r)) {
                yyerror("cannot divide %s by %s", typename_of(l), typename_of(r));
                freeval(l);
                freeval(r);
                break;
            }
            if (as_double(r) == 0.0) {
                yyerror("division by zero");
                freeval(l);
                freeval(r);
                break;
            }
            if (l->type == T_int && r->type == T_int) {
                long long x = ((struct int_value_t *)l)->value;
                long long y = ((struct int_value_t *)r)->value;
                ret = new_int_or_float(x / y);
            } else {
                ret = newfloatval(as_double(l) / as_double(r));
            }
            freeval(l);
            freeval(r);
            break;
        }

        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6': {
            struct value_t *l = eval(a->l);
            struct value_t *r = eval(a->r);
            if (l && r)
                ret = do_compare(a->nodetype - '0', l, r);
            freeval(l);
            freeval(r);
            break;
        }

        case 'I': {
            struct value_t *cond = eval(((struct flow *)a)->cond);
            if (!cond) break;
            bool b = truthy(cond);
            freeval(cond);
            if (b) {
                if (((struct flow *)a)->tl)
                    ret = eval(((struct flow *)a)->tl);
            } else {
                if (((struct flow *)a)->el)
                    ret = eval(((struct flow *)a)->el);
            }
            break;
        }

        case 'W': {
            struct ast *cond_ast = ((struct flow *)a)->cond;
            struct ast *body = ((struct flow *)a)->tl;
            if (!body) break;
            for (;;) {
                struct value_t *cond = eval(cond_ast);
                if (!cond) break;
                bool b = truthy(cond);
                freeval(cond);
                if (!b) break;

                ret = eval(body);
                if (ret && ret->type == T_return)
                    break;
                if (ret && ret->type == T_break) {
                    freeval(ret);
                    ret = NULL;
                    break;
                }
                if (ret && ret->type == T_continue) {
                    freeval(ret);
                    ret = NULL;
                    continue;
                }
                freeval(ret);
                ret = NULL;
            }
            break;
        }

        case 'P': {
            struct forloop *f = (struct forloop *)a;
            pushscope(NULL);
            bool leave = false;

            if (f->init) {
                struct value_t *v = eval(f->init);
                if (v) {
                    if (v->type == T_return) {
                        ret = v;
                        leave = true;
                    } else {
                        if (v->type == T_break || v->type == T_continue)
                            yyerror("break/continue outside loop");
                        freeval(v);
                    }
                }
            }

            for (; !leave;) {
                bool cond_true = true;
                if (f->cond) {
                    struct value_t *cond = eval(f->cond);
                    if (!cond) break;
                    cond_true = truthy(cond);
                    freeval(cond);
                }
                if (!cond_true) break;

                bool do_break = false;
                struct value_t *body = eval(f->body);
                if (body) {
                    if (body->type == T_return) {
                        ret = body;
                        leave = true;
                    } else if (body->type == T_break) {
                        freeval(body);
                        do_break = true;
                    } else {
                        /* normal value and T_continue both run the step */
                        freeval(body);
                    }
                }
                if (leave || do_break) break;

                if (f->step) {
                    struct value_t *step = eval(f->step);
                    if (step) {
                        if (step->type == T_return) {
                            ret = step;
                            leave = true;
                        } else {
                            if (step->type == T_break || step->type == T_continue)
                                yyerror("break/continue outside loop");
                            freeval(step);
                        }
                    }
                }
            }

            popscope();
            break;
        }

        case 'L': {
            ret = eval(a->l);
            if (ret && (ret->type == T_return ||
                        ret->type == T_break ||
                        ret->type == T_continue))
                break;
            freeval(ret);
            ret = eval(a->r);
            break;
        }

        case 'G': {
            struct value_t *container = eval(a->l);
            struct value_t *index = eval(a->r);
            if (!container || !index) {
                freeval(container);
                freeval(index);
                break;
            }
            if (container->type == T_list) {
                if (index->type != T_int) {
                    yyerror("list index must be an integer, got %s",
                            typename_of(index));
                } else {
                    int i = ((struct int_value_t *)index)->value;
                    struct list_value_t *lv = (struct list_value_t *)container;
                    if (i < 0 || (size_t)i >= lv->value.size())
                        yyerror("list index %d out of range", i);
                    else
                        ret = dupval(lv->value[i]);
                }
            } else if (container->type == T_string) {
                if (index->type != T_int) {
                    yyerror("string index must be an integer, got %s",
                            typename_of(index));
                } else {
                    int i = ((struct int_value_t *)index)->value;
                    const char *s = ((struct string_value_t *)container)->value;
                    int len = (int)std::strlen(s);
                    if (i < 0 || i >= len) {
                        yyerror("string index %d out of range", i);
                    } else {
                        char buf[2] = { s[i], '\0' };
                        ret = newstrval(buf);
                    }
                }
            } else if (container->type == T_dict) {
                struct dict_value_t *dv = (struct dict_value_t *)container;
                bool found = false;
                for (size_t i = 0; i < dv->value.size(); ++i) {
                    if (value_equals(dv->value[i].first, index)) {
                        ret = dupval(dv->value[i].second);
                        found = true;
                        break;
                    }
                }
                if (!found)
                    yyerror("key not found");
            } else {
                yyerror("cannot index %s", typename_of(container));
            }
            freeval(container);
            freeval(index);
            break;
        }

        case 'F':
            ret = callbuildin((struct fncall *)a);
            break;

        case 'C':
            ret = calluser((struct ufncall *)a);
            break;

        default:
            std::fprintf(stderr, "internal error: bad node %c\n", a->nodetype);
            break;
    }
    return ret;
}
