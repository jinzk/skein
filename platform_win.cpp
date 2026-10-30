#include "platform.h"

#include <cstdio>
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
