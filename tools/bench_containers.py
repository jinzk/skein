#!/usr/bin/env python3
"""Benchmark common container operations and check growth factors.

The check is intentionally coarse: it verifies that operations expected to be
linear / O(n log n) do not grow like O(n^2) when n doubles.  Absolute timings
are printed but never asserted, to avoid CI flakiness.
"""

import argparse
import statistics
import subprocess
import sys
import time

CASES = [
    {
        "name": "deque.push_back",
        "sizes": (40000, 80000),
        "expect": "linear",
        "imports": ("std", "std.deque"),
        "src": """
let d = std.deque.new();
let i = 0;
while (i < {n}) { d = std.deque.push_back(d, i); i = i + 1; }
print(std.deque.size(d));
""",
    },
    {
        "name": "heap.push",
        "sizes": (20000, 40000),
        "expect": "nlog",
        "imports": ("std", "std.heap"),
        "src": """
let h = std.heap.new(func(a, b) { return a < b; });
let i = 0;
while (i < {n}) { h = std.heap.push(h, i); i = i + 1; }
print(std.heap.size(h));
""",
    },
    {
        "name": "list.append",
        "sizes": (2000, 4000),
        "expect": "quadratic",
        "imports": ("std",),
        "src": """
let xs = [];
let i = 0;
while (i < {n}) { xs = std.list.append(xs, i); i = i + 1; }
print(len(xs));
""",
    },
    {
        "name": "dict.insert",
        "sizes": (40000, 80000),
        "expect": "linear",
        "imports": ("std",),
        "src": """
let d = {};
let i = 0;
while (i < {n}) { d[i] = i; i = i + 1; }
print(len(d));
""",
    },
    {
        "name": "dict.read",
        "sizes": (20000, 40000),
        "expect": "linear",
        "imports": ("std",),
        "src": """
let d = {};
let i = 0;
while (i < {n}) { d[i] = i; i = i + 1; }
let s = 0; i = 0;
while (i < 4 * {n}) { s = s + d[i % {n}]; i = i + 1; }
print(s);
""",
    },
    {
        "name": "dict.delete_all",
        "sizes": (2000, 4000),
        "expect": "quadratic",
        "imports": ("std",),
        "src": """
let d = {};
let i = 0;
while (i < {n}) { d[i] = i; i = i + 1; }
i = 0;
while (i < {n}) { d = std.dict.delete(d, i); i = i + 1; }
print(len(d));
""",
    },
    {
        "name": "set.add",
        "sizes": (20000, 40000),
        "expect": "nlog",
        "imports": ("std", "std.set"),
        "src": """
let s = std.set.new();
let i = 0;
while (i < {n}) { s = std.set.add(s, i); i = i + 1; }
print(std.set.size(s));
""",
    },
    {
        "name": "set.from_list",
        "sizes": (20000, 40000),
        "expect": "nlog",
        "imports": ("std", "std.set"),
        "src": """
let s = std.set.from_list(range({n}));
print(std.set.size(s));
""",
    },
    {
        "name": "graph.from_edges+bfs",
        "sizes": (4000, 8000),
        "expect": "linear",
        "imports": ("std", "std.graph"),
        "src": """
let edges = std.deque.new();
let i = 1;
while (i < {n}) { edges = std.deque.push_back(edges, [i, i + 1]); i = i + 1; }
let g = graph.from_edges({n}, std.deque.to_list(edges));
print(len(graph.bfs(g, 1)));
""",
    },
    {
        "name": "graph.add_node",
        "sizes": (2000, 4000),
        "expect": "quadratic",
        "imports": ("std", "std.graph"),
        "src": """
let g = graph.new();
let i = 0;
while (i < {n}) { let r = graph.add_node(g, i); g = r["graph"]; i = i + 1; }
print(graph.node_count(g));
""",
    },
    {
        "name": "arena.from_list",
        "sizes": (40000, 80000),
        "expect": "linear",
        "imports": ("std", "std.arena"),
        "src": """
let s = arena.from_list(range({n}));
print(arena.size(s));
""",
    },
    {
        "name": "arena.add",
        "sizes": (2000, 4000),
        "expect": "quadratic",
        "imports": ("std", "std.arena"),
        "src": """
let s = arena.new();
let i = 0;
while (i < {n}) { let r = arena.add(s, i); s = r["arena"]; i = i + 1; }
print(arena.size(s));
""",
    },
    {
        "name": "fsm.run",
        "sizes": (10000, 20000),
        "expect": "linear",
        "imports": ("std", "std.fsm"),
        "src": """
let m = fsm.define(0, func(state, event) { return state + 1; });
let ctx = fsm.run(m, fsm.start(m), range({n}));
print(ctx.state);
""",
    },
    {
        "name": "lru.put",
        "sizes": (2000, 4000),
        "expect": "quadratic",
        "imports": ("std",),
        "src": """
let l = std.lru.new({n});
let i = 0;
while (i < {n}) { let r = std.lru.put(l, i, i); l = r["lru"]; i = i + 1; }
print(std.lru.len(l));
""",
    },
    {
        "name": "list.unique",
        "sizes": (100000, 200000),
        "expect": "linear",
        "imports": ("std",),
        "src": """
let xs = range({n});
print(len(std.list.unique(xs)));
""",
    },
    {
        "name": "list.top_k(k=100)",
        "sizes": (200000, 400000),
        "expect": "linear",
        "imports": ("std",),
        "src": """
let xs = range({n});
print(len(std.list.top_k(xs, 100)));
""",
    },
    {
        "name": "dict.eq",
        "sizes": (40000, 80000),
        "expect": "linear",
        "imports": ("std",),
        "src": """
let a = {}; let b = {}; let i = 0;
while (i < {n}) { a[i] = i; b[i] = i; i = i + 1; }
print(a == b);
""",
    },
]

