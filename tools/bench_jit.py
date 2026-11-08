#!/usr/bin/env python3
"""Compare the JIT backend against the bytecode VM.

The script runs every benchmark twice (SKEIN_JIT=0 and SKEIN_JIT=1) with the
VM enabled, discards a warm-up run, and reports the median wall-clock time.

Exit status is non-zero with --check when:
  * any benchmark makes the JIT slower than the VM by more than --max-ratio;
  * fib does not reach --fib-speedup (default 2.0x).
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BENCHMARKS = [
    "fib_bench",
    "call_bench",
    "method_bench",
    "slot_bench",
    "scope_bench",
    "global_bench",
    "field_bench",
    "index_bench",
    "index_write_bench",
    "pattern_bench",
]


def run_once(binary, script, jit):
    env = dict(os.environ)
    env["SKEIN_VM"] = "1"
    env["SKEIN_JIT"] = "1" if jit else "0"
    env["SKEIN_JIT_THRESHOLD"] = "1"
    start = time.perf_counter()
    proc = subprocess.run(
        [str(binary), str(script)],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        timeout=120,
    )
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise RuntimeError(
            f"{script.name} with SKEIN_JIT={env['SKEIN_JIT']} failed: "
            f"{proc.stderr.decode('utf-8', errors='replace').strip()}"
        )
    return elapsed


def measure(binary, script, jit, repeats):
    run_once(binary, script, jit)  # warm-up
    return statistics.median(
        run_once(binary, script, jit) for _ in range(repeats)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="./build/skein")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--max-ratio", type=float, default=1.05,
                        help="maximum allowed JIT/VM time ratio")
    parser.add_argument("--fib-speedup", type=float, default=2.0,
                        help="minimum required VM/JIT speedup for fib")
    args = parser.parse_args()

    binary = Path(args.bin)
    if not binary.exists():
        print(f"missing binary: {binary}", file=sys.stderr)
        return 2
    if args.repeats < 1:
        print("--repeats must be positive", file=sys.stderr)
        return 2

    results = []
    failures = []
    for name in BENCHMARKS:
        script = ROOT / "benchmarks" / f"{name}.skein"
        vm = measure(binary, script, False, args.repeats)
        jit = measure(binary, script, True, args.repeats)
        ratio = jit / vm if vm > 0 else 0.0
        speedup = vm / jit if jit > 0 else 0.0
        results.append({
            "benchmark": name,
            "vm": vm,
            "jit": jit,
            "ratio": ratio,
            "speedup": speedup,
        })

        if args.check and ratio > args.max_ratio:
            failures.append(
                f"{name}: JIT/VM ratio {ratio:.2f} exceeds "
                f"{args.max_ratio:.2f}"
            )
        if args.check and name == "fib_bench" and speedup < args.fib_speedup:
            failures.append(
                f"fib: speedup {speedup:.2f}x is below "
                f"{args.fib_speedup:.2f}x"
            )

    if args.json:
        print(json.dumps(results, indent=2))
    else:
        print(f"{'benchmark':<18} {'VM':>8} {'JIT':>8} {'ratio':>8} "
              f"{'speedup':>9}")
        for item in results:
            print(f"{item['benchmark']:<18} {item['vm']:8.3f} "
                  f"{item['jit']:8.3f} {item['ratio']:8.2f} "
                  f"{item['speedup']:8.2f}x")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    if args.check:
        print("JIT benchmark gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
