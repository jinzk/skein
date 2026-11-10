#include "vm.h"
#include "vm_internal.h"

#include "ast.h"
#include "compiler.h"
#include "diagnostics.h"
#include "dict.h"
#include "eval.h"
#include "eval_context.h"
#include "eval_internal.h"
#include "pattern.h"
#include "scopestack.h"
#include "structtype.h"
#include "symbol.h"
#include "valuetype.h"
#include "runtime_context.h"
#include "vm_host.h"

#ifdef SKEIN_ENABLE_JIT
#include "jit.h"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct default_cache_entry {
    std::vector<skein::bc::ProtoPtr> protos;
};

struct function_vm_cache {
    function_code *code = nullptr;
    skein::bc::ProtoPtr proto;
    std::shared_ptr<const compiled_pattern_set> param_patterns;
#ifdef SKEIN_ENABLE_JIT
    skein::jit::JitCode *jit = nullptr;
    uint32_t jit_hits = 0;
    bool jit_failed = false;
#endif
    bool compile_failed = false;
};

thread_local std::unordered_map<function_code *, default_cache_entry> default_cache;
/*
 * Registry used only for lifecycle management.  The hot path reaches the
 * cache through function_code::vm_cache, so no hash lookup happens per call.
 */
thread_local std::unordered_map<function_code *, std::unique_ptr<function_vm_cache>>
    vm_caches;

void release_jit_code(function_vm_cache *cache) {
#ifdef SKEIN_ENABLE_JIT
    if (cache && cache->jit) {
        skein::jit::release(cache->jit);
        cache->jit = nullptr;
    }
#else
    (void)cache;
#endif
}

function_vm_cache *cache_for(function_code *code) {
    if (code->vm_cache)
        return static_cast<function_vm_cache *>(code->vm_cache);
    std::unique_ptr<function_vm_cache> entry(new function_vm_cache());
    entry->code = code;
    function_vm_cache *raw = entry.get();
    vm_caches.emplace(code, std::move(entry));
    code->vm_cache = raw;
    return raw;
}

void release_vm_cache(function_vm_cache *cache) {
    if (!cache)
        return;
    release_jit_code(cache);
    if (cache->code)
        cache->code->vm_cache = nullptr;
}

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

#ifdef SKEIN_ENABLE_JIT
int read_int_env(const char *name, int fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    int parsed = std::atoi(value);
    return parsed >= 0 ? parsed : fallback;
}
#endif

uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

void record_fallback_reason(const std::string &reason,
                            const std::string &context = std::string()) {
    const char *path = std::getenv("SKEIN_VM_FALLBACK_FILE");
    if (path && *path) {
        FILE *out = std::fopen(path, "a");
        if (out) {
            std::fprintf(out, "%s\t%s\n",
                         reason.empty() ? "unknown" : reason.c_str(),
                         context.c_str());
            std::fclose(out);
        }
    }
    if (!vm_stats.enabled)
        return;
    std::string key = reason.empty() ? "unknown" : reason;
    if (key.size() > 120)
        key.resize(120);
    ++vm_stats.fallback_reasons[key];
}

skein::bc::ProtoPtr prepare_function_body(
    const std::shared_ptr<function_def> &def) {
    if (!def || !def->code)
        return nullptr;
    function_code *code = def->code.get();
    function_vm_cache &cache = *cache_for(code);
    if (cache.compile_failed)
        return nullptr;
    if (cache.proto)
        return cache.proto;

    skein_vm_ensure_stats_flags();
    const auto compile_start = std::chrono::steady_clock::now();
    std::string error;
    skein::bc::ProtoPtr proto =
        skein_compile_function_body(code, code->module, error);
    if (vm_stats.enabled)
        vm_stats.compile_ns += elapsed_ns(compile_start);
    if (!proto) {
        if (vm_stats.enabled)
            ++vm_stats.functions_fallback;
        record_fallback_reason(
            error, "function " + code->name + " @" +
                       std::to_string(code->def_line) + ":" +
                       std::to_string(code->def_column));
        cache.compile_failed = true;
        if (read_bool_env("SKEIN_VM_DEBUG", false) && !error.empty())
            std::fprintf(stderr, "[vm] fallback function %s: %s\n",
                         code->name.c_str(), error.c_str());
        return nullptr;
    }
    cache.proto = proto;
    if (vm_stats.enabled)
        ++vm_stats.functions_compiled;
    if (!keep_function_ast())
        clear_function_body_ast(code);
    return proto;
}

struct bridge_depth_guard {
    eval_context &context;
    explicit bridge_depth_guard(eval_context &ctx) : context(ctx) {
        ++context.vm_call_depth;
    }
    ~bridge_depth_guard() { --context.vm_call_depth; }
};

#ifdef SKEIN_ENABLE_JIT
void jit_debug_proto(const char *what, const skein::bc::Proto &proto) {
    if (!read_bool_env("SKEIN_JIT_DEBUG", false))
        return;
    std::fprintf(stderr,
                 "[jit] %s %s params_direct=%d names=%d slots=%zu "
                 "closures=%d defers=%d handlers=%d method=%d gen=%d "
                 "async=%d variadic=%d nparams=%u required=%u stack_depth=%zu\n",
                 what, proto.name.empty() ? "<anonymous>" : proto.name.c_str(),
                 proto.params_direct ? 1 : 0, proto.uses_names ? 1 : 0,
                 proto.slots.size(), proto.has_closures ? 1 : 0,
                 proto.uses_defers ? 1 : 0, proto.uses_handlers ? 1 : 0,
                 proto.is_method ? 1 : 0, proto.is_generator ? 1 : 0,
                 proto.is_async ? 1 : 0, proto.variadic ? 1 : 0,
                 proto.nparams, proto.required_params,
                 proto.stack_depth.size());
}

bool jit_function_eligible(function_code *code) {
    return code && !code->is_generator && !code->is_async &&
           !code->variadic && code->required_params == code->nparams;
}

