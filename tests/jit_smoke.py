#!/usr/bin/env python3
"""Smoke test for the experimental JIT backend.

Runs the same programs with SKEIN_JIT=0 and SKEIN_JIT=1 and compares stdout,
stderr and exit status.  It only covers the small opcode subset implemented by
the spike (fib and simple int branches), so it is intentionally independent of
the full regression suite.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(binary, script, jit, threshold="0"):
    env = dict(os.environ)
    env["SKEIN_VM"] = "1"
    env["SKEIN_JIT"] = "1" if jit else "0"
    env["SKEIN_JIT_THRESHOLD"] = threshold
    proc = subprocess.run(
        [str(binary), str(script)],
        env=env,
        text=True,
        capture_output=True,
        timeout=120,
    )
    return proc.returncode, proc.stdout, proc.stderr


def compare(binary, label, script):
    vm = run(binary, script, False)
    jit = run(binary, script, True)
    if vm != jit:
        print(f"jit smoke: {label} mismatch", file=sys.stderr)
        print(f"  vm : {vm!r}", file=sys.stderr)
        print(f"  jit: {jit!r}", file=sys.stderr)
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

    if not ok:
        return 1
    print("jit smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
