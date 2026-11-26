#!/usr/bin/env python3
"""Strings, closures, loops, generators and async.

String cases live in tests/skein/runtime_strings.skein.  Range cases live in
tests/skein/runtime_range.skein.  List/dict copy-on-write cases live in
tests/skein/runtime_collections.skein.  Len/list/math cases live in
tests/skein/runtime_values.skein.  Loop/control-flow cases live in
tests/skein/runtime_control.skein.  Const/value-semantics cases live in
tests/skein/runtime_misc.skein.
"""
import os

from harness import *  # noqa: F401,F403


def run_cases():
    run_skein("runtime_strings.skein")
    run_skein("runtime_range.skein")
    run_skein("runtime_collections.skein")
    run_skein("runtime_values.skein")
    run_skein("runtime_control.skein")
    run_skein("runtime_misc.skein")
    check(
        "strings golden output",
        "\"a\" + \"b\"\n"
        "std.string.trim(\"  x  \")\n",
        stdout_all=["= ab", "= x"],
    )
    check(
        "range golden output",
        "range(3)\n",
        stdout="= [0, 1, 2]",
    )
    check(
        "collections golden output",
        "let a = [1, 2];\n"
        "let d = {\"x\": 1};\n"
        "a\n"
        "d\n",
        stdout_all=["= [1, 2]", "{x: 1}"],
    )
    check(
        "values golden output",
        "len([1, 2, 3])\n"
        "std.math.gcd(12, 18)\n",
        stdout_all=["= 3", "= 6"],
    )
    check(
        "control golden output",
        "for (let i = 0; i < 2; i = i + 1) { println(i); }\n",
        stdout="0\n1\n",
    )
    check(
        "misc golden output",
        "const x = 3;\n"
        "x\n",
        stdout="= 3",
    )

    default_env = dict(os.environ)
    default_env.pop("SKEIN_VM", None)
    default_env["SKEIN_VM_TRACE"] = "1"
    default_env["SKEIN_JIT"] = "0"
    check(
        "VM is the default backend",
        "println(1 + 2);\n",
        stdout="3",
        stderr="[vm]",
        env=default_env,
    )
    check(
        "VM stats report",
        "println(1 + 2);\n",
        stdout="3",
        stderr="[vm-stats] instructions=",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_STATS="1",
                 SKEIN_VM_STATS_QUIET="0"),
    )
    check(
        "frame VM deep recursion",
        "func down(n) {\n"
        "  if (n <= 0) { return 0; }\n"
        "  return down(n - 1);\n"
        "}\n"
        "println(down(5000));\n",
        stdout="0",
        env=dict(os.environ, SKEIN_VM="1",
                 SKEIN_VM_MAX_DEPTH="10000"),
    )
    check(
        "VM optimizer can be disabled",
        "struct P { x: int; }\n"
        "func sum(n) {\n"
        "  let p = P { x: 2 };\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < n) { total = total + p.x; i = i + 1; }\n"
        "  return total;\n"
        "}\n"
        "println(sum(20));\n",
        stdout="40",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_OPT="0"),
    )
    check(
        "global slot compound updates",
        "let g = 1;\n"
        "func f() {\n"
        "  g += 2;\n"
        "  g++;\n"
        "  ++g;\n"
        "  return g;\n"
        "}\n"
        "println(f());\n"
        "println(g);\n",
        stdout_all=["5", "5"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "global slot respects local shadowing",
        "let x = 10;\n"
        "func f() {\n"
        "  let x = 1;\n"
        "  if (true) { let x = 2; println(x); }\n"
        "  return x;\n"
        "}\n"
        "println(f());\n"
        "println(x);\n",
        stdout_all=["2", "1", "10"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "loop block shadows without leaking",
        "let x = 100;\n"
        "func f() {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < 3) {\n"
        "    let x = i;\n"
        "    total = total + x;\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return total;\n"
        "}\n"
        "println(f());\n"
        "println(x);\n",
        stdout_all=["3", "100"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "for loop variable shadows without leaking",
        "let x = 5;\n"
        "let total = 0;\n"
        "for (let x in [1, 2, 3]) { total = total + x; }\n"
        "println(total);\n"
        "println(x);\n",
        stdout_all=["6", "5"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "nested closure params capture per call",
        "import std;\n"
        "func build(n) {\n"
        "  let make = func(i) { return func() { return i; }; };\n"
        "  let out = [];\n"
        "  let i = 0;\n"
        "  while (i < n) { out = std.list.append(out, make(i)); i = i + 1; }\n"
        "  return out;\n"
        "}\n"
        "let fs = build(3);\n"
        "println(str(fs[0]() + fs[1]() + fs[2]()));\n",
        stdout_all=["3"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "index cache key and mutation",
        "let d = {\"a\": 1, \"b\": 2};\n"
        "func get(k) { return d[k]; }\n"
        "println(get(\"a\"));\n"
        "println(get(\"b\"));\n"
        "println(get(\"a\"));\n"
        "d[\"a\"] = 9;\n"
        "println(d[\"a\"]);\n"
        "d[\"c\"] = 4;\n"
        "println(d[\"a\"]);\n"
        "println(d[\"c\"]);\n"
        "let e = d;\n"
        "d[\"a\"] = 7;\n"
        "println(e[\"a\"]);\n"
        "println(d[\"a\"]);\n",
        stdout_all=["1", "2", "1", "9", "9", "4", "9", "7"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "index write base kinds",
        "struct S { x: int; }\n"
        "let shared = {\"k\": 0};\n"
        "func mutate(p, n) {\n"
        "  let local = {\"k\": 0};\n"
        "  let items = [0, 0];\n"
        "  let s = S { x: 0 };\n"
        "  let i = 0;\n"
        "  while (i < n) {\n"
        "    local[\"k\"] += 1;\n"
        "    p[\"k\"] += 1;\n"
        "    shared[\"k\"] += 1;\n"
        "    items[0] += 1;\n"
        "    s.x += 1;\n"
        "    i += 1;\n"
        "  }\n"
        "  return [local[\"k\"], p[\"k\"], shared[\"k\"], items[0], s.x];\n"
        "}\n"
        "func outer() {\n"
        "  let captured = {\"k\": 0};\n"
        "  let f = func() { captured[\"k\"] += 1; return captured[\"k\"]; };\n"
        "  f();\n"
        "  f();\n"
        "  return captured[\"k\"];\n"
        "}\n"
        "let p = {\"k\": 0};\n"
        "println(mutate(p, 3));\n"
        "println(p[\"k\"]);\n"
        "println(outer());\n",
        stdout_all=["[3, 3, 3, 3, 3]", "3", "2"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "scope name cache invalidation",
        "let g = 7;\n"
        "func f() {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < 3) {\n"
        "    { let g = i; total = total + g; }\n"
        "    { let g = i + 10; total = total + g; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return total + g;\n"
        "}\n"
        "println(f());\n",
        stdout="43",
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "parameter symbol reuse",
        "func add(a, b) { a = a + 1; b = b + 2; return a + b; }\n"
        "let i = 0;\n"
        "while (i < 100) {\n"
        "  if (add(1, 2) != 6) { fail \"bad\"; }\n"
        "  i = i + 1;\n"
        "}\n"
        "println(add(1, 2));\n"
        "func make(n) { return func() { n = n + 1; return n; }; }\n"
        "let c = make(0);\n"
        "println(c());\n"
        "println(c());\n"
        "func withdefer(x) { defer println(x); return x + 1; }\n"
        "println(withdefer(4));\n"
        "func rec(n) { if (n <= 0) { return 0; } return rec(n - 1) + 1; }\n"
        "println(rec(100));\n",
        stdout_all=["6", "1", "2", "4", "5", "100"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "slot symbol reuse",
        "func f() {\n"
        "  let total = 0;\n"
        "  for (let x in [1, 2, 3]) { total = total + x; }\n"
        "  let recovered = [][0] on error(err) { recover 5; };\n"
        "  return total + recovered;\n"
        "}\n"
        "println(f());\n"
        "func boom() { { let x = 1; fail \"x\"; } }\n"
        "println(boom() on error(e) { recover 7; });\n"
        "func g(n) {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < n) {\n"
        "    { let a = i; let b = a + 1; total = total + b; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return total;\n"
        "}\n"
        "println(g(3));\n",
        stdout_all=["11", "7", "6"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "constant branch folding",
        "func f(x) {\n"
        "  if (1 < 2) { println(\"a\"); } else { println(\"b\"); }\n"
        "  if (false) { println(\"bad\"); } else { println(\"c\"); }\n"
        "  if (null) { println(\"bad\"); } else { println(\"d\"); }\n"
        "  let y = true ? 1 : 2;\n"
        "  println(y);\n"
        "  while (false) { println(\"bad\"); }\n"
        "  if (x) { println(\"x\"); } else { println(\"no\"); }\n"
        "}\n"
        "f(true);\n"
        "f(false);\n",
        stdout_all=["a", "c", "d", "1", "x", "a", "c", "d", "1", "no"],
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_OPT="1"),
    )
    check(
        "constant branch folding disabled",
        "if (false) { println(\"bad\"); } else { println(\"ok\"); }\n"
        "println(1 < 2);\n",
        stdout_all=["ok", "true"],
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_OPT="0"),
    )
    check(
        "slot superinstructions",
        "func calc() {\n"
        "  let a = 7;\n"
        "  let b = 3;\n"
        "  let d = 0;\n"
        "  d = a + b; println(d);\n"
        "  d = a - b; println(d);\n"
        "  d = a * b; println(d);\n"
        "  d = a / b; println(d);\n"
        "  d = a % b; println(d);\n"
        "  d = a & b; println(d);\n"
        "  d = a | b; println(d);\n"
        "  d = a ^ b; println(d);\n"
        "  d = a << 1; println(d);\n"
        "  d = a >> 1; println(d);\n"
        "  let c = 0;\n"
        "  c = c + 2; println(c);\n"
        "  c = c * 3; println(c);\n"
        "  return d;\n"
        "}\n"
        "println(calc());\n",
        stdout_all=["10", "4", "21", "2", "1", "3", "7", "4",
                    "14", "3", "2", "6", "3"],
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_OPT="1"),
    )
    check(
        "slot superinstructions disabled",
        "func calc() {\n"
        "  let a = 7;\n"
        "  let b = 3;\n"
        "  let d = 0;\n"
        "  d = a + b;\n"
        "  d = a - b;\n"
        "  d = a * b;\n"
        "  d = a / b;\n"
        "  d = a % b;\n"
        "  let c = 1;\n"
        "  c = c + 2;\n"
        "  return [d, c];\n"
        "}\n"
        "println(calc());\n",
        stdout="[1, 3]",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_OPT="0"),
    )
    check(
        "opcode coverage extras",
        "func params(a) { a += 2; a++; ++a; return a; }\n"
        "func slots() { let i = 0; i++; ++i; return i; }\n"
        "func closure() {\n"
        "  let x = 1;\n"
        "  let f = func() { x += 2; x++; ++x; return x; };\n"
        "  return f();\n"
        "}\n"
        "let m = 6;\n"
        "let n = 3;\n"
        "println(params(1));\n"
        "println(slots());\n"
        "println(closure());\n"
        "println(m ^ n);\n"
        "println(~m);\n",
        stdout_all=["5", "2", "5", "5", "-7"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "fast call argument buffers",
        "func zero() { return 0; }\n"
        "func add(a, b, c, d, e, f, g, h) {\n"
        "  return a + b + c + d + e + f + g + h;\n"
        "}\n"
        "func add9(a, b, c, d, e, f, g, h, i) {\n"
        "  return add(a, b, c, d, e, f, g, h) + i;\n"
        "}\n"
        "func tail(n, acc) {\n"
        "  if (n <= 0) { return acc; }\n"
        "  return tail(n - 1, acc + 1);\n"
        "}\n"
        "func dflt(a, b = 2) { return a + b; }\n"
        "func vadd(...xs) {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < len(xs)) { total = total + xs[i]; i = i + 1; }\n"
        "  return total;\n"
        "}\n"
        "println(zero());\n"
        "println(add(1, 2, 3, 4, 5, 6, 7, 8));\n"
        "println(add9(1, 2, 3, 4, 5, 6, 7, 8, 9));\n"
        "println(tail(1000, 0));\n"
        "println(dflt(1));\n"
        "println(vadd(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));\n",
        stdout_all=["0", "36", "45", "1000", "3", "55"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "closure captures through upvalues",
        "func counter() {\n"
        "  let n = 0;\n"
        "  return func() { n = n + 1; return n; };\n"
        "}\n"
        "func nested() {\n"
        "  let x = 1;\n"
        "  return func() { return func() { x = x + 1; return x; }; };\n"
        "}\n"
        "let c = counter();\n"
        "println(c());\n"
        "println(c());\n"
        "println(nested()()());\n",
        stdout_all=["1", "2", "2"],
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_TRACE="1"),
        stderr="LOAD_UPVALUE",
    )
    check(
        "tail call reuses the current frame",
        "func down(n) {\n"
        "  if (n <= 0) { return 0; }\n"
        "  return down(n - 1);\n"
        "}\n"
        "println(down(500));\n",
        stdout="0",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_MAX_DEPTH="8"),
    )
    check(
        "mutual tail calls reuse the current frame",
        "func even(n) {\n"
        "  if (n == 0) { return true; }\n"
        "  return odd(n - 1);\n"
        "}\n"
        "func odd(n) {\n"
        "  if (n == 0) { return false; }\n"
        "  return even(n - 1);\n"
        "}\n"
        "println(even(400));\n",
        stdout="true",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_MAX_DEPTH="8"),
    )
    check(
        "defer prevents tail frame reuse",
        "func down(n) {\n"
        "  defer println(n);\n"
        "  if (n == 0) { return 0; }\n"
        "  return down(n - 1);\n"
        "}\n"
        "println(down(3));\n",
        stdout="0\n1\n2\n3\n0\n",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_MAX_DEPTH="8"),
    )
    check(
        "unicode identifier NFC struct",
        f"struct e\u0301 {{ e\u0301: int; }}\n"
        f"let p = \u00e9 {{ \u00e9: 7 }};\np.\u00e9\n",
        stdout="= 7",
    )
    check(
        "unicode identifier NFC label",
        f"e\u0301: while (0) {{ break \u00e9; }}\n1\n",
        stdout="= 1",
    )

    with tempfile.TemporaryDirectory() as tmp:
        package_file = os.path.join(tmp, "app", "\u00e9.skein")
        os.makedirs(os.path.dirname(package_file))
        with open(package_file, "w", encoding="utf-8") as f:
            f.write("package \u00e9 { export let value = 11; }\n")
        main_path = os.path.join(tmp, "app", "main.skein")
        with open(main_path, "w", encoding="utf-8") as f:
            f.write("import e\u0301;\nprintln(\u00e9.value);\n")
        proc = run_file(main_path)
        assert proc.returncode == 0, proc.stderr
        assert "11" in proc.stdout, proc.stdout
    check(
        "unicode emoji identifier rejected",
        "let 😀 = 1;\n",
        stderr="identifier cannot start with U+1F600",
    )
    check(
        "unicode punctuation identifier rejected",
        "let 。x = 1;\n",
        stderr="identifier cannot start with U+3002",
    )
    check(
        "combining mark identifier rejected",
        "let \u0301x = 1;\n",
        stderr="identifier cannot start with U+0301",
    )
    check(
        "combining mark identifier continuation",
        "let e\u0301 = 1;\ne\u0301\n",
        stdout="= 1",
    )
    check(
        "duplicate unicode parameter",
        "func f(变量, 变量) { }\n",
        stderr="duplicate parameter 变量",
    )
    check("unicode error column", "let 变量 = ;\n",
          stderr="1:12: syntax error")
    check("const reassignment", "const x = 1;\nx = 2;\n",
          stderr="cannot assign to constant x")
    check(
        "const closure reassignment",
        "const x = 1;\nfunc f() { x = 2; }\nf();\n",
        stderr="cannot assign to constant x",
    )

    check("utf8 string index out of range", '"你好"[2]\n',
          stderr="out of range")
    check("string split empty separator", 'std.string.split("a,b", "")\n',
          stderr="split separator must not be empty")
    check("string substr negative start", 'std.string.substr("hello", -1)\n',
          stderr="substr start must be a non-negative integer")
    check("string substr negative length", 'std.string.substr("hello", 1, -1)\n',
          stderr="substr length must be a non-negative integer")
    check("string split type", 'std.string.split(1, ",")\n',
          stderr="type error: expected string, got int")
    check("string split arity", 'std.string.split()\n',
          stderr="split expects 1 or 2 arguments")
    check("string split too many arguments",
          'std.string.split("a", ",", "x")\n',
          stderr="expects at most 2 argument")
    check("string find arity", 'std.string.find("a")\n',
          stderr="find expects 2 arguments")
    check("string substr arity", 'std.string.substr("a")\n',
          stderr="substr expects 2 or 3 arguments")

    check("dict key missing", 'let p = {"x": 1};\np["y"]\n',
          stderr="key not found")
    check("dict dot access rejected", 'let p = {"x": 1};\np.x\n',
          stderr='cannot access dict key with \'.\'')
    check("dict dot assignment rejected", 'let p = {"x": 1};\np.x = 2;\n',
          stderr='cannot access dict key with \'.\'')
    check("dict dot increment rejected", 'let p = {"x": 1};\np.x++;\n',
          stderr='cannot access dict key with \'.\'')
    check("dict float keys canonically match ints",
          'let d = {1: "a", 1.0: "b", 2.5: "c", -0.0: "z"};\n'
          'println(d[1]);\nprintln(d[2.5]);\nprintln(d[0.0]);\n'
          'println(len(d));\n',
          stdout_all=["b", "c", "z", "3"])
    check("dict NaN key rejected",
          'import std;\nlet n = std.math.log2(-1);\n{n: 1}\n',
          stderr="dict key must be int, float (not NaN), bool, string, char, byte, bytes, null, an immutable tuple of these, or a hashable struct/enum, got float")
    check("print list", "print([1,2,3])\n", stdout="[1, 2, 3]")
    check("dict key container rejected", "let d = {{}: 1};\n",
          stderr="dict key must be int, float (not NaN), bool, string, char, byte, bytes, null, an immutable tuple of these, or a hashable struct/enum, got dict")
    check("multi-line empty package body", "package p {\n}\n", code=0)

    check("index assignment out of range", "let a = [1];\na[3] = 9;\n",
          stderr="out of range")
    check("index assignment to string", 'let s = "abc";\ns[0] = "z";\n',
          stderr="cannot index-assign into string")
    check("assignment to literal", "1 = 2\n", stderr="cannot assign to this expression")
    check(
        "assignment through call result rejected",
        'func f() { return {"x": 1}; }\nf().x = 2;\n',
        stderr="cannot assign through this expression",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_DEBUG="1"),
        not_stderr="fallback statement",
    )
    check(
        "compound assignment through call result rejected",
        'func f() { return {"x": 1}; }\nf().x += 2;\n',
        stderr="cannot assign through this expression",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_DEBUG="1"),
        not_stderr="fallback statement",
    )
    check(
        "increment through call result rejected",
        'func f() { return [1]; }\nf()[0]++;\n',
        stderr="cannot assign through this expression",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_DEBUG="1"),
        not_stderr="fallback statement",
    )
    check(
        "keep AST keeps parameter metadata",
        'func f([a], b = 1) { return a + b; }\n'
        'println(f([2]));\n'
        'println(f([3], 4));\n',
        stdout_all=["3", "7"],
        env=dict(os.environ, SKEIN_VM="1", SKEIN_KEEP_AST="1"),
    )

    check("len invalid type", "len(1)\n",
          stderr="len expects string, bytes, list, dict, deque, heap, or set")
    check("for in invalid type", "for (let x in 1) { println(x); }\n",
          stderr="cannot iterate int")
    check("for in variable scope", "for (let x in [1]) { }\nx\n",
          stderr="symbol not found x")
    check("for in dict single variable yields entry pair",
          'let d: dict[string, int] = {"a": 1};\n'
          'for e in d { println(e[0] + "=" + str(e[1])); }\n',
          stdout="a=1\n")
    check("for in dict keys via keys",
          'let d: dict[string, int] = {"a": 1};\n'
          'for k in d.keys() { println(k); }\n',
          stdout="a\n")
    check("std.iter.elements dict yields entries",
          "import std;\n"
          'let d: dict[string, int] = {"a": 1};\n'
          'for e in std.iter.elements(d) { println(e[0]); }\n',
          stdout="a\n")
    check(
        "for in two variables requires pair or dict list",
        "for (let i, v in [10, 20]) { println(v); }\n",
        stderr="two-variable for-in over list[int] requires pair/tuple elements",
    )
    check(
        "for in two variables requires pair or dict string",
        'for (let i, ch in "ab") { println(ch); }\n',
        stderr="two-variable for-in over string requires pair/tuple elements",
    )
    check(
        "for in two variables requires pair or dict bytes",
        'for (let i, b in std.bytes.from_hex("00ff")) { println(b); }\n',
        stderr="two-variable for-in over bytes requires pair/tuple elements",
    )
    check("for in duplicate loop variables",
          'for (let x, x in {"a": 1}) { }\n',
          stderr="loop variable repeated: x")
    check("for in two variables scope",
          'for (let k, v in {"a": 1}) { }\nk\n',
          stderr="symbol not found k")

    check("do while while on next line rejected",
          "let i = 0;\ndo {\n  i = i + 1;\n}\nwhile (i < 2);\ni\n",
          stderr="while must be on the same line as the preceding '}'")
    check("range negative length", "range(0, -1)\n",
          stderr="range length must be non-negative")
    check("range zero step", "range(0, 3, 0)\n",
          stderr="range step must not be zero")
    check("range non-integer start", 'range("a", 2)\n',
          stderr="range start must be an integer")
    check("range non-integer length", 'range(0, "a")\n',
          stderr="range length must be an integer")
    check("range non-integer step", 'range(0, 2, 1.5)\n',
          stderr="range step must be an integer")
    check("range arity", "range()\n",
          stderr="range expects 1 to 3 arguments")
    check("range overflow", "range(9223372036854775807, 2)\n",
          stderr="range value overflow")
    check("std import alias", "import std as s;\ns.list.len([1, 2, 3])\n",
          stdout="= 3")
    check(
        "std import multiple aliases",
        "import std as s1;\n"
        "import std as s2;\n"
        "s1.list.len([1]) + s2.list.len([1, 2])\n",
        stdout="= 3",
    )
    check("import alias conflict", "let s = 1;\nimport std as s;\n",
          stderr="import name already exists: s")
    check(
        "std math helpers",
        "import std;\n"
        "std.math.sqrt(9)\nstd.math.sin(0)\nstd.math.min(3, 1, 2)\n",
        stdout_all=["= 3.0", "= 0.0", "= 1"],
    )
    check(
        "std io helpers",
        "import std;\n"
        'std.io.format("{} + {} = {}", 1, 2, 3)\n',
        stdout="= 1 + 2 = 3",
    )
    check(
        "std process helpers",
        'import std;\nstd.process.status("exit 0")\n',
        stdout="= 0",
    )
    check(
        "std type helpers",
        "import std;\n"
        "std.type.kind_of([])\nstd.type.name(1)\n"
        "std.type.package_name(1) == \"\"\n",
        stdout_all=["= list", "= int", "= true"],
    )
    check(
        "std task sleep",
        "import std;\n"
        "let a = std.task.sleep(50);\n"
        "std.task.done(a)\n"
        "std.task.wait(a)\n"
        "std.task.done(a)\n",
        stdout_all=["= false", "= true"],
    )
    check(
        "std task wait_all",
        "import std;\n"
        "let a = std.task.sleep(100);\n"
        "let b = std.task.sleep(100);\n"
        "std.task.wait_all([a, b])\n"
        "std.task.done(a) && std.task.done(b)\n",
        stdout="= true",
    )
    check(
        "std task timeout and cancel",
        "import std;\n"
        "let a = std.task.sleep(200);\n"
        "std.task.wait(a, 50);\n"
        "std.task.cancel(a)\n"
        "std.task.done(a)\n",
        stdout="= true",
        stderr="timed out",
    )
    check(
        "std task cancelled wait",
        "import std;\n"
        "let a = std.task.sleep(200);\n"
        "std.task.cancel(a);\n"
        "std.task.wait(a);\n",
        stderr="task was cancelled",
    )
    check(
        "std task yield",
        "import std;\n"
        "let y = std.task.yield();\n"
        "std.task.done(y)\n"
        "std.task.wait(y)\n"
        "std.task.done(y)\n",
        stdout_all=["= false", "= true"],
    )
    check(
        "std task pending shutdown",
        "import std;\nlet pending = std.task.sleep(10000);\n",
    )
    check(
        "std task advances during net wait",
        "import std;\n"
        'let listener = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
        "let t = std.task.sleep(10);\n"
        "std.net.accept(listener, 50);\n"
        "std.task.done(t)\n"
        "std.net.close(listener);\n",
        stdout="= true",
        stderr="timed out",
    )
    check(
        "task basic",
        "import std;\n"
        "func answer() { return 42; }\n"
        "let t = launch answer();\n"
        "t.wait()\n",
        stdout="= 42",
    )
    check(
        "task defer cleanup",
        "import std;\n"
        "func answer() {\n"
        "  defer println(\"cleanup\");\n"
        "  return 42;\n"
        "}\n"
        "answer()\n",
        stdout_all=["cleanup", "= 42"],
    )
    check(
        "task concurrent",
        "import std;\n"
        "func work(n) {\n"
        '  println("start " + str(n));\n'
        "  std.task.sleep(50).wait();\n"
        '  println("end " + str(n));\n'
        "  return n;\n"
        "}\n"
        "let a = launch work(1);\n"
        "let b = launch work(2);\n"
        "let ra = a.wait();\n"
        "let rb = b.wait();\n"
        "println(ra + rb);\n",
        stdout_all=["start 1", "start 2", "end 1", "end 2", "3"],
    )
    check(
        "task net",
        "import std;\n"
        "func resolve_one() {\n"
        '  return std.net.resolve_task("localhost", 1000, "ipv4").wait();\n'
        "}\n"
        "len(resolve_one()) > 0\n",
        stdout="= true",
    )
    check(
        "task cancellation",
        "import std;\n"
        "func wait_forever() {\n"
        "  std.task.sleep(1000).wait();\n"
        "  return 1;\n"
        "}\n"
        "let t = launch wait_forever();\n"
        "std.task.cancel(t)\n"
        "std.task.wait(t);\n",
        stdout="= true",
        stderr="task was cancelled",
    )
    check(
        "generator next",
        "import std;\n"
        "func numbers() {\n"
        "  yield 1;\n"
        "  yield 2;\n"
        "  yield 3;\n"
        "}\n"
        "let g = numbers();\n"
        "std.generator.next(g)\n"
        "std.generator.next(g)\n"
        "std.generator.next(g)\n"
        "std.generator.next(g)\n"
        "std.generator.done(g)\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "= {done: false, value: 2}",
            "= {done: false, value: 3}",
            "= {done: true, value: null}",
            "= true",
        ],
    )

    check("std math min rejects non-numbers",
          'import std;\nstd.math.min(1, "x");\n',
          stderr="min expects numbers")
    check("std math gcd rejects floats",
          'import std;\nstd.math.gcd(1.5, 2);\n',
          stderr="type error: expected int, got float")
