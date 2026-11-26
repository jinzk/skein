#!/usr/bin/env python3
"""Tests for built-in AST macros."""

from harness import check, run, run_skein


def run_cases():
    run_skein("macros_assert.skein")
    run_skein("macros_capture.skein")
    run_skein("macros_iter.skein")

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
        stderr="is not a known built-in or user-defined macro",
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

    check(
        "canonical log macro path",
        'import std;\n'
        'std.log.set_level("error");\n'
        'std.log.error!("bad");\n'
        'println("log-ok");\n',
        stdout="log-ok",
        stderr=" ERROR bad",
    )
    check(
        "canonical log_to macro path",
        'import std;\n'
        'let lg = std.log.new({"output": "stdout", "level": "trace", '
        '"name": "macro"});\n'
        'std.log.log_to!(lg, "debug", "canonical", {"k": 2});\n',
        stdout="DEBUG macro: canonical k=2",
    )
    check(
        "canonical capture and control macros",
        'import std;\n'
        'let r = std.result.capture! { std.error.fail!("bad"); };\n'
        'println(str(std.result.is_err(r)));\n',
        stdout="true",
    )
    check(
        "zero-argument and single-name macro shorthand",
        'import std;\n'
        'let r = capture! { fail!; };\n'
        'println(str(std.result.is_err(r)));\n'
        'let msg = "boom";\n'
        'let s = capture! { fail! msg; };\n'
        'println(str(std.result.is_err(s)));\n',
        stdout_all=["true", "true"],
    )
    check(
        "recover macro shorthand and canonical path",
        'import std;\n'
        'func boom(): int { fail("x"); }\n'
        'let seven = 7;\n'
        'let a = boom() on error(e) { recover! seven; };\n'
        'let b = boom() on error(e) { std.error.recover!(7); };\n'
        'println(str(a));\n'
        'println(str(b));\n',
        stdout_all=["7", "7"],
    )
    check(
        "wait macro shorthand and canonical path",
        'func f(): int { return 42; }\n'
        'let t = launch f();\n'
        'let u = launch f();\n'
        'println(str(wait!(t)));\n'
        'println(str(wait! u));\n'
        'let v = launch f();\n'
        'println(str(std.task.wait!(v)));\n',
        stdout_all=["42", "42", "42"],
    )
    check(
        "await! is a macro callee, not the contextual keyword",
        'await!(1);\n',
        stderr="macro await is not a known built-in or user-defined macro",
    )
    check(
        "control macro rejects block",
        'import std;\nfail! { println(1); };\n',
        stderr="does not accept a block",
    )
    check(
        "control macro rejects extra arguments",
        'import std;\nstd.error.fail!(1, 2);\n',
        stderr="expects 0 or 1 arguments",
    )
    check(
        "wait macro requires one argument",
        'import std;\nwait!();\n',
        stderr="expects 1 expression argument",
    )

    # User-defined macros (P4 v1).
    check(
        "user macro expr fragment",
        'macro twice(x: expr) { return #x * 2; }\n'
        'func test(): int { return twice!(1 + 2); }\n'
        'println(str(test()));\n',
        stdout="6",
    )
    check(
        "user macro ident and block fragments",
        'macro set_name(name: ident, value: expr, body: block) {\n'
        '  let #name = #value;\n'
        '  #body\n'
        '  return #name;\n'
        '}\n'
        'func test(): int {\n'
        '  let outer = 100;\n'
        '  return set_name!(v, 1 + 2) { println(str(outer)); };\n'
        '}\n'
        'println(str(test()));\n',
        stdout="100\n3",
    )
    check(
        "user macro expr list fragment",
        'func add3(a: int, b: int, c: int): int { return a + b + c; }\n'
        'macro call_all(f: expr, args: expr...) { return #f(#args...); }\n'
        'func test(): int { return call_all!(add3, 1, 2, 3); }\n'
        'println(str(test()));\n',
        stdout="6",
    )
    check(
        "user macro nested call",
        'macro inc(x: expr) { #x + 1; }\n'
        'macro twice(x: expr) { inc!(#x) * 2; }\n'
        'func test(): int { return twice!(5); }\n'
        'println(str(test()));\n',
        stdout="12",
    )
    check(
        "user macro hygiene",
        'macro wrap(value: expr, body: block) {\n'
        '  let tmp = #value;\n'
        '  #body\n'
        '  return tmp;\n'
        '}\n'
        'func test(): int {\n'
        '  let tmp = 100;\n'
        '  return wrap!(2) { println(str(tmp)); };\n'
        '}\n'
        'println(str(test()));\n',
        stdout="100\n2",
    )
    check(
        "user macro shadows builtin bare name but not canonical path",
        'import std;\n'
        'macro info(x: expr) { return #x + 100; }\n'
        'func bare(): int { return info!(1); }\n'
        'func canonical(): int {\n'
        '  std.log.set_level("trace");\n'
        '  std.log.info!("canonical");\n'
        '  return 7;\n'
        '}\n'
        'println(str(bare()));\n'
        'println(str(canonical()));\n',
        stdout="101\n7",
        stderr=" INFO  canonical",
    )
    check(
        "user macro must be declared before use",
        'func test(): int { return later!(1); }\n'
        'macro later(x: expr) { return #x; }\n',
        stderr="is not a known built-in or user-defined macro",
    )
    check(
        "duplicate user macro",
        'macro m(x: expr) { return #x; }\n'
        'macro m(x: expr) { return #x; }\n',
        stderr="duplicate macro definition",
    )
    check(
        "unknown user macro placeholder",
        'macro m(x: expr) { return #y; }\n',
        stderr="unknown macro placeholder",
    )
    check(
        "user macro unknown fragment kind",
        'macro m(x: type) { return 1; }\n',
        stderr="unknown macro fragment 'type'",
    )
    check(
        "user macro block fragment must be last",
        'macro m(b: block, x: expr) { #b return #x; }\n',
        stderr="block fragment must be the last parameter",
    )
    check(
        "user macro expr list must be last",
        'macro m(xs: expr..., y: expr) { return #y; }\n',
        stderr="expr... fragment must be the last parameter",
    )
    check(
        "recursive user macro",
        'macro bad(x: expr) { return bad!(#x); }\n'
        'println(str(bad!(1)));\n',
        stderr="recursive user macro calls are not allowed",
    )
    check(
        "mutually recursive user macros",
        'macro a(x: expr) { return b!(#x); }\n'
        'macro b(x: expr) { return a!(#x); }\n'
        'println(str(a!(1)));\n',
        stderr="recursive user macro calls are not allowed",
    )
    check(
        "user macro wrong arity",
        'macro one(x: expr) { return #x; }\n'
        'println(str(one!(1, 2)));\n',
        stderr="macro expects a different number of arguments",
    )
    check(
        "user macro missing block",
        'macro with_block(b: block) { #b }\n'
        'with_block!();\n',
        stderr="macro expects a block argument",
    )
    check(
        "user macro zero arguments",
        'macro forty_two() { 42; }\n'
        'println(str(forty_two!()));\n'
        'println(str(forty_two!));\n',
        stdout="42\n42",
    )
    check(
        "user macro shorthand single name",
        'macro plus_one(x: expr) { #x + 1; }\n'
        'let x = 5;\n'
        'println(str(plus_one! x));\n',
        stdout="6",
    )
