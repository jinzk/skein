#ifndef SKEIN_BYTECODE_H
#define SKEIN_BYTECODE_H

#include "valuetype.h"
#include "symbol.h"

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
struct Proto;

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
    CheckRedefine,
    LoadParam,
    StoreParam,
    CompoundParam,
    IncDecParam,
    DefineSlot,
    LoadSlot,
    StoreSlot,
    CompoundSlot,
    IncDecSlot,
    ClearSlot,
    LoadUpvalue,
    StoreUpvalue,
    CompoundUpvalue,
    IncDecUpvalue,

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
    CmpJump,

    IsType,

    Jump,
    JumpIfFalse,
    JumpIfTrue,

    NewList,
    NewDict,
    GetIndex,
    GetField,
    GetFieldName,

    IterNew,
    IterNext,
    IterEnd,

    SwitchCheck,
    CaseMatch,
    MatchBegin,
    MatchTest,
    MatchBind,

    AssignIndex,
    CompoundIndex,
    IncDecIndex,

    Await,
    Yield,
    Fail,
    Pass,
    Recover,

    PushFailHandler,
    PushRecoverHandler,
    PopHandler,
    CheckResult,
    BindName,
    EnterHandlerBody,
    LeaveHandlerBody,

    DeferPush,

    NewStruct,
    NewEnum,
    DeclareStruct,
    DeclareEnum,
    DeclareTrait,
    DeclareImpl,

    Closure,
    BindFunc,

    CallSpread,

    DeclarePackage,

    Import,

    Call,
    TailCall,
    Return,

    PushScope,
    PopScope,

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
    uint32_t key_slot = UINT32_MAX;
    uint32_t value_slot = UINT32_MAX;
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

struct ConstructDesc {
    std::string module;
    std::string name;
    std::string variant;
    std::vector<std::string> fields;
    std::vector<bool> spread;
};

struct CallDesc {
    std::vector<bool> spread;
};

struct PatternFieldDesc {
    std::string key;
    std::string rest_name;
    uint32_t pattern = UINT32_MAX;
    bool rest = false;
};

struct PatternSlotDesc {
    std::string name;
    uint32_t slot = UINT32_MAX;
};

struct PatternDesc {
    enum class Kind {
        Literal,
        Binding,
        Type,
        Or,
        Range,
        As,
        Rest,
        Never,
        List,
        Container
    };

    Kind kind = Kind::Never;
    std::string name;
    std::string module;
    std::string type_name;
    std::string variant;
    type_constraint type;
    long long range_start = 0;
    long long range_end = 0;
    std::shared_ptr<value_t> literal_value;
    std::shared_ptr<const Proto> literal_expr;
    std::vector<uint32_t> items;
    uint32_t child = UINT32_MAX;
    std::vector<PatternFieldDesc> fields;
    std::vector<PatternSlotDesc> binding_slots;
    bool is_dict = false;
};

struct MatchArmDesc {
    uint32_t pattern = UINT32_MAX;
    bool has_guard = false;
};

struct SlotDesc {
    std::string name;
    type_constraint type;
    bool is_const = false;
};

struct DeclDesc {
    std::string name;
    type_constraint type;
    bool exported = false;
    bool is_const = false;
};

struct DeclFieldMeta {
    std::string name;
    type_constraint type;
    std::shared_ptr<function_code> default_code;
};

struct DeclMethodMeta {
    std::string name;
    std::shared_ptr<function_code> code;
};

struct StructDeclMeta {
    std::string module;
    std::string name;
    bool exported = false;
    std::vector<DeclFieldMeta> fields;
};

struct EnumVariantMeta {
    std::string name;
    std::vector<DeclFieldMeta> fields;
};

struct EnumDeclMeta {
    std::string module;
    std::string name;
    bool exported = false;
    std::vector<EnumVariantMeta> variants;
};

struct TraitParamMeta {
    std::string name;
    type_constraint type;
    bool variadic = false;
    bool has_default = false;
};

struct TraitMethodMeta {
    std::string name;
    std::vector<TraitParamMeta> params;
};

struct TraitDeclMeta {
    std::string module;
    std::string name;
    bool exported = false;
    std::vector<TraitMethodMeta> methods;
};

struct ImplDeclMeta {
    std::string module;
    std::string type_name;
    std::string trait_name;
    bool has_trait = false;
    std::vector<DeclMethodMeta> methods;
};

struct PackageDesc {
    std::string name;
    std::shared_ptr<const Proto> body;
};

enum class ConstantKind {
    Value,
    Name,
    Type,
    Function
};

struct Constant {
    ConstantKind kind = ConstantKind::Value;
    std::shared_ptr<value_t> value;
    std::string name;
    const struct type_constraint *type = nullptr;
    std::shared_ptr<type_constraint> type_storage;
    std::shared_ptr<function_code> function;
};

/*
 * Per-name inline cache for globals.  It is shared by every Executor that
 * runs this Proto and is only valid while the owning global scope and its
 * generation are unchanged.
 */
struct NameCacheEntry {
    struct symbol *sym = nullptr;
    const void *global_owner = nullptr;
    uint64_t generation = 0;
};

/*
 * Per-call-site member cache for `obj.field` with a literal field name.
 * It stores the resolved struct/enum owner, field slot or method.  The
 * owner pointer identifies the type definition, so redefinitions naturally
 * miss the cache.
 */
struct MemberCacheEntry {
    const void *owner = nullptr;
    int variant = -1;
    int field = -1;
    std::shared_ptr<function_def> method;
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
    mutable std::vector<NameCacheEntry> name_ic;
    mutable std::vector<MemberCacheEntry> member_ic;
    std::vector<SourceLoc> locs;
    std::vector<IterDesc> iters;
    std::vector<IndexDesc> index_ops;
    std::vector<ImportDesc> imports;
    std::vector<ConstructDesc> constructs;
    std::vector<StructDeclMeta> struct_decls;
    std::vector<EnumDeclMeta> enum_decls;
    std::vector<TraitDeclMeta> trait_decls;
    std::vector<ImplDeclMeta> impl_decls;
    std::vector<PackageDesc> packages;
    std::vector<PatternDesc> patterns;
    std::vector<std::vector<MatchArmDesc>> match_arms;
    std::vector<CallDesc> call_descs;
    std::vector<std::shared_ptr<const Proto>> defer_protos;

    std::vector<std::string> param_names;
    std::vector<std::string> upvalues;
    std::vector<SlotDesc> slots;
    std::vector<DeclDesc> decls;
    bool uses_names = false;
    bool uses_params = false;
    bool uses_defers = false;
    bool uses_slots = false;

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
