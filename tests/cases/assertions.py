#!/usr/bin/env python3
"""Tests for runtime assertions."""

import os

from harness import check, run_skein


def run_cases():
    run_skein("assert_runtime.skein")
    check(
        "assert active with typecheck off",
        "import std;\n"
        'let r = std.result.capture(func() { '
        'std.assert.that!(false, "off"); });\n'
        'println(std.error.message(std.result.error(r)));\n',
        env={**os.environ, "SKEIN_TYPECHECK": "off"},
        stdout="off",
    )