THRESHOLDS = {
    "linear": 3.2,
    "nlog": 3.2,
    "quadratic": 6.5,
}


def run_once(binary, source, timeout):
    started = time.perf_counter()
    proc = subprocess.run(
        [binary, "-"],
        input=source,
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=timeout,
    )
    elapsed = time.perf_counter() - started
    if proc.returncode != 0:
        raise RuntimeError(
            f"benchmark failed (exit {proc.returncode}):\n{proc.stderr}")
    return elapsed


def measure(binary, source, repeat, timeout):
    run_once(binary, source, timeout)  # warmup
    samples = [run_once(binary, source, timeout) for _ in range(repeat)]
    return statistics.median(samples)


def baseline_source(imports):
    return "".join(f"import {name};\n" for name in imports) + "print(1);\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="./build/skein",
                        help="path to the skein binary")
    parser.add_argument("--repeat", type=int, default=3,
                        help="timed runs per size (median is used)")
    parser.add_argument("--check", action="store_true",
                        help="fail when a linear case grows quadratically")
    parser.add_argument("--json", action="store_true",
                        help="emit JSON instead of a table")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    baselines = {}
    results = []
    failed = False

    for case in CASES:
        imports = tuple(case["imports"])
        if imports not in baselines:
            baselines[imports] = measure(
                args.bin, baseline_source(imports), 3, args.timeout)
        baseline = baselines[imports]

        imports_source = "".join(f"import {name};\n" for name in imports)
        times = []
        for n in case["sizes"]:
            source = imports_source + case["src"].replace("{n}", str(n))
            times.append(measure(args.bin, source, args.repeat, args.timeout))

        adjusted = [max(0.001, t - baseline) for t in times]
        ratio = adjusted[1] / adjusted[0] if adjusted[0] > 0 else 0.0
        expected = case["expect"]
        threshold = THRESHOLDS[expected]
        noisy = min(adjusted) < 0.01
        if noisy:
            status = "skip (noise)"
        elif expected == "quadratic":
            status = "ok (known)" if ratio <= threshold else "FAIL"
            failed = failed or ratio > threshold
        else:
            status = "ok" if ratio <= threshold else "FAIL"
            failed = failed or ratio > threshold

        results.append({
            "name": case["name"],
            "n1": case["sizes"][0],
            "t1": adjusted[0],
            "n2": case["sizes"][1],
            "t2": adjusted[1],
            "growth": ratio,
            "expect": expected,
            "status": status,
        })

    if args.json:
        import json
        print(json.dumps(results, indent=2))
    else:
        print(f"{'case':28} {'n1':>8} {'t1':>9} {'n2':>8} {'t2':>9} "
              f"{'growth':>7}  {'expect':10} status")
        for r in results:
            print(f"{r['name']:28} {r['n1']:8d} {r['t1']:8.4f}s "
                  f"{r['n2']:8d} {r['t2']:8.4f}s {r['growth']:7.2f}  "
                  f"{r['expect']:10} {r['status']}")

    if args.check and failed:
        print("container benchmarks: growth check failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
