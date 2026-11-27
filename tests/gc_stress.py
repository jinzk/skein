#!/usr/bin/env python3
"""Run a small GC-stress workload against the VM and optional JIT path."""
import os
import subprocess
import sys

BIN = sys.argv[1]

SOURCE = """
import std;
func boom() { fail "boom"; }
func make(n) {
  let x = n;
  func f() { return x; }
  return f;
}
func slow_make(v) {
  let t = 0;
  for (let i in range(2000)) { t = t + i; }
  return make(v);
}
func slow_true(v) {
  let t = 0;
  for (let i in range(2000)) { t = t + i; }
  return true;
}
func slow_key(v) {
  let t = 0;
  for (let i in range(2000)) { t = t + i; }
  return v;
}
func slow_cmp(a, b) {
  let t = 0;
  for (let i in range(2000)) { t = t + i; }
  if (a < b) { return -1; }
  if (a > b) { return 1; }
  return 0;
}
func make_one(n) { return func(pad) { return n + pad; }; }
func slow_make_one(v) {
  let t = 0;
  for (let i in range(2000)) { t = t + i; }
  return make_one(v);
}
func apply_pair(a, b, pad) { return a(pad) + b(pad); }
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
println("loop " + str(total));

/* Builtin accumulators keep user values only in C++ locals while the
 * callback runs; they must be registered as GC roots. */
let mapped = std.list.map([1, 2, 3], slow_make);
let map_total = 0;
for (let f in mapped) { map_total = map_total + f(); }
println("map " + str(map_total));

let kept = std.list.filter([1, 2, 3], slow_true);
println("filter " + str(std.list.len(kept)));

println("reduce " + str(std.list.reduce([1, 2, 3], func(a, b) { return a + b; }, 0)));

let uniq = std.list.unique_by([1, 1, 2, 3], slow_key);
println("unique " + str(std.list.len(uniq)));

let sorted = std.list.sort([3, 1, 2], slow_cmp);
println("sort " + str(sorted));

let top = std.list.top_k([5, 4, 3], 2, slow_cmp);
println("top " + str(top));

let groups = std.list.group_by([1, 2, 3], func(v) { return v % 2; });
println("groups " + str(std.list.len(std.dict.keys(groups))));

func stage(n) {
  let x = n;
  return func(f) { return func() { return f() + x; }; };
}
let chain = std.fn.compose(stage(100), stage(200));
let chained = chain(func() { return 1; })();
println("compose " + str(chained));

let captured = std.result.capture(func() {
  let x = 9;
  return func() { return x; };
});
println("capture " + str(std.result.value(captured)()));

/* Closure upvalue cells: closed after the creator returns, and open while
 * the creator frame is still running. */
func make_counter(start) {
  let n = start;
  func step() { n = n + 1; return n; }
  return step;
}
let counter = make_counter(0);
let counter_total = 0;
let k = 0;
while (k < 200) { counter_total = counter_total + counter(); k = k + 1; }
println("closure " + str(counter_total));

func open_cell_total() {
  let n = 0;
  func step() { n = n + 1; return n; }
  let total = 0;
  let i = 0;
  while (i < 100) { total = total + step(); i = i + 1; }
  return total;
}
println("open " + str(open_cell_total()));

func make_many(start) {
  let n = start;
  func bump() { n = n + 1; return n; }
  return bump;
}
let made = 0;
let m = 0;
while (m < 200) {
  let c = make_many(m);
  made = made + c();
  m = m + 1;
}
println("make " + str(made));

/* Iterator state keeps the iterable reachable for the loop's lifetime. */
func iter_total(n) {
  let xs = [];
  let i = 0;
  while (i < n) { xs = std.list.append(xs, i); i = i + 1; }
  let total = 0;
  for (let x in xs) { total = total + x; }
  for (let k, v in {"a": 1, "b": 2}) { total = total + len(k) + v; }
  return total;
}
let iter_sum = 0;
let it = 0;
while (it < 200) { iter_sum = iter_sum + iter_total(4); it = it + 1; }
println("iter " + str(iter_sum));

/* Handlers keep the captured error and recover value alive across unwinds. */
func handler_fail(n) {
  if (n > 0) { fail "handler boom"; }
  return n;
}
func handler_total(n) {
  let total = 0;
  let i = 0;
  while (i < n) {
    total = total + (handler_fail(i) on error(e) { recover 1; });
    i = i + 1;
  }
  return total;
}
println("handler " + str(handler_total(50)));

/* Deferred assignments capture slots; their cells must stay rooted. */
func defer_total(n) {
  let total = 0;
  let i = 0;
  while (i < n) {
    defer total = total + i;
    i = i + 1;
  }
  return total;
}
println("defer " + str(defer_total(50)));

/* Lazy iterators and generator parameters live in C++ state until the
 * generator runs; they must be registered on the generator value. */
func make_adder(n) {
  let x = n + 100;
  return func(v) { return v + x; };
}
let lazy = std.iter.map([1, 2, 3], make_adder(10));
let lazy_total = 0;
std.iter.for_each(lazy, func(v) { lazy_total = lazy_total + v; });
println("lazy " + str(lazy_total));

func gen_echo(v) {
  yield v;
  v = null;
  yield 1;
}
let gen = gen_echo(slow_make(5));
let gen_first = std.generator.next(gen);
println("generator " + str(gen_first["value"]()));
std.generator.next(gen);
/* Direct AST calls collect arguments into C++ locals. A nested VM/JIT call
 * must not clear the scope of an earlier closure argument. */
let pair_total = apply_pair(make_one(1), slow_make_one(2), 0);
println("pair " + str(pair_total));

let churn = slow_true(0);
println("churn " + str(churn));
"""

EXPECTED = [
    "loop 5997000",
    "map 6",
    "filter 3",
    "reduce 6",
    "unique 3",
    "sort [1, 2, 3]",
    "top [3, 4]",
    "groups 2",
    "compose 301",
    "capture 9",
    "closure 20100",
    "open 5050",
    "make 20100",
    "iter 2200",
    "handler 49",
    "defer 1275",
    "lazy 336",
    "generator 5",
    "pair 3",
    "churn true",
]


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
    missing = [line for line in EXPECTED if line not in proc.stdout]
    if missing:
        sys.stderr.write(
            f"{label}: missing {missing}\nstdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}\n")
        return False
    return True


proc = run({"SKEIN_JIT": "0"})
ok = check(proc, "vm-stress")
proc = run({"SKEIN_JIT": "1", "SKEIN_JIT_THRESHOLD": "1"})
ok = check(proc, "jit-stress") and ok
sys.exit(0 if ok else 1)
