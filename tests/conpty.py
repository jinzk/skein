"""Minimal ConPTY driver for the interactive REPL tests.

Windows has no POSIX pty module, but Windows 10 1809+ exposes a pseudo
console through CreatePseudoConsole.  The child sees a real console (so
GetConsoleMode, ReadConsoleInputW and VT output all work); the parent talks to
it through two pipes.  This lets the same popup assertions run on Windows.
"""

import ctypes
import msvcrt
import os
import subprocess
import threading
import time
from ctypes import wintypes

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

EXTENDED_STARTUPINFO_PRESENT = 0x00080000
PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
STARTF_USESTDHANDLES = 0x00000100

HPCON = wintypes.HANDLE


class COORD(ctypes.Structure):
    _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("lpReserved", wintypes.LPWSTR),
        ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR),
        ("dwX", wintypes.DWORD),
        ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD),
        ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD),
        ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD),
        ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
        ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE),
        ("hStdError", wintypes.HANDLE),
    ]


class STARTUPINFOEXW(ctypes.Structure):
    _fields_ = [
        ("StartupInfo", STARTUPINFOW),
        ("lpAttributeList", ctypes.c_void_p),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("hProcess", wintypes.HANDLE),
        ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
    ]


class SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("nLength", wintypes.DWORD),
        ("lpSecurityDescriptor", ctypes.c_void_p),
        ("bInheritHandle", wintypes.BOOL),
    ]


kernel32.CreatePseudoConsole.argtypes = [
    COORD, wintypes.HANDLE, wintypes.HANDLE, wintypes.DWORD,
    ctypes.POINTER(HPCON),
]
kernel32.CreatePseudoConsole.restype = ctypes.c_long
kernel32.ResizePseudoConsole.argtypes = [HPCON, COORD]
kernel32.ResizePseudoConsole.restype = ctypes.c_long
kernel32.ClosePseudoConsole.argtypes = [HPCON]
kernel32.InitializeProcThreadAttributeList.argtypes = [
    ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
    ctypes.POINTER(ctypes.c_size_t),
]
kernel32.InitializeProcThreadAttributeList.restype = wintypes.BOOL
kernel32.UpdateProcThreadAttribute.argtypes = [
    ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t, ctypes.c_void_p,
    ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p,
]
kernel32.UpdateProcThreadAttribute.restype = wintypes.BOOL
kernel32.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
kernel32.CreatePipe.argtypes = [
    ctypes.POINTER(wintypes.HANDLE), ctypes.POINTER(wintypes.HANDLE),
    ctypes.c_void_p, wintypes.DWORD,
]
kernel32.CreatePipe.restype = wintypes.BOOL
kernel32.CreateProcessW.argtypes = [
    wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
    wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
    ctypes.c_void_p, ctypes.POINTER(PROCESS_INFORMATION),
]
kernel32.CreateProcessW.restype = wintypes.BOOL
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.GetExitCodeProcess.argtypes = [
    wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
]
kernel32.GetExitCodeProcess.restype = wintypes.BOOL
kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
kernel32.TerminateProcess.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


def conpty_available():
    return hasattr(kernel32, "CreatePseudoConsole")


def _check(ok, what):
    if not ok:
        raise OSError(ctypes.get_last_error(), what)


