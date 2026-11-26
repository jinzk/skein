#!/usr/bin/env python3
"""CLI tests for `skein fmt`."""

import os
import subprocess
import tempfile

import harness


MESSY = (
    "import std;\n"
    "func add(a:int,b:int):int{return a+b;}\n"
    'if(true){println("x+y");}else{println("no");}\n'
    "let xs:list[int]=[1,2,3];// trailing\n"
    "print(xs);\n"
)

EXPECTED = (
    "import std;\n"
    "func add(a: int, b: int): int { return a + b; }\n"
    'if (true) { println("x+y"); } else { println("no"); }\n'
    "let xs: list[int] = [1, 2, 3];  // trailing\n"
    "print(xs);\n"
)


def write_temp(source, suffix=".skein", newline="\n"):
    handle = tempfile.NamedTemporaryFile(
        "wb", suffix=suffix, delete=False
    )
    try:
        handle.write(source.replace("\n", newline).encode("utf-8"))
    finally:
        handle.close()
    return handle.name


def run_fmt(*args, stdin=None):
    return subprocess.run(
        [harness.BIN, "fmt", *args],
        input=stdin,
        capture_output=True,
        timeout=harness.DEFAULT_TIMEOUT,
    )


def decode(data):
    return data.decode("utf-8").replace("\r\n", "\n")


