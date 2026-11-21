#!/usr/bin/env python3
"""Cross-platform lexical paths provided by std.path.

The cases live in tests/skein/paths.skein; this module keeps the existing
ctest module name and shard wiring.  One Python golden stays here as an
independent check of list/string formatting.
"""
from harness import check, run_skein


def run_cases():
    run_skein("paths.skein")
    check(
        "path golden output",
        "import std.path;\n"
        'let p = path.parse("/a/b/../c.txt", "posix");\n'
        'print(p["style"]);\n'
        'print(p["parts"]);\n'
        'print(p["normalized"]);\n',
        stdout_all=["posix", "[a, b, .., c.txt]", "/a/c.txt"],
    )
