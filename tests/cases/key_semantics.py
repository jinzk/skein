#!/usr/bin/env python3
"""Consolidated dict/set key semantics tests.

Covers automatic scalar/tuple/struct/enum keys, custom DictKey hashing,
optional KeyEq equality, and std.dict.index_by.
"""
from harness import *  # noqa: F401,F403

import os
import tempfile

USER_HASH = (
    "import std;\n"
    "import std.dict_key;\n"
    "struct User { id: int; cache: list[int]; }\n"
    "impl dict_key.DictKey for User {\n"
    "  func dict_hash(self) { return self.id; }\n"
    "}\n"
)

USER_KEYEQ = USER_HASH + (
    "impl dict_key.KeyEq for User {\n"
    "  func key_eq(self, other) { return self.id == other.id; }\n"
    "}\n"
)


def run_cases():
    check(
        "key scalar types",
        "import std;\n"
        "let d = {1: 'i', true: 'b', 's': 's', 'a': 'c', "
        "byte(65): 'y', std.bytes.from_hex(\"00\"): 'x', null: 'n'};\n"
        "println(d[1]); println(d[true]); println(d['s']); "
        "println(d['a']);\n"
        "println(d[byte(65)]); "
        "println(d[std.bytes.from_hex(\"00\")]); println(d[null]);\n",
        stdout_all=["i", "b", "s", "c", "y", "x", "n"],
    )

    check(
        "key tuple and nested tuple",
        "let d = {(1, 'a'): 10, ((2, 3), 'b'): 20};\n"
        "println(d[(1, 'a')]);\n"
        "println(d[((2, 3), 'b')]);\n",
        stdout_all=["10", "20"],
    )

    check(
        "key mutable list rejected",
        "{[1]: 1}\n",
        stderr="dict key must be",
    )

    check(
        "key tuple mutable element rejected",
        "{(1, [2]): 1}\n",
        stderr="dict key must be",
    )

    check(
        "key tuple is immutable",
        "let t = (1, 2);\nt[0] = 9;\n",
        stderr="cannot modify an immutable tuple",
    )

    check(
        "key struct snapshot",
        'import std;\n'
        'struct Key { id: int; tag: string; }\n'
        'let k = Key { id: 1, tag: "a" };\n'
        'let d = {k: 42};\n'
        'k.id = 2;\n'
        'println(d[Key { id: 1, tag: "a" }]);\n'
        'println(std.dict.has(d, k));\n',
        stdout_all=["42", "0"],
    )

    check(
        "key struct mutable field rejected",
        "struct Key { xs: list[int]; }\n"
        "{Key { xs: [1] }: 1}\n",
        stderr="dict key must be",
    )

    check(
        "key enum values",
        "enum Key { A; B { id: int; } }\n"
        "let d = {Key.A: 'a', Key.B { id: 1 }: 'b'};\n"
        "println(d[Key.A]);\n"
        "println(d[Key.B { id: 1 }]);\n",
        stdout_all=["a", "b"],
    )

    check(
        "key enum mutable payload rejected",
        "enum Key { A { xs: list[int]; } }\n"
        "{Key.A { xs: [1] }: 1}\n",
        stderr="dict key must be",
    )

    check(
        "key custom hash ignores non-hashable fields",
        USER_HASH +
        'let u = User { id: 1, cache: [] };\n'
        'let d = {u: "x"};\n'
        'println(d[User { id: 1, cache: [] }]);\n'
        'println(std.dict.has(d, User { id: 1, cache: [1] }));\n'
        'let s = std.set.from_list([User { id: 1, cache: [] }, '
        'User { id: 1, cache: [1] }]);\n'
        'println(std.set.size(s));\n',
        stdout_all=["x", "0", "2"],
    )

    check(
        "key hash must return int",
        "import std.dict_key;\n"
        "struct Bad { id: int; }\n"
        "impl dict_key.DictKey for Bad {\n"
        "  func dict_hash(self) { return \"x\"; }\n"
        "}\n"
        "{Bad { id: 1 }: 1}\n",
        stderr="dict_hash must return an int",
    )

    check(
        "key KeyEq custom equality",
        USER_KEYEQ +
        'let a = User { id: 1, cache: [] };\n'
        'let b = User { id: 1, cache: [1] };\n'
        'let d = {a: "x"};\n'
        'println(d[b]);\n'
        'println(std.dict.has(d, b));\n'
        'println(a == b);\n'
        'let s = std.set.from_list([a, b]);\n'
        'println(std.set.size(s));\n',
        stdout_all=["x", "1", "false", "1"],
    )

    check(
        "key hash without KeyEq stays structural",
        USER_HASH +
        'let a = User { id: 1, cache: [] };\n'
        'let b = User { id: 1, cache: [1] };\n'
        'let d = {a: "x"};\n'
        'println(std.dict.has(d, b));\n',
        stdout_all=["0"],
    )

    check(
        "key KeyEq nested in tuple",
        USER_KEYEQ +
        'let d = {(User { id: 1, cache: [] }, "x"): true};\n'
        'println(d[(User { id: 1, cache: [1] }, "x")]);\n',
        stdout_all=["true"],
    )

    check(
        "key KeyEq nested in auto struct",
        USER_KEYEQ +
        "struct Wrapper { user: User; }\n"
        "let d = {Wrapper { user: User { id: 1, cache: [] } }: true};\n"
        "println(d[Wrapper { user: User { id: 1, cache: [1] } }]);\n",
        stdout_all=["true"],
    )

    check(
        "key KeyEq must return bool",
        "import std.dict_key;\n"
        "struct Bad { id: int; }\n"
        "impl dict_key.DictKey for Bad {\n"
        "  func dict_hash(self) { return self.id; }\n"
        "}\n"
        "impl dict_key.KeyEq for Bad {\n"
        "  func key_eq(self, other) { return 1; }\n"
        "}\n"
        "let d = {Bad { id: 1 }: 1};\n"
        "println(d[Bad { id: 1 }]);\n",
        stderr="key_eq must return a bool",
    )

    check(
        "key KeyEq reentrancy rejected",
        "import std.dict_key;\n"
        "struct A { id: int; }\n"
        "struct B { id: int; }\n"
        "impl dict_key.DictKey for A {\n"
        "  func dict_hash(self) { return self.id; }\n"
        "}\n"
        "impl dict_key.KeyEq for A {\n"
        "  func key_eq(self, other) {\n"
        "    let m = {B { id: 1 }: 1};\n"
        "    let n = {B { id: 1 }: 1};\n"
        "    return m[B { id: 1 }] == 1;\n"
        "  }\n"
        "}\n"
        "impl dict_key.DictKey for B {\n"
        "  func dict_hash(self) { return self.id; }\n"
        "}\n"
        "impl dict_key.KeyEq for B {\n"
        "  func key_eq(self, other) { return self.id == other.id; }\n"
        "}\n"
        "let d = {A { id: 1 }: 1};\n"
        "println(d[A { id: 1 }]);\n",
        stderr="key_eq must not re-enter dict/set key equality",
    )

    check(
        "key index_by",
        'import std;\n'
        'struct User { id: int; name: string; }\n'
        'let users = [User { id: 1, name: "a" }, '
        'User { id: 2, name: "b" }];\n'
        'let by_id = std.dict.index_by(users, func(u) { return u.id; });\n'
        'println(by_id[1].name);\n'
        'println(by_id[2].name);\n'
        'println(std.dict.keys(by_id));\n',
        stdout_all=["a", "b", "[1, 2]"],
    )

    check(
        "key index_by duplicate last wins",
        'import std;\n'
        'struct User { id: int; name: string; }\n'
        'let users = [User { id: 1, name: "a" }, '
        'User { id: 1, name: "b" }];\n'
        'let by_id = std.dict.index_by(users, func(u) { return u.id; });\n'
        'println(by_id[1].name);\n'
        'println(len(by_id));\n',
        stdout_all=["b", "1"],
    )

    check(
        "key index_by composite projection",
        'import std;\n'
        'let xs = [{"region": "eu", "id": 1}, '
        '{"region": "us", "id": 1}];\n'
        'let by_key = std.dict.index_by(xs, '
        'func(x) { return (x["region"], x["id"]); });\n'
        'println(by_key[("eu", 1)]["region"]);\n'
        'println(by_key[("us", 1)]["region"]);\n',
        stdout_all=["eu", "us"],
    )

    check(
        "key index_by invalid key",
        'import std;\n'
        'std.dict.index_by([1], func(x) { return 1.5; });\n',
        stderr="std.dict.index_by: dict key must be",
    )

    check(
        "key custom hash and KeyEq inside task",
        USER_KEYEQ +
        'func worker() {\n'
        '  let d = {User { id: 1, cache: [] }: "x"};\n'
        '  return d[User { id: 1, cache: [1] }];\n'
        '}\n'
        'println(launch worker().wait());\n',
        stdout_all=["x"],
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

    check(
        "key custom hash and KeyEq across entry threads",
        USER_KEYEQ +
        'func worker(ctx) {\n'
        '  let d = ctx.payload["d"];\n'
        '  let k = ctx.payload["k"];\n'
        '  return d[k];\n'
        '}\n'
        'let a = User { id: 1, cache: [] };\n'
        'let b = User { id: 1, cache: [1] };\n'
        'let d = {a: "x"};\n'
        'let blob = std.entry.encode(worker);\n'
        'let t = std.thread.spawn_entry(blob, '
        'std.thread.Message.from({"d": d, "k": b}));\n'
        "let r = t.join();\n"
        'println(str(r["ok"]));\n'
        'println(str(r["value"]));\n',
        stdout_all=["true", "x"],
    )
