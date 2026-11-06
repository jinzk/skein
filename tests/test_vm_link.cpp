/*
 * Link-level guard: this executable links against skein_vm only (plus its
 * bytecode/value-runtime dependencies).  It must never depend on interpreter
 * or builtins symbols.
 */
#include "vm.h"
#include "vm_internal.h"

#include "bytecode.h"
#include "valuetype.h"

#include <memory>
#include <utility>

namespace {

struct value_deleter {
    void operator()(value_t *value) const { freeval(value); }
};

std::shared_ptr<value_t> own_value(value_t *value) {
    return std::shared_ptr<value_t>(value, value_deleter());
}

} // namespace

int main() {
    bool previous = skein_vm_enabled();
    skein_vm_set_enabled(true);
    bool enabled = skein_vm_enabled();
    skein_vm_set_enabled(false);
    bool disabled = skein_vm_enabled();
    skein_vm_set_enabled(previous);
#ifdef SKEIN_VM_ONLY
    if (!enabled || !disabled)
        return 1;
#else
    if (!enabled || disabled)
        return 1;
#endif

    using skein::bc::Constant;
    using skein::bc::Instr;
    using skein::bc::Op;
    using skein::bc::Proto;

    Proto proto;
    proto.max_stack = 1;
    Constant constant;
    constant.value = own_value(newintval(42));
    proto.consts.push_back(std::move(constant));

    Instr load;
    load.op = static_cast<uint8_t>(Op::LoadConst);
    proto.code.push_back(load);

    value_t *result = skein_vm_execute_proto(proto);
    bool ok = result && result->type == T_int &&
              ((int_value_t *)result)->value == 42;
    freeval(result);
    return ok ? 0 : 1;
}
