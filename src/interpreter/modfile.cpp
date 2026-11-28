#include "modfile.h"

#include "path.h"
#include "platform.h"
#include "runtime_state.h"
#include "sha256.h"
#include "unicode_ident.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace skein {
namespace modfile {
namespace {

namespace fs = std::filesystem;

fs::path path_of(const std::string &text) {
    return skein::path_from_utf8(text);
}

std::string text_of(const fs::path &path) { return skein::path_to_utf8(path); }

std::string trim(const std::string &text) {
    size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
        ++begin;
    size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' ||
            text[end - 1] == '\r'))
        --end;
    return text.substr(begin, end - begin);
}

std::vector<std::string> words(const std::string &line) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() &&
               (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;
        if (pos >= line.size())
            break;
        size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t')
            ++pos;
        result.push_back(line.substr(start, pos - start));
    }
    return result;
}

struct sum_entry {
    std::string path;
    std::string version;
    std::string hash;
};

std::string sums_path(const std::string &root) {
    return text_of(path_of(root) / "skein.sum");
}

bool load_sums(const std::string &root, std::vector<sum_entry> &entries,
               std::string &error) {
    entries.clear();
    std::string data;
    if (!platform_read_file(sums_path(root), data, error))
        return false;
    size_t pos = 0;
    int line_number = 0;
    while (pos <= data.size()) {
        size_t end = data.find('\n', pos);
        std::string line = trim(data.substr(pos, end == std::string::npos
                                                    ? std::string::npos
                                                    : end - pos));
        ++line_number;
        if (!line.empty() && line[0] != '#') {
            std::vector<std::string> parts = words(line);
            if (parts.size() != 3 || parts[2].rfind("h1:", 0) != 0) {
                error = "invalid skein.sum line " + std::to_string(line_number);
                return false;
            }
            entries.push_back({parts[0], parts[1], parts[2]});
        }
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return true;
}

/* Write the complete file before replacing the destination.  This keeps
 * readers from observing a truncated skein.mod or skein.sum. */
bool write_atomic(const std::string &path, const std::string &data,
                  std::string &error) {
    const std::string temp = path + ".tmp";
    if (!platform_write_file(temp, data, false, error)) {
        std::error_code cleanup_ec;
        fs::remove(path_of(temp), cleanup_ec);
        return false;
    }
    std::error_code ec;
    fs::rename(path_of(temp), path_of(path), ec);
    if (!ec)
        return true;
    /* Windows does not replace an existing file with rename(). */
    if (ec == std::errc::file_exists) {
        std::error_code remove_ec;
        fs::remove(path_of(path), remove_ec);
        if (!remove_ec) {
            fs::rename(path_of(temp), path_of(path), ec);
            if (!ec)
                return true;
        }
    }
    std::error_code cleanup_ec;
    fs::remove(path_of(temp), cleanup_ec);
    error = "cannot replace " + path + ": " + ec.message();
    return false;
}

bool inspect_regular_file(const fs::path &path, bool &regular,
                          std::string &error) {
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            regular = false;
            return true;
        }
        error = ec.message();
        return false;
    }
    regular = fs::is_regular_file(status);
    return true;
}

bool save_sums(const std::string &root, const std::vector<sum_entry> &entries,
               std::string &error) {
    std::string data;
    for (const sum_entry &entry : entries)
        data += entry.path + " " + entry.version + " " + entry.hash + "\n";
    return write_atomic(sums_path(root), data, error);
}

