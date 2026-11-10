#!/usr/bin/env python3
"""Long-running memory trend check for thread pools, channels and HTTP.

The child interpreter runs a fixed number of rounds.  After every round the
parent samples the child's resident set size (RSS) and compares the tail of
the run with the post-warmup baseline.  A monotonic leak shows up as a growing
RSS; allocator retention is tolerated by the absolute growth budget.

Examples:

    python tools/soak_memory.py --bin build/skein --mode threads --rounds 300
    python tools/soak_memory.py --bin build/skein --mode both --check

Exit status is 0 unless --check is given and the growth budget is exceeded.
"""
import argparse
import http.client
import os
import statistics
import subprocess
import sys
import tempfile


ROUND_PREFIX = "ROUND "
SERVED_PREFIX = "SERVED "


def read_rss(pid):
    """Return the process RSS in bytes, or None when unavailable."""
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


def skein_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_text(path, text):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def thread_worker_source():
    return (
        "func handle(payload) {\n"
        '    return {"n": payload["n"] + 1};\n'
        "}\n"
    )


def http_worker_source():
    return (
        "func handle(req) {\n"
        '    return {"status": 200, "headers": {"Content-Type": "text/plain"},\n'
        '            "body": bytes_from_string("ok")};\n'
        "}\n"
    )


def thread_driver_source(worker, rounds, workers, jobs):
    return (
        "import std;\n"
        "func soak_round(i) {\n"
        f"  let pool = std.thread.pool({workers}, {skein_string(worker)});\n"
        "  let sum = 0;\n"
        f"  for (let j = 0; j < {jobs}; j = j + 1) {{\n"
        '    let r = pool.submit({"n": j}).wait();\n'
        '    if (r["ok"]) { sum = sum + r["value"]["n"]; }\n'
        "  }\n"
        "  pool.close();\n"
        "  pool.join();\n"
        "  let ch = std.thread.Channel.bounded(1);\n"
        '  ch.send(std.thread.Message.from({"k": i}));\n'
        "  let msg = ch.recv(2000);\n"
        '  if (msg != null) { sum = sum + msg.payload["k"]; }\n'
        "  return sum;\n"
        "}\n"
        f"for (let i = 0; i < {rounds}; i = i + 1) {{\n"
        "  let value = soak_round(i);\n"
        f'  print("{ROUND_PREFIX}"); print(i); print(" "); println(value);\n'
        "  std.io.flush(std.io.stdout);\n"
        "}\n"
    )


def http_driver_source(worker, workers, total_requests):
    return (
        "import std;\n"
        'let server = std.http.listen("127.0.0.1", 0);\n'
        f"let threads = std.http.serve_threads(server, {workers}, "
        f"{skein_string(worker)}, "
        '{"max_requests": ' + str(total_requests) +
        ', "strategy": "dispatch"});\n'
        'println(server["port"]);\n'
        "std.io.flush(std.io.stdout);\n"
        "let served = threads.join();\n"
        f'print("{SERVED_PREFIX}"); println(served);\n'
    )


def describe(samples):
    rss = [s for s in samples if s is not None]
    if not rss:
        return None
    return {
        "count": len(rss),
        "min_mb": min(rss) / (1024.0 * 1024.0),
        "max_mb": max(rss) / (1024.0 * 1024.0),
        "last_mb": rss[-1] / (1024.0 * 1024.0),
    }


