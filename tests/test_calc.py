#!/usr/bin/env python3
"""Golden/smoke tests for the calc-demo interpreter."""

import os
import signal
import subprocess
import sys
import tempfile
import time


BIN = sys.argv[1] if len(sys.argv) > 1 else "./calc-demo"


def run(source, timeout=5):
    try:
        return subprocess.run(
            [BIN],
            input=source,
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise AssertionError(f"interpreter timed out for input {source!r}") from exc


def run_file(path, timeout=5):
    return subprocess.run(
        [BIN, path],
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=timeout,
    )


def check(name, source, *, stdout=None, stdout_all=None, stderr=None, code=0):
    proc = run(source)
    combined = proc.stdout + proc.stderr
    assert "Segmentation fault" not in combined, f"{name}: crashed"
    assert proc.returncode == code, (
        f"{name}: expected exit {code}, got {proc.returncode}\n"
        f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
    )
    if stdout is not None:
        assert stdout in proc.stdout, (
            f"{name}: expected stdout to contain {stdout!r}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    if stdout_all is not None:
        for expected in stdout_all:
            assert expected in proc.stdout, (
                f"{name}: expected stdout to contain {expected!r}\n"
                f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
    if stderr is not None:
        assert stderr in proc.stderr, (
            f"{name}: expected stderr to contain {stderr!r}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )


def main():
    check("addition", "1+2\n", stdout="= 3")
    check("float", "1.5*2\n", stdout="= 3.0")
    check("float keeps type in output", "1.0\n", stdout="= 1.0")
    check("float shortest round-trip", "1.23456\n", stdout="= 1.23456")
    check("float binary rounding", "0.1 + 0.2\n",
          stdout="= 0.30000000000000004")
    check("integer division", "10/4\n", stdout="= 2")
    check("float division", "10/4.0\n", stdout="= 2.5")
    check("integer division truncates toward zero", "-7 / 4\n", stdout="= -1")
    check("int64 addition", "2147483647 + 1\n", stdout="= 2147483648")
    check("int64 maximum", "9223372036854775807\n",
          stdout="= 9223372036854775807")
    check("integer overflow", "9223372036854775807 + 1\n",
          stderr="integer overflow")
    check("integer multiplication boundary",
          "3037000499 * 3037000499\n", stdout="= 9223372030926249001")
    check("integer multiplication overflow",
          "3037000500 * 3037000500\n", stderr="integer overflow")
    check("INT64_MIN times -1", "(-9223372036854775807 - 1) * -1\n",
          stderr="integer overflow")
    check("out-of-range integer literal becomes float",
          "9223372036854775808\n", stdout="= 9.223372036854776e+18")
    check("INT64_MIN division overflow",
          "(-9223372036854775807 - 1) / -1\n", stderr="integer overflow")
    check("int64 exact mixed comparison",
          "9007199254740993 == 9007199254740992.0\n", stdout="= 0")

    check("modulo", "7 % 4\n", stdout="= 3")
    check("float modulo", "7.5 % 2\n", stdout="= 1.5")
    check("modulo sign follows dividend", "-7 % 4\n", stdout="= -3")
    check("modulo negative divisor", "7 % -4\n", stdout="= 3")
    check("modulo by zero", "1 % 0\n", stderr="modulo by zero")
    check("logical and", "1 && 2\n", stdout="= 1")
    check("logical and short-circuit", "0 && (1/0)\n", stdout="= 0")
    check("logical or", "0 || 2\n", stdout="= 1")
    check("logical or short-circuit", "1 || (1/0)\n", stdout="= 1")
    check("logical not", "!0\n", stdout="= 1")
    check("logical not string", '!""\n', stdout="= 1")
    check("bitwise and", "1 & 3\n", stdout="= 1")
    check("bitwise or", "1 | 2\n", stdout="= 3")
    check("bitwise xor", "5 ^ 3\n", stdout="= 6")
    check("bitwise not", "~0\n", stdout="= -1")
    check("shift left", "1 << 4\n", stdout="= 16")
    check("shift right", "16 >> 2\n", stdout="= 4")
    check("int64 shift", "1 << 62\n", stdout="= 4611686018427387904")
    check("shift count out of range", "1 << 64\n",
          stderr="shift count 64 out of range")
    check("bitwise vs equality precedence", "1 & 3 == 1\n", stdout="= 0")
    check("unary minus", "-3+5\n", stdout="= 2")
    check("unary minus alone", "-3\n", stdout="= -3")
    check("abs negative", "abs(-3)\n", stdout="= 3")
    check("abs overflow", "abs(-9223372036854775807 - 1)\n",
          stderr="integer overflow")

    check("nan equality is false", "sqrt(-1) == 1\n", stdout="= 0")
    check("nan inequality is true", "sqrt(-1) != 1\n", stdout="= 1")
    check("nan is not equal to itself", "sqrt(-1) == sqrt(-1)\n",
          stdout="= 0")
    check("nan ordering is false", "sqrt(-1) >= 1\n", stdout="= 0")

    check("divide by zero int", "1/0\n", stderr="division by zero")
    check("divide by zero float", "1.0/0\n", stderr="division by zero")

    check("if float truthy", 'if (1.5) { print("yes"); }\n', stdout="yes")
    check("if else", 'if (0) { print("yes"); } else { print("no"); }\n', stdout="no")

    check("zero-arg function", "func f() {\n  return 42;\n}\nf()\n", stdout="= 42")
    check(
        "return inside while",
        "func f(a) {\n  while (1) { return a; }\n}\nf(7)\n",
        stdout="= 7",
    )
    check(
        "recursion",
        "func fib(n) {\n"
        "  if (n < 2) { return n; }\n"
        "  return fib(n-1) + fib(n-2);\n"
        "}\n"
        "fib(10)\n",
        stdout="= 55",
    )
    check(
        "function value alias",
        "func f(a) { return a; }\nlet g = f;\nlet h = g;\nh(8)\n",
        stdout="= 8",
    )
    check(
        "anonymous function",
        "let double = func(x) { return x * 2; };\ndouble(21)\n",
        stdout="= 42",
    )
    check(
        "anonymous immediate call",
        "(func(x) { return x + 1; })(41)\n",
        stdout="= 42",
    )
    check(
        "anonymous higher order",
        "func apply(fn, x) { return fn(x); }\n"
        "apply(func(n) { return n * n; }, 9)\n",
        stdout="= 81",
    )
    check(
        "anonymous closure",
        "func make_adder(n) { return func(x) { return x + n; }; }\n"
        "let add5 = make_adder(5);\n"
        "add5(3)\n",
        stdout="= 8",
    )
    check(
        "anonymous recursion",
        "let fact = func(n) {\n"
        "  if (n <= 1) { return 1; }\n"
        "  return n * fact(n - 1);\n"
        "};\n"
        "fact(5)\n",
        stdout="= 120",
    )
    check(
        "anonymous function in list",
        "let fs = [func(x) { return x + 1; }, func(x) { return x * 2; }];\n"
        "fs[1](5)\n",
        stdout="= 10",
    )
    check(
        "anonymous duplicate parameter",
        "func(a, a) { return a; }\n",
        stderr="duplicate parameter a in function <anonymous>",
    )
    check(
        "anonymous arity",
        "func(a) { return a; }(1, 2)\n",
        stderr="function expects 1 argument(s), got 2",
    )
    check(
        "duplicate parameter",
        "func f(a,a) { return a; }\n",
        stderr="duplicate parameter",
    )
    check(
        "closure counter",
        "func make_counter() {\n"
        "  let n = 0;\n"
        "  func inc() { n = n + 1; return n; }\n"
        "  return inc;\n"
        "}\n"
        "let c1 = make_counter();\n"
        "let c2 = make_counter();\n"
        "c1()\n"
        "c1()\n"
        "c2()\n",
        stdout_all=["= 1", "= 2"],
    )
    check(
        "closure shared environment",
        "func make_pair() {\n"
        "  let n = 0;\n"
        "  func inc() { n = n + 1; return n; }\n"
        "  func get() { return n; }\n"
        "  let d = dict();\n"
        "  d[\"inc\"] = inc;\n"
        "  d[\"get\"] = get;\n"
        "  return d;\n"
        "}\n"
        "let p = make_pair();\n"
        "p[\"inc\"]()\n"
        "p[\"inc\"]()\n"
        "p[\"get\"]()\n",
        stdout_all=["= 1", "= 2"],
    )
    check(
        "closure factory",
        "func adder(x) {\n"
        "  func add(y) { return x + y; }\n"
        "  return add;\n"
        "}\n"
        "let a = adder(10);\n"
        "a(5)\n"
        "adder(3)(4)\n",
        stdout_all=["= 15", "= 7"],
    )
    check(
        "closure recursion",
        "func make_fact() {\n"
        "  func fact(n) {\n"
        "    if (n < 2) { return 1; }\n"
        "    return n * fact(n-1);\n"
        "  }\n"
        "  return fact;\n"
        "}\n"
        "let f = make_fact();\n"
        "f(5)\n",
        stdout="= 120",
    )
    check(
        "closure from block",
        "let c = 0;\n"
        "{\n"
        "  let x = 5;\n"
        "  func f() { return x; }\n"
        "  c = f;\n"
        "}\n"
        "c()\n",
        stdout="= 5",
    )
    check(
        "cycle collection preserves reachable closures",
        "func make_counter() {\n"
        "  let n = 0;\n"
        "  func inc() { n = n + 1; return n; }\n"
        "  return inc;\n"
        "}\n"
        "let c = make_counter();\n"
        + "1;\n" * 100
        + "c()\n"
        "c()\n",
        stdout_all=["= 1", "= 2"],
    )
    check(
        "cycle creation does not break execution",
        "func make_cycle() {\n"
        "  func f() { return f; }\n"
        "  return f;\n"
        "}\n"
        + "{ let g = make_cycle(); }\n" * 100
        + 'print("ok");\n',
        stdout="ok",
    )
    check(
        "function value in list",
        "func adder(x) { func add(y) { return x + y; } return add; }\n"
        "let fs = [adder(1), adder(2)];\n"
        "fs[0](10)\n"
        "fs[1](10)\n",
        stdout_all=["= 11", "= 12"],
    )
    check("underscore identifier", "let my_value = 3;\nmy_value\n",
          stdout="= 3")

    check("string concat", '"a" + "b"\n', stdout="= ab")
    check("string compare", '"a" < "b"\n', stdout="= 1")
    check("string equality", '"a" == "a"\n', stdout="= 1")
    check("string escape", 'print("a\\nb")\n', stdout="a\nb")
    check("string index", '"hello"[1]\n', stdout="= e")
    check("utf8 string index first", '"你好"[0]\n', stdout="= 你")
    check("utf8 string index second", '"你好"[1]\n', stdout="= 好")
    check("utf8 string index out of range", '"你好"[2]\n',
          stderr="out of range")
    check("string trim", 'trim("  hi\\t\\n")\n', stdout="= hi")
    check("string split", 'split("a,b,c", ",")\n', stdout="= [a, b, c]")
    check("string split empty fields", 'split(",a,", ",")\n',
          stdout="= [, a, ]")
    check("string split no match", 'split("abc", ",")\n', stdout="= [abc]")
    check("string split empty text", 'split("", ",") == [""]\n',
          stdout="= 1")
    check("string split empty separator", 'split("a,b", "")\n',
          stderr="split separator must not be empty")
    check("string split whitespace", 'split_ws("  a  b\\tc\\n d ")\n',
          stdout="= [a, b, c, d]")
    check("string split whitespace empty", 'split_ws("")\n',
          stdout="= []")
    check("string find", 'find("hello", "ll")\n', stdout="= 2")
    check("string find missing", 'find("hello", "x")\n', stdout="= -1")
    check("string find empty needle", 'find("hello", "")\n', stdout="= 0")
    check("string find utf8", 'find("你好世界", "世界")\n', stdout="= 2")
    check("string substr", 'substr("hello", 1, 3)\n', stdout="= ell")
    check("string substr to end", 'substr("hello", 2)\n', stdout="= llo")
    check("string substr past end", 'substr("hello", 10)\n', stdout="= ")
    check("string substr clamps length", 'substr("hello", 1, 100)\n',
          stdout="= ello")
    check("string substr utf8", 'substr("你好世界", 1, 2)\n',
          stdout="= 好世")
    check("string substr negative start", 'substr("hello", -1)\n',
          stderr="substr start must be a non-negative integer")
    check("string substr negative length", 'substr("hello", 1, -1)\n',
          stderr="substr length must be a non-negative integer")
    check("string split type", 'split(1, ",")\n',
          stderr="split expects string text and separator")
    check("string split arity", 'split("a")\n',
          stderr="split expects 2 arguments")
    check("string find arity", 'find("a")\n',
          stderr="find expects 2 arguments")
    check("string substr arity", 'substr("a")\n',
          stderr="substr expects 2 or 3 arguments")

    check("list expression", "[1+2, 3*4]\n", stdout="= [3, 12]")
    check("empty list", "[]\n", stdout="= []")
    check("list index", "[1,2][1]\n", stdout="= 2")
    check("nested list read", "let a = [[1,2],[3,4]];\na[0][1]\n",
          stdout="= 2")
    check(
        "list read value semantics",
        "let a = [[1]];\nlet b = a[0];\nb[0] = 9;\na\nb\n",
        stdout_all=["= [[1]]", "= [9]"],
    )
    check(
        "dict read value semantics",
        'let d = {"x": [1,2]};\nlet e = d["x"];\ne[0] = 9;\nd\n',
        stdout="{x: [1, 2]}",
    )
    check(
        "list alias copy on write",
        "let a = [1, 2];\nlet b = a;\nb[0] = 9;\na\nb\n",
        stdout_all=["= [1, 2]", "= [9, 2]"],
    )
    check(
        "dict alias copy on write",
        'let d = {"x": 1};\nlet e = d;\ne["x"] = 9;\nd\ne\n',
        stdout_all=["{x: 1}", "{x: 9}"],
    )
    check(
        "dict alias insert copy on write",
        'let d = {"a": 1};\nlet e = d;\ne["b"] = 2;\nd\ne\n',
        stdout_all=["{a: 1}", "{a: 1, b: 2}"],
    )
    check(
        "nested list alias copy on write",
        "let a = [[1], [2]];\nlet b = a;\nb[0][0] = 9;\na\nb\n",
        stdout_all=["= [[1], [2]]", "= [[9], [2]]"],
    )
    check(
        "function argument copy on write",
        "func mutate(x) { x[0] = 9; return x; }\n"
        "let a = [1, 2];\n"
        "mutate(a)\n"
        "a\n",
        stdout_all=["= [9, 2]", "= [1, 2]"],
    )
    check(
        "closure capture copy on write",
        "func make() {\n"
        "  let a = [1];\n"
        "  func get() { return a; }\n"
        "  return [a, get];\n"
        "}\n"
        "let pair = make();\n"
        "let x = pair[0];\n"
        "x[0] = 9;\n"
        "pair[0]\n"
        "pair[1]()\n",
        stdout_all=["= [1]", "= [1]"],
    )
    check(
        "index side effect falls back to copy",
        "let a = [10,20,30];\n"
        "func f() { a[0] = 99; return 0; }\n"
        "a[f()]\n",
        stdout="= 10",
    )
    check("list equality", "[1,2] == [1,2]\n", stdout="= 1")
    check("dict index", '{1:2, 3:4}[3]\n', stdout="= 4")
    check("dict field read", 'let p = {"x": 1, "y": 2};\np.x + p.y\n',
          stdout="= 3")
    check("dict field assignment", 'let p = {"x": 1};\np.x = 5;\np\n',
          stdout="{x: 5}")
    check(
        "nested dict field assignment",
        'let p = {"a": {"b": 2}};\np.a.b = 3;\np\n',
        stdout="{a: {b: 3}}",
    )
    check("dict field callable", 'let d = {"f": sqrt};\nd.f(9)\n',
          stdout="= 3.0")
    check("dict field missing", 'let p = {"x": 1};\np.y\n',
          stderr="key not found")
    check("float literals unaffected by dot", ".5 + 1\n", stdout="= 1.5")
    check("dict equality", "{1:2} == {1:2}\n", stdout="= 1")
    check("dict literal duplicate key last wins", '{1: "a", 1: "b"}\n',
          stdout="{1: b}")
    check("dict literal numeric duplicate key",
          '{1: "a", 1.0: "b"} == {1: "b"}\n', stdout="= 1")
    check("print list", "print([1,2,3])\n", stdout="[1, 2, 3]")
    check("multiline list", "let a = [1,\n 2,\n 3];\na[2]\n", stdout="= 3")
    check("empty dict builtin", "dict()\n", stdout="= {}")
    check("empty list builtin", "list()\n", stdout="= []")

    check("list index assignment", "let a = [1,2,3];\na[0] = 9;\na\n",
          stdout="= [9, 2, 3]")
    check("dict insert by assignment", "let d = {1:2};\nd[2] = 5;\nd\n",
          stdout="= {1: 2, 2: 5}")
    check(
        "nested index assignment",
        "let a = [[1,2],[3,4]];\na[0][1] = 99;\na\n",
        stdout="= [[1, 99], [3, 4]]",
    )
    check(
        "nested dict assignment",
        'let d = {"a": dict()};\nd["a"]["b"] = 42;\nd\n',
        stdout="{a: {b: 42}}",
    )
    check("index assignment out of range", "let a = [1];\na[3] = 9;\n",
          stderr="out of range")
    check("index assignment to string", 'let s = "abc";\ns[0] = "z";\n',
          stderr="cannot index-assign into string")
    check("assignment to literal", "1 = 2\n", stderr="cannot assign to this expression")

    check(
        "for loop",
        "for (let i = 0; i < 3; i = i + 1) { print(i); }\n",
        stdout="0\n1\n2\n",
    )
    check(
        "for loop sum",
        "let s = 0;\n"
        "for (let i = 1; i <= 4; i = i + 1) { s = s + i; }\n"
        "s\n",
        stdout="= 10",
    )
    check(
        "for loop break",
        "for (let i = 0; i < 5; i = i + 1) { "
        "if (i == 2) { break; } print(i); }\n",
        stdout="0\n1\n",
    )
    check(
        "for loop continue",
        "for (let i = 0; i < 4; i = i + 1) { "
        "if (i == 1) { continue; } print(i); }\n",
        stdout="0\n2\n3\n",
    )
    check(
        "return inside for",
        "func f(a) {\n"
        "  for (let i = 0; i < 10; i = i + 1) { return a; }\n"
        "}\n"
        "f(7)\n",
        stdout="= 7",
    )

    check(
        "break",
        "let i = 0;\n"
        "while (i < 10) { i = i + 1; if (i == 3) { break; } print(i); }\n",
        stdout="1\n2\n",
    )
    check(
        "continue",
        "let i = 0;\n"
        "while (i < 5) { i = i + 1; if (i == 2) { continue; } print(i); }\n",
        stdout="1\n3\n",
    )
    check(
        "block scope",
        "let x = 1;\n{ let x = 2; }\nx\n",
        stdout="= 1",
    )
    check(
        "block assignment",
        "let x = 1;\n{ x = 2; }\nx\n",
        stdout="= 2",
    )

    many_globals = "".join(f"let v{i} = {i};\n" for i in range(40))
    check("large symbol table lookup", many_globals + "v0 + v39\n",
          stdout="= 39")
    check("large symbol table duplicate", many_globals + "let v0 = 1;\n",
          stderr="symbol repeated")
    check(
        "large symbol table closure",
        many_globals
        + "func make() { let n = 5; func get() { return n + v39; } return get; }\n"
        + "let g = make();\ng()\n",
        stdout="= 44",
    )
    check(
        "large symbol table shadowing",
        many_globals + "let x = 1;\nfunc f() { let x = 2; return x; }\nf()\nx\n",
        stdout_all=["= 2", "= 1"],
    )

    check("call non-function", "let x = 1;\nx(2)\n", stderr="cannot call int")
    check("recursion depth limit",
          "func f(n) { return f(n + 1); }\nf(0)\n",
          stderr="maximum recursion depth")
    check("function without return yields null",
          "func noret() { }\nprint(noret())\n", stdout="null")
    check("null arithmetic", "func noret() { }\nnoret() + 1\n",
          stderr="cannot add null and int")
    check("null truthiness",
          "func noret() { }\nif (noret()) { print(1); } else { print(2); }\n",
          stdout="2")
    check("null as argument",
          "func noret() { }\nfunc add(a,b) { return a+b; }\nadd(noret(), 2)\n",
          stderr="cannot add null and int")
    check("null equality", "func noret() { }\nnoret() == noret()\n",
          stdout="= 1")
    check("null literal", "print(null)\n", stdout="null")
    check("null literal equality", "null == null\n", stdout="= 1")
    check("null literal inequality", "null == 0\n", stdout="= 0")
    check("null literal falsy",
          "if (null) { print(1); } else { print(2); }\n", stdout="2")
    check("null literal arithmetic", "null + 1\n",
          stderr="cannot add null and int")
    check("null is not callable", "null()\n", stderr="cannot call null")
    check("str null", "str(null)\n", stdout="= null")
    check("format null", 'format("{}", null)\n', stdout="= null")
    check("explicit empty return", "func f() { return; }\nprint(f())\n",
          stdout="null")
    check("print returns null", "let x = print(1);\nprint(x)\n",
          stdout_all=["1", "null"])
    check("arity error after argument evaluation",
          "func one(a) { return a; }\none(print(1), 2)\n",
          stdout="1", stderr="expects 1 argument")
    check("non-callable evaluates arguments", "1(print(7))\n",
          stdout="7", stderr="cannot call int")

    check("builtin as value", "let f = sqrt;\nf(9)\n", stdout="= 3.0")
    check("builtin as callback",
          "func apply(f, x) { return f(x); }\napply(sqrt, 16)\n",
          stdout="= 4.0")
    check("builtin in list", "let fs = [sqrt, abs];\nfs[0](25)\n",
          stdout="= 5.0")
    check("builtin equality", "sqrt == sqrt\nsqrt == abs\n",
          stdout_all=["= 1", "= 0"])
    check("print as value", 'let p = print;\np("ok")\n', stdout="ok")
    check("format as value", 'let f = format;\nf("{} + {}", 1, 2)\n',
          stdout="= 1 + 2")
    check("builtin string form", "str(sqrt)\n", stdout="= <builtin>")
    check("command output", 'command("echo hi")\n', stdout="= hi")
    check("command in format", 'format("{}", command("echo hi"))\n',
          stdout="= hi")
    check("command as value", 'let run = command;\nrun("echo ok")\n',
          stdout="= ok")
    check("command type error", "command(1)\n",
          stderr="command expects a string")
    check("command arity error", "command()\n",
          stderr="expects 1 or 2 arguments")
    command_env_test = (
        'command("echo %FOO%", {"FOO": "bar"})\n'
        if sys.platform == "win32"
        else 'command("echo $FOO", {"FOO": "bar"})\n'
    )
    check("command environment override", command_env_test, stdout="= bar")
    command_status_env_test = (
        'command_status("exit %CODE%", {"CODE": "3"})\n'
        if sys.platform == "win32"
        else 'command_status("exit $CODE", {"CODE": "3"})\n'
    )
    check("command_status environment override", command_status_env_test,
          stdout="= 3")
    check("command empty environment", "command(\"echo hi\", dict())\n",
          stdout="= hi")
    check("command env type", "command(\"echo hi\", 1)\n",
          stderr="command env must be a dict")
    check("command env key type", 'command("echo hi", {1: "x"})\n',
          stderr="command env keys must be strings")
    check("command env value type", 'command("echo hi", {"A": 1})\n',
          stderr="command env values must be strings")
    check("command env invalid name", 'command("echo hi", {"A=B": "x"})\n',
          stderr="invalid environment variable name")
    check("command env arity error", 'command("echo hi", dict(), 1)\n',
          stderr="expects 1 or 2 arguments")
    check("command_status env type", "command_status(\"exit 0\", 1)\n",
          stderr="command_status env must be a dict")
    check("command_status success", 'command_status("exit 0")\n',
          stdout="= 0")
    check("command_status failure", 'command_status("exit 3")\n',
          stdout="= 3")
    check("command_status output", 'command_status("echo hi")\n',
          stdout_all=["hi", "= 0"])
    check("command_status as value",
          'let run = command_status;\nrun("exit 0")\n', stdout="= 0")
    check("command_status type error", "command_status(1)\n",
          stderr="command_status expects a string")
    check("command_status arity error", "command_status()\n",
          stderr="expects 1 or 2 arguments")
    check("input reads a line", "input()\nhello\n", stdout="= hello")
    check("input with prompt", 'input("Name: ")\nAlice\n',
          stdout_all=["Name: ", "= Alice"])
    check("input with print", 'print(input("> "))\nworld\n',
          stdout_all=["> ", "world"])
    check("input preserves spaces", "input()\n  hi  \n", stdout="=   hi  ")
    check("input CRLF", "input()\r\nhello\r\n", stdout="= hello")
    check("input backspace", "input()\nhello\x7f\x7f\n", stdout="= hel")
    check("input EOF is null", "input() == null\n", stdout="= 1")
    check("input EOF print", "print(input())\n", stdout="null")
    check("input prompt type", "input(1)\n",
          stderr="input prompt must be a string")
    check("input arity", 'input("a", "b")\n',
          stderr="input expects 0 or 1 arguments")
    check("input as value", 'let f = input;\nf("> ")\nvalue\n',
          stdout_all=["> ", "= value"])
    check("read_file type error", "read_file(1)\n",
          stderr="read_file expects a string path")
    check("write_file type error", 'write_file("x", 1)\n',
          stderr="write_file expects a string path and data")
    check("write_file arity error", 'write_file("x")\n',
          stderr="write_file expects 2 arguments")
    check("append_file arity error", 'append_file("x")\n',
          stderr="append_file expects 2 arguments")
    check("file_exists type error", "file_exists(1)\n",
          stderr="file_exists expects a string path")
    check("bytes_from_string display", 'bytes_from_string("abc")\n',
          stdout="= <bytes: 3>")
    check("bytes_len", 'bytes_len(bytes_from_string("abc"))\n', stdout="= 3")
    check("bytes_get", 'bytes_get(bytes_from_string("abc"), 1)\n',
          stdout="= 98")
    check("bytes_get out of range",
          'bytes_get(bytes_from_string("a"), 5)\n',
          stderr="bytes_get index 5 out of range")
    check("bytes_get type", 'bytes_get("x", 0)\n',
          stderr="bytes_get expects bytes")
    check("bytes_slice", 'bytes_hex(bytes_slice('
          'bytes_from_string("abcdef"), 1, 3))\n', stdout="= 626364")
    check("bytes_slice past end",
          'bytes_len(bytes_slice(bytes_from_string("abc"), 10))\n',
          stdout="= 0")
    check("bytes_slice clamps length",
          'bytes_hex(bytes_slice(bytes_from_string("abc"), 1, 100))\n',
          stdout="= 6263")
    check("bytes_concat", 'bytes_hex(bytes_concat('
          'bytes_from_string("ab"), bytes_from_string("cd")))\n',
          stdout="= 61626364")
    check("bytes_hex", 'bytes_hex(bytes_from_hex("00ff10"))\n',
          stdout="= 00ff10")
    check("bytes_from_hex odd", 'bytes_from_hex("0")\n',
          stderr="even number of hex digits")
    check("bytes_from_hex invalid", 'bytes_from_hex("zz")\n',
          stderr="invalid hex digit")
    check("bytes_from_hex type", "bytes_from_hex(1)\n",
          stderr="bytes_from_hex expects a string")
    check("bytes_to_string", 'bytes_to_string(bytes_from_hex("68656c6c6f"))\n',
          stdout="= hello")
    check("bytes_to_string NUL", 'bytes_to_string(bytes_from_hex("00"))\n',
          stderr="bytes contain NUL byte")
    check("bytes_to_string invalid utf8",
          'bytes_to_string(bytes_from_hex("ff"))\n',
          stderr="not valid UTF-8")
    check("bytes_to_string overlong utf8",
          'bytes_to_string(bytes_from_hex("c080"))\n',
          stderr="not valid UTF-8")
    check("bytes_to_string surrogate utf8",
          'bytes_to_string(bytes_from_hex("eda080"))\n',
          stderr="not valid UTF-8")
    check("bytes_to_string utf8 roundtrip",
          'bytes_to_string(bytes_from_string("你好"))\n',
          stdout="= 你好")
    check("bytes_to_string emoji roundtrip",
          'bytes_to_string(bytes_from_string("😀"))\n',
          stdout="= 😀")
    check("bytes_to_int little",
          'bytes_to_int(bytes_from_hex("78563412"), 4, "little")\n',
          stdout="= 305419896")
    check("bytes_to_int big",
          'bytes_to_int(bytes_from_hex("78563412"), 4, "big")\n',
          stdout="= 2018915346")
    check("bytes_to_int signed 16",
          'bytes_to_int(bytes_from_hex("ffff"), 2, "little", 0, 1)\n',
          stdout="= -1")
    check("bytes_to_int unsigned 16",
          'bytes_to_int(bytes_from_hex("ffff"), 2, "little", 0, 0)\n',
          stdout="= 65535")
    check("bytes_to_int signed 8",
          'bytes_to_int(bytes_from_hex("80"), 1, "big", 0, 1)\n',
          stdout="= -128")
    check("bytes_to_int offset",
          'bytes_to_int(bytes_from_hex("00ff80"), 2, "big", 1)\n',
          stdout="= 65408")
    check("bytes_to_int signed 64",
          'bytes_to_int(bytes_from_hex("ffffffffffffffff"), 8, "little", 0, 1)\n',
          stdout="= -1")
    check("bytes_to_int unsigned 64 overflow",
          'bytes_to_int(bytes_from_hex("ffffffffffffffff"), 8, "little")\n',
          stderr="unsigned 64-bit value is too large")
    check("bytes_to_int truncated",
          'bytes_to_int(bytes_from_hex("0102"), 4, "little")\n',
          stderr="need 4 bytes at offset 0, got 2")
    check("bytes_to_int size range",
          'bytes_to_int(bytes_from_hex("01"), 0, "little")\n',
          stderr="size must be between 1 and 8")
    check("bytes_to_int byteorder",
          'bytes_to_int(bytes_from_hex("01"), 1, "middle")\n',
          stderr='byteorder must be "little" or "big"')
    check("bytes_to_int negative offset",
          'bytes_to_int(bytes_from_hex("01"), 1, "little", -1)\n',
          stderr="offset must be a non-negative integer")
    check("bytes_to_int type", 'bytes_to_int("x", 1, "little")\n',
          stderr="bytes_to_int expects bytes")
    check("bytes_to_int arity",
          'bytes_to_int(bytes_from_hex("01"))\n',
          stderr="bytes_to_int expects 3 to 5 arguments")
    check("int_to_bytes little",
          'bytes_hex(int_to_bytes(305419896, 4, "little"))\n',
          stdout="= 78563412")
    check("int_to_bytes big",
          'bytes_hex(int_to_bytes(305419896, 4, "big"))\n',
          stdout="= 12345678")
    check("int_to_bytes negative",
          'bytes_hex(int_to_bytes(-1, 2, "little"))\n', stdout="= ffff")
    check("int_to_bytes negative big",
          'bytes_hex(int_to_bytes(-2, 4, "big"))\n', stdout="= fffffffe")
    check("int_to_bytes unsigned 32",
          'bytes_hex(int_to_bytes(4294967295, 4, "little"))\n',
          stdout="= ffffffff")
    check("int_to_bytes range high",
          'int_to_bytes(256, 1, "little")\n',
          stderr="does not fit in 1 bytes")
    check("int_to_bytes range low",
          'int_to_bytes(-129, 1, "little")\n',
          stderr="does not fit in 1 bytes")
    check("int_to_bytes size range",
          'int_to_bytes(1, 9, "little")\n',
          stderr="size must be between 1 and 8")
    check("int_to_bytes byteorder",
          'int_to_bytes(1, 4, "middle")\n',
          stderr='byteorder must be "little" or "big"')
    check("int_to_bytes type", 'int_to_bytes("x", 4, "little")\n',
          stderr="int_to_bytes expects an integer value")
    check("int_to_bytes arity", 'int_to_bytes(1, 4)\n',
          stderr="int_to_bytes expects 3 arguments")
    check("bytes/int roundtrip",
          'bytes_to_int(int_to_bytes(-123456789, 8, "big"), '
          '8, "big", 0, 1)\n',
          stdout="= -123456789")
    check("bytes index read", 'bytes_from_string("hello")[1]\n',
          stdout="= 101")
    check("bytes index out of range", 'bytes_from_string("a")[5]\n',
          stderr="bytes index 5 out of range")
    check("bytes index assignment",
          'let b = bytes_from_string("abc");\n'
          'b[1] = 255;\n'
          'bytes_hex(b)\n',
          stdout="= 61ff63")
    check("bytes index assignment range",
          'let b = bytes_from_string("abc");\n'
          'b[0] = 256;\n',
          stderr="bytes value 256 out of range")
    check("bytes equality",
          'bytes_from_string("ab") == bytes_from_hex("6162")\n',
          stdout="= 1")
    check("bytes arity", "bytes_len()\n",
          stderr="built-in function expects 1 argument")
    check("bytes_slice arity", "bytes_slice(bytes_from_string(\"a\"))\n",
          stderr="bytes_slice expects 2 or 3 arguments")
    check("read_bytes type", "read_bytes(1)\n",
          stderr="read_bytes expects a string path")
    check("write_bytes type", 'write_bytes("x", "text")\n',
          stderr="write_bytes expects a string path and bytes")
    check("bytes hex roundtrip type", "bytes_hex(1)\n",
          stderr="bytes_hex expects bytes")

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "io.txt")
        quoted = path.replace("\\", "\\\\").replace('"', '\\"')
        check("write and read file",
              f'write_file("{quoted}", "hello");\nread_file("{quoted}")\n',
              stdout="= hello")
        check("append file",
              f'append_file("{quoted}", " world");\nread_file("{quoted}")\n',
              stdout="= hello world")
        check("write file truncates",
              f'write_file("{quoted}", "reset");\nread_file("{quoted}")\n',
              stdout="= reset")
        check("file exists", f'file_exists("{quoted}")\n', stdout="= 1")
        check("file does not exist",
              f'file_exists("{quoted}.missing")\n', stdout="= 0")
        check("read missing file", f'read_file("{quoted}.missing")\n',
              stderr="read_file:")
        missing_dir = os.path.join(tmp, "missing", "file.txt")
        quoted_missing = missing_dir.replace("\\", "\\\\")
        check("write missing directory",
              f'write_file("{quoted_missing}", "x")\n',
              stderr="write_file:")

        empty = os.path.join(tmp, "empty.txt")
        quoted_empty = empty.replace("\\", "\\\\")
        check("empty file",
              f'write_file("{quoted_empty}", "");\nread_file("{quoted_empty}")\n',
              stdout="= ")

        nul_file = os.path.join(tmp, "nul.bin")
        with open(nul_file, "wb") as f:
            f.write(b"a\0b")
        quoted_nul = nul_file.replace("\\", "\\\\")
        check("read file with NUL", f'read_file("{quoted_nul}")\n',
              stderr="contains NUL byte")
        check("read_bytes keeps NUL",
              f'bytes_hex(read_bytes("{quoted_nul}"))\n',
              stdout="= 610062")
        binary_file = os.path.join(tmp, "binary.bin")
        quoted_binary = binary_file.replace("\\", "\\\\")
        check("write and read bytes",
              f'write_bytes("{quoted_binary}", bytes_from_hex("00ff10"));\n'
              f'bytes_hex(read_bytes("{quoted_binary}"))\n',
              stdout="= 00ff10")
        check("append bytes",
              f'append_bytes("{quoted_binary}", bytes_from_hex("20"));\n'
              f'bytes_hex(read_bytes("{quoted_binary}"))\n',
              stdout="= 00ff1020")
        check("read missing bytes",
              f'read_bytes("{quoted_binary}.missing")\n',
              stderr="read_bytes:")
        check(
            "file handle binary seek/write",
            f'let h = file_open("{quoted_binary}", "w+b");\n'
            f'file_write_bytes(h, bytes_from_hex("01020304"));\n'
            f'file_seek(h, 1);\n'
            f'file_write_bytes(h, bytes_from_hex("ff"));\n'
            f'file_close(h);\n'
            f'bytes_hex(read_bytes("{quoted_binary}"))\n',
            stdout="= 01ff0304",
        )
        check(
            "file handle read bytes",
            f'let h = file_open("{quoted_binary}", "rb");\n'
            f'bytes_hex(file_read_bytes(h, 2));\n'
            f'file_tell(h);\n'
            f'bytes_hex(file_read_bytes(h));\n'
            f'file_close(h);\n',
            stdout_all=["= 01ff", "= 2", "= 0304"],
        )
        check(
            "file handle read bytes keeps NUL",
            f'let h = file_open("{quoted_nul}", "rb");\n'
            f'bytes_hex(file_read_bytes(h))\n',
            stdout="= 610062",
        )
        check("file_read_bytes arity", "file_read_bytes()\n",
              stderr="file_read_bytes expects 1 or 2 arguments")
        check("file_read_bytes type", "file_read_bytes(1)\n",
              stderr="file_read_bytes expects a file")
        check("file_write_bytes type",
              f'let h = file_open("{quoted_binary}", "wb");\n'
              f'file_write_bytes(h, "x")\n',
              stderr="file_write_bytes expects bytes data")

        handle_path = os.path.join(tmp, "handle.txt")
        quoted_handle = handle_path.replace("\\", "\\\\")
        check(
            "file handle write/seek/read/tell",
            f'let h = file_open("{quoted_handle}", "w+");\n'
            f'file_write(h, "hello world");\n'
            f'file_seek(h, 0);\n'
            f'file_read(h, 5);\n'
            f'file_tell(h);\n'
            f'file_close(h);\n',
            stdout_all=["= hello", "= 5"],
        )
        check(
            "file handle read all and eof",
            f'let h = file_open("{quoted_handle}", "r");\n'
            f'file_read(h);\n'
            f'file_eof(h);\n'
            f'file_close(h);\n',
            stdout_all=["= hello world", "= 1"],
        )
        check(
            "file handle seek from end",
            f'let h = file_open("{quoted_handle}", "r");\n'
            f'file_seek(h, -1, 2);\n'
            f'file_read(h, 1)\n',
            stdout="= d",
        )
        check(
            "file handle NUL",
            f'let h = file_open("{quoted_nul}", "r");\n'
            f'file_read(h)\n',
            stderr="contains NUL byte",
        )
        check(
            "file handle close and copy",
            f'let h = file_open("{quoted_handle}", "r");\n'
            f'let g = h;\n'
            f'file_close(h);\n'
            f'file_read(g, 1)\n',
            stderr="file is closed",
        )
        check(
            "file handle factory",
            f'func open_file(path, mode) {{\n'
            f'  let h = file_open(path, mode);\n'
            f'  func read(n) {{ return file_read(h, n); }}\n'
            f'  func write(s) {{ return file_write(h, s); }}\n'
            f'  func seek(n) {{ return file_seek(h, n); }}\n'
            f'  func close() {{ return file_close(h); }}\n'
            f'  return {{"read": read, "write": write, '
            f'"seek": seek, "close": close}};\n'
            f'}}\n'
            f'let f = open_file("{quoted_handle}", "w+");\n'
            f'f.write("abcdef");\n'
            f'f.seek(0);\n'
            f'f.read(3);\n'
            f'f.close();\n',
            stdout="= abc",
        )
        check("file_open invalid mode", 'file_open("x", "bad")\n',
              stderr="invalid file mode")
        check("file_open arity", 'file_open("x")\n',
              stderr="file_open expects 2 arguments")
        check("file_read arity", "file_read()\n",
              stderr="file_read expects 1 or 2 arguments")
        check("file_write arity", 'file_write("x")\n',
              stderr="file_write expects 2 arguments")
        check("file_seek arity", "file_seek(1)\n",
              stderr="file_seek expects 2 or 3 arguments")
        check("file_read type", "file_read(1)\n",
              stderr="file_read expects a file")
        check("file_read size type",
              f'let h = file_open("{quoted_handle}", "r");\n'
              f'file_read(h, "x")\n',
              stderr="file_read size must be a non-negative integer")
        check("file_seek whence type",
              f'let h = file_open("{quoted_handle}", "r");\n'
              f'file_seek(h, 0, "x")\n',
              stderr="file_seek whence must be an integer")

    proc = run("func bad() { 1/0; }\nbad() + 1\n")
    assert "division by zero" in proc.stderr
    assert "cannot add null" not in proc.stderr

    proc = run(
        "func f(n) { if (n == 0) { return 1/0; } return f(n - 1); }\n"
        "f(3)\n"
    )
    assert "division by zero" in proc.stderr
    assert "maximum recursion depth" not in proc.stderr

    proc = run('1/0; print("after");\n')
    assert "division by zero" in proc.stderr
    assert "after" not in proc.stdout


    check("int conversion from float", "int(2.9)\n", stdout="= 2")
    check("int conversion truncates toward zero", "int(-2.9)\n", stdout="= -2")
    check("int conversion from string", 'int("  -42  ")\n', stdout="= -42")
    check("float conversion from int", "float(3)\n", stdout="= 3.0")
    check("float conversion from string", 'float("1.5")\n', stdout="= 1.5")
    check("str conversion of float", "str(1.0)\n", stdout="= 1.0")
    check("str conversion of list", "str([1, 2])\n", stdout="= [1, 2]")
    check("int conversion rejects float string", 'int("4.2")\n',
          stderr="int() cannot convert")
    check("int conversion out of range", "int(9223372036854775808.0)\n",
          stderr="int() value out of range")
    check("int conversion from int64 string",
          'int("9223372036854775807")\n', stdout="= 9223372036854775807")

    check("format basic", 'format("x = {}", 3)\n', stdout="= x = 3")
    check("format multiple", 'format("{} + {} = {}", 1, 2, 3)\n',
          stdout="= 1 + 2 = 3")
    check("format positional", 'format("{1} {0}", "a", "b")\n',
          stdout="= b a")
    check("format escaped braces", 'format("{{}} = {}", 1)\n',
          stdout="= {} = 1")
    check("format float precision", 'format("{:.2f}", 1.5)\n',
          stdout="= 1.50")
    check("format integer type", 'format("{:d}", 42)\n', stdout="= 42")
    check("format hex", 'format("{:x} {:X}", 255, 255)\n', stdout="= ff FF")
    check("format width", 'format("[{:>6}]", "hi")\n', stdout="= [    hi]")
    check("format center", 'format("[{:*^8}]", "hi")\n', stdout="= [***hi***]")
    check("format zero padding", 'format("{:08}", -42)\n', stdout="= -0000042")
    check("format utf8 width", 'format("[{:4}]", "你")\n', stdout="= [你   ]")
    check("format utf8 precision", 'format("{:.2s}", "你好ab")\n',
          stdout="= 你好")
    check("format extra args ignored", 'format("{}", 1, 2)\n', stdout="= 1")
    check("format too few args", 'format("{} {}", 1)\n',
          stderr="out of range")
    check("format mixed numbering", 'format("{} {0}", 1)\n',
          stderr="cannot mix")
    check("format unmatched brace", 'format("{")\n', stderr="unmatched")
    check("format unknown type", 'format("{:q}", 1)\n',
          stderr="unknown format type")
    check("format type mismatch", 'format("{:d}", "x")\n',
          stderr="requires an integer")
    check("format precision requires type", 'format("{:.2}", 1.5)\n',
          stderr="precision requires")
    check("print formatting", 'print("x = {}", 3)\n', stdout="x = 3")
    check("print raw single argument", 'print("{{}}")\n', stdout="{{}}")
    check("print multi-arg formatting", 'print("{} + {} = {}", 1, 2, 3)\n',
          stdout="1 + 2 = 3")
    check("print extra args ignored", 'print("x", 1)\n', stdout="x")
    check("print non-string format", 'print(1, 2)\n',
          stderr="first argument must be a string")
    check("builtin arity", "sqrt(4,9)\n", stderr="expects 1 argument")
    check("return outside", "return 5;\n", stderr="return outside function")
    check("break outside", "break;\n", stderr="break outside loop")
    check("mystery character", "1 @ 2\n",
          stderr="lexical error: unexpected character '@' (0x40)")

    # Unterminated strings must never hang or crash.
    check("unterminated string newline", 'print("unterminated)\n',
          stderr="unterminated string literal")
    proc = run('print("unterminated)', timeout=5)
    assert "unterminated string literal" in proc.stderr
    assert "Segmentation fault" not in proc.stderr

    check("empty input", "", code=0)
    check("empty block", "{}\n", code=0)
    check("utf8 bom", "\ufeff1+2\n", stdout="= 3")
    check("crlf input", "1+2\r\n", stdout="= 3")

    with tempfile.TemporaryDirectory() as tmp:
        script = os.path.join(tmp, "script.calc")
        with open(script, "w", encoding="utf-8") as f:
            f.write(
                "#!/usr/bin/env calc-demo\n"
                'print("script");\n'
                "1 + 2\n"
            )
        proc = run_file(script)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "script\n", repr(proc.stdout)
        assert proc.stderr == "", proc.stderr

        bom_script = os.path.join(tmp, "bom.calc")
        with open(bom_script, "w", encoding="utf-8") as f:
            f.write("\ufeff#!/usr/bin/env calc-demo\nprint(\"bom\");\n")
        proc = run_file(bom_script)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "bom\n", repr(proc.stdout)

        input_script = os.path.join(tmp, "input.calc")
        with open(input_script, "w", encoding="utf-8") as f:
            f.write('print(input("? "));\n')
        proc = subprocess.run(
            [BIN, input_script],
            input="hello\n",
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "? hello\n", repr(proc.stdout)

        if sys.platform != "win32":
            executable = os.path.abspath(BIN)
            if " " not in executable:
                shebang_script = os.path.join(tmp, "shebang.calc")
                with open(shebang_script, "w", encoding="utf-8") as f:
                    f.write(f"#!{executable}\nprint(\"shebang\");\n")
                os.chmod(shebang_script, 0o755)
                proc = subprocess.run(
                    [shebang_script],
                    text=True,
                    encoding="utf-8",
                    capture_output=True,
                    timeout=5,
                )
                assert proc.returncode == 0, proc.stderr
                assert proc.stdout == "shebang\n", repr(proc.stdout)

    proc = subprocess.run(
        [BIN, "-"],
        input='print("stdin-script");\n',
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=5,
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout == "stdin-script\n", repr(proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        runtime_script = os.path.join(tmp, "runtime.calc")
        with open(runtime_script, "w", encoding="utf-8") as f:
            f.write('print("a");\n1/0;\n')
        proc = run_file(runtime_script)
        assert proc.returncode == 1, proc.stderr
        assert "2:1: runtime error: division by zero" in proc.stderr, proc.stderr
        assert "  1/0;\n  ^" in proc.stderr, proc.stderr

        nested_script = os.path.join(tmp, "nested.calc")
        with open(nested_script, "w", encoding="utf-8") as f:
            f.write(
                "func g() {\n"
                "  return 10 / 0;\n"
                "}\n"
                "func f() {\n"
                "  return g();\n"
                "}\n"
                "f();\n"
            )
        proc = run_file(nested_script)
        assert proc.returncode == 1, proc.stderr
        assert "2:10: runtime error: division by zero" in proc.stderr, proc.stderr
        assert "in g() called from line 5" in proc.stderr, proc.stderr
        assert "in f() called from line 7" in proc.stderr, proc.stderr

        clean_script = os.path.join(tmp, "clean.calc")
        with open(clean_script, "w", encoding="utf-8") as f:
            f.write("print(\"ok\");\n")
        proc = run_file(clean_script)
        assert proc.returncode == 0, proc.stderr

        syntax_script = os.path.join(tmp, "syntax.calc")
        with open(syntax_script, "w", encoding="utf-8") as f:
            f.write("let x = ;\n")
        proc = run_file(syntax_script)
        assert proc.returncode == 1, proc.stderr
        assert "1:9: syntax error: unexpected ';'" in proc.stderr, proc.stderr

    check("syntax error detail", "let x = ;\n",
          stderr="syntax error: unexpected ';', expecting")
    check("syntax error snippet", "let x = ;\n",
          stderr="  let x = ;\n          ^")
    check("lexical error position", "1 @ 2\n",
          stderr="1:3: lexical error: unexpected character '@' (0x40)")
    check("unterminated string position", 'print("unterminated)\n',
          stderr="1:7: lexical error: unterminated string literal")
    check("definition error location", "func f(a, a) {\n  return a;\n}\n",
          stderr="1:1: runtime error: duplicate parameter")
    check("break error location", "{\n  break;\n}\n",
          stderr="2:3: runtime error: break outside loop")

    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write(
            "app/aa/aa.calc",
            'package aa { export let root = "aa"; }\n',
        )
        write(
            "app/aa/bb/bb.calc",
            'package bb { export let name = "bb"; }\n',
        )
        write(
            "app/aa/bb/cc/cc.calc",
            "package cc {\n"
            "  export let value = 42;\n"
            "  export func hello(name) { return \"hi \" + name; }\n"
            "  func hidden() { return 99; }\n"
            "}\n",
        )
        app_main = write(
            "app/main.calc",
            "import aa.bb.cc;\n"
            "print(cc.value);\n"
            "print(cc.hello(\"x\"));\n"
            "import aa.bb;\n"
            "print(bb.name);\n"
            "print(bb.cc.value);\n"
            "import aa;\n"
            "print(aa.root);\n"
            "print(aa.bb.cc.value);\n",
        )
        proc = run_file(app_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["42", "hi x", "bb", "42", "aa", "42"], (
            proc.stdout
        )
        assert proc.stderr == "", proc.stderr

        write(
            "app/aa/state/state.calc",
            "package state {\n"
            "  let n = 0;\n"
            "  export func inc() { n = n + 1; return n; }\n"
            "  export func get() { return n; }\n"
            "}\n",
        )
        state_main = write(
            "app/state_main.calc",
            "import aa.state;\n"
            "func again() {\n"
            "  import aa.state;\n"
            "  return state.inc();\n"
            "}\n"
            + "1;\n" * 70
            + "print(state.inc());\n"
            "print(again());\n"
            "print(state.get());\n",
        )
        proc = run_file(state_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["1", "2", "2"], proc.stdout

        write(
            "container/aa/bb/cc/cc.calc",
            'package cc { export let value = "deep"; }\n',
        )
        container_main = write(
            "container/main.calc",
            "import aa;\nprint(aa.bb.cc.value);\n",
        )
        proc = run_file(container_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "deep\n", repr(proc.stdout)

        # Package lookup paths: source dir > CLI dirs > environment dirs.
        write(
            "search/app/aa/bb/cc/cc.calc",
            'package cc { export let value = "source"; }\n',
        )
        search_main = write(
            "search/app/main.calc",
            "import aa.bb.cc;\nprint(cc.value);\n",
        )
        write(
            "search/lib1/aa/bb/cc/cc.calc",
            'package cc { export let value = "cli1"; }\n',
        )
        write(
            "search/lib2/aa/bb/cc/cc.calc",
            'package cc { export let value = "cli2"; }\n',
        )
        write(
            "search/env/aa/bb/cc/cc.calc",
            'package cc { export let value = "env"; }\n',
        )
        proc = run_file(search_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "source\n", repr(proc.stdout)

        app_without_local = write(
            "search/app2/main.calc",
            "import aa.bb.cc;\nprint(cc.value);\n",
        )
        lib1 = os.path.join(tmp, "search/lib1")
        lib2 = os.path.join(tmp, "search/lib2")
        envlib = os.path.join(tmp, "search/env")
        proc = subprocess.run(
            [BIN, "-I", lib1, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "cli1\n", repr(proc.stdout)

        proc = subprocess.run(
            [BIN, "-I", lib1, "-I", lib2, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "cli1\n", repr(proc.stdout)

        proc = subprocess.run(
            [BIN, "-I", lib2, "-I", lib1, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "cli2\n", repr(proc.stdout)

        env = os.environ.copy()
        env["CALC_PACKAGE_PATH"] = envlib
        proc = subprocess.run(
            [BIN, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
            env=env,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "env\n", repr(proc.stdout)

        # CLI dirs take priority over environment dirs.
        proc = subprocess.run(
            [BIN, "-I", lib1, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
            env=env,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "cli1\n", repr(proc.stdout)

        proc = subprocess.run(
            [BIN, "-I", lib1],
            input="import aa.bb.cc;\nprint(cc.value);\n",
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert "cli1\n" in proc.stdout, repr(proc.stdout)

        # Packages are isolated from the main package's globals.
        write(
            "isolated/aa/bb/cc/cc.calc",
            "package cc { export let leaked = main_secret; }\n",
        )
        isolated_main = write(
            "isolated/main.calc",
            "let main_secret = 1;\nimport aa.bb.cc;\n",
        )
        proc = run_file(isolated_main)
        assert proc.returncode == 1, proc.stderr
        assert "symbol not found main_secret" in proc.stderr, proc.stderr

        # Package names must match their containing folder.
        write(
            "wrongname/aa/bb/cc/cc.calc",
            "package wrong { export let value = 1; }\n",
        )
        wrong_main = write(
            "wrongname/main.calc",
            "import aa.bb.cc;\n",
        )
        proc = run_file(wrong_main)
        assert proc.returncode == 1, proc.stderr
        assert "must match folder" in proc.stderr, proc.stderr

        # Explicit main package and exported/private names.
        main_pkg = write(
            "mainpkg/main/main.calc",
            "package main {\n"
            "  export let x = 5;\n"
            "  func private() { return 9; }\n"
            "}\n"
            "print(main.x);\n",
        )
        proc = run_file(main_pkg)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "5\n", repr(proc.stdout)

        # Cyclic imports fail cleanly instead of hanging.
        write(
            "cycle/aa/bb/bb.calc",
            "package bb { import aa.cc; export let x = 1; }\n",
        )
        write(
            "cycle/aa/cc/cc.calc",
            "package cc { import aa.bb; export let y = 2; }\n",
        )
        cycle_main = write(
            "cycle/main.calc",
            "import aa.bb;\n",
        )
        proc = run_file(cycle_main)
        assert proc.returncode == 1, proc.stderr
        assert "cyclic package import" in proc.stderr, proc.stderr

        missing_main = write(
            "missing/main.calc",
            "import no.such;\n",
        )
        proc = run_file(missing_main)
        assert proc.returncode == 1, proc.stderr
        assert "package not found" in proc.stderr, proc.stderr

    with tempfile.TemporaryDirectory() as tmp:
        failfast = os.path.join(tmp, "failfast.calc")
        with open(failfast, "w", encoding="utf-8") as f:
            f.write('print("before");\n1/0;\nprint("after");\n')
        proc = run_file(failfast)
        assert proc.returncode == 1, proc.stderr
        assert "before\n" in proc.stdout, proc.stdout
        assert "after\n" not in proc.stdout, proc.stdout

        proc = subprocess.run(
            [BIN, "-k", failfast],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 1, proc.stderr
        assert "before\n" in proc.stdout, proc.stdout
        assert "after\n" in proc.stdout, proc.stdout

        check(
            "assignment failure preserves old value",
            "let x = 1;\n"
            "func f() { return 1/0; }\n"
            "x = f();\n"
            "x\n",
            stdout="= 1",
        )

        lexical_script = os.path.join(tmp, "lexical.calc")
        with open(lexical_script, "w", encoding="utf-8") as f:
            f.write('1 @ 2;\nprint("after");\n')
        proc = run_file(lexical_script)
        assert proc.returncode == 1, proc.stderr
        assert "lexical error" in proc.stderr, proc.stderr
        assert "syntax error" not in proc.stderr, proc.stderr
        assert "after\n" not in proc.stdout, proc.stdout

        proc = subprocess.run(
            [BIN, "--bad-option"],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 2, proc.stderr

        proc = subprocess.run(
            [BIN, os.path.join(tmp, "missing.calc")],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 2, proc.stderr

        if sys.platform != "win32":
            interrupt_script = os.path.join(tmp, "interrupt.calc")
            with open(interrupt_script, "w", encoding="utf-8") as f:
                f.write("while (1) { }\n")
            proc = subprocess.Popen(
                [BIN, interrupt_script],
                text=True,
                encoding="utf-8",
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            time.sleep(0.2)
            proc.send_signal(signal.SIGINT)
            stdout, stderr = proc.communicate(timeout=5)
            assert proc.returncode == 130, (proc.returncode, stdout, stderr)

    print(f"all tests passed for {BIN}")


if __name__ == "__main__":
    main()