bool jit_args_match(function_code *code, value_t **args, uint32_t argc,
                    bool pattern_frame) {
    if ((int)argc != code->nparams)
        return false;
    int index = 0;
    for (struct symlist *sl = code->sl; sl; sl = sl->next, ++index) {
        if (sl->variadic || (sl->has_pattern && !pattern_frame))
            return false;
        if (!type_constraint_matches(sl->type, args[(size_t)index]))
            return false;
    }
    return true;
}

bool jit_params_have_patterns(function_code *code) {
    for (struct symlist *sl = code->sl; sl; sl = sl->next)
        if (sl->has_pattern)
            return true;
    return false;
}

bool jit_patterns_ready(function_code *code) {
    function_vm_cache *cache = cache_for(code);
    if (cache->param_patterns)
        return !cache->param_patterns->failed;
    std::string error;
    std::shared_ptr<const compiled_pattern_set> compiled =
        skein_compile_parameter_patterns(code, code->module, error);
    if (!compiled) {
        std::shared_ptr<compiled_pattern_set> failed =
            std::make_shared<compiled_pattern_set>();
        failed->failed = true;
        cache->param_patterns = failed;
        return false;
    }
    cache->param_patterns = compiled;
    if (!keep_function_ast()) {
        for (struct symlist *sl = code->sl; sl; sl = sl->next) {
            if (symlist_pattern_ast(sl))
                set_symlist_pattern_ast(sl, NULL);
        }
    }
    return true;
}

uint32_t jit_threshold() {
    static int threshold = -1;
    if (threshold < 0)
        threshold = read_int_env("SKEIN_JIT_THRESHOLD", 100);
    return (uint32_t)threshold;
}

/*
 * Try a direct JIT call.  Returns true when the JIT handled the call and
 * consumed the argument pointers; `out` is then the result (or null on
 * error).  Returns false when the caller should continue with the normal VM
 * fast-call path; arguments are untouched in that case.  The callee value is
 * always borrowed.
 */
bool jit_resolve_and_enter(value_t *callee, value_t **args,
                           uint32_t argc, skein::jit::thread_state *ts,
                           value_t **out) {
    *out = nullptr;
    if (!callee || !ts)
        return false;

    std::shared_ptr<function_def> def;
    function_code *code = nullptr;
    struct method_value_t *method = nullptr;
    std::array<value_t *, 8> inline_args;
    std::vector<value_t *> heap_args;
    value_t **call_args = args;
    uint32_t call_argc = argc;
    bool pattern_frame = false;

    if (callee->type == T_func) {
        struct func_value_t *fv = (struct func_value_t *)callee;
        if (!fv->def || !fv->def->code)
            return false;
        if (fv->def->code->is_method)
            return false;
        def = fv->def;
        code = def->code.get();
        pattern_frame = jit_params_have_patterns(code);
        if (!jit_function_eligible(code) ||
            !jit_args_match(code, call_args, call_argc, pattern_frame))
            return false;
    } else if (callee->type == T_method) {
        method = (struct method_value_t *)callee;
        if (!method->method || !method->method->code)
            return false;
        def = method->method;
        code = def->code.get();
        if (!code->is_method || !jit_function_eligible(code) ||
            jit_params_have_patterns(code))
            return false;
        call_argc = argc + 1;
        if ((int)call_argc != code->nparams)
            return false;
        if (call_argc <= inline_args.size()) {
            call_args = inline_args.data();
        } else {
            heap_args.resize(call_argc);
            call_args = heap_args.data();
        }
        call_args[0] = method->receiver ? method->receiver : newnull();
        for (uint32_t i = 0; i < argc; ++i)
            call_args[i + 1] = args[i];
        if (!jit_args_match(code, call_args, call_argc, false))
            return false;
    } else {
        return false;
    }

    if (pattern_frame && !jit_patterns_ready(code))
        return false;

    function_vm_cache *cache = cache_for(code);
    if (cache->jit_failed)
        return false;
    if (!cache->jit) {
        if (!skein_vm_enabled())
            return false;
        if (cache->jit_hits++ < jit_threshold())
            return false;
        skein::bc::ProtoPtr proto = prepare_function_body(def);
        if (proto)
            jit_debug_proto("try", *proto);
        if (!proto ||
            !skein::jit::can_compile(*proto, false, pattern_frame)) {
            if (read_bool_env("SKEIN_JIT_DEBUG", false))
                std::fprintf(stderr, "[jit] cannot compile %s\n",
                             code->name.empty() ? "<anonymous>"
                                                 : code->name.c_str());
            cache->jit_failed = true;
            return false;
        }
        cache->jit = skein::jit::compile(*proto, false, pattern_frame);
        if (!cache->jit) {
            cache->jit_failed = true;
            return false;
        }
    }

    if (method)
        method->receiver = nullptr;
    *out = skein::jit::enter(cache->jit, ts, def.get(), call_args,
                             call_argc);
    return true;
}

bool skein_vm_try_jit_call_impl(value_t *callee, value_t **args,
                                uint32_t argc, value_t **out,
                                bool tail_call) {
    *out = nullptr;
    if (!skein::jit::available() || !callee)
        return false;

    skein::jit::thread_state ts;
    ts.ctx = &active_eval_context();
    ts.max_depth = skein_vm_max_depth();
    ts.tail_call = tail_call;
    ts.root_module = diag_current_module_name();
    ts.root_source = diag_current_source_name();

    value_t *result = nullptr;
    if (!jit_resolve_and_enter(callee, args, argc, &ts, &result))
        return false;

    while (ts.tail_pending) {
        value_t *next_callee = ts.tail_callee;
        std::vector<value_t *> next_args = std::move(ts.tail_args);
        uint32_t next_argc = ts.tail_argc;
        ts.tail_pending = false;
        ts.tail_callee = nullptr;
        ts.tail_argc = 0;
        ts.tail_call = false;

        value_t *next_out = nullptr;
        if (!jit_resolve_and_enter(next_callee, next_args.data(), next_argc,
                                   &ts, &next_out)) {
            value_t *vm_result = skein_vm_call_value_args(
                next_callee, std::move(next_args));
            freeval(next_callee);
            result = vm_result;
            break;
        }
        freeval(next_callee);
        result = next_out;
        if (!ts.tail_pending)
            break;
    }

    *out = result;
    diag_set_module_name(ts.root_module);
    diag_set_source_name(ts.root_source);
    for (int i = 0; i < ts.retained_diag_frames; ++i)
        diag_pop_call();
    return true;
}

