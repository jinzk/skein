#!/usr/bin/env python3
"""Expanded JIT differential matrix.

For each curated program, run the VM backend with the JIT disabled and with
the JIT forced on, then compare exit status, stdout and stderr.  This covers
control flow, tasks, GC roots and runtime assumptions that the smaller
``jit_smoke`` opcode subset does not reach.

Every case is deterministic: no fuzzing, no network, no fixed ports.  A case
may additionally assert that the JIT accepted or rejected its hot function,
so a test cannot pass merely because the JIT never engaged.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT = float(os.environ.get("SKEIN_TEST_TIMEOUT", "60"))


def case(name, source, extra_env=None, expect_debug=None):
    return {
        "name": name,
        "source": source,
        "extra_env": extra_env or {},
        "expect_debug": expect_debug,
    }


CASES = [
    case(
        "handlers",
        "import std;\nfunc risky(x) {\n"
        "  return (10 / x) on error(e) {\n"
        "    std.io.print(\"handler:\" + str(e) + \";\");\n"
        "    recover -1;\n"
        "  };\n"
        "}\n"
        "std.io.println(str(risky(2)));\n"
        "std.io.println(str(risky(0)));\n"
        "std.io.println(str(risky(5)));\n",
        expect_debug="[jit] enter risky",
    ),
    case(
        "p5-handler-contextual-names",
        "import std;\n"
        "struct Box { on: int; error: int; }\n"
        "let on = 1;\n"
        "let error = 2;\n"
        "let b = Box { on: on, error: error };\n"
        "func risky(x) {\n"
        "  return (10 / x) on error(e) { recover on + error; };\n"
        "}\n"
        "std.io.println(str(b.on + b.error));\n"
        "std.io.println(str(risky(2)));\n"
        "std.io.println(str(risky(0)));\n"
        "macro error(x: expr) { #x + 100; }\n"
        "std.io.println(str(error!(1)));\n",
        expect_debug="[jit] enter risky",
    ),
    case(
        "detached-task-fail",
        "import std;\n"
        "func bad() {\n"
        "  std.task.sleep(1).wait();\n"
        '  fail "boom";\n'
        "}\n"
        "func parent() {\n"
        "  let t = launch bad();\n"
        "  t.detach();\n"
        "  std.task.sleep(30).wait();\n"
        "}\n"
        "parent();\n",
        expect_debug="cannot compile parent",
    ),
    case(
        "task-wait-and-suspend",
        "import std;\n"
        "func work(n) {\n"
        "  let data = [n, n + 1, n + 2];\n"
        "  std.task.sleep(1).wait();\n"
        "  return data[0] + data[1] + data[2];\n"
        "}\n"
        "std.io.println(str(work(3)));\n"
        "std.io.println(str(wait launch work(10)));\n"
        "std.io.println(str(wait launch work(20)));\n",
        expect_debug="[jit] enter work",
    ),
    case(
        "gc-stress-jit-frames",
        "import std;\nfunc make(n) {\n"
        "  let data = [n, n + 1, n + 2];\n"
        "  let extra = {\"k\": n};\n"
        "  return data[0] + data[1] + data[2] + extra[\"k\"];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 40) {\n"
        "  std.io.println(str(make(i)));\n"
        "  i = i + 1;\n"
        "}\n",
        extra_env={"SKEIN_GC_STRESS": "1"},
        expect_debug="[jit] enter make",
    ),
    case(
        "selective-import",
        "import std;\nimport {basename} from std.path;\n"
        "func selected(path) { return basename(path); }\n"
        'std.io.println(selected("/a/b.txt"));\n',
        expect_debug="[jit] enter selected",
    ),
    case(
        "runtime-types-and-inline-caches",
        "import std;\nfunc add(a, b) { return a + b; }\n"
        "func get(c, k) { return c[k]; }\n"
        "std.io.println(str(add(1, 2)));\n"
        'std.io.println(str(add("a", "b")));\n'
        "std.io.println(str(add(1.5, 2.5)));\n"
        'std.io.println(str(get({"x": 1}, "x")));\n'
        "std.io.println(str(get([10, 20], 1)));\n",
        expect_debug="[jit] enter add",
    ),
    case(
        "dict-index-by",
        "import std;\n"
        "struct User { id: int; name: string; }\n"
        "func lookup(i) {\n"
        '  let xs = [User { id: 1, name: "a" }, '
        'User { id: 2, name: "b" }];\n'
        "  let d = std.dict.index_by(xs, func(u) { return u.id; });\n"
        "  return d[i].name;\n"
        "}\n"
        "let i = 1;\n"
        "while (i <= 2) { std.io.println(lookup(i)); i = i + 1; }\n",
        expect_debug="[jit] enter lookup",
    ),
    case(
        "tuple-dict-keys",
        "import std;\nfunc roundtrip(i) {\n"
        "  let d = {(1, 'a'): i, (2, 'b'): i + 1};\n"
        "  d[(3, 'c')] = i + 2;\n"
        "  return d[(1, 'a')] + d[(2, 'b')] + d[(3, 'c')];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { std.io.println(str(roundtrip(i))); i = i + 1; }\n",
        expect_debug="[jit] enter roundtrip",
    ),
    case(
        "struct-dict-keys",
        "import std;\nstruct Key { id: int; tag: string; }\n"
        "func roundtrip(i) {\n"
        '  let d = {Key { id: 1, tag: "a" }: i};\n'
        '  d[Key { id: 2, tag: "b" }] = i + 1;\n'
        '  return d[Key { id: 1, tag: "a" }] + '
        'd[Key { id: 2, tag: "b" }];\n'
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { std.io.println(str(roundtrip(i))); i = i + 1; }\n",
        expect_debug="[jit] enter roundtrip",
    ),
    case(
        "enum-dict-keys",
        "import std;\nenum Key { A { id: int; }; B; }\n"
        "func roundtrip(i) {\n"
        "  let d = {Key.A { id: 1 }: i, Key.B: i + 1};\n"
        "  return d[Key.A { id: 1 }] + d[Key.B];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { std.io.println(str(roundtrip(i))); i = i + 1; }\n",
    ),
    case(
        "custom-struct-hash",
        "import std;\nimport std.dict_key;\n"
        "struct Key { id: int; cache: list[int]; }\n"
        "impl Key {\n"
        "  func dict_hash(self): int { return self.id; }\n"
        "}\n"
        "func roundtrip(i) {\n"
        "  let d = {Key { id: 1, cache: [] }: i};\n"
        "  d[Key { id: 2, cache: [] }] = i + 1;\n"
        "  return d[Key { id: 1, cache: [] }] + "
        "d[Key { id: 2, cache: [] }];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { std.io.println(str(roundtrip(i))); i = i + 1; }\n",
        expect_debug="[jit] enter roundtrip",
    ),
    case(
        "custom-key-equality",
        "import std;\nimport std.dict_key;\n"
        "struct Key { id: int; cache: list[int]; }\n"
        "impl Key {\n"
        "  func dict_hash(self): int { return self.id; }\n"
        "}\n"
        "impl Key {\n"
        "  func key_eq(self, other): bool { return self.id == other.id; }\n"
        "}\n"
        "func roundtrip(i) {\n"
        "  let d = {Key { id: 1, cache: [] }: i};\n"
        "  d[Key { id: 2, cache: [] }] = i + 1;\n"
        "  return d[Key { id: 1, cache: [i] }] + "
        "d[Key { id: 2, cache: [i] }];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { std.io.println(str(roundtrip(i))); i = i + 1; }\n",
        expect_debug="[jit] enter roundtrip",
    ),
    case(
        "structural-protocol-dispatch",
        "import std;\nproto Doubler {\n"
        "  func value(self) -> int;\n"
        "  func double(self) { return self.value() * 2; }\n"
        "}\n"
        "struct Num { n: int; }\n"
        "impl Num { func value(self): int { return self.n; } }\n"
        "func roundtrip(x: Doubler) { return x.double(); }\n"
        "let i = 0;\n"
        "while (i < 20) { std.io.println(str(roundtrip(Num { n: i }))); i = i + 1; }\n",
    ),
    case(
        "inline-struct-method-dispatch",
        "import std;\nstruct Counter { n: int; func bump(self): int { return self.n + 1; } }\n"
        "impl Counter { func twice_bumped(self) -> self.bump() * 2; }\n"
        "func roundtrip(i) {\n"
        "  let c = Counter { n: i };\n"
        "  return c.bump() + c.twice_bumped();\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { std.io.println(str(roundtrip(i))); i = i + 1; }\n",
        expect_debug="[jit] enter roundtrip",
    ),
    case(
        "structural-protocol-thread-transfer",
        "import std;\n"
        "proto Doubler {\n"
        "  func value(self) -> int;\n"
        "  func double(self) { return self.value() * 2; }\n"
        "}\n"
        "struct Num { n: int; }\n"
        "impl Num { func value(self): int { return self.n; } }\n"
        "func work(x: Doubler) {\n"
        "  std.task.sleep(1).wait();\n"
        "  return x.double();\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) {\n"
        "  std.io.println(str(wait launch work(Num { n: i })));\n"
        "  i = i + 1;\n"
        "}\n",
    ),
    case(
        "globals-and-upvalues",
        "import std;\nlet g = 0;\n"
        "func bump() { g = g + 1; return g; }\n"
        "std.io.println(str(bump()));\n"
        "std.io.println(str(bump()));\n"
        "func outer(n) { return func(x) { return x + n; }; }\n"
        "let f = outer(10);\n"
        "std.io.println(str(f(5)));\n",
        expect_debug="[jit] enter bump",
    ),
    case(
        "defer",
        "import std;\nfunc f(n) {\n"
        "  defer std.io.print(\"defer;\");\n"
        "  return n * 2;\n"
        "}\n"
        "std.io.println(str(f(1)));\n"
        "std.io.println(str(f(2)));\n",
        expect_debug="[jit] enter f",
    ),
    case(
        "generator-fallback",
        "import std;\n"
        "func gen() { yield 1; yield 2; }\n"
        "for (let x in gen()) { std.io.println(str(x)); }\n",
        expect_debug="suspendable gen",
    ),
    case(
        "pattern-params-fallback",
        "import std;\nfunc pat([a, b]) { return a + b; }\n"
        "std.io.println(str(pat([1, 2])));\n"
        "std.io.println(str(pat([10, 20])));\n",
    ),
    case(
        "top-level-statements",
        "import std;\n"
        "func add(a, b) { return a + b; }\n"
        "let xs = [1, 2, 3];\n"
        "let total = 0;\n"
        "for (let x in xs) { total = total + add(x, 1); }\n"
        "std.io.println(str(total));\n"
        "let i = 0;\n"
        "while (i < 100) { total = total + add(i, 2); i = i + 1; }\n"
        "std.io.println(str(total));\n",
        expect_debug="[jit] top-level",
    ),
    case(
        "tight-loop",
        "import std;\nfunc sum(n) {\n"
        "  let i = 0;\n"
        "  let total = 0;\n"
        "  while (i < n) { total = total + i; i = i + 1; }\n"
        "  return total;\n"
        "}\n"
        "std.io.println(str(sum(1000)));\n"
        "std.io.println(str(sum(2000)));\n",
        expect_debug="[jit] enter sum",
    ),
    case(
        "macro-type-fragment-annotation",
        "import std;\nmacro pick(T: type, v: expr) { let x: #T = #v; return x; }\n"
        "func hot(n) {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < n) { total = total + pick!(int, i); i = i + 1; }\n"
        "  return total;\n"
        "}\n"
        "std.io.println(str(hot(100)));\n",
        expect_debug="[jit] enter hot",
    ),
    case(
        "macro-type-fragment-wrappers",
        "import std;\nmacro wrappers(T: type) {\n"
        "  let r: ref #T? = null;\n"
        "  let w: weak #T? = null;\n"
        "  return 1;\n"
        "}\n"
        "func hot(n) {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < n) { total = total + wrappers!(int); i = i + 1; }\n"
        "  return total;\n"
        "}\n"
        "std.io.println(str(hot(100)));\n",
        expect_debug="[jit] enter hot",
    ),
    case(
        "macro-type-fragment-cast-is",
        "import std;\nmacro cast_it(T: type, v: expr) { let x = #v as #T; return x; }\n"
        "macro is_it(T: type, v: expr) { return #v is #T; }\n"
        "func hot(n) {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < n) {\n"
        "    let x = cast_it!(int, i);\n"
        "    if is_it!(int, x) { total = total + x; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return total;\n"
        "}\n"
        "std.io.println(str(hot(100)));\n",
        expect_debug="[jit] enter hot",
    ),
    case(
        "macro-literal-generic-application",
        "func id[T any](x: T): T { return x; }\n"
        "struct Box[T any] { value: T; }\n"
        "macro call(v: expr) {\n"
        "  let b = Box[int] { value: id[int](#v) };\n"
        "  return b.value;\n"
        "}\n"
        "func hot(n) {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < n) { total = total + call!(i); i = i + 1; }\n"
        "  return total;\n"
        "}\n"
        "println(str(hot(100)));\n",
        expect_debug="[jit] enter hot",
    ),
    case(
        "macro-generic-type-fragment-application",
        "func id[T any](x: T): T { return x; }\n"
        "struct Box[T any] { value: T; }\n"
        "macro call(T: type, v: expr) {\n"
        "  let b = Box[#T] { value: id[#T](#v) };\n"
        "  return b.value;\n"
        "}\n"
        "func hot(n) {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < n) { total = total + call!(int, i); i = i + 1; }\n"
        "  return total;\n"
        "}\n"
        "println(str(hot(100)));\n",
        expect_debug="[jit] enter hot",
    ),
    case(
        "range-forin-fast-path",
        "import std;\n"
        "func hot(n) {\n"
        "  let total = 0;\n"
        "  for i in std.utils.range(1000000000) {\n"
        "    total = total + i;\n"
        "    if (i == 2) { break; }\n"
        "  }\n"
        "  for j in std.utils.range(5, 3, -2) {\n"
        "    total = total + j;\n"
        "  }\n"
        "  return total + n;\n"
        "}\n"
        "std.io.println(str(hot(1)));\n",
        expect_debug="[jit] enter hot",
    ),
]


def run(binary, script, jit, extra_env, debug=False):
    env = dict(os.environ)
    env["SKEIN_VM"] = "1"
    env["SKEIN_JIT"] = "1" if jit else "0"
    env["SKEIN_JIT_THRESHOLD"] = "0"
    env.update(extra_env)
    if debug:
        env["SKEIN_JIT_DEBUG"] = "1"
    return subprocess.run(
        [str(binary), str(script)],
        env=env,
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=TIMEOUT,
    )


def outcome(proc):
    return proc.returncode, proc.stdout, proc.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default="./skein")
    args = parser.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        print(f"missing binary: {binary}", file=sys.stderr)
        return 2

    ok = True
    with tempfile.TemporaryDirectory() as temporary:
        for entry in CASES:
            script = Path(temporary) / (entry["name"] + ".skein")
            script.write_text(entry["source"], encoding="utf-8")

            baseline = run(binary, script, False, entry["extra_env"])
            jit = run(binary, script, True, entry["extra_env"])
            if outcome(baseline) != outcome(jit):
                print(f"jit differential: {entry['name']} mismatch",
                      file=sys.stderr)
                print(f"--- source ---\n{entry['source']}", file=sys.stderr)
                print(f"  vm : {outcome(baseline)!r}", file=sys.stderr)
                print(f"  jit: {outcome(jit)!r}", file=sys.stderr)
                ok = False
                continue

            expected = entry["expect_debug"]
            if expected:
                debug = run(binary, script, True, entry["extra_env"],
                            debug=True)
                if expected not in debug.stderr:
                    print(f"jit differential: {entry['name']} did not "
                          f"produce {expected!r}", file=sys.stderr)
                    print(f"  stderr: {debug.stderr!r}", file=sys.stderr)
                    ok = False

    if not ok:
        return 1
    print(f"jit differential: {len(CASES)} cases matched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
