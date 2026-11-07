#ifndef SKEIN_VM_SLOTS_H
#define SKEIN_VM_SLOTS_H

#include "bytecode.h"
#include "valuetype.h"

#include <cstdint>
#include <vector>

struct function_def;

struct VmSlotView {
    const skein::bc::Proto *proto = nullptr;
    const function_def *current_def = nullptr;
    std::vector<value_t *> *param_values = nullptr;
    std::vector<value_t *> *slot_values = nullptr;
    std::vector<uint8_t> *slot_const = nullptr;
};

value_t *assign_direct_param(const VmSlotView &view, uint32_t index,
                             value_t *value);
value_t *compound_direct_param(const VmSlotView &view, uint32_t index,
                               int op, value_t *value);
value_t *incdec_direct_param(const VmSlotView &view, uint32_t index,
                             int delta, bool prefix);

value_t *assign_direct_slot(const VmSlotView &view, uint32_t index,
                            value_t *value);
value_t *compound_direct_slot(const VmSlotView &view, uint32_t index,
                              int op, value_t *value);
value_t *incdec_direct_slot(const VmSlotView &view, uint32_t index,
                            int delta, bool prefix);

value_t *binary_slot_fused_direct(const VmSlotView &view,
                                  const skein::bc::Instr &ins,
                                  bool constant_rhs);

#endif
