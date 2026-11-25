#!/usr/bin/env python3
"""Pattern matching, enums, structs, char and byte.

Sorting cases live in tests/skein/match_types_sorting.skein.  Char and byte
cases live in tests/skein/match_types_char_byte.skein.  Enum/struct decode cases
live in tests/skein/match_types_decode.skein.
"""
import os
import tempfile

from harness import *  # noqa: F401,F403


def run_cases():
    run_skein("match_types_sorting.skein")
    run_skein("match_types_char_byte.skein")
    run_skein("match_types_decode.skein")
    check(
        "list sort golden output",
        "import std;\n"
        "println(std.list.sort([3, 1, 2]));\n",
        stdout="[1, 2, 3]",
    )
    check(
        "char golden output",
        "let c = 'a';\n"
        "println(c);\n"
        "println(kind_of(c));\n",
        stdout_all=["a", "char"],
    )
    check(
        "decode golden output",
        "import std;\n"
        "struct D { x: int; }\n"
        "let d = std.struct.decode({\"x\": 1}, \"D\");\n"
        "println(d.x);\n",
        stdout="1",
    )


    check("match as whole value",
          'match ({"a": 1}) { case {a} as whole => '
          'println(whole["a"] + a); }\n',
          stdout="2")
    check("match as struct",
          'struct P { x: int; }\n'
          'let p = P { x: 1 };\n'
          'match (p) { case P {x} as whole => println(whole.x + x); }\n',
          stdout="2")
    check("match value pattern const",
          'const IDLE = "idle";\n'
          'match (IDLE) { case == IDLE => println("idle"); '
          'default => println("no"); }\n',
          stdout="idle")
    check("match value pattern literal",
          'match (1) { case == 1 => println("one"); default => println("no"); }\n',
          stdout="one")
    check("match value pattern enum",
          'enum E { A; B; }\n'
          'match (E.A) { case == E.A => println("a"); default => println("no"); }\n',
          stdout="a")
    check("match value pattern rejects comparison",
          'match (1) { case < 1 => println("bad"); default => println("no"); }\n',
          stderr="only == is allowed in a value pattern")
    check("match multi scrutinee",
          'match (1, 2) { case [a, b] => println(a + b); default => println(0); }\n',
          stdout="3")
    check("match multi scrutinee literal",
          'match (1, 2) { case [1, 2] => println("pair"); '
          'default => println("no"); }\n',
          stdout="pair")
    check("match multi scrutinee enum",
          'enum S { Idle; }\n'
          'enum E { Msg { text: string; }; }\n'
          'match (S.Idle, E.Msg { text: "hi" }) {\n'
          '  case [S.Idle, E.Msg {text}] => println(text);\n'
          '  default => println("no");\n'
          '}\n',
          stdout="hi")
    check("match multi scrutinee needs list pattern",
          'match (1, 2) { case 1 => println("bad"); default => println("no"); }\n',
          stdout="no")

    check("enum no payload",
          'enum E { A; B; }\nprintln(E.A);\nprintln(E.A == E.A);\n'
          'println(E.A == E.B);\n',
          stdout_all=["E.A", "true", "false"])
    check("enum payload",
          'enum E { A { n: int; s: string; }; B; }\n'
          'let e = E.A { n: 1, s: "x" };\n'
          'println(e.n);\nprintln(e.s);\nprintln(e);\n',
          stdout_all=["1", "x", "E.A { n: 1, s: x }"])
    check("enum type info",
          'enum E { A; }\nprintln(kind_of(E.A));\nprintln(type_name(E.A));\n'
          'println(type_package(E.A));\n',
          stdout_all=["enum", "E", "main"])
    check("enum type annotation",
          'enum E { A { n: int; }; }\nlet e: E = E.A { n: 1 };\nprintln(e.n);\n',
          stdout="1")
    check("enum variant pattern",
          'enum E { A { n: int; }; B; }\n'
          'println(match (E.A { n: 2 }) { '
          'case E.A {n} if n > 1 => "big"; case E.A {n} => "small"; '
          'case E.B => "b"; });\n',
          stdout="big")
    check("enum variant pattern rest",
          'enum E { A { n: int; s: string; }; }\n'
          'println(match (E.A { n: 1, s: "x" }) { '
          'case E.A {n, ...rest} => rest; });\n',
          stdout="{s: x}")
    check("enum field wrong variant",
          'enum E { A { n: int; }; B; }\nprintln(E.B.n);\n',
          stderr="field n not found in E.B")
    check("enum unknown variant",
          'enum E { A; }\nE.B\n', stderr="unknown enum variant E.B")
    check("enum unknown field in construction",
          'enum E { A { n: int; }; }\nE.A { m: 1 }\n',
          stderr="unknown field m in E.A")
    check("enum missing field",
          'enum E { A { n: int; }; }\nE.A {}\n',
          stderr="field n of E.A is required")
    check("enum field type mismatch",
          'enum E { A { n: int; }; }\nE.A { n: "x" }\n',
          stderr="field n of E.A expects int, got string")
    check("enum variant repeated",
          'enum E { A; A; }\n', stderr="enum variant repeated A")
    check("enum empty payload rejected",
          'enum E { A {}; }\n', stderr="empty payload")
    check("enum json",
          'import std;\nenum E { A { n: int; }; B; }\n'
          'println(std.json.stringify(E.A { n: 1 }));\n'
          'println(std.json.stringify(E.B));\n',
          stdout_all=[
              '{"$enum":"E.A","payload":{"n":1}}',
              '{"$enum":"E.B","payload":{}}',
          ])
    check("struct json",
          'import std;\nstruct P { x: int; y: string; }\n'
          'println(std.json.stringify(P { x: 1, y: "a" }));\n',
          stdout='{"x":1,"y":"a"}')
    check("recursive struct json",
          'import std;\nstruct Node { value: int; next: Node?; }\n'
          'println(std.json.stringify(Node { value: 1, next: null }));\n',
          stdout='{"value":1,"next":null}')
    check("enum nested in match",
          'enum E { A { n: int; }; }\n'
          'println(match (E.A { n: 3 }) { '
          'case E.A {n} => match (n) { case 3 => "three"; default => "no"; }; '
          'default => "no"; });\n',
          stdout="three")
    check("enum self reference",
          'enum Expr { Add { l: Expr; r: Expr; }; Num { n: int; }; }\n'
          'let e = Expr.Add { l: Expr.Num { n: 1 }, r: Expr.Num { n: 2 } };\n'
          'println(match (e) { case Expr.Add {l, r} => "add"; '
          'default => "other"; });\n',
          stdout="add")
    check("struct self reference",
          'struct Node { value: int; next: Node?; }\n'
          'let n = Node { value: 1, next: null };\n'
          'let m = Node { value: 2, next: n };\n'
          'println(m.value);\nprintln(m.next.value);\nprintln(m.next.next);\n',
          stdout_all=["2", "1", "null"])
    check("struct nullable type mismatch",
          'struct Node { next: Node?; }\nNode { next: 1 }\n',
          stderr="type error: expected Node?, got int")
    check("nullable int",
          'let x: int? = null;\nlet y: int? = 3;\nlet z: int?;\n'
          'println(x);\nprintln(y);\nprintln(z);\n',
          stdout_all=["null", "3", "null"])
    check("nullable type mismatch",
          'let x: int? = "s";\n', stderr="type error: expected int?, got string")
    check("struct cycle rejected",
          'struct Node { value: int; next: Node?; }\n'
          'let a = Node { value: 1, next: null };\n'
          'let b = Node { value: 2, next: null };\n'
          'a.next = b;\nb.next = a;\n',
          stderr="recursive value would create a cycle")
    check("struct indirect cycle rejected",
          'struct Node { value: int; kids: list; }\n'
          'let a = Node { value: 1, kids: [] };\n'
          'let b = Node { value: 2, kids: [] };\n'
          'a.kids = [b];\nb.kids = [a];\n',
          stderr="recursive value would create a cycle")
    check("struct dag sharing allowed",
          'struct Node { value: int; next: Node?; }\n'
          'let c = Node { value: 3, next: null };\n'
          'let a = Node { value: 1, next: c };\n'
          'let b = Node { value: 2, next: c };\n'
          'println(a.next.value);\nprintln(b.next.value);\n',
          stdout_all=["3", "3"])
    check("struct self assignment rejected",
          'struct Node { value: int; next: Node?; }\n'
          'let a = Node { value: 1, next: null };\n'
          'a.next = a;\n',
          stderr="recursive value would create a cycle")

    check("destructure dict shorthand",
          'let {a, b} = {"a": 1, "b": 2};\nprintln(a);\nprintln(b);\n',
          stdout_all=["1", "2"])
    check("destructure dict alias",
          'let {a: x} = {"a": 9};\nprintln(x);\n', stdout="9")
    check("destructure dict string key",
          'let {"a-b": x} = {"a-b": 7};\nprintln(x);\n', stdout="7")
    check("destructure dict rest",
          'let {a, ...rest} = {"a": 1, "b": 2, "c": 3};\nprintln(rest);\n',
          stdout="{b: 2, c: 3}")
    check("destructure list",
          'let [a, b] = [1, 2];\nprintln(a + b);\n', stdout="3")
    check("destructure list rest",
          'let [h, ...t] = [1, 2, 3];\nprintln(h);\nprintln(t);\n',
          stdout_all=["1", "[2, 3]"])
    check("destructure nested",
          'let {user: {name}} = {"user": {"name": "alice"}};\nprintln(name);\n',
          stdout="alice")
    check("destructure struct",
          'struct P { x: int; y: int; }\n'
          'let pt = P { x: 1, y: 2 };\n'
          'let P {x, y} = pt;\nprintln(x + y);\n',
          stdout="3")
    check("destructure missing key",
          'let {missing} = {};\n',
          stderr="destructuring pattern did not match dict")
    check("destructure list length mismatch",
          'let [a, b] = [1];\n',
          stderr="destructuring pattern did not match list")
    check("destructure repeated symbol",
          'let a = 1;\nlet {a} = {"a": 2};\n', stderr="symbol repeated a")
    check("destructure const read-only",
          'const {a} = {"a": 1};\na = 2;\n',
          stderr="cannot assign to constant a")
    check("destructure let writable",
          'let {a} = {"a": 1};\na = 2;\nprintln(a);\n', stdout="2")
    check("destructure keeps whole via rest",
          'let d = {"a": 1, "b": 2};\nlet {a} = d;\nprintln(d["b"]);\n',
          stdout="2")

    check("std.fsm embedded import",
          'import std.fsm;\n'
          'let m = fsm.define(0, func(state, ev) { return state + ev; });\n'
          'let ctx = fsm.start(m);\n'
          'println(ctx.state);\n'
          'ctx = fsm.step(m, ctx, 2);\n'
          'println(ctx.state);\n'
          'ctx = fsm.run(m, ctx, [1, 2, 3]);\n'
          'println(ctx.state);\n'
          'println(fsm.history(ctx));\n',
          stdout_all=["0", "2", "8", "[0, 2, 3, 5, 8]"])
    check("std.fsm undo redo",
          'import std.fsm;\n'
          'let m = fsm.define(0, func(state, ev) { return state + ev; });\n'
          'let ctx = fsm.run(m, fsm.start(m), [1, 2, 3]);\n'
          'ctx = fsm.undo(ctx);\n'
          'println(ctx.state);\n'
          'println(fsm.snapshot(ctx)["redo"]);\n'
          'ctx = fsm.redo(ctx);\n'
          'println(ctx.state);\n',
          stdout_all=["3", "[3]", "6"])
    check("std.fsm large run",
          'import std.fsm;\n'
          'let m = fsm.define(0, func(state, ev) { return state + 1; });\n'
          'let ctx = fsm.run(m, fsm.start(m), range(20000));\n'
          'println(ctx.state);\n'
          'println(len(fsm.history(ctx)));\n',
          stdout_all=["20000", "20001"])
    check("std.fsm branch clears redo",
          'import std.fsm;\n'
          'let m = fsm.define(0, func(state, ev) { return state + ev; });\n'
          'let ctx = fsm.run(m, fsm.start(m), [1, 2, 3]);\n'
          'ctx = fsm.undo(ctx);\n'
          'ctx = fsm.step(m, ctx, 10);\n'
          'println(ctx.state);\n'
          'println(fsm.snapshot(ctx)["redo"]);\n'
          'println(fsm.history(ctx));\n',
          stdout_all=["13", "[]", "[0, 1, 3, 13]"])
    check("std.fsm alias import",
          'import std.fsm as f;\n'
          'let m = f.define(7, func(ctx, ev) { return ctx; });\n'
          'println(f.initial(m));\n',
          stdout="7")
    check("std.fsm invalid step",
          'import std.fsm;\nfsm.define(0, 1);\n',
          stderr="uncaught fail: step must be callable")
    check("std.fsm private helper hidden",
          'import std.fsm;\nprintln(fsm._internal());\n',
          stderr="key not found")
    check("std.fsm package-private field hidden",
          'import std.fsm;\n'
          'let m = fsm.define(7, func(ctx, ev) { return ctx; });\n'
          'let ctx = fsm.start(m);\n'
          'println(ctx._history);\n',
          stderr="private member _history")
    check("std.fsm coexists with std",
          'import std;\nimport std.fsm;\n'
          'println(std.regex.match("a+", "a"));\n'
          'println(fsm.initial(fsm.define(5, func(ctx, ev) { return ctx; })));\n',
          stdout_all=["true", "5"])
    check("std.enum.decode rejects non-enum",
          'import std;\nstd.enum.decode({"a": 1});\n',
          stderr="std.enum.decode expects an enum object")
    check("std.enum.tag errors",
          'import std;\nenum E { A; }\nstd.enum.tag(1);\n',
          stderr="std.enum.tag expects an enum value")
    check("std.enum.tag unknown variant",
          'import std;\nenum E { A; }\nstd.enum.tag("E", "Nope");\n',
          stderr="unknown enum variant E.Nope")

    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write("aa.skein",
              "package aa { export enum E { A { n: int; }; } }\n")
        main = write(
            "main.skein",
            "import std;\n"
            "import aa;\n"
            "enum E { A { n: int; }; }\n"
            'let v = {"$enum": "E.A", "payload": {"n": 1}};\n'
            'println(std.enum.tag(std.enum.decode(v, "main.E")));\n'
            'println(std.enum.tag(std.enum.decode(v, "aa.E")));\n')
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == [
            "main.E.A",
            "aa.E.A",
        ], proc.stdout

    check("std.struct.decode missing field",
          'import std;\nstruct P { x: int; }\n'
          'std.struct.decode({}, "P");\n',
          stderr="missing field x in P")
    check("std.struct.decode unknown field",
          'import std;\nstruct P { x: int; }\n'
          'std.struct.decode({"x": 1, "y": 2}, "P");\n',
          stderr="unknown field y in P")
    check("std.struct.decode type mismatch",
          'import std;\nstruct P { x: int; }\n'
          'std.struct.decode({"x": "s"}, "P");\n',
          stderr="P.x expects int, got string")
    check("std.struct.decode unknown option",
          'import std;\nstruct C { x: int = 1; }\n'
          'std.struct.decode({}, "C", {"default": true});\n',
          stderr="unknown option default")
    check("std.enum.decode type hint mismatch",
          'import std;\nenum E { A { n: int; }; }\n'
          'enum F { A { n: int; }; }\n'
          'std.enum.decode({"$enum": "F.A", "payload": {"n": 1}}, "E");\n',
          stderr="does not match expected main.E")
    check("std.enum.decode unknown type hint",
          'import std;\nenum E { A { n: int; }; }\n'
          'std.enum.decode({"$enum": "E.A", "payload": {"n": 1}}, "Nope");\n',
          stderr="unknown or ambiguous enum type Nope")
    check("std.fsm snapshot restore",
          'import std.fsm;\n'
          'let m = fsm.define(0, func(s, e) { return s + e; });\n'
          'let ctx = fsm.run(m, fsm.start(m), [1, 2, 3]);\n'
          'let ctx2 = fsm.restore(fsm.snapshot(ctx));\n'
          'println(ctx2.state);\nprintln(fsm.history(ctx2));\n',
          stdout_all=["6", "[0, 1, 3, 6]"])
    check("std.fsm on_transition trace",
          "import std;\nimport std.fsm;\n"
          "let trail = [];\n"
          "let m = fsm.define(0, func(s, e) { return s + e; },\n"
          "  func(from, ev, to) { "
          "trail = std.list.append(trail, [from, ev, to]); });\n"
          "let ctx = fsm.run(m, fsm.start(m), [1, 2]);\n"
          "println(ctx.state);\nprintln(trail);\n",
          stdout_all=["3", "[[0, 1, 1], [1, 2, 3]]"])
    check("std.fsm on_transition must be callable",
          "import std.fsm;\n"
          "fsm.define(0, func(s, e) { return s; }, 1);\n",
          stderr="on_transition must be callable")
    check("std.fsm enum step type check",
          "import std;\nimport std.fsm;\n"
          "enum S { A; B; }\n"
          "let m = fsm.define(S.A, func(s, e) { return \"x\"; });\n"
          "fsm.step(m, fsm.start(m), 1);\n",
          stderr="step must return S, got string string")
    check("std.fsm struct step type check",
          "import std;\nimport std.fsm;\n"
          "struct P { x: int; }\n"
          "struct Q { y: int; }\n"
          "let m = fsm.define(P { x: 1 }, func(s, e) { return Q { y: 2 }; });\n"
          "fsm.step(m, fsm.start(m), 1);\n",
          stderr="step must return P, got struct Q")
    check("std.fsm json snapshot",
          'import std;\nimport std.fsm;\n'
          'enum S { Idle; Count { n: int; }; Done; }\n'
          'let m = fsm.define(S.Idle, func(state, ev) {\n'
          '  return match ([state, ev]) {\n'
          '    case [S.Idle, "inc"] => S.Count { n: 1 };\n'
          '    case [S.Count {n}, "inc"] => S.Count { n: n + 1 };\n'
          '    case [S.Count {n}, "done"] => S.Done;\n'
          '    default => state;\n'
          '  };\n'
          '});\n'
          'let ctx = fsm.run(m, fsm.start(m), ["inc", "inc", "done"]);\n'
          'let json = std.json.stringify(fsm.snapshot(ctx));\n'
          'let ctx2 = fsm.restore(std.json.parse(json));\n'
          'println(ctx2.state);\nprintln(fsm.history(ctx2));\n',
          stdout_all=["S.Done", "[S.Idle, S.Count { n: 1 }, S.Count { n: 2 }, S.Done]"])
    check("std.fsm struct snapshot with decoder",
          'import std;\nimport std.fsm;\n'
          'struct S { n: int; }\n'
          'let m = fsm.define(S { n: 0 }, func(state, ev) {\n'
          '  return S { n: state.n + ev };\n'
          '});\n'
          'let ctx = fsm.run(m, fsm.start(m), [1, 2, 3]);\n'
          'let json = std.json.stringify(fsm.snapshot(ctx));\n'
          'let ctx2 = fsm.restore(std.json.parse(json), '
          'func(v) { return std.struct.decode(v, "S"); });\n'
          'println(ctx2.state.n);\nprintln(fsm.history(ctx2));\n',
          stdout_all=["6", "[S { n: 0 }, S { n: 1 }, S { n: 3 }, S { n: 6 }]"])
    check("list sort mixed types rejected",
          'import std;\nstd.list.sort([1, "a"]);\n',
          stderr="std.list.sort expects all numbers, strings, chars, or bytes")
    check("list sort comparator must return int",
          'import std;\nstd.list.sort([1, 2], '
          'func(a, b) { return "x"; });\n',
          stderr="std.list.sort comparator must return int, got string")
    check("char invalid empty",
          "''\n", stderr="empty character literal")
    check("char invalid multi",
          "'ab'\n", stderr="exactly one character")
    check("char invalid surrogate",
          r"'\u{D800}'" + "\n", stderr="not a Unicode scalar value")
    check("char invalid range",
          r"'\u{110000}'" + "\n", stderr="not a Unicode scalar value")
    check("char conversion error",
          "char(\"ab\")\n", stderr="char() expects exactly one UTF-8 character")
    check("byte range error",
          "byte(256)\n", stderr="byte() value out of range")
    check("char from bytes error",
          'std.bytes.to_char(std.bytes.from_string("ab"))\n',
          stderr="char_from_bytes expects exactly one UTF-8 character")
    check("dict large lookup and order",
          "import std;\nlet d = {};\n"
          "for (let i in range(200)) { d[i] = i * 2; }\n"
          "println(len(d));\nprintln(d[100]);\nprintln(std.dict.keys(d)[0]);\n"
          "println(std.dict.keys(d)[199]);\n",
          stdout_all=["200", "200", "0", "199"])
    check("dict scalar keys",
          "import std;\n"
          "let d = {1: 'i', true: 'b', 's': 's', 'a': 'c', "
          "byte(65): 'y', std.bytes.from_hex(\"00\"): 'x', null: 'n'};\n"
          "println(d[1]); println(d[true]); println(d['s']); println(d['a']);\n"
          "println(d[byte(65)]); println(d[std.bytes.from_hex(\"00\")]); println(d[null]);\n",
          stdout_all=["i", "b", "s", "c", "y", "x", "n"])
    check("dict float key",
          "let d = {1.5: 1, 1: 2, 2.5: 3};\n"
          "println(d[1.5]);\nprintln(d[1.0]);\nprintln(d[2.5]);\n"
          "println(len(d));\n",
          stdout_all=["1", "2", "3", "3"])
    check("dict list key rejected",
          "{[1]: 1}\n",
          stderr="dict key must be int, float (not NaN), bool, string, char, byte, bytes, null, an immutable tuple of these, or a hashable struct/enum, got list")
    check("dict struct key",
          "struct P { x: int; }\n"
          "let d = {P { x: 1 }: 'a'};\n"
          "println(d[P { x: 1 }]);\n",
          stdout_all=["a"])
    check("dict nested struct key",
          "struct Inner { id: int; }\n"
          "struct Outer { inner: Inner; tag: string; }\n"
          'let k = Outer { inner: Inner { id: 1 }, tag: "x" };\n'
          "let d = {k: true};\n"
          'println(d[Outer { inner: Inner { id: 1 }, tag: "x" }]);\n',
          stdout_all=["true"])
    check("dict struct key snapshot after mutation",
          "import std;\n"
          "struct Key { id: int; }\n"
          "let k = Key { id: 1 };\n"
          "let d = {k: 42};\n"
          "k.id = 2;\n"
          "println(d[Key { id: 1 }]);\n"
          "println(std.dict.has(d, k));\n",
          stdout_all=["42", "0"])
    check("dict struct with mutable field rejected",
          "struct P { xs: list[int]; }\n"
          "{P { xs: [1] }: 1}\n",
          stderr="dict key must be int, float (not NaN), bool, string, char, byte, bytes, null, an immutable tuple of these, or a hashable struct/enum")
    check("dict enum key",
          "enum E { A; B { id: int; } }\n"
          "let d = {E.A: 'a', E.B { id: 1 }: 'b'};\n"
          "println(d[E.A]);\n"
          "println(d[E.B { id: 1 }]);\n",
          stdout_all=["a", "b"])
    check("dict enum mutable payload rejected",
          "enum E { A { xs: list[int]; } }\n"
          "{E.A { xs: [1] }: 1}\n",
          stderr="dict key must be int, float (not NaN), bool, string, char, byte, bytes, null, an immutable tuple of these, or a hashable struct/enum")
    check("dict tuple keys",
          "let d = {(1, 'a'): 10, (2, 'b'): 20};\n"
          "println(d[(1, 'a')]);\n"
          "d[(3, 'c')] = 30;\n"
          "println(d[(3, 'c')]);\n"
          "println(len(d));\n",
          stdout_all=["10", "30", "3"])
    check("dict nested tuple keys",
          "let d = {((1, 2), 'x'): true};\n"
          "println(d[((1, 2), 'x')]);\n",
          stdout_all=["true"])
    check("dict large tuple keys",
          "let d = {};\n"
          "let i = 0;\n"
          "while (i < 40) { d[(i, i + 1)] = i; i = i + 1; }\n"
          "println(d[(20, 21)]);\n"
          "println(len(d));\n"
          "println(d[(39, 40)]);\n",
          stdout_all=["20", "40", "39"])
    check("dict tuple mutable element rejected",
          "{(1, [2]): 1}\n",
          stderr="dict key must be int, float (not NaN), bool, string, char, byte, bytes, null, an immutable tuple of these, or a hashable struct/enum")
    check("dict tuple float element",
          "let d = {(1, 2.0): 1, (3, 4.5): 2};\n"
          "println(d[(1, 2)]);\nprintln(d[(3.0, 4.5)]);\n"
          "println(len(d));\n",
          stdout_all=["1", "2", "2"])
    check("tuple index assignment rejected",
          "let t = (1, 2);\nt[0] = 9;\n",
          stderr="cannot modify an immutable tuple")
    check("tuple concatenation remains hashable",
          "let a = (1, 2);\nlet b = (3, 4);\nlet c = a + b;\n"
          "println({c: true}[(1, 2, 3, 4)]);\n"
          "let xs = [1, 2];\nlet ys = [3, 4];\nlet zs = xs + ys;\n"
          "zs[0] = 9;\nprintln(zs);\n"
          "let mixed = [0] + (1, 2);\nprintln(mixed);\nprintln(a);\n",
          stdout_all=["true", "[9, 2, 3, 4]", "[0, 1, 2]", "[1, 2]"])

    check("match type pattern",
          'println(match (5) { case is int => "int"; '
          'case is string => "string"; default => "other"; });\n'
          'println(match ("x") { case is int => "int"; '
          'case is string => "string"; default => "other"; });\n',
          stdout_all=["int", "string"])
    check("match type pattern as",
          'println(match (5) { case is int as n => n + 1; '
          'default => 0; });\n',
          stdout="6")
    check("match type pattern struct",
          'struct P { x: int; }\n'
          'let p = P { x: 3 };\n'
          'println(match (p) { case is P as value => value.x; '
          'case is int => -1; default => 0; });\n',
          stdout="3")
    check("match enum exhaustive",
          'enum E { A; B; }\n'
          'let e = E.B;\n'
          'println(match (e) { case E.A => 1; case E.B => 2; });\n',
          stdout="2")
    check("match enum exhaustive type",
          'enum E { A; B; }\n'
          'let e = E.A;\n'
          'println(match (e) { case is E => 9; });\n',
          stdout="9")
    check("match enum missing variant",
          'enum E { A; B; }\n'
          'let e = E.A;\n'
          'match (e) { case E.A => 1; }\n',
          stderr="match is not exhaustive: missing E.B")
    check("match enum guarded arm not exhaustive",
          'enum E { A; B; }\n'
          'let e = E.A;\n'
          'match (e) { case E.A if true => 1; }\n',
          stderr="match is not exhaustive: missing E.A, E.B")

    check("match or pattern",
          'println(match (2) { case 1 | 2 | 3 => "small"; default => "other"; });\n'
          'println(match ("b") { case "a" | "b" => "ab"; default => "x"; });\n',
          stdout_all=["small", "ab"])
    check("match range pattern",
          'println(match (3) { case 1..5 => "low"; case 6..9 => "high"; '
          'default => "other"; });\n'
          'println(match (7) { case 1..5 => "low"; case 6..9 => "high"; '
          'default => "other"; });\n',
          stdout_all=["low", "high"])
    check("match enum or exhaustive",
          'enum E { A; B; C; }\n'
          'let e = E.B;\n'
          'println(match (e) { case E.A | E.B | E.C => "all"; });\n',
          stdout="all")
    check("match enum or missing variant",
          'enum E { A; B; C; }\n'
          'let e = E.A;\n'
          'match (e) { case E.A | E.B => 1; }\n',
          stderr="match is not exhaustive: missing E.C")
    check("function parameter destructuring",
          'func sum([a, b]) { return a + b; }\n'
          'func pair({x, y}) { return x * y; }\n'
          'struct P { x: int; y: int; }\n'
          'func point(P {x, y}) { return x + y; }\n'
          'func rest([h, ...t]) { return str(h) + ":" + str(len(t)); }\n'
          'println(sum([2, 3]));\n'
          'println(pair({"x": 3, "y": 4}));\n'
          'println(point(P { x: 5, y: 6 }));\n'
          'println(rest([9, 8, 7]));\n',
          stdout_all=["5", "12", "11", "9:2"])
    check("function parameter destructuring mismatch",
          'func f([a, b]) { return a + b; }\n'
          'f([1]);\n',
          stderr="function parameter does not match its pattern")
    check("function parameter destructuring uses compiled pattern",
          'func sum([a, b]) { return a + b; }\n'
          'func pair({x, y}) { return x * y; }\n'
          'func rest([h, ...t]) { return str(h) + ":" + str(len(t)); }\n'
          'println(sum([2, 3]));\n'
          'println(pair({"x": 3, "y": 4}));\n'
          'println(rest([9, 8, 7]));\n',
          stdout_all=["5", "12", "9:2"],
          env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_DEBUG="1"),
          not_stderr="fallback parameter pattern")

    check("function parameter patterns bind in vm frames",
          'import std;\n'
          'struct P { x: int; y: int; }\n'
          'func nested([[a, b], {x, y}]) { return a + b + x + y; }\n'
          'func rest([h, ...t]) { return h * len(t); }\n'
          'func point(P {x, y}) { return x * y; }\n'
          'func rec([n, acc]) {\n'
          '  if (n <= 0) { return acc; }\n'
          '  return rec([n - 1, acc + n]);\n'
          '}\n'
          'func aspat([a as whole]) { return len(whole) + a[0]; }\n'
          'println(str(nested([[1, 2], {"x": 3, "y": 4}])));\n'
          'println(str(rest([5, 1, 2])));\n'
          'println(str(point(P { x: 3, y: 4 })));\n'
          'println(str(rec([4, 0])));\n'
          'println(str(aspat([[7]])));\n',
          stdout_all=["10", "10", "12", "10", "8"],
          env=dict(os.environ, SKEIN_VM="1", SKEIN_VM_STATS="1"),
          stderr="calls=")

    check("match or pattern binding rollback",
          'println(match ([1, 2]) { case [a] | [a, b] => a + b; '
          'default => 0; });\n'
          'println(match ({"x": 1}) { case {x: a, y: b} | {x: a} => a; '
          'default => 0; });\n',
          stdout_all=["3", "1"])
