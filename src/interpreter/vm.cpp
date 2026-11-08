#include "vm.h"
#include "vm_internal.h"

#include "bytecode.h"
#include "diagnostics.h"
#include "dict.h"
#include "eval_context.h"
#include "eval_helpers.h"
#include "fiber.h"
#include "gc.h"
#include "gc_roots.h"
#include "package.h"
#include "pattern.h"
#include "runtime_state.h"
#include "scopestack.h"
#include "structeval.h"
#include "structtype.h"
#include "symbol.h"
#include "task.h"
#include "unicode.h"
#include "value_ops.h"
#include "value_runtime.h"
#include "valuetype.h"
#include "vm_eval.h"
#include "vm_frame.h"
#include "vm_gc_roots.h"
#include "vm_slots.h"
#include "vm_state.h"
#include "vm_host.h"
#include "vm_host_calls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

vm_stats_data vm_stats;

namespace {

using skein::bc::Constant;
using skein::bc::ConstantKind;
using skein::bc::Instr;
using skein::bc::NameCacheEntry;
using skein::bc::Op;
using skein::bc::Proto;
using skein::bc::ProtoPtr;
using skein::bc::SourceLoc;

thread_local int vm_enabled_state = -1;
thread_local int vm_max_depth_state = -1;
thread_local int vm_stats_state = -1;
thread_local bool vm_trace_state = false;
thread_local bool vm_trace_initialized = false;

int vm_max_depth() {
    if (vm_max_depth_state < 0) {
        const char *value = std::getenv("SKEIN_VM_MAX_DEPTH");
        int parsed = value ? std::atoi(value) : 0;
        vm_max_depth_state = parsed > 0 ? parsed : 1000;
    }
    return vm_max_depth_state;
}

struct vm_depth_guard {
    eval_context &context;
    explicit vm_depth_guard(eval_context &ctx) : context(ctx) {
        ++context.vm_call_depth;
    }
    ~vm_depth_guard() { --context.vm_call_depth; }
};

bool read_bool_env(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
        std::strcmp(value, "off") == 0 || std::strcmp(value, "no") == 0)
        return false;
    return true;
}

/*
 * These switches are consulted on every VM call.  Cache them: getenv is
 * cheap on POSIX but surprisingly expensive in the Windows CRT, where it
 * dominated faster call-heavy benchmarks.
 */
thread_local int vm_param_pool_state = -1;
thread_local int vm_slot_pool_state = -1;

bool vm_param_pool_enabled() {
    if (vm_param_pool_state < 0)
        vm_param_pool_state =
            read_bool_env("SKEIN_VM_PARAM_POOL", true) ? 1 : 0;
    return vm_param_pool_state != 0;
}

bool vm_slot_pool_enabled() {
    if (vm_slot_pool_state < 0)
        vm_slot_pool_state =
            read_bool_env("SKEIN_VM_SLOT_POOL", true) ? 1 : 0;
    return vm_slot_pool_state != 0;
}


void ensure_stats_flags() {
    if (vm_stats_state < 0)
        vm_stats_state = read_bool_env("SKEIN_VM_STATS", false) ? 1 : 0;
    vm_stats.enabled = vm_stats_state != 0;
}

void ensure_runtime_flags() {
    ensure_stats_flags();
#ifdef SKEIN_VM_ONLY
    vm_enabled_state = 1;
#else
    if (vm_enabled_state < 0)
        vm_enabled_state = read_bool_env("SKEIN_VM", true) ? 1 : 0;
#endif
    if (!vm_trace_initialized) {
        vm_trace_state = read_bool_env("SKEIN_VM_TRACE", false);
        vm_trace_initialized = true;
    }
}

const Constant *constant_at(const Proto &proto, uint32_t index) {
    if (index >= proto.consts.size())
        return NULL;
    return &proto.consts[index];
}

const char *name_at(const Proto &proto, uint32_t index) {
    const Constant *c = constant_at(proto, index);
    return c ? c->name.c_str() : "";
}

