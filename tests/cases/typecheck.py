#!/usr/bin/env python3
"""Static container generics: list[int], dict[K,V], set[T], deque[T], heap[T]."""

import os

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
          "let raw = [1];\n"
          "let xs: list[int] = raw;\n"
          "println(xs);\n",
          stderr="type warning: implicit any assigned to list[int]",
          env=WARN_ENV)

    check("generic as suppresses warning",
          "let raw = [1];\n"
          "let xs: list[int] = raw as list[int];\n"
          "println(xs);\n",
          not_stderr="type warning",
          env=WARN_ENV)

    check("generic strict rejects implicit any",
          "let raw = [1];\n"
          "let xs: list[int] = raw;\n",
          stderr="type error: implicit any assigned to list[int]",
          env=STRICT_ENV)

    check("generic loose suppresses any warning",
          "let raw = [1];\n"
          "let xs: list[int] = raw;\n"
          "println(xs);\n",
          not_stderr="type warning",
          env=LOOSE_ENV)

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
