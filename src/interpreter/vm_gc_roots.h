#ifndef SKEIN_VM_GC_ROOTS_H
#define SKEIN_VM_GC_ROOTS_H

#include "gc_roots.h"
#include "vm_state.h"

#include <memory>
#include <vector>

/*
 * Borrowed view of the VM frame arrays that participate in GC.  The VM
 * executor builds this on demand; the marker never stores it past the
 * collection.
 */
struct VmRootView {
    const std::vector<value_t *> *stack = nullptr;
    const std::vector<struct symbol *> *param_symbols = nullptr;
    const std::vector<value_t *> *param_values = nullptr;
    const std::vector<struct symbol *> *slot_symbols = nullptr;
    const std::vector<value_t *> *slot_values = nullptr;
    const std::vector<std::shared_ptr<struct upvalue_cell>> *open_upvalues =
        nullptr;
    const std::shared_ptr<function_def> *current_def = nullptr;
    const std::vector<iterator_state> *iterators = nullptr;
    const std::vector<std::vector<defer_record>> *defer_scopes = nullptr;
    const std::vector<SavedVmState> *saved_states = nullptr;
};

void mark_vm_roots(const VmRootView &view, gc_mark_fn mark,
                   void *mark_ctx);

#endif
