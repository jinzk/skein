#include "get_tool.h"

#include "modfile.h"
#include "platform.h"
#include "runtime_state.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

void print_get_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s get [-C dir] [--cache dir] "
                 "<module/path>[@version] ...\n",
                 program.c_str());
}

void print_mod_init_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s mod init [-C dir] [--force] [module/path]\n",
                 program.c_str());
}

std::string shell_quote(std::string text) {
    for (char &c : text) {
        if (c == '\\')
            c = '/';
    }
    std::string out = "\"";
    for (char c : text) {
        if (c == '"')
            out += "\\\"";
        else
            out.push_back(c);
    }
    out += "\"";
    return out;
}

struct project_options {
    std::string directory;
    std::string cache;
    bool force = false;
};

bool parse_common(const std::vector<std::string> &args, std::size_t start,
                  void (*usage)(const std::string &),
                  const std::string &program, project_options &options,
                  std::vector<std::string> &positional) {
    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "-C" || arg == "--project") {
            if (i + 1 >= args.size()) {
                usage(program);
                return false;
            }
            options.directory = args[++i];
        } else if (arg.rfind("--project=", 0) == 0) {
            options.directory = arg.substr(10);
        } else if (arg == "--cache") {
            if (i + 1 >= args.size()) {
                usage(program);
                return false;
            }
            options.cache = args[++i];
        } else if (arg.rfind("--cache=", 0) == 0) {
            options.cache = arg.substr(8);
        } else if (arg == "--force") {
            options.force = true;
        } else if (!arg.empty() && arg[0] == '-') {
            usage(program);
            return false;
        } else {
            positional.push_back(arg);
        }
    }
    return true;
}

struct parsed_spec {
    std::string module;
    std::string version;
};

bool parse_spec(const std::string &text, parsed_spec &out) {
    const std::string marker = "@latest";
    std::string body = text;
    if (body.size() >= marker.size() &&
        body.compare(body.size() - marker.size(), marker.size(), marker) == 0)
        body.resize(body.size() - marker.size());
    else {
        std::size_t at = body.rfind('@');
        if (at != std::string::npos) {
            out.version = body.substr(at + 1);
            body.resize(at);
        }
    }
    if (!skein::modfile::is_valid_module_path(body))
        return false;
    out.module = body;
    if (!out.version.empty() &&
        !skein::modfile::is_valid_version(out.version))
        return false;
    return true;
}

bool is_semver(const std::string &version) {
    std::vector<long long> numbers;
    bool prerelease = false;
    return skein::modfile::parse_semver(version, numbers, prerelease);
}

/* A /vN module path requires a version whose major component equals N. */
bool version_matches_path(const std::string &path, const std::string &version) {
    if (!skein::modfile::has_version_suffix(path))
        return true;
    std::vector<long long> numbers;
    bool prerelease = false;
    if (!skein::modfile::parse_semver(version, numbers, prerelease) ||
        numbers.empty())
        return false;
    const std::string major = path.substr(path.rfind('/') + 2);
    try {
        return numbers[0] == std::stoll(major);
    } catch (...) {
        return false;
    }
}

