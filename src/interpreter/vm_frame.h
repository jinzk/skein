#ifndef SKEIN_VM_FRAME_H
#define SKEIN_VM_FRAME_H

#include "bytecode.h"
#include "vm_state.h"

#include <cstdint>
#include <memory>
#include <vector>

struct eval_context;

/*
 * Borrowed view of the executor's frame-owned state.  Save/restore moves
 * the vectors between the live frame and SavedVmState, so the view exposes
 * non-const pointers to both the slots and the recycling pools.
 */
struct VmFrameView {
    const skein::bc::Proto **proto = nullptr;
    size_t *pc = nullptr;
    size_t *scope_mark = nullptr;
    size_t *frame_stack_base = nullptr;
    int *errors_before = nullptr;
    int *saved_handler_depth = nullptr;
    int *handler_body_depth = nullptr;
    eval_context *context = nullptr;
    size_t *diag_frames = nullptr;
    uint64_t *name_scope_generation = nullptr;
    bool *param_pool_safe = nullptr;
    bool *slot_pool_safe = nullptr;
    bool *direct_params = nullptr;
    bool *direct_slots = nullptr;
    std::shared_ptr<function_def> *current_def = nullptr;
    std::vector<NameCacheSlot> *name_cache = nullptr;
    std::vector<struct symbol *> *param_symbols = nullptr;
    std::vector<value_t *> *param_values = nullptr;
    std::vector<struct symbol *> *slot_symbols = nullptr;
    std::vector<value_t *> *slot_values = nullptr;
    std::vector<uint8_t> *slot_const = nullptr;
    std::vector<std::shared_ptr<struct upvalue_cell>> *open_upvalues = nullptr;
    std::vector<iterator_state> *iterators = nullptr;
    std::vector<std::vector<defer_record>> *defer_scopes = nullptr;
    std::vector<handler_state> *handlers = nullptr;
    std::vector<SavedVmState> *saved_states = nullptr;
    std::vector<std::vector<NameCacheSlot>> *name_cache_pool = nullptr;
    std::vector<std::vector<struct symbol *>> *param_symbols_pool = nullptr;
    std::vector<std::vector<value_t *>> *param_values_pool = nullptr;
    std::vector<std::vector<struct symbol *>> *slot_symbols_pool = nullptr;
    std::vector<std::vector<value_t *>> *slot_values_pool = nullptr;
};

void save_vm_frame(const VmFrameView &view);
void restore_vm_frame(const VmFrameView &view, SavedVmState state);

#endif
