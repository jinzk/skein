#include "lsp_tool.h"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "lsp/lsp_server.h"
#include "typecheck.h"

namespace {

void print_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s lsp --stdio [--typecheck=off|loose|warn|strict] "
                 "[-I dir] [--encoding name]\n",
                 program.c_str());
}

bool parse_mode(const std::string &program, const std::string &value,
                skein::typecheck::Mode &mode) {
    if (!skein::typecheck::parse_mode(value.c_str(), mode)) {
        std::fprintf(stderr, "%s: invalid --typecheck value '%s'\n",
                     program.c_str(), value.c_str());
        return false;
    }
    return true;
}

} // namespace

int run_lsp_tool(const std::vector<std::string> &args,
                 const std::string &program) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    skein::lsp::lsp_options options;
    bool stdio_mode = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "--stdio" || arg == "--server") {
            stdio_mode = true;
        } else if (arg == "-I" || arg == "--package-path") {
            if (i + 1 >= args.size()) {
                print_usage(program);
                return 2;
            }
            options.package_dirs.push_back(args[++i]);
        } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            options.package_dirs.push_back(arg.substr(2));
        } else if (arg.rfind("--package-path=", 0) == 0) {
            options.package_dirs.push_back(arg.substr(15));
        } else if (arg == "--encoding") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                print_usage(program);
                return 2;
            }
            options.encoding = args[++i];
        } else if (arg.rfind("--encoding=", 0) == 0) {
            options.encoding = arg.substr(11);
            if (options.encoding.empty()) {
                print_usage(program);
                return 2;
            }
        } else if (arg == "--typecheck") {
            if (i + 1 >= args.size() ||
                !parse_mode(program, args[++i], options.typecheck_mode)) {
                print_usage(program);
                return 2;
            }
        } else if (arg.rfind("--typecheck=", 0) == 0) {
            if (!parse_mode(program, arg.substr(12),
                            options.typecheck_mode))
                return 2;
        } else {
            print_usage(program);
            return 2;
        }
    }

    /* Stdio is the only transport in this release; accepting an omitted flag
     * keeps editor snippets that launch `skein lsp` working. */
    (void)stdio_mode;
    return skein::lsp::run_lsp_server(options, stdin, stdout);
}
