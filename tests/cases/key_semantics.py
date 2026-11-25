#!/usr/bin/env python3
"""Consolidated dict/set key semantics tests.

Non-thread cases live in tests/skein/key_semantics.skein.  Cross-thread method
and key transfer cases stay here because they need host-level thread control and
remain part of the TSan coverage.
"""
from harness import *  # noqa: F401,F403

import os
import tempfile

USER_KEYEQ = (
    "import std;\n"
    "import std.dict_key;\n"
    "struct User { id: int; cache: list[int]; }\n"
    "impl User {\n"
    "  func dict_hash(self): int { return self.id; }\n"
    "  func key_eq(self, other): bool { return self.id == other.id; }\n"
    "}\n"
)


def run_cases():
    run_skein("key_semantics.skein")

    check(
        "key scalar golden",
        "import std;\n"
        "let d = {1: 'i', true: 'b'};\n"
        "println(d[1]);\n"
        "println(d[true]);\n",
        stdout_all=["i", "b"],
    )

    with tempfile.TemporaryDirectory() as thread_tmp:
        worker = os.path.join(thread_tmp, "keyeq_worker.skein")
        with open(worker, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                "import std;\n"
                "func main(ctx) {\n"
                '  let d = ctx.payload["d"];\n'
                '  let k = ctx.payload["k"];\n'
                "  return d[k];\n"
                "}\n"
            )
        check(
            "key custom hash and KeyEq across threads",
            USER_KEYEQ +
            'let a = User { id: 1, cache: [] };\n'
            'let b = User { id: 1, cache: [1] };\n'
            'let d = {a: "x"};\n'
            f'let t = std.thread.spawn_script({skein_string(worker)}, '
            'std.thread.Message.from({"d": d, "k": b}));\n'
            "let r = t.join();\n"
            'println(str(r["ok"]));\n'
            'println(str(r["value"]));\n',
            stdout_all=["true", "x"],
        )

    with tempfile.TemporaryDirectory() as method_tmp:
        method_worker = os.path.join(method_tmp, "method_worker.skein")
        with open(method_worker, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                "import std;\n"
                "func main(ctx) {\n"
                '  let u = ctx.payload["u"];\n'
                "  return u.describe();\n"
                "}\n"
            )
        check(
            "struct method across script threads",
            "import std;\n"
            "struct User { id: int; }\n"
            "impl User {\n"
            "  func describe(self) { return \"user:\" + str(self.id); }\n"
            "}\n"
            f'let t = std.thread.spawn_script({skein_string(method_worker)}, '
            'std.thread.Message.from({"u": User { id: 7 }}));\n'
            "let r = t.join();\n"
            'println(str(r["ok"]));\n'
            'println(str(r["value"]));\n',
            stdout_all=["true", "user:7"],
        )

    check(
        "struct method across entry threads",
        "import std;\n"
        "struct User { id: int; }\n"
        "impl User {\n"
        "  func describe(self) { return \"user:\" + str(self.id); }\n"
        "}\n"
        "func worker(ctx) {\n"
        '  return ctx.payload["u"].describe();\n'
        "}\n"
        "let blob = std.entry.encode(worker);\n"
        'let t = std.thread.spawn_entry(blob, '
        'std.thread.Message.from({"u": User { id: 8 }}));\n'
        "let r = t.join();\n"
        'println(str(r["ok"]));\n'
        'println(str(r["value"]));\n',
        stdout_all=["true", "user:8"],
    )

    check(
        "parallel worker method attachment",
        "import std;\n"
        "struct User { id: int; }\n"
        "impl User {\n"
        "  func describe(self) { return \"user:\" + str(self.id); }\n"
        "}\n"
        "func worker(ctx) {\n"
        '  return ctx.payload["u"].describe();\n'
        "}\n"
        "let tasks = [];\n"
        "let i = 0;\n"
        "while (i < 8) {\n"
        "  let blob = std.entry.encode(worker);\n"
        '  let t = std.thread.spawn_entry(blob, '
        'std.thread.Message.from({"u": User { id: i }}));\n'
        "  tasks = std.list.append(tasks, t);\n"
        "  i = i + 1;\n"
        "}\n"
        "for (let t in tasks) {\n"
        "  let r = t.join();\n"
        '  println(str(r["value"]));\n'
        "}\n",
        stdout_all=[f"user:{i}" for i in range(8)],
    )

    check(
        "key custom hash and KeyEq across entry threads",
        USER_KEYEQ +
        'func worker(ctx) {\n'
        '  let d = ctx.payload["d"];\n'
        '  let k = ctx.payload["k"];\n'
        "  return d[k];\n"
        "}\n"
        'let a = User { id: 1, cache: [] };\n'
        'let b = User { id: 1, cache: [1] };\n'
        'let d = {a: "x"};\n'
        "let blob = std.entry.encode(worker);\n"
        'let t = std.thread.spawn_entry(blob, '
        'std.thread.Message.from({"d": d, "k": b}));\n'
        "let r = t.join();\n"
        'println(str(r["ok"]));\n'
        'println(str(r["value"]));\n',
        stdout_all=["true", "x"],
    )
