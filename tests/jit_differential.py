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
        "func risky(x) {\n"
        "  return (10 / x) on error(e) {\n"
        "    print(\"handler:\" + str(e) + \";\");\n"
        "    recover -1;\n"
        "  };\n"
        "}\n"
        "println(str(risky(2)));\n"
        "println(str(risky(0)));\n"
        "println(str(risky(5)));\n",
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
        "println(str(work(3)));\n"
        "println(str(wait launch work(10)));\n"
        "println(str(wait launch work(20)));\n",
        expect_debug="[jit] enter work",
    ),
    case(
        "gc-stress-jit-frames",
        "func make(n) {\n"
        "  let data = [n, n + 1, n + 2];\n"
        "  let extra = {\"k\": n};\n"
        "  return data[0] + data[1] + data[2] + extra[\"k\"];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 40) {\n"
        "  println(str(make(i)));\n"
        "  i = i + 1;\n"
        "}\n",
        extra_env={"SKEIN_GC_STRESS": "1"},
        expect_debug="[jit] enter make",
    ),
    case(
        "runtime-types-and-inline-caches",
        "func add(a, b) { return a + b; }\n"
        "func get(c, k) { return c[k]; }\n"
        "println(str(add(1, 2)));\n"
        'println(str(add("a", "b")));\n'
        "println(str(add(1.5, 2.5)));\n"
        'println(str(get({"x": 1}, "x")));\n'
        "println(str(get([10, 20], 1)));\n",
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
        "while (i <= 2) { println(lookup(i)); i = i + 1; }\n",
        expect_debug="[jit] enter lookup",
    ),
    case(
        "tuple-dict-keys",
        "func roundtrip(i) {\n"
        "  let d = {(1, 'a'): i, (2, 'b'): i + 1};\n"
        "  d[(3, 'c')] = i + 2;\n"
        "  return d[(1, 'a')] + d[(2, 'b')] + d[(3, 'c')];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { println(str(roundtrip(i))); i = i + 1; }\n",
        expect_debug="[jit] enter roundtrip",
    ),
    case(
        "struct-dict-keys",
        "struct Key { id: int; tag: string; }\n"
        "func roundtrip(i) {\n"
        '  let d = {Key { id: 1, tag: "a" }: i};\n'
        '  d[Key { id: 2, tag: "b" }] = i + 1;\n'
        '  return d[Key { id: 1, tag: "a" }] + '
        'd[Key { id: 2, tag: "b" }];\n'
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { println(str(roundtrip(i))); i = i + 1; }\n",
        expect_debug="[jit] enter roundtrip",
    ),
    case(
        "enum-dict-keys",
        "enum Key { A { id: int; }; B; }\n"
        "func roundtrip(i) {\n"
        "  let d = {Key.A { id: 1 }: i, Key.B: i + 1};\n"
        "  return d[Key.A { id: 1 }] + d[Key.B];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { println(str(roundtrip(i))); i = i + 1; }\n",
    ),
    case(
        "custom-struct-hash",
        "import std.dict_key;\n"
        "struct Key { id: int; cache: list[int]; }\n"
        "impl dict_key.DictKey for Key {\n"
        "  func dict_hash(self) { return self.id; }\n"
        "}\n"
        "func roundtrip(i) {\n"
        "  let d = {Key { id: 1, cache: [] }: i};\n"
        "  d[Key { id: 2, cache: [] }] = i + 1;\n"
        "  return d[Key { id: 1, cache: [] }] + "
        "d[Key { id: 2, cache: [] }];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { println(str(roundtrip(i))); i = i + 1; }\n",
        expect_debug="[jit] enter roundtrip",
    ),
    case(
        "custom-key-equality",
        "import std.dict_key;\n"
        "struct Key { id: int; cache: list[int]; }\n"
        "impl dict_key.DictKey for Key {\n"
        "  func dict_hash(self) { return self.id; }\n"
        "}\n"
        "impl dict_key.KeyEq for Key {\n"
        "  func key_eq(self, other) { return self.id == other.id; }\n"
        "}\n"
        "func roundtrip(i) {\n"
        "  let d = {Key { id: 1, cache: [] }: i};\n"
        "  d[Key { id: 2, cache: [] }] = i + 1;\n"
        "  return d[Key { id: 1, cache: [i] }] + "
        "d[Key { id: 2, cache: [i] }];\n"
        "}\n"
        "let i = 0;\n"
        "while (i < 20) { println(str(roundtrip(i))); i = i + 1; }\n",
        expect_debug="[jit] enter roundtrip",
    ),
    case(
        "globals-and-upvalues",
        "let g = 0;\n"
        "func bump() { g = g + 1; return g; }\n"
        "println(str(bump()));\n"
        "println(str(bump()));\n"
        "func outer(n) { return func(x) { return x + n; }; }\n"
        "let f = outer(10);\n"
        "println(str(f(5)));\n",
        expect_debug="[jit] enter bump",
    ),
    case(
        "defer",
        "func f(n) {\n"
        "  defer print(\"defer;\");\n"
        "  return n * 2;\n"
        "}\n"
        "println(str(f(1)));\n"
        "println(str(f(2)));\n",
        expect_debug="[jit] enter f",
    ),
    case(
        "generator-fallback",
        "import std;\n"
        "func gen() { yield 1; yield 2; }\n"
        "for (let x in gen()) { println(str(x)); }\n",
        expect_debug="suspendable gen",
    ),
    case(
        "pattern-params-fallback",
        "func pat([a, b]) { return a + b; }\n"
        "println(str(pat([1, 2])));\n"
        "println(str(pat([10, 20])));\n",
    ),
    case(
        "top-level-statements",
        "import std;\n"
        "func add(a, b) { return a + b; }\n"
        "let xs = [1, 2, 3];\n"
        "let total = 0;\n"
        "for (let x in xs) { total = total + add(x, 1); }\n"
        "println(str(total));\n"
        "let i = 0;\n"
        "while (i < 100) { total = total + add(i, 2); i = i + 1; }\n"
        "println(str(total));\n",
        expect_debug="[jit] top-level",
    ),
    case(
        "tight-loop",
        "func sum(n) {\n"
        "  let i = 0;\n"
        "  let total = 0;\n"
        "  while (i < n) { total = total + i; i = i + 1; }\n"
        "  return total;\n"
        "}\n"
        "println(str(sum(1000)));\n"
        "println(str(sum(2000)));\n",
        expect_debug="[jit] enter sum",
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
