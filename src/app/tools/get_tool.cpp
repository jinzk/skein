#include "get_tool.h"

#include "file_lock.h"
#include "modfile.h"
#include "path.h"
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
                 "usage: %s get [-C dir] [--cache dir] [--offline] [--force] "
                 "<module/path>[@version] ...\n",
                 program.c_str());
}

void print_mod_init_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s mod init [-C dir] [--force] [module/path]\n",
                 program.c_str());
}

void print_mod_clean_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s mod clean [-C dir] [--cache dir] [--dry-run] "
                 "[--force]\n",
                 program.c_str());
}

void print_mod_download_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s mod download [-C dir] [--cache dir] [--offline] "
                 "[--force]\n",
                 program.c_str());
}

void print_mod_verify_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s mod verify [-C dir] [--cache dir]\n",
                 program.c_str());
}

void print_mod_tidy_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s mod tidy [-C dir] [--cache dir] [--offline] "
                 "[--force]\n",
                 program.c_str());
}

void print_mod_prune_usage(const std::string &program) {
    std::fprintf(stderr,
                 "usage: %s mod prune [-C dir] [--cache dir] [--dry-run] "
                 "[--all]\n",
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
    bool dry_run = false;
    bool offline = false;
    bool all = false;
};

bool parse_common(const std::vector<std::string> &args, std::size_t start,
                  void (*usage)(const std::string &),
                  const std::string &program, project_options &options,
                  std::vector<std::string> &positional,
                  bool allow_all = false) {
    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string &arg = args[i];
        if (arg == "-C" || arg == "--project") {
            if (i + 1 >= args.size()) {
                usage(program);
                return false;
            }
            options.directory = args[++i];
        } else if (arg.starts_with("--project=")) {
            options.directory = arg.substr(10);
        } else if (arg == "--cache") {
            if (i + 1 >= args.size()) {
                usage(program);
                return false;
            }
            options.cache = args[++i];
        } else if (arg.starts_with("--cache=")) {
            options.cache = arg.substr(8);
        } else if (arg == "--force") {
            options.force = true;
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "--offline") {
            options.offline = true;
        } else if (arg == "--all" && allow_all) {
            options.all = true;
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

/* SKEIN_MODULE_URL accepts either a single URL for every module or
 * "path=url,..." pairs.  Both forms may use {path} and {version} templates so
 * one setting can point all modules at a reachable mirror. */
std::string expand_url(std::string url, const std::string &module,
                       const std::string &version) {
    replace_all(url, "{path}", module);
    if (!version.empty())
        replace_all(url, "{version}", version);
    return url;
}

std::string module_url(const std::string &module,
                       const std::string &version = std::string()) {
    std::string override;
    if (thread_env_get("SKEIN_MODULE_URL", override) && !override.empty()) {
        std::size_t pos = 0;
        while (pos <= override.size()) {
            std::size_t end = override.find(',', pos);
            std::string item = override.substr(pos, end == std::string::npos
                                                       ? std::string::npos
                                                       : end - pos);
            std::size_t equals = item.find('=');
            if (equals != std::string::npos && item.substr(0, equals) == module)
                return expand_url(item.substr(equals + 1), module, version);
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
        if (override.find('=') == std::string::npos)
            return expand_url(override, module, version);
    }
    return "https://" + module;
}

bool resolve_latest(const std::string &module, std::string &version,
                    std::string &error) {
    const std::string url = module_url(module);
    if (url.find("{version}") != std::string::npos) {
        error = "module URL template with {version} cannot resolve @latest "
                "for " + module + "; use an explicit version";
        return false;
    }
    const std::string command =
        "git ls-remote --tags --refs " + shell_quote(url);
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
    return skein::path_to_utf8(
        skein::path_from_utf8(cache) /
        (".tmp-" + std::to_string(stamp)));
}

/* Git checkouts contain read-only object files.  remove_all() alone cannot
 * delete those on Windows, so clear the write bit before removing the tree. */
bool remove_tree(const fs::path &path, std::string &error) {
    std::error_code ec;
    if (!fs::exists(path, ec))
        return !ec;
    fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, ec);
    if (!ec) {
        fs::recursive_directory_iterator it(
            path, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            fs::permissions(it->path(), fs::perms::owner_write,
                            fs::perm_options::add, ec);
        }
    }
    if (ec) {
        error = "cannot prepare module cache for removal: " +
                skein::path_to_utf8(path) + ": " + ec.message();
        return false;
    }
    fs::remove_all(path, ec);
    if (ec) {
        error = "cannot remove module cache: " + skein::path_to_utf8(path) +
                ": " + ec.message();
        return false;
    }
    return true;
}

void discard_tree(const fs::path &path) {
    std::string ignored;
    remove_tree(path, ignored);
}

struct cached_module {
    fs::path directory;
    std::string path;
    std::string version;
};

std::vector<cached_module> list_cached_modules(const fs::path &cache) {
    std::vector<cached_module> modules;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        cache, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = skein::path_to_utf8(it->path().filename());
        if (!it->is_directory(ec))
            continue;
        if (name == ".build" || name == ".locks" || name == ".git" ||
            name.starts_with(".tmp-")) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code manifest_ec;
        if (!fs::is_regular_file(it->path() / "module.skein", manifest_ec))
            continue;

        std::error_code relative_ec;
        const fs::path relative =
            fs::relative(it->path(), cache, relative_ec);
        if (relative_ec || relative.empty())
            continue;
        const fs::path version_path = relative.parent_path();
        if (version_path.empty() || version_path.parent_path().empty())
            continue;
        const std::string version = skein::path_to_utf8(version_path.filename());
        const std::string module_name = skein::path_to_utf8(relative.filename());
        const fs::path module_parent = version_path.parent_path();
        const std::string parent_text = skein::path_to_utf8_generic(module_parent);
        std::string module_path;
        if (skein::modfile::has_version_suffix(parent_text) &&
            skein::modfile::module_name_from_path(parent_text) == module_name) {
            module_path = parent_text;
        } else {
            module_path = skein::path_to_utf8_generic(
                module_parent / relative.filename());
        }
        if (!skein::modfile::is_valid_module_path(module_path) ||
            !skein::modfile::is_valid_version(version))
            continue;
        modules.push_back({it->path(), module_path, version});
        it.disable_recursion_pending();
    }
    return modules;
}

/* Batch checksum updates so the success path writes skein.sum once.  If a
 * later module fails, keep the shared cache and best-effort persist hashes for
 * modules that were already prepared, so a retry normally does not need
 * --force and other projects sharing the cache are not disrupted. */
struct pending_sums {
    std::string root;
    std::vector<skein::modfile::sum_update> updates;
    bool committed = false;

    ~pending_sums() {
        if (committed || root.empty() || updates.empty())
            return;
        std::string ignored;
        skein::modfile::update_sums(root, updates, ignored);
    }
};

bool download_module(const std::string &module, const std::string &version,
                     const std::string &destination, std::string &error) {
    const fs::path cache = skein::path_from_utf8(destination).parent_path();
    std::error_code ec;
    fs::create_directories(cache, ec);
    if (ec) {
        error = "cannot create module cache: " + skein::path_to_utf8(cache);
        return false;
    }

    const std::string temp = unique_temp_dir(skein::path_to_utf8(cache));
    const std::string command =
        "git -c advice.detachedHead=false clone --quiet --depth 1 --branch " +
        shell_quote(version) + " " + shell_quote(module_url(module, version)) +
        " " + shell_quote(temp);
    int status = -1;
    if (!platform_run_command_status(command, {}, status)) {
        error = "cannot run git clone; install git and retry";
        return false;
    }
    if (status != 0) {
        discard_tree(temp);
        error = "git clone failed for " + module + "@" + version;
        return false;
    }

    std::error_code manifest_ec;
    if (!fs::is_regular_file(skein::path_from_utf8(temp) / "module.skein",
                             manifest_ec) ||
        manifest_ec) {
        discard_tree(temp);
        error = "module " + module + " has no module.skein at its root";
        return false;
    }

    if (!remove_tree(destination, error))
        return false;
    fs::rename(temp, destination, ec);
    if (ec) {
        discard_tree(temp);
        error = "cannot install module into cache: " + destination + ": " +
                ec.message();
        return false;
    }
    return true;
}

struct selection {
    std::string version;
    bool indirect = true;
};

/* Checksums are the trust root: never silently overwrite an existing entry.
 * `force` is the explicit opt-in that refreshes the cache and the sum. */
bool prepare_module(const std::string &root, const std::string &path,
                    const std::string &version, bool force, bool offline,
                    fs::path &directory, skein::modfile::mod_file &mod,
                    std::string &hash, bool &needs_sum, std::string &error) {
    file_lock lock;
    if (!acquire_module_lock(path, version, lock, lock_mode::exclusive, error))
        return false;

    directory = skein::path_from_utf8(
        skein::modfile::cache_path(path, version));
    const bool cached = fs::is_directory(directory);
    bool have_sum = false;
    std::string expected;
    if (!skein::modfile::find_sum_entry(root, path, version, have_sum,
                                        expected, error))
        return false;

    hash.clear();
    needs_sum = false;
    if (cached && !force && have_sum) {
        if (!skein::modfile::dirhash(skein::path_to_utf8(directory), hash, error))
            return false;
        if (hash != expected) {
            error = "checksum mismatch for " + path + "@" + version +
                    "; refusing to overwrite skein.sum; run 'skein get "
                    "--force " + path + "@" + version + "' to refresh";
            return false;
        }
    }

    /* A cache without this project's checksum is not trusted: refresh it from
     * the source and write a new checksum instead of adopting bytes written by
     * another project or process. */
    if (!cached || force || !have_sum) {
        if (offline) {
            if (cached) {
                error = "module " + path + "@" + version +
                        " is cached but has no checksum for this project; run "
                        "'skein get " + path + "@" + version + "' online once";
            } else {
                error = "module " + path + "@" + version +
                        " is not cached; run 'skein get " + path + "@" +
                        version + "' online first or omit --offline";
            }
            return false;
        }
        if (!download_module(path, version,
                             skein::modfile::cache_path(path, version),
                             error))
            return false;
        if (!skein::modfile::dirhash(skein::path_to_utf8(directory), hash, error)) {
            discard_tree(directory);
            return false;
        }
        if (have_sum && !force) {
            if (hash != expected) {
                discard_tree(directory);
                error = "checksum mismatch for " + path + "@" + version +
                        "; downloaded content does not match skein.sum";
                return false;
            }
        } else {
            needs_sum = true;
        }
    }
    if (!skein::modfile::load(skein::path_to_utf8(directory), mod, error)) {
        if (!cached || force)
            discard_tree(directory);
        return false;
    }
    return true;
}

/* Expand the selected set through transitive requirements using MVS. */
bool expand_modules(const std::string &root,
                    std::map<std::string, selection> &selected, bool force,
                    bool offline, pending_sums &sums, std::string &error) {
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
        std::string hash;
        bool needs_sum = false;
        if (!prepare_module(root, item.first, item.second, force, offline,
                            directory, mod, hash, needs_sum, error))
            return false;
        if (needs_sum)
            sums.updates.push_back({item.first, item.second, hash});
        for (const skein::modfile::require_spec &child : mod.requirements) {
            if (!is_semver(child.version)) {
                error = item.first + " requires non-semver version " +
                        child.version + " of " + child.path;
                return false;
            }
            if (!version_matches_path(child.path, child.version)) {
                error = item.first + " requires version " + child.version +
                        " that does not match module path " + child.path;
                return false;
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
    return true;
}

}  // namespace

int run_get_tool(const std::vector<std::string> &args,
                 const std::string &program) {
    project_options options;
    std::vector<std::string> positional;
    if (!parse_common(args, 2, print_get_usage, program, options, positional))
        return 2;
    if (options.dry_run) {
        print_get_usage(program);
        return 2;
    }
    if (options.offline && options.force) {
        std::fprintf(stderr, "%s: --force and --offline are mutually exclusive\n",
                     program.c_str());
        return 2;
    }
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

    std::string error;
    file_lock project_guard;
    if (!acquire_project_lock(root, project_guard, lock_mode::exclusive, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    file_lock cache_guard;
    if (!acquire_cache_lock(cache_guard, lock_mode::shared, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    skein::modfile::mod_file existing;
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
            if (options.offline) {
                error = "cannot resolve @latest in offline mode; specify an "
                        "explicit version";
                std::fprintf(stderr, "%s: %s\n", program.c_str(),
                             error.c_str());
                return 1;
            }
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

    pending_sums sums;
    sums.root = root;
    if (!expand_modules(root, selected, options.force, options.offline, sums,
                        error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    skein::modfile::mod_file updated;
    updated.module = existing.module;
    for (const auto &entry : selected) {
        updated.requirements.push_back(
            {entry.first, entry.second.version,
             entry.second.indirect || direct.count(entry.first) == 0});
    }
    if (!skein::modfile::update_sums(root, sums.updates, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    sums.committed = true;
    if (!skein::modfile::save(root, updated, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    for (const parsed_spec &spec : requested) {
        std::printf("%s@%s\n", spec.module.c_str(), spec.version.c_str());
    }
    return 0;
}

int run_mod_download_tool(const std::vector<std::string> &args,
                          const std::string &program) {
    project_options options;
    std::vector<std::string> positional;
    if (!parse_common(args, 3, print_mod_download_usage, program, options,
                      positional))
        return 2;
    if (!positional.empty() || options.dry_run) {
        print_mod_download_usage(program);
        return 2;
    }
    if (options.offline && options.force) {
        std::fprintf(stderr, "%s: --force and --offline are mutually exclusive\n",
                     program.c_str());
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

    std::string error;
    file_lock project_guard;
    if (!acquire_project_lock(root, project_guard, lock_mode::exclusive, error)) {
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
    pending_sums sums;
    sums.root = root;
    for (const skein::modfile::require_spec &req : mod.requirements) {
        if (!is_semver(req.version)) {
            error = "module " + req.path + " has non-semver version " +
                    req.version;
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
        if (!version_matches_path(req.path, req.version)) {
            error = "version " + req.version + " does not match module path " +
                    req.path;
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
        fs::path directory;
        skein::modfile::mod_file dep;
        std::string hash;
        bool needs_sum = false;
        if (!prepare_module(root, req.path, req.version, options.force,
                            options.offline, directory, dep, hash, needs_sum,
                            error)) {
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
        if (needs_sum)
            sums.updates.push_back({req.path, req.version, hash});
        std::printf("downloaded %s@%s\n", req.path.c_str(),
                    req.version.c_str());
    }
    if (!skein::modfile::update_sums(root, sums.updates, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    sums.committed = true;
    if (mod.requirements.empty())
        std::printf("no modules to download\n");
    return 0;
}

int run_mod_verify_tool(const std::vector<std::string> &args,
                        const std::string &program) {
    project_options options;
    std::vector<std::string> positional;
    if (!parse_common(args, 3, print_mod_verify_usage, program, options,
                      positional))
        return 2;
    if (!positional.empty() || options.dry_run || options.force) {
        print_mod_verify_usage(program);
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

    std::string error;
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
    for (const skein::modfile::require_spec &req : mod.requirements) {
        if (!is_semver(req.version)) {
            error = "module " + req.path + " has non-semver version " +
                    req.version;
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
        if (!version_matches_path(req.path, req.version)) {
            error = "version " + req.version + " does not match module path " +
                    req.path;
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
        file_lock module_guard;
        if (!acquire_module_lock(req.path, req.version, module_guard,
                                 lock_mode::shared, error)) {
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
        if (!skein::modfile::verify_cached_module(root, req.path, req.version,
                                                  error)) {
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
    }
    std::printf("verified %zu module(s)\n", mod.requirements.size());
    return 0;
}

int run_mod_tidy_tool(const std::vector<std::string> &args,
                      const std::string &program) {
    project_options options;
    std::vector<std::string> positional;
    if (!parse_common(args, 3, print_mod_tidy_usage, program, options,
                      positional))
        return 2;
    if (!positional.empty() || options.dry_run) {
        print_mod_tidy_usage(program);
        return 2;
    }
    if (options.offline && options.force) {
        std::fprintf(stderr, "%s: --force and --offline are mutually exclusive\n",
                     program.c_str());
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

    std::string error;
    file_lock project_guard;
    if (!acquire_project_lock(root, project_guard, lock_mode::exclusive, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    file_lock cache_guard;
    if (!acquire_cache_lock(cache_guard, lock_mode::shared, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    skein::modfile::mod_file existing;
    if (!skein::modfile::load(root, existing, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    std::set<std::string> direct;
    std::map<std::string, selection> selected;
    for (const skein::modfile::require_spec &req : existing.requirements) {
        if (!req.indirect) {
            direct.insert(req.path);
            selected[req.path] = {req.version, false};
        }
    }

    pending_sums sums;
    sums.root = root;
    if (!expand_modules(root, selected, options.force, options.offline, sums,
                        error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    skein::modfile::mod_file updated;
    updated.module = existing.module;
    for (const auto &entry : selected) {
        updated.requirements.push_back(
            {entry.first, entry.second.version,
             direct.count(entry.first) == 0});
    }
    if (!skein::modfile::update_sums(root, sums.updates, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    sums.committed = true;
    if (!skein::modfile::save(root, updated, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    std::printf("tidied %zu module(s)\n", updated.requirements.size());
    return 0;
}

int run_mod_prune_tool(const std::vector<std::string> &args,
                       const std::string &program) {
    project_options options;
    std::vector<std::string> positional;
    if (!parse_common(args, 3, print_mod_prune_usage, program, options,
                      positional, true))
        return 2;
    if (!positional.empty() || options.offline || options.force) {
        print_mod_prune_usage(program);
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

    std::string error;
    file_lock project_guard;
    if (!acquire_project_lock(root, project_guard, lock_mode::shared, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    file_lock cache_guard;
    if (!acquire_cache_lock(cache_guard, lock_mode::exclusive, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }

    skein::modfile::mod_file mod;
    if (!skein::modfile::load(root, mod, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    std::set<std::string> keep;
    for (const skein::modfile::require_spec &req : mod.requirements)
        keep.insert(req.path + "@" + req.version);

    const fs::path cache = skein::path_from_utf8(skein::modfile::cache_dir());
    const std::vector<cached_module> cached = list_cached_modules(cache);
    std::vector<cached_module> unused;
    for (const cached_module &entry : cached) {
        if (!keep.count(entry.path + "@" + entry.version))
            unused.push_back(entry);
    }
    if (unused.empty()) {
        std::printf("nothing to prune\n");
        return 0;
    }

    std::size_t removed = 0;
    for (const cached_module &entry : unused) {
        if (!options.all || options.dry_run) {
            std::printf("would remove %s@%s\n", entry.path.c_str(),
                        entry.version.c_str());
            continue;
        }
        if (!remove_tree(entry.directory, error)) {
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
        const fs::path native = skein::path_from_utf8(
            skein::modfile::native_build_dir(entry.path, entry.version));
        std::error_code ec;
        if (fs::exists(native, ec) && !remove_tree(native, error)) {
            std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
            return 1;
        }
        std::printf("removed %s@%s\n", entry.path.c_str(),
                    entry.version.c_str());
        ++removed;
    }
    if (!options.all && !options.dry_run) {
        std::printf("pass --all to remove these cache entries\n");
    } else if (options.all && !options.dry_run) {
        std::printf("pruned %zu module version(s)\n", removed);
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
    if (options.dry_run || options.offline) {
        print_mod_init_usage(program);
        return 2;
    }
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
    std::string error;
    file_lock project_guard;
    if (!acquire_project_lock(directory, project_guard, lock_mode::exclusive,
                              error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    std::error_code exists_ec;
    if (fs::exists(skein::path_from_utf8(directory) / "skein.mod", exists_ec) &&
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

    if (!skein::modfile::save(directory, mod, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    std::printf("created %s\n",
                skein::path_to_utf8(skein::path_from_utf8(directory) /
                                    "skein.mod").c_str());
    return 0;
}

bool same_path(const fs::path &left, const fs::path &right) {
    std::error_code ec;
    if (fs::exists(left, ec) && !ec && fs::exists(right, ec) && !ec) {
        bool equivalent = fs::equivalent(left, right, ec);
        if (!ec)
            return equivalent;
    }
    return left == right;
}

bool matches_env_path(const fs::path &candidate, const char *name) {
    std::string value;
    if (!thread_env_get(name, value) || value.empty())
        return false;
    std::error_code ec;
    const fs::path from_env = fs::absolute(skein::path_from_utf8(value), ec);
    return !ec && same_path(candidate, from_env);
}

bool unsafe_cache_root(const fs::path &root) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(root, ec);
    if (ec || absolute.empty())
        return true;
    if (absolute == absolute.root_path() || absolute.parent_path() == absolute)
        return true;
    if (matches_env_path(absolute, "HOME") ||
        matches_env_path(absolute, "USERPROFILE") ||
        matches_env_path(absolute, "LOCALAPPDATA") ||
        matches_env_path(absolute, "APPDATA"))
        return true;
    const std::string cwd = thread_cwd();
    if (!cwd.empty()) {
        const fs::path absolute_cwd =
            fs::absolute(skein::path_from_utf8(cwd), ec);
        if (!ec && same_path(absolute, absolute_cwd))
            return true;
    }
    return false;
}

int run_mod_clean_tool(const std::vector<std::string> &args,
                       const std::string &program) {
    project_options options;
    std::vector<std::string> positional;
    if (!parse_common(args, 3, print_mod_clean_usage, program, options,
                      positional))
        return 2;
    if (!positional.empty() || options.offline) {
        print_mod_clean_usage(program);
        return 2;
    }
    if (!options.cache.empty())
        platform_set_env("SKEINMODCACHE", options.cache);

    const fs::path root = skein::path_from_utf8(skein::modfile::cache_dir());
    const std::string display = skein::path_to_utf8(root);
    if (!options.force && unsafe_cache_root(root)) {
        std::fprintf(stderr,
                     "%s: refusing to clean unsafe cache root %s; "
                     "pass --force to override\n",
                     program.c_str(), display.c_str());
        return 2;
    }
    if (options.dry_run) {
        std::printf("would clean %s\n", display.c_str());
        return 0;
    }

    std::error_code ec;
    const bool exists = fs::exists(root, ec);
    if (ec) {
        std::fprintf(stderr, "%s: cannot inspect cache root %s: %s\n",
                     program.c_str(), display.c_str(), ec.message().c_str());
        return 1;
    }
    if (!exists) {
        std::printf("nothing to clean: %s\n", display.c_str());
        return 0;
    }
    std::string error;
    file_lock cache_guard;
    if (!acquire_cache_lock(cache_guard, lock_mode::exclusive, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    if (!remove_tree(root, error)) {
        std::fprintf(stderr, "%s: %s\n", program.c_str(), error.c_str());
        return 1;
    }
    std::printf("cleaned %s\n", display.c_str());
    return 0;
}
