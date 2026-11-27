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

    do_return_source = (
        "func f() {\n"
        "  let v = do { return 5; };\n"
        "  return v;\n"
        "}\n"
        "f();\n"
    )
    do_return = run_lint(do_return_source)
    assert do_return.returncode == 0, do_return.stderr
    assert "return inside a do block" in do_return.stderr, do_return.stderr
    assert do_return.stdout == "", "lint must not execute warned scripts"

    do_return_json = run_lint(do_return_source, "--format=json")
    assert do_return_json.returncode == 0, do_return_json.stderr
    do_return_payload = json.loads(do_return_json.stdout)
    assert len(do_return_payload["diagnostics"]) == 1, do_return_payload
    do_return_item = do_return_payload["diagnostics"][0]
    assert do_return_item["severity"] == "warning", do_return_item
    assert do_return_item["category"] == "warning", do_return_item
    assert "return inside a do block" in do_return_item["message"], (
        do_return_item
    )

    lambda_return_source = (
        "func f() {\n"
        "  let g = (x) -> { return x + 1; };\n"
        "  return g(1);\n"
        "}\n"
        "f();\n"
    )
    lambda_return = run_lint(lambda_return_source)
    assert lambda_return.returncode == 0, lambda_return.stderr
    assert "return inside a do block" not in lambda_return.stderr, (
        lambda_return.stderr
    )

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
            "  std.native.load(" + skein_string(harness.NATIVE_MODULE) +
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

    # External imports are resolved against skein.mod without downloading or
    # evaluating the module.
    with tempfile.TemporaryDirectory() as project:
        main = os.path.join(project, "main.skein")
        with open(main, "w", encoding="utf-8", newline="\n") as handle:
            handle.write('import "example.test/util/json";\n')
        with open(os.path.join(project, "skein.mod"), "w",
                  encoding="utf-8", newline="\n") as handle:
            handle.write("module example.test/lint\n")
        env = os.environ.copy()
        env["SKEINMODCACHE"] = os.path.join(project, "empty-cache")
        missing = subprocess.run(
            [harness.BIN, "lint", main],
            cwd=project,
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=harness.DEFAULT_TIMEOUT,
        )
        assert missing.returncode == 1, missing.stderr
        assert "external module not required" in missing.stderr, missing.stderr

        with open(os.path.join(project, "skein.mod"), "w",
                  encoding="utf-8", newline="\n") as handle:
            handle.write(
                "module example.test/lint\n"
                "require example.test/util v1.0.0\n"
            )
        resolved = subprocess.run(
            [harness.BIN, "lint", main],
            cwd=project,
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=harness.DEFAULT_TIMEOUT,
        )
        assert resolved.returncode == 0, resolved.stderr

    macro_locations = run_lint(
        "macro bad(T: type) { let x: #T = 1; return x; }\n"
        "func test(): int { return bad!(1 + 2); }\n",
        "--typecheck=strict",
        "--format=json",
    )
    assert macro_locations.returncode == 1, macro_locations.stderr
    payload = json.loads(macro_locations.stdout)
    error = next(
        item for item in payload["diagnostics"]
        if item["message"] == "type fragment expects a type name"
    )
    assert error["line"] == 2, error
    assert len(error["related"]) == 1, error
    assert error["related"][0]["line"] == 1, error
    assert "macro bad defined here" in error["related"][0]["message"], error

    definition_location = run_lint(
        "macro bad(T: type) { let x = #T; return 1; }\n",
        "--typecheck=strict",
        "--format=json",
    )
    assert definition_location.returncode == 1, definition_location.stderr
    payload = json.loads(definition_location.stdout)
    error = next(
        item for item in payload["diagnostics"]
        if "type fragment cannot be used as a value" in item["message"]
    )
    assert error["line"] == 1, error
    assert error["related"] == [], error
