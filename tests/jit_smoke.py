#!/usr/bin/env python3
"""Smoke test for the experimental JIT backend.

Runs the same programs with SKEIN_JIT=0 and SKEIN_JIT=1 and compares stdout,
stderr and exit status.  It only covers the small opcode subset implemented by
the spike (fib and simple int branches), so it is intentionally independent of
the full regression suite.
"""

import argparse
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(binary, script, jit, threshold="1", debug=False):
    env = dict(os.environ)
    env["SKEIN_VM"] = "1"
    env["SKEIN_JIT"] = "1" if jit else "0"
    # The JIT compiles after `jit_hits >= threshold`; 0 would disable it
    # entirely, so use 1 to exercise code generation on the second call.
    env["SKEIN_JIT_THRESHOLD"] = threshold
    if debug:
        env["SKEIN_JIT_DEBUG"] = "1"
    proc = subprocess.run(
        [str(binary), str(script)],
        env=env,
        text=True,
        capture_output=True,
        timeout=120,
    )
    return proc.returncode, proc.stdout, proc.stderr


def normalize_output(text):
    """Drop the self-reported timing line so VM/JIT outputs stay comparable."""
    return "\n".join(
        line for line in text.splitlines()
        if not line.startswith("SKEIN_BENCH_MS=")
    )


def compare(binary, label, script, threshold="1"):
    vm = run(binary, script, False, threshold=threshold)
    jit = run(binary, script, True, threshold=threshold)
    normalized = (
        vm[0], normalize_output(vm[1]), vm[2],
    )
    if normalized != (jit[0], normalize_output(jit[1]), jit[2]):
        print(f"jit smoke: {label} mismatch", file=sys.stderr)
        print(f"  vm : {vm!r}", file=sys.stderr)
        print(f"  jit: {jit!r}", file=sys.stderr)
        return False
    return True


