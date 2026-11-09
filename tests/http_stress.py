#!/usr/bin/env python3
"""Drive repeated HTTP requests through serve_threads and check teardown."""
import os
import subprocess
import sys
import tempfile
import threading
import urllib.request

BIN = sys.argv[1]
REQUESTS = 200
CLIENTS = 8


def main():
    with tempfile.TemporaryDirectory() as tmp:
        worker = os.path.join(tmp, "worker.skein")
        with open(worker, "w", encoding="utf-8") as handle:
            handle.write(
                "func handle(payload) {\n"
                '    return {"status": 200, "body": "ok"};\n'
                "}\n"
            )
        worker_text = worker.replace("\\", "\\\\")
        source = (
            "import std;\n"
            "let base = std.thread.debug_counts();\n"
            'let s = std.http.listen("127.0.0.1", 0);\n'
            f'let ts = std.http.serve_threads(s, 2, "{worker_text}", '
            '{"strategy": "dispatch", "max_requests": 200, '
            '"max_per_connection": 10});\n'
            'println(s["port"]);\n'
            "std.io.flush(std.io.stdout);\n"
            "let served = ts.join(15000);\n"
            "ts = null;\n"
            "std.thread.collect();\n"
            "let after = std.thread.debug_counts();\n"
            'print("served=" + str(served) + "|");\n'
            'print("m=" + str(after["messages"] - base["messages"]) + "|");\n'
            'print("c=" + str(after["channels"] - base["channels"]) + "|");\n'
            'print("d=" + str(after["duplexes"] - base["duplexes"]) + "|");\n'
            'print("e=" + str(after["endpoints"] - base["endpoints"]) + "|");\n'
        )
        server = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        server.stdin.write(source)
        server.stdin.close()
        port_line = server.stdout.readline().strip()
        if not port_line.isdigit():
            sys.stderr.write(port_line + "\n" + server.stderr.read())
            server.kill()
            server.wait()
            return 1
        port = int(port_line)
        failures = []
        lock = threading.Lock()

        def client(count):
            try:
                for _ in range(count):
                    with urllib.request.urlopen(
                            f"http://127.0.0.1:{port}/stress", timeout=10) as r:
                        body = r.read()
                    if r.status != 200 or body != b"ok":
                        raise AssertionError((r.status, body))
            except Exception as exc:
                with lock:
                    failures.append(repr(exc))

        per_client = REQUESTS // CLIENTS
        threads = [threading.Thread(target=client, args=(per_client,))
                   for _ in range(CLIENTS)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        out, err = server.communicate(timeout=30)
        if server.returncode != 0:
            sys.stderr.write(err)
            return 1
        if failures:
            sys.stderr.write(repr(failures) + "\n")
            return 1
        expected = "served=200|m=0|c=0|d=0|e=0|"
        if expected not in out:
            sys.stderr.write(out + err)
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(main())
