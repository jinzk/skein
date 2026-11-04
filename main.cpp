#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "parser.hpp"
#include "builtins.h"
#include "diagnostics.h"
#include "error_report.h"
#include "eval.h"
#include "eval_context.h"
#include "eval_helpers.h"
#include "event_loop.h"
#include "fiber.h"
#include "http.h"
#include "http2.h"
#include "net.h"
#include "package.h"
#include "platform.h"
#include "process_spawn.h"
#include "repl.h"
#include "runtime_context.h"
#include "scopestack.h"
#include "source_encoding.h"
#include "structtype.h"
#include "task.h"

#include <utility>

extern FILE *yyin;
extern int indent_count;

static struct value_t *runtime_call(struct value_t *callee,
                                    std::vector<struct value_t *> args) {
    return call_value_args(callee, std::move(args));
}

static struct value_t *runtime_call_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    return call_builtin(functype, args);
}

static void print_usage(const char *program) {
    std::fprintf(stderr,
                 "usage: %s [-i] [-I dir] [--package-path dir] "
                 "[--encoding name] [-k|--keep-going] [script|-]\n",
                 program);
}

int main(int argc, char **argv) {
    platform_init();
    platform_install_interrupt_handler();

    struct skein_context runtime_hooks = {runtime_call,
                                          runtime_call_builtin,
                                          runtime_error_report};
    skein_context_set(&runtime_hooks);

    std::vector<std::string> args = platform_command_line_args(argc, argv);
    const char *program =
        args.empty() || args[0].empty() ? "skein" : args[0].c_str();

    std::string script_path;
    std::vector<std::string> package_dirs;
    std::string source_encoding;
    bool force_interactive = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "-I" || arg == "--package-path") {
            if (i + 1 >= args.size()) {
                print_usage(program);
                return 2;
            }
            package_dirs.push_back(args[++i]);
        } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            package_dirs.push_back(arg.substr(2));
        } else if (arg.rfind("--package-path=", 0) == 0) {
            package_dirs.push_back(arg.substr(15));
        } else if (arg == "--encoding") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                print_usage(program);
                return 2;
            }
            source_encoding = args[++i];
        } else if (arg.rfind("--encoding=", 0) == 0) {
            source_encoding = arg.substr(11);
            if (source_encoding.empty()) {
                print_usage(program);
                return 2;
            }
        } else if (arg == "-i") {
            force_interactive = true;
        } else if (arg == "-k" || arg == "--keep-going") {
            g_keep_going = true;
        } else if (arg == "-" || (!arg.empty() && arg[0] != '-')) {
            if (!script_path.empty()) {
                print_usage(program);
                return 2;
            }
            script_path = arg;
        } else {
            print_usage(program);
            return 2;
        }
    }

    source_encoding_set(source_encoding);
    package_configure(script_path, package_dirs);
    diag_set_module_name("main");
    if (!script_path.empty() && script_path != "-")
        diag_set_source_name(script_path);

    FILE *script = NULL;
    if (!script_path.empty()) {
        g_interactive = false;
    } else {
        g_show_prompt = force_interactive || platform_stdin_is_tty();
    }
    if (!script_path.empty()) {
        if (script_path != "-") {
            std::string source_text;
            std::string source_error;
            if (!load_source_file(script_path, source_encoding_get(),
                                  source_text, source_error)) {
                std::fprintf(stderr, "%s: %s\n", script_path.c_str(),
                             source_error.c_str());
                return 2;
            }
            script = source_text_file(source_text, source_error);
            if (!script) {
                std::fprintf(stderr, "%s: %s\n", script_path.c_str(),
                             source_error.c_str());
                return 2;
            }
            yyin = script;
        }
    }

    eval_context context;
    context.allow_await = true;
    set_active_eval_context(&context);
    eval_begin_root_defers();
    initscope();
    if (g_show_prompt)
        repl_history_load();

    int status = 0;
    for (;;) {
        g_interrupted = 0;
        status = yyparse();

        if (!g_interrupted || g_stdin_eof)
            break;
        if (!g_interactive)
            break;

        /* Ctrl-C: drop the current line/block and start a fresh statement. */
        g_interrupted = 0;
        indent_count = 0;
        reset_scanner_state(yyin ? yyin : stdin);
    }

    if (g_show_prompt)
        repl_history_save();
    eval_run_root_defers();
    fiber_disable_generator_warnings();
    task_shutdown();
    fiber_shutdown();
    http_shutdown();
#ifdef SKEIN_ENABLE_HTTP2
    http2_shutdown();
#endif
    net_shutdown();
    donescope();
    process_spawn_shutdown();
    structtype_shutdown();
    event_loop_shutdown();
    process_spawn_finish();
    set_active_eval_context(NULL);
    if (script)
        std::fclose(script);

    if (g_interactive) {
        status = 0;
    } else if (g_interrupted) {
        status = 130;
    } else if (diag_error_count() > 0) {
        status = 1;
    } else if (status != 0) {
        status = 1;
    }
    return status;
}
