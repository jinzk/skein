#!/usr/bin/env python3
"""std.meta type introspection and protocol conformance.

Pure introspection cases live in tests/skein/meta.skein.  Structural protocol
facades live in tests/skein/protocol_facade.skein.  Package visibility tests
stay here because they need temp modules and CLI diagnostics.
"""
import os
import tempfile

from harness import *  # noqa: F401,F403


def run_cases():
    run_skein("meta.skein")
    run_skein("protocol_facade.skein")
    check(
        "meta golden output",
        "import std;\n"
        "struct MetaGolden { x: int; }\n"
        "std.io.println(std.meta.type_info(MetaGolden { x: 1 })[\"name\"]);\n"
        "std.io.println(std.meta.fields(MetaGolden { x: 1 })[0][\"type\"]);\n",
        stdout_all=["MetaGolden", "int"],
    )

    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write("aa.skein",
              "package aa {\n"
              "  import std;\n"
              "  export struct Pub { x: int; }\n"
              "  struct Hidden { y: int; }\n"
              "  export func make_hidden() { return Hidden { y: 2 }; }\n"
              "  export func local_meta() {\n"
              '    return std.meta.type_info("Hidden");\n'
              "  }\n"
              "}\n")
        main = write("main_vis.skein",
                     "import std;\n"
                     "import aa;\n"
                     'std.io.println(std.meta.type_info("Pub")["name"]);\n'
                     'std.io.println(std.meta.type_info("aa.Pub")["name"]);\n'
                     'std.io.println(std.meta.has_type("aa.Hidden"));\n'
                     'std.io.println(aa.local_meta()["name"]);\n'
                     'std.meta.type_info("aa.Hidden");\n')
        proc = run_file(main)
        assert proc.returncode != 0, proc.stdout
        assert "Pub" in proc.stdout, proc.stdout
        assert "false" in proc.stdout, proc.stdout
        assert "Hidden" in proc.stdout, proc.stdout
        assert "type aa.Hidden is not visible" in proc.stderr, proc.stderr

        main = write("main_value.skein",
                     "import std;\n"
                     "import aa;\n"
                     "let hidden = aa.make_hidden();\n"
                     "std.meta.type_info(hidden);\n")
        proc = run_file(main)
        assert proc.returncode != 0, proc.stdout
        assert "type aa.Hidden is not visible" in proc.stderr, proc.stderr

        main = write("main_alias.skein",
                     "import std;\n"
                     "import aa as lib;\n"
                     'std.io.println(std.meta.type_info("lib.Pub")["name"]);\n'
                     'std.io.println(std.meta.has_type("lib.Hidden"));\n')
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["Pub", "false"], proc.stdout

        write("bb.skein",
              "package bb { export struct Pub { y: int; } }\n")
        main = write("main_ambiguous.skein",
                     "import std;\n"
                     "import aa;\n"
                     "import bb;\n"
                     'std.meta.type_info("Pub");\n')
        proc = run_file(main)
        assert proc.returncode != 0, proc.stdout
        assert "ambiguous type Pub" in proc.stderr, proc.stderr
