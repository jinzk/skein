#!/usr/bin/env python3
"""Lists, dicts, destructuring, LRU, sorting.

Set/deque/heap cases live in tests/skein/collections_containers.skein.
Graph, lru, and list utility cases live in tests/skein/collections_more.skein.
Arena, builder, append/concat, pair producer, and large-dict equality cases
live in tests/skein/collections_mutation.skein.
"""
from harness import *  # noqa: F401,F403


def run_cases():
    run_skein("collections_containers.skein")
    run_skein("collections_more.skein")
    run_skein("collections_mutation.skein")
    check("list unique arity",
          "import std;\nstd.list.unique()\n",
          stderr="std.list.unique expects 1 or 2 arguments")
    check("list unique_by arity",
          "import std;\nstd.list.unique_by([1])\n",
          stderr="std.list.unique_by expects 2 arguments")
    check(
        "collections containers golden",
        "import std;\n"
        "import std.set;\n"
        "import std.deque;\n"
        "let s = set.from_list([2, 1, 3]);\n"
        "std.io.println(set.values(s));\n"
        "std.io.println(std.json.stringify(s));\n"
        "let d = deque.from_list([1, 2]);\n"
        "std.io.println(deque.to_list(d));\n",
        stdout_all=["[2, 1, 3]", "[2,1,3]", "[1, 2]"],
    )
    check(
        "collections more golden",
        "import std;\n"
        "import std.graph;\n"
        "import std.lru;\n"
        "let g = graph.from_edges(2, [[1, 2]]);\n"
        "std.io.println(graph.path(g, 1, 2));\n"
        "std.io.println(std.json.stringify(lru.new(2)));\n",
        stdout_all=["{ok: true, path: [1, 2]}",
                    '{"cap":2,"map":{},"order":[]}'],
    )
    check(
        "collections mutation golden",
        "import std;\n"
        "import std.arena;\n"
        "let s = arena.from_list([1, 2]);\n"
        "std.io.println(arena.values(s));\n",
        stdout="[1, 2]",
    )


    check("dict delete reinsert order",
          "import std;\n"
          "let d = {1: 'a', 2: 'b', 3: 'c'};\n"
          "d = std.dict.delete(d, 2);\n"
          "d[2] = 'z';\n"
          "std.io.println(std.dict.keys(d));\n",
          stdout_all=["[1, 3, 2]"])
    check("dict index_by",
          'import std;\n'
          'struct User { id: int; name: string; }\n'
          'let users = [User { id: 1, name: "a" }, '
          'User { id: 2, name: "b" }];\n'
          'let by_id = std.dict.index_by(users, func(u) { return u.id; });\n'
          'std.io.println(by_id[1].name);\n'
          'std.io.println(by_id[2].name);\n'
          'std.io.println(std.dict.keys(by_id));\n',
          stdout_all=["a", "b", "[1, 2]"])
    check("dict index_by duplicate keys last wins",
          'import std;\n'
          'struct User { id: int; name: string; }\n'
          'let users = [User { id: 1, name: "a" }, '
          'User { id: 1, name: "b" }];\n'
          'let by_id = std.dict.index_by(users, func(u) { return u.id; });\n'
          'std.io.println(by_id[1].name);\n'
          'std.io.println((by_id).length());\n',
          stdout_all=["b", "1"])
    check("dict index_by composite keys",
          'import std;\n'
          'let xs = [{"region": "eu", "id": 1}, '
          '{"region": "us", "id": 1}];\n'
          'let by_key = std.dict.index_by(xs, '
          'func(x) { return (x["region"], x["id"]); });\n'
          'std.io.println(by_key[("eu", 1)]["region"]);\n'
          'std.io.println(by_key[("us", 1)]["region"]);\n',
          stdout_all=["eu", "us"])
    check("dict index_by empty list",
          'import std;\n'
          'std.io.println((std.dict.index_by([], func(x) { return x; })).length());\n',
          stdout_all=["0"])
    check("dict index_by invalid key",
          'import std;\n'
          'std.dict.index_by([1], func(x): any { return [x]; });\n',
          stderr="std.dict.index_by: dict key must be")
    check("dict large indexed reads",
          "import std;\n"
          "let d = {};\n"
          "let i = 0;\n"
          "while (i < 20000) { d[i] = i; i = i + 1; }\n"
          "let s = 0;\n"
          "i = 0;\n"
          "while (i < 100000) {\n"
          "  let k = i % 20000;\n"
          "  s = s + d[k];\n"
          "  d[k] += 1;\n"
          "  i = i + 1;\n"
          "}\n"
          "std.io.println(s);\nstd.io.println(d[0]);\n",
          stdout_all=["1000150000", "5"])


    check("custom struct dict hash",
          'import std;\nimport std.dict_key;\n'
          'struct User { id: int; cache: list[int]; }\n'
          'impl User {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          'let u = User { id: 1, cache: [] };\n'
          'let d = {u: "x"};\n'
          'std.io.println(d[User { id: 1, cache: [] }]);\n'
          'std.io.println(std.dict.has(d, User { id: 1, cache: [1] }));\n'
          'let s = std.set.from_list([u, User { id: 1, cache: [] }]);\n'
          'std.io.println(std.set.size(s));\n',
          stdout_all=["x", "0", "1"])
    check("custom struct dict hash large",
          'import std;\nimport std.dict_key;\n'
          'struct User { id: int; cache: list[int]; }\n'
          'impl User {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          'let d = {};\n'
          'let i = 0;\n'
          'while (i < 40) { d[User { id: i, cache: [] }] = i; i = i + 1; }\n'
          'std.io.println(d[User { id: 20, cache: [] }]);\n'
          'std.io.println((d).length());\n'
          'std.io.println(std.dict.has(d, User { id: 39, cache: [1] }));\n',
          stdout_all=["20", "40", "0"])
    check("custom struct key equality",
          'import std;\nimport std.dict_key;\n'
          'struct User { id: int; cache: list[int]; }\n'
          'impl User {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          'impl User {\n'
          '  func key_eq(self, other): bool { return self.id == other.id; }\n'
          '}\n'
          'let d = {User { id: 1, cache: [] }: "a"};\n'
          'std.io.println(d[User { id: 1, cache: [1] }]);\n'
          'std.io.println(std.dict.has(d, User { id: 1, cache: [1] }));\n'
          'let s = std.set.from_list([User { id: 1, cache: [] }, '
          'User { id: 1, cache: [1] }]);\n'
          'std.io.println(std.set.size(s));\n'
          'let big = {};\nlet i = 0;\n'
          'while (i < 40) { big[User { id: i, cache: [] }] = i; i = i + 1; }\n'
          'std.io.println(big[User { id: 20, cache: [1] }]);\n',
          stdout_all=["a", "1", "1", "20"])
    check("custom hash keeps structural equality without KeyEquatable",
          'import std;\nimport std.dict_key;\n'
          'struct User { id: int; cache: list[int]; }\n'
          'impl User {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          'let d = {User { id: 1, cache: [] }: "a"};\n'
          'std.io.println(std.dict.has(d, User { id: 1, cache: [1] }));\n',
          stdout_all=["0"])
    check("custom key equality in tuple keys",
          'import std;\nimport std.dict_key;\n'
          'struct User { id: int; cache: list[int]; }\n'
          'impl User {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          'impl User {\n'
          '  func key_eq(self, other): bool { return self.id == other.id; }\n'
          '}\n'
          'let d = {(User { id: 1, cache: [] }, "x"): true};\n'
          'std.io.println(d[(User { id: 1, cache: [1] }, "x")]);\n',
          stdout_all=["true"])
    check("custom key equality return type",
          'import std.dict_key;\n'
          'struct Bad { id: int; }\n'
          'impl Bad {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          'impl Bad {\n'
          '  func key_eq(self, other) { return 1; }\n'
          '}\n'
          'let d = {Bad { id: 1 }: 1};\n'
          'std.io.println(d[Bad { id: 1 }]);\n',
          stderr="key_eq must return a bool")
    check("custom key equality reentrancy",
          'import std.dict_key;\n'
          'struct A { id: int; }\n'
          'struct B { id: int; }\n'
          'impl A {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          'impl A {\n'
          '  func key_eq(self, other): bool {\n'
          '    let m = {B { id: 1 }: 1};\n'
          '    let n = {B { id: 1 }: 1};\n'
          '    return m[B { id: 1 }] == 1;\n'
          '  }\n'
          '}\n'
          'impl B {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          'impl B {\n'
          '  func key_eq(self, other): bool { return self.id == other.id; }\n'
          '}\n'
          'let d = {A { id: 1 }: 1};\n'
          'std.io.println(d[A { id: 1 }]);\n',
          stderr="key_eq must not re-enter dict/set key equality")
    check("custom struct dict hash snapshot",
          'import std;\nimport std.dict_key;\n'
          'struct User { id: int; cache: list[int]; }\n'
          'impl User {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          'let u = User { id: 1, cache: [] };\n'
          'let d = {u: 42};\n'
          'u.id = 2;\n'
          'std.io.println(d[User { id: 1, cache: [] }]);\n'
          'std.io.println(std.dict.has(d, User { id: 2, cache: [] }));\n',
          stdout_all=["42", "0"])
    check("custom struct dict hash return type",
          'import std.dict_key;\n'
          'struct Bad { id: int; }\n'
          'impl Bad {\n'
          '  func dict_hash(self) { return "x"; }\n'
          '}\n'
          '{Bad { id: 1 }: 1}\n',
          stderr="dict_hash must return an int")
    check("custom struct dict hash reentrancy",
          'import std.dict_key;\n'
          'struct A { id: int; }\n'
          'struct B { id: int; }\n'
          'impl A {\n'
          '  func dict_hash(self): int {\n'
          '    let m = {B { id: 1 }: 1};\n'
          '    return self.id;\n'
          '  }\n'
          '}\n'
          'impl B {\n'
          '  func dict_hash(self): int { return self.id; }\n'
          '}\n'
          '{A { id: 1 }: 1}\n',
          stderr="dict_hash must not re-enter dict/set key hashing")
    check("custom enum dict hash",
          'import std;\nimport std.dict_key;\n'
          'enum E { A { xs: list[int]; }; B { id: int; }; }\n'
          'impl E {\n'
          '  func dict_hash(self): int {\n'
          '    return match (self) {\n'
          '      case E.A { xs } => 1;\n'
          '      case E.B { id } => id;\n'
          '    };\n'
          '  }\n'
          '}\n'
          'let d = {E.A { xs: [] }: "a", E.B { id: 2 }: "b"};\n'
          'std.io.println(d[E.A { xs: [] }]);\n'
          'std.io.println(std.dict.has(d, E.A { xs: [1] }));\n'
          'let s = std.set.from_list([E.A { xs: [] }, E.A { xs: [] }]);\n'
          'std.io.println(std.set.size(s));\n',
          stdout_all=["a", "0", "1"])

    check("arena handle must be int",
          'import std;\nimport std.arena;\n'
          'arena.get(arena.new(), "x");\n',
          stderr="arena handle must be an int")

    check("control flow in function argument",
          "func f(x) { return x; }\n"
          "func g() {\n"
          "  let r = f(match (1) { case 1 => { return 10; } default => 0; });\n"
          "  return 99;\n"
          "}\n"
          "std.io.println(g());\n",
          stdout="10")
    check("break in function argument",
          "func f(x) { return x; }\n"
          "for (let i in [1, 2, 3]) {\n"
          "  f(match (i) { case 2 => { break; } default => i; });\n"
          "  std.io.println(i);\n"
          "}\n",
          stdout="1\n")
    check("continue in function argument",
          "func f(x) { return x; }\n"
          "for (let i in [1, 2, 3]) {\n"
          "  f(match (i) { case 2 => { continue; } default => i; });\n"
          "  std.io.println(i);\n"
          "}\n",
          stdout="1\n3\n")
    check("break in nested call argument",
          "func f(a, b) { return b; }\n"
          "func g(x) { return x; }\n"
          "for (let i in [1, 2, 3]) {\n"
          "  f(0, g(match (i) { case 2 => { break; } default => i; }));\n"
          "  std.io.println(i);\n"
          "}\n",
          stdout="1\n")
    check("break through match and call temporaries",
          "func f(x) { return x; }\n"
          "for (let i in [1, 2, 3]) {\n"
          "  let x = match (i) {\n"
          "    case 2 => f(match (i) { case 2 => { break; } default => i; });\n"
          "    default => i;\n"
          "  };\n"
          "  std.io.println(i);\n"
          "}\n",
          stdout="1\n")
    check("deep recursive struct print",
          "struct Node { value: int; next: Node?; }\n"
          "let n = null;\n"
          "for (let i in std.utils.range(2000)) { n = Node { value: i, next: n }; }\n"
          "std.io.println(\"built\");\n"
          "std.io.println(n == n);\n"
          "std.io.println(n.value);\n"
          "let s = str(n);\n"
          "std.io.println((s).length() > 0);\n",
          stdout_all=["built", "true", "1999", "true"])

    check("unary minus", "-3+5\n", stdout="= 2")
    check("unary minus alone", "-3\n", stdout="= -3")
    check("abs negative", "std.math.abs(-3)\n", stdout="= 3")
    check("abs overflow", "std.math.abs(-9223372036854775807 - 1)\n",
          stderr="integer overflow")
    check("trig functions",
          "std.math.sin(0)\nstd.math.cos(0)\nstd.math.tan(0)\n"
          "std.math.asin(1)\nstd.math.acos(1)\nstd.math.atan(1)\nstd.math.atan2(1, 1)\n",
          stdout_all=["= 0.0", "= 1.0", "= 0.0", "= 1.5707963267948966",
                      "= 0.0", "= 0.7853981633974483",
                      "= 0.7853981633974483"])
    check("math power and logs",
          "std.math.pow(2, 10)\nstd.math.log2(8)\nstd.math.log10(100)\nstd.math.hypot(3, 4)\n",
          stdout_all=["= 1024.0", "= 3.0", "= 2.0", "= 5.0"])
    check("math rounding",
          "std.math.floor(1.9)\nstd.math.ceil(1.1)\nstd.math.round(-1.5)\nstd.math.trunc(-1.9)\n",
          stdout_all=["= 1", "= 2", "= -2", "= -1"])
    check("math min max",
          "std.math.min(3, 1, 2)\nstd.math.max(2.5, 1)\nstd.math.min(1)\nstd.math.max(1, 2.0)\n",
          stdout_all=["= 1", "= 2.5", "= 1", "= 2.0"])
    check("math type error",
          'std.math.sin("x")\n', stderr="type error: expected number, got string")
    check("math arity error",
          "std.math.atan2(1)\n", stderr="atan2 expects 2 arguments, got 1")
    check("math variadic arity",
          "std.math.min()\n", stderr="min expects at least 1 argument")
    check("math rounding overflow",
          "std.math.floor(1e100)\n", stderr="floor result out of integer range")

    check("nan equality is false", "std.math.sqrt(-1) == 1\n", stdout="= false")
    check("nan inequality is true", "std.math.sqrt(-1) != 1\n", stdout="= true")
    check("nan is not equal to itself", "std.math.sqrt(-1) == std.math.sqrt(-1)\n",
          stdout="= false")
    check("nan ordering is false", "std.math.sqrt(-1) >= 1\n", stdout="= false")

    check("divide by zero int", "1/0\n", stderr="division by zero")
    check("divide by zero float", "1.0/0\n", stderr="division by zero")

    check("if float truthy", 'if (1.5) { std.io.println("yes"); }\n', stdout="yes")
    check("if else", 'if (0) { std.io.println("yes"); } else { std.io.println("no"); }\n', stdout="no")

    check("zero-arg function", "func f() {\n  return 42;\n}\nf()\n", stdout="= 42")
    check(
        "return inside while",
        "func f(a) {\n  while (1) { return a; }\n}\nf(7)\n",
        stdout="= 7",
    )
    check(
        "recursion",
        "func fib(n) {\n"
        "  if (n < 2) { return n; }\n"
        "  return fib(n-1) + fib(n-2);\n"
        "}\n"
        "fib(10)\n",
        stdout="= 55",
    )
    check(
        "function value alias",
        "func f(a) { return a; }\nlet g = f;\nlet h = g;\nh(8)\n",
        stdout="= 8",
    )
    check(
        "anonymous function",
        "let double = func(x) { return x * 2; };\ndouble(21)\n",
        stdout="= 42",
    )
    check(
        "anonymous immediate call",
        "(func(x) { return x + 1; })(41)\n",
        stdout="= 42",
    )
    check(
        "anonymous higher order",
        "func apply(fn, x) { return fn(x); }\n"
        "apply(func(n) { return n * n; }, 9)\n",
        stdout="= 81",
    )
    check(
        "anonymous closure",
        "func make_adder(n) { return func(x) { return x + n; }; }\n"
        "let add5 = make_adder(5);\n"
        "add5(3)\n",
        stdout="= 8",
    )
    check(
        "anonymous recursion",
        "let fact = func(n) {\n"
        "  if (n <= 1) { return 1; }\n"
        "  return n * fact(n - 1);\n"
        "};\n"
        "fact(5)\n",
        stdout="= 120",
    )
    check(
        "anonymous function in list",
        "let fs = [func(x) { return x + 1; }, func(x) { return x * 2; }];\n"
        "fs[1](5)\n",
        stdout="= 10",
    )
    check(
        "anonymous duplicate parameter",
        "func(a, a) { return a; }\n",
        stderr="duplicate parameter a in function <anonymous>",
    )
    check(
        "anonymous arity",
        "func(a) { return a; }(1, 2)\n",
        stderr="function expects 1 argument(s), got 2",
    )
    check(
        "duplicate parameter",
        "func f(a,a) { return a; }\n",
        stderr="duplicate parameter",
    )
    check(
        "closure counter",
        "func make_counter() {\n"
        "  let n = 0;\n"
        "  func inc() { n = n + 1; return n; }\n"
        "  return inc;\n"
        "}\n"
        "let c1 = make_counter();\n"
        "let c2 = make_counter();\n"
        "c1()\n"
        "c1()\n"
        "c2()\n",
        stdout_all=["= 1", "= 2"],
    )
    check(
        "closure shared environment",
        "func make_pair() {\n"
        "  let n = 0;\n"
        "  func inc() { n = n + 1; return n; }\n"
        "  func get() { return n; }\n"
        "  let d = std.dict.new();\n"
        "  d[\"inc\"] = inc;\n"
        "  d[\"get\"] = get;\n"
        "  return d;\n"
        "}\n"
        "let p = make_pair();\n"
        "p[\"inc\"]()\n"
        "p[\"inc\"]()\n"
        "p[\"get\"]()\n",
        stdout_all=["= 1", "= 2"],
    )
    check(
        "closure factory",
        "func adder(x) {\n"
        "  func add(y) { return x + y; }\n"
        "  return add;\n"
        "}\n"
        "let a = adder(10);\n"
        "a(5)\n"
        "adder(3)(4)\n",
        stdout_all=["= 15", "= 7"],
    )
    check(
        "closure recursion",
        "func make_fact() {\n"
        "  func fact(n) {\n"
        "    if (n < 2) { return 1; }\n"
        "    return n * fact(n-1);\n"
        "  }\n"
        "  return fact;\n"
        "}\n"
        "let f = make_fact();\n"
        "f(5)\n",
        stdout="= 120",
    )
    check(
        "closure from block",
        "let c = 0;\n"
        "{\n"
        "  let x = 5;\n"
        "  func f() { return x; }\n"
        "  c = f;\n"
        "}\n"
        "c()\n",
        stdout="= 5",
    )
    check(
        "cycle collection preserves reachable closures",
        "func make_counter() {\n"
        "  let n = 0;\n"
        "  func inc() { n = n + 1; return n; }\n"
        "  return inc;\n"
        "}\n"
        "let c = make_counter();\n"
        + "1;\n" * 100
        + "c()\n"
        "c()\n",
        stdout_all=["= 1", "= 2"],
    )
    check(
        "cycle creation does not break execution",
        "func make_cycle() {\n"
        "  func f() { return f; }\n"
        "  return f;\n"
        "}\n"
        + "{ let g = make_cycle(); }\n" * 100
        + 'std.io.println("ok");\n',
        stdout="ok",
    )
    check(
        "function value in list",
        "func adder(x) { func add(y) { return x + y; } return add; }\n"
        "let fs = [adder(1), adder(2)];\n"
        "fs[0](10)\n"
        "fs[1](10)\n",
        stdout_all=["= 11", "= 12"],
    )
    check("underscore identifier", "let my_value = 3;\nmy_value\n",
          stdout="= 3")
    check("unicode identifier", "let 变量 = 3;\n变量\n", stdout="= 3")
    check("unicode accented identifier", "let café = 5;\ncafé\n",
          stdout="= 5")
    check("unicode greek identifier", "let λ = 2;\nλ\n", stdout="= 2")
    check("unicode mixed identifier", "let foo变量 = 4;\nfoo变量\n",
          stdout="= 4")
    check(
        "unicode function name and parameters",
        "func 加法(甲, 乙) { return 甲 + 乙; }\n加法(1, 2)\n",
        stdout="= 3",
    )
    check(
        "unicode typed parameter",
        'func 问候(名字: string) { return "你好 " + 名字; }\n问候("世界")\n',
        stdout="= 你好 世界",
    )
    check(
        "unicode variadic parameter",
        "func 求和(...数字) { return (数字).length(); }\n求和(1, 2, 3)\n",
        stdout="= 3",
    )
    check(
        "unicode struct field",
        "struct 点 { x: int; }\nlet p = 点 { x: 1 };\np.x\n",
        stdout="= 1",
    )
    check(
        "unicode label",
        "开始: while (0) { break 开始; }\n1\n",
        stdout="= 1",
    )
    check("unicode keyword-like identifier", "let if变量 = 7;\nif变量\n",
          stdout="= 7")
    check("unicode fullwidth identifier", "let ｉｆ = 8;\nｉｆ\n",
          stdout="= 8")
    check(
        "unicode identifier NFC composed access",
        f"let e\u0301 = 42;\n\u00e9\n",
        stdout="= 42",
    )
    check(
        "unicode identifier NFC decomposed access",
        f"let \u00e9 = 43;\ne\u0301\n",
        stdout="= 43",
    )
    check(
        "unicode identifier NFC duplicate",
        f"let e\u0301 = 1;\nlet \u00e9 = 2;\n",
        stderr="symbol repeated",
    )
    check(
        "unicode identifier NFC function",
        f"func e\u0301() {{ return 9; }}\n\u00e9()\n",
        stdout="= 9",
    )

    # Values reachable only through newer container types must keep their
    # captured scopes alive across the periodic cycle collector.
    def repeated_lets(count=80):
        return "".join(f"let tmp{i} = {i};\n" for i in range(1, count + 1))

    check("enum closure survives cycle collection",
          'import std;\n'
          'enum Box { B { f: Callable; }; }\n'
          'func make() {\n'
          '  let n = 41;\n'
          '  return Box.B { f: func() { return n + 1; } };\n'
          '}\n'
          'let b = make();\n'
          + repeated_lets()
          + 'std.io.println(b.f());\n',
          stdout="42")
    check("lru closure survives cycle collection",
          'import std;\n'
          'func make() {\n'
          '  let n = 41;\n'
          '  let l = lru.new(1);\n'
          '  let r = lru.put(l, "f", func() { return n + 1; });\n'
          '  return r["lru"];\n'
          '}\n'
          'let l = make();\n'
          + repeated_lets()
          + 'let f = lru.get(l, "f")["value"];\n'
          + 'std.io.println(f());\n',
          stdout="42")
    check("result closure survives cycle collection",
          'import std;\n'
          'func make() {\n'
          '  let n = 41;\n'
          '  return std.result.ok(func() { return n + 1; });\n'
          '}\n'
          'let r = make();\n'
          + repeated_lets()
          + 'let f = std.result.value(r);\n'
          + 'std.io.println(f());\n',
          stdout="42")

    check("list append keeps error message",
          "import std;\n"
          "let d = 1;\n"
          "d = std.list.append(d, 2);\n",
          stderr="std.list.append expects a list, got int")
    check("list concat keeps error message",
          "import std;\n"
          "let n = 1;\n"
          "n = n + [2];\n",
          stderr="cannot add int and list")
