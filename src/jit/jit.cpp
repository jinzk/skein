#include "jit.h"

#include "diagnostics.h"
#include "eval_context.h"
#include "scopestack.h"
#include "symbol.h"
#include "value_ops.h"
#include "value_runtime.h"
#include "vm_eval.h"

#include <asmjit/asmjit.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
value_t *skein_jit_null();
value_t *skein_jit_dup(value_t *value);
void skein_jit_free(value_t *value);
value_t *skein_jit_binary(value_t *left, value_t *right, int op, int line,
                          int column);
int skein_jit_compare(int op, value_t *left, value_t *right, int *valid,
                      int line, int column);
int skein_jit_truthy(value_t *value);
value_t *skein_jit_load_global(const skein::bc::Proto *proto, uint32_t slot,
                               uint32_t name_index, int line, int column);
value_t *skein_jit_invoke(value_t *callee, value_t **args, uint32_t argc,
                          int line, int column);
int skein_jit_enter(skein::jit::thread_state *ts, function_def *def);
void skein_jit_leave(skein::jit::thread_state *ts);
void skein_jit_cleanup(skein::jit::thread_state *ts, value_t **values,
                       uint32_t count);
}

namespace {

bool read_bool_env(const char *name, bool fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
        std::strcmp(value, "off") == 0 || std::strcmp(value, "no") == 0)
        return false;
    return true;
}

struct JitRuntimeState {
    asmjit::JitRuntime runtime;
    std::mutex mutex;
};

JitRuntimeState &runtime_state() {
    static JitRuntimeState state;
    return state;
}

template <typename Fn>
uint64_t fn_address(Fn fn) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fn));
}

constexpr int kTsOff = 8;
constexpr int kDefOff = 16;
constexpr int kResultOff = 24;
constexpr int kStatusOff = 32;
constexpr int kHeaderSize = 40;

} // namespace

