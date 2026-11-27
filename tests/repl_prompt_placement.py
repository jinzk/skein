#!/usr/bin/env python3
"""REPL prompts must start on a fresh line after no-newline output."""
import subprocess
import sys

BIN = sys.argv[1]

CASES = [
    (b'import std; std.io.print("abc");\n:quit\n', b'> abc\n> '),
    (b'import std; std.io.println("abc");\n:quit\n', b'> abc\n> '),
    (b'import std; std.io.print("a"); std.io.print("b");\n:quit\n',
     b'> ab\n> '),
]


def run(source):
    proc = subprocess.run(
        [BIN, "-i"],
        input=source,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
    )
    stdout = proc.stdout.replace(b"\r\n", b"\n")
    if proc.returncode != 0:
        raise AssertionError(
            f"interpreter exited with {proc.returncode}: "
            f"stdout={stdout!r}, stderr={proc.stderr!r}")
    if proc.stderr:
        raise AssertionError(f"unexpected stderr: {proc.stderr!r}")
    return stdout


for source, expected in CASES:
    actual = run(source)
    if actual != expected:
        raise AssertionError(
            f"input={source!r}: expected {expected!r}, got {actual!r}")
