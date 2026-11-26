#!/usr/bin/env python3
"""Core language: literals, operators, control flow, switch.

Numeric literal and operator cases live in
tests/skein/language_core_numbers.skein.  Iteration cases live in
tests/skein/language_core_iteration.skein.  Logical, bitwise, bool, and
ternary cases live in tests/skein/language_core_operators.skein.  Switch
cases live in tests/skein/language_core_switch.skein.  Pattern matching cases
live in tests/skein/language_core_match.skein.  Increment, compound assignment,
const propagation, and slice cases live in
tests/skein/language_core_updates.skein.  Default-parameter and closure cases
live in tests/skein/language_core_defaults.skein.  Lambda, pipe, placeholder,
string-append, and protocol cases live in tests/skein/language_core_functions.skein
and tests/skein/language_core_traits.skein.  Ref / weak / `?.` cases live in
tests/skein/language_core_refs.skein.  Control-flow header forms live in
tests/skein/language_core_headers.skein.
"""
import os

from harness import *  # noqa: F401,F403


def run_cases():
    run_skein("language_core_numbers.skein")
    run_skein("language_core_iteration.skein")
    run_skein("language_core_operators.skein")
    run_skein("language_core_switch.skein")
    run_skein("language_core_match.skein")
    run_skein("language_core_updates.skein")
    run_skein("language_core_defaults.skein")
    run_skein("language_core_functions.skein")
    run_skein("generic_functions.skein")
    run_skein("generic_structs.skein")
    run_skein("generic_enums.skein")
    run_skein("generic_protocols.skein")
    run_skein("generic_meta.skein")
    run_skein("generic_serde.skein")
    run_skein("iterator_methods.skein")
    run_skein("container_methods.skein")
    run_skein("traits.skein")
    run_skein("language_core_traits.skein")
    run_skein("language_core_refs.skein")
    run_skein("language_core_headers.skein")
    check(
        "numeric golden output",
        "1.5*2\n"
        "0.1 + 0.2\n",
        stdout_all=["= 3.0", "= 0.30000000000000004"],
    )
    check(
        "iteration golden output",
        "for x in [1, 2] { println(x); }\n",
        stdout="1\n2\n",
    )
    check(
        "in is a contextual keyword",
        "let in = 7;\n"
        "println(in);\n"
        "for in in [1, 2] { println(in); }\n",
        stdout="7\n1\n2\n",
    )
    check(
        "export is a contextual keyword",
        "let export = 7;\n"
        "println(export);\n",
        stdout="7\n",
    )
    check(
        "package, import, module, and include are contextual identifiers",
        "let import = 7;\n"
        "let package = 8;\n"
        "let module = 9;\n"
        "let include = 10;\n"
        "println(import + package + module + include);\n",
        stdout="34\n",
    )
    check(
        "operators golden output",
        "1 && 2\n"
        "0 ? 2 : 3\n",
        stdout_all=["= true", "= 3"],
    )
    check(
        "switch golden output",
        "switch (2) { case 1 => println(\"one\"); "
        "case 2, 3 => println(\"two or three\"); }\n",
        stdout="two or three",
    )
    check(
        "match golden output",
        "println(match (2) { case 2 => \"two\"; default => \"other\"; });\n",
        stdout="two",
    )
    check(
        "updates golden output",
        "let x = 1;\n"
        "x += 2;\n"
        "x\n",
        stdout="= 3",
    )
    check(
        "defaults golden output",
        "func f(a, b = a + 1) { return b; }\n"
        "println(f(10));\n",
        stdout="11",
    )
    check(
        "functions golden output",
        "func f(x) -> x * 2;\n"
        "println(f(21));\n",
        stdout="42",
    )
    check(
        "protocols golden output",
        "proto G { func d(self) { return \"g\"; } }\n"
        "struct S { x: int; }\n"
        "println(S { x: 1 }.d());\n",
        stdout="g",
    )
    check(
        "protocol is ordinary identifier",
        "let protocol = 7;\nprintln(protocol);\n",
        stdout="7",
    )
    check(
        "headers golden output",
        "let x = true;\n"
        "if x { println(\"yes\"); }\n",
        stdout="yes",
    )

    check("addition", "1+2\n", stdout="= 3")
    check("pipe mode has no REPL prompt", "1+2\n", stdout="= 3")
    assert "> " not in run("1+2\n").stdout
    check("ans and underscore reference last result",
          "1 + 2\nans * 4\n_\n_ + 1\n",
          stdout_all=["= 3", "= 12", "= 12", "= 13"])
    check("hex integer literal out of range", "0x8000000000000000\n",
          stderr="hexadecimal integer literal out of range")
    check("invalid binary digit", "0b102\n",
          stderr="invalid binary integer literal")
    check("invalid octal digit", "0o8\n",
          stderr="invalid octal integer literal")
    check("empty hex literal", "0x\n",
          stderr="invalid hexadecimal integer literal")
    check("hex literal trailing underscore", "0xF_\n",
          stderr="invalid hexadecimal integer literal")
    check("hex literal underscore after prefix", "0x_FF\n",
          stderr="invalid hexadecimal integer literal")
    check("consecutive integer underscores", "1__0\n",
          stderr="invalid numeric literal")


    check("for in without let infers element type",
          "for x in [1, 2] { let y: string = x; }\n",
          stderr="variable y expects string, got int")
    check("for in without let requires block",
          "let xs = [1, 2];\nfor x in xs println(x);\n",
          stderr="syntax error")
    check("for in without let struct literal requires parens",
          "struct P { x: int = 1; }\n"
          'for p in P { x: 1 } { println(p.x); }\n',
          stderr="syntax error")
    check("mutually recursive plain struct fields require ref",
          "struct A { b: B? = null; }\n"
          "struct B { a: A? = null; }\n",
          stderr="mutually recursive fields require ref B? or weak B?")
    check("for in without let two vars require pair or dict",
          "for k, v in [1, 2] { println(k); }\n",
          stderr="two-variable for-in over list[int] requires pair/tuple elements")
    check("for in without let rejects three variables over dict",
          'for a, b, c in ({"a": 1}) { println(a); }\n',
          stderr="tuple pattern requires a pair or tuple")
    check("if condition composite literal needs parens",
          'let d = {"a": 1};\n'
          'if d == {"a": 1} { println("yes"); }\n',
          stderr="syntax error")
    check("std.iter.for_each requires callable",
          "import std;\nstd.iter.for_each([1], 42)\n",
          stderr="std.iter.for_each expects a callable")
    check("std.iter.for_each arity",
          "import std;\nstd.iter.for_each([1])\n",
          stderr="std.iter.for_each expects 2 arguments")
    check("std.iter.for_each rejects three callback parameters",
          "import std;\n"
          "std.iter.for_each([1], func(a, b, c) { print(a); })\n",
          stderr="callback must accept 1 or 2 parameters")
    check("std.iter.for_each propagates callback errors",
          "import std;\n"
          "std.iter.for_each([1, 2], func(x) { missing_symbol; })\n",
          stderr="symbol not found missing_symbol")
    check("for_each is not a global builtin",
          "for_each([1], func(x) { })\n",
          stderr="symbol not found for_each")
    check("body brace must share header line",
          "func f() {\n"
          "  if (true)\n"
          "  {\n"
          "  }\n"
          "}\n"
          "f();\n",
          stderr="opening brace must be on the same line as its header")
    check("else must share closing brace line",
          "func f() {\n"
          "  if (false) {\n"
          "  }\n"
          "  else {\n"
          "  }\n"
          "}\n"
          "f();\n",
          stderr="else must be on the same line as the preceding token")
    check("do block expression value",
          "let v = do { 1; 2; };\nv\n",
          stdout="= 2")
    check("do block expression declaration and empty",
          "let a = do { let x = 5; };\n"
          "let b = do { };\n"
          "print(\"a=\" + str(a));\n"
          "print(\"b=\" + str(b));\n",
          stdout_all=["a=null", "b=null"])
    check("do block expression in expression context",
          "let v = 1 + do { 3; };\nv\n",
          stdout="= 4")
    check("do block expression return",
          "func f() {\n"
          "  let v = do { return 5; };\n"
          "  return v + 1;\n"
          "}\n"
          "f()\n",
          stdout="= 5",
          stderr="return inside a do block exits the enclosing function")
    check("do while body return does not warn",
          "func f() {\n"
          "  do { return 5; } while (0);\n"
          "  return 1;\n"
          "}\n"
          "f()\n",
          stdout="= 5",
          not_stderr="return inside a do block")
    check("lambda brace block return does not warn",
          "func f() {\n"
          "  let g = (x) -> {\n"
          "    return x + 1;\n"
          "  };\n"
          "  return g(2);\n"
          "}\n"
          "f()\n",
          stdout="= 3",
          not_stderr="return inside a do block")
    check("lambda do body return warns",
          "func f() {\n"
          "  let g = (x) -> do {\n"
          "    return x + 1;\n"
          "  };\n"
          "  return g(2);\n"
          "}\n"
          "f()\n",
          stdout="= 3",
          stderr="return inside a do block exits the enclosing function")
    check("lambda return inside do block does not warn",
          "func f() {\n"
          "  let v = do {\n"
          "    let g = (x) -> { return x + 1; };\n"
          "    g(1);\n"
          "  };\n"
          "  return v;\n"
          "}\n"
          "f()\n",
          stdout="= 2",
          not_stderr="return inside a do block")
    check("do block inside lambda body warns",
          "func f() {\n"
          "  let g = () -> {\n"
          "    let v = do { return 5; };\n"
          "    v;\n"
          "  };\n"
          "  return g();\n"
          "}\n"
          "f()\n",
          stdout="= 5",
          stderr="return inside a do block exits the enclosing function")
    check("nested func return inside do block does not warn",
          "func f() {\n"
          "  let v = do {\n"
          "    let g = func() { return 5; };\n"
          "    g();\n"
          "  };\n"
          "  return v;\n"
          "}\n"
          "f()\n",
          stdout="= 5",
          not_stderr="return inside a do block")
    check("do block expression break continue",
          "let i = 0;\n"
          "let total = 0;\n"
          "while (i < 5) {\n"
          "  let v = do {\n"
          "    i = i + 1;\n"
          "    if (i == 2) { continue; }\n"
          "    if (i == 4) { break; }\n"
          "    i * 10;\n"
          "  };\n"
          "  total = total + v;\n"
          "}\n"
          "print(total);\n"
          "print(i);\n",
          stdout_all=["40", "4"])
    check("do block expression then while",
          "let v = do { 7; };\n"
          "let i = 0;\n"
          "while (i < 2) { i = i + 1; }\n"
          "print(v);\n"
          "print(i);\n",
          stdout_all=["7", "2"])
    check("do while must share closing brace line",
          'do {\n  print("x");\n}\nwhile (false);\n',
          stderr="while must be on the same line as the preceding '}'")
    check("struct body brace must share header line",
          "func f() {\n"
          "  struct P\n"
          "  {\n"
          "    x: int = 1;\n"
          "  }\n"
          "}\n"
          "f();\n",
          stderr="opening brace must be on the same line as its header")
    check("switch body brace must share header line",
          "func f() {\n"
          "  switch (1)\n"
          "  {\n"
          '    case 1 => print("x");\n'
          "  }\n"
          "}\n"
          "f();\n",
          stderr="opening brace must be on the same line as its header")
    check("for in body brace must share header line",
          "func f() {\n"
          "  for x in [1, 2]\n"
          "  {\n"
          "    print(x);\n"
          "  }\n"
          "}\n"
          "f();\n",
          stderr="opening brace must be on the same line as its header")
    check("increment const binding",
          "const x = 1;\nx++;\n",
          stderr="cannot assign to constant x")
    check("const propagation can be disabled",
          "func f() {\n"
          "  const A = 2;\n"
          "  const B = A * 3 + 1;\n"
          "  return B;\n"
          "}\nprintln(f());\n",
          stdout="7",
          env=dict(os.environ, SKEIN_VM="1", SKEIN_NO_CONST_PROP="1"))
    check("increment string", 'let s = "a";\ns++;\n',
          stderr="cannot increment string")
    check("decrement bool", "let b = true;\nb--;\n",
          stderr="cannot decrement bool")
    check("increment literal", "1++\n",
          stderr="cannot increment this expression")
    check("increment missing dict key",
          'let d = {};\nd["x"]++;\n', stderr="key not found")
    check("increment integer overflow",
          "let x = 9223372036854775807;\nx++;\n",
          stderr="integer overflow")
    check("compound typed variable type error",
          "let x: int = 1;\nx += \"bad\";\n",
          stderr="cannot add int and string")
    check("compound const error",
          "const x = 1;\nx += 1;\n",
          stderr="cannot assign to constant x")
    check("slice step zero",
          "println([1, 2, 3][::0]);\n",
          stderr="slice step cannot be zero")
    check("slice bound must be integer",
          "println([1, 2, 3][1.0:]);\n",
          stderr="slice start must be an integer")
    check("slice unsupported type",
          "println(1[0:1]);\n",
          stderr="slice expects list, string, or bytes")
    check("bool print", "println(true)\nprintln(false)\n",
          stdout_all=["true", "false"])


    check("switch case scope",
          "switch (1) { case 1 => let y = 2; }\ny\n",
          stderr="symbol not found y")
    check("switch float selector",
          'switch (1.0) { case 1 => println("bad"); }\n',
          stderr="switch does not support float")
    check("switch float case",
          'switch (1) { case 1.0 => println("bad"); }\n',
          stderr="switch does not support float case value")
    check("switch list case",
          'switch (1) { case [1] => println("bad"); }\n',
          stderr="switch does not support list case value")
    check("switch builtin case",
          'switch (1) { case std.math.sqrt => println("bad"); }\n',
          stderr="switch does not support builtin case value")
    check("switch type name is not pattern",
          'switch (1) { case int => println("bad"); }\n',
          stderr="switch does not support builtin case value")
    check("switch case condition rejected",
          'switch (true) { case 1 < 2 => println("bad"); }\n',
          stderr="logical or comparison expressions are not allowed in case values")
    check("switch list selector",
          'switch ([1]) { default => println("bad"); }\n',
          stderr="switch does not support list")

    check("match binding scope",
          'match (1) { case x => 0; }\nx\n', stderr="symbol not found x")
    check("match binding is read-only",
          'match (1) { case x => { x = 2; } }\n',
          stderr="cannot assign to constant x")
    check("match duplicate binding",
          'match ({"a": 1, "b": 2}) { case {a: x, b: x} => 1; default => 0; }\n',
          stderr="pattern binds x more than once")
    check("match no arm matched",
          'match (9) { case 1 => "one"; }\n',
          stderr="match: no arm matched")
    check("match empty body",
          'match (1) {}\n', stderr="match requires at least one arm")
    check("match default must be last",
          'match (1) { default => "d"; case 1 => "one"; }\n',
          stderr="default must be the last match arm")


    check("protocol default conflict",
          'proto A { func m(self) { return 1; } }\n'
          'proto B { func m(self) { return 2; } }\n'
          'struct S { }\n',
          stderr="conflicts with another proto default")
    check("protocol default conflict resolved by concrete override",
          'struct S { }\n'
          'impl S { func m(self) { return 3; } }\n'
          'proto A { func m(self) { return 1; } }\n'
          'proto B { func m(self) { return 2; } }\n'
          'println(S {}.m());\n',
          stdout="3")
    check("protocol default conflict resolved by inline method",
          'struct S { func m(self) { return 3; } }\n'
          'proto A { func m(self) { return 1; } }\n'
          'proto B { func m(self) { return 2; } }\n'
          'println(S {}.m());\n',
          stdout="3")
    check("protocol missing method still rejected",
          'proto T { func m(self); }\n'
          'struct S { }\n'
          'func use(x: T) { return x.m(); }\n'
          'use(S {});\n',
          stderr="expected T, got S")
    check("protocol return type mismatch rejected",
          'proto R { func m(self) -> int; }\n'
          'struct S { }\n'
          'impl S { func m(self): string { return "x"; } }\n'
          'func use(x: R) { return x.m(); }\n'
          'use(S {});\n',
          stderr="expected R, got S")
    check("default parameter forward reference",
          'func f(a = b + 1, b = 2) { return a; }\nf();\n',
          stderr="parameter b is used before it is initialized")
    check("default parameter self reference",
          'func f(a = a + 1) { return a; }\nf();\n',
          stderr="parameter a is used before it is initialized")
    check("default parameter order",
          'func f(a, b = 1, c) { return c; }\n',
          stderr="without default cannot follow")
    check("default parameter type mismatch",
          'func f(x: int = "a") { return x; }\nf();\n',
          stderr="parameter x default expects int, got string")
    check("default parameter too few",
          'func f(a, b = a + 1) { return b; }\nf();\n',
          stderr="expects at least 1 argument(s)")
    check("default parameter too many",
          'func f(a, b = 1) { return b; }\nf(1, 2, 3);\n',
          stderr="expects at most 2 argument(s)")
    check("default parameter self default rejected",
          'struct S {}\nimpl S { func m(self = S {}, x = 1) -> x; }\n',
          stderr="method self cannot have a default value")
    check("default parameter protocol rejected",
          'proto T { func m(self, x: int = 1); }\n',
          stderr="proto method m cannot have default parameter values")

    check("trait keyword removed",
          'trait T { func m(self); }\n',
          stderr="syntax error")
    check("impl Protocol for Type removed",
          'proto D { func d(self); }\n'
          'struct S { }\n'
          'impl D for S { func d(self) { return 1; } }\n',
          stderr="syntax error")
    check("protocol with shorthand removed",
          'proto D { func d(self) { return 1; } }\n'
          'struct S with D { }\n',
          stderr="syntax error")
    check("export protocol removed",
          'export proto D { func d(self); }\n',
          stderr="syntax error")
    check("std.meta.traits removed",
          'import std;\n'
          'import std.meta;\n'
          'println(str(std.dict.has(std.meta, "traits")));\n',
          stdout="0")
    check("with remains ordinary identifier",
          'let with = 7;\n'
          'struct S { with: int; }\n'
          'let s = S { with: 9 };\n'
          'println(with + s.with);\n',
          stdout="16")

    check("export expression function",
          'package p { export func twice(x) -> x * 2; }\n'
          'println(p.twice(21));\n',
          stdout="42")


    check("pipe apply operator removed",
          'func h(x) { return y -> x + y; }\n'
          'println(4 |>> h(3));\n',
          stderr="syntax error")
    check("parenthesized lambda parameter must be a name",
          "let f = (1) -> 2;\n",
          stderr="lambda parameter must be a name")
    check("numbered placeholder rejects gaps",
          'func f(a, b) { return a; }\n'
          'let g = f($2);\n',
          stderr="placeholder indices must be contiguous")
    check("numbered placeholder rejects mixed forms",
          'func f(a, b) { return a; }\n'
          'let g = f($, $1);\n',
          stderr="cannot mix bare $ and numbered")
    check("numbered placeholder rejects zero",
          'func f(a) { return a; }\n'
          'let g = f($0);\n',
          stderr="invalid placeholder $0")
    check("numbered placeholder rejects multi-digit form",
          'func f(a) { return a; }\n'
          'let g = f($10);\n',
          stderr="invalid placeholder $10")
    check("numbered placeholder rejects braced form",
          'func f(a) { return a; }\n'
          'let g = f(${1});\n',
          stderr="invalid placeholder ${1}")
    check("string append keeps error message",
          "import std;\n"
          "let s = \"a\";\n"
          "s = s + 1;\n",
          stderr="cannot add string and int")

    check("struct constructor",
          "struct Point { x: float; y: float; }\n"
          "impl Point {\n"
          "  func new(x, y) { return Point { x: x, y: y }; }\n"
          "}\n"
          "let p = Point.new(3.0, 4.0);\n"
          "println(str(p.x) + \" \" + str(p.y));\n",
          stdout="3.0 4.0")

    check("inline struct constructor",
          "struct Point {\n"
          "  x: float;\n"
          "  func new(x) { return Point { x: x }; }\n"
          "}\n"
          "println(str(Point.new(1.5).x));\n",
          stdout="1.5")

    check("constructor validation",
          "struct Point { x: float; }\n"
          "impl Point {\n"
          "  func new(x) {\n"
          "    if (x < 0.0) { fail \"x must be >= 0\"; }\n"
          "    return Point { x: x };\n"
          "  }\n"
          "}\n"
          "Point.new(-1.0);\n",
          stderr="x must be >= 0")

    check("constructor is not an instance method",
          "struct Point { x: float; }\n"
          "impl Point { func new(x) { return Point { x: x }; } }\n"
          "let p = Point.new(1.0);\n"
          "p.new(2.0);\n",
          stderr="not found in Point")

    check("constructor return type is implicit",
          "struct Point { x: float; }\n"
          "impl Point { func new(x): Point { return Point { x: x }; } }\n",
          stderr="constructor return type is implicit")

    check("constructor requires a struct type",
          "enum Color { Red; }\n"
          "impl Color { func new() { return Color.Red; } }\n",
          stderr="constructor requires a struct type")

    check("constructor cannot have decorators",
          "func dec(fn) { return fn; }\n"
          "struct Point { x: float; }\n"
          "impl Point { @dec func new(x) { return Point { x: x }; } }\n",
          stderr="constructor new cannot have decorators")

    check("constructor implicitly wraps to ref",
          "struct Point { x: float; }\n"
          "impl Point { func new(x) { return Point { x: x }; } }\n"
          "let a: ref Point = Point.new(1.0);\n"
          "let b = a;\n"
          "b.x = 9.0;\n"
          "println(str(a.x));\n",
          stdout="9.0")

    check("struct literal implicitly wraps to ref",
          "struct Point { x: float; }\n"
          "let a: ref Point = Point { x: 1.0 };\n"
          "let b = a;\n"
          "b.x = 4.0;\n"
          "println(str(a.x));\n",
          stdout="4.0")

    check("struct value requires explicit ref",
          "struct Point { x: float; }\n"
          "impl Point { func new(x) { return Point { x: x }; } }\n"
          "let s = Point.new(1.0);\n"
          "let a: ref Point = s;\n",
          stderr="expected ref Point, got Point")

    check("ref reassignment wraps a fresh constructor",
          "struct Point { x: float; }\n"
          "impl Point { func new(x) { return Point { x: x }; } }\n"
          "let a = ref Point { x: 1.0 };\n"
          "a = Point.new(2.0);\n"
          "let b = a;\n"
          "b.x = 9.0;\n"
          "println(str(a.x));\n",
          stdout="9.0")

    check("ref field store wraps a fresh constructor",
          "struct Point { x: float; }\n"
          "impl Point { func new(x) { return Point { x: x }; } }\n"
          "struct Holder { p: ref Point; }\n"
          "let h = Holder { p: Point.new(1.0) };\n"
          "h.p = Point.new(2.0);\n"
          "println(str(h.p.x));\n",
          stdout="2.0")

    check("ref parameter wraps a fresh constructor",
          "struct Point { x: float; }\n"
          "impl Point { func new(x) { return Point { x: x }; } }\n"
          "func bump(p: ref Point) { p.x = 9.0; }\n"
          "bump(Point.new(1.0));\n"
          "println(\"ok\");\n",
          stdout="ok")

    check("explicit ref conversion copies a struct value",
          "struct Point { x: float; }\n"
          "let s = Point { x: 4.0 };\n"
          "let a = ref(s);\n"
          "a.x = 5.0;\n"
          "println(str(s.x) + \" \" + str(a.x));\n",
          stdout="4.0 5.0")

    check("as ref wraps a struct value",
          "struct Point { x: float; }\n"
          "let s = Point { x: 1.0 };\n"
          "let a = s as ref Point;\n"
          "a.x = 7.0;\n"
          "println(str(a.x));\n",
          stdout="7.0")

    check("P5.2 as is an identifier outside cast, alias and pattern slots",
          "import std;\n"
          "struct Box { as: int; }\n"
          "let as = 7;\n"
          "let b = Box { as: as };\n"
          "println(str(b.as));\n"
          "println(str(1 as float));\n"
          "println(str(as));\n",
          stdout="7\n1\n7")

    check("P5.2 as can be a user macro name",
          "macro as(x: expr) { #x + 100; }\n"
          "println(str(as!(1)));\n",
          stdout="101")

    check("P5.2 as remains the import alias separator",
          "import std.io as io;\n"
          "import {println as p} from std.io;\n"
          'p("alias-ok");\n',
          stdout="alias-ok")

    check("P5.2 as remains the pattern binding separator",
          'match ({"a": 1}) { case {a} as whole => '
          'println(str(whole["a"] + a)); }\n'
          "match (5) { case is int as n => println(str(n + 1)); }\n",
          stdout="2\n6")

    check("P5.3 ref is an identifier outside ref slots",
          "import std;\n"
          "let ref = 7;\n"
          "struct Box { ref: int; }\n"
          "let b = Box { ref: ref };\n"
          "println(str(ref));\n"
          "println(str(b.ref));\n",
          stdout="7\n7")

    check("P5.3 ref conversion, literal and type prefix still work",
          "struct Point { x: float; }\n"
          "let s = Point { x: 1.0 };\n"
          "let a = ref(s);\n"
          "a.x = 7.0;\n"
          "let b = ref Point { x: 2.0 };\n"
          "let c: ref Point = a;\n"
          "println(str(a.x + b.x + c.x));\n",
          stdout="16.0")

    check("P5.3 ref can be a user macro name",
          "macro ref(x: expr) { #x + 100; }\n"
          "println(str(ref!(1)));\n",
          stdout="101")

    check("ref rejects a ref handle",
          "struct Point { x: float; }\n"
          "let a = ref Point { x: 1.0 };\n"
          "let b = ref(a);\n",
          stderr="ref expects a struct value, got ref Point")

    check("ref rejects null",
          "let b = ref(null);\n",
          stderr="ref expects a struct value, got null")

    check("P5.4 weak is an identifier outside type slots",
          "import std;\n"
          "let weak = 7;\n"
          "struct Box { weak: int; }\n"
          "let b = Box { weak: weak };\n"
          "println(str(weak));\n"
          "println(str(b.weak));\n"
          "println(str(weak as float));\n",
          stdout="7\n7\n7")

    check("P5.4 weak type prefix still works",
          "import std;\n"
          "struct Node { value: int; }\n"
          "let n = ref Node { value: 1 };\n"
          "let w: weak Node = n;\n"
          "println(str(std.ref.upgrade(w) != null));\n",
          stdout="true")

    check("P5.4 weak can be a user macro name",
          "macro weak(x: expr) { #x + 100; }\n"
          "println(str(weak!(1)));\n",
          stdout="101")

    check("P5.5 is is an identifier outside type-test slots",
          "import std;\n"
          "let is = 7;\n"
          "struct Box { is: int; }\n"
          "let b = Box { is: is };\n"
          "println(str(is));\n"
          "println(str(b.is));\n"
          "println(str(is is int));\n"
          "func f(is: int) { return is + 1; }\n"
          "println(str(f(2)));\n"
          "println(str(is as float));\n",
          stdout="7\n7\ntrue\n3\n7")

    check("P5.5 is operator and type pattern still work",
          "import std;\n"
          "match (5) { case is int as n => println(str(n + 1)); }\n"
          "println(str(5 is int));\n",
          stdout="6\ntrue")

    check("P5.5 is can be a user macro name",
          "macro is(x: expr) { #x + 100; }\n"
          "println(str(is!(1)));\n",
          stdout="101")

    check("weak accepts a ref handle",
          "import std;\n"
          "struct Node { value: int; }\n"
          "let n = ref Node { value: 1 };\n"
          "let w: weak Node = n;\n"
          "println(str(std.ref.upgrade(w) != null));\n",
          stdout="true")

    check("container elements do not implicitly wrap",
          "struct Point { x: float; }\n"
          "let points: list[ref Point] = [Point { x: 1.0 }];\n",
          stderr="expected ref Point, got Point")