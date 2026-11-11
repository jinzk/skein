#!/usr/bin/env python3
"""Task methods, trait methods and launch/task metadata."""
import os
import tempfile

from harness import *  # noqa: F401,F403


def run_case(source, expected):
    proc = run(source)
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.splitlines() == expected, proc.stdout


def run_cases():
    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  func get(self) { return self.x; }\n"
        "}\n"
        "let c = C { x: 7 };\n"
        "println(c.get());\n",
        ["7"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  func get(self) {\n"
        '    println("start");\n'
        "    std.task.sleep(1).wait();\n"
        '    println("end");\n'
        "    return self.x;\n"
        "  }\n"
        "}\n"
        "println(C { x: 8 }.get());\n",
        ["start", "end", "8"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  func get(self) { return self.x; }\n"
        "}\n"
        "let c = C { x: 9 };\n"
        "let f = c.get;\n"
        "println(f());\n",
        ["9"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  func add(self, a: int = self.x, b: int = a + 1) {\n"
        "    return a + b;\n"
        "  }\n"
        "}\n"
        "println(C { x: 3 }.add());\n"
        "println(C { x: 3 }.add(10));\n",
        ["7", "21"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  func total(self, ...nums: int) {\n"
        "    let total = self.x;\n"
        "    for (let n in nums) { total = total + n; }\n"
        "    return total;\n"
        "  }\n"
        "}\n"
        "println(C { x: 1 }.total(2, 3, 4));\n",
        ["10"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  func get(self) {\n"
        '    defer println("cleanup");\n'
        "    return self.x;\n"
        "  }\n"
        "}\n"
        "println(C { x: 4 }.get());\n",
        ["cleanup", "4"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  func fact(self, n: int) {\n"
        "    if (n <= 1) { return 1; }\n"
        "    return n * self.fact(n - 1);\n"
        "  }\n"
        "}\n"
        "println(C { x: 0 }.fact(4));\n",
        ["24"],
    )

    run_case(
        "import std;\n"
        "enum E { A { n: int; }; B; }\n"
        "impl E {\n"
        "  func value(self) { return 1; }\n"
        "}\n"
        "println(E.A { n: 2 }.value());\n",
        ["1"],
    )

    run_case(
        "import std;\n"
        "trait S { func handle(self, x: int); }\n"
        "struct C { base: int; }\n"
        "impl S for C {\n"
        "  func handle(self, x: int) { return self.base + x; }\n"
        "}\n"
        "func use(s: S) { return s.handle(2); }\n"
        "println(use(C { base: 5 }));\n",
        ["7"],
    )

    run_case(
        "import std;\n"
        "trait S { func handle(self); }\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  func handle(self) { return self.x; }\n"
        "}\n"
        "impl S for C;\n"
        "func use(s: S) { return s.handle(); }\n"
        "println(use(C { x: 11 }));\n",
        ["11"],
    )

    run_case(
        "import std;\n"
        "trait S {\n"
        "  func handle(self);\n"
        "  func twice(self) { return self.handle() * 2; }\n"
        "}\n"
        "struct C { x: int; }\n"
        "impl S for C {\n"
        "  func handle(self) { return self.x; }\n"
        "}\n"
        "println(C { x: 11 }.twice());\n",
        ["22"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  func get(self) { return self.x; }\n"
        "}\n"
        "let m = std.meta.methods(\"C\")[0];\n"
        "println(m[\"name\"]);\n"
        "println(str(m[\"params\"]));\n",
        ["get", "[]"],
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
            "  export struct C { x: int; }\n"
            "  export func make(x) { return C { x: x }; }\n"
            "  impl C {\n"
            "    func get(self) { return self.x; }\n"
            "  }\n"
            "}\n",
        )
        main = write(
            "main.skein",
            "import aa;\n"
            "let c = aa.make(12);\n"
            "println(c.get());\n",
        )
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["12"], proc.stdout

    env = os.environ.copy()
    env["SKEIN_FIBER_STACK_STATS"] = "1"
    proc = run(
        "import std;\n"
        "func work(n) { return n + 1; }\n"
        "let total = 0;\n"
        "for (let i = 0; i < 40; i = i + 1) {\n"
        "  total = total + launch work(i).wait();\n"
        "}\n"
        "println(total);\n",
        env=env,
    )
    assert proc.returncode == 0, proc.stderr
    assert "820" in proc.stdout, proc.stdout
    stats = [
        line for line in proc.stderr.splitlines()
        if line.startswith("fiber stack pool:")
    ]
    assert stats, proc.stderr
    fields = dict(
        item.split("=", 1)
        for item in stats[-1].split(": ", 1)[1].split())
    assert int(fields["reused"]) > 0, stats[-1]
    assert int(fields["allocated"]) <= 2, stats[-1]

    check(
        "suspended generator survives cycle collection",
        "import std;\n"
        "func make_gen() {\n"
        "  let data = [1, 2, 3];\n"
        "  func peek() { return data; }\n"
        "  return func() {\n"
        "    yield peek();\n"
        "    data = std.list.append(data, 4);\n"
        "    yield peek();\n"
        "  };\n"
        "}\n"
        "let g = make_gen()();\n"
        "let first = std.generator.next(g);\n"
        "println(first[\"value\"]);\n"
        + "if (false) { }\n" * 80
        + "let second = std.generator.next(g);\n"
        "println(second[\"value\"]);\n",
        stdout="[1, 2, 3]\n[1, 2, 3, 4]\n",
    )

    check(
        "suspended task survives cycle collection",
        "import std;\n"
        "func hold(v) {\n"
        "  for (let i = 0; i < 3; i = i + 1) {\n"
        "    std.task.sleep(1).wait();\n"
        "    v = std.list.append(v, i);\n"
        "  }\n"
        "  return v;\n"
        "}\n"
        "let t = launch hold([1, 2, 3]);\n"
        + "if (false) { }\n" * 80
        + "println(t.wait());\n",
        stdout="[1, 2, 3, 0, 1, 2]\n",
    )

    check(
        "launch returns a task",
        "import std;\n"
        "func work(n) { return n + 1; }\n"
        "let t = launch work(41);\n"
        "println(t.wait());\n"
        "println(t.done());\n"
        "println(launch work(1).wait());\n",
        stdout="42\ntrue\n2\n",
    )

    check(
        "launch rejects statically task-returning callee",
        "import std;\n"
        "func make_task(): task[null] { return std.task.sleep(1); }\n"
        "launch make_task();\n",
        stderr="launch cannot start a call that already returns task[null]",
    )

    check(
        "launch rejects task builtin",
        "import std;\n"
        "launch std.task.sleep(1);\n",
        stderr="launch cannot start a call that already returns task[null]",
    )

    check(
        "launch task methods",
        "import std;\n"
        "func work(n) { return n; }\n"
        "let t = launch work(7);\n"
        "println(t.detached());\n"
        "println(t.try_wait());\n",
        stdout="true\nok(7)\n",
    )

    check(
        "launch task type annotation",
        "import std;\n"
        "func work(n) { return n + 1; }\n"
        "let t: task[int] = launch work(41);\n"
        "println(t.wait());\n",
        stdout="42\n",
    )

    check(
        "launch spread",
        "import std;\n"
        "func add(a, b) { return a + b; }\n"
        "let xs = [20, 22];\n"
        "println((launch add(...xs)).wait());\n",
        stdout="42\n",
    )

    check(
        "task scope waits for children",
        "import std;\n"
        "func child(n) {\n"
        "  std.task.sleep(5).wait();\n"
        '  println("child-" + str(n));\n'
        "  return n;\n"
        "}\n"
        "let v = std.task.scope(func() {\n"
        "  let a = launch child(1);\n"
        "  let b = launch child(2);\n"
        '  println("body");\n'
        "  return a.wait() + b.wait();\n"
        "});\n"
        'println("scope=" + str(v));\n',
        stdout="body\nchild-1\nchild-2\nscope=3\n",
    )

    check(
        "task scope detach",
        "import std;\n"
        "func child() {\n"
        "  std.task.sleep(10).wait();\n"
        '  println("child");\n'
        "  return 1;\n"
        "}\n"
        "let v = std.task.scope(func() {\n"
        "  let t = launch child();\n"
        "  t.detach();\n"
        "  return 0;\n"
        "});\n"
        'println("scope=" + str(v));\n'
        "std.task.sleep(30).wait();\n",
        stdout="scope=0\nchild\n",
    )

    check(
        "task scope child failure cancels siblings",
        "import std;\n"
        "func ok() {\n"
        "  std.task.sleep(30).wait();\n"
        '  println("ok");\n'
        "  return 1;\n"
        "}\n"
        "func bad() {\n"
        "  std.task.sleep(5).wait();\n"
        '  fail "scope-child-fail";\n'
        "}\n"
        "std.task.scope(func() {\n"
        "  let a = launch ok();\n"
        "  let b = launch bad();\n"
        "  return 0;\n"
        "});\n",
        stderr="scope-child-fail",
        not_stderr="ok",
    )

    check(
        "task scope callback failure",
        'import std;\nstd.task.scope(func() { fail "scope-callback-fail"; });\n',
        stderr="scope-callback-fail",
    )

    check(
        "task scope callback must be callable",
        "import std;\nstd.task.scope(1);\n",
        stderr="std.task.scope callback must be callable",
    )

    check(
        "task scope named function",
        "import std;\n"
        "func body() { return 7; }\n"
        "println(str(std.task.scope(body)));\n",
        stdout="7\n",
    )

    check(
        "launch structured waits for children",
        "import std;\n"
        "func child() {\n"
        "  std.task.sleep(10).wait();\n"
        '  println("child");\n'
        "  return 1;\n"
        "}\n"
        "func parent() {\n"
        "  let t = launch child();\n"
        '  println("parent-body");\n'
        "  return 0;\n"
        "}\n"
        "println(launch parent().wait());\n",
        stdout="parent-body\nchild\n0\n",
    )

    check(
        "launch child failure cancels siblings",
        "import std;\n"
        "func ok(tag) {\n"
        "  std.task.sleep(20).wait();\n"
        '  println("ok-" + tag);\n'
        "  return tag;\n"
        "}\n"
        "func bad() {\n"
        "  std.task.sleep(5).wait();\n"
        '  fail "sibling-fail";\n'
        "}\n"
        "func parent() {\n"
        '  let a = launch ok("a");\n'
        "  let b = launch bad();\n"
        '  let c = launch ok("c");\n'
        "  return 0;\n"
        "}\n"
        "println(launch parent().wait());\n",
        stderr="sibling-fail",
        not_stderr="ok-a",
    )

    check(
        "launch detach removes structured wait",
        "import std;\n"
        "func child() {\n"
        "  std.task.sleep(10).wait();\n"
        '  println("child");\n'
        "  return 1;\n"
        "}\n"
        "func parent() {\n"
        "  let t = launch child();\n"
        "  t.detach();\n"
        '  println("parent");\n'
        "  return 0;\n"
        "}\n"
        "println(launch parent().wait());\n"
        "std.task.sleep(30).wait();\n",
        stdout="parent\n0\nchild\n",
    )

    check(
        "detached task failure is reported",
        "import std;\n"
        "func bad() {\n"
        "  std.task.sleep(5).wait();\n"
        '  fail "detached-boom";\n'
        "}\n"
        "func parent() {\n"
        "  let t = launch bad();\n"
        "  t.detach();\n"
        "  std.task.sleep(20).wait();\n"
        "}\n"
        "parent();\n",
        stderr="detached task failed: detached-boom",
    )

    check(
        "launch expects a call",
        'import std;\nlaunch 1;\n',
        stderr="launch expects a call expression",
    )
