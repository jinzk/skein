#!/usr/bin/env python3
"""Interactive completion popup must render and accept a selected candidate.

POSIX drives the REPL through a pty.  Windows has no pty module, so the same
scenario runs against a ConPTY pseudo console, which still gives the child a
real console for raw input and VT output.  Platforms with neither exit with
SKIP_RETURN_CODE (registered in CMake) so the report says "Skipped" instead of
pretending the test passed.
"""

import os
import re
import select
import struct
import subprocess
import sys
import time

SKIP_RETURN_CODE = 77
BIN = sys.argv[1]

ANSI = re.compile(rb"\x1b\][^\x07]*\x07|\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[@-_]")


def plain(data):
    return ANSI.sub(b"", data).replace(b"\r", b"")


def wait_for(session, needle, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        session.drain(0.05)
        if needle in plain(session.buffer()):
            return True
    return False


def run_scenario(session):
    session.drain(0.3)
    session.send(b":\t")
    assert wait_for(session, b"candidates (6)"), plain(session.buffer())
    assert b"> :exit" in plain(session.buffer()), plain(session.buffer())

    session.send(b"\x1b[B\t")
    assert wait_for(session, b":help"), plain(session.buffer())

    session.send(b"\r")
    session.send(b":q\r")
    assert session.wait(10) == 0, plain(session.buffer())


class PosixPtySession:
    def __init__(self, argv):
        self._master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, 80, 0, 0))
        self._process = subprocess.Popen(
            argv, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
        os.close(slave)
        self._buffer = bytearray()

    def drain(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([self._master], [], [], 0.05)
            if ready:
                try:
                    self._buffer += os.read(self._master, 4096)
                except OSError:
                    return

    def send(self, data):
        os.write(self._master, data)

    def buffer(self):
        return bytes(self._buffer)

    def wait(self, timeout):
        try:
            return self._process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def close(self):
        if self._process.poll() is None:
            self._process.kill()
            self._process.wait()
        try:
            os.close(self._master)
        except OSError:
            pass


def open_session():
    if os.name == "posix":
        global fcntl, pty, termios
        try:
            import fcntl
            import pty
            import termios
        except ImportError:  # pragma: no cover - unusual POSIX Python
            return None
        if not hasattr(termios, "TIOCSWINSZ"):
            return None
        return PosixPtySession([BIN, "-i"])
    if os.name == "nt":
        try:
            from conpty import ConPtySession, conpty_available
        except ImportError:  # pragma: no cover - missing helper
            return None
        if not conpty_available():
            return None
        return ConPtySession([BIN, "-i"], columns=80, rows=24)
    return None  # pragma: no cover - other platforms


def main():
    session = open_session()
    if session is None:
        sys.exit(SKIP_RETURN_CODE)
    try:
        run_scenario(session)
    finally:
        session.close()


if __name__ == "__main__":
    main()
