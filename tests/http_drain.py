#!/usr/bin/env python3
"""HTTP drain: finish in-flight requests, close the listener, exit cleanly."""

import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

BIN = os.path.abspath(sys.argv[1])

SERVER = """import std;

let s = std.http.listen(\"127.0.0.1\", 0);
std.io.println(s[\"port\"]);
std.io.flush(std.io.stdout);

func handle(req) {
    if (req[\"path\"] == \"/slow\") {
        std.task.sleep(1200).wait();
        return {\"status\": 200, \"headers\": {}, \"body\": \"slow-ok\"};
    }
    if (req[\"path\"] == \"/stuck\") {
        std.task.sleep(30000).wait();
        return {\"status\": 200, \"headers\": {}, \"body\": \"late\"};
    }
    return {\"status\": 200, \"headers\": {}, \"body\": \"ok\"};
}

std.http.serve(s, handle);
"""

PROCESS_SERVER = """import std;

let s = std.http.listen(\"127.0.0.1\", 0);
let ps = std.http.serve_processes(
    s, 2, \"@WORKER@\", {\"strategy\": \"reuse_port\"});
std.io.println(s[\"port\"]);
std.io.flush(std.io.stdout);
let served = ps.join();
std.io.print(\"served=\" + str(served) + \"\\n\");
"""

WORKER = """import std;

func handle(req) {
    if (req["path"] == "/slow") {
        std.task.sleep(1200).wait();
        return {"status": 200, "headers": {}, "body": "slow-ok"};
    }
    return {"status": 200, "headers": {}, "body": "ok"};
}
"""

DISPATCH_SERVER = """import std;

let s = std.http.listen(\"127.0.0.1\", 0);
let ps = std.http.serve_processes(
    s, 2, \"@WORKER@\", {\"strategy\": \"dispatch\"});
std.io.println(s[\"port\"]);
std.io.flush(std.io.stdout);
std.task.sleep(1500).wait();
let served = ps.shutdown();
std.io.print(\"served=\" + str(served) + \"\\n\");
"""

DISPATCH_WORKER = """import std;

func handle(req) {
    if (req["path"] == "/slow") {
        std.task.sleep(3000).wait();
        return {"status": 200, "headers": {}, "body": "slow-ok"};
    }
    return {"status": 200, "headers": {}, "body": "ok"};
}
"""


