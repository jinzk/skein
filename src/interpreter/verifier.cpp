#include "verifier.h"

#include <climits>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skein {
namespace bc {
namespace {

bool fail(std::string &error, const std::string &message) {
    error = "bytecode verify: " + message;
    return false;
}

std::string at_pc(const char *what, size_t pc) {
    return std::string(what) + " at pc " + std::to_string(pc);
}

bool check_jump(const Proto &proto, uint32_t target, size_t pc,
                std::string &error) {
    if (target > proto.code.size())
        return fail(error, at_pc("jump target out of range", pc));
    return true;
}

bool check_slot(const Proto &proto, uint32_t index, size_t pc,
                std::string &error) {
    if (index >= proto.slots.size())
        return fail(error, at_pc("slot index out of range", pc));
    return true;
}

bool check_param(const Proto &proto, uint32_t index, size_t pc,
                 std::string &error) {
    if (index >= proto.param_names.size())
        return fail(error, at_pc("parameter index out of range", pc));
    return true;
}

bool check_decl(const Proto &proto, uint32_t index, size_t pc,
                std::string &error) {
    if (index >= proto.decls.size())
        return fail(error, at_pc("declaration index out of range", pc));
    return true;
}

bool check_iter(const Proto &proto, uint32_t index, size_t pc,
                std::string &error) {
    if (index >= proto.iters.size())
        return fail(error, at_pc("iterator index out of range", pc));
    return true;
}

bool check_pattern(const Proto &proto, uint32_t index, size_t pc,
                   std::string &error) {
    if (index >= proto.patterns.size())
        return fail(error, at_pc("pattern index out of range", pc));
    return true;
}

bool check_constant(const Proto &proto, uint32_t index, size_t pc,
                    std::string &error, bool require_kind = false,
                    ConstantKind kind = ConstantKind::Value) {
    if (index >= proto.consts.size())
        return fail(error, at_pc("constant index out of range", pc));
    if (require_kind && proto.consts[index].kind != kind)
        return fail(error, at_pc("constant kind mismatch", pc));
    return true;
}

bool check_count(uint64_t count, size_t pc, const char *what,
                 std::string &error) {
    if (count > static_cast<uint64_t>(INT_MAX))
        return fail(error, at_pc(what, pc));
    return true;
}

struct Effect {
    int need = 0;
    int pop = 0;
    int push = 0;
    bool terminal = false;
    bool conditional = false;
    uint32_t target = UINT32_MAX;
};

bool effect_for(const Proto &proto, const Instr &ins, size_t pc,
                Effect &effect, std::string &error) {
    const Op op = static_cast<Op>(ins.op);
    if (ins.op >= static_cast<uint8_t>(Op::Count))
        return fail(error, at_pc("invalid opcode", pc));

    auto need = [&](int value) { effect.need = value; };
    auto stack = [&](int pop, int push) {
        effect.pop = pop;
        effect.push = push;
    };
    auto terminal = [&]() { effect.terminal = true; };
    auto jump = [&](uint32_t target, bool conditional) {
        if (!check_jump(proto, target, pc, error))
            return false;
        effect.target = target;
        effect.conditional = conditional;
        return true;
    };

    switch (op) {
        case Op::Nop:
            break;
        case Op::LoadNull:
            stack(0, 1);
            break;
        case Op::LoadConst:
            if (!check_constant(proto, ins.b, pc, error))
                return false;
            stack(0, 1);
            break;
        case Op::LoadName:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            stack(0, 1);
            break;
        case Op::StoreName:
        case Op::CompoundName:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            need(1);
            stack(1, 1);
            break;
        case Op::IncDecName:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            stack(0, 1);
            break;
        case Op::LoadGlobal:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            stack(0, 1);
            break;
        case Op::StoreGlobal:
        case Op::CompoundGlobal:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            need(1);
            stack(1, 1);
            break;
        case Op::IncDecGlobal:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            stack(0, 1);
            break;
        case Op::LoadParam:
            if (!check_param(proto, ins.b, pc, error))
                return false;
            stack(0, 1);
            break;
        case Op::StoreParam:
        case Op::CompoundParam:
            if (!check_param(proto, ins.b, pc, error))
                return false;
            need(1);
            stack(1, 1);
            break;
        case Op::IncDecParam:
            if (!check_param(proto, ins.b, pc, error))
                return false;
            stack(0, 1);
            break;
        case Op::LoadSlot:
            if (!check_slot(proto, ins.b, pc, error))
                return false;
            stack(0, 1);
            break;
        case Op::StoreSlot:
        case Op::CompoundSlot:
            if (!check_slot(proto, ins.b, pc, error))
                return false;
            need(1);
            stack(1, 1);
            break;
        case Op::IncDecSlot:
            if (!check_slot(proto, ins.b, pc, error))
                return false;
            stack(0, 1);
            break;
        case Op::LoadUpvalue:
            if (ins.b >= proto.upvalues.size())
                return fail(error, at_pc("upvalue index out of range", pc));
            stack(0, 1);
            break;
        case Op::StoreUpvalue:
        case Op::CompoundUpvalue:
            if (ins.b >= proto.upvalues.size())
                return fail(error, at_pc("upvalue index out of range", pc));
            need(1);
            stack(1, 1);
            break;
        case Op::IncDecUpvalue:
            if (ins.b >= proto.upvalues.size())
                return fail(error, at_pc("upvalue index out of range", pc));
            stack(0, 1);
            break;
        case Op::Closure:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Function))
                return false;
            stack(0, 1);
            break;
        case Op::BindFunc:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            need(1);
            stack(1, 1);
            break;
        case Op::DefineSlot:
            if (!check_slot(proto, ins.b, pc, error))
                return false;
            if (ins.flags & 1) {
                need(1);
                stack(1, 1);
            } else {
                stack(0, 1);
            }
            break;
        case Op::ClearSlot:
            if (!check_slot(proto, ins.b, pc, error))
                return false;
            break;
        case Op::CheckRedefine:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            break;
        case Op::DefineName:
            if (!check_decl(proto, ins.b, pc, error))
                return false;
            if (ins.flags & 1) {
                need(1);
                stack(1, 1);
            } else {
                stack(0, 1);
            }
            break;
        case Op::Pop:
            need(1);
            stack(1, 0);
            break;
        case Op::Dup:
            need(1);
            stack(0, 1);
            break;
        case Op::Swap:
            need(2);
            break;
        case Op::Neg:
        case Op::Not:
        case Op::BitNot:
            need(1);
            stack(1, 1);
            break;
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
        case Op::CmpEq:
        case Op::CmpNe:
        case Op::CmpLt:
        case Op::CmpLe:
        case Op::CmpGt:
        case Op::CmpGe:
            need(2);
            stack(2, 1);
            break;
        case Op::CmpJump:
            if (!jump(ins.b, true))
                return false;
            need(2);
            stack(2, 0);
            break;
        case Op::IsType:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Type))
                return false;
            need(1);
            stack(1, 1);
            break;
        case Op::Jump:
            if (!jump(ins.b, false))
                return false;
            break;
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
            if (!jump(ins.b, true))
                return false;
            need(1);
            stack(1, 0);
            break;
        case Op::NewList: {
            if (!check_count(ins.b, pc, "list size too large", error))
                return false;
            need(static_cast<int>(ins.b));
            stack(static_cast<int>(ins.b), 1);
            break;
        }
        case Op::NewDict: {
            uint64_t count = static_cast<uint64_t>(ins.b) * 2;
            if (!check_count(count, pc, "dict size too large", error))
                return false;
            need(static_cast<int>(count));
            stack(static_cast<int>(count), 1);
            break;
        }
        case Op::GetIndex:
        case Op::GetField:
            need(2);
            stack(2, 1);
            break;
        case Op::GetIndexCached:
            if (ins.b >= proto.index_read_ic.size())
                return fail(error,
                            at_pc("index cache descriptor out of range", pc));
            need(2);
            stack(2, 1);
            break;
        case Op::GetFieldName:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            need(1);
            stack(1, 1);
            break;
        case Op::IterNew:
            if (!check_iter(proto, ins.a, pc, error))
                return false;
            need(1);
            stack(1, 0);
            break;
        case Op::IterNext:
            if (!check_iter(proto, ins.a, pc, error))
                return false;
            if (!jump(ins.b, true))
                return false;
            break;
        case Op::IterEnd:
            if (!check_iter(proto, ins.a, pc, error))
                return false;
            break;
        case Op::SwitchCheck:
            need(1);
            break;
        case Op::CaseMatch:
            need(2);
            stack(1, 1);
            break;
        case Op::MatchBegin:
            if (ins.b >= proto.match_arms.size())
                return fail(error, at_pc("match arm index out of range", pc));
            need(1);
            break;
        case Op::MatchTest:
        case Op::MatchBind:
            if (!check_pattern(proto, ins.b, pc, error))
                return false;
            need(1);
            stack(1, 1);
            break;
        case Op::AssignIndex:
        case Op::CompoundIndex:
        case Op::IncDecIndex: {
            if (ins.b >= proto.index_ops.size())
                return fail(error, at_pc("index descriptor out of range", pc));
            const IndexDesc &desc = proto.index_ops[ins.b];
            if (desc.fields.empty())
                return fail(error, at_pc("empty index descriptor", pc));
            int count = static_cast<int>(desc.fields.size());
            need(count + (op == Op::IncDecIndex ? 0 : 1));
            stack(count + (op == Op::IncDecIndex ? 0 : 1), 1);
            break;
        }
        case Op::Await:
        case Op::Yield:
            need(1);
            stack(1, 1);
            break;
        case Op::Fail:
        case Op::Pass:
        case Op::Recover:
            need(1);
            terminal();
            break;
        case Op::PushFailHandler:
        case Op::PushRecoverHandler:
            if (!check_jump(proto, ins.b, pc, error))
                return false;
            break;
        case Op::PopHandler:
        case Op::EnterHandlerBody:
        case Op::LeaveHandlerBody:
            break;
        case Op::CheckResult:
            need(1);
            stack(1, 1);
            break;
        case Op::BindName:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            if (ins.a != UINT16_MAX)
                if (!check_slot(proto, ins.a, pc, error))
                    return false;
            need(1);
            stack(1, 0);
            break;
        case Op::DeferPush:
            if (ins.b >= proto.defer_protos.size())
                return fail(error, at_pc("defer descriptor out of range", pc));
            if (!proto.uses_defers)
                return fail(error, at_pc("defer flag missing", pc));
            break;
        case Op::NewStruct:
        case Op::NewEnum: {
            if (ins.b >= proto.constructs.size())
                return fail(error, at_pc("constructor out of range", pc));
            int count =
                static_cast<int>(proto.constructs[ins.b].fields.size());
            need(count);
            stack(count, 1);
            break;
        }
        case Op::DeclareStruct:
            if (ins.b >= proto.struct_decls.size())
                return fail(error, at_pc("struct declaration out of range", pc));
            stack(0, 1);
            break;
        case Op::DeclareEnum:
            if (ins.b >= proto.enum_decls.size())
                return fail(error, at_pc("enum declaration out of range", pc));
            stack(0, 1);
            break;
        case Op::DeclareTrait:
            if (ins.b >= proto.trait_decls.size())
                return fail(error, at_pc("trait declaration out of range", pc));
            stack(0, 1);
            break;
        case Op::DeclareImpl:
            if (ins.b >= proto.impl_decls.size())
                return fail(error, at_pc("impl declaration out of range", pc));
            stack(0, 1);
            break;
        case Op::DeclarePackage:
            if (ins.b >= proto.packages.size())
                return fail(error, at_pc("package descriptor out of range", pc));
            if (!proto.packages[ins.b].body)
                return fail(error, at_pc("package body missing", pc));
            stack(0, 1);
            break;
        case Op::Import:
            if (ins.a >= proto.imports.size())
                return fail(error, at_pc("import descriptor out of range", pc));
            if (ins.b != UINT32_MAX)
                if (!check_slot(proto, ins.b, pc, error))
                    return false;
            stack(0, 1);
            break;
        case Op::Call:
            if (!check_count(static_cast<uint64_t>(ins.b) + 1, pc,
                             "call argument count too large", error))
                return false;
            need(static_cast<int>(ins.b) + 1);
            stack(static_cast<int>(ins.b) + 1, 1);
            break;
        case Op::TailCall:
            if (!check_count(static_cast<uint64_t>(ins.b) + 1, pc,
                             "call argument count too large", error))
                return false;
            need(static_cast<int>(ins.b) + 1);
            stack(static_cast<int>(ins.b) + 1, 0);
            terminal();
            break;
        case Op::CallSpread: {
            if (ins.b >= proto.call_descs.size())
                return fail(error, at_pc("call descriptor out of range", pc));
            uint64_t count = proto.call_descs[ins.b].spread.size();
            if (!check_count(count + 1, pc, "call argument count too large",
                             error))
                return false;
            need(static_cast<int>(count) + 1);
            stack(static_cast<int>(count) + 1, 1);
            break;
        }
        case Op::Return:
            need(1);
            terminal();
            break;
        case Op::PushScope:
        case Op::PopScope:
            break;
        case Op::ControlBreak:
        case Op::ControlContinue:
            if (ins.b != UINT32_MAX &&
                !check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            terminal();
            break;
        case Op::RuntimeError:
            if (!check_constant(proto, ins.b, pc, error, true,
                                ConstantKind::Name))
                return false;
            terminal();
            break;
        case Op::Count:
            return fail(error, at_pc("invalid opcode", pc));
    }
    return true;
}

