#!/usr/bin/env python3
"""Run a small GC-stress workload against the VM and optional JIT path."""
import os
import subprocess
import sys

BIN = sys.argv[1]

SOURCE = """
func boom() { fail "boom"; }
func make(n) {
  let x = n;
  func f() { return x; }
  return f;
}
let i = 0;
let total = 0;
while (i < 2000) {
  let f = make(i);
  total = total + f();
  let v = boom() on error(e) {
    let y = make(i);
    func g() { return y(); }
    total = total + g();
    recover g;
  };
  total = total + v();
  i = i + 1;
}
println(total);
"""


def run(extra):
    env = os.environ.copy()
    env["SKEIN_VM"] = "1"
    env["SKEIN_GC_STRESS"] = "1"
    env["SKEIN_TEST_TIMEOUT"] = "180"
    env.update(extra)
    return subprocess.run(
        [BIN],
        input=SOURCE,
        text=True,
        encoding="utf-8",
        capture_output=True,
        env=env,
        timeout=180,
    )


def check(proc, label):
    if proc.returncode != 0:
        sys.stderr.write(f"{label}: exit {proc.returncode}\n{proc.stderr}")
        return False
    if "5997000" not in proc.stdout:
        sys.stderr.write(f"{label}: unexpected stdout\n{proc.stdout}\n")
        return False
    return True


proc = run({})
ok = check(proc, "vm-stress")
proc = run({"SKEIN_JIT": "1", "SKEIN_JIT_THRESHOLD": "1"})
ok = check(proc, "jit-stress") and ok
sys.exit(0 if ok else 1)