namespace skein {
namespace jit {

struct JitCode {
    const skein::bc::Proto *proto = nullptr;
    asmjit::JitRuntime *runtime = nullptr;
    void (*entry)() = nullptr;
    size_t size = 0;
};

namespace {

invoke_fn g_invoke = nullptr;

} // namespace

void set_invoke(invoke_fn fn) { g_invoke = fn; }
invoke_fn invoke() { return g_invoke; }

bool available() {
#ifdef _WIN32
    /* Win64 ABI lowering is not implemented yet; keep the JIT off. */
    return false;
#else
    static int state = -1;
    if (state < 0)
        state = read_bool_env("SKEIN_JIT", false) ? 1 : 0;
    return state != 0;
#endif
}

bool can_compile(const skein::bc::Proto &proto) {
#ifdef _WIN32
    (void)proto;
    return false;
#else
    if (!proto.params_direct || proto.has_closures || proto.uses_defers ||
        proto.uses_handlers || proto.uses_names || proto.uses_slots ||
        proto.is_generator || proto.is_async || proto.is_method ||
        proto.variadic || !proto.upvalues.empty())
        return false;
    if (proto.nparams != proto.required_params)
        return false;
    if (proto.stack_depth.size() < proto.code.size())
        return false;

    const uint64_t value_count =
        static_cast<uint64_t>(proto.nparams) + proto.max_stack;
    if (value_count > 512)
        return false;

    for (size_t pc = 0; pc < proto.code.size(); ++pc) {
        const skein::bc::Instr &ins = proto.code[pc];
        switch (static_cast<skein::bc::Op>(ins.op)) {
            case skein::bc::Op::Nop:
            case skein::bc::Op::LoadNull:
            case skein::bc::Op::LoadParam:
            case skein::bc::Op::LoadGlobal:
            case skein::bc::Op::Add:
            case skein::bc::Op::Sub:
            case skein::bc::Op::CmpJump:
            case skein::bc::Op::Jump:
            case skein::bc::Op::JumpIfFalse:
            case skein::bc::Op::JumpIfTrue:
            case skein::bc::Op::Call:
            case skein::bc::Op::Return:
            case skein::bc::Op::PushScope:
            case skein::bc::Op::PopScope:
            case skein::bc::Op::Pop:
                break;
            case skein::bc::Op::LoadConst:
                if (ins.b >= proto.consts.size() ||
                    proto.consts[ins.b].kind !=
                        skein::bc::ConstantKind::Value ||
                    !proto.consts[ins.b].value)
                    return false;
                break;
            default:
                return false;
        }
    }
    return true;
#endif
}

JitCode *compile(const skein::bc::Proto &proto) {
    if (!available() || !can_compile(proto))
        return nullptr;

    asmjit::JitRuntime &rt = runtime_state().runtime;
    asmjit::CodeHolder code;
    if (code.init(rt.environment(), rt.cpuFeatures()) != asmjit::kErrorOk)
        return nullptr;

    asmjit::StringLogger logger;
    const bool dump = read_bool_env("SKEIN_JIT_DUMP", false);
    if (dump)
        code.setLogger(&logger);

    asmjit::x86::Assembler a(&code);

    std::vector<asmjit::Label> labels(proto.code.size());
    for (asmjit::Label &label : labels)
        label = a.newLabel();
    const asmjit::Label end_label = a.newLabel();
    const asmjit::Label abort_label = a.newLabel();
    const asmjit::Label epilogue_label = a.newLabel();

    const uint32_t value_count = proto.nparams + proto.max_stack;
    const int values_low =
        kHeaderSize + static_cast<int>(value_count) * 8;
    const int frame_size = (values_low + 15) & ~15;

    auto value_off = [&](uint32_t index) -> int {
        return -values_low + static_cast<int>(index) * 8;
    };
    auto param_off = [&](uint32_t index) -> int {
        return value_off(index);
    };
    auto stack_off = [&](int depth) -> int {
        return value_off(proto.nparams + static_cast<uint32_t>(depth));
    };
    auto target_label = [&](uint32_t target) -> asmjit::Label {
        return target < labels.size() ? labels[target] : end_label;
    };
    auto call_addr = [&](uint64_t address) {
        a.mov(asmjit::x86::r11, asmjit::imm(address));
        a.call(asmjit::x86::r11);
    };
    auto push_reg = [&](int depth, const asmjit::x86::Gp &reg) {
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                     stack_off(depth)),
              reg);
    };
    auto clear_slot = [&](int depth) {
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                     stack_off(depth)),
              asmjit::imm(0));
    };

    /* Prologue. */
    a.push(asmjit::x86::rbp);
    a.mov(asmjit::x86::rbp, asmjit::x86::rsp);
    a.sub(asmjit::x86::rsp, frame_size);
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff),
          asmjit::x86::rdi);
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff),
          asmjit::x86::rsi);
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff),
          asmjit::imm(0));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kStatusOff),
          asmjit::imm(0));
    a.xor_(asmjit::x86::eax, asmjit::x86::eax);
    for (uint32_t i = 0; i < value_count; ++i)
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                     value_off(i)),
              asmjit::x86::rax);
    for (uint32_t i = 0; i < proto.nparams; ++i) {
        a.mov(asmjit::x86::rax,
              asmjit::x86::ptr(asmjit::x86::rdx, i * 8));
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                     param_off(i)),
              asmjit::x86::rax);
    }

    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    a.mov(asmjit::x86::rsi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kDefOff));
    call_addr(fn_address(&skein_jit_enter));
    a.test(asmjit::x86::eax, asmjit::x86::eax);
    a.jz(abort_label);
    a.jmp(proto.code.empty() ? end_label : labels[0]);

    for (size_t pc = 0; pc < proto.code.size(); ++pc) {
        a.bind(labels[pc]);
        const int depth = proto.stack_depth[pc];
        if (depth < 0)
            continue;

        const skein::bc::Instr &ins = proto.code[pc];
        const skein::bc::SourceLoc loc =
            pc < proto.locs.size() ? proto.locs[pc]
                                   : skein::bc::SourceLoc();
        switch (static_cast<skein::bc::Op>(ins.op)) {
            case skein::bc::Op::Nop:
                break;

            case skein::bc::Op::LoadNull:
                call_addr(fn_address(&skein_jit_null));
                push_reg(depth, asmjit::x86::rax);
                break;

            case skein::bc::Op::LoadConst: {
                const skein::bc::Constant &constant = proto.consts[ins.b];
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(constant.value.get())));
                call_addr(fn_address(&skein_jit_dup));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::LoadParam:
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             param_off(ins.b)));
                call_addr(fn_address(&skein_jit_dup));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::LoadGlobal:
                a.mov(asmjit::x86::rdi,
                      asmjit::imm(fn_address(&proto)));
                a.mov(asmjit::x86::esi, asmjit::imm(ins.a));
                a.mov(asmjit::x86::edx, asmjit::imm(ins.b));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_load_global));
                push_reg(depth, asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;

            case skein::bc::Op::Add:
            case skein::bc::Op::Sub: {
                const int left_depth = depth - 2;
                const int right_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(right_depth)));
                const int op = static_cast<skein::bc::Op>(ins.op) ==
                                       skein::bc::Op::Add
                                   ? '+'
                                   : '-';
                a.mov(asmjit::x86::edx, asmjit::imm(op));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_binary));
                clear_slot(right_depth);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)),
                      asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::CmpJump: {
                const int left_depth = depth - 2;
                const int right_depth = depth - 1;
                const int op = ins.flags & 0x7f;
                a.mov(asmjit::x86::edi, asmjit::imm(op));
                a.mov(asmjit::x86::rsi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(left_depth)));
                a.mov(asmjit::x86::rdx,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(right_depth)));
                a.lea(asmjit::x86::rcx,
                      asmjit::x86::ptr(asmjit::x86::rbp, -kStatusOff));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r9d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_compare));
                clear_slot(left_depth);
                clear_slot(right_depth);
                a.cmp(asmjit::x86::dword_ptr(asmjit::x86::rbp,
                                             -kStatusOff),
                      asmjit::imm(0));
                a.je(abort_label);
                a.test(asmjit::x86::al, asmjit::x86::al);
                if ((ins.flags & 0x80) != 0)
                    a.jnz(target_label(ins.b));
                else
                    a.jz(target_label(ins.b));
                break;
            }

            case skein::bc::Op::Jump:
                a.jmp(target_label(ins.b));
                break;

            case skein::bc::Op::JumpIfFalse:
            case skein::bc::Op::JumpIfTrue: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                call_addr(fn_address(&skein_jit_truthy));
                clear_slot(value_depth);
                a.test(asmjit::x86::al, asmjit::x86::al);
                if (static_cast<skein::bc::Op>(ins.op) ==
                    skein::bc::Op::JumpIfFalse)
                    a.jz(target_label(ins.b));
                else
                    a.jnz(target_label(ins.b));
                break;
            }

            case skein::bc::Op::Pop: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                call_addr(fn_address(&skein_jit_free));
                clear_slot(value_depth);
                break;
            }

            case skein::bc::Op::Call: {
                const int argc = static_cast<int>(ins.b);
                const int callee_depth = depth - argc - 1;
                a.mov(asmjit::x86::rdi,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)));
                a.lea(asmjit::x86::rsi,
                      asmjit::x86::ptr(asmjit::x86::rbp,
                                       stack_off(depth - argc)));
                a.mov(asmjit::x86::edx, asmjit::imm(argc));
                a.mov(asmjit::x86::ecx, asmjit::imm(loc.line));
                a.mov(asmjit::x86::r8d, asmjit::imm(loc.column));
                call_addr(fn_address(&skein_jit_invoke));
                for (int i = callee_depth; i < depth; ++i)
                    clear_slot(i);
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(callee_depth)),
                      asmjit::x86::rax);
                a.test(asmjit::x86::rax, asmjit::x86::rax);
                a.jz(abort_label);
                break;
            }

            case skein::bc::Op::Return: {
                const int value_depth = depth - 1;
                a.mov(asmjit::x86::rax,
                      asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             stack_off(value_depth)));
                a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp,
                                             -kResultOff),
                      asmjit::x86::rax);
                clear_slot(value_depth);
                a.jmp(epilogue_label);
                break;
            }

            case skein::bc::Op::PushScope:
            case skein::bc::Op::PopScope:
                break;

            default:
                a.jmp(abort_label);
                break;
        }
    }

    a.bind(end_label);
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff),
          asmjit::imm(0));
    a.jmp(epilogue_label);

    a.bind(abort_label);
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff),
          asmjit::imm(0));

    a.bind(epilogue_label);
    a.mov(asmjit::x86::rdi,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kTsOff));
    a.lea(asmjit::x86::rsi,
          asmjit::x86::ptr(asmjit::x86::rbp, -values_low));
    a.mov(asmjit::x86::edx, asmjit::imm(value_count));
    call_addr(fn_address(&skein_jit_cleanup));
    a.mov(asmjit::x86::rax,
          asmjit::x86::qword_ptr(asmjit::x86::rbp, -kResultOff));
    a.mov(asmjit::x86::rsp, asmjit::x86::rbp);
    a.pop(asmjit::x86::rbp);
    a.ret();

    if (dump) {
        std::fprintf(stderr, "[jit] compile %s\n%s\n",
                     proto.name.empty() ? "<anonymous>" : proto.name.c_str(),
                     logger.data());
    }

    void (*entry)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mutex);
        asmjit::Error err = rt.add(&entry, &code);
        if (err != asmjit::kErrorOk) {
            if (read_bool_env("SKEIN_JIT_DEBUG", false))
                std::fprintf(stderr, "[jit] asmjit error: %s\n",
                             asmjit::DebugUtils::errorAsString(err));
            return nullptr;
        }
    }

    JitCode *result = new JitCode;
    result->proto = &proto;
    result->runtime = &rt;
    result->entry = entry;
    result->size = code.codeSize();
    return result;
}

