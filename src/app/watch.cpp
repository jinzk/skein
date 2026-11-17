#include "watch.h"

#include "runtime_state.h"

#include <uv.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <chrono>
#endif

#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif

namespace {

namespace fs = std::filesystem;

constexpr int kPollIntervalMs = 200;
constexpr int kDebounceMs = 120;
constexpr int kKillGraceMs = 2000;

struct file_stamp {
    /*
     * Modification time as 100 ns ticks on Windows and nanoseconds elsewhere.
     * MinGW's fs::last_write_time is backed by _wstat64 and only reports whole
     * seconds, which misses back-to-back saves of equal size, so the Windows
     * path reads the native FILETIME instead.
     */
    std::int64_t modified = 0;
    std::uintmax_t size = 0;

    bool operator==(const file_stamp &other) const {
        return modified == other.modified && size == other.size;
    }
};

bool read_write_stamp(const fs::path &path, std::int64_t &stamp) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return false;
    stamp = (static_cast<std::int64_t>(data.ftLastWriteTime.dwHighDateTime)
             << 32) |
            static_cast<std::int64_t>(data.ftLastWriteTime.dwLowDateTime);
    return true;
#else
    std::error_code error;
    const fs::file_time_type modified = fs::last_write_time(path, error);
    if (error)
        return false;
    stamp = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            modified.time_since_epoch())
            .count());
    return true;
#endif
}

using file_snapshot = std::map<std::string, file_stamp>;

bool ignored_directory(const fs::path &path) {
    const std::string name = path.filename().u8string();
    if (name == ".git" || name == ".hg" || name == ".svn" ||
        name == ".cache" || name == "__pycache__" ||
        name == "node_modules" || name == "dist" || name == "target" ||
        name == "out") {
        return true;
    }
    /* Build trees contain no source files but can be huge.  A directory
     * explicitly passed as a watch root is still scanned even if its own
     * name looks like a build directory. */
    return name == "build" || name.rfind("build-", 0) == 0;
}

bool is_skein_file(const fs::path &path) {
    std::string extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return extension == ".skein";
}

void add_file(file_snapshot &snapshot, const fs::path &path) {
    std::int64_t modified = 0;
    if (!read_write_stamp(path, modified))
        return;
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error)
        return;
    snapshot[path.lexically_normal().u8string()] =
        file_stamp{modified, size};
}

file_snapshot scan_roots(const std::vector<std::string> &roots) {
    file_snapshot snapshot;
    for (const std::string &root : roots) {
        const fs::path root_path = fs::u8path(root);
        std::error_code error;
        if (fs::is_regular_file(root_path, error)) {
            if (is_skein_file(root_path))
                add_file(snapshot, root_path);
            continue;
        }
        if (!fs::is_directory(root_path, error))
            continue;

        fs::recursive_directory_iterator entry(
            root_path, fs::directory_options::skip_permission_denied, error);
        const fs::recursive_directory_iterator end;
        while (!error && entry != end) {
            const fs::directory_entry &item = *entry;
            std::error_code item_error;
            if (item.is_directory(item_error)) {
                if (ignored_directory(item.path()))
                    entry.disable_recursion_pending();
            } else if (item.is_regular_file(item_error) &&
                       is_skein_file(item.path())) {
                add_file(snapshot, item.path());
            }
            entry.increment(error);
        }
    }
    return snapshot;
}

std::vector<std::string> diff_snapshot(const file_snapshot &previous,
                                       const file_snapshot &current) {
    std::vector<std::string> changed;
    for (const auto &entry : current) {
        auto old = previous.find(entry.first);
        if (old == previous.end() || !(old->second == entry.second))
            changed.push_back(entry.first);
    }
    for (const auto &entry : previous) {
        if (current.find(entry.first) == current.end())
            changed.push_back(entry.first);
    }
    return changed;
}

std::string display_path(const std::string &path) {
    std::error_code error;
    const fs::path relative =
        fs::relative(fs::u8path(path), fs::current_path(error), error);
    return error ? path : relative.u8string();
}

std::string format_changes(const std::vector<std::string> &changes) {
    constexpr std::size_t limit = 5;
    std::string text;
    const std::size_t shown = std::min(changes.size(), limit);
    for (std::size_t i = 0; i < shown; ++i) {
        if (!text.empty())
            text += ", ";
        text += display_path(changes[i]);
    }
    if (changes.size() > shown)
        text += " (+" + std::to_string(changes.size() - shown) + " more)";
    return text;
}

struct watch_state {
    watch_options options;
    uv_loop_t loop{};
    uv_timer_t poll_timer{};
    uv_timer_t debounce_timer{};
    uv_timer_t kill_timer{};
    uv_process_t *child = nullptr;
    file_snapshot files;
    bool stopping = false;
    bool restart_pending = false;
    bool terminating = false;
    bool timers_closed = false;
};

void start_child(watch_state &state);

void on_process_closed(uv_handle_t *handle) {
    delete reinterpret_cast<uv_process_t *>(handle);
}

void close_timers(watch_state &state) {
    if (state.timers_closed)
        return;
    state.timers_closed = true;
    uv_timer_stop(&state.poll_timer);
    uv_timer_stop(&state.debounce_timer);
    uv_timer_stop(&state.kill_timer);
    uv_close(reinterpret_cast<uv_handle_t *>(&state.poll_timer), nullptr);
    uv_close(reinterpret_cast<uv_handle_t *>(&state.debounce_timer), nullptr);
    uv_close(reinterpret_cast<uv_handle_t *>(&state.kill_timer), nullptr);
}