std::string base64_encode(const std::string &data) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t pos = 0;
    while (pos + 3 <= data.size()) {
        unsigned int value = ((unsigned int)(unsigned char)data[pos] << 16) |
                             ((unsigned int)(unsigned char)data[pos + 1] << 8) |
                             ((unsigned int)(unsigned char)data[pos + 2]);
        out.push_back(alphabet[(value >> 18) & 0x3F]);
        out.push_back(alphabet[(value >> 12) & 0x3F]);
        out.push_back(alphabet[(value >> 6) & 0x3F]);
        out.push_back(alphabet[value & 0x3F]);
        pos += 3;
    }
    size_t remaining = data.size() - pos;
    if (remaining == 1) {
        unsigned int value = (unsigned int)(unsigned char)data[pos] << 16;
        out.push_back(alphabet[(value >> 18) & 0x3F]);
        out.push_back(alphabet[(value >> 12) & 0x3F]);
        out += "==";
    } else if (remaining == 2) {
        unsigned int value = ((unsigned int)(unsigned char)data[pos] << 16) |
                             ((unsigned int)(unsigned char)data[pos + 1] << 8);
        out.push_back(alphabet[(value >> 18) & 0x3F]);
        out.push_back(alphabet[(value >> 12) & 0x3F]);
        out.push_back(alphabet[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool module_path_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
           c == '~';
}

bool version_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
           c == '+';
}

bool is_version_suffix(const std::string &part) {
    if (part.size() < 2 || part[0] != 'v')
        return false;
    for (std::size_t i = 1; i < part.size(); ++i) {
        if (part[i] < '0' || part[i] > '9')
            return false;
    }
    return true;
}

std::string env_or(const char *name, const std::string &fallback) {
    std::string value;
    if (thread_env_get(name, value) && !value.empty())
        return value;
    return fallback;
}

}  // namespace

std::string find_root(const std::string &start) {
    if (start.empty())
        return std::string();
    fs::path directory = path_of(start);
    std::error_code ec;
    if (directory.is_relative())
        directory = fs::absolute(directory, ec);
    if (!ec) {
        fs::path canonical = fs::weakly_canonical(directory, ec);
        if (!ec)
            directory = canonical;
    }
    if (ec)
        return std::string();

    for (const char *marker : {"skein.mod", ".skeinroot"}) {
        fs::path current = directory;
        for (;;) {
            std::error_code exists_ec;
            if (fs::exists(current / marker, exists_ec) && !exists_ec)
                return text_of(current);
            fs::path parent = current.parent_path();
            if (parent.empty() || parent == current)
                break;
            current = parent;
        }
    }
    return std::string();
}

bool exists(const std::string &root) {
    std::error_code ec;
    return fs::is_regular_file(path_of(root) / "skein.mod", ec) && !ec;
}

