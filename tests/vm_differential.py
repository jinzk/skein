#!/usr/bin/env python3
"""Deterministic AST/VM differential smoke test.

Generates small valid programs and runs both backends.  Any difference in
stdout, stderr or exit status is reported with the generated source.
"""

import os
import random
import subprocess
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "./skein"
CASES = int(sys.argv[2]) if len(sys.argv) > 2 else 160
TIMEOUT = float(os.environ.get("SKEIN_TEST_TIMEOUT", "20"))

rng = random.Random(0x5EED)


def literal():
    kind = rng.randrange(8)
    if kind == 0:
        return str(rng.randint(-40, 40))
    if kind == 1:
        return f"{rng.randint(-20, 20)}.{rng.randrange(1, 10)}"
    if kind == 2:
        return rng.choice(["true", "false"])
    if kind == 3:
        return '"' + rng.choice(["a", "b", "xy", "hello"]) + '"'
    if kind == 4:
        return "null"
    if kind == 5:
        return "[" + ", ".join(str(rng.randint(0, 9))
                                for _ in range(rng.randint(0, 4))) + "]"
    if kind == 6:
        return "{" + ", ".join(
            f'"k{i}": {rng.randint(0, 9)}'
            for i in range(rng.randint(0, 3))) + "}"
    return f"'{rng.choice('abc')}'"


def simple_expr():
    kind = rng.randrange(6)
    if kind <= 2:
        return literal()
    if kind == 3:
        return rng.choice(["x", "y", "z"])
    if kind == 4:
        return rng.choice(["x + y", "x - z", "x * 2", "y + 3"])
    return rng.choice(["[x, y]", '{"v": x}', "x < y", "x == y"])


def statement(depth=0):
    kind = rng.randrange(9)
    if kind == 0:
        return f"print({simple_expr()});"
    if kind == 1:
        return f"x = {simple_expr()};"
    if kind == 2:
        return f"y = x + {rng.randint(0, 9)};"
    if kind == 3:
        return f"if (x < {rng.randint(-5, 20)}) {{ print(x); }} else {{ print(y); }}"
    if kind == 4:
        return (
            "let i = 0;\n"
            f"while (i < {rng.randint(0, 4)}) {{ x = x + i; i = i + 1; }}"
        )
    if kind == 5:
        items = ", ".join(str(rng.randint(0, 9))
                          for _ in range(rng.randint(1, 4)))
        return (
            "let total = 0;\n"
            f"for (v in [{items}]) {{ total = total + v; }}\n"
            "print(total);"
        )
    if kind == 6:
        return (
            "let r = match ([x, y]) {\n"
            "  case [a, b] => a + b;\n"
            "  default => 0;\n"
            "};\n"
            "print(r);"
        )
    if kind == 7:
        return (
            f"func add(a, b) {{ return a + b; }}\n"
            f"print(add(x, {rng.randint(0, 9)}));"
        )
    return (
        "func make(n) {\n"
        "  return func(v) { return v + n; };\n"
        "}\n"
        f"let f = make({rng.randint(0, 9)});\n"
        "print(f(x));"
    )


def program():
    lines = ["let x = 1;", "let y = 2;", "let z = 3;"]
    for _ in range(rng.randint(2, 6)):
        lines.append(statement())
    return "\n".join(lines) + "\n"


def run(source, vm):
    env = dict(os.environ)
    env["SKEIN_VM"] = vm
    return subprocess.run(
        [BIN],
        input=source,
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=TIMEOUT,
        env=env,
    )


def main():
    for index in range(CASES):
        source = program()
        ast = run(source, "0")
        vm = run(source, "1")
        if (ast.returncode, ast.stdout, ast.stderr) != (
                vm.returncode, vm.stdout, vm.stderr):
            print(f"differential mismatch at case {index}", file=sys.stderr)
            print("--- source ---", file=sys.stderr)
            print(source, file=sys.stderr)
            print("--- AST ---", file=sys.stderr)
            print(repr((ast.returncode, ast.stdout, ast.stderr)),
                  file=sys.stderr)
            print("--- VM ---", file=sys.stderr)
            print(repr((vm.returncode, vm.stdout, vm.stderr)),
                  file=sys.stderr)
            return 1
    print(f"vm differential: {CASES} cases matched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
