#include "vm_slots.h"

#include "symbol.h"
#include "value_ops.h"
#include "value_runtime.h"

#include <cstddef>

namespace {

struct symlist *param_meta(const function_def *def, uint32_t index) {
    if (!def || !def->code)
        return NULL;
    int i = 0;
    for (struct symlist *sl = def->code->sl; sl; sl = sl->next, ++i) {
        if (static_cast<uint32_t>(i) == index)
            return sl;
    }
    return NULL;
}

const char *param_name(const skein::bc::Proto &proto,
                       const function_def *def, uint32_t index) {
    if (index < proto.param_names.size())
        return proto.param_names[index].c_str();
    struct symlist *sl = param_meta(def, index);
    return sl && sl->sym && sl->sym->name ? sl->sym->name : "parameter";
}

bool check_direct_param(const VmSlotView &view, uint32_t index,
                        value_t *value) {
    struct symlist *sl = param_meta(view.current_def, index);
    if (!sl || !sl->type)
        return true;
    if (type_constraint_matches(sl->type, value))
        return true;
    yyerror("variable %s expects %s, got %s",
            param_name(*view.proto, view.current_def, index),
            type_constraint_name(sl->type), typename_of(value));
    return false;
}

bool check_direct_slot(const VmSlotView &view, uint32_t index,
                       value_t *value, bool check_const) {
    if (index >= view.proto->slots.size())
        return false;
    const skein::bc::SlotDesc &slot = view.proto->slots[index];
    if (check_const &&
        (slot.is_const ||
         (index < view.slot_const->size() && (*view.slot_const)[index]))) {
        yyerror("cannot assign to constant %s", slot.name.c_str());
        return false;
    }
    if (!validate_type_constraint(&slot.type))
        return false;
    if (!type_constraint_matches(&slot.type, value)) {
        yyerror("variable %s expects %s, got %s", slot.name.c_str(),
                type_constraint_name(&slot.type), typename_of(value));
        return false;
    }
    return true;
}

int slot_binary_char(uint8_t opcode) {
    switch (static_cast<skein::bc::Op>(opcode)) {
        case skein::bc::Op::Add: return '+';
        case skein::bc::Op::Sub: return '-';
        case skein::bc::Op::Mul: return '*';
        case skein::bc::Op::Div: return '/';
        case skein::bc::Op::Mod: return '%';
        case skein::bc::Op::BitAnd: return '&';
        case skein::bc::Op::BitOr: return '|';
        case skein::bc::Op::BitXor: return '^';
        case skein::bc::Op::Shl: return 's';
        case skein::bc::Op::Shr: return 'r';
        default: return 0;
    }
}

const skein::bc::Constant *constant_at(const skein::bc::Proto &proto,
                                       uint32_t index) {
    if (index >= proto.consts.size())
        return NULL;
    return &proto.consts[index];
}

} // namespace

value_t *assign_direct_param(const VmSlotView &view, uint32_t index,
                             value_t *value) {
    if (!value || index >= view.param_values->size())
        return value;
    if (value->type == T_return || value->type == T_break ||
        value->type == T_continue)
        return value;
    if (!check_direct_param(view, index, value)) {
        freeval(value);
        return NULL;
    }
    freeval((*view.param_values)[index]);
    (*view.param_values)[index] = value;
    return dupval(value);
}

value_t *compound_direct_param(const VmSlotView &view, uint32_t index,
                               int op, value_t *value) {
    if (!value || index >= view.param_values->size() ||
        !(*view.param_values)[index]) {
        freeval(value);
        yyerror("invalid bytecode parameter slot");
        return NULL;
    }
    value_t *result = apply_binary_value(op, (*view.param_values)[index],
                                         value);
    freeval(value);
    if (!result)
        return NULL;
    if (!check_direct_param(view, index, result)) {
        freeval(result);
        return NULL;
    }
    freeval((*view.param_values)[index]);
    (*view.param_values)[index] = result;
    return dupval(result);
}