bool load(const std::string &root, mod_file &out, std::string &error) {
    out = mod_file();
    const std::string file = text_of(path_of(root) / "skein.mod");
    std::error_code ec;
    if (!fs::is_regular_file(path_of(file), ec))
        return true;
    std::string data;
    if (!platform_read_file(file, data, error))
        return false;

    size_t pos = 0;
    int line_number = 0;
    while (pos <= data.size()) {
        size_t end = data.find('\n', pos);
        std::string line = trim(data.substr(pos, end == std::string::npos
                                                    ? std::string::npos
                                                    : end - pos));
        ++line_number;
        size_t comment = line.find("//");
        bool indirect = false;
        if (comment != std::string::npos) {
            std::string note = trim(line.substr(comment + 2));
            indirect = note == "indirect";
            line = trim(line.substr(0, comment));
        }
        comment = line.find('#');
        if (comment != std::string::npos)
            line = trim(line.substr(0, comment));
        if (!line.empty()) {
            std::vector<std::string> parts = words(line);
            if (parts[0] == "module") {
                if (parts.size() != 2 || !is_valid_module_path(parts[1])) {
                    error = "invalid module directive at skein.mod:" +
                            std::to_string(line_number);
                    return false;
                }
                if (!out.module.empty()) {
                    error = "duplicate module directive at skein.mod:" +
                            std::to_string(line_number);
                    return false;
                }
                out.module = parts[1];
            } else if (parts[0] == "require") {
                if (parts.size() != 3 || !is_valid_module_path(parts[1]) ||
                    !is_valid_version(parts[2])) {
                    error = "invalid require directive at skein.mod:" +
                            std::to_string(line_number);
                    return false;
                }
                bool duplicate = false;
                for (require_spec &spec : out.requirements) {
                    if (spec.path == parts[1]) {
                        duplicate = true;
                        if (spec.version == parts[2])
                            break;
                        error = "conflicting versions for " + parts[1] +
                                " at skein.mod:" + std::to_string(line_number);
                        return false;
                    }
                }
                if (!duplicate)
                    out.requirements.push_back({parts[1], parts[2], indirect});
            } else {
                error = "unknown directive '" + parts[0] +
                        "' at skein.mod:" + std::to_string(line_number);
                return false;
            }
        }
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return true;
}

bool save(const std::string &root, const mod_file &mod, std::string &error) {
    std::string data;
    if (!mod.module.empty())
        data += "module " + mod.module + "\n";
    for (const require_spec &spec : mod.requirements) {
        data += "require " + spec.path + " " + spec.version;
        if (spec.indirect)
            data += " // indirect";
        data += "\n";
    }
    return write_atomic(text_of(path_of(root) / "skein.mod"), data, error);
}

bool upsert_require(const std::string &root, const std::string &path,
                    const std::string &version, bool indirect,
                    std::string &error) {
    mod_file mod;
    if (!load(root, mod, error))
        return false;
    bool found = false;
    for (require_spec &spec : mod.requirements) {
        if (spec.path == path) {
            spec.version = version;
            spec.indirect = indirect;
            found = true;
            break;
        }
    }
    if (!found)
        mod.requirements.push_back({path, version, indirect});
    return save(root, mod, error);
}

bool update_sums(const std::string &root,
                 const std::vector<sum_update> &updates, std::string &error) {
    if (updates.empty())
        return true;
    std::vector<sum_entry> entries;
    bool have_sums = false;
    std::string inspect_error;
    if (!inspect_regular_file(path_of(sums_path(root)), have_sums,
                              inspect_error)) {
        error = "cannot inspect skein.sum: " + inspect_error;
        return false;
    }
    if (have_sums && !load_sums(root, entries, error))
        return false;
    for (const sum_update &update : updates) {
        bool found = false;
        for (sum_entry &entry : entries) {
            if (entry.path == update.path && entry.version == update.version) {
                entry.hash = update.hash;
                found = true;
                break;
            }
        }
        if (!found)
            entries.push_back({update.path, update.version, update.hash});
    }
    return save_sums(root, entries, error);
}

bool add_sum(const std::string &root, const std::string &path,
             const std::string &version, const std::string &hash,
             std::string &error) {
    return update_sums(root, {{path, version, hash}}, error);
}

bool find_sum(const std::string &root, const std::string &path,
              const std::string &version, std::string &hash,
              std::string &error) {
    std::vector<sum_entry> entries;
    if (!load_sums(root, entries, error)) {
        error = "cannot read skein.sum: " + error;
        return false;
    }
    for (const sum_entry &entry : entries) {
        if (entry.path == path && entry.version == version) {
            hash = entry.hash;
            return true;
        }
    }
    error = "missing checksum for " + path + " " + version +
            "; run 'skein get " + path + "@" + version + "'";
    return false;
}

bool find_sum_entry(const std::string &root, const std::string &path,
                    const std::string &version, bool &found,
                    std::string &hash, std::string &error) {
    found = false;
    bool have_sums = false;
    std::string inspect_error;
    if (!inspect_regular_file(path_of(root) / "skein.sum", have_sums,
                              inspect_error)) {
        error = "cannot inspect skein.sum: " + inspect_error;
        return false;
    }
    if (!have_sums)
        return true;
    std::vector<sum_entry> entries;
    if (!load_sums(root, entries, error)) {
        error = "cannot read skein.sum: " + error;
        return false;
    }
    for (const sum_entry &entry : entries) {
        if (entry.path == path && entry.version == version) {
            found = true;
            hash = entry.hash;
            return true;
        }
    }
    return true;
}

bool verify_cached_module(const std::string &root, const std::string &path,
                          const std::string &version, std::string &error) {
    const std::string directory = cache_path(path, version);
    std::error_code ec;
    if (!fs::is_directory(path_of(directory), ec)) {
        error = "module not downloaded: " + path + "@" + version +
                "; run 'skein get " + path + "@" + version + "'";
        return false;
    }
    std::string expected;
    if (!find_sum(root, path, version, expected, error))
        return false;
    std::string actual;
    if (!dirhash(directory, actual, error))
        return false;
    if (expected != actual) {
        error = "checksum mismatch for " + path + "@" + version +
                "\n  skein.sum: " + expected + "\n  cache:     " + actual;
        return false;
    }
    return true;
}

bool is_valid_module_path(const std::string &path) {
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string::npos)
        return false;
    size_t start = 0;
    int segments = 0;
    bool host = false;
    for (;;) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos
                                                  ? std::string::npos
                                                  : slash - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (char c : part) {
            if (!module_path_char(c))
                return false;
        }
        if (segments == 0)
            host = part.find('.') != std::string::npos;
        ++segments;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return segments >= 2 && host;
}

