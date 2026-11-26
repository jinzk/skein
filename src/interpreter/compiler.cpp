#include "compiler.h"

#include "ast.h"
#include "ast_analysis.h"
#include "compiler_analysis.h"
#include "diagnostics.h"
#include "label_analysis.h"
#include "verifier.h"
#include "eval_context.h"
#include "eval_internal.h"
#include "scopestack.h"
#include "structeval.h"
#include "typecheck_state.h"
#include "value_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

std::string resolve_selective_type_source(const char *source,
                                          const char *name) {
    if (!source)
        return std::string();
    const auto source_it =
        skein::typecheck::detail::g_selective_imports.find(source);
    if (source_it == skein::typecheck::detail::g_selective_imports.end() ||
        !name)
        return source;
    const auto alias_it = source_it->second.find(name);
    return alias_it == source_it->second.end() ? source : alias_it->second;
}

struct value_deleter {
    void operator()(value_t *value) const { freeval(value); }
};

std::shared_ptr<value_t> own_value(value_t *value) {
    return std::shared_ptr<value_t>(value, value_deleter());
}

bool bytecode_optimization_enabled() {
    static const bool enabled = []() {
        const char *configured = std::getenv("SKEIN_VM_OPT");
        if (!configured || !*configured)
            return true;
        return std::strcmp(configured, "0") != 0 &&
               std::strcmp(configured, "false") != 0 &&
               std::strcmp(configured, "off") != 0;
    }();
    return enabled;
}

bool op_has_jump_target(Op op) {
    switch (op) {
        case Op::Jump:
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
        case Op::CmpJump:
        case Op::IterNext:
        case Op::PushFailHandler:
        case Op::PushRecoverHandler:
            return true;
        default:
            return false;
    }
}

bool op_is_conditional_jump(Op op) {
    switch (op) {
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
        case Op::CmpJump:
        case Op::IterNext:
            return true;
        default:
            return false;
    }
}

bool op_is_threadable_jump(Op op) {
    return op == Op::Jump || op_is_conditional_jump(op);
}

bool op_is_terminal(Op op) {
    switch (op) {
        case Op::Return:
        case Op::TailCall:
        case Op::Fail:
        case Op::Pass:
        case Op::Recover:
        case Op::ControlBreak:
        case Op::ControlContinue:
        case Op::RuntimeError:
            return true;
        default:
            return false;
    }
}

void thread_jumps(Proto &proto) {
    const size_t count = proto.code.size();
    for (size_t pc = 0; pc < count; ++pc) {
        Instr &ins = proto.code[pc];
        const Op op = static_cast<Op>(ins.op);
        if (!op_is_threadable_jump(op))
            continue;
        uint32_t target = ins.b;
        for (int depth = 0; depth < 64 && target < count; ++depth) {
            if (static_cast<Op>(proto.code[target].op) != Op::Jump)
                break;
            const uint32_t next = proto.code[target].b;
            if (next == target)
                break;
            target = next;
        }
        ins.b = target;
    }
}

void eliminate_unreachable(Proto &proto) {
    const size_t count = proto.code.size();
    if (count == 0)
        return;

    /*
     * The verifier performs a linear handler-balance pass over the whole
     * instruction stream, including unreachable handler-closing opcodes.
     * Keep those protos intact rather than removing bookkeeping instructions
     * that the runtime never executes but verification still requires.
     */
    for (const Instr &ins : proto.code) {
        const Op op = static_cast<Op>(ins.op);
        if (op == Op::PushFailHandler || op == Op::PushRecoverHandler ||
            op == Op::PopHandler || op == Op::EnterHandlerBody ||
            op == Op::LeaveHandlerBody)
            return;
    }

    std::vector<uint8_t> reachable(count, 0);
    std::vector<uint32_t> worklist;
    worklist.push_back(0);
    while (!worklist.empty()) {
        const uint32_t pc = worklist.back();
        worklist.pop_back();
        if (pc >= count || reachable[pc])
            continue;
        reachable[pc] = 1;
        const Instr &ins = proto.code[pc];
        const Op op = static_cast<Op>(ins.op);
        if (op_has_jump_target(op) && ins.b < count)
            worklist.push_back(ins.b);
        if (op == Op::Jump || op_is_terminal(op))
            continue;
        if (pc + 1 < count)
            worklist.push_back(pc + 1);
    }

    std::vector<uint32_t> new_pc(count + 1, 0);
    uint32_t kept = 0;
    for (size_t pc = 0; pc < count; ++pc) {
        new_pc[pc] = kept;
        if (reachable[pc])
            ++kept;
    }
    new_pc[count] = kept;
    if (kept == count)
        return;

    std::vector<Instr> code;
    std::vector<SourceLoc> locs;
    code.reserve(kept);
    locs.reserve(kept);
    for (size_t pc = 0; pc < count; ++pc) {
        if (!reachable[pc])
            continue;
        Instr ins = proto.code[pc];
        const Op op = static_cast<Op>(ins.op);
        if (op_has_jump_target(op))
            ins.b = new_pc[ins.b < count ? ins.b : count];
        code.push_back(ins);
        if (pc < proto.locs.size())
            locs.push_back(proto.locs[pc]);
        else
            locs.push_back(SourceLoc());
    }
    proto.code = std::move(code);
    proto.locs = std::move(locs);
}

bool op_is_binary_arith(Op op) {
    switch (op) {
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
            return true;
        default:
            return false;
    }
}

void fuse_slot_operations(Proto &proto) {
    const size_t count = proto.code.size();
    if (count < 4)
        return;

    std::vector<uint8_t> target(count, 0);
    for (const Instr &ins : proto.code) {
        const Op op = static_cast<Op>(ins.op);
        if (op_has_jump_target(op) && ins.b < count)
            target[ins.b] = 1;
    }

    for (size_t i = 0; i + 3 < count; ++i) {
        if (target[i + 1] || target[i + 2] || target[i + 3])
            continue;
        auto fusable_dest = [&](uint32_t dest) {
            return dest < proto.slots.size() &&
                   !proto.slots[dest].is_const &&
                   proto.slots[dest].type.kind == TYPE_ANY;
        };
        const Op first = static_cast<Op>(proto.code[i].op);
        const Op second = static_cast<Op>(proto.code[i + 1].op);
        const Op operation = static_cast<Op>(proto.code[i + 2].op);
        const Op store = static_cast<Op>(proto.code[i + 3].op);
        if (store != Op::StoreSlot || !op_is_binary_arith(operation))
            continue;

        if (first == Op::LoadSlot && second == Op::LoadSlot) {
            const uint32_t left = proto.code[i].b;
            const uint32_t right = proto.code[i + 1].b;
            const uint32_t dest = proto.code[i + 3].b;
            if (left > UINT16_MAX || right > UINT16_MAX ||
                dest > UINT16_MAX || !fusable_dest(dest))
                continue;
            Instr fused;
            fused.op = static_cast<uint8_t>(Op::BinarySlots);
            fused.flags = proto.code[i + 2].op;
            fused.a = static_cast<uint16_t>(left);
            fused.b = right | (dest << 16);
            proto.code[i] = fused;
            for (size_t j = i + 1; j <= i + 3; ++j)
                proto.code[j].op = static_cast<uint8_t>(Op::Nop);
            i += 3;
            continue;
        }

        if (first == Op::LoadSlot && second == Op::LoadConst) {
            const uint32_t slot = proto.code[i].b;
            const uint32_t dest = proto.code[i + 3].b;
            if (slot > UINT16_MAX || dest != slot || !fusable_dest(dest))
                continue;
            Instr fused;
            fused.op = static_cast<uint8_t>(Op::BinarySlotConst);
            fused.flags = proto.code[i + 2].op;
            fused.a = static_cast<uint16_t>(slot);
            fused.b = proto.code[i + 1].b;
            proto.code[i] = fused;
            for (size_t j = i + 1; j <= i + 3; ++j)
                proto.code[j].op = static_cast<uint8_t>(Op::Nop);
            i += 3;
        }
    }
}

void fold_constant_jumps(Proto &proto) {
    const size_t count = proto.code.size();
    if (count < 2)
        return;

    std::vector<uint8_t> target(count, 0);
    for (const Instr &ins : proto.code) {
        const Op op = static_cast<Op>(ins.op);
        if (op_has_jump_target(op) && ins.b < count)
            target[ins.b] = 1;
    }

    for (size_t i = 0; i + 1 < count; ++i) {
        const Op value_op = static_cast<Op>(proto.code[i].op);
        if (target[i + 1] ||
            (value_op != Op::LoadConst && value_op != Op::LoadNull))
            continue;
        const Op jump = static_cast<Op>(proto.code[i + 1].op);
        if (jump != Op::JumpIfFalse && jump != Op::JumpIfTrue)
            continue;
        bool truth = false;
        if (value_op == Op::LoadConst) {
            if (proto.code[i].b >= proto.consts.size())
                continue;
            const Constant &constant = proto.consts[proto.code[i].b];
            if (constant.kind != ConstantKind::Value)
                continue;
            truth = constant.value ? truthy(constant.value.get()) : false;
        }
        const bool jump_true = jump == Op::JumpIfTrue;
        const bool taken = jump_true ? truth : !truth;
        if (taken) {
            Instr branch = proto.code[i + 1];
            branch.op = static_cast<uint8_t>(Op::Jump);
            branch.flags = 0;
            branch.a = 0;
            proto.code[i] = branch;
            if (i < proto.locs.size() && i + 1 < proto.locs.size())
                proto.locs[i] = proto.locs[i + 1];
        } else {
            proto.code[i].op = static_cast<uint8_t>(Op::Nop);
        }
        proto.code[i + 1].op = static_cast<uint8_t>(Op::Nop);
        ++i;
    }
}

void remove_nops(Proto &proto) {
    const size_t count = proto.code.size();
    std::vector<uint32_t> new_pc(count + 1, 0);
    uint32_t kept = 0;
    for (size_t pc = 0; pc < count; ++pc) {
        new_pc[pc] = kept;
        if (static_cast<Op>(proto.code[pc].op) != Op::Nop)
            ++kept;
    }
    new_pc[count] = kept;
    if (kept == count)
        return;

    std::vector<Instr> code;
    std::vector<SourceLoc> locs;
    code.reserve(kept);
    locs.reserve(kept);
    for (size_t pc = 0; pc < count; ++pc) {
        const Op op = static_cast<Op>(proto.code[pc].op);
        if (op == Op::Nop)
            continue;
        Instr ins = proto.code[pc];
        if (op_has_jump_target(op))
            ins.b = new_pc[ins.b < count ? ins.b : count];
        code.push_back(ins);
        if (pc < proto.locs.size())
            locs.push_back(proto.locs[pc]);
        else
            locs.push_back(SourceLoc());
    }
    proto.code = std::move(code);
    proto.locs = std::move(locs);
}

void optimize_proto(Proto &proto) {
    if (!bytecode_optimization_enabled())
        return;
    thread_jumps(proto);
    fold_constant_jumps(proto);
    eliminate_unreachable(proto);
    fuse_slot_operations(proto);
    remove_nops(proto);
}

bool finalize_proto(Proto &proto, std::string &error) {
    optimize_proto(proto);
    proto.call_method_ic.assign(proto.code.size(),
                                skein::bc::MethodCallCacheEntry{});
    uint32_t max_stack = 0;
    if (!skein::bc::verify(proto, error, &max_stack))
        return false;
    proto.max_stack = max_stack;

    proto.slots_direct_safe = true;
    return true;
}
struct LoopContext {
    std::string label;
    std::vector<size_t> break_patches;
    std::vector<size_t> continue_patches;
    int scope_depth = 0;
    int iter_depth = 0;
    int call_depth = 0;
    int stack_cleanup = 0;
    int handler_body_depth = 0;
    int handler_stack_depth = 0;
    bool accepts_continue = true;
    bool accepts_break = true;
};

struct LabelTarget {
    size_t pc = static_cast<size_t>(-1);
    std::vector<size_t> patches;
    int scope_depth = 0;
    int iter_depth = 0;
    int loop_depth = 0;
    int call_depth = 0;
    int handler_stack_depth = 0;
    int handler_body_depth = 0;
    int decl_index = 0;
};


struct StmtListContext {
    std::unordered_map<std::string, LabelTarget> labels;
    int scope_depth = 0;
    int iter_depth = 0;
    int loop_depth = 0;
    int call_depth = 0;
    int handler_stack_depth = 0;
    int handler_body_depth = 0;
};

struct Compiler {
    Proto proto;
    std::string error;
    std::vector<LoopContext> loops;
    std::vector<StmtListContext> stmt_lists;
    std::vector<int> call_cleanup;
    std::unordered_map<std::string, skein::labels::LabelInfo> function_labels;
    std::unordered_set<std::string> used_function_labels;
    std::vector<int> iter_stack;
    struct SlotScope {
        std::unordered_map<std::string, int> names;
        std::vector<int> declared;
        std::unordered_map<std::string, std::shared_ptr<value_t>> constants;
    };

    std::string pending_label;
    std::vector<std::string> param_names;
    std::vector<std::string> upvalue_names;
    std::unordered_set<std::string> declared_names;
    std::unordered_set<std::string> selective_bindings;
    std::vector<SlotScope> slot_scopes;
    int scope_depth = 0;
    bool slot_fast_path = false;
    int handler_body_depth = 0;
    int handler_stack_depth = 0;
    bool forbid_control_exit = false;
    bool inside_function = false;
    bool param_fast_path = false;
    bool ok = true;

    explicit Compiler(const std::string &package) {
        proto.package = package;
    }

    SourceLoc loc_of(const ast *node) const {
        SourceLoc loc;
        if (node) {
            loc.line = node->line > 0 ? node->line : 1;
            loc.column = node->column > 0 ? node->column : 1;
        }
        return loc;
    }

    size_t emit(Op op, uint8_t flags, uint16_t a, uint32_t b,
                SourceLoc loc) {
        switch (op) {
            case Op::LoadName:
            case Op::StoreName:
            case Op::CompoundName:
            case Op::IncDecName:
            /* A name-based definition also lives in the current scope, so
             * the frame still needs one. */
            case Op::DefineName:
                proto.uses_names = true;
                break;
            case Op::LoadParam:
            case Op::StoreParam:
            case Op::CompoundParam:
            case Op::IncDecParam:
                proto.uses_params = true;
                break;
            case Op::DeferPush:
                proto.uses_defers = true;
                break;
            case Op::Closure:
                proto.has_closures = true;
                break;
            case Op::PushFailHandler:
            case Op::PushRecoverHandler:
                proto.uses_handlers = true;
                break;
            case Op::Yield:
            case Op::ControlBreak:
            case Op::ControlContinue:
                proto.fast_frame_blocked = true;
                break;
            /* These resolve a base name through the live scope chain. */
            case Op::GetFieldOrEnum:
            case Op::Import:
                proto.uses_names = true;
                break;
            /* These define or capture symbols in the current block scope. */
            case Op::BindFunc:
            case Op::DeclareStruct:
            case Op::DeclareEnum:
            case Op::DeclareProtocol:
            case Op::DeclareImpl:
                proto.uses_names = true;
                break;
            case Op::DefineSlot:
            case Op::LoadSlot:
            case Op::StoreSlot:
            case Op::CompoundSlot:
            case Op::IncDecSlot:
            case Op::ClearSlot:
                proto.uses_slots = true;
                break;
            default:
                break;
        }
        Instr ins;
        ins.op = static_cast<uint8_t>(op);
        ins.flags = flags;
        ins.a = a;
        ins.b = b;
        proto.code.push_back(ins);
        proto.locs.push_back(loc);
        return proto.code.size() - 1;
    }

