#!/usr/bin/env python3
"""Process memory introspection: rss, children and tree_rss."""

import os
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])


def skein_path(path):
    return path.replace("\\", "/")


def main():
    with tempfile.TemporaryDirectory(prefix="skein-proc-mem-") as tmp:
        leaf = os.path.join(tmp, "leaf.skein")
        parent = os.path.join(tmp, "parent.skein")
        with open(leaf, "w", encoding="utf-8") as handle:
            handle.write("import std;\nstd.task.sleep(1500).wait();\n")
        with open(parent, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                f'let leaf = std.process.spawn_command("{skein_path(BIN)}", '
                '{"args": ["' + skein_path(leaf) + '"]});\n'
                "println(str(std.process.pid(leaf)));\n"
                "std.io.flush(std.io.stdout);\n"
                "std.task.sleep(1000).wait();\n"
                "std.process.wait(leaf);\n"
                "std.process.close(leaf);\n"
            )
        source = (
            "import std;\n"
            f'let parent = std.process.spawn_command("{skein_path(BIN)}", '
            '{"args": ["' + skein_path(parent) + '"]});\n'
            'let leaf_pid = int(bytes_to_string('
            'std.io.read_line(parent["stdout"])));\n'
            "std.task.sleep(300).wait();\n"
            'println("parent_rss=" + str(std.process.rss(parent) > 0));\n'
            'println("leaf_rss=" + str(std.process.rss(leaf_pid) > 0));\n'
            'let kids = std.process.children(parent);\n'
            'println("kids=" + str(len(kids) >= 1));\n'
            'println("tree=" + str(std.process.tree_rss(parent) >= '
            'std.process.rss(parent)));\n'
            "std.process.wait(parent);\n"
            "std.process.close(parent);\n"
        )
        proc = subprocess.run(
            [BIN],
            input=source,
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=60,
        )
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1
        expected = ["parent_rss=true", "leaf_rss=true", "kids=true",
                    "tree=true"]
        missing = [line for line in expected if line not in proc.stdout]
        if missing:
            sys.stderr.write(proc.stdout + proc.stderr)
            print(f"missing: {missing}", file=sys.stderr)
            return 1
        print("process memory test passed")
        return 0


if __name__ == "__main__":
    sys.exit(main())