int skein_vm_bind_pattern_frame(function_def *def, value_t **values,
                                uint32_t argc) {
    if (!def || !def->code)
        return 0;
    skein_vm_eval_push_closure_scope(def->closure);

    int index = 0;
    for (struct symlist *sl = def->code->sl;
         sl && !sl->variadic; sl = sl->next, ++index) {
        if (index >= (int)argc)
            break;
        if (sl->has_pattern) {
            std::vector<pattern_binding> bindings;
            std::shared_ptr<function_def> def_ref(def,
                                                  [](function_def *) {});
            ParamPatternResult native = skein_vm_match_parameter_pattern(
                def_ref, index, values[(size_t)index], bindings);
            if (native != ParamPatternResult::Matched) {
                free_pattern_bindings(bindings);
                yyerror("function parameter does not match its pattern");
                return 0;
            }
            for (auto &binding : bindings) {
                struct symbol *target = newsym(binding.name.c_str());
                target->val = binding.value;
                binding.value = NULL;
                savesym(target);
            }
            bindings.clear();
            freeval(values[(size_t)index]);
            values[(size_t)index] = NULL;
        } else {
            struct symbol *sym = newsym(sl->sym->name);
            sym->val = values[(size_t)index];
            values[(size_t)index] = NULL;
            if (sl->type)
                type_constraint_assign(&sym->type, sl->type);
            savesym(sym);
        }
    }
    return 1;
}

value_t *skein_vm_build_struct(const skein::bc::ConstructDesc &desc,
                               value_t **fields) {
    std::vector<std::pair<std::string, value_t *>> entries;
    entries.reserve(desc.fields.size());
    for (size_t i = 0; i < desc.fields.size(); ++i) {
        entries.emplace_back(desc.fields[i], fields[i]);
        fields[i] = nullptr;
    }
    auto fn = skein_vm_host().fn_build_struct_value;
    if (!fn) {
        for (auto &entry : entries)
            freeval(entry.second);
        yyerror("JIT struct service is not installed");
        return nullptr;
    }
    return fn(desc.module.c_str(), desc.name.c_str(), entries, desc.spread);
}

/* CALL helper used by generated code.  Always consumes callee and args. */
value_t *skein_vm_jit_invoke(value_t *callee, value_t **args, uint32_t argc) {
    value_t *out = nullptr;
    if (skein_vm_try_jit_call_impl(callee, args, argc, &out, false)) {
        freeval(callee);
        return out;
    }

    std::vector<value_t *> fallback(args, args + argc);
    value_t *result = skein_vm_call_value_args(callee, std::move(fallback));
    freeval(callee);
    return result;
}

bool skein_vm_try_jit_call(value_t *callee, value_t **args, uint32_t argc,
                           value_t **out) {
    return skein_vm_try_jit_call_impl(callee, args, argc, out, false);
}

value_t *skein_vm_jit_tail_invoke(value_t *callee, value_t **args,
                                  uint32_t argc) {
    value_t *out = nullptr;
    if (skein_vm_try_jit_call_impl(callee, args, argc, &out, true)) {
        freeval(callee);
        return out;
    }

    std::vector<value_t *> fallback(args, args + argc);
    value_t *result = skein_vm_call_value_args(callee, std::move(fallback));
    freeval(callee);
    return result;
}
#endif

} // namespace

#ifdef SKEIN_ENABLE_JIT
/*
 * Fast-path resolver called from generated CALL code.  It only returns an
 * entry when the callee is already compiled and its arguments match; the
 * generic invoke path remains responsible for compiling, methods, builtins,
 * pattern bindings, and VM fallbacks.
 */
static void *jit_resolve_direct_target(value_t *callee, value_t **args,
                                       uint32_t argc,
                                       skein::jit::thread_state *ts,
                                       function_def **def_out) {
    if (def_out)
        *def_out = nullptr;
    if (!callee || callee->type != T_func || !ts)
        return nullptr;
    /*
     * Tail calls and pending trampolines rely on per-call thread_state
     * ownership; keep those on the generic invoke path.
     */
    if (ts->tail_call || ts->tail_pending)
        return nullptr;
    struct func_value_t *fn = (struct func_value_t *)callee;
    if (!fn->def || !fn->def->code)
        return nullptr;
    function_code *code = fn->def->code.get();
    if (code->is_method || !jit_function_eligible(code))
        return nullptr;
    if ((uint32_t)code->nparams != argc)
        return nullptr;
    if (jit_params_have_patterns(code))
        return nullptr;
    if (!jit_args_match(code, args, argc, false))
        return nullptr;
    function_vm_cache *cache = cache_for(code);
    if (!cache->jit || !cache->proto)
        return nullptr;
    for (const skein::bc::Instr &ins : cache->proto->code) {
        if (static_cast<skein::bc::Op>(ins.op) == skein::bc::Op::TailCall)
            return nullptr;
    }
    if (def_out)
        *def_out = fn->def.get();
    return skein::jit::entry(cache->jit);
}
#endif