std::string module_url(const std::string &module) {
    std::string override;
    if (thread_env_get("SKEIN_MODULE_URL", override) && !override.empty()) {
        /* Accept either a single URL for all modules, or "path=url,..." pairs
         * so tests and mirrors can override each module independently. */
        std::size_t pos = 0;
        while (pos <= override.size()) {
            std::size_t end = override.find(',', pos);
            std::string item = override.substr(pos, end == std::string::npos
                                                       ? std::string::npos
                                                       : end - pos);
            std::size_t equals = item.find('=');
            if (equals != std::string::npos && item.substr(0, equals) == module)
                return item.substr(equals + 1);
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
        if (override.find('=') == std::string::npos)
            return override;
    }
    return "https://" + module;
}

bool resolve_latest(const std::string &module, std::string &version,
                    std::string &error) {
    const std::string command =
        "git ls-remote --tags --refs " + shell_quote(module_url(module));
    std::string output;
    int status = -1;
    if (!platform_run_command(command, {}, output, status)) {
        error = "cannot run git ls-remote; install git and retry";
        return false;
    }
    if (status != 0) {
        error = "git ls-remote failed for " + module;
        return false;
    }

    std::string best;
    bool have_best = false;
    std::size_t pos = 0;
    while (pos < output.size()) {
        std::size_t end = output.find('\n', pos);
        std::string line = output.substr(pos, end == std::string::npos
                                                  ? std::string::npos
                                                  : end - pos);
        pos = end == std::string::npos ? output.size() : end + 1;
        const std::string prefix = "refs/tags/";
        std::size_t tag = line.find(prefix);
        if (tag == std::string::npos)
            continue;
        std::string candidate = line.substr(tag + prefix.size());
        if (!is_semver(candidate) || !version_matches_path(module, candidate))
            continue;
        if (!have_best ||
            skein::modfile::compare_semver(candidate, best) > 0) {
            best = candidate;
            have_best = true;
        }
    }
    if (!have_best) {
        error = "no semver tags found for " + module +
                "; specify an explicit @version";
        return false;
    }
    version = best;
    return true;
}

std::string unique_temp_dir(const std::string &cache) {
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    return (fs::path(cache) / (".tmp-" + std::to_string(stamp))).u8string();
}

bool download_module(const std::string &module, const std::string &version,
                     const std::string &destination, std::string &error) {
    const fs::path cache = fs::path(destination).parent_path();
    std::error_code ec;
    fs::create_directories(cache, ec);
    if (ec) {
        error = "cannot create module cache: " + cache.u8string();
        return false;
    }

    const std::string temp = unique_temp_dir(cache.u8string());
    const std::string command =
        "git -c advice.detachedHead=false clone --quiet --depth 1 --branch " +
        shell_quote(version) + " " + shell_quote(module_url(module)) + " " +
        shell_quote(temp);
    int status = -1;
    if (!platform_run_command_status(command, {}, status)) {
        error = "cannot run git clone; install git and retry";
        return false;
    }
    if (status != 0) {
        fs::remove_all(temp, ec);
        error = "git clone failed for " + module + "@" + version;
        return false;
    }

    std::error_code manifest_ec;
    if (!fs::is_regular_file(fs::path(temp) / "module.skein", manifest_ec) ||
        manifest_ec) {
        fs::remove_all(temp, ec);
        error = "module " + module + " has no module.skein at its root";
        return false;
    }

    fs::remove_all(destination, ec);
    fs::rename(temp, destination, ec);
    if (ec) {
        fs::remove_all(temp, ec);
        error = "cannot install module into cache: " + destination;
        return false;
    }
    return true;
}

struct selection {
    std::string version;
    bool indirect = true;
};

/* Ensure a module version is downloaded and checksummed, then return its
 * cached directory and parsed skein.mod. */
bool prepare_module(const std::string &root, const std::string &path,
                    const std::string &version, fs::path &directory,
                    skein::modfile::mod_file &mod, std::string &error) {
    directory = fs::u8path(skein::modfile::cache_path(path, version));
    if (!skein::modfile::verify_cached_module(root, path, version, error)) {
        if (!download_module(path, version,
                             skein::modfile::cache_path(path, version),
                             error))
            return false;
        std::string hash;
        if (!skein::modfile::dirhash(directory.u8string(), hash, error))
            return false;
        if (!skein::modfile::add_sum(root, path, version, hash, error))
            return false;
    }
    if (!skein::modfile::load(directory.u8string(), mod, error))
        return false;
    return true;
}

}  // namespace

