#!/usr/bin/env python3
"""Golden/smoke tests for the calc-demo interpreter."""

import os
import subprocess
import sys
import tempfile


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

    check("zero-arg function", "def f() {\n  return 42;\n}\nf()\n", stdout="= 42")
    check(
        "return inside while",
        "def f(a) {\n  while (1) { return a; }\n}\nf(7)\n",
        stdout="= 7",
    )
    check(
        "recursion",
        "def fib(n) {\n"
        "  if (n < 2) { return n; }\n"
        "  return fib(n-1) + fib(n-2);\n"
        "}\n"
        "fib(10)\n",
        stdout="= 55",
    )
    check(
        "function value alias",
        "def f(a) { return a; }\nlet g = f;\nlet h = g;\nh(8)\n",
        stdout="= 8",
    )
    check(
        "duplicate parameter",
        "def f(a,a) { return a; }\n",
        stderr="duplicate parameter",
    )
    check(
        "closure counter",
        "def make_counter() {\n"
        "  let n = 0;\n"
        "  def inc() { n = n + 1; return n; }\n"
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
        "def make_pair() {\n"
        "  let n = 0;\n"
        "  def inc() { n = n + 1; return n; }\n"
        "  def get() { return n; }\n"
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
        "def adder(x) {\n"
        "  def add(y) { return x + y; }\n"
        "  return add;\n"
        "}\n"
        "let a = adder(10);\n"
        "a(5)\n"
        "adder(3)(4)\n",
        stdout_all=["= 15", "= 7"],
    )
    check(
        "closure recursion",
        "def make_fact() {\n"
        "  def fact(n) {\n"
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
        "  def f() { return x; }\n"
        "  c = f;\n"
        "}\n"
        "c()\n",
        stdout="= 5",
    )
    check(
        "function value in list",
        "def adder(x) { def add(y) { return x + y; } return add; }\n"
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
        "index side effect falls back to copy",
        "let a = [10,20,30];\n"
        "def f() { a[0] = 99; return 0; }\n"
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
        "def f(a) {\n"
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
        + "def make() { let n = 5; def get() { return n + v39; } return get; }\n"
        + "let g = make();\ng()\n",
        stdout="= 44",
    )
    check(
        "large symbol table shadowing",
        many_globals + "let x = 1;\ndef f() { let x = 2; return x; }\nf()\nx\n",
        stdout_all=["= 2", "= 1"],
    )

    check("call non-function", "let x = 1;\nx(2)\n", stderr="cannot call int")
    check("recursion depth limit",
          "def f(n) { return f(n + 1); }\nf(0)\n",
          stderr="maximum recursion depth")
    check("function without return yields null",
          "def noret() { }\nprint(noret())\n", stdout="null")
    check("null arithmetic", "def noret() { }\nnoret() + 1\n",
          stderr="cannot add null and int")
    check("null truthiness",
          "def noret() { }\nif (noret()) { print(1); } else { print(2); }\n",
          stdout="2")
    check("null as argument",
          "def noret() { }\ndef add(a,b) { return a+b; }\nadd(noret(), 2)\n",
          stderr="cannot add null and int")
    check("null equality", "def noret() { }\nnoret() == noret()\n",
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
    check("explicit empty return", "def f() { return; }\nprint(f())\n",
          stdout="null")
    check("print returns null", "let x = print(1);\nprint(x)\n",
          stdout_all=["1", "null"])
    check("arity error after argument evaluation",
          "def one(a) { return a; }\none(print(1), 2)\n",
          stdout="1", stderr="expects 1 argument")
    check("non-callable evaluates arguments", "1(print(7))\n",
          stdout="7", stderr="cannot call int")

    check("builtin as value", "let f = sqrt;\nf(9)\n", stdout="= 3.0")
    check("builtin as callback",
          "def apply(f, x) { return f(x); }\napply(sqrt, 16)\n",
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

    proc = run("def bad() { 1/0; }\nbad() + 1\n")
    assert "division by zero" in proc.stderr
    assert "cannot add null" not in proc.stderr

    proc = run(
        "def f(n) { if (n == 0) { return 1/0; } return f(n - 1); }\n"
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
    check("mystery character", "1 @ 2\n", stderr="mystery character")

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

    print(f"all tests passed for {BIN}")


if __name__ == "__main__":
    main()
