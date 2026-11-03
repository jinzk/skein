#!/usr/bin/env python3
"""Lists, dicts, destructuring, LRU, sorting.

Extracted from the original test_skein.py main() body, lines 1700-2303.
"""
from harness import *  # noqa: F401,F403


def run_cases():
    check("dict delete reinsert order",
          "import std;\n"
          "let d = {1: 'a', 2: 'b', 3: 'c'};\n"
          "d = std.dict.delete(d, 2);\n"
          "d[2] = 'z';\n"
          "print(std.dict.keys(d));\n",
          stdout_all=["[1, 3, 2]"])
    check("set basics",
          "import std;\nimport std.set;\n"
          "let s = set.from_list([1, 2, 2, 3]);\n"
          "print(set.size(s));\nprint(set.values(s));\n"
          "print(set.has(s, 2));\nprint(set.has(s, 9));\n"
          "s = set.remove(s, 2);\nprint(set.values(s));\n",
          stdout_all=["3", "[1, 2, 3]", "true", "false", "[1, 3]"])
    check("set operations",
          "import std;\nimport std.set;\n"
          "let a = set.from_list([1, 2]);\n"
          "let b = set.from_list([2, 3]);\n"
          "print(set.values(set.union(a, b)));\n"
          "print(set.values(set.intersect(a, b)));\n"
          "print(set.values(set.difference(a, b)));\n"
          "print(set.is_subset(set.from_list([1]), a));\n"
          "print(set.is_subset(set.from_list([1, 9]), a));\n",
          stdout_all=["[1, 2, 3]", "[2]", "[1]", "true", "false"])
    check("deque basics",
          "import std;\nimport std.deque;\n"
          "let d = deque.from_list([2, 3]);\n"
          "d = deque.push_front(d, 1);\n"
          "d = deque.push_back(d, 4);\n"
          "print(deque.size(d));\n"
          "print(deque.to_list(d));\n"
          "print(deque.peek_front(d));\nprint(deque.peek_back(d));\n"
          "let r = deque.pop_front(d);\n"
          "print(r[\"value\"]);\n"
          "d = r[\"deque\"];\n"
          "print(deque.to_list(d));\n",
          stdout_all=["4", "[1, 2, 3, 4]", "1", "4", "1", "[2, 3, 4]"])
    check("deque empty pop",
          "import std;\nimport std.deque;\n"
          "let r = deque.pop_front(deque.new());\n"
          "print(r[\"ok\"]);\nprint(r[\"error\"]);\n",
          stdout_all=["false", "empty deque"])
    check("heap min",
          "import std;\nimport std.heap;\n"
          "let h = heap.from_list([3, 1, 2], func(a, b) { return a < b; });\n"
          "print(heap.size(h));\nprint(heap.peek(h));\n"
          "let r = heap.pop(h); print(r[\"value\"]); h = r[\"heap\"];\n"
          "r = heap.pop(h); print(r[\"value\"]); h = r[\"heap\"];\n"
          "r = heap.pop(h); print(r[\"value\"]); h = r[\"heap\"];\n"
          "print(heap.size(h));\n",
          stdout_all=["3", "1", "1", "2", "3", "0"])
    check("heap max",
          "import std;\nimport std.heap;\n"
          "let h = heap.from_list([3, 1, 2], func(a, b) { return a > b; });\n"
          "print(heap.peek(h));\n"
          "let r = heap.pop(h); print(r[\"value\"]);\n",
          stdout_all=["3", "3"])
    check("heap empty pop",
          "import std;\nimport std.heap;\n"
          "let h = heap.new(func(a, b) { return a < b; });\n"
          "let r = heap.pop(h);\n"
          "print(r[\"ok\"]);\nprint(r[\"error\"]);\n",
          stdout_all=["false", "empty heap"])

    check("route dispatch",
          'import std;\nimport std.route;\n'
          'let r = route.new();\n'
          'r = route.get(r, "/users/:id", func(req) { '
          'return {"status": 200, "body": req["params"]["id"]}; });\n'
          'r = route.get(r, "/users/me", func(req) { '
          'return {"status": 200, "body": "me"}; });\n'
          'print(route.dispatch(r, {"method": "GET", "path": "/users/42"})["body"]);\n'
          'print(route.dispatch(r, {"method": "GET", "path": "/users/me"})["body"]);\n'
          'print(route.dispatch(r, {"method": "POST", "path": "/users/42"})["status"]);\n'
          'print(route.dispatch(r, {"method": "GET", "path": "/nope"})["status"]);\n',
          stdout_all=["42", "me", "405", "404"])
    check("route wildcard",
          'import std;\nimport std.route;\n'
          'let r = route.new();\n'
          'r = route.get(r, "/files/*path", func(req) { '
          'return {"status": 200, "body": req["params"]["path"]}; });\n'
          'print(route.dispatch(r, {"method": "GET", "path": "/files/a/b.txt"})["body"]);\n',
          stdout="a/b.txt")
    check("route middleware",
          'import std;\nimport std.route;\n'
          'let r = route.new();\n'
          'r = route.get(r, "/private", func(req) { '
          'return {"status": 200, "body": "ok"}; });\n'
          'r = route.use(r, func(req) { '
          'if (req["path"] == "/private") { return {"status": 401, "body": "no"}; } '
          'return null; });\n'
          'print(route.dispatch(r, {"method": "GET", "path": "/private"})["status"]);\n'
          'print(route.dispatch(r, {"method": "GET", "path": "/public"})["status"]);\n',
          stdout_all=["401", "404"])
    check("route duplicate",
          'import std;\nimport std.route;\n'
          'let r = route.new();\n'
          'r = route.get(r, "/x", func(req) { return null; });\n'
          'route.get(r, "/x", func(req) { return null; });\n',
          stderr="duplicate route GET /x")
    check("route wildcard method",
          'import std;\nimport std.route;\n'
          'let r = route.new();\n'
          'r = route.add(r, "*", "/any", func(req) { '
          'return {"status": 200, "body": req["method"]}; });\n'
          'print(route.dispatch(r, {"method": "PATCH", "path": "/any"})["body"]);\n',
          stdout="PATCH")

    check("graph basics",
          'import std;\nimport std.graph;\n'
          'let g = graph.new();\n'
          'let r = graph.add_node(g, "a"); g = r["graph"]; let a = r["id"];\n'
          'r = graph.add_node(g, "b"); g = r["graph"]; let b = r["id"];\n'
          'r = graph.add_node(g, "c"); g = r["graph"]; let c = r["id"];\n'
          'r = graph.add_edge(g, a, b, "next"); g = r["graph"];\n'
          'r = graph.add_edge(g, b, c, "next"); g = r["graph"];\n'
          'print(graph.node_count(g));\nprint(graph.edge_count(g));\n'
          'print(graph.neighbors(g, a));\n'
          'print(graph.neighbors(g, a, "next"));\n'
          'print(graph.bfs(g, a));\nprint(graph.dfs(g, a));\n',
          stdout_all=["3", "2", "[2]", "[2]", "[1, 2, 3]", "[1, 2, 3]"])
    check("graph topo and cycle",
          'import std;\nimport std.graph;\n'
          'let g = graph.new();\n'
          'let r = graph.add_node(g, "a"); g = r["graph"]; let a = r["id"];\n'
          'r = graph.add_node(g, "b"); g = r["graph"]; let b = r["id"];\n'
          'r = graph.add_edge(g, a, b); g = r["graph"];\n'
          'print(graph.topo_sort(g)["ok"]);\nprint(graph.has_cycle(g));\n'
          'r = graph.add_edge(g, b, a); g = r["graph"];\n'
          'print(graph.topo_sort(g)["ok"]);\nprint(graph.has_cycle(g));\n',
          stdout_all=["true", "false", "false", "true"])
    check("graph path",
          'import std;\nimport std.graph;\n'
          'let g = graph.new();\n'
          'let r = graph.add_node(g, "a"); g = r["graph"]; let a = r["id"];\n'
          'r = graph.add_node(g, "b"); g = r["graph"]; let b = r["id"];\n'
          'r = graph.add_node(g, "c"); g = r["graph"]; let c = r["id"];\n'
          'r = graph.add_edge(g, a, b); g = r["graph"];\n'
          'r = graph.add_edge(g, b, c); g = r["graph"];\n'
          'print(graph.path(g, a, c));\n'
          'print(graph.path(g, c, a)["ok"]);\n',
          stdout_all=["{ok: true, path: [1, 2, 3]}", "false"])
    check("graph remove node cascades",
          'import std;\nimport std.graph;\n'
          'let g = graph.new();\n'
          'let r = graph.add_node(g, "a"); g = r["graph"]; let a = r["id"];\n'
          'r = graph.add_node(g, "b"); g = r["graph"]; let b = r["id"];\n'
          'r = graph.add_edge(g, a, b); g = r["graph"];\n'
          'g = graph.remove_node(g, a);\n'
          'print(graph.node_count(g));\nprint(graph.edge_count(g));\n',
          stdout_all=["1", "0"])

    check("lru basics",
          'import std;\n'
          'let l = std.lru.new(2);\n'
          'let r = std.lru.put(l, "a", 1); l = r["lru"];\n'
          'r = std.lru.put(l, "b", 2); l = r["lru"];\n'
          'print(std.lru.keys(l));\n'
          'r = std.lru.get(l, "a"); l = r["lru"];\n'
          'print(std.lru.keys(l));\n'
          'r = std.lru.put(l, "c", 3); l = r["lru"];\n'
          'print(r["evicted"]["key"]);\n'
          'print(std.lru.keys(l));\n'
          'let p = std.lru.peek(l, "a")["lru"];\n'
          'print(std.lru.keys(p));\n'
          'r = std.lru.delete(l, "a"); l = r["lru"];\n'
          'print(r["removed"]);\n'
          'print(std.lru.len(l));\n'
          'print(std.lru.capacity(l));\n'
          'print(std.lru.clear(l));\n',
          stdout_all=["[a, b]", "[b, a]", "b", "[a, c]", "[a, c]",
                      "true", "1", "2", "lru(0/2, {})"])
    check("lru get default",
          'import std;\n'
          'let l = std.lru.new(2);\n'
          'let r = std.lru.get(l, "missing", 42);\n'
          'print(r["found"]);\nprint(r["value"]);\n',
          stdout_all=["false", "42"])
    check("lru value semantics",
          'import std;\n'
          'let a = std.lru.new(2);\n'
          'let r = std.lru.put(a, 1, "x");\n'
          'let b = r["lru"];\n'
          'print(std.lru.len(a));\nprint(std.lru.len(b));\n'
          'let r2 = std.lru.put(a, 1, "x");\n'
          'print(b == r2["lru"]);\n',
          stdout_all=["0", "1", "true"])
    check("lru capacity must be positive",
          'import std;\nstd.lru.new(0);\n',
          stderr="std.lru.new capacity must be at least 1")
    check("lru key restrictions",
          'import std;\nlet l = std.lru.new(2);\nstd.lru.get(l, 1.5);\n',
          stderr="std.lru key: dict key must be")
    check("lru json encode rejected",
          'import std;\nstd.json.stringify(std.lru.new(2));\n',
          stderr="cannot encode lru as JSON")

    check("list unique and group_by",
          'import std;\n'
          'print(std.list.unique([1, 2, 1, 3, 2]));\n'
          'print(std.list.unique(["a", "b", "a"]));\n'
          'print(std.list.unique_by([{"n": 1}, {"n": 1}, {"n": 2}], '
          'func(p) { return p["n"]; }));\n'
          'print(std.list.group_by([1, 2, 3, 4], '
          'func(x) { return x % 2; }));\n',
          stdout_all=["[1, 2, 3]", "[a, b]", "[{n: 1}, {n: 2}]",
                      "{1: [1, 3], 0: [2, 4]}"])
    check("list chunk and window",
          'import std;\n'
          'print(std.list.chunk([1, 2, 3, 4, 5], 2));\n'
          'print(std.list.chunk([], 3));\n'
          'print(std.list.window([1, 2, 3, 4, 5], 3));\n'
          'print(std.list.window([1, 2, 3, 4, 5], 3, 2));\n'
          'print(std.list.window([1, 2], 3));\n',
          stdout_all=["[[1, 2], [3, 4], [5]]", "[]",
                      "[[1, 2, 3], [2, 3, 4], [3, 4, 5]]",
                      "[[1, 2, 3], [3, 4, 5]]", "[]"])
    check("list zip and top_k",
          'import std;\n'
          'print(std.list.zip([1, 2, 3], ["a", "b"]));\n'
          'print(std.list.zip([1, 2], ["a", "b"], [true, false]));\n'
          'print(std.list.top_k([5, 1, 4, 2, 3], 3));\n'
          'print(std.list.top_k([5, 1, 4, 2, 3], 2, '
          'func(a, b) { return b - a; }));\n'
          'print(std.list.top_k([1], 0));\n',
          stdout_all=["[[1, a], [2, b]]",
                      "[[1, a, true], [2, b, false]]",
                      "[1, 2, 3]", "[5, 4]", "[]"])
    check("list binary_search",
          'import std;\n'
          'print(std.list.binary_search([1, 3, 5, 7], 5));\n'
          'print(std.list.binary_search([1, 3, 5, 7], 4));\n'
          'print(std.list.binary_search(["a", "c", "e"], "c"));\n'
          'print(std.list.binary_search([], 1));\n',
          stdout_all=["{found: true, index: 2}",
                      "{found: false, index: 2}",
                      "{found: true, index: 1}",
                      "{found: false, index: 0}"])
    check("list chunk size must be positive",
          'import std;\nstd.list.chunk([1], 0);\n',
          stderr="std.list.chunk size must be at least 1")
    check("list window step must be positive",
          'import std;\nstd.list.window([1], 1, 0);\n',
          stderr="std.list.window step must be at least 1")
    check("list top_k k must be non-negative",
          'import std;\nstd.list.top_k([1], -1);\n',
          stderr="std.list.top_k k must be non-negative")
    check("list group_by key restrictions",
          'import std;\nstd.list.group_by([1], func(x) { return 1.5; });\n',
          stderr="std.list.group_by key: dict key must be")

    check("arena add get set remove",
          'import std;\nimport std.arena;\n'
          'let s = arena.new();\n'
          'let r = arena.add(s, "a"); s = r["arena"]; let a = r["id"];\n'
          'r = arena.add(s, "b"); s = r["arena"]; let b = r["id"];\n'
          'print(arena.size(s));\n'
          'print(arena.values(s));\n'
          'print(arena.get(s, a)["value"]);\n'
          'r = arena.set(s, a, "A"); s = r["arena"];\n'
          'print(r["ok"]);\n'
          'r = arena.remove(s, b); s = r["arena"];\n'
          'print(r["removed"]);\n'
          'print(arena.size(s));\n'
          'print(arena.has(s, b));\n'
          'r = arena.add(s, "c"); s = r["arena"]; let c = r["id"];\n'
          'print(arena.capacity(s));\n'
          'print(arena.values(s));\n'
          'print(arena.has(s, c));\n'
          'print(arena.has(s, b));\n',
          stdout_all=["2", "[a, b]", "a", "true", "true", "1",
                      "false", "2", "[A, c]", "true", "false"])
    check("arena stale handle after reuse",
          'import std;\nimport std.arena;\n'
          'let s = arena.new();\n'
          'let r = arena.add(s, 1); s = r["arena"]; let a = r["id"];\n'
          'r = arena.remove(s, a); s = r["arena"];\n'
          'r = arena.add(s, 2); s = r["arena"];\n'
          'print(arena.has(s, a));\n'
          'print(arena.get(s, a)["ok"]);\n'
          'print(arena.values(s));\n',
          stdout_all=["false", "false", "[2]"])
    check("arena value semantics",
          'import std;\nimport std.arena;\n'
          'let s0 = arena.new();\n'
          'let r = arena.add(s0, 1);\n'
          'let s1 = r["arena"]; let a = r["id"];\n'
          'print(arena.size(s0));\nprint(arena.size(s1));\n'
          'let r2 = arena.remove(s1, a);\n'
          'print(arena.size(s1));\nprint(arena.size(r2["arena"]));\n',
          stdout_all=["0", "1", "1", "0"])
    check("arena clear",
          'import std;\nimport std.arena;\n'
          'let s = arena.new();\n'
          'let r = arena.add(s, 1); s = r["arena"];\n'
          's = arena.clear(s);\n'
          'print(arena.size(s));\nprint(arena.values(s));\n',
          stdout_all=["0", "[]"])
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
          "print(g());\n",
          stdout="10")
    check("break in function argument",
          "func f(x) { return x; }\n"
          "for (let i in [1, 2, 3]) {\n"
          "  f(match (i) { case 2 => { break; } default => i; });\n"
          "  print(i);\n"
          "}\n",
          stdout="1\n")
    check("continue in function argument",
          "func f(x) { return x; }\n"
          "for (let i in [1, 2, 3]) {\n"
          "  f(match (i) { case 2 => { continue; } default => i; });\n"
          "  print(i);\n"
          "}\n",
          stdout="1\n3\n")
    check("deep recursive struct print",
          "struct Node { value: int; next: Node?; }\n"
          "let n = null;\n"
          "for (let i in range(2000)) { n = Node { value: i, next: n }; }\n"
          "print(\"built\");\n"
          "print(n == n);\n"
          "print(n.value);\n"
          "let s = str(n);\n"
          "print(len(s) > 0);\n",
          stdout_all=["built", "true", "1999", "true"])

    check("unary minus", "-3+5\n", stdout="= 2")
    check("unary minus alone", "-3\n", stdout="= -3")
    check("abs negative", "abs(-3)\n", stdout="= 3")
    check("abs overflow", "abs(-9223372036854775807 - 1)\n",
          stderr="integer overflow")
    check("trig functions",
          "sin(0)\ncos(0)\ntan(0)\n"
          "asin(1)\nacos(1)\natan(1)\natan2(1, 1)\n",
          stdout_all=["= 0.0", "= 1.0", "= 0.0", "= 1.5707963267948966",
                      "= 0.0", "= 0.7853981633974483",
                      "= 0.7853981633974483"])
    check("math power and logs",
          "pow(2, 10)\nlog2(8)\nlog10(100)\nhypot(3, 4)\n",
          stdout_all=["= 1024.0", "= 3.0", "= 2.0", "= 5.0"])
    check("math rounding",
          "floor(1.9)\nceil(1.1)\nround(-1.5)\ntrunc(-1.9)\n",
          stdout_all=["= 1", "= 2", "= -2", "= -1"])
    check("math min max",
          "min(3, 1, 2)\nmax(2.5, 1)\nmin(1)\nmax(1, 2.0)\n",
          stdout_all=["= 1", "= 2.5", "= 1", "= 2.0"])
    check("math type error",
          'sin("x")\n', stderr="sin expects a number, got string")
    check("math arity error",
          "atan2(1)\n", stderr="atan2 expects 2 arguments, got 1")
    check("math variadic arity",
          "min()\n", stderr="min expects at least 1 argument")
    check("math rounding overflow",
          "floor(1e100)\n", stderr="floor result out of integer range")

    check("nan equality is false", "sqrt(-1) == 1\n", stdout="= false")
    check("nan inequality is true", "sqrt(-1) != 1\n", stdout="= true")
    check("nan is not equal to itself", "sqrt(-1) == sqrt(-1)\n",
          stdout="= false")
    check("nan ordering is false", "sqrt(-1) >= 1\n", stdout="= false")

    check("divide by zero int", "1/0\n", stderr="division by zero")
    check("divide by zero float", "1.0/0\n", stderr="division by zero")

    check("if float truthy", 'if (1.5) { print("yes"); }\n', stdout="yes")
    check("if else", 'if (0) { print("yes"); } else { print("no"); }\n', stdout="no")

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
        "  let d = dict();\n"
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
        + 'print("ok");\n',
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
        "func 求和(...数字) { return len(数字); }\n求和(1, 2, 3)\n",
        stdout="= 3",
    )
    check(
        "unicode struct field",
        "struct 点 { x: int; }\nlet p = 点 { x: 1 };\np.x\n",
        stdout="= 1",
    )
    check(
        "unicode label",
        "label 开始: while (0) { break 开始; }\n1\n",
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
          'enum Box { B { f: callable; }; }\n'
          'func make() {\n'
          '  let n = 41;\n'
          '  return Box::B { f: func() { return n + 1; } };\n'
          '}\n'
          'let b = make();\n'
          + repeated_lets()
          + 'print(b.f());\n',
          stdout="42")
    check("lru closure survives cycle collection",
          'import std;\n'
          'func make() {\n'
          '  let n = 41;\n'
          '  let l = std.lru.new(1);\n'
          '  let r = std.lru.put(l, "f", func() { return n + 1; });\n'
          '  return r["lru"];\n'
          '}\n'
          'let l = make();\n'
          + repeated_lets()
          + 'let f = std.lru.get(l, "f")["value"];\n'
          + 'print(f());\n',
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
          + 'print(f());\n',
          stdout="42")