    size_t emit_jump(Op op, SourceLoc loc) {
        return emit(op, 0, 0, 0, loc);
    }

    size_t compile_condition(ast *condition, bool jump_if_true,
                             SourceLoc loc) {
        if (value_t *folded = fold_node(condition)) {
            const bool truth = truthy(folded);
            freeval(folded);
            value_t *constant = newboolval(truth);
            uint32_t index = add_value(constant);
            freeval(constant);
            emit(Op::LoadConst, 0, 0, index, loc);
            return emit_jump(jump_if_true ? Op::JumpIfTrue
                                          : Op::JumpIfFalse,
                             loc);
        }
        if (condition && condition->nodetype >= '1' &&
            condition->nodetype <= '6') {
            compile(condition->l);
            if (!ok) return static_cast<size_t>(-1);
            compile(condition->r);
            if (!ok) return static_cast<size_t>(-1);
            uint8_t flags = static_cast<uint8_t>(
                condition->nodetype - '0');
            if (jump_if_true)
                flags |= 0x80;
            return emit(Op::CmpJump, flags, 0, 0, loc);
        }
        compile(condition);
        if (!ok) return static_cast<size_t>(-1);
        return emit_jump(jump_if_true ? Op::JumpIfTrue
                                      : Op::JumpIfFalse,
                         loc);
    }

    void patch(size_t at, size_t target) {
        if (at < proto.code.size())
            proto.code[at].b = static_cast<uint32_t>(target);
    }

    uint32_t add_value(value_t *value) {
        Constant c;
        c.kind = ConstantKind::Value;
        if (value)
            c.value = own_value(dupval(value));
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    uint32_t add_builtin_value(int functype) {
        struct value_t *value = newbuiltinval(functype);
        uint32_t index = add_value(value);
        freeval(value);
        return index;
    }

    uint32_t add_name(const std::string &name) {
        Constant c;
        c.kind = ConstantKind::Name;
        c.name = name;
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    uint32_t add_decl(struct symdef *decl) {
        skein::bc::DeclDesc desc;
        desc.name = decl && decl->name ? decl->name : "";
        desc.type = decl && decl->type
                        ? copy_type_constraint(decl->type)
                        : any_type_constraint();
        desc.exported = decl && decl->exported != 0;
        desc.is_const = decl && decl->is_const != 0;
        proto.decls.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.decls.size() - 1);
    }

    uint32_t add_type(const struct type_constraint *type) {
        Constant c;
        c.kind = ConstantKind::Type;
        c.type_storage =
            std::make_shared<type_constraint>(
                type ? *type : any_type_constraint());
        c.type = c.type_storage.get();
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    uint32_t add_iter(const char *key_name, const char *value_name,
                      uint32_t key_slot, uint32_t value_slot) {
        skein::bc::IterDesc desc;
        desc.key_name = key_name ? key_name : "";
        desc.value_name = value_name ? value_name : "";
        desc.two_vars = value_name != NULL;
        desc.key_slot = key_slot;
        desc.value_slot = value_slot;
        proto.iters.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.iters.size() - 1);
    }

    uint32_t add_index(const char *base_name,
                       const std::vector<bool> &fields,
                       skein::bc::IndexBaseKind base_kind,
                       uint32_t base_index) {
        skein::bc::IndexDesc desc;
        desc.base_name = base_name ? base_name : "";
        desc.fields = fields;
        desc.base_kind = base_kind;
        desc.base_index = base_index;
        proto.index_ops.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.index_ops.size() - 1);
    }

    skein::bc::IndexBaseKind classify_index_base(const char *base_name,
                                                 uint32_t &index) {
        using skein::bc::IndexBaseKind;
        IndexBaseKind kind = IndexBaseKind::Name;
        index = UINT32_MAX;
        int slot = slot_fast_path ? find_slot(base_name) : -1;
        if (slot >= 0) {
            kind = IndexBaseKind::Slot;
            index = static_cast<uint32_t>(slot);
            proto.uses_slots = true;
        } else {
            slot = param_fast_path ? param_index(base_name) : -1;
            if (slot >= 0) {
                kind = IndexBaseKind::Param;
                index = static_cast<uint32_t>(slot);
                proto.uses_params = true;
            } else {
                int upvalue = upvalue_index(base_name);
                if (upvalue >= 0) {
                    kind = IndexBaseKind::Upvalue;
                    index = static_cast<uint32_t>(upvalue);
                } else {
                    int global = global_index(base_name);
                    if (global >= 0) {
                        kind = IndexBaseKind::Global;
                        index = static_cast<uint32_t>(global);
                    }
                }
            }
        }
        if (kind == skein::bc::IndexBaseKind::Name)
            proto.uses_names = true;
        return kind;
    }

    uint32_t add_index_for_base(const char *base_name,
                                const std::vector<bool> &fields) {
        uint32_t index = UINT32_MAX;
        skein::bc::IndexBaseKind kind =
            classify_index_base(base_name, index);
        return add_index(base_name, fields, kind, index);
    }

    uint32_t add_index_read_cache() {
        proto.index_read_ic.emplace_back();
        return static_cast<uint32_t>(proto.index_read_ic.size() - 1);
    }

    void fill_generic_meta(struct generic_param *params,
                           std::vector<std::string> &names,
                           std::vector<std::string> &defaults) {
        for (struct generic_param *param = params; param;
             param = param->next) {
            if (!param->name || !*param->name)
                continue;
            names.push_back(param->name);
            defaults.push_back(
                param->default_type
                    ? type_constraint_name(param->default_type)
                    : std::string());
        }
    }

    uint32_t add_struct_decl(struct struct_node *node) {
        skein::bc::StructDeclMeta meta;
        meta.source = node->source ? node->source : "";
        meta.module = node->module ? node->module : "";
        meta.name = node->name ? node->name : "";
        meta.exported = node->exported != 0;
        fill_generic_meta(node->generic_params, meta.generic_params,
                          meta.generic_defaults);
        std::vector<std::string> forbidden;
        forbidden.push_back("self");
        for (struct struct_member *member = node->members; member;
             member = member->next) {
            if (member->nodetype == 'f' && member->name)
                forbidden.push_back(member->name);
        }
        for (struct struct_member *member = node->members; member;
             member = member->next) {
            if (member->nodetype == 'f') {
                skein::bc::DeclFieldMeta field;
                field.name = member->name ? member->name : "";
                field.type = member->type
                                 ? copy_type_constraint(member->type)
                                 : any_type_constraint();
                if (member->default_value) {
                    if (!check_field_default(member->default_value,
                                             member->name, forbidden))
                        return 0;
                    field.default_code = add_field_default(
                        member->default_value, node->name, member->name);
                    member->default_value = NULL;
                }
                meta.fields.push_back(std::move(field));
            } else if (member->nodetype == 'm') {
                yyerror("methods must be declared in impl");
                ok = false;
                return 0;
            }
        }
        proto.struct_decls.push_back(std::move(meta));
        return static_cast<uint32_t>(proto.struct_decls.size() - 1);
    }

    uint32_t add_enum_decl(struct enum_node *node) {
        skein::bc::EnumDeclMeta meta;
        meta.source = node->source ? node->source : "";
        meta.module = node->module ? node->module : "";
        meta.name = node->name ? node->name : "";
        meta.exported = node->exported != 0;
        fill_generic_meta(node->generic_params, meta.generic_params,
                          meta.generic_defaults);
        std::vector<std::string> forbidden;
        forbidden.push_back("self");
        for (struct enum_variant *variant = node->variants; variant;
             variant = variant->next) {
            for (struct struct_member *member = variant->fields; member;
                 member = member->next) {
                if (member->nodetype == 'f' && member->name)
                    forbidden.push_back(member->name);
            }
        }
        for (struct enum_variant *variant = node->variants; variant;
             variant = variant->next) {
            skein::bc::EnumVariantMeta variant_meta;
            variant_meta.name = variant->name ? variant->name : "";
            for (struct struct_member *member = variant->fields; member;
                 member = member->next) {
                if (member->nodetype != 'f')
                    continue;
                skein::bc::DeclFieldMeta field;
                field.name = member->name ? member->name : "";
                field.type = member->type
                                 ? copy_type_constraint(member->type)
                                 : any_type_constraint();
                if (member->default_value) {
                    if (!check_field_default(member->default_value,
                                             member->name, forbidden))
                        return 0;
                    field.default_code = add_field_default(
                        member->default_value, node->name, member->name);
                    member->default_value = NULL;
                }
                variant_meta.fields.push_back(std::move(field));
            }
            meta.variants.push_back(std::move(variant_meta));
        }
        proto.enum_decls.push_back(std::move(meta));
        return static_cast<uint32_t>(proto.enum_decls.size() - 1);
    }

