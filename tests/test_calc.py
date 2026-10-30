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
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise AssertionError(f"interpreter timed out for input {source!r}") from exc


def check(name, source, *, stdout=None, stderr=None, code=0):
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
    if stderr is not None:
        assert stderr in proc.stderr, (
            f"{name}: expected stderr to contain {stderr!r}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )


def main():
    check("addition", "1+2\n", stdout="= 3")
    check("float", "1.5*2\n", stdout="= 3")
    check("integer division", "10/4\n", stdout="= 2")
    check("float division", "10/4.0\n", stdout="= 2.5")
    check("integer overflow falls back to float", "2147483647 + 1\n",
          stdout="= 2.147e+09")
    check("INT_MIN division overflow", "(-2147483647 - 1) / -1\n",
          stdout="= 2.147e+09")
    check("unary minus", "-3+5\n", stdout="= 2")
    check("unary minus alone", "-3\n", stdout="= -3")
    check("abs negative", "abs(-3)\n", stdout="= 3")

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

    check("string concat", '"a" + "b"\n', stdout="= ab")
    check("string compare", '"a" < "b"\n', stdout="= 1")
    check("string equality", '"a" == "a"\n', stdout="= 1")
    check("string escape", 'print("a\\nb")\n', stdout="a\nb")
    check("string index", '"hello"[1]\n', stdout="= e")

    check("list expression", "[1+2, 3*4]\n", stdout="= [3, 12]")
    check("empty list", "[]\n", stdout="= []")
    check("list index", "[1,2][1]\n", stdout="= 2")
    check("list equality", "[1,2] == [1,2]\n", stdout="= 1")
    check("dict index", '{1:2, 3:4}[3]\n', stdout="= 4")
    check("dict equality", "{1:2} == {1:2}\n", stdout="= 1")
    check("print list", "print([1,2,3])\n", stdout="[1, 2, 3]")
    check("multiline list", "let a = [1,\n 2,\n 3];\na[2]\n", stdout="= 3")

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

    check("call non-function", "let x = 1;\nx(2)\n", stderr="not a function")
    check("builtin arity", "sqrt(4,9)\n", stderr="expects 1 argument")
    check("return outside", "return 5;\n", stderr="return outside function")
    check("break outside", "break;\n", stderr="break outside loop")
    check("mystery character", "1 % 2\n", stderr="mystery character")

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
