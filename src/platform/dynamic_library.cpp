#include "dynamic_library.h"

#include "platform.h"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

#ifdef _WIN32

std::string windows_error_message(DWORD code) {
    wchar_t *buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, (LPWSTR)&buffer, 0, nullptr);
    std::string result;
    if (length && buffer) {
        std::wstring text(buffer, (size_t)length);
        if (!platform_wide_to_utf8(text, result))
            result = "error " + std::to_string(code);
    } else {
        result = "error " + std::to_string(code);
    }
    if (buffer)
        LocalFree(buffer);
    while (!result.empty() &&
           (result.back() == '\r' || result.back() == '\n'))
        result.pop_back();
    return result.empty() ? "error " + std::to_string(code) : result;
}

#else

std::string posix_error() {
    const char *message = dlerror();
    return message ? message : "unknown dynamic loader error";
}

#endif

} // namespace

dynamic_library::dynamic_library(void *handle, std::string path)
    : handle_(handle), path_(std::move(path)) {}

dynamic_library::~dynamic_library() {
    if (!handle_)
        return;
#ifdef _WIN32
    FreeLibrary((HMODULE)handle_);
#else
    dlclose(handle_);
#endif
}

std::shared_ptr<dynamic_library> dynamic_library::open(
    const std::string &path, std::string &error) {
    error.clear();
#ifdef _WIN32
    std::wstring wide;
    if (!platform_utf8_to_wide(path, wide)) {
        error = "path is not valid UTF-8";
        return nullptr;
    }
    SetLastError(0);
    HMODULE handle = LoadLibraryW(wide.c_str());
    if (!handle) {
        error = windows_error_message(GetLastError());
        return nullptr;
    }
    return std::shared_ptr<dynamic_library>(
        new dynamic_library((void *)handle, path));
#else
    dlerror();
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        error = posix_error();
        return nullptr;
    }
    return std::shared_ptr<dynamic_library>(
        new dynamic_library(handle, path));
#endif
}

void *dynamic_library::symbol(const char *name, std::string &error) const {
    error.clear();
    if (!handle_) {
        error = "library is closed";
        return nullptr;
    }
    if (!name || !*name) {
        error = "empty symbol name";
        return nullptr;
    }
#ifdef _WIN32
    SetLastError(0);
    FARPROC proc = GetProcAddress((HMODULE)handle_, name);
    if (!proc) {
        error = windows_error_message(GetLastError());
        return nullptr;
    }
    return (void *)proc;
#else
    dlerror();
    void *result = dlsym(handle_, name);
    if (!result) {
        error = posix_error();
        return nullptr;
    }
    return result;
#endif
}

bool dynamic_library::valid() const {
    return handle_ != nullptr;
}

const std::string &dynamic_library::path() const {
    return path_;
}