int run_get_tool(const std::vector<std::string> &args,
                 const std::string &program) {
    project_options options;
    std::vector<std::string> positional;
    if (!parse_common(args, 2, print_get_usage, program, options, positional))
        return 2;
    if (positional.empty()) {
        print_get_usage(program);
        return 2;
    }
    if (!options.cache.empty())
        platform_set_env("SKEINMODCACHE", options.cache);

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

    std::vector<parsed_spec> specs;
    for (const std::string &text : positional) {
        parsed_spec spec;
        if (!parse_spec(text, spec)) {
            std::fprintf(stderr, "%s: invalid module spec '%s'\n",
                         program.c_str(), text.c_str());
            return 2;
        }
        specs.push_back(spec);
    }

    skein::modfile::mod_file existing;
    std::string error;
    if (!skein::modfile::load(root, existing, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    /* Direct dependencies are: modules named on the command line, plus
     * existing root requirements not marked indirect. */
    std::set<std::string> direct;
    for (const skein::modfile::require_spec &spec : existing.requirements) {
        if (!spec.indirect)
            direct.insert(spec.path);
    }

    std::map<std::string, selection> selected;
    for (const skein::modfile::require_spec &spec : existing.requirements) {
        selected[spec.path] = {spec.version, spec.indirect};
    }

    std::vector<parsed_spec> requested;
    for (const parsed_spec &spec : specs) {
        parsed_spec resolved = spec;
        if (resolved.version.empty()) {
            if (!resolve_latest(resolved.module, resolved.version, error)) {
                std::fprintf(stderr, "%s: %s\n", program.c_str(),
                             error.c_str());
                return 1;
            }
        }
        if (!is_semver(resolved.version)) {
            std::fprintf(stderr,
                         "%s: non-semver version '%s' for %s; "
                         "use an explicit semver tag\n",
                         program.c_str(), resolved.version.c_str(),
                         resolved.module.c_str());
            return 1;
        }
        if (!version_matches_path(resolved.module, resolved.version)) {
            std::fprintf(stderr,
                         "%s: version %s does not match module path %s\n",
                         program.c_str(), resolved.version.c_str(),
                         resolved.module.c_str());
            return 1;
        }
        direct.insert(resolved.module);
        auto it = selected.find(resolved.module);
        if (it == selected.end() ||
            skein::modfile::compare_semver(resolved.version, it->second.version) >
                0)
            selected[resolved.module] = {resolved.version, false};
        else
            it->second.indirect = false;
        requested.push_back(resolved);
    }

    /* Minimal version selection: start from every selected root requirement
     * and expand transitive requirements, always keeping the highest version
     * for a given module path. Upgraded versions are queued for expansion. */
    std::set<std::pair<std::string, std::string>> visited;
    std::vector<std::pair<std::string, std::string>> queue;
    for (const auto &entry : selected)
        queue.push_back({entry.first, entry.second.version});
    while (!queue.empty()) {
        auto item = queue.back();
        queue.pop_back();
        if (!visited.insert(item).second)
            continue;
        fs::path directory;
        skein::modfile::mod_file mod;
        if (!prepare_module(root, item.first, item.second, directory, mod,
                            error)) {
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
        for (const skein::modfile::require_spec &child : mod.requirements) {
            if (!is_semver(child.version)) {
                std::fprintf(stderr,
                             "%s: %s requires non-semver version %s of %s\n",
                             program.c_str(), item.first.c_str(),
                             child.version.c_str(), child.path.c_str());
                return 1;
            }
            if (!version_matches_path(child.path, child.version)) {
                std::fprintf(stderr,
                             "%s: %s requires version %s that does not "
                             "match module path %s\n",
                             program.c_str(), item.first.c_str(),
                             child.version.c_str(), child.path.c_str());
                return 1;
            }
            auto it = selected.find(child.path);
            if (it == selected.end()) {
                selected[child.path] = {child.version, true};
                queue.push_back({child.path, child.version});
            } else if (skein::modfile::compare_semver(child.version,
                                                      it->second.version) > 0) {
                it->second.version = child.version;
                queue.push_back({child.path, child.version});
            }
        }
    }

    skein::modfile::mod_file updated;
    updated.module = existing.module;
    for (const auto &entry : selected) {
        updated.requirements.push_back(
            {entry.first, entry.second.version,
             entry.second.indirect || direct.count(entry.first) == 0});
    }
    if (!skein::modfile::save(root, updated, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    for (const parsed_spec &spec : requested) {
        std::printf("%s@%s\n", spec.module.c_str(), spec.version.c_str());
    }
    return 0;
}

int run_mod_init_tool(const std::vector<std::string> &args,
                      const std::string &program) {
    project_options options;
    std::vector<std::string> positional;
    if (!parse_common(args, 3, print_mod_init_usage, program, options,
                      positional))
        return 2;
    if (positional.size() > 1) {
        print_mod_init_usage(program);
        return 2;
    }

    const std::string directory = options.directory.empty()
                                      ? (thread_cwd().empty() ? "."
                                                              : thread_cwd())
                                      : options.directory;
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        std::fprintf(stderr, "%s: cannot create directory %s\n",
                     program.c_str(), directory.c_str());
        return 1;
    }
    std::error_code exists_ec;
    if (fs::exists(fs::path(directory) / "skein.mod", exists_ec) &&
        !exists_ec && !options.force) {
        std::fprintf(stderr, "%s: skein.mod already exists (use --force)\n",
                     program.c_str());
        return 1;
    }

    skein::modfile::mod_file mod;
    if (!positional.empty()) {
        if (!skein::modfile::is_valid_module_path(positional[0])) {
            std::fprintf(stderr, "%s: invalid module path '%s'\n",
                         program.c_str(), positional[0].c_str());
            return 2;
        }
        mod.module = positional[0];
    }

    std::string error;
    if (!skein::modfile::save(directory, mod, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    std::printf("created %s\n",
                (fs::path(directory) / "skein.mod").u8string().c_str());
    return 0;
}
