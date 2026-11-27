#!/usr/bin/env python3
"""Tests for built-in AST macros."""

import os
import subprocess
import tempfile

from harness import BIN, check, run, run_file, run_skein


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
        "user macro preserves generic type arguments in expr fragment",
        'func identity[T any](x: T): T { return x; }\n'
        'macro keep(x: expr) { #x; }\n'
        'func test(): int {\n'
        '  let g = keep!(identity[int]);\n'
        '  return g(7);\n'
        '}\n'
        'println(str(test()));\n',
        stdout="7",
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
        "P5.1 error! uses the ordinary macro path and can be shadowed",
        'import std;\n'
        'macro error(x: expr) { #x + 100; }\n'
        'func bare(): int { return error!(1); }\n'
        'func canonical(): int {\n'
        '  std.log.set_level("trace");\n'
        '  std.log.error!("canonical");\n'
        '  return 7;\n'
        '}\n'
        'println(str(bare()));\n'
        'println(str(canonical()));\n',
        stdout="101\n7",
        stderr=" ERROR canonical",
    )
    check(
        "P5.1 on/error are identifiers outside a handler",
        'import std;\n'
        'struct Box { on: int; error: int; }\n'
        'let on = 1;\n'
        'let error = 2;\n'
        'let b = Box { on: on, error: error };\n'
        'func f() { fail "x"; }\n'
        'println(str(b.on + b.error));\n'
        'println(str(f() on error(e) { recover on + error; }));\n',
        stdout="3\n3",
    )
    check(
        "P5.1 error stays in the type namespace",
        'import std;\n'
        'let e: error = 1;\n',
        stderr="variable e expects error, got int",
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
        "user macro type fragment annotation",
        'macro make(T: type, v: expr) { let x: #T = #v; return x; }\n'
        'func test(): int { return make!(int, 7); }\n'
        'println(str(test()));\n',
        stdout="7",
    )
    check(
        "user macro type fragment cast",
        'macro cast_it(T: type, v: expr) { let x = #v as #T; return x; }\n'
        'func test(): int { return cast_it!(int, 7); }\n'
        'println(str(test()));\n',
        stdout="7",
    )
    check(
        "user macro type fragment is proto",
        'proto Drawable { func draw(self); }\n'
        'struct D { func draw(self) { } }\n'
        'macro is_p(P: type, v: expr) { return #v is #P; }\n'
        'func test(): bool { let d = D { }; return is_p!(Drawable, d); }\n'
        'println(str(test()));\n',
        stdout="true",
    )
    check(
        "user macro type fragment nullable",
        "macro nullable(T: type, v: expr) { let x: #T? = #v; return x == null; }\n"
        "func test(): bool { return nullable!(int, 7); }\n"
        "println(str(test()));\n",
        stdout="false",
    )
    check(
        "user macro type fragment ref and weak",
        "macro wrappers(T: type) {\n"
        "  let r: ref #T? = null;\n"
        "  let w: weak #T? = null;\n"
        "  return 1;\n"
        "}\n"
        "func test(): int { return wrappers!(int); }\n"
        "println(str(test()));\n",
        stdout="1",
    )
    check(
        "user macro type fragment function parameter and return",
        "macro mkfn(T: type) {\n"
        "  let f = func(x: #T): #T { return x; };\n"
        "  return f(7);\n"
        "}\n"
        "func test(): int { return mkfn!(int); }\n"
        "println(str(test()));\n",
        stdout="7",
    )
    check(
        "user macro type fragment explicit generic type",
        'macro make(T: type, v: expr) { let x = #v as #T; return x; }\n'
        'func test(): list[int] { return make!(type(list[int]), [1, 2, 3]); }\n'
        'println(str(test()[1]));\n',
        stdout="2",
    )
    check(
        "user macro type fragment explicit function type",
        'macro make(T: type, v: expr) { let x = #v as #T; return x; }\n'
        'func test(): (int) -> int {\n'
        '  return make!(type((int) -> int), func(x: int): int -> x * 2);\n'
        '}\n'
        'println(str(test()(5)));\n',
        stdout="10",
    )
    check(
        "user macro type fragment explicit nested type",
        'macro inner(T: type) { let x = 1 as #T; return x; }\n'
        'macro outer(T: type) { return inner!(type(#T)); }\n'
        'func test(): int { return outer!(int); }\n'
        'println(str(test()));\n',
        stdout="1",
    )
    check(
        "user macro type fragment must be a type name",
        'macro bad(T: type) { return 1; }\n'
        'func test(): int { return bad!(1 + 2); }\n'
        'println(str(test()));\n',
        stderr="type fragment expects a type name",
    )
    check(
        "user macro type fragment cannot be a value",
        'macro bad(T: type) { let x = #T; return 1; }\n'
        'func test(): int { return bad!(int); }\n'
        'println(str(test()));\n',
        stderr="type fragment cannot be used as a value or name",
    )
    check(
        "user macro expr fragment cannot be a type",
        'macro bad(T: expr, v: expr) { let x: #T = #v; return x; }\n'
        'func test(): int { return bad!(int, 7); }\n'
        'println(str(test()));\n',
        stderr="macro fragment cannot be used here",
    )
    check(
        "user macro type argument requires a type fragment",
        'macro bad(v: expr) { return #v; }\n'
        'func test(): int { return bad!(type(int)); }\n'
        'println(str(test()));\n',
        stderr="type argument requires a type fragment",
    )
    check(
        "user macro type argument rejects expression syntax",
        'macro bad(T: type) { let x = 1 as #T; return x; }\n'
        'func test(): int { return bad!(type(1 + 2)); }\n'
        'println(str(test()));\n',
        stderr="syntax error",
    )
    check(
        "builtin macro rejects type argument",
        'info!(type(int));\n'
        'println("ok");\n',
        stderr="does not accept type arguments",
    )
    check(
        "user macro type fragment rejects variadic",
        'macro bad(T: type...) { return 1; }\n'
        'func test(): int { return bad!(int); }\n'
        'println(str(test()));\n',
        stderr="only expr parameters support '...'",
    )
    check(
        "user macro type fragment rejects generic arguments",
        'struct Box[T any] { value: T; }\n'
        'macro bad(P: type) { let x: Box[#P] = 1; return 1; }\n'
        'func test(): int { return bad!(int); }\n'
        'println(str(test()));\n',
        stderr="type fragment cannot be used inside a generic type",
    )
    check(
        "user macro cannot generate declarations",
        'macro bad() { func f() { return 1; } return f(); }\n'
        'func test(): int { return bad!(); }\n'
        'println(str(test()));\n',
        stderr="macro template cannot generate declarations",
    )
    check(
        "user macro cannot generate struct declarations",
        'macro bad() { struct S { x: int; } return 1; }\n'
        'func test(): int { return bad!(); }\n'
        'println(str(test()));\n',
        stderr="macro template cannot generate declarations",
    )
    check(
        "user macro cannot generate impl declarations",
        'proto P { func f(self); }\n'
        'macro bad() { impl P { } return 1; }\n'
        'func test(): int { return bad!(); }\n'
        'println(str(test()));\n',
        stderr="macro template cannot generate declarations",
    )
    check(
        "user macro cannot generate generic declarations",
        'macro bad() { func f[T any]() { return 1; } return 1; }\n'
        'func test(): int { return bad!(); }\n'
        'println(str(test()));\n',
        stderr="macro template cannot generate generic syntax",
    )
    check(
        "user macro can generate literal generic calls",
        'func id[T any](x: T) { return x; }\n'
        'macro call(v: expr) { return id[int](#v); }\n'
        'func test(): int { return call!(7); }\n'
        'println(str(test()));\n',
        stdout="7",
    )
    check(
        "user macro can generate generic function values",
        'func id[T any](x: T) { return x; }\n'
        'macro get() { let g = id[int]; return g(7); }\n'
        'func test(): int { return get!(); }\n'
        'println(str(test()));\n',
        stdout="7",
    )
    check(
        "user macro cannot generate generic calls with type fragments",
        'func id[T any](x: T) { return x; }\n'
        'macro bad(T: type, v: expr) { return id[#T](#v); }\n'
        'func test(): int { return bad!(int, 7); }\n'
        'println(str(test()));\n',
        stderr="macro template cannot generate generic syntax",
    )
    check(
        "user macro can generate literal generic struct literals",
        'struct Box[T any] { value: T; }\n'
        'macro make(v: expr) { let b = Box[int] { value: #v }; return b.value; }\n'
        'func test(): int { return make!(7); }\n'
        'println(str(test()));\n',
        stdout="7",
    )
    check(
        "user macro can generate nested literal generic struct literals",
        'struct Box[T any] { value: T; }\n'
        'macro make(v: expr) {\n'
        '  let b = Box[list[int]] { value: [#v] };\n'
        '  return b.value[0];\n'
        '}\n'
        'func test(): int { return make!(7); }\n'
        'println(str(test()));\n',
        stdout="7",
    )
    check(
        "user macro can generate plain struct literals",
        'struct Point { x: int; }\n'
        'macro make(v: expr) { let p = Point { x: #v }; return p.x; }\n'
        'func test(): int { return make!(7); }\n'
        'println(str(test()));\n',
        stdout="7",
    )
    check(
        "user macro can generate literal generic enum literals",
        'enum Option[T any] { None; Some { value: T; } }\n'
        'macro make(v: expr) {\n'
        '  let o = Option[int].Some { value: #v };\n'
        '  return o.value;\n'
        '}\n'
        'func test(): int { return make!(7); }\n'
        'println(str(test()));\n',
        stdout="7",
    )
    check(
        "user macro can generate fieldless literal generic enum literals",
        'enum Option[T any] { None; Some { value: T; } }\n'
        'macro none() { let o = Option[int].None; return 1; }\n'
        'func test(): int { return none!(); }\n'
        'println(str(test()));\n',
        stdout="1",
    )
    check(
        "user macro cannot generate generic struct literals with type fragments",
        'struct Box[T any] { value: T; }\n'
        'macro bad(T: type) { let b = Box[#T] { value: 1 }; return 1; }\n'
        'func test(): int { return bad!(int); }\n'
        'println(str(test()));\n',
        stderr="macro template cannot generate generic syntax",
    )
    check(
        "user macro cannot generate generic enum literals with type fragments",
        'enum Option[T any] { None; Some { value: T; } }\n'
        'macro bad(T: type) { let o = Option[#T].None; return 1; }\n'
        'func test(): int { return bad!(int); }\n'
        'println(str(test()));\n',
        stderr="macro template cannot generate generic syntax",
    )
    check(
        "user macro cannot nest type fragments in literal generic arguments",
        'struct Box[T any] { value: T; }\n'
        'macro bad(T: type) {\n'
        '  let b = Box[list[#T]] { value: [1] };\n'
        '  return 1;\n'
        '}\n'
        'func test(): int { return bad!(int); }\n'
        'println(str(test()));\n',
        stderr="macro template cannot generate generic syntax",
    )
    check(
        "user macro type fragment cannot reach introspection",
        'macro bad(T: type) { let k = kind_of(#T); return 1; }\n'
        'func test(): int { return bad!(int); }\n'
        'println(str(test()));\n',
        stderr="type fragment cannot be used as a value or name",
    )
    check(
        "user macro allows anonymous function expressions",
        'macro mk() { let f = func(x) { return x; }; return f(7); }\n'
        'func test(): int { return mk!(); }\n'
        'println(str(test()));\n',
        stdout="7",
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
        "recursive user macro inside struct literal field",
        'struct Box[T any] { value: T; }\n'
        'macro bad() { let b = Box[int] { value: bad!() }; return 1; }\n'
        'func test(): int { return bad!(); }\n'
        'println(str(test()));\n',
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

    # G5 core constraints: no text expansion, no compile-time execution, no
    # first-class macro values.  Hygiene, declaration-before-use, recursion,
    # generics boundaries and type introspection are covered above.
    check(
        "G5 macro call text is not textually expanded",
        'macro twice(x: expr) { return #x * 2; }\n'
        'let s = "twice!(21)";\n'
        '// twice!(99)\n'
        'let twice = 7;\n'
        'println(s);\n'
        'println(str(twice));\n',
        stdout="twice!(21)\n7",
    )
    check(
        "G5 unused macro is neither executed nor typechecked",
        'macro side() {\n'
        '  let x: int = "not checked before expansion";\n'
        '  println("TEMPLATE RAN");\n'
        '  return x;\n'
        '}\n'
        'println("DONE");\n',
        stdout="DONE",
    )
    check(
        "G5 macro is not a first-class value",
        'macro twice(x: expr) { return #x * 2; }\n'
        'let f = twice;\n'
        'println(str(f(2)));\n',
        stderr="symbol not found twice",
    )
    check(
        "user macro cannot generate enum declarations",
        'macro bad() { enum E { A; } return 1; }\n'
        'func test(): int { return bad!(); }\n',
        stderr="macro template cannot generate declarations",
    )
    check(
        "user macro cannot generate proto declarations",
        'macro bad() { proto P { func f(self); } return 1; }\n'
        'func test(): int { return bad!(); }\n',
        stderr="macro template cannot generate declarations",
    )
    check(
        "user macro type fragment cannot reach type_name",
        'macro bad(T: type) { let k = type_name(#T); return 1; }\n'
        'func test(): int { return bad!(int); }\n',
        stderr="type fragment cannot be used as a value or name",
    )
    check(
        "user macro expr list cannot be used as a value",
        'macro bad(xs: expr...) { return #xs; }\n',
        stderr="expr... fragment must be used in an argument list",
    )
    check(
        "user macro block fragment cannot be used as an expression",
        'macro bad(b: block) { let x = #b; return 1; }\n',
        stderr="unexpected macro block fragment",
    )

    # Cross-package user macros (P4.3).
    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
            return path

        def run_main(source):
            return run_file(write("main.skein", source))

        write("util.skein",
              'println("UTIL RAN");\n'
              'package util {\n'
              '  export func twice() { return 7; }\n'
              '  export macro twice(x: expr) { #x * 2; }\n'
              '  macro scale(x: expr) { #x * 3; }\n'
              '  export macro triple(x: expr) { scale!(#x); }\n'
              '  export macro wrap2(value: expr, body: block) {\n'
              '    let tmp = #value;\n'
              '    #body\n'
              '    return tmp;\n'
              '  }\n'
              '}\n')

        proc = run_main(
            'import {twice, twice! as t} from util;\n'
            'func test(): int { return t!(21) + twice(); }\n'
            'println(str(test()));\n')
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["UTIL RAN", "49"], proc.stdout

        proc = run_main(
            'import {triple!} from util;\n'
            'func test(): int { return triple!(7); }\n'
            'println(str(test()));\n')
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["21"], proc.stdout

        proc = run_main(
            'import {wrap2!} from util;\n'
            'func test(): int {\n'
            '  let tmp = 100;\n'
            '  return wrap2!(1 + 2) { tmp = 999; };\n'
            '}\n'
            'println(str(test()));\n')
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["3"], proc.stdout

        write("types.skein",
              'package types {\n'
              '  export struct P { x: int; }\n'
              '  proto D { func d(self); }\n'
              '  impl P {\n'
              '    func d(self) { return self.x; }\n'
              '  }\n'
              '  export func make() { return P { x: 7 }; }\n'
              '  export macro typed() { let x: P = types.make(); return x.x; }\n'
              '}\n')
        write("typed_main.skein",
              'import types;\n'
              'import types as lib;\n'
              'import {typed!} from types;\n'
              'struct P { y: string; }\n'
              'macro is_p(P: type, v: expr) { return #v is #P; }\n'
              'func test(): bool { return is_p!(lib.D, lib.make()); }\n'
              'func test_typed(): int { return typed!(); }\n'
              'println(str(test()));\n'
              'println(str(test_typed()));\n')
        proc = run_file(os.path.join(tmp, "typed_main.skein"))
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["true", "7"], proc.stdout

        write("helpers.skein",
              'package helpers {\n'
              '  export macro scale(x: expr) { #x * 3; }\n'
              '}\n')
        write("chain.skein",
              'package chain {\n'
              '  import {scale!} from helpers;\n'
              '  export macro six(x: expr) { scale!(#x) * 2; }\n'
              '}\n')
        proc = run_main(
            'import {six!} from chain;\n'
            'func test(): int { return six!(7); }\n'
            'println(str(test()));\n')
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["42"], proc.stdout

        proc = run_main('import {missing!} from util;\nprintln(1);\n')
        assert proc.returncode != 0, proc.stdout
        assert "macro not exported by package: missing" in proc.stderr, \
            proc.stderr

        write("a.skein",
              'package a {\n'
              '  import {bm!} from b;\n'
              '  export macro am(x: expr) { bm!(#x); }\n'
              '}\n')
        write("b.skein",
              'package b {\n'
              '  import {am!} from a;\n'
              '  export macro bm(x: expr) { am!(#x); }\n'
              '}\n')
        proc = run_main('import {am!} from a;\nprintln(1);\n')
        assert proc.returncode != 0, proc.stdout
        assert "cyclic macro import" in proc.stderr, proc.stderr

        write("a2.skein",
              'package a2 {\n'
              '  export macro am(x: expr) { #x; }\n'
              '  import {bm!} from b2;\n'
              '}\n')
        write("b2.skein",
              'package b2 {\n'
              '  import {am!} from a2;\n'
              '  export macro bm(x: expr) { am!(#x); }\n'
              '}\n')
        proc = run_main('import {am!} from a2;\nprintln(1);\n')
        assert proc.returncode != 0, proc.stdout
        assert "cyclic macro import" in proc.stderr, proc.stderr

        proc = run_main(
            'import {twice!} from util;\n'
            'macro twice(x: expr) { #x * 3; }\n'
            'func test(): int { return twice!(7); }\n'
            'println(str(test()));\n')
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["21"], proc.stdout

        proc = run_main(
            'import {twice!, twice! as twice} from util;\nprintln(1);\n')
        assert proc.returncode != 0, proc.stdout
        assert "duplicate import alias" in proc.stderr, proc.stderr

        write("shadow.skein",
              'package shadow {\n'
              '  export macro info(x: expr) { #x + 1; }\n'
              '}\n')
        proc = run_main(
            'import std;\n'
            'std.log.set_level("error");\n'
            'import {info!} from shadow;\n'
            'println(str(info!(1)));\n'
            'std.log.info!("bad");\n')
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["2"], proc.stdout
        assert proc.stderr == "", proc.stderr

        main_path = write(
            "lint_main.skein",
            'import {twice!} from util;\n'
            'func test(): int { return twice!(21); }\n')
        proc = subprocess.run(
            [BIN, "lint", main_path],
            capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert proc.returncode == 0, proc.stderr
        assert "UTIL RAN" not in proc.stdout + proc.stderr, proc.stderr
