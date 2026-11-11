#include "fs_extra.h"

#include "ast.h"
#include "runtime_state.h"
#include "valuetype.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

bool bool_argument(const struct value_t *value, bool &result) {
    if (value->type == T_bool) {
        result = ((struct bool_value_t *)value)->value;
        return true;
    }
    if (value->type == T_int) {
        result = ((struct int_value_t *)value)->value != 0;
        return true;
    }
    return false;
}

std::string path_text(const fs::path &path) {
    return path.u8string();
}

fs::path text_path(const std::string &text) {
    return fs::u8path(text);
}

fs::path resolved_path(const std::string &text) {
    return text_path(thread_resolve_path(text));
}

std::string path_error(const std::error_code &error) {
    return error.message();
}

struct value_t *make_string_list(const std::vector<std::string> &items) {
    struct list_value_t *result = newlistval();
    result->payload->items.reserve(items.size());
    for (const std::string &item : items)
        result->payload->items.push_back(newstrval(item.c_str()));
    return (value_t *)result;
}

bool require_string(const char *who, const struct value_t *value,
                    const char *what) {
    if (value->type == T_string)
        return true;
    yyerror("%s expects %s string, got %s", who, what, typename_of(value));
    return false;
}

std::vector<std::string> split_path_list(const std::string &text,
                                         char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t end = text.find(separator, start);
        if (end == std::string::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

bool is_executable_file(const fs::path &path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error)
        return false;
#if defined(_WIN32)
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
}

bool wildcard_match(const std::string &pattern, const std::string &text) {
    size_t pattern_pos = 0;
    size_t text_pos = 0;
    size_t star = std::string::npos;
    size_t star_text = 0;
    while (text_pos < text.size()) {
        if (pattern_pos < pattern.size() &&
            (pattern[pattern_pos] == '?' ||
             pattern[pattern_pos] == text[text_pos])) {
            ++pattern_pos;
            ++text_pos;
        } else if (pattern_pos < pattern.size() &&
                   pattern[pattern_pos] == '*') {
            star = pattern_pos++;
            star_text = text_pos;
        } else if (star != std::string::npos) {
            pattern_pos = star + 1;
            text_pos = ++star_text;
        } else {
            return false;
        }
    }
    while (pattern_pos < pattern.size() && pattern[pattern_pos] == '*')
        ++pattern_pos;
    return pattern_pos == pattern.size();
}

std::string unique_suffix() {
    static std::atomic<unsigned long long> counter{0};
    static std::mt19937_64 engine([]() {
        std::random_device device;
        const unsigned long long seed =
            ((unsigned long long)device() << 32) ^ device();
        return seed == 0 ? 1ULL : seed;
    }());
    const unsigned long long serial = counter.fetch_add(1);
    const unsigned long long stamp =
        (unsigned long long)std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
    const unsigned long long value = engine() ^ stamp ^ (serial << 8);
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int shift = 60; shift >= 0; shift -= 4)
        out.push_back(digits[(value >> shift) & 0xF]);
    return out;
}

}  // namespace

