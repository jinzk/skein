#!/usr/bin/env python3
"""Method decorators: @expr on impl methods."""
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
    run_case(
        TRACE
        + "struct P { x: int; }\n"
        + "impl P {\n"
        + "  @trace\n"
        + "  func get(self, n: int = 1) { return self.x * n; }\n"
        + "}\n"
        + "let p = P { x: 3 };\n"
        + "println(p.get());\n"
        + "println(p.get(2));\n"
        + "let f = p.get;\n"
        + "println(f(4));\n",
        ["enter", "exit", "3", "enter", "exit", "6",
         "enter", "exit", "12"],
    )

    run_case(
        "func around(name) {\n"
        "  return func(fn) {\n"
        "    return func(...args) {\n"
        '      println("enter " + name);\n'
        "      let r = fn(...args);\n"
        '      println("exit " + name);\n'
        "      return r;\n"
        "    };\n"
        "  };\n"
        "}\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        '  @around("outer")\n'
        '  @around("inner")\n'
        "  func get(self) { println(\"body\"); return self.x; }\n"
        "}\n"
        "println(P { x: 1 }.get());\n",
        ["enter outer", "enter inner", "body", "exit inner",
         "exit outer", "1"],
    )

    run_case(
        "func show_args(fn) {\n"
        "  return func(...args) {\n"
        "    println(str(args[1]));\n"
        "    println(str(args[2]));\n"
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
        "println(P { x: 3 }.add());\n"
        "println(P { x: 3 }.add(10));\n",
        ["3", "4", "7", "10", "11", "21"],
    )

    run_case(
        "func count_args(fn) {\n"
        "  return func(...args) {\n"
        "    println(str(len(args)));\n"
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
        "println(P { x: 1 }.total());\n"
        "println(P { x: 1 }.total(2, 3, 4));\n",
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
        "println(P { x: 1 }.get(2));\n",
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
        + "println(P { x: 1 }.fact(3));\n",
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
        "println(calls);\n"
        "println(P { x: 1 }.get());\n"
        "println(P { x: 2 }.get());\n"
        "println(calls);\n",
        ["1", "1", "2", "1"],
    )

    run_case(
        "struct P { x: int; }\n"
        "impl P {\n"
        '  @(func(fn) { return func(...args) { println("anon"); return fn(...args); }; })\n'
        "  func get(self) { return self.x; }\n"
        "}\n"
        "println(P { x: 9 }.get());\n",
        ["anon", "9"],
    )

    run_case(
        "func make_dec(prefix) {\n"
        "  return func(fn) {\n"
        "    return func(...args) {\n"
        "      println(prefix);\n"
        "      return fn(...args);\n"
        "    };\n"
        "  };\n"
        "}\n"
        "struct P { x: int; }\n"
        "impl P {\n"
        '  @make_dec("hello")\n'
        "  func get(self) { return self.x; }\n"
        "}\n"
        "println(P { x: 2 }.get());\n",
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
        + "println(make().get());\n",
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
        "trait Source { func items(self); }\n"
        "struct P { n: int; }\n"
        "impl Source for P {\n"
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
        TRACE
        + "@trace\n"
        + "func add(a, b) { return a + b; }\n"
        + "println(add(1, 2));\n",
        ["enter", "exit", "3"],
    )

    run_case(
        "func around(name) {\n"
        "  return func(fn) {\n"
        "    return func(...args) {\n"
        '      println("enter " + name);\n'
        "      let r = fn(...args);\n"
        '      println("exit " + name);\n'
        "      return r;\n"
        "    };\n"
        "  };\n"
        "}\n"
        '@around("outer")\n'
        '@around("inner")\n'
        "func f() { println(\"body\"); return 1; }\n"
        "println(f());\n",
        ["enter outer", "enter inner", "body", "exit inner",
         "exit outer", "1"],
    )

    run_case(
        TRACE
        + "@trace\n"
        + "func fact(n) {\n"
        + "  if (n <= 1) { return 1; }\n"
        + "  return n * fact(n - 1);\n"
        + "}\n"
        + "println(fact(3));\n",
        ["enter", "enter", "enter", "exit", "exit", "exit", "6"],
    )

    run_case(
        "func dec(fn) {\n"
        "  return func(...args) {\n"
        '    println("outer dec");\n'
        "    return fn(...args);\n"
        "  };\n"
        "}\n"
        "func outer() {\n"
        "  @dec\n"
        "  func dec(x) { return x; }\n"
        "  return dec(1);\n"
        "}\n"
        "println(outer());\n",
        ["outer dec", "1"],
    )

    run_case(
        "func trace(fn) {\n"
        "  return func(...args) {\n"
        '    println("before");\n'
        "    let r = fn(...args);\n"
        '    println("after");\n'
        "    return r;\n"
        "  };\n"
        "}\n"
        "@trace\n"
        "func get() { return 7; }\n"
        "println(get());\n",
        ["before", "after", "7"],
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

    run_case(
        "import std;\n"
        "func timed(fn) {\n"
        "  return func(...args) {\n"
        '    println("before");\n'
        "    std.task.sleep(1).wait();\n"
        "    let r = fn(...args);\n"
        '    println("after");\n'
        "    return r;\n"
        "  };\n"
        "}\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  @timed\n"
        "  func get(self) { return self.x; }\n"
        "}\n"
        "println(C { x: 7 }.get());\n",
        ["before", "after", "7"],
    )

    run_case(
        "import std;\n"
        "func timed(fn) {\n"
        "  return func(...args) {\n"
        '    println("before");\n'
        "    let r = fn(...args);\n"
        '    println("after");\n'
        "    return r;\n"
        "  };\n"
        "}\n"
        "trait S { func handle(self); }\n"
        "struct C { x: int; }\n"
        "impl S for C {\n"
        "  @timed\n"
        "  func handle(self) { return self.x; }\n"
        "}\n"
        "func use(s: S) { return s.handle(); }\n"
        "println(use(C { x: 5 }));\n",
        ["before", "after", "5"],
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

    check(
        "decorator trait method",
        TRACE
        + "trait D { func draw(self); }\n"
        "struct P { x: int; }\n"
        "impl D for P {\n"
        "  @trace\n"
        "  func draw(self) { return \"P\" + str(self.x); }\n"
        "}\n"
        "func render(x: D) { println(x.draw()); }\n"
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
        "func render(x: D) { println(x.draw()); }\n"
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
        + "println(E.A { n: 2 }.value());\n",
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
        "println(m[\"name\"]);\n"
        "println(m[\"params\"]);\n"
        "println(m[\"variadic\"]);\n"
        "println(m[\"has_defaults\"]);\n",
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
