#!/usr/bin/env python3
"""Env/cwd isolation and FFI concurrency stress.

Runs:
  * a thread pool where every worker sets a different env value and cwd;
  * a spawned script that must inherit the parent's env/cwd snapshot;
  * a child process started from a pool worker, which must see that
    worker's env/cwd overrides;
  * concurrent ffi.call_task calls with callbacks from pool workers.

The pool size and job count scale through SKEIN_STRESS_ROUNDS so the same
test can back both the regular gate and the nightly soak.
"""

import os
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])
SHIM = (os.path.abspath(sys.argv[2])
        if len(sys.argv) > 2 and sys.argv[2] else None)
ROUNDS = int(os.environ.get("SKEIN_STRESS_ROUNDS", "8"))
WORKERS = int(os.environ.get("SKEIN_STRESS_WORKERS", "4"))


def run_source(source, cwd=None):
    proc = subprocess.run(
        [BIN],
        input=source,
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=180,
        cwd=cwd,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
    return proc


def check(label, proc, expected):
    if proc.returncode != 0:
        print(f"FAIL {label}: rc={proc.returncode}", file=sys.stderr)
        return False
    ok = True
    for text in expected:
        if text not in proc.stdout:
            print(f"FAIL {label}: missing {text!r} in {proc.stdout!r}",
                  file=sys.stderr)
            ok = False
    return ok


def pool_isolation(tmp):
    worker_dir = os.path.join(tmp, "pool")
    os.makedirs(worker_dir, exist_ok=True)
    worker = os.path.join(worker_dir, "worker.skein")
    with open(worker, "w", encoding="utf-8") as handle:
        handle.write(
            "import std;\n"
            "func handle(payload) {\n"
            "  let n = payload[\"n\"];\n"
            "  std.env.set(\"SKEIN_STRESS_ENV\", \"worker-\" + str(n));\n"
            "  std.fs.chdir(payload[\"dir\"]);\n"
            "  std.task.sleep(1).wait();\n"
            "  let out = std.fs.open(\"marker.txt\", \"wb\");\n"
            "  std.io.write_all(out, \"marker-\" + str(n));\n"
            "  std.io.close(out);\n"
            "  return {\"n\": n, \"env\": std.env.get(\"SKEIN_STRESS_ENV\", \"\"),\n"
            "          \"cwd\": std.fs.cwd()};\n"
            "}\n"
        )
    dirs = []
    for i in range(ROUNDS):
        directory = os.path.join(worker_dir, f"w{i}")
        os.makedirs(directory, exist_ok=True)
        dirs.append(directory)
    worker_text = worker.replace("\\", "\\\\")
    submits = "".join(
        f'  futures = std.list.append(futures, pool.submit({{"n": {i}, '
        f'"dir": "{dirs[i].replace(chr(92), "/")}"}}));\n'
        for i in range(ROUNDS)
    )
    source = (
        "import std;\n"
        "std.env.set(\"SKEIN_STRESS_ENV\", \"parent\");\n"
        "let parent_cwd = std.fs.cwd();\n"
        f'let pool = std.thread.pool({WORKERS}, "{worker_text}", '
        '{"worker_vm": true});\n'
        "let futures = [];\n"
        + submits +
        "let bad = 0;\n"
        "for (let f in futures) {\n"
        "  let r = f.wait();\n"
        "  if (!r[\"ok\"]) { fail r[\"error\"]; }\n"
        "  let want = \"worker-\" + str(r[\"value\"][\"n\"]);\n"
        "  if (r[\"value\"][\"env\"] != want) { bad = bad + 1; }\n"
        "}\n"
        "println(\"bad=\" + str(bad));\n"
        "println(\"parent_env=\" + std.env.get(\"SKEIN_STRESS_ENV\", \"\"));\n"
        "println(\"parent_cwd_same=\" + str(std.fs.cwd() == parent_cwd));\n"
        "pool.close();\n"
        "pool.join();\n"
    )
    proc = run_source(source)
    ok = check("pool isolation", proc, [
        "bad=0", "parent_env=parent", "parent_cwd_same=true"])
    for i, directory in enumerate(dirs):
        marker = os.path.join(directory, "marker.txt")
        if not os.path.exists(marker):
            print(f"FAIL pool isolation: missing {marker}", file=sys.stderr)
            ok = False
    return ok


def spawn_inheritance(tmp):
    directory = os.path.join(tmp, "inherit")
    os.makedirs(directory, exist_ok=True)
    worker = os.path.join(directory, "child.skein")
    with open(worker, "w", encoding="utf-8") as handle:
        handle.write(
            "import std;\n"
            "func main(ctx) {\n"
            "  let env = std.env.get(\"SKEIN_INHERIT\", \"\");\n"
            "  let cwd = std.fs.cwd();\n"
            "  std.env.set(\"SKEIN_INHERIT\", \"child\");\n"
            "  return {\"env\": env, \"cwd\": cwd};\n"
            "}\n"
        )
    worker_text = worker.replace("\\", "\\\\")
    directory_text = directory.replace("\\", "/")
    source = (
        "import std;\n"
        "std.env.set(\"SKEIN_INHERIT\", \"parent\");\n"
        f'std.fs.chdir("{directory_text}");\n'
        f'let t = std.thread.spawn_script("{worker_text}", '
        "std.thread.Message.from({}));\n"
        "let r = t.join();\n"
        "if (!r[\"ok\"]) { fail r[\"error\"]; }\n"
        "println(\"child_env=\" + r[\"value\"][\"env\"]);\n"
        "println(\"child_cwd=\" + r[\"value\"][\"cwd\"]);\n"
        "println(\"parent_env=\" + std.env.get(\"SKEIN_INHERIT\", \"\"));\n"
    )
    proc = run_source(source)
    ok = check("spawn inheritance", proc, [
        "child_env=parent", "parent_env=parent"])
    child_cwd = ""
    for line in proc.stdout.splitlines():
        if line.startswith("child_cwd="):
            child_cwd = line[len("child_cwd="):]
    if child_cwd.replace("\\", "/").rstrip("/") != directory_text.rstrip("/"):
        print(f"FAIL spawn inheritance: child cwd {child_cwd!r} != "
              f"{directory_text!r}", file=sys.stderr)
        ok = False
    return ok


def child_process_worker(tmp):
    directory = os.path.join(tmp, "proc")
    os.makedirs(directory, exist_ok=True)
    probe = os.path.join(directory, "probe.skein")
    with open(probe, "w", encoding="utf-8") as handle:
        handle.write(
            "import std;\n"
            "println(\"env=\" + std.env.get(\"SKEIN_CHILD_ENV\", \"\"));\n"
            "println(\"cwd=\" + std.fs.cwd());\n"
        )
    worker = os.path.join(directory, "worker.skein")
    with open(worker, "w", encoding="utf-8") as handle:
        handle.write(
            "import std;\n"
            "func handle(payload) {\n"
            "  std.env.set(\"SKEIN_CHILD_ENV\", \"from-worker\");\n"
            "  std.fs.chdir(payload[\"dir\"]);\n"
            "  let p = std.process.spawn_command(payload[\"bin\"], "
            "{\"args\": [payload[\"probe\"]]});\n"
            "  let out = std.io.read_all(p[\"stdout\"]);\n"
            "  let err = std.io.read_all(p[\"stderr\"]);\n"
            "  let status = std.process.wait(p);\n"
            "  std.io.close(p[\"stdout\"]);\n"
            "  std.io.close(p[\"stderr\"]);\n"
            "  return {\"out\": bytes_to_string(out),\n"
            "          \"err\": bytes_to_string(err),\n"
            "          \"status\": str(status)};\n"
            "}\n"
        )
    worker_text = worker.replace("\\", "\\\\")
    directory_text = directory.replace("\\", "/")
    bin_text = BIN.replace("\\", "/")
    probe_text = probe.replace("\\", "/")
    source = (
        "import std;\n"
        f'let pool = std.thread.pool(1, "{worker_text}", '
        '{"worker_vm": true});\n'
        f'let f = pool.submit({{"dir": "{directory_text}", '
        f'"bin": "{bin_text}", "probe": "{probe_text}"}});\n'
        "let r = f.wait();\n"
        "if (!r[\"ok\"]) { fail r[\"error\"]; }\n"
        "println(r[\"value\"][\"out\"]);\n"
        "println(\"status=\" + r[\"value\"][\"status\"]);\n"
        "println(\"err=\" + r[\"value\"][\"err\"]);\n"
        "pool.close();\n"
        "pool.join();\n"
    )
    proc = run_source(source)
    ok = check("child process env/cwd", proc, ["env=from-worker"])
    child_cwd = ""
    status = ""
    for line in proc.stdout.splitlines():
        if line.startswith("cwd="):
            child_cwd = line[len("cwd="):]
        elif line.startswith("status="):
            status = line[len("status="):]
    if child_cwd.replace("\\", "/").rstrip("/") != directory_text.rstrip("/"):
        print(f"FAIL child process env/cwd: cwd {child_cwd!r} != "
              f"{directory_text!r} (status={status})", file=sys.stderr)
        ok = False
    return ok


def ffi_concurrency(tmp):
    if not SHIM:
        return True
    directory = os.path.join(tmp, "ffi")
    os.makedirs(directory, exist_ok=True)
    worker = os.path.join(directory, "worker.skein")
    with open(worker, "w", encoding="utf-8") as handle:
        handle.write(
            "import std;\n"
            "func handle(payload) {\n"
            "  let lib = std.ffi.open(payload[\"shim\"]);\n"
            "  let Cmp = std.ffi.signature(\"i32\", [\"i32\", \"i32\"]);\n"
            "  let cb = std.ffi.callback(Cmp, func(a, b) { return a - b; });\n"
            "  let value = std.ffi.call_task(lib, \"shim_apply_cmp\", \"i32\", "
            "[Cmp, \"i32\", \"i32\"], [cb, 9, 4]);\n"
            "  std.ffi.close(lib);\n"
            "  return {\"value\": value};\n"
            "}\n"
        )
    worker_text = worker.replace("\\", "\\\\")
    shim_text = SHIM.replace("\\", "/")
    source = (
        "import std;\n"
        f'let pool = std.thread.pool({WORKERS}, "{worker_text}", '
        '{"worker_vm": true});\n'
        "let futures = [];\n"
        f"for (let i = 0; i < {ROUNDS}; i = i + 1) {{\n"
        f'  futures = std.list.append(futures, '
        f'pool.submit({{"shim": "{shim_text}"}}));\n'
        "}\n"
        "let bad = 0;\n"
        "for (let f in futures) {\n"
        "  let r = f.wait();\n"
        "  if (!r[\"ok\"]) { fail r[\"error\"]; }\n"
        "  if (r[\"value\"][\"value\"] != 5) {\n"
        "    bad = bad + 1;\n"
        "    println(\"bad_value=\" + str(r[\"value\"][\"value\"]));\n"
        "  }\n"
        "}\n"
        "println(\"ffi_bad=\" + str(bad));\n"
        "pool.close();\n"
        "pool.join();\n"
    )
    proc = run_source(source)
    ok = check("ffi concurrency", proc, ["ffi_bad=0"])
    if not ok and proc.stderr:
        print("ffi stderr:", proc.stderr, file=sys.stderr)
    return ok


def main():
    with tempfile.TemporaryDirectory(prefix="skein-env-ffi-") as tmp:
        ok = pool_isolation(tmp)
        ok &= spawn_inheritance(tmp)
        ok &= child_process_worker(tmp)
        ok &= ffi_concurrency(tmp)
        if not ok:
            return 1
        print("env/ffi stress passed")
        return 0


if __name__ == "__main__":
    sys.exit(main())
