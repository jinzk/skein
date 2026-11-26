#!/usr/bin/env python3
"""Tests for built-in AST macros."""

from harness import check, run, run_skein


def run_cases():
    run_skein("macros_assert.skein")
    run_skein("macros_capture.skein")

    proc = run(
        "import std;\n"
        'func boom() { fail("must not run"); }\n'
        'std.log.set_level("error");\n'
        'info!("hidden", {"x": boom()});\n'
        'println("guard-ok");\n'
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.splitlines() == ["guard-ok"], proc.stdout

    proc = run(
        "import std;\n"
        'std.log.set_level("trace");\n'
        'info!("visible", {"x": 3});\n'
    )
    assert proc.returncode == 0, proc.stderr
    assert " INFO  visible x=3" in proc.stderr, proc.stderr

    proc = run(
        "import std;\n"
        'std.log.set_level("error");\n'
        'error!("bad", {"x": 1});\n'
        'let v = info!("guarded");\n'
        'println(str(v));\n'
    )
    assert proc.returncode == 0, proc.stderr
    assert " ERROR bad x=1" in proc.stderr, proc.stderr
    assert proc.stdout.splitlines() == ["null"], proc.stdout

    proc = run(
        "import std;\n"
        'let lg = std.log.new({"output": "stdout", "level": "trace", '
        '"name": "macro"});\n'
        'log_to!(lg, "debug", "to logger", {"k": 1});\n'
    )
    assert proc.returncode == 0, proc.stderr
    assert "DEBUG macro: to logger k=1" in proc.stdout, proc.stdout

    proc = run(
        "import std;\n"
        'let lg = std.log.new({"output": "stdout", "level": "trace"});\n'
        'func get_logger() {\n'
        '  println("get");\n'
        '  return lg;\n'
        '}\n'
        'log_to!(get_logger(), "debug", "once");\n'
    )
    assert proc.returncode == 0, proc.stderr
    assert sum(line == "get" for line in proc.stdout.splitlines()) == 1, \
        proc.stdout

    check(
        "macro unary not preserved",
        'import std;\nprintln(str(!(1 == 2)));\n',
        stdout="true",
    )
    check(
        "unknown macro",
        "import std;\nfoo!(1);\n",
        stderr="is not a known built-in macro",
    )
    check(
        "macro wrong arity",
        'import std;\ninfo!();\n',
        stderr="expects 1 or 2 expression arguments",
    )
    check(
        "macro mixed invocation rejects block",
        'import std;\ninfo!("x") { println(1); };\n',
        stderr="does not accept a block",
    )
