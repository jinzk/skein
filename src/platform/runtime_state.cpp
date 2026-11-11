#include "runtime_state.h"

#include "platform.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

volatile std::sig_atomic_t g_interrupted = 0;
volatile std::sig_atomic_t g_stdin_eof = 0;

thread_local bool g_interactive = true;
thread_local bool g_show_prompt = false;
thread_local int g_package_loading = 0;
thread_local bool g_keep_going = false;
thread_local bool g_script_stop = false;
thread_local bool g_statement_invalid = false;

namespace {

struct thread_environment {
    std::unordered_map<std::string, std::string> values;
    std::unordered_set<std::string> removed;
    bool has_cwd = false;
    std::string cwd;
};

thread_environment &environment() {
    static thread_local thread_environment state;
    return state;
}

}  // namespace

struct thread_env_snapshot {
    thread_environment state;
};

std::shared_ptr<const thread_env_snapshot> thread_env_capture() {
    std::shared_ptr<thread_env_snapshot> snapshot =
        std::make_shared<thread_env_snapshot>();
    snapshot->state = environment();
    return snapshot;
}

void thread_env_install(
    const std::shared_ptr<const thread_env_snapshot> &snapshot) {
    if (snapshot)
        environment() = snapshot->state;
}

bool thread_env_valid_name(const std::string &name) {
    return !name.empty() && name.find('=') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

bool thread_env_get(const std::string &name, std::string &value) {
    thread_environment &state = environment();
    if (state.removed.count(name))
        return false;
    auto it = state.values.find(name);
    if (it != state.values.end()) {
        value = it->second;
        return true;
    }
    return platform_get_env(name, value);
}

void thread_env_set(const std::string &name, const std::string &value) {
    thread_environment &state = environment();
    state.removed.erase(name);
    state.values[name] = value;
}

void thread_env_unset(const std::string &name) {
    thread_environment &state = environment();
    state.values.erase(name);
    state.removed.insert(name);
}

void thread_env_all(
    std::vector<std::pair<std::string, std::string>> &entries) {
    entries.clear();
    platform_environ(entries);
    thread_environment &state = environment();

    if (!state.removed.empty()) {
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                           [&state](const auto &entry) {
                               return state.removed.count(entry.first) != 0;
                           }),
            entries.end());
    }

    for (const auto &override : state.values) {
        bool replaced = false;
        for (auto &entry : entries) {
            if (entry.first == override.first) {
                entry.second = override.second;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            entries.emplace_back(override.first, override.second);
    }
}

void thread_env_overrides(
    std::vector<std::pair<std::string, std::string>> &entries) {
    entries.clear();
    const thread_environment &state = environment();
    entries.reserve(state.values.size());
    for (const auto &entry : state.values)
        entries.push_back(entry);
}

void thread_env_unsets(std::vector<std::string> &names) {
    names.clear();
    const thread_environment &state = environment();
    names.reserve(state.removed.size());
    for (const std::string &name : state.removed)
        names.push_back(name);
}

std::string thread_cwd() {
    thread_environment &state = environment();
    if (state.has_cwd)
        return state.cwd;
    std::error_code error;
    fs::path path = fs::current_path(error);
    if (error)
        return std::string();
    return path.u8string();
}

const std::string &thread_cwd_override() {
    return environment().cwd;
}

bool thread_chdir(const std::string &path, std::string &error) {
    const std::string resolved = thread_resolve_path(path);
    std::error_code fs_error;
    fs::path target = fs::u8path(resolved);
    if (!fs::is_directory(target, fs_error) || fs_error) {
        error = fs_error ? fs_error.message() : "not a directory";
        return false;
    }
    target = fs::absolute(target, fs_error);
    if (fs_error) {
        error = fs_error.message();
        return false;
    }
    thread_environment &state = environment();
    state.has_cwd = true;
    state.cwd = target.lexically_normal().u8string();
    return true;
}

std::string thread_resolve_path(const std::string &path) {
    if (path.empty())
        return path;
    fs::path value = fs::u8path(path);
    if (value.is_absolute())
        return path;
    const std::string base = thread_cwd();
    if (base.empty())
        return path;
    return (fs::u8path(base) / value).lexically_normal().u8string();
}

namespace {

std::vector<std::string> g_sys_args;
std::string g_sys_script;
std::string g_sys_executable;

}  // namespace

void sys_invocation_set(std::vector<std::string> args, std::string script,
                        std::string executable) {
    g_sys_args = std::move(args);
    g_sys_script = std::move(script);
    g_sys_executable = std::move(executable);
}

const std::vector<std::string> &sys_args() { return g_sys_args; }

const std::string &sys_script() { return g_sys_script; }

const std::string &sys_executable() { return g_sys_executable; }
