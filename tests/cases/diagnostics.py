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
        stderr="1:41: runtime error: uncaught fail: division by zero",
    )
    check(
        "diagnostic defer failure location",
        'func boom() { fail "defer boom"; }\n'
        "func f() { defer boom(); return 1; }\n"
        "f();\n",
        stderr="3:5: runtime error: uncaught fail: defer boom",
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
        "diagnostic invalid label location",
        "label x: if (1) { print(1); }\n",
        stderr="1:14: runtime error: label must be attached to a loop or switch",
    )
