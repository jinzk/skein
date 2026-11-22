#!/usr/bin/env python3
"""Optional SQLite and PostgreSQL driver tests."""

import os

from harness import *  # noqa: F401,F403


def run_cases():
    probe = run(
        "import std;\n"
        'let db = std.db.open(":memory:");\n'
        "std.db.close(db);\n"
    )
    if probe.returncode == 0 and not probe.stderr.strip():
        run_skein("db.skein")
    if os.environ.get("SKEIN_TEST_POSTGRES_DSN"):
        run_skein("db_postgres.skein")
