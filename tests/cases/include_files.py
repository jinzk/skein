#!/usr/bin/env python3
"""Declaration-only include files."""
import os
import tempfile

from harness import *  # noqa: F401,F403


def write(tmp, rel, text):
    path = os.path.join(tmp, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    return path


def run_cases():
    with tempfile.TemporaryDirectory() as tmp:
        write(tmp, "shared/dep.skein",
              "package dep {\n"
              "  export func twice(x) { return x * 2; }\n"
              "}\n")
        write(tmp, "shared/helpers.skein",
              "import std;\n"
              "import .dep;\n"
              "let header_value = 5;\n"
              "func greet(name) {\n"
              '  return "hi " + name + " " + str(dep.twice(header_value));\n'
              "}\n")
        write(tmp, "pkg.skein",
              "package pkg {\n"
              '  include "shared/helpers.skein";\n'
              '  export func inside() { return greet("pkg"); }\n'
              "}\n")
        main = write(tmp, "main.skein",
                     "import std;\n"
                     "import pkg;\n"
                     'include "shared/helpers.skein";\n'
                     'include "shared/helpers.skein";\n'
                     'println(greet("main"));\n'
                     "println(str(header_value));\n"
                     "println(pkg.inside());\n")
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == [
            "hi main 10", "5", "hi pkg 10"], proc.stdout

    # Included declarations inherit the includer's package/module identity.
    with tempfile.TemporaryDirectory() as tmp:
        write(tmp, "shared/types.skein",
              "import std;\n"
              "struct T { x: int; }\n"
              "func describe() {\n"
              "  return type_package(T { x: 1 }) + \"|\" +\n"
              "         type_module_name(T { x: 1 });\n"
              "}\n")
        write(tmp, "pkg.skein",
              "package pkg {\n"
              '  include "shared/types.skein";\n'
              "  export func info() { return describe(); }\n"
              "}\n")
        write(tmp, "shared/main_types.skein",
              "import std;\n"
              "struct M { x: int; }\n"
              "func describe_main() {\n"
              "  return type_package(M { x: 1 }) + \"|\" +\n"
              "         type_module_name(M { x: 1 });\n"
              "}\n")
        main = write(tmp, "main.skein",
                     "import std;\n"
                     "import pkg;\n"
                     'include "shared/main_types.skein";\n'
                     "println(describe_main());\n"
                     "println(pkg.info());\n")
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["main|", "pkg|"], proc.stdout

    # Package/module declarations and executable statements are rejected.
    with tempfile.TemporaryDirectory() as tmp:
        write(tmp, "bad_exec.skein", 'println("nope");\n')
        main = write(tmp, "main_exec.skein",
                     'include "bad_exec.skein";\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stdout
        assert "include file may only contain declarations" in proc.stderr, (
            proc.stderr)

        write(tmp, "bad_pkg.skein",
              "package bad { export func f() { return 1; } }\n")
        main = write(tmp, "main_pkg.skein", 'include "bad_pkg.skein";\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stdout
        assert "include file may only contain declarations" in proc.stderr, (
            proc.stderr)

        write(tmp, "ok.skein", "func ok() { return 1; }\n")
        main = write(tmp, "main_func.skein",
                     'func outer() {\n  include "ok.skein";\n  return 1;\n}\n'
                     "println(str(outer()));\n")
        proc = run_file(main)
        assert proc.returncode == 1, proc.stdout
        assert "include is only allowed at declaration level" in proc.stderr, (
            proc.stderr)

    # include-once, cycles and missing files.
    with tempfile.TemporaryDirectory() as tmp:
        write(tmp, "a.skein", 'include "b.skein";\n')
        write(tmp, "b.skein", 'include "a.skein";\n')
        main = write(tmp, "main_cycle.skein", 'include "a.skein";\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stdout
        assert "cyclic include" in proc.stderr, proc.stderr

        main = write(tmp, "main_missing.skein",
                     'include "missing.skein";\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stdout
        assert "include file not found" in proc.stderr, proc.stderr

    # Paths may use .. and absolute locations.
    with tempfile.TemporaryDirectory() as tmp:
        write(tmp, "shared/value.skein", "let shared_value = 9;\n")
        main = write(tmp, "app/main.skein",
                     'include "../shared/value.skein";\n'
                     "println(str(shared_value));\n")
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "9\n", repr(proc.stdout)
