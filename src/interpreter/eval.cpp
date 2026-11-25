#include "eval.h"
#include "control_runtime.h"
#include "diagnostics.h"
#include "dict.h"
#include "eval_context.h"
#include "eval_helpers.h"
#include "fiber.h"
#include "format.h"
#include "package.h"
#include "pattern.h"
#include "refvalue.h"
#include "cycle.h"
#include "platform.h"
#include "repl.h"
#include "runtime_context.h"
#include "scopestack.h"
#include "structeval.h"
#include "structtype.h"
#include "symbol.h"
#include "valuetype.h"
#include "unicode.h"

#include <cstring>
#include <unordered_set>

#include "eval_internal.h"

static const char *literal_field_name(const struct ast *node) {
    if (!node || node->nodetype != 'V')
        return NULL;
    const struct val *value = (const struct val *)node;
    if (!value->val || value->val->type != T_string)
        return NULL;
    return ((const struct string_value_t *)value->val)->value;
}

static struct value_t *eval_optional_chain(struct optional_chain_node *chain) {
    if (!chain)
        return NULL;
    struct value_t *base = eval(chain->l);
    if (!base)
        return NULL;
    if (is_weak_value(base)) {
        struct value_t *strong = ref_upgrade(base);
        freeval(base);
        base = strong;
        if (!base)
            return NULL;
        if (base->type == T_null)
            return base;
    } else if (base->type == T_null) {
        return base;
    }
    const char *name = literal_field_name(chain->r);
    if (!name)
        return NULL;
    struct string_value_t key;
    key.type = T_string;
    key.value = const_cast<char *>(name);
    struct value_t *result = read_member_value(base, (struct value_t *)&key, true);
    freeval(base);
    return result;
}

namespace {

/* Release a temporary even when a native call throws through eval. */
struct value_release_guard {
    struct value_t *value;
    explicit value_release_guard(struct value_t *v) : value(v) {}
    ~value_release_guard() {
        if (value)
            freeval(value);
    }
};

} // namespace

static bool item_declares_local(const struct ast *item) {
    if (!item)
        return false;
    if (item->nodetype == 'g')
        return item_declares_local(((const struct label_node *)item)->stmt);
    return item->nodetype == 'D' || item->nodetype == NODE_DESTRUCTURE;
}

static value_t *eval_statement_list(struct ast *body) {
    std::vector<struct ast *> items;
    for (struct ast *item = body; item;) {
        if (item->nodetype == 'L') {
            items.push_back(item->l);
            item = item->r;
        } else {
            items.push_back(item);
            break;
        }
    }
    if (items.empty() && body)
        items.push_back(body);

    size_t pc = 0;
    struct value_t *last = NULL;
    while (pc < items.size()) {
        const int errors_before = eval_ctx().runtime_error_count;
        struct value_t *value = eval(items[pc++]);
        if (eval_ctx().control != EVAL_CONTROL_NONE) {
            freeval(last);
            return value;
        }
        if (eval_ctx().runtime_error_count != errors_before) {
            freeval(last);
            freeval(value);
            return NULL;
        }
        if (value && value->type == T_goto) {
            const char *name = ((struct control_value_t *)value)->label;
            size_t target = items.size();
            for (size_t i = 0; i < items.size(); ++i) {
                struct ast *item = items[i];
                if (item->nodetype == 'g') {
                    struct label_node *label = (struct label_node *)item;
                    if (name && label->name &&
                        std::strcmp(name, label->name) == 0) {
                        target = i;
                        break;
                    }
                }
            }
            if (target != items.size()) {
                size_t source = pc - 1;
                bool crosses = false;
                if (target > source) {
                    for (size_t i = source + 1; i < target; ++i) {
                        if (item_declares_local(items[i])) {
                            crosses = true;
                            break;
                        }
                    }
                } else {
                    for (size_t i = target; i < source; ++i) {
                        if (item_declares_local(items[i])) {
                            crosses = true;
                            break;
                        }
                    }
                }
                if (crosses) {
                    diag_set_location(items[source]->line,
                                      items[source]->column);
                    yyerror("goto cannot cross a let or const declaration");
                    freeval(last);
                    freeval(value);
                    return NULL;
                }
                freeval(last);
                last = NULL;
                freeval(value);
                pc = target;
                continue;
            }
            freeval(last);
            return value;
        }
        if (value && (value->type == T_return || value->type == T_break ||
                      value->type == T_continue ||
                      value->type == T_goto)) {
            freeval(last);
            return value;
        }
        freeval(last);
        last = value;
    }
    return last;
}

static value_t *eval_body(struct ast *body) {
    return eval_statement_list(body);
}

struct value_t *eval(struct ast *a) {
    if (!a) return NULL;
    if (g_interrupted) return NULL;
    eval_location_guard location_guard(a->line, a->column);
    struct value_t *ret = NULL;

