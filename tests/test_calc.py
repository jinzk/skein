#!/usr/bin/env python3
"""Golden/smoke tests for the calc-demo interpreter."""

import subprocess
import sys


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
    check("integer overflow", "2147483647 + 1\n",
          stderr="integer overflow")
    check("out-of-range integer literal becomes float", "2147483648\n",
          stdout="= 2147483648.0")
    check("INT_MIN division overflow", "(-2147483647 - 1) / -1\n",
          stderr="integer overflow")

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
    check("bitwise vs equality precedence", "1 & 3 == 1\n", stdout="= 0")
    check("unary minus", "-3+5\n", stdout="= 2")
    check("unary minus alone", "-3\n", stdout="= -3")
    check("abs negative", "abs(-3)\n", stdout="= 3")
    check("abs overflow", "abs(-2147483647 - 1)\n", stderr="integer overflow")

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
    check("list equality", "[1,2] == [1,2]\n", stdout="= 1")
    check("dict index", '{1:2, 3:4}[3]\n', stdout="= 4")
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

    check("call non-function", "let x = 1;\nx(2)\n", stderr="cannot call int")
    check("recursion depth limit",
          "def f(n) { return f(n + 1); }\nf(0)\n",
          stderr="maximum recursion depth")
    check("int conversion from float", "int(2.9)\n", stdout="= 2")
    check("int conversion truncates toward zero", "int(-2.9)\n", stdout="= -2")
    check("int conversion from string", 'int("  -42  ")\n', stdout="= -42")
    check("float conversion from int", "float(3)\n", stdout="= 3.0")
    check("float conversion from string", 'float("1.5")\n', stdout="= 1.5")
    check("str conversion of float", "str(1.0)\n", stdout="= 1.0")
    check("str conversion of list", "str([1, 2])\n", stdout="= [1, 2]")
    check("int conversion rejects float string", 'int("4.2")\n',
          stderr="int() cannot convert")
    check("int conversion out of range", "int(2147483648.0)\n",
          stderr="int() value out of range")
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

    print(f"all tests passed for {BIN}")


if __name__ == "__main__":
    main()
