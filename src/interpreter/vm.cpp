#include "vm.h"

#include "ast.h"
#include "bytecode.h"
#include "compiler.h"
#include "diagnostics.h"
#include "dict.h"
#include "eval.h"
#include "eval_context.h"
#include "eval_helpers.h"
#include "eval_internal.h"
#include "fiber.h"
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
#include "valuetype.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using skein::bc::Constant;
using skein::bc::ConstantKind;
using skein::bc::Instr;
using skein::bc::Op;
using skein::bc::Proto;
using skein::bc::ProtoPtr;
using skein::bc::SourceLoc;

int vm_enabled_state = -1;
int vm_max_depth_state = -1;
bool vm_trace_state = false;
bool vm_trace_initialized = false;

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

bool keep_function_ast() {
    return read_bool_env("SKEIN_KEEP_AST", false);
}

void ensure_runtime_flags() {
    if (vm_enabled_state < 0)
        vm_enabled_state = read_bool_env("SKEIN_VM", false) ? 1 : 0;
    if (!vm_trace_initialized) {
        vm_trace_state = read_bool_env("SKEIN_VM_TRACE", false);
        vm_trace_initialized = true;
    }
}

struct default_cache_entry {
    std::vector<ProtoPtr> protos;
};

std::unordered_map<function_code *, default_cache_entry> default_cache;

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
        eval_popscope();
}

void free_stack(std::vector<value_t *> &stack) {
    for (value_t *value : stack)
        freeval(value);
    stack.clear();
}

enum class UnwindKind {
    Fail,
    Recover
};

struct vm_unwind {
    UnwindKind kind;
    value_t *value;
};

struct handler_state {
    UnwindKind kind = UnwindKind::Fail;
    size_t target = 0;
    size_t stack_depth = 0;
    size_t scope_depth = 0;
    int handler_body_depth = 0;
    int eval_handler_depth = 0;
    bool capture = false;
    eval_capture_snapshot snapshot;
};

[[noreturn]] void throw_unwind(UnwindKind kind, value_t *value) {
    throw vm_unwind{kind, value};
}

struct defer_record {
    std::shared_ptr<const Proto> proto;
    std::shared_ptr<scope_t> scope;
};

value_t *execute_proto(const Proto &proto);

struct iterator_state {
    value_t *iterable = NULL;
    size_t index = 0;
    size_t count = 0;
    bool generator = false;
    bool two_vars = false;
    bool active = false;
    struct symbol *key_sym = NULL;
    struct symbol *value_sym = NULL;
};

class Executor {
public:
    explicit Executor(const Proto &proto)
        : proto_(proto),
          context_(active_eval_context()),
          errors_before_(context_.runtime_error_count),
          scope_mark_(context_.scopes.restore_stack.size()),
          saved_handler_depth_(context_.handler_depth) {
        stack_.reserve(64);
        if (proto.uses_names)
            name_cache_.assign(proto.consts.size(), NULL);
        if (proto.uses_defers)
            defer_scopes_.emplace_back();
        if (proto.uses_params) {
            param_symbols_.reserve(proto.param_names.size());
            for (const std::string &name : proto.param_names)
                param_symbols_.push_back(lookupsym(name.c_str()));
        }
        if (proto.uses_slots)
            slot_symbols_.assign(proto.slots.size(), NULL);
    }

    ~Executor() {
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
        context_.handler_depth = saved_handler_depth_;
    }

