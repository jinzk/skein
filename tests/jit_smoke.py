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

    if not ok:
        return 1
    print("jit smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