bool parse_semver(const std::string &version, std::vector<long long> &numbers,
                  bool &prerelease) {
    numbers.clear();
    prerelease = false;
    if (version.empty() || version[0] != 'v')
        return false;
    std::size_t end = version.find_first_of("-+");
    std::string core = version.substr(1, end == std::string::npos
                                             ? std::string::npos
                                             : end - 1);
    prerelease = end != std::string::npos && version[end] == '-';
    if (core.empty())
        return false;
    std::size_t start = 0;
    for (;;) {
        std::size_t dot = core.find('.', start);
        std::string part = core.substr(start, dot == std::string::npos
                                                  ? std::string::npos
                                                  : dot - start);
        if (part.empty())
            return false;
        for (char c : part) {
            if (c < '0' || c > '9')
                return false;
        }
        try {
            numbers.push_back(std::stoll(part));
        } catch (...) {
            return false;
        }
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return true;
}

int compare_semver(const std::string &a, const std::string &b) {
    std::vector<long long> left;
    std::vector<long long> right;
    bool left_pre = false;
    bool right_pre = false;
    if (!parse_semver(a, left, left_pre) ||
        !parse_semver(b, right, right_pre))
        return 0; /* caller rejects non-semver requirements separately */
    std::size_t count = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        long long l = i < left.size() ? left[i] : 0;
        long long r = i < right.size() ? right[i] : 0;
        if (l < r)
            return -1;
        if (l > r)
            return 1;
    }
    if (left_pre != right_pre)
        return left_pre ? -1 : 1;
    return a.compare(b);
}

bool has_version_suffix(const std::string &path) {
    std::size_t slash = path.rfind('/');
    return slash != std::string::npos &&
           is_version_suffix(path.substr(slash + 1));
}

std::string module_name_from_path(const std::string &path) {
    std::size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos
                           ? path
                           : path.substr(slash + 1);
    if (is_version_suffix(name) && slash != std::string::npos) {
        std::size_t previous = path.rfind('/', slash - 1);
        name = path.substr(previous == std::string::npos
                               ? 0
                               : previous + 1,
                           slash - (previous == std::string::npos
                                        ? 0
                                        : previous + 1));
    }
    return name;
}

bool is_valid_version(const std::string &version) {
    if (version.empty())
        return false;
    for (char c : version) {
        if (!version_char(c))
            return false;
    }
    return true;
}

