#!/usr/bin/env python3
"""Error handling, math, generators and task failures.

Extracted from the original test_skein.py main() body, lines 2888-3522.
"""
from harness import *  # noqa: F401,F403

import os
import tempfile


def run_cases():
    check(
        "generator return value",
        "import std;\n"
        "func value() {\n"
        "  yield 1;\n"
        "  return 9;\n"
        "}\n"
        "let g = value();\n"
        "std.generator.next(g)\n"
        "std.generator.next(g)\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "= {done: true, value: 9}",
        ],
    )
    check(
        "generator fail from sent error",
        "import std;\n"
        "func worker() {\n"
        "  let signal = yield 1;\n"
        "  if (signal is error) { fail signal; }\n"
        "  yield 2;\n"
        "}\n"
        "let g = worker();\n"
        "std.generator.next(g);\n"
        "let v = std.generator.next(g, "
        "std.error.new(\"cancel\", \"stop\")) on error(e) {\n"
        "  std.io.print(std.error.kind(e));\n"
        "  std.io.print(str(e));\n"
        "  recover 0;\n"
        "};\n"
        "v\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "cancel",
            "stop",
            "= 0",
        ],
    )
    check(
        "generator close",
        "import std;\n"
        "func g() {\n"
        "  defer std.io.print(\"cleanup\");\n"
        "  yield 1;\n"
        "  std.io.print(\"after\");\n"
        "  yield 2;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen)\n"
        "std.generator.close(gen)\n"
        "std.generator.done(gen)\n"
        "std.generator.next(gen)\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "cleanup",
            "= true",
            "= true",
            "= {done: true, value: null}",
        ],
    )
    check(
        "generator drop warning",
        "import std;\n"
        "func g() {\n"
        "  defer std.io.print(\"cleanup\");\n"
        "  yield 1;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n"
        "gen = null;\n"
        "std.io.print(\"after drop\");\n",
        stdout="after drop",
        stderr="suspended generator dropped without close",
        code=0,
    )
    check(
        "generator close no warning",
        "import std;\n"
        "func g() {\n"
        "  defer std.io.print(\"cleanup\");\n"
        "  yield 1;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n"
        "std.generator.close(gen);\n"
        "gen = null;\n"
        "std.io.print(\"after close\");\n",
        stdout="cleanup",
        not_stderr="dropped without close",
    )
    check(
        "generator drop without defers",
        "import std;\n"
        "func g() { yield 1; }\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n"
        "gen = null;\n"
        "std.io.print(\"after\");\n",
        stdout="after",
        not_stderr="dropped without close",
    )
    check(
        "generator shutdown no warning",
        "import std;\n"
        "func g() {\n"
        "  defer std.io.print(\"cleanup\");\n"
        "  yield 1;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n",
        not_stderr="dropped without close",
    )
    check(
        "generator close defer error",
        "import std;\n"
        "func boom() { fail \"defer boom\"; }\n"
        "func g() {\n"
        "  defer boom();\n"
        "  yield 1;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n"
        "let v = std.generator.close(gen) on error(e) {\n"
        "  std.io.print(std.error.kind(e));\n"
        "  std.io.print(str(e));\n"
        "  recover false;\n"
        "};\n"
        "v\n"
        "std.generator.done(gen)\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "fail",
            "defer boom",
            "= false",
            "= true",
        ],
    )
    check(
        "generator close before start",
        "import std;\n"
        "func never() {\n"
        "  std.io.print(\"started\");\n"
        "  defer std.io.print(\"never cleanup\");\n"
        "  yield 1;\n"
        "}\n"
        "let gen = never();\n"
        "std.generator.close(gen)\n"
        "std.generator.done(gen)\n",
        stdout_all=["= true", "= true"],
    )
    check(
        "generator close done",
        "import std;\n"
        "func g() { yield 1; }\n"
        "let gen = g();\n"
        "std.generator.next(gen)\n"
        "std.generator.next(gen)\n"
        "std.generator.close(gen)\n",
        stdout="= false",
    )
    check(
        "generator close running",
        "import std;\n"
        "let target = null;\n"
        "func g() {\n"
        "  std.generator.close(target);\n"
        "  yield 1;\n"
        "}\n"
        "target = g();\n"
        "std.generator.next(target);\n",
        stderr="cannot close a running generator",
    )
    check(
        "yield outside generator",
        "yield 1;\n",
        stderr="yield outside generator",
    )
    check(
        "generator error",
        "import std;\n"
        "func broken() {\n"
        "  yield 1;\n"
        "  let x = 1 / 0;\n"
        "}\n"
        "let g = broken();\n"
        "std.generator.next(g)\n"
        "std.generator.next(g);\n",
        stderr="division by zero",
    )
    check(
        "error value type",
        "import std;\n"
        "let e = std.error.new(\"io\", \"boom\");\n"
        "std.type.kind_of(e)\n"
        "std.error.kind(e)\n"
        "std.error.message(e)\n"
        "e is error\n"
        "str(e)\n"
        "e == \"boom\"\n"
        "e == std.error.new(\"io\", \"boom\")\n",
        stdout_all=[
            "= error", "= io", "= boom", "= true",
            "= boom", "= true", "= true",
        ],
    )
    check(
        "on error runtime",
        "import std;\n"
        "let v = (1 / 0) on error(e) {\n"
        "  std.io.print(std.error.kind(e));\n"
        "  std.io.print(str(e));\n"
        "  recover 42;\n"
        "};\n"
        "v\n",
        stdout_all=["runtime", "division by zero", "= 42"],
    )
    check(
        "on error fail payload",
        "import std;\n"
        "func f() { fail \"boom\"; }\n"
        "let v = f() on error(e) {\n"
        "  std.io.print(std.type.kind_of(e));\n"
        "  std.io.print(std.error.kind(e));\n"
        "  std.io.print(str(e));\n"
        "  recover 7;\n"
        "};\n"
        "v\n",
        stdout_all=["error", "fail", "boom", "= 7"],
    )
    check(
        "capture fail",
        "import std;\n"
        "let r = std.result.capture(func() { fail \"boom\"; });\n"
        "std.result.is_err(r)\n"
        "std.error.kind(std.result.error(r))\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= fail", "= boom"],
    )
    check(
        "unwrap raises",
        "import std;\n"
        "let v = std.result.unwrap(std.result.err(\"boom\")) on error(e) {\n"
        "  recover str(e);\n"
        "};\n"
        "v\n",
        stdout="= boom",
    )
    check(
        "task fail payload",
        "import std;\n"
        "func f() { fail \"task boom\"; }\n"
        "let r = std.task.catch(launch f()).wait();\n"
        "std.result.is_err(r)\n"
        "std.error.kind(std.result.error(r))\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= fail", "= task boom"],
    )
    check(
        "generator error payload",
        "import std;\n"
        "func broken() {\n"
        "  fail \"generator boom\";\n"
        "  yield 1;\n"
        "}\n"
        "let v = std.generator.next(broken()) on error(e) {\n"
        "  std.io.print(std.error.kind(e));\n"
        "  recover str(e);\n"
        "};\n"
        "v\n",
        stdout_all=["fail", "= generator boom"],
    )
    check(
        "task error kind",
        "import std;\n"
        "let t = std.task.sleep(1000);\n"
        "std.task.cancel(t);\n"
        "let r = std.result.capture(func() { return std.task.wait(t); });\n"
        "std.error.kind(std.result.error(r))\n",
        stdout="= cancelled",
    )
    check(
        "generator closure value",
        "import std;\n"
        "func make() {\n"
        "  let x = 5;\n"
        "  yield func() { return x; };\n"
        "}\n"
        "let g = make();\n"
        "let r = std.generator.next(g);\n"
        "let f = r[\"value\"];\n"
        "f()\n",
        stdout="= 5",
    )
    check(
        "generator send value",
        "import std;\n"
        "func echo() {\n"
        "  let x = yield 1;\n"
        "  yield x + 1;\n"
        "}\n"
        "let g = echo();\n"
        "std.generator.next(g)\n"
        "std.generator.next(g, 10)\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "= {done: false, value: 11}",
        ],
    )
    check(
        "generator yield null shorthand",
        "import std;\n"
        "func echo() {\n"
        "  let x = yield;\n"
        "  yield x;\n"
        "}\n"
        "let g = echo();\n"
        "std.generator.next(g)\n"
        "std.generator.next(g, 5)\n"
        "func bare() {\n"
        "  yield;\n"
        "  yield 1;\n"
        "}\n"
        "let b = bare();\n"
        "std.generator.next(b)\n"
        "std.generator.next(b)\n",
        stdout_all=[
            "= {done: false, value: null}",
            "= {done: false, value: 5}",
            "= {done: false, value: null}",
            "= {done: false, value: 1}",
        ],
    )
    check(
        "generator for-in",
        "import std;\n"
        "func count(n) {\n"
        "  for (let i in std.utils.range(n)) { yield i; }\n"
        "}\n"
        "for (let x in count(3)) { std.io.print(x); }\n",
        stdout_all=["0", "1", "2"],
    )
    check(
        "std result basics",
        "import std;\n"
        "let a = std.result.ok(42);\n"
        "std.result.is_ok(a)\n"
        "std.result.unwrap(a)\n"
        "let b = std.result.err(\"boom\");\n"
        "std.result.is_err(b)\n"
        "std.result.unwrap_or(b, 7)\n"
        "std.result.error(b)\n"
        "std.type.kind_of(a)\n"
        "a is result\n",
        stdout_all=[
            "= true", "= 42", "= true", "= 7", "= boom",
            "= result", "= true",
        ],
    )
    check(
        "std result capture",
        "import std;\n"
        "let good = std.result.capture(func() { return 5; });\n"
        "std.result.unwrap(good)\n"
        "let bad = std.result.capture(func() { return 1 / 0; });\n"
        "std.result.is_err(bad)\n"
        "std.result.error(bad)\n",
        stdout_all=["= 5", "= true", "= division by zero"],
    )
    check(
        "std result capture keeps error trace",
        "import std;\n"
        "let r = std.result.capture(func() { return 1 / 0; });\n"
        "let e = std.result.error(r);\n"
        'std.io.println(str(std.error.location(e)["line"]));\n'
        "std.io.println(str(std.utils.len(std.error.trace(e))));\n"
        "std.io.println(std.error.format_trace(e));\n",
        stdout_all=["2", "1", "runtime: division by zero",
                    "in <anonymous>() called from line 2"],
    )
    with tempfile.TemporaryDirectory() as thread_tmp:
        thread_worker = os.path.join(thread_tmp, "worker.skein")
        with open(thread_worker, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                "import std;\n"
                'func main(ctx) { fail "thread boom"; }\n'
            )
        check(
            "thread error keeps trace",
            "import std;\n"
            f"let t = std.thread.spawn_script({skein_string(thread_worker)}, "
            "std.thread.Message.from({}));\n"
            "let r = t.join();\n"
            'let e = r["error"];\n'
            'std.io.println(str(std.error.location(e)["line"]));\n'
            "std.io.println(str(std.utils.len(std.error.trace(e))));\n"
            "std.io.println(std.error.format_trace(e));\n",
            stdout_all=["2", "1", "fail: thread boom",
                        "in main() called from"],
        )
    with tempfile.TemporaryDirectory() as tuple_thread_tmp:
        tuple_worker = os.path.join(tuple_thread_tmp, "worker.skein")
        with open(tuple_worker, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                "import std;\n"
                "func main(ctx) {\n"
                '  let d = ctx.payload["d"];\n'
                '  return d[(1, "a")];\n'
                "}\n"
            )
        check(
            "thread payload preserves tuple dict keys",
            "import std;\n"
            'let d = {(1, "a"): 42};\n'
            f"let t = std.thread.spawn_script({skein_string(tuple_worker)}, "
            'std.thread.Message.from({"d": d}));\n'
            "let r = t.join();\n"
            'std.io.println(str(r["ok"]));\n'
            'std.io.println(str(r["value"]));\n',
            stdout_all=["true", "42"],
        )
        struct_worker = os.path.join(tuple_thread_tmp, "struct_worker.skein")
        with open(struct_worker, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                "import std;\n"
                "func main(ctx) {\n"
                '  let d = ctx.payload["d"];\n'
                '  let k = ctx.payload["k"];\n'
                "  return d[k];\n"
                "}\n"
            )
        check(
            "thread payload preserves struct dict keys",
            "import std;\n"
            'struct Key { id: int; tag: string; }\n'
            'let k = Key { id: 1, tag: "a" };\n'
            "let d = {k: 42};\n"
            f"let t = std.thread.spawn_script({skein_string(struct_worker)}, "
            'std.thread.Message.from({"d": d, "k": k}));\n'
            "let r = t.join();\n"
            'std.io.println(str(r["ok"]));\n'
            'std.io.println(str(r["value"]));\n',
            stdout_all=["true", "42"],
        )
    check(
        "std result capture net",
        "import std;\n"
        "let r = std.result.capture(func() {\n"
        "  return std.net.tcp_connect(\"127.0.0.1\", 1, 100);\n"
        "});\n"
        "std.result.is_err(r)\n",
        stdout="= true",
    )
    check(
        "result on error unwrap",
        "import std;\n"
        "let v = std.result.ok(42) on error(e) { recover 0; };\n"
        "v\n",
        stdout="= 42",
    )
    check(
        "result on error handler",
        "import std;\n"
        "let v = std.result.err(\"boom\") on error(e) {\n"
        "  std.io.print(std.error.kind(e));\n"
        "  std.io.print(str(e));\n"
        "  recover 7;\n"
        "};\n"
        "v\n",
        stdout_all=["error", "boom", "= 7"],
    )
    check(
        "result on error pass",
        "import std;\n"
        "func f() { return std.result.err(\"inner\"); }\n"
        "let v = (f() on error(e) { pass e; }) on error(e2) {\n"
        "  std.io.print(\"outer \" + str(e2));\n"
        "  recover 9;\n"
        "};\n"
        "v\n",
        stdout_all=["outer inner", "= 9"],
    )
    check(
        "result on error normal value",
        "import std;\n"
        "let v = 5 on error(e) { recover 0; };\n"
        "v\n",
        stdout="= 5",
    )
    check(
        "result on error nested",
        "import std;\n"
        "let v = std.result.ok(std.result.ok(1)) on error(e) {\n"
        "  recover 0;\n"
        "};\n"
        "v\n",
        stdout="= ok(1)",
    )
    check(
        "result on error recover result",
        "import std;\n"
        "let r = std.result.err(\"x\") on error(e) {\n"
        "  recover std.result.err(\"y\");\n"
        "};\n"
        "std.result.is_err(r)\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= y"],
    )
    check(
        "do block on error value",
        "import std;\n"
        "let v = do {\n"
        "  let a = 3;\n"
        "  a + 4;\n"
        "} on error(e) { recover 0; };\n"
        "v\n",
        stdout="= 7",
    )
    check(
        "do block on error catches fail",
        "import std;\n"
        "let v = do {\n"
        "  fail \"boom\";\n"
        "  1;\n"
        "} on error(e) {\n"
        "  std.io.print(str(e));\n"
        "  recover 42;\n"
        "};\n"
        "v\n",
        stdout_all=["boom", "= 42"],
    )
    check(
        "do block on error catches runtime",
        "import std;\n"
        "let v = do {\n"
        "  1 / 0;\n"
        "} on error(e) {\n"
        "  std.io.print(std.error.kind(e));\n"
        "  recover -1;\n"
        "};\n"
        "v\n",
        stdout_all=["runtime", "= -1"],
    )
    check(
        "do block on error result unwrap",
        "import std;\n"
        "let v = do {\n"
        "  std.result.ok(11);\n"
        "} on error(e) { recover 0; };\n"
        "v\n",
        stdout="= 11",
    )
    check(
        "do block on error result error",
        "import std;\n"
        "let v = do {\n"
        "  std.result.err(\"bad\");\n"
        "} on error(e) {\n"
        "  std.io.print(str(e));\n"
        "  recover 12;\n"
        "};\n"
        "v\n",
        stdout_all=["bad", "= 12"],
    )
    check(
        "do block on error declaration value",
        "import std;\n"
        "let a = do {\n"
        "  let x = 5;\n"
        "} on error(e) { recover 1; };\n"
        "let b = do {\n"
        "} on error(e) { recover 2; };\n"
        "std.io.print(a);\n"
        "std.io.print(b);\n",
        stdout_all=["null", "null"],
    )
    check(
        "do block on error pass",
        "import std;\n"
        "func inner() {\n"
        "  return do {\n"
        "    fail \"deep\";\n"
        "  } on error(e) { pass e; };\n"
        "}\n"
        "let v = inner() on error(e) {\n"
        "  std.io.print(\"outer \" + str(e));\n"
        "  recover 8;\n"
        "};\n"
        "v\n",
        stdout_all=["outer deep", "= 8"],
    )
    check(
        "do block on error return",
        "import std;\n"
        "func f() {\n"
        "  let v = do {\n"
        "    return 5;\n"
        "  } on error(e) { recover 0; };\n"
        "  return v + 1;\n"
        "}\n"
        "f()\n",
        stdout="= 5",
    )
    check(
        "do block on error break continue",
        "import std;\n"
        "let total = 0;\n"
        "let i = 0;\n"
        "while (i < 5) {\n"
        "  let v = do {\n"
        "    i = i + 1;\n"
        "    if (i == 2) { continue; }\n"
        "    if (i == 4) { break; }\n"
        "    i * 10;\n"
        "  } on error(e) { recover 0; };\n"
        "  total = total + v;\n"
        "}\n"
        "std.io.print(total);\n"
        "std.io.print(i);\n",
        stdout_all=["40", "4"],
    )
    check(
        "do block on error then while",
        "import std;\n"
        "func f(n) {\n"
        "  let v = do { n + 1; } on error(e) { recover 0; };\n"
        "  let i = 0;\n"
        "  while (i < 3) { i = i + 1; }\n"
        "  return v + i;\n"
        "}\n"
        "f(10)\n",
        stdout="= 14",
    )
    check(
        "do block on error closure capture",
        "import std;\n"
        "func make(base) {\n"
        "  return func(x) {\n"
        "    return do { base + x; } on error(e) { recover -1; };\n"
        "  };\n"
        "}\n"
        "make(10)(5)\n",
        stdout="= 15",
    )
    check(
        "do block on error defer",
        "import std;\n"
        "func f(flag) {\n"
        "  return do {\n"
        "    defer std.io.print(\"deferred\");\n"
        "    if (flag) { fail \"flagged\"; }\n"
        "    1;\n"
        "  } on error(e) { recover 2; };\n"
        "}\n"
        "std.io.print(f(false));\n"
        "std.io.print(f(true));\n",
        stdout_all=["deferred", "1", "deferred", "2"],
    )
    check(
        "do block expression statement",
        "import std;\n"
        "do {\n"
        "  std.io.print(\"x\");\n"
        "};\n",
        stdout="x",
    )
    check(
        "do block keeps statement semicolons",
        "import std;\n"
        "let v = do {\n"
        "  1\n"
        "} on error(e) { recover 0; };\n"
        "v\n",
        stderr="syntax error",
    )
    check(
        "result on error task",
        "import std;\n"
        "func bad() { fail \"task boom\"; }\n"
        "let v = (std.task.catch(launch bad()).wait()) on error(e) {\n"
        "  std.io.print(\"task \" + str(e));\n"
        "  recover 3;\n"
        "};\n"
        "v\n",
        stdout_all=["task task boom", "= 3"],
    )
    check(
        "std result capture task",
        "import std;\n"
        "let t = std.task.sleep(1000);\n"
        "std.task.cancel(t);\n"
        "let r = std.result.capture(func() { return std.task.wait(t); });\n"
        "std.result.is_err(r)\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= task was cancelled"],
    )
    check(
        "std task catch",
        "import std;\n"
        "let t = std.task.sleep(1000);\n"
        "let c = std.task.catch(t);\n"
        "std.task.cancel(t);\n"
        "let r = std.task.wait(c);\n"
        "std.result.is_err(r)\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= task was cancelled"],
    )
    check(
        "std task try_wait",
        "import std;\n"
        "let t = std.task.sleep(10);\n"
        "let r = std.task.try_wait(t, 100);\n"
        "std.result.is_ok(r)\n"
        "let x = std.task.sleep(1000);\n"
        "std.result.is_err(std.task.try_wait(x, 10))\n",
        stdout_all=["= true", "= true"],
    )
    check(
        "task catch",
        "import std;\n"
        "func bad() {\n"
        "  let x = 1 / 0;\n"
        "  return x;\n"
        "}\n"
        "let t = launch bad();\n"
        "let r = std.task.catch(t).wait();\n"
        "std.result.is_err(r)\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= division by zero"],
    )
    check(
        "defer order",
        "import std;\n"
        "func f() {\n"
        "  defer std.io.print(\"a\");\n"
        "  defer std.io.print(\"b\");\n"
        "  std.io.print(\"x\");\n"
        "}\n"
        "f();\n",
        stdout_all=["x", "b", "a"],
    )
    check(
        "defer block scope",
        "import std;\n"
        "func f() {\n"
        "  { defer std.io.print(\"block\"); std.io.print(\"in\"); }\n"
        "  std.io.print(\"after\");\n"
        "}\n"
        "f();\n",
        stdout_all=["in", "block", "after"],
    )
    check(
        "defer rejects later declaration",
        "import std;\n"
        "let x = 100;\n"
        "func f() {\n"
        "  defer std.io.print(str(x));\n"
        "  let x = 5;\n"
        "}\n"
        "f();\n",
        stderr="references x before its declaration",
    )
    check(
        "defer rejects later shadow declaration",
        "import std;\n"
        "func f() {\n"
        "  let x = 1;\n"
        "  { defer std.io.print(str(x)); let x = 2; }\n"
        "}\n"
        "f();\n",
        stderr="references x before its declaration",
    )
    check(
        "defer captures local closure",
        "import std;\n"
        "func f() {\n"
        "  let x = 5;\n"
        "  defer std.io.print(str(func() { return x; }()));\n"
        "  x = 7;\n"
        "}\n"
        "f();\n",
        stdout="7",
    )
    check(
        "defer captures shadowed local",
        "import std;\n"
        "func f() {\n"
        "  let x = 1;\n"
        "  { let x = 2; defer std.io.print(str(x)); }\n"
        "  defer std.io.print(str(x));\n"
        "}\n"
        "f();\n",
        stdout="21",
    )
    check(
        "defer inside error handler captures local",
        "import std;\n"
        "func boom() { fail \"boom\"; }\n"
        "func f() {\n"
        "  let x = 1;\n"
        "  defer std.io.print(str(x));\n"
        "  let v = boom() on error(e) {\n"
        "    let y = 2;\n"
        "    defer std.io.print(str(y));\n"
        "    recover 5;\n"
        "  };\n"
        "  std.io.print(str(v));\n"
        "}\n"
        "f();\n",
        stdout="251",
    )
    check(
        "handler captures local closure",
        "import std;\n"
        "func boom() { fail \"boom\"; }\n"
        "func f() {\n"
        "  let g = func() { return 0; };\n"
        "  let v = boom() on error(e) {\n"
        "    let y = 4;\n"
        "    func h() { return y; }\n"
        "    g = h;\n"
        "    recover y;\n"
        "  };\n"
        "  std.io.print(str(v));\n"
        "  std.io.print(str(g()));\n"
        "}\n"
        "f();\n",
        stdout="44",
    )
    check(
        "defer return",
        "import std;\n"
        "func f() {\n"
        "  defer std.io.print(\"cleanup\");\n"
        "  return 1;\n"
        "}\n"
        "std.io.print(f());\n",
        stdout_all=["cleanup", "1"],
    )
    check(
        "defer on error",
        "import std;\n"
        "func f() {\n"
        "  defer std.io.print(\"cleanup\");\n"
        "  let x = 1 / 0;\n"
        "}\n"
        "f();\n",
        stdout="cleanup",
        stderr="division by zero",
    )
    check(
        "defer preserves fail payload",
        "import std;\n"
        "func f() {\n"
        "  defer std.io.print(\"cleanup\");\n"
        "  fail \"boom\";\n"
        "}\n"
        "let v = f() on error(e) {\n"
        "  std.io.print(std.error.kind(e));\n"
        "  std.io.print(str(e));\n"
        "  recover 1;\n"
        "};\n"
        "v\n",
        stdout_all=["cleanup", "fail", "boom", "= 1"],
    )
    check(
        "defer error propagates",
        "import std;\n"
        "func boom() { fail \"defer boom\"; }\n"
        "func f() {\n"
        "  defer boom();\n"
        "  return 1;\n"
        "}\n"
        "let v = f() on error(e) {\n"
        "  std.io.print(\"caught \" + str(e));\n"
        "  recover 2;\n"
        "};\n"
        "v\n",
        stdout_all=["caught defer boom", "= 2"],
    )
    check(
        "defer keeps original error",
        "import std;\n"
        "func boom() { fail \"defer boom\"; }\n"
        "func f() {\n"
        "  defer boom();\n"
        "  fail \"main boom\";\n"
        "}\n"
        "let v = f() on error(e) {\n"
        "  std.io.print(\"caught \" + str(e));\n"
        "  recover 3;\n"
        "};\n"
        "v\n",
        stdout_all=["caught main boom", "= 3"],
        stderr="error in defer: defer boom",
    )
    check(
        "defer loop break",
        "import std;\n"
        "for (let i in std.utils.range(5)) {\n"
        "  defer std.io.print(\"done \" + str(i));\n"
        "  if (i == 1) { break; }\n"
        "  std.io.print(\"body \" + str(i));\n"
        "}\n",
        stdout_all=["body 0", "done 0", "done 1"],
    )
    check(
        "defer root rejected",
        "import std;\n"
        "defer std.io.print(\"root\");\n"
        "std.io.print(\"body\");\n",
        stdout="body",
        stderr="defer is only allowed inside a function or block",
    )
    check(
        "generator unstarted cleanup",
        "import std;\n"
        "func pending() { yield 1; }\n"
        "let g = pending();\n",
    )
    check(
        "std net resolve",
        'import std;\nstd.utils.len(std.net.resolve("localhost")) > 0\n',
        stdout="= true",
    )
    check(
        "std net resolve ipv4",
        'import std;\nstd.utils.len(std.net.resolve("localhost", 1000, "ipv4")) > 0\n',
        stdout="= true",
    )
    check(
        "std net resolve async",
        "import std;\n"
        'let t = std.net.resolve_task("localhost", 1000, "ipv4");\n'
        "std.utils.len(std.task.wait(t)) > 0\n",
        stdout="= true",
    )
    check(
        "std net resolve task",
        "import std;\n"
        'let t = std.net.resolve_task("localhost", 1000, "ipv4");\n'
        "std.utils.len(std.task.wait(t)) > 0\n",
        stdout="= true",
    )
    check(
        "std net concurrent resolve async",
        "import std;\n"
        'let a = std.net.resolve_task("localhost", 1000, "ipv4");\n'
        'let b = std.net.resolve_task("localhost", 1000, "ipv4");\n'
        "let values = std.task.wait_all([a, b]);\n"
        "std.utils.len(values) == 2 && std.utils.len(values[0]) > 0 && std.utils.len(values[1]) > 0\n",
        stdout="= true",
    )
    check(
        "std net async resolve cancel",
        "import std;\n"
        'let t = std.net.resolve_task("localhost", 5000);\n'
        "std.task.cancel(t)\n"
        "std.task.wait(t);\n",
        stdout="= true",
        stderr="task was cancelled",
    )

    check("task retry succeeds after failures",
          'import std;\n'
          'let n = 0;\n'
          'let r = std.task.retry(func() {\n'
          '  n = n + 1;\n'
          '  if (n < 3) { return std.result.err("boom"); }\n'
          '  return std.result.ok(n);\n'
          '}, {"attempts": 5, "delay_ms": 0});\n'
          'std.io.print(r);\nstd.io.print(n);\n',
          stdout_all=["ok(3)", "3"])
    check("task retry exhausts attempts",
          'import std;\n'
          'let n = 0;\n'
          'let r = std.task.retry(func() {\n'
          '  n = n + 1;\n'
          '  return std.result.err("boom");\n'
          '}, {"attempts": 3});\n'
          'std.io.print(n);\nstd.io.print(std.result.is_err(r));\n',
          stdout_all=["3", "true"])
    check("task retry filters error kinds",
          'import std;\n'
          'let n = 0;\n'
          'let r = std.task.retry(func() {\n'
          '  n = n + 1;\n'
          '  return std.result.err("boom");\n'
          '}, {"attempts": 5, "retry_on": "other"});\n'
          'std.io.print(n);\nstd.io.print(std.result.is_err(r));\n',
          stdout_all=["1", "true"])
    check("task retry rejects bad options",
          'import std;\n'
          'std.task.retry(func() { return 1; }, {"attempts": 0});\n',
          stderr="std.task.retry attempts must be at least 1")
    check("task timeout returns result",
          'import std;\n'
          'let r = std.task.timeout(std.task.sleep(100), 1);\n'
          'std.io.print(std.result.is_err(r));\n',
          stdout="true")
    check("task rate limit",
          'import std;\n'
          'let l = std.task.rate_limit(20, 1);\n'
          'let start = std.time.monotonic_ms();\n'
          'std.task.acquire(l);\n'
          'std.task.acquire(l);\n'
          'std.task.acquire(l);\n'
          'std.io.print(std.time.monotonic_ms() - start >= 90);\n',
          stdout="true")
    check("task bounded parallel map",
          'import std;\n'
          'func work(x) {\n'
          '  std.task.sleep(5).wait();\n'
          '  if (x == 2) { fail "bad"; }\n'
          '  return x * 2;\n'
          '}\n'
          'let r = std.task.bounded_parallel_map(\n'
          '  [1, 2, 3], 2, func(x) { return launch work(x); });\n'
          'std.io.print(std.utils.len(r));\n'
          'std.io.print(std.result.value(r[0]));\n'
          'std.io.print(std.result.is_err(r[1]));\n'
          'std.io.print(std.result.value(r[2]));\n',
          stdout_all=["3", "2", "true", "6"])
    check("task bounded parallel map limit",
          'import std;\n'
          'std.task.bounded_parallel_map([1], 0, func(x) { return x; });\n',
          stderr="std.task.bounded_parallel_map limit must be at least 1")
    check("wait rejects non-task value",
          'let v = wait 1;\n',
          stderr="wait expects a task, got int")
    check("wait rejects non-task call",
          'func f(): int { return 1; }\n'
          'let v = wait f();\n',
          stderr="wait expects a task, got int")
    check("wait is no longer reserved",
          'let wait = 1;\n'
          'std.io.println(wait);\n',
          stdout="1")
    check("wait may name a function",
          'func wait() { return 1; }\n'
          'std.io.println((wait)());\n',
          stdout="1")
    check("launch rejects wait expression",
          'func f() { return 1; }\n'
          'let t = launch wait f();\n',
          stderr="launch cannot be applied to a wait expression")
    check("member names may be keywords",
          'import std;\n'
          'func work(n) { return n + 1; }\n'
          'let t = launch work(1);\n'
          'std.io.print(t.wait());\n'
          'std.io.print(std.task.wait(t));\n'
          'std.io.print(std.regex.match("a", "a"));\n'
          'std.io.print(std.type.kind_of(std.process.wait));\n',
          stdout_all=["2", "2", "true", "builtin"])
