#!/usr/bin/env python3
"""Module v2: module manifests and explicit subdirectory package exports."""
import os
import subprocess
import sys
import tempfile

from harness import *  # noqa: F401,F403

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHECK_MODULES = os.path.join(ROOT, "tools", "check_modules.py")


def write(tmp, rel, text):
    path = os.path.join(tmp, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    return path


def run_module(tmp, main_source, expected):
    main = write(tmp, "main.skein", main_source)
    proc = run_file(main)
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.splitlines() == expected, proc.stdout


def run_module_error(tmp, main_source, expected_error):
    main = write(tmp, "main.skein", main_source)
    proc = run_file(main)
    assert proc.returncode != 0, proc.stdout
    assert expected_error in proc.stderr, proc.stderr


def directory_module(tmp):
    write(
        tmp,
        "math/module.skein",
        "module math {\n"
        "  export add.add;\n"
        "  export vector.sum as sum;\n"
        "  export internal.helpers.normalize as normalize;\n"
        "  export package internal.helpers as helpers;\n"
        "}\n",
    )
    write(
        tmp,
        "math/add.skein",
        "package add {\n"
        "  export func add(a, b) { return a + b; }\n"
        "  func hidden() { return 99; }\n"
        "}\n",
    )
    write(
        tmp,
        "math/vector.skein",
        "package vector {\n"
        "  import .internal.helpers;\n"
        "  export func sum(xs) {\n"
        "    return helpers.normalize(xs[0] + xs[1]);\n"
        "  }\n"
        "}\n",
    )
    write(
        tmp,
        "math/internal/helpers.skein",
        "package helpers {\n"
        "  export func normalize(x) { return x + 1; }\n"
        "  export func tag() { return \"helpers\"; }\n"
        "  func hidden_helper() { return 7; }\n"
        "}\n",
    )


def file_module(tmp):
    write(
        tmp,
        "math/module.skein",
        "module math {\n"
        "  export add.add;\n"
        "  export vector.sum as sum;\n"
        "  export internal.helpers.normalize as normalize;\n"
        "  export package internal.helpers as helpers;\n"
        "}\n",
    )
    write(
        tmp,
        "math/add.skein",
        "package add {\n"
        "  export func add(a, b) { return a + b; }\n"
        "}\n",
    )
    write(
        tmp,
        "math/vector.skein",
        "package vector {\n"
        "  import .internal.helpers;\n"
        "  export func sum(xs) {\n"
        "    return helpers.normalize(xs[0] + xs[1]);\n"
        "  }\n"
        "}\n",
    )
    write(
        tmp,
        "math/internal/helpers.skein",
        "package helpers {\n"
        "  export func normalize(x) { return x * 2; }\n"
        "}\n",
    )


def run_cases():
    with tempfile.TemporaryDirectory() as tmp:
        directory_module(tmp)
        run_module(
            tmp,
            "import math;\n"
            "import math.helpers as h;\n"
            "println(str(math.add(2, 3)));\n"
            "println(str(math.sum([4, 5])));\n"
            "println(str(math.normalize(41)));\n"
            "println(str(math.helpers.tag()));\n"
            "println(str(h.tag()));\n",
            ["5", "10", "42", "helpers", "helpers"],
        )
        run_module_error(
            tmp,
            "import math.internal.helpers;\n",
            "module member is not exported",
        )
        run_module_error(
            tmp,
            "import math.add;\n",
            "does not export package 'add'",
        )
        run_module_error(
            tmp,
            "import math;\nprintln(str(math.hidden));\n",
            "key not found",
        )
        run_module_error(
            tmp,
            "import math;\nprintln(str(math.helpers.hidden_helper()));\n",
            "key not found",
        )
        run_module(
            tmp,
            "import std;\n"
            "import math;\n"
            "func worker(ctx) {\n"
            '  return {"v": math.normalize(ctx.payload["x"])};\n'
            "}\n"
            "let blob = std.entry.encode(worker);\n"
            "let t = std.thread.spawn_entry(blob, "
            'std.thread.Message.from({"x": 41}));\n'
            "let r = t.join();\n"
            'println(str(r["ok"]));\n'
            'println(str(r["value"]["v"]));\n',
            ["true", "42"],
        )
        run_module(
            tmp,
            "import std;\n"
            "import math;\n"
            "func worker(ctx) {\n"
            '  return {"v": math.normalize(ctx.payload["x"])};\n'
            "}\n"
            "let blob = std.entry.encode(worker);\n"
            "let r = std.process.run_entry(blob, "
            'std.thread.Message.from({"x": 41}));\n'
            'println(str(r["ok"]));\n'
            'println(str(r["value"]["v"]));\n',
            ["true", "42"],
        )

    with tempfile.TemporaryDirectory() as tmp:
        file_module(tmp)
        run_module(
            tmp,
            "import math;\n"
            "println(str(math.add(1, 2)));\n"
            "println(str(math.sum([3, 4])));\n"
            "println(str(math.normalize(5)));\n"
            "println(str(math.helpers.normalize(6)));\n",
            ["3", "14", "10", "12"],
        )

    with tempfile.TemporaryDirectory() as tmp:
        write(
            tmp,
            "math/module.skein",
            "module math { export add.add; }\n",
        )
        write(
            tmp,
            "math/add/add.skein",
            "package add { export func add(a, b) { return a + b; } }\n",
        )
        run_module_error(
            tmp,
            "import math;\n",
            "package not found",
        )

    with tempfile.TemporaryDirectory() as tmp:
        write(
            tmp,
            "math/module.skein",
            "module wrong { export add.add; }\n",
        )
        write(
            tmp,
            "math/add.skein",
            "package add { export func add(a, b) { return a + b; } }\n",
        )
        run_module_error(
            tmp,
            "import math;\n",
            "does not match directory",
        )

    with tempfile.TemporaryDirectory() as tmp:
        write(
            tmp,
            "math/module.skein",
            "module math { export add.missing; }\n",
        )
        write(
            tmp,
            "math/add.skein",
            "package add { export func add(a, b) { return a + b; } }\n",
        )
        run_module_error(
            tmp,
            "import math;\n",
            "does not export 'missing'",
        )

    with tempfile.TemporaryDirectory() as tmp:
        write(
            tmp,
            "math/module.skein",
            "module math {\n"
            "  export add.add;\n"
            "  export vector.sum as add;\n"
            "}\n",
        )
        write(
            tmp,
            "math/add.skein",
            "package add { export func add(a, b) { return a + b; } }\n",
        )
        write(
            tmp,
            "math/vector.skein",
            "package vector { export func sum(xs) { return 0; } }\n",
        )
        run_module_error(
            tmp,
            "import math;\n",
            "duplicate module export alias",
        )

    with tempfile.TemporaryDirectory() as tmp:
        write(
            tmp,
            "math/module.skein",
            "module math { export loop.loop; }\n",
        )
        write(
            tmp,
            "math/loop.skein",
            "package loop {\n"
            "  import math;\n"
            "  export func loop() { return 1; }\n"
            "}\n",
        )
        run_module_error(
            tmp,
            "import math;\n",
            "cyclic module import",
        )

    with tempfile.TemporaryDirectory() as tmp:
        write(
            tmp,
            "math/module.skein",
            "module math { export add.add; }\n",
        )
        write(
            tmp,
            "math/math.skein",
            "package math { export func add() { return 1; } }\n",
        )
        write(
            tmp,
            "math/add.skein",
            "package add { export func add(a, b) { return a + b; } }\n",
        )
        run_module_error(
            tmp,
            "import math;\n",
            "module root cannot contain package file",
        )

    with tempfile.TemporaryDirectory() as tmp:
        directory_module(tmp)
        proc = subprocess.run(
            [sys.executable, CHECK_MODULES, tmp],
            capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
        assert "module manifests ok" in proc.stdout, proc.stdout

        listing = subprocess.run(
            [sys.executable, CHECK_MODULES, tmp, "--list"],
            capture_output=True, text=True)
        assert listing.returncode == 0, listing.stderr
        assert "math.helpers\tpackage\tinternal.helpers" in listing.stdout, (
            listing.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        write(
            tmp,
            "math/module.skein",
            "module math { export missing.pkg; }\n",
        )
        proc = subprocess.run(
            [sys.executable, CHECK_MODULES, tmp],
            capture_output=True, text=True)
        assert proc.returncode != 0, proc.stdout
        assert "was not found" in proc.stderr, proc.stderr