bool is_valid_package_subpath(const std::string &subpath) {
    if (subpath.empty())
        return true;
    size_t start = 0;
    for (;;) {
        size_t slash = subpath.find('/', start);
        std::string part = subpath.substr(start, slash == std::string::npos
                                                     ? std::string::npos
                                                     : slash - start);
        if (part.empty() || !unicode_ident::is_identifier(part))
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

std::string cache_dir() {
    std::string env_cache;
    if (thread_env_get("SKEINMODCACHE", env_cache) && !env_cache.empty())
        return env_cache;
#ifdef _WIN32
    std::string local = env_or("LOCALAPPDATA", "");
    if (!local.empty())
        return text_of(path_of(local) / "skein" / "mod");
    std::string profile = env_or("USERPROFILE", "");
    if (!profile.empty())
        return text_of(path_of(profile) / ".skein" / "mod");
#else
    std::string home = env_or("HOME", "");
    if (!home.empty())
        return text_of(path_of(home) / ".skein" / "mod");
#endif
    return text_of(path_of(thread_cwd().empty() ? "." : thread_cwd()) /
                   ".skein" / "mod");
}

std::string cache_path(const std::string &path, const std::string &version) {
    const std::size_t slash = path.rfind('/');
    const std::string parent = slash == std::string::npos
                                   ? std::string()
                                   : path.substr(0, slash);
    /* /vN paths keep the suffix in the cache path but use the underlying
     * module name as the directory name. */
    const std::string name = module_name_from_path(path);
    fs::path result = path_of(cache_dir());
    if (!parent.empty())
        result /= path_of(parent);
    if (has_version_suffix(path))
        result /= path.substr(slash + 1);
    result /= version;
    result /= name;
    return text_of(result);
}

static const char *native_platform_key() {
#if defined(_WIN32)
    const char *os = "win";
#elif defined(__linux__)
    const char *os = "linux";
#elif defined(__APPLE__)
    const char *os = "macos";
#else
    const char *os = "other";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    const char *arch = "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char *arch = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    const char *arch = "x86";
#else
    const char *arch = "unknown";
#endif
#if defined(_MSC_VER)
    const char *compiler = "msvc";
#elif defined(__clang__)
    const char *compiler = "clang";
#elif defined(__GNUC__)
    const char *compiler = "gcc";
#else
    const char *compiler = "unknown";
#endif
    static std::string key = std::string(os) + "-" + arch + "-" + compiler;
    return key.c_str();
}

std::string native_build_dir(const std::string &path,
                             const std::string &version) {
    fs::path source = path_of(cache_path(path, version));
    return text_of(source.parent_path() / ".build" / native_platform_key() /
                   source.filename());
}

std::string native_library_name(const std::string &output) {
    const std::string name = output.empty() ? "skein_native" : output;
#if defined(_WIN32)
    return name + ".dll";
#elif defined(__APPLE__)
    return "lib" + name + ".dylib";
#else
    return "lib" + name + ".so";
#endif
}

bool dirhash(const std::string &directory, std::string &hash,
             std::string &error) {
    std::vector<std::pair<std::string, std::string>> files;
    std::function<void(const fs::path &, const std::string &)> walk =
        [&](const fs::path &dir, const std::string &prefix) {
            std::error_code ec;
            fs::directory_iterator it(dir, ec);
            if (ec)
                return;
            for (const fs::directory_entry &entry : it) {
                std::error_code entry_ec;
                fs::path full = entry.path();
                std::string name = text_of(full.filename());
                if (entry.is_directory(entry_ec)) {
                    if (name == ".git")
                        continue;
                    walk(full, prefix.empty() ? name : prefix + "/" + name);
                    continue;
                }
                if (!entry.is_regular_file(entry_ec))
                    continue;
                std::string relative = prefix.empty() ? name : prefix + "/" + name;
                files.push_back({relative, text_of(full)});
            }
        };
    walk(path_of(directory), std::string());
    std::sort(files.begin(), files.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    std::string manifest;
    for (const auto &item : files) {
        std::string data;
        if (!platform_read_file(item.second, data, error))
            return false;
        manifest += skein_hex_encode(skein_sha256_bytes(data));
        manifest += "  ";
        manifest += item.first;
        manifest += "\n";
    }
    hash = "h1:" + base64_encode(skein_sha256_bytes(manifest));
    return true;
}

}  // namespace modfile
}  // namespace skein