static void ensure_vm_host() {
    static const bool installed = []() {
        vm_host &host = skein_vm_host();
        host.fn_bind_pattern_value = &bind_pattern_value;
        host.fn_free_pattern_bindings = &free_pattern_bindings;
        host.fn_build_struct_value = &build_struct_value;
        host.fn_build_enum_value = &build_enum_value;
        host.fn_declare_struct_meta = &declare_struct_meta;
        host.fn_declare_enum_meta = &declare_enum_meta;
        host.fn_declare_trait_meta = &declare_trait_meta;
        host.fn_declare_impl_meta = &declare_impl_meta;
        host.fn_fiber_await_task = &fiber_await_task;
        host.fn_fiber_is_generator = &fiber_is_generator;
        host.fn_fiber_yield_value = &fiber_yield_value;
        host.fn_generator_next_value = &generator_next_value;
        host.fn_get_generator_value = &get_generator_value;
        host.fn_get_task_value = &get_task_value;
        host.fn_package_build_namespace = &package_build_namespace;
        host.fn_package_import = &package_import;
        host.fn_package_is_loading = &package_is_loading;
        host.fn_package_publish = &package_publish;
        host.fn_skein_vm_call_value_args = &skein_vm_call_value_args;
        host.fn_skein_vm_prepare_function_body =
            &skein_vm_prepare_function_body;
#ifdef SKEIN_ENABLE_JIT
        host.fn_skein_vm_try_jit_call = &skein_vm_try_jit_call;
        host.fn_skein_vm_jit_invoke = &skein_vm_jit_invoke;
        skein::jit::set_invoke(&skein_vm_jit_invoke);
        skein::jit::set_direct(&jit_resolve_direct_target);
        skein::jit::set_tail_invoke(&skein_vm_jit_tail_invoke);
        skein::jit::set_construct(&skein_vm_build_struct);
        skein::jit::set_bind_frame(&skein_vm_bind_pattern_frame);
#endif
        return true;
    }();
    (void)installed;
}

struct value_t *skein_vm_call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args) {
    return call_value_args(callee, std::move(args));
}

skein::bc::ProtoPtr skein_vm_prepare_function_body(
    const std::shared_ptr<function_def> &def) {
    ensure_vm_host();
    return prepare_function_body(def);
}

static void populate_ast_upvalues(const std::shared_ptr<function_def> &def) {
    if (!def || !def->code || !def->code->upvalues.empty() || !def->closure)
        return;
    /* The AST backend does not pre-populate function_code::upvalues.
     * Recover the visible non-global names before compiling so the body
     * binds them as upvalues and captured locals shadow globals. */
    std::unordered_set<std::string> seen;
    for (scope_t *scope = def->closure.get();
         scope && !scope->is_global; scope = scope->parent.get()) {
        for (struct symbol *sym : scope->syms.small) {
            if (!sym || !sym->name || !*sym->name)
                continue;
            if (seen.insert(sym->name).second)
                def->code->upvalues.emplace_back(sym->name);
        }
    }
}

static void recover_ast_proto_upvalues(
    const std::shared_ptr<function_def> &def, skein::bc::Proto &copy) {
    if (!copy.upvalues.empty() || !def->closure)
        return;
    /* A cached Proto may have been compiled before the AST backend
     * populated function_code::upvalues.  Recover the free names referenced
     * by the body so the child can rebuild a closure scope for them. */
    auto find_closure_symbol = [&](const std::string &name) {
        std::string_view key(name);
        for (scope_t *scope = def->closure.get();
             scope && !scope->is_global; scope = scope->parent.get()) {
            if (struct symbol *sym = scope->syms.find(key))
                return sym;
        }
        return (struct symbol *)nullptr;
    };
    std::unordered_set<std::string> seen;
    auto add_upvalue = [&](const std::string &name) {
        if (name.empty() || name == "std")
            return;
        if (!seen.insert(name).second)
            return;
        if (!find_closure_symbol(name))
            return;
        copy.upvalues.push_back(name);
    };
    for (const skein::bc::Instr &ins : copy.code) {
        switch ((skein::bc::Op)ins.op) {
            case skein::bc::Op::LoadName:
            case skein::bc::Op::StoreName:
            case skein::bc::Op::CompoundName:
            case skein::bc::Op::IncDecName:
                if (ins.b < copy.consts.size()) {
                    const skein::bc::Constant &constant = copy.consts[ins.b];
                    if (constant.kind == skein::bc::ConstantKind::Name)
                        add_upvalue(constant.name);
                }
                break;
            default:
                break;
        }
    }
    for (const skein::bc::IndexDesc &index : copy.index_ops) {
        if (index.base_kind == skein::bc::IndexBaseKind::Name)
            add_upvalue(index.base_name);
    }
}

static std::shared_ptr<skein::bc::Proto> clone_thread_proto(
    const skein::bc::ProtoPtr &source) {
    std::shared_ptr<skein::bc::Proto> copy =
        std::make_shared<skein::bc::Proto>(*source);
    copy->name_ic.clear();
    copy->member_ic.clear();
    copy->index_read_ic.clear();
    return copy;
}

static bool validate_transferable_body(
    const skein::bc::ProtoPtr &source, const char *what,
    std::string &error) {
    if (source->has_closures || source->uses_defers ||
        !source->defer_protos.empty()) {
        error = std::string(what) + " cannot use closures or defer";
        return false;
    }
    for (const skein::bc::Constant &constant : source->consts) {
        if (constant.function) {
            error = std::string(what) + " contains a nested function";
            return false;
        }
    }
    for (const skein::bc::PatternDesc &pattern : source->patterns) {
        if (pattern.literal_expr) {
            error = std::string(what) +
                    " contains a nested pattern expression";
            return false;
        }
    }
    return true;
}

bool skein_vm_prepare_thread_proto(
    const struct value_t *function, skein::bc::ProtoPtr &proto,
    std::string &error) {
    if (!function || function->type != T_func) {
        error = "thread entry must be a function";
        return false;
    }
    const func_value_t *value = (const func_value_t *)function;
    if (!value->def || !value->def->code) {
        error = "thread entry has no function code";
        return false;
    }
    const std::shared_ptr<function_def> &def = value->def;
    if (def->code->nparams != 1 ||
        def->code->required_params != 1 ||
        def->code->variadic ||
        def->code->is_async ||
        def->code->is_generator ||
        def->code->is_method) {
        error = "thread entry must be a synchronous function with one "
                "parameter";
        return false;
    }
    populate_ast_upvalues(def);
    skein::bc::ProtoPtr source = skein_vm_prepare_function_body(def);
    if (!source) {
        error = "cannot compile thread entry";
        return false;
    }
    if (!validate_transferable_body(source, "thread entry", error))
        return false;
    std::shared_ptr<skein::bc::Proto> copy = clone_thread_proto(source);
    recover_ast_proto_upvalues(def, *copy);
    proto = std::move(copy);
    return true;
}

