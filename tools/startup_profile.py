#!/usr/bin/env python3
"""Measure Skein interpreter startup phases.

The in-process marks emitted by SKEIN_STARTUP_STATS only cover time inside
main().  Wall time also includes process creation, the OS loader, CRT startup
and shared-library initialization, which for short scripts is usually larger
than everything the interpreter does.

Marks are sequential deltas between lifecycle points.  Spans record
independently nested regions (imports, namespace construction, embedded package
parsing).  Span rows are indented by nesting depth in the profiler output.
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import time

CASES = [
    ("empty", ""),
    ("import std", "import std;\n"),
    ("import std.web", "import std.web;\n"),
]

VARIANTS = [
    ("default", {}),
    ("jit=0", {"SKEIN_JIT": "0"}),
    ("no-vm", {"SKEIN_VM": "0"}),
]

MARK_RE = re.compile(r"\[startup\] (\S+)\s+([0-9.]+) ms$")
SPAN_RE = re.compile(r"\[startup\] ( *\S.*?)\s+([0-9.]+) ms$")


def run_once(binary, source, env):
    started = time.perf_counter()
    proc = subprocess.run(
        [binary, "-"],
        input=source,
        text=True,
        capture_output=True,
        env=env,
        timeout=30,
    )
    wall_ms = (time.perf_counter() - started) * 1000.0
    marks = {}
    spans = []
    total = None
    in_spans = False
    for line in proc.stderr.splitlines():
        match = re.match(r"\[startup\] total_ms=([0-9.]+)", line)
        if match:
            total = float(match.group(1))
            continue
        if line.rstrip() == "[startup] spans:":
            in_spans = True
            continue
        match = SPAN_RE.match(line) if in_spans else None
        if match:
            text = match.group(1)
            depth = max(0, (len(text) - len(text.lstrip()) - 2) // 2)
            spans.append((text.strip(), depth, float(match.group(2))))
            continue
        match = MARK_RE.match(line)
        if match:
            name = match.group(1)
            marks[name] = marks.get(name, 0.0) + float(match.group(2))
    if total is None:
        raise RuntimeError(
            f"profiler output missing; exit={proc.returncode}\n{proc.stderr}")
    return total, wall_ms, marks, spans


def measure(binary, source, env, runs):
    samples = [run_once(binary, source, env) for _ in range(runs + 3)]
    samples = samples[3:]
    phases = {}
    span_names = {}
    imports = []
    for _, _, marks, spans in samples:
        for name, value in marks.items():
            phases.setdefault(name, []).append(value)
        for name, depth, value in spans:
            span_names.setdefault(name, []).append(value)
        imports.append(sum(value for name, depth, value in spans
                           if depth == 0 and name.startswith("import ")))
    return {
        "total": statistics.median(item[0] for item in samples),
        "wall": statistics.median(item[1] for item in samples),
        "imports": statistics.median(imports),
        "phases": {
            name: statistics.median(values)
            for name, values in phases.items()
        },
        "spans": {
            name: statistics.median(values)
            for name, values in span_names.items()
        },
        "last_spans": samples[-1][3],
    }


def main():
    default_bin = "build/skein.exe" if os.name == "nt" else "build/skein"
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", default=default_bin)
    parser.add_argument("--runs", type=int, default=12)
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="print every mark and the span tree for each case")
    args = parser.parse_args()

    binary = os.path.abspath(args.bin)
    if not os.path.exists(binary):
        raise SystemExit(f"binary not found: {binary}")

    header = (f"{'variant':10s} {'case':14s} {'wall':>8s} {'main':>8s} "
              f"{'config':>8s} {'imports':>8s} {'std_ns':>8s} "
              f"{'eval_rest':>9s}")
    print(header)

    for variant, extra in VARIANTS:
        env = os.environ.copy()
        env["SKEIN_STARTUP_STATS"] = "1"
        env.update(extra)
        for label, source in CASES:
            result = measure(binary, source, env, args.runs)
            phases = result["phases"]
            imports = result["imports"]
            std_ns = result["spans"].get("std_namespace", 0.0)
            eval_rest = max(0.0, phases.get("parse_eval", 0.0) - imports)
            print(f"{variant:10s} {label:14s} "
                  f"{result['wall']:8.3f} {result['total']:8.3f} "
                  f"{phases.get('runtime_config', 0.0):8.3f} "
                  f"{imports:8.3f} {std_ns:8.3f} {eval_rest:9.3f}")

    if not args.verbose:
        return

    env = os.environ.copy()
    env["SKEIN_STARTUP_STATS"] = "1"
    for label, source in CASES:
        result = measure(binary, source, env, args.runs)
        print(f"\n[{label}] main={result['total']:.3f} ms "
              f"wall={result['wall']:.3f} ms")
        for name, value in result["phases"].items():
            print(f"  {name:26s} {value:8.3f} ms")
        if result["last_spans"]:
            print("  spans:")
            for name, depth, value in result["last_spans"]:
                print(f"  {' ' * (2 + depth * 2)}{name:24s} {value:8.3f} ms")


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
