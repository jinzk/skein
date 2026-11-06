#!/usr/bin/env python3
"""Interactive-mode print must flush before the next prompt/input."""
import os
import select
import subprocess
import sys
import time

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
    ready, _, _ = select.select([proc.stdout], [], [], 3.0)
    if not ready:
        sys.stderr.write("print output was not flushed\n")
        sys.exit(1)
    chunk = os.read(proc.stdout.fileno(), 4096)
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
