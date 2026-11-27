#!/usr/bin/env python3
"""Tuple literals, static types, destructuring and match patterns."""

import os

from harness import *  # noqa: F401,F403


OFF_ENV = dict(os.environ, SKEIN_TYPECHECK="off")


def run_cases():
    run_skein("tuple_semantics.skein")

    check("tuple constructor exposes length and iter",
          "let t = tuple([1, 2, 3]);\n"
          "std.io.println(str((t).length()));\n"
          "std.io.println(str(std.tuple.length(t)));\n"
          "let total = 0;\n"
          "for x in (t).iter() { total = total + x; }\n"
          "std.io.println(str(total));\n",
          stdout_all=["3", "3", "6"])

    check("std.tuple.new copies iterables",
          "let t = std.tuple.new([1, 2]);\n"
          "let u = std.tuple.new(t);\n"
          "std.io.println(str((u).length()));\n"
          "std.io.println(str((u)[0]));\n"
          "std.io.println(str((std.tuple.new()).length()));\n",
          stdout_all=["2", "1", "0"])

    check("tuple constructor rejects extra arguments",
          "tuple([1], [2])\n",
          stderr="function expects at most 1 argument(s), got 2")
    check("tuple constructor reports runtime arity without typecheck",
          "tuple([1], [2])\n",
          env=OFF_ENV,
          stderr="std.tuple.new expects 0 or 1 arguments")

    check("tuple pattern rejects list literal",
          "let (a, b) = [1, 2];\n"
          "std.io.print(a);\n",
          stderr="tuple pattern requires a pair or tuple, got list[int]")

    check("pair type rejects list literal",
          "let p: pair[int, int] = [1, 2];\n"
          "std.io.print(p);\n",
          stderr="pair expects a tuple literal; use (a, b)")

    check("tuple type rejects list literal",
          "let t: tuple[int, int, int] = [1, 2, 3];\n"
          "std.io.print(t);\n",
          stderr="tuple expects a tuple literal; use (a, b)")

    check("tuple parameter rejects list argument",
          "func f((a, b)) { return a + b; }\n"
          "std.io.print(f([1, 2]));\n",
          stderr="expected pair[any, any], got list[int]")

    check("tuple pattern arity mismatch",
          "let (a, b, c) = (1, 2);\n"
          "std.io.print(a);\n",
          stderr="tuple pattern expects 3 element(s), got 2")

    check("single item parenthesis is not a tuple pattern",
          "let (a) = (1, 2);\n"
          "std.io.print(a);\n",
          stderr="syntax error")

    check("tuple pattern rejects rest",
          "let (a, ...) = (1, 2);\n"
          "std.io.print(a);\n",
          stderr="syntax error")

    check("tuple pattern rejects any subject",
          "let x: any = (1, 2);\n"
          "let (a, b) = x;\n"
          "std.io.print(a);\n",
          stderr="tuple pattern requires a pair or tuple, got any")

    check("tuple pattern rejects list match subject",
          "let m = match [1, 2] { case (x, y) => x * y; };\n"
          "std.io.print(m);\n",
          stderr="tuple pattern requires a pair or tuple, got list[int]")

    check("tuple value is immutable",
          "let a = (1, 2);\n"
          "a[0] = 9;\n",
          stderr="cannot modify an immutable tuple")

    check("typed pair runtime rejects mutable list",
          "let p: pair[int, int] = [1, 2];\n"
          "std.io.print(p);\n",
          stderr="expects pair[int, int], got list",
          env=OFF_ENV)

    check("typed pair runtime rejects wrong arity",
          "let p: pair[int, int] = (1, 2, 3);\n"
          "std.io.print(p);\n",
          stderr="expects pair[int, int], got list",
          env=OFF_ENV)

    check("typed tuple runtime rejects wrong arity",
          "let t: tuple[int, int, int] = (1, 2);\n"
          "std.io.print(t);\n",
          stderr="expects tuple[int, int, int], got list",
          env=OFF_ENV)

    check("typed pair parameter runtime rejects mutable list",
          "func f(p: pair[int, int]) { return p[0] + p[1]; }\n"
          "std.io.print(f([1, 2]));\n",
          stderr="expects pair[int, int], got list",
          env=OFF_ENV)

    check("tuple pattern runtime rejects mutable list",
          "let v = [1, 2];\n"
          "let (a, b) = v;\n"
          "std.io.print(a);\n",
          stderr="destructuring pattern did not match list",
          env=OFF_ENV)

    check("tuple parameter runtime rejects mutable list",
          "func f((a, b)) { return a + b; }\n"
          "std.io.print(f([1, 2]));\n",
          stderr="function parameter does not match its pattern",
          env=OFF_ENV)

    check("tuple match runtime rejects mutable list",
          "let v = [1, 2];\n"
          "let r = match v {\n"
          "  case (a, b) => \"tuple\";\n"
          "  default => \"list\";\n"
          "};\n"
          "std.io.print(r);\n",
          stdout="list",
          env=OFF_ENV)
