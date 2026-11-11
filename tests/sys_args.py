#!/usr/bin/env python3
"""Script argument plumbing: std.sys.args/script/executable."""

import os
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])


def main():
    with tempfile.TemporaryDirectory(prefix="skein-sys-args-") as tmp:
        script = os.path.join(tmp, "probe.skein")
        with open(script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "println(str(std.sys.args()));\n"
                'println("count=" + str(len(std.sys.args())));\n'
                'println("script=" + std.sys.script());\n'
                'println("exe=" + std.sys.executable());\n'
            )
        proc = subprocess.run(
            [BIN, "--no-vm", script, "--bin", "build/skein", "--check",
             "a b"],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
        )
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1
        expected = [
            "[--bin, build/skein, --check, a b]",
            "count=4",
            "script=" + script,
        ]
        missing = [line for line in expected if line not in proc.stdout]
        if "exe=" not in proc.stdout or "exe=\n" in proc.stdout:
            missing.append("non-empty executable")
        if missing:
            sys.stderr.write(proc.stdout + proc.stderr)
            print(f"missing: {missing}", file=sys.stderr)
            return 1

        # Interpreter flags after the script path belong to the script.
        proc = subprocess.run(
            [BIN, script, "--no-vm"],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
        )
        if proc.returncode != 0 or "count=1" not in proc.stdout or \
                "[--no-vm]" not in proc.stdout:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1

        # Stdin scripts have no script path and no arguments.
        proc = subprocess.run(
            [BIN],
            input="import std;\nprintln(str(std.sys.args()));\n"
                  'println("script=[" + std.sys.script() + "]");\n',
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
        )
        if proc.returncode != 0 or "[]" not in proc.stdout or \
                "script=[]" not in proc.stdout:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1
        print("sys args test passed")
        return 0


if __name__ == "__main__":
    sys.exit(main())
