#include "runtime_state.h"

#include "path.h"
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

volatile std::sig_atomic_t g_interrupted = 0;
volatile std::sig_atomic_t g_stdin_eof = 0;
volatile std::sig_atomic_t g_drain_requested = 0;

thread_local bool g_interactive = true;
thread_local bool g_show_prompt = false;
thread_local int g_package_loading = 0;
thread_local bool g_keep_going = false;
thread_local bool g_script_stop = false;
thread_local bool g_statement_invalid = false;

namespace {

constexpr long long kDefaultDrainTimeoutMs = 20000;

/* Cached SKEIN_DRAIN_TIMEOUT_MS result; -1 means "not read yet". */
std::atomic<long long> g_drain_timeout{-1};
/* Monotonic start of the drain deadline; 0 means "not started yet". */
std::atomic<long long> g_drain_started_ms{0};
/* Set once the runtime finished shutting down. */
std::atomic<bool> g_drain_finished{false};

long long monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void drain_request() { g_drain_requested = 1; }

bool drain_requested() { return g_drain_requested != 0; }

long long drain_timeout_ms() {
    long long cached = g_drain_timeout.load(std::memory_order_relaxed);
    if (cached >= 0)
        return cached;

    long long value = kDefaultDrainTimeoutMs;
    const char *env = std::getenv("SKEIN_DRAIN_TIMEOUT_MS");
    if (env && *env) {
        char *end = nullptr;
        errno = 0;
        long long parsed = std::strtoll(env, &end, 10);
        if (errno == 0 && end && *end == '\0' && parsed >= 0)
            value = parsed;
    }
    g_drain_timeout.store(value, std::memory_order_relaxed);
    return value;
}

long long drain_remaining_ms() {
    if (!drain_requested())
        return -1;

    const long long limit = drain_timeout_ms();
    if (limit <= 0)
        return -1;

    const long long now = monotonic_ms();
    long long started = g_drain_started_ms.load(std::memory_order_acquire);
    if (started == 0) {
        long long expected = 0;
        if (g_drain_started_ms.compare_exchange_strong(
                expected, now, std::memory_order_acq_rel))
            started = now;
        else
            started = expected;
    }

    const long long elapsed = now > started ? now - started : 0;
    if (elapsed >= limit)
        return 0;
    return limit - elapsed;
}

bool drain_expired() { return drain_remaining_ms() == 0; }

void drain_finish() { g_drain_finished.store(true, std::memory_order_release); }

bool drain_finished() {
    return g_drain_finished.load(std::memory_order_acquire);
}

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
                               return state.removed.contains(entry.first);
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
    return skein::path_to_utf8(path);
}

const std::string &thread_cwd_override() {
    return environment().cwd;
}

bool thread_chdir(const std::string &path, std::string &error) {
    const std::string resolved = thread_resolve_path(path);
    std::error_code fs_error;
    fs::path target = skein::path_from_utf8(resolved);
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
    state.cwd = skein::path_to_utf8(target.lexically_normal());
    return true;
}

std::string thread_resolve_path(const std::string &path) {
    if (path.empty())
        return path;
    fs::path value = skein::path_from_utf8(path);
    if (value.is_absolute())
        return path;
    const std::string base = thread_cwd();
    if (base.empty())
        return path;
    return skein::path_to_utf8(
        (skein::path_from_utf8(base) / value).lexically_normal());
}

namespace {

std::vector<std::string> g_sys_args;
std::string g_sys_script;
std::string g_sys_executable;
std::atomic<int> g_sys_exit_code{-1};

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

void sys_exit_request(int code) {
    g_sys_exit_code.store(code);
    g_interrupted = 1;
    g_script_stop = true;
}

bool sys_exit_requested() { return g_sys_exit_code.load() >= 0; }

int sys_exit_code() { return g_sys_exit_code.load(); }
