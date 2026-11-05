#include "bytecode.h"

#include <iomanip>
#include <sstream>

namespace skein {
namespace bc {

const char *op_name(Op op) {
    switch (op) {
        case Op::Nop: return "NOP";
        case Op::LoadNull: return "LOAD_NULL";
        case Op::LoadConst: return "LOAD_CONST";
        case Op::LoadName: return "LOAD_NAME";
        case Op::StoreName: return "STORE_NAME";
        case Op::CompoundName: return "COMPOUND_NAME";
        case Op::IncDecName: return "INCDEC_NAME";
        case Op::DefineName: return "DEFINE_NAME";
        case Op::Pop: return "POP";
        case Op::Dup: return "DUP";
        case Op::Swap: return "SWAP";
        case Op::Add: return "ADD";
        case Op::Sub: return "SUB";
        case Op::Mul: return "MUL";
        case Op::Div: return "DIV";
        case Op::Mod: return "MOD";
        case Op::Neg: return "NEG";
        case Op::BitAnd: return "BIT_AND";
        case Op::BitOr: return "BIT_OR";
        case Op::BitXor: return "BIT_XOR";
        case Op::Shl: return "SHL";
        case Op::Shr: return "SHR";
        case Op::BitNot: return "BIT_NOT";
        case Op::Not: return "NOT";
        case Op::CmpEq: return "CMP_EQ";
        case Op::CmpNe: return "CMP_NE";
        case Op::CmpLt: return "CMP_LT";
        case Op::CmpLe: return "CMP_LE";
        case Op::CmpGt: return "CMP_GT";
        case Op::CmpGe: return "CMP_GE";
        case Op::IsType: return "IS_TYPE";
        case Op::Jump: return "JUMP";
        case Op::JumpIfFalse: return "JUMP_IF_FALSE";
        case Op::JumpIfTrue: return "JUMP_IF_TRUE";
        case Op::NewList: return "NEW_LIST";
        case Op::NewDict: return "NEW_DICT";
        case Op::GetIndex: return "GET_INDEX";
        case Op::GetField: return "GET_FIELD";
        case Op::IterNew: return "ITER_NEW";
        case Op::IterNext: return "ITER_NEXT";
        case Op::IterEnd: return "ITER_END";
        case Op::SwitchCheck: return "SWITCH_CHECK";
        case Op::CaseMatch: return "CASE_MATCH";
        case Op::AssignIndex: return "ASSIGN_INDEX";
        case Op::CompoundIndex: return "COMPOUND_INDEX";
        case Op::IncDecIndex: return "INCDEC_INDEX";
        case Op::Await: return "AWAIT";
        case Op::Yield: return "YIELD";
        case Op::Fail: return "FAIL";
        case Op::Import: return "IMPORT";
        case Op::Call: return "CALL";
        case Op::Return: return "RETURN";
        case Op::PushScope: return "PUSH_SCOPE";
        case Op::PopScope: return "POP_SCOPE";
        case Op::EvalAst: return "EVAL_AST";
        case Op::ControlBreak: return "CONTROL_BREAK";
        case Op::ControlContinue: return "CONTROL_CONTINUE";
        case Op::RuntimeError: return "RUNTIME_ERROR";
        case Op::Count: break;
    }
    return "?";
}

namespace {

std::string constant_brief(const Constant &c) {
    switch (c.kind) {
        case ConstantKind::Value:
            return c.value ? value_to_string(c.value.get()) : "null";
        case ConstantKind::Name:
            return c.name;
        case ConstantKind::Ast:
            return "<ast>";
        case ConstantKind::Decl:
            return "<decl>";
        case ConstantKind::Type:
            return "<type>";
    }
    return "?";
}

} // namespace

std::string disassemble(const Proto &proto) {
    std::ostringstream out;
    out << "proto " << (proto.name.empty() ? "<anonymous>" : proto.name)
        << " params=" << proto.nparams
        << " required=" << proto.required_params
        << " locals=" << proto.nlocals
        << " max_stack=" << proto.max_stack << "\n";

    for (size_t pc = 0; pc < proto.code.size(); ++pc) {
        const Instr &ins = proto.code[pc];
        const SourceLoc &loc =
            pc < proto.locs.size() ? proto.locs[pc] : SourceLoc();
        out << std::setw(5) << pc << "  "
            << std::setw(18) << std::left << op_name(static_cast<Op>(ins.op))
            << std::right;
        if (ins.flags)
            out << " flags=" << (unsigned)ins.flags;
        if (ins.a)
            out << " a=" << ins.a;
        if (ins.b)
            out << " b=" << ins.b;
        const Op op = static_cast<Op>(ins.op);
        bool const_operand =
            op == Op::LoadConst || op == Op::LoadName ||
            op == Op::StoreName || op == Op::CompoundName ||
            op == Op::IncDecName || op == Op::DefineName ||
            op == Op::IsType || op == Op::EvalAst ||
            op == Op::ControlBreak || op == Op::ControlContinue ||
            op == Op::RuntimeError;
        if (const_operand && ins.b < proto.consts.size())
            out << " (" << constant_brief(proto.consts[ins.b]) << ")";
        if ((op == Op::IterNew || op == Op::IterNext || op == Op::IterEnd) &&
            ins.a < proto.iters.size()) {
            out << " iter=" << ins.a << "(" << proto.iters[ins.a].key_name;
            if (proto.iters[ins.a].two_vars)
                out << "," << proto.iters[ins.a].value_name;
            out << ")";
        }
        if ((op == Op::AssignIndex || op == Op::CompoundIndex ||
             op == Op::IncDecIndex) &&
            ins.b < proto.index_ops.size()) {
            out << " index=" << ins.b << "("
                << proto.index_ops[ins.b].base_name << ","
                << proto.index_ops[ins.b].fields.size() << ")";
        }
        if (op == Op::Import && ins.a < proto.imports.size())
            out << " import=" << proto.imports[ins.a].path;
        if (loc.line > 0)
            out << "  @" << loc.line << ":" << loc.column;
        out << "\n";
    }
    return out.str();
}

} // namespace bc
} // namespace skein
