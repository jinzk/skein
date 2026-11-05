#ifndef SKEIN_BYTECODE_H
#define SKEIN_BYTECODE_H

#include "valuetype.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ast;
struct symdef;
struct type_constraint;

namespace skein {
namespace bc {

/*
 * A compact, stack-based instruction.  The first implementation keeps a
 * fixed 8-byte encoding: one opcode byte, one flags byte, a 16-bit small
 * operand and a 32-bit large operand (constant index, jump target or argc).
 *
 * The layout deliberately leaves room for wide instructions and side tables
 * (inline caches, handlers, defer tables) in later phases.
 */
struct Instr {
    uint8_t op = 0;
    uint8_t flags = 0;
    uint16_t a = 0;
    uint32_t b = 0;
};

enum class Op : uint8_t {
    Nop = 0,

    LoadNull,
    LoadConst,
    LoadName,

    StoreName,
    CompoundName,
    IncDecName,
    DefineName,

    Pop,
    Dup,
    Swap,

    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    BitNot,
    Not,

    CmpEq,
    CmpNe,
    CmpLt,
    CmpLe,
    CmpGt,
    CmpGe,

    IsType,

    Jump,
    JumpIfFalse,
    JumpIfTrue,

    NewList,
    NewDict,
    GetIndex,
    GetField,

    IterNew,
    IterNext,
    IterEnd,

    SwitchCheck,
    CaseMatch,

    AssignIndex,
    CompoundIndex,
    IncDecIndex,

    Await,
    Yield,
    Fail,

    Import,

    Call,
    Return,

    PushScope,
    PopScope,

    /*
     * Transitional escape hatch: evaluate a subtree with the AST
     * interpreter in the current runtime scope.  It is used for constructs
     * that have not been lowered to bytecode yet.  The compiler guarantees
     * that a fallback subtree cannot contain control flow that would escape
     * the surrounding bytecode loop; otherwise the whole statement/function
     * falls back to the AST interpreter instead.
     */
    EvalAst,

    ControlBreak,
    ControlContinue,
    RuntimeError,

    Count
};

struct SourceLoc {
    int line = 0;
    int column = 0;
};

struct IterDesc {
    std::string key_name;
    std::string value_name;
    bool two_vars = false;
};

struct IndexDesc {
    std::string base_name;
    std::vector<bool> fields;
};

struct ImportDesc {
    std::string path;
    std::string alias;
    std::string base;
};

enum class ConstantKind {
    Value,
    Name,
    Ast,
    Decl,
    Type
};

struct Constant {
    ConstantKind kind = ConstantKind::Value;
    std::shared_ptr<value_t> value;
    std::string name;
    struct ast *ast = nullptr;
    struct symdef *decl = nullptr;
    const struct type_constraint *type = nullptr;
};

/*
 * A compiled function or top-level chunk.
 *
 * `locs` is parallel to `code`: one source location per instruction.  This is
 * enough to preserve the current diagnostics file:line:column output without
 * storing line information in every instruction.
 */
struct Proto {
    std::string name;
    std::string module;
    int def_line = 0;
    int def_column = 0;

    std::vector<Instr> code;
    std::vector<Constant> consts;
    std::vector<SourceLoc> locs;
    std::vector<IterDesc> iters;
    std::vector<IndexDesc> index_ops;
    std::vector<ImportDesc> imports;

    uint32_t nparams = 0;
    uint32_t required_params = 0;
    uint32_t nlocals = 0;
    uint32_t max_stack = 0;

    bool variadic = false;
    bool is_async = false;
    bool is_generator = false;
    bool is_method = false;

    uint32_t version = 1;
};

using ProtoPtr = std::shared_ptr<const Proto>;

const char *op_name(Op op);

/* Human-readable disassembly, used by SKEIN_VM_TRACE and debugging. */
std::string disassemble(const Proto &proto);

} // namespace bc
} // namespace skein

#endif
