#include "build_tool.h"

#include "file_lock.h"
#include "modfile.h"
#include "native_artifact.h"
#include "package.h"
#include "path.h"
#include "platform.h"
#include "runtime_state.h"
#include "sha256.h"

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct build_options {
    std::string directory;
    std::string cache;
};

void print_build_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s build [-C dir] [--cache dir] "
                 "[<module/path>[@version] ...]\n",
                 program.c_str());
}

std::string shell_quote(const std::string &value) {
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '\\')
            quoted += '/';
        else if (c == '"')
            quoted += "\\\"";
        else
            quoted += c;
    }
    quoted += '"';
    return quoted;
}

void replace_all(std::string &text, const std::string &from,
                 const std::string &to) {
    if (from.empty())
        return;
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string slash_path(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

bool parse_options(const std::vector<std::string> &args, size_t start,
                   const std::string &program, build_options &options,
                   std::vector<std::string> &positional) {
    for (size_t i = start; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "-C" || arg == "--project") {
            if (i + 1 >= args.size()) {
                print_build_usage(program);
                return false;
            }
            options.directory = args[++i];
        } else if (arg.rfind("--project=", 0) == 0) {
            options.directory = arg.substr(10);
        } else if (arg == "--cache") {
            if (i + 1 >= args.size()) {
                print_build_usage(program);
                return false;
            }
            options.cache = args[++i];
        } else if (arg.rfind("--cache=", 0) == 0) {
            options.cache = arg.substr(8);
        } else if (!arg.empty() && arg[0] == '-') {
            print_build_usage(program);
            return false;
        } else {
            positional.push_back(arg);
        }
    }
    return true;
}

bool safe_relative(const std::string &value) {
    if (value.empty())
        return false;
    fs::path path = skein::path_from_utf8(value);
    if (path.is_absolute())
        return false;
    for (const fs::path &part : path)
        if (part == "..")
            return false;
    return true;
}

bool safe_artifact_name(const std::string &value) {
    return value.empty() ||
           (value != "." && value != ".." &&
            value.find('/') == std::string::npos &&
            value.find('\\') == std::string::npos &&
            value.find(':') == std::string::npos);
}

bool build_one(const std::string &root, const std::string &path,
               const std::string &version, std::string &error) {
    file_lock lock;
    if (!acquire_module_lock(path, version, lock, lock_mode::exclusive, error))
        return false;
    if (!skein::modfile::verify_cached_module(root, path, version, error))
        return false;

    module_native_spec spec;
    if (!load_cached_module_native(path, version, spec, error))
        return false;
    if (!spec.present)
        return true;
    if (spec.sources.empty()) {
        error = "native build has no sources: " + path + "@" + version;
        return false;
    }
    if (!safe_artifact_name(spec.output)) {
        error = "invalid native output name: " + spec.output;
        return false;
    }

    const fs::path source_root = skein::path_from_utf8(
        skein::modfile::cache_path(path, version));
    const fs::path destination = skein::path_from_utf8(
        skein::modfile::native_build_dir(path, version));
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        error = "cannot create native build directory: " +
                skein::path_to_utf8(destination);
        return false;
    }

    const std::string artifact = skein::path_to_utf8(
        destination / skein::modfile::native_library_name(spec.output));
    const std::string compiler = native_compiler_from_env();
    std::string compiler_version;
    bool compiler_available = false;
    std::string version_error;
    native_compiler_version(compiler, compiler_version, compiler_available,
                            version_error);
    if (!compiler_available)
        compiler_version = "unknown";
    const fs::path compiler_path = skein::path_from_utf8(compiler);
    const std::string compiler_name =
        skein::path_to_utf8(compiler_path.filename());
    const bool msvc = compiler_name == "cl" || compiler_name == "cl.exe";

    std::string build_flags;
    if (!msvc) {
        build_flags = "-std=c++17 -O2 -shared -DSKEIN_MODULE_BUILD=1";
#ifndef _WIN32
        build_flags += " -fPIC";
#else
        build_flags += " -Wl,--export-all-symbols -Wl,--no-insert-timestamp";
#endif
        build_flags += " -ffile-prefix-map=" +
                       shell_quote(slash_path(skein::path_to_utf8(source_root)) + "=.");
    } else {
        build_flags =
            "/nologo /LD /EHsc /O2 /DSKEIN_MODULE_BUILD=1 /Brepro";
    }
    std::string command = shell_quote(compiler) + " " + build_flags;

    if (msvc) {
        command += " /I" + shell_quote(skein::path_to_utf8(source_root));
        for (const std::string &include : spec.includes) {
            if (!safe_relative(include)) {
                error = "invalid native include path: " + include;
                return false;
            }
            command += " /I" + shell_quote(skein::path_to_utf8(source_root / skein::path_from_utf8(include)));
        }
        for (const std::string &define : spec.defines)
            command += " /D" + shell_quote(define);
        command += " /Fe:" + shell_quote(artifact);
    } else {
        command += " -I" + shell_quote(skein::path_to_utf8(source_root));
        for (const std::string &include : spec.includes) {
            if (!safe_relative(include)) {
                error = "invalid native include path: " + include;
                return false;
            }
            command += " -I" + shell_quote(skein::path_to_utf8(source_root / skein::path_from_utf8(include)));
        }
        for (const std::string &define : spec.defines)
            command += " -D" + shell_quote(define);
        command += " -o " + shell_quote(artifact);
    }

    for (const std::string &source : spec.sources) {
        if (!safe_relative(source)) {
            error = "invalid native source path: " + source;
            return false;
        }
        command += " " + shell_quote(skein::path_to_utf8(source_root / skein::path_from_utf8(source)));
    }
    for (const std::string &lib : spec.libs)
        command += msvc ? " " + shell_quote(lib)
                        : (lib.empty() || lib[0] == '-' || lib[0] == '/'
                               ? " " + lib
                               : " -l" + shell_quote(lib));

    std::string canonical = slash_path(command);
    replace_all(canonical, slash_path(skein::path_to_utf8(source_root)), "<source>");
    replace_all(canonical, slash_path(skein::path_to_utf8(destination)),
                "<build>");
    replace_all(canonical, slash_path(artifact), "<artifact>");
    const std::string command_hash =
        skein_hex_encode(skein_sha256_bytes(canonical));

    int status = -1;
    const std::vector<std::pair<std::string, std::string>> build_env = {
        {"SOURCE_DATE_EPOCH", "0"},
    };
    if (!platform_run_command_status(command, build_env, status)) {
        error = "cannot run native compiler; set SKEIN_CXX to a C++ compiler";
        return false;
    }
    if (status != 0) {
        error = "native build failed for " + path + "@" + version;
        return false;
    }
    if (!fs::is_regular_file(skein::path_from_utf8(artifact), ec)) {
        error = "native compiler did not produce " + artifact;
        return false;
    }
    if (!write_native_artifact_metadata(
            path, version, compiler, compiler_version, build_flags,
            command_hash, artifact, error))
        return false;
    std::printf("built %s\n", artifact.c_str());
    return true;
}

} // namespace

