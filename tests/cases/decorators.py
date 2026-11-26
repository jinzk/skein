#!/usr/bin/env python3
"""Method decorators: @expr on impl methods.

Pure decorator behavior cases live in tests/skein/decorators_behavior.skein.
Generator, strict-typecheck, and temp-package cases stay here.
"""
import os
import tempfile

from harness import *  # noqa: F401,F403


TRACE = (
    "func trace(fn) {\n"
    "  return func(...args) {\n"
    '    println("enter");\n'
    "    let r = fn(...args);\n"
    '    println("exit");\n'
    "    return r;\n"
    "  };\n"
    "}\n"
)


def run_case(source, expected):
    proc = run(source)
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.splitlines() == expected, proc.stdout


def run_cases():
    run_skein("decorators_behavior.skein")
    check(
        "decorator golden output",
        "func trace(fn) {\n"
        "  return func(...args) {\n"
        '    println("enter");\n'
        "    let r = fn(...args);\n"
        '    println("exit");\n'
        "    return r;\n"
        "  };\n"
        "}\n"
        "struct G { x: int; }\n"
        "impl G { @trace func get(self) { return self.x; } }\n"
        "println(G { x: 3 }.get());\n",
        stdout_all=["enter", "exit", "3"],
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

    run_case(
        TRACE
        + "struct P { n: int; }\n"
        + "impl P {\n"
        + "  @trace\n"
        + "  func nums(self) {\n"
        + "    for (let i in range(self.n)) { yield i; }\n"
        + "  }\n"
        + "}\n"
        + "let p = P { n: 3 };\n"
        + "for (let x in p.nums()) { println(x); }\n"
        + "let f = p.nums;\n"
        + 'for (let x in f()) { println(str(x) + "!"); }\n',
        ["enter", "exit", "0", "1", "2",
         "enter", "exit", "0!", "1!", "2!"],
    )

    run_case(
        "import std;\n"
        "func limit(n: int) {\n"
        "  return func(fn) {\n"
        "    return func(...args) {\n"
        "      let count = 0;\n"
        "      for (let value in fn(...args)) {\n"
        "        if (count >= n) { return; }\n"
        "        yield value;\n"
        "        count = count + 1;\n"
        "      }\n"
        "    };\n"
        "  };\n"
        "}\n"
        "struct P { n: int; }\n"
        "impl P {\n"
        "  @limit(2)\n"
        "  func nums(self) {\n"
        "    for (let i in range(self.n)) { yield i; }\n"
        "  }\n"
        "}\n"
        "let g: generator[int] = P { n: 5 }.nums();\n"
        "println(std.iter.collect(g));\n",
        ["[0, 1]"],
    )

    run_case(
        "import std;\n"
        "func echo(fn) { return fn; }\n"
        "struct P { n: int; }\n"
        "impl P {\n"
        "  @echo\n"
        "  func dialog(self) {\n"
        '    let x = yield "start";\n'
        '    yield x + "!";\n'
        "  }\n"
        "}\n"
        "let g = P { n: 0 }.dialog();\n"
        "println(std.generator.next(g));\n"
        'println(std.generator.next(g, "hi"));\n'
        "println(std.generator.next(g));\n",
        ["{done: false, value: start}",
         "{done: false, value: hi!}",
         "{done: true, value: null}"],
    )

    run_case(
        "import std;\n"
        "func identity(fn) { return fn; }\n"
        "struct P { n: int; }\n"
        "impl P {\n"
        "  @identity\n"
        "  func worker(self) {\n"
        '    defer println("cleanup");\n'
        "    yield 1;\n"
        '    println("not reached");\n'
        "  }\n"
        "}\n"
        "let g = P { n: 0 }.worker();\n"
        "println(std.generator.next(g));\n"
        "println(std.generator.close(g));\n",
        ["{done: false, value: 1}", "cleanup", "true"],
    )

    run_case(
        "import std;\n"
        "func trace(fn) {\n"
        "  return func(...args) {\n"
        '    println("enter");\n'
        "    let r = fn(...args);\n"
        '    println("exit");\n'
        "    return r;\n"
        "  };\n"
        "}\n"
        "struct P { n: int; }\n"
        "impl P {\n"
        "  @trace\n"
        "  func nums(self) {\n"
        "    std.task.sleep(1).wait();\n"
        "    yield self.n;\n"
        "    yield self.n + 1;\n"
        "  }\n"
        "}\n"
        "let xs: list[int] = std.iter.collect(P { n: 5 }.nums());\n"
        "println(xs);\n",
        ["enter", "exit", "[5, 6]"],
    )

    run_case(
        "import std;\n"
        "func identity(fn) { return fn; }\n"
        "proto Source { func items(self); }\n"
        "struct P { n: int; }\n"
        "impl P {\n"
        "  @identity\n"
        "  func items(self) { yield 1; yield self.n; }\n"
        "}\n"
        "func render(source: Source) {\n"
        "  println(std.iter.collect(source.items()));\n"
        "}\n"
        "render(P { n: 7 });\n",
        ["[1, 7]"],
    )

    check(
        "decorator generator method error cleanup",
        "func bad() { return 1; }\n"
        "struct P { n: int; }\n"
        "impl P {\n"
        "  @bad(1)\n"
        "  func nums(self) { yield 1; }\n"
        "}\n",
        stderr="function expects 0 argument(s), got 1",
        env=dict(os.environ, SKEIN_TYPECHECK="strict"),
    )

    check(
        "decorator generator method type",
        "import std;\n"
        "func trace(fn) { return func(...args) { return fn(...args); }; }\n"
        "struct P { n: int; }\n"
        "impl P {\n"
        "  @trace\n"
        "  func nums(self) { yield self.n; yield self.n + 1; }\n"
        "}\n"
        "let g: generator[int] = P { n: 5 }.nums();\n"
        "println(std.iter.collect(g));\n",
        stdout_all=["[5, 6]"],
        not_stderr="type warning",
        env=dict(os.environ, SKEIN_TYPECHECK="strict"),
    )

    check(
        "decorator generator method send type",
        "import std;\n"
        "func echo(fn) { return fn; }\n"
        "struct P { n: int; }\n"
        "impl P {\n"
        "  @echo\n"
        "  func dialog(self): generator[string, string] {\n"
        '    let x = yield "a";\n'
        "    yield x;\n"
        "  }\n"
        "}\n"
        "let g: generator[string, string] = P { n: 0 }.dialog();\n"
        "std.generator.next(g, 1);\n",
        stderr="expected string, got int",
        env=dict(os.environ, SKEIN_TYPECHECK="strict"),
    )

    run_case(
        "import std;\n"
        "func trace(fn) {\n"
        "  return func(...args) {\n"
        '    println("enter");\n'
        "    return fn(...args);\n"
        "  };\n"
        "}\n"
        "struct P { n: int; }\n"
        "impl P {\n"
        "  @trace\n"
        "  func down(self) {\n"
        "    yield self.n;\n"
        "    if (self.n > 0) {\n"
        "      let next = P { n: self.n - 1 };\n"
        "      for (let v in next.down()) { yield v; }\n"
        "    }\n"
        "  }\n"
        "}\n"
        "println(std.iter.collect(P { n: 2 }.down()));\n",
        ["enter", "enter", "enter", "[2, 1, 0]"],
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


    run_case(
        "func trace(fn) {\n"
        "  return func(...args) {\n"
        '    println("create");\n'
        "    return fn(...args);\n"
        "  };\n"
        "}\n"
        "@trace\n"
        "func nums() { yield 1; yield 2; }\n"
        "for (let x in nums()) { println(x); }\n",
        ["create", "1", "2"],
    )


    check(
        "decorator non-callable result on method",
        "func bad(fn) { return 42; }\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  @bad\n"
        "  func get(self) { return self.x; }\n"
        "}\n",
        stderr="expects callable, got int",
    )

    check(
        "task-returning decorator",
        "import std;\n"
        "func adec(fn) { return std.task.sleep(1); }\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  @adec\n"
        "  func get(self) { return self.x; }\n"
        "}\n",
        stderr="expects callable, got task",
    )

    check(
        "decorator non-callable function result",
        "func bad(fn) { return 42; }\n"
        "@bad\n"
        "func f() { return 1; }\n",
        stderr="expects callable, got int",
    )


    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write(
            "aa.skein",
            "package aa {\n"
            "  export struct P { x: int; }\n"
            "  export func make(x) { return P { x: x }; }\n"
            "  export func trace(fn) {\n"
            "    return func(...args) {\n"
            '      println("[aa trace]");\n'
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
            "println(p.get());\n",
        )
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["[aa trace]", "7"], proc.stdout

    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write(
            "aa.skein",
            "package aa {\n"
            "  func logged(fn) {\n"
            "    return func(...args) {\n"
            '      println("[aa fn]");\n'
            "      return fn(...args);\n"
            "    };\n"
            "  }\n"
            "  @logged\n"
            "  export func add(a, b) { return a + b; }\n"
            "}\n",
        )
        main = write(
            "main.skein",
            "import aa;\n"
            "println(aa.add(2, 3));\n",
        )
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["[aa fn]", "5"], proc.stdout

    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write(
            "aa.skein",
            "package aa {\n"
            "  export struct P { n: int; }\n"
            "  export func make(n) { return P { n: n }; }\n"
            "  export func trace(fn) {\n"
            "    return func(...args) {\n"
            '      println("[aa gen]");\n'
            "      return fn(...args);\n"
            "    };\n"
            "  }\n"
            "  impl P {\n"
            "    @trace\n"
            "    func nums(self) { yield self.n; yield self.n + 1; }\n"
            "  }\n"
            "}\n",
        )
        main = write(
            "main.skein",
            "import aa;\n"
            "for (let x in aa.make(7).nums()) { println(x); }\n",
        )
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["[aa gen]", "7", "8"], proc.stdout
