#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>

#include "repl.h"

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

bool platform_stdin_is_tty() {
    return _isatty(_fileno(stdin)) != 0;
}

void platform_install_interrupt_handler() {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
}

namespace {

bool widen_utf8(const std::string &text, std::wstring &wide) {
    wide.clear();
    if (text.empty())
        return true;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    text.data(), (int)text.size(),
                                    nullptr, 0);
    if (count <= 0)
        return false;
    wide.resize((size_t)count);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               text.data(), (int)text.size(),
                               wide.data(), count) == count;
}

bool narrow_utf8(const std::wstring &wide, std::string &text) {
    text.clear();
    if (wide.empty())
        return true;
    int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                    (int)wide.size(), nullptr, 0,
                                    nullptr, nullptr);
    if (count <= 0)
        return false;
    text.resize((size_t)count);
    return WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(),
                               text.data(), count, nullptr,
                               nullptr) == count;
}

std::wstring command_processor() {
    wchar_t buffer[32768];
    DWORD length = GetEnvironmentVariableW(
        L"ComSpec", buffer, (DWORD)(sizeof buffer / sizeof buffer[0]));
    if (length > 0 && length < sizeof buffer / sizeof buffer[0])
        return std::wstring(buffer, length);
    return L"cmd.exe";
}

bool build_command_line(const std::string &command, std::wstring &application,
                        std::wstring &line) {
    std::wstring wide_command;
    if (!widen_utf8(command, wide_command))
        return false;
    application = command_processor();
    line = L"\"" + application + L"\" /D /S /C \"" + wide_command + L"\"";
    return true;
}

bool equal_case_insensitive(const std::wstring &a, const std::wstring &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i]))
            return false;
    }
    return true;
}

bool less_case_insensitive(const std::wstring &a, const std::wstring &b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        wint_t left = std::towlower(a[i]);
        wint_t right = std::towlower(b[i]);
        if (left != right)
            return left < right;
    }
    return a.size() < b.size();
}

struct environment_entry {
    std::wstring name;
    std::wstring value;
};

bool build_environment(
    const std::vector<std::pair<std::string, std::string>> &overrides,
    std::wstring &block) {
    std::vector<environment_entry> entries;

    LPWSTR raw = GetEnvironmentStringsW();
    if (raw) {
        for (LPWSTR p = raw; *p; p += std::wcslen(p) + 1) {
            std::wstring item(p);
            size_t equals;
            if (!item.empty() && item[0] == L'=')
                equals = item.find(L'=', 1);
            else
                equals = item.find(L'=');
            if (equals == std::wstring::npos)
                continue;
            entries.push_back(
                {item.substr(0, equals), item.substr(equals + 1)});
        }
        FreeEnvironmentStringsW(raw);
    }

    for (const auto &override_entry : overrides) {
        std::wstring name;
        std::wstring value;
        if (!widen_utf8(override_entry.first, name) ||
            !widen_utf8(override_entry.second, value))
            return false;

        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                           [&](const environment_entry &entry) {
                               return equal_case_insensitive(entry.name, name);
                           }),
            entries.end());
        entries.push_back({name, value});
    }

    std::sort(entries.begin(), entries.end(),
              [](const environment_entry &a, const environment_entry &b) {
                  return less_case_insensitive(a.name, b.name);
              });

    block.clear();
    for (const environment_entry &entry : entries) {
        block += entry.name;
        block += L'=';
        block += entry.value;
        block.push_back(L'\0');
    }
    if (entries.empty())
        block.push_back(L'\0');
    block.push_back(L'\0');
    return true;
}

HANDLE duplicate_inheritable(HANDLE source) {
    if (!source || source == INVALID_HANDLE_VALUE)
        return NULL;
    HANDLE duplicate = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                         &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return NULL;
    return duplicate;
}

