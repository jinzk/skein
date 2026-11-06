#include "vm.h"
#include "vm_internal.h"

#include "ast.h"
#include "compiler.h"
#include "diagnostics.h"
#include "eval.h"
#include "eval_context.h"
#include "eval_internal.h"
#include "pattern.h"
#include "symbol.h"
#include "valuetype.h"
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
#include <unordered_map>
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

std::unordered_map<function_code *, default_cache_entry> default_cache;
/*
 * Registry used only for lifecycle management.  The hot path reaches the
 * cache through function_code::vm_cache, so no hash lookup happens per call.
 */
std::unordered_map<function_code *, std::unique_ptr<function_vm_cache>>
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
bool jit_call_eligible(function_code *code, value_t **args, uint32_t argc) {
    if (!code || code->is_generator || code->is_async || code->is_method ||
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
bool skein_vm_try_jit_call(value_t *callee, value_t **args, uint32_t argc,
                           value_t **out) {
    *out = nullptr;
    if (!skein::jit::available())
        return false;
    if (!callee || callee->type != T_func)
        return false;

    struct func_value_t *fv = (struct func_value_t *)callee;
    if (!fv->def || !fv->def->code)
        return false;
    function_code *code = fv->def->code.get();
    if (!jit_call_eligible(code, args, argc))
        return false;

    function_vm_cache *cache = cache_for(code);
    if (cache->jit_failed)
        return false;
    if (!cache->jit) {
        if (!skein_vm_enabled())
            return false;
        if (cache->jit_hits++ < jit_threshold())
            return false;
        skein::bc::ProtoPtr proto = prepare_function_body(fv->def);
        if (!proto || !skein::jit::can_compile(*proto)) {
            cache->jit_failed = true;
            return false;
        }
        cache->jit = skein::jit::compile(*proto);
        if (!cache->jit) {
            cache->jit_failed = true;
            return false;
        }
    }

    skein::jit::thread_state ts;
    ts.ctx = &active_eval_context();
    ts.max_depth = skein_vm_max_depth();
    *out = skein::jit::enter(cache->jit, &ts, fv->def.get(), args,
                             (uint32_t)argc);
    return true;
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
    if (skein_vm_try_jit_call(callee, args, argc, &out)) {
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
        skein::jit::set_construct(&skein_vm_build_struct);
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

static bool cache_clearing = false;

void skein_vm_forget_function_code(function_code *code) {
    if (!code || cache_clearing)
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
    default_cache.clear();
    for (auto &entry : vm_caches) {
        release_vm_cache(entry.second.get());
    }
    vm_caches.clear();
    cache_clearing = false;
}

struct value_t *skein_vm_eval_statement(struct ast *statement) {
    ensure_vm_host();
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
