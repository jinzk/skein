#include "fs_extra.h"

#include "ast.h"
#include "valuetype.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

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
                text_path(((struct string_value_t *)args[0])->value);
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
                text_path(((struct string_value_t *)args[0])->value);
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
                text_path(((struct string_value_t *)args[0])->value);
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
                text_path(((struct string_value_t *)args[0])->value);
            const fs::path target =
                text_path(((struct string_value_t *)args[1])->value);
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
                text_path(((struct string_value_t *)args[0])->value);
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
            std::error_code error;
            const fs::path path = fs::current_path(error);
            if (error) {
                yyerror("std.fs.cwd: %s", path_error(error).c_str());
                return NULL;
            }
            return newstrval(path_text(path).c_str());
        }
        case B_fs_chdir: {
            if (args.size() != 1) {
                yyerror("std.fs.chdir expects 1 argument, got %d",
                        (int)args.size());
                return NULL;
            }
            if (!require_string("std.fs.chdir", args[0], "a path"))
                return NULL;
            std::error_code error;
            fs::current_path(
                text_path(((struct string_value_t *)args[0])->value),
                error);
            if (error) {
                yyerror("std.fs.chdir: %s", path_error(error).c_str());
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
                result = fs::absolute(path, error);
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
        default:
            yyerror("unknown fs function");
            return NULL;
    }
}
