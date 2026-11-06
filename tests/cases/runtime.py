#!/usr/bin/env python3
"""Strings, closures, loops, generators and async.

Extracted from the original test_skein.py main() body, lines 2304-2887.
"""
import os

from harness import *  # noqa: F401,F403


def run_cases():
    default_env = dict(os.environ)
    default_env.pop("SKEIN_VM", None)
    default_env["SKEIN_VM_TRACE"] = "1"
    check(
        "VM is the default backend",
        "print(1 + 2);\n",
        stdout="3",
        stderr="[vm]",
        env=default_env,
    )
    check(
        "VM stats report",
        "print(1 + 2);\n",
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
        "print(down(5000));\n",
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
        "print(sum(20));\n",
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
        "print(f());\n"
        "print(g);\n",
        stdout_all=["5", "5"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "global slot respects local shadowing",
        "let x = 10;\n"
        "func f() {\n"
        "  let x = 1;\n"
        "  if (true) { let x = 2; print(x); }\n"
        "  return x;\n"
        "}\n"
        "print(f());\n"
        "print(x);\n",
        stdout_all=["2", "1", "10"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "index cache key and mutation",
        "let d = {\"a\": 1, \"b\": 2};\n"
        "func get(k) { return d[k]; }\n"
        "print(get(\"a\"));\n"
        "print(get(\"b\"));\n"
        "print(get(\"a\"));\n"
        "d[\"a\"] = 9;\n"
        "print(d[\"a\"]);\n"
        "d[\"c\"] = 4;\n"
        "print(d[\"a\"]);\n"
        "print(d[\"c\"]);\n"
        "let e = d;\n"
        "d[\"a\"] = 7;\n"
        "print(e[\"a\"]);\n"
        "print(d[\"a\"]);\n",
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
        "print(mutate(p, 3));\n"
        "print(p[\"k\"]);\n"
        "print(outer());\n",
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
        "print(f());\n",
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
        "print(add(1, 2));\n"
        "func make(n) { return func() { n = n + 1; return n; }; }\n"
        "let c = make(0);\n"
        "print(c());\n"
        "print(c());\n"
        "func withdefer(x) { defer print(x); return x + 1; }\n"
        "print(withdefer(4));\n"
        "func rec(n) { if (n <= 0) { return 0; } return rec(n - 1) + 1; }\n"
        "print(rec(100));\n",
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
        "print(f());\n"
        "func boom() { { let x = 1; fail \"x\"; } }\n"
        "print(boom() on error(e) { recover 7; });\n"
        "func g(n) {\n"
        "  let total = 0;\n"
        "  let i = 0;\n"
        "  while (i < n) {\n"
        "    { let a = i; let b = a + 1; total = total + b; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return total;\n"
        "}\n"
        "print(g(3));\n",
        stdout_all=["11", "7", "6"],
        env=dict(os.environ, SKEIN_VM="1"),
    )
    check(
        "constant branch folding",
        "func f(x) {\n"
        "  if (1 < 2) { print(\"a\"); } else { print(\"b\"); }\n"
        "  if (false) { print(\"bad\"); } else { print(\"c\"); }\n"
        "  if (null) { print(\"bad\"); } else { print(\"d\"); }\n"
        "  let y = true ? 1 : 2;\n"
        "  print(y);\n"
        "  while (false) { print(\"bad\"); }\n"
        "  if (x) { print(\"x\"); } else { print(\"no\"); }\n"
        "}\n"
        "f(true);\n"
        "f(false);\n",
        stdout_all=["a", "c", "d", "1", "x", "a", "c", "d", "1", "no"],
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_OPT="1"),
    )
    check(
        "constant branch folding disabled",
        "if (false) { print(\"bad\"); } else { print(\"ok\"); }\n"
        "print(1 < 2);\n",
        stdout_all=["ok", "true"],
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_OPT="0"),
    )
    check(
        "slot superinstructions",
        "func calc() {\n"
        "  let a = 7;\n"
        "  let b = 3;\n"
        "  let d = 0;\n"
        "  d = a + b; print(d);\n"
        "  d = a - b; print(d);\n"
        "  d = a * b; print(d);\n"
        "  d = a / b; print(d);\n"
        "  d = a % b; print(d);\n"
        "  d = a & b; print(d);\n"
        "  d = a | b; print(d);\n"
        "  d = a ^ b; print(d);\n"
        "  d = a << 1; print(d);\n"
        "  d = a >> 1; print(d);\n"
        "  let c = 0;\n"
        "  c = c + 2; print(c);\n"
        "  c = c * 3; print(c);\n"
        "  return d;\n"
        "}\n"
        "print(calc());\n",
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
        "print(calc());\n",
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
        "print(params(1));\n"
        "print(slots());\n"
        "print(closure());\n"
        "print(m ^ n);\n"
        "print(~m);\n",
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
        "print(zero());\n"
        "print(add(1, 2, 3, 4, 5, 6, 7, 8));\n"
        "print(add9(1, 2, 3, 4, 5, 6, 7, 8, 9));\n"
        "print(tail(1000, 0));\n"
        "print(dflt(1));\n"
        "print(vadd(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));\n",
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
        "print(c());\n"
        "print(c());\n"
        "print(nested()()());\n",
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
        "print(down(500));\n",
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
        "print(even(400));\n",
        stdout="true",
        env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_MAX_DEPTH="8"),
    )
    check(
        "defer prevents tail frame reuse",
        "func down(n) {\n"
        "  defer print(n);\n"
        "  if (n == 0) { return 0; }\n"
        "  return down(n - 1);\n"
        "}\n"
        "print(down(3));\n",
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
        f"label e\u0301: while (0) {{ break \u00e9; }}\n1\n",
        stdout="= 1",
    )

    with tempfile.TemporaryDirectory() as tmp:
        package_dir = os.path.join(tmp, "app", "\u00e9")
        os.makedirs(package_dir)
        with open(os.path.join(package_dir, "\u00e9.skein"),
                  "w", encoding="utf-8") as f:
            f.write("package \u00e9 { export let value = 11; }\n")
        main_path = os.path.join(tmp, "app", "main.skein")
        with open(main_path, "w", encoding="utf-8") as f:
            f.write("import e\u0301;\nprint(\u00e9.value);\n")
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
    check("const declaration", "const x = 3;\nx\n", stdout="= 3")
    check("const reassignment", "const x = 1;\nx = 2;\n",
          stderr="cannot assign to constant x")
    check(
        "const closure reassignment",
        "const x = 1;\nfunc f() { x = 2; }\nf();\n",
        stderr="cannot assign to constant x",
    )
    check(
        "const binding is shallow",
        "const a = [1, 2];\na[0] = 9;\na\n",
        stdout="= [9, 2]",
    )

    check("string concat", '"a" + "b"\n', stdout="= ab")
    check("string compare", '"a" < "b"\n', stdout="= true")
    check("string equality", '"a" == "a"\n', stdout="= true")
    check("string escape", 'print("a\\nb")\n', stdout="a\nb")
    check("string index", '"hello"[1]\n', stdout="= e")
    check("utf8 string index first", '"你好"[0]\n', stdout="= 你")
    check("utf8 string index second", '"你好"[1]\n', stdout="= 好")
    check("utf8 string index out of range", '"你好"[2]\n',
          stderr="out of range")
    check("string trim", 'trim("  hi\\t\\n")\n', stdout="= hi")
    check("string split", 'split("a,b,c", ",")\n', stdout="= [a, b, c]")
    check("string split empty fields", 'split(",a,", ",")\n',
          stdout="= [, a, ]")
    check("string split no match", 'split("abc", ",")\n', stdout="= [abc]")
    check("string split empty text", 'split("", ",") == [""]\n',
          stdout="= true")
    check("string split empty separator", 'split("a,b", "")\n',
          stderr="split separator must not be empty")
    check("string split whitespace", 'split_ws("  a  b\\tc\\n d ")\n',
          stdout="= [a, b, c, d]")
    check("string split whitespace empty", 'split_ws("")\n',
          stdout="= []")
    check("string find", 'find("hello", "ll")\n', stdout="= 2")
    check("string find missing", 'find("hello", "x")\n', stdout="= -1")
    check("string find empty needle", 'find("hello", "")\n', stdout="= 0")
    check("string find utf8", 'find("你好世界", "世界")\n', stdout="= 2")
    check("string substr", 'substr("hello", 1, 3)\n', stdout="= ell")
    check("string substr to end", 'substr("hello", 2)\n', stdout="= llo")
    check("string substr past end", 'substr("hello", 10)\n', stdout="= ")
    check("string substr clamps length", 'substr("hello", 1, 100)\n',
          stdout="= ello")
    check("string substr utf8", 'substr("你好世界", 1, 2)\n',
          stdout="= 好世")
    check("string substr negative start", 'substr("hello", -1)\n',
          stderr="substr start must be a non-negative integer")
    check("string substr negative length", 'substr("hello", 1, -1)\n',
          stderr="substr length must be a non-negative integer")
    check("string split type", 'split(1, ",")\n',
          stderr="split expects string text and separator")
    check("string split arity", 'split("a")\n',
          stderr="split expects 2 arguments")
    check("string find arity", 'find("a")\n',
          stderr="find expects 2 arguments")
    check("string substr arity", 'substr("a")\n',
          stderr="substr expects 2 or 3 arguments")

    check("list expression", "[1+2, 3*4]\n", stdout="= [3, 12]")
    check("empty list", "[]\n", stdout="= []")
    check("list index", "[1,2][1]\n", stdout="= 2")
    check("nested list read", "let a = [[1,2],[3,4]];\na[0][1]\n",
          stdout="= 2")
    check(
        "list read value semantics",
        "let a = [[1]];\nlet b = a[0];\nb[0] = 9;\na\nb\n",
        stdout_all=["= [[1]]", "= [9]"],
    )
    check(
        "dict read value semantics",
        'let d = {"x": [1,2]};\nlet e = d["x"];\ne[0] = 9;\nd\n',
        stdout="{x: [1, 2]}",
    )
    check(
        "list alias copy on write",
        "let a = [1, 2];\nlet b = a;\nb[0] = 9;\na\nb\n",
        stdout_all=["= [1, 2]", "= [9, 2]"],
    )
    check(
        "dict alias copy on write",
        'let d = {"x": 1};\nlet e = d;\ne["x"] = 9;\nd\ne\n',
        stdout_all=["{x: 1}", "{x: 9}"],
    )
    check(
        "dict alias insert copy on write",
        'let d = {"a": 1};\nlet e = d;\ne["b"] = 2;\nd\ne\n',
        stdout_all=["{a: 1}", "{a: 1, b: 2}"],
    )
    check(
        "nested list alias copy on write",
        "let a = [[1], [2]];\nlet b = a;\nb[0][0] = 9;\na\nb\n",
        stdout_all=["= [[1], [2]]", "= [[9], [2]]"],
    )
    check(
        "function argument copy on write",
        "func mutate(x) { x[0] = 9; return x; }\n"
        "let a = [1, 2];\n"
        "mutate(a)\n"
        "a\n",
        stdout_all=["= [9, 2]", "= [1, 2]"],
    )
    check(
        "closure capture copy on write",
        "func make() {\n"
        "  let a = [1];\n"
        "  func get() { return a; }\n"
        "  return [a, get];\n"
        "}\n"
        "let pair = make();\n"
        "let x = pair[0];\n"
        "x[0] = 9;\n"
        "pair[0]\n"
        "pair[1]()\n",
        stdout_all=["= [1]", "= [1]"],
    )
    check(
        "index side effect falls back to copy",
        "let a = [10,20,30];\n"
        "func f() { a[0] = 99; return 0; }\n"
        "a[f()]\n",
        stdout="= 10",
    )
    check("list equality", "[1,2] == [1,2]\n", stdout="= true")
    check("dict index", '{1:2, 3:4}[3]\n', stdout="= 4")
    check("dict key read", 'let p = {"x": 1, "y": 2};\np["x"] + p["y"]\n',
          stdout="= 3")
    check("dict key assignment", 'let p = {"x": 1};\np["x"] = 5;\np\n',
          stdout="{x: 5}")
    check(
        "nested dict key assignment",
        'let p = {"a": {"b": 2}};\np["a"]["b"] = 3;\np\n',
        stdout="{a: {b: 3}}",
    )
    check("dict key callable", 'let d = {"f": sqrt};\nd["f"](9)\n',
          stdout="= 3.0")
    check("dict key missing", 'let p = {"x": 1};\np["y"]\n',
          stderr="key not found")
    check("dict dot access rejected", 'let p = {"x": 1};\np.x\n',
          stderr='cannot access dict key with \'.\'')
    check("dict dot assignment rejected", 'let p = {"x": 1};\np.x = 2;\n',
          stderr='cannot access dict key with \'.\'')
    check("dict dot increment rejected", 'let p = {"x": 1};\np.x++;\n',
          stderr='cannot access dict key with \'.\'')
    check("float literals unaffected by dot", ".5 + 1\n", stdout="= 1.5")
    check("dict equality", "{1:2} == {1:2}\n", stdout="= true")
    check("dict literal duplicate key last wins", '{1: "a", 1: "b"}\n',
          stdout="{1: b}")
    check("dict float key rejected",
          '{1: "a", 1.0: "b"}\n',
          stderr="dict key must be int, bool, string, char, byte, bytes, or null, got float")
    check("print list", "print([1,2,3])\n", stdout="[1, 2, 3]")
    check("multiline list", "let a = [1,\n 2,\n 3];\na[2]\n", stdout="= 3")
    check("empty dict literal", "{}\n", stdout="= {}")
    check("empty list literal", "[]\n", stdout="= []")
    check("empty dict literal assignment", "let d = {};\nd\n", stdout="= {}")
    check("empty list literal assignment", "let l = [];\nl\n", stdout="= []")
    check("empty dict builtin", "dict()\n", stdout="= {}")
    check("empty list builtin", "list()\n", stdout="= []")
    check("multi-line empty dict", "let d = {\n};\nd\n", stdout="= {}")
    check("comment-only empty dict", "let d = { // comment\n};\nd\n",
          stdout="= {}")
    check("multi-line comment-only empty dict",
          "let d = {\n  // comment\n\n};\nd\n", stdout="= {}")
    check("dict key container rejected", "let d = {{}: 1};\n",
          stderr="dict key must be int, bool, string, char, byte, bytes, or null, got dict")
    check("empty dict inside block statement", "{ {}; }\n", code=0)
    check("multi-line empty if block", "if (1) {\n}\n", code=0)
    check("comment-only empty if block", "if (1) { // comment\n}\n", code=0)
    check("multi-line empty func body", "func f() {\n}\nf()\n", code=0)
    check("multi-line empty package body", "package p {\n}\n", code=0)

    check("list index assignment", "let a = [1,2,3];\na[0] = 9;\na\n",
          stdout="= [9, 2, 3]")
    check("dict insert by assignment", "let d = {1:2};\nd[2] = 5;\nd\n",
          stdout="= {1: 2, 2: 5}")
    check(
        "nested index assignment",
        "let a = [[1,2],[3,4]];\na[0][1] = 99;\na\n",
        stdout="= [[1, 99], [3, 4]]",
    )
    check(
        "nested dict assignment",
        'let d = {"a": dict()};\nd["a"]["b"] = 42;\nd\n',
        stdout="{a: {b: 42}}",
    )
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
        'print(f([2]));\n'
        'print(f([3], 4));\n',
        stdout_all=["3", "7"],
        env=dict(os.environ, SKEIN_VM="1", SKEIN_KEEP_AST="1"),
    )

    check(
        "for loop",
        "for (let i = 0; i < 3; i = i + 1) { print(i); }\n",
        stdout="0\n1\n2\n",
    )
    check(
        "for loop sum",
        "let s = 0;\n"
        "for (let i = 1; i <= 4; i = i + 1) { s = s + i; }\n"
        "s\n",
        stdout="= 10",
    )
    check(
        "for loop break",
        "for (let i = 0; i < 5; i = i + 1) { "
        "if (i == 2) { break; } print(i); }\n",
        stdout="0\n1\n",
    )
    check(
        "for loop continue",
        "for (let i = 0; i < 4; i = i + 1) { "
        "if (i == 1) { continue; } print(i); }\n",
        stdout="0\n2\n3\n",
    )
    check(
        "return inside for",
        "func f(a) {\n"
        "  for (let i = 0; i < 10; i = i + 1) { return a; }\n"
        "}\n"
        "f(7)\n",
        stdout="= 7",
    )
    check("len list", "len([1, 2, 3])\n", stdout="= 3")
    check("len empty list", "len([])\n", stdout="= 0")
    check("len dict", 'len({"a": 1, "b": 2})\n', stdout="= 2")
    check("len utf8 string", 'len("你好a")\n', stdout="= 3")
    check("len bytes", 'len(bytes_from_hex("00ff"))\n', stdout="= 2")
    check("len invalid type", "len(1)\n",
          stderr="len expects string, bytes, list, dict, deque, heap, or set")
    check(
        "for in list",
        "for (let x in [1, 2, 3]) { print(x); }\n",
        stdout="1\n2\n3\n",
    )
    check(
        "for in dict keys",
        'let d = {"a": 1, "b": 2};\n'
        "for (let k in d) { print(k + \"=\" + str(d[k])); }\n",
        stdout="a=1\nb=2\n",
    )
    check(
        "for in utf8 string",
        'for (let ch in "你好") { print(ch); }\n',
        stdout="你\n好\n",
    )
    check(
        "for in bytes",
        'for (let b in bytes_from_hex("00ff")) { print(b); }\n',
        stdout="0\n255\n",
    )
    check(
        "for in break continue",
        "for (let x in [1, 2, 3, 4]) {\n"
        "  if (x == 2) { continue; }\n"
        "  if (x == 4) { break; }\n"
        "  print(x);\n"
        "}\n",
        stdout="1\n3\n",
    )
    check(
        "for in snapshot",
        "let a = [1, 2];\n"
        "for (let x in a) { a[0] = 9; print(x); }\n",
        stdout="1\n2\n",
    )
    check("for in invalid type", "for (let x in 1) { print(x); }\n",
          stderr="cannot iterate int")
    check("for in variable scope", "for (let x in [1]) { }\nx\n",
          stderr="symbol not found x")
    check(
        "for in dict key value",
        'for (let k, v in {"a": 1, "b": 2}) { '
        'print(k + "=" + str(v)); }\n',
        stdout="a=1\nb=2\n",
    )
    check(
        "for in two variables requires dict list",
        "for (let i, v in [10, 20]) { print(v); }\n",
        stderr="two-variable for-in requires dict, got list",
    )
    check(
        "for in two variables requires dict string",
        'for (let i, ch in "ab") { print(ch); }\n',
        stderr="two-variable for-in requires dict, got string",
    )
    check(
        "for in two variables requires dict bytes",
        'for (let i, b in bytes_from_hex("00ff")) { print(b); }\n',
        stderr="two-variable for-in requires dict, got bytes",
    )
    check("for in duplicate loop variables",
          'for (let x, x in {"a": 1}) { }\n',
          stderr="loop variable repeated: x")
    check("for in two variables scope",
          'for (let k, v in {"a": 1}) { }\nk\n',
          stderr="symbol not found k")

    check("do while executes once",
          "let i = 0;\ndo { i = i + 1; } while (0);\ni\n",
          stdout="= 1")
    check("do while condition",
          "let i = 0;\ndo { i = i + 1; } while (i < 3);\ni\n",
          stdout="= 3")
    check("do while single statement",
          "let i = 0;\ndo i = i + 1; while (i < 2);\ni\n",
          stdout="= 2")
    check("do while while on next line",
          "let i = 0;\ndo {\n  i = i + 1;\n}\nwhile (i < 2);\ni\n",
          stdout="= 2")
    check(
        "do while break continue",
        "let i = 0;\n"
        "do {\n"
        "  i = i + 1;\n"
        "  if (i == 2) { continue; }\n"
        "  if (i == 4) { break; }\n"
        "  print(i);\n"
        "} while (i < 10);\n",
        stdout="1\n3\n",
    )
    check("do while return",
          "func f(a) {\n  do { return a; } while (1);\n}\nf(7)\n",
          stdout="= 7")
    check("do while empty body", "do {} while (0);\n", code=0)
    check("do while no trailing semicolon", "do {} while (0)\n", code=0)
    check("range length only", "range(5)\n", stdout="= [0, 1, 2, 3, 4]")
    check("range start and length", "range(-2, 3)\n", stdout="= [-2, -1, 0]")
    check("range positive step", "range(5, 3, 2)\n", stdout="= [5, 7, 9]")
    check("range negative step", "range(5, 3, -2)\n", stdout="= [5, 3, 1]")
    check("range empty", "range(10, 0)\n", stdout="= []")
    check("range len", "len(range(0, 7, 2))\n", stdout="= 7")
    check(
        "for in range",
        "for (let x in range(1, 3)) { print(x); }\n",
        stdout="1\n2\n3\n",
    )
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
        "std.type.module(1) == \"\"\n",
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
        "async await basic",
        "import std;\n"
        "async func answer() { return 42; }\n"
        "let t = answer();\n"
        "await t\n",
        stdout="= 42",
    )
    check(
        "async defer cleanup",
        "import std;\n"
        "async func answer() {\n"
        "  defer print(\"cleanup\");\n"
        "  return 42;\n"
        "}\n"
        "await answer()\n",
        stdout_all=["cleanup", "= 42"],
    )
    check(
        "async await concurrent",
        "import std;\n"
        "async func work(n) {\n"
        '  print("start " + str(n));\n'
        "  await std.task.sleep(50);\n"
        '  print("end " + str(n));\n'
        "  return n;\n"
        "}\n"
        "let a = work(1);\n"
        "let b = work(2);\n"
        "let ra = await a;\n"
        "let rb = await b;\n"
        "print(ra + rb);\n",
        stdout_all=["start 1", "start 2", "end 1", "end 2", "3"],
    )
    check(
        "async await net",
        "import std;\n"
        "async func resolve_one() {\n"
        '  return await std.net.resolve_async("localhost", 1000, "ipv4");\n'
        "}\n"
        "len(await resolve_one()) > 0\n",
        stdout="= true",
    )
    check(
        "await outside async",
        "import std;\n"
        "func bad() { await std.task.sleep(1); }\n"
        "bad();\n",
        stderr="await outside async function",
    )
    check(
        "async cancellation",
        "import std;\n"
        "async func wait_forever() {\n"
        "  await std.task.sleep(1000);\n"
        "  return 1;\n"
        "}\n"
        "let t = wait_forever();\n"
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

    check("std math constants and helpers",
          'import std;\n'
          'print(std.math.pi > 3.14 && std.math.pi < 3.15);\n'
          'print(std.math.e > 2.71 && std.math.e < 2.72);\n'
          'print(std.math.tau > 6.28 && std.math.tau < 6.29);\n'
          'print(std.math.sign(-3));\n'
          'print(std.math.sign(2.5));\n'
          'print(std.math.clamp(5, 1, 3));\n'
          'print(std.math.clamp(1.5, 0, 2));\n'
          'print(std.math.cbrt(-8));\n',
          stdout_all=["true", "true", "true", "-1", "1", "3", "1.5", "-2.0"])
    check("std math remainder variants",
          'import std;\n'
          'print(std.math.fmod(7, 3));\n'
          'print(std.math.fmod(-7, 3));\n'
          'print(std.math.mod(7, 3));\n'
          'print(std.math.mod(-7, 3));\n'
          'print(std.math.mod(7, -3));\n',
          stdout_all=["1.0", "-1.0", "1", "2", "-2"])
    check("std math float classification",
          'import std;\n'
          'let nan = float("nan");\n'
          'let inf = float("inf");\n'
          'print(std.math.is_nan(nan));\n'
          'print(std.math.is_inf(inf));\n'
          'print(std.math.is_finite(1));\n'
          'print(std.math.is_finite(inf));\n'
          'print(std.math.sign(nan));\n',
          stdout_all=["true", "true", "true", "false", "nan"])
    check("std math gcd and lcm",
          'import std;\n'
          'print(std.math.gcd(12, 18));\n'
          'print(std.math.gcd(-12, 18));\n'
          'print(std.math.lcm(4, 6));\n'
          'print(std.math.lcm(0, 5));\n'
          'print(std.math.lcm(-4, 6));\n',
          stdout_all=["6", "6", "12", "0", "12"])
    check("std math angle conversion",
          'import std;\n'
          'print(std.math.degrees(std.math.pi));\n'
          'print(std.math.radians(180));\n',
          stdout_all=["180.0", "3.141592653589793"])
    check("std math domain errors are nan",
          'import std;\n'
          'print(std.math.is_nan(std.math.sqrt(-1)));\n'
          'print(std.math.is_nan(std.math.log(0)));\n'
          'print(std.math.is_nan(std.math.log2(0)));\n'
          'print(std.math.is_nan(std.math.log10(-1)));\n',
          stdout_all=["true", "true", "true", "true"])
    check("std math round negative halves down",
          'import std;\n'
          'print(std.math.round(-1.5));\n'
          'print(std.math.round(-1.4));\n',
          stdout_all=["-2", "-1"])
    check("std math min rejects non-numbers",
          'import std;\nstd.math.min(1, "x");\n',
          stderr="min expects numbers")
    check("std math gcd rejects floats",
          'import std;\nstd.math.gcd(1.5, 2);\n',
          stderr="std.math.gcd expects integers")
