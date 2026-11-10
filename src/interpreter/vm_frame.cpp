#include "vm_frame.h"

#include "diagnostics.h"
#include "eval_context.h"

namespace {

template <typename T>
std::vector<T> take_pool(std::vector<std::vector<T>> &pool) {
    if (pool.empty())
        return std::vector<T>();
    std::vector<T> value = std::move(pool.back());
    pool.pop_back();
    value.clear();
    return value;
}

void recycle_frame_vectors(const VmFrameView &view) {
    if (view.name_cache->capacity() != 0)
        view.name_cache_pool->push_back(std::move(*view.name_cache));
    if (view.param_symbols->capacity() != 0)
        view.param_symbols_pool->push_back(std::move(*view.param_symbols));
    if (view.param_values->capacity() != 0)
        view.param_values_pool->push_back(std::move(*view.param_values));
    if (view.slot_symbols->capacity() != 0)
        view.slot_symbols_pool->push_back(std::move(*view.slot_symbols));
    if (view.slot_values->capacity() != 0)
        view.slot_values_pool->push_back(std::move(*view.slot_values));
}

} // namespace

void save_vm_frame(const VmFrameView &view) {
    SavedVmState state;
    state.proto = *view.proto;
    state.pc = *view.pc;
    state.scope_mark = *view.scope_mark;
    state.frame_stack_base = *view.frame_stack_base;
    state.errors_before = *view.errors_before;
    state.saved_handler_depth = *view.saved_handler_depth;
    state.caller_handler_depth = view.context->handler_depth;
    state.handler_body_depth = *view.handler_body_depth;
    state.diag_frames = *view.diag_frames;
    state.diag_module = diag_current_module_name();
    state.diag_source = diag_current_source_name();
    state.name_scope_generation = *view.name_scope_generation;
    state.param_pool_safe = *view.param_pool_safe;
    state.slot_pool_safe = *view.slot_pool_safe;
    state.direct_params = *view.direct_params;
    state.direct_slots = *view.direct_slots;
    state.def = std::move(*view.current_def);
    state.name_cache = std::move(*view.name_cache);
    state.param_symbols = std::move(*view.param_symbols);
    state.param_values = std::move(*view.param_values);
    state.slot_symbols = std::move(*view.slot_symbols);
    state.slot_values = std::move(*view.slot_values);
    state.slot_const = std::move(*view.slot_const);
    state.open_upvalues = std::move(*view.open_upvalues);
    view.open_upvalues->clear();
    state.iterators = std::move(*view.iterators);
    state.defer_scopes = std::move(*view.defer_scopes);
    state.handlers = std::move(*view.handlers);
    view.saved_states->push_back(std::move(state));

    *view.name_cache = take_pool(*view.name_cache_pool);
    *view.param_symbols = take_pool(*view.param_symbols_pool);
    *view.param_values = take_pool(*view.param_values_pool);
    *view.slot_symbols = take_pool(*view.slot_symbols_pool);
    *view.slot_values = take_pool(*view.slot_values_pool);
    *view.name_scope_generation = 0;
    *view.param_pool_safe = false;
    *view.direct_params = false;
    *view.direct_slots = false;
    *view.slot_pool_safe = false;
}

void restore_vm_frame(const VmFrameView &view, SavedVmState state) {
    recycle_frame_vectors(view);
    *view.proto = state.proto;
    *view.pc = state.pc;
    *view.scope_mark = state.scope_mark;
    *view.frame_stack_base = state.frame_stack_base;
    *view.errors_before = state.errors_before;
    *view.saved_handler_depth = state.saved_handler_depth;
    *view.handler_body_depth = state.handler_body_depth;
    view.context->handler_depth = state.caller_handler_depth;
    *view.diag_frames = state.diag_frames;
    diag_set_module_name(state.diag_module);
    diag_set_source_name(state.diag_source);
    *view.name_scope_generation = state.name_scope_generation;
    *view.param_pool_safe = state.param_pool_safe;
    *view.direct_params = state.direct_params;
    *view.direct_slots = state.direct_slots;
    *view.slot_pool_safe = state.slot_pool_safe;
    *view.current_def = std::move(state.def);
    *view.name_cache = std::move(state.name_cache);
    *view.param_symbols = std::move(state.param_symbols);
    *view.param_values = std::move(state.param_values);
    *view.slot_symbols = std::move(state.slot_symbols);
    *view.slot_values = std::move(state.slot_values);
    *view.slot_const = std::move(state.slot_const);
    *view.open_upvalues = std::move(state.open_upvalues);
    *view.iterators = std::move(state.iterators);
    *view.defer_scopes = std::move(state.defer_scopes);
    *view.handlers = std::move(state.handlers);
}
