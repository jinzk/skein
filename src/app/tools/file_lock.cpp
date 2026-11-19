#include "file_lock.h"

#include "modfile.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

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
    const fs::path path = fs::u8path(file);
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "cannot create lock directory: " +
                    path.parent_path().u8string() + ": " + ec.message();
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

    DWORD flags = mode == lock_mode::exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    OVERLAPPED overlapped = {};
    if (!LockFileEx(handle, flags | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                    &overlapped)) {
        const DWORD code = GetLastError();
        if (code != ERROR_LOCK_VIOLATION) {
            error = "cannot lock " + label + ": windows error " +
                    std::to_string(code);
            CloseHandle(handle);
            return false;
        }
        std::fprintf(stderr, "skein: waiting for %s...\n", label.c_str());
        OVERLAPPED waiting = {};
        if (!LockFileEx(handle, flags, 0, 1, 0, &waiting)) {
            error = "cannot lock " + label + ": windows error " +
                    std::to_string(GetLastError());
            CloseHandle(handle);
            return false;
        }
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
    if (flock(fd_, operation | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            error = "cannot lock " + label + ": " + std::strerror(errno);
            close(fd_);
            fd_ = -1;
            return false;
        }
        std::fprintf(stderr, "skein: waiting for %s...\n", label.c_str());
        if (flock(fd_, operation) != 0) {
            error = "cannot lock " + label + ": " + std::strerror(errno);
            close(fd_);
            fd_ = -1;
            return false;
        }
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
        fs::u8path(skein::modfile::cache_dir()) / ".locks";
    const fs::path file = directory / lock_file_name(module, version);
    return lock.acquire(file.u8string(),
                        "module " + module + "@" + version, mode, error);
}

bool acquire_project_lock(const std::string &root, file_lock &lock,
                          lock_mode mode, std::string &error) {
    const fs::path file = fs::u8path(root) / ".skein" / "lock";
    return lock.acquire(file.u8string(), "project lock " + root, mode, error);
}

bool acquire_cache_lock(file_lock &lock, lock_mode mode, std::string &error) {
    /* Keep the lock beside the cache root, not inside it, so `mod clean` can
     * remove the root while still holding the lock. */
    fs::path file = fs::u8path(skein::modfile::cache_dir());
    if (!file.has_filename())
        file = file.parent_path();
    file += ".lock";
    return lock.acquire(file.u8string(), "module cache lock", mode, error);
}