bool skein_vm_prepare_thread_helper(
    const struct value_t *function, skein::bc::ProtoPtr &proto,
    std::string &error) {
    if (!function || function->type != T_func) {
        error = "thread function value is not callable";
        return false;
    }
    const func_value_t *value = (const func_value_t *)function;
    if (!value->def || !value->def->code) {
        error = "thread function value has no function code";
        return false;
    }
    const std::shared_ptr<function_def> &def = value->def;
    if (def->code->is_method) {
        error = "cannot transfer a method value";
        return false;
    }
    if (def->code->variadic) {
        error = "cannot transfer a variadic function";
        return false;
    }
    if (def->code->required_params != def->code->nparams) {
        error = "cannot transfer a function with default parameters";
        return false;
    }
    for (struct symlist *param = def->code->sl; param; param = param->next) {
        if (param->has_pattern) {
            error = "cannot transfer a function with pattern parameters";
            return false;
        }
    }
    populate_ast_upvalues(def);
    skein::bc::ProtoPtr source = skein_vm_prepare_function_body(def);
    if (!source) {
        error = "cannot compile thread function value";
        return false;
    }
    if (!validate_transferable_body(source, "thread function value", error))
        return false;
    std::shared_ptr<skein::bc::Proto> copy = clone_thread_proto(source);
    recover_ast_proto_upvalues(def, *copy);
    proto = std::move(copy);
    return true;
}

struct thread_transfer_context {
    std::unordered_map<const thread_transfer_function *, struct value_t *>
        functions;

    ~thread_transfer_context() {
        /* Break capture cycles before dropping the materialized functions. */
        for (auto &entry : functions) {
            struct func_value_t *function = (struct func_value_t *)entry.second;
            if (!function->def)
                continue;
            for (const std::shared_ptr<upvalue_cell> &cell :
                 function->def->upvalues) {
                if (cell && cell->value) {
                    freeval(cell->value);
                    cell->value = nullptr;
                }
            }
        }
        for (auto &entry : functions)
            freeval(entry.second);
    }
};

