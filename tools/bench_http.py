#!/usr/bin/env python3
"""Compare std.http.serve_threads with std.http.serve_processes.

Starts a generated server, sends a fixed number of keep-alive HTTP/1.1
requests from a configurable number of client threads, and reports throughput
plus latency percentiles. The server uses max_requests=requests so it stops on
its own and join() returns an exact served count.

With --check-rate and/or --baseline it also acts as a regression gate.

Usage:
    python tools/bench_http.py --bin build-msys2-jit-mi2/skein.exe
    python tools/bench_http.py --bin ./skein --mode processes --strategy reuse_port
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
    call = "serve_threads" if mode == "threads" else "serve_processes"
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
            rss_ready = read_tree_rss(process.pid)
            peak = {"rss": rss_ready or 0}
            sampler_stop = threading.Event()

            def sample_rss():
                while not sampler_stop.is_set():
                    rss = read_tree_rss(process.pid)
                    if rss and rss > peak["rss"]:
                        peak["rss"] = rss
                    sampler_stop.wait(0.025)

            sampler = threading.Thread(target=sample_rss, daemon=True)
            sampler.start()
            try:
                elapsed, latencies, errors = run_clients(
                    port, requests, concurrency, timeout)
            finally:
                sampler_stop.set()
                sampler.join(timeout=2.0)
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
            rss_peak = peak["rss"] or rss_ready
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
                "rss_ready_mb": (rss_ready / (1024.0 * 1024.0)
                                 if rss_ready else None),
                "rss_peak_mb": (rss_peak / (1024.0 * 1024.0)
                                if rss_peak else None),
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
        f"p99={result['p99_ms']:.2f}ms "
        f"rss={result['rss_ready_mb'] or 0:.1f}->"
        f"{result['rss_peak_mb'] or 0:.1f}MiB"
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


def _read_rss_windows(pid):
    import ctypes
    from ctypes import wintypes

    class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    handle = kernel32.OpenProcess(0x1000 | 0x0400, False, pid)
    if not handle:
        return None
    try:
        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(
                handle, ctypes.byref(counters), counters.cb):
            return None
        return int(counters.WorkingSetSize)
    finally:
        kernel32.CloseHandle(handle)


def _process_tree_windows(pid):
    import ctypes
    from ctypes import wintypes

    class PROCESSENTRY32(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
            ("th32ModuleID", wintypes.DWORD),
            ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD),
            ("pcPriClassBase", ctypes.c_long),
            ("dwFlags", wintypes.DWORD),
            ("szExeFile", ctypes.c_char * 260),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        return [pid]
    parents = {}
    try:
        entry = PROCESSENTRY32()
        entry.dwSize = ctypes.sizeof(entry)
        if not kernel32.Process32First(snapshot, ctypes.byref(entry)):
            return [pid]
        while True:
            parents[entry.th32ProcessID] = entry.th32ParentProcessID
            if not kernel32.Process32Next(snapshot, ctypes.byref(entry)):
                break
    finally:
        kernel32.CloseHandle(snapshot)
    tree = [pid]
    changed = True
    while changed:
        changed = False
        for child, parent in parents.items():
            if parent in tree and child not in tree:
                tree.append(child)
                changed = True
    return tree


def read_rss(pid):
    """Return a process RSS in bytes, or None when unavailable."""
    if os.name == "nt":
        return _read_rss_windows(pid)
    try:
        with open(f"/proc/{pid}/status", "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) * 1024
    except OSError:
        return None
    return None


def read_tree_rss(pid):
    """Return the RSS of a process and its children in bytes, or None."""
    if os.name == "nt":
        total = 0
        found = False
        for item in _process_tree_windows(pid):
            rss = _read_rss_windows(item)
            if rss is not None:
                total += rss
                found = True
        return total if found else None

    parents = {}
    rss_by_pid = {}
    try:
        entries = os.listdir("/proc")
    except OSError:
        return read_rss(pid)
    for entry in entries:
        if not entry.isdigit():
            continue
        item = int(entry)
        try:
            with open(f"/proc/{item}/stat", "r", encoding="utf-8") as handle:
                stat = handle.read()
            close = stat.rfind(")")
            fields = stat[close + 2:].split()
            parents[item] = int(fields[1])
        except (OSError, ValueError, IndexError):
            continue
        rss = read_rss(item)
        if rss is not None:
            rss_by_pid[item] = rss
    tree = [pid]
    changed = True
    while changed:
        changed = False
        for child, parent in parents.items():
            if parent in tree and child not in tree:
                tree.append(child)
                changed = True
    total = sum(rss_by_pid.get(item, 0) for item in tree)
    return total if total > 0 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="./skein",
                        help="path to the skein interpreter")
    parser.add_argument("--mode", choices=["threads", "processes", "both"],
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

    modes = ["threads", "processes"] if args.mode == "both" else [args.mode]
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
