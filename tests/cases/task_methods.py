#!/usr/bin/env python3
"""Task methods, protocol methods and launch/task metadata.

Method and launch/scope behavior cases live in tests/skein/task_methods_basic.skein
and tests/skein/task_methods_launch.skein.  GC, error, and temp-package cases
stay here.
"""
import os
import tempfile

from harness import *  # noqa: F401,F403


def run_cases():
    run_skein("task_methods_basic.skein")
    run_skein("task_methods_launch.skein")
    run_skein("wait.skein")
    check(
        "task methods golden output",
        "import std;\n"
        "struct G { x: int; }\n"
        "impl G { func get(self) { return self.x; } }\n"
        "std.io.println(G { x: 5 }.get());\n",
        stdout="5",
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
            "  export struct C { x: int; }\n"
            "  export func make(x) { return C { x: x }; }\n"
            "  impl C {\n"
            "    func get(self) { return self.x; }\n"
            "  }\n"
            "}\n",
        )
        main = write(
            "main.skein",
            "import std;\n"
            "import aa;\n"
            "let c = aa.make(12);\n"
            "std.io.println(c.get());\n",
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
        "  total = total + wait launch work(i);\n"
        "}\n"
        "std.io.println(total);\n",
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
        "std.io.println(first[\"value\"]);\n"
        + "if (false) { }\n" * 80
        + "let second = std.generator.next(g);\n"
        "std.io.println(second[\"value\"]);\n",
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
        + "std.io.println(t.wait());\n",
        stdout="[1, 2, 3, 0, 1, 2]\n",
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
        "task scope detach",
        "import std;\n"
        "func child() {\n"
        "  std.task.sleep(10).wait();\n"
        '  std.io.println("child");\n'
        "  return 1;\n"
        "}\n"
        "let v = std.task.scope(func() {\n"
        "  let t = launch child();\n"
        "  t.detach();\n"
        "  return 0;\n"
        "});\n"
        'std.io.println("scope=" + str(v));\n'
        "std.task.sleep(30).wait();\n",
        stdout="scope=0\nchild\n",
    )

    check(
        "task scope child failure cancels siblings",
        "import std;\n"
        "func ok() {\n"
        "  std.task.sleep(30).wait();\n"
        '  std.io.println("ok");\n'
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
        "launch child failure cancels siblings",
        "import std;\n"
        "func ok(tag) {\n"
        "  std.task.sleep(20).wait();\n"
        '  std.io.println("ok-" + tag);\n'
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
        "std.io.println(wait launch parent());\n",
        stderr="sibling-fail",
        not_stderr="ok-a",
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

    check(
        "await launches and waits for a call",
        "import std;\n"
        "func work(n) { return n + 1; }\n"
        "std.io.println(await work(41));\n"
        "std.io.println(wait launch work(41));\n",
        stdout="42\n42\n",
    )

    check(
        "await requires a call expression",
        "import std;\n"
        "let t = launch std.task.sleep(1);\n"
        "await t;\n",
        stderr="launch expects a call expression",
    )

    check(
        "await on error catches task failure",
        'import std;\n'
        'func boom() { fail "await-boom"; }\n'
        'let v = await boom() on error(e) { recover 7; };\n'
        'std.io.println(v);\n',
        stdout="7",
        not_stderr="on error does not catch failures from the launched function",
    )

    check(
        "await postfix binds before the prefix",
        "import std;\n"
        "struct Box { x: int; }\n"
        "func make(): Box { return Box { x: 7 }; }\n"
        "std.io.println(await make().x);\n"
        "std.io.println((await make()).x);\n",
        stdout="7\n7\n",
    )

    check(
        "task keyword names are shadowable",
        "import std;\n"
        "func work(): int { return 8; }\n"
        "let launch = work;\n"
        "let wait = work;\n"
        "let await = work;\n"
        "std.io.println((launch)());\n"
        "std.io.println((wait)());\n"
        "std.io.println((await)());\n"
        "std.io.println(await(work()));\n",
        stdout="8\n8\n8\n8\n",
    )

    check(
        "await as a member name stays an identifier",
        "import std;\n"
        "struct Holder { await: int; }\n"
        "let h = Holder { await: 9 };\n"
        "std.io.println(h.await);\n",
        stdout="9",
    )

    check(
        "launch on error warns about task failures",
        "import std;\n"
        "func noop() {}\n"
        "let t = launch noop() on error(e) { recover null; };\n"
        "wait t;\n",
        stderr="on error does not catch failures from the launched function",
    )

    check(
        "wait with on error does not warn",
        "func noop() {}\n"
        "let t = launch noop();\n"
        "wait t on error(e) { recover null; };\n"
        "wait launch noop() on error(e) { recover null; };\n",
        not_stderr="on error does not catch failures from the launched function",
    )

    check(
        "wait launch with on error catches task failure",
        "import std;\n"
        "func boom() {\n"
        '  fail "boom";\n'
        "}\n"
        "let r = wait launch boom() on error(e) {\n"
        "  recover str(e);\n"
        "};\n"
        "std.io.println(r);\n",
        stdout="boom",
        not_stderr="detached task failed",
    )

    check(
        "wait launch with unbound on error",
        "func boom() {\n"
        '  fail "boom";\n'
        "}\n"
        "let r = wait launch boom() on error() {\n"
        '  recover "caught";\n'
        "};\n"
        "std.io.println(r);\n",
        stdout="caught",
        not_stderr="detached task failed",
    )

    check(
        "wait task variable with on error catches task failure",
        "import std;\n"
        "func boom() {\n"
        "  std.task.sleep(1);\n"
        '  fail "boom";\n'
        "}\n"
        "let t = launch boom();\n"
        "let r = wait t on error(e) {\n"
        "  recover str(e);\n"
        "};\n"
        "std.io.println(r);\n",
        stdout="boom",
        not_stderr="detached task failed",
    )

    check(
        "wait parenthesized launch with on error",
        "func boom() {\n"
        '  fail "boom";\n'
        "}\n"
        "let r = wait (launch boom()) on error(e) {\n"
        "  recover str(e);\n"
        "};\n"
        "std.io.println(r);\n",
        stdout="boom",
        not_stderr="detached task failed",
    )

    check(
        "on error without a binding",
        "let r = (1 / 0) on error() { recover 7; };\n"
        "std.io.println(r);\n",
        stdout="7",
    )
