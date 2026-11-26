#include "format_tool.h"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "formatter.h"
#include "platform.h"
#include "source_encoding.h"
#include "tool_analysis.h"
#include "typecheck.h"

namespace {

struct format_options {
    std::vector<std::string> files;
    std::vector<std::string> package_dirs;
    std::string encoding;
    bool check = false;
    bool write = false;
};

void print_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s fmt [--check] [-w|--write] [--encoding name] "
                 "[-I dir] <file...|->\n",
                 program.c_str());
}

bool parse_options(const std::vector<std::string> &args,
                   const std::string &program, format_options &options) {
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "--check") {
            options.check = true;
        } else if (arg == "-w" || arg == "--write") {
            options.write = true;
        } else if (arg == "-I" || arg == "--package-path") {
            if (i + 1 >= args.size()) {
                print_usage(program);
                return false;
            }
            options.package_dirs.push_back(args[++i]);
        } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            options.package_dirs.push_back(arg.substr(2));
        } else if (arg.rfind("--package-path=", 0) == 0) {
            options.package_dirs.push_back(arg.substr(15));
        } else if (arg == "--encoding") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                print_usage(program);
                return false;
            }
            options.encoding = args[++i];
        } else if (arg.rfind("--encoding=", 0) == 0) {
            options.encoding = arg.substr(11);
            if (options.encoding.empty()) {
                print_usage(program);
                return false;
            }
        } else if (arg == "-" || (!arg.empty() && arg[0] != '-')) {
            options.files.push_back(arg);
        } else {
            print_usage(program);
            return false;
        }
    }

    if (options.files.empty() || (options.check && options.write)) {
        print_usage(program);
        return false;
    }
    if (!options.check && !options.write && options.files.size() != 1) {
        std::fprintf(stderr,
                     "%s: fmt stdout mode accepts exactly one file; "
                     "use --check or -w for multiple files\n",
                     program.c_str());
        return false;
    }
    for (const std::string &file : options.files) {
        if (file == "-" && options.write) {
            std::fprintf(stderr,
                         "%s: fmt -w cannot be used with stdin\n",
                         program.c_str());
            return false;
        }
    }
    return true;
}

bool read_source(const std::string &path, const std::string &encoding,
                 std::string &source, std::string &error) {
    if (path == "-") {
        char buffer[8192];
        while (true) {
            std::size_t count = std::fread(buffer, 1, sizeof(buffer), stdin);
            source.append(buffer, count);
            if (count < sizeof(buffer))
                return !std::ferror(stdin);
        }
    }
    return load_source_file(path, encoding, source, error);
}

bool validate_source(const std::string &formatted, const std::string &path,
                     const format_options &options, std::string &error) {
    analysis_options analysis;
    analysis.source_name = path;
    analysis.encoding = options.encoding;
    analysis.package_dirs = options.package_dirs;
    analysis.typecheck_mode = skein::typecheck::Mode::Off;
    analysis.keep_going = true;
    analysis.suppress_text = true;
    analysis.resolve_macros = false;

    analysis_result result = analyze_source_text(formatted, analysis);
    if (result.error_count == 0)
        return true;
    if (result.error_count == 2) {
        error = result.diagnostics.empty() ? "cannot prepare syntax validation"
                                           : result.diagnostics.front().message;
        return false;
    }
    if (!result.diagnostics.empty()) {
        const diagnostic_record &record = result.diagnostics.front();
        error = std::to_string(record.line) + ":" +
                std::to_string(record.column) + ": " + record.category + ": " +
                record.message;
    } else {
        error = "formatted source failed syntax validation";
    }
    return false;
}

} // namespace

int run_format_tool(const std::vector<std::string> &args,
                    const std::string &program) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    format_options options;
    if (!parse_options(args, program, options))
        return 2;

    bool changed = false;
    for (const std::string &path : options.files) {
        std::string source;
        std::string error;
        if (!read_source(path, options.encoding, source, error)) {
            std::fprintf(stderr, "skein fmt: %s: %s\n", path.c_str(),
                         error.c_str());
            return 2;
        }

        std::string formatted;
        if (!skein::formatter::format_source(source, formatted, error)) {
            std::fprintf(stderr, "skein fmt: %s: %s\n", path.c_str(),
                         error.c_str());
            return 1;
        }

        if (!validate_source(formatted, path, options, error)) {
            std::fprintf(stderr, "skein fmt: %s: formatted output is invalid: "
                                 "%s\n",
                         path.c_str(), error.c_str());
            return 1;
        }

        if (options.check) {
            if (formatted != source)
                changed = true;
        } else if (options.write) {
            if (formatted != source &&
                !platform_write_file(path, formatted, false, error)) {
                std::fprintf(stderr, "skein fmt: %s: %s\n", path.c_str(),
                             error.c_str());
                return 2;
            }
        } else {
            std::fwrite(formatted.data(), 1, formatted.size(), stdout);
            std::fflush(stdout);
        }
    }
    return changed ? 1 : 0;
}
