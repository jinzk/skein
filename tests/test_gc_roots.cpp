/*
 * GC root-table guard: a closure scope referenced only by a registered
 * VM/JIT root must stay alive.  Closed upvalue cells must also be traversed,
 * so scopes reachable only through a captured function stay alive.
 */
#include "gc.h"
#include "gc_roots.h"
#include "eval_context.h"
#include "scopestack.h"
#include "symbol.h"
#include "valuetype.h"

#include <functional>
#include <memory>
#include <vector>

namespace {

std::shared_ptr<scope_t> make_scope(const std::shared_ptr<scope_t> &parent) {
    std::shared_ptr<scope_t> scope = std::make_shared<scope_t>();
    scope->parent = parent;
    scope->is_function = false;
    scope->is_global = false;
    scope->ret = NULL;
    scope->function_scope = NULL;
    return scope;
}

void add_symbol(scope_context &context, const std::shared_ptr<scope_t> &scope,
                const char *name, value_t *value) {
    context.all_scopes.push_back(scope);
    struct symbol *sym = newsym(name);
    sym->val = value;
    scope->syms.insert(sym);
}

struct visitor_payload {
    value_t *child = nullptr;

    ~visitor_payload() { freeval(child); }
};

std::string visitor_describe(const void *) { return "visitor"; }

void visitor_for_each_child(
    const void *data,
    const std::function<void(const value_t *child)> &visit) {
    const visitor_payload *payload = static_cast<const visitor_payload *>(data);
    if (payload && payload->child)
        visit(payload->child);
}

const native_type kVisitorType = {
    "visitor",
    visitor_describe,
    nullptr,
    nullptr,
    nullptr,
    visitor_for_each_child,
};

} // namespace

int main() {
    scope_context context;
    initscope(context);

    std::shared_ptr<scope_t> outer = make_scope(context.current);
    std::shared_ptr<scope_t> captured = make_scope(context.current);
    add_symbol(context, outer, "outer", newintval(1));
    add_symbol(context, captured, "captured", newintval(2));

    value_t *captured_fn = newfuncval(nullptr, captured, {});
    auto cell = std::make_shared<upvalue_cell>();
    cell->value = captured_fn;
    value_t *root_fn = newfuncval(nullptr, outer, {cell});

    value_t *roots[1] = {root_fn};
    const size_t mark = gc_roots_push_array(context, roots, 1);

    collect_cycles(context);
    const bool outer_live = !outer->syms.small.empty();
    const bool captured_live = !captured->syms.small.empty();

    gc_roots_pop(context, mark);
    /*
     * The cell owns its captured value once closed, so dropping the last
     * host reference to the closure also releases the captured function.
     */
    cell.reset();
    freeval(root_fn);
    root_fn = nullptr;
    captured_fn = nullptr;
    collect_cycles(context);
    const bool outer_cleared = outer->syms.small.empty();

    donescope(context);

    eval_context eval;
    std::shared_ptr<scope_t> control_scope = make_scope(nullptr);
    add_symbol(eval.scopes, control_scope, "control", newintval(3));
    eval.control_value = newfuncval(nullptr, control_scope, {});

    collect_cycles(eval.scopes);
    const bool control_live = !control_scope->syms.small.empty();

    freeval(eval.control_value);
    eval.control_value = NULL;
    collect_cycles(eval.scopes);
    const bool control_cleared = control_scope->syms.small.empty();

    /*
     * C++-held values (builtin accumulators) are only visible through an
     * explicit gc_root_scope.  Without the guard the collector clears the
     * closure scope; with it the scope survives, and it is cleared again
     * once the guard is gone.
     */
    scope_context guarded_context;
    initscope(guarded_context);
    std::shared_ptr<scope_t> guarded_scope =
        make_scope(guarded_context.current);
    add_symbol(guarded_context, guarded_scope, "guarded", newintval(11));
    value_t *guarded_fn = newfuncval(nullptr, guarded_scope, {});
    std::vector<value_t *> live_values;
    live_values.push_back(guarded_fn);
    {
        gc_root_scope roots(guarded_context);
        roots.add_vector(&live_values);
        collect_cycles(guarded_context);
    }
    const bool guard_live = !guarded_scope->syms.small.empty();
    live_values.clear();
    freeval(guarded_fn);
    guarded_fn = nullptr;
    collect_cycles(guarded_context);
    const bool guard_cleared = guarded_scope->syms.small.empty();
    donescope(guarded_context);

    /* Userdata payloads must expose their script children through the
     * native_type visitor; otherwise their captured scopes are cleared. */
    scope_context visitor_context;
    initscope(visitor_context);
    std::shared_ptr<scope_t> visitor_scope =
        make_scope(visitor_context.current);
    add_symbol(visitor_context, visitor_scope, "guest", newintval(99));
    value_t *guest_fn = newfuncval(nullptr, visitor_scope, {});
    auto payload = std::make_shared<visitor_payload>();
    payload->child = guest_fn;
    value_t *visitor = newuserdataval(&kVisitorType, payload);
    std::vector<value_t *> visitor_roots{visitor};
    {
        gc_root_scope roots(visitor_context);
        roots.add_vector(&visitor_roots);
        collect_cycles(visitor_context);
    }
    const bool visitor_live = !visitor_scope->syms.small.empty();
    visitor_roots.clear();
    freeval(visitor);
    collect_cycles(visitor_context);
    const bool visitor_cleared = visitor_scope->syms.small.empty();
    donescope(visitor_context);

    return outer_live && captured_live && outer_cleared &&
                   control_live && control_cleared && guard_live &&
                   guard_cleared && visitor_live && visitor_cleared
               ? 0
               : 1;
}