int pattern_dict_string_key_index(const dict_value_t *dict,
                                  const char *key) {
    if (!dict || !dict->payload || !key)
        return -1;
    for (size_t i = 0; i < dict->payload->items.size(); ++i) {
        value_t *item_key = dict->payload->items[i].first;
        if (item_key && item_key->type == T_string &&
            std::strcmp(((struct string_value_t *)item_key)->value, key) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

bool pattern_fields_have_key(
    const std::vector<skein::bc::PatternFieldDesc> &fields,
    const char *key) {
    for (const auto &field : fields) {
        if (!field.rest && field.key == key)
            return true;
    }
    return false;
}

void pop_scopes_to(size_t mark) {
    eval_context &context = active_eval_context();
    while (context.scopes.restore_stack.size() > mark)
        skein_vm_eval_popscope();
}

void free_stack(std::vector<value_t *> &stack) {
    for (value_t *value : stack)
        freeval(value);
    stack.clear();
}

struct vm_unwind {
    UnwindKind kind;
    value_t *value;
};

[[noreturn]] void throw_unwind(UnwindKind kind, value_t *value) {
    throw vm_unwind{kind, value};
}

value_t *execute_proto(
    const Proto &proto,
    const std::shared_ptr<function_def> &def = nullptr);

class Executor {
public:
    explicit Executor(const Proto &proto,
                      std::shared_ptr<function_def> def = nullptr)
        : proto_ptr_(&proto),
          context_(active_eval_context()),
          errors_before_(context_.runtime_error_count),
          scope_mark_(context_.scopes.restore_stack.size()),
          saved_handler_depth_(context_.handler_depth),
          current_def_(std::move(def)) {
        stack_.reserve(64);
        if (proto.uses_names)
            name_cache_.assign(proto.consts.size(), NameCacheSlot{});
        if (proto.uses_defers)
            defer_scopes_.emplace_back();
        if (proto.uses_params) {
            param_symbols_.reserve(proto.param_names.size());
            for (const std::string &name : proto.param_names)
                param_symbols_.push_back(lookupsym(name.c_str()));
        }
        direct_slots_ = !proto.slots.empty() && proto.slots_direct_safe &&
                        vm_slot_pool_enabled();
        if (direct_slots_) {
            slot_values_.assign(proto.slots.size(), NULL);
            slot_const_.assign(proto.slots.size(), 0);
        } else if (proto.uses_slots) {
            slot_symbols_.assign(proto.slots.size(), NULL);
        }
        gc_root_mark_ = gc_roots_push_visitor(
            context_.scopes, this, &Executor::gc_root_visit);
        gc_roots_registered_ = true;
    }

    ~Executor() {
        if (gc_roots_registered_)
            gc_roots_pop(context_.scopes, gc_root_mark_);
        if (!saved_states_.empty())
            unwind_fast_frames();
        free_stack(stack_);
        close_upvalues();
        for (value_t *value : param_values_)
            freeval(value);
        param_values_.clear();
        for (value_t *value : slot_values_)
            freeval(value);
        slot_values_.clear();
        for (iterator_state &state : iterators_) {
            freeval(state.iterable);
            state.iterable = NULL;
        }
        while (!handlers_.empty()) {
            value_t *error = end_handler_capture(handlers_.back());
            freeval(error);
            handlers_.pop_back();
        }
        for (std::vector<defer_record> &scope : defer_scopes_) {
            if (context_.vm_defer_count >= (int)scope.size())
                context_.vm_defer_count -= (int)scope.size();
            else
                context_.vm_defer_count = 0;
        }
        defer_scopes_.clear();
        for (struct symbol *sym : param_symbol_pool_)
            std::free(sym);
        param_symbol_pool_.clear();
        for (struct symbol *sym : slot_symbol_pool_)
            std::free(sym);
        slot_symbol_pool_.clear();
        context_.handler_depth = saved_handler_depth_;
    }

    value_t *run() {
        while (true) {
            if (pc_ >= proto().code.size()) {
                if (saved_states_.empty())
                    break;
                bool completed = false;
                value_t *abort =
                    fast_frame_return(newnull(), completed);
                if (!completed)
                    return abort;
                continue;
            }
            if (((++instruction_count_) & 1023u) == 0 && g_interrupted) {
                return abort_error();
            }
            if (gc_stress_enabled() &&
                ((++gc_stress_ticks_) & 4095u) == 0) {
                collect_cycles(context_.scopes);
            }

            try {
            const Instr ins = proto().code[pc_];
            if (vm_stats.enabled) {
                ++vm_stats.instructions;
                if (ins.op < vm_stats.opcodes.size())
                    ++vm_stats.opcodes[ins.op];
            }
            const SourceLoc loc =
                pc_ < proto().locs.size() ? proto().locs[pc_] : SourceLoc();
            ++pc_;
            if (loc.line > 0)
                diag_set_location(loc.line, loc.column);

            if (vm_trace_state) {
                std::fprintf(stderr,
                             "[vm] %s pc=%zu op=%s a=%u b=%u stack=%zu\n",
                             proto().name.empty() ? "<top>"
                                                 : proto().name.c_str(),
                             pc_ - 1, skein::bc::op_name(
                                          static_cast<Op>(ins.op)),
                             (unsigned)ins.a, ins.b, stack_.size());
            }

            switch (static_cast<Op>(ins.op)) {
                case Op::Nop:
                    break;

                case Op::LoadNull:
                    push(newnull());
                    break;

                case Op::LoadConst: {
                    const Constant *c = constant_at(proto(), ins.b);
                    if (!c) {
                        yyerror("invalid bytecode constant");
                        return abort_error();
                    }
                    if (c->kind == ConstantKind::Value && c->value)
                        push(dupval(c->value.get()));
                    else
                        push(newnull());
                    break;
                }

                case Op::LoadName: {
                    const char *name = name_at(proto(), ins.b);
                    struct symbol *sym = cached_symbol(ins.b);
                    if (!sym) {
                        yyerror("symbol not found %s", name);
                        return abort_error();
                    }
                    if (sym->is_uninitialized) {
                        yyerror("parameter %s is used before it is initialized",
                                name);
                        return abort_error();
                    }
                    push(dupval(sym->val));
                    break;
                }

                case Op::LoadGlobal: {
                    const char *name = name_at(proto(), ins.b);
                    struct symbol *sym = global_symbol(ins.a, ins.b);
                    if (!sym) {
                        yyerror("symbol not found %s", name);
                        return abort_error();
                    }
                    if (sym->is_uninitialized) {
                        yyerror("parameter %s is used before it is initialized",
                                name);
                        return abort_error();
                    }
                    push(dupval(sym->val));
                    break;
                }

                case Op::StoreName: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    struct symbol *sym = cached_symbol(ins.b);
                    if (!sym) {
                        yyerror("symbol not found %s",
                                name_at(proto(), ins.b));
                        freeval(value);
                        return abort_error();
                    }
                    value_t *result = skein_vm_assign_symbol_direct(sym, value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::CompoundName: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    struct symbol *sym = cached_symbol(ins.b);
                    if (!sym) {
                        yyerror("symbol not found %s",
                                name_at(proto(), ins.b));
                        freeval(value);
                        return abort_error();
                    }
                    value_t *result = skein_vm_compound_symbol_direct(
                        sym, (int)ins.flags, value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::IncDecName: {
                    struct symbol *sym = cached_symbol(ins.b);
                    if (!sym) {
                        yyerror("symbol not found %s",
                                name_at(proto(), ins.b));
                        return abort_error();
                    }
                    value_t *result = skein_vm_increment_symbol_direct(
                        sym, ins.a ? 1 : -1, (ins.flags & 1) != 0);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::StoreGlobal: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    struct symbol *sym = global_symbol(ins.a, ins.b);
                    if (!sym) {
                        yyerror("symbol not found %s",
                                name_at(proto(), ins.b));
                        freeval(value);
                        return abort_error();
                    }
                    value_t *result = skein_vm_assign_symbol_direct(sym, value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::CompoundGlobal: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    struct symbol *sym = global_symbol(ins.a, ins.b);
                    if (!sym) {
                        yyerror("symbol not found %s",
                                name_at(proto(), ins.b));
                        freeval(value);
                        return abort_error();
                    }
                    value_t *result = skein_vm_compound_symbol_direct(
                        sym, (int)ins.flags, value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::IncDecGlobal: {
                    struct symbol *sym = global_symbol(ins.a, ins.b);
                    if (!sym) {
                        yyerror("symbol not found %s",
                                name_at(proto(), ins.b));
                        return abort_error();
                    }
                    value_t *result = skein_vm_increment_symbol_direct(
                        sym, (ins.flags & 2) ? 1 : -1,
                        (ins.flags & 1) != 0);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::LoadParam: {
                    if (direct_params_) {
                        if (ins.b >= param_values_.size()) {
                            yyerror("invalid bytecode parameter slot");
                            return abort_error();
                        }
                        if (!param_values_[ins.b]) {
                            yyerror("parameter %s is used before it is "
                                    "initialized", param_name(ins.b));
                            return abort_error();
                        }
                        push(dupval(param_values_[ins.b]));
                        break;
                    }
                    if (ins.b >= param_symbols_.size()) {
                        yyerror("invalid bytecode parameter slot");
                        return abort_error();
                    }
                    struct symbol *sym = param_symbols_[ins.b];
                    if (!sym) {
                        yyerror("symbol not found %s",
                                ins.b < proto().param_names.size()
                                    ? proto().param_names[ins.b].c_str()
                                    : "");
                        return abort_error();
                    }
                    if (sym->is_uninitialized) {
                        yyerror("parameter %s is used before it is "
                                "initialized", sym->name);
                        return abort_error();
                    }
                    push(dupval(sym->val));
                    break;
                }

                case Op::StoreParam: {
                    if (direct_params_) {
                        value_t *value = pop_checked();
                        if (!value)
                            return abort_error();
                        value_t *result = assign_direct_param(ins.b, value);
                        if (!result)
                            return abort_error();
                        if (is_control(result))
                            return abort_control(result);
                        push(result);
                        break;
                    }
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (ins.b >= param_symbols_.size() ||
                        !param_symbols_[ins.b]) {
                        yyerror("invalid bytecode parameter slot");
                        freeval(value);
                        return abort_error();
                    }
                    value_t *result =
                        skein_vm_assign_symbol_direct(param_symbols_[ins.b], value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::CompoundParam: {
                    if (direct_params_) {
                        value_t *value = pop_checked();
                        if (!value)
                            return abort_error();
                        value_t *result = compound_direct_param(
                            ins.b, (int)ins.flags, value);
                        if (!result)
                            return abort_error();
                        if (is_control(result))
                            return abort_control(result);
                        push(result);
                        break;
                    }
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (ins.b >= param_symbols_.size() ||
                        !param_symbols_[ins.b]) {
                        yyerror("invalid bytecode parameter slot");
                        freeval(value);
                        return abort_error();
                    }
                    value_t *result = skein_vm_compound_symbol_direct(
                        param_symbols_[ins.b], (int)ins.flags, value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::IncDecParam: {
                    if (direct_params_) {
                        value_t *result = incdec_direct_param(
                            ins.b, ins.a ? 1 : -1,
                            (ins.flags & 1) != 0);
                        if (!result)
                            return abort_error();
                        if (is_control(result))
                            return abort_control(result);
                        push(result);
                        break;
                    }
                    if (ins.b >= param_symbols_.size() ||
                        !param_symbols_[ins.b]) {
                        yyerror("invalid bytecode parameter slot");
                        return abort_error();
                    }
                    value_t *result = skein_vm_increment_symbol_direct(
                        param_symbols_[ins.b], ins.a ? 1 : -1,
                        (ins.flags & 1) != 0);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::DefineSlot: {
                    if (ins.b >= proto().slots.size() ||
                        (!direct_slots_ && ins.b >= slot_symbols_.size())) {
                        yyerror("invalid bytecode slot");
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto().slots[ins.b];
                    value_t *value = NULL;
                    if (ins.flags & 1) {
                        value = pop_checked();
                        if (!value)
                            return abort_error();
                    } else {
                        value = skein_vm_default_value_for_type(&slot.type);
                        if (!value) {
                            yyerror(
                                "variable %s of type %s requires an "
                                "initializer", slot.name.c_str(),
                                type_constraint_name(&slot.type));
                            return abort_error();
                        }
                    }
                    if (is_control(value))
                        return abort_control(value);
                    if (isrepeated(slot.name.c_str())) {
                        yyerror("symbol repeated %s", slot.name.c_str());
                        freeval(value);
                        return abort_error();
                    }
                    if (!validate_type_constraint(&slot.type)) {
                        freeval(value);
                        return abort_error();
                    }
                    if (!type_constraint_matches(&slot.type, value)) {
                        yyerror("variable %s expects %s, got %s",
                                slot.name.c_str(),
                                type_constraint_name(&slot.type),
                                typename_of(value));
                        freeval(value);
                        return abort_error();
                    }
                    if (direct_slots_) {
                        if (ins.b >= slot_values_.size()) {
                            yyerror("invalid bytecode slot");
                            freeval(value);
                            return abort_error();
                        }
                        freeval(slot_values_[ins.b]);
                        slot_values_[ins.b] = value;
                        if (ins.b < slot_const_.size())
                            slot_const_[ins.b] = slot.is_const ? 1 : 0;
                        push(newnull());
                        break;
                    }
                    struct symbol *sym = NULL;
                    if (slot_pool_safe_)
                        sym = take_pooled_symbol(slot_symbol_pool_,
                                                 slot.name.c_str());
                    if (sym) {
                        reset_slot_symbol(sym, slot, value);
                    } else {
                        sym = newsym(slot.name.c_str());
                        sym->val = value;
                        type_constraint_assign(&sym->type, &slot.type);
                        sym->is_const = slot.is_const;
                    }
                    if (!slot_pool_safe_)
                        savesym(sym);
                    slot_symbols_[ins.b] = sym;
                    invalidate_name_cache(slot.name.c_str());
                    push(newnull());
                    break;
                }

                case Op::LoadSlot: {
                    if (ins.b >= proto().slots.size() ||
                        (!direct_slots_ && ins.b >= slot_symbols_.size())) {
                        yyerror("invalid bytecode slot");
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto().slots[ins.b];
                    if (direct_slots_) {
                        if (ins.b >= slot_values_.size() ||
                            !slot_values_[ins.b]) {
                            yyerror("symbol not found %s",
                                    slot.name.c_str());
                            return abort_error();
                        }
                        push(dupval(slot_values_[ins.b]));
                        break;
                    }
                    struct symbol *sym = slot_symbols_[ins.b];
                    if (!sym)
                        sym = lookupsym(slot.name.c_str());
                    if (!sym) {
                        yyerror("symbol not found %s", slot.name.c_str());
                        return abort_error();
                    }
                    if (sym->is_uninitialized) {
                        yyerror("parameter %s is used before it is "
                                "initialized", sym->name);
                        return abort_error();
                    }
                    push(dupval(sym->val));
                    break;
                }

                case Op::StoreSlot: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (ins.b >= proto().slots.size() ||
                        (!direct_slots_ && ins.b >= slot_symbols_.size())) {
                        yyerror("invalid bytecode slot");
                        freeval(value);
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto().slots[ins.b];
                    if (direct_slots_) {
                        value_t *result = assign_direct_slot(ins.b, value);
                        if (!result)
                            return abort_error();
                        if (is_control(result))
                            return abort_control(result);
                        push(result);
                        break;
                    }
                    value_t *result = NULL;
                    if (slot_symbols_[ins.b])
                        result = skein_vm_assign_symbol_direct(
                            slot_symbols_[ins.b], value);
                    else
                        result = skein_vm_assign_symbol_value(slot.name.c_str(),
                                                     value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::CompoundSlot: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (ins.b >= proto().slots.size() ||
                        (!direct_slots_ && ins.b >= slot_symbols_.size())) {
                        yyerror("invalid bytecode slot");
                        freeval(value);
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto().slots[ins.b];
                    if (direct_slots_) {
                        value_t *result = compound_direct_slot(
                            ins.b, (int)ins.flags, value);
                        if (!result)
                            return abort_error();
                        if (is_control(result))
                            return abort_control(result);
                        push(result);
                        break;
                    }
                    value_t *result = NULL;
                    if (slot_symbols_[ins.b])
                        result = skein_vm_compound_symbol_direct(
                            slot_symbols_[ins.b], (int)ins.flags, value);
                    else
                        result = skein_vm_compound_symbol_value(
                            slot.name.c_str(), (int)ins.flags, value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::IncDecSlot: {
                    if (ins.b >= proto().slots.size() ||
                        (!direct_slots_ && ins.b >= slot_symbols_.size())) {
                        yyerror("invalid bytecode slot");
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto().slots[ins.b];
                    if (direct_slots_) {
                        value_t *result = incdec_direct_slot(
                            ins.b, ins.a ? 1 : -1,
                            (ins.flags & 1) != 0);
                        if (!result)
                            return abort_error();
                        if (is_control(result))
                            return abort_control(result);
                        push(result);
                        break;
                    }
                    value_t *result = NULL;
                    if (slot_symbols_[ins.b])
                        result = skein_vm_increment_symbol_direct(
                            slot_symbols_[ins.b], ins.a ? 1 : -1,
                            (ins.flags & 1) != 0);
                    else
                        result = skein_vm_increment_symbol_value(
                            slot.name.c_str(), ins.a ? 1 : -1,
                            (ins.flags & 1) != 0);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::BinarySlots:
                case Op::BinarySlotConst: {
                    value_t *result = binary_slot_fused(
                        ins, static_cast<Op>(ins.op) ==
                                 Op::BinarySlotConst);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::ClearSlot:
                    if (direct_slots_) {
                        if (ins.b < slot_values_.size()) {
                            for (auto &cell : open_upvalues_) {
                                if (cell->slot == &slot_values_[ins.b])
                                    close_upvalue(*cell);
                            }
                            freeval(slot_values_[ins.b]);
                            slot_values_[ins.b] = NULL;
                            if (ins.b < slot_const_.size())
                                slot_const_[ins.b] = 0;
                        }
                        break;
                    }
                    if (ins.b < slot_symbols_.size()) {
                        struct symbol *sym = slot_symbols_[ins.b];
                        if (sym) {
                            for (auto &cell : open_upvalues_) {
                                if (cell->slot == &sym->val)
                                    close_upvalue(*cell);
                            }
                        }
                        if (sym && slot_pool_safe_ && !sym->scope_owned) {
                            freeval(sym->val);
                            sym->val = NULL;
                            sym->is_uninitialized = false;
                            if (slot_symbol_pool_.size() < 256)
                                slot_symbol_pool_.push_back(sym);
                            else
                                std::free(sym);
                        }
                        slot_symbols_[ins.b] = NULL;
                    }
                    break;

                case Op::LoadUpvalue: {
                    struct upvalue_cell *cell = upvalue_cell_at(ins.b);
                    if (cell) {
                        value_t *value = cell_value(cell);
                        if (!value) {
                            yyerror("invalid bytecode upvalue");
                            return abort_error();
                        }
                        push(dupval(value));
                        break;
                    }
                    struct symbol *sym = upvalue_fallback_symbol(ins.b);
                    if (!sym)
                        return abort_error();
                    if (sym->is_uninitialized) {
                        yyerror("parameter %s is used before it is initialized",
                                sym->name);
                        return abort_error();
                    }
                    push(dupval(sym->val));
                    break;
                }

                case Op::StoreUpvalue: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    struct upvalue_cell *cell = upvalue_cell_at(ins.b);
                    value_t *result = NULL;
                    if (cell) {
                        result = assign_upvalue_cell(cell, value);
                    } else {
                        struct symbol *sym = upvalue_fallback_symbol(ins.b);
                        if (!sym) {
                            freeval(value);
                            return abort_error();
                        }
                        result = skein_vm_assign_symbol_direct(sym, value);
                    }
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::CompoundUpvalue: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    struct upvalue_cell *cell = upvalue_cell_at(ins.b);
                    value_t *result = NULL;
                    if (cell) {
                        result = compound_upvalue_cell(
                            cell, (int)ins.flags, value);
                    } else {
                        struct symbol *sym = upvalue_fallback_symbol(ins.b);
                        if (!sym) {
                            freeval(value);
                            return abort_error();
                        }
                        result = skein_vm_compound_symbol_direct(
                            sym, (int)ins.flags, value);
                    }
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::IncDecUpvalue: {
                    struct upvalue_cell *cell = upvalue_cell_at(ins.b);
                    value_t *result = NULL;
                    if (cell) {
                        result = incdec_upvalue_cell(
                            cell, ins.a ? 1 : -1,
                            (ins.flags & 1) != 0);
                    } else {
                        struct symbol *sym = upvalue_fallback_symbol(ins.b);
                        if (!sym)
                            return abort_error();
                        result = skein_vm_increment_symbol_direct(
                            sym, ins.a ? 1 : -1, (ins.flags & 1) != 0);
                    }
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::CheckRedefine: {
                    const char *name = name_at(proto(), ins.b);
                    if (isrepeated(name)) {
                        yyerror("symbol repeated %s",
                                name ? name : "");
                        return abort_error();
                    }
                    break;
                }

                case Op::DefineName: {
                    if (ins.b >= proto().decls.size()) {
                        yyerror("invalid bytecode declaration");
                        return abort_error();
                    }
                    const skein::bc::DeclDesc &def = proto().decls[ins.b];
                    value_t *value = NULL;
                    if (ins.flags & 1) {
                        value = pop_checked();
                        if (!value)
                            return abort_error();
                    } else {
                        value = skein_vm_default_value_for_type(&def.type);
                        if (!value) {
                            yyerror(
                                "variable %s of type %s requires an "
                                "initializer",
                                def.name.c_str(),
                                type_constraint_name(&def.type));
                            return abort_error();
                        }
                    }

                    if (is_control(value))
                        return abort_control(value);

                    if (isrepeated(def.name.c_str())) {
                        yyerror("symbol repeated %s", def.name.c_str());
                        freeval(value);
                        return abort_error();
                    }
                    if (!validate_type_constraint(&def.type)) {
                        freeval(value);
                        return abort_error();
                    }
                    if (!type_constraint_matches(&def.type, value)) {
                        yyerror("variable %s expects %s, got %s",
                                def.name.c_str(),
                                type_constraint_name(&def.type),
                                typename_of(value));
                        freeval(value);
                        return abort_error();
                    }

                    struct symbol *sym = newsym(def.name.c_str());
                    sym->val = value;
                    type_constraint_assign(&sym->type, &def.type);
                    sym->exported = def.exported;
                    sym->is_const = def.is_const;
                    savesym(sym);
                    invalidate_name_cache(def.name.c_str());
                    push(newnull());
                    break;
                }

                case Op::Pop: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    freeval(value);
                    break;
                }

                case Op::Dup: {
                    if (stack_.empty()) {
                        yyerror("internal bytecode stack underflow");
                        return abort_error();
                    }
                    push(dupval(stack_.back()));
                    break;
                }

                case Op::Swap: {
                    if (stack_.size() < 2) {
                        yyerror("internal bytecode stack underflow");
                        return abort_error();
                    }
                    std::swap(stack_[stack_.size() - 1],
                              stack_[stack_.size() - 2]);
                    break;
                }

                case Op::Neg: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    value_t *result = NULL;
                    if (value->type == T_int) {
                        long long n = ((struct int_value_t *)value)->value;
                        result = new_int_checked(-(__int128)n);
                    } else if (value->type == T_float) {
                        result = newfloatval(
                            -((struct float_value_t *)value)->value);
                    } else {
                        yyerror("cannot negate %s", typename_of(value));
                    }
                    freeval(value);
                    if (!result)
                        return abort_error();
                    push(result);
                    break;
                }

                case Op::Not: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    value_t *result = newboolval(!truthy(value));
                    freeval(value);
                    push(result);
                    break;
                }

                case Op::BitNot: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    value_t *result = NULL;
                    if (value->type != T_int) {
                        yyerror("bitwise not expects an int, got %s",
                                typename_of(value));
                    } else {
                        result = newintval(
                            ~((struct int_value_t *)value)->value);
                    }
                    freeval(value);
                    if (!result)
                        return abort_error();
                    push(result);
                    break;
                }

                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::Div:
                case Op::Mod:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor:
                case Op::Shl:
                case Op::Shr:
                    if (!binary_op(ins.op))
                        return abort_error();
                    break;

                case Op::CmpEq:
                case Op::CmpNe:
                case Op::CmpLt:
                case Op::CmpLe:
                case Op::CmpGt:
                case Op::CmpGe:
                    if (!compare_op(ins.op))
                        return abort_error();
                    break;

                case Op::CmpJump: {
                    value_t *right = pop_checked();
                    value_t *left = pop_checked();
                    if (!left || !right) {
                        freeval(left);
                        freeval(right);
                        return abort_error();
                    }
                    const int op = ins.flags & 0x7f;
                    const bool jump_true = (ins.flags & 0x80) != 0;
                    bool value = false;
                    const bool valid =
                        compare_values(op, left, right, value);
                    freeval(left);
                    freeval(right);
                    if (!valid)
                        return abort_error();
                    if (jump_true ? value : !value)
                        pc_ = ins.b;
                    break;
                }

                case Op::IsType: {
                    const Constant *c = constant_at(proto(), ins.b);
                    if (!c || c->kind != ConstantKind::Type) {
                        yyerror("invalid bytecode type constant");
                        return abort_error();
                    }
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (!validate_type_constraint(c->type)) {
                        freeval(value);
                        return abort_error();
                    }
                    bool matched =
                        type_constraint_matches(c->type, value);
                    freeval(value);
                    push(newboolval(matched));
                    break;
                }

                case Op::SwitchCheck: {
                    if (stack_.empty()) {
                        yyerror("internal bytecode stack underflow");
                        return abort_error();
                    }
                    value_t *subject = stack_.back();
                    if (!skein_vm_switch_value_supported(subject)) {
                        yyerror("switch does not support %s",
                                typename_of(subject));
                        return abort_error();
                    }
                    break;
                }

                case Op::CaseMatch: {
                    value_t *values = pop_checked();
                    if (!values)
                        return abort_error();
                    if (stack_.empty()) {
                        freeval(values);
                        yyerror("internal bytecode stack underflow");
                        return abort_error();
                    }
                    value_t *subject = stack_.back();
                    if (values->type != T_list) {
                        yyerror("switch case values must be a list");
                        freeval(values);
                        return abort_error();
                    }
                    struct list_value_t *list =
                        (struct list_value_t *)values;
                    for (value_t *item : list->payload->items) {
                        if (!skein_vm_switch_value_supported(item)) {
                            yyerror("switch does not support %s case value",
                                    typename_of(item));
                            freeval(values);
                            return abort_error();
                        }
                    }
                    bool matched = false;
                    for (value_t *item : list->payload->items) {
                        if (value_equals(subject, item)) {
                            matched = true;
                            break;
                        }
                    }
                    freeval(values);
                    push(newboolval(matched));
                    break;
                }

                case Op::Jump:
                    pc_ = ins.b;
                    break;

                case Op::JumpIfFalse: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    bool truth = truthy(value);
                    freeval(value);
                    if (!truth)
                        pc_ = ins.b;
                    break;
                }

                case Op::JumpIfTrue: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    bool truth = truthy(value);
                    freeval(value);
                    if (truth)
                        pc_ = ins.b;
                    break;
                }

                case Op::NewList: {
                    uint32_t count = ins.b;
                    std::vector<value_t *> items(count);
                    for (uint32_t i = count; i > 0; --i) {
                        value_t *item = pop_checked();
                        if (!item) {
                            free_items(items);
                            return abort_error();
                        }
                        items[i - 1] = item;
                    }
                    struct list_value_t *list = newlistval();
                    for (value_t *item : items)
                        list->payload->items.push_back(item);
                    push((value_t *)list);
                    break;
                }

                case Op::NewDict: {
                    uint32_t count = ins.b;
                    std::vector<value_t *> items((size_t)count * 2);
                    for (uint32_t i = count * 2; i > 0; --i) {
                        value_t *item = pop_checked();
                        if (!item) {
                            free_items(items);
                            return abort_error();
                        }
                        items[i - 1] = item;
                    }
                    struct dict_value_t *dict = newdictval();
                    for (uint32_t i = 0; i < count; ++i) {
                        value_t *key = items[(size_t)i * 2];
                        value_t *value = items[(size_t)i * 2 + 1];
                        if (!dict_key_allowed(key)) {
                            yyerror("%s, got %s", dict_key_error(),
                                    typename_of(key));
                            freeval(key);
                            freeval(value);
                            free_items(items, (size_t)i * 2 + 2);
                            freeval((value_t *)dict);
                            return abort_error();
                        }
                        int found = dict_find_index(dict, key);
                        if (found >= 0) {
                            freeval(dict->payload->items[(size_t)found].second);
                            dict->payload->items[(size_t)found].second = value;
                            freeval(key);
                        } else {
                            dict_append_item(dict, key, value);
                        }
                    }
                    push((value_t *)dict);
                    break;
                }

                case Op::GetIndex:
                case Op::GetField: {
                    value_t *index = pop_checked();
                    value_t *container = pop_checked();
                    if (!index || !container) {
                        freeval(index);
                        freeval(container);
                        return abort_error();
                    }
                    value_t *result = skein_vm_read_member_value(
                        container, index, static_cast<Op>(ins.op) == Op::GetField);
                    freeval(container);
                    freeval(index);
                    if (!result)
                        return abort_error();
                    push(result);
                    break;
                }

                case Op::GetIndexCached: {
                    value_t *index = pop_checked();
                    value_t *container = pop_checked();
                    if (!index || !container) {
                        freeval(index);
                        freeval(container);
                        return abort_error();
                    }
                    skein::bc::IndexReadCacheEntry *cache = NULL;
                    if (ins.b < proto().index_read_ic.size())
                        cache = &proto().index_read_ic[ins.b];
                    value_t *result =
                        read_index_cached(container, index, cache);
                    freeval(container);
                    freeval(index);
                    if (!result)
                        return abort_error();
                    push(result);
                    break;
                }

                case Op::GetFieldName: {
                    value_t *container = pop_checked();
                    if (!container)
                        return abort_error();
                    value_t *result = read_field_cached(
                        container, name_at(proto(), ins.b),
                        member_cache(ins.b));
                    freeval(container);
                    if (!result)
                        return abort_error();
                    push(result);
                    break;
                }

                case Op::GetFieldOrEnum: {
                    if (ins.b >= proto().field_or_enum.size()) {
                        yyerror("invalid field-or-enum descriptor");
                        return abort_error();
                    }
                    const skein::bc::FieldOrEnumDesc &desc =
                        proto().field_or_enum[ins.b];
                    value_t *result = NULL;
                    struct symbol *base =
                        lookupsym(desc.base.c_str());
                    if (base) {
                        if (base->is_uninitialized) {
                            yyerror("parameter %s is used before it is "
                                    "initialized",
                                    desc.base.c_str());
                            return abort_error();
                        }
                        value_t *container = dupval(base->val);
                        if (!container)
                            return abort_error();
                        result = read_field_cached(
                            container, desc.field.c_str(), NULL);
                        freeval(container);
                    } else {
                        structtype_lookup_result lookup;
                        std::string error;
                        structtype_lookup_status status =
                            structtype_lookup_type(
                                nullptr, desc.module.c_str(),
                                desc.base.c_str(), lookup, error);
                        if (status == STRUCTTYPE_LOOKUP_FOUND &&
                            lookup.kind == STRUCTTYPE_LOOKUP_ENUM) {
                            std::vector<std::pair<std::string, value_t *>>
                                fields;
                            std::vector<bool> spread;
                            result = build_enum_value(
                                lookup.enumeration->module.c_str(),
                                lookup.enumeration->name.c_str(),
                                desc.field.c_str(), fields, spread);
                        } else if (status == STRUCTTYPE_LOOKUP_AMBIGUOUS ||
                                   status ==
                                       STRUCTTYPE_LOOKUP_NOT_VISIBLE ||
                                   status == STRUCTTYPE_LOOKUP_ERROR) {
                            yyerror("%s", error.c_str());
                        } else {
                            yyerror("symbol not found %s",
                                    desc.base.c_str());
                        }
                    }
                    if (!result)
                        return abort_error();
                    push(result);
                    break;
                }

                case Op::IterNew: {
                    uint32_t desc_index = ins.a;
                    if (desc_index >= proto().iters.size()) {
                        yyerror("invalid bytecode iterator");
                        return abort_error();
                    }
                    const skein::bc::IterDesc &desc =
                        proto().iters[desc_index];
                    value_t *iterable = pop_checked();
                    if (!iterable)
                        return abort_error();

                    if (iterators_.size() <= desc_index)
                        iterators_.resize(desc_index + 1);
                    iterator_state &state = iterators_[desc_index];
                    if (state.active) {
                        freeval(state.iterable);
                        state.iterable = NULL;
                        state.active = false;
                    }
                    state.key_sym = NULL;
                    state.value_sym = NULL;
                    state.direct = false;
                    state.key_slot = UINT32_MAX;
                    state.value_slot = UINT32_MAX;
                    state.index = 0;
                    state.count = 0;
                    state.generator = false;
                    state.two_vars = desc.two_vars;

                    if (fiber_is_generator(iterable)) {
                        if (desc.two_vars) {
                            yyerror("two-variable for-in requires dict, "
                                    "got generator");
                            freeval(iterable);
                            return abort_error();
                        }
                        state.generator = true;
                        state.iterable = iterable;
                    } else {
                        if (iterable->type == T_deque ||
                            iterable->type == T_heap ||
                            iterable->type == T_set) {
                            struct list_value_t *materialized =
                                newlistval();
                            std::vector<value_t *> items;
                            if (iterable->type == T_deque)
                                deque_items_in_order(
                                    ((struct deque_value_t *)iterable)
                                        ->payload.get(),
                                    items);
                            else if (iterable->type == T_heap)
                                heap_items_level_order(
                                    ((struct heap_value_t *)iterable)
                                        ->payload.get(),
                                    items);
                            else
                                set_values_in_order(
                                    ((struct set_value_t *)iterable)
                                        ->payload.get(),
                                    items);
                            materialized->payload->items.reserve(
                                items.size());
                            for (value_t *item : items)
                                materialized->payload->items.push_back(
                                    dupval(item));
                            freeval(iterable);
                            iterable = (value_t *)materialized;
                        }

                        if (iterable->type == T_list)
                            state.count = ((struct list_value_t *)iterable)
                                              ->payload->items.size();
                        else if (iterable->type == T_dict)
                            state.count = ((struct dict_value_t *)iterable)
                                              ->payload->items.size();
                        else if (iterable->type == T_string)
                            state.count = utf8_length(
                                ((struct string_value_t *)iterable)->value);
                        else if (iterable->type == T_bytes)
                            state.count =
                                ((struct bytes_value_t *)iterable)
                                    ->value.size();
                        else {
                            yyerror("cannot iterate %s",
                                    typename_of(iterable));
                            freeval(iterable);
                            return abort_error();
                        }

                        if (desc.two_vars && iterable->type != T_dict) {
                            yyerror("two-variable for-in requires dict, "
                                    "got %s", typename_of(iterable));
                            freeval(iterable);
                            return abort_error();
                        }
                        if (desc.two_vars &&
                            desc.key_name == desc.value_name) {
                            yyerror("loop variable repeated: %s",
                                    desc.key_name.c_str());
                            freeval(iterable);
                            return abort_error();
                        }

                        state.iterable = iterable;
                    }

                    state.direct = direct_slots_;
                    if (direct_slots_) {
                        state.key_slot = desc.key_slot;
                        state.value_slot = desc.value_slot;
                        if (state.key_slot < slot_values_.size()) {
                            freeval(slot_values_[state.key_slot]);
                            slot_values_[state.key_slot] = NULL;
                        }
                        if (desc.two_vars &&
                            state.value_slot < slot_values_.size()) {
                            freeval(slot_values_[state.value_slot]);
                            slot_values_[state.value_slot] = NULL;
                        }
                    } else {
                        struct symbol *key_sym =
                            newsym(desc.key_name.c_str());
                        key_sym->val = NULL;
                        savesym(key_sym);
                        state.key_sym = key_sym;
                        if (desc.key_slot != UINT32_MAX &&
                            desc.key_slot < slot_symbols_.size())
                            slot_symbols_[desc.key_slot] = key_sym;
                        if (desc.two_vars) {
                            struct symbol *value_sym =
                                newsym(desc.value_name.c_str());
                            value_sym->val = NULL;
                            savesym(value_sym);
                            state.value_sym = value_sym;
                            if (desc.value_slot != UINT32_MAX &&
                                desc.value_slot < slot_symbols_.size())
                                slot_symbols_[desc.value_slot] = value_sym;
                        }
                    }
                    state.active = true;
                    if (vm_stats.enabled &&
                        iterators_.size() > vm_stats.max_iterator_depth)
                        vm_stats.max_iterator_depth = iterators_.size();
                    invalidate_name_cache(desc.key_name.c_str());
                    if (desc.two_vars)
                        invalidate_name_cache(desc.value_name.c_str());
                    break;
                }

                case Op::IterNext: {
                    uint32_t desc_index = ins.a;
                    if (desc_index >= iterators_.size()) {
                        yyerror("invalid bytecode iterator");
                        return abort_error();
                    }
                    iterator_state &state = iterators_[desc_index];
                    if (!state.active ||
                        (!state.direct && !state.key_sym)) {
                        pc_ = ins.b;
                        break;
                    }

                    if (state.generator) {
                        generator_ptr generator;
                        if (!get_generator_value(state.iterable, generator)) {
                            return abort_error();
                        }
                        value_t *item = NULL;
                        bool done = false;
                        if (!generator_next_value(generator, NULL, item,
                                                  done)) {
                            freeval(item);
                            return abort_error();
                        }
                        if (done) {
                            freeval(item);
                            pc_ = ins.b;
                            break;
                        }
                        if (state.direct) {
                            if (state.key_slot < slot_values_.size()) {
                                freeval(slot_values_[state.key_slot]);
                                slot_values_[state.key_slot] = item;
                            } else {
                                freeval(item);
                            }
                        } else {
                            freeval(state.key_sym->val);
                            state.key_sym->val = item;
                        }
                        ++state.index;
                        break;
                    }

                    if (state.index >= state.count) {
                        pc_ = ins.b;
                        break;
                    }

                    value_t *item = NULL;
                    value_t *second = NULL;
                    size_t index = state.index;
                    if (state.iterable->type == T_list) {
                        item = dupval(
                            ((struct list_value_t *)state.iterable)
                                ->payload->items[index]);
                    } else if (state.iterable->type == T_dict) {
                        struct dict_value_t *dict =
                            (struct dict_value_t *)state.iterable;
                        item = dupval(
                            dict->payload->items[index].first);
                        if (state.two_vars)
                            second = dupval(
                                dict->payload->items[index].second);
                    } else if (state.iterable->type == T_string) {
                        std::string ch;
                        if (!utf8_char_at(
                                ((struct string_value_t *)state.iterable)
                                    ->value,
                                index, ch)) {
                            yyerror("invalid UTF-8 string iteration");
                            return abort_error();
                        }
                        size_t consumed = 0;
                        unsigned int codepoint = 0;
                        if (!utf8_decode_one(ch.data(), ch.size(),
                                             consumed, codepoint)) {
                            yyerror("invalid UTF-8 string iteration");
                            return abort_error();
                        }
                        item = newcharval(codepoint);
                    } else {
                        item = newintval((unsigned char)
                            ((struct bytes_value_t *)state.iterable)
                                ->value[index]);
                    }

                    if (state.direct) {
                        if (state.key_slot < slot_values_.size()) {
                            freeval(slot_values_[state.key_slot]);
                            slot_values_[state.key_slot] = item;
                        } else {
                            freeval(item);
                        }
                        if (state.two_vars &&
                            state.value_slot < slot_values_.size()) {
                            freeval(slot_values_[state.value_slot]);
                            slot_values_[state.value_slot] = second;
                        } else {
                            freeval(second);
                        }
                    } else {
                        freeval(state.key_sym->val);
                        state.key_sym->val = item;
                        if (state.two_vars && state.value_sym) {
                            freeval(state.value_sym->val);
                            state.value_sym->val = second;
                        } else {
                            freeval(second);
                        }
                    }
                    ++state.index;
                    break;
                }

                case Op::IterEnd: {
                    uint32_t desc_index = ins.a;
                    if (desc_index < iterators_.size()) {
                        iterator_state &state = iterators_[desc_index];
                        freeval(state.iterable);
                        state.iterable = NULL;
                        state.active = false;
                        state.direct = false;
                        state.key_slot = UINT32_MAX;
                        state.value_slot = UINT32_MAX;
                        state.key_sym = NULL;
                        state.value_sym = NULL;
                    }
                    break;
                }

                case Op::MatchBegin: {
                    if (stack_.empty()) {
                        yyerror("internal bytecode stack underflow");
                        return abort_error();
                    }
                    if (!match_exhaustive_desc(ins.b, stack_.back()))
                        return abort_error();
                    break;
                }

                case Op::MatchTest: {
                    value_t *subject = pop_checked();
                    if (!subject)
                        return abort_error();
                    std::vector<pattern_binding> bindings;
                    bool matched =
                        match_pattern_proto(proto(), ins.b, subject, bindings);
                    freeval(subject);
                    if (context_.runtime_error_count != errors_before_) {
                        free_pattern_bindings(bindings);
                        return abort_error();
                    }
                    if (!matched) {
                        free_pattern_bindings(bindings);
                        push(newboolval(false));
                        break;
                    }
                    for (pattern_binding &binding : bindings) {
                        if (direct_slots_) {
                            value_t *value = binding.value;
                            binding.value = NULL;
                            if (!assign_pattern_slot_direct(
                                    ins.b, binding.name, value, true)) {
                                freeval(value);
                                free_pattern_bindings(bindings);
                                return abort_error();
                            }
                            continue;
                        }
                        struct symbol *sym = newsym(binding.name.c_str());
                        sym->val = binding.value;
                        binding.value = NULL;
                        sym->is_const = true;
                        savesym(sym);
                        assign_pattern_slot(ins.b, binding.name, sym);
                    }
                    if (!direct_slots_) {
                        for (const pattern_binding &binding : bindings)
                            invalidate_name_cache(binding.name.c_str());
                    }
                    bindings.clear();
                    push(newboolval(true));
                    break;
                }

                case Op::MatchBind: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    std::vector<pattern_binding> bindings;
                    bool matched =
                        match_pattern_proto(proto(), ins.b, value, bindings);
                    if (context_.runtime_error_count != errors_before_) {
                        free_pattern_bindings(bindings);
                        freeval(value);
                        return abort_error();
                    }
                    if (!matched) {
                        free_pattern_bindings(bindings);
                        yyerror("destructuring pattern did not match %s",
                                typename_of(value));
                        freeval(value);
                        return abort_error();
                    }
                    for (pattern_binding &binding : bindings) {
                        if (isrepeated(binding.name.c_str())) {
                            yyerror("symbol repeated %s",
                                    binding.name.c_str());
                            free_pattern_bindings(bindings);
                            freeval(value);
                            return abort_error();
                        }
                    }
                    for (pattern_binding &binding : bindings) {
                        if (direct_slots_) {
                            value_t *bound = binding.value;
                            binding.value = NULL;
                            if (!assign_pattern_slot_direct(
                                    ins.b, binding.name, bound,
                                    (ins.flags & 1) != 0)) {
                                freeval(bound);
                                free_pattern_bindings(bindings);
                                freeval(value);
                                return abort_error();
                            }
                            continue;
                        }
                        struct symbol *sym = newsym(binding.name.c_str());
                        sym->val = binding.value;
                        binding.value = NULL;
                        sym->is_const = (ins.flags & 1) != 0;
                        savesym(sym);
                        assign_pattern_slot(ins.b, binding.name, sym);
                    }
                    if (!direct_slots_) {
                        for (const pattern_binding &binding : bindings)
                            invalidate_name_cache(binding.name.c_str());
                    }
                    bindings.clear();
                    freeval(value);
                    push(newnull());
                    break;
                }

                case Op::Closure: {
                    const Constant *c = constant_at(proto(), ins.b);
                    if (!c || c->kind != ConstantKind::Function ||
                        !c->function) {
                        yyerror("invalid bytecode function");
                        return abort_error();
                    }
                    if (!skein_vm_validate_function_params(
                            c->function->name.c_str(),
                            c->function->sl))
                        return abort_error();
                    std::shared_ptr<scope_t> closure = current_scope();
                    if (closure && closure->is_global)
                        closure.reset();
                    std::vector<std::shared_ptr<struct upvalue_cell>> upvalues;
                    upvalues.reserve(c->function->upvalues.size());
                    for (const std::string &name :
                         c->function->upvalues) {
                        upvalues.push_back(capture_upvalue(name));
                    }
                    push(newfuncval(c->function, closure,
                                    std::move(upvalues)));
                    break;
                }

                case Op::BindFunc: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    const char *name = name_at(proto(), ins.b);
                    if (isrepeated(name)) {
                        yyerror("symbol repeated %s", name);
                        freeval(value);
                        return abort_error();
                    }
                    struct symbol *sym = newsym(name);
                    sym->val = value;
                    sym->exported = (ins.flags & 1) != 0;
                    savesym(sym);
                    invalidate_name_cache(name);
                    push(newnull());
                    break;
                }

                case Op::DeclareStruct: {
                    if (ins.b >= proto().struct_decls.size()) {
                        yyerror("invalid bytecode struct declaration");
                        return abort_error();
                    }
                    const skein::bc::StructDeclMeta &meta =
                        proto().struct_decls[ins.b];
                    std::vector<field_meta> fields;
                    fields.reserve(meta.fields.size());
                    for (const auto &field : meta.fields) {
                        field_meta declared;
                        declared.name = field.name;
                        declared.type = field.type;
                        declared.default_code = field.default_code;
                        fields.push_back(std::move(declared));
                    }
                    std::shared_ptr<scope_t> closure = current_scope();
                    if (closure && closure->is_global)
                        closure.reset();
                    std::string error;
                    if (!declare_struct_meta(
                            meta.module.c_str(),
                            diag_current_module_name().c_str(),
                            meta.name.c_str(), meta.exported, fields,
                            closure, error))
                        return abort_error();
                    push(newnull());
                    break;
                }

                case Op::DeclareEnum: {
                    if (ins.b >= proto().enum_decls.size()) {
                        yyerror("invalid bytecode enum declaration");
                        return abort_error();
                    }
                    const skein::bc::EnumDeclMeta &meta =
                        proto().enum_decls[ins.b];
                    std::vector<variant_meta> variants;
                    variants.reserve(meta.variants.size());
                    for (const auto &variant : meta.variants) {
                        variant_meta declared;
                        declared.name = variant.name;
                        declared.fields.reserve(variant.fields.size());
                        for (const auto &field : variant.fields) {
                            field_meta field_meta_value;
                            field_meta_value.name = field.name;
                            field_meta_value.type = field.type;
                            field_meta_value.default_code =
                                field.default_code;
                            declared.fields.push_back(
                                std::move(field_meta_value));
                        }
                        variants.push_back(std::move(declared));
                    }
                    std::shared_ptr<scope_t> closure = current_scope();
                    if (closure && closure->is_global)
                        closure.reset();
                    std::string error;
                    if (!declare_enum_meta(
                            meta.module.c_str(),
                            diag_current_module_name().c_str(),
                            meta.name.c_str(), meta.exported, variants,
                            closure, error))
                        return abort_error();
                    push(newnull());
                    break;
                }

                case Op::DeclareTrait: {
                    if (ins.b >= proto().trait_decls.size()) {
                        yyerror("invalid bytecode trait declaration");
                        return abort_error();
                    }
                    const skein::bc::TraitDeclMeta &meta =
                        proto().trait_decls[ins.b];
                    std::vector<trait_method_meta> methods;
                    methods.reserve(meta.methods.size());
                    for (const auto &method : meta.methods) {
                        trait_method_meta method_meta;
                        method_meta.name = method.name;
                        method_meta.is_async = method.is_async;
                        method_meta.params.reserve(method.params.size());
                        for (const auto &param : method.params) {
                            trait_param_meta param_meta;
                            param_meta.name = param.name;
                            param_meta.type = param.type;
                            param_meta.variadic = param.variadic;
                            param_meta.has_default = param.has_default;
                            method_meta.params.push_back(
                                std::move(param_meta));
                        }
                        methods.push_back(std::move(method_meta));
                    }
                    std::string error;
                    if (!declare_trait_meta(
                            meta.module.c_str(),
                            diag_current_module_name().c_str(),
                            meta.name.c_str(), meta.exported, methods,
                            error))
                        return abort_error();
                    push(newnull());
                    break;
                }

                case Op::DeclareImpl: {
                    if (ins.b >= proto().impl_decls.size()) {
                        yyerror("invalid bytecode impl declaration");
                        return abort_error();
                    }
                    const skein::bc::ImplDeclMeta &meta =
                        proto().impl_decls[ins.b];
                    std::vector<std::pair<
                        std::string, std::shared_ptr<function_code>>>
                        methods;
                    methods.reserve(meta.methods.size());
                    for (const auto &method : meta.methods)
                        methods.emplace_back(method.name, method.code);
                    std::shared_ptr<scope_t> closure = current_scope();
                    if (closure && closure->is_global)
                        closure.reset();
                    std::string error;
                    if (!declare_impl_meta(
                            meta.module.c_str(),
                            diag_current_module_name().c_str(),
                            meta.type_name.c_str(), meta.has_trait,
                            meta.trait_name.c_str(), methods, closure,
                            error, [this](const std::string &name) {
                                return capture_upvalue(name);
                            }))
                        return abort_error();
                    push(newnull());
                    break;
                }

                case Op::NewStruct:
                case Op::NewEnum: {
                    if (ins.b >= proto().constructs.size()) {
                        yyerror("invalid bytecode constructor");
                        return abort_error();
                    }
                    const skein::bc::ConstructDesc &desc =
                        proto().constructs[ins.b];
                    std::vector<std::pair<std::string, value_t *>>
                        fields(desc.fields.size());
                    for (size_t i = desc.fields.size(); i > 0; --i) {
                        value_t *value = pop_checked();
                        if (!value) {
                            for (auto &field : fields)
                                freeval(field.second);
                            return abort_error();
                        }
                        fields[i - 1] =
                            std::make_pair(desc.fields[i - 1], value);
                    }
                    value_t *result =
                        static_cast<Op>(ins.op) == Op::NewStruct
                            ? build_struct_value(desc.module.c_str(),
                                                 desc.name.c_str(),
                                                 fields, desc.spread)
                            : build_enum_value(desc.module.c_str(),
                                               desc.name.c_str(),
                                               desc.variant.c_str(),
                                               fields, desc.spread);
                    if (!result)
                        return abort_error();
                    push(result);
                    break;
                }

                case Op::AssignIndex:
                case Op::CompoundIndex:
                case Op::IncDecIndex: {
                    if (ins.b >= proto().index_ops.size()) {
                        yyerror("invalid bytecode index descriptor");
                        return abort_error();
                    }
                    const skein::bc::IndexDesc &desc =
                        proto().index_ops[ins.b];
                    size_t count = desc.fields.size();
                    if (count == 0) {
                        yyerror("invalid bytecode index descriptor");
                        return abort_error();
                    }
                    std::vector<value_t *> indices(count);
                    for (size_t i = count; i > 0; --i) {
                        value_t *index = pop_checked();
                        if (!index) {
                            free_args(indices);
                            return abort_error();
                        }
                        indices[i - 1] = index;
                    }

                    value_t *result = NULL;
                    struct symbol *base = index_base_symbol(desc);
                    if (static_cast<Op>(ins.op) == Op::IncDecIndex) {
                        result = skein_vm_increment_index_symbol(
                            base, desc.base_name.c_str(),
                            ins.a ? 1 : -1, (ins.flags & 1) != 0,
                            indices, desc.fields);
                    } else {
                        value_t *value = pop_checked();
                        if (!value) {
                            free_args(indices);
                            return abort_error();
                        }
                        if (static_cast<Op>(ins.op) == Op::AssignIndex) {
                            result = skein_vm_assign_index_symbol(
                                base, desc.base_name.c_str(), indices,
                                desc.fields, value);
                        } else {
                            result = skein_vm_compound_index_symbol(
                                base, desc.base_name.c_str(),
                                (int)ins.flags, indices, desc.fields, value);
                        }
                    }
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::Await: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (!context_.allow_await) {
                        yyerror("await outside async function");
                        freeval(value);
                        return abort_error();
                    }
                    task_ptr task;
                    value_t *result = NULL;
                    if (!get_task_value(value, task)) {
                        freeval(value);
                        return abort_error();
                    }
                    result = fiber_await_task(task);
                    freeval(value);
                    if (context_.control != EVAL_CONTROL_NONE ||
                        context_.runtime_error_count != errors_before_) {
                        freeval(result);
                        return abort_error();
                    }
                    push(result ? result : newnull());
                    break;
                }

                case Op::Yield: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    value_t *result = fiber_yield_value(value);
                    if (context_.control != EVAL_CONTROL_NONE ||
                        context_.runtime_error_count != errors_before_) {
                        freeval(result);
                        return abort_error();
                    }
                    push(result ? result : newnull());
                    break;
                }

                case Op::Fail: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (context_.control != EVAL_CONTROL_NONE) {
                        freeval(value);
                        return abort_error();
                    }
                    raise_fail_value(value, "fail");
                    return abort_error();
                }

                case Op::Pass: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (handler_body_depth_ <= 0) {
                        yyerror("pass outside error handler");
                        freeval(value);
                        return abort_error();
                    }
                    if (context_.control != EVAL_CONTROL_NONE) {
                        freeval(value);
                        return abort_error();
                    }
                    raise_fail_value(value, "fail");
                    return abort_error();
                }

                case Op::Recover: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (!has_handler(UnwindKind::Recover)) {
                        yyerror("recover outside error handler");
                        freeval(value);
                        return abort_error();
                    }
                    throw_unwind(UnwindKind::Recover, value);
                }

                case Op::PushFailHandler:
                case Op::PushRecoverHandler: {
                    handler_state handler;
                    handler.kind =
                        static_cast<Op>(ins.op) == Op::PushFailHandler
                            ? UnwindKind::Fail
                            : UnwindKind::Recover;
                    handler.target = ins.b;
                    handler.stack_depth = stack_.size();
                    handler.scope_depth =
                        context_.scopes.restore_stack.size();
                    if (static_cast<Op>(ins.op) == Op::PushRecoverHandler &&
                        (ins.flags & 1) != 0 && handler.scope_depth > 0)
                        --handler.scope_depth;
                    handler.handler_body_depth = handler_body_depth_;
                    handler.eval_handler_depth = context_.handler_depth;
                    handler.capture =
                        static_cast<Op>(ins.op) == Op::PushFailHandler &&
                        (ins.flags & 1) != 0;
                    if (handler.capture)
                        skein_vm_eval_capture_begin(handler.snapshot);
                    handlers_.push_back(handler);
                    if (vm_stats.enabled &&
                        handlers_.size() > vm_stats.max_handler_depth)
                        vm_stats.max_handler_depth = handlers_.size();
                    break;
                }

                case Op::PopHandler: {
                    if (handlers_.empty()) {
                        yyerror("internal bytecode handler underflow");
                        return abort_error();
                    }
                    value_t *error =
                        end_handler_capture(handlers_.back());
                    freeval(error);
                    handlers_.pop_back();
                    break;
                }

                case Op::CheckResult: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (value->type != T_result) {
                        push(value);
                        break;
                    }
                    struct result_value_t *result =
                        (struct result_value_t *)value;
                    if (result->ok) {
                        value_t *inner = dupval(result->value);
                        freeval(value);
                        push(inner ? inner : newnull());
                        break;
                    }
                    value_t *error = dupval(result->error);
                    freeval(value);
                    if (!error)
                        error = newerrorval("fail", "");
                    if (has_handler(UnwindKind::Fail))
                        throw_unwind(UnwindKind::Fail, error);
                    skein_vm_eval_raise(error);
                    return abort_error();
                }

                case Op::BindName: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (direct_slots_ && ins.a != UINT16_MAX &&
                        ins.a < slot_values_.size()) {
                        freeval(slot_values_[ins.a]);
                        slot_values_[ins.a] = value;
                        if (ins.a < slot_const_.size())
                            slot_const_[ins.a] =
                                (ins.flags & 1) != 0 ? 1 : 0;
                        break;
                    }
                    const char *name = name_at(proto(), ins.b);
                    struct symbol *sym = newsym(name);
                    sym->val = value;
                    sym->is_const = (ins.flags & 1) != 0;
                    savesym(sym);
                    if (ins.a != UINT16_MAX &&
                        ins.a < slot_symbols_.size())
                        slot_symbols_[ins.a] = sym;
                    invalidate_name_cache(name);
                    break;
                }

                case Op::EnterHandlerBody:
                    ++handler_body_depth_;
                    ++context_.handler_depth;
                    break;

                case Op::LeaveHandlerBody:
                    if (handler_body_depth_ > 0)
                        --handler_body_depth_;
                    if (context_.handler_depth > saved_handler_depth_)
                        --context_.handler_depth;
                    break;

                case Op::DeferPush: {
                    if (ins.b >= proto().defer_protos.size() ||
                        defer_scopes_.empty()) {
                        yyerror("invalid bytecode defer descriptor");
                        return abort_error();
                    }
                    defer_record record;
                    record.proto = proto().defer_protos[ins.b];
                    if (ins.b < proto().defer_captures.size()) {
                        const std::vector<skein::bc::DeferCapture>
                            &captures = proto().defer_captures[ins.b];
                        record.upvalues.reserve(captures.size());
                        for (const skein::bc::DeferCapture &capture :
                             captures) {
                            std::shared_ptr<struct upvalue_cell> cell =
                                capture_defer_cell(capture);
                            if (!cell)
                                return abort_error();
                            record.upvalues.push_back(std::move(cell));
                        }
                    }
                    defer_scopes_.back().push_back(std::move(record));
                    ++context_.vm_defer_count;
                    if (vm_stats.enabled &&
                        (uint64_t)context_.vm_defer_count >
                            vm_stats.max_defer_depth)
                        vm_stats.max_defer_depth =
                            context_.vm_defer_count;
                    break;
                }

                case Op::DeclarePackage: {
                    if (ins.b >= proto().packages.size()) {
                        yyerror("invalid bytecode package descriptor");
                        return abort_error();
                    }
                    const skein::bc::PackageDesc &desc =
                        proto().packages[ins.b];
                    if (!desc.body) {
                        yyerror("invalid package body");
                        return abort_error();
                    }

                    int package_errors = context_.runtime_error_count;
                    std::string error;
                    if (package_is_loading())
                        skein_vm_eval_pushscope();
                    else
                        skein_vm_eval_push_isolated_scope();

                    std::string saved_module = diag_current_module_name();
                    std::string package_module_name =
                        (saved_module.empty() || saved_module == "main")
                            ? desc.name
                            : saved_module;
                    diag_set_module_name(package_module_name);
                    value_t *body_ret = execute_proto(*desc.body);
                    bool invalid_control =
                        body_ret && (body_ret->type == T_return ||
                                     body_ret->type == T_break ||
                                     body_ret->type == T_continue);
                    if (invalid_control) {
                        yyerror("invalid control flow in package %s",
                                desc.name.c_str());
                    }

                    value_t *ns = NULL;
                    if (context_.runtime_error_count == package_errors)
                        ns = package_build_namespace(current_scope().get(),
                                                     error);
                    skein_vm_eval_popscope();
                    diag_set_module_name(saved_module);
                    freeval(body_ret);

                    if (!ns) {
                        if (context_.runtime_error_count == package_errors) {
                            yyerror("package %s: %s", desc.name.c_str(),
                                    error.empty() ? "cannot build namespace"
                                                  : error.c_str());
                        }
                        return abort_error();
                    }
                    if (!package_publish(desc.name.c_str(), ns, error)) {
                        yyerror("package %s: %s", desc.name.c_str(),
                                error.c_str());
                        freeval(ns);
                        return abort_error();
                    }
                    push(newnull());
                    break;
                }

                case Op::Import: {
                    if (ins.a >= proto().imports.size()) {
                        yyerror("invalid bytecode import descriptor");
                        return abort_error();
                    }
                    const skein::bc::ImportDesc &desc =
                        proto().imports[ins.a];
                    std::string error;
                    int errors_before_import = diag_error_count();
                    value_t *ns = package_import(
                        desc.path.c_str(),
                        desc.base.empty() ? NULL : desc.base.c_str(),
                        error);
                    if (!ns) {
                        if (diag_error_count() == errors_before_import) {
                            yyerror("import %s: %s", desc.path.c_str(),
                                    error.c_str());
                        } else {
                            diag_mark_script_failure();
                        }
                        return abort_error();
                    }

                    const char *name =
                        desc.alias.empty()
                            ? skein_vm_import_binding_name(desc.path.c_str())
                            : desc.alias.c_str();
                    if (direct_slots_ && ins.b != UINT32_MAX &&
                        ins.b < slot_values_.size()) {
                        freeval(slot_values_[ins.b]);
                        slot_values_[ins.b] = ns;
                        if (ins.b < slot_const_.size())
                            slot_const_[ins.b] = 0;
                    } else {
                        if (isrepeated(name)) {
                            yyerror("import name already exists: %s", name);
                            freeval(ns);
                            return abort_error();
                        }

                        struct symbol *sym = newsym(name);
                        sym->val = ns;
                        savesym(sym);
                        if (ins.b != UINT32_MAX &&
                            ins.b < slot_symbols_.size())
                            slot_symbols_[ins.b] = sym;
                        invalidate_name_cache(name);
                    }

                    const std::string &path = desc.path;
                    if (!path.empty() && path[0] != '.' && path[0] != '@') {
                        const char *alias =
                            desc.alias.empty() ? NULL
                                               : desc.alias.c_str();
                        if (!alias) {
                            size_t dot = path.rfind('.');
                            alias = dot == std::string::npos
                                        ? path.c_str()
                                        : path.c_str() + dot + 1;
                        }
                        structtype_register_alias(
                            diag_current_source_name().c_str(), alias,
                            path.c_str());
                    }
                    push(newnull());
                    break;
                }

                case Op::CallSpread: {
                    if (ins.b >= proto().call_descs.size()) {
                        yyerror("invalid bytecode call descriptor");
                        return abort_error();
                    }
                    const skein::bc::CallDesc &desc =
                        proto().call_descs[ins.b];
                    size_t count = desc.spread.size();
                    std::vector<value_t *> raw(count);
                    for (size_t i = count; i > 0; --i) {
                        value_t *arg = pop_checked();
                        if (!arg) {
                            free_args(raw);
                            return abort_error();
                        }
                        raw[i - 1] = arg;
                    }

                    std::vector<value_t *> args;
                    args.reserve(count);
                    bool spread_failed = false;
                    for (size_t i = 0; i < count; ++i) {
                        value_t *value = raw[i];
                        raw[i] = NULL;
                        if (!desc.spread[i]) {
                            args.push_back(value);
                            continue;
                        }
                        if (!value || value->type != T_list) {
                            yyerror("spread expects a list, got %s",
                                    typename_of(value));
                            freeval(value);
                            free_args(args);
                            free_args(raw);
                            spread_failed = true;
                            break;
                        }
                        struct list_value_t *list =
                            (struct list_value_t *)value;
                        for (value_t *item : list->payload->items)
                            args.push_back(dupval(item));
                        freeval(value);
                    }
                    if (spread_failed)
                        return abort_error();

                    value_t *callee = pop_checked();
                    if (!callee) {
                        free_args(args);
                        free_args(raw);
                        return abort_error();
                    }
                    value_t *result =
                        skein_vm_call_value_args(callee, std::move(args));
                    freeval(callee);
                    if (context_.control != EVAL_CONTROL_NONE) {
                        freeval(result);
                        return abort_error();
                    }
                    if (context_.runtime_error_count != errors_before_) {
                        freeval(result);
                        return abort_error();
                    }
                    if (result && is_control(result))
                        return abort_control(result);
                    push(result ? result : newnull());
                    break;
                }

                case Op::Call: {
                    if (vm_stats.enabled)
                        ++vm_stats.calls;
                    uint32_t argc = ins.b;
                    std::array<value_t *, 8> inline_args;
                    std::vector<value_t *> heap_args;
                    value_t **args = inline_args.data();
                    if (argc > inline_args.size()) {
                        heap_args.resize(argc);
                        args = heap_args.data();
                    }
                    for (uint32_t i = argc; i > 0; --i) {
                        value_t *arg = pop_checked();
                        if (!arg) {
                            free_args(args, argc);
                            return abort_error();
                        }
                        args[i - 1] = arg;
                    }
                    value_t *callee = pop_checked();
                    if (!callee) {
                        free_args(args, argc);
                        return abort_error();
                    }
                    FastCallResult fast = try_fast_call(callee, args, argc);
                    if (fast == FastCallResult::Pushed) {
                        if (vm_stats.enabled) {
                            ++vm_stats.fast_calls;
                            if ((uint64_t)context_.vm_call_depth >
                                vm_stats.max_depth)
                                vm_stats.max_depth =
                                    context_.vm_call_depth;
                        }
                        freeval(callee);
                        break;
                    }
                    if (fast == FastCallResult::JitFailed) {
                        /* The JIT consumed the args before reporting. */
                        freeval(callee);
                        return abort_error();
                    }
                    if (fast == FastCallResult::Failed) {
                        freeval(callee);
                        free_args(args, argc);
                        return abort_error();
                    }
                    if (vm_stats.enabled)
                        ++vm_stats.fallback_calls;
                    std::vector<value_t *> fallback(args, args + argc);
                    value_t *result =
                        skein_vm_call_value_args(callee, std::move(fallback));
                    freeval(callee);
                    if (context_.control != EVAL_CONTROL_NONE) {
                        freeval(result);
                        return abort_error();
                    }
                    if (context_.runtime_error_count != errors_before_) {
                        freeval(result);
                        return abort_error();
                    }
                    if (result && is_control(result))
                        return abort_control(result);
                    push(result ? result : newnull());
                    break;
                }

                case Op::TailCall: {
                    if (vm_stats.enabled)
                        ++vm_stats.calls;
                    uint32_t argc = ins.b;
                    std::array<value_t *, 8> inline_args;
                    std::vector<value_t *> heap_args;
                    value_t **args = inline_args.data();
                    if (argc > inline_args.size()) {
                        heap_args.resize(argc);
                        args = heap_args.data();
                    }
                    for (uint32_t i = argc; i > 0; --i) {
                        value_t *arg = pop_checked();
                        if (!arg) {
                            free_args(args, argc);
                            return abort_error();
                        }
                        args[i - 1] = arg;
                    }
                    value_t *callee = pop_checked();
                    if (!callee) {
                        free_args(args, argc);
                        return abort_error();
                    }
                    if (try_tail_call(callee, args, argc)) {
                        if (vm_stats.enabled)
                            ++vm_stats.tail_calls;
                        freeval(callee);
                        break;
                    }
                    if (vm_stats.enabled)
                        ++vm_stats.fallback_calls;
                    std::vector<value_t *> fallback(args, args + argc);
                    value_t *result =
                        skein_vm_call_value_args(callee, std::move(fallback));
                    freeval(callee);
                    if (context_.control != EVAL_CONTROL_NONE) {
                        freeval(result);
                        return abort_error();
                    }
                    if (context_.runtime_error_count != errors_before_) {
                        freeval(result);
                        return abort_error();
                    }
                    if (result && is_control(result))
                        return abort_control(result);
                    if (!saved_states_.empty()) {
                        bool completed = false;
                        value_t *abort =
                            fast_frame_return(result, completed);
                        if (!completed)
                            return abort;
                        break;
                    }
                    if (!retval(result)) {
                        yyerror("return outside function");
                        freeval(result);
                        return abort_error();
                    }
                    return abort_control(newreturn());
                }

                case Op::Return: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (!saved_states_.empty()) {
                        bool completed = false;
                        value_t *abort =
                            fast_frame_return(value, completed);
                        if (!completed)
                            return abort;
                        break;
                    }
                    if (is_control(value))
                        return abort_control(value);
                    if (!retval(value)) {
                        yyerror("return outside function");
                        freeval(value);
                        return abort_error();
                    }
                    return abort_control(newreturn());
                }

                case Op::PushScope:
                    /*
                     * A fresh scope has no symbols yet, so it cannot shadow
                     * anything.  Cached outer-symbol pointers remain valid
                     * until a declaration in the new scope invalidates them.
                     */
                    skein_vm_eval_pushscope();
                    if (proto().uses_defers)
                        defer_scopes_.emplace_back();
                    break;

                case Op::PopScope: {
                    scope_t *scope = topscope();
                    bool had_symbols =
                        scope && !scope->syms.small.empty();
                    if (proto().uses_defers && !defer_scopes_.empty()) {
                        run_defer_scope(defer_scopes_.back());
                        defer_scopes_.pop_back();
                    }
                    close_scope_upvalues(scope);
                    skein_vm_eval_popscope();
                    if (had_symbols)
                        invalidate_name_cache();
                    if (context_.control != EVAL_CONTROL_NONE)
                        return abort_error();
                    if (context_.runtime_error_count != errors_before_)
                        return abort_error();
                    break;
                }

                case Op::ControlBreak:
                case Op::ControlContinue: {
                    const char *label = NULL;
                    if (ins.b != UINT32_MAX)
                        label = name_at(proto(), ins.b);
                    diag_set_flow_location(
                        loc.line > 0 ? loc.line : 0,
                        loc.column > 0 ? loc.column : 0);
                    value_t *control =
                        static_cast<Op>(ins.op) == Op::ControlBreak
                            ? newbreak(label)
                            : newcontinue(label);
                    return abort_control(control);
                }

                case Op::RuntimeError: {
                    const char *message = name_at(proto(), ins.b);
                    yyerror("%s", message);
                    return abort_error();
                }

                case Op::Count:
                    yyerror("invalid bytecode opcode");
                    return abort_error();
            }
            } catch (vm_unwind &unwind) {
                if (!dispatch_unwind(unwind)) {
                    discard_handlers();
                    run_all_defers();
                    close_upvalues();
                    free_stack(stack_);
                    recycle_params();
                    recycle_slots();
                    pop_scopes_to(scope_mark_);
                    return NULL;
                }
                continue;
            }
        }

        return finish();
    }

private:
    const Proto *proto_ptr_;
    eval_context &context_;
    int errors_before_;
    size_t scope_mark_;
    int saved_handler_depth_;
    std::vector<value_t *> stack_;
    std::vector<NameCacheSlot> name_cache_;
    std::vector<struct symbol *> param_symbols_;
    std::vector<value_t *> param_values_;
    std::vector<struct symbol> param_symbol_views_;
    std::vector<struct symbol *> slot_symbols_;
    std::vector<value_t *> slot_values_;
    std::vector<uint8_t> slot_const_;
    std::vector<struct symbol> slot_symbol_views_;
    std::vector<struct symbol> upvalue_symbol_views_;
    std::vector<std::shared_ptr<struct upvalue_cell>> open_upvalues_;
    std::vector<iterator_state> iterators_;
    std::vector<std::vector<defer_record>> defer_scopes_;
    std::vector<handler_state> handlers_;
    int handler_body_depth_ = 0;
    size_t pc_ = 0;
    size_t frame_stack_base_ = 0;
    size_t diag_frames_ = 0;
    uint64_t name_scope_generation_ = 0;
    std::vector<SavedVmState> saved_states_;
    std::shared_ptr<function_def> current_def_;
    bool param_pool_safe_ = false;
    bool slot_pool_safe_ = false;
    bool direct_params_ = false;
    bool direct_slots_ = false;
    bool gc_roots_registered_ = false;
    size_t gc_root_mark_ = 0;
    std::vector<struct symbol *> param_symbol_pool_;
    std::vector<struct symbol *> slot_symbol_pool_;
    unsigned instruction_count_ = 0;
    unsigned gc_stress_ticks_ = 0;

    /*
     * Fast calls move the caller's bookkeeping vectors into saved_states_.
     * Recycle the callee vectors so repeated calls at the same depth do not
     * allocate name/slot caches from scratch.
     */
    std::vector<std::vector<NameCacheSlot>> name_cache_pool_;
    std::vector<std::vector<struct symbol *>> param_symbols_pool_;
    std::vector<std::vector<value_t *>> param_values_pool_;
    std::vector<std::vector<struct symbol *>> slot_symbols_pool_;
    std::vector<std::vector<value_t *>> slot_values_pool_;

    const Proto &proto() const { return *proto_ptr_; }

    VmFrameView make_frame_view() {
        VmFrameView view;
        view.proto = &proto_ptr_;
        view.pc = &pc_;
        view.scope_mark = &scope_mark_;
        view.frame_stack_base = &frame_stack_base_;
        view.errors_before = &errors_before_;
        view.saved_handler_depth = &saved_handler_depth_;
        view.handler_body_depth = &handler_body_depth_;
        view.context = &context_;
        view.diag_frames = &diag_frames_;
        view.name_scope_generation = &name_scope_generation_;
        view.param_pool_safe = &param_pool_safe_;
        view.slot_pool_safe = &slot_pool_safe_;
        view.direct_params = &direct_params_;
        view.direct_slots = &direct_slots_;
        view.current_def = &current_def_;
        view.name_cache = &name_cache_;
        view.param_symbols = &param_symbols_;
        view.param_values = &param_values_;
        view.slot_symbols = &slot_symbols_;
        view.slot_values = &slot_values_;
        view.slot_const = &slot_const_;
        view.open_upvalues = &open_upvalues_;
        view.iterators = &iterators_;
        view.defer_scopes = &defer_scopes_;
        view.handlers = &handlers_;
        view.saved_states = &saved_states_;
        view.name_cache_pool = &name_cache_pool_;
        view.param_symbols_pool = &param_symbols_pool_;
        view.param_values_pool = &param_values_pool_;
        view.slot_symbols_pool = &slot_symbols_pool_;
        view.slot_values_pool = &slot_values_pool_;
        return view;
    }

    void save_current_state() {
        save_vm_frame(make_frame_view());
    }

    void restore_state(SavedVmState state) {
        restore_vm_frame(make_frame_view(), std::move(state));
    }

    void unwind_one_fast_frame() {
        if (saved_states_.empty())
            return;
        discard_handlers();
        run_all_defers();
        free_iterators();
        close_upvalues();
        while (stack_.size() > frame_stack_base_) {
            freeval(stack_.back());
            stack_.pop_back();
        }
        recycle_params();
        recycle_slots();
        pop_scopes_to(scope_mark_);
        while (diag_frames_ > 0) {
            diag_pop_call();
            --diag_frames_;
        }
        if (context_.vm_call_depth > 0)
            --context_.vm_call_depth;
        SavedVmState state = std::move(saved_states_.back());
        saved_states_.pop_back();
        restore_state(std::move(state));
    }

    void unwind_fast_frames() {
        while (!saved_states_.empty())
            unwind_one_fast_frame();
    }

    static bool proto_can_frame(const Proto &proto) {
        for (const Instr &ins : proto.code) {
            switch (static_cast<Op>(ins.op)) {
                case Op::Await:
                case Op::Yield:
                case Op::ControlBreak:
                case Op::ControlContinue:
                    return false;
                default:
                    break;
            }
        }
        return true;
    }

    enum class FastCallResult {
        Pushed,
        Fallback,
        Failed,
        /* The JIT ran the call and already consumed the argument pointers. */
        JitFailed
    };

    bool fast_call_target(value_t *callee,
                          value_t **args, size_t argc,
                          std::shared_ptr<function_def> &def,
                          ProtoPtr &proto) {
        if (!callee || callee->type != T_func)
            return false;
        struct func_value_t *fv = (struct func_value_t *)callee;
        def = fv->def;
        if (!def || !def->code)
            return false;
        function_code *code = def->code.get();
        if (code->is_generator || code->is_async || code->is_method ||
            code->variadic || code->required_params != code->nparams ||
            (int)argc != code->nparams)
            return false;

        int index = 0;
        for (struct symlist *sl = code->sl; sl; sl = sl->next, ++index) {
            if (sl->has_pattern || sl->variadic)
                return false;
            if (!type_constraint_matches(sl->type, args[(size_t)index]))
                return false;
        }

        proto = skein_vm_prepare_function_body(def);
        return proto && proto_can_frame(*proto);
    }

    static struct symbol *take_pooled_symbol(
        std::vector<struct symbol *> &pool, const char *name) {
        if (!name)
            return NULL;
        const size_t len = std::strlen(name);
        for (size_t i = pool.size(); i > 0; --i) {
            struct symbol *sym = pool[i - 1];
            if (!sym || sym->name_len != len ||
                std::memcmp(sym->name, name, len) != 0)
                continue;
            pool[i - 1] = pool.back();
            pool.pop_back();
            return sym;
        }
        return NULL;
    }

    struct symbol *take_param_symbol(const char *name) {
        return take_pooled_symbol(param_symbol_pool_, name);
    }

    static void reset_param_symbol(struct symbol *sym,
                                   struct symlist *sl,
                                   value_t *value) {
        sym->val = value;
        type_constraint_set_any(&sym->type);
        sym->exported = false;
        sym->is_const = false;
        sym->is_uninitialized = false;
        sym->scope_owned = false;
        sym->global_slot = UINT32_MAX;
        if (sl->type)
            type_constraint_assign(&sym->type, sl->type);
    }

    static void reset_slot_symbol(struct symbol *sym,
                                  const skein::bc::SlotDesc &slot,
                                  value_t *value) {
        sym->val = value;
        type_constraint_assign(&sym->type, &slot.type);
        sym->exported = false;
        sym->is_const = slot.is_const;
        sym->is_uninitialized = false;
        sym->scope_owned = false;
        sym->global_slot = UINT32_MAX;
    }

    struct symlist *param_meta(uint32_t index) {
        if (!current_def_ || !current_def_->code)
            return NULL;
        int i = 0;
        for (struct symlist *sl = current_def_->code->sl; sl;
             sl = sl->next, ++i) {
            if ((uint32_t)i == index)
                return sl;
        }
        return NULL;
    }

    const char *param_name(uint32_t index) {
        if (index < proto().param_names.size())
            return proto().param_names[index].c_str();
        struct symlist *sl = param_meta(index);
        return sl && sl->sym && sl->sym->name ? sl->sym->name : "parameter";
    }

    VmSlotView make_slot_view() {
        VmSlotView view;
        view.proto = &proto();
        view.current_def = current_def_.get();
        view.param_values = &param_values_;
        view.slot_values = &slot_values_;
        view.slot_const = &slot_const_;
        return view;
    }

    value_t *assign_direct_param(uint32_t index, value_t *value) {
        return ::assign_direct_param(make_slot_view(), index, value);
    }

    value_t *compound_direct_param(uint32_t index, int op, value_t *value) {
        return ::compound_direct_param(make_slot_view(), index, op, value);
    }

    value_t *incdec_direct_param(uint32_t index, int delta, bool prefix) {
        return ::incdec_direct_param(make_slot_view(), index, delta, prefix);
    }

    struct symbol *direct_param_symbol(uint32_t index) {
        if (index >= param_values_.size() ||
            index >= proto().param_names.size())
            return NULL;
        if (param_symbol_views_.size() <= index)
            param_symbol_views_.resize(index + 1);
        struct symbol &sym = param_symbol_views_[index];
        const std::string &name = proto().param_names[index];
        sym.name = const_cast<char *>(name.c_str());
        sym.name_len = name.size();
        sym.val = param_values_[index];
        type_constraint_set_any(&sym.type);
        sym.exported = false;
        sym.is_const = false;
        sym.is_uninitialized = false;
        sym.scope_owned = false;
        sym.global_slot = UINT32_MAX;
        return &sym;
    }

    void recycle_param_values() {
        for (value_t *value : param_values_)
            freeval(value);
        param_values_.clear();
    }

    void recycle_params() {
        if (direct_params_) {
            recycle_param_values();
            param_symbols_.clear();
        } else {
            recycle_param_symbols();
        }
    }

    void recycle_param_symbols() {
        if (!param_pool_safe_)
            return;
        for (struct symbol *sym : param_symbols_) {
            if (!sym)
                continue;
            freeval(sym->val);
            sym->val = NULL;
            sym->is_uninitialized = false;
            if (param_symbol_pool_.size() < 128)
                param_symbol_pool_.push_back(sym);
            else
                std::free(sym);
        }
        param_symbols_.clear();
    }

    struct symbol *direct_slot_symbol(uint32_t index) {
        if (index >= slot_values_.size() || index >= proto().slots.size())
            return NULL;
        if (slot_symbol_views_.size() <= index)
            slot_symbol_views_.resize(index + 1);
        struct symbol &sym = slot_symbol_views_[index];
        const std::string &name = proto().slots[index].name;
        sym.name = const_cast<char *>(name.c_str());
        sym.name_len = name.size();
        sym.val = slot_values_[index];
        type_constraint_set_any(&sym.type);
        sym.exported = false;
        sym.is_const = proto().slots[index].is_const ||
                       (index < slot_const_.size() && slot_const_[index]);
        sym.is_uninitialized = false;
        sym.scope_owned = false;
        sym.global_slot = UINT32_MAX;
        return &sym;
    }

    value_t *assign_direct_slot(uint32_t index, value_t *value) {
        return ::assign_direct_slot(make_slot_view(), index, value);
    }

    value_t *compound_direct_slot(uint32_t index, int op, value_t *value) {
        return ::compound_direct_slot(make_slot_view(), index, op, value);
    }

    value_t *incdec_direct_slot(uint32_t index, int delta, bool prefix) {
        return ::incdec_direct_slot(make_slot_view(), index, delta, prefix);
    }

    value_t *binary_slot_fused_direct(const Instr &ins,
                                      bool constant_rhs) {
        return ::binary_slot_fused_direct(make_slot_view(), ins,
                                          constant_rhs);
    }

    void recycle_slot_values() {
        for (value_t *value : slot_values_)
            freeval(value);
        slot_values_.clear();
    }

    void recycle_slots() {
        if (direct_slots_) {
            recycle_slot_values();
            slot_symbols_.clear();
        } else {
            recycle_slot_symbols();
        }
    }

    void recycle_slot_symbols() {
        if (!slot_pool_safe_) {
            slot_symbols_.clear();
            return;
        }
        for (struct symbol *&sym : slot_symbols_) {
            if (!sym)
                continue;
            if (sym->scope_owned) {
                sym = NULL;
                continue;
            }
            freeval(sym->val);
            sym->val = NULL;
            sym->is_uninitialized = false;
            if (slot_symbol_pool_.size() < 256)
                slot_symbol_pool_.push_back(sym);
            else
                std::free(sym);
            sym = NULL;
        }
        slot_symbols_.clear();
    }

    void bind_fast_arguments(function_code *code,
                             value_t **args, size_t argc) {
        (void)argc;
        if (direct_params_) {
            param_values_.clear();
            param_values_.reserve((size_t)code->nparams);
            for (int i = 0; i < code->nparams; ++i) {
                param_values_.push_back(args[(size_t)i]);
                args[(size_t)i] = NULL;
            }
            return;
        }
        int index = 0;
        for (struct symlist *sl = code->sl; sl; sl = sl->next, ++index) {
            struct symbol *sym =
                param_pool_safe_ ? take_param_symbol(sl->sym->name) : NULL;
            if (sym) {
                reset_param_symbol(sym, sl, args[(size_t)index]);
            } else {
                sym = newsym(sl->sym->name);
                sym->val = args[(size_t)index];
                if (sl->type)
                    type_constraint_assign(&sym->type, sl->type);
            }
            args[(size_t)index] = NULL;
            if (!param_pool_safe_)
                savesym(sym);
            param_symbols_.push_back(sym);
        }
    }

    FastCallResult try_fast_call(value_t *callee,
                                 value_t **args, size_t argc) {
#ifdef SKEIN_ENABLE_JIT
        if (skein_vm_host().fn_skein_vm_try_jit_call) {
            value_t *jit_result = NULL;
            if (skein_vm_try_jit_call(callee, args, (uint32_t)argc,
                                      &jit_result)) {
                if (!jit_result)
                    return FastCallResult::JitFailed;
                push(jit_result);
                return FastCallResult::Pushed;
            }
        }
#endif
        std::shared_ptr<function_def> def;
        ProtoPtr proto;
        if (!fast_call_target(callee, args, argc, def, proto))
            return FastCallResult::Fallback;
        function_code *code = def->code.get();

        int limit = vm_max_depth();
        if (context_.vm_call_depth >= limit) {
            yyerror("maximum recursion depth (%d) exceeded", limit);
            return FastCallResult::Failed;
        }

        const char *name = code->name.empty() ? "<anonymous>"
                                              : code->name.c_str();
        int line = g_error_line > 0 ? g_error_line : code->def_line;
        int column = g_error_line > 0 ? g_error_column : code->def_column;

        save_current_state();
        if (!code->module.empty())
            diag_set_module_name(code->module);
        if (!code->source.empty())
            diag_set_source_name(code->source);
        current_def_ = std::move(def);
        proto_ptr_ = proto.get();
        param_pool_safe_ = proto->params_direct &&
                           !proto->has_closures && !proto->uses_defers &&
                           vm_param_pool_enabled();
        direct_params_ = proto->params_direct &&
                         vm_param_pool_enabled();
        direct_slots_ = !proto->slots.empty() &&
                        proto->slots_direct_safe &&
                        vm_slot_pool_enabled();
        slot_pool_safe_ = proto->params_direct &&
                          !proto->has_closures && !proto->uses_defers &&
                          !proto->uses_handlers &&
                          vm_slot_pool_enabled();
        pc_ = 0;
        errors_before_ = context_.runtime_error_count;
        saved_handler_depth_ = context_.handler_depth;
        handler_body_depth_ = 0;
        context_.handler_depth = 0;
        context_.allow_await = false;
        scope_mark_ = context_.scopes.restore_stack.size();
        frame_stack_base_ = stack_.size();
        name_cache_.assign(proto->consts.size(), NameCacheSlot{});
        name_scope_generation_ = 0;
        param_symbols_.clear();
        param_symbols_.reserve((size_t)code->nparams);
        slot_symbols_.clear();
        if (direct_slots_) {
            slot_values_.assign(proto->slots.size(), NULL);
            slot_const_.assign(proto->slots.size(), 0);
        } else
            slot_symbols_.assign(proto->slots.size(), NULL);
        iterators_.clear();
        handlers_.clear();
        defer_scopes_.clear();
        if (proto->uses_defers)
            defer_scopes_.emplace_back();

        skein_vm_eval_push_closure_scope(current_def_->closure);
        bind_fast_arguments(code, args, argc);
        ++context_.vm_call_depth;
        diag_frames_ = 1;
        diag_push_call(name, line, column);
        return FastCallResult::Pushed;
    }

    bool try_tail_call(value_t *callee, value_t **args, size_t argc) {
        if (saved_states_.empty())
            return false;
        for (const std::vector<defer_record> &scope : defer_scopes_)
            if (!scope.empty())
                return false;
        if (!handlers_.empty() || !iterators_.empty() ||
            handler_body_depth_ != 0)
            return false;

        std::shared_ptr<function_def> def;
        ProtoPtr proto;
        if (!fast_call_target(callee, args, argc, def, proto))
            return false;
        if (stack_.size() != frame_stack_base_)
            return false;

        function_code *code = def->code.get();
        const char *name = code->name.empty() ? "<anonymous>"
                                              : code->name.c_str();
        int line = g_error_line > 0 ? g_error_line : code->def_line;
        int column = g_error_line > 0 ? g_error_column : code->def_column;

        close_upvalues();
        recycle_params();
        recycle_slots();
        pop_scopes_to(scope_mark_);

        current_def_ = std::move(def);
        proto_ptr_ = proto.get();
        param_pool_safe_ = proto->params_direct &&
                           !proto->has_closures && !proto->uses_defers &&
                           vm_param_pool_enabled();
        direct_params_ = proto->params_direct &&
                         vm_param_pool_enabled();
        direct_slots_ = !proto->slots.empty() &&
                        proto->slots_direct_safe &&
                        vm_slot_pool_enabled();
        slot_pool_safe_ = proto->params_direct &&
                          !proto->has_closures && !proto->uses_defers &&
                          !proto->uses_handlers &&
                          vm_slot_pool_enabled();
        pc_ = 0;
        errors_before_ = context_.runtime_error_count;
        handler_body_depth_ = 0;
        context_.handler_depth = 0;
        context_.allow_await = false;
        name_cache_.assign(proto->consts.size(), NameCacheSlot{});
        name_scope_generation_ = 0;
        param_symbols_.clear();
        param_symbols_.reserve((size_t)code->nparams);
        slot_symbols_.clear();
        if (direct_slots_) {
            slot_values_.assign(proto->slots.size(), NULL);
            slot_const_.assign(proto->slots.size(), 0);
        } else
            slot_symbols_.assign(proto->slots.size(), NULL);
        handlers_.clear();
        iterators_.clear();
        defer_scopes_.clear();
        if (proto->uses_defers)
            defer_scopes_.emplace_back();

        skein_vm_eval_push_closure_scope(current_def_->closure);
        bind_fast_arguments(code, args, argc);
        if (diag_frames_ < 64) {
            diag_push_call(name, line, column);
            ++diag_frames_;
        }
        return true;
    }

    void free_iterators() {
        for (iterator_state &state : iterators_) {
            freeval(state.iterable);
            state.iterable = NULL;
        }
        iterators_.clear();
    }

    /*
     * Complete the current fast frame.  Returns NULL on success; otherwise
     * returns the control/error result that run() must propagate.
     */
    value_t *fast_frame_return(value_t *value, bool &completed) {
        completed = false;
        if (is_control(value)) {
            freeval(value);
            return abort_control(value);
        }
        if (context_.runtime_error_count != errors_before_ ||
            context_.control != EVAL_CONTROL_NONE) {
            freeval(value);
            return abort_error();
        }
        discard_handlers();
        run_all_defers();
        free_iterators();
        if (context_.runtime_error_count != errors_before_ ||
            context_.control != EVAL_CONTROL_NONE) {
            freeval(value);
            return abort_error();
        }
        close_upvalues();
        while (stack_.size() > frame_stack_base_) {
            freeval(stack_.back());
            stack_.pop_back();
        }
        recycle_params();
        recycle_slots();
        pop_scopes_to(scope_mark_);
        while (diag_frames_ > 0) {
            diag_pop_call();
            --diag_frames_;
        }
        if (context_.vm_call_depth > 0)
            --context_.vm_call_depth;
        if (context_.runtime_error_count != errors_before_ ||
            context_.control != EVAL_CONTROL_NONE) {
            freeval(value);
            return abort_error();
        }
        SavedVmState state = std::move(saved_states_.back());
        saved_states_.pop_back();
        restore_state(std::move(state));
        push(value);
        completed = true;
        return NULL;
    }

    void push(value_t *value) {
        stack_.push_back(value);
        if (vm_stats.enabled && stack_.size() > vm_stats.max_stack)
            vm_stats.max_stack = stack_.size();
    }

    value_t *pop_checked() {
        if (stack_.size() <= frame_stack_base_) {
            yyerror("internal bytecode stack underflow");
            return NULL;
        }
        value_t *value = stack_.back();
        stack_.pop_back();
        return value;
    }

    value_t *read_index_cached(value_t *container, value_t *index,
                               skein::bc::IndexReadCacheEntry *cache) {
        if (container && container->type == T_dict) {
            struct dict_value_t *dict = (struct dict_value_t *)container;
            if (!dict_key_allowed(index)) {
                yyerror("%s, got %s", dict_key_error(),
                        typename_of(index));
                return NULL;
            }
            const auto &items = dict->payload->items;
            if (cache && cache->index >= 0 && cache->size == items.size() &&
                (size_t)cache->index < items.size() &&
                value_equals(items[(size_t)cache->index].first, index)) {
                if (vm_stats.enabled)
                    ++vm_stats.index_ic_hits;
                return dupval(items[(size_t)cache->index].second);
            }
            if (vm_stats.enabled)
                ++vm_stats.index_ic_misses;
            int found = dict_find_index(dict, index);
            if (found < 0) {
                yyerror("key not found");
                return NULL;
            }
            if (cache) {
                cache->size = items.size();
                cache->index = found;
            }
            return dupval(items[(size_t)found].second);
        }
        return skein_vm_read_member_value(container, index, false);
    }

    value_t *read_field_cached(value_t *container, const char *name,
                               skein::bc::MemberCacheEntry *cache) {
        if (!container || !name)
            return NULL;

        if (container->type == T_struct) {
            struct struct_value_t *sv = (struct struct_value_t *)container;
            if (!sv->def) {
                yyerror("invalid struct value");
                return NULL;
            }
            if (cache && cache->owner == sv->def) {
                if (cache->field >= 0 && sv->payload &&
                    (size_t)cache->field < sv->payload->fields.size()) {
                    if (vm_stats.enabled)
                        ++vm_stats.member_ic_hits;
                    return dupval(sv->payload->fields[(size_t)cache->field]);
                }
                if (cache->field < 0 && cache->method) {
                    if (vm_stats.enabled)
                        ++vm_stats.member_ic_hits;
                    return newmethodval(dupval(container), cache->method);
                }
            }
            if (vm_stats.enabled)
                ++vm_stats.member_ic_misses;
            int field = structtype_field_index(sv->def, name);
            if (field >= 0 && sv->payload &&
                (size_t)field < sv->payload->fields.size()) {
                if (cache) {
                    cache->owner = sv->def;
                    cache->variant = -1;
                    cache->field = field;
                    cache->method.reset();
                }
                return dupval(sv->payload->fields[(size_t)field]);
            }
            auto method = sv->def->methods.find(name);
            if (method != sv->def->methods.end()) {
                if (cache) {
                    cache->owner = sv->def;
                    cache->variant = -1;
                    cache->field = -1;
                    cache->method = method->second;
                }
                return newmethodval(dupval(container), method->second);
            }
            yyerror("field %s not found in %s", name, sv->def->name.c_str());
            return NULL;
        }

        if (container->type == T_enum) {
            struct enum_value_t *ev = (struct enum_value_t *)container;
            if (!ev->def || ev->variant < 0 ||
                (size_t)ev->variant >= ev->def->variants.size()) {
                yyerror("invalid enum value");
                return NULL;
            }
            const struct variant_def &variant =
                ev->def->variants[(size_t)ev->variant];
            if (cache && cache->owner == ev->def &&
                cache->variant == ev->variant) {
                if (cache->field >= 0 && ev->payload &&
                    (size_t)cache->field < ev->payload->fields.size()) {
                    if (vm_stats.enabled)
                        ++vm_stats.member_ic_hits;
                    return dupval(ev->payload->fields[(size_t)cache->field]);
                }
                if (cache->field < 0 && cache->method) {
                    if (vm_stats.enabled)
                        ++vm_stats.member_ic_hits;
                    return newmethodval(dupval(container), cache->method);
                }
            }
            if (vm_stats.enabled)
                ++vm_stats.member_ic_misses;
            int field = structtype_variant_field_index(&variant, name);
            if (field >= 0 && ev->payload &&
                (size_t)field < ev->payload->fields.size()) {
                if (cache) {
                    cache->owner = ev->def;
                    cache->variant = ev->variant;
                    cache->field = field;
                    cache->method.reset();
                }
                return dupval(ev->payload->fields[(size_t)field]);
            }
            auto method = ev->def->methods.find(name);
            if (method != ev->def->methods.end()) {
                if (cache) {
                    cache->owner = ev->def;
                    cache->variant = ev->variant;
                    cache->field = -1;
                    cache->method = method->second;
                }
                return newmethodval(dupval(container), method->second);
            }
            yyerror("field %s not found in %s.%s", name,
                    ev->def->name.c_str(), variant.name.c_str());
            return NULL;
        }

        /*
         * Dict namespaces, userdata and invalid member kinds keep the
         * generic path.  A stack string avoids allocating a key value.
         */
        struct string_value_t key;
        key.type = T_string;
        key.value = const_cast<char *>(name);
        return skein_vm_read_member_value(container, (struct value_t *)&key, true);
    }

    skein::bc::MemberCacheEntry *member_cache(uint32_t index) {
        if (proto().member_ic.size() < proto().consts.size())
            proto().member_ic.resize(proto().consts.size());
        if (index >= proto().member_ic.size())
            return NULL;
        return &proto().member_ic[index];
    }

    struct symbol *global_symbol(uint16_t slot, uint32_t name_index) {
        scope_context &scope = active_scope_context();
        const char *name = name_at(proto(), name_index);
        if (slot < scope.global_slots.size()) {
            struct symbol *sym = scope.global_slots[slot];
            if (sym && sym->name && std::strcmp(sym->name, name) == 0)
                return sym;
        }
        return cached_symbol(name_index);
    }

    struct symbol *global_symbol(uint16_t slot, const char *name) {
        scope_context &scope = active_scope_context();
        if (slot < scope.global_slots.size()) {
            struct symbol *sym = scope.global_slots[slot];
            if (sym && sym->name && name &&
                std::strcmp(sym->name, name) == 0)
                return sym;
        }
        return lookupsym(name);
    }

    struct symbol *index_base_symbol(const skein::bc::IndexDesc &desc) {
        using skein::bc::IndexBaseKind;
        switch (desc.base_kind) {
            case IndexBaseKind::Slot:
                if (direct_slots_)
                    return direct_slot_symbol(desc.base_index);
                if (desc.base_index < slot_symbols_.size())
                    return slot_symbols_[desc.base_index];
                break;
            case IndexBaseKind::Param:
                if (direct_params_)
                    return direct_param_symbol(desc.base_index);
                if (desc.base_index < param_symbols_.size())
                    return param_symbols_[desc.base_index];
                break;
            case IndexBaseKind::Upvalue:
                return upvalue_symbol_view(desc.base_index);
            case IndexBaseKind::Global:
                if (desc.base_index <= UINT16_MAX)
                    return global_symbol(
                        static_cast<uint16_t>(desc.base_index),
                        desc.base_name.c_str());
                break;
            case IndexBaseKind::Name:
            default:
                return lookupsym(desc.base_name.c_str());
        }
        return NULL;
    }

    struct symbol *cached_symbol(uint32_t index) {
        if (index < name_cache_.size() && name_cache_[index].sym &&
            name_cache_[index].generation == name_scope_generation_)
            return name_cache_[index].sym;

        scope_context &scope_context = active_scope_context();
        const char *name = name_at(proto(), index);
        const uint64_t generation = scope_global_generation();
        const void *owner = scope_context.global_scope.get();

        if (index < proto().name_ic.size()) {
            const NameCacheEntry &entry = proto().name_ic[index];
            if (entry.sym && entry.global_owner == owner &&
                entry.generation == generation) {
                if (vm_stats.enabled)
                    ++vm_stats.name_ic_hits;
                if (index < name_cache_.size())
                    name_cache_[index] =
                        NameCacheSlot{entry.sym, name_scope_generation_};
                return entry.sym;
            }
        }

        if (vm_stats.enabled)
            ++vm_stats.name_ic_misses;
        struct symbol *sym = lookupsym(name);
        if (sym && index < name_cache_.size())
            name_cache_[index] =
                NameCacheSlot{sym, name_scope_generation_};
        if (sym && scope_context.global_scope &&
            scope_context.global_scope->syms.find(name) == sym) {
            if (proto().name_ic.size() < proto().consts.size())
                proto().name_ic.resize(proto().consts.size());
            if (index < proto().name_ic.size())
                proto().name_ic[index] =
                    NameCacheEntry{sym, owner, generation};
        }
        return sym;
    }

    static value_t *cell_value(const struct upvalue_cell *cell) {
        if (!cell)
            return NULL;
        return cell->slot ? *cell->slot : cell->value;
    }

    static void close_upvalue(struct upvalue_cell &cell) {
        if (!cell.slot)
            return;
        cell.value = *cell.slot;
        *cell.slot = NULL;
        cell.slot = NULL;
    }

    void close_upvalues() {
        for (auto &cell : open_upvalues_)
            close_upvalue(*cell);
        open_upvalues_.clear();
    }

    void mark_gc_roots(gc_mark_fn mark, void *ctx) {
        VmRootView view;
        view.stack = &stack_;
        view.param_symbols = &param_symbols_;
        view.param_values = &param_values_;
        view.slot_symbols = &slot_symbols_;
        view.slot_values = &slot_values_;
        view.open_upvalues = &open_upvalues_;
        view.current_def = &current_def_;
        view.iterators = &iterators_;
        view.defer_scopes = &defer_scopes_;
        view.saved_states = &saved_states_;
        mark_vm_roots(view, mark, ctx);
    }

    static void gc_root_visit(void *ctx, gc_mark_fn mark,
                              void *mark_ctx) {
        static_cast<Executor *>(ctx)->mark_gc_roots(mark, mark_ctx);
    }

    /*
     * Symbols owned by a scope are freed when that scope pops.  Open
     * upvalue cells may still point at their storage (for example a defer
     * capture in a handler body), so detach them before the symbols go
     * away.
     */
    void close_scope_upvalues(scope_t *scope) {
        if (!scope)
            return;
        for (struct symbol *sym : scope->syms.small) {
            if (!sym)
                continue;
            for (auto &cell : open_upvalues_) {
                if (cell->slot == &sym->val)
                    close_upvalue(*cell);
            }
        }
    }

    std::shared_ptr<struct upvalue_cell> open_cell(
        value_t **storage, const struct type_constraint *type,
        const std::string &name, bool is_const) {
        for (auto &cell : open_upvalues_) {
            if (cell->slot == storage)
                return cell;
        }
        std::shared_ptr<struct upvalue_cell> cell =
            std::make_shared<struct upvalue_cell>();
        cell->slot = storage;
        cell->type = type;
        cell->name = name;
        cell->is_const = is_const;
        open_upvalues_.push_back(cell);
        return cell;
    }

    std::shared_ptr<struct upvalue_cell> capture_defer_cell(
        const skein::bc::DeferCapture &capture) {
        switch (capture.kind) {
            case skein::bc::DeferCaptureKind::Slot: {
                if (capture.index >= proto().slots.size()) {
                    yyerror("invalid defer slot capture %s",
                            capture.name.c_str());
                    return NULL;
                }
                const skein::bc::SlotDesc &slot =
                    proto().slots[capture.index];
                bool is_const =
                    slot.is_const ||
                    (capture.index < slot_const_.size() &&
                     slot_const_[capture.index]);
                if (direct_slots_)
                    return open_cell(&slot_values_[capture.index],
                                     &slot.type, capture.name, is_const);
                if (capture.index >= slot_symbols_.size() ||
                    !slot_symbols_[capture.index]) {
                    yyerror("local %s is not captured by a frame slot",
                            capture.name.c_str());
                    return NULL;
                }
                struct symbol *sym = slot_symbols_[capture.index];
                return open_cell(&sym->val, &sym->type, capture.name,
                                 sym->is_const);
            }
            case skein::bc::DeferCaptureKind::Param: {
                if (capture.index >= proto().param_names.size()) {
                    yyerror("invalid defer parameter capture %s",
                            capture.name.c_str());
                    return NULL;
                }
                if (direct_params_ &&
                    capture.index < param_values_.size()) {
                    struct symlist *sl = param_meta(capture.index);
                    return open_cell(&param_values_[capture.index],
                                     sl ? sl->type : NULL,
                                     capture.name, false);
                }
                if (capture.index >= param_symbols_.size() ||
                    !param_symbols_[capture.index]) {
                    yyerror("parameter %s is not captured by a frame slot",
                            capture.name.c_str());
                    return NULL;
                }
                struct symbol *sym = param_symbols_[capture.index];
                return open_cell(&sym->val, &sym->type, capture.name,
                                 sym->is_const);
            }
            case skein::bc::DeferCaptureKind::Upvalue:
                if (!current_def_ ||
                    capture.index >= current_def_->upvalues.size() ||
                    !current_def_->upvalues[capture.index]) {
                    yyerror("upvalue %s is not captured by a frame slot",
                            capture.name.c_str());
                    return NULL;
                }
                return current_def_->upvalues[capture.index];
        }
        yyerror("invalid defer capture %s", capture.name.c_str());
        return NULL;
    }

    bool frame_uses_symbol_cells() const {
        return !direct_slots_ || !direct_params_;
    }

    std::shared_ptr<struct upvalue_cell> capture_upvalue(
        const std::string &name) {
        if (current_def_) {
            for (size_t i = 0; i < proto().upvalues.size() &&
                               i < current_def_->upvalues.size();
                 ++i) {
                if (proto().upvalues[i] == name &&
                    current_def_->upvalues[i])
                    return current_def_->upvalues[i];
            }
        }
        if (direct_params_) {
            for (size_t i = 0; i < proto().param_names.size(); ++i) {
                if (proto().param_names[i] != name)
                    continue;
                struct symlist *sl = param_meta((uint32_t)i);
                return open_cell(&param_values_[i],
                                 sl ? sl->type : NULL, name, false);
            }
        }
        if (direct_slots_) {
            for (size_t i = 0; i < proto().slots.size(); ++i) {
                const skein::bc::SlotDesc &slot = proto().slots[i];
                if (slot.name != name)
                    continue;
                return open_cell(
                    &slot_values_[i], &slot.type, name,
                    slot.is_const ||
                        (i < slot_const_.size() && slot_const_[i]));
            }
        }
        struct symbol *sym = lookupsym(name.c_str());
        scope_context &scopes = active_scope_context();
        if (!sym || (scopes.global_scope &&
                     scopes.global_scope->syms.find(name.c_str()) == sym))
            return NULL;
        if (frame_uses_symbol_cells())
            return open_cell(&sym->val, &sym->type, name, sym->is_const);
        yyerror("local %s is not captured by a frame slot",
                name.c_str());
        return NULL;
    }

    struct upvalue_cell *upvalue_cell_at(uint32_t index) {
        if (index >= proto().upvalues.size()) {
            yyerror("invalid bytecode upvalue");
            return NULL;
        }
        if (!current_def_ || index >= current_def_->upvalues.size())
            return NULL;
        return current_def_->upvalues[index].get();
    }

    struct symbol *upvalue_symbol_view(uint32_t index) {
        if (index >= proto().upvalues.size()) {
            yyerror("invalid bytecode upvalue");
            return NULL;
        }
        struct upvalue_cell *cell = upvalue_cell_at(index);
        if (!cell) {
            const char *name = proto().upvalues[index].c_str();
            struct symbol *sym = name && *name ? lookupsym(name) : NULL;
            scope_context &scopes = active_scope_context();
            if (!sym) {
                yyerror("symbol not found %s", name ? name : "");
                return NULL;
            }
            if (!scopes.global_scope ||
                scopes.global_scope->syms.find(name) != sym) {
                if (frame_uses_symbol_cells())
                    return sym;
                yyerror("upvalue %s is not captured by a frame slot", name);
                return NULL;
            }
            return sym;
        }
        if (upvalue_symbol_views_.size() <= index)
            upvalue_symbol_views_.resize(index + 1);
        struct symbol &sym = upvalue_symbol_views_[index];
        sym.name = const_cast<char *>(proto().upvalues[index].c_str());
        sym.name_len = proto().upvalues[index].size();
        sym.val = cell_value(cell);
        if (cell->type)
            type_constraint_assign(&sym.type, cell->type);
        else
            type_constraint_set_any(&sym.type);
        sym.exported = false;
        sym.is_const = cell->is_const;
        sym.is_uninitialized = false;
        sym.scope_owned = false;
        sym.global_slot = UINT32_MAX;
        return &sym;
    }

    struct symbol *upvalue_fallback_symbol(uint32_t index) {
        if (index >= proto().upvalues.size()) {
            yyerror("invalid bytecode upvalue");
            return NULL;
        }
        const char *name = proto().upvalues[index].c_str();
        struct symbol *sym = name && *name ? lookupsym(name) : NULL;
        scope_context &scopes = active_scope_context();
        if (!sym) {
            yyerror("symbol not found %s", name ? name : "");
            return NULL;
        }
        if (!scopes.global_scope ||
            scopes.global_scope->syms.find(name) != sym) {
            if (frame_uses_symbol_cells())
                return sym;
            yyerror("upvalue %s is not captured by a frame slot", name);
            return NULL;
        }
        return sym;
    }

    value_t *assign_upvalue_cell(struct upvalue_cell *cell,
                                 value_t *value) {
        if (!cell || !value)
            return NULL;
        if (value->type == T_return || value->type == T_break ||
            value->type == T_continue)
            return value;
        if (cell->is_const) {
            yyerror("cannot assign to constant %s", cell->name.c_str());
            freeval(value);
            return NULL;
        }
        if (cell->type && !validate_type_constraint(cell->type)) {
            freeval(value);
            return NULL;
        }
        if (cell->type && !type_constraint_matches(cell->type, value)) {
            yyerror("variable %s expects %s, got %s", cell->name.c_str(),
                    type_constraint_name(cell->type), typename_of(value));
            freeval(value);
            return NULL;
        }
        freeval(cell_value(cell));
        if (cell->slot)
            *cell->slot = value;
        else
            cell->value = value;
        return dupval(value);
    }

    value_t *compound_upvalue_cell(struct upvalue_cell *cell, int op,
                                   value_t *value) {
        if (!cell || !value) {
            freeval(value);
            return NULL;
        }
        value_t *result = apply_binary_value(op, cell_value(cell), value);
        freeval(value);
        if (!result)
            return NULL;
        if (cell->is_const) {
            yyerror("cannot assign to constant %s", cell->name.c_str());
            freeval(result);
            return NULL;
        }
        if (cell->type && !validate_type_constraint(cell->type)) {
            freeval(result);
            return NULL;
        }
        if (cell->type && !type_constraint_matches(cell->type, result)) {
            yyerror("variable %s expects %s, got %s", cell->name.c_str(),
                    type_constraint_name(cell->type), typename_of(result));
            freeval(result);
            return NULL;
        }
        freeval(cell_value(cell));
        if (cell->slot)
            *cell->slot = result;
        else
            cell->value = result;
        return dupval(result);
    }

    value_t *incdec_upvalue_cell(struct upvalue_cell *cell, int delta,
                                 bool prefix) {
        if (!cell) {
            yyerror("invalid bytecode upvalue");
            return NULL;
        }
        value_t *old = cell_value(cell);
        if (!old) {
            yyerror("invalid bytecode upvalue");
            return NULL;
        }
        value_t *delta_value = newintval(delta);
        value_t *result = apply_binary_value('+', old, delta_value);
        freeval(delta_value);
        if (!result)
            return NULL;
        if (cell->is_const) {
            yyerror("cannot assign to constant %s", cell->name.c_str());
            freeval(result);
            return NULL;
        }
        if (cell->type && !validate_type_constraint(cell->type)) {
            freeval(result);
            return NULL;
        }
        if (cell->type && !type_constraint_matches(cell->type, result)) {
            yyerror("variable %s expects %s, got %s", cell->name.c_str(),
                    type_constraint_name(cell->type), typename_of(result));
            freeval(result);
            return NULL;
        }
        value_t *ret = prefix ? dupval(result) : dupval(old);
        freeval(old);
        if (cell->slot)
            *cell->slot = result;
        else
            cell->value = result;
        return ret;
    }

    void invalidate_name_cache() {
        /*
         * PopScope invalidates every symbol from the popped scope.  Bumping
         * the epoch makes those entries miss lazily instead of walking the
         * whole cache on every scope exit.
         */
        ++name_scope_generation_;
    }

    void invalidate_name_cache(const char *name) {
        if (!name || !*name) {
            invalidate_name_cache();
            return;
        }
        for (size_t i = 0; i < name_cache_.size(); ++i) {
            if (!name_cache_[i].sym)
                continue;
            if (i < proto().consts.size() &&
                proto().consts[i].kind == ConstantKind::Name &&
                proto().consts[i].name == name)
                name_cache_[i].sym = nullptr;
        }
    }

    static void free_items(std::vector<value_t *> &items, size_t start = 0) {
        for (size_t i = start; i < items.size(); ++i) {
            freeval(items[i]);
            items[i] = NULL;
        }
    }

    static void free_args(std::vector<value_t *> &args) {
        for (value_t *arg : args)
            freeval(arg);
        args.clear();
    }

    static void free_args(value_t **args, size_t argc) {
        for (size_t i = 0; i < argc; ++i)
            freeval(args[i]);
    }

    static bool is_control(const value_t *value) {
        return value && (value->type == T_return || value->type == T_break ||
                         value->type == T_continue);
    }

    bool assign_pattern_slot_direct(uint32_t pattern_index,
                                    const std::string &name,
                                    value_t *value, bool is_const) {
        if (pattern_index >= proto().patterns.size())
            return false;
        for (const auto &binding :
             proto().patterns[pattern_index].binding_slots) {
            if (binding.name == name &&
                binding.slot < slot_values_.size()) {
                freeval(slot_values_[binding.slot]);
                slot_values_[binding.slot] = value;
                if (binding.slot < slot_const_.size())
                    slot_const_[binding.slot] = is_const ? 1 : 0;
                return true;
            }
        }
        return false;
    }

    void assign_pattern_slot(uint32_t pattern_index,
                             const std::string &name,
                             struct symbol *sym) {
        if (pattern_index >= proto().patterns.size())
            return;
        for (const auto &binding :
             proto().patterns[pattern_index].binding_slots) {
            if (binding.name == name &&
                binding.slot < slot_symbols_.size()) {
                slot_symbols_[binding.slot] = sym;
                break;
            }
        }
    }

public:
    static bool match_pattern_proto(const Proto &proto, uint32_t index,
                                    value_t *value,
                                    std::vector<pattern_binding> &bindings) {
        if (index >= proto.patterns.size() || !value)
            return false;
        const skein::bc::PatternDesc &pattern = proto.patterns[index];

        switch (pattern.kind) {
            case skein::bc::PatternDesc::Kind::Literal: {
                value_t *literal = NULL;
                if (pattern.literal_value) {
                    literal = dupval(pattern.literal_value.get());
                } else if (pattern.literal_expr) {
                    literal = execute_proto(*pattern.literal_expr);
                }
                if (!literal)
                    return false;
                bool equal = value_equals(literal, value);
                freeval(literal);
                return equal;
            }
            case skein::bc::PatternDesc::Kind::Binding:
                return bind_pattern_value(bindings, pattern.name.c_str(),
                                          value);
            case skein::bc::PatternDesc::Kind::Type:
                return type_constraint_matches(&pattern.type, value);
            case skein::bc::PatternDesc::Kind::Or: {
                size_t mark = bindings.size();
                for (uint32_t item_index : pattern.items) {
                    if (match_pattern_proto(proto, item_index, value, bindings))
                        return true;
                    for (size_t i = mark; i < bindings.size(); ++i)
                        freeval(bindings[i].value);
                    bindings.resize(mark);
                }
                return false;
            }
            case skein::bc::PatternDesc::Kind::Range:
                return value->type == T_int &&
                       ((struct int_value_t *)value)->value >=
                           pattern.range_start &&
                       ((struct int_value_t *)value)->value <=
                           pattern.range_end;
            case skein::bc::PatternDesc::Kind::As:
                if (!match_pattern_proto(proto, pattern.child, value, bindings))
                    return false;
                return bind_pattern_value(bindings, pattern.name.c_str(),
                                          value);
            case skein::bc::PatternDesc::Kind::Rest:
                yyerror("rest pattern is only allowed at the end of a "
                        "list or dict pattern");
                return false;
            case skein::bc::PatternDesc::Kind::Never:
                return false;
            case skein::bc::PatternDesc::Kind::List: {
                if (value->type != T_list)
                    return false;
                struct list_value_t *list = (struct list_value_t *)value;
                size_t item_index = 0;
                for (size_t pattern_index_pos = 0;
                     pattern_index_pos < pattern.items.size();
                     ++pattern_index_pos) {
                    uint32_t pattern_index =
                        pattern.items[pattern_index_pos];
                    const skein::bc::PatternDesc &item =
                        proto.patterns[pattern_index];
                    if (item.kind ==
                        skein::bc::PatternDesc::Kind::Rest) {
                        if (pattern_index_pos + 1 !=
                            pattern.items.size()) {
                            yyerror("rest pattern must be last");
                            return false;
                        }
                        struct list_value_t *rest = newlistval();
                        while (item_index < list->payload->items.size())
                            rest->payload->items.push_back(
                                dupval(list->payload->items[item_index++]));
                        bool ok = bind_pattern_value(
                            bindings, item.name.c_str(),
                            (struct value_t *)rest);
                        freeval((struct value_t *)rest);
                        return ok;
                    }
                    if (item_index >= list->payload->items.size())
                        return false;
                    if (!match_pattern_proto(
                            proto, pattern_index,
                            list->payload->items[item_index], bindings))
                        return false;
                    ++item_index;
                }
                return item_index == list->payload->items.size();
            }
            case skein::bc::PatternDesc::Kind::Container: {
                if (!pattern.variant.empty()) {
                    if (value->type != T_enum)
                        return false;
                    struct enum_value_t *instance =
                        (struct enum_value_t *)value;
                    enum_def *def = structtype_find_enum(
                        pattern.module.c_str(), pattern.type_name.c_str());
                    if (!def || instance->def != def)
                        return false;
                    int variant_index = structtype_variant_index(
                        def, pattern.variant.c_str());
                    if (variant_index < 0) {
                        yyerror("unknown enum variant %s.%s",
                                def->name.c_str(),
                                pattern.variant.c_str());
                        return false;
                    }
                    if (instance->variant != variant_index)
                        return false;
                    const variant_def &variant =
                        def->variants[(size_t)variant_index];
                    std::vector<bool> seen(variant.fields.size(), false);
                    for (const auto &field : pattern.fields) {
                        if (field.rest)
                            continue;
                        int field_index =
                            structtype_variant_field_index(
                                &variant, field.key.c_str());
                        if (field_index < 0) {
                            yyerror("unknown field %s in %s.%s",
                                    field.key.c_str(), def->name.c_str(),
                                    variant.name.c_str());
                            return false;
                        }
                        if (seen[(size_t)field_index]) {
                            yyerror("field %s repeated in pattern",
                                    field.key.c_str());
                            return false;
                        }
                        seen[(size_t)field_index] = true;
                        if (!match_pattern_proto(proto,
                                field.pattern,
                                instance->payload
                                    ->fields[(size_t)field_index],
                                bindings))
                            return false;
                    }
                    for (const auto &field : pattern.fields) {
                        if (!field.rest)
                            continue;
                        if (!field.rest_name.empty()) {
                            struct dict_value_t *rest = newdictval();
                            for (size_t i = 0; i < variant.fields.size();
                                 ++i) {
                                if (seen[i])
                                    continue;
                                rest->payload->items.push_back(
                                    std::make_pair(
                                        newstrval(variant.fields[i]
                                                      .name.c_str()),
                                        dupval(instance->payload
                                                   ->fields[i])));
                            }
                            bool ok = bind_pattern_value(
                                bindings, field.rest_name.c_str(),
                                (struct value_t *)rest);
                            freeval((struct value_t *)rest);
                            if (!ok)
                                return false;
                        }
                        break;
                    }
                    return true;
                }

                if (!pattern.type_name.empty()) {
                    if (value->type != T_struct)
                        return false;
                    struct struct_value_t *instance =
                        (struct struct_value_t *)value;
                    struct_def *def = structtype_find_struct(
                        pattern.module.c_str(), pattern.type_name.c_str());
                    if (!def || instance->def != def)
                        return false;
                    std::vector<bool> seen(def->fields.size(), false);
                    for (const auto &field : pattern.fields) {
                        if (field.rest)
                            continue;
                        int field_index = structtype_field_index(
                            def, field.key.c_str());
                        if (field_index < 0) {
                            yyerror("unknown field %s in %s",
                                    field.key.c_str(), def->name.c_str());
                            return false;
                        }
                        if (seen[(size_t)field_index]) {
                            yyerror("field %s repeated in pattern",
                                    field.key.c_str());
                            return false;
                        }
                        seen[(size_t)field_index] = true;
                        if (!match_pattern_proto(proto,
                                field.pattern,
                                instance->payload
                                    ->fields[(size_t)field_index],
                                bindings))
                            return false;
                    }
                    for (const auto &field : pattern.fields) {
                        if (!field.rest)
                            continue;
                        if (!field.rest_name.empty()) {
                            struct dict_value_t *rest = newdictval();
                            for (size_t i = 0; i < def->fields.size(); ++i) {
                                if (seen[i])
                                    continue;
                                rest->payload->items.push_back(
                                    std::make_pair(
                                        newstrval(
                                            def->fields[i].name.c_str()),
                                        dupval(instance->payload
                                                   ->fields[i])));
                            }
                            bool ok = bind_pattern_value(
                                bindings, field.rest_name.c_str(),
                                (struct value_t *)rest);
                            freeval((struct value_t *)rest);
                            if (!ok)
                                return false;
                        }
                        break;
                    }
                    return true;
                }

                if (value->type != T_dict)
                    return false;
                struct dict_value_t *dict = (struct dict_value_t *)value;
                for (const auto &field : pattern.fields) {
                    if (field.rest)
                        continue;
                    int field_index = pattern_dict_string_key_index(
                        dict, field.key.c_str());
                    if (field_index < 0)
                        return false;
                    if (!match_pattern_proto(proto,
                            field.pattern,
                            dict->payload->items[(size_t)field_index].second,
                            bindings))
                        return false;
                }
                for (const auto &field : pattern.fields) {
                    if (!field.rest)
                        continue;
                    if (!field.rest_name.empty()) {
                        struct dict_value_t *rest = newdictval();
                        for (const auto &entry : dict->payload->items) {
                            bool listed = false;
                            if (entry.first &&
                                entry.first->type == T_string) {
                                listed = pattern_fields_have_key(
                                    pattern.fields,
                                    ((struct string_value_t *)entry.first)
                                        ->value);
                            }
                            if (!listed) {
                                rest->payload->items.push_back(
                                    std::make_pair(
                                        dupval(entry.first),
                                        dupval(entry.second)));
                            }
                        }
                        bool ok = bind_pattern_value(
                            bindings, field.rest_name.c_str(),
                            (struct value_t *)rest);
                        freeval((struct value_t *)rest);
                        if (!ok)
                            return false;
                    }
                    break;
                }
                return true;
            }
        }
        return false;
    }

private:
    bool pattern_covers_all_desc(enum_def *def, value_t *subject,
                                 uint32_t index) {
        if (index >= proto().patterns.size())
            return false;
        const skein::bc::PatternDesc &pattern = proto().patterns[index];
        if (pattern.kind == skein::bc::PatternDesc::Kind::As)
            return pattern_covers_all_desc(def, subject, pattern.child);
        if (pattern.kind == skein::bc::PatternDesc::Kind::Binding)
            return true;
        if (pattern.kind == skein::bc::PatternDesc::Kind::Type &&
            type_constraint_matches(&pattern.type, subject))
            return true;
        if (pattern.kind == skein::bc::PatternDesc::Kind::Or) {
            for (uint32_t child : pattern.items) {
                if (pattern_covers_all_desc(def, subject, child))
                    return true;
            }
        }
        return false;
    }

    void collect_enum_variants_desc(enum_def *def, uint32_t index,
                                    std::vector<bool> &covered) {
        if (index >= proto().patterns.size())
            return;
        const skein::bc::PatternDesc &pattern = proto().patterns[index];
        if (pattern.kind == skein::bc::PatternDesc::Kind::As) {
            collect_enum_variants_desc(def, pattern.child, covered);
            return;
        }
        if (pattern.kind == skein::bc::PatternDesc::Kind::Or) {
            for (uint32_t child : pattern.items)
                collect_enum_variants_desc(def, child, covered);
            return;
        }
        if ((pattern.kind ==
                 skein::bc::PatternDesc::Kind::Container ||
             pattern.kind ==
                 skein::bc::PatternDesc::Kind::Literal) &&
            !pattern.variant.empty()) {
            enum_def *pattern_def = structtype_find_enum(
                pattern.module.c_str(), pattern.type_name.c_str());
            if (pattern_def != def)
                return;
            int variant = structtype_variant_index(
                def, pattern.variant.c_str());
            if (variant >= 0)
                covered[(size_t)variant] = true;
        }
    }

    bool match_exhaustive_desc(uint32_t arms_index, value_t *subject) {
        if (!subject || subject->type != T_enum)
            return true;
        if (arms_index >= proto().match_arms.size())
            return false;
        struct enum_value_t *instance = (struct enum_value_t *)subject;
        enum_def *def = instance->def;
        if (!def || def->variants.empty())
            return true;

        std::vector<bool> covered(def->variants.size(), false);
        for (const skein::bc::MatchArmDesc &arm :
             proto().match_arms[arms_index]) {
            if (arm.pattern == UINT32_MAX)
                return true;
            if (arm.has_guard)
                continue;
            if (pattern_covers_all_desc(def, subject, arm.pattern))
                return true;
            collect_enum_variants_desc(def, arm.pattern, covered);
        }

        std::string missing;
        for (size_t i = 0; i < def->variants.size(); ++i) {
            if (covered[i])
                continue;
            if (!missing.empty())
                missing += ", ";
            missing += def->name + "." + def->variants[i].name;
        }
        if (missing.empty())
            return true;

        yyerror("match is not exhaustive: missing %s", missing.c_str());
        return false;
    }

    bool has_handler(UnwindKind kind) const {
        for (const handler_state &handler : handlers_) {
            if (handler.kind == kind)
                return true;
        }
        return false;
    }

    value_t *end_handler_capture(handler_state &handler) {
        if (!handler.capture)
            return NULL;
        handler.capture = false;
        eval_capture_result result = skein_vm_eval_capture_end(handler.snapshot);
        return result.error;
    }

    void discard_handlers() {
        while (!handlers_.empty()) {
            value_t *error = end_handler_capture(handlers_.back());
            freeval(error);
            handlers_.pop_back();
        }
        handler_body_depth_ = 0;
        context_.handler_depth = saved_handler_depth_;
    }

    value_t *run_defer(const defer_record &record) {
        if (!record.proto) {
            yyerror("invalid defer");
            return NULL;
        }
        std::shared_ptr<function_def> def =
            std::make_shared<function_def>();
        def->upvalues = record.upvalues;
        Executor executor(*record.proto, std::move(def));
        return executor.run();
    }

    void run_defer_scope(std::vector<defer_record> &records) {
        if (records.empty())
            return;

        int pending = static_cast<int>(records.size());
        if (context_.vm_defer_count >= pending)
            context_.vm_defer_count -= pending;
        else
            context_.vm_defer_count = 0;

        std::vector<defer_record> defers = std::move(records);
        records.clear();

        eval_control_kind saved_control = context_.control;
        value_t *saved_value = context_.control_value;
        context_.control = EVAL_CONTROL_NONE;
        context_.control_value = NULL;

        bool saved_capture = context_.capture_errors;
        if (saved_control != EVAL_CONTROL_NONE)
            context_.capture_errors = false;

        eval_control_kind pending_control = EVAL_CONTROL_NONE;
        value_t *pending_value = NULL;

        for (size_t i = defers.size(); i > 0; --i) {
            value_t *result = run_defer(defers[i - 1]);
            freeval(result);
            if (context_.control == EVAL_CONTROL_NONE)
                continue;

            if (pending_control != EVAL_CONTROL_NONE ||
                (saved_control != EVAL_CONTROL_NONE &&
                 saved_control != EVAL_CONTROL_CLOSE)) {
                if (context_.control == EVAL_CONTROL_FAIL) {
                    yyerror("error in defer: %s",
                            error_message_of(context_.control_value).c_str());
                } else {
                    yyerror("error in defer: recover outside error handler");
                }
                freeval(context_.control_value);
                context_.control_value = NULL;
                context_.control = EVAL_CONTROL_NONE;
                continue;
            }

            pending_control = context_.control;
            pending_value = context_.control_value;
            context_.control = EVAL_CONTROL_NONE;
            context_.control_value = NULL;
            context_.capture_errors = false;
        }

        if (saved_control == EVAL_CONTROL_CLOSE &&
            pending_control == EVAL_CONTROL_FAIL) {
            freeval(saved_value);
            context_.control = pending_control;
            context_.control_value = pending_value;
        } else if (saved_control != EVAL_CONTROL_NONE) {
            freeval(pending_value);
            context_.control = saved_control;
            context_.control_value = saved_value;
        } else if (pending_control != EVAL_CONTROL_NONE) {
            context_.control = pending_control;
            context_.control_value = pending_value;
        }

        context_.capture_errors = saved_capture;
    }

    void run_all_defers() {
        while (!defer_scopes_.empty()) {
            run_defer_scope(defer_scopes_.back());
            defer_scopes_.pop_back();
        }
    }

    void pop_scopes_with_defers(size_t mark) {
        while (context_.scopes.restore_stack.size() > mark) {
            if (defer_scopes_.size() > 1) {
                run_defer_scope(defer_scopes_.back());
                defer_scopes_.pop_back();
            }
            close_scope_upvalues(topscope());
            skein_vm_eval_popscope();
        }
    }

    bool dispatch_unwind(vm_unwind &unwind) {
        for (size_t i = handlers_.size(); i > 0; --i) {
            if (handlers_[i - 1].kind != unwind.kind)
                continue;

            handler_state handler = handlers_[i - 1];
            for (size_t j = handlers_.size(); j > i; --j) {
                value_t *error = end_handler_capture(handlers_[j - 1]);
                freeval(error);
            }
            value_t *captured = end_handler_capture(handlers_[i - 1]);
            if (!unwind.value)
                unwind.value = captured;
            else
                freeval(captured);
            handlers_.resize(i - 1);

            while (stack_.size() > handler.stack_depth) {
                freeval(stack_.back());
                stack_.pop_back();
            }
            pop_scopes_with_defers(handler.scope_depth);

            handler_body_depth_ = handler.handler_body_depth;
            context_.handler_depth = handler.eval_handler_depth;

            value_t *value = unwind.value;
            if (!value)
                value = newerrorval("runtime", "runtime error");
            push(value);
            pc_ = handler.target;
            return true;
        }

        freeval(unwind.value);
        unwind.value = NULL;
        return false;
    }

    void raise_fail_value(value_t *value, const char *kind) {
        value_t *error = normalize_error(value, kind);
        if (has_handler(UnwindKind::Fail))
            throw_unwind(UnwindKind::Fail, error);
        skein_vm_eval_raise(error);
    }

    value_t *abort_control(value_t *control) {
        unwind_fast_frames();
        discard_handlers();
        run_all_defers();
        close_upvalues();
        free_stack(stack_);
        recycle_params();
        recycle_slots();
        pop_scopes_to(scope_mark_);
        return control;
    }

    value_t *abort_error() {
        while (true) {
            if (context_.control != EVAL_CONTROL_CLOSE) {
                if (context_.control == EVAL_CONTROL_FAIL &&
                    has_handler(UnwindKind::Fail)) {
                    value_t *error =
                        skein_vm_take_eval_control(EVAL_CONTROL_FAIL);
                    if (!error)
                        error = newerrorval("fail", "");
                    throw_unwind(UnwindKind::Fail, error);
                }
                if (context_.control == EVAL_CONTROL_RECOVER &&
                    has_handler(UnwindKind::Recover)) {
                    value_t *value =
                        skein_vm_take_eval_control(EVAL_CONTROL_RECOVER);
                    throw_unwind(UnwindKind::Recover, value);
                }
                if (context_.control == EVAL_CONTROL_NONE &&
                    has_handler(UnwindKind::Fail)) {
                    std::string message =
                        context_.has_captured_error
                            ? context_.captured_error
                            : "runtime error";
                    throw_unwind(UnwindKind::Fail,
                                 newerrorval("runtime", message));
                }
            }
            if (saved_states_.empty())
                break;
            unwind_one_fast_frame();
        }

        discard_handlers();
        run_all_defers();
        close_upvalues();
        free_stack(stack_);
        recycle_params();
        recycle_slots();
        pop_scopes_to(scope_mark_);
        return NULL;
    }

    value_t *finish() {
        unwind_fast_frames();
        value_t *result = NULL;
        if (!stack_.empty()) {
            result = stack_.back();
            stack_.pop_back();
        }
        discard_handlers();
        run_all_defers();
        close_upvalues();
        free_stack(stack_);
        recycle_params();
        recycle_slots();
        pop_scopes_to(scope_mark_);

        if (context_.control != EVAL_CONTROL_NONE) {
            freeval(result);
            return NULL;
        }
        if (context_.runtime_error_count != errors_before_) {
            freeval(result);
            return NULL;
        }
        return result ? result : newnull();
    }

    static bool compare_result(int op, int cmp) {
        switch (op) {
            case 1: return cmp > 0;
            case 2: return cmp < 0;
            case 3: return cmp != 0;
            case 4: return cmp == 0;
            case 5: return cmp >= 0;
            case 6: return cmp <= 0;
            default: return false;
        }
    }

    value_t *binary_slot_fused(const Instr &ins, bool constant_rhs) {
        if (direct_slots_)
            return binary_slot_fused_direct(ins, constant_rhs);
        const uint16_t left_index = ins.a;
        const uint16_t right_index =
            constant_rhs ? 0 : static_cast<uint16_t>(ins.b & 0xffffu);
        const uint16_t dest_index =
            constant_rhs ? ins.a : static_cast<uint16_t>(ins.b >> 16);
        if (left_index >= slot_symbols_.size() ||
            dest_index >= slot_symbols_.size() ||
            (!constant_rhs && right_index >= slot_symbols_.size())) {
            yyerror("invalid bytecode slot");
            return NULL;
        }
        auto resolve = [&](uint16_t index) -> struct symbol * {
            struct symbol *sym = slot_symbols_[index];
            if (!sym)
                sym = lookupsym(proto().slots[index].name.c_str());
            return sym;
        };
        struct symbol *left = resolve(left_index);
        struct symbol *right = constant_rhs ? NULL : resolve(right_index);
        struct symbol *dest = resolve(dest_index);
        if (!left || (!constant_rhs && !right) || !dest) {
            yyerror("symbol not found %s",
                    !left ? proto().slots[left_index].name.c_str()
                          : (!constant_rhs && !right)
                                ? proto().slots[right_index].name.c_str()
                                : proto().slots[dest_index].name.c_str());
            return NULL;
        }
        if (left->is_uninitialized || (!constant_rhs && right->is_uninitialized)) {
            struct symbol *bad = left->is_uninitialized ? left : right;
            yyerror("parameter %s is used before it is initialized",
                    bad->name);
            return NULL;
        }

        value_t *right_value = NULL;
        if (constant_rhs) {
            const Constant *constant = constant_at(proto(), ins.b);
            if (!constant || constant->kind != ConstantKind::Value) {
                yyerror("invalid bytecode constant");
                return NULL;
            }
            right_value = constant->value ? dupval(constant->value.get())
                                          : newnull();
        } else {
            right_value = dupval(right->val);
        }
        push(dupval(left->val));
        push(right_value);
        if (!binary_op(ins.flags))
            return NULL;
        value_t *result = pop_checked();
        if (!result)
            return NULL;
        if (slot_symbols_[dest_index])
            return skein_vm_assign_symbol_direct(dest, result);
        return skein_vm_assign_symbol_value(dest->name, result);
    }

    bool binary_op(uint8_t opcode) {
        value_t *right = pop_checked();
        value_t *left = pop_checked();
        if (!left || !right) {
            freeval(left);
            freeval(right);
            return false;
        }

        const Op op = static_cast<Op>(opcode);
        if (left->type == T_int && right->type == T_int) {
            const long long x = ((struct int_value_t *)left)->value;
            const long long y = ((struct int_value_t *)right)->value;
            value_t *result = NULL;
            bool handled = true;
            switch (op) {
                case Op::Add:
                    result = new_int_checked((__int128)x + y);
                    break;
                case Op::Sub:
                    result = new_int_checked((__int128)x - y);
                    break;
                case Op::Mul:
                    result = new_int_checked((__int128)x * y);
                    break;
                case Op::Div:
                    if (y == 0) {
                        yyerror("division by zero");
                    } else if (x == std::numeric_limits<long long>::min() &&
                               y == -1) {
                        result = new_int_checked((__int128)x / y);
                    } else {
                        result = newintval(x / y);
                    }
                    break;
                case Op::Mod:
                    if (y == 0) {
                        yyerror("modulo by zero");
                    } else if (y == -1) {
                        result = newintval(0);
                    } else {
                        result = newintval(x % y);
                    }
                    break;
                case Op::BitAnd:
                    result = newintval(x & y);
                    break;
                case Op::BitOr:
                    result = newintval(x | y);
                    break;
                case Op::BitXor:
                    result = newintval(x ^ y);
                    break;
                case Op::Shl:
                    if (y < 0 || y >= 64) {
                        yyerror("shift count %lld out of range", y);
                    } else {
                        result = newintval(
                            (long long)((unsigned long long)x << y));
                    }
                    break;
                case Op::Shr:
                    if (y < 0 || y >= 64) {
                        yyerror("shift count %lld out of range", y);
                    } else {
                        result = newintval(x >> y);
                    }
                    break;
                default:
                    handled = false;
                    break;
            }
            if (handled) {
                freeval(left);
                freeval(right);
                if (!result)
                    return false;
                push(result);
                return true;
            }
        }

        if ((left->type == T_int || left->type == T_float) &&
            (right->type == T_int || right->type == T_float)) {
            const double x = as_double(left);
            const double y = as_double(right);
            value_t *result = NULL;
            bool handled = true;
            switch (op) {
                case Op::Add: result = newfloatval(x + y); break;
                case Op::Sub: result = newfloatval(x - y); break;
                case Op::Mul: result = newfloatval(x * y); break;
                case Op::Div:
                    if (y == 0.0)
                        yyerror("division by zero");
                    else
                        result = newfloatval(x / y);
                    break;
                case Op::Mod:
                    if (y == 0.0)
                        yyerror("modulo by zero");
                    else
                        result = newfloatval(std::fmod(x, y));
                    break;
                default:
                    handled = false;
                    break;
            }
            if (handled) {
                freeval(left);
                freeval(right);
                if (!result)
                    return false;
                push(result);
                return true;
            }
        }

        char symbol = '+';
        switch (op) {
            case Op::Add: symbol = '+'; break;
            case Op::Sub: symbol = '-'; break;
            case Op::Mul: symbol = '*'; break;
            case Op::Div: symbol = '/'; break;
            case Op::Mod: symbol = '%'; break;
            case Op::BitAnd: symbol = '&'; break;
            case Op::BitOr: symbol = '|'; break;
            case Op::BitXor: symbol = '^'; break;
            case Op::Shl: symbol = 's'; break;
            case Op::Shr: symbol = 'r'; break;
            default: break;
        }
        value_t *result = skein_vm_apply_binary_value(symbol, left, right);
        freeval(left);
        freeval(right);
        if (!result)
            return false;
        push(result);
        return true;
    }

    bool compare_values(int op, value_t *left, value_t *right,
                        bool &value) {
        if (left->type == T_int && right->type == T_int) {
            const long long x = ((struct int_value_t *)left)->value;
            const long long y = ((struct int_value_t *)right)->value;
            const int cmp = x < y ? -1 : (x > y ? 1 : 0);
            value = compare_result(op, cmp);
            return true;
        }

        if (left->type == T_float && right->type == T_float) {
            const double x = ((struct float_value_t *)left)->value;
            const double y = ((struct float_value_t *)right)->value;
            if (std::isnan(x) || std::isnan(y))
                value = (op == 3);
            else {
                const int cmp = x < y ? -1 : (x > y ? 1 : 0);
                value = compare_result(op, cmp);
            }
            return true;
        }

        if ((left->type == T_int || left->type == T_float) &&
            (right->type == T_int || right->type == T_float)) {
            bool unordered = false;
            const int cmp = compare_numeric(left, right, unordered);
            value = unordered ? (op == 3) : compare_result(op, cmp);
            return true;
        }

        value_t *result = skein_vm_do_compare(op, left, right);
        if (!result)
            return false;
        value = truthy(result);
        freeval(result);
        return true;
    }

    bool compare_op(uint8_t opcode) {
        value_t *right = pop_checked();
        value_t *left = pop_checked();
        if (!left || !right) {
            freeval(left);
            freeval(right);
            return false;
        }
        int op = 4;
        switch (static_cast<Op>(opcode)) {
            case Op::CmpGt: op = 1; break;
            case Op::CmpLt: op = 2; break;
            case Op::CmpNe: op = 3; break;
            case Op::CmpEq: op = 4; break;
            case Op::CmpGe: op = 5; break;
            case Op::CmpLe: op = 6; break;
            default: break;
        }

        bool value = false;
        const bool ok = compare_values(op, left, right, value);
        freeval(left);
        freeval(right);
        if (!ok)
            return false;
        push(newboolval(value));
        return true;
    }
};

struct vm_module_guard {
    std::string previous_module;
    std::string previous_source;
    vm_module_guard(const std::string &module, const std::string &source)
        : previous_module(diag_current_module_name()),
          previous_source(diag_current_source_name()) {
        if (!module.empty())
            diag_set_module_name(module);
        if (!source.empty())
            diag_set_source_name(source);
    }
    ~vm_module_guard() {
        diag_set_module_name(previous_module);
        diag_set_source_name(previous_source);
    }
};

value_t *execute_proto(const Proto &proto,
                       const std::shared_ptr<function_def> &def) {
    if (vm_trace_state)
        std::fputs(skein::bc::disassemble(proto).c_str(), stderr);
    vm_module_guard module_guard(
        def && def->code ? def->code->module : std::string(),
        def && def->code ? def->code->source : std::string());
    Executor executor(proto, def);
    return executor.run();
}

} // namespace

int skein_vm_max_depth() {
    return vm_max_depth();
}

void skein_vm_ensure_stats_flags() {
    ensure_stats_flags();
}

value_t *skein_vm_execute_proto(
    const Proto &proto, const std::shared_ptr<function_def> &def) {
    return execute_proto(proto, def);
}

bool skein_vm_match_pattern_proto(
    const Proto &proto, uint32_t index, value_t *value,
    std::vector<pattern_binding> &bindings) {
    return Executor::match_pattern_proto(proto, index, value, bindings);
}

bool skein_vm_enabled() {
    ensure_runtime_flags();
    return vm_enabled_state != 0;
}

void skein_vm_set_enabled(bool enabled) {
    ensure_runtime_flags();
#ifdef SKEIN_VM_ONLY
    (void)enabled;
    vm_enabled_state = 1;
#else
    vm_enabled_state = enabled ? 1 : 0;
#endif
}

void skein_vm_set_stats(bool enabled) {
    ensure_runtime_flags();
    vm_stats_state = enabled ? 1 : 0;
    vm_stats.enabled = enabled;
}

void skein_vm_stats_report() {
    ensure_stats_flags();
    const char *coverage_path = std::getenv("SKEIN_VM_COVERAGE_FILE");
    if (coverage_path && *coverage_path) {
        FILE *out = std::fopen(coverage_path, "a");
        if (out) {
            for (size_t i = 0; i < vm_stats.opcodes.size(); ++i) {
                if (vm_stats.opcodes[i] == 0)
                    continue;
                std::fprintf(
                    out, "%zu %s %llu\n", i,
                    skein::bc::op_name(static_cast<Op>(i)),
                    (unsigned long long)vm_stats.opcodes[i]);
            }
            std::fclose(out);
        }
    }
    if (!vm_stats.enabled)
        return;
    if (read_bool_env("SKEIN_VM_STATS_QUIET", false))
        return;
    std::fprintf(stderr,
                 "[vm-stats] statements compiled=%llu fallback=%llu\n",
                 (unsigned long long)vm_stats.statements_compiled,
                 (unsigned long long)vm_stats.statements_fallback);
    std::fprintf(stderr,
                 "[vm-stats] functions compiled=%llu fallback=%llu\n",
                 (unsigned long long)vm_stats.functions_compiled,
                 (unsigned long long)vm_stats.functions_fallback);
    std::fprintf(stderr,
                 "[vm-stats] instructions=%llu calls=%llu fast=%llu "
                 "tail=%llu fallback=%llu\n",
                 (unsigned long long)vm_stats.instructions,
                 (unsigned long long)vm_stats.calls,
                 (unsigned long long)vm_stats.fast_calls,
                 (unsigned long long)vm_stats.tail_calls,
                 (unsigned long long)vm_stats.fallback_calls);
    std::fprintf(stderr,
                 "[vm-stats] name-ic hits=%llu misses=%llu\n",
                 (unsigned long long)vm_stats.name_ic_hits,
                 (unsigned long long)vm_stats.name_ic_misses);
    std::fprintf(stderr,
                 "[vm-stats] member-ic hits=%llu misses=%llu\n",
                 (unsigned long long)vm_stats.member_ic_hits,
                 (unsigned long long)vm_stats.member_ic_misses);
    std::fprintf(stderr,
                 "[vm-stats] index-ic hits=%llu misses=%llu\n",
                 (unsigned long long)vm_stats.index_ic_hits,
                 (unsigned long long)vm_stats.index_ic_misses);
    std::fprintf(stderr,
                 "[vm-stats] max-depth=%llu max-stack=%llu "
                 "max-handler=%llu max-defer=%llu max-iterator=%llu\n",
                 (unsigned long long)vm_stats.max_depth,
                 (unsigned long long)vm_stats.max_stack,
                 (unsigned long long)vm_stats.max_handler_depth,
                 (unsigned long long)vm_stats.max_defer_depth,
                 (unsigned long long)vm_stats.max_iterator_depth);
    std::fprintf(stderr,
                 "[vm-stats] compile=%.3fms execute=%.3fms\n",
                 (double)vm_stats.compile_ns / 1000000.0,
                 (double)vm_stats.execute_ns / 1000000.0);

    if (!vm_stats.fallback_reasons.empty()) {
        std::vector<std::pair<std::string, uint64_t>> reasons;
        reasons.reserve(vm_stats.fallback_reasons.size());
        for (const auto &entry : vm_stats.fallback_reasons)
            reasons.push_back(entry);
        std::sort(reasons.begin(), reasons.end(),
                  [](const auto &a, const auto &b) {
                      if (a.second != b.second)
                          return a.second > b.second;
                      return a.first < b.first;
                  });
        for (const auto &entry : reasons)
            std::fprintf(stderr, "[vm-stats] fallback: %s=%llu\n",
                         entry.first.c_str(),
                         (unsigned long long)entry.second);
    }

    std::vector<std::pair<uint64_t, const char *>> top;
    for (size_t i = 0; i < vm_stats.opcodes.size(); ++i) {
        if (vm_stats.opcodes[i] != 0)
            top.emplace_back(vm_stats.opcodes[i],
                             skein::bc::op_name(static_cast<Op>(i)));
    }
    std::sort(top.begin(), top.end(),
              [](const auto &a, const auto &b) {
                  return a.first > b.first;
              });
    std::fprintf(stderr, "[vm-stats] opcodes");
    size_t shown = top.size() < 10 ? top.size() : 10;
    for (size_t i = 0; i < shown; ++i)
        std::fprintf(stderr, " %s=%llu", top[i].second,
                     (unsigned long long)top[i].first);
    std::fprintf(stderr, "\n");
}
