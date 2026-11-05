#include "eval.h"
#include "diagnostics.h"
#include "dict.h"
#include "eval_context.h"
#include "eval_helpers.h"
#include "fiber.h"
#include "format.h"
#include "package.h"
#include "pattern.h"
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
#include "vm.h"

void free_args(std::vector<struct value_t *> &args) {
    for (size_t i = 0; i < args.size(); ++i)
        freeval(args[i]);
    args.clear();
}

/*
 * Function arguments are represented as a right-nested 'L' chain.  Evaluate
 * them left-to-right without using eval('L'), because eval('L') intentionally
 * frees intermediate statement values.
 */
static bool collect_args(struct ast *a, std::vector<struct value_t *> &out,
                         struct value_t *&control) {
    if (!a) return true;
    if (out.capacity() == 0)
        out.reserve(8);
    if (a->nodetype == 'L') {
        if (!collect_args(a->l, out, control)) return false;
        return collect_args(a->r, out, control);
    }
    if (a->nodetype == 'x') {
        struct value_t *v = eval(((struct spread_node *)a)->value);
        if (!v) return false;
        if (v->type == T_return || v->type == T_break ||
            v->type == T_continue) {
            control = v;
            return false;
        }
        if (v->type != T_list) {
            yyerror("spread expects a list, got %s", typename_of(v));
            freeval(v);
            return false;
        }
        struct list_value_t *list = (struct list_value_t *)v;
        for (size_t i = 0; i < list->payload->items.size(); ++i)
            out.push_back(dupval(list->payload->items[i]));
        freeval(v);
        return true;
    }
    struct value_t *v = eval(a);
    if (!v) return false;
    if (v->type == T_return || v->type == T_break || v->type == T_continue) {
        control = v;
        return false;
    }
    out.push_back(v);
    return true;
}

static struct value_t *invoke_value(
    struct value_t *callee, std::vector<struct value_t *> &args);
struct value_t *call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args);

static constexpr int MAX_CALL_DEPTH = 1000;
static int call_depth = 0;

namespace {
struct call_depth_guard {
    call_depth_guard() { ++call_depth; }
    ~call_depth_guard() { --call_depth; }
};
}

static struct value_t *invoke_function_body(
    const std::shared_ptr<function_def> &def,
    std::vector<struct value_t *> &args) {
    int nparams = def->code->nparams;
    std::string frame_name =
        def->code->name.empty() ? "<anonymous>" : def->code->name;

    struct symlist *rest = NULL;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next) {
        if (sl->variadic) {
            rest = sl;
            break;
        }
    }

    int errors_before = eval_ctx().runtime_error_count;
    int frame_line = g_error_line;
    int frame_column = g_error_column;
    if (frame_line <= 0 && def->code->ast) {
        frame_line = def->code->ast->line;
        frame_column = def->code->ast->column;
    }
    call_stack_guard frame_guard(frame_name, frame_line, frame_column);
    bool saved_allow_await = eval_ctx().allow_await;
    eval_ctx().allow_await = def->code->is_async;
    eval_push_closure_scope(def->closure);
    int i = 0;
    bool pattern_failed = false;
    for (struct symlist *sl = def->code->sl; sl && !sl->variadic;
         sl = sl->next, ++i) {
        struct symbol *sym = newsym(sl->sym->name);
        sym->val = args[i];
        if (sl->type) {
            sym->type.kind = sl->type->kind;
            sym->type.name = sl->type->name;
        }
        if (i == 0 && def->code->is_method) {
            sym->is_self = true;
            sym->is_const = true;
        }
        savesym(sym);

        if (sl->pattern) {
            std::vector<pattern_binding> bindings;
            if (!match_pattern(sl->pattern, sym->val, bindings)) {
                free_pattern_bindings(bindings);
                yyerror("function parameter does not match its pattern");
                pattern_failed = true;
                break;
            }
            for (auto &binding : bindings) {
                struct symbol *target = newsym(binding.name.c_str());
                target->val = binding.value;
                binding.value = NULL;
                savesym(target);
            }
            bindings.clear();
        }
    }
    if (pattern_failed) {
        for (size_t k = (size_t)i + 1; k < args.size(); ++k)
            freeval(args[k]);
        args.clear();
        eval_popscope();
        eval_ctx().allow_await = saved_allow_await;
        return NULL;
    }
    if (rest) {
        struct list_value_t *list = newlistval();
        for (size_t k = (size_t)nparams; k < args.size(); ++k) {
            list->payload->items.push_back(args[k]);
            args[k] = NULL;
        }
        struct symbol *sym = newsym(rest->sym->name);
        sym->val = (struct value_t *)list;
        sym->type.kind = TYPE_LIST;
        sym->type.name = NULL;
        savesym(sym);
    }
    args.clear(); /* ownership transferred to the function scope */

    int saved_handler_depth = eval_ctx().handler_depth;
    eval_ctx().handler_depth = 0;
    struct value_t *body_ret = NULL;
    if (!skein_vm_try_execute_function_body(def, body_ret))
        body_ret = eval(def->code->ast);
    eval_ctx().handler_depth = saved_handler_depth;
    struct value_t *result = topscope()->ret;
    topscope()->ret = NULL;

    if (body_ret) {
        if (body_ret->type == T_break) {
            diag_set_location(g_flow_line, g_flow_column);
            yyerror("break outside loop in function");
        } else if (body_ret->type == T_continue) {
            diag_set_location(g_flow_line, g_flow_column);
            yyerror("continue outside loop in function");
        }
        freeval(body_ret);
    }

    eval_popscope();
    eval_ctx().allow_await = saved_allow_await;

    if (eval_ctx().control != EVAL_CONTROL_NONE) {
        freeval(result);
        return NULL;
    }

    bool errored = eval_ctx().runtime_error_count != errors_before;
    if (g_interrupted || errored) {
        freeval(result);
        return NULL;
    }
    return result ? result : newnull();
}

