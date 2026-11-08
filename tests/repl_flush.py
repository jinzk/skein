#!/usr/bin/env python3
"""Interactive-mode print must flush before the next prompt/input."""
import subprocess
import sys
import threading

BIN = sys.argv[1]

proc = subprocess.Popen(
    [BIN],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
try:
    proc.stdin.write(b'print("A");\n')
    proc.stdin.flush()

    # select() only works on sockets under Windows, so wait for the first
    # output byte with a reader thread instead.  This works for pipes on
    # both POSIX and Windows.
    result = {}

    def read_first_byte():
        try:
            result["byte"] = proc.stdout.read(1)
        except Exception as exc:  # pragma: no cover - diagnostics only
            result["error"] = exc

    reader = threading.Thread(target=read_first_byte, daemon=True)
    reader.start()
    reader.join(3.0)
    if reader.is_alive():
        sys.stderr.write("print output was not flushed\n")
        sys.exit(1)
    if "error" in result:
        sys.stderr.write(f"failed to read output: {result['error']!r}\n")
        sys.exit(1)
    chunk = result.get("byte", b"")
    if b"A" not in chunk:
        sys.stderr.write(f"unexpected output: {chunk!r}\n")
        sys.exit(1)

    proc.stdin.close()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        sys.stderr.write("interpreter did not exit after stdin EOF\n")
        sys.exit(1)
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
