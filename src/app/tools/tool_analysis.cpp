#include "tool_analysis.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "analysis_mode.h"
#include "ast.h"
#include "diag_collect.h"
#include "diagnostics.h"
#include "eval_context.h"
#include "event_loop.h"
#include "fiber.h"
#include "http.h"
#ifdef SKEIN_ENABLE_HTTP2
#include "http2.h"
#endif
#include "net.h"
#include "package.h"
#include "platform.h"
#include "process_spawn.h"
#include "repl.h"
#include "runtime_state.h"
#include "scopestack.h"
#include "source_encoding.h"
#include "static_type.h"
#include "structtype.h"
#include "symbol.h"
#include "task.h"
#include "typecheck.h"
#include "typecheck_state.h"
#include "vm.h"

extern FILE *yyin;
extern int yyparse();

namespace {

void collect_scope_symbols(scope_t *scope,
                           std::unordered_set<std::string> &seen,
                           std::vector<analysis_symbol> &symbols) {
    if (!scope)
        return;
    for (struct symbol *sym : scope->syms.small) {
        if (!sym || !sym->name || !sym->name[0])
            continue;
        const std::string name(sym->name);
        if (!seen.insert(name).second)
            continue;
        analysis_symbol info;
        info.name = name;
        if (sym->type.display)
            info.type = sym->type.display;
        info.callable = sym->val &&
                        (sym->val->type == T_func ||
                         sym->val->type == T_builtin ||
                         sym->val->type == T_method ||
                         sym->val->type == T_native);
        symbols.push_back(std::move(info));
    }
}

void collect_typecheck_symbols(const std::string &source,
                               std::unordered_set<std::string> &seen,
                               std::vector<analysis_symbol> &symbols) {
    const auto it =
        skein::typecheck::detail::g_persistent_bindings.find(source);
    if (it == skein::typecheck::detail::g_persistent_bindings.end())
        return;
    for (const auto &entry : it->second) {
        if (entry.first.empty() || !seen.insert(entry.first).second)
            continue;
        analysis_symbol info;
        info.name = entry.first;
        const skein::typecheck::TypeNode *type = entry.second.type;
        if (type) {
            info.type = type->display;
            info.callable = skein::typecheck::type_is_function_signature(type);
        }
        symbols.push_back(std::move(info));
    }
}

void collect_visible_symbols(const std::string &source, scope_context &scopes,
                             std::vector<analysis_symbol> &symbols) {
    std::unordered_set<std::string> seen;
    for (const auto &weak : scopes.all_scopes) {
        if (std::shared_ptr<scope_t> scope = weak.lock())
            collect_scope_symbols(scope.get(), seen, symbols);
    }
    collect_scope_symbols(scopes.current.get(), seen, symbols);
    collect_typecheck_symbols(source, seen, symbols);
}

void shutdown_analysis_runtime() {
    task_shutdown();
    fiber_shutdown();
    http_shutdown();
#ifdef SKEIN_ENABLE_HTTP2
    http2_shutdown();
#endif
    net_shutdown();
    donescope();
    ast_thread_shutdown();
    process_spawn_shutdown();
    skein_vm_shutdown();
    structtype_shutdown();
    event_loop_shutdown();
    process_spawn_finish();
}

} // namespace

analysis_result analyze_source_text(const std::string &source,
                                    const analysis_options &options) {
    analysis_result result;

    std::string executable = platform_executable_path();
    if (executable.empty())
        executable = "skein";
    sys_invocation_set({}, options.source_name, executable);

    source_encoding_set(options.encoding);
    package_configure(options.source_name, options.package_dirs);
    skein::typecheck::set_mode(options.typecheck_mode);

    g_interactive = false;
    g_show_prompt = false;
    g_keep_going = options.keep_going;
    g_script_stop = false;
    g_statement_invalid = false;

    std::string source_error;
    FILE *script = source_text_file(source, source_error);
    if (!script) {
        result.error_count = 2;
        result.diagnostics.push_back(diagnostic_record{});
        result.diagnostics.back().source = options.source_name;
        result.diagnostics.back().severity = DiagnosticSeverity::Error;
        result.diagnostics.back().code = "IO0001";
        result.diagnostics.back().category = "source error";
        result.diagnostics.back().message = source_error;
        return result;
    }
    yyin = script;

    diag_reset_errors();
    diag_start_source();
    diag_set_source_name(options.source_name);
    diag_set_package_name("main");
    diag_set_module_name("");
    skein::typecheck::forget_source(options.source_name);

    eval_context context;
    set_active_eval_context(&context);
    initscope();
    reset_scanner_state(yyin);

    diag_collect_begin(options.suppress_text);
    {
        check_only_guard check_only;
        yyparse();
    }
    result.diagnostics = diag_collect_end();
    result.error_count = diag_error_count() > 0 ? 1 : 0;
    collect_visible_symbols(options.source_name, context.scopes,
                            result.symbols);

    shutdown_analysis_runtime();
    set_active_eval_context(NULL);
    drain_finish();
    std::fclose(script);
    return result;
}