value_t *enter(JitCode *code, thread_state *ts, function_def *def,
               value_t **args, uint32_t argc) {
    if (!code || !code->entry || !ts)
        return nullptr;
    using Entry = value_t *(*)(thread_state *, function_def *, value_t **,
                               uint32_t);
    Entry entry = reinterpret_cast<Entry>(code->entry);
    return entry(ts, def, args, argc);
}

void release(JitCode *code) {
    if (!code)
        return;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mutex);
        if (code->runtime && code->entry)
            code->runtime->release(code->entry);
    }
    delete code;
}

} // namespace jit
} // namespace skein

/*
 * C helpers called from generated code.  They must not throw C++ exceptions
 * across the JIT frame.
 */

extern "C" value_t *skein_jit_null() { return newnull(); }

extern "C" value_t *skein_jit_dup(value_t *value) { return dupval(value); }

extern "C" void skein_jit_free(value_t *value) { freeval(value); }

extern "C" value_t *skein_jit_binary(value_t *left, value_t *right, int op,
                                    int line, int column) {
    diag_set_location(line, column);
    if (!left || !right) {
        freeval(left);
        freeval(right);
        return nullptr;
    }
    value_t *result = apply_binary_value(op, left, right);
    freeval(left);
    freeval(right);
    return result;
}

