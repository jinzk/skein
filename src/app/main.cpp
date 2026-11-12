#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "parser.hpp"
#include "builtins.h"
#include "ffi_bindgen.h"
#ifdef SKEIN_ENABLE_FFI
#include "ffi_check.h"
#endif
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
#include "runtime_state.h"
#include "scopestack.h"
#include "source_encoding.h"
#include "structtype.h"
#include "task.h"
#include "thread_channel.h"
#include "typecheck.h"
#include "vm.h"

#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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
                 "usage: %s [-i] [--vm|--no-vm] [--vm-stats] [-I dir] "
                 "[--package-path dir] [--encoding name] "
                 "[--typecheck=off|loose|warn|strict] "
                 "[-k|--keep-going] [script|-]\n"
                 "       the bytecode VM is enabled by default; "
                 "--no-vm selects the AST backend\n",
                 program);
}

int main(int argc, char **argv) {
    platform_init();
    platform_install_interrupt_handler();

    skein::typecheck::Mode typecheck_mode = skein::typecheck::Mode::Warn;
    if (const char *env_mode = std::getenv("SKEIN_TYPECHECK")) {
        skein::typecheck::Mode parsed;
        if (skein::typecheck::parse_mode(env_mode, parsed))
            typecheck_mode = parsed;
        else
            std::fprintf(stderr,
                         "skein: invalid SKEIN_TYPECHECK value '%s'; "
                         "using warn\n",
                         env_mode);
    }
    skein::typecheck::set_mode(typecheck_mode);

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--check-ffi") == 0) {
#ifdef SKEIN_ENABLE_FFI
            if (i + 2 >= argc) {
                std::fprintf(stderr,
                             "usage: skein --check-ffi <library> <manifest>\n");
                return 2;
            }
            std::string check_error;
            int check_code =
                ffi_check::run_check(argv[i + 1], argv[i + 2], check_error);
            if (check_code != 0 && !check_error.empty())
                std::fprintf(stderr, "skein: %s\n", check_error.c_str());
            return check_code;
#else
            std::fprintf(stderr, "skein: built without FFI support\n");
            return 2;
#endif
        }
        if (std::strcmp(argv[i], "--ffi-gen") == 0) {
            std::vector<std::string> gen_args;
            for (int j = i + 1; j < argc; ++j)
                gen_args.emplace_back(argv[j]);
            std::string gen_error;
            int code = skein::run_ffi_gen(gen_args, gen_error);
            if (code != 0 && !gen_error.empty())
                std::fprintf(stderr, "skein: %s\n", gen_error.c_str());
            return code;
        }
    }

    struct skein_context runtime_hooks = {runtime_call,
                                          runtime_call_builtin,
                                          runtime_error_report};
    skein_context_set(&runtime_hooks);

    std::string module_error;
    if (!builtins_register_modules(module_error)) {
        std::fprintf(stderr, "skein: %s\n", module_error.c_str());
        return 1;
    }

    std::vector<std::string> args = platform_command_line_args(argc, argv);
    const char *program =
        args.empty() || args[0].empty() ? "skein" : args[0].c_str();

    std::string script_path;
    std::vector<std::string> package_dirs;
    std::vector<std::string> script_args;
    std::string source_encoding;
    bool force_interactive = false;
    bool run_entry = false;

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
        } else if (arg == "--typecheck") {
            if (i + 1 >= args.size()) {
                print_usage(program);
                return 2;
            }
            skein::typecheck::Mode parsed;
            if (!skein::typecheck::parse_mode(args[++i].c_str(), parsed)) {
                std::fprintf(stderr,
                             "%s: invalid --typecheck value '%s'\n",
                             program, args[i].c_str());
                return 2;
            }
            typecheck_mode = parsed;
            skein::typecheck::set_mode(typecheck_mode);
        } else if (arg.rfind("--typecheck=", 0) == 0) {
            std::string value = arg.substr(12);
            skein::typecheck::Mode parsed;
            if (!skein::typecheck::parse_mode(value.c_str(), parsed)) {
                std::fprintf(stderr,
                             "%s: invalid --typecheck value '%s'\n",
                             program, value.c_str());
                return 2;
            }
            typecheck_mode = parsed;
            skein::typecheck::set_mode(typecheck_mode);
        } else if (arg == "--vm") {
            skein_vm_set_enabled(true);
        } else if (arg == "--no-vm") {
#ifdef SKEIN_VM_ONLY
            std::fprintf(stderr,
                         "%s: --no-vm is unavailable in VM-only builds\n",
                         program);
            return 2;
#else
            skein_vm_set_enabled(false);
#endif
        } else if (arg == "--vm-stats") {
            skein_vm_set_stats(true);
        } else if (arg == "-k" || arg == "--keep-going") {
            g_keep_going = true;
        } else if (arg == "--run-entry") {
            run_entry = true;
        } else if (arg == "-" || (!arg.empty() && arg[0] != '-')) {
            if (!script_path.empty()) {
                print_usage(program);
                return 2;
            }
            script_path = arg;
            /* Everything after the script path belongs to the script. */
            for (size_t j = i + 1; j < args.size(); ++j)
                script_args.push_back(args[j]);
            break;
        } else {
            print_usage(program);
            return 2;
        }
    }

    std::string executable = platform_executable_path();
    if (executable.empty())
        executable = program;
    sys_invocation_set(std::move(script_args), script_path,
                       std::move(executable));

    source_encoding_set(source_encoding);
    if (run_entry) {
        if (!script_path.empty()) {
            print_usage(program);
            return 2;
        }
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::string request;
        char buffer[65536];
        for (;;) {
            std::size_t count = std::fread(buffer, 1, sizeof(buffer), stdin);
            if (count == 0)
                break;
            request.append(buffer, count);
        }
        std::string result;
        std::string error;
        if (!thread_run_entry_request(request, result, error)) {
            std::fprintf(stderr, "skein: %s\n", error.c_str());
            return 1;
        }
        std::fwrite(result.data(), 1, result.size(), stdout);
        std::fflush(stdout);
        return 0;
    }
    package_configure(script_path, package_dirs);
    diag_set_package_name("main");
    diag_set_module_name("");
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
    ast_thread_shutdown();
    process_spawn_shutdown();
    skein_vm_shutdown();
    structtype_shutdown();
    event_loop_shutdown();
    process_spawn_finish();
    set_active_eval_context(NULL);
    if (script)
        std::fclose(script);

    skein_vm_stats_report();

    if (sys_exit_requested()) {
        status = sys_exit_code();
    } else if (g_interactive) {
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
