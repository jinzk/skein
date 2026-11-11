#include "process_builtin.h"

#include "ast.h"
#include "platform.h"
#include "runtime_state.h"
#include "valuetype.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct temp_files {
    std::string input;
    std::string output;
    std::string error;
    std::string output_path;
    std::string error_path;

    ~temp_files() {
        std::error_code ignored;
        if (!input.empty())
            fs::remove(fs::u8path(input), ignored);
        if (!output_path.empty())
            fs::remove(fs::u8path(output_path), ignored);
        if (!error_path.empty())
            fs::remove(fs::u8path(error_path), ignored);
    }
};

std::string temp_path(const char *suffix) {
    std::random_device device;
    const uint64_t value =
        ((uint64_t)device() << 32) ^ (uint64_t)device() ^
        (uint64_t)std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count();
    char name[96];
    std::snprintf(name, sizeof name, "skein-%016llx-%s",
                  (unsigned long long)value, suffix);
    std::error_code error;
    const fs::path directory = fs::temp_directory_path(error);
    if (error)
        return std::string();
    return (directory / fs::u8path(name)).u8string();
}

std::string quote_posix(const std::string &text) {
    std::string result = "'";
    for (char c : text) {
        if (c == '\'')
            result += "'\\''";
        else
            result.push_back(c);
    }
    result.push_back('\'');
    return result;
}

#ifdef _WIN32
std::string quote_windows(const std::string &text) {
    std::string result = "\"";
    for (char c : text) {
        if (c == '"')
            result += "\"\"";
        else
            result.push_back(c);
    }
    result.push_back('"');
    return result;
}
#endif

std::string quote_for_shell(const std::string &text) {
#ifdef _WIN32
    return quote_windows(text);
#else
    return quote_posix(text);
#endif
}

bool collect_env(const struct value_t *value,
                 std::vector<std::pair<std::string, std::string>> &env) {
    if (value->type != T_dict) {
        yyerror("std.process.run_command env must be a dict, got %s",
                typename_of(value));
        return false;
    }
    struct dict_value_t *dict = (struct dict_value_t *)value;
    for (const auto &entry : dict->payload->items) {
        if (entry.first->type != T_string || entry.second->type != T_string) {
            yyerror("std.process.run_command env keys and values must be strings");
            return false;
        }
        env.push_back(std::make_pair(
            ((struct string_value_t *)entry.first)->value,
            ((struct string_value_t *)entry.second)->value));
    }
    return true;
}

}  // namespace

struct value_t *call_process_builtin(
    int functype, const std::vector<struct value_t *> &args) {
    if (functype != B_process_run_command) {
        yyerror("unknown process function");
        return NULL;
    }
    if (args.size() < 1 || args.size() > 2) {
        yyerror("std.process.run_command expects 1 or 2 arguments, got %d",
                (int)args.size());
        return NULL;
    }
    if (args[0]->type != T_string) {
        yyerror("std.process.run_command expects a command string, got %s",
                typename_of(args[0]));
        return NULL;
    }

    std::string stdin_data;
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env;
    if (args.size() == 2) {
        if (args[1]->type != T_dict) {
            yyerror("std.process.run_command options must be a dict, got %s",
                    typename_of(args[1]));
            return NULL;
        }
        struct dict_value_t *options = (struct dict_value_t *)args[1];
        for (const auto &entry : options->payload->items) {
            if (entry.first->type != T_string)
                continue;
            const std::string key =
                ((struct string_value_t *)entry.first)->value;
            if (key == "stdin") {
                if (entry.second->type == T_string) {
                    stdin_data =
                        ((struct string_value_t *)entry.second)->value;
                } else if (entry.second->type == T_bytes) {
                    stdin_data =
                        ((struct bytes_value_t *)entry.second)->value;
                } else {
                    yyerror("std.process.run_command stdin must be string or bytes");
                    return NULL;
                }
            } else if (key == "cwd") {
                if (entry.second->type != T_string) {
                    yyerror("std.process.run_command cwd must be a string");
                    return NULL;
                }
                cwd = ((struct string_value_t *)entry.second)->value;
            } else if (key == "env") {
                if (!collect_env(entry.second, env))
                    return NULL;
            }
        }
    }

    {
        std::vector<std::pair<std::string, std::string>> overrides;
        thread_env_overrides(overrides);
        for (const auto &override : overrides) {
            bool replaced = false;
            for (auto &entry : env) {
                if (entry.first == override.first) {
                    entry.second = override.second;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                env.push_back(override);
        }
    }
    if (cwd.empty())
        cwd = thread_cwd();
    else
        cwd = thread_resolve_path(cwd);

    temp_files files;
    files.input = temp_path("stdin");
    files.output_path = temp_path("stdout");
    files.error_path = temp_path("stderr");
    if (files.input.empty() || files.output_path.empty() ||
        files.error_path.empty()) {
        yyerror("std.process.run_command: cannot create temporary files");
        return NULL;
    }

    std::string error;
    if (!platform_write_file(files.input, stdin_data, false, error)) {
        yyerror("std.process.run_command: %s", error.c_str());
        return NULL;
    }

    std::string command = ((struct string_value_t *)args[0])->value;
    std::string shell = "(" + command + ") < " +
                        quote_for_shell(files.input) + " > " +
                        quote_for_shell(files.output_path) + " 2> " +
                        quote_for_shell(files.error_path);
    if (!cwd.empty()) {
#ifdef _WIN32
        shell = "cd /d " + quote_for_shell(cwd) + " && " + shell;
#else
        shell = "cd " + quote_for_shell(cwd) + " && " + shell;
#endif
    }

    int status = -1;
    if (!platform_run_command_status(shell, env, status)) {
        yyerror("std.process.run_command: command failed to start");
        return NULL;
    }

    std::string stdout_text;
    std::string stderr_text;
    if (!platform_read_file(files.output_path, stdout_text, error) ||
        !platform_read_file(files.error_path, stderr_text, error)) {
        yyerror("std.process.run_command: %s", error.c_str());
        return NULL;
    }

    struct dict_value_t *result = newdictval();
    result->payload->items.push_back(std::make_pair(
        newstrval("status"), newintval((long long)status)));
    result->payload->items.push_back(std::make_pair(
        newstrval("stdout"), newbytesval(std::move(stdout_text))));
    result->payload->items.push_back(std::make_pair(
        newstrval("stderr"), newbytesval(std::move(stderr_text))));
    return (value_t *)result;
}