def run_cases():
    path = write_temp(MESSY)
    try:
        formatted = run_fmt(path)
        assert formatted.returncode == 0, decode(formatted.stderr)
        assert decode(formatted.stdout) == EXPECTED, decode(formatted.stdout)

        second = run_fmt("-", stdin=formatted.stdout)
        assert second.returncode == 0, decode(second.stderr)
        assert second.stdout == formatted.stdout, (
            "fmt must be idempotent\n"
            f"first:\n{decode(formatted.stdout)}\n"
            f"second:\n{decode(second.stdout)}"
        )

        check_messy = run_fmt("--check", path)
        assert check_messy.returncode == 1, decode(check_messy.stderr)
        assert check_messy.stdout == b"", decode(check_messy.stdout)

        written = run_fmt("-w", path)
        assert written.returncode == 0, decode(written.stderr)
        with open(path, "rb") as handle:
            assert handle.read().decode("utf-8") == EXPECTED

        check_clean = run_fmt("--check", path)
        assert check_clean.returncode == 0, decode(check_clean.stderr)
    finally:
        os.unlink(path)

    comment_source = (
        "#!/usr/bin/env skein\n"
        "\n"
        "// header\n"
        "let x:int=1; // trailing\n"
        "\n"
        "\n"
        "print(x);\n"
    )
    comment_expected = (
        "#!/usr/bin/env skein\n"
        "\n"
        "// header\n"
        "let x: int = 1;  // trailing\n"
        "\n"
        "print(x);\n"
    )
    comment_path = write_temp(comment_source)
    try:
        result = run_fmt(comment_path)
        assert result.returncode == 0, decode(result.stderr)
        assert decode(result.stdout) == comment_expected, decode(result.stdout)
    finally:
        os.unlink(comment_path)

    invalid_path = write_temp("let = ;\n")
    try:
        invalid = run_fmt(invalid_path)
        assert invalid.returncode == 1, decode(invalid.stderr)
        assert b"syntax error" in invalid.stderr, decode(invalid.stderr)
        assert invalid.stdout == b"", decode(invalid.stdout)
    finally:
        os.unlink(invalid_path)

    crlf_source = "let x:int=1;\nprint(x);\n"
    crlf_expected = "let x: int = 1;\r\nprint(x);\r\n"
    crlf_path = write_temp(crlf_source, newline="\r\n")
    try:
        result = run_fmt(crlf_path)
        assert result.returncode == 0, decode(result.stderr)
        assert result.stdout == crlf_expected.encode("utf-8"), repr(result.stdout)

        formatted_path = write_temp(
            "let x: int = 1;\nprint(x);\n", newline="\r\n"
        )
        try:
            check = run_fmt("--check", formatted_path)
            assert check.returncode == 0, decode(check.stderr)
        finally:
            os.unlink(formatted_path)
    finally:
        os.unlink(crlf_path)

    lambda_source = (
        "let f=(x)->{\n"
        "let y=x+1;\n"
        "y*2;\n"
        "};\n"
        "let g=()->do{\n"
        "let a=1;\n"
        "a+1;\n"
        "};\n"
        "let h=()->{};\n"
        'let d=(x)->({"v":x});\n'
    )
    lambda_expected = (
        "let f = (x) -> {\n"
        "  let y = x + 1;\n"
        "  y * 2;\n"
        "};\n"
        "let g = () -> do {\n"
        "  let a = 1;\n"
        "  a + 1;\n"
        "};\n"
        "let h = () -> {};\n"
        'let d = (x) -> ({ "v": x});\n'
    )
    lambda_path = write_temp(lambda_source)
    try:
        formatted = run_fmt(lambda_path)
        assert formatted.returncode == 0, decode(formatted.stderr)
        assert decode(formatted.stdout) == lambda_expected, decode(
            formatted.stdout
        )

        second = run_fmt("-", stdin=formatted.stdout)
        assert second.returncode == 0, decode(second.stderr)
        assert second.stdout == formatted.stdout, (
            "fmt must be idempotent for lambda blocks\n"
            f"first:\n{decode(formatted.stdout)}\n"
            f"second:\n{decode(second.stdout)}"
        )

        check_messy = run_fmt("--check", lambda_path)
        assert check_messy.returncode == 1, decode(check_messy.stderr)

        written = run_fmt("-w", lambda_path)
        assert written.returncode == 0, decode(written.stderr)
        check_clean = run_fmt("--check", lambda_path)
        assert check_clean.returncode == 0, decode(check_clean.stderr)
    finally:
        os.unlink(lambda_path)

    selective_source = "import{basename as base}from std.path;\n"
    selective_expected = "import {basename as base} from std.path;\n"
    selective_path = write_temp(selective_source)
    try:
        formatted = run_fmt(selective_path)
        assert formatted.returncode == 0, decode(formatted.stderr)
        assert decode(formatted.stdout) == selective_expected, decode(
            formatted.stdout
        )

        second = run_fmt("-", stdin=formatted.stdout)
        assert second.returncode == 0, decode(second.stderr)
        assert second.stdout == formatted.stdout, (
            "fmt must be idempotent for selective imports\n"
            f"first:\n{decode(formatted.stdout)}\n"
            f"second:\n{decode(second.stdout)}"
        )

        check_clean = run_fmt("--check", selective_path)
        assert check_clean.returncode == 1, decode(check_clean.stderr)
    finally:
        os.unlink(selective_path)

    macro_source = (
        "import std;\n"
        "func f():int{return 1;}\n"
        "let a=1;\n"
        "info!(a);\n"
        "fail!;\n"
        "let b=2;\n"
        "fail! b;\n"
        "let t=launch f();\n"
        "println(str(wait! t));\n"
    )
    macro_expected = (
        "import std;\n"
        "func f(): int { return 1; }\n"
        "let a = 1;\n"
        "info!(a);\n"
        "fail!;\n"
        "let b = 2;\n"
        "fail! b;\n"
        "let t = launch f();\n"
        "println(str(wait! t));\n"
    )
    macro_path = write_temp(macro_source)
    try:
        formatted = run_fmt(macro_path)
        assert formatted.returncode == 0, decode(formatted.stderr)
        assert decode(formatted.stdout) == macro_expected, decode(
            formatted.stdout
        )

        second = run_fmt("-", stdin=formatted.stdout)
        assert second.returncode == 0, decode(second.stderr)
        assert second.stdout == formatted.stdout, (
            "fmt must be idempotent for macro shorthands\n"
            f"first:\n{decode(formatted.stdout)}\n"
            f"second:\n{decode(second.stdout)}"
        )
    finally:
        os.unlink(macro_path)
