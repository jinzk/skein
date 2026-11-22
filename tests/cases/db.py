#!/usr/bin/env python3
"""Optional SQLite driver tests."""

from harness import *  # noqa: F401,F403


def run_cases():
    probe = run(
        "import std;\n"
        'let db = std.db.open(":memory:");\n'
        "std.db.close(db);\n"
    )
    if probe.returncode != 0:
        return
    run_skein("db.skein")
