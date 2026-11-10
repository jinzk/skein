#include "gc.h"

#include "cycle.h"
#include "gc_roots.h"
#include "scopestack.h"
#include "structtype.h"
#include "symbol.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

namespace {

thread_local uint64_t gc_cell_epoch = 0;

/*
 * Cycle collection works at scope granularity.  Every cycle that can occur in
 * the current object model passes through a function closure, and therefore
 * through a scope.  Clearing symbol tables of scopes that are not reachable
 * from the active roots breaks those cycles.
 */
struct cycle_collector {
    std::unordered_set<scope_t *> live_scopes;
    std::unordered_set<value_t *> seen_values;
    std::unordered_set<list_payload *> seen_lists;
    std::unordered_set<dict_payload *> seen_dicts;
    std::vector<scope_t *> scope_work;
    std::vector<value_t *> value_work;

    void mark_scope(scope_t *s) {
        if (s)
            scope_work.push_back(s);
    }

    void mark_value(value_t *v) {
        if (v)
            value_work.push_back(v);
    }

    void mark_cell(const std::shared_ptr<struct upvalue_cell> &cell) {
        if (!cell)
            return;
        cell->gc_epoch = gc_cell_epoch;
        if (cell->slot)
            mark_value(*cell->slot);
        else
            mark_value(cell->value);
    }

    void mark_function(const std::shared_ptr<function_def> &def) {
        if (!def)
            return;
        if (def->closure)
            mark_scope(def->closure.get());
        for (const auto &cell : def->upvalues)
            mark_cell(cell);
    }

    void drain() {
        for (;;) {
            while (!scope_work.empty()) {
                scope_t *s = scope_work.back();
                scope_work.pop_back();
                if (!live_scopes.insert(s).second)
                    continue;

                if (s->parent)
                    mark_scope(s->parent.get());
                for (struct symbol *sym : s->syms.small) {
                    if (sym)
                        mark_value(sym->val);
                }
                mark_value(s->ret);
            }

            if (value_work.empty())
                break;

            while (!value_work.empty()) {
                value_t *v = value_work.back();
                value_work.pop_back();
                if (!seen_values.insert(v).second)
                    continue;

                switch (v->type) {
                    case T_func: {
                        struct func_value_t *fv = (struct func_value_t *)v;
                        mark_function(fv->def);
                        break;
                    }
                    case T_method: {
                        struct method_value_t *method =
                            (struct method_value_t *)v;
                        mark_function(method->method);
                        break;
                    }
                    default:
                        break;
                }

                for_each_value_child(v, [&](const value_t *child) {
                    mark_value(const_cast<value_t *>(child));
                });
            }
        }
    }
};

void mark_root_value(void *ctx, value_t *value) {
    static_cast<cycle_collector *>(ctx)->mark_value(value);
}

std::atomic<void (*)()> thread_cycle_collector{nullptr};

/*
 * Closed upvalue cells own their value.  A closure cycle such as
 * `func f() { return f; }` is held through a cell rather than a scope, so
 * clearing scopes alone cannot free it.  Break every cell that was not
 * reached by the current mark pass (or every closed cell at teardown):
 * detach the values first so freeing one cycle cannot recursively touch a
 * cell that is still being swept, then free them.
 */
void sweep_cells(bool all, struct scope_context *only_context = nullptr) {
    std::vector<value_t *> doomed;
    for (struct upvalue_cell *cell = gc_cell_registry_head(); cell;
         cell = cell->gc_next) {
        if (only_context && cell->owner != only_context)
            continue;
        if (!all && cell->gc_epoch == gc_cell_epoch)
            continue;
        if (cell->slot || !cell->value)
            continue;
        doomed.push_back(cell->value);
        cell->value = nullptr;
    }
    for (value_t *value : doomed)
        freeval(value);
}

} // namespace

void gc_mark_upvalue_cell(struct upvalue_cell *cell) {
    if (cell)
        cell->gc_epoch = gc_cell_epoch;
}

void gc_sweep_all_cells(scope_context &context) {
    sweep_cells(true, &context);
}

void gc_set_thread_cycle_collector(void (*fn)()) {
    thread_cycle_collector.store(fn);
}

void collect_thread_cycles() {
    void (*collector)() = thread_cycle_collector.load();
    if (collector)
        collector();
}

void collect_cycles(scope_context &context) {
    prune_scopes(context);
    ++gc_cell_epoch;

    std::vector<std::shared_ptr<scope_t>> scopes;
    scopes.reserve(context.all_scopes.size());
    for (auto &w : context.all_scopes) {
        if (std::shared_ptr<scope_t> s = w.lock())
            scopes.push_back(std::move(s));
    }

    cycle_collector collector;
    collector.mark_scope(context.global_scope.get());
    collector.mark_scope(context.current.get());
    for (auto &s : context.restore_stack)
        collector.mark_scope(s.get());
    gc_roots_visit(context, mark_root_value, &collector);
    structtype_visit_functions(
        [&collector](const std::shared_ptr<function_def> &def) {
            collector.mark_function(def);
        });
    collector.drain();

    for (auto &s : scopes) {
        if (collector.live_scopes.count(s.get()))
            continue;
        clear_scope(s.get());
        if (s->ret) {
            freeval(s->ret);
            s->ret = NULL;
        }
    }

    sweep_cells(false, &context);
    scopes.clear();
    collect_thread_cycles();
    prune_scopes(context);
}

void maybe_collect_cycles() {
    scope_context &context = active_scope_context();
    static constexpr int COLLECT_INTERVAL = 64;
    static constexpr std::size_t SCOPE_GROWTH_THRESHOLD = 1024;

    ++context.statements_since_collect;
    prune_scopes(context);

    bool due = context.statements_since_collect >= COLLECT_INTERVAL;
    if (!due && context.all_scopes.size() >
                    context.scopes_at_last_collect + SCOPE_GROWTH_THRESHOLD)
        due = true;
    if (!due)
        return;

    context.statements_since_collect = 0;
    collect_cycles(context);
    prune_scopes(context);
    context.scopes_at_last_collect = context.all_scopes.size();
}

bool gc_stress_enabled() {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = std::getenv("SKEIN_GC_STRESS");
        enabled = 0;
        if (value && *value) {
            enabled = !(std::strcmp(value, "0") == 0 ||
                        std::strcmp(value, "false") == 0 ||
                        std::strcmp(value, "off") == 0 ||
                        std::strcmp(value, "no") == 0);
        }
    }
    return enabled != 0;
}
