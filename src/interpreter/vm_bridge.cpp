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

struct function_execution_cache {
    skein::bc::ProtoPtr proto;
    std::shared_ptr<const compiled_pattern_set> param_patterns;
    bool compile_failed = false;
};

std::unordered_map<function_code *, default_cache_entry> default_cache;
std::unordered_map<function_code *, function_execution_cache> execution_cache;

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
    function_execution_cache &cache = execution_cache[code];
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

} // namespace

void skein_vm_eval_capture_begin(struct eval_capture_snapshot &snapshot) {
    eval_capture_begin(snapshot);
}

struct eval_capture_result skein_vm_eval_capture_end(
    const struct eval_capture_snapshot &snapshot) {
    return eval_capture_end(snapshot);
}

void skein_vm_eval_raise(struct value_t *error) {
    eval_raise(error);
}

void skein_vm_eval_pushscope() {
    eval_pushscope();
}

void skein_vm_eval_push_isolated_scope() {
    eval_push_isolated_scope();
}

void skein_vm_eval_push_closure_scope(
    const std::shared_ptr<struct scope_t> &parent) {
    eval_push_closure_scope(parent);
}

void skein_vm_eval_popscope() {
    eval_popscope();
}

struct value_t *skein_vm_call_value_args(
    struct value_t *callee, std::vector<struct value_t *> args) {
    return call_value_args(callee, std::move(args));
}

struct value_t *skein_vm_read_member_value(
    struct value_t *container, struct value_t *index, bool field_access) {
    return read_member_value(container, index, field_access);
}

struct value_t *skein_vm_assign_symbol_value(
    const char *name, struct value_t *value) {
    return assign_symbol_value(name, value);
}

struct value_t *skein_vm_compound_symbol_value(
    const char *name, int op, struct value_t *value) {
    return compound_symbol_value(name, op, value);
}

struct value_t *skein_vm_increment_symbol_value(
    const char *name, int delta, bool prefix) {
    return increment_symbol_value(name, delta, prefix);
}

struct value_t *skein_vm_assign_symbol_direct(
    struct symbol *sym, struct value_t *value) {
    return assign_symbol_direct(sym, value);
}

struct value_t *skein_vm_compound_symbol_direct(
    struct symbol *sym, int op, struct value_t *value) {
    return compound_symbol_direct(sym, op, value);
}

struct value_t *skein_vm_increment_symbol_direct(
    struct symbol *sym, int delta, bool prefix) {
    return increment_symbol_direct(sym, delta, prefix);
}

struct value_t *skein_vm_assign_index_symbol(
    struct symbol *base, const char *base_name,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields, struct value_t *value) {
    return assign_index_symbol(base, base_name, indices, fields, value);
}

struct value_t *skein_vm_compound_index_symbol(
    struct symbol *base, const char *base_name, int op,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields, struct value_t *value) {
    return compound_index_symbol(base, base_name, op, indices, fields,
                                 value);
}

struct value_t *skein_vm_increment_index_symbol(
    struct symbol *base, const char *base_name, int delta, bool prefix,
    std::vector<struct value_t *> &indices,
    const std::vector<bool> &fields) {
    return increment_index_symbol(base, base_name, delta, prefix, indices,
                                  fields);
}

struct value_t *skein_vm_default_value_for_type(
    const struct type_constraint *type) {
    return default_value_for_type(type);
}

struct value_t *skein_vm_take_eval_control(enum eval_control_kind kind) {
    return take_eval_control(kind);
}

bool skein_vm_switch_value_supported(const struct value_t *value) {
    return switch_value_supported(value);
}

struct value_t *skein_vm_do_compare(
    int op, struct value_t *left, struct value_t *right) {
    return do_compare(op, left, right);
}

struct value_t *skein_vm_apply_binary_value(
    int op, struct value_t *left, struct value_t *right) {
    return apply_binary_value(op, left, right);
}

const char *skein_vm_import_binding_name(const char *path) {
    return import_binding_name(path);
}

bool skein_vm_validate_function_params(
    const char *name, struct symlist *params) {
    return validate_function_params(name, params);
}

skein::bc::ProtoPtr skein_vm_prepare_function_body(
    const std::shared_ptr<function_def> &def) {
    return prepare_function_body(def);
}

static bool cache_clearing = false;

void skein_vm_forget_function_code(function_code *code) {
    if (!code || cache_clearing)
        return;
    default_cache.erase(code);
    execution_cache.erase(code);
}

void skein_vm_shutdown() {
    if (cache_clearing)
        return;
    cache_clearing = true;
    default_cache.clear();
    execution_cache.clear();
    cache_clearing = false;
}

struct value_t *skein_vm_eval_statement(struct ast *statement) {
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
    if (!def || !def->code || index < 0 || !value)
        return ParamPatternResult::Unsupported;

    function_code *code = def->code.get();
    function_execution_cache &cache = execution_cache[code];
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
    out = NULL;
    if (!def || !def->code)
        return false;

    function_code *code = def->code.get();
    auto cached = execution_cache.find(code);
    if ((cached == execution_cache.end() || !cached->second.proto) &&
        !skein_vm_enabled())
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