struct value_t *call_fs_extra_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    switch (functype) {
        case B_fs_listdir: {
            if (args.size() != 1) {
                yyerror("std.fs.listdir expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.listdir", args[0], "a path"))
                return NULL;
            const fs::path directory =
                resolved_path(((struct string_value_t *)args[0])->value);
            std::error_code error;
            if (!fs::is_directory(directory, error)) {
                yyerror("std.fs.listdir: %s",
                        error ? path_error(error).c_str()
                              : "not a directory");
                return NULL;
            }
            std::vector<std::string> names;
            fs::directory_iterator it(directory, error);
            if (error) {
                yyerror("std.fs.listdir: %s", path_error(error).c_str());
                return NULL;
            }
            const fs::directory_iterator end;
            while (it != end) {
                names.push_back(it->path().filename().u8string());
                it.increment(error);
                if (error) {
                    yyerror("std.fs.listdir: %s",
                            path_error(error).c_str());
                    return NULL;
                }
            }
            std::sort(names.begin(), names.end());
            return make_string_list(names);
        }
        case B_fs_mkdir: {
            if (args.size() < 1 || args.size() > 2) {
                yyerror("std.fs.mkdir expects 1 or 2 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.mkdir", args[0], "a path"))
                return NULL;
            bool recursive = false;
            if (args.size() == 2 && !bool_argument(args[1], recursive)) {
                yyerror("std.fs.mkdir expects a bool recursive flag, got %s",
                        typename_of(args[1]));
                return NULL;
            }
            const fs::path path =
                resolved_path(((struct string_value_t *)args[0])->value);
            std::error_code error;
            bool created = recursive
                               ? fs::create_directories(path, error)
                               : fs::create_directory(path, error);
            if (error) {
                yyerror("std.fs.mkdir: %s", path_error(error).c_str());
                return NULL;
            }
            if (!created) {
                std::error_code exists_error;
                if (!fs::is_directory(path, exists_error)) {
                    yyerror("std.fs.mkdir: path exists and is not a "
                            "directory");
                    return NULL;
                }
            }
            return newnull();
        }
        case B_fs_remove: {
            if (args.size() != 1) {
                yyerror("std.fs.remove expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.remove", args[0], "a path"))
                return NULL;
            const fs::path path =
                resolved_path(((struct string_value_t *)args[0])->value);
            std::error_code error;
            const bool removed = fs::remove(path, error);
            if (error || !removed) {
                yyerror("std.fs.remove: %s",
                        error ? path_error(error).c_str()
                              : "path does not exist");
                return NULL;
            }
            return newnull();
        }
        case B_fs_rename: {
            if (args.size() != 2) {
                yyerror("std.fs.rename expects 2 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.rename", args[0], "a source path") ||
                !require_string("std.fs.rename", args[1], "a target path"))
                return NULL;
            const fs::path source =
                resolved_path(((struct string_value_t *)args[0])->value);
            const fs::path target =
                resolved_path(((struct string_value_t *)args[1])->value);
            std::error_code error;
            fs::rename(source, target, error);
            if (error) {
                yyerror("std.fs.rename: %s", path_error(error).c_str());
                return NULL;
            }
            return newnull();
        }
        case B_fs_stat: {
            if (args.size() != 1) {
                yyerror("std.fs.stat expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.stat", args[0], "a path"))
                return NULL;
            const fs::path path =
                resolved_path(((struct string_value_t *)args[0])->value);
            std::error_code error;
            const fs::file_status status = fs::status(path, error);
            if (error || !fs::exists(status)) {
                yyerror("std.fs.stat: %s",
                        error ? path_error(error).c_str()
                              : "path does not exist");
                return NULL;
            }
            struct dict_value_t *result = newdictval();
            const char *type = "other";
            if (fs::is_regular_file(status))
                type = "file";
            else if (fs::is_directory(status))
                type = "dir";
            result->payload->items.push_back(std::make_pair(
                newstrval("type"), newstrval(type)));
            uintmax_t size = 0;
            if (fs::is_regular_file(status))
                size = fs::file_size(path, error);
            if (error) {
                freeval((value_t *)result);
                yyerror("std.fs.stat: %s", path_error(error).c_str());
                return NULL;
            }
            result->payload->items.push_back(std::make_pair(
                newstrval("size"), newintval((long long)size)));
            const fs::file_time_type file_time =
                fs::last_write_time(path, error);
            if (error) {
                freeval((value_t *)result);
                yyerror("std.fs.stat: %s", path_error(error).c_str());
                return NULL;
            }
            const auto system_time =
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    file_time - fs::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
            const std::time_t timestamp =
                std::chrono::system_clock::to_time_t(system_time);
            result->payload->items.push_back(std::make_pair(
                newstrval("mtime"), newintval((long long)timestamp)));
            return (value_t *)result;
        }
        case B_fs_cwd: {
            if (!args.empty()) {
                yyerror("std.fs.cwd expects 0 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            const std::string cwd = thread_cwd();
            if (cwd.empty()) {
                yyerror("std.fs.cwd: cannot read working directory");
                return NULL;
            }
            return newstrval(cwd.c_str());
        }
        case B_fs_chdir: {
            if (args.size() != 1) {
                yyerror("std.fs.chdir expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.chdir", args[0], "a path"))
                return NULL;
            std::string chdir_error;
            if (!thread_chdir(
                    ((struct string_value_t *)args[0])->value,
                    chdir_error)) {
                yyerror("std.fs.chdir: %s", chdir_error.c_str());
                return NULL;
            }
            return newnull();
        }
        case B_fs_path_join: {
            if (args.empty()) {
                yyerror("std.fs.path_join expects at least 1 argument");
                return NULL;
            }
            fs::path result;
            for (const struct value_t *arg : args) {
                if (!require_string("std.fs.path_join", arg, "a path"))
                    return NULL;
                result /= text_path(
                    ((struct string_value_t *)arg)->value);
            }
            return newstrval(path_text(result).c_str());
        }
        case B_fs_path_basename:
        case B_fs_path_dirname:
        case B_fs_path_extname:
        case B_fs_path_abs:
        case B_fs_path_normalize: {
            if (args.size() != 1) {
                yyerror("std.fs path function expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs path function", args[0], "a path"))
                return NULL;
            const fs::path path =
                text_path(((struct string_value_t *)args[0])->value);
            fs::path result;
            if (functype == B_fs_path_basename) {
                result = path.filename();
            } else if (functype == B_fs_path_dirname) {
                result = path.parent_path();
                if (result.empty())
                    result = ".";
            } else if (functype == B_fs_path_extname) {
                result = path.extension();
            } else if (functype == B_fs_path_abs) {
                std::error_code error;
                result = fs::absolute(
                    resolved_path(
                        ((struct string_value_t *)args[0])->value),
                    error);
                if (error) {
                    yyerror("std.fs.path_abs: %s",
                            path_error(error).c_str());
                    return NULL;
                }
            } else {
                result = path.lexically_normal();
            }
            return newstrval(path_text(result).c_str());
        }
        case B_fs_temp_dir: {
            if (args.size() > 1) {
                yyerror("std.fs.temp_dir expects 0 or 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            std::string prefix = "skein-";
            if (!args.empty()) {
                if (!require_string("std.fs.temp_dir", args[0], "a prefix"))
                    return NULL;
                prefix = ((struct string_value_t *)args[0])->value;
                if (prefix.empty())
                    prefix = "skein-";
                if (prefix.find('/') != std::string::npos ||
                    prefix.find('\\') != std::string::npos) {
                    yyerror("std.fs.temp_dir prefix must not contain path "
                            "separators");
                    return NULL;
                }
            }
            std::error_code error;
            std::string temp_base;
            if (!thread_env_get("TMPDIR", temp_base) || temp_base.empty()) {
                if (!thread_env_get("TMP", temp_base) || temp_base.empty())
                    thread_env_get("TEMP", temp_base);
            }
            fs::path base;
            if (!temp_base.empty())
                base = resolved_path(temp_base);
            else
                base = fs::temp_directory_path(error);
            if (error) {
                yyerror("std.fs.temp_dir: %s", path_error(error).c_str());
                return NULL;
            }
            for (int attempt = 0; attempt < 100; ++attempt) {
                const fs::path candidate = base / (prefix + unique_suffix());
                std::error_code create_error;
                if (fs::create_directory(candidate, create_error))
                    return newstrval(path_text(candidate).c_str());
                if (create_error &&
                    create_error != std::errc::file_exists) {
                    yyerror("std.fs.temp_dir: %s",
                            path_error(create_error).c_str());
                    return NULL;
                }
            }
            yyerror("std.fs.temp_dir: cannot create a unique directory");
            return NULL;
        }
        case B_fs_remove_all: {
            if (args.size() != 1) {
                yyerror("std.fs.remove_all expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.remove_all", args[0], "a path"))
                return NULL;
            std::error_code error;
            fs::remove_all(
                resolved_path(((struct string_value_t *)args[0])->value),
                error);
            if (error) {
                yyerror("std.fs.remove_all: %s", path_error(error).c_str());
                return NULL;
            }
            return newnull();
        }
        case B_fs_copy: {
            if (args.size() != 2) {
                yyerror("std.fs.copy expects 2 arguments, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.copy", args[0], "a source path") ||
                !require_string("std.fs.copy", args[1], "a target path"))
                return NULL;
            const fs::path source =
                resolved_path(((struct string_value_t *)args[0])->value);
            fs::path target =
                resolved_path(((struct string_value_t *)args[1])->value);
            std::error_code error;
            const fs::file_status status = fs::symlink_status(source, error);
            if (error || !fs::exists(status)) {
                yyerror("std.fs.copy: source does not exist");
                return NULL;
            }
            if (fs::is_directory(status)) {
                std::error_code exists_error;
                if (fs::exists(target, exists_error)) {
                    yyerror("std.fs.copy: destination already exists");
                    return NULL;
                }
                fs::copy(source, target, fs::copy_options::recursive, error);
            } else if (fs::is_regular_file(status)) {
                std::error_code target_error;
                if (fs::is_directory(target, target_error))
                    target /= source.filename();
                fs::copy_file(source, target,
                              fs::copy_options::overwrite_existing, error);
            } else {
                yyerror("std.fs.copy: unsupported source type");
                return NULL;
            }
            if (error) {
                yyerror("std.fs.copy: %s", path_error(error).c_str());
                return NULL;
            }
            return newnull();
        }
        case B_fs_which: {
            if (args.size() != 1) {
                yyerror("std.fs.which expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.which", args[0], "a name"))
                return NULL;
            const std::string name =
                ((struct string_value_t *)args[0])->value;
            if (name.empty())
                return newstrval("");
            const fs::path direct = text_path(name);
            if (direct.has_parent_path()) {
                const fs::path resolved = resolved_path(name);
                if (is_executable_file(resolved))
                    return newstrval(path_text(resolved).c_str());
                return newstrval("");
            }
            std::string path_env;
            if (!thread_env_get("PATH", path_env))
                return newstrval("");
            std::vector<std::string> extensions;
#if defined(_WIN32)
            if (direct.has_extension()) {
                extensions.push_back("");
            } else {
                std::string pathext;
                if (!thread_env_get("PATHEXT", pathext) || pathext.empty())
                    pathext = ".COM;.EXE;.BAT;.CMD";
                extensions = split_path_list(pathext, ';');
                extensions.push_back("");
            }
            const char separator = ';';
#else
            extensions.push_back("");
            const char separator = ':';
#endif
            for (const std::string &dir : split_path_list(path_env,
                                                          separator)) {
                const fs::path base =
                    dir.empty() ? fs::path(".") : text_path(dir);
                for (const std::string &extension : extensions) {
                    const fs::path candidate = base / (name + extension);
                    const fs::path resolved =
                        resolved_path(path_text(candidate));
                    if (is_executable_file(resolved))
                        return newstrval(path_text(resolved).c_str());
                }
            }
            return newstrval("");
        }
        case B_fs_glob: {
            if (args.size() != 1) {
                yyerror("std.fs.glob expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.glob", args[0], "a pattern"))
                return NULL;
            const fs::path pattern = text_path(thread_resolve_path(
                ((struct string_value_t *)args[0])->value));
            fs::path parent = pattern.parent_path();
            if (parent.empty())
                parent = ".";
            const std::string name_pattern = pattern.filename().u8string();
            std::vector<std::string> matches;
            std::error_code error;
            if (!name_pattern.empty() && fs::is_directory(parent, error)) {
                fs::directory_iterator it(parent, error);
                fs::directory_iterator end;
                while (!error && it != end) {
                    const std::string name =
                        it->path().filename().u8string();
                    if (wildcard_match(name_pattern, name))
                        matches.push_back(path_text(it->path()));
                    it.increment(error);
                }
            }
            std::sort(matches.begin(), matches.end());
            return make_string_list(matches);
        }
        default:
            yyerror("unknown fs function");
            return NULL;
    }
}