def check_trend(samples, max_growth_mb, label):
    rss = [s for s in samples if s is not None]
    if len(rss) < 6:
        print(f"{label}: not enough RSS samples ({len(rss)})")
        return True
    warmup = max(3, len(rss) // 4)
    window = max(3, len(rss) // 6)
    baseline = statistics.median(rss[warmup:warmup + window])
    tail = statistics.median(rss[-window:])
    growth_mb = (tail - baseline) / (1024.0 * 1024.0)
    rounds = max(1, len(rss) - warmup)
    slope_kb = ((tail - baseline) / 1024.0) / rounds
    info = describe(samples)
    print(
        f"{label}: samples={info['count']} "
        f"min={info['min_mb']:.1f}MiB max={info['max_mb']:.1f}MiB "
        f"last={info['last_mb']:.1f}MiB "
        f"baseline={baseline / (1024.0 * 1024.0):.1f}MiB "
        f"tail={tail / (1024.0 * 1024.0):.1f}MiB "
        f"growth={growth_mb:+.1f}MiB slope={slope_kb:+.1f}KiB/round"
    )
    if growth_mb > max_growth_mb:
        print(
            f"{label}: memory growth {growth_mb:+.1f}MiB exceeds the "
            f"{max_growth_mb:.1f}MiB budget"
        )
        return False
    return True


def run_thread_soak(args, tmp):
    worker = os.path.join(tmp, "soak_worker.skein")
    driver = os.path.join(tmp, "soak_threads.skein")
    write_text(worker, thread_worker_source())
    write_text(driver, thread_driver_source(
        worker, args.rounds, args.workers, args.jobs))

    env = os.environ.copy()
    err_path = os.path.join(tmp, "threads.err")
    samples = []
    with open(err_path, "w+", encoding="utf-8") as err_file:
        proc = subprocess.Popen(
            [args.bin, driver],
            stdout=subprocess.PIPE,
            stderr=err_file,
            text=True,
            encoding="utf-8",
            env=env,
        )
        try:
            for line in proc.stdout:
                line = line.strip()
                if not line.startswith(ROUND_PREFIX):
                    continue
                samples.append(read_rss(proc.pid))
            proc.wait(timeout=args.timeout)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        err_file.seek(0)
        stderr = err_file.read()
    if proc.returncode != 0:
        print(f"thread soak failed (exit {proc.returncode}):\n{stderr}")
        return False, samples
    return True, samples


def run_http_soak(args, tmp):
    worker = os.path.join(tmp, "soak_http_worker.skein")
    driver = os.path.join(tmp, "soak_http.skein")
    write_text(worker, http_worker_source())
    total = args.http_rounds * args.http_requests
    write_text(driver, http_driver_source(worker, args.workers, total))

    err_path = os.path.join(tmp, "http.err")
    samples = []
    served = None
    with open(err_path, "w+", encoding="utf-8") as err_file:
        proc = subprocess.Popen(
            [args.bin, driver],
            stdout=subprocess.PIPE,
            stderr=err_file,
            text=True,
            encoding="utf-8",
            env=os.environ.copy(),
        )
        connection = None
        try:
            port_line = proc.stdout.readline().strip()
            if not port_line.isdigit():
                raise RuntimeError(
                    f"serve_threads did not report a port: {port_line!r}")
            port = int(port_line)
            samples.append(read_rss(proc.pid))
            connection = http.client.HTTPConnection(
                "127.0.0.1", port, timeout=args.timeout)
            for _ in range(args.http_rounds):
                for _ in range(args.http_requests):
                    connection.request("GET", "/soak")
                    response = connection.getresponse()
                    body = response.read()
                    if response.status != 200 or body != b"ok":
                        raise RuntimeError(
                            f"unexpected HTTP response {response.status} "
                            f"{body!r}")
                samples.append(read_rss(proc.pid))
            for line in proc.stdout:
                line = line.strip()
                if line.startswith(SERVED_PREFIX):
                    served = int(line[len(SERVED_PREFIX):])
            proc.wait(timeout=args.timeout)
        finally:
            if connection is not None:
                connection.close()
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            err_file.seek(0)
            stderr = err_file.read()
    if proc.returncode != 0:
        print(f"http soak failed (exit {proc.returncode}):\n{stderr}")
        return False, samples
    if served is not None and served != total:
        print(f"http soak served {served} of {total} requests")
        return False, samples
    return True, samples


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", required=True, help="skein executable")
    parser.add_argument(
        "--mode", choices=("threads", "http", "both"), default="threads")
    parser.add_argument("--rounds", type=int, default=200,
                        help="thread pool rounds (default: 200)")
    parser.add_argument("--workers", type=int, default=2,
                        help="thread pool / HTTP worker count")
    parser.add_argument("--jobs", type=int, default=20,
                        help="pool jobs per round")
    parser.add_argument("--http-rounds", type=int, default=100,
                        help="HTTP request rounds")
    parser.add_argument("--http-requests", type=int, default=10,
                        help="requests per HTTP round")
    parser.add_argument("--max-growth-mb", type=float, default=16.0,
                        help="allowed RSS growth after warmup")
    parser.add_argument("--timeout", type=float, default=300.0,
                        help="per-process timeout in seconds")
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero when the growth budget is exceeded")
    args = parser.parse_args(argv)
    if args.rounds <= 0 or args.http_rounds <= 0 or args.http_requests <= 0:
        parser.error("rounds and HTTP counts must be positive")
    return args


def main(argv):
    args = parse_args(argv)
    ok = True
    with tempfile.TemporaryDirectory(prefix="skein-soak-") as tmp:
        if args.mode in ("threads", "both"):
            passed, samples = run_thread_soak(args, tmp)
            ok = passed and check_trend(
                samples, args.max_growth_mb, "threads") and ok
        if args.mode in ("http", "both"):
            passed, samples = run_http_soak(args, tmp)
            ok = passed and check_trend(
                samples, args.max_growth_mb, "http") and ok
    if args.check and not ok:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