bool verify_single(const Proto &proto, std::string &error,
                   uint32_t *max_stack) {
    if (error.empty())
        error.clear();

    int handler_depth = 0;
    int handler_body_depth = 0;
    for (size_t pc = 0; pc < proto.code.size(); ++pc) {
        const Instr &ins = proto.code[pc];
        Effect effect;
        if (!effect_for(proto, ins, pc, effect, error))
            return false;

        const Op op = static_cast<Op>(ins.op);
        if (op == Op::PushFailHandler || op == Op::PushRecoverHandler) {
            ++handler_depth;
        } else if (op == Op::PopHandler) {
            if (handler_depth <= 0)
                return fail(error, at_pc("handler underflow", pc));
            --handler_depth;
        } else if (op == Op::EnterHandlerBody) {
            if (handler_depth <= 0)
                return fail(error, at_pc("handler body outside handler", pc));
            ++handler_body_depth;
        } else if (op == Op::LeaveHandlerBody) {
            if (handler_body_depth <= 0)
                return fail(error, at_pc("handler body underflow", pc));
            --handler_body_depth;
        }
    }
    if (handler_depth != 0)
        return fail(error, "unclosed handler");
    if (handler_body_depth != 0)
        return fail(error, "unclosed handler body");

    std::vector<int> depth(proto.code.size() + 1, -1);
    std::vector<size_t> pending;
    depth[0] = 0;
    pending.push_back(0);
    uint32_t maximum = 0;

    auto reach = [&](size_t target, int value) -> bool {
        if (target > proto.code.size())
            return fail(error, "successor out of range");
        if (depth[target] < 0) {
            depth[target] = value;
            pending.push_back(target);
        } else if (depth[target] != value) {
            return fail(error, at_pc("inconsistent stack depth", target));
        }
        return true;
    };

    while (!pending.empty()) {
        size_t pc = pending.back();
        pending.pop_back();
        if (pc == proto.code.size())
            continue;

        const Instr &ins = proto.code[pc];
        Effect effect;
        if (!effect_for(proto, ins, pc, effect, error))
            return false;

        const int current = depth[pc];
        if (current < effect.need)
            return fail(error, at_pc("stack underflow", pc));
        const int next = current - effect.pop + effect.push;
        if (next < 0)
            return fail(error, at_pc("negative stack depth", pc));
        if (next > static_cast<int>(maximum))
            maximum = static_cast<uint32_t>(next);

        if (effect.terminal)
            continue;
        if (static_cast<Op>(ins.op) == Op::Jump) {
            if (!reach(effect.target, next))
                return false;
            continue;
        }
        if (effect.conditional) {
            if (!reach(effect.target, next))
                return false;
        }
        if (pc + 1 <= proto.code.size()) {
            if (!reach(pc + 1, next))
                return false;
        }
    }

    if (max_stack)
        *max_stack = maximum;
    return true;
}

