#!/usr/bin/env python3
"""Async methods, async trait methods and their metadata."""
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
        "  async func get(self) { return self.x; }\n"
        "}\n"
        "let c = C { x: 7 };\n"
        "let t = c.get();\n"
        "println(await t);\n",
        ["7"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  async func get(self) {\n"
        '    println("start");\n'
        "    await std.task.sleep(1);\n"
        '    println("end");\n'
        "    return self.x;\n"
        "  }\n"
        "}\n"
        "println(await C { x: 8 }.get());\n",
        ["start", "end", "8"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  async func get(self) { return self.x; }\n"
        "}\n"
        "let c = C { x: 9 };\n"
        "let f = c.get;\n"
        "println(await f());\n",
        ["9"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  async func add(self, a: int = self.x, b: int = a + 1) {\n"
        "    return a + b;\n"
        "  }\n"
        "}\n"
        "println(await C { x: 3 }.add());\n"
        "println(await C { x: 3 }.add(10));\n",
        ["7", "21"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  async func total(self, ...nums: int) {\n"
        "    let total = self.x;\n"
        "    for (let n in nums) { total = total + n; }\n"
        "    return total;\n"
        "  }\n"
        "}\n"
        "println(await C { x: 1 }.total(2, 3, 4));\n",
        ["10"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  async func get(self) {\n"
        '    defer println("cleanup");\n'
        "    return self.x;\n"
        "  }\n"
        "}\n"
        "println(await C { x: 4 }.get());\n",
        ["cleanup", "4"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  async func fact(self, n: int) {\n"
        "    if (n <= 1) { return 1; }\n"
        "    return n * await self.fact(n - 1);\n"
        "  }\n"
        "}\n"
        "println(await C { x: 0 }.fact(4));\n",
        ["24"],
    )

    run_case(
        "import std;\n"
        "enum E { A { n: int; }; B; }\n"
        "impl E {\n"
        "  async func value(self) { return 1; }\n"
        "}\n"
        "println(await E.A { n: 2 }.value());\n",
        ["1"],
    )

    run_case(
        "import std;\n"
        "trait S { async func handle(self, x: int); }\n"
        "struct C { base: int; }\n"
        "impl S for C {\n"
        "  async func handle(self, x: int) { return self.base + x; }\n"
        "}\n"
        "async func use(s: S) { return await s.handle(2); }\n"
        "println(await use(C { base: 5 }));\n",
        ["7"],
    )

    run_case(
        "import std;\n"
        "trait S { async func handle(self); }\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  async func handle(self) { return self.x; }\n"
        "}\n"
        "impl S for C;\n"
        "async func use(s: S) { return await s.handle(); }\n"
        "println(await use(C { x: 11 }));\n",
        ["11"],
    )

    run_case(
        "import std;\n"
        "trait S {\n"
        "  async func handle(self);\n"
        "  async func twice(self) { return await self.handle() * 2; }\n"
        "}\n"
        "struct C { x: int; }\n"
        "impl S for C {\n"
        "  async func handle(self) { return self.x; }\n"
        "}\n"
        "println(await C { x: 11 }.twice());\n",
        ["22"],
    )

    run_case(
        "import std;\n"
        "struct C { x: int; }\n"
        "impl C {\n"
        "  async func get(self) { return self.x; }\n"
        "}\n"
        "let m = std.meta.methods(\"C\")[0];\n"
        "println(m[\"name\"]);\n"
        "println(m[\"async\"]);\n",
        ["get", "true"],
    )

    check(
        "async trait marker mismatch",
        "trait S { async func handle(self); }\n"
        "struct C { x: int; }\n"
        "impl S for C {\n"
        "  func handle(self) { return self.x; }\n"
        "}\n",
        stderr="async marker does not match",
    )

    check(
        "sync trait marker mismatch",
        "trait S { func handle(self); }\n"
        "struct C { x: int; }\n"
        "impl S for C {\n"
        "  async func handle(self) { return self.x; }\n"
        "}\n",
        stderr="async marker does not match",
    )

    check(
        "await non-task",
        "await 42\n",
        stderr="expected a task, got int",
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
            "    async func get(self) { return self.x; }\n"
            "  }\n"
            "}\n",
        )
        main = write(
            "main.skein",
            "import aa;\n"
            "let c = aa.make(12);\n"
            "println(await c.get());\n",
        )
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["12"], proc.stdout

    env = os.environ.copy()
    env["SKEIN_FIBER_STACK_STATS"] = "1"
    proc = run(
        "import std;\n"
        "async func work(n) { return n + 1; }\n"
        "let total = 0;\n"
        "for (let i = 0; i < 40; i = i + 1) {\n"
        "  total = total + await work(i);\n"
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
        "suspended async task survives cycle collection",
        "import std;\n"
        "async func hold(v) {\n"
        "  for (let i = 0; i < 3; i = i + 1) {\n"
        "    await std.task.sleep(1);\n"
        "    v = std.list.append(v, i);\n"
        "  }\n"
        "  return v;\n"
        "}\n"
        "let t = hold([1, 2, 3]);\n"
        + "if (false) { }\n" * 80
        + "println(await t);\n",
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
        "launch adopts async task",
        "import std;\n"
        "async func add(a, b) { return a + b; }\n"
        "let t = launch add(2, 3);\n"
        "println(t.wait());\n",
        stdout="5\n",
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
        "launch structured waits for children",
        "import std;\n"
        "async func child() {\n"
        "  await std.task.sleep(10);\n"
        '  println("child");\n'
        "  return 1;\n"
        "}\n"
        "async func parent() {\n"
        "  let t = launch child();\n"
        '  println("parent-body");\n'
        "  return 0;\n"
        "}\n"
        "println(await parent());\n",
        stdout="parent-body\nchild\n0\n",
    )

    check(
        "launch child failure cancels siblings",
        "import std;\n"
        "async func ok(tag) {\n"
        "  await std.task.sleep(20);\n"
        '  println("ok-" + tag);\n'
        "  return tag;\n"
        "}\n"
        "async func bad() {\n"
        "  await std.task.sleep(5);\n"
        '  fail "sibling-fail";\n'
        "}\n"
        "async func parent() {\n"
        '  let a = launch ok("a");\n'
        "  let b = launch bad();\n"
        '  let c = launch ok("c");\n'
        "  return 0;\n"
        "}\n"
        "println(await parent());\n",
        stderr="sibling-fail",
        not_stderr="ok-a",
    )

    check(
        "launch detach removes structured wait",
        "import std;\n"
        "async func child() {\n"
        "  await std.task.sleep(10);\n"
        '  println("child");\n'
        "  return 1;\n"
        "}\n"
        "async func parent() {\n"
        "  let t = launch child();\n"
        "  t.detach();\n"
        '  println("parent");\n'
        "  return 0;\n"
        "}\n"
        "println(await parent());\n"
        "await std.task.sleep(30);\n",
        stdout="parent\n0\nchild\n",
    )

    check(
        "launch expects a call",
        'import std;\nlaunch 1;\n',
        stderr="launch expects a call expression",
    )
