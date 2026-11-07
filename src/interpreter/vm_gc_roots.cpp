#include "vm_gc_roots.h"

#include "symbol.h"
#include "valuetype.h"

#include <cstddef>

namespace {

void gc_mark_value(gc_mark_fn mark, void *ctx, value_t *value) {
    if (value)
        mark(ctx, value);
}

void gc_mark_symbols(const std::vector<struct symbol *> &symbols,
                     gc_mark_fn mark, void *ctx) {
    for (struct symbol *sym : symbols) {
        if (sym)
            gc_mark_value(mark, ctx, sym->val);
    }
}

void gc_mark_cells(
    const std::vector<std::shared_ptr<struct upvalue_cell>> &cells,
    gc_mark_fn mark, void *ctx) {
    for (const auto &cell : cells) {
        if (!cell)
            continue;
        if (cell->slot)
            gc_mark_value(mark, ctx, *cell->slot);
        else
            gc_mark_value(mark, ctx, cell->value);
    }
}

void gc_mark_iterators(const std::vector<iterator_state> &iterators,
                       gc_mark_fn mark, void *ctx) {
    for (const iterator_state &state : iterators) {
        gc_mark_value(mark, ctx, state.iterable);
        if (state.key_sym)
            gc_mark_value(mark, ctx, state.key_sym->val);
        if (state.value_sym)
            gc_mark_value(mark, ctx, state.value_sym->val);
    }
}

void gc_mark_defers(
    const std::vector<std::vector<defer_record>> &scopes,
    gc_mark_fn mark, void *ctx) {
    for (const std::vector<defer_record> &scope : scopes) {
        for (const defer_record &record : scope)
            gc_mark_cells(record.upvalues, mark, ctx);
    }
}

void gc_mark_state(const SavedVmState &state, gc_mark_fn mark,
                   void *ctx) {
    gc_mark_symbols(state.param_symbols, mark, ctx);
    for (value_t *value : state.param_values)
        gc_mark_value(mark, ctx, value);
    gc_mark_symbols(state.slot_symbols, mark, ctx);
    for (value_t *value : state.slot_values)
        gc_mark_value(mark, ctx, value);
    gc_mark_cells(state.open_upvalues, mark, ctx);
    if (state.def)
        gc_mark_cells(state.def->upvalues, mark, ctx);
    gc_mark_iterators(state.iterators, mark, ctx);
    gc_mark_defers(state.defer_scopes, mark, ctx);
}

} // namespace

void mark_vm_roots(const VmRootView &view, gc_mark_fn mark,
                   void *mark_ctx) {
    if (view.stack) {
        for (value_t *value : *view.stack)
            gc_mark_value(mark, mark_ctx, value);
    }
    if (view.param_symbols)
        gc_mark_symbols(*view.param_symbols, mark, mark_ctx);
    if (view.param_values) {
        for (value_t *value : *view.param_values)
            gc_mark_value(mark, mark_ctx, value);
    }
    if (view.slot_symbols)
        gc_mark_symbols(*view.slot_symbols, mark, mark_ctx);
    if (view.slot_values) {
        for (value_t *value : *view.slot_values)
            gc_mark_value(mark, mark_ctx, value);
    }
    if (view.open_upvalues)
        gc_mark_cells(*view.open_upvalues, mark, mark_ctx);
    if (view.current_def && *view.current_def)
        gc_mark_cells((*view.current_def)->upvalues, mark, mark_ctx);
    if (view.iterators)
        gc_mark_iterators(*view.iterators, mark, mark_ctx);
    if (view.defer_scopes)
        gc_mark_defers(*view.defer_scopes, mark, mark_ctx);
    if (view.saved_states) {
        for (const SavedVmState &state : *view.saved_states)
            gc_mark_state(state, mark, mark_ctx);
    }
}
