#!/usr/bin/env python3
"""Run the VM benchmark suite against the AST backend.

Each benchmark is executed several times with SKEIN_VM=0 and SKEIN_VM=1.
The reported number is the median wall-clock time.  In --check mode the
script fails when the VM is unexpectedly slower than AST or exceeds a
generous absolute time budget.
"""

import argparse
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BENCHMARKS = [
    ("slot", ROOT / "benchmarks" / "slot_bench.skein", 1.5),
    ("fib", ROOT / "benchmarks" / "fib_bench.skein", 1.5),
    ("call", ROOT / "benchmarks" / "call_bench.skein", 1.5),
    ("pattern", ROOT / "benchmarks" / "pattern_bench.skein", 1.5),
    ("field", ROOT / "benchmarks" / "field_bench.skein", 1.5),
    ("method", ROOT / "benchmarks" / "method_bench.skein", 1.5),
]


def run_once(binary, script, vm):
    env = dict(os.environ)
    env["SKEIN_VM"] = vm
    start = time.perf_counter()
    proc = subprocess.run(
        [str(binary), str(script)],
        env=env,
        text=True,
        capture_output=True,
        timeout=60,
    )
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise RuntimeError(
            f"{script.name} with SKEIN_VM={vm} failed: "
            f"{proc.stderr.strip()}"
        )
    return elapsed


def measure(binary, script, vm, repeats):
    return statistics.median(
        run_once(binary, script, vm) for _ in range(repeats)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="./build/skein")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--max-ratio", type=float, default=0)
    args = parser.parse_args()

    binary = Path(args.bin)
    if not binary.exists():
        print(f"missing binary: {binary}", file=sys.stderr)
        return 2
    if args.repeats < 1:
        print("--repeats must be positive", file=sys.stderr)
        return 2

    failures = []
    print(f"{'benchmark':<12} {'AST':>8} {'VM':>8} {'ratio':>8}")
    for name, script, ratio_limit in BENCHMARKS:
        ast = measure(binary, script, "0", args.repeats)
        vm = measure(binary, script, "1", args.repeats)
        ratio = vm / ast if ast > 0 else 0.0
        print(f"{name:<12} {ast:8.3f} {vm:8.3f} {ratio:8.2f}")

        limit = args.max_ratio if args.max_ratio > 0 else ratio_limit
        if args.check and ratio > limit:
            failures.append(
                f"{name}: VM/AST ratio {ratio:.2f} exceeds {limit:.2f}"
            )
        if args.check and vm > 10.0:
            failures.append(f"{name}: VM took {vm:.2f}s (limit 10s)")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    if args.check:
        print("vm benchmark regression check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