def write_file(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def read_port(proc, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline()
        if line == "":
            if proc.poll() is not None:
                break
            time.sleep(0.02)
            continue
        line = line.strip()
        if line.isdigit():
            return int(line)
    stderr = proc.stderr.read() if proc.stderr else ""
    raise AssertionError(f"server did not report a port\n{stderr}")


def request(port, path, results, timeout=15):
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port}{path}", timeout=timeout) as response:
            results.append((response.status, response.read()))
    except Exception as exc:  # noqa: BLE001 - reported by the caller
        results.append(exc)


def start_server(tmp, source, drain_ms, name):
    script_path = os.path.join(tmp, name)
    write_file(script_path, source)
    env = os.environ.copy()
    env["SKEIN_DRAIN_TIMEOUT_MS"] = str(drain_ms)
    proc = subprocess.Popen(
        [BIN, script_path],
        cwd=tmp,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    return proc


def graceful_drain(tmp):
    proc = start_server(tmp, SERVER, 8000, "server.skein")
    try:
        port = read_port(proc)
        results = []
        client = threading.Thread(
            target=request, args=(port, "/slow", results), daemon=True)
        client.start()
        # Let the request reach the handler before requesting shutdown.
        time.sleep(0.3)

        started = time.time()
        proc.send_signal(signal.SIGTERM)
        client.join(timeout=15)
        if not results:
            raise AssertionError("in-flight request did not finish")
        outcome = results[0]
        if isinstance(outcome, Exception):
            raise AssertionError(f"in-flight request failed: {outcome!r}")
        status, body = outcome
        if status != 200 or body != b"slow-ok":
            raise AssertionError(f"unexpected response: {status} {body!r}")

        stdout, stderr = proc.communicate(timeout=15)
        elapsed = time.time() - started
        if proc.returncode != 0:
            raise AssertionError(
                f"drain exited with {proc.returncode}\n{stdout}\n{stderr}")
        if elapsed > 10:
            raise AssertionError(f"drain took too long: {elapsed:.2f}s")
        if "Traceback" in stderr:
            raise AssertionError(stderr)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()


def drain_deadline(tmp):
    proc = start_server(tmp, SERVER, 300, "stuck_server.skein")
    try:
        port = read_port(proc)
        results = []
        client = threading.Thread(
            target=request, args=(port, "/stuck", results), daemon=True)
        client.start()
        time.sleep(0.3)

        started = time.time()
        proc.send_signal(signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=15)
        elapsed = time.time() - started
        client.join(timeout=5)

        if proc.returncode != 0:
            raise AssertionError(
                f"deadline drain exited with {proc.returncode}\n"
                f"{stdout}\n{stderr}")
        # 300 ms deadline plus the short force grace, not the 30 s handler.
        if elapsed > 5:
            raise AssertionError(
                f"drain deadline was not enforced: {elapsed:.2f}s")
        if "Traceback" in stderr:
            raise AssertionError(stderr)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()


def process_server_drain(tmp):
    worker_path = os.path.join(tmp, "worker.skein")
    write_file(worker_path, WORKER)
    source = PROCESS_SERVER.replace(
        "@WORKER@", worker_path.replace("\\", "\\\\"))
    proc = start_server(tmp, source, 8000, "process_server.skein")
    try:
        port = read_port(proc)
        results = []
        client = threading.Thread(
            target=request, args=(port, "/slow", results), daemon=True)
        client.start()
        time.sleep(0.3)

        started = time.time()
        proc.send_signal(signal.SIGTERM)
        client.join(timeout=15)
        if not results:
            raise AssertionError("in-flight request did not finish")
        outcome = results[0]
        if isinstance(outcome, Exception):
            raise AssertionError(f"in-flight request failed: {outcome!r}")
        status, body = outcome
        if status != 200 or body != b"slow-ok":
            raise AssertionError(f"unexpected response: {status} {body!r}")

        stdout, stderr = proc.communicate(timeout=15)
        elapsed = time.time() - started
        if proc.returncode != 0:
            raise AssertionError(
                f"process drain exited with {proc.returncode}\n"
                f"{stdout}\n{stderr}")
        if elapsed > 10:
            raise AssertionError(
                f"process drain took too long: {elapsed:.2f}s")
        if "Traceback" in stderr:
            raise AssertionError(stderr)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()


def dispatch_shutdown(tmp):
    """ProcessServer.shutdown drains in-flight work on every platform.

    Unlike SIGTERM this path also exists on Windows, where signals cannot be
    delivered to a console process and drain is driven by console events.
    """
    worker_path = os.path.join(tmp, "dispatch_worker.skein")
    write_file(worker_path, DISPATCH_WORKER)
    source = DISPATCH_SERVER.replace(
        "@WORKER@", worker_path.replace("\\", "\\\\"))
    proc = start_server(tmp, source, 8000, "dispatch_server.skein")
    try:
        port = read_port(proc)
        results = []
        client = threading.Thread(
            target=request, args=(port, "/slow", results), daemon=True)
        client.start()
        client.join(timeout=15)
        if not results:
            raise AssertionError("in-flight request did not finish")
        outcome = results[0]
        if isinstance(outcome, Exception):
            raise AssertionError(f"in-flight request failed: {outcome!r}")
        status, body = outcome
        if status != 200 or body != b"slow-ok":
            raise AssertionError(f"unexpected response: {status} {body!r}")

        stdout, stderr = proc.communicate(timeout=15)
        if proc.returncode != 0:
            raise AssertionError(
                f"dispatch shutdown exited with {proc.returncode}\n"
                f"{stdout}\n{stderr}")
        if "served=" not in stdout:
            raise AssertionError(
                f"shutdown did not report served requests\n{stdout}\n{stderr}")
        if "Traceback" in stderr:
            raise AssertionError(stderr)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()


def main():
    with tempfile.TemporaryDirectory(prefix="skein-drain-") as tmp:
        dispatch_shutdown(tmp)
    # Windows has no catchable SIGTERM: Popen.send_signal(SIGTERM) maps to
    # TerminateProcess, so the signal-driven cases only run on POSIX.
    if os.name != "nt":
        with tempfile.TemporaryDirectory(prefix="skein-drain-sig-") as tmp:
            graceful_drain(tmp)
        with tempfile.TemporaryDirectory(prefix="skein-drain-stuck-") as tmp:
            drain_deadline(tmp)
    if sys.platform.startswith("linux") or sys.platform.startswith("freebsd"):
        with tempfile.TemporaryDirectory(prefix="skein-drain-proc-") as tmp:
            process_server_drain(tmp)
    print("http drain tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
