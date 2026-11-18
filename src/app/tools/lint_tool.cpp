#include "lint_tool.h"

#include <cstdio>
#include <string>
#include <vector>

#include "source_encoding.h"
#include "tool_analysis.h"
#include "typecheck.h"

namespace {

void print_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s lint [--format=text|json] "
                 "[--typecheck=off|loose|warn|strict] "
                 "[-k|--keep-going] [--no-keep-going] "
                 "[-I dir] [--encoding name] <file|->\n",
                 program.c_str());
}

std::string json_escape(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

const char *severity_name(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Error: return "error";
        case DiagnosticSeverity::Warning: return "warning";
        case DiagnosticSeverity::Information: return "information";
        case DiagnosticSeverity::Hint: return "hint";
    }
    return "error";
}

void print_json(const std::vector<diagnostic_record> &records) {
    std::fputs("{\"diagnostics\":[", stdout);
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i)
            std::fputc(',', stdout);
        const diagnostic_record &d = records[i];
        std::fprintf(stdout,
                     "{\"file\":\"%s\",\"line\":%d,\"column\":%d,"
                     "\"byteOffset\":%zu,\"utf16Offset\":%zu,"
                     "\"severity\":\"%s\",\"code\":\"%s\","
                     "\"category\":\"%s\",\"message\":\"%s\"}",
                     json_escape(d.source).c_str(), d.line, d.column,
                     d.byte_offset, d.utf16_offset,
                     severity_name(d.severity),
                     json_escape(d.code).c_str(),
                     json_escape(d.category).c_str(),
                     json_escape(d.message).c_str());
    }
    std::fputs("]}\n", stdout);
    std::fflush(stdout);
}

struct lint_options {
    std::string source;
    std::string encoding;
    std::vector<std::string> package_dirs;
    skein::typecheck::Mode typecheck_mode = skein::typecheck::Mode::Warn;
    bool json = false;
    bool keep_going = true;
    bool have_source = false;
};

bool parse_mode_value(const std::string &program, const std::string &value,
                      skein::typecheck::Mode &mode) {
    if (!skein::typecheck::parse_mode(value.c_str(), mode)) {
        std::fprintf(stderr, "%s: invalid --typecheck value '%s'\n",
                     program.c_str(), value.c_str());
        return false;
    }
    return true;
}

bool parse_options(const std::vector<std::string> &args,
                   const std::string &program, lint_options &options) {
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "-I" || arg == "--package-path") {
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
        } else if (arg == "--typecheck") {
            if (i + 1 >= args.size() ||
                !parse_mode_value(program, args[++i],
                                  options.typecheck_mode)) {
                print_usage(program);
                return false;
            }
        } else if (arg.rfind("--typecheck=", 0) == 0) {
            if (!parse_mode_value(program, arg.substr(12),
                                  options.typecheck_mode))
                return false;
        } else if (arg == "--format") {
            if (i + 1 >= args.size()) {
                print_usage(program);
                return false;
            }
            const std::string value = args[++i];
            if (value == "json")
                options.json = true;
            else if (value == "text")
                options.json = false;
            else {
                std::fprintf(stderr, "%s: invalid --format value '%s'\n",
                             program.c_str(), value.c_str());
                return false;
            }
        } else if (arg.rfind("--format=", 0) == 0) {
            const std::string value = arg.substr(9);
            if (value == "json")
                options.json = true;
            else if (value == "text")
                options.json = false;
            else {
                std::fprintf(stderr, "%s: invalid --format value '%s'\n",
                             program.c_str(), value.c_str());
                return false;
            }
        } else if (arg == "-k" || arg == "--keep-going") {
            options.keep_going = true;
        } else if (arg == "--no-keep-going") {
            options.keep_going = false;
        } else if (arg == "-" || (!arg.empty() && arg[0] != '-')) {
            if (options.have_source) {
                print_usage(program);
                return false;
            }
            options.source = arg;
            options.have_source = true;
        } else {
            print_usage(program);
            return false;
        }
    }
    if (!options.have_source) {
        print_usage(program);
        return false;
    }
    return true;
}

bool read_source(const lint_options &options, std::string &source,
                 std::string &error) {
    if (options.source == "-") {
        char buffer[8192];
        while (true) {
            std::size_t count = std::fread(buffer, 1, sizeof(buffer), stdin);
            source.append(buffer, count);
            if (count < sizeof(buffer))
                return !std::ferror(stdin);
        }
    }
    return load_source_file(options.source, options.encoding, source, error);
}

} // namespace

int run_lint_tool(const std::vector<std::string> &args,
                  const std::string &program) {
    lint_options options;
    if (!parse_options(args, program, options))
        return 2;

    std::string source;
    std::string read_error;
    if (!read_source(options, source, read_error)) {
        std::fprintf(stderr, "%s: %s\n", options.source.c_str(),
                     read_error.c_str());
        return 2;
    }

    analysis_options analysis;
    analysis.source_name = options.source;
    analysis.encoding = options.encoding;
    analysis.package_dirs = options.package_dirs;
    analysis.typecheck_mode = options.typecheck_mode;
    analysis.keep_going = options.keep_going;
    analysis.suppress_text = options.json;

    analysis_result result = analyze_source_text(source, analysis);

    if (options.json)
        print_json(result.diagnostics);

    if (result.error_count == 2)
        return 2;
    return result.error_count > 0 ? 1 : 0;
}
