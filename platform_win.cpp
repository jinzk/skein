#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include "repl.h"

namespace {

class scoped_environment {
    struct saved_value {
        std::string name;
        std::string value;
        bool existed;
    };

    std::vector<saved_value> saved_;
    bool ok_ = true;

public:
    explicit scoped_environment(
        const std::vector<std::pair<std::string, std::string>> &env) {
        for (const auto &entry : env) {
            const char *old = std::getenv(entry.first.c_str());
            saved_.push_back(
                {entry.first, old ? old : "", old != nullptr});
            if (_putenv_s(entry.first.c_str(), entry.second.c_str()) != 0) {
                ok_ = false;
                break;
            }
        }
    }

    ~scoped_environment() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            if (it->existed)
                _putenv_s(it->name.c_str(), it->value.c_str());
            else
                _putenv_s(it->name.c_str(), "");
        }
    }

    bool ok() const { return ok_; }
};

}  // namespace

static HANDLE main_thread = NULL;

typedef BOOL(WINAPI *CancelSynchronousIoFn)(HANDLE);

static CancelSynchronousIoFn cancel_synchronous_io() {
    static CancelSynchronousIoFn fn = [] {
        FARPROC proc = GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                      "CancelSynchronousIo");
        CancelSynchronousIoFn result = nullptr;
        static_assert(sizeof(result) == sizeof(proc),
                      "unexpected function pointer size");
        std::memcpy(&result, &proc, sizeof result);
        return result;
    }();
    return fn;
}

static BOOL WINAPI on_console_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_interrupted = 1;
        if (main_thread) {
            CancelSynchronousIoFn cancel = cancel_synchronous_io();
            if (cancel)
                cancel(main_thread);
        }
        FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
        return TRUE;
    }
    return FALSE;
}

void platform_init() {
    /* Keep the interpreter's UTF-8 byte stream contract on Windows. */
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    /* Piped UTF-8 input should not be treated as Windows text (0x1A/CRLF). */
    if (!_isatty(_fileno(stdin)))
        _setmode(_fileno(stdin), _O_BINARY);
}

void platform_install_interrupt_handler() {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
}

bool platform_run_command(
    const std::string &command,
    const std::vector<std::pair<std::string, std::string>> &env,
    std::string &output, int &status) {
    output.clear();
    status = -1;

    scoped_environment scope(env);
    if (!scope.ok())
        return false;

    FILE *pipe = _popen(command.c_str(), "r");
    if (!pipe)
        return false;

    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0)
        output.append(buffer, n);

    int rc = _pclose(pipe);
    if (rc != -1)
        status = rc;

    /* Normalize Windows line endings to the interpreter's '\n'. */
    std::string normalized;
    normalized.reserve(output.size());
    for (size_t i = 0; i < output.size(); ++i) {
        if (output[i] == '\r' && i + 1 < output.size() &&
            output[i + 1] == '\n')
            continue;
        normalized.push_back(output[i]);
    }
    output.swap(normalized);

    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return true;
}

bool platform_run_command_status(
    const std::string &command,
    const std::vector<std::pair<std::string, std::string>> &env,
    int &status) {
    status = -1;

    scoped_environment scope(env);
    if (!scope.ok())
        return false;

    int rc = std::system(command.c_str());
    if (rc == -1)
        return false;
    status = rc;
    return true;
}
