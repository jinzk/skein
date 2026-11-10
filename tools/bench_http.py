#!/usr/bin/env python3
"""Compare std.http.serve_threads with std.http.serve_workers.

Starts a generated server, sends a fixed number of keep-alive HTTP/1.1
requests from a configurable number of client threads, and reports throughput
plus latency percentiles. The server uses max_requests=requests so it stops on
its own and join() returns an exact served count.

With --check-rate and/or --baseline it also acts as a regression gate.

Usage:
    python tools/bench_http.py --bin build-msys2-jit-mi2/skein.exe
    python tools/bench_http.py --bin ./skein --mode workers --strategy reuse_port
"""

import argparse
import http.client
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BASELINE = os.path.join(ROOT, "benchmarks",
                                "bench_http_baseline.json")


def escape_skein_path(path):
    return path.replace("\\", "\\\\")


def write_worker(path):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            "import std;\n"
            "func handle(req) {\n"
            "  return {\"status\": 200,\n"
            "          \"headers\": {\"Content-Type\": \"text/plain\"},\n"
            "          \"body\": \"ok\"};\n"
            "}\n"
        )


def build_server(mode, workers, requests, strategy, worker_path):
    lines = [
        "import std;\n",
        'let s = std.http.listen("127.0.0.1", 0);\n',
    ]
    options = [
        f'"max_requests": {requests}',
        '"max_per_connection": 0',
    ]
    if strategy:
        options.append(f'"strategy": "{strategy}"')
    option_text = "{" + ", ".join(options) + "}"
    call = "serve_threads" if mode == "threads" else "serve_workers"
    lines.append(
        f'let srv = std.http.{call}(s, {workers}, '
        f'"{escape_skein_path(worker_path)}", {option_text});\n'
    )
    lines.extend([
        'println(s["port"]);\n',
        "std.io.flush(std.io.stdout);\n",
        'println("ready");\n',
        "std.io.flush(std.io.stdout);\n",
        'println("served=" + str(srv.join()));\n',
    ])
    return "".join(lines)


def run_clients(port, requests, concurrency, timeout):
    per_thread = requests // concurrency
    remainder = requests % concurrency
    start_event = threading.Event()
    latencies = []
    errors = []
    lock = threading.Lock()

    def client(count):
        connection = None
        try:
            connection = http.client.HTTPConnection(
                "127.0.0.1", port, timeout=timeout)
            start_event.wait()
            for _ in range(count):
                began = time.perf_counter()
                connection.request("GET", "/bench")
                response = connection.getresponse()
                body = response.read()
                elapsed = time.perf_counter() - began
                with lock:
                    latencies.append(elapsed)
                if response.status != 200 or body != b"ok":
                    with lock:
                        errors.append(
                            f"status={response.status} body={body!r}")
                    break
        except Exception as exc:  # noqa: BLE001 - report client failures
            with lock:
                errors.append(repr(exc))
        finally:
            if connection is not None:
                connection.close()

    threads = []
    for index in range(concurrency):
        count = per_thread + (1 if index < remainder else 0)
        thread = threading.Thread(target=client, args=(count,))
        thread.start()
        threads.append(thread)

    started = time.perf_counter()
    start_event.set()
    for thread in threads:
        thread.join()
    elapsed = time.perf_counter() - started
    return elapsed, latencies, errors


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = int(round((len(ordered) - 1) * fraction))
    return ordered[index]


