#!/usr/bin/env python3
"""Interactive completion popup must render and accept a selected candidate."""
import os
import select
import struct
import subprocess
import sys
import time

# ctest treats this exit code as "skipped" rather than "passed".  The popup
# needs a POSIX pty; the underlying candidate/layout logic is covered on every
# platform by repl_ui_test.
SKIP_RETURN_CODE = 77

if os.name != "posix":  # pragma: no cover - Windows has no POSIX pty modules
    sys.exit(SKIP_RETURN_CODE)

import fcntl  # noqa: E402
import pty  # noqa: E402
import termios  # noqa: E402

BIN = sys.argv[1]

if not hasattr(termios, "TIOCSWINSZ"):  # pragma: no cover - non-pty platforms
    sys.exit(SKIP_RETURN_CODE)

master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
proc = subprocess.Popen(
    [BIN, "-i"], stdin=slave, stdout=slave, stderr=slave, close_fds=True
)
os.close(slave)
output = b""


def drain(seconds):
    global output
    deadline = time.time() + seconds
    while time.time() < deadline:
        ready, _, _ = select.select([master], [], [], 0.05)
        if ready:
            try:
                output += os.read(master, 4096)
            except OSError:
                return


def send(data):
    os.write(master, data)
    drain(0.25)


try:
    drain(0.2)
    send(b":\t")
    assert b"candidates (6)" in output, output
    assert b"> :exit" in output, output

    send(b"\x1b[B\t")
    assert b"\r> \x1b[36m:help\x1b[0m" in output or b":help" in output, output
    send(b"\r")
    send(b":q\r")
    drain(0.2)
    proc.wait(timeout=5)
    assert proc.returncode == 0, proc.returncode
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    os.close(master)