void close_handle(HANDLE &handle) {
    if (handle && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    handle = NULL;
}

void normalize_output(std::string &output) {
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
}

bool spawn_shell(const std::string &command,
                 const std::vector<std::pair<std::string, std::string>> &env,
                 bool capture_output, HANDLE &read_pipe,
                 PROCESS_INFORMATION &process) {
    read_pipe = NULL;
    process = {};

    std::wstring application;
    std::wstring line;
    std::wstring environment;
    if (!build_command_line(command, application, line) ||
        !build_environment(env, environment))
        return false;

    HANDLE pipe_read = NULL;
    HANDLE pipe_write = NULL;
    if (capture_output) {
        SECURITY_ATTRIBUTES attributes = {};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        if (!CreatePipe(&pipe_read, &pipe_write, &attributes, 0))
            return false;
        if (!SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0)) {
            close_handle(pipe_read);
            close_handle(pipe_write);
            return false;
        }
    }

    HANDLE dup_in = duplicate_inheritable(GetStdHandle(STD_INPUT_HANDLE));
    HANDLE dup_err = duplicate_inheritable(GetStdHandle(STD_ERROR_HANDLE));
    HANDLE dup_out = capture_output
                         ? NULL
                         : duplicate_inheritable(GetStdHandle(STD_OUTPUT_HANDLE));

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput =
        dup_in ? dup_in : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdError =
        dup_err ? dup_err : GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdOutput =
        capture_output ? pipe_write
                       : (dup_out ? dup_out
                                  : GetStdHandle(STD_OUTPUT_HANDLE));

    std::vector<wchar_t> line_buffer(line.begin(), line.end());
    line_buffer.push_back(L'\0');
    std::vector<wchar_t> environment_buffer(environment.begin(),
                                            environment.end());

    BOOL ok = CreateProcessW(
        application.c_str(), line_buffer.data(), NULL, NULL, TRUE,
        CREATE_UNICODE_ENVIRONMENT, environment_buffer.data(), NULL, &startup,
        &process);

    close_handle(pipe_write);
    if (dup_in)
        CloseHandle(dup_in);
    if (dup_err)
        CloseHandle(dup_err);
    if (dup_out)
        CloseHandle(dup_out);

    if (!ok) {
        close_handle(pipe_read);
        process = {};
        return false;
    }

    read_pipe = pipe_read;
    return true;
}

}  // namespace

std::vector<std::string> platform_command_line_args(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int wide_argc = 0;
    LPWSTR *wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv)
        return {};

    std::vector<std::string> args;
    args.reserve((size_t)wide_argc);
    for (int i = 0; i < wide_argc; ++i) {
        std::string arg;
        if (!narrow_utf8(wide_argv[i], arg))
            arg.clear();
        args.push_back(std::move(arg));
    }
    LocalFree(wide_argv);
    return args;
}

bool platform_get_env(const std::string &name, std::string &value) {
    value.clear();

    std::wstring wide_name;
    if (!widen_utf8(name, wide_name))
        return false;

    DWORD needed = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (needed == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return false;
        return true;
    }

    std::wstring wide_value((size_t)needed, L'\0');
    DWORD written = GetEnvironmentVariableW(
        wide_name.c_str(), wide_value.data(), needed);
    if (written == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return false;
        return true;
    }

    wide_value.resize((size_t)written);
    return narrow_utf8(wide_value, value);
}

bool platform_run_command(
    const std::string &command,
    const std::vector<std::pair<std::string, std::string>> &env,
    std::string &output, int &status) {
    output.clear();
    status = -1;

    HANDLE read_pipe = NULL;
    PROCESS_INFORMATION process = {};
    if (!spawn_shell(command, env, true, read_pipe, process))
        return false;

    char buffer[4096];
    DWORD received = 0;
    while (ReadFile(read_pipe, buffer, sizeof buffer, &received, NULL) &&
           received > 0)
        output.append(buffer, received);
    close_handle(read_pipe);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(process.hProcess, &code))
        status = (int)code;

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    normalize_output(output);
    return true;
}

bool platform_run_command_status(
    const std::string &command,
    const std::vector<std::pair<std::string, std::string>> &env,
    int &status) {
    status = -1;

    HANDLE read_pipe = NULL;
    PROCESS_INFORMATION process = {};
    if (!spawn_shell(command, env, false, read_pipe, process))
        return false;

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(process.hProcess, &code))
        status = (int)code;

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

FILE *platform_open_file(const std::string &path, const char *mode) {
    std::wstring wide_path;
    if (!widen_utf8(path, wide_path))
        return NULL;
    std::wstring wide_mode;
    for (const char *p = mode; *p; ++p)
        wide_mode.push_back((wchar_t)(unsigned char)*p);
    return _wfopen(wide_path.c_str(), wide_mode.c_str());
}