class ConPtySession:
    def __init__(self, argv, cwd=None, columns=80, rows=24):
        self._buffer = bytearray()
        self._lock = threading.Lock()
        self._process = wintypes.HANDLE()
        self._pseudo = HPCON()
        self._in_fd = -1
        self._out_fd = -1
        self._reader = None

        if not conpty_available():
            raise OSError(0, "CreatePseudoConsole is unavailable")

        input_read = wintypes.HANDLE()
        input_write = wintypes.HANDLE()
        output_read = wintypes.HANDLE()
        output_write = wintypes.HANDLE()
        security = SECURITY_ATTRIBUTES()
        security.nLength = ctypes.sizeof(security)
        security.bInheritHandle = True
        _check(kernel32.CreatePipe(ctypes.byref(input_read),
                                   ctypes.byref(input_write),
                                   ctypes.cast(ctypes.byref(security),
                                               ctypes.c_void_p), 0),
               "CreatePipe(input)")
        _check(kernel32.CreatePipe(ctypes.byref(output_read),
                                   ctypes.byref(output_write),
                                   ctypes.cast(ctypes.byref(security),
                                               ctypes.c_void_p), 0),
               "CreatePipe(output)")

        size = COORD(columns, rows)
        hres = kernel32.CreatePseudoConsole(
            size, input_read, output_write, 0, ctypes.byref(self._pseudo))
        if hres < 0:
            raise OSError(hres, "CreatePseudoConsole")
        # The pseudoconsole owns the child side of both pipes now.
        kernel32.CloseHandle(input_read)
        kernel32.CloseHandle(output_write)

        attribute_size = ctypes.c_size_t(0)
        kernel32.InitializeProcThreadAttributeList(
            None, 1, 0, ctypes.byref(attribute_size))
        attribute_buffer = ctypes.create_string_buffer(attribute_size.value)
        _check(kernel32.InitializeProcThreadAttributeList(
                   attribute_buffer, 1, 0, ctypes.byref(attribute_size)),
               "InitializeProcThreadAttributeList")
        try:
            _check(kernel32.UpdateProcThreadAttribute(
                       attribute_buffer, 0,
                       PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                       ctypes.c_void_p(self._pseudo.value),
                       ctypes.sizeof(HPCON), None, None),
                   "UpdateProcThreadAttribute")

            startup = STARTUPINFOEXW()
            startup.StartupInfo.cb = ctypes.sizeof(startup)
            # The parent's stdio is redirected under CI, and Windows would
            # duplicate those handles into the child, overriding the
            # pseudoconsole.  Using STARTF_USESTDHANDLES with null handles
            # keeps stdio on the pseudoconsole.
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES
            startup.lpAttributeList = ctypes.cast(attribute_buffer,
                                                  ctypes.c_void_p)
            process_info = PROCESS_INFORMATION()
            command_line = subprocess.list2cmdline(argv)
            _check(kernel32.CreateProcessW(
                       None, ctypes.create_unicode_buffer(command_line),
                       None, None, False, EXTENDED_STARTUPINFO_PRESENT,
                       None, cwd, ctypes.byref(startup),
                       ctypes.byref(process_info)),
                   "CreateProcessW")
        finally:
            kernel32.DeleteProcThreadAttributeList(attribute_buffer)

        kernel32.CloseHandle(process_info.hThread)
        self._process = process_info.hProcess

        self._in_fd = msvcrt.open_osfhandle(
            int(input_write.value), os.O_WRONLY | os.O_BINARY)
        self._out_fd = msvcrt.open_osfhandle(
            int(output_read.value), os.O_RDONLY | os.O_BINARY)
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        while True:
            try:
                chunk = os.read(self._out_fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            with self._lock:
                self._buffer.extend(chunk)

    def send(self, data):
        os.write(self._in_fd, data)

    def drain(self, seconds):
        time.sleep(seconds)

    def buffer(self):
        with self._lock:
            return bytes(self._buffer)

    def resize(self, columns=100, rows=30):
        kernel32.ResizePseudoConsole(self._pseudo, COORD(columns, rows))

    def wait(self, timeout=10.0):
        milliseconds = int(timeout * 1000)
        result = kernel32.WaitForSingleObject(self._process, milliseconds)
        if result != 0:  # WAIT_OBJECT_0
            return None
        code = wintypes.DWORD()
        if not kernel32.GetExitCodeProcess(self._process, ctypes.byref(code)):
            return None
        return int(code.value)

    def close(self):
        if self.wait(0) is None:
            kernel32.TerminateProcess(self._process, 1)
            self.wait(5)
        if self._in_fd >= 0:
            os.close(self._in_fd)
            self._in_fd = -1
        if self._pseudo:
            kernel32.ClosePseudoConsole(self._pseudo)
            self._pseudo = HPCON()
        if self._reader is not None:
            self._reader.join(timeout=5)
        if self._out_fd >= 0:
            try:
                os.close(self._out_fd)
            except OSError:
                pass
            self._out_fd = -1
        if self._process:
            kernel32.CloseHandle(self._process)
            self._process = wintypes.HANDLE()
