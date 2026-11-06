/*
 * GC root-table guard: a closure scope referenced only by a registered
 * VM/JIT root must stay alive.  Closed upvalue cells must also be traversed,
 * so scopes reachable only through a captured function stay alive.
 */
#include "gc.h"
#include "gc_roots.h"
#include "scopestack.h"
#include "symbol.h"
#include "valuetype.h"

#include <memory>

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
    collect_cycles(context);
    const bool outer_cleared = outer->syms.small.empty();

    freeval(root_fn);
    freeval(captured_fn);
    donescope(context);

    return outer_live && captured_live && outer_cleared ? 0 : 1;
}