extern "C" int skein_jit_compare(int op, value_t *left, value_t *right,
                                 int *valid, int line, int column) {
    diag_set_location(line, column);
    if (valid)
        *valid = 0;
    if (!left || !right) {
        freeval(left);
        freeval(right);
        return 0;
    }

    bool result = false;
    if (left->type == T_int && right->type == T_int) {
        const long long x = ((int_value_t *)left)->value;
        const long long y = ((int_value_t *)right)->value;
        const int cmp = x < y ? -1 : (x > y ? 1 : 0);
        switch (op) {
            case 1: result = cmp > 0; break;
            case 2: result = cmp < 0; break;
            case 3: result = cmp != 0; break;
            case 4: result = cmp == 0; break;
            case 5: result = cmp >= 0; break;
            case 6: result = cmp <= 0; break;
            default: break;
        }
        if (valid)
            *valid = 1;
    } else {
        value_t *compared = skein_vm_do_compare(op, left, right);
        if (compared) {
            result = truthy(compared);
            freeval(compared);
            if (valid)
                *valid = 1;
        }
    }

    freeval(left);
    freeval(right);
    return result ? 1 : 0;
}

extern "C" int skein_jit_truthy(value_t *value) {
    int result = truthy(value) ? 1 : 0;
    freeval(value);
    return result;
}

