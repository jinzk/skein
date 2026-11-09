#!/usr/bin/env python3
"""Repeat thread pool / channel workloads and check runtime counters settle."""
import os
import subprocess
import sys
import tempfile

BIN = sys.argv[1]

WORKER = """
import std;
func handle(payload) {
    return {"v": payload["n"] + 1};
}
func main(ctx) {
    let signal = ctx.handles["signal"];
    signal.send(std.thread.Message.from({"v": ctx.payload["n"] + 2}));
    return {"v": ctx.payload["n"] + 2};
}
"""


def main():
    with tempfile.TemporaryDirectory() as tmp:
        worker = os.path.join(tmp, "worker.skein")
        with open(worker, "w", encoding="utf-8") as handle:
            handle.write(WORKER)
        worker_text = worker.replace("\\", "\\\\")
        source = (
            "import std;\n"
            f'func stress_round(worker) {{\n'
            f'  let pool = std.thread.pool(2, "{worker_text}");\n'
            "  for (let i = 0; i < 5; i++) {\n"
            '    let r = pool.submit({"n": i}).wait();\n'
            "  }\n"
            "  pool.close();\n"
            "  pool.join();\n"
            "  let ch = std.thread.Channel(1);\n"
            '  let msg = std.thread.Message.from({"n": 7}, '
            '{"signal": ch});\n'
            f'  let t = std.thread.spawn("{worker_text}", msg);\n'
            "  let signal = ch.recv();\n"
            "  let joined = t.join();\n"
            "  ch.close();\n"
            "}\n"
            "let base = std.thread.debug_counts();\n"
            "for (let i = 0; i < 10; i++) {\n"
            "  stress_round(worker);\n"
            "}\n"
            "std.thread.collect();\n"
            "let after = std.thread.debug_counts();\n"
            'print("m=" + str(after["messages"] - base["messages"]) + "|");\n'
            'print("c=" + str(after["channels"] - base["channels"]) + "|");\n'
            'print("d=" + str(after["duplexes"] - base["duplexes"]) + "|");\n'
            'print("e=" + str(after["endpoints"] - base["endpoints"]) + "|");\n'
            'print("g=" + str(after["collected"] > base["collected"]) + "|");\n'
        )
        env = os.environ.copy()
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        proc = subprocess.run(
            [BIN],
            input=source,
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=300,
            env=env,
        )
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            return 1
        expected = "m=0|c=0|d=0|e=0|"
        if expected not in proc.stdout:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(main())