def check_jit_interrupt(binary, script, fn_name):
    """A JIT-compiled tight loop must react to SIGINT like the VM does."""
    if sys.platform == "win32":
        return True
    env = dict(os.environ)
    env["SKEIN_VM"] = "1"
    env["SKEIN_JIT"] = "1"
    env["SKEIN_JIT_THRESHOLD"] = "0"
    env["SKEIN_JIT_DEBUG"] = "1"
    proc = subprocess.Popen(
        [str(binary), str(script)],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    time.sleep(0.5)
    if proc.poll() is not None:
        out, err = proc.communicate()
        print("jit smoke: interrupt loop exited before SIGINT", file=sys.stderr)
        print(f"  rc={proc.returncode} out={out!r} err={err!r}",
              file=sys.stderr)
        return False
    proc.send_signal(signal.SIGINT)
    try:
        out, err = proc.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        print("jit smoke: JIT loop did not react to SIGINT", file=sys.stderr)
        print(f"  out={out!r} err={err!r}", file=sys.stderr)
        return False
    if proc.returncode != 130 or f"[jit] enter {fn_name}" not in err:
        print("jit smoke: interrupt result mismatch", file=sys.stderr)
        print(f"  rc={proc.returncode} out={out!r} err={err!r}",
              file=sys.stderr)
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="./build/skein")
    args = parser.parse_args()

    binary = Path(args.bin)
    if not binary.exists():
        print(f"missing binary: {binary}", file=sys.stderr)
        return 2

    ok = True
    ok &= compare(binary, "fib-bench", ROOT / "benchmarks" / "fib_bench.skein")
    ok &= compare(binary, "slot-bench", ROOT / "benchmarks" / "slot_bench.skein")
    ok &= compare(binary, "scope-bench", ROOT / "benchmarks" / "scope_bench.skein")
    ok &= compare(binary, "global-bench",
                  ROOT / "benchmarks" / "global_bench.skein")
    ok &= compare(binary, "field-bench",
                  ROOT / "benchmarks" / "field_bench.skein")
    ok &= compare(binary, "index-bench",
                  ROOT / "benchmarks" / "index_bench.skein")
    ok &= compare(binary, "index-write-bench",
                  ROOT / "benchmarks" / "index_write_bench.skein")
    ok &= compare(binary, "method-bench",
                  ROOT / "benchmarks" / "method_bench.skein")
    ok &= compare(binary, "call-bench",
                  ROOT / "benchmarks" / "call_bench.skein")
    ok &= compare(binary, "pattern-bench",
                  ROOT / "benchmarks" / "pattern_bench.skein")

    with tempfile.TemporaryDirectory(prefix="skein-jit-") as tmp:
        tmp_path = Path(tmp)

        values = tmp_path / "values.skein"
        values.write_text(
            "func fib(n) {\n"
            "  if (n < 2) { return n; }\n"
            "  return fib(n - 1) + fib(n - 2);\n"
            "}\n"
            "let i = 0;\n"
            "while (i <= 20) {\n"
            "  println(fib(i));\n"
            "  i = i + 1;\n"
            "}\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "fib-values", values)

        bad = tmp_path / "bad.skein"
        bad.write_text(
            "func fib(n) {\n"
            "  if (n < 2) { return n; }\n"
            "  return fib(n - 1) + fib(n - 2);\n"
            "}\n"
            'fib("x");\n',
            encoding="utf-8",
        )
        ok &= compare(binary, "fib-type-error", bad)

        slot_error = tmp_path / "slot_error.skein"
        slot_error.write_text(
            "func f(n) {\n"
            "  let x: int = n;\n"
            '  x = "s";\n'
            "  return x;\n"
            "}\n"
            "f(1);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "slot-type-error", slot_error)

        const_error = tmp_path / "const_error.skein"
        const_error.write_text(
            "func f(n) {\n"
            "  const x = n;\n"
            "  x = 2;\n"
            "  return x;\n"
            "}\n"
            "f(1);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "const-error", const_error)

        field_error = tmp_path / "field_error.skein"
        field_error.write_text(
            "struct Point { x: int; y: int; }\n"
            "func f(n) {\n"
            "  let p = Point { x: n, y: 2 };\n"
            "  return p.z;\n"
            "}\n"
            "f(1);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "field-error", field_error)

        index_error = tmp_path / "index_error.skein"
        index_error.write_text(
            'let mapping = {"key": 1};\n'
            "func f(n) {\n"
            '  return mapping["missing"];\n'
            "}\n"
            "f(1);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "index-error", index_error)

        method_error = tmp_path / "method_error.skein"
        method_error.write_text(
            "struct Counter { n: int; }\n"
            "impl Counter {\n"
            "  func add(self, d) { return self.n + d; }\n"
            "}\n"
            "func f(n) {\n"
            "  let c = Counter { n: n };\n"
            '  return c.add("x");\n'
            "}\n"
            "f(1);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "method-error", method_error)

        top_error = tmp_path / "top_error.skein"
        top_error.write_text(
            "let total = 0;\n"
            "let i = 0;\n"
            "while (i < 2) {\n"
            '  total = total + "x";\n'
            "  i = i + 1;\n"
            "}\n"
            "println(total);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "top-level-error", top_error)

        tail = tmp_path / "tail.skein"
        tail.write_text(
            "func tail(n, acc) {\n"
            "  if (n <= 0) { return acc; }\n"
            "  return tail(n - 1, acc + 1);\n"
            "}\n"
            "println(tail(1000, 0));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "tail-recursion", tail)

        tail_deep = tmp_path / "tail_deep.skein"
        tail_deep.write_text(
            "func tail(n, acc) {\n"
            "  if (n <= 0) { return acc; }\n"
            "  return tail(n - 1, acc + 1);\n"
            "}\n"
            "println(tail(100000, 0));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "tail-deep-recursion", tail_deep)

        mutual_tail = tmp_path / "mutual_tail.skein"
        mutual_tail.write_text(
            "func even(n) {\n"
            "  if (n == 0) { return true; }\n"
            "  return odd(n - 1);\n"
            "}\n"
            "func odd(n) {\n"
            "  if (n == 0) { return false; }\n"
            "  return even(n - 1);\n"
            "}\n"
            "println(even(100000));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "mutual-tail-recursion", mutual_tail)

        tail_nonjit = tmp_path / "tail_nonjit.skein"
        tail_nonjit.write_text(
            "func target(x) {\n"
            "  let l = [1, 2, 3];\n"
            "  let s = 0;\n"
            "  for (let v in l) { s = s + v; }\n"
            "  return s + x;\n"
            "}\n"
            "func tail(n) {\n"
            "  if (n <= 0) { return 0; }\n"
            "  return target(n);\n"
            "}\n"
            "println(tail(10));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "tail-call-non-jit-target", tail_nonjit)

        dict_error = tmp_path / "dict_error.skein"
        dict_error.write_text(
            "func f() {\n"
            "  return {[1]: 2};\n"
            "}\n"
            "f();\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "dict-key-error", dict_error)

        ops = tmp_path / "ops.skein"
        ops.write_text(
            "func calc(n) {\n"
            "  let x = n; x *= 3; x += 4; x -= 2; x /= 2;\n"
            "  let y = x % 5;\n"
            "  let z = n << 2; z = z | 1; z = z ^ 3; z = z & 7;\n"
            "  let a = n; a++; ++a;\n"
            "  return x + y + z + a;\n"
            "}\n"
            "println(calc(7));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "compound-and-bitops", ops)

        compound_const = tmp_path / "compound_const.skein"
        compound_const.write_text(
            "func f(n) {\n"
            "  const x = n;\n"
            "  x += 1;\n"
            "  return x;\n"
            "}\n"
            "f(1);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "compound-const-error", compound_const)

        global_ops = tmp_path / "global_ops.skein"
        global_ops.write_text(
            "let g = 1;\n"
            "func f(n) {\n"
            "  g += n; g++; ++g;\n"
            "  let x = -n;\n"
            "  let y = !false;\n"
            "  let z = ~n;\n"
            "  if (y) { return g + x + z; }\n"
            "  return 0;\n"
            "}\n"
            "println(f(3));\n"
            "println(g);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "global-compound-unary", global_ops)

        global_const = tmp_path / "global_const.skein"
        global_const.write_text(
            "const g = 1;\n"
            "func f() { g += 1; return g; }\n"
            "f();\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "global-const-error", global_const)

        pattern_error = tmp_path / "pattern_error.skein"
        pattern_error.write_text(
            "func add([a, b]) { return a + b; }\n"
            "println(add([1, 2]));\n"
            "add([1]);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "pattern-list-error", pattern_error)

        pattern_dict_error = tmp_path / "pattern_dict_error.skein"
        pattern_dict_error.write_text(
            "func pair({x, y}) { return x + y; }\n"
            'println(pair({"x": 1, "y": 2}));\n'
            'pair({"x": 1});\n',
            encoding="utf-8",
        )
        ok &= compare(binary, "pattern-dict-error", pattern_dict_error)

        suspend_gen = tmp_path / "suspend_gen.skein"
        suspend_gen.write_text(
            "import std;\n"
            "func work(n) { yield n + 1; }\n"
            'println(std.generator.next(work(1))["value"]);\n'
            'println(std.generator.next(work(2))["value"]);\n'
            "func gen() { yield 1; yield 2; }\n"
            "for (let x in gen()) { println(x); }\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "suspend-generator", suspend_gen)
        rc, out, err = run(binary, suspend_gen, True, debug=True)
        if rc != 0 or "[jit] suspendable work" not in err:
            print(
                "jit smoke: suspendable function was not rejected",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # Closures read/write captured locals through upvalue cells.  Only the
        # inner closure is JIT-compiled; the outer function still creates it.
        closure_ops = tmp_path / "closure_ops.skein"
        closure_ops.write_text(
            "func make_counter(start) {\n"
            "  let n = start;\n"
            "  func step() {\n"
            "    let before = n;\n"
            "    let a = n++;\n"
            "    let b = ++n;\n"
            "    n += 1;\n"
            "    return before * 10000 + a * 100 + b + n;\n"
            "  }\n"
            "  return step;\n"
            "}\n"
            "let c = make_counter(0);\n"
            "let i = 0;\n"
            "while (i < 300) {\n"
            "  let r = c();\n"
            "  if (i == 0) { println(r); }\n"
            "  i = i + 1;\n"
            "}\n"
            "println(str(i));\n"
            "func make_tail(base) {\n"
            "  func step(n) {\n"
            "    if (n <= 0) { return base; }\n"
            "    return step(n - 1);\n"
            "  }\n"
            "  return step;\n"
            "}\n"
            "println(str(make_tail(7)(100000)));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "closure-upvalues", closure_ops)
        rc, out, err = run(binary, closure_ops, True, debug=True)
        if rc != 0 or "[jit] enter step" not in err:
            print(
                "jit smoke: closure/upvalue function was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # Closure creation inside a JIT frame: params, slots and inherited
        # upvalues are captured into cells and closed when the frame returns.
        closure_create = tmp_path / "closure_create.skein"
        closure_create.write_text(
            "func make_adder(x) {\n"
            "  func add(y) { return x + y; }\n"
            "  return add;\n"
            "}\n"
            "func make_pair() {\n"
            "  let n = 0;\n"
            "  func inc() { n = n + 1; return n; }\n"
            "  func get() { return n; }\n"
            "  return [inc, get];\n"
            "}\n"
            "let i = 0;\n"
            "let total = 0;\n"
            "while (i < 300) {\n"
            "  let add = make_adder(i);\n"
            "  total = total + add(1);\n"
            "  i = i + 1;\n"
            "}\n"
            "println(str(total));\n"
            "let k = 0;\n"
            "while (k < 200) {\n"
            "  let pair = make_pair();\n"
            "  let a = pair[0]();\n"
            "  let b = pair[0]();\n"
            "  if (k == 0) { println(str(a) + str(b) + str(pair[1]())); }\n"
            "  k = k + 1;\n"
            "}\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "closure-creation", closure_create)
        rc, out, err = run(binary, closure_create, True, debug=True)
        if rc != 0 or "[jit] enter make_adder" not in err:
            print(
                "jit smoke: closure creation was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # A body with a backward branch is compiled on its first call even
        # when the call-count threshold is not reached: one call can still
        # run a hot loop.  Loopless helpers must stay interpreted.
        loop_first = tmp_path / "loop_first.skein"
        loop_first.write_text(
            "func count(n) {\n"
            "  let total = 0;\n"
            "  let i = 0;\n"
            "  while (i < n) { total = total + i; i = i + 1; }\n"
            "  return total;\n"
            "}\n"
            "func flat(n) { return n + 1; }\n"
            "println(str(count(10)));\n"
            "println(str(flat(1)));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "loop-first-call", loop_first, threshold="100")
        loop_env = dict(os.environ)
        loop_env.update({
            "SKEIN_VM": "1",
            "SKEIN_JIT": "1",
            "SKEIN_JIT_THRESHOLD": "100",
            "SKEIN_JIT_DEBUG": "1",
        })
        proc = subprocess.run(
            [str(binary), str(loop_first)],
            env=loop_env, text=True, capture_output=True, timeout=120,
        )
        if proc.returncode != 0 or "[jit] enter count" not in proc.stderr:
            print(
                "jit smoke: loop function was not compiled on first call",
                file=sys.stderr,
            )
            print(f"  rc={proc.returncode} err={proc.stderr!r}",
                  file=sys.stderr)
            ok = False
        if "[jit] enter flat" in proc.stderr:
            print(
                "jit smoke: loopless helper was compiled before the threshold",
                file=sys.stderr,
            )
            ok = False
        loop_env["SKEIN_JIT_LOOPS"] = "0"
        proc = subprocess.run(
            [str(binary), str(loop_first)],
            env=loop_env, text=True, capture_output=True, timeout=120,
        )
        if proc.returncode != 0 or "[jit] enter count" in proc.stderr:
            print(
                "jit smoke: SKEIN_JIT_LOOPS=0 did not disable loop entry",
                file=sys.stderr,
            )
            print(f"  rc={proc.returncode} err={proc.stderr!r}",
                  file=sys.stderr)
            ok = False

        # for-in over the container kinds the JIT lowers directly, including
        # two-variable dict iteration, nested loops and early exits.
        iterators = tmp_path / "iterators.skein"
        iterators.write_text(
            "import std;\n"
            "import std.deque;\n"
            "import std.heap;\n"
            "import std.set;\n"
            "func sum_list(n) {\n"
            "  let xs = [];\n"
            "  let i = 0;\n"
            "  while (i < n) { xs = std.list.append(xs, i); i = i + 1; }\n"
            "  let total = 0;\n"
            "  for (let x in xs) { total = total + x; }\n"
            "  return total;\n"
            "}\n"
            "func sum_dict() {\n"
            "  let total = 0;\n"
            "  for (let k, v in {\"a\": 1, \"b\": 2, \"c\": 3}) {\n"
            "    total = total + len(k) + v;\n"
            "  }\n"
            "  return total;\n"
            "}\n"
            "func sum_text() {\n"
            "  let total = 0;\n"
            "  for (let ch in \"hello\") { total = total + 1; }\n"
            "  return total;\n"
            "}\n"
            "func sum_deque() {\n"
            "  let total = 0;\n"
            "  for (let x in std.deque.from_list([1, 2, 3])) {\n"
            "    total = total + x;\n"
            "  }\n"
            "  return total;\n"
            "}\n"
            "func sum_heap() {\n"
            "  let less = func(a, b) { return a < b; };\n"
            "  let h = std.heap.from_list([4, 1, 3], less);\n"
            "  let total = 0;\n"
            "  for (let x in h) { total = total + x; }\n"
            "  return total;\n"
            "}\n"
            "func sum_set() {\n"
            "  let total = 0;\n"
            "  for (let x in std.set.from_list([1, 2, 3])) {\n"
            "    total = total + x;\n"
            "  }\n"
            "  return total;\n"
            "}\n"
            "func gen(n) {\n"
            "  let i = 0;\n"
            "  while (i < n) { yield i; i = i + 1; }\n"
            "}\n"
            "func sum_gen(n) {\n"
            "  let total = 0;\n"
            "  for (let x in gen(n)) { total = total + x; }\n"
            "  return total;\n"
            "}\n"
            "func nested() {\n"
            "  let total = 0;\n"
            "  for (let a in [1, 2, 3]) {\n"
            "    for (let b in [10, 20]) {\n"
            "      if (a == 2) { break; }\n"
            "      total = total + a * b;\n"
            "    }\n"
            "  }\n"
            "  return total;\n"
            "}\n"
            "func early_exit() {\n"
            "  let total = 0;\n"
            "  for (let k, v in {\"a\": 1, \"b\": 2, \"c\": 3}) {\n"
            "    total = total + v;\n"
            "    if (total > 1) { return total; }\n"
            "  }\n"
            "  return total;\n"
            "}\n"
            "println(str(sum_list(5)));\n"
            "println(str(sum_dict()));\n"
            "println(str(sum_text()));\n"
            "println(str(sum_deque()));\n"
            "println(str(sum_heap()));\n"
            "println(str(sum_set()));\n"
            "println(str(sum_gen(6)));\n"
            "println(str(nested()));\n"
            "println(str(early_exit()));\n"
            "let i = 0;\n"
            "while (i < 200) {\n"
            "  sum_list(4);\n"
            "  sum_dict();\n"
            "  sum_gen(4);\n"
            "  i = i + 1;\n"
            "}\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "iterators", iterators)
        rc, out, err = run(binary, iterators, True, debug=True)
        if rc != 0 or "[jit] enter sum_gen" not in err:
            print(
                "jit smoke: iterator loop was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # Name-mode locals: a package read/modify/compound-updates an exported
        # binding through LOAD_NAME/STORE_NAME/COMPOUND_NAME.
        pkg_dir = tmp_path / "name_mode_pkg"
        pkg_dir.mkdir()
        (pkg_dir / "demo.skein").write_text(
            "package demo {\n"
            "  export let value = 41;\n"
            "  export func bump() {\n"
            "    value = value + 1;\n"
            "    return value;\n"
            "  }\n"
            "  export func add(n) {\n"
            "    value += n;\n"
            "    return value;\n"
            "  }\n"
            "}\n",
            encoding="utf-8",
        )
        name_main = pkg_dir / "main.skein"
        name_main.write_text(
            "import demo;\n"
            "let i = 0;\n"
            "let last = 0;\n"
            "while (i < 200) { last = demo.bump(); i = i + 1; }\n"
            "println(str(last));\n"
            "println(str(demo.add(10)));\n"
            "println(str(demo.value));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "name-mode", name_main)
        rc, out, err = run(binary, name_main, True, debug=True)
        if rc != 0 or "[jit] enter bump" not in err:
            print(
                "jit smoke: name-mode function was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # Route dispatch exercises name-mode recursion, string splitting with
        # for-in, index assignment into a dict and value comparisons.
        route_jit = tmp_path / "route_jit.skein"
        route_jit.write_text(
            "import std;\n"
            "import std.route;\n"
            "let app = route.new();\n"
            "app.get(\"/static\", func(req) {\n"
            "  return {\"status\": 200, \"body\": \"static\"};\n"
            "});\n"
            "app.get(\"/users/:id\", func(req) {\n"
            "  return {\"status\": 200, \"body\": req[\"params\"][\"id\"]};\n"
            "});\n"
            "let i = 0;\n"
            "let sum = 0;\n"
            "while (i < 300) {\n"
            "  let r = app.dispatch({\"method\": \"GET\",\n"
            "                        \"path\": \"/users/\" + str(i % 7)});\n"
            "  sum = sum + len(r[\"body\"]);\n"
            "  i = i + 1;\n"
            "}\n"
            "println(str(sum));\n"
            "println(str(app.dispatch({\"method\": \"GET\", \"path\": \"/static\"})"
            "[\"body\"]));\n"
            "println(str(app.dispatch({\"method\": \"POST\", \"path\": \"/static\"})"
            "[\"status\"]));\n"
            "println(str(app.dispatch({\"method\": \"GET\", \"path\": \"/missing\"})"
            "[\"status\"]));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "route-dispatch", route_jit)
        rc, out, err = run(binary, route_jit, True, debug=True)
        if rc != 0 or "[jit] enter _walk" not in err:
            print(
                "jit smoke: route name-mode recursion was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # Enum variants are resolved through GET_FIELD_OR_ENUM.
        enum_ops = tmp_path / "enum_ops.skein"
        enum_ops.write_text(
            "import std;\n"
            "enum Color { Red; Green; Blue; }\n"
            "func pick(n) {\n"
            "  if (n == 0) { return Color.Red; }\n"
            "  if (n == 1) { return Color.Green; }\n"
            "  return Color.Blue;\n"
            "}\n"
            "func name_of(n) {\n"
            "  let c = pick(n);\n"
            "  if (c == Color.Red) { return \"r\"; }\n"
            "  if (c == Color.Green) { return \"g\"; }\n"
            "  return \"b\";\n"
            "}\n"
            "let i = 0;\n"
            "let total = 0;\n"
            "while (i < 300) {\n"
            "  if (name_of(i % 3) == \"r\") { total = total + 1; }\n"
            "  i = i + 1;\n"
            "}\n"
            "println(str(total));\n"
            "println(str(name_of(1)));\n"
            "println(str(pick(2)));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "enum-field-or-enum", enum_ops)
        rc, out, err = run(binary, enum_ops, True, debug=True)
        if rc != 0 or "[jit] enter pick" not in err:
            print(
                "jit smoke: enum field resolution was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # Index assignment whose base is a captured (upvalue) container.
        upvalue_index = tmp_path / "upvalue_index.skein"
        upvalue_index.write_text(
            "func make() {\n"
            "  let store = {\"n\": 0};\n"
            "  func bump(k) {\n"
            "    store[k] = store[k] + 1;\n"
            "    return store[k];\n"
            "  }\n"
            "  return bump;\n"
            "}\n"
            "let bump = make();\n"
            "let i = 0;\n"
            "let total = 0;\n"
            "while (i < 300) { total = total + bump(\"n\"); i = i + 1; }\n"
            "println(str(total));\n"
            "println(str(bump(\"n\")));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "upvalue-index-write", upvalue_index)
        rc, out, err = run(binary, upvalue_index, True, debug=True)
        if rc != 0 or "[jit] enter bump" not in err:
            print(
                "jit smoke: upvalue index write was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # `fail` inside a compiled function propagates to a VM handler.
        fail_probe = tmp_path / "fail_probe.skein"
        fail_probe.write_text(
            "import std;\n"
            "func boom(n) {\n"
            "  if (n > 0) { fail \"bad \" + str(n); }\n"
            "  return n;\n"
            "}\n"
            "func maybe(n) { return boom(n); }\n"
            "let i = 0;\n"
            "let hits = 0;\n"
            "while (i < 300) {\n"
            "  let v = maybe(1) on error(e) { recover 7; };\n"
            "  hits = hits + v;\n"
            "  i = i + 1;\n"
            "}\n"
            "println(str(hits));\n"
            "println(str(maybe(0)));\n"
            "let caught = maybe(5) on error(e) {\n"
            "  recover std.error.message(e);\n"
            "};\n"
            "println(caught);\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "fail-catch", fail_probe)
        rc, out, err = run(binary, fail_probe, True, debug=True)
        if rc != 0 or "[jit] enter boom" not in err:
            print(
                "jit smoke: fail path was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # Handlers: nested on-error, pass to an outer handler, runtime errors
        # inside a handler body and Result unwrapping (CHECK_RESULT).
        handler_ops = tmp_path / "handler_ops.skein"
        handler_ops.write_text(
            "import std;\n"
            "func risky(n) {\n"
            "  if (n > 0) { fail \"bad \" + str(n); }\n"
            "  return n;\n"
            "}\n"
            "func nested(n) {\n"
            "  return risky(n) on error(e) {\n"
            "    let inner = risky(n + 1) on error(e2) { recover 5; };\n"
            "    recover inner;\n"
            "  };\n"
            "}\n"
            "func pass_outer() {\n"
            "  return (risky(1) on error(e) { pass e; })\n"
            "    on error(e2) { recover 7; };\n"
            "}\n"
            "func handler_throws() {\n"
            "  return risky(1) on error(e) {\n"
            "    recover (risky(2) on error(e2) { recover 9; });\n"
            "  };\n"
            "}\n"
            "func outer_catches_error() {\n"
            "  return (risky(1) on error(e) { recover 1 / 0; })\n"
            "    on error(e2) { recover 8; };\n"
            "}\n"
            "func captured(n) {\n"
            "  return std.result.capture(func() {\n"
            "    if (n > 0) { fail \"boom\"; }\n"
            "    return n * 2;\n"
            "  });\n"
            "}\n"
            "func use_result(n) {\n"
            "  return captured(n) on error(e) { recover -1; };\n"
            "}\n"
            "func boxed(n) {\n"
            "  return risky(n) on error(e) {\n"
            "    recover {\"message\": std.error.message(e)};\n"
            "  };\n"
            "}\n"
            "println(str(nested(1)));\n"
            "println(str(pass_outer()));\n"
            "println(str(handler_throws()));\n"
            "println(str(outer_catches_error()));\n"
            "println(str(use_result(0)));\n"
            "println(str(use_result(5)));\n"
            "println(boxed(1)[\"message\"]);\n"
            "let i = 0;\n"
            "let total = 0;\n"
            "while (i < 200) {\n"
            "  total = total + nested(1) + pass_outer() +\n"
            "          handler_throws() + outer_catches_error();\n"
            "  i = i + 1;\n"
            "}\n"
            "println(str(total));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "handler-ops", handler_ops)
        rc, out, err = run(binary, handler_ops, True, debug=True)
        if rc != 0 or "[jit] enter nested" not in err:
            print(
                "jit smoke: handler function was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # defer: LIFO order, block scopes, captures, early return and a
        # deferred fail propagating to the caller's handler.
        defer_ops = tmp_path / "defer_ops.skein"
        defer_ops.write_text(
            "import std;\n"
            "func basic(n) {\n"
            "  defer print(\"d1:\" + str(n) + \";\");\n"
            "  defer print(\"d2;\");\n"
            "  return n * 2;\n"
            "}\n"
            "func block(n) {\n"
            "  let total = 0;\n"
            "  let i = 0;\n"
            "  while (i < n) {\n"
            "    defer print(\"iter\" + str(i) + \";\");\n"
            "    total = total + i;\n"
            "    i = i + 1;\n"
            "  }\n"
            "  return total;\n"
            "}\n"
            "func captured() {\n"
            "  let x = 1;\n"
            "  defer print(\"x=\" + str(x) + \";\");\n"
            "  x = 9;\n"
            "  return x;\n"
            "}\n"
            "func outer() {\n"
            "  let y = 1;\n"
            "  func inner() {\n"
            "    defer print(\"y=\" + str(y) + \";\");\n"
            "    y = y + 1;\n"
            "    return y;\n"
            "  }\n"
            "  return inner();\n"
            "}\n"
            "func early(n) {\n"
            "  defer print(\"early;\");\n"
            "  if (n > 0) { return 1; }\n"
            "  return 2;\n"
            "}\n"
            "func failing() {\n"
            "  defer print(\"cleanup;\");\n"
            "  fail \"boom\";\n"
            "}\n"
            "func caller() {\n"
            "  return failing() on error(e) {\n"
            "    print(\"caught;\");\n"
            "    recover 1;\n"
            "  };\n"
            "}\n"
            "println(str(basic(3)));\n"
            "println(str(block(3)));\n"
            "println(str(captured()));\n"
            "println(str(outer()));\n"
            "println(str(early(1)));\n"
            "println(str(early(0)));\n"
            "println(str(caller()));\n"
            "let i = 0;\n"
            "while (i < 100) {\n"
            "  basic(1);\n"
            "  block(2);\n"
            "  captured();\n"
            "  i = i + 1;\n"
            "}\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "defer-ops", defer_ops)
        rc, out, err = run(binary, defer_ops, True, debug=True)
        if rc != 0 or "[jit] enter basic" not in err:
            print(
                "jit smoke: defer function was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        closure_const = tmp_path / "closure_const.skein"
        closure_const.write_text(
            "func make_const() {\n"
            "  const c = 5;\n"
            "  func set() { c = 9; return c; }\n"
            "  return set;\n"
            "}\n"
            "let f = make_const();\n"
            "let i = 0;\n"
            "while (i < 3) {\n"
            "  let r = f() on error(e) { recover 0; };\n"
            "  i = i + 1;\n"
            "}\n"
            "println(\"ok\");\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "closure-const-error", closure_const)

        native_worker = tmp_path / "native_wait_worker.skein"
        native_worker.write_text(
            "import std;\n"
            "func inner(n) {\n"
            "  let data = [n, n + 1, n + 2];\n"
            "  std.task.sleep(1).wait();\n"
            "  return data[0] + data[1] + data[2];\n"
            "}\n"
            "func handle(payload) { return {\"v\": inner(payload[\"n\"])}; }\n",
            encoding="utf-8",
        )
        native_worker_text = str(native_worker).replace("\\", "\\\\")
        native_wait = tmp_path / "native_wait.skein"
        native_wait.write_text(
            "import std;\n"
            f'let pool = std.thread.pool(2, "{native_worker_text}", '
            '{"worker_vm": true});\n'
            "let futures = [];\n"
            "for (let i = 0; i < 8; i = i + 1) {\n"
            '  futures = std.list.append(futures, pool.submit({"n": i}));\n'
            "}\n"
            "let total = 0;\n"
            "for (let f in futures) {\n"
            '  let r = f.wait();\n'
            '  if (!r["ok"]) { fail r["error"]; }\n'
            '  total = total + r["value"]["v"];\n'
            "}\n"
            "pool.close();\n"
            "pool.join();\n"
            "println(str(total));\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "native-wait", native_wait)
        rc, out, err = run(binary, native_wait, True, debug=True)
        if rc != 0 or "[jit] enter inner" not in err:
            print(
                "jit smoke: native task.wait suspension was not JIT-compiled",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # `launch` starts a fiber; the JIT must reject functions containing
        # it instead of calling the callee synchronously in the caller's
        # fiber.  A `fail` in a detached task must stay attached to the task.
        launch_task = tmp_path / "launch_task.skein"
        launch_task.write_text(
            "import std;\n"
            "func bad() {\n"
            "  std.task.sleep(1).wait();\n"
            '  fail "boom";\n'
            "}\n"
            "func parent() {\n"
            "  let t = launch bad();\n"
            "  t.detach();\n"
            "  std.task.sleep(20).wait();\n"
            "}\n"
            "parent();\n",
            encoding="utf-8",
        )
        ok &= compare(binary, "launch-detached-fail", launch_task,
                      threshold="0")
        rc, out, err = run(binary, launch_task, True, threshold="0",
                           debug=True)
        if rc != 0 or "detached task failed: boom" not in err:
            print(
                "jit smoke: detached task failure was not reported",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False
        if "[jit] cannot compile parent" not in err:
            print(
                "jit smoke: function using launch was not rejected",
                file=sys.stderr,
            )
            print(f"  rc={rc} out={out!r} err={err!r}", file=sys.stderr)
            ok = False

        # A JIT-compiled loop used to ignore SIGINT because generated code
        # only polled g_interrupted after a call helper returned.  A tight
        # loop with no calls had to be killed with SIGKILL.
        interrupt_loop = tmp_path / "interrupt_loop.skein"
        interrupt_loop.write_text(
            "func burn() {\n"
            "  let i = 0;\n"
            "  let total = 0;\n"
            "  while (true) { total = total + i; i = i + 1; }\n"
            "  return total;\n"
            "}\n"
            "burn();\n",
            encoding="utf-8",
        )
        ok &= check_jit_interrupt(binary, interrupt_loop, "burn")

        # Self tail calls jump back to pc 0 without a bytecode backedge;
        # the same header poll must cover that path too.
        interrupt_tail = tmp_path / "interrupt_tail.skein"
        interrupt_tail.write_text(
            "func spin(n) { return spin(n + 1); }\n"
            "spin(0);\n",
            encoding="utf-8",
        )
        ok &= check_jit_interrupt(binary, interrupt_tail, "spin")

    # Default-on: without SKEIN_JIT in the environment the JIT must run.
    # SKEIN_JIT=0 remains the kill switch.
    with tempfile.TemporaryDirectory(prefix="skein-jit-default-") as tmp:
        default_probe = Path(tmp) / "default_probe.skein"
        default_probe.write_text(
            "func ping(n) { return n + 1; }\n"
            "let i = 0;\n"
            "let total = 0;\n"
            "while (i < 20) { total = ping(total); i = i + 1; }\n"
            "println(total);\n",
            encoding="utf-8",
        )
        default_env = dict(os.environ)
        default_env["SKEIN_VM"] = "1"
        default_env.pop("SKEIN_JIT", None)
        default_env["SKEIN_JIT_THRESHOLD"] = "1"
        default_env["SKEIN_JIT_DEBUG"] = "1"
        proc = subprocess.run(
            [str(binary), str(default_probe)],
            env=default_env, text=True, capture_output=True, timeout=120)
        if proc.returncode != 0 or "[jit] enter ping" not in proc.stderr:
            print("jit smoke: JIT is not enabled by default", file=sys.stderr)
            print(f"  rc={proc.returncode} err={proc.stderr!r}", file=sys.stderr)
            ok = False

        default_env["SKEIN_JIT"] = "0"
        proc = subprocess.run(
            [str(binary), str(default_probe)],
            env=default_env, text=True, capture_output=True, timeout=120)
        if proc.returncode != 0 or "[jit]" in proc.stderr:
            print("jit smoke: SKEIN_JIT=0 did not disable the JIT",
                  file=sys.stderr)
            print(f"  rc={proc.returncode} err={proc.stderr!r}", file=sys.stderr)
            ok = False

    if not ok:
        return 1
    print("jit smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