extern "C" value_t *skein_jit_load_global(const skein::bc::Proto *proto,
                                          uint32_t slot,
                                          uint32_t name_index,
                                          int line, int column) {
    diag_set_location(line, column);
    if (!proto || name_index >= proto->consts.size()) {
        yyerror("invalid bytecode constant");
        return nullptr;
    }
    const char *name = proto->consts[name_index].name.c_str();
    symbol *sym = nullptr;

    scope_context &scope = active_scope_context();
    if (slot < scope.global_slots.size()) {
        symbol *candidate = scope.global_slots[slot];
        if (candidate && candidate->name &&
            std::strcmp(candidate->name, name) == 0)
            sym = candidate;
    }
    if (!sym)
        sym = lookupsym(name);
    if (!sym) {
        yyerror("symbol not found %s", name);
        return nullptr;
    }
    if (sym->is_uninitialized) {
        yyerror("parameter %s is used before it is initialized", name);
        return nullptr;
    }
    return dupval(sym->val);
}

extern "C" value_t *skein_jit_invoke(value_t *callee, value_t **args,
                                     uint32_t argc, int line, int column) {
    diag_set_location(line, column);
    skein::jit::invoke_fn fn = skein::jit::invoke();
    if (!fn) {
        for (uint32_t i = 0; i < argc; ++i)
            freeval(args[i]);
        freeval(callee);
        yyerror("JIT call target is not installed");
        return nullptr;
    }
    return fn(callee, args, argc);
}

extern "C" int skein_jit_enter(skein::jit::thread_state *ts,
                               function_def *def) {
    if (!ts || !ts->ctx)
        return 0;
    eval_context &context = *ts->ctx;
    if (context.vm_call_depth >= ts->max_depth) {
        yyerror("maximum recursion depth (%d) exceeded", ts->max_depth);
        return 0;
    }

    ts->saved_handler_depth = context.handler_depth;
    ts->entered = true;
    ++context.vm_call_depth;
    context.handler_depth = 0;

    const char *name = "<anonymous>";
    int line = g_error_line;
    int column = g_error_column;
    if (def && def->code) {
        if (!def->code->name.empty())
            name = def->code->name.c_str();
        if (line <= 0)
            line = def->code->def_line;
        if (column <= 0)
            column = def->code->def_column;
    }
    diag_push_call(name, line, column);
    return 1;
}

extern "C" void skein_jit_leave(skein::jit::thread_state *ts) {
    if (!ts || !ts->entered || !ts->ctx)
        return;
    eval_context &context = *ts->ctx;
    context.handler_depth = ts->saved_handler_depth;
    if (context.vm_call_depth > 0)
        --context.vm_call_depth;
    diag_pop_call();
    ts->entered = false;
}

extern "C" void skein_jit_cleanup(skein::jit::thread_state *ts,
                                  value_t **values, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        freeval(values[i]);
        values[i] = nullptr;
    }
    skein_jit_leave(ts);
}