def run_case(binary, mode, workers, requests, concurrency, strategy,
             timeout):
    with tempfile.TemporaryDirectory() as tmp:
        worker_path = os.path.join(tmp, "bench_worker.skein")
        write_worker(worker_path)
        server_path = os.path.join(tmp, "bench_server.skein")
        with open(server_path, "w", encoding="utf-8") as handle:
            handle.write(build_server(
                mode, workers, requests, strategy, worker_path))

        process = subprocess.Popen(
            [os.path.abspath(binary), server_path],
            cwd=tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        try:
            port_line = process.stdout.readline().strip()
            if not port_line.isdigit():
                raise SystemExit(
                    f"server did not report a port: {port_line!r} / "
                    f"{process.stderr.read()}")
            ready_line = process.stdout.readline().strip()
            if ready_line != "ready":
                raise SystemExit(
                    f"server did not become ready: {ready_line!r} / "
                    f"{process.stderr.read()}")
            port = int(port_line)

            elapsed, latencies, errors = run_clients(
                port, requests, concurrency, timeout)
            if errors:
                raise SystemExit(f"client errors: {errors[:3]}")

            out, err = process.communicate(timeout=timeout)
            if process.returncode != 0:
                raise SystemExit(
                    f"server exited {process.returncode}: {err}")
            served = None
            for line in out.splitlines():
                if line.startswith("served="):
                    served = line.split("=", 1)[1]
            if served != str(requests):
                raise SystemExit(
                    f"expected served={requests}, got {served!r}: {out}")
            rate = requests / elapsed if elapsed > 0 else 0.0
            mean = (sum(latencies) / len(latencies) * 1000
                    if latencies else 0.0)
            return {
                "mode": mode,
                "strategy": strategy or "auto",
                "workers": workers,
                "concurrency": concurrency,
                "requests": requests,
                "elapsed": elapsed,
                "served": served,
                "rate": rate,
                "mean_ms": mean,
                "p50_ms": percentile(latencies, 0.50) * 1000,
                "p95_ms": percentile(latencies, 0.95) * 1000,
                "p99_ms": percentile(latencies, 0.99) * 1000,
            }
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def print_result(result):
    print(
        f"{result['mode']:<8} strategy={result['strategy']:<10} "
        f"workers={result['workers']:<3} "
        f"concurrency={result['concurrency']:<3} "
        f"requests={result['requests']:<6} "
        f"served={result['served']:<6} "
        f"elapsed={result['elapsed']:.3f}s "
        f"rate={result['rate']:.0f} req/s "
        f"mean={result['mean_ms']:.2f}ms "
        f"p50={result['p50_ms']:.2f}ms "
        f"p95={result['p95_ms']:.2f}ms "
        f"p99={result['p99_ms']:.2f}ms"
    )


def baseline_key(result):
    return f"{result['mode']}_{result['strategy']}"


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="./skein",
                        help="path to the skein interpreter")
    parser.add_argument("--mode", choices=["threads", "workers", "both"],
                        default="both", help="server mode to benchmark")
    parser.add_argument("--workers", type=int, default=4,
                        help="server worker count")
    parser.add_argument("--requests", type=int, default=4000,
                        help="total requests per case")
    parser.add_argument("--concurrency", type=int, default=8,
                        help="client threads")
    parser.add_argument("--strategy", default=None,
                        choices=[None, "auto", "dispatch", "reuse_port"],
                        help="force a server strategy (default: server auto)")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="server/client timeout in seconds")
    parser.add_argument("--check-rate", type=float, default=0.0,
                        help="fail when throughput is below this req/s")
    parser.add_argument("--baseline", default=DEFAULT_BASELINE,
                        help="throughput baseline JSON")
    parser.add_argument("--max-regression", type=float, default=0.5,
                        help="allowed fractional drop below the baseline")
    args = parser.parse_args()

    if args.workers <= 0:
        parser.error("--workers must be positive")
    if args.requests <= 0:
        parser.error("--requests must be positive")
    if args.concurrency <= 0:
        parser.error("--concurrency must be positive")
    if args.requests < args.concurrency:
        parser.error("--requests must be at least --concurrency")

    modes = ["threads", "workers"] if args.mode == "both" else [args.mode]
    baseline = load_baseline(args.baseline)
    failures = 0
    for mode in modes:
        try:
            result = run_case(
                args.bin, mode, args.workers, args.requests,
                args.concurrency, args.strategy, args.timeout)
        except SystemExit as exc:
            failures += 1
            print(f"{mode}: {exc}", file=sys.stderr)
            continue
        print_result(result)
        if args.check_rate > 0 and result["rate"] < args.check_rate:
            failures += 1
            print(
                f"{mode}: throughput {result['rate']:.0f} req/s is below "
                f"{args.check_rate:.0f} req/s",
                file=sys.stderr,
            )
        if baseline:
            key = baseline_key(result)
            reference = baseline.get(key)
            if isinstance(reference, (int, float)) and reference > 0:
                limit = reference * (1.0 - args.max_regression)
                if result["rate"] < limit:
                    failures += 1
                    print(
                        f"{key}: throughput {result['rate']:.0f} req/s is "
                        f"below {limit:.0f} req/s (baseline {reference:.0f}, "
                        f"max regression {args.max_regression:.0%})",
                        file=sys.stderr,
                    )
    if failures:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