    switch (a->nodetype) {
        case 'V':
            ret = dupval(((struct val *)a)->val);
            break;

        case 'N': {
            struct symbol *s = lookupsym(((struct symref *)a)->name);
            if (!s) {
                yyerror("symbol not found %s", ((struct symref *)a)->name);
            } else if (s->is_uninitialized) {
                yyerror("parameter %s is used before it is initialized",
                        s->name);
            } else {
                ret = dupval(s->val);
            }
            break;
        }

        case 'J': {
            struct import_node *imp = (struct import_node *)a;
            std::string error;
            std::string imported_package;
            std::string display_path = imp->path ? imp->path : "";
            if (!display_path.empty() && display_path[0] == '\x02')
                display_path.erase(0, 1);
            int errors_before = diag_error_count();
            struct value_t *ns = package_import(
                imp->path, imp->base, &imported_package, error);
            if (!ns) {
                if (diag_error_count() == errors_before)
                    yyerror("import %s: %s", display_path.c_str(),
                            error.c_str());
                else
                    diag_mark_script_failure();
                break;
            }
            const char *name = imp->alias
                                   ? imp->alias
                                   : import_binding_name(imp->path);
            if (isrepeated(name)) {
                struct symbol *existing = lookupsym(name);
                bool same_namespace =
                    existing && existing->val && ns &&
                    existing->val->type == T_dict && ns->type == T_dict &&
                    ((struct dict_value_t *)existing->val)->payload ==
                        ((struct dict_value_t *)ns)->payload;
                if (!same_namespace) {
                    yyerror("import name already exists: %s", name);
                }
                freeval(ns);
            } else {
                struct symbol *sym = newsym(name);
                sym->val = ns;
                savesym(sym);
                const char *path = imp->path;
                if (path && *path && path[0] != '.' && path[0] != '@') {
                    const char *alias = imp->alias;
                    if (!alias) {
                        const char *dot = std::strrchr(path, '.');
                        alias = dot ? dot + 1 : path;
                    }
                    structtype_register_alias(
                        diag_current_source_name().c_str(), alias,
                        imported_package.empty() ? path
                                                 : imported_package.c_str());
                }
            }
            break;
        }

        case NODE_INCLUDE: {
            struct include_node *inc = (struct include_node *)a;
            if (diag_call_depth() > 0) {
                yyerror("include is only allowed at declaration level");
                break;
            }
            std::string include_error;
            if (!evaluate_include_source(inc->path, include_error)) {
                yyerror("include %s: %s", inc->path,
                        include_error.c_str());
            }
            break;
        }

        case 'K': {
            struct package_node *pkg = (struct package_node *)a;
            int errors_before = eval_ctx().runtime_error_count;
            std::string error;

            if (package_is_loading())
                eval_pushscope();
            else
                eval_push_isolated_scope();

            std::string saved_package_name = diag_current_package_name();
            std::string package_name =
                (saved_package_name.empty() || saved_package_name == "main")
                    ? pkg->name
                    : saved_package_name;
            diag_set_package_name(package_name);
            struct value_t *body_ret = eval(pkg->body);
            if (body_ret &&
                (body_ret->type == T_return || body_ret->type == T_break ||
                 body_ret->type == T_continue || body_ret->type == T_goto)) {
                yyerror("invalid control flow in package %s", pkg->name);
            }
            value_t *ns = NULL;
            if (eval_ctx().runtime_error_count == errors_before)
                ns = package_build_namespace(current_scope().get(), error);
            eval_popscope();
            diag_set_package_name(saved_package_name);

            freeval(body_ret);

            if (!ns) {
                if (eval_ctx().runtime_error_count == errors_before)
                    yyerror("package %s: %s", pkg->name,
                            error.empty() ? "cannot build namespace"
                                          : error.c_str());
                break;
            }

            if (!package_publish(pkg->name, ns, error)) {
                yyerror("package %s: %s", pkg->name, error.c_str());
                freeval(ns);
            }
            break;
        }

        case NODE_DESTRUCTURE: {
            struct destructure_node *d = (struct destructure_node *)a;
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *value = eval(d->value);
            if (!value ||
                eval_ctx().runtime_error_count != errors_before) {
                freeval(value);
                break;
            }

            std::vector<pattern_binding> bindings;
            if (!match_pattern(d->pattern, value, bindings)) {
                free_pattern_bindings(bindings);
                if (eval_ctx().runtime_error_count == errors_before)
                    yyerror("destructuring pattern did not match %s",
                            typename_of(value));
                freeval(value);
                break;
            }

            bool ok = true;
            for (auto &binding : bindings) {
                if (isrepeated(binding.name.c_str())) {
                    yyerror("symbol repeated %s", binding.name.c_str());
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                free_pattern_bindings(bindings);
                freeval(value);
                break;
            }

            for (auto &binding : bindings) {
                struct symbol *symbol = newsym(binding.name.c_str());
                symbol->val = binding.value;
                binding.value = NULL;
                symbol->is_const = d->is_const != 0;
                savesym(symbol);
            }
            bindings.clear();
            freeval(value);
            break;
        }

        case 'D': {
            struct symdef *def = (struct symdef *)a;
            char *name = def->name;
            if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else if (!validate_type_constraint(def->type)) {
                break;
            } else {
                struct value_t *v = def->v
                    ? eval(def->v)
                    : default_value_for_type(def->type);
                if (!v) {
                    if (!def->v)
                        yyerror("variable %s of type %s requires an initializer",
                                name, type_constraint_name(def->type));
                    break;
                }
                if (v->type == T_return || v->type == T_goto ||
                    v->type == T_break || v->type == T_continue) {
                    ret = v;
                    break;
                }
                v = coerce_store_value(def->type, NULL, v);
                if (!v)
                    break;
                if (!type_constraint_matches(def->type, v)) {
                    yyerror("variable %s expects %s, got %s", name,
                            type_constraint_name(def->type), typename_of(v));
                    freeval(v);
                    break;
                }
                struct symbol *s = newsym(name);
                s->val = v;
                if (def->type)
                    type_constraint_assign(&s->type, def->type);
                else
                    type_constraint_set_from_ref(&s->type, v);
                s->exported = def->exported;
                s->is_const = def->is_const;
                savesym(s);
            }
            break;
        }

        case 'Y':
            eval_struct_decl((struct struct_node *)a);
            break;

        case NODE_ENUM_DECL:
            eval_enum_decl((struct enum_node *)a);
            break;

        case 'H':
            eval_impl_decl((struct impl_node *)a);
            break;

        case 't':
            eval_protocol_decl((struct protocol_node *)a);
            break;

        case 'y':
            ret = eval_struct_lit((struct struct_lit_node *)a);
            break;

        case NODE_OPTIONAL_CHAIN:
            ret = eval_optional_chain((struct optional_chain_node *)a);
            break;

        case NODE_REF_TARGET: {
            struct value_t *base = eval(a->l);
            if (!base)
                break;
            ret = ref_snapshot(base);
            freeval(base);
            break;
        }

        case NODE_ENUM_LITERAL:
            ret = eval_enum_lit((struct enum_lit_node *)a);
            break;

        case 'Z': {
            struct funcdef *f = (struct funcdef *)a;
            char *name = f->name;
            struct symlist *sl = f->sl;
            if (!validate_function_params(name, sl))
                break;

            std::shared_ptr<scope_t> closure = current_scope();
            /* Top-level functions resolve globals through the fallback parent. */
            if (closure && closure->is_global)
                closure.reset();

            if (!name) {
                std::shared_ptr<function_code> code = get_function_code(f);
                f->moved = 1;
                ret = newfuncval(code, closure);
            } else if (isrepeated(name)) {
                yyerror("symbol repeated %s", name);
            } else {
                std::shared_ptr<function_code> code = get_function_code(f);
                struct symbol *sym = newsym(name);
                sym->val = newfuncval(code, closure);
                sym->exported = f->exported;
                f->moved = 1;
                savesym(sym);
            }
            break;
        }

        case 'R': {
            struct ast *value_ast = ((struct fnret *)a)->v;
            struct value_t *v = eval(value_ast);
            if (value_ast && !v)
                break;
            if (v && (v->type == T_return || v->type == T_goto ||
                      v->type == T_break || v->type == T_continue)) {
                ret = v;
                break;
            }
            if (!v)
                v = newnull();
            if (!retval(v)) {
                yyerror("return outside function");
                freeval(v);
                break;
            }
            ret = newreturn();
            break;
        }

        case 'B':
            diag_set_flow_location(a->line, a->column);
            ret = newbreak(((struct nbreak *)a)->label);
            break;

        case 'O':
            diag_set_flow_location(a->line, a->column);
            ret = newcontinue(((struct ncontinue *)a)->label);
            break;

        case NODE_FUNCTION_BODY:
            ret = eval_body(a->l);
            break;

        case NODE_GOTO:
            ret = newgotoval(((struct goto_node *)a)->name);
            break;

        case 'g': {
            struct label_node *node = (struct label_node *)a;
            const char *saved = eval_ctx().pending_label;
            if (label_directly_applies(node->stmt))
                eval_ctx().pending_label = node->name;
            ret = eval(node->stmt);
            eval_ctx().pending_label = saved;
            break;
        }

        case 'u': {
            struct control_stmt *stmt = (struct control_stmt *)a;
            struct value_t *value =
                stmt->value ? eval(stmt->value) : newnull();
            if (!value)
                break;
            if (eval_ctx().control != EVAL_CONTROL_NONE) {
                freeval(value);
                break;
            }
            struct value_t *error = normalize_error(value, "fail");
            eval_annotate_error(error);
            set_eval_control(EVAL_CONTROL_FAIL, error);
            break;
        }

        case 'v': {
            if (eval_ctx().handler_depth <= 0) {
                yyerror("recover outside error handler");
                break;
            }
            struct control_stmt *stmt = (struct control_stmt *)a;
            struct value_t *value =
                stmt->value ? eval(stmt->value) : newnull();
            if (!value)
                break;
            if (eval_ctx().control != EVAL_CONTROL_NONE) {
                freeval(value);
                break;
            }
            set_eval_control(EVAL_CONTROL_RECOVER, value);
            break;
        }

        case 'p': {
            if (eval_ctx().handler_depth <= 0) {
                yyerror("pass outside error handler");
                break;
            }
            struct control_stmt *stmt = (struct control_stmt *)a;
            struct value_t *value = eval(stmt->value);
            if (!value)
                break;
            if (eval_ctx().control != EVAL_CONTROL_NONE) {
                freeval(value);
                break;
            }
            struct value_t *error = normalize_error(value, "fail");
            eval_annotate_error(error);
            set_eval_control(EVAL_CONTROL_FAIL, error);
            break;
        }

        case 'o': {
            struct on_error_node *handler = (struct on_error_node *)a;
            eval_capture_snapshot snapshot;
            eval_capture_begin(snapshot);
            struct value_t *value = eval(handler->call);
            struct eval_capture_result captured = eval_capture_end(snapshot);
            if (g_interrupted) {
                freeval(value);
                freeval(captured.error);
                break;
            }
            struct value_t *error_value = captured.error;
            if (captured.failed) {
                freeval(value);
                value = NULL;
            } else if (value && value->type == T_result) {
                struct result_value_t *result =
                    (struct result_value_t *)value;
                if (result->ok) {
                    ret = dupval(result->value);
                    freeval(value);
                    freeval(error_value);
                    break;
                }
                freeval(error_value);
                error_value = dupval(result->error);
                freeval(value);
                value = NULL;
            }
            if (value) {
                freeval(error_value);
                ret = value;
                break;
            }

            if (!error_value) {
                if (eval_ctx().control != EVAL_CONTROL_FAIL)
                    break;
                error_value = take_eval_control(EVAL_CONTROL_FAIL);
            }
            error_value = normalize_error(error_value, "fail");
            eval_pushscope();
            struct symbol *error_sym = newsym(handler->name);
            error_sym->val = error_value;
            error_sym->is_const = true;
            savesym(error_sym);

            int handler_errors = eval_ctx().runtime_error_count;
            int saved_handler_depth = eval_ctx().handler_depth;
            eval_ctx().handler_depth = saved_handler_depth + 1;
            struct value_t *body = eval(handler->body);
            eval_ctx().handler_depth = saved_handler_depth;
            eval_popscope();

            if (eval_ctx().control == EVAL_CONTROL_RECOVER) {
                ret = take_eval_control(EVAL_CONTROL_RECOVER);
                freeval(body);
                break;
            }
            if (eval_ctx().control == EVAL_CONTROL_FAIL) {
                freeval(body);
                break;
            }
            if (body && (body->type == T_return || body->type == T_break ||
                         body->type == T_continue ||
                         body->type == T_goto)) {
                ret = body;
                break;
            }
            if (eval_ctx().runtime_error_count != handler_errors) {
                freeval(body);
                break;
            }
            freeval(body);
            yyerror("error handler must end with recover, pass, or fail");
            break;
        }

        case 'X': {
            int errors_before = eval_ctx().runtime_error_count;
            eval_pushscope();
            ret = eval_body(((struct nscope *)a)->ast);
            eval_popscope();
            /*
             * A block used as an expression yields null when its last
             * statement is a declaration, or when it is empty.  The VM
             * already pushes null for those statements.
             */
            if (!ret && eval_ctx().runtime_error_count == errors_before &&
                eval_ctx().control == EVAL_CONTROL_NONE)
                ret = newnull();
            break;
        }

        case '=': {
            struct symasgn *asgn = (struct symasgn *)a;
            if (asgn->target->nodetype == 'N') {
                struct symref *target = (struct symref *)asgn->target;
                if (asgn->v && asgn->v->nodetype == '+' && asgn->v->l &&
                    asgn->v->l->nodetype == 'N' && asgn->v->r &&
                    asgn->v->r->nodetype == 'V') {
                    struct val *literal = (struct val *)asgn->v->r;
                    struct symref *base =
                        (struct symref *)asgn->v->l;
                    if (literal->val &&
                        (literal->val->type == T_string ||
                         literal->val->type == T_char) &&
                        base->name && target->name &&
                        std::strcmp(base->name, target->name) == 0) {
                        ret = string_append_name(target, asgn->v->r);
                        break;
                    }
                }
                ret = assign_name(target, asgn->v);
            }
            else if (asgn->target->nodetype == 'G' ||
                     asgn->target->nodetype == 'j')
                ret = assign_index(asgn->target, asgn->v);
            else if (asgn->target->nodetype == NODE_REF_TARGET)
                ret = assign_ref_target(asgn->target, asgn->v);
            else
                yyerror("cannot assign to this expression");
            break;
        }

        case 'a': {
            struct compound_asgn *asgn = (struct compound_asgn *)a;
            if (asgn->target->nodetype == 'N')
                ret = compound_name((struct symref *)asgn->target, asgn->op,
                                    asgn->v);
            else if (asgn->target->nodetype == 'G' ||
                     asgn->target->nodetype == 'j')
                ret = compound_index(asgn->target, asgn->op, asgn->v);
            else
                yyerror("cannot assign to this expression");
            break;
        }

        case 'U': {
            struct incdec_node *u = (struct incdec_node *)a;
            if (u->target->nodetype == 'N')
                ret = increment_name((struct symref *)u->target, u->delta,
                                     u->prefix);
            else if (u->target->nodetype == 'G' ||
                     u->target->nodetype == 'j')
                ret = increment_index(u->target, u->delta, u->prefix);
            else
                yyerror("cannot %s this expression",
                        u->delta > 0 ? "increment" : "decrement");
            break;
        }

        case 'l':
        case NODE_TUPLE: {
            struct list_node *p = (struct list_node *)a;
            struct list_value_t *v =
                a->nodetype == NODE_TUPLE ? newtupleval() : newlistval();
            while (p != NULL) {
                if (p->value) {
                    struct value_t *item = eval(p->value);
                    if (!item) {
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    v->payload->items.push_back(item);
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
                    if (!key) {
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    struct value_t *val = eval(p->value);
                    if (!val) {
                        freeval(key);
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    if (!dict_key_allowed(key)) {
                        yyerror("%s, got %s", dict_key_error(),
                                typename_of(key));
                        freeval(key);
                        freeval(val);
                        freeval((struct value_t *)v);
                        return NULL;
                    }
                    int found = dict_find_index(v, key);
                    if (found >= 0) {
                        freeval(v->payload->items[(size_t)found].second);
                        v->payload->items[(size_t)found].second = val;
                        freeval(key);
                    } else {
                        dict_append_item(v, key, val);
                    }
                }
                p = (struct dict_node *)p->next;
            }
            ret = (struct value_t *)v;
            break;
        }

        case 'w': {
            struct value_t *v = a->l ? eval(a->l) : newnull();
            if (!v)
                break;
            ret = fiber_yield_value(v);
            break;
        }

        case 'n': {
            eval_context &context = eval_ctx();
            if (context.defer_stack.size() <= 1) {
                yyerror("defer is only allowed inside a function or block");
                break;
            }
            context.defer_stack.back().push_back(a->l);
            ret = newnull();
            break;
        }

        case 'M': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            if (v->type == T_int) {
                long long n = ((struct int_value_t *)v)->value;
                ret = new_int_checked(-(__int128)n);
            } else if (v->type == T_float) {
                ret = newfloatval(-((struct float_value_t *)v)->value);
            } else {
                yyerror("cannot negate %s", typename_of(v));
            }
            freeval(v);
            break;
        }

        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '&':
        case '|':
        case '^':
        case 's':
        case 'r': {
            struct value_t *l = eval(a->l);
            if (!l)
                break;
            struct value_t *r = eval(a->r);
            if (!r) {
                freeval(l);
                break;
            }
            ret = apply_binary_value(a->nodetype, l, r);
            freeval(l);
            freeval(r);
            break;
        }

        case 'A': {
            struct value_t *l = eval(a->l);
            if (!l) break;
            bool left = truthy(l);
            freeval(l);
            if (!left) {
                ret = newboolval(false);
                break;
            }
            struct value_t *r = eval(a->r);
            if (!r) break;
            ret = newboolval(truthy(r));
            freeval(r);
            break;
        }

        case 'Q': {
            struct value_t *l = eval(a->l);
            if (!l) break;
            bool left = truthy(l);
            freeval(l);
            if (left) {
                ret = newboolval(true);
                break;
            }
            struct value_t *r = eval(a->r);
            if (!r) break;
            ret = newboolval(truthy(r));
            freeval(r);
            break;
        }

        case '!': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            ret = newboolval(!truthy(v));
            freeval(v);
            break;
        }

        case '~': {
            struct value_t *v = eval(a->l);
            if (!v) break;
            if (v->type != T_int) {
                yyerror("bitwise not expects an int, got %s", typename_of(v));
            } else {
                ret = newintval(~((struct int_value_t *)v)->value);
            }
            freeval(v);
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

        case NODE_MATCH: {
            struct match_node *m = (struct match_node *)a;
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *subject = eval(m->expr);
            if (!subject ||
                eval_ctx().runtime_error_count != errors_before) {
                freeval(subject);
                break;
            }

            if (!match_exhaustive_check(m->arms, subject)) {
                freeval(subject);
                break;
            }

            bool matched = false;
            bool aborted = false;

            for (struct ast *arm_ast = m->arms; arm_ast;
                 arm_ast = ((struct match_arm *)arm_ast)->next) {
                struct match_arm *arm = (struct match_arm *)arm_ast;
                std::vector<pattern_binding> bindings;

                if (arm->pattern &&
                    !match_pattern(arm->pattern, subject, bindings)) {
                    free_pattern_bindings(bindings);
                    if (eval_ctx().runtime_error_count != errors_before) {
                        aborted = true;
                        break;
                    }
                    continue;
                }

                eval_pushscope();
                for (auto &binding : bindings) {
                    struct symbol *symbol = newsym(binding.name.c_str());
                    symbol->val = binding.value;
                    binding.value = NULL;
                    symbol->is_const = true;
                    savesym(symbol);
                }
                bindings.clear();

                bool guard_ok = true;
                if (arm->guard) {
                    struct value_t *guard = eval(arm->guard);
                    if (!guard) {
                        eval_popscope();
                        aborted = true;
                        break;
                    }
                    guard_ok = truthy(guard);
                    freeval(guard);
                }
                if (!guard_ok) {
                    eval_popscope();
                    continue;
                }

                int body_errors = eval_ctx().runtime_error_count;
                struct value_t *body_ret = eval(arm->body);
                eval_popscope();
                if (eval_ctx().runtime_error_count != body_errors) {
                    freeval(body_ret);
                    aborted = true;
                    break;
                }

                matched = true;
                ret = body_ret;
                break;
            }

            if (!matched && !aborted &&
                eval_ctx().runtime_error_count == errors_before) {
                yyerror("match: no arm matched");
            }
            freeval(subject);
            break;
        }

        case 'S': {
            struct switch_node *sw = (struct switch_node *)a;
            const char *switch_label = take_pending_label();
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *subject = eval(sw->expr);
            if (!subject || eval_ctx().runtime_error_count != errors_before) {
                freeval(subject);
                break;
            }
            if (!switch_value_supported(subject)) {
                yyerror("switch does not support %s", typename_of(subject));
                freeval(subject);
                break;
            }

            auto run_body = [&](struct ast *body) {
                eval_pushscope();
                int body_errors = eval_ctx().runtime_error_count;
                struct value_t *body_ret = eval(body);
                if (eval_ctx().runtime_error_count != body_errors) {
                    freeval(body_ret);
                } else if (body_ret) {
                    if (body_ret->type == T_break) {
                        if (control_matches_label(body_ret, switch_label))
                            freeval(body_ret);
                        else
                            ret = body_ret;
                    } else if (body_ret->type == T_continue ||
                               body_ret->type == T_return ||
                               body_ret->type == T_goto) {
                        ret = body_ret;
                    } else {
                        freeval(body_ret);
                    }
                }
                eval_popscope();
            };

            struct case_node *default_case = NULL;
            bool matched = false;
            bool stop = false;

            for (struct case_node *c = (struct case_node *)sw->cases;
                 c && !stop; c = (struct case_node *)c->next) {
                if (!c->values) {
                    default_case = c;
                    continue;
                }
                if (case_values_have_condition(c->values)) {
                    yyerror("logical or comparison expressions are not allowed "
                            "in case values");
                    stop = true;
                    break;
                }

                errors_before = eval_ctx().runtime_error_count;
                struct value_t *values = eval(c->values);
                if (!values || eval_ctx().runtime_error_count != errors_before) {
                    freeval(values);
                    stop = true;
                    break;
                }
                if (values->type != T_list) {
                    yyerror("switch case values must be a list");
                    freeval(values);
                    stop = true;
                    break;
                }

                struct list_value_t *list = (struct list_value_t *)values;
                for (size_t i = 0; i < list->payload->items.size(); ++i) {
                    if (!switch_value_supported(list->payload->items[i])) {
                        yyerror("switch does not support %s case value",
                                typename_of(list->payload->items[i]));
                        stop = true;
                        break;
                    }
                }
                if (!stop) {
                    for (size_t i = 0; i < list->payload->items.size(); ++i) {
                        if (value_equals(subject, list->payload->items[i])) {
                            matched = true;
                            break;
                        }
                    }
                }
                freeval(values);

                if (stop) break;
                if (matched) {
                    run_body(c->body);
                    stop = true;
                    break;
                }
            }

            if (!stop && !matched && default_case)
                run_body(default_case->body);

            freeval(subject);
            break;
        }

        case 'T': {
            struct flow *f = (struct flow *)a;
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *cond = eval(f->cond);
            if (eval_ctx().runtime_error_count != errors_before) {
                freeval(cond);
                break;
            }
            if (!cond) break;
            bool b = truthy(cond);
            freeval(cond);
            ret = eval(b ? f->tl : f->el);
            break;
        }

        case 'I': {
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *cond = eval(((struct flow *)a)->cond);
            if (eval_ctx().runtime_error_count != errors_before) {
                freeval(cond);
                break;
            }
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

        case 'E': {
            struct flow *f = (struct flow *)a;
            const char *loop_label = take_pending_label();
            if (!f->cond || !f->tl) break;

            for (;;) {
                if (g_interrupted) break;

                int errors_before = eval_ctx().runtime_error_count;
                struct value_t *body = eval(f->tl);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(body);
                    break;
                }
                if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(body);
                    break;
                }

                bool do_break = false;
                if (body) {
                    if (body->type == T_return || body->type == T_goto) {
                        ret = body;
                        break;
                    }
                    if (body->type == T_break) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                            do_break = true;
                        } else {
                            ret = body;
                            break;
                        }
                    } else if (body->type == T_continue) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                        } else {
                            ret = body;
                            break;
                        }
                    } else {
                        freeval(body);
                    }
                }
                if (do_break) break;

                errors_before = eval_ctx().runtime_error_count;
                struct value_t *cond = eval(f->cond);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(cond);
                    break;
                }
                if (!cond) break;
                bool b = truthy(cond);
                freeval(cond);
                if (!b) break;
            }
            break;
        }

        case 'W': {
            const char *loop_label = take_pending_label();
            struct ast *cond_ast = ((struct flow *)a)->cond;
            struct ast *body = ((struct flow *)a)->tl;
            if (!body) break;
            for (;;) {
                if (g_interrupted) break;
                int errors_before = eval_ctx().runtime_error_count;
                struct value_t *cond = eval(cond_ast);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(cond);
                    break;
                }
                if (!cond) break;
                bool b = truthy(cond);
                freeval(cond);
                if (!b) break;

                errors_before = eval_ctx().runtime_error_count;
                ret = eval(body);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(ret);
                    ret = NULL;
                    break;
                }
                if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(ret);
                    ret = NULL;
                    break;
                }
                if (ret && (ret->type == T_return || ret->type == T_goto))
                    break;
                if (ret && ret->type == T_break) {
                    if (control_matches_label(ret, loop_label)) {
                        freeval(ret);
                        ret = NULL;
                        break;
                    }
                    break;
                }
                if (ret && ret->type == T_continue) {
                    if (control_matches_label(ret, loop_label)) {
                        freeval(ret);
                        ret = NULL;
                        continue;
                    }
                    break;
                }
                freeval(ret);
                ret = NULL;
            }
            break;
        }

        case 'F': {
            struct forinloop *f = (struct forinloop *)a;
            const char *loop_label = take_pending_label();
            int errors_before = eval_ctx().runtime_error_count;
            struct value_t *iter = eval(f->iterable);
            if (!iter || eval_ctx().runtime_error_count != errors_before) {
                freeval(iter);
                break;
            }

            if (fiber_is_generator(iter)) {
                if (f->value_name) {
                    yyerror("two-variable for-in requires dict, got generator");
                    freeval(iter);
                    break;
                }
                generator_ptr generator;
                if (!get_generator_value(iter, generator)) {
                    freeval(iter);
                    break;
                }

                eval_pushscope();
                struct symbol *sym = newsym(f->name);
                sym->val = NULL;
                savesym(sym);

                bool leave = false;
                while (!leave) {
                    if (g_interrupted)
                        break;

                    struct value_t *item = NULL;
                    bool done = false;
                    if (!generator_next_value(generator, NULL, item, done)) {
                        freeval(item);
                        leave = true;
                        break;
                    }
                    if (done) {
                        freeval(item);
                        break;
                    }
                    if (sym->val)
                        freeval(sym->val);
                    sym->val = item;

                    errors_before = eval_ctx().runtime_error_count;
                    struct value_t *body = eval(f->body);
                    if (eval_ctx().runtime_error_count != errors_before) {
                        freeval(body);
                        leave = true;
                        break;
                    }
                    if (eval_ctx().control != EVAL_CONTROL_NONE) {
                        freeval(body);
                        leave = true;
                        break;
                    }
                    if (body) {
                        if (body->type == T_return || body->type == T_goto) {
                            ret = body;
                            leave = true;
                            break;
                        }
                        if (body->type == T_break) {
                            if (control_matches_label(body, loop_label)) {
                                freeval(body);
                                leave = true;
                                break;
                            }
                            ret = body;
                            leave = true;
                            break;
                        }
                        if (body->type == T_continue) {
                            if (control_matches_label(body, loop_label)) {
                                freeval(body);
                                continue;
                            }
                            ret = body;
                            leave = true;
                            break;
                        }
                        freeval(body);
                    }
                }

                eval_popscope();
                freeval(iter);
                break;
            }

            if (iter->type == T_deque || iter->type == T_heap ||
                iter->type == T_set) {
                struct list_value_t *materialized = newlistval();
                std::vector<value_t *> items;
                if (iter->type == T_deque)
                    deque_items_in_order(
                        ((struct deque_value_t *)iter)->payload.get(), items);
                else if (iter->type == T_heap)
                    heap_items_level_order(
                        ((struct heap_value_t *)iter)->payload.get(), items);
                else
                    set_values_in_order(
                        ((struct set_value_t *)iter)->payload.get(), items);
                materialized->payload->items.reserve(items.size());
                for (struct value_t *item : items)
                    materialized->payload->items.push_back(dupval(item));
                freeval(iter);
                iter = (struct value_t *)materialized;
            }

            size_t count = 0;
            if (iter->type == T_list)
                count = ((struct list_value_t *)iter)->payload->items.size();
            else if (iter->type == T_dict)
                count = ((struct dict_value_t *)iter)->payload->items.size();
            else if (iter->type == T_string)
                count = string_length(
                    (struct string_value_t *)iter);
            else if (iter->type == T_bytes)
                count = ((struct bytes_value_t *)iter)->value.size();
            else {
                yyerror("cannot iterate %s", typename_of(iter));
                freeval(iter);
                break;
            }

            if (f->value_name && iter->type != T_dict) {
                yyerror("two-variable for-in requires dict, got %s",
                        typename_of(iter));
                freeval(iter);
                break;
            }

            if (f->value_name &&
                std::strcmp(f->name, f->value_name) == 0) {
                yyerror("loop variable repeated: %s", f->name);
                freeval(iter);
                break;
            }

            eval_pushscope();
            struct symbol *sym = newsym(f->name);
            sym->val = NULL;
            savesym(sym);
            struct symbol *value_sym = NULL;
            if (f->value_name) {
                value_sym = newsym(f->value_name);
                value_sym->val = NULL;
                savesym(value_sym);
            }

            bool leave = false;
            for (size_t i = 0; i < count && !leave; ++i) {
                if (g_interrupted)
                    break;

                struct value_t *item = NULL;
                struct value_t *second = NULL;
                if (iter->type == T_list) {
                    item = dupval(
                        ((struct list_value_t *)iter)->payload->items[i]);
                } else if (iter->type == T_dict) {
                    struct dict_value_t *dict = (struct dict_value_t *)iter;
                    item = dupval(dict->payload->items[i].first);
                    if (f->value_name)
                        second = dupval(dict->payload->items[i].second);
                } else if (iter->type == T_string) {
                    unsigned int codepoint = 0;
                    if (!string_codepoint_at(
                            (struct string_value_t *)iter, i, codepoint)) {
                        yyerror("invalid UTF-8 string iteration");
                        leave = true;
                        break;
                    }
                    item = newcharval(codepoint);
                } else {
                    item = newintval((unsigned char)
                        ((struct bytes_value_t *)iter)->value[i]);
                }

                freeval(sym->val);
                sym->val = item;
                if (value_sym) {
                    freeval(value_sym->val);
                    value_sym->val = second;
                }

                errors_before = eval_ctx().runtime_error_count;
                struct value_t *body = eval(f->body);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (body) {
                    if (body->type == T_return || body->type == T_goto) {
                        ret = body;
                        leave = true;
                        break;
                    }
                    if (body->type == T_break) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                            leave = true;
                            break;
                        }
                        ret = body;
                        leave = true;
                        break;
                    }
                    if (body->type == T_continue) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                            continue;
                        }
                        ret = body;
                        leave = true;
                        break;
                    }
                    freeval(body);
                }
            }

            eval_popscope();
            freeval(iter);
            break;
        }

        case 'P': {
            struct forloop *f = (struct forloop *)a;
            const char *loop_label = take_pending_label();
            eval_pushscope();
            bool leave = false;

            if (f->init) {
                int errors_before = eval_ctx().runtime_error_count;
                struct value_t *v = eval(f->init);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(v);
                    leave = true;
                } else if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(v);
                    leave = true;
                } else if (v) {
                    if (v->type == T_return || v->type == T_goto) {
                        ret = v;
                        leave = true;
                    } else {
                        if (v->type == T_break || v->type == T_continue) {
                            diag_set_location(g_flow_line, g_flow_column);
                            yyerror("break/continue outside loop");
                            leave = true;
                        }
                        freeval(v);
                    }
                }
            }

            for (; !leave;) {
                if (g_interrupted) break;

                bool cond_true = true;
                if (f->cond) {
                    int errors_before = eval_ctx().runtime_error_count;
                    struct value_t *cond = eval(f->cond);
                    if (eval_ctx().runtime_error_count != errors_before) {
                        freeval(cond);
                        leave = true;
                        break;
                    }
                    if (!cond) break;
                    cond_true = truthy(cond);
                    freeval(cond);
                }
                if (!cond_true) break;

                bool do_break = false;
                int errors_before = eval_ctx().runtime_error_count;
                struct value_t *body = eval(f->body);
                if (eval_ctx().runtime_error_count != errors_before) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (eval_ctx().control != EVAL_CONTROL_NONE) {
                    freeval(body);
                    leave = true;
                    break;
                }
                if (body) {
                    if (body->type == T_return || body->type == T_goto) {
                        ret = body;
                        leave = true;
                    } else if (body->type == T_break) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                            do_break = true;
                        } else {
                            ret = body;
                            leave = true;
                        }
                    } else if (body->type == T_continue) {
                        if (control_matches_label(body, loop_label)) {
                            freeval(body);
                        } else {
                            ret = body;
                            leave = true;
                        }
                    } else {
                        freeval(body);
                    }
                }
                if (leave || do_break) break;

                if (f->step) {
                    errors_before = eval_ctx().runtime_error_count;
                    struct value_t *step = eval(f->step);
                    if (eval_ctx().runtime_error_count != errors_before) {
                        freeval(step);
                        leave = true;
                        break;
                    }
                    if (eval_ctx().control != EVAL_CONTROL_NONE) {
                        freeval(step);
                        leave = true;
                        break;
                    }
                    if (step) {
                        if (step->type == T_return || step->type == T_goto) {
                            ret = step;
                            leave = true;
                        } else {
                            if (step->type == T_break || step->type == T_continue) {
                                diag_set_location(g_flow_line, g_flow_column);
                                yyerror("break/continue outside loop");
                                leave = true;
                            }
                            freeval(step);
                        }
                    }
                }
            }

            eval_popscope();
            break;
        }

        case 'L': {
            int errors_before = eval_ctx().runtime_error_count;
            ret = eval(a->l);
            if (eval_ctx().runtime_error_count != errors_before) {
                freeval(ret);
                ret = NULL;
                break;
            }
            if (eval_ctx().control != EVAL_CONTROL_NONE)
                break;
            if (ret && (ret->type == T_return || ret->type == T_goto ||
                        ret->type == T_break ||
                        ret->type == T_continue))
                break;
            freeval(ret);
            ret = eval(a->r);
            break;
        }

        case 'k': {
            struct is_node *node = (struct is_node *)a;
            struct value_t *value = eval(node->value);
            if (!value)
                break;
            if (!validate_type_constraint(node->type)) {
                freeval(value);
                break;
            }
            bool matched = type_constraint_matches(node->type, value);
            freeval(value);
            ret = newboolval(matched);
            break;
        }

        case NODE_CAST: {
            struct cast_node *node = (struct cast_node *)a;
            ret = eval(node->value);
            break;
        }

        case 'G': {
            struct value_t *fast = NULL;
            if (read_index_chain(a, fast)) {
                ret = fast;
                break;
            }
            struct value_t *container = eval(a->l);
            if (!container)
                break;
            struct value_t *index = eval(a->r);
            if (!index) {
                freeval(container);
                break;
            }
            ret = read_member_value(container, index);
            freeval(container);
            freeval(index);
            break;
        }

        case 'j': {
            const char *field = literal_field_name(a->r);
            if (field && a->l && a->l->nodetype == 'N') {
                const char *base = ((struct symref *)a->l)->name;
                if (base && !lookupsym(base)) {
                    structtype_lookup_result lookup;
                    std::string error;
                    structtype_lookup_status status =
                        structtype_lookup_type(
                            nullptr, diag_current_package_name().c_str(),
                            base, lookup, error);
                    if (status == STRUCTTYPE_LOOKUP_FOUND &&
                        lookup.kind == STRUCTTYPE_LOOKUP_ENUM) {
                        std::vector<std::pair<std::string, value_t *>>
                            fields;
                        std::vector<bool> spread;
                        ret = build_enum_value(
                            lookup.enumeration->source.c_str(),
                            lookup.enumeration->name.c_str(), field,
                            fields, spread);
                        break;
                    }
                    if (status == STRUCTTYPE_LOOKUP_FOUND &&
                        lookup.kind == STRUCTTYPE_LOOKUP_STRUCT &&
                        std::strcmp(field, "new") == 0) {
                        ret = structtype_constructor_value(
                            lookup.structure);
                        if (!ret) {
                            yyerror("type %s has no constructor", base);
                        }
                        break;
                    }
                    if (status == STRUCTTYPE_LOOKUP_AMBIGUOUS ||
                        status == STRUCTTYPE_LOOKUP_NOT_VISIBLE ||
                        status == STRUCTTYPE_LOOKUP_ERROR) {
                        yyerror("%s", error.c_str());
                        break;
                    }
                }
            }
            struct value_t *container = eval(a->l);
            if (!container)
                break;
            struct value_t *index = eval(a->r);
            if (!index) {
                freeval(container);
                break;
            }
            ret = read_member_value(container, index, true);
            freeval(container);
            freeval(index);
            break;
        }

        case 'C': {
            struct ufncall *call = (struct ufncall *)a;
            if (call->launch) {
                struct value_t *callee = eval(call->callee);
                if (callee) {
                    value_release_guard callee_guard(callee);
                    ret = launch_value(callee, call->args,
                                       call->awaited != 0);
                }
                break;
            }
            bool field_call =
                call->callee && call->callee->nodetype == 'j';
            /*
             * `Type.new(...)` has an unresolved base; send it through the
             * generic callee path so the type-name lookup can build the
             * constructor function value.
             */
            bool unresolved_field_base =
                field_call && call->callee->l &&
                call->callee->l->nodetype == 'N' &&
                !lookupsym(((struct symref *)call->callee->l)->name);
            if (call->callee && (call->callee->nodetype == 'G' ||
                                 (field_call && !unresolved_field_base))) {
                bool field_access = field_call;
                struct value_t *receiver = eval(call->callee->l);
                if (!receiver)
                    break;
                struct value_t *key = eval(call->callee->r);
                if (!key) {
                    freeval(receiver);
                    break;
                }
                if (receiver->type == T_struct) {
                    if (key->type != T_string) {
                        yyerror("struct method name must be a string, got %s",
                                typename_of(key));
                    } else {
                        struct struct_value_t *sv =
                            (struct struct_value_t *)receiver;
                        const char *name =
                            ((struct string_value_t *)key)->value;
                        const char *private_owner =
                            sv->def ? sv->def->package.c_str() : nullptr;
                        if (!private_member_access_allowed(name,
                                                           private_owner)) {
                            yyerror("private member %s", name);
                            freeval(receiver);
                            freeval(key);
                            break;
                        }
                        int field =
                            sv->def ? structtype_field_index(sv->def, name)
                                    : -1;
                        if (field >= 0 && sv->payload &&
                            (size_t)field < sv->payload->fields.size()) {
                            struct value_t *member =
                                dupval(sv->payload->fields[(size_t)field]);
                            freeval(receiver);
                            freeval(key);
                            if (member) {
                                value_release_guard member_guard(member);
                                ret = callvalue(member, call->args);
                            }
                            break;
                        }
                        bool called = false;
                        if (sv->def) {
                            std::shared_ptr<function_def> method =
                                structtype_method(sv->def, name);
                            if (method &&
                                !structtype_is_constructor(method.get())) {
                                try {
                                    ret = call_method_value(receiver, method,
                                                            call->args);
                                    called = true;
                                } catch (...) {
                                    freeval(receiver);
                                    freeval(key);
                                    throw;
                                }
                            }
                        }
                        if (!called) {
                            yyerror("method %s not found in %s", name,
                                    sv->def ? sv->def->name.c_str()
                                            : "struct");
                        }
                    }
                    freeval(receiver);
                    freeval(key);
                    break;
                }

                struct value_t *member =
                    read_member_value(receiver, key, field_access);
                freeval(receiver);
                freeval(key);
                if (member) {
                    value_release_guard member_guard(member);
                    ret = callvalue(member, call->args);
                }
                break;
            }

            struct value_t *callee = eval(call->callee);
            if (callee) {
                value_release_guard callee_guard(callee);
                ret = callvalue(callee, call->args);
            }
            break;
        }

        default:
            std::fprintf(stderr, "internal error: bad node %c\n", a->nodetype);
            break;
    }
    return ret;
}