    uint32_t add_package(struct package_node *node) {
        Compiler child(proto.package);
        child.proto.name = node->name
                               ? std::string("package ") + node->name
                               : std::string("package");
        child.compile(node->body);
        if (!child.ok) {
            fail(child.error.empty() ? "cannot compile package body"
                                     : child.error.c_str());
            return 0;
        }
        if (!finalize_proto(child.proto, child.error)) {
            fail(child.error.c_str());
            return 0;
        }
        skein::bc::PackageDesc desc;
        desc.name = node->name ? node->name : "";
        desc.body =
            std::make_shared<const Proto>(std::move(child.proto));
        proto.packages.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.packages.size() - 1);
    }

    uint32_t add_protocol_decl(struct protocol_node *node) {
        skein::bc::ProtocolDeclMeta meta;
        meta.source = node->source ? node->source : "";
        meta.module = node->module ? node->module : "";
        meta.name = node->name ? node->name : "";
        meta.exported = node->exported != 0;
        for (struct generic_param *param = node->generic_params; param;
             param = param->next) {
            if (param->name && *param->name)
                meta.generic_params.push_back(param->name);
        }
        for (struct protocol_method *method = node->methods; method;
             method = method->next) {
            skein::bc::ProtocolMethodMeta method_meta;
            method_meta.name = method->name ? method->name : "";
            struct symlist *params =
                method->func
                    ? ((struct funcdef *)method->func)->sl
                    : method->params;
            for (struct symlist *param = params; param;
                 param = param->next) {
                skein::bc::ProtocolParamMeta param_meta;
                param_meta.name =
                    param->sym && param->sym->name ? param->sym->name : "";
                param_meta.type =
                    param->type ? copy_type_constraint(param->type)
                                : any_type_constraint();
                param_meta.variadic = param->variadic;
                param_meta.has_default = param->has_default;
                method_meta.params.push_back(std::move(param_meta));
            }
            struct funcdef *default_func =
                method->func ? (struct funcdef *)method->func : NULL;
            struct type_constraint *declared_return =
                default_func && default_func->ret_type
                    ? default_func->ret_type
                    : method->ret_type;
            if (declared_return)
                method_meta.return_type =
                    std::make_shared<type_constraint>(*declared_return);
            if (method->has_default) {
                uint32_t function =
                    add_function((struct funcdef *)method->func);
                if (!ok)
                    return 0;
                method_meta.code = proto.consts[function].function;
            }
            meta.methods.push_back(std::move(method_meta));
        }
        proto.protocol_decls.push_back(std::move(meta));
        return static_cast<uint32_t>(proto.protocol_decls.size() - 1);
    }

    uint32_t add_impl_decl(struct impl_node *node) {
        skein::bc::ImplDeclMeta meta;
        meta.source = node->source ? node->source : "";
        meta.type_name = node->type_name ? node->type_name : "";
        for (struct struct_member *member = node->methods; member;
             member = member->next) {
            if (member->nodetype != 'm')
                continue;
            skein::bc::DeclMethodMeta method;
            method.name = member->name ? member->name : "";
            uint32_t function =
                add_function((struct funcdef *)member->func);
            if (!ok) return 0;
            method.code = proto.consts[function].function;
            meta.methods.push_back(std::move(method));
        }
        proto.impl_decls.push_back(std::move(meta));
        return static_cast<uint32_t>(proto.impl_decls.size() - 1);
    }

    bool check_field_default(
        struct ast *expr, const char *field,
        const std::vector<std::string> &forbidden) {
        if (!expr)
            return true;
        struct function_code code;
        code.sl = NULL;
        code.nparams = 0;
        code.required_params = 0;
        code.name = std::string("<default ") + (field ? field : "?") + ">";
        code.package = proto.package;
        std::string error;
        if (!skein_check_default_references(&code, expr, forbidden,
                                            proto.package, error)) {
            std::string message = "field ";
            message += field ? field : "?";
            message += " default: ";
            message += error;
            fail(message.c_str());
            diag_set_location(expr->line, expr->column);
            yyerror("%s", message.c_str());
            return false;
        }
        return true;
    }

    std::shared_ptr<function_code> add_field_default(
        struct ast *expr, const char *owner, const char *field) {        if (!expr)
            return NULL;
        std::shared_ptr<function_code> code(
            new function_code(), free_function_code);
        code->sl = NULL;
        code->nparams = 0;
        code->required_params = 0;
        code->name = std::string("<default ") +
                     (owner ? owner : "?") + "." +
                     (field ? field : "?") + ">";
        code->is_method = false;
        code->variadic = false;
        code->is_generator = false;
        code->def_line = expr->line;
        code->def_column = expr->column;
        code->package = proto.package;
        set_function_body_ast(code.get(), newret(expr));
        return code;
    }

    uint32_t add_function(struct funcdef *node) {
        std::shared_ptr<function_code> code = get_function_code(node);
        if (code) {
            code->package = proto.package;
            if ((inside_function || !slot_scopes.empty()) &&
                code->upvalues.empty()) {
                std::unordered_set<std::string> seen;
                auto add = [&](const std::string &name) {
                    if (!name.empty() && seen.insert(name).second)
                        code->upvalues.push_back(name);
                };
                for (const std::string &name : param_names)
                    add(name);
                std::vector<std::string> locals(declared_names.begin(),
                                                declared_names.end());
                std::sort(locals.begin(), locals.end());
                for (const std::string &name : locals)
                    add(name);
                for (const std::string &name : upvalue_names)
                    add(name);
                for (const SlotScope &scope : slot_scopes) {
                    for (const auto &entry : scope.names)
                        add(entry.first);
                }
            }
        }
        if (node)
            node->moved = 1;
        Constant c;
        c.kind = ConstantKind::Function;
        c.function = std::move(code);
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    uint32_t add_call_desc(std::vector<bool> spread) {
        skein::bc::CallDesc desc;
        desc.spread = std::move(spread);
        proto.call_descs.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.call_descs.size() - 1);
    }

    uint32_t add_construct(const char *source, const char *name,
                           const char *variant,
                           std::vector<std::string> fields,
                           std::vector<bool> spread,
                           bool is_ref = false) {
        skein::bc::ConstructDesc desc;
        desc.source = source ? source : "";
        desc.name = name ? name : "";
        desc.variant = variant ? variant : "";
        desc.is_ref = is_ref;
        desc.fields = std::move(fields);
        desc.spread = std::move(spread);
        proto.constructs.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.constructs.size() - 1);
    }

    uint32_t add_field_or_enum(const char *source, const char *base,
                               const char *field) {
        skein::bc::FieldOrEnumDesc desc;
        desc.source = source ? source : "";
        desc.base = base ? base : "";
        desc.field = field ? field : "";
        proto.field_or_enum.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.field_or_enum.size() - 1);
    }

    std::vector<skein::bc::DeferCapture> collect_defer_captures(
        const struct ast *expression) const {
        std::vector<skein::bc::DeferCapture> captures;
        std::unordered_set<std::string> referenced;
        skein::ast_analysis::collect_referenced_names(expression, referenced);
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string &name,
                       skein::bc::DeferCaptureKind kind, uint32_t index) {
            if (name.empty() || referenced.count(name) == 0 ||
                !seen.insert(name).second)
                return;
            skein::bc::DeferCapture capture;
            capture.name = name;
            capture.kind = kind;
            capture.index = index;
            captures.push_back(std::move(capture));
        };
        for (size_t depth = slot_scopes.size(); depth > 0; --depth) {
            const SlotScope &scope = slot_scopes[depth - 1];
            std::vector<std::pair<std::string, int>> names(
                scope.names.begin(), scope.names.end());
            std::sort(names.begin(), names.end(),
                      [](const std::pair<std::string, int> &left,
                         const std::pair<std::string, int> &right) {
                          return left.first < right.first;
                      });
            for (const auto &entry : names)
                add(entry.first, skein::bc::DeferCaptureKind::Slot,
                    static_cast<uint32_t>(entry.second));
        }
        for (size_t i = 0; i < param_names.size(); ++i)
            add(param_names[i], skein::bc::DeferCaptureKind::Param,
                static_cast<uint32_t>(i));
        for (size_t i = 0; i < upvalue_names.size(); ++i)
            add(upvalue_names[i], skein::bc::DeferCaptureKind::Upvalue,
                static_cast<uint32_t>(i));
        return captures;
    }

    uint32_t add_expression_proto(
        struct ast *expression, const std::string &suffix,
        const std::vector<skein::bc::DeferCapture> *captures = nullptr,
        bool forbid_control_exit = false) {
        Compiler child(proto.package);
        child.inside_function = captures ? true : inside_function;
        child.handler_body_depth = handler_body_depth;
        child.forbid_control_exit = forbid_control_exit;
        child.proto.name =
            (proto.name.empty() ? std::string("<anonymous>") : proto.name) +
            suffix;
        if (captures) {
            for (const skein::bc::DeferCapture &capture : *captures) {
                child.upvalue_names.push_back(capture.name);
                child.proto.upvalues.push_back(capture.name);
            }
        }
        child.compile(expression);
        if (!child.ok) {
            fail(child.error.empty() ? "cannot compile expression"
                                     : child.error.c_str());
            return 0;
        }
        if (!finalize_proto(child.proto, child.error)) {
            fail(child.error.c_str());
            return 0;
        }
        proto.defer_protos.push_back(
            std::make_shared<const Proto>(std::move(child.proto)));
        proto.defer_captures.push_back(
            captures ? *captures
                     : std::vector<skein::bc::DeferCapture>());
        return static_cast<uint32_t>(proto.defer_protos.size() - 1);
    }

    uint32_t add_defer_proto(struct ast *expression) {
        std::vector<skein::bc::DeferCapture> captures =
            collect_defer_captures(expression);
        return add_expression_proto(expression, "$defer", &captures, true);
    }

    uint32_t add_import(const struct import_node *node,
                        const std::vector<uint32_t> &selection_slots = {}) {
        skein::bc::ImportDesc desc;
        desc.path = node && node->path ? node->path : "";
        desc.alias = node && node->alias ? node->alias : "";
        desc.base = node && node->base ? node->base : "";
        size_t index = 0;
        for (const struct import_selection *sel =
                 node ? node->selections : NULL;
             sel; sel = sel->next, ++index) {
            skein::bc::ImportSelectionDesc item;
            item.name = sel->name ? sel->name : "";
            item.alias = sel->alias ? sel->alias
                                    : (sel->name ? sel->name : "");
            item.slot = index < selection_slots.size()
                            ? selection_slots[index]
                            : UINT32_MAX;
            desc.selections.push_back(std::move(item));
        }
        proto.imports.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.imports.size() - 1);
    }

    static bool collect_index_chain(ast *target,
                                    std::vector<ast *> &indices,
                                    std::vector<bool> &fields,
                                    ast *&base) {
        if (!target)
            return false;
        if (target->nodetype == 'G' || target->nodetype == 'j') {
            if (!collect_index_chain(target->l, indices, fields, base))
                return false;
            indices.push_back(target->r);
            fields.push_back(target->nodetype == 'j');
            return true;
        }
        base = target;
        return target->nodetype == 'N';
    }

    uint32_t add_label(const char *label) {
        if (!label || !*label)
            return UINT32_MAX;
        return add_name(label);
    }

    uint32_t add_bool(bool value) {
        Constant c;
        c.kind = ConstantKind::Value;
        c.value = std::shared_ptr<value_t>(newboolval(value), value_deleter());
        proto.consts.push_back(std::move(c));
        return static_cast<uint32_t>(proto.consts.size() - 1);
    }

    void fail(const char *message) {
        if (ok)
            error = message ? message : "cannot compile";
        ok = false;
    }

    void compile_runtime_error(const char *message, SourceLoc loc) {
        uint32_t index = add_name(message ? message : "runtime error");
        emit(Op::RuntimeError, 0, 0, index, loc);
    }


    int param_index(const char *name) const {
        if (!name || declared_names.count(name) != 0)
            return -1;
        for (size_t i = 0; i < param_names.size(); ++i) {
            if (param_names[i] == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    int upvalue_index(const char *name) const {
        if (!name || declared_names.count(name) != 0)
            return -1;
        for (size_t i = 0; i < upvalue_names.size(); ++i) {
            if (upvalue_names[i] == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    int global_index(const char *name) const {
        if (!slot_fast_path || !name ||
            declared_names.count(name) != 0)
            return -1;
        int slot = global_symbol_slot(name);
        if (slot < 0 || slot > UINT16_MAX)
            return -1;
        return slot;
    }


    int find_slot(const char *name) const {
        if (!name)
            return -1;
        for (size_t i = slot_scopes.size(); i > 0; --i) {
            auto it = slot_scopes[i - 1].names.find(name);
            if (it != slot_scopes[i - 1].names.end())
                return it->second;
        }
        return -1;
    }

    /* Names resolved by the compiler can use the normal field-access path.
     * Unresolved simple names may be enum types in dot syntax (Enum.Variant),
     * so those use GetFieldOrEnum and decide at runtime. */
    bool name_is_resolved(const char *name) const {
        if (!name)
            return false;
        if (find_slot(name) >= 0)
            return true;
        if (param_index(name) >= 0)
            return true;
        if (upvalue_index(name) >= 0)
            return true;
        if (global_index(name) >= 0)
            return true;
        return lookupsym(name) != NULL;
    }

    static bool const_prop_enabled() {
        static const bool enabled = []() {
            const char *disabled = std::getenv("SKEIN_NO_CONST_PROP");
            return !(disabled && *disabled &&
                     std::strcmp(disabled, "0") != 0);
        }();
        return enabled;
    }

    static bool is_const_scalar(const value_t *value) {
        if (!value)
            return false;
        switch (value->type) {
            case T_int:
            case T_float:
            case T_bool:
            case T_string:
            case T_char:
            case T_byte:
            case T_null:
                return true;
            default:
                return false;
        }
    }

    value_t *find_const(const char *name) const {
        if (!name || !const_prop_enabled() || !slot_fast_path)
            return NULL;
        for (size_t i = slot_scopes.size(); i > 0; --i) {
            const SlotScope &scope = slot_scopes[i - 1];
            auto it = scope.names.find(name);
            if (it == scope.names.end())
                continue;
            auto constant = scope.constants.find(name);
            return constant == scope.constants.end()
                       ? NULL
                       : constant->second.get();
        }
        return NULL;
    }

    void remember_const(const char *name, value_t *value) {
        if (!name || !value || !slot_fast_path || slot_scopes.empty() ||
            !is_const_scalar(value)) {
            freeval(value);
            return;
        }
        slot_scopes.back().constants[name] = own_value(value);
    }

    void push_slot_scope() {
        if (slot_fast_path)
            slot_scopes.emplace_back();
    }

    void emit_clear_current_slots(SourceLoc loc) {
        if (!slot_fast_path || slot_scopes.empty())
            return;
        for (int index : slot_scopes.back().declared) {
            emit(Op::ClearSlot, 0, 0,
                 static_cast<uint32_t>(index), loc);
        }
    }

    void pop_slot_scope(bool clear, SourceLoc loc) {
        if (!slot_fast_path || slot_scopes.empty())
            return;
        if (clear)
            emit_clear_current_slots(loc);
        slot_scopes.pop_back();
    }




    void attach_pattern_slots(uint32_t pattern_index,
                              const std::vector<std::string> &names,
                              bool is_const) {
        if (!slot_fast_path || slot_scopes.empty() ||
            pattern_index >= proto.patterns.size())
            return;
        for (const std::string &name : names) {
            uint32_t slot = add_slot(name.c_str(), NULL, is_const);
            slot_scopes.back().names[name] = static_cast<int>(slot);
            slot_scopes.back().declared.push_back(
                static_cast<int>(slot));
            skein::bc::PatternSlotDesc binding;
            binding.name = name;
            binding.slot = slot;
            proto.patterns[pattern_index].binding_slots.push_back(
                std::move(binding));
        }
    }

    uint32_t add_slot(const char *name,
                      const struct type_constraint *type, bool is_const) {
        skein::bc::SlotDesc slot;
        slot.name = name ? name : "";
        slot.type = type ? copy_type_constraint(type)
                         : any_type_constraint();
        slot.is_const = is_const;
        proto.slots.push_back(std::move(slot));
        proto.uses_slots = true;
        return static_cast<uint32_t>(proto.slots.size() - 1);
    }

    void emit_null(SourceLoc loc) {
        emit(Op::LoadNull, 0, 0, 0, loc);
    }

    static bool is_numeric_value(const value_t *value) {
        return value && (value->type == T_int || value->type == T_float);
    }

    static double numeric_value(const value_t *value) {
        if (value->type == T_int)
            return (double)((const struct int_value_t *)value)->value;
        return ((const struct float_value_t *)value)->value;
    }

    static bool ll_in_range(__int128 value) {
        return value >= (__int128)std::numeric_limits<long long>::min() &&
               value <= (__int128)std::numeric_limits<long long>::max();
    }

    static value_t *fold_binary(char op, value_t *left, value_t *right) {
        if (!left || !right)
            return NULL;
        if (left->type == T_int && right->type == T_int) {
            const long long x = ((struct int_value_t *)left)->value;
            const long long y = ((struct int_value_t *)right)->value;
            switch (op) {
                case '+':
                case '-':
                case '*': {
                    __int128 n = op == '+' ? (__int128)x + y
                                : op == '-' ? (__int128)x - y
                                            : (__int128)x * y;
                    return ll_in_range(n) ? newintval((long long)n) : NULL;
                }
                case '/':
                    if (y == 0 ||
                        (x == std::numeric_limits<long long>::min() &&
                         y == -1))
                        return NULL;
                    return newintval(x / y);
                case '%':
                    if (y == 0)
                        return NULL;
                    return newintval(y == -1 ? 0 : x % y);
                case '&': return newintval(x & y);
                case '|': return newintval(x | y);
                case '^': return newintval(x ^ y);
                case 's':
                    if (y < 0 || y >= 64)
                        return NULL;
                    return newintval(
                        (long long)((unsigned long long)x << y));
                case 'r':
                    if (y < 0 || y >= 64)
                        return NULL;
                    return newintval(x >> y);
                default:
                    return NULL;
            }
        }

        if (op == '+' && left->type == T_string &&
            right->type == T_string) {
            std::string text = ((struct string_value_t *)left)->value;
            text += ((struct string_value_t *)right)->value;
            return newstrval(text.c_str());
        }

        if (is_numeric_value(left) && is_numeric_value(right)) {
            const double x = numeric_value(left);
            const double y = numeric_value(right);
            switch (op) {
                case '+': return newfloatval(x + y);
                case '-': return newfloatval(x - y);
                case '*': return newfloatval(x * y);
                case '/':
                    return y == 0.0 ? NULL : newfloatval(x / y);
                case '%':
                    return y == 0.0 ? NULL
                                    : newfloatval(std::fmod(x, y));
                default:
                    return NULL;
            }
        }
        return NULL;
    }

    static value_t *fold_compare(char op, value_t *left, value_t *right) {
        int cmp = 0;
        if (left->type == T_int && right->type == T_int) {
            const long long x = ((struct int_value_t *)left)->value;
            const long long y = ((struct int_value_t *)right)->value;
            cmp = x < y ? -1 : (x > y ? 1 : 0);
        } else if (left->type == T_float && right->type == T_float) {
            const double x = ((struct float_value_t *)left)->value;
            const double y = ((struct float_value_t *)right)->value;
            if (std::isnan(x) || std::isnan(y))
                return newboolval(op == '3');
            cmp = x < y ? -1 : (x > y ? 1 : 0);
        } else {
            return NULL;
        }
        switch (op) {
            case '1': return newboolval(cmp > 0);
            case '2': return newboolval(cmp < 0);
            case '3': return newboolval(cmp != 0);
            case '4': return newboolval(cmp == 0);
            case '5': return newboolval(cmp >= 0);
            case '6': return newboolval(cmp <= 0);
            default: return NULL;
        }
    }

    static const char *literal_string(ast *node) {
        if (!node || node->nodetype != 'V')
            return NULL;
        struct val *literal = (struct val *)node;
        if (!literal->val || literal->val->type != T_string)
            return NULL;
        return ((struct string_value_t *)literal->val)->value;
    }

    static bool literal_text(ast *node) {
        if (!node || node->nodetype != 'V')
            return false;
        struct val *literal = (struct val *)node;
        return literal->val &&
               (literal->val->type == T_string ||
                literal->val->type == T_char);
    }

    /* Conservative purity check for the extra expression in
     * `xs = std.list.append(xs, expr)`: it must not be able to reassign
     * or free `xs` while the receiver is still on the stack. */
    static bool grow_pure(ast *node) {
        if (!node)
            return true;
        switch (node->nodetype) {
            case 'V':
            case 'N':
                return true;
            case 'l': {
                for (ast *item = node; item;
                     item = ((struct list_node *)item)->next) {
                    if (!grow_pure(((struct list_node *)item)->value))
                        return false;
                }
                return true;
            }
            case 'G':
            case 'j':
                return grow_pure(node->l) && grow_pure(node->r);
            default:
                return false;
        }
    }

    static bool is_std_list_append(ast *callee) {
        if (!callee || callee->nodetype != 'j')
            return false;
        const char *field = literal_string(callee->r);
        if (!field || std::strcmp(field, "append") != 0)
            return false;
        ast *module = callee->l;
        if (!module || module->nodetype != 'j')
            return false;
        const char *module_name = literal_string(module->r);
        if (!module_name || std::strcmp(module_name, "list") != 0)
            return false;
        ast *root = module->l;
        if (!root || root->nodetype != 'N')
            return false;
        const char *root_name = ((struct symref *)root)->name;
        return root_name && std::strcmp(root_name, "std") == 0;
    }

    value_t *fold_node(ast *node) {
        if (!node)
            return NULL;
        if (node->nodetype == 'V') {
            struct val *literal = (struct val *)node;
            return literal->val ? dupval(literal->val) : NULL;
        }
        if (node->nodetype == 'N') {
            value_t *constant = find_const(((struct symref *)node)->name);
            return constant ? dupval(constant) : NULL;
        }
        if (node->nodetype == 'M' || node->nodetype == '~') {
            value_t *value = fold_node(node->l);
            if (!value)
                return NULL;
            value_t *result = NULL;
            if (node->nodetype == 'M') {
                if (value->type == T_int) {
                    long long n = ((struct int_value_t *)value)->value;
                    if (n != std::numeric_limits<long long>::min())
                        result = newintval(-n);
                } else if (value->type == T_float) {
                    result = newfloatval(
                        -((struct float_value_t *)value)->value);
                }
            } else if (value->type == T_int) {
                result = newintval(
                    ~((struct int_value_t *)value)->value);
            }
            freeval(value);
            return result;
        }
        if (node->nodetype >= '1' && node->nodetype <= '6') {
            value_t *left = fold_node(node->l);
            value_t *right = fold_node(node->r);
            value_t *result =
                left && right ? fold_compare(node->nodetype, left, right)
                              : NULL;
            freeval(left);
            freeval(right);
            return result;
        }
        switch (node->nodetype) {
            case '+':
            case '-':
            case '*':
            case '/':
            case '%':
            case '&':
            case '|':
            case '^':
            case 's':
            case 'r': {
                value_t *left = fold_node(node->l);
                value_t *right = fold_node(node->r);
                value_t *result =
                    left && right
                        ? fold_binary(node->nodetype, left, right)
                        : NULL;
                freeval(left);
                freeval(right);
                return result;
            }
            default:
                return NULL;
        }
    }

    void compile(ast *node) {
        if (!ok)
            return;
        if (!node) {
            emit(Op::LoadNull, 0, 0, 0, SourceLoc());
            return;
        }

        SourceLoc here = loc_of(node);
        if (value_t *folded = fold_node(node)) {
            uint32_t index = add_value(folded);
            freeval(folded);
            emit(Op::LoadConst, 0, 0, index, here);
            return;
        }
        switch (node->nodetype) {
            case 'V': {
                struct val *literal = (struct val *)node;
                if (!literal->val) {
                    emit_null(here);
                } else {
                    uint32_t index = add_value(literal->val);
                    emit(Op::LoadConst, 0, 0, index, here);
                }
                return;
            }

            case 'N': {
                struct symref *ref = (struct symref *)node;
                if (value_t *constant = find_const(ref->name)) {
                    uint32_t index = add_value(dupval(constant));
                    emit(Op::LoadConst, 0, 0, index, here);
                    return;
                }
                int slot = slot_fast_path ? find_slot(ref->name) : -1;
                if (slot >= 0) {
                    emit(Op::LoadSlot, 0, 0,
                         static_cast<uint32_t>(slot), here);
                } else {
                    slot = param_fast_path ? param_index(ref->name) : -1;
                    if (slot >= 0) {
                        emit(Op::LoadParam, 0, 0,
                             static_cast<uint32_t>(slot), here);
                    } else {
                        int upvalue = upvalue_index(ref->name);
                        if (upvalue >= 0) {
                            emit(Op::LoadUpvalue, 0, 0,
                                 static_cast<uint32_t>(upvalue), here);
                        } else {
                            int global = global_index(ref->name);
                            uint32_t index =
                                add_name(ref->name ? ref->name : "");
                            if (global >= 0) {
                                emit(Op::LoadGlobal, 0,
                                     static_cast<uint16_t>(global),
                                     index, here);
                            } else {
                                emit(Op::LoadName, 0, 0, index, here);
                            }
                        }
                    }
                }
                return;
            }

            case 'D': {
                struct symdef *def = (struct symdef *)node;
                if (slot_fast_path && !slot_scopes.empty() &&
                    def->name) {
                    SlotScope &scope = slot_scopes.back();
                    if (scope.names.count(def->name) != 0) {
                        uint32_t message = add_name(
                            std::string("symbol repeated ") + def->name);
                        emit(Op::RuntimeError, 0, 0, message, here);
                        return;
                    }
                    uint32_t slot = add_slot(
                        def->name, def->type, def->is_const != 0);
                    scope.names[def->name] = static_cast<int>(slot);
                    scope.declared.push_back(static_cast<int>(slot));
                    value_t *constant = NULL;
                    if (def->is_const && def->v && const_prop_enabled())
                        constant = fold_node(def->v);
                    if (def->v) {
                        compile(def->v);
                        if (!ok) {
                            freeval(constant);
                            return;
                        }
                    }
                    if (constant)
                        remember_const(def->name, constant);
                    emit(Op::DefineSlot, def->v ? 1 : 0, 0, slot, here);
                    return;
                }
                if (def->v) {
                    uint32_t name = add_name(def->name ? def->name : "");
                    emit(Op::CheckRedefine, 0, 0, name, here);
                    compile(def->v);
                    if (!ok) return;
                }
                uint32_t index = add_decl(def);
                emit(Op::DefineName, def->v ? 1 : 0, 0, index, here);
                return;
            }

            case '=': {
                struct symasgn *asgn = (struct symasgn *)node;
                if (asgn->target && asgn->target->nodetype == 'N') {
                    struct symref *ref = (struct symref *)asgn->target;
                    bool grown = false;
                    if (asgn->v && asgn->v->nodetype == '+' &&
                        asgn->v->l && asgn->v->l->nodetype == 'N' &&
                        asgn->v->r && asgn->v->r->nodetype == 'l' &&
                        ref->name &&
                        ((struct symref *)asgn->v->l)->name &&
                        std::strcmp(((struct symref *)asgn->v->l)->name,
                                    ref->name) == 0 &&
                        grow_pure(asgn->v->r)) {
                        compile(asgn->v->l);
                        if (!ok) return;
                        compile(asgn->v->r);
                        if (!ok) return;
                        emit(Op::ListConcatSelf, 0, 0, 0, loc_of(asgn->v));
                        grown = true;
                    } else if (asgn->v && asgn->v->nodetype == '+' &&
                               asgn->v->l &&
                               asgn->v->l->nodetype == 'N' && ref->name &&
                               ((struct symref *)asgn->v->l)->name &&
                               std::strcmp(
                                   ((struct symref *)asgn->v->l)->name,
                                   ref->name) == 0 &&
                               literal_text(asgn->v->r)) {
                        compile(asgn->v->l);
                        if (!ok) return;
                        compile(asgn->v->r);
                        if (!ok) return;
                        emit(Op::StringAppendSelf, 0, 0, 0,
                             loc_of(asgn->v));
                        grown = true;
                    } else if (asgn->v && asgn->v->nodetype == 'C') {
                        struct ufncall *call = (struct ufncall *)asgn->v;
                        std::vector<ast *> grow_args;
                        collect_chain(call->args, grow_args);
                        if (!call->launch && grow_args.size() == 2 &&
                            is_std_list_append(call->callee) &&
                            grow_args[0] &&
                            grow_args[0]->nodetype == 'N' && ref->name &&
                            ((struct symref *)grow_args[0])->name &&
                            std::strcmp(((struct symref *)grow_args[0])->name,
                                        ref->name) == 0 &&
                            grow_pure(grow_args[1])) {
                            compile(call->callee);
                            if (!ok) return;
                            compile(grow_args[0]);
                            if (!ok) return;
                            compile(grow_args[1]);
                            if (!ok) return;
                            emit(Op::ListAppendSelf, 0, 1, 0, loc_of(asgn->v));
                            grown = true;
                        }
                    }
                    if (!grown) {
                        compile(asgn->v);
                        if (!ok) return;
                    }
                    int slot = slot_fast_path ? find_slot(ref->name) : -1;
                    if (slot >= 0) {
                        emit(Op::StoreSlot, 0, 0,
                             static_cast<uint32_t>(slot), here);
                    } else {
                        slot = param_fast_path ? param_index(ref->name) : -1;
                        if (slot >= 0) {
                            emit(Op::StoreParam, 0, 0,
                                 static_cast<uint32_t>(slot), here);
                        } else {
                            int upvalue = upvalue_index(ref->name);
                            if (upvalue >= 0) {
                                emit(Op::StoreUpvalue, 0, 0,
                                     static_cast<uint32_t>(upvalue), here);
                            } else {
                                int global = global_index(ref->name);
                                uint32_t index =
                                    add_name(ref->name ? ref->name : "");
                                if (global >= 0) {
                                    emit(Op::StoreGlobal, 0,
                                         static_cast<uint16_t>(global),
                                         index, here);
                                } else {
                                    emit(Op::StoreName, 0, 0, index, here);
                                }
                            }
                        }
                    }
                    return;
                }
                if (asgn->target &&
                    asgn->target->nodetype == NODE_REF_TARGET) {
                    compile(asgn->target->l);
                    if (!ok) return;
                    emit(Op::LoadConst, 0, 0,
                         add_builtin_value(B_ref_set), here);
                    emit(Op::Swap, 0, 0, 0, here);
                    compile(asgn->v);
                    if (!ok) return;
                    emit(Op::Call, 0, 2, 2, here);
                    return;
                }
                if (asgn->target &&
                    (asgn->target->nodetype == 'G' ||
                     asgn->target->nodetype == 'j')) {
                    std::vector<ast *> indices;
                    std::vector<bool> fields;
                    ast *base = NULL;
                    if (!collect_index_chain(asgn->target, indices, fields,
                                             base) ||
                        !base || base->nodetype != 'N') {
                        compile_runtime_error(
                            "cannot assign through this expression", here);
                        return;
                    }
                    compile(asgn->v);
                    if (!ok) return;
                    for (ast *idx_ast : indices) {
                        compile(idx_ast);
                        if (!ok) return;
                    }
                    uint32_t desc = add_index_for_base(
                        ((struct symref *)base)->name, fields);
                    emit(Op::AssignIndex, 0, 0, desc, here);
                    return;
                }
                compile_runtime_error("cannot assign to this expression",
                                      here);
                return;
            }

            case 'a': {
                struct compound_asgn *asgn = (struct compound_asgn *)node;
                if (asgn->target && asgn->target->nodetype == 'N') {
                    struct symref *ref = (struct symref *)asgn->target;
                    compile(asgn->v);
                    if (!ok) return;
                    int slot = slot_fast_path ? find_slot(ref->name) : -1;
                    if (slot >= 0) {
                        emit(Op::CompoundSlot,
                             static_cast<uint8_t>(asgn->op), 0,
                             static_cast<uint32_t>(slot), here);
                    } else {
                        slot = param_fast_path ? param_index(ref->name) : -1;
                        if (slot >= 0) {
                            emit(Op::CompoundParam,
                                 static_cast<uint8_t>(asgn->op), 0,
                                 static_cast<uint32_t>(slot), here);
                        } else {
                            int upvalue = upvalue_index(ref->name);
                            if (upvalue >= 0) {
                                emit(Op::CompoundUpvalue,
                                     static_cast<uint8_t>(asgn->op), 0,
                                     static_cast<uint32_t>(upvalue), here);
                            } else {
                                int global = global_index(ref->name);
                                uint32_t index =
                                    add_name(ref->name ? ref->name : "");
                                if (global >= 0) {
                                    emit(Op::CompoundGlobal,
                                         static_cast<uint8_t>(asgn->op),
                                         static_cast<uint16_t>(global),
                                         index, here);
                                } else {
                                    emit(Op::CompoundName,
                                         static_cast<uint8_t>(asgn->op), 0,
                                         index, here);
                                }
                            }
                        }
                    }
                    return;
                }
                if (asgn->target &&
                    (asgn->target->nodetype == 'G' ||
                     asgn->target->nodetype == 'j')) {
                    std::vector<ast *> indices;
                    std::vector<bool> fields;
                    ast *base = NULL;
                    if (!collect_index_chain(asgn->target, indices, fields,
                                             base) ||
                        !base || base->nodetype != 'N') {
                        compile_runtime_error(
                            "cannot assign through this expression", here);
                        return;
                    }
                    compile(asgn->v);
                    if (!ok) return;
                    for (ast *idx_ast : indices) {
                        compile(idx_ast);
                        if (!ok) return;
                    }
                    uint32_t desc = add_index_for_base(
                        ((struct symref *)base)->name, fields);
                    emit(Op::CompoundIndex,
                         static_cast<uint8_t>(asgn->op), 0, desc, here);
                    return;
                }
                compile_runtime_error("cannot assign to this expression",
                                      here);
                return;
            }

            case 'U': {
                struct incdec_node *inc = (struct incdec_node *)node;
                if (inc->target && inc->target->nodetype == 'N') {
                    struct symref *ref = (struct symref *)inc->target;
                    int slot = slot_fast_path ? find_slot(ref->name) : -1;
                    if (slot >= 0) {
                        emit(Op::IncDecSlot, inc->prefix ? 1 : 0,
                             inc->delta > 0 ? 1 : 0,
                             static_cast<uint32_t>(slot), here);
                    } else {
                        slot = param_fast_path ? param_index(ref->name) : -1;
                        if (slot >= 0) {
                            emit(Op::IncDecParam, inc->prefix ? 1 : 0,
                                 inc->delta > 0 ? 1 : 0,
                                 static_cast<uint32_t>(slot), here);
                        } else {
                            int upvalue = upvalue_index(ref->name);
                            if (upvalue >= 0) {
                                emit(Op::IncDecUpvalue,
                                     inc->prefix ? 1 : 0,
                                     inc->delta > 0 ? 1 : 0,
                                     static_cast<uint32_t>(upvalue), here);
                            } else {
                                int global = global_index(ref->name);
                                uint32_t index =
                                    add_name(ref->name ? ref->name : "");
                                if (global >= 0) {
                                    emit(Op::IncDecGlobal,
                                         static_cast<uint8_t>(
                                             (inc->prefix ? 1 : 0) |
                                             (inc->delta > 0 ? 2 : 0)),
                                         static_cast<uint16_t>(global),
                                         index, here);
                                } else {
                                    emit(Op::IncDecName,
                                         inc->prefix ? 1 : 0,
                                         inc->delta > 0 ? 1 : 0,
                                         index, here);
                                }
                            }
                        }
                    }
                    return;
                }
                if (inc->target &&
                    (inc->target->nodetype == 'G' ||
                     inc->target->nodetype == 'j')) {
                    std::vector<ast *> indices;
                    std::vector<bool> fields;
                    ast *base = NULL;
                    if (!collect_index_chain(inc->target, indices, fields,
                                             base) ||
                        !base || base->nodetype != 'N') {
                        compile_runtime_error(
                            "cannot assign through this expression", here);
                        return;
                    }
                    for (ast *idx_ast : indices) {
                        compile(idx_ast);
                        if (!ok) return;
                    }
                    uint32_t desc = add_index_for_base(
                        ((struct symref *)base)->name, fields);
                    emit(Op::IncDecIndex, inc->prefix ? 1 : 0,
                         inc->delta > 0 ? 1 : 0, desc, here);
                    return;
                }
                compile_runtime_error(
                    inc->delta > 0 ? "cannot increment this expression"
                                   : "cannot decrement this expression",
                    here);
                return;
            }

            case 'M':
                compile(node->l);
                if (!ok) return;
                emit(Op::Neg, 0, 0, 0, here);
                return;

            case '!':
                compile(node->l);
                if (!ok) return;
                emit(Op::Not, 0, 0, 0, here);
                return;

            case '~':
                compile(node->l);
                if (!ok) return;
                emit(Op::BitNot, 0, 0, 0, here);
                return;

            case '+':
            case '-':
            case '*':
            case '/':
            case '%':
            case '&':
            case '|':
            case '^':
            case 's':
            case 'r': {
                compile(node->l);
                compile(node->r);
                if (!ok) return;
                Op op = Op::Nop;
                switch (node->nodetype) {
                    case '+': op = Op::Add; break;
                    case '-': op = Op::Sub; break;
                    case '*': op = Op::Mul; break;
                    case '/': op = Op::Div; break;
                    case '%': op = Op::Mod; break;
                    case '&': op = Op::BitAnd; break;
                    case '|': op = Op::BitOr; break;
                    case '^': op = Op::BitXor; break;
                    case 's': op = Op::Shl; break;
                    case 'r': op = Op::Shr; break;
                }
                emit(op, 0, 0, 0, here);
                return;
            }

            case 'A':
                compile_logical(node, true);
                return;

            case 'Q':
                compile_logical(node, false);
                return;

            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6': {
                compile(node->l);
                compile(node->r);
                if (!ok) return;
                Op op = Op::CmpEq;
                switch (node->nodetype) {
                    case '1': op = Op::CmpGt; break;
                    case '2': op = Op::CmpLt; break;
                    case '3': op = Op::CmpNe; break;
                    case '4': op = Op::CmpEq; break;
                    case '5': op = Op::CmpGe; break;
                    case '6': op = Op::CmpLe; break;
                }
                emit(op, 0, 0, 0, here);
                return;
            }

            case 'T': {
                struct flow *f = (struct flow *)node;
                size_t jf = compile_condition(f->cond, false, here);
                if (!ok) return;
                compile(f->tl);
                if (!ok) return;
                size_t jend = emit_jump(Op::Jump, here);
                patch(jf, proto.code.size());
                compile(f->el);
                if (!ok) return;
                patch(jend, proto.code.size());
                return;
            }

            case 'I': {
                struct flow *f = (struct flow *)node;
                size_t jf = compile_condition(f->cond, false, here);
                if (!ok) return;
                compile(f->tl);
                if (!ok) return;
                size_t jend = emit_jump(Op::Jump, here);
                patch(jf, proto.code.size());
                if (f->el)
                    compile(f->el);
                else
                    emit_null(here);
                if (!ok) return;
                patch(jend, proto.code.size());
                return;
            }

            case 'W':
                compile_while(node);
                return;

            case 'E':
                compile_do(node);
                return;

            case 'P':
                compile_for(node);
                return;

            case 'B': {
                struct nbreak *brk = (struct nbreak *)node;
                compile_break_continue(true, brk->label, here);
                return;
            }

            case 'O': {
                struct ncontinue *ctn = (struct ncontinue *)node;
                compile_break_continue(false, ctn->label, here);
                return;
            }

            case 'R': {
                struct fnret *ret = (struct fnret *)node;
                if (forbid_control_exit) {
                    uint32_t message = add_name(
                        "defer expression cannot return, break, or continue");
                    emit(Op::RuntimeError, 0, 0, message, here);
                    return;
                }
                if (ret->v && ret->v->nodetype == 'C') {
                    compile_call(ret->v, true);
                    return;
                }
                if (ret->v)
                    compile(ret->v);
                else
                    emit_null(here);
                if (!ok) return;
                emit(Op::Return, 0, 0, 0, here);
                return;
            }

            case 'g': {
                struct label_node *lab = (struct label_node *)node;
                ast *child = lab->stmt;
                if (child && (child->nodetype == 'W' || child->nodetype == 'E' ||
                              child->nodetype == 'P' || child->nodetype == 'F' ||
                              child->nodetype == 'S')) {
                    std::string saved = pending_label;
                    pending_label = lab->name ? lab->name : "";
                    compile(child);
                    pending_label = saved;
                } else {
                    compile(child);
                }
                return;
            }

            case NODE_GOTO: {
                struct goto_node *jump = (struct goto_node *)node;
                std::string name = jump->name ? jump->name : "";
                LabelTarget *target = find_label(name);
                if (!target) {
                    yyerror("goto target %s not found in this block",
                            name.c_str());
                    ok = false;
                } else {
                    used_function_labels.insert(name);
                    if (proto.is_generator) {
                        yyerror("goto is not allowed in generator functions");
                        ok = false;
                    }
                    emit_goto_cleanup(*target, here);
                    if (!ok) return;
                    if (target->pc != static_cast<size_t>(-1)) {
                        emit(Op::Jump, 0, 0,
                             static_cast<uint32_t>(target->pc), here);
                    } else {
                        target->patches.push_back(emit_jump(Op::Jump, here));
                    }
                }
                return;
            }

            case 'l':
                compile_list(node, false);
                return;

            case NODE_TUPLE:
                compile_list(node, true);
                return;

            case 'd':
                compile_dict(node);
                return;

            case 'G': {
                if (((struct index_node *)node)->generic_function_value) {
                    compile(node->l);
                    return;
                }
                /*
                 * `name[key]` with a compile-time key avoids reloading and
                 * duplicating both the container and the key on every
                 * evaluation.  Only direct frame/global bases are fused; the
                 * key must be side-effect free, which folding guarantees.
                 */
                if (node->l && node->l->nodetype == 'N') {
                    const char *base_name =
                        ((struct symref *)node->l)->name;
                    if (base_name && !find_const(base_name)) {
                        uint32_t base_index = UINT32_MAX;
                        skein::bc::IndexBaseKind kind =
                            classify_index_base(base_name, base_index);
                        if (kind == skein::bc::IndexBaseKind::Slot ||
                            kind == skein::bc::IndexBaseKind::Param ||
                            kind == skein::bc::IndexBaseKind::Global) {
                            value_t *key = fold_node(node->r);
                            if (key) {
                                std::vector<bool> fields;
                                fields.push_back(false);
                                uint32_t desc =
                                    add_index(base_name, fields, kind,
                                              base_index);
                                proto.index_ops[desc].read_cache =
                                    add_index_read_cache();
                                proto.index_ops[desc].const_key =
                                    add_value(key);
                                freeval(key);
                                emit(Op::GetIndexBaseCached, 0, 0, desc,
                                     here);
                                return;
                            }
                        }
                    }
                }
                compile(node->l);
                compile(node->r);
                if (!ok) return;
                emit(Op::GetIndexCached, 0, 0, add_index_read_cache(),
                     here);
                return;
            }

            case NODE_REF_TARGET: {
                compile(node->l);
                if (!ok) return;
                emit(Op::LoadConst, 0, 0,
                     add_builtin_value(B_ref_snapshot), here);
                emit(Op::Swap, 0, 0, 0, here);
                emit(Op::Call, 0, 1, 1, here);
                return;
            }

            case NODE_OPTIONAL_CHAIN: {
                struct optional_chain_node *chain =
                    (struct optional_chain_node *)node;
                compile(chain->l);
                if (!ok) return;
                /* A weak base must be upgraded first.  Keeping this in the
                 * bytecode path makes AST/VM/JIT agree on stale weak handles. */
                emit(Op::LoadConst, 0, 0,
                     add_builtin_value(B_ref_upgrade), here);
                emit(Op::Swap, 0, 0, 0, here);
                emit(Op::Call, 0, 1, 1, here);
                struct type_constraint null_type = any_type_constraint();
                null_type.kind = TYPE_NULL;
                uint32_t null_constant = add_type(&null_type);
                emit(Op::Dup, 0, 0, 0, here);
                emit(Op::IsType, 0, 0, null_constant, here);
                size_t jump_null = emit_jump(Op::JumpIfTrue, here);
                const char *field = literal_string(chain->r);
                uint32_t name = add_name(field ? field : "");
                emit(Op::GetFieldName, 0, 0, name, here);
                size_t jump_end = emit_jump(Op::Jump, here);
                patch(jump_null, proto.code.size());
                emit(Op::Pop, 0, 0, 0, here);
                emit_null(here);
                patch(jump_end, proto.code.size());
                return;
            }

            case 'j': {
                const char *field = literal_string(node->r);
                if (field && node->l && node->l->nodetype == 'N') {
                    const char *base = ((struct symref *)node->l)->name;
                    if (base && (!name_is_resolved(base) ||
                                 selective_bindings.count(base) != 0)) {
                        uint32_t desc = add_field_or_enum(
                            proto.package.c_str(), base, field);
                        emit(Op::GetFieldOrEnum, 0, 0, desc, here);
                        return;
                    }
                }
                compile(node->l);
                if (!ok) return;
                if (field) {
                    uint32_t name = add_name(field);
                    emit(Op::GetFieldName, 0, 0, name, here);
                    return;
                }
                compile(node->r);
                if (!ok) return;
                emit(Op::GetField, 0, 0, 0, here);
                return;
            }

            case 'C':
                compile_call(node);
                return;

            case 'X': {
                struct nscope *scope = (struct nscope *)node;
                emit(Op::PushScope, 0, 0, 0, here);
                ++scope_depth;
                push_slot_scope();
                compile(scope->ast);
                if (!ok) return;
                pop_slot_scope(true, here);
                emit(Op::PopScope, 0, 0, 0, here);
                --scope_depth;
                return;
            }

            case 'L':
                compile_statement_list(node);
                return;

            case NODE_FUNCTION_BODY:
                compile_statement_list(node->l);
                return;

            case '`': {
                struct destructure_node *d = (struct destructure_node *)node;
                compile(d->value);
                if (!ok) return;
                uint32_t pattern = add_pattern(d->pattern);
                if (!ok) return;
                std::vector<std::string> bindings;
                skein::ast_analysis::collect_pattern_bindings(d->pattern, bindings);
                attach_pattern_slots(pattern, bindings,
                                     d->is_const != 0);
                emit(Op::MatchBind, d->is_const ? 1 : 0, 0, pattern, here);
                return;
            }

            case 'k': {
                struct is_node *is = (struct is_node *)node;
                compile(is->value);
                if (!ok) return;
                uint32_t index = add_type(is->type);
                emit(Op::IsType, 0, 0, index, here);
                return;
            }

            case NODE_CAST: {
                struct cast_node *cast = (struct cast_node *)node;
                compile(cast->value);
                return;
            }

            case 'Z': {
                struct funcdef *def = (struct funcdef *)node;
                uint32_t descriptor = add_function(def);
                emit(Op::Closure, 0, 0, descriptor, here);
                if (def->name) {
                    if (slot_fast_path && !def->exported &&
                        !slot_scopes.empty()) {
                        SlotScope &scope = slot_scopes.back();
                        if (scope.names.count(def->name) != 0) {
                            uint32_t message = add_name(
                                std::string("symbol repeated ") + def->name);
                            emit(Op::RuntimeError, 0, 0, message, here);
                            return;
                        }
                        uint32_t slot = add_slot(def->name, nullptr, false);
                        scope.names[def->name] = static_cast<int>(slot);
                        scope.declared.push_back(static_cast<int>(slot));
                        emit(Op::DefineSlot, 1, 0, slot, here);
                    } else {
                        uint32_t name = add_name(def->name);
                        emit(Op::BindFunc, def->exported ? 1 : 0, 0, name,
                             here);
                    }
                }
                return;
            }

            case 'Y': {
                uint32_t descriptor =
                    add_struct_decl((struct struct_node *)node);
                if (!ok) return;
                emit(Op::DeclareStruct, 0, 0, descriptor, here);
                emit_null(here);
                return;
            }

            case '<': {
                uint32_t descriptor =
                    add_enum_decl((struct enum_node *)node);
                if (!ok) return;
                emit(Op::DeclareEnum, 0, 0, descriptor, here);
                emit_null(here);
                return;
            }

            case 't': {
                uint32_t descriptor =
                    add_protocol_decl((struct protocol_node *)node);
                if (!ok) return;
                emit(Op::DeclareProtocol, 0, 0, descriptor, here);
                emit_null(here);
                return;
            }

            case 'H': {
                uint32_t descriptor =
                    add_impl_decl((struct impl_node *)node);
                if (!ok) return;
                emit(Op::DeclareImpl, 0, 0, descriptor, here);
                emit_null(here);
                return;
            }

            case 'K': {
                uint32_t descriptor =
                    add_package((struct package_node *)node);
                if (!ok) return;
                emit(Op::DeclarePackage, 0, 0, descriptor, here);
                emit_null(here);
                return;
            }

            case 'y': {
                struct struct_lit_node *lit = (struct struct_lit_node *)node;
                std::vector<std::string> fields;
                std::vector<bool> spread;
                for (struct struct_init *init = lit->fields; init;
                     init = init->next) {
                    compile(init->value);
                    if (!ok) return;
                    fields.push_back(init->name ? init->name : "");
                    spread.push_back(init->name == NULL);
                }
                const std::string source = resolve_selective_type_source(
                    lit->source, lit->name);
                uint32_t descriptor = add_construct(
                    source.c_str(), lit->name, "", std::move(fields),
                    std::move(spread), lit->is_ref);
                emit(Op::NewStruct, 0, 0, descriptor, here);
                return;
            }

            case '?': {
                struct enum_lit_node *lit = (struct enum_lit_node *)node;
                std::vector<std::string> fields;
                std::vector<bool> spread;
                for (struct struct_init *init = lit->fields; init;
                     init = init->next) {
                    if (init->name == NULL) {
                        yyerror("copy-with is only supported for structs");
                        return;
                    }
                    compile(init->value);
                    if (!ok) return;
                    fields.push_back(init->name);
                    spread.push_back(false);
                }
                const std::string source = resolve_selective_type_source(
                    lit->source, lit->name);
                uint32_t descriptor = add_construct(
                    source.c_str(), lit->name, lit->variant,
                    std::move(fields), std::move(spread));
                emit(Op::NewEnum, 0, 0, descriptor, here);
                return;
            }

            case 'w':
                compile(node->l);
                if (!ok) return;
                emit(Op::Yield, 0, 0, 0, here);
                return;

            case 'u': {
                struct control_stmt *stmt = (struct control_stmt *)node;
                if (stmt->value)
                    compile(stmt->value);
                else
                    emit_null(here);
                if (!ok) return;
                emit(Op::Fail, 0, 0, 0, here);
                return;
            }

            case 'J': {
                struct import_node *imp = (struct import_node *)node;
                uint32_t name_slot = UINT32_MAX;
                std::vector<uint32_t> selection_slots;
                if (imp->selections) {
                    for (const struct import_selection *sel = imp->selections;
                         sel; sel = sel->next) {
                        const char *binding =
                            sel->alias ? sel->alias : sel->name;
                        if (binding && *binding)
                            selective_bindings.insert(binding);
                    }
                    if (slot_fast_path && !slot_scopes.empty()) {
                        SlotScope &scope = slot_scopes.back();
                        for (const struct import_selection *sel =
                                 imp->selections;
                             sel; sel = sel->next) {
                            const char *binding =
                                sel->alias ? sel->alias : sel->name;
                            if (binding && scope.names.count(binding) != 0) {
                                uint32_t message = add_name(
                                    std::string("symbol repeated ") + binding);
                                emit(Op::RuntimeError, 0, 0, message, here);
                                return;
                            }
                            uint32_t slot = add_slot(binding, NULL, false);
                            if (binding && *binding) {
                                scope.names[binding] =
                                    static_cast<int>(slot);
                            }
                            scope.declared.push_back(static_cast<int>(slot));
                            selection_slots.push_back(slot);
                        }
                    }
                    uint32_t desc = add_import(imp, selection_slots);
                    emit(Op::Import, 0, static_cast<uint16_t>(desc),
                         UINT32_MAX, here);
                    return;
                }
                if (slot_fast_path && !slot_scopes.empty()) {
                    const char *binding =
                        imp->alias ? imp->alias
                                   : import_binding_name(imp->path);
                    if (binding && *binding) {
                        uint32_t slot = add_slot(binding, NULL, false);
                        slot_scopes.back().names[binding] =
                            static_cast<int>(slot);
                        slot_scopes.back().declared.push_back(
                            static_cast<int>(slot));
                        name_slot = slot;
                    }
                }
                uint32_t desc = add_import(imp);
                emit(Op::Import, 0, static_cast<uint16_t>(desc),
                     name_slot, here);
                return;
            }

            case 'F':
                compile_forin(node);
                return;

            case 'S':
                compile_switch(node);
                return;

            case '7':
                compile_match(node);
                return;

            case 'o':
                compile_on_error(node);
                return;

            case 'v': {
                struct control_stmt *stmt = (struct control_stmt *)node;
                if (stmt->value)
                    compile(stmt->value);
                else
                    emit_null(here);
                if (!ok) return;
                emit(Op::Recover, 0, 0, 0, here);
                return;
            }

            case 'p': {
                struct control_stmt *stmt = (struct control_stmt *)node;
                if (stmt->value)
                    compile(stmt->value);
                else
                    emit_null(here);
                if (!ok) return;
                emit(Op::Pass, 0, 0, 0, here);
                return;
            }

            case 'n': {
                if (!inside_function && scope_depth == 0) {
                    uint32_t message = add_name(
                        "defer is only allowed inside a function or block");
                    emit(Op::RuntimeError, 0, 0, message, here);
                    return;
                }
                uint32_t descriptor = add_defer_proto(node->l);
                if (!ok) return;
                emit(Op::DeferPush, 0, 0, descriptor, here);
                emit_null(here);
                return;
            }

            default:
                fail("unsupported expression");
                return;
        }
    }

    void compile_logical(ast *node, bool is_and) {
        SourceLoc here = loc_of(node);
        compile(node->l);
        if (!ok) return;
        emit(Op::Dup, 0, 0, 0, here);
        size_t branch =
            emit_jump(is_and ? Op::JumpIfFalse : Op::JumpIfTrue, here);
        emit(Op::Pop, 0, 0, 0, here);
        compile(node->r);
        if (!ok) return;
        emit(Op::Not, 0, 0, 0, here);
        emit(Op::Not, 0, 0, 0, here); // normalize to bool
        size_t jend = emit_jump(Op::Jump, here);
        patch(branch, proto.code.size());
        emit(Op::Pop, 0, 0, 0, here);
        uint32_t bool_index = add_bool(!is_and);
        emit(Op::LoadConst, 0, 0, bool_index, here);
        patch(jend, proto.code.size());
    }

    LabelTarget *find_label(const std::string &name) {
        for (size_t depth = stmt_lists.size(); depth > 0; --depth) {
            auto it = stmt_lists[depth - 1].labels.find(name);
            if (it != stmt_lists[depth - 1].labels.end())
                return &it->second;
        }
        return NULL;
    }

    void emit_goto_cleanup(const LabelTarget &target, SourceLoc loc) {
        if (handler_stack_depth > target.handler_stack_depth) {
            if (target.handler_body_depth > 255) {
                yyerror("error handler nesting is too deep");
                ok = false;
                return;
            }
            emit(Op::PopHandlers,
                 static_cast<uint8_t>(target.handler_body_depth),
                 static_cast<uint16_t>(target.handler_stack_depth), 0, loc);
        }
        for (size_t i = iter_stack.size();
             i > static_cast<size_t>(target.iter_depth); --i) {
            emit(Op::IterEnd, 0, static_cast<uint16_t>(iter_stack[i - 1]), 0,
                 loc);
        }
        for (size_t i = loops.size();
             i > static_cast<size_t>(target.loop_depth); --i) {
            for (int n = 0; n < loops[i - 1].stack_cleanup; ++n)
                emit(Op::Pop, 0, 0, 0, loc);
        }
        size_t call_base = static_cast<size_t>(target.call_depth) <
                                   call_cleanup.size()
                               ? static_cast<size_t>(target.call_depth)
                               : call_cleanup.size();
        for (size_t i = call_base; i < call_cleanup.size(); ++i) {
            for (int n = 0; n < call_cleanup[i]; ++n)
                emit(Op::Pop, 0, 0, 0, loc);
        }
        for (int depth = scope_depth; depth > target.scope_depth; --depth)
            emit(Op::PopScope, 0, 0, 0, loc);
    }

    void compile_statement_list(ast *node) {
        std::vector<ast *> items;
        collect_chain(node, items);
        if (items.empty()) {
            emit(Op::LoadNull, 0, 0, 0, loc_of(node));
            return;
        }

        StmtListContext ctx;
        ctx.scope_depth = scope_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        ctx.loop_depth = static_cast<int>(loops.size());
        ctx.call_depth = static_cast<int>(call_cleanup.size());
        ctx.handler_stack_depth = handler_stack_depth;
        ctx.handler_body_depth = handler_body_depth;
        std::vector<int> decl_before(items.size() + 1, 0);
        for (size_t i = 0; i < items.size(); ++i) {
            decl_before[i + 1] =
                decl_before[i] + (skein::compiler_analysis::statement_declares_local(items[i]) ? 1 : 0);
        }
        for (size_t i = 0; i < items.size(); ++i) {
            ast *item = items[i];
            if (item->nodetype != 'g')
                continue;
            struct label_node *lab = (struct label_node *)item;
            std::string name = lab->name ? lab->name : "";
            if (function_labels.count(name)) {
                yyerror("duplicate label %s", name.c_str());
                ok = false;
                return;
            }
            function_labels[name] = skein::labels::label_info(item);
            LabelTarget target;
            target.scope_depth = ctx.scope_depth;
            target.iter_depth = ctx.iter_depth;
            target.loop_depth = ctx.loop_depth;
            target.call_depth = ctx.call_depth;
            target.handler_stack_depth = ctx.handler_stack_depth;
            target.handler_body_depth = ctx.handler_body_depth;
            target.decl_index = decl_before[i];
            ctx.labels[name] = target;
        }
        stmt_lists.push_back(std::move(ctx));
        const size_t ctx_index = stmt_lists.size() - 1;

        for (size_t i = 0; i < items.size(); ++i) {
            if (!ok) {
                stmt_lists.pop_back();
                return;
            }
            ast *item = items[i];
            if (item->nodetype == 'g') {
                struct label_node *lab = (struct label_node *)item;
                std::string name = lab->name ? lab->name : "";
                stmt_lists[ctx_index].labels[name].pc = proto.code.size();
                ast *child = lab->stmt;
                if (child && (child->nodetype == 'W' ||
                              child->nodetype == 'E' ||
                              child->nodetype == 'P' ||
                              child->nodetype == 'F' ||
                              child->nodetype == 'S')) {
                    std::string saved = pending_label;
                    pending_label = name;
                    compile(child);
                    pending_label = saved;
                } else {
                    compile(child);
                }
                if (!ok) {
                    stmt_lists.pop_back();
                    return;
                }
                LabelTarget &target = stmt_lists[ctx_index].labels[name];
                for (size_t at : target.patches)
                    patch(at, target.pc);
                target.patches.clear();
            } else if (item->nodetype == NODE_GOTO) {
                std::string name = ((struct goto_node *)item)->name
                                       ? ((struct goto_node *)item)->name
                                       : "";
                LabelTarget *target = find_label(name);
                if (target) {
                    std::unordered_map<std::string, LabelTarget>::iterator
                        local = stmt_lists[ctx_index].labels.find(name);
                    if (local != stmt_lists[ctx_index].labels.end() &&
                        local->second.decl_index != decl_before[i]) {
                        yyerror("goto cannot cross a let or const declaration");
                        ok = false;
                    }
                }
                if (!target) {
                    yyerror("goto target %s not found in this block",
                            name.c_str());
                    ok = false;
                } else if (ok) {
                    used_function_labels.insert(name);
                    if (proto.is_generator) {
                        yyerror("goto is not allowed in generator functions");
                        ok = false;
                    }
                    emit_goto_cleanup(*target, loc_of(item));
                    if (!ok) {
                        stmt_lists.pop_back();
                        return;
                    }
                    if (target->pc != static_cast<size_t>(-1)) {
                        emit(Op::Jump, 0, 0,
                             static_cast<uint32_t>(target->pc),
                             loc_of(item));
                    } else {
                        target->patches.push_back(
                            emit_jump(Op::Jump, loc_of(item)));
                    }
                }
                continue;
            } else {
                compile(item);
            }
            if (!ok) {
                stmt_lists.pop_back();
                return;
            }
            if (i + 1 < items.size())
                emit(Op::Pop, 0, 0, 0, loc_of(item));
        }

        stmt_lists.pop_back();
    }

    static void collect_chain(ast *node, std::vector<ast *> &out) {
        if (!node) return;
        if (node->nodetype == 'L') {
            collect_chain(node->l, out);
            collect_chain(node->r, out);
        } else {
            out.push_back(node);
        }
    }

    void compile_while(ast *node) {
        struct flow *f = (struct flow *)node;
        SourceLoc here = loc_of(node);
        size_t start = proto.code.size();

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.handler_body_depth = handler_body_depth;
        ctx.handler_stack_depth = handler_stack_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        ctx.call_depth = static_cast<int>(call_cleanup.size());
        loops.push_back(ctx);
        size_t ctx_index = loops.size() - 1;

        size_t jf = static_cast<size_t>(-1);
        if (f->cond) {
            jf = compile_condition(f->cond, false, here);
            if (!ok) return;
        }

        compile(f->tl);
        if (!ok) return;
        emit(Op::Pop, 0, 0, 0, here);

        for (size_t p : loops[ctx_index].continue_patches)
            patch(p, start);

        emit(Op::Jump, 0, 0, static_cast<uint32_t>(start), here);

        size_t end = proto.code.size();
        for (size_t p : loops[ctx_index].break_patches)
            patch(p, end);
        if (jf != static_cast<size_t>(-1))
            patch(jf, end);
        loops.pop_back();
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    void compile_do(ast *node) {
        struct flow *f = (struct flow *)node;
        SourceLoc here = loc_of(node);
        size_t start = proto.code.size();

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.handler_body_depth = handler_body_depth;
        ctx.handler_stack_depth = handler_stack_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        ctx.call_depth = static_cast<int>(call_cleanup.size());
        loops.push_back(ctx);
        size_t ctx_index = loops.size() - 1;

        compile(f->tl);
        if (!ok) return;
        emit(Op::Pop, 0, 0, 0, here);

        size_t cond_start = proto.code.size();
        for (size_t p : loops[ctx_index].continue_patches)
            patch(p, cond_start);

        size_t jt = compile_condition(f->cond, true, here);
        if (!ok) return;
        patch(jt, start);

        size_t end = proto.code.size();
        for (size_t p : loops[ctx_index].break_patches)
            patch(p, end);
        loops.pop_back();
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    void compile_for(ast *node) {
        struct forloop *f = (struct forloop *)node;
        SourceLoc here = loc_of(node);

        emit(Op::PushScope, 0, 0, 0, here);
        ++scope_depth;
        push_slot_scope();
        if (f->init) {
            compile(f->init);
            if (!ok) return;
            emit(Op::Pop, 0, 0, 0, here);
        }

        size_t start = proto.code.size();
        size_t jf = static_cast<size_t>(-1);
        if (f->cond) {
            jf = compile_condition(f->cond, false, here);
            if (!ok) return;
        }

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.handler_body_depth = handler_body_depth;
        ctx.handler_stack_depth = handler_stack_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        ctx.call_depth = static_cast<int>(call_cleanup.size());
        loops.push_back(ctx);
        size_t ctx_index = loops.size() - 1;

        compile(f->body);
        if (!ok) return;
        emit(Op::Pop, 0, 0, 0, here);

        size_t step_start = proto.code.size();
        for (size_t p : loops[ctx_index].continue_patches)
            patch(p, step_start);

        if (f->step) {
            compile(f->step);
            if (!ok) return;
            emit(Op::Pop, 0, 0, 0, here);
        }

        emit(Op::Jump, 0, 0, static_cast<uint32_t>(start), here);

        size_t end = proto.code.size();
        for (size_t p : loops[ctx_index].break_patches)
            patch(p, end);
        loops.pop_back();
        if (jf != static_cast<size_t>(-1))
            patch(jf, end);

        pop_slot_scope(true, here);
        emit(Op::PopScope, 0, 0, 0, here);
        --scope_depth;
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    void compile_forin(ast *node) {
        struct forinloop *f = (struct forinloop *)node;
        SourceLoc here = loc_of(node);

        compile(f->iterable);
        if (!ok) return;

        emit(Op::PushScope, 0, 0, 0, here);
        ++scope_depth;
        push_slot_scope();

        uint32_t key_slot = UINT32_MAX;
        uint32_t value_slot = UINT32_MAX;
        if (slot_fast_path && !slot_scopes.empty()) {
            key_slot = add_slot(f->name, NULL, false);
            slot_scopes.back().names[f->name] =
                static_cast<int>(key_slot);
            slot_scopes.back().declared.push_back(
                static_cast<int>(key_slot));
            if (f->value_name &&
                std::strcmp(f->name, f->value_name) != 0) {
                value_slot = add_slot(f->value_name, NULL, false);
                slot_scopes.back().names[f->value_name] =
                    static_cast<int>(value_slot);
                slot_scopes.back().declared.push_back(
                    static_cast<int>(value_slot));
            }
        }

        uint32_t desc = add_iter(f->name, f->value_name, key_slot,
                                 value_slot);
        emit(Op::IterNew, 0, static_cast<uint16_t>(desc), 0, here);

        size_t next_pc = proto.code.size();
        size_t jend = emit(Op::IterNext, 0, static_cast<uint16_t>(desc),
                           0, here);

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.handler_body_depth = handler_body_depth;
        ctx.handler_stack_depth = handler_stack_depth;
        ctx.call_depth = static_cast<int>(call_cleanup.size());
        loops.push_back(ctx);
        iter_stack.push_back(static_cast<int>(desc));
        loops.back().iter_depth = static_cast<int>(iter_stack.size());
        size_t ctx_index = loops.size() - 1;

        compile(f->body);
        if (!ok) return;
        emit(Op::Pop, 0, 0, 0, here);

        emit(Op::Jump, 0, 0, static_cast<uint32_t>(next_pc), here);

        size_t end = proto.code.size();
        patch(jend, end);
        for (size_t p : loops[ctx_index].continue_patches)
            patch(p, next_pc);
        for (size_t p : loops[ctx_index].break_patches)
            patch(p, end);
        loops.pop_back();
        iter_stack.pop_back();

        emit(Op::IterEnd, 0, static_cast<uint16_t>(desc), 0, here);
        pop_slot_scope(true, here);
        emit(Op::PopScope, 0, 0, 0, here);
        --scope_depth;
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    void compile_switch(ast *node) {
        struct switch_node *sw = (struct switch_node *)node;
        SourceLoc here = loc_of(node);

        compile(sw->expr);
        if (!ok) return;
        emit(Op::SwitchCheck, 0, 0, 0, here);

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.handler_body_depth = handler_body_depth;
        ctx.handler_stack_depth = handler_stack_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        ctx.stack_cleanup = 1; // the subject stays on the operand stack
        ctx.accepts_continue = false;
        ctx.call_depth = static_cast<int>(call_cleanup.size());
        loops.push_back(ctx);
        size_t ctx_index = loops.size() - 1;

        std::vector<size_t> end_jumps;
        struct case_node *default_case = NULL;

        for (struct case_node *c = (struct case_node *)sw->cases; c;
             c = (struct case_node *)c->next) {
            if (!c->values) {
                default_case = c;
                continue;
            }
            SourceLoc case_loc = loc_of((struct ast *)c);
            if (case_values_have_condition(c->values)) {
                diag_set_location(case_loc.line, case_loc.column);
                yyerror("logical or comparison expressions are not allowed "
                        "in case values");
                ok = false;
                return;
            }

            compile(c->values);
            if (!ok) return;
            emit(Op::CaseMatch, 0, 0, 0, here);
            size_t jf = emit_jump(Op::JumpIfFalse, case_loc);

            emit(Op::PushScope, 0, 0, 0, case_loc);
            ++scope_depth;
            push_slot_scope();
            compile(c->body);
            if (!ok) return;
            pop_slot_scope(true, case_loc);
            emit(Op::PopScope, 0, 0, 0, case_loc);
            --scope_depth;
            emit(Op::Pop, 0, 0, 0, case_loc);
            end_jumps.push_back(emit_jump(Op::Jump, case_loc));
            patch(jf, proto.code.size());
        }

        if (default_case) {
            SourceLoc case_loc = loc_of((struct ast *)default_case);
            emit(Op::PushScope, 0, 0, 0, case_loc);
            ++scope_depth;
            push_slot_scope();
            compile(default_case->body);
            if (!ok) return;
            pop_slot_scope(true, case_loc);
            emit(Op::PopScope, 0, 0, 0, case_loc);
            --scope_depth;
            emit(Op::Pop, 0, 0, 0, case_loc);
        }

        size_t end_cleanup = proto.code.size();
        for (size_t patch_at : end_jumps)
            patch(patch_at, end_cleanup);
        for (size_t patch_at : loops[ctx_index].break_patches)
            patch(patch_at, end_cleanup);
        loops.pop_back();

        emit(Op::Pop, 0, 0, 0, here); // discard the switch subject
        emit(Op::LoadNull, 0, 0, 0, here);
    }

    uint32_t add_pattern(struct ast *node) {
        if (!node || !ok) {
            fail("invalid pattern");
            return UINT32_MAX;
        }
        struct pattern_node *pattern = (struct pattern_node *)node;
        skein::bc::PatternDesc desc;

        switch (pattern->nodetype) {
            case NODE_PATTERN_LITERAL: {
                desc.kind = skein::bc::PatternDesc::Kind::Literal;
                struct ast *literal = pattern->literal;
                if (literal && literal->nodetype == 'V') {
                    struct val *value = (struct val *)literal;
                    if (value->val)
                        desc.literal_value = own_value(dupval(value->val));
                } else if (literal &&
                           literal->nodetype == NODE_ENUM_LITERAL) {
                    struct enum_lit_node *enum_literal =
                        (struct enum_lit_node *)literal;
                    desc.source = enum_literal->source
                                      ? enum_literal->source
                                      : "";
                    desc.type_name = enum_literal->name
                                         ? enum_literal->name
                                         : "";
                    desc.variant = enum_literal->variant
                                       ? enum_literal->variant
                                       : "";
                }
                if (!desc.literal_value) {
                    uint32_t proto_index =
                        add_expression_proto(literal, "$pattern");
                    if (!ok)
                        return UINT32_MAX;
                    desc.literal_expr = proto.defer_protos[proto_index];
                }
                break;
            }
            case NODE_PATTERN_BINDING:
                desc.kind = skein::bc::PatternDesc::Kind::Binding;
                desc.name = pattern->name ? pattern->name : "";
                break;
            case NODE_PATTERN_TYPE:
                desc.kind = skein::bc::PatternDesc::Kind::Type;
                desc.type = pattern->type
                                ? copy_type_constraint(pattern->type)
                                : any_type_constraint();
                break;
            case NODE_PATTERN_OR:
                desc.kind = skein::bc::PatternDesc::Kind::Or;
                for (struct pattern_node *item =
                         (struct pattern_node *)pattern->items;
                     item; item = (struct pattern_node *)item->next) {
                    uint32_t child = add_pattern((struct ast *)item);
                    if (!ok)
                        return UINT32_MAX;
                    desc.items.push_back(child);
                }
                break;
            case NODE_PATTERN_RANGE:
                desc.kind = skein::bc::PatternDesc::Kind::Range;
                desc.range_start = pattern->range_start;
                desc.range_end = pattern->range_end;
                break;
            case NODE_PATTERN_REST:
                desc.kind = skein::bc::PatternDesc::Kind::Rest;
                desc.name = pattern->name ? pattern->name : "";
                break;
            case NODE_PATTERN_AS: {
                desc.kind = skein::bc::PatternDesc::Kind::As;
                desc.name = pattern->name ? pattern->name : "";
                desc.child = add_pattern(pattern->items);
                if (!ok)
                    return UINT32_MAX;
                break;
            }
            case NODE_PATTERN_NEVER:
                desc.kind = skein::bc::PatternDesc::Kind::Never;
                break;
            case NODE_PATTERN_LIST:
                desc.kind = skein::bc::PatternDesc::Kind::List;
                for (struct pattern_node *item =
                         (struct pattern_node *)pattern->items;
                     item; item = (struct pattern_node *)item->next) {
                    uint32_t child = add_pattern((struct ast *)item);
                    if (!ok)
                        return UINT32_MAX;
                    desc.items.push_back(child);
                }
                break;
            case NODE_PATTERN_TUPLE:
                desc.kind = skein::bc::PatternDesc::Kind::Tuple;
                for (struct pattern_node *item =
                         (struct pattern_node *)pattern->items;
                     item; item = (struct pattern_node *)item->next) {
                    uint32_t child = add_pattern((struct ast *)item);
                    if (!ok)
                        return UINT32_MAX;
                    desc.items.push_back(child);
                }
                break;
            case NODE_PATTERN_CONTAINER:
                desc.kind = skein::bc::PatternDesc::Kind::Container;
                desc.source = pattern->source ? pattern->source : "";
                desc.type_name = pattern->name ? pattern->name : "";
                desc.variant = pattern->variant ? pattern->variant : "";
                desc.is_dict = pattern->name == NULL &&
                               pattern->variant == NULL;
                for (struct pattern_field *field = pattern->fields; field;
                     field = field->next) {
                    skein::bc::PatternFieldDesc field_desc;
                    if (field->key) {
                        field_desc.key = field->key;
                        field_desc.pattern = add_pattern(field->pattern);
                        if (!ok)
                            return UINT32_MAX;
                    } else {
                        field_desc.rest = true;
                        field_desc.rest_name =
                            field->rest_name ? field->rest_name : "";
                    }
                    desc.fields.push_back(std::move(field_desc));
                }
                break;
            default:
                fail("unsupported pattern");
                return UINT32_MAX;
        }

        proto.patterns.push_back(std::move(desc));
        return static_cast<uint32_t>(proto.patterns.size() - 1);
    }

    void compile_match(ast *node) {
        struct match_node *m = (struct match_node *)node;
        SourceLoc here = loc_of(node);

        compile(m->expr);
        if (!ok) return;

        std::vector<uint32_t> pattern_indices;
        std::vector<std::vector<std::string>> arm_bindings;
        std::vector<skein::bc::MatchArmDesc> arm_descs;
        for (struct match_arm *arm = (struct match_arm *)m->arms; arm;
             arm = (struct match_arm *)arm->next) {
            skein::bc::MatchArmDesc desc;
            std::vector<std::string> bindings;
            if (arm->pattern) {
                skein::ast_analysis::collect_pattern_bindings(arm->pattern, bindings);
                desc.pattern = add_pattern(arm->pattern);
                if (!ok) return;
            }
            desc.has_guard = arm->guard != NULL;
            pattern_indices.push_back(desc.pattern);
            arm_bindings.push_back(std::move(bindings));
            arm_descs.push_back(desc);
        }
        uint32_t arms = static_cast<uint32_t>(proto.match_arms.size());
        proto.match_arms.push_back(std::move(arm_descs));
        emit(Op::MatchBegin, 0, 0, arms, here);

        LoopContext ctx;
        ctx.label = take_pending_label();
        ctx.scope_depth = scope_depth;
        ctx.handler_body_depth = handler_body_depth;
        ctx.handler_stack_depth = handler_stack_depth;
        ctx.iter_depth = static_cast<int>(iter_stack.size());
        ctx.stack_cleanup = 1; // the subject stays on the operand stack
        ctx.accepts_continue = false;
        ctx.accepts_break = false;
        ctx.call_depth = static_cast<int>(call_cleanup.size());
        loops.push_back(ctx);

        std::vector<size_t> end_jumps;
        size_t arm_index = 0;

        for (struct match_arm *arm = (struct match_arm *)m->arms; arm;
             arm = (struct match_arm *)arm->next, ++arm_index) {
            SourceLoc arm_loc = loc_of((struct ast *)arm);
            bool is_default = arm->pattern == NULL;

            emit(Op::PushScope, 0, 0, 0, arm_loc);
            ++scope_depth;
            push_slot_scope();

            size_t fail_jump = static_cast<size_t>(-1);
            size_t guard_jump = static_cast<size_t>(-1);

            if (!is_default) {
                attach_pattern_slots(pattern_indices[arm_index],
                                     arm_bindings[arm_index], true);
                emit(Op::Dup, 0, 0, 0, arm_loc);
                emit(Op::MatchTest, 0, 0, pattern_indices[arm_index],
                     here);
                fail_jump = emit_jump(Op::JumpIfFalse, arm_loc);
            }

            if (arm->guard) {
                guard_jump = compile_condition(arm->guard, false,
                                               arm_loc);
                if (!ok) return;
            }

            compile(arm->body);
            if (!ok) return;

            emit_clear_current_slots(arm_loc);
            emit(Op::PopScope, 0, 0, 0, arm_loc);
            --scope_depth;
            emit(Op::Swap, 0, 0, 0, arm_loc);
            emit(Op::Pop, 0, 0, 0, arm_loc);
            end_jumps.push_back(emit_jump(Op::Jump, arm_loc));

            if (is_default) {
                // A default arm always matches; later arms are unreachable.
                pop_slot_scope(false, arm_loc);
                break;
            }

            size_t fail_target = proto.code.size();
            if (fail_jump != static_cast<size_t>(-1))
                patch(fail_jump, fail_target);
            if (guard_jump != static_cast<size_t>(-1))
                patch(guard_jump, fail_target);

            // Failure path: discard the arm scope, then try the next arm.
            emit_clear_current_slots(arm_loc);
            emit(Op::PopScope, 0, 0, 0, arm_loc);
            pop_slot_scope(false, arm_loc);
        }

        uint32_t message = add_name("match: no arm matched");
        emit(Op::RuntimeError, 0, 0, message, here);

        size_t end = proto.code.size();
        for (size_t patch_at : end_jumps)
            patch(patch_at, end);
        loops.pop_back();
    }

    void compile_on_error(ast *node) {
        struct on_error_node *handler = (struct on_error_node *)node;
        SourceLoc here = loc_of(node);

        size_t fail_handler =
            emit(Op::PushFailHandler, 1, 0, 0, here); // capture errors
        ++handler_stack_depth;

        compile(handler->call);
        if (!ok) return;
        emit(Op::CheckResult, 0, 0, 0, here);
        emit(Op::PopHandler, 0, 0, 0, here);
        --handler_stack_depth;
        size_t done_jump = emit_jump(Op::Jump, here);

        size_t handler_entry = proto.code.size();
        patch(fail_handler, handler_entry);

        emit(Op::PushScope, 0, 0, 0, here);
        ++scope_depth;
        push_slot_scope();
        uint16_t error_slot = UINT16_MAX;
        if (slot_fast_path && !slot_scopes.empty() && handler->name) {
            uint32_t slot = add_slot(handler->name, NULL, true);
            slot_scopes.back().names[handler->name] =
                static_cast<int>(slot);
            slot_scopes.back().declared.push_back(
                static_cast<int>(slot));
            error_slot = static_cast<uint16_t>(slot);
        }
        uint32_t error_name =
            add_name(handler->name ? handler->name : "");
        emit(Op::BindName, 1, error_slot, error_name, here);

        /*
         * The recover handler is pushed inside the handler scope but must
         * unwind to the outer scope.  flags bit 0 tells the VM to record
         * (current scope depth - 1).
         */
        size_t recover_handler =
            emit(Op::PushRecoverHandler, 1, 0, 0, here);
        ++handler_stack_depth;
        emit(Op::EnterHandlerBody, 0, 0, 0, here);
        ++handler_body_depth;

        compile(handler->body);
        if (!ok) return;

        emit(Op::LeaveHandlerBody, 0, 0, 0, here);
        --handler_body_depth;
        emit(Op::PopHandler, 0, 0, 0, here);
        --handler_stack_depth;
        pop_slot_scope(true, here);
        emit(Op::PopScope, 0, 0, 0, here);
        --scope_depth;

        uint32_t message = add_name(
            "error handler must end with recover, pass, or fail");
        emit(Op::RuntimeError, 0, 0, message, here);

        size_t recover_entry = proto.code.size();
        patch(recover_handler, recover_entry);
        patch(done_jump, recover_entry);
    }

    std::string take_pending_label() {
        std::string label = pending_label;
        pending_label.clear();
        return label;
    }

    void compile_break_continue(bool is_break, const char *label,
                                SourceLoc loc) {
        LoopContext *target = NULL;
        if (label && *label) {
            for (size_t i = loops.size(); i > 0; --i) {
                if (loops[i - 1].label != label)
                    continue;
                if (is_break && !loops[i - 1].accepts_break)
                    continue;
                if (!is_break && !loops[i - 1].accepts_continue)
                    continue;
                target = &loops[i - 1];
                break;
            }
        } else if (is_break) {
            for (size_t i = loops.size(); i > 0; --i) {
                if (loops[i - 1].accepts_break) {
                    target = &loops[i - 1];
                    break;
                }
            }
        } else {
            for (size_t i = loops.size(); i > 0; --i) {
                if (loops[i - 1].accepts_continue) {
                    target = &loops[i - 1];
                    break;
                }
            }
        }

        if (forbid_control_exit && !target) {
            uint32_t message = add_name(
                "defer expression cannot return, break, or continue");
            emit(Op::RuntimeError, 0, 0, message, loc);
            return;
        }

        if (!target) {
            if (label && *label) {
                diag_set_location(loc.line, loc.column);
                bool exists =
                    function_labels.find(label) != function_labels.end();
                if (exists)
                    used_function_labels.insert(label);
                std::string message = skein::labels::target_error(
                    is_break, label, exists);
                yyerror("%s", message.c_str());
                ok = false;
                return;
            }
            uint32_t index = add_label(label);
            emit(is_break ? Op::ControlBreak : Op::ControlContinue, 0, 0,
                 index, loc);
            return;
        }

        size_t target_index =
            static_cast<size_t>(target - loops.data());
        if (label && *label)
            used_function_labels.insert(label);

        if (handler_stack_depth > target->handler_stack_depth) {
            if (target->handler_body_depth > 255) {
                yyerror("error handler nesting is too deep");
                return;
            }
            emit(Op::PopHandlers,
                 static_cast<uint8_t>(target->handler_body_depth),
                 static_cast<uint16_t>(target->handler_stack_depth), 0,
                 loc);
        }

        /*
         * A break/continue may leave lexical blocks inside the loop body.
         * Pop those scopes before jumping so defers run and symbols do not
         * leak past their block.
         *
         * If the jump leaves one or more for-in loops, release their
         * iterator state as well.  The target loop's own iterator (if any)
         * is not included; its normal exit path emits the matching IterEnd.
         */
        for (size_t i = iter_stack.size();
             i > static_cast<size_t>(target->iter_depth); --i) {
            emit(Op::IterEnd, 0,
                 static_cast<uint16_t>(iter_stack[i - 1]), 0, loc);
        }

        /*
         * A switch keeps its subject value on the operand stack.  When a
         * break/continue jumps past a switch (for example to an outer loop),
         * discard that subject first.
         */
        for (size_t i = loops.size(); i > target_index + 1; --i) {
            for (int n = 0; n < loops[i - 1].stack_cleanup; ++n)
                emit(Op::Pop, 0, 0, 0, loc);
        }

        size_t call_base = static_cast<size_t>(target->call_depth) <
                                   call_cleanup.size()
                               ? static_cast<size_t>(target->call_depth)
                               : call_cleanup.size();
        for (size_t i = call_base; i < call_cleanup.size(); ++i) {
            for (int n = 0; n < call_cleanup[i]; ++n)
                emit(Op::Pop, 0, 0, 0, loc);
        }

        for (int depth = scope_depth; depth > target->scope_depth; --depth)
            emit(Op::PopScope, 0, 0, 0, loc);

        size_t patch_at = emit_jump(Op::Jump, loc);
        if (is_break)
            target->break_patches.push_back(patch_at);
        else
            target->continue_patches.push_back(patch_at);
    }

    void compile_list(ast *node, bool frozen) {
        struct list_node *item = (struct list_node *)node;
        std::vector<ast *> values;
        for (; item; item = (struct list_node *)item->next) {
            if (!item->value) continue;
            if (item->value->nodetype == 'x') {
                fail("spread is not allowed in a list literal");
                return;
            }
            values.push_back(item->value);
        }
        for (ast *value : values) {
            compile(value);
            if (!ok) return;
        }
        emit(Op::NewList, frozen ? 1 : 0,
             static_cast<uint16_t>(values.size()),
             static_cast<uint32_t>(values.size()), loc_of(node));
    }

    void compile_dict(ast *node) {
        struct dict_node *entry = (struct dict_node *)node;
        std::vector<ast *> keys;
        std::vector<ast *> values;
        for (; entry; entry = (struct dict_node *)entry->next) {
            if (!entry->key || !entry->value) continue;
            keys.push_back(entry->key);
            values.push_back(entry->value);
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            compile(keys[i]);
            compile(values[i]);
            if (!ok) return;
        }
        emit(Op::NewDict, 0, static_cast<uint16_t>(keys.size()),
             static_cast<uint32_t>(keys.size()), loc_of(node));
    }

    void compile_call(ast *node, bool tail = false) {
        struct ufncall *call = (struct ufncall *)node;
        /* `return launch f()` still needs the Return after the launch Call. */
        bool return_value = tail;
        if (call->launch)
            tail = false;
        std::vector<ast *> args;
        collect_chain(call->args, args);
        call_cleanup.push_back(0);

        bool has_spread = false;
        for (ast *arg : args) {
            if (arg && arg->nodetype == 'x') {
                has_spread = true;
                break;
            }
        }

        /* `obj.method(args)` avoids materializing a bound method value.
         * Spread, launch and tail calls keep the generic path. */
        const char *method_name = nullptr;
        bool member_call = false;
        if (!tail && !call->launch && !has_spread && call->callee &&
            call->callee->nodetype == 'j' && args.size() <= 0xFFFF) {
            method_name = literal_string(call->callee->r);
            if (method_name) {
                ast *base = call->callee->l;
                const bool unresolved_base =
                    base && base->nodetype == 'N' &&
                    !name_is_resolved(((struct symref *)base)->name);
                member_call = !unresolved_base;
            }
        }
        if (member_call) {
            compile(call->callee->l);
            if (!ok) return;
            ++call_cleanup.back();
            for (ast *arg : args) {
                compile(arg);
                if (!ok) return;
                ++call_cleanup.back();
            }
            const uint32_t method_index = add_name(method_name);
            emit(Op::CallMethod, 0,
                 static_cast<uint16_t>(args.size()), method_index,
                 loc_of(node));
            call_cleanup.pop_back();
            return;
        }

        compile(call->callee);
        if (!ok) return;
        ++call_cleanup.back();

        if (!has_spread) {
            for (ast *arg : args) {
                compile(arg);
                if (!ok) return;
                ++call_cleanup.back();
            }
            if (!ok) return;
            emit(tail ? Op::TailCall : Op::Call,
                 (call->launch ? 1 : 0) | (call->awaited ? 2 : 0),
                 static_cast<uint16_t>(args.size()),
                 static_cast<uint32_t>(args.size()), loc_of(node));
            call_cleanup.pop_back();
            if (!tail && return_value)
                emit(Op::Return, 0, 0, 0, loc_of(node));
            return;
        }

        std::vector<bool> spread;
        spread.reserve(args.size());
        for (ast *arg : args) {
            if (arg && arg->nodetype == 'x') {
                compile(((struct spread_node *)arg)->value);
                spread.push_back(true);
            } else {
                compile(arg);
                spread.push_back(false);
            }
            if (!ok) return;
            ++call_cleanup.back();
        }
        uint32_t descriptor = add_call_desc(std::move(spread));
        emit(Op::CallSpread,
             (call->launch ? 1 : 0) | (call->awaited ? 2 : 0),
             0, descriptor, loc_of(node));
        call_cleanup.pop_back();
        if (tail || (!tail && return_value))
            emit(Op::Return, 0, 0, 0, loc_of(node));
    }
};

} // namespace

ProtoPtr skein_compile_statement(struct ast *statement,
                                 const std::string &package,
                                 std::string &error) {
    Compiler compiler(package);
    compiler.proto.name = "<top>";
    compiler.slot_fast_path = skein::compiler_analysis::can_use_slots(statement);
    compiler.compile(statement);
    if (!compiler.ok) {
        error = compiler.error.empty() ? "cannot compile statement"
                                       : compiler.error;
        return nullptr;
    }
    if (!finalize_proto(compiler.proto, error))
        return nullptr;
    return std::make_shared<const Proto>(std::move(compiler.proto));
}

ProtoPtr skein_compile_default_expression(
    struct function_code *code, struct ast *expression, int index,
    const std::string &package, std::string &error) {
    if (!code || !expression) {
        error = "invalid default expression";
        return nullptr;
    }

    Compiler compiler(package);
    compiler.proto.name =
        (code->name.empty() ? std::string("<anonymous>") : code->name) +
        "$default" + std::to_string(index);
    for (struct symlist *param = code->sl; param; param = param->next) {
        compiler.param_names.push_back(
            param->sym && param->sym->name ? param->sym->name : "");
    }
    compiler.param_fast_path =
        !skein::compiler_analysis::has_local_declarations(expression);
    compiler.proto.param_names = compiler.param_names;
    compiler.compile(expression);
    if (!compiler.ok) {
        error = compiler.error.empty() ? "cannot compile default expression"
                                       : compiler.error;
        return nullptr;
    }
    if (!finalize_proto(compiler.proto, error))
        return nullptr;
    return std::make_shared<const Proto>(std::move(compiler.proto));
}

static bool check_proto_default_references(
    const skein::bc::Proto &proto,
    const std::vector<std::string> &forbidden,
    std::unordered_set<const struct function_code *> &visited,
    std::string &error) {
    for (const skein::bc::Instr &ins : proto.code) {
        const skein::bc::Op op = static_cast<skein::bc::Op>(ins.op);
        if (op != skein::bc::Op::LoadName &&
            op != skein::bc::Op::LoadGlobal)
            continue;
        if (ins.b >= proto.consts.size())
            continue;
        const skein::bc::Constant &constant = proto.consts[ins.b];
        if (constant.kind != skein::bc::ConstantKind::Name)
            continue;
        for (const std::string &name : forbidden) {
            if (constant.name == name) {
                error = "default value cannot reference " + name;
                return false;
            }
        }
    }

    for (const skein::bc::Constant &constant : proto.consts) {
        if (constant.kind != skein::bc::ConstantKind::Function ||
            !constant.function)
            continue;
        const struct function_code *code = constant.function.get();
        if (!visited.insert(code).second)
            continue;
        std::string compile_error;
        skein::bc::ProtoPtr nested = skein_compile_function_body(
            const_cast<struct function_code *>(code), code->package,
            compile_error);
        if (!nested)
            continue;
        if (!check_proto_default_references(*nested, forbidden, visited,
                                            error))
            return false;
    }
    return true;
}

bool skein_check_default_references(
    struct function_code *code, struct ast *expression,
    const std::vector<std::string> &forbidden,
    const std::string &package, std::string &error) {
    if (!code || !expression || forbidden.empty())
        return true;

    Compiler compiler(package);
    compiler.proto.name =
        (code->name.empty() ? std::string("<anonymous>") : code->name) +
        "$defaultcheck";
    for (struct symlist *param = code->sl; param; param = param->next) {
        compiler.param_names.push_back(
            param->sym && param->sym->name ? param->sym->name : "");
    }
    compiler.param_fast_path =
        !skein::compiler_analysis::has_local_declarations(expression);
    compiler.proto.param_names = compiler.param_names;
    compiler.inside_function = true;
    compiler.slot_fast_path = skein::compiler_analysis::can_use_slots(expression);
    if (compiler.slot_fast_path)
        compiler.slot_scopes.emplace_back();
    compiler.compile(expression);
    if (!compiler.ok)
        return true;

    std::unordered_set<const struct function_code *> visited;
    return check_proto_default_references(compiler.proto, forbidden, visited,
                                          error);
}

std::shared_ptr<const compiled_pattern_set>
skein_compile_parameter_patterns(struct function_code *code,
                                 const std::string &package,
                                 std::string &error) {
    if (!code) {
        error = "invalid parameter pattern owner";
        return nullptr;
    }

    Compiler compiler(package);
    compiler.inside_function = true;
    compiler.proto.name =
        (code->name.empty() ? std::string("<anonymous>") : code->name) +
        "$patterns";

    std::shared_ptr<compiled_pattern_set> compiled =
        std::make_shared<compiled_pattern_set>();
    for (struct symlist *param = code->sl; param; param = param->next) {
        struct ast *pattern = symlist_pattern_ast(param);
        if (!pattern) {
            compiled->roots.push_back(UINT32_MAX);
            continue;
        }
        uint32_t index = compiler.add_pattern(pattern);
        if (!compiler.ok || index == UINT32_MAX) {
            error = compiler.error.empty() ? "cannot compile parameter pattern"
                                           : compiler.error;
            return nullptr;
        }
        compiled->roots.push_back(index);
    }

    if (!finalize_proto(compiler.proto, error))
        return nullptr;
    compiled->proto =
        std::make_shared<const Proto>(std::move(compiler.proto));
    return compiled;
}

namespace {

/*
 * Compile a function body into a VM proto.
 *
 * When `bind_param_patterns` is set, destructuring parameters are compiled
 * into the proto and their bindings get slots that the VM fills at frame
 * entry.  That keeps the call in the VM fast path instead of falling back to
 * the AST call sequence.  Returns NULL with `pattern_abort` set when the
 * parameter patterns cannot be represented, so the caller can retry without
 * them.
 */
ProtoPtr compile_function_body_impl(struct function_code *code,
                                    const std::string &package,
                                    bool bind_param_patterns,
                                    bool &pattern_abort,
                                    std::string &error) {
    struct ast *body = function_body_ast(code);
    if (!code || !body) {
        error = "function has no body";
        return nullptr;
    }

    Compiler compiler(package);
    compiler.inside_function = true;
    compiler.proto.name = code->name;
    for (struct symlist *param = code->sl; param; param = param->next) {
        compiler.param_names.push_back(
            param->sym && param->sym->name ? param->sym->name : "");
    }
    compiler.upvalue_names = code->upvalues;
    compiler.proto.upvalues = code->upvalues;
    skein::compiler_analysis::collect_local_declared_names(body,
                                            compiler.declared_names);
    compiler.param_fast_path = true;
    compiler.slot_fast_path = skein::compiler_analysis::can_use_slots(body);
    if (compiler.slot_fast_path)
        compiler.slot_scopes.emplace_back();
    compiler.proto.params_direct = compiler.slot_fast_path;
    for (const std::string &name : compiler.param_names) {
        if (compiler.declared_names.count(name) != 0) {
            compiler.proto.params_direct = false;
            break;
        }
    }
    compiler.proto.nparams = static_cast<uint32_t>(code->nparams);
    compiler.proto.required_params =
        static_cast<uint32_t>(code->required_params);
    compiler.proto.variadic = code->variadic;
    compiler.proto.is_generator = code->is_generator;
    compiler.proto.is_method = code->is_method;
    compiler.proto.def_line = body->line;
    compiler.proto.def_column = body->column;
    compiler.proto.param_names = compiler.param_names;

    if (bind_param_patterns) {
        bool any_pattern = false;
        for (struct symlist *param = code->sl; param;
             param = param->next) {
            if (param->has_pattern) {
                any_pattern = true;
                break;
            }
        }
        if (any_pattern && !compiler.slot_fast_path) {
            pattern_abort = true;
            return nullptr;
        }
        if (any_pattern) {
            std::vector<uint32_t> roots((size_t)code->nparams, UINT32_MAX);
            int index = 0;
            for (struct symlist *param = code->sl; param;
                 param = param->next, ++index) {
                if (!param->has_pattern)
                    continue;
                struct ast *pattern = symlist_pattern_ast(param);
                if (!pattern) {
                    /* The AST matcher already released this pattern. */
                    pattern_abort = true;
                    return nullptr;
                }
                uint32_t root = compiler.add_pattern(pattern);
                if (!compiler.ok || root == UINT32_MAX) {
                    pattern_abort = true;
                    return nullptr;
                }
                std::vector<std::string> bindings;
                skein::ast_analysis::collect_pattern_bindings(pattern, bindings);
                compiler.attach_pattern_slots(root, bindings, false);
                if ((size_t)index < roots.size())
                    roots[(size_t)index] = root;
            }
            compiler.proto.param_patterns = std::move(roots);
            compiler.proto.param_patterns_bound = true;
        }
    }

    compiler.compile(body);
    if (!compiler.ok) {
        error = compiler.error.empty() ? "cannot compile function"
                                       : compiler.error;
        return nullptr;
    }
    for (const auto &entry : compiler.function_labels) {
        if (compiler.used_function_labels.count(entry.first) != 0)
            continue;
        std::string message = "label " + entry.first + " is never used";
        diag_report_warning(entry.second.line, entry.second.column,
                            "warning", message.c_str());
    }
    if (!finalize_proto(compiler.proto, error))
        return nullptr;
    return std::make_shared<const Proto>(std::move(compiler.proto));
}

} // namespace

ProtoPtr skein_compile_function_body(struct function_code *code,
                                     const std::string &package,
                                     std::string &error) {
    bool pattern_abort = false;
    std::string pattern_error;
    ProtoPtr proto = compile_function_body_impl(
        code, package, true, pattern_abort, pattern_error);
    if (proto)
        return proto;
    if (!pattern_abort) {
        error = pattern_error;
        return nullptr;
    }
    return compile_function_body_impl(code, package, false, pattern_abort,
                                      error);
}