value_t *incdec_direct_param(const VmSlotView &view, uint32_t index,
                             int delta, bool prefix) {
    if (index >= view.param_values->size() ||
        !(*view.param_values)[index]) {
        yyerror("invalid bytecode parameter slot");
        return NULL;
    }
    value_t *old = (*view.param_values)[index];
    value_t *delta_value = newintval(delta);
    value_t *result = apply_binary_value('+', old, delta_value);
    freeval(delta_value);
    if (!result)
        return NULL;
    if (!check_direct_param(view, index, result)) {
        freeval(result);
        return NULL;
    }
    value_t *ret = prefix ? dupval(result) : dupval(old);
    freeval(old);
    (*view.param_values)[index] = result;
    return ret;
}

value_t *assign_direct_slot(const VmSlotView &view, uint32_t index,
                            value_t *value) {
    if (!value || index >= view.slot_values->size())
        return value;
    if (value->type == T_return || value->type == T_break ||
        value->type == T_continue)
        return value;
    if (!check_direct_slot(view, index, value, true)) {
        freeval(value);
        return NULL;
    }
    freeval((*view.slot_values)[index]);
    (*view.slot_values)[index] = value;
    return dupval(value);
}

value_t *compound_direct_slot(const VmSlotView &view, uint32_t index,
                              int op, value_t *value) {
    if (!value || index >= view.slot_values->size() ||
        !(*view.slot_values)[index]) {
        freeval(value);
        yyerror("invalid bytecode slot");
        return NULL;
    }
    value_t *result = apply_binary_value(op, (*view.slot_values)[index],
                                         value);
    freeval(value);
    if (!result)
        return NULL;
    if (!check_direct_slot(view, index, result, true)) {
        freeval(result);
        return NULL;
    }
    freeval((*view.slot_values)[index]);
    (*view.slot_values)[index] = result;
    return dupval(result);
}

value_t *incdec_direct_slot(const VmSlotView &view, uint32_t index,
                            int delta, bool prefix) {
    if (index >= view.slot_values->size() ||
        !(*view.slot_values)[index]) {
        yyerror("invalid bytecode slot");
        return NULL;
    }
    value_t *old = (*view.slot_values)[index];
    value_t *delta_value = newintval(delta);
    value_t *result = apply_binary_value('+', old, delta_value);
    freeval(delta_value);
    if (!result)
        return NULL;
    if (!check_direct_slot(view, index, result, true)) {
        freeval(result);
        return NULL;
    }
    value_t *ret = prefix ? dupval(result) : dupval(old);
    freeval(old);
    (*view.slot_values)[index] = result;
    return ret;
}

value_t *binary_slot_fused_direct(const VmSlotView &view,
                                  const skein::bc::Instr &ins,
                                  bool constant_rhs) {
    const uint16_t left_index = ins.a;
    const uint16_t right_index =
        constant_rhs ? 0 : static_cast<uint16_t>(ins.b & 0xffffu);
    const uint16_t dest_index =
        constant_rhs ? ins.a : static_cast<uint16_t>(ins.b >> 16);
    if (left_index >= view.slot_values->size() ||
        dest_index >= view.slot_values->size() ||
        (!constant_rhs && right_index >= view.slot_values->size()) ||
        !(*view.slot_values)[left_index] ||
        (!constant_rhs && !(*view.slot_values)[right_index])) {
        yyerror("invalid bytecode slot");
        return NULL;
    }
    value_t *left = dupval((*view.slot_values)[left_index]);
    value_t *right = NULL;
    if (constant_rhs) {
        const skein::bc::Constant *constant = constant_at(*view.proto, ins.b);
        if (!constant ||
            constant->kind != skein::bc::ConstantKind::Value) {
            freeval(left);
            yyerror("invalid bytecode constant");
            return NULL;
        }
        right = constant->value ? dupval(constant->value.get())
                                : newnull();
    } else {
        right = dupval((*view.slot_values)[right_index]);
    }
    value_t *result =
        apply_binary_value(slot_binary_char(ins.flags), left, right);
    freeval(left);
    freeval(right);
    if (!result)
        return NULL;
    if (!check_direct_slot(view, dest_index, result, true)) {
        freeval(result);
        return NULL;
    }
    freeval((*view.slot_values)[dest_index]);
    (*view.slot_values)[dest_index] = result;
    return dupval(result);
}
