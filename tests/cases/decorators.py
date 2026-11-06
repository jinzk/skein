#!/usr/bin/env python3
"""Method decorators: @expr on impl methods."""
import os
import tempfile

from harness import *  # noqa: F401,F403


TRACE = (
    "func trace(fn) {\n"
    "  return func(...args) {\n"
    '    print("enter");\n'
    "    let r = fn(...args);\n"
    '    print("exit");\n'
    "    return r;\n"
    "  };\n"
    "}\n"
)


def run_case(source, expected):
    proc = run(source)
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.splitlines() == expected, proc.stdout


def run_cases():
    run_case(
        TRACE
        + "struct P { x: int; }\n"
        + "impl P {\n"
        + "  @trace\n"
        + "  func get(self, n: int = 1) { return self.x * n; }\n"
        + "}\n"
        + "let p = P { x: 3 };\n"
        + "print(p.get());\n"
        + "print(p.get(2));\n"
        + "let f = p.get;\n"
        + "print(f(4));\n",
        ["enter", "exit", "3", "enter", "exit", "6",
         "enter", "exit", "12"],
    )

    run_case(
        "func around(name) {\n"
        "  return func(fn) {\n"
        "    return func(...args) {\n"
        '      print("enter " + name);\n'
        "      let r = fn(...args);\n"
        '      print("exit " + name);\n'
        "      return r;\n"
        "    };\n"
        "  };\n"
        "}\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        '  @around("outer")\n'
        '  @around("inner")\n'
        "  func get(self) { print(\"body\"); return self.x; }\n"
        "}\n"
        "print(P { x: 1 }.get());\n",
        ["enter outer", "enter inner", "body", "exit inner",
         "exit outer", "1"],
    )

    run_case(
        "func show_args(fn) {\n"
        "  return func(...args) {\n"
        "    print(str(args[1]));\n"
        "    print(str(args[2]));\n"
        "    return fn(...args);\n"
        "  };\n"
        "}\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        "  @show_args\n"
        "  func add(self, a: int = self.x, b: int = a + 1) {\n"
        "    return a + b;\n"
        "  }\n"
        "}\n"
        "print(P { x: 3 }.add());\n"
        "print(P { x: 3 }.add(10));\n",
        ["3", "4", "7", "10", "11", "21"],
    )

    run_case(
        "func count_args(fn) {\n"
        "  return func(...args) {\n"
        "    print(str(len(args)));\n"
        "    return fn(...args);\n"
        "  };\n"
        "}\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        "  @count_args\n"
        "  func total(self, ...nums: int) {\n"
        "    let sum = self.x;\n"
        "    for (let n in nums) { sum = sum + n; }\n"
        "    return sum;\n"
        "  }\n"
        "}\n"
        "print(P { x: 1 }.total());\n"
        "print(P { x: 1 }.total(2, 3, 4));\n",
        ["1", "1", "4", "10"],
    )

    run_case(
        "func bump(fn) {\n"
        "  return func(...args) {\n"
        "    args[0] = P { x: 99 };\n"
        "    args[1] = args[1] + 1;\n"
        "    return fn(...args);\n"
        "  };\n"
        "}\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        "  @bump\n"
        "  func get(self, n: int) { return self.x + n; }\n"
        "}\n"
        "print(P { x: 1 }.get(2));\n",
        ["102"],
    )

    run_case(
        TRACE
        + "struct P { x: int; }\n"
        + "impl P {\n"
        + "  @trace\n"
        + "  func fact(self, n: int) {\n"
        + "    if (n <= 1) { return 1; }\n"
        + "    return n * self.fact(n - 1);\n"
        + "  }\n"
        + "}\n"
        + "print(P { x: 1 }.fact(3));\n",
        ["enter", "enter", "enter", "exit", "exit", "exit", "6"],
    )

    run_case(
        "let calls = 0;\n"
        "func counted(fn) {\n"
        "  calls = calls + 1;\n"
        "  return func(...args) { return fn(...args); };\n"
        "}\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        "  @counted\n"
        "  func get(self) { return self.x; }\n"
        "}\n"
        "print(calls);\n"
        "print(P { x: 1 }.get());\n"
        "print(P { x: 2 }.get());\n"
        "print(calls);\n",
        ["1", "1", "2", "1"],
    )

    run_case(
        "struct P { x: int; }\n"
        "impl P {\n"
        '  @(func(fn) { return func(...args) { print("anon"); return fn(...args); }; })\n'
        "  func get(self) { return self.x; }\n"
        "}\n"
        "print(P { x: 9 }.get());\n",
        ["anon", "9"],
    )

    run_case(
        "func make_dec(prefix) {\n"
        "  return func(fn) {\n"
        "    return func(...args) {\n"
        "      print(prefix);\n"
        "      return fn(...args);\n"
        "    };\n"
        "  };\n"
        "}\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        '  @make_dec("hello")\n'
        "  func get(self) { return self.x; }\n"
        "}\n"
        "print(P { x: 2 }.get());\n",
        ["hello", "2"],
    )

    run_case(
        TRACE
        + "func make() {\n"
        + "  struct P { x: int; }\n"
        + "  impl P {\n"
        + "    @trace\n"
        + "    func get(self) { return self.x; }\n"
        + "  }\n"
        + "  return P { x: 4 };\n"
        + "}\n"
        + "print(make().get());\n",
        ["enter", "exit", "4"],
    )

    check(
        "decorator non-callable",
        "func bad(fn) { return 42; }\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        "  @bad\n"
        "  func m(self) { return 1; }\n"
        "}\n",
        stderr="expects callable, got int",
    )

    check(
        "decorator destructuring parameter",
        TRACE
        + "struct P { x: int; }\n"
        "impl P {\n"
        "  @trace\n"
        "  func m(self, [a, b]) { return a + b; }\n"
        "}\n",
        stderr="cannot use destructuring patterns",
    )

    check(
        "decorator generator method",
        TRACE
        + "struct P { x: int; }\n"
        "impl P {\n"
        "  @trace\n"
        "  func m(self) { yield 1; }\n"
        "}\n",
        stderr="cannot be a generator",
    )

    check(
        "decorator requires self",
        TRACE
        + "struct P { x: int; }\n"
        "impl P {\n"
        "  @trace\n"
        "  func m(x) { return x; }\n"
        "}\n",
        stderr="must start with self",
    )

    check(
        "decorator self cannot have default",
        TRACE
        + "struct P { x: int; }\n"
        "impl P {\n"
        "  @trace\n"
        "  func m(self = P { x: 1 }) { return self.x; }\n"
        "}\n",
        stderr="cannot have a default self parameter",
    )

    check(
        "decorator missing symbol",
        "struct P { x: int; }\n"
        "impl P {\n"
        "  @not_defined\n"
        "  func m(self) { return self.x; }\n"
        "}\n",
        stderr="symbol not found not_defined",
    )

    check(
        "decorator top-level function unsupported",
        "@trace\nfunc f(x) { return x; }\n",
        stderr="unexpected '@'",
    )

    check(
        "decorator trait method",
        TRACE
        + "trait D { func draw(self); }\n"
        "struct P { x: int; }\n"
        "impl D for P {\n"
        "  @trace\n"
        "  func draw(self) { return \"P\" + str(self.x); }\n"
        "}\n"
        "func render(x: D) { print(x.draw()); }\n"
        "render(P { x: 7 });\n",
        stdout_all=["enter", "exit", "P7"],
    )

    check(
        "decorator trait adapter",
        TRACE
        + "trait D { func draw(self); }\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        "  @trace\n"
        "  func draw(self) { return \"P\" + str(self.x); }\n"
        "}\n"
        "impl D for P;\n"
        "func render(x: D) { print(x.draw()); }\n"
        "render(P { x: 8 });\n",
        stdout_all=["enter", "exit", "P8"],
    )

    run_case(
        TRACE
        + "enum E { A { n: int; }; B; }\n"
        + "impl E {\n"
        + "  @trace\n"
        + "  func value(self) { return 1; }\n"
        + "}\n"
        + "print(E::A { n: 2 }.value());\n",
        ["enter", "exit", "1"],
    )

    check(
        "decorator meta signature unchanged",
        "import std;\n"
        + TRACE
        + "struct P { x: int; }\n"
        "impl P {\n"
        "  @trace\n"
        "  func area(self, scale: int = 1) { return self.x * scale; }\n"
        "}\n"
        "let m = std.meta.methods(\"P\")[0];\n"
        "print(m[\"name\"]);\n"
        "print(m[\"params\"]);\n"
        "print(m[\"variadic\"]);\n"
        "print(m[\"has_defaults\"]);\n",
        stdout_all=["area", "[int]", "false", "true"],
    )

    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write(
            "aa/aa.skein",
            "package aa {\n"
            "  export struct P { x: int; }\n"
            "  export func make(x) { return P { x: x }; }\n"
            "  export func trace(fn) {\n"
            "    return func(...args) {\n"
            '      print("[aa trace]");\n'
            "      return fn(...args);\n"
            "    };\n"
            "  }\n"
            "  impl P {\n"
            "    @trace\n"
            "    func get(self) { return self.x; }\n"
            "  }\n"
            "}\n",
        )
        main = write(
            "main.skein",
            "import aa;\n"
            "let p = aa.make(7);\n"
            "print(p.get());\n",
        )
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["[aa trace]", "7"], proc.stdout
