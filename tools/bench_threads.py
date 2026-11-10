#!/usr/bin/env python3
"""Benchmark thread pools, channels and thread spawn/join throughput.

Each case runs a generated driver in a child interpreter, warms up once, and
reports the median of N repeats.  Timings are taken inside the driver so
process startup does not dominate.

With --check the script compares rates against a recorded baseline JSON
(keys: "default" or the platform name from sys.platform) and fails when a case
drops below baseline * (1 - --max-regression).  Explicit --min-* floors take
precedence when given.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BASELINE = os.path.join(ROOT, "benchmarks",
                                "bench_threads_baseline.json")

CASES = {
    "pool_seq": {
        "script": "pool_seq",
        "jobs": 2000,
    },
    "pool_batch": {
        "script": "pool_batch",
        "jobs": 2000,
    },
    "channel": {
        "script": "channel",
        "messages": 20000,
    },
    "spawn": {
        "script": "spawn",
        "threads": 200,
    },
}


def skein_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_text(path, text):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def pool_worker_source():
    return (
        "func handle(payload) {\n"
        '    return {"n": payload["n"] + 1};\n'
        "}\n"
    )


def pool_driver_source(worker, workers, jobs, batch):
    submit = (
        "  let r = pool.submit({\"n\": i}).wait();\n"
        '  if (r["ok"]) { total = total + r["value"]["n"]; }\n'
        if not batch else
        '  futures = std.list.append(futures, '
        'pool.submit({"n": i}));\n'
    )
    wait = (
        ""
        if not batch else
        "for (let f in futures) {\n"
        "  let r = f.wait();\n"
        '  if (r["ok"]) { total = total + r["value"]["n"]; }\n'
        "}\n"
    )
    return (
        "import std;\n"
        f"let pool = std.thread.pool({workers}, {skein_string(worker)});\n"
        "let total = 0;\n"
        "let futures = [];\n"
        "let t0 = std.time.monotonic_ms();\n"
        f"for (let i = 0; i < {jobs}; i = i + 1) {{\n"
        + submit +
        "}\n"
        + wait +
        "let elapsed = std.time.monotonic_ms() - t0;\n"
        "pool.close();\n"
        "pool.join();\n"
        'print("TIME "); print(elapsed); print(" TOTAL "); println(total);\n'
    )


def channel_driver_source(messages):
    return (
        "import std;\n"
        "thread func echo(ctx) {\n"
        '  let ch = ctx.handles["ch"];\n'
        "  while (true) {\n"
        "    let m = ch.recv(10000);\n"
        "    if (m == null) { return; }\n"
        '    if (m.payload["stop"]) { return; }\n'
        "    ch.send(m);\n"
        "  }\n"
        "}\n"
        "let ch = std.thread.Channel.bounded(1);\n"
        "let worker = std.thread.spawn(echo, "
        'std.thread.Message.from({"n": 0}, {"ch": ch}));\n'
        "let t0 = std.time.monotonic_ms();\n"
        f"for (let i = 0; i < {messages}; i = i + 1) {{\n"
        '  ch.send(std.thread.Message.from({"i": i, "stop": false}));\n'
        "  let m = ch.recv(10000);\n"
        "}\n"
        "let elapsed = std.time.monotonic_ms() - t0;\n"
        'ch.send(std.thread.Message.from({"i": 0, "stop": true}));\n'
        "worker.join();\n"
        'print("TIME "); print(elapsed); print(" TOTAL "); println(0);\n'
    )


def spawn_driver_source(threads):
    return (
        "import std;\n"
        "thread func quick(ctx) {\n"
        '  return {"n": ctx.payload["n"]};\n'
        "}\n"
        "let total = 0;\n"
        "let t0 = std.time.monotonic_ms();\n"
        f"for (let i = 0; i < {threads}; i = i + 1) {{\n"
        '  let t = std.thread.spawn(quick, '
        'std.thread.Message.from({"n": i}));\n'
        "  let r = t.join();\n"
        '  if (r["ok"]) { total = total + r["value"]["n"]; }\n'
        "}\n"
        "let elapsed = std.time.monotonic_ms() - t0;\n"
        'print("TIME "); print(elapsed); print(" TOTAL "); println(total);\n'
    )


def run_case(binary, name, workers, tmp):
    case = CASES[name]
    script = case["script"]
    if script in ("pool_seq", "pool_batch"):
        worker = os.path.join(tmp, f"{name}_worker.skein")
        write_text(worker, pool_worker_source())
        driver = os.path.join(tmp, f"{name}.skein")
        write_text(driver, pool_driver_source(
            worker, workers, case["jobs"], script == "pool_batch"))
        units = case["jobs"]
    elif script == "channel":
        driver = os.path.join(tmp, "channel.skein")
        write_text(driver, channel_driver_source(case["messages"]))
        units = case["messages"]
    else:
        driver = os.path.join(tmp, "spawn.skein")
        write_text(driver, spawn_driver_source(case["threads"]))
        units = case["threads"]

    proc = subprocess.run(
        [os.path.abspath(binary), driver],
        cwd=tmp,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        timeout=180,
        env=os.environ.copy(),
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"{name}: interpreter exited {proc.returncode}: "
            f"{proc.stderr.strip() or proc.stdout.strip()}")
    elapsed_ms = None
    total = None
    for line in proc.stdout.splitlines():
        if line.startswith("TIME "):
            parts = line.split()
            try:
                elapsed_ms = float(parts[1])
                total = int(parts[3])
            except (IndexError, ValueError):
                pass
    if elapsed_ms is None or elapsed_ms <= 0:
        raise RuntimeError(f"{name}: no timing line: {proc.stdout!r}")
    rate = units * 1000.0 / elapsed_ms
    return {"case": name, "elapsed_ms": elapsed_ms, "units": units,
            "rate": rate, "total": total}


def measure(binary, name, workers, repeats, tmp):
    run_case(binary, name, workers, tmp)  # warm-up
    results = [run_case(binary, name, workers, tmp) for _ in range(repeats)]
    return {
        "case": name,
        "elapsed_ms": statistics.median(r["elapsed_ms"] for r in results),
        "rate": statistics.median(r["rate"] for r in results),
    }


def load_baseline(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    if sys.platform in data and isinstance(data[sys.platform], dict):
        return data[sys.platform]
    default = data.get("default")
    return default if isinstance(default, dict) else None


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="./build/skein")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--baseline", default=DEFAULT_BASELINE)
    parser.add_argument("--max-regression", type=float, default=0.5,
                        help="allowed fractional drop below the baseline")
    parser.add_argument("--min-pool-seq", type=float, default=0.0)
    parser.add_argument("--min-pool-batch", type=float, default=0.0)
    parser.add_argument("--min-channel", type=float, default=0.0)
    parser.add_argument("--min-spawn", type=float, default=0.0)
    args = parser.parse_args(argv)
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    return args


def main(argv):
    args = parse_args(argv)
    floors = {
        "pool_seq": args.min_pool_seq,
        "pool_batch": args.min_pool_batch,
        "channel": args.min_channel,
        "spawn": args.min_spawn,
    }
    baseline = load_baseline(args.baseline)
    results = []
    failures = []
    with tempfile.TemporaryDirectory(prefix="skein-bench-") as tmp:
        for name in CASES:
            try:
                result = measure(args.bin, name, args.workers,
                                 args.repeats, tmp)
            except (RuntimeError, subprocess.TimeoutExpired) as exc:
                failures.append(str(exc))
                continue
            results.append(result)
            floor = floors.get(name, 0.0)
            if args.check and floor > 0 and result["rate"] < floor:
                failures.append(
                    f"{name}: {result['rate']:.0f}/s is below floor "
                    f"{floor:.0f}/s")
            if (args.check and baseline and name in baseline and
                    baseline[name] > 0):
                limit = baseline[name] * (1.0 - args.max_regression)
                if result["rate"] < limit:
                    failures.append(
                        f"{name}: {result['rate']:.0f}/s is below "
                        f"{limit:.0f}/s (baseline {baseline[name]:.0f}/s, "
                        f"max regression {args.max_regression:.0%})")

    if args.json:
        print(json.dumps({"results": results,
                          "baseline": baseline}, indent=2))
    else:
        print(f"{'case':<12} {'ms':>10} {'rate/s':>12}")
        for item in results:
            print(f"{item['case']:<12} {item['elapsed_ms']:10.1f} "
                  f"{item['rate']:12.0f}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    if args.check:
        if baseline is None:
            print("thread benchmark gate passed (no baseline found)")
        else:
            print("thread benchmark gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