/*
 * Fill trailing parameters from their default expressions.  Defaults are
 * evaluated in a temporary scope parented to the function's defining scope,
 * with earlier parameters already bound so expressions such as `b = a + 1`
 * work.  Later parameters are pre-bound as uninitialized symbols so a
 * forward reference cannot silently resolve an outer variable.
 */
static bool fill_default_arguments(
    const std::shared_ptr<function_def> &def,
    const std::string &frame_name,
    std::vector<struct value_t *> &args) {
    int nparams = def->code->nparams;
    size_t provided = args.size();

    eval_push_closure_scope(def->closure);
    struct default_scope_guard {
        ~default_scope_guard() { eval_popscope(); }
    } scope_guard;

    std::vector<struct symbol *> temp_syms;
    temp_syms.reserve((size_t)nparams + 1);
    int index = 0;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next) {
        struct symbol *s = newsym(sl->sym->name);
        s->val = NULL;
        s->is_uninitialized = true;
        s->is_const = true;
        if (sl->variadic) {
            s->type.kind = TYPE_LIST;
            s->type.name = NULL;
        } else if (sl->type) {
            s->type = copy_type_constraint(sl->type);
        }
        if (index == 0 && def->code->is_method)
            s->is_self = true;
        savesym(s);
        temp_syms.push_back(s);
        if (sl->variadic)
            break;
        ++index;
    }

    int i = 0;
    for (struct symlist *sl = def->code->sl; sl && !sl->variadic;
         sl = sl->next, ++i) {
        struct value_t *v = NULL;
        bool from_default = false;
        if ((size_t)i < provided) {
            v = args[(size_t)i];
        } else {
            from_default = true;
            if (!sl->default_value) {
                yyerror("function %s is missing required argument %s",
                        frame_name.c_str(), sl->sym->name);
                free_args(args);
                return false;
            }
            v = eval(sl->default_value);
            if (!v) {
                free_args(args);
                return false;
            }
            if (v->type == T_return || v->type == T_break ||
                v->type == T_continue) {
                yyerror("default value for parameter %s cannot return "
                        "control flow", sl->sym->name);
                freeval(v);
                free_args(args);
                return false;
            }
        }

        if (!type_constraint_matches(sl->type, v)) {
            if (from_default) {
                yyerror("function %s parameter %s default expects %s, got %s",
                        frame_name.c_str(), sl->sym->name,
                        type_constraint_name(sl->type), typename_of(v));
                freeval(v);
            } else {
                yyerror("function %s argument %d (%s) expects %s, got %s",
                        frame_name.c_str(), i + 1, sl->sym->name,
                        type_constraint_name(sl->type), typename_of(v));
            }
            free_args(args);
            return false;
        }

        temp_syms[(size_t)i]->val = dupval(v);
        temp_syms[(size_t)i]->is_uninitialized = false;
        if (from_default)
            args.push_back(v);
    }
    return true;
}

