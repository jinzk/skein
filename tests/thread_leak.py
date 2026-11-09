#!/usr/bin/env python3
"""Run worker-pool and thread workloads under LeakSanitizer."""
import os
import subprocess
import sys
import tempfile

BIN = sys.argv[1]

WORKER = """
func handle(payload) {
    return {"v": payload["n"] + 1};
}
func main(ctx) {
    return {"v": ctx.payload["n"] + 2};
}
"""


def main():
    with tempfile.TemporaryDirectory() as tmp:
        worker = os.path.join(tmp, "worker.skein")
        with open(worker, "w", encoding="utf-8") as handle:
            handle.write(WORKER)
        source = (
            "import std;\n"
            f'let pool = std.thread.pool(2, "{worker.replace(chr(92), chr(92) * 2)}");\n'
            'let r1 = pool.submit({"n": 1}).wait();\n'
            'let r2 = pool.submit({"n": 2}).wait();\n'
            "pool.close();\n"
            "pool.join();\n"
            f'let msg = std.thread.Message.from({{"n": 3}});\n'
            f'let t = std.thread.spawn('
            f'"{worker.replace(chr(92), chr(92) * 2)}", msg);\n'
            "let joined = t.join();\n"
            'let src = "func main(ctx) {\\n'
            '    return {\\"v\\": ctx.payload[\\"n\\"] + 2};\\n'
            '}\\n";\n'
            f'let t2 = std.thread.spawn_source(src, '
            f'std.thread.Message.from({{"n": 3}}));\n'
            "let joined2 = t2.join();\n"
            "thread func proto_worker(ctx) {\n"
            '    return {"v": ctx.payload["n"] + 3};\n'
            "}\n"
            'let t3 = std.thread.spawn(proto_worker, '
            'std.thread.Message.from({"n": 3}));\n'
            "let joined3 = t3.join();\n"
            'println("pool=" + str(r1["value"]["v"]) + ":" + '
            'str(r2["value"]["v"]) + " thread=" + '
            'str(joined["value"]["v"]) + " src=" + '
            'str(joined2["value"]["v"]) + " proto=" + '
            'str(joined3["value"]["v"]));\n'
        )
        env = os.environ.copy()
        env.setdefault("ASAN_OPTIONS", "detect_leaks=1")
        proc = subprocess.run(
            [BIN],
            input=source,
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=180,
            env=env,
        )
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            return 1
        expected = "pool=2:3 thread=5 src=5 proto=6"
        if expected not in proc.stdout:
            sys.stderr.write(proc.stdout + proc.stderr)
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(main())
