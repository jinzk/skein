#!/usr/bin/env python3
"""Filesystem helpers: temp_dir, remove_all, copy, which and glob."""

import os
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])


def main():
    bin_dir = os.path.dirname(BIN)
    name = os.path.splitext(os.path.basename(BIN))[0]
    env = dict(os.environ)
    env["PATH"] = bin_dir + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="skein-fs-helpers-") as tmp:
        root = os.path.join(tmp, "root").replace("\\", "/")
        source = (
            "import std;\n"
            f'let root = "{root}";\n'
            "std.fs.mkdir(root, true);\n"
            'let base = std.fs.temp_dir("skein-fs-");\n'
            'println("temp_dir=" + str(std.fs.stat(base)["type"] == "dir"));\n'
            'std.fs.mkdir(std.fs.path_join(base, "a/b/c"), true);\n'
            'std.fs.write_text(std.fs.path_join(base, "a/b/c/one.txt"), "one", "utf-8");\n'
            'std.fs.write_text(std.fs.path_join(base, "root.txt"), "root", "utf-8");\n'
            'let files = std.fs.glob(std.fs.path_join(base, "*.txt"));\n'
            'println("glob=" + str(len(files)));\n'
            'std.fs.copy(std.fs.path_join(base, "root.txt"), '
            'std.fs.path_join(base, "copy.txt"));\n'
            'println("copy=" + str(std.fs.exists('
            'std.fs.path_join(base, "copy.txt")) == 1));\n'
            'std.fs.copy(base, std.fs.path_join(root, "tree"));\n'
            'println("tree=" + str(std.fs.exists('
            'std.fs.path_join(root, "tree/root.txt")) == 1));\n'
            'println("which_abs=" + str(std.fs.which(std.sys.executable()) '
            '!= ""));\n'
            f'println("which_name=" + str(std.fs.which("{name}") != ""));\n'
            'println("which_missing=" + str(std.fs.which('
            '"definitely-not-a-real-tool-xyz") == ""));\n'
            "std.fs.remove_all(base);\n"
            "std.fs.remove_all(base);\n"
            'println("removed=" + str(std.fs.exists(base) == 0));\n'
            "std.fs.remove_all(root);\n"
        )
        proc = subprocess.run(
            [BIN],
            input=source,
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
            env=env,
        )
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1
        expected = ["temp_dir=true", "glob=1", "copy=true", "tree=true",
                    "which_abs=true", "which_name=true",
                    "which_missing=true", "removed=true"]
        missing = [line for line in expected if line not in proc.stdout]
        if missing:
            sys.stderr.write(proc.stdout + proc.stderr)
            print(f"missing: {missing}", file=sys.stderr)
            return 1
        print("fs helpers test passed")
        return 0


if __name__ == "__main__":
    sys.exit(main())