static struct value_t *invoke_value(
    struct value_t *callee, std::vector<struct value_t *> &args) {
    if (!callee) {
        free_args(args);
        return NULL;
    }

    if (callee->type == T_builtin) {
        struct skein_context *ctx = skein_context_current();
        struct value_t *ret = NULL;
        if (!ctx || !ctx->call_builtin) {
            yyerror("runtime is not initialized");
        } else {
            ret = ctx->call_builtin(
                ((struct builtin_value_t *)callee)->functype, args);
        }
        free_args(args);
        return ret;
    }

    if (callee->type == T_native) {
        struct native_function_value *fn =
            (struct native_function_value *)callee;
        struct value_t *ret = fn->call ? fn->call(args) : NULL;
        free_args(args);
        return ret;
    }

    if (callee->type == T_method) {
        struct method_value_t *method = (struct method_value_t *)callee;
        std::vector<struct value_t *> all_args;
        all_args.reserve(args.size() + 1);
        all_args.push_back(newstructval(method->def, method->receiver));
        for (size_t i = 0; i < args.size(); ++i)
            all_args.push_back(args[i]);
        args.clear();
        struct func_value_t function;
        function.type = T_func;
        function.def = method->method;
        return invoke_value((struct value_t *)&function, all_args);
    }

    if (callee->type != T_func) {
        yyerror("cannot call %s", typename_of(callee));
        free_args(args);
        return NULL;
    }

    struct func_value_t *fv = (struct func_value_t *)callee;
    /*
     * Keep the function definition alive for the whole call.  The body can
     * reassign/free the variable that holds this function value.
     */
    std::shared_ptr<function_def> def = fv->def;
    int nparams = def->code->nparams;
    int required = def->code->required_params;
    bool variadic = def->code->variadic;
    bool has_defaults = required != nparams;
    std::string frame_name =
        def->code->name.empty() ? "<anonymous>" : def->code->name;
    if ((int)args.size() < required) {
        if (variadic || has_defaults)
            yyerror("function %s expects at least %d argument(s), got %d",
                    frame_name.c_str(), required, (int)args.size());
        else
            yyerror("function expects %d argument(s), got %d",
                    nparams, (int)args.size());
        free_args(args);
        return NULL;
    }
    if (!variadic && (int)args.size() > nparams) {
        if (has_defaults)
            yyerror("function %s expects at most %d argument(s), got %d",
                    frame_name.c_str(), nparams, (int)args.size());
        else
            yyerror("function expects %d argument(s), got %d",
                    nparams, (int)args.size());
        free_args(args);
        return NULL;
    }
    if ((int)args.size() < nparams) {
        if (!fill_default_arguments(def, frame_name, args))
            return NULL;
    }

    struct symlist *rest = NULL;
    int i = 0;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next, ++i) {
        if (sl->variadic) {
            rest = sl;
            break;
        }
        if (!type_constraint_matches(sl->type, args[i])) {
            yyerror("function %s argument %d (%s) expects %s, got %s",
                    frame_name.c_str(), i + 1, sl->sym->name,
                    type_constraint_name(sl->type), typename_of(args[i]));
            free_args(args);
            return NULL;
        }
    }
    if (rest) {
        for (size_t k = (size_t)nparams; k < args.size(); ++k) {
            if (!type_constraint_matches(rest->type, args[k])) {
                yyerror("function %s rest argument %d expects %s, got %s",
                        frame_name.c_str(), (int)(k - (size_t)nparams + 1),
                        type_constraint_name(rest->type),
                        typename_of(args[k]));
                free_args(args);
                return NULL;
            }
        }
    }

    if (def->code->is_generator) {
        std::shared_ptr<std::vector<value_t *>> owned(
            new std::vector<value_t *>(std::move(args)),
            [](std::vector<value_t *> *values) {
                free_args(*values);
                delete values;
            });
        generator_ptr generator = generator_create(
            [def, owned]() {
                return invoke_function_body(def, *owned);
            });
        if (!generator) {
            return NULL;
        }
        return new_generator_value(std::move(generator));
    }

    if (def->code->is_async) {
        task_ptr task = fiber_start_async(
            [def, owned = std::move(args)]() mutable {
                return invoke_function_body(def, owned);
            });
        if (!task) {
            free_args(args);
            return NULL;
        }
        return new_task_value(std::move(task));
    }

    return invoke_function_body(def, args);
}

struct value_t *call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args) {
    if (!callee) {
        free_args(args);
        return NULL;
    }
    if (call_depth >= MAX_CALL_DEPTH) {
        yyerror("maximum recursion depth (%d) exceeded", MAX_CALL_DEPTH);
        free_args(args);
        return NULL;
    }
    call_depth_guard depth_guard;
    return invoke_value(callee, args);
}

const char *import_binding_name(const char *path) {
    while (path && (*path == '.' || *path == '@'))
        ++path;
    if (!path || !*path)
        return "";
    const char *dot = std::strrchr(path, '.');
    return dot ? dot + 1 : path;
}

struct value_t *callvalue(struct value_t *callee, struct ast *args_ast) {
    if (!callee)
        return NULL;

    /*
     * Keep the call-depth guard before argument evaluation so deeply nested
     * call expressions cannot exhaust the C++ stack.
     */
    if (call_depth >= MAX_CALL_DEPTH) {
        yyerror("maximum recursion depth (%d) exceeded", MAX_CALL_DEPTH);
        return NULL;
    }
    call_depth_guard depth_guard;

    std::vector<struct value_t *> args;
    struct value_t *control = NULL;
    if (!collect_args(args_ast, args, control)) {
        free_args(args);
        return control;
    }

    return invoke_value(callee, args);
}

struct value_t *call_method_value(struct value_t *receiver,
                                         std::shared_ptr<function_def> def,
                                         struct ast *args_ast) {
    if (call_depth >= MAX_CALL_DEPTH) {
        yyerror("maximum recursion depth (%d) exceeded", MAX_CALL_DEPTH);
        return NULL;
    }
    call_depth_guard depth_guard;

    std::vector<struct value_t *> args;
    args.reserve(8);
    args.push_back(dupval(receiver));
    struct value_t *control = NULL;
    if (!collect_args(args_ast, args, control)) {
        free_args(args);
        return control;
    }

    struct func_value_t callee;
    callee.type = T_func;
    callee.def = std::move(def);
    return invoke_value((struct value_t *)&callee, args);
}
