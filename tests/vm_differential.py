#!/usr/bin/env python3
"""Deterministic AST/VM differential test.

Generates small valid programs and runs both backends.  Any difference in
stdout, stderr or exit status is reported with the generated source.

The default run also checks that the VM did not silently fall back to the AST
for generated programs.  ``--fuzz-seconds`` keeps generating cases for a
longer period, and failures are reduced line-by-line before reporting.
"""

import argparse
import os
import random
import subprocess
import sys
import time

TIMEOUT = float(os.environ.get("SKEIN_TEST_TIMEOUT", "20"))


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


def statement():
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


def basic_program():
    lines = ["let x = 1;", "let y = 2;", "let z = 3;"]
    for _ in range(rng.randint(2, 6)):
        lines.append(statement())
    return "\n".join(lines) + "\n"


def struct_program():
    return (
        "struct Point { x: int = 1; y: int = 2; }\n"
        "impl Point { func sum(self) { return self.x + self.y; } }\n"
        "let a = Point {};\n"
        "let b = Point { x: 5 };\n"
        "let c = Point { ...a, y: 9 };\n"
        "let f = b.sum;\n"
        "print(a.sum());\n"
        "print(f());\n"
        "print(c.x + c.y);\n"
    )


def enum_program():
    return (
        "enum E { A { n: int = 0; s: string = \"x\"; }; B; }\n"
        "let a = E::A {};\n"
        "let b = E::A { n: 7 };\n"
        "print(a.n);\n"
        "print(b.n);\n"
        "print(a.s);\n"
        "print(E::B is E);\n"
    )


def empty_program():
    return (
        "print(len([]));\n"
        "print(len({}));\n"
        "print(len(\"\"));\n"
        "print(str([]));\n"
        "print(str({}));\n"
        "print([][0] on error(e) { recover \"caught\"; });\n"
        "print({\"k\": 1}[\"missing\"] on error(e) { recover \"miss\"; });\n"
    )


def overflow_program():
    return (
        "print(9223372036854775807 + 1);\n"
        "print(-9223372036854775807 - 2);\n"
        "print(1.0e308 * 10.0);\n"
        "print(-1.0e308 * 10.0);\n"
    )


def error_program():
    return rng.choice([
        "print(1 / 0);\n",
        "struct S { x: int; }\nlet s = S { x: 1 };\nprint(s.y);\n",
        "print([1, 2][9]);\n",
        "func f() { fail \"boom\"; }\nf();\n",
        "func f(a) { return a; }\nf();\n",
        "print(1 + \"x\");\n",
    ])


def generator_program():
    return (
        "import std;\n"
        "func gen(n) {\n"
        "  let i = 0;\n"
        "  while (i < n) { yield i; i = i + 1; }\n"
        "  return 99;\n"
        "}\n"
        "let g = gen(3);\n"
        "print(std.generator.next(g)[\"value\"]);\n"
        "print(std.generator.next(g)[\"value\"]);\n"
        "print(std.generator.next(g)[\"value\"]);\n"
        "print(std.generator.next(g)[\"value\"]);\n"
        "std.generator.close(g);\n"
    )


def async_program():
    return (
        "import std;\n"
        "async func inc(x) => x + 1;\n"
        "print(await inc(3));\n"
        "print(await inc(await inc(4)));\n"
    )


def closure_scope_program():
    return (
        "func make(n) {\n"
        "  let total = n;\n"
        "  return func(x) {\n"
        "    total = total + x;\n"
        "    return func() { return total; };\n"
        "  };\n"
        "}\n"
        "let f = make(1);\n"
        "let g = f(2);\n"
        "let h = f(3);\n"
        "print(g());\n"
        "print(h());\n"
    )


def defer_error_program():
    return (
        "func run(n) {\n"
        "  defer print(\"done\");\n"
        "  let i = 0;\n"
        "  let total = 0;\n"
        "  while (i < n) {\n"
        "    total = total + ([][0] on error(e) { recover i; });\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return total;\n"
        "}\n"
        "print(run(3));\n"
    )