namespace {

void free_transferred_def(struct function_def *def) {
    if (!def)
        return;
    for (const std::shared_ptr<upvalue_cell> &cell : def->upvalues) {
        if (cell && cell->value) {
            freeval(cell->value);
            cell->value = nullptr;
        }
    }
    delete def;
}

std::shared_ptr<function_code> make_transfer_code(
    const thread_transfer_function &payload) {
    const skein::bc::Proto &proto = *payload.proto;
    std::shared_ptr<function_code> code(
        new function_code(), free_function_code);
    code->nparams = (int)proto.nparams;
    code->required_params = (int)proto.required_params;
    code->name = proto.name.empty() ? payload.name : proto.name;
    code->module = proto.module;
    code->is_async = proto.is_async;
    code->is_generator = proto.is_generator;
    code->is_method = proto.is_method;
    code->variadic = proto.variadic;
    code->def_line = proto.def_line;
    code->def_column = proto.def_column;
    code->upvalues = proto.upvalues;

    struct symlist *head = nullptr;
    struct symlist *tail = nullptr;
    for (uint32_t i = 0; i < proto.param_names.size(); ++i) {
        std::string name = proto.param_names[i];
        struct symbol *sym = newsym(name.empty() ? "arg" : name.c_str());
        struct symlist *node = newsymlist(sym, nullptr, nullptr);
        if (proto.variadic && i + 1 == proto.param_names.size())
            node->variadic = true;
        if (i < payload.pattern_roots.size() &&
            payload.pattern_roots[i] != UINT32_MAX)
            node->has_pattern = true;
        if (tail)
            tail->next = node;
        else
            head = node;
        tail = node;
    }
    code->sl = head;

    function_vm_cache &cache = *cache_for(code.get());
    cache.proto = payload.proto;
    cache.compile_failed = false;
    if (payload.pattern_proto && !payload.pattern_roots.empty()) {
        std::shared_ptr<compiled_pattern_set> compiled =
            std::make_shared<compiled_pattern_set>();
        compiled->proto = payload.pattern_proto;
        compiled->roots = payload.pattern_roots;
        cache.param_patterns = compiled;
    }
    return code;
}

bool materialize_thread_function(
    const std::shared_ptr<thread_transfer_context> &context,
    const std::shared_ptr<thread_transfer_function> &payload,
    struct value_t *&out, std::string &error);

bool materialize_thread_value(
    const std::shared_ptr<thread_transfer_context> &context,
    const thread_transfer_value &value, struct value_t *&out,
    std::string &error) {
    switch (value.kind) {
        case thread_transfer_value::Kind::Null:
            out = newnull();
            return true;
        case thread_transfer_value::Kind::Data:
            if (!value.data) {
                error = "invalid transferred data value";
                return false;
            }
            out = dupval(value.data.get());
            return true;
        case thread_transfer_value::Kind::Namespace: {
            struct dict_value_t *dict = newdictval();
            dict->payload->is_namespace = true;
            for (const auto &field : value.fields) {
                struct value_t *field_value = nullptr;
                if (!materialize_thread_value(context, field.second,
                                              field_value, error)) {
                    freeval((struct value_t *)dict);
                    return false;
                }
                dict_append_item(dict, newstrval(field.first.c_str()),
                                 field_value);
            }
            out = (struct value_t *)dict;
            return true;
        }
        case thread_transfer_value::Kind::Heap: {
            if (!value.heap) {
                error = "invalid transferred heap";
                return false;
            }
            struct value_t *less = nullptr;
            if (!materialize_thread_value(context, value.heap->less, less,
                                          error))
                return false;
            struct heap_less_deleter {
                void operator()(struct value_t *item) const {
                    freeval(item);
                }
            };
            std::unique_ptr<struct value_t, heap_less_deleter> less_owner(
                less);
            std::vector<std::shared_ptr<heap_node>> nodes;            nodes.reserve(value.heap->nodes.size());
            for (const thread_transfer_heap_node &record :
                 value.heap->nodes) {
                std::shared_ptr<heap_node> node = std::make_shared<heap_node>();
                node->rank = (int)record.rank;
                if (!materialize_thread_value(context, record.value,
                                              node->value, error))
                    return false;
                nodes.push_back(std::move(node));
            }
            if (nodes.empty()) {
                if (value.heap->root != UINT32_MAX) {
                    error = "transferred heap root is out of range";
                    return false;
                }
            } else if (value.heap->root >= nodes.size()) {
                error = "transferred heap root is out of range";
                return false;
            }
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const thread_transfer_heap_node &record =
                    value.heap->nodes[i];
                if (record.left != UINT32_MAX) {
                    if (record.left >= nodes.size()) {
                        error = "transferred heap child is out of range";
                        return false;
                    }
                    nodes[i]->left = nodes[record.left];
                }
                if (record.right != UINT32_MAX) {
                    if (record.right >= nodes.size()) {
                        error = "transferred heap child is out of range";
                        return false;
                    }
                    nodes[i]->right = nodes[record.right];
                }
            }
            struct heap_value_t *heap =
                newheapval(less_owner.release());
            if (!nodes.empty())
                heap->payload->root = nodes[value.heap->root];
            heap->payload->size = (std::size_t)value.heap->size;
            out = (struct value_t *)heap;
            return true;
        }
        case thread_transfer_value::Kind::Container: {
            if (!value.container) {
                error = "invalid transferred container";
                return false;
            }
            const thread_transfer_container &source = *value.container;
            switch (source.kind) {
            case thread_transfer_container::Kind::List: {
                struct list_value_t *list = newlistval();
                for (const thread_transfer_value &item : source.items) {
                    struct value_t *child = nullptr;
                    if (!materialize_thread_value(context, item, child,
                                                  error)) {
                        freeval((struct value_t *)list);
                        return false;
                    }
                    list->payload->items.push_back(child);
                }
                out = (struct value_t *)list;
                return true;
            }
            case thread_transfer_container::Kind::Dict: {
                struct dict_value_t *dict = newdictval();
                for (const auto &entry : source.entries) {
                    struct value_t *key = nullptr;
                    struct value_t *item = nullptr;
                    if (!materialize_thread_value(context, entry.first, key,
                                                  error)) {
                        freeval((struct value_t *)dict);
                        return false;
                    }
                    if (!materialize_thread_value(context, entry.second, item,
                                                  error)) {
                        freeval(key);
                        freeval((struct value_t *)dict);
                        return false;
                    }
                    dict_append_item(dict, key, item);
                }
                out = (struct value_t *)dict;
                return true;
            }
            case thread_transfer_container::Kind::Set: {
                struct set_value_t *set = newsetval();
                for (const thread_transfer_value &item : source.items) {
                    struct value_t *key = nullptr;
                    if (!materialize_thread_value(context, item, key,
                                                  error)) {
                        freeval((struct value_t *)set);
                        return false;
                    }
                    bool added = false;
                    std::shared_ptr<set_node> root =
                        set_insert(set->payload->root, key, added);
                    if (added) {
                        set->payload->root = std::move(root);
                        set->payload->order = deque_push_back_payload(
                            set->payload->order, key);
                        ++set->payload->size;
                    }
                    freeval(key);
                }
                out = (struct value_t *)set;
                return true;
            }
            case thread_transfer_container::Kind::Deque: {
                struct deque_value_t *deque = newdequeval();
                for (const thread_transfer_value &item : source.items) {
                    struct value_t *child = nullptr;
                    if (!materialize_thread_value(context, item, child,
                                                  error)) {
                        freeval((struct value_t *)deque);
                        return false;
                    }
                    deque->payload = deque_push_back_payload(
                        deque->payload, child);
                    freeval(child);
                }
                out = (struct value_t *)deque;
                return true;
            }
            case thread_transfer_container::Kind::Lru: {
                struct lru_value_t *lru =
                    newlruval((std::size_t)source.capacity);
                for (const auto &entry : source.entries) {
                    struct value_t *key = nullptr;
                    struct value_t *item = nullptr;
                    if (!materialize_thread_value(context, entry.first, key,
                                                  error)) {
                        freeval((struct value_t *)lru);
                        return false;
                    }
                    if (!materialize_thread_value(context, entry.second, item,
                                                  error)) {
                        freeval(key);
                        freeval((struct value_t *)lru);
                        return false;
                    }
                    lru->payload->items.emplace_back(key, item);
                }
                out = (struct value_t *)lru;
                return true;
            }
            }
            error = "invalid transferred container kind";
            return false;
        }
        case thread_transfer_value::Kind::Function:
            if (!value.function) {
                error = "invalid transferred function value";
                return false;
            }
            return materialize_thread_function(context, value.function, out,
                                                error);
    }
    error = "invalid transferred value";
    return false;
}

bool materialize_thread_function(
    const std::shared_ptr<thread_transfer_context> &context,
    const std::shared_ptr<thread_transfer_function> &payload,
    struct value_t *&out, std::string &error) {
    if (!payload || !payload->proto) {
        error = "invalid transferred function";
        return false;
    }
    auto found = context->functions.find(payload.get());
    if (found != context->functions.end()) {
        out = dupval(found->second);
        return true;
    }

    ensure_vm_host();
    std::shared_ptr<function_code> code = make_transfer_code(*payload);
    std::shared_ptr<function_def> def(
        new function_def(), free_transferred_def);
    def->code = code;
    def->upvalues.assign(payload->proto->upvalues.size(), nullptr);
    struct func_value_t *function =
        (struct func_value_t *)newfuncval(code, nullptr, {});
    function->def = std::move(def);
    context->functions.emplace(payload.get(), (struct value_t *)function);

    for (std::size_t i = 0;
         i < payload->captures.size() && i < function->def->upvalues.size();
         ++i) {
        struct value_t *capture = nullptr;
        if (!materialize_thread_value(context, payload->captures[i], capture,
                                      error)) {
            return false;
        }
        std::shared_ptr<upvalue_cell> cell = std::make_shared<upvalue_cell>();
        cell->value = capture;
        if (i < payload->proto->upvalues.size())
            cell->name = payload->proto->upvalues[i];
        function->def->upvalues[i] = std::move(cell);
    }

    struct symlist *param = code->sl;
    for (std::size_t i = 0;
         i < payload->defaults.size() && param &&
         (int)i < code->nparams;
         ++i, param = param->next) {
        if (payload->defaults[i].kind == thread_transfer_value::Kind::Null)
            continue;
        struct value_t *value = nullptr;
        if (!materialize_thread_value(context, payload->defaults[i], value,
                                      error)) {
            return false;
        }
        param->has_default = true;
        set_symlist_default_ast(param, newvalnode(value));
    }
    out = dupval((struct value_t *)function);
    return true;
}

} // namespace

