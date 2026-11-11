#!/usr/bin/env python3
"""Stress worker_vm / VM stats across a thread pool."""
import os
import subprocess
import sys
import tempfile

BIN = sys.argv[1]

CPU_WORKER = """
import std;
func handle(payload) {
    let total = 0;
    for (let i = 0; i < 300; i = i + 1) {
        total = total + i;
    }
    return {"v": payload["n"] + total};
}
"""

SUSPEND_WORKER = """
import std;
func inner(n) {
    let data = [n, n + 1, n + 2];
    std.task.sleep(1).wait();
    return data[0] + data[1] + data[2];
}
func handle(payload) {
    return {"v": inner(payload["n"])};
}
"""


def run_case(tmp, name, worker_source, expected, gc_stress):
    worker = os.path.join(tmp, name + ".skein")
    with open(worker, "w", encoding="utf-8") as handle:
        handle.write(worker_source)
    worker_text = worker.replace("\\", "\\\\")
    source = (
        "import std;\n"
        f'let pool = std.thread.pool(4, "{worker_text}", '
        '{"worker_vm": true});\n'
        "let futures = [];\n"
        "for (let i = 0; i < 64; i = i + 1) {\n"
        '  futures = std.list.append(futures, pool.submit({"n": i}));\n'
        "}\n"
        "let total = 0;\n"
        "for (let f in futures) {\n"
        '  let r = f.wait();\n'
        '  if (!r["ok"]) { fail r["error"]; }\n'
        '  total = total + r["value"]["v"];\n'
        "}\n"
        "pool.close();\n"
        "pool.join();\n"
        "println(str(total));\n"
    )
    env = os.environ.copy()
    env["SKEIN_VM"] = "1"
    env["SKEIN_VM_STATS"] = "1"
    env["SKEIN_VM_STATS_QUIET"] = "0"
    env["SKEIN_JIT"] = "1"
    env["SKEIN_JIT_THRESHOLD"] = "5"
    if gc_stress:
        env["SKEIN_GC_STRESS"] = "1"
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
        sys.stderr.write(proc.stdout + proc.stderr)
        return False
    if str(expected) not in proc.stdout:
        sys.stderr.write(proc.stdout + proc.stderr)
        return False
    if "[vm-stats] instructions=" not in proc.stderr:
        sys.stderr.write(proc.stderr)
        return False
    if "ThreadSanitizer" in proc.stderr:
        sys.stderr.write(proc.stderr)
        return False
    return True


def main():
    with tempfile.TemporaryDirectory() as tmp:
        # 64 jobs; each returns n + sum(0..299).
        cpu_total = 64 * 44850 + 64 * 63 // 2
        if not run_case(tmp, "cpu_worker", CPU_WORKER, cpu_total, False):
            return 1
        # 64 jobs; each returns n + (n+1) + (n+2).
        suspend_total = 3 * (64 * 63 // 2) + 64 * 3
        if not run_case(tmp, "suspend_worker", SUSPEND_WORKER, suspend_total,
                        True):
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(main())
