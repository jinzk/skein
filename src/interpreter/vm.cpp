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

void ensure_runtime_flags() {
    if (vm_enabled_state < 0)
        vm_enabled_state = read_bool_env("SKEIN_VM", false) ? 1 : 0;
    if (!vm_trace_initialized) {
        vm_trace_state = read_bool_env("SKEIN_VM_TRACE", false);
        vm_trace_initialized = true;
    }
}

struct cache_entry {
    ProtoPtr proto;
    bool failed = false;
};

std::unordered_map<function_code *, cache_entry> function_cache;

const Constant *constant_at(const Proto &proto, uint32_t index) {
    if (index >= proto.consts.size())
        return NULL;
    return &proto.consts[index];
}

const char *name_at(const Proto &proto, uint32_t index) {
    const Constant *c = constant_at(proto, index);
    return c ? c->name.c_str() : "";
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
          scope_mark_(context_.scopes.restore_stack.size()) {
        stack_.reserve(64);
        name_cache_.assign(proto.consts.size(), NULL);
    }

    ~Executor() {
        for (iterator_state &state : iterators_) {
            freeval(state.iterable);
            state.iterable = NULL;
        }
    }

    value_t *run() {
        while (pc_ < proto_.code.size()) {
            if (((++instruction_count_) & 1023u) == 0 && g_interrupted) {
                return abort_error();
            }

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

                case Op::DefineName: {
                    const Constant *c = constant_at(proto_, ins.b);
                    if (!c || c->kind != ConstantKind::Decl || !c->decl) {
                        yyerror("invalid bytecode declaration");
                        return abort_error();
                    }
                    struct symdef *def = c->decl;
                    value_t *value = NULL;
                    if (ins.flags & 1) {
                        value = pop_checked();
                        if (!value)
                            return abort_error();
                    } else {
                        value = default_value_for_type(def->type);
                        if (!value) {
                            yyerror(
                                "variable %s of type %s requires an "
                                "initializer",
                                def->name ? def->name : "",
                                type_constraint_name(def->type));
                            return abort_error();
                        }
                    }

                    if (is_control(value))
                        return abort_control(value);

                    if (isrepeated(def->name)) {
                        yyerror("symbol repeated %s",
                                def->name ? def->name : "");
                        freeval(value);
                        return abort_error();
                    }
                    if (!validate_type_constraint(def->type)) {
                        freeval(value);
                        return abort_error();
                    }
                    if (!type_constraint_matches(def->type, value)) {
                        yyerror("variable %s expects %s, got %s",
                                def->name ? def->name : "",
                                type_constraint_name(def->type),
                                typename_of(value));
                        freeval(value);
                        return abort_error();
                    }

                    struct symbol *sym = newsym(def->name);
                    sym->val = value;
                    if (def->type) {
                        sym->type.kind = def->type->kind;
                        sym->type.name = def->type->name;
                    }
                    sym->exported = def->exported != 0;
                    sym->is_const = def->is_const != 0;
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
                    if (desc.two_vars) {
                        struct symbol *value_sym =
                            newsym(desc.value_name.c_str());
                        value_sym->val = NULL;
                        savesym(value_sym);
                        state.value_sym = value_sym;
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
                    eval_raise(value);
                    return abort_error();
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
                    break;

                case Op::PopScope: {
                    scope_t *scope = topscope();
                    bool had_symbols =
                        scope && !scope->syms.small.empty();
                    eval_popscope();
                    if (had_symbols)
                        invalidate_name_cache();
                    if (context_.control != EVAL_CONTROL_NONE)
                        return abort_error();
                    if (context_.runtime_error_count != errors_before_)
                        return abort_error();
                    break;
                }

                case Op::EvalAst: {
                    const Constant *c = constant_at(proto_, ins.b);
                    if (!c || !c->ast) {
                        yyerror("invalid bytecode AST constant");
                        return abort_error();
                    }
                    value_t *result = eval(c->ast);
                    invalidate_name_cache();
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
        }

        return finish();
    }

private:
    const Proto &proto_;
    eval_context &context_;
    int errors_before_;
    size_t scope_mark_;
    std::vector<value_t *> stack_;
    std::vector<struct symbol *> name_cache_;
    std::vector<iterator_state> iterators_;
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

    value_t *abort_control(value_t *control) {
        free_stack(stack_);
        pop_scopes_to(scope_mark_);
        return control;
    }

    value_t *abort_error() {
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
    function_cache.erase(code);
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
    return execute_proto(*proto);
}

bool skein_vm_try_execute_function_body(
    const std::shared_ptr<function_def> &def, struct value_t *&out) {
    out = NULL;
    if (!skein_vm_enabled() || !def || !def->code)
        return false;

    function_code *code = def->code.get();
    auto it = function_cache.find(code);
    if (it == function_cache.end()) {
        std::string error;
        ProtoPtr proto = skein_compile_function_body(
            code, diag_current_module_name(), error);
        cache_entry entry;
        if (!proto) {
            entry.failed = true;
            if (read_bool_env("SKEIN_VM_DEBUG", false) && !error.empty())
                std::fprintf(stderr, "[vm] fallback function %s: %s\n",
                             code->name.c_str(), error.c_str());
        } else {
            entry.proto = std::move(proto);
        }
        it = function_cache.emplace(code, std::move(entry)).first;
    }

    if (it->second.failed || !it->second.proto)
        return false;

    eval_context &context = active_eval_context();
    int limit = vm_max_depth();
    if (context.vm_call_depth >= limit) {
        yyerror("maximum recursion depth (%d) exceeded", limit);
        out = NULL;
        return true;
    }

    vm_depth_guard guard(context);
    out = execute_proto(*it->second.proto);
    return true;
}
