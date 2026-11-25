#!/usr/bin/env python3
"""Diagnostic parity: exact line and column for AST and VM backends."""

from harness import *  # noqa: F401,F403


def run_cases():
    check(
        "diagnostic type error location",
        "1 + true;\n",
        stderr="1:1: runtime error: cannot add int and bool",
    )
    check(
        "diagnostic undefined symbol location",
        "missing;\n",
        stderr="1:8: runtime error: symbol not found missing",
    )
    check(
        "diagnostic missing argument location",
        "func f(a) { return a; }\nf();\n",
        stderr="2:2: runtime error: function expects 1 argument(s), got 0",
    )
    check(
        "diagnostic extra argument location",
        "func f(a) { return a; }\nf(1, 2);\n",
        stderr="2:6: type error: function expects 1 argument(s), got 2",
    )
    check(
        "diagnostic match failure location",
        "match (1) { case 2 => 1; }\n",
        stderr="1:8: runtime error: match: no arm matched",
    )
    check(
        "diagnostic uncaught pass location",
        "let v = (1 / 0) on error(e) { pass e; };\n",
        stderr="1:10: runtime error: uncaught fail: division by zero",
    )
    check(
        "diagnostic defer failure location",
        'func boom() { fail "defer boom"; }\n'
        "func f() { defer boom(); return 1; }\n"
        "f();\n",
        stderr="1:31: runtime error: uncaught fail: defer boom",
    )
    check(
        "diagnostic parameter pattern location",
        "func f([a, b]) { return a; }\nf([1]);\n",
        stderr="2:2: runtime error: function parameter does not match its pattern",
    )
    check(
        "diagnostic const assignment location",
        "const x = 1; x = 2;\n",
        stderr="1:16: runtime error: cannot assign to constant x",
    )
    check(
        "diagnostic break location",
        "break;\n",
        stderr="1:1: runtime error: break outside loop",
    )
    check(
        "diagnostic recover location",
        "recover 1;\n",
        stderr="1:9: runtime error: recover outside error handler",
    )
    check(
        "diagnostic complex lvalue location",
        'func f() { return {"x": 1}; } f().x = 2;\n',
        stderr="1:32: runtime error: cannot assign through this expression",
    )
    check(
        "diagnostic label on non-loop",
        "x: if (1) { print(1); }\n",
        stdout="1",
    )
    check(
        "diagnostic fail trace",
        "import std;\n"
        'func inner() { fail "boom"; }\n'
        "func middle() { inner(); }\n"
        "func outer() { middle(); }\n"
        "let value = outer() on error(e) {\n"
        "  println(str(len(std.error.trace(e))));\n"
        '  println(std.error.trace(e)[0]["function"]);\n'
        '  println(str(std.error.location(e)["line"]));\n'
        "  println(std.error.format_trace(e));\n"
        "  recover null;\n"
        "};\n",
        stdout_all=[
            "3", "inner", "2",
            "in inner() called from line 3",
            "in middle() called from line 4",
            "in outer() called from line 5",
        ],
    )
    check(
        "diagnostic runtime error trace",
        "import std;\n"
        'func inner() { std.string.replace("a", "", "b"); }\n'
        "func middle() { inner(); }\n"
        "middle() on error(e) {\n"
        "  println(std.error.kind(e));\n"
        "  println(str(len(std.error.trace(e))));\n"
        '  println(str(std.error.location(e)["line"]));\n'
        "  println(std.error.format_trace(e));\n"
        "  recover null;\n"
        "};\n",
        stdout_all=[
            "runtime", "2", "2",
            "runtime: std.string.replace search must not be empty",
            "in inner() called from line 3",
        ],
    )
    check(
        "diagnostic user error has no trace",
        "import std;\n"
        'let e = std.error.new("custom", "boom");\n'
        "println(str(std.error.location(e)));\n"
        "println(str(len(std.error.trace(e))));\n"
        "println(std.error.format_trace(e));\n",
        stdout_all=["null", "0", "custom: boom"],
    )
    check(
        "diagnostic private field",
        "struct Box { _value: int; }\n"
        "let box = Box { _value: 1 };\n"
        "println(box._value);\n",
        stderr="runtime error: private member _value",
    )
    check(
        "diagnostic private method",
        "struct Box { func _secret(self) { return 1; } }\n"
        "let box = Box {};\n"
        "println(box._secret());\n",
        stderr="runtime error: private member _secret",
    )
