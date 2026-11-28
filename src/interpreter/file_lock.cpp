#include "file_lock.h"

#include "modfile.h"
#include "path.h"
#include "platform.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

long long lock_timeout_ms() {
    std::string value;
    if (!platform_get_env("SKEIN_LOCK_TIMEOUT_MS", value) || value.empty())
        return 0;
    char *end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || parsed <= 0)
        return 0;
    return parsed;
}

/* Collision only serializes unrelated modules, never causes missed locking. */
std::string lock_file_name(const std::string &module,
                           const std::string &version) {
    const std::string key = module + "@" + version;
    unsigned long long hash = 0xCBF29CE484222325ULL;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char digits[17];
    std::snprintf(digits, sizeof(digits), "%016llx", hash);
    return std::string(digits) + ".lock";
}

}  // namespace

file_lock::~file_lock() { release(); }

bool file_lock::acquire(const std::string &file, const std::string &label,
                        lock_mode mode, std::string &error) {
    std::error_code ec;
    const fs::path path = skein::path_from_utf8(file);
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "cannot create lock directory: " +
                    skein::path_to_utf8(path.parent_path()) + ": " +
                    ec.message();
            return false;
        }
    }

#ifdef _WIN32
    const std::wstring wide = path.wstring();
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "cannot open lock for " + label + ": windows error " +
                std::to_string(GetLastError());
        return false;
    }

    const DWORD flags =
        mode == lock_mode::exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    const long long timeout_ms = lock_timeout_ms();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    bool waiting_printed = false;
    for (;;) {
        OVERLAPPED overlapped = {};
        if (LockFileEx(handle, flags | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                       &overlapped))
            break;
        const DWORD code = GetLastError();
        if (code != ERROR_LOCK_VIOLATION) {
            error = "cannot lock " + label + ": windows error " +
                    std::to_string(code);
            CloseHandle(handle);
            return false;
        }
        if (!waiting_printed) {
            std::fprintf(stderr, "skein: waiting for %s...\n", label.c_str());
            waiting_printed = true;
        }
        if (timeout_ms <= 0) {
            OVERLAPPED waiting = {};
            if (!LockFileEx(handle, flags, 0, 1, 0, &waiting)) {
                error = "cannot lock " + label + ": windows error " +
                        std::to_string(GetLastError());
                CloseHandle(handle);
                return false;
            }
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "timed out waiting for " + label;
            CloseHandle(handle);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    handle_ = static_cast<void *>(handle);
#else
    fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) {
        error = "cannot open lock for " + label + ": " +
                std::strerror(errno);
        return false;
    }
    const int operation =
        mode == lock_mode::exclusive ? LOCK_EX : LOCK_SH;
    const long long timeout_ms = lock_timeout_ms();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    bool waiting_printed = false;
    for (;;) {
        if (flock(fd_, operation | LOCK_NB) == 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            error = "cannot lock " + label + ": " + std::strerror(errno);
            close(fd_);
            fd_ = -1;
            return false;
        }
        if (!waiting_printed) {
            std::fprintf(stderr, "skein: waiting for %s...\n", label.c_str());
            waiting_printed = true;
        }
        if (timeout_ms <= 0) {
            if (flock(fd_, operation) != 0) {
                if (errno == EINTR)
                    continue;
                error = "cannot lock " + label + ": " +
                        std::strerror(errno);
                close(fd_);
                fd_ = -1;
                return false;
            }
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "timed out waiting for " + label;
            close(fd_);
            fd_ = -1;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
    locked_ = true;
    return true;
}

void file_lock::release() {
    if (!locked_)
        return;
#ifdef _WIN32
    HANDLE handle = static_cast<HANDLE>(handle_);
    OVERLAPPED overlapped = {};
    UnlockFileEx(handle, 0, 1, 0, &overlapped);
    CloseHandle(handle);
    handle_ = nullptr;
#else
    flock(fd_, LOCK_UN);
    close(fd_);
    fd_ = -1;
#endif
    locked_ = false;
}

bool acquire_module_lock(const std::string &module, const std::string &version,
                         file_lock &lock, lock_mode mode, std::string &error) {
    const fs::path directory =
        skein::path_from_utf8(skein::modfile::cache_dir()) / ".locks";
    const fs::path file = directory / lock_file_name(module, version);
    return lock.acquire(skein::path_to_utf8(file),
                        "module " + module + "@" + version, mode, error);
}

bool acquire_project_lock(const std::string &root, file_lock &lock,
                          lock_mode mode, std::string &error) {
    const fs::path file = skein::path_from_utf8(root) / ".skein" / "lock";
    return lock.acquire(skein::path_to_utf8(file), "project lock " + root, mode,
                        error);
}

bool acquire_cache_lock(file_lock &lock, lock_mode mode, std::string &error) {
    /* Keep the lock beside the cache root, not inside it, so `mod clean` can
     * remove the root while still holding the lock. */
    fs::path file = skein::path_from_utf8(skein::modfile::cache_dir());
    if (!file.has_filename())
        file = file.parent_path();
    file += ".lock";
    return lock.acquire(skein::path_to_utf8(file), "module cache lock", mode,
                        error);
}

bool acquire_runtime_cache_lock(std::string &error) {
    static std::mutex mutex;
    static file_lock lock;
    static bool acquired = false;
    static std::string acquired_path;

    std::lock_guard<std::mutex> guard(mutex);
    const std::string current = skein::modfile::cache_dir();
    if (acquired) {
        if (current == acquired_path)
            return true;
        error = "module cache root changed after runtime lock was acquired";
        return false;
    }
    if (!acquire_cache_lock(lock, lock_mode::shared, error))
        return false;
    acquired = true;
    acquired_path = current;
    return true;
}
