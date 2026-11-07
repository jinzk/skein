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