bool verify_tree_impl(const Proto &proto, std::string &error,
                      std::unordered_set<const Proto *> &visiting,
                      std::unordered_set<const Proto *> &done) {
    if (done.count(&proto))
        return true;
    if (visiting.count(&proto))
        return fail(error, "proto graph cycle");
    visiting.insert(&proto);

    if (!verify_single(proto, error, nullptr))
        return false;

    for (const ProtoPtr &child : proto.defer_protos) {
        if (child && !verify_tree_impl(*child, error, visiting, done))
            return false;
    }
    for (const PackageDesc &package : proto.packages) {
        if (package.body &&
            !verify_tree_impl(*package.body, error, visiting, done))
            return false;
    }
    for (const PatternDesc &pattern : proto.patterns) {
        if (pattern.literal_expr &&
            !verify_tree_impl(*pattern.literal_expr, error, visiting, done))
            return false;
    }

    visiting.erase(&proto);
    done.insert(&proto);
    return true;
}

} // namespace

bool verify(const Proto &proto, std::string &error, uint32_t *max_stack) {
    return verify_single(proto, error, max_stack);
}

bool verify_tree(const Proto &proto, std::string &error) {
    std::unordered_set<const Proto *> visiting;
    std::unordered_set<const Proto *> done;
    return verify_tree_impl(proto, error, visiting, done);
}

} // namespace bc
} // namespace skein
