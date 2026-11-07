#!/usr/bin/env python3
"""Static container generics: list[int], dict[K,V], set[T], deque[T], heap[T]."""

import os
import tempfile

from harness import *  # noqa: F401,F403


WARN_ENV = dict(os.environ, SKEIN_TYPECHECK="warn")
LOOSE_ENV = dict(os.environ, SKEIN_TYPECHECK="loose")
STRICT_ENV = dict(os.environ, SKEIN_TYPECHECK="strict")
OFF_ENV = dict(os.environ, SKEIN_TYPECHECK="off")


def run_cases():
    check("generic list initialization and insert",
          "let xs: list[int] = [1, 2];\n"
          "xs[0] = 3;\n"
          "xs += [4];\n"
          "println(xs);\n",
          stdout_all=["[3, 2, 4]"])

    check("generic list literal element error",
          'let xs: list[int] = [1, "a"];\n',
          stderr="type error: expected int, got string")

    check("generic list index insert error",
          'let xs: list[int] = [1];\n'
          'xs[0] = "a";\n',
          stderr="type error: expected int, got string")

    check("generic list append error",
          "import std;\n"
          "let xs: list[int] = [1];\n"
          'xs = std.list.append(xs, "a");\n',
          stderr="type error: expected int, got string")

    check("generic list concat error",
          'let xs: list[int] = [1];\n'
          'xs = xs + ["a"];\n',
          stderr="type error: expected int, got string")

    check("generic dict initialization and insert",
          "let d: dict[int, int] = {1: 2};\n"
          "d[3] = 4;\n"
          "println(d);\n",
          stdout_all=["{1: 2, 3: 4}"])

    check("generic dict key error",
          'let d: dict[int, int] = {1: 2};\n'
          'd["a"] = 3;\n',
          stderr="type error: expected int, got string")

    check("generic dict value error",
          'let d: dict[int, int] = {1: 2};\n'
          'd[1] = "a";\n',
          stderr="type error: expected int, got string")

    check("generic set initialization error",
          "import std;\n"
          'let s: set[int] = std.set.from_list([1, "a"]);\n',
          stderr="type error: expected int, got string")

    check("generic set add error",
          "import std;\n"
          "let s: set[int] = std.set.from_list([1]);\n"
          's = std.set.add(s, "a");\n',
          stderr="type error: expected int, got string")

    check("generic deque initialization error",
          "import std;\n"
          'let q: deque[int] = std.deque.from_list([1, "a"]);\n',
          stderr="type error: expected int, got string")

    check("generic deque push error",
          "import std;\n"
          "let q: deque[int] = std.deque.from_list([1]);\n"
          'q = std.deque.push_back(q, "a");\n',
          stderr="type error: expected int, got string")

    check("generic heap initialization error",
          "import std;\n"
          "func less(a, b) { return a < b; }\n"
          'let h: heap[int] = std.heap.from_list([1, "a"], less);\n',
          stderr="type error: expected int, got string")

    check("generic heap push error",
          "import std;\n"
          "func less(a, b) { return a < b; }\n"
          "let h: heap[int] = std.heap.from_list([1, 2], less);\n"
          'h = std.heap.push(h, "a");\n',
          stderr="type error: expected int, got string")

    check("generic function parameter error",
          "func f(xs: list[int]) { println(xs); }\n"
          'f([1, "a"]);\n',
          stderr="type error: expected int, got string")

    check("generic struct field construction error",
          "struct S { xs: list[int]; }\n"
          'let s = S { xs: [1, "a"] };\n',
          stderr="type error: expected int, got string")

    check("generic struct field default error",
          "struct S { xs: list[int] = [1, \"a\"]; }\n",
          stderr="type error: expected int, got string")

    check("generic struct field insert error",
          "struct S { xs: list[int]; }\n"
          "let s: S = S { xs: [1] };\n"
          's.xs[0] = "a";\n',
          stderr="type error: expected int, got string")

    check("nested generic error",
          'let d: dict[int, list[int]] = {1: [2, "a"]};\n',
          stderr="type error: expected int, got string")

    check("nullable generic element",
          "let xs: list[int?] = [1, null];\n"
          "println(xs);\n",
          stdout_all=["[1, null]"])

    check("generic implicit any warning",
          "func raw() { return [1]; }\n"
          "let xs: list[int] = raw();\n"
          "println(xs);\n",
          stderr="type warning: implicit any assigned to list[int]",
          env=WARN_ENV)

    check("generic as suppresses warning",
          "func raw() { return [1]; }\n"
          "let xs: list[int] = raw() as list[int];\n"
          "println(xs);\n",
          not_stderr="type warning",
          env=WARN_ENV)

    check("generic strict rejects implicit any",
          "func raw() { return [1]; }\n"
          "let xs: list[int] = raw();\n",
          stderr="type error: implicit any assigned to list[int]",
          env=STRICT_ENV)

    check("generic loose suppresses any warning",
          "func raw() { return [1]; }\n"
          "let xs: list[int] = raw();\n"
          "println(xs);\n",
          not_stderr="type warning",
          env=LOOSE_ENV)

    check("unannotated let infers container type",
          "let raw = [1, 2];\n"
          "let xs: list[int] = raw;\n"
          "println(xs);\n",
          not_stderr="type warning")

    check("unannotated container mutation stays dynamic",
          "let xs = [1];\n"
          'xs[0] = "a";\n'
          "println(xs);\n",
          not_stderr="type error")

    check("unannotated method receiver is checked",
          "struct Box { xs: list[int]; }\n"
          "impl Box {\n"
          "  func add(self, x: int): list[int] { return self.xs + [x]; }\n"
          "}\n"
          "let b = Box { xs: [1] };\n"
          'b.add("a");\n',
          stderr="type error: expected int, got string")

    check("anonymous function return annotation",
          "let make = func(x: int): list[int] -> [x];\n"
          "let xs: list[int] = make(2);\n"
          'xs[0] = "a";\n',
          stderr="type error: expected int, got string")

    check("map infers single-return block lambda",
          "import std;\n"
          "let xs: list[int] = [1];\n"
          "let ys: list[int] = std.list.map(xs, func(x) { return x * 2; });\n"
          'ys[0] = "a";\n',
          stderr="type error: expected int, got string")

    check("generic loose still reports concrete errors",
          'let xs: list[int] = [1, "a"];\n',
          stderr="type error: expected int, got string",
          env=LOOSE_ENV)

    check("generic strict accepts explicit as",
          "let raw = [1];\n"
          "let xs: list[int] = raw as list[int];\n"
          "println(xs);\n",
          not_stderr="type error",
          env=STRICT_ENV)

    check("generic off keeps runtime check",
          "let xs: list[int] = 5;\n",
          stderr="variable xs expects list[int], got int",
          env=OFF_ENV)

    check("generic generic args rejected in is",
          "let xs = [1];\n"
          "println(xs is list[int]);\n",
          stderr="generic type arguments are not allowed in 'is'")

    check("generic arity error",
          "let xs: list[int, int] = [1, 2];\n",
          stderr="type list expects 1 type argument(s)")

    check("generic user type rejects type args",
          "struct S { x: int; }\n"
          "let s: S[int] = S { x: 1 };\n",
          stderr="type S does not accept type arguments")

    check("generic cast result indexing",
          "let raw = [1];\n"
          "println((raw as list[int])[0]);\n",
          stdout_all=["1"])

    check("generic as is not an lvalue",
          "let raw = [1];\n"
          "(raw as list[int])[0] = 2;\n",
          stderr="cannot assign through 'as'")

    check("generic plain annotations keep runtime behavior",
          'let x: int = "a";\n',
          stderr="runtime error",
          not_stderr="type error")

    check("return annotation propagates container type",
          "func numbers(): list[int] { return [1, 2]; }\n"
          "let xs: list[int] = numbers();\n"
          "xs[0] = 3;\n"
          "println(xs);\n",
          stdout_all=["[3, 2]"])

    check("return annotation checks container literal",
          'func numbers(): list[int] { return [1, "a"]; }\n',
          stderr="type error: expected int, got string")

    check("return annotation propagates insert checks",
          "func numbers(): list[int] { return [1, 2]; }\n"
          "let xs: list[int] = numbers();\n"
          'xs[0] = "a";\n',
          stderr="type error: expected int, got string")

    check("return annotation checks plain scalar",
          'func bad(): int { return "a"; }\n',
          stderr="type error: expected int, got string")

    check("return annotation rejects missing value",
          "func bad(): int { return; }\n",
          stderr="type error: expected int, got null")

    check("nullable return annotation accepts missing value",
          "func maybe(): int? { return; }\n"
          "println(maybe());\n",
          stdout_all=["null"])

    check("expression body return annotation",
          "func numbers(): list[int] -> [1, 2];\n"
          "let xs: list[int] = numbers();\n"
          "println(xs);\n",
          stdout_all=["[1, 2]"])

    check("method return annotation checks body",
          "struct S { x: int; }\n"
          'impl S { func get(self): int { return "a"; } }\n',
          stderr="type error: expected int, got string")

    check("return annotation any warning",
          "func raw() { return [1]; }\n"
          "func numbers(): list[int] { return raw(); }\n"
          "println(numbers());\n",
          stderr="type warning: implicit any assigned to list[int]",
          env=WARN_ENV)

    check("return annotation as suppresses warning",
          "func raw() { return [1]; }\n"
          "func numbers(): list[int] { return raw() as list[int]; }\n"
          "println(numbers());\n",
          not_stderr="type warning",
          env=WARN_ENV)

    check("function without return annotation stays any",
          "func numbers() { return [1, 2]; }\n"
          "let xs: list[int] = numbers();\n"
          "println(xs);\n",
          stderr="type warning: implicit any assigned to list[int]",
          env=WARN_ENV)

    check("return annotation callable accepts builtin",
          "func get_fn(): callable { return sqrt; }\n"
          "println(get_fn()(9));\n",
          stdout_all=["3"])

    check("function type syntax",
          "let f: (int) -> int = func(x: int): int -> x * 2;\n"
          "let g: () -> int = func(): int -> 7;\n"
          "println(f(3));\n"
          "println(g());\n",
          stdout_all=["6", "7"])

    check("nested function type syntax",
          "import std;\n"
          "struct S { f: ((int) -> int) -> int; }\n"
          "let s = S { f: func(x: (int) -> int): int -> x(2) };\n"
          "let n = s.f(func(y: int): int -> y * 3);\n"
          "println(n);\n"
          "println(std.meta.fields(s));\n",
          stdout_all=["6", "((int) -> int) -> int"])

    check("function type in container and parameter",
          "let fs: list[(int) -> int] = [func(x: int): int -> x];\n"
          "func apply(f: (int) -> int, x: int): int { return f(x); }\n"
          "println(fs[0](4));\n"
          "println(apply(func(x: int): int -> x + 1, 4));\n",
          stdout_all=["4", "5"])

    check("function type accepts builtin",
          "let f: (number) -> number = sqrt;\n"
          "println(f(9));\n",
          not_stderr="type warning",
          stdout_all=["3"])

    check("builtin function signatures",
          "import std;\n"
          "let f: (number) -> int = floor;\n"
          "let g: (int, int) -> int = std.math.gcd;\n"
          "let h: (string) -> string = trim;\n"
          "println(f(2.9));\n"
          "println(g(12, 18));\n"
          "println(h(\"  x  \"));\n",
          stdout_all=["2", "6", "x"])

    check("builtin signature argument check",
          'sqrt("x");\n',
          stderr="type error: expected number, got string")

    check("function type assignment and call",
          "let f: (int) -> int = func(x: int): int -> x * 2;\n"
          "println(f(3));\n",
          stdout_all=["6"])

    check("function type return mismatch",
          'let f: (int) -> int = func(x: int): string -> "a";\n',
          stderr="type error: expected (int) -> int, got (int) -> string")

    check("function type parameter mismatch",
          "let f: (int) -> int = func(x: string): int -> 1;\n",
          stderr="type error: expected (int) -> int, got (string) -> int")

    check("function type guides short lambda parameters",
          "let f: (int) -> int = x -> x * 2;\n"
          "println(f(3));\n"
          'f("a");\n',
          stderr="type error: expected int, got string")

    check("function type parameter variance",
          "let f: (int) -> int = func(x: number): int -> 1;\n"
          "println(f(2));\n",
          stdout_all=["1"])

    check("function type return covariance",
          "let f: (int) -> number = func(x: int): int -> x;\n"
          "println(f(2));\n",
          stdout_all=["2"])

    check("function type narrow return rejected",
          "let f: (int) -> int = func(x: int): number -> x;\n",
          stderr="type error: expected (int) -> int, got (int) -> number")

    check("function type call argument check",
          "let f: (int) -> int = func(x: int): int -> x;\n"
          'f("a");\n',
          stderr="type error: expected int, got string")

    check("function type field and container calls",
          "struct S { cb: (int) -> int; }\n"
          "let s: S = S { cb: func(x: int): int -> x + 1 };\n"
          "let fs: list[(int) -> int] = [func(x: int): int -> x];\n"
          "println(s.cb(2));\n"
          "println(fs[0](2));\n"
          'fs[0]("a");\n',
          stderr="type error: expected int, got string")

    check("function type any warning",
          "func raw() { return func(x: int): int -> x; }\n"
          "let f: (int) -> int = raw();\n"
          "println(f(2));\n",
          stderr="type warning: implicit any assigned to (int) -> int",
          env=WARN_ENV)

    check("function type as suppresses warning",
          "func raw() { return func(x: int): int -> x; }\n"
          "let f: (int) -> int = raw() as (int) -> int;\n"
          "println(f(2));\n",
          not_stderr="type warning",
          env=WARN_ENV)

    check("function type strict rejects any",
          "func raw() { return func(x: int): int -> x; }\n"
          "let f: (int) -> int = raw();\n",
          stderr="type error: implicit any assigned to (int) -> int",
          env=STRICT_ENV)

    check("function type is rejected in is",
          "let f = func(x) { return x; };\n"
          "println(f is (int) -> int);\n",
          stderr="function type signatures are not allowed in 'is'")

    check("function call scalar argument check",
          "func f(x: int) { return x; }\n"
          'f("a");\n',
          stderr="type error: expected int, got string")

    check("method call argument check",
          "struct Box { xs: list[int]; }\n"
          "impl Box {\n"
          "  func add(self, x: int): list[int] {\n"
          "    return self.xs + [x];\n"
          "  }\n"
          "}\n"
          "let b: Box = Box { xs: [1] };\n"
          'b.add("a");\n',
          stderr="type error: expected int, got string")

    check("method return type propagates",
          "struct Box { xs: list[int]; }\n"
          "impl Box {\n"
          "  func add(self, x: int): list[int] {\n"
          "    return self.xs + [x];\n"
          "  }\n"
          "}\n"
          "let b: Box = Box { xs: [1] };\n"
          "let ys: list[int] = b.add(2);\n"
          'ys[0] = "a";\n',
          stderr="type error: expected int, got string")

    check("destructure list element inference",
          "let xs: list[list[int]] = [[1], [2]];\n"
          "let [a, b] = xs;\n"
          'a[0] = "x";\n',
          stderr="type error: expected int, got string")

    check("destructure struct pattern inference",
          "struct P { xs: list[int]; }\n"
          "let p: P = P { xs: [1] };\n"
          "let P { xs } = p;\n"
          'xs[0] = "a";\n',
          stderr="type error: expected int, got string")

    check("destructure dict pattern inference",
          'let d: dict[string, list[int]] = {"a": [1]};\n'
          "let {a: xs} = d;\n"
          'xs[0] = "a";\n',
          stderr="type error: expected int, got string")

    check("match pattern inference",
          "let xs: list[list[int]] = [[1]];\n"
          'match (xs) { case [a] => { a[0] = "x"; } }\n',
          stderr="type error: expected int, got string")

    check("for binding inference",
          "let xs: list[list[int]] = [[1]];\n"
          'for (let a in xs) { a[0] = "x"; }\n',
          stderr="type error: expected int, got string")

    check("method value function type",
          "struct Box { xs: list[int]; }\n"
          "impl Box {\n"
          "  func add(self, x: int): list[int] { return self.xs + [x]; }\n"
          "}\n"
          "let b: Box = Box { xs: [1] };\n"
          "let f = b.add;\n"
          "let ys: list[int] = f(2);\n"
          'f("a");\n',
          stderr="type error: expected int, got string")

    check("higher-order builtin callbacks",
          "import std;\n"
          "let xs: list[int] = [1, 2, 3];\n"
          "let ys: list[string] = std.list.map(\n"
          "  xs, func(x: int): string -> str(x));\n"
          "let zs: list[int] = std.list.filter(\n"
          "  xs, func(x: int): bool -> x > 1);\n"
          "let total: int = std.list.reduce(\n"
          "  xs, func(a: int, b: int): int -> a + b, 0);\n"
          "let groups: dict[int, list[int]] = std.list.group_by(\n"
          "  xs, func(x: int): int -> x % 2);\n"
          "let sorted: list[int] = std.list.sort_by(\n"
          "  xs, func(x: int): int -> 0 - x);\n"
          "println(ys); println(zs); println(total);\n"
          "println(groups); println(sorted);\n",
          stdout_all=["[1, 2, 3]", "[2, 3]", "6",
                      "{1: [1, 3], 0: [2]}", "[3, 2, 1]"])

    check("filter callback return check",
          "import std;\n"
          "let xs: list[int] = [1];\n"
          "std.list.filter(xs, func(x: int): int -> x);\n",
          stderr="type error: expected (int) -> bool, got (int) -> int")

    check("map callback parameter check",
          "import std;\n"
          "let xs: list[int] = [1];\n"
          "std.list.map(xs, func(x: string): int -> 1);\n",
          stderr="type error: expected (int) -> any, got (string) -> int")

    check("polymorphic builtin value append",
          "import std;\n"
          "let f = std.list.append;\n"
          "let ys: list[int] = f([1], 2);\n"
          "println(ys);\n",
          stdout="[1, 2]", not_stderr="type warning")

    check("polymorphic builtin value element mismatch",
          "import std;\n"
          "let f = std.list.append;\n"
          'f([1], "a");\n',
          stderr="type error: expected int, got string")

    check("polymorphic builtin value assignment",
          "import std;\n"
          "let f: (list[int], int) -> list[int] = std.list.append;\n"
          "let ys: list[int] = f([2], 3);\n"
          "println(ys);\n",
          stdout="[2, 3]", not_stderr="type warning")

    check("polymorphic builtin value bad signature",
          "import std;\n"
          "let f: (list[int], int) -> list[string] = std.list.append;\n",
          stderr="expected (list[int], int) -> list[string], "
                 "got (list[int], int) -> list[int]")

    check("polymorphic map value",
          "import std;\n"
          "let f = std.list.map;\n"
          "let ys: list[int] = f([1, 2], x -> x * 2);\n"
          "println(ys);\n",
          stdout="[2, 4]", not_stderr="type warning")

    check("polymorphic map callback mismatch",
          "import std;\n"
          "let f = std.list.map;\n"
          "f([1], func(x: string): int -> 1);\n",
          stderr="type error: expected (int) -> any, got (string) -> int")

    check("polymorphic dict keys value",
          "import std;\n"
          "let f = std.dict.keys;\n"
          'let ks: list[string] = f({"a": 1});\n'
          "println(ks);\n",
          stdout="[a]", not_stderr="type warning")

    check("polymorphic set from_list value",
          "import std;\n"
          "let f = std.set.from_list;\n"
          "let s: set[int] = f([1, 2]);\n"
          "println(std.set.values(s));\n",
          stdout="[1, 2]", not_stderr="type warning")

    check("function type arity check",
          "let f: (int) -> int = func(x: int): int -> x;\n"
          "f(1, 2);\n",
          stderr="type error: function expects 1 argument(s), got 2")

    check("builtin template arity check",
          "import std;\n"
          "std.list.append([1], 2, 3);\n",
          stderr="type error: function expects 2 argument(s), got 3")

    check("optional builtin parameter type",
          "import std;\n"
          "let s: (list[int]) -> list[int] = std.list.sort;\n"
          "let xs: list[int] = s([2, 1]);\n"
          "let f = std.list.sort;\n"
          "let ys: list[int] = f([2, 1], "
          "func(a: int, b: int): int -> a - b);\n"
          "println(xs); println(ys);\n",
          stdout_all=["[1, 2]", "[1, 2]"])

    check("default parameter function type",
          "func f(a: int, b: int = 1): int { return a + b; }\n"
          "let g: (int) -> int = f;\n"
          "println(g(3));\n",
          stdout="4", not_stderr="type warning")

    check("default parameter as callback",
          "func add(a: int, b: int = 1): int { return a + b; }\n"
          "func apply(f: (int) -> int): int { return f(3); }\n"
          "println(apply(add));\n",
          stdout="4", not_stderr="type warning")

    check("variadic function as callback",
          "func count(...xs: int): int { return len(xs); }\n"
          "func apply(f: (int, int) -> int): int { return f(1, 2); }\n"
          "println(apply(count));\n",
          stdout="2", not_stderr="type warning")

    check("variadic callback rest type",
          "func count(...xs: int): int { return len(xs); }\n"
          "func apply(f: (string) -> int): int { return f(\"x\"); }\n"
          "apply(count);\n",
          stderr="type error: expected (string) -> int, "
                 "got (...int) -> int")

    check("variadic function value rest check",
          "func count(...xs: int): int { return len(xs); }\n"
          "let f = count;\n"
          "let g: (int, int) -> int = f;\n"
          "println(g(1, 2));\n"
          'f(1, "a");\n',
          stdout="2", stderr="type error: expected int, got string")

    check("iter callback checks",
          "import std;\n"
          "std.iter.filter([1], func(x: int): int -> x);\n",
          stderr="type error: expected (int) -> bool, got (int) -> int")

    check("iter map value checks list callback",
          "import std;\n"
          "let m = std.iter.map;\n"
          "m([1], func(x: string): int -> 1);\n",
          stderr="type error: expected (int) -> any, got (string) -> int")

    check("iter map accepts dict",
          "import std;\n"
          'let g = std.iter.map({"a": 1}, '
          "func(k: string): int -> 1);\n"
          "println(std.iter.collect(g));\n",
          stdout="[1]")

    check("fn compose and pipe types",
          "import std;\n"
          "let c = std.fn.compose;\n"
          "let f: (int) -> string = c("
          "func(x: int): string -> str(x), "
          "func(x: int): int -> x + 1);\n"
          "let p = std.fn.pipe;\n"
          "let g: (int) -> string = p("
          "func(x: int): int -> x + 1, "
          "func(x: int): string -> str(x));\n"
          "println(f(1)); println(g(1));\n",
          stdout_all=["2", "2"])

    check("fn compose shape mismatch",
          "import std;\n"
          "let c = std.fn.compose;\n"
          "c(func(x: string): string -> x, "
          "func(x: int): int -> x);\n",
          stderr="type error: expected (int) -> string, got (int) -> int")

    check("fn compose exactly two arguments",
          "import std;\n"
          "let c = std.fn.compose;\n"
          "c(func(x: int): int -> x + 1, "
          "func(x: int): int -> x * 2,\n"
          "  func(x: int): int -> x - 3);\n",
          stderr="std.fn.compose expects exactly 2 argument(s), got 3")

    check("fn compose first-class arity",
          "import std;\n"
          "let c = std.fn.compose;\n"
          "c(func(x: int): int -> x + 1);\n",
          stderr="std.fn.compose expects exactly 2 argument(s), got 1")

    check("fn compose nested",
          "import std;\n"
          "let inner = std.fn.compose("
          "func(x: bool): int -> 1, "
          "func(x: string): bool -> len(x) > 0);\n"
          "let outer = std.fn.compose("
          "func(x: int): string -> str(x), inner);\n"
          "let f: (string) -> string = outer;\n"
          "println(f(\"a\"));\n",
          stdout="1", not_stderr="type warning")

    check("local variable shadows function name",
          "import std;\n"
          "func path(g, from, to) { return 1; }\n"
          "func walk(from: int, to: int) {\n"
          "  let path = [to];\n"
          "  let i = 0;\n"
          "  while (i < 1) {\n"
          "    path = path + [from];\n"
          "    i = i + 1;\n"
          "  }\n"
          "  let out: list[int] = std.list.append(\n"
          "    path as list[int], to);\n"
          "  return out;\n"
          "}\n"
          "println(walk(1, 2));\n",
          stdout="[2, 1, 2]")

    check("map infers expression lambda result",
          "import std;\n"
          "let xs: list[int] = [1];\n"
          "let ys: list[int] = std.list.map(xs, x -> x * 2);\n"
          'ys[0] = "a";\n',
          stderr="type error: expected int, got string")

    check("map result type mismatch",
          "import std;\n"
          "let xs: list[int] = [1];\n"
          'let ys: list[int] = std.list.map(xs, func(x: int) -> "a");\n',
          stderr="type error: expected list[int], got list[string]")

    check("zip second argument must be list",
          "import std;\n"
          "let xs: list[int] = [1];\n"
          "let ys = std.list.zip(xs, 1);\n",
          stderr="type error: expected list, got int")

    with tempfile.TemporaryDirectory() as tmp:
        pkg_dir = os.path.join(tmp, "app", "aa")
        os.makedirs(pkg_dir)
        with open(os.path.join(pkg_dir, "aa.skein"), "w",
                  encoding="utf-8") as f:
            f.write(
                "package aa {\n"
                "  export func make(n: int): list[int] {\n"
                "    return [n, n + 1];\n"
                "  }\n"
                "  export func bad(n: int): int { return n; }\n"
                "  export struct P { xs: list[int]; }\n"
                "  impl P {\n"
                "    func add(self, x: int): list[int] {\n"
                "      return self.xs + [x];\n"
                "    }\n"
                "  }\n"
                "  export func make_p(): P { return P { xs: [1] }; }\n"
                "  export let nums: list[int] = [1, 2];\n"
                "  export let nums2 = [3, 4];\n"
                "  export let changed = [1];\n"
                "  changed = [\"x\"];\n"
                "}\n")
        main = os.path.join(tmp, "app", "main.skein")
        with open(main, "w", encoding="utf-8") as f:
            f.write(
                "import aa;\n"
                "let xs: list[int] = aa.make(1);\n"
                'xs[0] = "a";\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stderr
        assert "type error: expected int, got string" in proc.stderr, proc.stderr

        with open(main, "w", encoding="utf-8") as f:
            f.write(
                "import aa;\n"
                'aa.bad("x");\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stderr
        assert "type error: expected int, got string" in proc.stderr, proc.stderr

        with open(main, "w", encoding="utf-8") as f:
            f.write(
                "import aa;\n"
                "let p: aa.P = aa.make_p();\n"
                'p.add("x");\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stderr
        assert "type error: expected int, got string" in proc.stderr, proc.stderr

        with open(main, "w", encoding="utf-8") as f:
            f.write(
                "import aa;\n"
                "let ys: list[int] = aa.nums;\n"
                'ys[0] = "a";\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stderr
        assert "type error: expected int, got string" in proc.stderr, proc.stderr

        with open(main, "w", encoding="utf-8") as f:
            f.write(
                "import aa;\n"
                "let ys: list[int] = aa.nums2;\n"
                'ys[0] = "a";\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stderr
        assert "type error: expected int, got string" in proc.stderr, proc.stderr

        with open(main, "w", encoding="utf-8") as f:
            f.write(
                "import aa;\n"
                "let p = aa.make_p();\n"
                'p.add("x");\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stderr
        assert "type error: expected int, got string" in proc.stderr, proc.stderr

        with open(main, "w", encoding="utf-8") as f:
            f.write(
                "import aa as lib;\n"
                "let xs: list[int] = lib.make(1);\n"
                'xs[0] = "a";\n')
        proc = run_file(main)
        assert proc.returncode == 1, proc.stderr
        assert "type error: expected int, got string" in proc.stderr, proc.stderr

        with open(main, "w", encoding="utf-8") as f:
            f.write(
                "import aa;\n"
                "let ys: list[int] = aa.changed;\n")
        proc = run_file(main)
        assert proc.returncode == 1, proc.stderr
        assert "expected list[int], got list[string]" in proc.stderr, proc.stderr

    proc = run(
        "func f(a: int, b: string) { return a; }\n"
        'f("x", 1);\n')
    assert proc.stderr.count("type error:") == 1, proc.stderr