def pattern_default_program():
    return (
        "func pat([a, b], c = 1, ...rest) {\n"
        "  return [a, b, c, rest];\n"
        "}\n"
        "print(pat([2, 3]));\n"
        "print(pat([4, 5], 6, 7, 8));\n"
    )


def match_binding_program():
    return (
        "let r = match ({\"a\": 1, \"b\": 2, \"c\": 3}) {\n"
        "  case {a, ...rest} => [a, rest];\n"
        "  default => [];\n"
        "};\n"
        "print(r);\n"
    )


FEATURES = [
    basic_program,
    struct_program,
    enum_program,
    empty_program,
    overflow_program,
    error_program,
    generator_program,
    async_program,
    closure_scope_program,
    defer_error_program,
    pattern_default_program,
    match_binding_program,
]


def program():
    # Keep the original shape common, but regularly exercise feature-focused
    # programs that are difficult to reach with the random expression grammar.
    if rng.randrange(3) == 0:
        return rng.choice(FEATURES)()
    return basic_program()


def run(binary, source, vm, debug=False):
    env = dict(os.environ)
    env["SKEIN_VM"] = vm
    if debug:
        env["SKEIN_VM_DEBUG"] = "1"
    return subprocess.run(
        [binary],
        input=source,
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=TIMEOUT,
        env=env,
    )


def outcome(binary, source):
    ast = run(binary, source, "0")
    vm = run(binary, source, "1")
    return (ast.returncode, ast.stdout, ast.stderr), (
        vm.returncode, vm.stdout, vm.stderr)


def differs(binary, source):
    ast, vm = outcome(binary, source)
    return ast != vm


def minimize(binary, source):
    lines = source.splitlines(keepends=True)
    changed = True
    while changed and len(lines) > 1:
        changed = False
        for index in range(len(lines)):
            candidate = "".join(lines[:index] + lines[index + 1:])
            try:
                if candidate.strip() and differs(binary, candidate):
                    lines = candidate.splitlines(keepends=True)
                    changed = True
                    break
            except subprocess.TimeoutExpired:
                continue
    return "".join(lines)


def report_failure(binary, source):
    ast, vm = outcome(binary, source)
    print("differential mismatch", file=sys.stderr)
    print("--- source ---", file=sys.stderr)
    print(source, file=sys.stderr)
    minimized = minimize(binary, source)
    if minimized != source:
        print("--- minimized source ---", file=sys.stderr)
        print(minimized, file=sys.stderr)
    print("--- AST ---", file=sys.stderr)
    print(repr(ast), file=sys.stderr)
    print("--- VM ---", file=sys.stderr)
    print(repr(vm), file=sys.stderr)


def check_fallbacks(binary, source):
    debug = run(binary, source, "1", debug=True)
    if "[vm] fallback" in debug.stderr:
        print("unexpected VM fallback", file=sys.stderr)
        print("--- source ---", file=sys.stderr)
        print(source, file=sys.stderr)
        print("--- VM debug ---", file=sys.stderr)
        print(debug.stderr, file=sys.stderr)
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default="./skein")
    parser.add_argument("cases", nargs="?", type=int, default=160)
    parser.add_argument("--seed", type=int, default=0x5EED)
    parser.add_argument("--fuzz-seconds", type=float, default=0.0)
    parser.add_argument("--skip-fallback-check", action="store_true")
    args = parser.parse_args()

    global rng
    rng = random.Random(args.seed)

    started = time.monotonic()
    index = 0
    while True:
        if args.fuzz_seconds > 0:
            if time.monotonic() - started >= args.fuzz_seconds:
                break
        elif index >= args.cases:
            break

        source = program()
        try:
            if differs(args.binary, source):
                report_failure(args.binary, source)
                return 1
            if not args.skip_fallback_check and not check_fallbacks(
                    args.binary, source):
                return 1
        except subprocess.TimeoutExpired:
            print(f"timeout at case {index}", file=sys.stderr)
            print("--- source ---", file=sys.stderr)
            print(source, file=sys.stderr)
            return 1
        index += 1

    print(f"vm differential: {index} cases matched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