std::shared_ptr<thread_transfer_context>
skein_vm_thread_transfer_context_create() {
    return std::make_shared<thread_transfer_context>();
}

bool skein_vm_materialize_thread_value(
    const std::shared_ptr<thread_transfer_context> &context,
    const struct thread_transfer_value &value, struct value_t *&out,
    std::string &error) {
    if (!context) {
        error = "missing thread transfer context";
        out = nullptr;
        return false;
    }
    out = nullptr;
    return materialize_thread_value(context, value, out, error);
}

bool skein_vm_apply_thread_types(
    const std::shared_ptr<thread_transfer_context> &context,
    const std::vector<struct thread_transfer_type> &types,
    std::string &error) {
    if (!context) {
        error = "missing thread transfer context";
        return false;
    }
    for (const thread_transfer_type &type : types) {
        struct struct_def *structure = nullptr;
        struct enum_def *enumeration = nullptr;
        if (type.is_enum)
            enumeration =
                structtype_find_enum(type.module.c_str(), type.name.c_str());
        else
            structure =
                structtype_find_struct(type.module.c_str(), type.name.c_str());
        if (!structure && !enumeration) {
            error = "thread payload type is not registered: " + type.module +
                    "." + type.name;
            return false;
        }
        struct type_def *definition =
            structure ? (struct type_def *)structure
                      : (struct type_def *)enumeration;
        for (const auto &method : type.methods) {
            if (!method.second) {
                error = "thread payload method is missing: " + method.first;
                return false;
            }
            if (definition->methods.count(method.first))
                continue;
            struct value_t *function_value = nullptr;
            thread_transfer_value transfer;
            transfer.kind = thread_transfer_value::Kind::Function;
            transfer.function = method.second;
            if (!materialize_thread_value(context, transfer, function_value,
                                          error))
                return false;
            if (!function_value || function_value->type != T_func) {
                freeval(function_value);
                error = "thread payload method is not callable";
                return false;
            }
            const func_value_t *function =
                (const func_value_t *)function_value;
            std::shared_ptr<function_def> method_def =
                function->def;
            freeval(function_value);
            if (!method_def) {
                error = "thread payload method has no function definition";
                return false;
            }
            std::string method_error;
            if (!structtype_add_method(definition, method.first,
                                       std::move(method_def), method_error)) {
                error = method_error.empty()
                            ? "cannot attach thread payload method"
                            : method_error;
                return false;
            }
        }
        auto attach_default = [&](struct field_def &field,
                                  const thread_transfer_type_field &source) {
            if (!source.has_default || !source.default_function)
                return true;
            if (field.has_default)
                return true;
            struct value_t *function_value = nullptr;
            thread_transfer_value transfer;
            transfer.kind = thread_transfer_value::Kind::Function;
            transfer.function = source.default_function;
            if (!materialize_thread_value(context, transfer, function_value,
                                          error))
                return false;
            if (!function_value || function_value->type != T_func) {
                freeval(function_value);
                error = "thread payload field default is not callable";
                return false;
            }
            const func_value_t *function =
                (const func_value_t *)function_value;
            field.default_value = function->def;
            field.has_default = true;
            freeval(function_value);
            return true;
        };
        if (structure) {
            for (const thread_transfer_type_field &field : type.fields) {
                for (struct field_def &declared : structure->fields) {
                    if (declared.name == field.name) {
                        if (!attach_default(declared, field))
                            return false;
                        break;
                    }
                }
            }
        } else {
            for (const thread_transfer_type_variant &variant : type.variants) {
                for (struct variant_def &declared : enumeration->variants) {
                    if (declared.name != variant.name)
                        continue;
                    for (const thread_transfer_type_field &field :
                         variant.fields) {
                        for (struct field_def &declared_field :
                             declared.fields) {
                            if (declared_field.name == field.name) {
                                if (!attach_default(declared_field, field))
                                    return false;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }
    }
    return true;
}

static thread_local bool cache_clearing = false;
static thread_local bool vm_bridge_teardown = false;

void skein_vm_forget_function_code(function_code *code) {
    if (!code || cache_clearing || vm_bridge_teardown)
        return;
    default_cache.erase(code);
    auto it = vm_caches.find(code);
    if (it != vm_caches.end()) {
        release_vm_cache(it->second.get());
        vm_caches.erase(it);
    }
}

void skein_vm_shutdown() {
    if (cache_clearing)
        return;
    cache_clearing = true;
    vm_bridge_teardown = true;
    default_cache.clear();
    for (auto &entry : vm_caches) {
        release_vm_cache(entry.second.get());
    }
    vm_caches.clear();
    cache_clearing = false;
}

struct value_t *skein_vm_eval_statement(struct ast *statement) {
    ensure_vm_host();
    std::string defer_error;
    int defer_line = 0;
    int defer_column = 0;
    if (!skein_validate_defer_references(statement, defer_error,
                                         &defer_line, &defer_column)) {
        if (defer_line > 0)
            diag_set_location(defer_line, defer_column);
        yyerror("%s", defer_error.c_str());
        return NULL;
    }
    if (!skein_vm_enabled())
        return eval(statement);

    skein_vm_ensure_stats_flags();
    const auto compile_start = std::chrono::steady_clock::now();
    std::string error;
    skein::bc::ProtoPtr proto = skein_compile_statement(
        statement, diag_current_module_name(), error);
    if (vm_stats.enabled)
        vm_stats.compile_ns += elapsed_ns(compile_start);
    if (!proto) {
        if (vm_stats.enabled)
            ++vm_stats.statements_fallback;
        record_fallback_reason(
            error, "statement @" +
                       std::to_string(statement ? statement->line : 0) + ":" +
                       std::to_string(statement ? statement->column : 0));
        if (read_bool_env("SKEIN_VM_DEBUG", false) && !error.empty())
            std::fprintf(stderr, "[vm] fallback statement: %s\n",
                         error.c_str());
        return eval(statement);
    }
    if (vm_stats.enabled)
        ++vm_stats.statements_compiled;
#ifdef SKEIN_ENABLE_JIT
    if (skein::jit::available() && skein::jit::can_compile(*proto, true)) {
        skein::jit::JitCode *code = skein::jit::compile(*proto, true);
        if (code) {
            const auto jit_start = std::chrono::steady_clock::now();
            skein::jit::thread_state ts;
            ts.ctx = &active_eval_context();
            ts.max_depth = skein_vm_max_depth();
            ts.top_level = true;
            value_t *result =
                skein::jit::enter(code, &ts, nullptr, nullptr, 0);
            skein::jit::release(code);
            if (vm_stats.enabled)
                vm_stats.execute_ns += elapsed_ns(jit_start);
            if (active_eval_context().control != EVAL_CONTROL_NONE)
                diag_set_location(0, 0);
            return result;
        }
    }
#endif
    const auto execute_start = std::chrono::steady_clock::now();
    value_t *result = skein_vm_execute_proto(*proto);
    if (vm_stats.enabled)
        vm_stats.execute_ns += elapsed_ns(execute_start);
    if (active_eval_context().control != EVAL_CONTROL_NONE)
        diag_set_location(0, 0);
    return result;
}

bool skein_vm_try_eval_default(const std::shared_ptr<function_def> &def,
                               int index, struct value_t *&out) {
    ensure_vm_host();
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
            struct ast *default_ast = symlist_default_ast(param);
            if (!param->has_default || !default_ast)
                continue;
            std::string error;
            skein::bc::ProtoPtr proto = skein_compile_default_expression(
                code, default_ast, i, code->module, error);
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
            if (!keep_function_ast())
                set_symlist_default_ast(param, NULL);
        }
        it = default_cache.emplace(code, std::move(entry)).first;
    }

    if ((size_t)index >= it->second.protos.size() ||
        !it->second.protos[(size_t)index])
        return false;

    out = skein_vm_execute_proto(*it->second.protos[(size_t)index]);
    return true;
}

ParamPatternResult skein_vm_match_parameter_pattern(
    const std::shared_ptr<function_def> &def, int index,
    struct value_t *value, std::vector<pattern_binding> &bindings) {
    ensure_vm_host();
    if (!def || !def->code || index < 0 || !value)
        return ParamPatternResult::Unsupported;
    if (!skein_vm_enabled())
        return ParamPatternResult::Unsupported;

    function_code *code = def->code.get();
    function_vm_cache &cache = *cache_for(code);
    if (!cache.param_patterns) {
        if (!skein_vm_enabled())
            return ParamPatternResult::Unsupported;
        std::string error;
        std::shared_ptr<const compiled_pattern_set> compiled =
            skein_compile_parameter_patterns(code, code->module, error);
        if (!compiled) {
            std::shared_ptr<compiled_pattern_set> failed =
                std::make_shared<compiled_pattern_set>();
            failed->failed = true;
            cache.param_patterns = failed;
            if (read_bool_env("SKEIN_VM_DEBUG", false) && !error.empty()) {
                std::fprintf(stderr,
                             "[vm] fallback parameter pattern %s: %s\n",
                             code->name.c_str(), error.c_str());
            }
            return ParamPatternResult::Unsupported;
        }
        cache.param_patterns = compiled;
        if (read_bool_env("SKEIN_VM_DEBUG", false)) {
            std::fprintf(stderr,
                         "[vm] compiled parameter patterns for %s\n",
                         code->name.empty() ? "<anonymous>"
                                            : code->name.c_str());
        }
        if (!keep_function_ast()) {
            for (struct symlist *param = code->sl; param;
                 param = param->next) {
                if (symlist_pattern_ast(param))
                    set_symlist_pattern_ast(param, NULL);
            }
        }
    }

    std::shared_ptr<const compiled_pattern_set> compiled =
        cache.param_patterns;
    if (compiled->failed || !compiled->proto ||
        (size_t)index >= compiled->roots.size())
        return ParamPatternResult::Unsupported;

    uint32_t root = compiled->roots[(size_t)index];
    if (root == UINT32_MAX)
        return ParamPatternResult::Unsupported;

    bool matched = skein_vm_match_pattern_proto(
        *compiled->proto, root, value, bindings);
    return matched ? ParamPatternResult::Matched
                   : ParamPatternResult::NotMatched;
}

bool skein_vm_try_execute_function_body(
    const std::shared_ptr<function_def> &def, struct value_t *&out) {
    ensure_vm_host();
    out = NULL;
    if (!def || !def->code)
        return false;
    if (!skein_vm_enabled())
        return false;

    function_code *code = def->code.get();
    function_vm_cache *cached = cache_for(code);
    if (!cached->proto && !skein_vm_enabled())
        return false;

    skein::bc::ProtoPtr proto = prepare_function_body(def);
    if (!proto)
        return false;

    eval_context &context = active_eval_context();
    int limit = skein_vm_max_depth();
    if (context.vm_call_depth >= limit) {
        yyerror("maximum recursion depth (%d) exceeded", limit);
        out = NULL;
        return true;
    }

    bridge_depth_guard guard(context);
    if (vm_stats.enabled &&
        (uint64_t)context.vm_call_depth > vm_stats.max_depth)
        vm_stats.max_depth = context.vm_call_depth;
    out = skein_vm_execute_proto(*proto, def);
    return true;
}