void on_kill_timeout(uv_timer_t *timer) {
    watch_state &state = *static_cast<watch_state *>(timer->data);
    if (state.child && state.terminating)
        uv_process_kill(state.child, SIGKILL);
}

void begin_terminate(watch_state &state) {
    if (!state.child || state.terminating)
        return;
    state.terminating = true;
    if (uv_process_kill(state.child, SIGTERM) < 0) {
        /* The exit callback will still run if the process is already gone. */
    }
    uv_timer_start(&state.kill_timer, on_kill_timeout, kKillGraceMs, 0);
}

void begin_stop(watch_state &state) {
    if (state.stopping)
        return;
    state.stopping = true;
    state.restart_pending = false;
    uv_timer_stop(&state.poll_timer);
    uv_timer_stop(&state.debounce_timer);
    if (state.child)
        begin_terminate(state);
    else
        close_timers(state);
}

void on_exit(uv_process_t *process, int64_t exit_status, int term_signal) {
    watch_state &state = *static_cast<watch_state *>(process->data);
    state.child = nullptr;
    state.terminating = false;
    uv_timer_stop(&state.kill_timer);
    uv_close(reinterpret_cast<uv_handle_t *>(process), on_process_closed);

    if (state.stopping) {
        close_timers(state);
        return;
    }

    if (state.restart_pending) {
        state.restart_pending = false;
        start_child(state);
        return;
    }

    if (term_signal != 0) {
        std::fprintf(stderr, "[watch] child stopped by signal %d\n",
                     term_signal);
    } else {
        std::fprintf(stderr, "[watch] child exited with status %lld\n",
                     static_cast<long long>(exit_status));
    }
    std::fprintf(stderr,
                 "[watch] waiting for changes before starting it again\n");
}

void start_child(watch_state &state) {
    if (state.stopping || state.child)
        return;

    std::vector<char *> argv;
    argv.reserve(state.options.command.size() + 1);
    for (const std::string &argument : state.options.command)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);

    uv_stdio_container_t stdio[3]{};
    for (int i = 0; i < 3; ++i) {
        stdio[i].flags = UV_INHERIT_FD;
        stdio[i].data.fd = i;
    }

    uv_process_options_t options{};
    options.exit_cb = on_exit;
    options.file = state.options.executable.c_str();
    options.args = argv.data();
    options.stdio_count = 3;
    options.stdio = stdio;

    auto *process = new uv_process_t();
    const int result = uv_spawn(&state.loop, process, &options);
    process->data = &state;
    if (result < 0) {
        std::fprintf(stderr, "[watch] cannot start %s: %s\n",
                     state.options.executable.c_str(),
                     uv_strerror(result));
        uv_close(reinterpret_cast<uv_handle_t *>(process),
                 on_process_closed);
        return;
    }

    state.child = process;
    std::fprintf(stderr, "[watch] started pid %d\n",
                 static_cast<int>(process->pid));
}

void on_debounce(uv_timer_t *timer) {
    watch_state &state = *static_cast<watch_state *>(timer->data);
    if (state.stopping)
        return;
    if (state.child) {
        state.restart_pending = true;
        std::fprintf(stderr, "[watch] restarting\n");
        begin_terminate(state);
    } else {
        std::fprintf(stderr, "[watch] starting\n");
        start_child(state);
    }
}

void on_poll(uv_timer_t *timer) {
    watch_state &state = *static_cast<watch_state *>(timer->data);
    if (g_interrupted || drain_requested()) {
        begin_stop(state);
        return;
    }

    file_snapshot current = scan_roots(state.options.roots);
    std::vector<std::string> changed =
        diff_snapshot(state.files, current);
    if (changed.empty())
        return;

    state.files = std::move(current);
    std::fprintf(stderr, "[watch] changed: %s\n",
                 format_changes(changed).c_str());
    if (state.stopping)
        return;
    uv_timer_start(&state.debounce_timer, on_debounce, kDebounceMs, 0);
}

} // namespace

int run_watch_mode(const watch_options &options) {
    if (options.executable.empty() || options.command.empty()) {
        std::fprintf(stderr, "[watch] no command to run\n");
        return 2;
    }

    for (const std::string &root : options.roots) {
        std::error_code error;
        if (!fs::is_directory(fs::u8path(root), error)) {
            std::fprintf(stderr, "[watch] not a directory: %s\n",
                         root.c_str());
            return 2;
        }
    }

    watch_state state;
    state.options = options;
    state.files = scan_roots(state.options.roots);

    if (uv_loop_init(&state.loop) != 0) {
        std::fprintf(stderr, "[watch] cannot initialize event loop\n");
        return 1;
    }
    uv_timer_init(&state.loop, &state.poll_timer);
    uv_timer_init(&state.loop, &state.debounce_timer);
    uv_timer_init(&state.loop, &state.kill_timer);
    state.poll_timer.data = &state;
    state.debounce_timer.data = &state;
    state.kill_timer.data = &state;

    std::fprintf(stderr, "[watch] watching");
    for (std::size_t i = 0; i < state.options.roots.size(); ++i) {
        std::fprintf(stderr, "%s%s", i == 0 ? " " : ", ",
                     state.options.roots[i].c_str());
    }
    std::fprintf(stderr, "\n");

    g_interrupted = 0;
    start_child(state);
    uv_timer_start(&state.poll_timer, on_poll, kPollIntervalMs,
                   kPollIntervalMs);
    uv_run(&state.loop, UV_RUN_DEFAULT);
    uv_loop_close(&state.loop);

    return g_interrupted ? 130 : 0;
}