int run_build_tool(const std::vector<std::string> &args,
                   const std::string &program) {
    build_options options;
    std::vector<std::string> positional;
    if (!parse_options(args, 2, program, options, positional))
        return 2;
    if (!options.cache.empty())
        thread_env_set("SKEINMODCACHE", options.cache);

    const std::string start = options.directory.empty()
                                  ? (thread_cwd().empty() ? "." : thread_cwd())
                                  : options.directory;
    const std::string root = skein::modfile::find_root(start);
    if (root.empty() || !skein::modfile::exists(root)) {
        std::fprintf(stderr,
                     "%s: no skein.mod found; run '%s mod init' first\n",
                     program.c_str(), program.c_str());
        return 1;
    }

    std::string error;
    file_lock project_guard;
    if (!acquire_project_lock(root, project_guard, lock_mode::shared, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    file_lock cache_guard;
    if (!acquire_cache_lock(cache_guard, lock_mode::shared, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    skein::modfile::mod_file mod;
    if (!skein::modfile::load(root, mod, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    std::vector<std::pair<std::string, std::string>> targets;
    if (positional.empty()) {
        for (const skein::modfile::require_spec &req : mod.requirements)
            targets.push_back({req.path, req.version});
    } else {
        for (const std::string &spec : positional) {
            std::string path = spec;
            std::string version;
            const size_t at = spec.rfind('@');
            if (at != std::string::npos) {
                path = spec.substr(0, at);
                version = spec.substr(at + 1);
                if (!skein::modfile::is_valid_module_path(path) ||
                    !skein::modfile::is_valid_version(version)) {
                    std::fprintf(stderr, "%s: invalid module spec '%s'\n",
                                 program.c_str(), spec.c_str());
                    return 2;
                }
            } else {
                if (!skein::modfile::is_valid_module_path(path)) {
                    std::fprintf(stderr, "%s: invalid module spec '%s'\n",
                                 program.c_str(), spec.c_str());
                    return 2;
                }
                bool found = false;
                for (const skein::modfile::require_spec &req : mod.requirements) {
                    if (req.path == path) {
                        version = req.version;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    std::fprintf(stderr,
                                 "%s: module not required: %s; run '%s get %s'\n",
                                 program.c_str(), path.c_str(), program.c_str(),
                                 path.c_str());
                    return 1;
                }
            }
            targets.push_back({path, version});
        }
    }

    for (const auto &target : targets) {
        if (!build_one(root, target.first, target.second, error)) {
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
    }
    return 0;
}