bool platform_read_file(const std::string &path, std::string &data,
                        std::string &error) {
    data.clear();
    error.clear();

    std::wstring wide_path;
    if (!widen_utf8(path, wide_path)) {
        error = "invalid file path";
        return false;
    }

    FILE *file = _wfopen(wide_path.c_str(), L"rb");
    if (!file) {
        error = std::strerror(errno);
        return false;
    }

    char buffer[4096];
    size_t received;
    while ((received = std::fread(buffer, 1, sizeof buffer, file)) > 0)
        data.append(buffer, received);

    if (std::ferror(file)) {
        int saved = errno;
        std::fclose(file);
        error = std::strerror(saved);
        return false;
    }
    if (std::fclose(file) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool platform_write_file(const std::string &path, const std::string &data,
                         bool append, std::string &error) {
    error.clear();

    std::wstring wide_path;
    if (!widen_utf8(path, wide_path)) {
        error = "invalid file path";
        return false;
    }

    FILE *file = _wfopen(wide_path.c_str(), append ? L"ab" : L"wb");
    if (!file) {
        error = std::strerror(errno);
        return false;
    }

    size_t written = std::fwrite(data.data(), 1, data.size(), file);
    if (written != data.size()) {
        int saved = errno;
        std::fclose(file);
        error = std::strerror(saved);
        return false;
    }
    if (std::fclose(file) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool platform_file_exists(const std::string &path) {
    std::wstring wide_path;
    if (!widen_utf8(path, wide_path))
        return false;
    return GetFileAttributesW(wide_path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct platform_file {
    FILE *stream = nullptr;
    std::string path;

    ~platform_file() {
        if (stream)
            std::fclose(stream);
    }
};

platform_file_ptr platform_file_open(const std::string &path,
                                     const std::string &mode,
                                     std::string &error) {
    error.clear();
    std::wstring wide_path;
    if (!widen_utf8(path, wide_path)) {
        error = "invalid file path";
        return nullptr;
    }
    std::wstring wide_mode(mode.begin(), mode.end());
    FILE *stream = _wfopen(wide_path.c_str(), wide_mode.c_str());
    if (!stream) {
        error = std::strerror(errno);
        return nullptr;
    }
    std::setvbuf(stream, nullptr, _IONBF, 0);
    std::shared_ptr<platform_file> file = std::make_shared<platform_file>();
    file->stream = stream;
    file->path = path;
    return file;
}

bool platform_file_read(const platform_file_ptr &file, size_t limit,
                        std::string &data, std::string &error) {
    data.clear();
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return false;
    }

    char buffer[4096];
    size_t total = 0;
    while (limit == 0 || total < limit) {
        size_t want = sizeof buffer;
        if (limit != 0 && limit - total < want)
            want = limit - total;
        size_t received = std::fread(buffer, 1, want, file->stream);
        if (received > 0) {
            data.append(buffer, received);
            total += received;
        }
        if (received < want) {
            if (std::ferror(file->stream)) {
                error = std::strerror(errno);
                return false;
            }
            break;
        }
    }
    return true;
}

bool platform_file_write(const platform_file_ptr &file,
                         const std::string &data, std::string &error) {
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return false;
    }
    size_t written = 0;
    while (written < data.size()) {
        size_t n = std::fwrite(data.data() + written, 1,
                               data.size() - written, file->stream);
        if (n == 0) {
            error = std::strerror(errno);
            return false;
        }
        written += n;
    }
    return true;
}

bool platform_file_seek(const platform_file_ptr &file, long long offset,
                        int whence, std::string &error) {
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return false;
    }
    if (whence < 0 || whence > 2) {
        error = "invalid whence";
        return false;
    }
    if (_fseeki64(file->stream, offset, whence) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

long long platform_file_tell(const platform_file_ptr &file,
                             std::string &error) {
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return -1;
    }
    long long position = _ftelli64(file->stream);
    if (position < 0) {
        error = std::strerror(errno);
        return -1;
    }
    return (long long)position;
}

bool platform_file_eof(const platform_file_ptr &file) {
    return file && file->stream && std::feof(file->stream) != 0;
}

bool platform_file_flush(const platform_file_ptr &file, std::string &error) {
    error.clear();
    if (!file || !file->stream) {
        error = "file is closed";
        return false;
    }
    if (std::fflush(file->stream) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

void platform_file_close(const platform_file_ptr &file, std::string &error) {
    error.clear();
    if (!file || !file->stream)
        return;
    if (std::fclose(file->stream) != 0)
        error = std::strerror(errno);
    file->stream = nullptr;
}

bool platform_file_is_open(const platform_file_ptr &file) {
    return file && file->stream != nullptr;
}

int platform_file_fd(const platform_file_ptr &file) {
    if (!file || !file->stream)
        return -1;
    return _fileno(file->stream);
}

std::string platform_file_path(const platform_file_ptr &file) {
    return file ? file->path : std::string();
}
