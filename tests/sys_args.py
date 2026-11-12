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

        # std.sys.exit stops the script and controls the process status.
        proc = subprocess.run(
            [BIN],
            input="import std;\n"
                  'println("before-exit");\n'
                  "std.sys.exit(3);\n"
                  'println("after-exit");\n',
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
        )
        if proc.returncode != 3 or "before-exit" not in proc.stdout or \
                "after-exit" in proc.stdout:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1

        proc = subprocess.run(
            [BIN],
            input="import std;\nstd.sys.exit();\n",
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
        )
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1

        bad_script = os.path.join(tmp, "bad_exit.skein")
        with open(bad_script, "w", encoding="utf-8") as handle:
            handle.write("import std;\nstd.sys.exit(256);\n")
        proc = subprocess.run(
            [BIN, bad_script],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
        )
        if proc.returncode != 1 or "between 0 and 255" not in proc.stderr:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1

        helper = os.path.join(tmp, "helper.skein")
        with open(helper, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "package helper {\n"
                "  export func in_package() { return std.sys.is_main(); }\n"
                "  export func call_main(fn) { return fn(); }\n"
                "}\n"
            )
        main_script = os.path.join(tmp, "is_main.skein")
        with open(main_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "import helper;\n"
                "func callback() { return std.sys.is_main(); }\n"
                "println(str(std.sys.is_main()));\n"
                "println(str(helper.in_package()));\n"
                "println(str(helper.call_main(callback)));\n"
            )
        proc = subprocess.run(
            [BIN, main_script],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
        )
        if proc.returncode != 0 or proc.stdout.splitlines() != [
                "true", "false", "true"]:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1

        proc = subprocess.run(
            [BIN],
            input="import std;\nprintln(str(std.sys.is_main()));\n",
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
        )
        if proc.returncode != 0 or proc.stdout.strip() != "true":
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1
        print("sys args test passed")
        return 0


if __name__ == "__main__":
    sys.exit(main())
