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

    check("compound assignment widens untyped target",
          "let x = 1;\n"
          "x += 2.5;\n"
          "println(x);\n",
          stdout_all=["3.5"])

    check("compound list accepts tuple",
          "let xs: list[int] = [1];\n"
          "xs += (2, 3);\n"
          "println(xs);\n",
          stdout_all=["[1, 2, 3]"])

    check("compound int rejects float result",
          "let x: int = 1;\n"
          "x += 2.5;\n",
          stderr="type error: expected int, got float")

    check("compound bitwise rejects float",
          "let f: float = 1.0;\n"
          "f &= 1;\n",
          stderr="type error: bitwise operator expects ints, got float and int")

    check("compound string rejects multiplication",
          'let s: string = "a";\n'
          "s *= 2;\n",
          stderr="type error: cannot use * on string and int")

    check("compound list rejects subtraction",
          "let xs: list[int] = [1];\n"
          "xs -= [1];\n",
          stderr="type error: cannot use - on list[int] and list[int]")

    check("compound pair result arity",
          "let p: pair[int, int] = (1, 2);\n"
          "p += (3, 4);\n",
          stderr="type error: expected pair[int, int], "
                 "got tuple[int, int, int, int]")

    check("compound tuple result arity",
          "let t: tuple[int, int, int] = (1, 2, 3);\n"
          "t += (4, 5);\n",
          stderr="type error: expected tuple[int, int, int], "
                 "got tuple[int, int, int, int, int]")

    check("generic dict initialization and insert",
          "let d: dict[int, int] = {1: 2};\n"
          "d[3] = 4;\n"
          "println(d);\n",
          stdout_all=["{1: 2, 3: 4}"])

    check("generic dict float keys",
          "let d: dict[float, int] = {1.5: 1};\n"
          "d[2.5] = 2;\n"
          "println(d[1.5]);\nprintln(d[2.5]);\n",
          stdout_all=["1", "2"])

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

    check("dict key contract mutable struct",
          "struct Bad { xs: list[int]; }\n"
          "let d: dict[Bad, int] = {};\n",
          stderr="type error: dict key type Bad is not hashable")

    check("set element contract mutable struct",
          "import std;\n"
          "struct Bad { xs: list[int]; }\n"
          "let s: set[Bad] = std.set.new();\n",
          stderr="type error: set element type Bad is not hashable")

    check("nested dict key contract",
          "struct Bad { xs: list[int]; }\n"
          "let xs: list[dict[Bad, int]] = [];\n",
          stderr="type error: dict key type Bad is not hashable")

    check("dict key contract custom hash",
          "import std;\n"
          "import std.dict_key;\n"
          "struct User { id: int; cache: list[int]; }\n"
          "impl User { func dict_hash(self): int { return self.id; } }\n"
          "let d: dict[User, int] = {};\n"
          "println(len(d));\n",
          stdout="0", not_stderr="type warning")

    check("set element contract custom hash",
          "import std;\n"
          "import std.dict_key;\n"
          "struct User { id: int; cache: list[int]; }\n"
          "impl User { func dict_hash(self): int { return self.id; } }\n"
          "let s: set[User] = std.set.from_list([]);\n"
          "println(std.set.size(s));\n",
          stdout="0", not_stderr="type warning")

    check("key_eq without dict_hash keeps structural key",
          "import std;\n"
          "struct User { id: int; }\n"
          "impl User { func key_eq(self, other): bool "
          "{ return self.id == other.id; } }\n"
          "let d: dict[User, int] = {};\n"
          "println(len(d));\n",
          stdout="0", not_stderr="type warning")

    check("dict hash impl must precede use",
          "struct User { xs: list[int]; }\n"
          "let d: dict[User, int] = {};\n"
          "impl User { func dict_hash(self): int { return 1; } }\n",
          stderr="type error: dict key type User is not hashable")

    check("dict key contract invalid hash return",
          "import std;\n"
          "struct User { id: int; }\n"
          "impl User { func dict_hash(self) { return \"x\"; } }\n"
          "let d: dict[User, int] = {};\n",
          stderr="type error: dict key type User is not hashable")

    check("dict key contract invalid key_eq return",
          "import std;\n"
          "struct User { id: int; }\n"
          "impl User { func dict_hash(self): int { return self.id; } }\n"
          "impl User { func key_eq(self, other) { return 1; } }\n"
          "let d: dict[User, int] = {};\n",
          stderr="type error: dict key type User is not hashable")

    check("generic struct union constraint",
          "struct Box[T int | string] { value: T; }\n"
          "let b = Box[bool] { value: true };\n",
          stderr="type error: type argument bool does not satisfy generic constraint for Box")

    check("generic struct hashable constraint",
          "struct Box[T Hashable] { value: T; }\n"
          "let b = Box[list[int]] { value: [1] };\n",
          stderr="type error: type argument list[int] does not satisfy generic constraint for Box")

    check("generic struct protocol constraint",
          "proto Drawable { func draw(self); }\n"
          "struct D { func draw(self) { } }\n"
          "struct Box[T Drawable] { value: T; }\n"
          "let b = Box[int] { value: 1 };\n",
          stderr="type error: type argument int does not satisfy generic constraint for Box")

    check("generic function union constraint",
          "func pick[T int | string](x: T): T { return x; }\n"
          "let b = pick(true);\n",
          stderr="type error: type argument bool does not satisfy generic constraint for pick")

    check("generic function explicit constraint",
          "func pick[T int | string](x: T): T { return x; }\n"
          "let b = pick[bool](true);\n",
          stderr="type error: type argument bool does not satisfy generic constraint for pick")

    check("explicit generic parse error keeps bracket position",
           "func identity[T any](x: T): T { return x; }\n"
           "let y = identity[int | string](1);\n",
           stderr="2:17: type error: expected ',' or end of type list")

    check("multiline explicit generic call",
           "func identity[T any](x: T): T { return x; }\n"
           "let y = identity[\n"
           "  int\n"
           "](3);\n"
           "println(str(y));\n",
           stdout="3")

    check("multiline explicit generic function value",
           "func identity[T any](x: T): T { return x; }\n"
           "let g = identity[\n"
           "  int\n"
           "];\n"
           "println(str(g(4)));\n",
           stdout="4")

    check("multiline explicit generic struct literal",
           "struct Box[T any] { value: T; }\n"
           "let b = Box[\n"
           "  int\n"
           "] { value: 5 };\n"
           "println(str(b.value));\n",
           stdout="5")

    check("generic type marker union constraint",
          "struct Box[T type int | string] { value: T; }\n"
          "let b = Box[bool] { value: true };\n",
          stderr="type error: type argument bool does not satisfy generic constraint for Box")

    check("generic union type argument constraint",
          "struct Box[T int | string] { value: T; }\n"
          "let b = Box[union[int, bool]] { value: true };\n",
          stderr="type error: type argument union[int, bool] does not satisfy generic constraint for Box")

    check("generic invariant any",
          "struct Box[T any] { value: T; }\n"
          "let a: Box[int] = Box { value: 1 };\n"
          "let b: Box[any] = a;\n",
          stderr="type error: expected Box[any], got Box[int]")

    check("generic invariant union",
          "struct Box[T any] { value: T; }\n"
          "let a: Box[int] = Box { value: 1 };\n"
          "let b: Box[union[int, string]] = a;\n",
          stderr="type error: expected Box[union[int, string]], got Box[int]")

    check("protocol is not a type argument",
          "proto Drawable { func draw(self); }\n"
          "struct Box[T any] { value: T; }\n"
          "let b: Box[Drawable] = Box { value: 1 };\n",
          stderr="type error: proto Drawable cannot be used as a type argument")

    check("generic protocol constraint",
          "proto Keyed[K any] { func key(self): K; }\n"
          "struct NoKey { }\n"
          "func need[T Keyed](x: T): T { return x; }\n"
          "let a = need(NoKey {});\n",
          stderr="type error: type argument NoKey does not satisfy generic constraint for need")

    check("generic protocol type parameter consistency",
          "proto PairKeyed[K any] {\n"
          "  func first(self): K;\n"
          "  func second(self): K;\n"
          "}\n"
          "struct Bad {\n"
          "  func first(self): int { return 1; }\n"
          "  func second(self): string { return \"x\"; }\n"
          "}\n"
          "func need[T PairKeyed](x: T): T { return x; }\n"
          "let a = need(Bad {});\n",
          stderr="type error: type argument Bad does not satisfy generic constraint for need")

    check("generic protocol explicit type argument",
          "proto Keyed[K any] { func key(self): K; }\n"
          "struct StringKey { func key(self): string { return \"a\"; } }\n"
          "func need[T Keyed[int]](x: T): T { return x; }\n"
          "let a = need(StringKey {});\n",
          stderr="type error: type argument StringKey does not satisfy generic constraint for need")

    check("generic protocol self-referential rhs mismatch",
          "proto Add[R any, O any] { func op_add(self, rhs: R): O; }\n"
          "struct BadRhs { func op_add(self, rhs: int): BadRhs { return self; } }\n"
          "func add[T Add[T, T]](x: T, y: T): T { return x.op_add(y); }\n"
          "add(BadRhs {}, BadRhs {});\n",
          stderr="type error: type argument BadRhs does not satisfy generic constraint for add")

    check("generic protocol self-referential result mismatch",
          "proto Add[R any, O any] { func op_add(self, rhs: R): O; }\n"
          "struct BadOut { func op_add(self, rhs: BadOut): int { return 1; } }\n"
          "func add[T Add[T, T]](x: T, y: T): T { return x.op_add(y); }\n"
          "add(BadOut {}, BadOut {});\n",
          stderr="type error: type argument BadOut does not satisfy generic constraint for add")

    check("generic struct self-referential constraint mismatch",
          "proto Add[R any, O any] { func op_add(self, rhs: R): O; }\n"
          "struct BadRhs { func op_add(self, rhs: int): BadRhs { return self; } }\n"
          "struct AddBox[T Add[T, T]] { value: T; }\n"
          "let b = AddBox[BadRhs] { value: BadRhs {} };\n",
          stderr="type error: type argument BadRhs does not satisfy generic constraint for AddBox")

    check("generic default self-referential constraint mismatch",
          "proto Add[R any, O any] { func op_add(self, rhs: R): O; }\n"
          "struct BadRhs { func op_add(self, rhs: int): BadRhs { return self; } }\n"
          "func add[T Add[T, T] = BadRhs](x: T): T { return x.op_add(x); }\n",
          stderr="type error: default type BadRhs does not satisfy the constraint for T")

    check("std.ops equatable constraint mismatch",
          "import {Equatable} from std.ops;\n"
          "struct NoEq { }\n"
          "func equal[T Equatable[T]](a: T, b: T): bool { return a.op_eq(b); }\n"
          "equal(NoEq {}, NoEq {});\n",
          stderr="type error: type argument NoEq does not satisfy generic constraint for equal")

    check("std.ops comparable constraint mismatch",
          "import {Comparable} from std.ops;\n"
          "struct NoCmp { }\n"
          "func compare[T Comparable[T]](a: T, b: T): int { return a.op_cmp(b); }\n"
          "compare(NoCmp {}, NoCmp {});\n",
          stderr="type error: type argument NoCmp does not satisfy generic constraint for compare")

    check("arithmetic overload rejects wrong rhs type",
          "struct Op { func op_add(self, rhs: Op): Op { return self; } }\n"
          "let x = Op {} + 1;\n",
          stderr="type error: expected Op, got int")

    check("arithmetic without overload keeps builtin error",
          "struct NoOp { }\n"
          "let x = NoOp {} + NoOp {};\n",
          stderr="runtime error: cannot add NoOp and NoOp")

    check("generic default type must be trailing",
          "struct Bad[T = int, U any] { }\n",
          stderr="type error: default type parameters must be trailing")

    check("generic default type must satisfy constraint",
          "struct Bad[T int | string = bool] { }\n",
          stderr="type error: default type bool does not satisfy the constraint for T")

    check("generic constraint rejects concrete type in and-group",
          "struct Bad[T int & Hashable] { }\n",
          stderr="type error: concrete type int cannot be used with '&' in the constraint for T")

    check("generic constraint rejects hashable union",
          "struct Bad[T Hashable | int] { }\n",
          stderr="type error: proto Hashable cannot be used with '|'")

    check("bare generic type uses default arguments",
          "struct Box[T = any] { value: T; }\n"
          "let a: Box = Box[int] { value: 1 };\n",
          stderr="type error: expected Box[any], got Box[int]")

    check("generic proto marker constraint",
          "proto Drawable { func draw(self); }\n"
          "struct Box[T proto Drawable] { value: T; }\n"
          "let b = Box[int] { value: 1 };\n",
          stderr="type error: type argument int does not satisfy generic constraint for Box")

    check("generic function type marker constraint",
          "func pick[T type int | string](x: T): T { return x; }\n"
          "let b = pick(true);\n",
          stderr="type error: type argument bool does not satisfy generic constraint for pick")

    check("generic function value constraint",
          "func pick[T int | string](x: T): T { return x; }\n"
          "let g = pick[bool];\n",
          stderr="type error: type argument bool does not satisfy generic constraint for pick")

    check("generic enum explicit literal field type",
          "enum Option[T any] { None; Some { value: T; } }\n"
          "let a = Option[string].Some { value: 1 };\n",
          stderr="type error: expected string, got int")

    check("dict builtin key contract",
          "import std;\n"
          "struct Bad { xs: list[int]; }\n"
          "let d = std.dict.index_by([1], "
          "func(x) { return Bad { xs: [] }; });\n",
          stderr="type error: dict key type Bad is not hashable")

    check("set builtin key contract",
          "import std;\n"
          "struct Bad { xs: list[int]; }\n"
          "let s = std.set.from_list([Bad { xs: [] }]);\n",
          stderr="type error: set element type Bad is not hashable")

    check("dict any key keeps runtime fallback",
          "import std;\n"
          "struct Bad { xs: list[int]; }\n"
          "let d: dict[any, int] = {};\n"
          "d[Bad { xs: [] }] = 1;\n",
          stderr="runtime error: dict key must be")

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
          "func get_fn(): Callable { return std.math.sqrt; }\n"
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
          "let f: (number) -> number = std.math.sqrt;\n"
          "println(f(9));\n",
          not_stderr="type warning",
          stdout_all=["3"])

    check("builtin function signatures",
          "import std;\n"
          "let f: (number) -> int = std.math.floor;\n"
          "let g: (int, int) -> int = std.math.gcd;\n"
          "let h: (string) -> string = std.string.trim;\n"
          "println(f(2.9));\n"
          "println(g(12, 18));\n"
          "println(h(\"  x  \"));\n",
          stdout_all=["2", "6", "x"])

    check("builtin signature argument check",
          'std.math.sqrt("x");\n',
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
          "func(entry) -> entry[1]);\n"
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

    check("function result type",
          "func fetch(x: int): string { return \"ok\"; }\n"
          "let s: string = fetch(1);\n"
          "let f: (int) -> string = fetch;\n"
          "println(s); println(f(2));\n",
          stdout_all=["ok", "ok"], not_stderr="type warning")

    check("function result mismatch",
          "func fetch(x: int): string { return \"ok\"; }\n"
          "let f: (int) -> int = fetch;\n",
          stderr="type error: expected (int) -> int, "
                 "got (int) -> string")

    check("launch task void",
          "func noop(): void { let x = 1; }\n"
          "let t: task[void] = launch noop();\n"
          "println(str(t.wait()));\n",
          stdout="null", not_stderr="type warning")

    check("launch task any",
          "func work(n) { return n + 1; }\n"
          "let t: task[any] = launch work(41);\n"
          "println(t.wait());\n",
          stdout="42", not_stderr="type warning")

    check("task invariant mismatch",
          "struct Dog { name: string; }\n"
          "struct Animal { name: string; }\n"
          "func make_dog(): Dog { return Dog { name: \"d\" }; }\n"
          "let t: task[Animal] = launch make_dog();\n",
          stderr="expected task[Animal], got task[Dog]")

    check("launch lambda task type",
          "let add = func(a: int, b: int): int {\n"
          "  return a + b;\n"
          "};\n"
          "let t: task[int] = launch add(1, 2);\n"
          "println(t.wait());\n",
          stdout="3", not_stderr="type warning")

    check("method result type",
          "struct Box { value: int; }\n"
          "impl Box {\n"
          "  func get(self, x: int): string { return \"ok\"; }\n"
          "}\n"
          "let b: Box = Box { value: 1 };\n"
          "let s: string = b.get(2);\n"
          "let f = b.get;\n"
          "let g: (int) -> string = f;\n"
          "println(s); println(g(3));\n",
          stdout_all=["ok", "ok"], not_stderr="type warning")

    check("task wait builtins",
          "import std;\n"
          "func fetch(x: int): string { return \"ok\"; }\n"
          "let ts: list[task[string]] = [launch fetch(1), launch fetch(2)];\n"
          "let rs: list[string] = std.task.wait_all(ts);\n"
          "let s: string = std.task.wait(launch fetch(3));\n"
          "println(rs); println(s);\n",
          stdout_all=["[ok, ok]", "ok"], not_stderr="type warning")

    check("task detach builtin type",
          "import std;\n"
          "func fetch(): string { return \"ok\"; }\n"
          "let d: task[string] = std.task.detach(launch fetch());\n"
          "let s: string = d.wait();\n"
          "println(s);\n",
          stdout="ok", not_stderr="type warning")

    check("generator element type",
          "import std;\n"
          "func parts() { yield \"hello\"; yield \"world\"; }\n"
          "let g: generator[string] = parts();\n"
          "let xs: list[string] = std.iter.collect(g);\n"
          "println(xs);\n",
          stdout="[hello, world]", not_stderr="type warning")

    check("generator element mismatch",
          "import std;\n"
          "func nums() { yield 1; yield 2; }\n"
          "let xs: list[string] = std.iter.collect(nums());\n",
          stderr="type error: expected list[string], got list[int]")

    check("generator for element type",
          "func parts() { yield \"a\"; }\n"
          "for (let x in parts()) {\n"
          "  let xs: list[int] = [x];\n"
          "}\n",
          stderr="type error: expected int, got string")

    check("generator next value type",
          "import std;\n"
          "func parts() { yield \"a\"; }\n"
          "let r = std.generator.next(parts());\n"
          "let xs: list[int] = [r[\"value\"]];\n",
          stderr="type error: expected int, got string")

    check("generator lambda and method",
          "import std;\n"
          "let make = func() { yield 1; yield 2; };\n"
          "let g: generator[int] = make();\n"
          "println(std.iter.collect(g));\n"
          "struct Box { n: int; }\n"
          "impl Box {\n"
          "  func nums(self) { yield self.n; yield self.n + 1; }\n"
          "}\n"
          "let b: Box = Box { n: 5 };\n"
          "let h: generator[int] = b.nums();\n"
          "println(std.iter.collect(h));\n",
          stdout_all=["[1, 2]", "[5, 6]"], not_stderr="type warning")

    check("iter generators",
          "import std;\n"
          "func nums() { yield 1; yield 2; yield 3; }\n"
          "let mapped: list[string] = std.iter.collect(\n"
          "  std.iter.map(nums(), func(x: int): string -> str(x)));\n"
          "let filtered: list[int] = std.iter.collect(\n"
          "  std.iter.filter(nums(), func(x: int): bool -> x > 1));\n"
          "println(mapped); println(filtered);\n",
          stdout_all=["[1, 2, 3]", "[2, 3]"], not_stderr="type warning")

    check("generator annotation mismatch",
          "func parts() { yield \"a\"; }\n"
          "let g: generator[int] = parts();\n",
          stderr="type error: expected generator[int], got generator[string]")

    check("generator suspension",
          "import std;\n"
          "func gen() {\n"
          "  std.task.sleep(1).wait();\n"
          "  yield 1;\n"
          "  yield 2;\n"
          "}\n"
          "let g: generator[int] = gen();\n"
          "let xs: list[int] = std.iter.collect(g);\n"
          "println(xs);\n",
          stdout="[1, 2]", not_stderr="type warning")

    check("generator yielded values",
          "import std;\n"
          "func fetch(x: int): int { return x * 2; }\n"
          "func gen() {\n"
          "  yield 1;\n"
          "  yield fetch(3);\n"
          "}\n"
          "let xs: list[int] = std.iter.collect(gen());\n"
          "println(xs);\n",
          stdout="[1, 6]", not_stderr="type warning")

    check("generator send type inference",
          "import std;\n"
          "func echo() {\n"
          "  let x: string = yield \"a\";\n"
          "  yield x + \"!\";\n"
          "}\n"
          "let g: generator[string, string] = echo();\n"
          "println(std.generator.next(g, \"hi\"));\n"
          "println(std.generator.next(g, \"!\"));\n",
          stdout_all=["a", "!!"], not_stderr="type warning")

    check("generator send value mismatch",
          "import std;\n"
          "func echo() {\n"
          "  let x: string = yield \"a\";\n"
          "  yield x;\n"
          "}\n"
          "std.generator.next(echo(), 1);\n",
          stderr="type error: expected string, got int")

    check("generator declared yield and send",
          "import std;\n"
          "func typed(): generator[int, string] {\n"
          "  yield 1;\n"
          "  let x = yield 2;\n"
          "  yield len(x);\n"
          "}\n"
          "let g: generator[int, string] = typed();\n"
          "println(std.generator.next(g, \"ok\"));\n",
          stdout="1", not_stderr="type warning")

    check("generator declared yield mismatch",
          "func g(): generator[int, string] {\n"
          "  yield \"a\";\n"
          "}\n",
          stderr="type error: expected int, got string")

    check("pair indexing and destructuring",
          "import std;\n"
          "let p: pair[int, string] = (1, \"a\");\n"
          "let xs: list[int] = [p[0]];\n"
          "let ys: list[string] = [p[1]];\n"
          "let [n, s] = p;\n"
          "let zs: list[int] = [n];\n"
          "let ws: list[string] = [s];\n"
          "println(xs); println(ys); println(zs); println(ws);\n",
          stdout_all=["[1]", "[a]", "[1]", "[a]"], not_stderr="type warning")

    check("pair literal mismatch",
          "let p: pair[int, string] = (\"a\", 1);\n",
          stderr="type error: expected int, got string")

    check("pair length mismatch",
          "let p: pair[int, string] = (1, \"a\", true);\n",
          stderr="type error: pair expects 2 element(s), got 3")

    check("zip pair type",
          "import std;\n"
          "let pairs: list[pair[int, string]] = "
          "std.list.zip([1, 2], [\"a\", \"b\"]);\n"
          "let first = pairs[0];\n"
          "let xs: list[int] = [first[0]];\n"
          "let ys: list[string] = [first[1]];\n"
          "println(xs); println(ys);\n",
          stdout_all=["[1]", "[a]"], not_stderr="type warning")

    check("items and enumerate pair types",
          "import std;\n"
          "let items: list[pair[string, int]] = "
          "std.dict.items({\"a\": 1});\n"
          "let e: generator[pair[int, int]] = "
          "std.iter.enumerate([10, 20]);\n"
          "println(items); println(std.iter.collect(e));\n",
          stdout_all=["[[a, 1]]", "[[0, 10], [1, 20]]"],
          not_stderr="type warning")

    check("union values",
          "import std;\n"
          "let x: union[int, string] = 1;\n"
          "x = \"a\";\n"
          "let xs = [1, \"a\"];\n"
          "let ys: list[union[int, string]] = xs;\n"
          "println(ys);\n",
          stdout="[1, a]", not_stderr="type warning")

    check("union mismatch",
          "import std;\n"
          "let x: union[int, string] = true;\n",
          stderr="type error: expected union[int, string], got bool")

    check("union function parameter",
          "func f(x: union[int, string]) { }\n"
          "f(1);\n"
          "f(\"a\");\n"
          "f(true);\n",
          stderr="type error: expected union[int, string], got bool")

    check("nullable union matches optional",
          "let a: int? = null;\n"
          "let b: union[int, null] = a;\n"
          "let c: int? = b;\n"
          "let d: union[int, null];\n"
          "let e: union[int, string, null];\n"
          "println(a); println(b); println(c); println(d); println(e);\n",
          stdout_all=["null", "null", "null", "null", "null"],
          not_stderr="type warning")

    check("nullable union function type",
          "func f(x: int?): int? -> x;\n"
          "let g: (union[int, null]) -> union[int, null] = f;\n"
          "println(g(1));\n",
          stdout="1", not_stderr="type warning")

    check("nullable union runtime check",
          "func f(x: union[int, null]) { println(x); }\n"
          "let a: any = true;\n"
          "f(a);\n",
          stderr="runtime error: function f argument 1 (x) expects int?, got bool")

    check("nullable union suffix accepts null",
          "func f(x: union[int, string]?) {\n"
          "  if (x == null) { println(\"null\"); }\n"
          "  else if (x is int) { println(\"int\"); }\n"
          "  else { println(\"string\"); }\n"
          "}\n"
          "let y: union[int, string]?;\n"
          "f(null); f(1); f(\"s\");\n"
          "println(y);\n",
          stdout_all=["null", "int", "string", "null"],
          not_stderr="type warning")

    check("union is narrowing",
          "func use_int(v: int) { }\n"
          "func use_string(v: string) { }\n"
          "func f(x: union[int, string]) {\n"
          "  if (x is int) { use_int(x); }\n"
          "  else { use_string(x); }\n"
          "}\n",
          not_stderr="type warning")

    check("union is narrowing mismatch",
          "func use_string(v: string) { }\n"
          "func f(x: union[int, string]) {\n"
          "  if (x is int) { use_string(x); }\n"
          "}\n",
          stderr="type error: expected string, got int")

    check("union negation and and narrowing",
          "func use_int(v: int) { }\n"
          "func use_string(v: string) { }\n"
          "func f(x: union[int, string]) {\n"
          "  if (!(x is int)) { use_string(x); }\n"
          "  else { use_int(x); }\n"
          "  if (x is int && x > 0) { use_int(x); }\n"
          "}\n",
          not_stderr="type warning")

    check("union match narrowing",
          "func use_int(v: int) { }\n"
          "func use_string(v: string) { }\n"
          "func f(x: union[int, string]) {\n"
          "  match (x) {\n"
          "    case is int => use_int(x);\n"
          "    case is string => use_string(x);\n"
          "  };\n"
          "  match (x) {\n"
          "    case is int => use_int(x);\n"
          "    default => use_string(x);\n"
          "  };\n"
          "}\n",
          not_stderr="type warning")

    check("union match narrowing mismatch",
          "func use_string(v: string) { }\n"
          "func f(x: union[int, string]) {\n"
          "  match (x) {\n"
          "    case is int => use_string(x);\n"
          "    default => 0;\n"
          "  };\n"
          "}\n",
          stderr="type error: expected string, got int")

    check("field path narrowing",
          "func use_int(v: int) { }\n"
          "func use_string(v: string) { }\n"
          "struct Box { value: union[int, string]; }\n"
          "struct Outer { box: Box; }\n"
          "func f(b: Box) {\n"
          "  if (b.value is int) { use_int(b.value); }\n"
          "  else { use_string(b.value); }\n"
          "  match (b.value) {\n"
          "    case is int => use_int(b.value);\n"
          "    default => use_string(b.value);\n"
          "  };\n"
          "}\n"
          "func g(o: Outer) {\n"
          "  if (o.box.value is int) { use_int(o.box.value); }\n"
          "}\n",
          not_stderr="type warning")

    check("field path narrowing mismatch",
          "func use_string(v: string) { }\n"
          "struct Box { value: union[int, string]; }\n"
          "func f(b: Box) {\n"
          "  if (b.value is int) { use_string(b.value); }\n"
          "}\n",
          stderr="type error: expected string, got int")

    check("field path invalidation",
          "func use_int(v: int) { }\n"
          "struct Box { value: union[int, string]; }\n"
          "impl Box { func clear(self) { } }\n"
          "func f(b: Box) {\n"
          "  if (b.value is int) {\n"
          "    b.clear();\n"
          "    use_int(b.value);\n"
          "  }\n"
          "}\n",
          stderr="type error: expected int, got union[int, string]")

    check("union or negative narrowing",
          "func use_string(v: string) { }\n"
          "func f(x: union[int, string]) {\n"
          "  if (x is int || x is int) { }\n"
          "  else { use_string(x); }\n"
          "}\n",
          not_stderr="type warning")

    check("flat union three members",
          "import std;\n"
          "let x: union[int, string, bool] = 1;\n"
          "x = \"a\";\n"
          "x = true;\n"
          "let xs = [1, \"a\", true];\n"
          "let ys: list[union[int, string, bool]] = xs;\n"
          "let z: union[int, string, bool, null] = null;\n"
          "println(ys); println(z);\n",
          stdout_all=["[1, a, true]", "null"], not_stderr="type warning")

    check("flat union narrowing",
          "func use_sb(v: union[string, bool]) { }\n"
          "func f(x: union[int, string, bool]) {\n"
          "  if (x is int) { }\n"
          "  else { use_sb(x); }\n"
          "  match (x) {\n"
          "    case is int => 0;\n"
          "    default => use_sb(x);\n"
          "  };\n"
          "}\n",
          not_stderr="type warning")

    check("flat pair arity rejected",
          "let p: pair[int, string, bool] = (1, \"a\", true);\n",
          stderr="type error: type pair expects 2 type argument(s)")

    check("tuple literal inference",
          "let p = (1, \"a\");\n"
          "let t = (1, \"a\", true);\n"
          "let xs: list[int] = [p[0]];\n"
          "let ys: list[string] = [p[1]];\n"
          "let zs: list[bool] = [t[2]];\n"
          "let [a, b] = p;\n"
          "let [c, d, e] = t;\n"
          "println(xs); println(ys); println(zs); println(a); println(e);\n",
          stdout_all=["[1]", "[a]", "[true]", "1", "true"],
          not_stderr="type warning")

    check("tuple type annotations",
          "let p: pair[int, string] = (1, \"a\");\n"
          "let t: tuple[int, string, bool] = (1, \"a\", true);\n"
          "println(p); println(t);\n",
          stdout_all=["[1, a]", "[1, a, true]"], not_stderr="type warning")

    check("tuple length mismatch",
          "let t: tuple[int, string] = (1, \"a\", true);\n",
          stderr="type error: tuple expects 2 element(s), got 3")

    check("tuple function argument",
          "func use(p: pair[int, string]) { }\n"
          "use((1, \"a\"));\n"
          "use((1, 2));\n",
          stderr="type error: expected string, got int")

    check("tuple call boundary",
          "func id(v: pair[int, string]): pair[int, string] {\n"
          "  return v;\n"
          "}\n"
          "let p: pair[int, string] = id((1, \"a\"));\n"
          "println(p);\n",
          stdout="[1, a]", not_stderr="type warning")

    check("tuple match subject",
          "match (1, \"a\") {\n"
          "  case [n, s] => println(n);\n"
          "  default => println(\"no\");\n"
          "};\n",
          stdout="1")

    check("opaque userdata types",
          "import std;\n"
          "func check() {\n"
          "  let l: listener = std.net.tcp_listen(\"127.0.0.1\", 0);\n"
          "  let addr: dict[string, any] = std.net.local_addr(l);\n"
          "  let s: socket = std.net.tcp_connect(\n"
          "    \"127.0.0.1\", addr[\"port\"]);\n"
          "  let st: stream = std.fs.open(\"/tmp/x\", \"w\");\n"
          "  let buf: stream = std.io.buffered(st, 1024);\n"
          "  let wt: task[websocket] =\n"
          "    std.websocket.connect_task(\"ws://x\");\n"
          "}\n",
          not_stderr="type warning")

    check("opaque userdata mismatch",
          "import std;\n"
          "func check() {\n"
          "  let l: listener = std.net.tcp_listen(\"127.0.0.1\", 0);\n"
          "  let s: socket = std.net.accept(l);\n"
          "  let x: listener = s;\n"
          "}\n",
          stderr="type error: expected listener, got socket")

    check("builtin task types",
          "import std;\n"
          "func check() {\n"
          "  let t = std.http.get_task(\"http://x\");\n"
          "  let d: dict[string, any] = t.wait();\n"
          "  let c = std.net.tcp_connect_task(\"127.0.0.1\", 1);\n"
          "  let sock: userdata = c.wait();\n"
          "  let u = std.net.recv_task(sock, 10);\n"
          "  let b: bytes = u.wait();\n"
          "}\n",
          not_stderr="type warning")

    check("builtin task mismatch",
          "import std;\n"
          "func check() {\n"
          "  let t: task[int] = std.http.get_task(\"http://x\");\n"
          "}\n",
          stderr="type error: expected task[int], "
                 "got task[dict[string, any]]")

    check("task control builtins",
          "import std;\n"
          "func check() {\n"
          "  let t = std.task.sleep(1);\n"
          "  let c = std.task.catch(t);\n"
          "  let r: result[null, error] = c.wait();\n"
          "  let tw: result[null, error] = std.task.try_wait(t);\n"
          "  let ta: result[list[null], error] =\n"
          "    std.task.try_wait_all([t]);\n"
          "  let to: result[null, error] = std.task.timeout(t, 10);\n"
          "  let rl = std.task.rate_limit(10, 2);\n"
          "  let ok: bool = std.task.acquire(rl);\n"
          "}\n",
          not_stderr="type warning")

    check("task try_wait mismatch",
          "import std;\n"
          "func check() {\n"
          "  let t = std.task.sleep(1);\n"
          "  let r: result[string, error] = std.task.try_wait(t);\n"
          "}\n",
          stderr="type error: expected result[string, error], "
                 "got result[null, error]")

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
        pkg_dir = os.path.join(tmp, "app")
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

    check("traits hashable constraint rejects list",
          "func f[T Hashable](x: T): T { return x; }\n"
          "f([1, 2]);\n",
          stderr="does not satisfy generic constraint")
    check("traits iterable constraint rejects int",
          "func f[T Iterable](x: T): T { return x; }\n"
          "f(1);\n",
          stderr="does not satisfy generic constraint")
    check("hashable cannot be used with union",
          "func f[T Hashable | int](x: T): T { return x; }\n",
          stderr="cannot be used with '|'")
    check("iterable cannot be used with union",
          "func f[T Iterable | int](x: T): T { return x; }\n",
          stderr="cannot be used with '|'")
    check("iterable proto rejects list-returning iter",
          "struct A { func iter(self): list[int] { return [1]; } }\n"
          "func f[T Iterable](x: T): T { return x; }\n"
          "f(A {});\n",
          stderr="does not satisfy generic constraint")
    check("hashable let annotation rejects list",
          "let x: Hashable = [1, 2];\n",
          stderr="expected Hashable, got list[int]")
    check("iterable let annotation rejects int",
          "let x: Iterable = 1;\n",
          stderr="expected Iterable, got int")
    check("for in user iterable requires generator return",
          "struct B { xs: list[int]; "
          "func iter(self): list[int] { return self.xs; } }\n"
          "for x in (B { xs: [1] }) { }\n",
          stderr="does not implement Iterable; iter must return generator[T]")
    check("for in user type without iter",
          "struct N { x: int; }\n"
          "for x in (N { x: 1 }) { }\n",
          stderr="is not iterable")
    check("iterable let annotation checks element type",
          'let x: Iterable[int] = ["a"];\n',
          stderr="expected Iterable[int], got list[string]")

    proc = run(
        "func f(a: int, b: string) { return a; }\n"
        'f("x", 1);\n')
    assert proc.stderr.count("type error:") == 1, proc.stderr

    # Embedded std packages skip the checker at startup for speed.  The
    # SKEIN_TYPECHECK_EMBEDDED override keeps them covered by this suite.
    embedded_env = dict(os.environ, SKEIN_TYPECHECK="strict",
                        SKEIN_TYPECHECK_EMBEDDED="1")
    proc = run("import std.web;\n"
               "import std.auth;\n"
               "import std.cache;\n"
               "import std.kv;\n"
               "import std.fsm;\n"
               'println("ok");\n',
               env=embedded_env)
    assert proc.returncode == 0, proc.stderr
    assert "type error" not in proc.stderr, proc.stderr
    assert "ok" in proc.stdout, proc.stdout
