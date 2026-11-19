#!/usr/bin/env python3
"""CLI tests for `skein lint`."""

import json
import os
import subprocess
import tempfile

import harness
from harness import *  # noqa: F401,F403


def run_lint(source, *extra, env=None):
    with tempfile.NamedTemporaryFile(
        "w", suffix=".skein", encoding="utf-8", newline="\n", delete=False
    ) as handle:
        handle.write(source)
        path = handle.name
    try:
        proc = subprocess.run(
            [harness.BIN, "lint", *extra, path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=harness.DEFAULT_TIMEOUT,
            env=env,
        )
    finally:
        os.unlink(path)
    proc.stdout = proc.stdout.replace("\r\n", "\n")
    proc.stderr = proc.stderr.replace("\r\n", "\n")
    return proc


def run_cases():
    clean = run_lint(
        "let x: int = 1;\n"
        'println("SHOULD_NOT_PRINT");\n',
        "--typecheck=strict",
    )
    assert clean.returncode == 0, (
        f"lint clean: unexpected exit {clean.returncode}\n"
        f"stdout:\n{clean.stdout}\nstderr:\n{clean.stderr}"
    )
    assert clean.stdout == "", "lint must not print script stdout"
    assert clean.stderr == "", "lint clean should be silent"

    type_error = run_lint(
        'let xs: list[int] = [1, "a"];\n', "--typecheck=strict"
    )
    assert type_error.returncode == 1, (
        f"lint type error: expected exit 1, got {type_error.returncode}"
    )
    assert "type error" in type_error.stderr, type_error.stderr
    assert type_error.stdout == "", "lint text mode must not write stdout"

    json_result = run_lint(
        'let xs: list[int] = [1, "a"];\n',
        "--typecheck=strict",
        "--format=json",
    )
    assert json_result.returncode == 1, json_result.stderr
    assert json_result.stderr == "", "lint JSON mode must not write stderr"
    payload = json.loads(json_result.stdout)
    assert len(payload["diagnostics"]) == 1, payload
    item = payload["diagnostics"][0]
    assert item["severity"] == "error", item
    assert item["code"] == "TYP0001", item
    assert item["line"] == 1, item
    assert item["category"] == "type error", item

    warn_env = dict(os.environ, SKEIN_TYPECHECK="warn")
    warning_source = (
        "func raw() { return [1]; }\n"
        "let xs: list[int] = raw();\n"
    )
    warning = run_lint(warning_source, env=warn_env)
    assert warning.returncode == 0, warning.stderr
    assert "type warning" in warning.stderr, warning.stderr
    assert warning.stdout == "", "lint must not execute warned scripts"

    warning_json = run_lint(warning_source, "--format=json", env=warn_env)
    assert warning_json.returncode == 0, warning_json.stderr
    warning_payload = json.loads(warning_json.stdout)
    assert len(warning_payload["diagnostics"]) == 1, warning_payload
    assert warning_payload["diagnostics"][0]["severity"] == "warning"

    syntax_error = run_lint("let = ;\n", "--typecheck=strict")
    assert syntax_error.returncode == 1, syntax_error.stderr
    assert "syntax error" in syntax_error.stderr, syntax_error.stderr

    multiple = run_lint(
        'let xs: list[int] = [1, "x"];\n'
        'let ys: list[int] = [1, "y"];\n',
        "--typecheck=strict",
    )
    assert multiple.returncode == 1, multiple.stderr
    assert multiple.stderr.count("type error") == 2, multiple.stderr

    if harness.NATIVE_MODULE:
        plugin_load = run_lint(
            "package p {\n"
            "  import std;\n"
            "  std.plugin.load(" + skein_string(harness.NATIVE_MODULE) +
            ");\n"
            "}\n"
        )
        assert plugin_load.returncode == 1, plugin_load.stderr
        assert "check-only mode" in plugin_load.stderr, plugin_load.stderr

    ffi_open = run_lint(
        "package p {\n"
        "  import std;\n"
        '  std.ffi.open("/definitely/not/a/library.so");\n'
        "}\n"
    )
    assert ffi_open.returncode == 1, ffi_open.stderr
    assert "check-only mode" in ffi_open.stderr, ffi_open.stderr