    value_t *run() {
        while (pc_ < proto_.code.size()) {
            if (((++instruction_count_) & 1023u) == 0 && g_interrupted) {
                return abort_error();
            }

            try {
            const Instr ins = proto_.code[pc_];
            const SourceLoc loc =
                pc_ < proto_.locs.size() ? proto_.locs[pc_] : SourceLoc();
            ++pc_;
            if (loc.line > 0)
                diag_set_location(loc.line, loc.column);

            if (vm_trace_state) {
                std::fprintf(stderr,
                             "[vm] %s pc=%zu op=%s a=%u b=%u stack=%zu\n",
                             proto_.name.empty() ? "<top>"
                                                 : proto_.name.c_str(),
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
                    const Constant *c = constant_at(proto_, ins.b);
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
                    const char *name = name_at(proto_, ins.b);
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

                case Op::StoreName: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    struct symbol *sym = cached_symbol(ins.b);
                    if (!sym) {
                        yyerror("symbol not found %s",
                                name_at(proto_, ins.b));
                        freeval(value);
                        return abort_error();
                    }
                    value_t *result = assign_symbol_direct(sym, value);
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
                                name_at(proto_, ins.b));
                        freeval(value);
                        return abort_error();
                    }
                    value_t *result = compound_symbol_direct(
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
                                name_at(proto_, ins.b));
                        return abort_error();
                    }
                    value_t *result = increment_symbol_direct(
                        sym, ins.a ? 1 : -1, (ins.flags & 1) != 0);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::LoadParam: {
                    if (ins.b >= param_symbols_.size()) {
                        yyerror("invalid bytecode parameter slot");
                        return abort_error();
                    }
                    struct symbol *sym = param_symbols_[ins.b];
                    if (!sym) {
                        yyerror("symbol not found %s",
                                ins.b < proto_.param_names.size()
                                    ? proto_.param_names[ins.b].c_str()
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
                        assign_symbol_direct(param_symbols_[ins.b], value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::CompoundParam: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    if (ins.b >= param_symbols_.size() ||
                        !param_symbols_[ins.b]) {
                        yyerror("invalid bytecode parameter slot");
                        freeval(value);
                        return abort_error();
                    }
                    value_t *result = compound_symbol_direct(
                        param_symbols_[ins.b], (int)ins.flags, value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::IncDecParam: {
                    if (ins.b >= param_symbols_.size() ||
                        !param_symbols_[ins.b]) {
                        yyerror("invalid bytecode parameter slot");
                        return abort_error();
                    }
                    value_t *result = increment_symbol_direct(
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
                    if (ins.b >= proto_.slots.size() ||
                        ins.b >= slot_symbols_.size()) {
                        yyerror("invalid bytecode slot");
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto_.slots[ins.b];
                    value_t *value = NULL;
                    if (ins.flags & 1) {
                        value = pop_checked();
                        if (!value)
                            return abort_error();
                    } else {
                        value = default_value_for_type(&slot.type);
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
                    struct symbol *sym = newsym(slot.name.c_str());
                    sym->val = value;
                    sym->type.kind = slot.type.kind;
                    sym->type.name = slot.type.name;
                    sym->is_const = slot.is_const;
                    savesym(sym);
                    slot_symbols_[ins.b] = sym;
                    invalidate_name_cache();
                    push(newnull());
                    break;
                }

                case Op::LoadSlot: {
                    if (ins.b >= proto_.slots.size() ||
                        ins.b >= slot_symbols_.size()) {
                        yyerror("invalid bytecode slot");
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto_.slots[ins.b];
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
                    if (ins.b >= proto_.slots.size() ||
                        ins.b >= slot_symbols_.size()) {
                        yyerror("invalid bytecode slot");
                        freeval(value);
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto_.slots[ins.b];
                    value_t *result = NULL;
                    if (slot_symbols_[ins.b])
                        result = assign_symbol_direct(
                            slot_symbols_[ins.b], value);
                    else
                        result = assign_symbol_value(slot.name.c_str(),
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
                    if (ins.b >= proto_.slots.size() ||
                        ins.b >= slot_symbols_.size()) {
                        yyerror("invalid bytecode slot");
                        freeval(value);
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto_.slots[ins.b];
                    value_t *result = NULL;
                    if (slot_symbols_[ins.b])
                        result = compound_symbol_direct(
                            slot_symbols_[ins.b], (int)ins.flags, value);
                    else
                        result = compound_symbol_value(
                            slot.name.c_str(), (int)ins.flags, value);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::IncDecSlot: {
                    if (ins.b >= proto_.slots.size() ||
                        ins.b >= slot_symbols_.size()) {
                        yyerror("invalid bytecode slot");
                        return abort_error();
                    }
                    const skein::bc::SlotDesc &slot = proto_.slots[ins.b];
                    value_t *result = NULL;
                    if (slot_symbols_[ins.b])
                        result = increment_symbol_direct(
                            slot_symbols_[ins.b], ins.a ? 1 : -1,
                            (ins.flags & 1) != 0);
                    else
                        result = increment_symbol_value(
                            slot.name.c_str(), ins.a ? 1 : -1,
                            (ins.flags & 1) != 0);
                    if (!result)
                        return abort_error();
                    if (is_control(result))
                        return abort_control(result);
                    push(result);
                    break;
                }

                case Op::ClearSlot:
                    if (ins.b < slot_symbols_.size())
                        slot_symbols_[ins.b] = NULL;
                    break;

                case Op::DefineName: {
                    if (ins.b >= proto_.decls.size()) {
                        yyerror("invalid bytecode declaration");
                        return abort_error();
                    }
                    const skein::bc::DeclDesc &def = proto_.decls[ins.b];
                    value_t *value = NULL;
                    if (ins.flags & 1) {
                        value = pop_checked();
                        if (!value)
                            return abort_error();
                    } else {
                        value = default_value_for_type(&def.type);
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
                    sym->type.kind = def.type.kind;
                    sym->type.name = def.type.name;
                    sym->exported = def.exported;
                    sym->is_const = def.is_const;
                    savesym(sym);
                    invalidate_name_cache();
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

                case Op::IsType: {
                    const Constant *c = constant_at(proto_, ins.b);
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
                    if (!switch_value_supported(subject)) {
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
                        if (!switch_value_supported(item)) {
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
                    value_t *result = read_member_value(
                        container, index, static_cast<Op>(ins.op) == Op::GetField);
                    freeval(container);
                    freeval(index);
                    if (!result)
                        return abort_error();
                    push(result);
                    break;
                }

                case Op::IterNew: {
                    uint32_t desc_index = ins.a;
                    if (desc_index >= proto_.iters.size()) {
                        yyerror("invalid bytecode iterator");
                        return abort_error();
                    }
                    const skein::bc::IterDesc &desc =
                        proto_.iters[desc_index];
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
                    state.active = true;
                    invalidate_name_cache();
                    break;
                }

                case Op::IterNext: {
                    uint32_t desc_index = ins.a;
                    if (desc_index >= iterators_.size()) {
                        yyerror("invalid bytecode iterator");
                        return abort_error();
                    }
                    iterator_state &state = iterators_[desc_index];
                    if (!state.active || !state.key_sym) {
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
                        freeval(state.key_sym->val);
                        state.key_sym->val = item;
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

                    freeval(state.key_sym->val);
                    state.key_sym->val = item;
                    if (state.two_vars && state.value_sym) {
                        freeval(state.value_sym->val);
                        state.value_sym->val = second;
                    } else {
                        freeval(second);
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
                        match_pattern_proto(proto_, ins.b, subject, bindings);
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
                        struct symbol *sym = newsym(binding.name.c_str());
                        sym->val = binding.value;
                        binding.value = NULL;
                        sym->is_const = true;
                        savesym(sym);
                        assign_pattern_slot(ins.b, binding.name, sym);
                    }
                    bindings.clear();
                    invalidate_name_cache();
                    push(newboolval(true));
                    break;
                }

                case Op::MatchBind: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    std::vector<pattern_binding> bindings;
                    bool matched =
                        match_pattern_proto(proto_, ins.b, value, bindings);
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
                        struct symbol *sym = newsym(binding.name.c_str());
                        sym->val = binding.value;
                        binding.value = NULL;
                        sym->is_const = (ins.flags & 1) != 0;
                        savesym(sym);
                        assign_pattern_slot(ins.b, binding.name, sym);
                    }
                    bindings.clear();
                    freeval(value);
                    invalidate_name_cache();
                    push(newnull());
                    break;
                }

                case Op::Closure: {
                    const Constant *c = constant_at(proto_, ins.b);
                    if (!c || c->kind != ConstantKind::Function ||
                        !c->function) {
                        yyerror("invalid bytecode function");
                        return abort_error();
                    }
                    if (!validate_function_params(
                            c->function->name.c_str(),
                            c->function->sl))
                        return abort_error();
                    std::shared_ptr<scope_t> closure = current_scope();
                    if (closure && closure->is_global)
                        closure.reset();
                    push(newfuncval(c->function, closure));
                    break;
                }

                case Op::BindFunc: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    const char *name = name_at(proto_, ins.b);
                    if (isrepeated(name)) {
                        yyerror("symbol repeated %s", name);
                        freeval(value);
                        return abort_error();
                    }
                    struct symbol *sym = newsym(name);
                    sym->val = value;
                    sym->exported = (ins.flags & 1) != 0;
                    savesym(sym);
                    invalidate_name_cache();
                    push(newnull());
                    break;
                }

                case Op::DeclareStruct: {
                    if (ins.b >= proto_.struct_decls.size()) {
                        yyerror("invalid bytecode struct declaration");
                        return abort_error();
                    }
                    const skein::bc::StructDeclMeta &meta =
                        proto_.struct_decls[ins.b];
                    std::vector<std::pair<std::string, type_constraint>>
                        fields;
                    fields.reserve(meta.fields.size());
                    for (const auto &field : meta.fields)
                        fields.emplace_back(field.name, field.type);
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
                    if (!declare_struct_meta(
                            meta.module.c_str(),
                            diag_current_module_name().c_str(),
                            meta.name.c_str(), meta.exported, fields,
                            methods, closure, error))
                        return abort_error();
                    push(newnull());
                    break;
                }

                case Op::DeclareEnum: {
                    if (ins.b >= proto_.enum_decls.size()) {
                        yyerror("invalid bytecode enum declaration");
                        return abort_error();
                    }
                    const skein::bc::EnumDeclMeta &meta =
                        proto_.enum_decls[ins.b];
                    std::vector<std::pair<
                        std::string,
                        std::vector<std::pair<std::string,
                                              type_constraint>>>>
                        variants;
                    variants.reserve(meta.variants.size());
                    for (const auto &variant : meta.variants) {
                        std::vector<std::pair<std::string, type_constraint>>
                            fields;
                        fields.reserve(variant.fields.size());
                        for (const auto &field : variant.fields)
                            fields.emplace_back(field.name, field.type);
                        variants.emplace_back(variant.name,
                                              std::move(fields));
                    }
                    std::string error;
                    if (!declare_enum_meta(
                            meta.module.c_str(),
                            diag_current_module_name().c_str(),
                            meta.name.c_str(), meta.exported, variants,
                            error))
                        return abort_error();
                    push(newnull());
                    break;
                }

                case Op::DeclareTrait: {
                    if (ins.b >= proto_.trait_decls.size()) {
                        yyerror("invalid bytecode trait declaration");
                        return abort_error();
                    }
                    const skein::bc::TraitDeclMeta &meta =
                        proto_.trait_decls[ins.b];
                    std::vector<trait_method_meta> methods;
                    methods.reserve(meta.methods.size());
                    for (const auto &method : meta.methods) {
                        trait_method_meta method_meta;
                        method_meta.name = method.name;
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
                    if (ins.b >= proto_.impl_decls.size()) {
                        yyerror("invalid bytecode impl declaration");
                        return abort_error();
                    }
                    const skein::bc::ImplDeclMeta &meta =
                        proto_.impl_decls[ins.b];
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
                            error))
                        return abort_error();
                    push(newnull());
                    break;
                }

                case Op::NewStruct:
                case Op::NewEnum: {
                    if (ins.b >= proto_.constructs.size()) {
                        yyerror("invalid bytecode constructor");
                        return abort_error();
                    }
                    const skein::bc::ConstructDesc &desc =
                        proto_.constructs[ins.b];
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
                                                 fields)
                            : build_enum_value(desc.module.c_str(),
                                               desc.name.c_str(),
                                               desc.variant.c_str(),
                                               fields);
                    if (!result)
                        return abort_error();
                    push(result);
                    break;
                }

                case Op::AssignIndex:
                case Op::CompoundIndex:
                case Op::IncDecIndex: {
                    if (ins.b >= proto_.index_ops.size()) {
                        yyerror("invalid bytecode index descriptor");
                        return abort_error();
                    }
                    const skein::bc::IndexDesc &desc =
                        proto_.index_ops[ins.b];
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
                    if (static_cast<Op>(ins.op) == Op::IncDecIndex) {
                        result = increment_index_values(
                            desc.base_name.c_str(), ins.a ? 1 : -1,
                            (ins.flags & 1) != 0, indices, desc.fields);
                    } else {
                        value_t *value = pop_checked();
                        if (!value) {
                            free_args(indices);
                            return abort_error();
                        }
                        if (static_cast<Op>(ins.op) == Op::AssignIndex) {
                            result = assign_index_values(
                                desc.base_name.c_str(), indices,
                                desc.fields, value);
                        } else {
                            result = compound_index_values(
                                desc.base_name.c_str(), (int)ins.flags,
                                indices, desc.fields, value);
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
                    if (get_task_value(value, task))
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
                        eval_capture_begin(handler.snapshot);
                    handlers_.push_back(handler);
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
                    eval_raise(error);
                    return abort_error();
                }

                case Op::BindName: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
                    const char *name = name_at(proto_, ins.b);
                    struct symbol *sym = newsym(name);
                    sym->val = value;
                    sym->is_const = (ins.flags & 1) != 0;
                    savesym(sym);
                    if (ins.a != UINT16_MAX &&
                        ins.a < slot_symbols_.size())
                        slot_symbols_[ins.a] = sym;
                    invalidate_name_cache();
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
                    if (ins.b >= proto_.defer_protos.size() ||
                        defer_scopes_.empty()) {
                        yyerror("invalid bytecode defer descriptor");
                        return abort_error();
                    }
                    defer_record record;
                    record.proto = proto_.defer_protos[ins.b];
                    record.scope = current_scope();
                    defer_scopes_.back().push_back(std::move(record));
                    ++context_.vm_defer_count;
                    break;
                }

                case Op::DeclarePackage: {
                    if (ins.b >= proto_.packages.size()) {
                        yyerror("invalid bytecode package descriptor");
                        return abort_error();
                    }
                    const skein::bc::PackageDesc &desc =
                        proto_.packages[ins.b];
                    if (!desc.body) {
                        yyerror("invalid package body");
                        return abort_error();
                    }

                    int package_errors = context_.runtime_error_count;
                    std::string error;
                    if (package_is_loading())
                        eval_pushscope();
                    else
                        eval_push_isolated_scope();

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
                    eval_popscope();
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
                    if (ins.a >= proto_.imports.size()) {
                        yyerror("invalid bytecode import descriptor");
                        return abort_error();
                    }
                    const skein::bc::ImportDesc &desc =
                        proto_.imports[ins.a];
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
                            ? import_binding_name(desc.path.c_str())
                            : desc.alias.c_str();
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
                    invalidate_name_cache();

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
                    if (ins.b >= proto_.call_descs.size()) {
                        yyerror("invalid bytecode call descriptor");
                        return abort_error();
                    }
                    const skein::bc::CallDesc &desc =
                        proto_.call_descs[ins.b];
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
                        call_value_args(callee, std::move(args));
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
                    uint32_t argc = ins.b;
                    std::vector<value_t *> args;
                    args.resize(argc);
                    for (uint32_t i = argc; i > 0; --i) {
                        value_t *arg = pop_checked();
                        if (!arg) {
                            free_args(args);
                            return abort_error();
                        }
                        args[i - 1] = arg;
                    }
                    value_t *callee = pop_checked();
                    if (!callee) {
                        free_args(args);
                        return abort_error();
                    }
                    value_t *result =
                        call_value_args(callee, std::move(args));
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

                case Op::Return: {
                    value_t *value = pop_checked();
                    if (!value)
                        return abort_error();
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
                    eval_pushscope();
                    if (proto_.uses_defers)
                        defer_scopes_.emplace_back();
                    break;

                case Op::PopScope: {
                    scope_t *scope = topscope();
                    bool had_symbols =
                        scope && !scope->syms.small.empty();
                    if (proto_.uses_defers && !defer_scopes_.empty()) {
                        run_defer_scope(defer_scopes_.back());
                        defer_scopes_.pop_back();
                    }
                    eval_popscope();
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
                        label = name_at(proto_, ins.b);
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
                    const char *message = name_at(proto_, ins.b);
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
                    free_stack(stack_);
                    pop_scopes_to(scope_mark_);
                    return NULL;
                }
                continue;
            }
        }

        return finish();
    }

private:
    const Proto &proto_;
    eval_context &context_;
    int errors_before_;
    size_t scope_mark_;
    int saved_handler_depth_;
    std::vector<value_t *> stack_;
    std::vector<struct symbol *> name_cache_;
    std::vector<struct symbol *> param_symbols_;
    std::vector<struct symbol *> slot_symbols_;
    std::vector<iterator_state> iterators_;
    std::vector<std::vector<defer_record>> defer_scopes_;
    std::vector<handler_state> handlers_;
    int handler_body_depth_ = 0;
    size_t pc_ = 0;
    unsigned instruction_count_ = 0;

    void push(value_t *value) { stack_.push_back(value); }

    value_t *pop_checked() {
        if (stack_.empty()) {
            yyerror("internal bytecode stack underflow");
            return NULL;
        }
        value_t *value = stack_.back();
        stack_.pop_back();
        return value;
    }

    struct symbol *cached_symbol(uint32_t index) {
        if (index < name_cache_.size() && name_cache_[index])
            return name_cache_[index];
        const char *name = name_at(proto_, index);
        struct symbol *sym = lookupsym(name);
        if (sym && index < name_cache_.size())
            name_cache_[index] = sym;
        return sym;
    }

    void invalidate_name_cache() {
        std::fill(name_cache_.begin(), name_cache_.end(), nullptr);
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

    static bool is_control(const value_t *value) {
        return value && (value->type == T_return || value->type == T_break ||
                         value->type == T_continue);
    }

    void assign_pattern_slot(uint32_t pattern_index,
                             const std::string &name,
                             struct symbol *sym) {
        if (pattern_index >= proto_.patterns.size())
            return;
        for (const auto &binding :
             proto_.patterns[pattern_index].binding_slots) {
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
                        yyerror("unknown enum variant %s::%s",
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
                            yyerror("unknown field %s in %s::%s",
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
        if (index >= proto_.patterns.size())
            return false;
        const skein::bc::PatternDesc &pattern = proto_.patterns[index];
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
        if (index >= proto_.patterns.size())
            return;
        const skein::bc::PatternDesc &pattern = proto_.patterns[index];
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
        if (arms_index >= proto_.match_arms.size())
            return false;
        struct enum_value_t *instance = (struct enum_value_t *)subject;
        enum_def *def = instance->def;
        if (!def || def->variants.empty())
            return true;

        std::vector<bool> covered(def->variants.size(), false);
        for (const skein::bc::MatchArmDesc &arm :
             proto_.match_arms[arms_index]) {
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
            missing += def->name + "::" + def->variants[i].name;
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
        eval_capture_result result = eval_capture_end(handler.snapshot);
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
        if (!record.proto || !record.scope) {
            yyerror("invalid defer");
            return NULL;
        }
        std::shared_ptr<scope_t> saved_scope = context_.scopes.current;
        context_.scopes.current = record.scope;
        Executor executor(*record.proto);
        value_t *result = executor.run();
        context_.scopes.current = saved_scope;
        return result;
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
            eval_popscope();
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
        eval_raise(error);
    }

    value_t *abort_control(value_t *control) {
        discard_handlers();
        run_all_defers();
        free_stack(stack_);
        pop_scopes_to(scope_mark_);
        return control;
    }

    value_t *abort_error() {
        if (context_.control != EVAL_CONTROL_CLOSE) {
            if (context_.control == EVAL_CONTROL_FAIL &&
                has_handler(UnwindKind::Fail)) {
                value_t *error = take_eval_control(EVAL_CONTROL_FAIL);
                if (!error)
                    error = newerrorval("fail", "");
                throw_unwind(UnwindKind::Fail, error);
            }
            if (context_.control == EVAL_CONTROL_RECOVER &&
                has_handler(UnwindKind::Recover)) {
                value_t *value = take_eval_control(EVAL_CONTROL_RECOVER);
                throw_unwind(UnwindKind::Recover, value);
            }
            if (context_.control == EVAL_CONTROL_NONE &&
                has_handler(UnwindKind::Fail)) {
                std::string message = context_.has_captured_error
                                          ? context_.captured_error
                                          : "runtime error";
                throw_unwind(UnwindKind::Fail,
                             newerrorval("runtime", message));
            }
        }

        discard_handlers();
        run_all_defers();
        free_stack(stack_);
        pop_scopes_to(scope_mark_);
        return NULL;
    }

    value_t *finish() {
        value_t *result = NULL;
        if (!stack_.empty()) {
            result = stack_.back();
            stack_.pop_back();
        }
        discard_handlers();
        run_all_defers();
        free_stack(stack_);
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

    bool binary_op(uint8_t opcode) {
        value_t *right = pop_checked();
        value_t *left = pop_checked();
        if (!left || !right) {
            freeval(left);
            freeval(right);
            return false;
        }
        char op = '+';
        switch (static_cast<Op>(opcode)) {
            case Op::Add: op = '+'; break;
            case Op::Sub: op = '-'; break;
            case Op::Mul: op = '*'; break;
            case Op::Div: op = '/'; break;
            case Op::Mod: op = '%'; break;
            case Op::BitAnd: op = '&'; break;
            case Op::BitOr: op = '|'; break;
            case Op::BitXor: op = '^'; break;
            case Op::Shl: op = 's'; break;
            case Op::Shr: op = 'r'; break;
            default: break;
        }
        value_t *result = apply_binary_value(op, left, right);
        freeval(left);
        freeval(right);
        if (!result)
            return false;
        push(result);
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
        value_t *result = do_compare(op, left, right);
        freeval(left);
        freeval(right);
        if (!result)
            return false;
        push(result);
        return true;
    }
};

value_t *execute_proto(const Proto &proto) {
    if (vm_trace_state)
        std::fputs(skein::bc::disassemble(proto).c_str(), stderr);
    Executor executor(proto);
    return executor.run();
}

} // namespace

bool skein_vm_enabled() {
    ensure_runtime_flags();
    return vm_enabled_state != 0;
}

void skein_vm_set_enabled(bool enabled) {
    ensure_runtime_flags();
    vm_enabled_state = enabled ? 1 : 0;
}

void skein_vm_forget_function_code(function_code *code) {
    if (!code)
        return;
    default_cache.erase(code);
}

struct value_t *skein_vm_eval_statement(struct ast *statement) {
    if (!skein_vm_enabled())
        return eval(statement);

    std::string error;
    ProtoPtr proto = skein_compile_statement(
        statement, diag_current_module_name(), error);
    if (!proto) {
        if (read_bool_env("SKEIN_VM_DEBUG", false) && !error.empty())
            std::fprintf(stderr, "[vm] fallback statement: %s\n",
                         error.c_str());
        return eval(statement);
    }
    value_t *result = execute_proto(*proto);
    /*
     * The AST evaluator restores the source location when a control value
     * escapes a statement.  Uncaught fail/recover is therefore reported at
     * the statement token, not at the fail/pass/defer instruction.  Match
     * that behavior for the VM path.
     */
    if (active_eval_context().control != EVAL_CONTROL_NONE)
        diag_set_location(0, 0);
    return result;
}

bool skein_vm_try_eval_default(const std::shared_ptr<function_def> &def,
                               int index, struct value_t *&out) {
    out = NULL;
    if (!def || !def->code || index < 0)
        return false;

    function_code *code = def->code.get();
    auto it = default_cache.find(code);
    if (it == default_cache.end()) {
        if (!skein_vm_enabled())
            return false;
        default_cache_entry entry;
        entry.protos.resize(code->nparams);
        int i = 0;
        for (struct symlist *param = code->sl;
             param && i < code->nparams; param = param->next, ++i) {
            if (!param->has_default || !param->default_value)
                continue;
            std::string error;
            ProtoPtr proto = skein_compile_default_expression(
                code, param->default_value, i, code->module, error);
            if (!proto) {
                if (read_bool_env("SKEIN_VM_DEBUG", false) &&
                    !error.empty()) {
                    std::fprintf(stderr,
                                 "[vm] fallback default %s[%d]: %s\n",
                                 code->name.c_str(), i, error.c_str());
                }
                continue;
            }
            entry.protos[(size_t)i] = std::move(proto);
            if (!keep_function_ast()) {
                treefree(param->default_value);
                param->default_value = NULL;
            }
        }
        it = default_cache.emplace(code, std::move(entry)).first;
    }

    if ((size_t)index >= it->second.protos.size() ||
        !it->second.protos[(size_t)index])
        return false;

    out = execute_proto(*it->second.protos[(size_t)index]);
    return true;
}

ParamPatternResult skein_vm_match_parameter_pattern(
    const std::shared_ptr<function_def> &def, int index,
    struct value_t *value, std::vector<pattern_binding> &bindings) {
    if (!def || !def->code || index < 0 || !value)
        return ParamPatternResult::Unsupported;

    function_code *code = def->code.get();
    if (!code->vm_param_patterns) {
        if (!skein_vm_enabled())
            return ParamPatternResult::Unsupported;
        std::string error;
        std::shared_ptr<const compiled_pattern_set> compiled =
            skein_compile_parameter_patterns(code, code->module, error);
        if (!compiled) {
            std::shared_ptr<compiled_pattern_set> failed =
                std::make_shared<compiled_pattern_set>();
            failed->failed = true;
            code->vm_param_patterns = failed;
            if (read_bool_env("SKEIN_VM_DEBUG", false) && !error.empty()) {
                std::fprintf(stderr,
                             "[vm] fallback parameter pattern %s: %s\n",
                             code->name.c_str(), error.c_str());
            }
            return ParamPatternResult::Unsupported;
        }
        code->vm_param_patterns = compiled;
        if (read_bool_env("SKEIN_VM_DEBUG", false)) {
            std::fprintf(stderr,
                         "[vm] compiled parameter patterns for %s\n",
                         code->name.empty() ? "<anonymous>"
                                            : code->name.c_str());
        }
        if (!keep_function_ast()) {
            for (struct symlist *param = code->sl; param;
                 param = param->next) {
                if (param->pattern) {
                    treefree(param->pattern);
                    param->pattern = NULL;
                }
            }
        }
    }

    std::shared_ptr<const compiled_pattern_set> compiled =
        std::static_pointer_cast<const compiled_pattern_set>(
            code->vm_param_patterns);
    if (compiled->failed || !compiled->proto ||
        (size_t)index >= compiled->roots.size())
        return ParamPatternResult::Unsupported;

    uint32_t root = compiled->roots[(size_t)index];
    if (root == UINT32_MAX)
        return ParamPatternResult::Unsupported;

    bool matched = Executor::match_pattern_proto(
        *compiled->proto, root, value, bindings);
    return matched ? ParamPatternResult::Matched
                   : ParamPatternResult::NotMatched;
}

bool skein_vm_try_execute_function_body(
    const std::shared_ptr<function_def> &def, struct value_t *&out) {
    out = NULL;
    if (!def || !def->code)
        return false;

    function_code *code = def->code.get();
    if (code->vm_compile_failed)
        return false;
    if (!code->vm_proto) {
        if (!skein_vm_enabled())
            return false;
        std::string error;
        ProtoPtr proto = skein_compile_function_body(
            code, code->module, error);
        if (!proto) {
            code->vm_compile_failed = true;
            if (read_bool_env("SKEIN_VM_DEBUG", false) && !error.empty())
                std::fprintf(stderr, "[vm] fallback function %s: %s\n",
                             code->name.c_str(), error.c_str());
            return false;
        }
        code->vm_proto = proto;
        if (code->ast && !keep_function_ast()) {
            treefree(code->ast);
            code->ast = NULL;
        }
    }

    ProtoPtr proto = std::static_pointer_cast<const Proto>(code->vm_proto);

    eval_context &context = active_eval_context();
    int limit = vm_max_depth();
    if (context.vm_call_depth >= limit) {
        yyerror("maximum recursion depth (%d) exceeded", limit);
        out = NULL;
        return true;
    }

    vm_depth_guard guard(context);
    out = execute_proto(*proto);
    return true;
}
