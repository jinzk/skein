#!/usr/bin/env python3
"""Core language: literals, operators, control flow, switch.

Extracted from the original test_skein.py main() body, lines 710-1099.
"""
import os

from harness import *  # noqa: F401,F403


def run_cases():
    check("addition", "1+2\n", stdout="= 3")
    check("pipe mode has no REPL prompt", "1+2\n", stdout="= 3")
    assert "> " not in run("1+2\n").stdout
    check("ans and underscore reference last result",
          "1 + 2\nans * 4\n_\n_ + 1\n",
          stdout_all=["= 3", "= 12", "= 12", "= 13"])
    check("float", "1.5*2\n", stdout="= 3.0")
    check("float keeps type in output", "1.0\n", stdout="= 1.0")
    check("float shortest round-trip", "1.23456\n", stdout="= 1.23456")
    check("float binary rounding", "0.1 + 0.2\n",
          stdout="= 0.30000000000000004")
    check("integer division", "10/4\n", stdout="= 2")
    check("float division", "10/4.0\n", stdout="= 2.5")
    check("integer division truncates toward zero", "-7 / 4\n", stdout="= -1")
    check("int64 addition", "2147483647 + 1\n", stdout="= 2147483648")
    check("int64 maximum", "9223372036854775807\n",
          stdout="= 9223372036854775807")
    check("integer overflow", "9223372036854775807 + 1\n",
          stderr="integer overflow")
    check("integer multiplication boundary",
          "3037000499 * 3037000499\n", stdout="= 9223372030926249001")
    check("integer multiplication overflow",
          "3037000500 * 3037000500\n", stderr="integer overflow")
    check("INT64_MIN times -1", "(-9223372036854775807 - 1) * -1\n",
          stderr="integer overflow")
    check("out-of-range integer literal becomes float",
          "9223372036854775808\n", stdout="= 9.223372036854776e+18")
    check("integer literal underscores", "1_000_000\n", stdout="= 1000000")
    check("integer literal underscores near int64 max",
          "9_223_372_036_854_775_807\n",
          stdout="= 9223372036854775807")
    check("out-of-range integer literal with underscores becomes float",
          "9_223_372_036_854_775_808\n",
          stdout="= 9.223372036854776e+18")
    check("hex integer literal", "0xff\n", stdout="= 255")
    check("uppercase hex integer literal", "0XFF\n", stdout="= 255")
    check("hex integer literal mixed case", "0xDeAdBeEf\n",
          stdout="= 3735928559")
    check("hex integer literal underscores", "0xFF_FF\n", stdout="= 65535")
    check("binary integer literal", "0b1010\n", stdout="= 10")
    check("uppercase binary integer literal", "0B1111\n", stdout="= 15")
    check("binary integer literal underscores", "0b1010_0101\n",
          stdout="= 165")
    check("octal integer literal", "0o17\n", stdout="= 15")
    check("uppercase octal integer literal", "0O777\n", stdout="= 511")
    check("octal integer literal underscores", "0o7_7\n", stdout="= 63")
    check("prefixed literals in arithmetic",
          "0x10 + 0b10 + 0o10\n", stdout="= 26")
    check("hex int64 maximum", "0x7FFFFFFFFFFFFFFF\n",
          stdout="= 9223372036854775807")
    check("hex integer literal out of range", "0x8000000000000000\n",
          stderr="hexadecimal integer literal out of range")
    check("invalid binary digit", "0b102\n",
          stderr="invalid binary integer literal")
    check("invalid octal digit", "0o8\n",
          stderr="invalid octal integer literal")
    check("empty hex literal", "0x\n",
          stderr="invalid hexadecimal integer literal")
    check("hex literal trailing underscore", "0xF_\n",
          stderr="invalid hexadecimal integer literal")
    check("hex literal underscore after prefix", "0x_FF\n",
          stderr="invalid hexadecimal integer literal")
    check("consecutive integer underscores", "1__0\n",
          stderr="invalid numeric literal")
    check("scientific notation", "1e3\n", stdout="= 1000.0")
    check("scientific notation uppercase", "1E+3\n", stdout="= 1000.0")
    check("scientific notation negative exponent", "1e-3\n",
          stdout="= 0.001")
    check("scientific notation with dot and no fraction", "1.e2\n",
          stdout="= 100.0")
    check("scientific notation leading dot", ".5e2\n", stdout="= 50.0")
    check("float literal underscores", "1_000.5_5\n", stdout="= 1000.55")
    check("scientific notation exponent underscores", "1.5e1_0\n",
          stdout="= 15000000000.0")
    check("INT64_MIN division overflow",
          "(-9223372036854775807 - 1) / -1\n", stderr="integer overflow")
    check("int64 exact mixed comparison",
          "9007199254740993 == 9007199254740992.0\n", stdout="= false")

    check("modulo", "7 % 4\n", stdout="= 3")
    check("float modulo", "7.5 % 2\n", stdout="= 1.5")
    check("modulo sign follows dividend", "-7 % 4\n", stdout="= -3")
    check("modulo negative divisor", "7 % -4\n", stdout="= 3")
    check("modulo by zero", "1 % 0\n", stderr="modulo by zero")

    check("postfix increment returns old value",
          "let x = 1;\nlet a = x++;\na\nx\n", stdout_all=["= 1", "= 2"])
    check("prefix increment returns new value",
          "let x = 1;\nlet a = ++x;\na\nx\n", stdout_all=["= 2", "= 2"])
    check("postfix decrement returns old value",
          "let x = 2;\nlet a = x--;\na\nx\n", stdout_all=["= 2", "= 1"])
    check("prefix decrement returns new value",
          "let x = 2;\nlet a = --x;\na\nx\n", stdout_all=["= 1", "= 1"])
    check("increment in for loop",
          "let n = 0;\nfor (let i = 0; i < 3; i++) { n = n + i; }\nn\n",
          stdout="= 3")
    check("for in without let over list",
          "for x in [1, 2, 3] { println(x); }\n",
          stdout="1\n2\n3\n")
    check("for in without let over dict keys",
          'for k in ({"a": 1, "b": 2}) { println(k); }\n',
          stdout="a\nb\n")
    check("for in without let dict pairs",
          'for k, v in ({"a": 1, "b": 2}) { println(k + "=" + str(v)); }\n',
          stdout="a=1\nb=2\n")
    check("for in without let range builtin",
          "for i in range(3) { println(i); }\n",
          stdout="0\n1\n2\n")
    check("for in without let empty body",
          "for x in [] {}\n", code=0)
    check("for in without let shadows outer variable",
          "let x = 99;\nfor x in [1, 2] { println(x); }\nprintln(x);\n",
          stdout="1\n2\n99\n")
    check("for in without let iterable uses outer binding",
          "let x = [4, 5];\nfor x in x { println(x); }\nprintln(len(x));\n",
          stdout="4\n5\n2\n")
    check("for in without let string",
          'for c in "ab" { println(c); }\n', stdout="a\nb\n")
    check("for in without let bytes",
          'for b in bytes_from_hex("00ff") { println(b); }\n',
          stdout="0\n255\n")
    check("for in without let generator",
          "func gen() { yield 1; yield 2; }\n"
          "for x in gen() { println(x); }\n",
          stdout="1\n2\n")
    check("for in without let nested label",
          "label outer: for i in [1, 2] {\n"
          "  for j in [1, 2] {\n"
          "    if (j == 2) { continue outer; }\n"
          "    println(i * 10 + j);\n"
          "  }\n"
          "}\n",
          stdout="11\n21\n")
    check("for in without let break",
          "for x in [1, 2, 3] { if (x == 2) { break; } println(x); }\n",
          stdout="1\n")
    check("for in without let set",
          "import std;\n"
          "let s = std.set.from_list([1, 2, 3]);\n"
          "let n = 0;\n"
          "for v in s { n = n + v; }\n"
          "println(n);\n",
          stdout="6\n")
    check("for in without let infers element type",
          "for x in [1, 2] { let y: string = x; }\n",
          stderr="variable y expects string, got int")
    check("for in without let requires block",
          "let xs = [1, 2];\nfor x in xs println(x);\n",
          stderr="syntax error")
    check("for in without let dict literal operand",
          'for k, v in {1: 10, 2: 20} { println(str(k) + "=" + str(v)); }\n',
          stdout="1=10\n2=20\n")
    check("for in without let empty dict literal operand",
          'for k in {} { println(k); }\n',
          stdout="")
    check("for in without let struct literal requires parens",
          "struct P { x: int = 1; }\n"
          'for p in P { x: 1 } { println(p.x); }\n',
          stderr="syntax error")
    check("for in without let two vars require dict",
          "for k, v in [1, 2] { println(k); }\n",
          stderr="two-variable for-in requires dict")
    check("for in without let rejects three variables",
          'for a, b, c in ({"a": 1}) { println(a); }\n',
          stderr="syntax error")
    check("std.iter.for_each over list",
          "import std;\n"
          'std.iter.for_each([1, 2, 3], func(x) { print(x); })\n'
          'println("");\n',
          stdout="123\n")
    check("std.iter.for_each over dict keys",
          "import std;\n"
          'std.iter.for_each({"a": 1, "b": 2}, func(k) { print(k); })\n'
          'println("");\n',
          stdout="ab\n")
    check("std.iter.for_each list index and value",
          "import std;\n"
          "std.iter.for_each([10, 20], func(i, x) {"
          ' print(str(i) + ":" + str(x) + " "); })\n'
          'println("");\n',
          stdout="0:10 1:20 \n")
    check("std.iter.for_each dict key and value",
          "import std;\n"
          'std.iter.for_each({"a": 1, "b": 2}, func(k, v) {'
          ' print(k + "=" + str(v) + " "); })\n'
          'println("");\n',
          stdout="a=1 b=2 \n")
    check("std.iter.for_each string index and char",
          "import std;\n"
          'std.iter.for_each("ab", func(i, c) {'
          ' print(str(i) + ":" + str(c) + " "); })\n'
          'println("");\n',
          stdout="0:a 1:b \n")
    check("std.iter.for_each closure accumulator",
          "import std;\n"
          "let total = 0;\n"
          "std.iter.for_each([1, 2, 3], func(x) { total = total + x; })\n"
          "println(total);\n",
          stdout="6\n")
    check("std.iter.for_each string and bytes",
          "import std;\n"
          'std.iter.for_each("ab", func(c) { print(c); })\n'
          'std.iter.for_each(bytes_from_hex("00ff"), func(b) { print(b); })\n'
          'println("");\n',
          stdout="ab0255\n")
    check("std.iter.for_each generator",
          "import std;\n"
          "func gen() { yield 1; yield 2; }\n"
          'std.iter.for_each(gen(), func(v) { print(v); })\n'
          'println("");\n',
          stdout="12\n")
    check("std.iter.for_each set",
          "import std;\n"
          "let n = 0;\n"
          "std.iter.for_each(std.set.from_list([1, 2, 3]),"
          " func(v) { n = n + v; })\n"
          "println(n);\n",
          stdout="6\n")
    check("std.iter.for_each first class",
          "import std;\n"
          "let f = std.iter.for_each;\n"
          'f([1, 2], func(x) { print(x); })\nprintln("");\n',
          stdout="12\n")
    check("std.iter.for_each return exits callback only",
          "import std;\n"
          "std.iter.for_each([1, 2, 3], func(x) {"
          " if (x == 2) { return; } print(x); })\n"
          'println("");\n',
          stdout="13\n")
    check("std.iter.for_each requires callable",
          "import std;\nstd.iter.for_each([1], 42)\n",
          stderr="std.iter.for_each expects a callable")
    check("std.iter.for_each arity",
          "import std;\nstd.iter.for_each([1])\n",
          stderr="std.iter.for_each expects 2 arguments")
    check("std.iter.for_each rejects three callback parameters",
          "import std;\n"
          "std.iter.for_each([1], func(a, b, c) { print(a); })\n",
          stderr="callback must accept 1 or 2 parameters")
    check("std.iter.for_each propagates callback errors",
          "import std;\n"
          "std.iter.for_each([1, 2], func(x) { missing_symbol; })\n",
          stderr="symbol not found missing_symbol")
    check("for_each is not a global builtin",
          "for_each([1], func(x) { })\n",
          stderr="symbol not found for_each")
    check("body brace must share header line",
          "func f() {\n"
          "  if (true)\n"
          "  {\n"
          "  }\n"
          "}\n"
          "f();\n",
          stderr="opening brace must be on the same line as its header")
    check("else must share closing brace line",
          "func f() {\n"
          "  if (false) {\n"
          "  }\n"
          "  else {\n"
          "  }\n"
          "}\n"
          "f();\n",
          stderr="else must be on the same line as the preceding token")
    check("do while must share closing brace line",
          'do {\n  print("x");\n}\nwhile (false);\n',
          stderr="while must be on the same line as the preceding '}'")
    check("struct body brace must share header line",
          "func f() {\n"
          "  struct P\n"
          "  {\n"
          "    x: int = 1;\n"
          "  }\n"
          "}\n"
          "f();\n",
          stderr="opening brace must be on the same line as its header")
    check("switch body brace must share header line",
          "func f() {\n"
          "  switch (1)\n"
          "  {\n"
          '    case 1 => print("x");\n'
          "  }\n"
          "}\n"
          "f();\n",
          stderr="opening brace must be on the same line as its header")
    check("for in body brace must share header line",
          "func f() {\n"
          "  for x in [1, 2]\n"
          "  {\n"
          "    print(x);\n"
          "  }\n"
          "}\n"
          "f();\n",
          stderr="opening brace must be on the same line as its header")
    check("increment float", "let x = 1.5;\nx++\nx\n++x\nx\n",
          stdout_all=["= 1.5", "= 2.5", "= 3.5", "= 3.5"])
    check("increment list element",
          "let a = [1, 2];\na[0]++;\n++a[1];\na\n",
          stdout_all=["= 1", "= 3", "= [2, 3]"])
    check("increment nested index",
          "let a = [[1]];\na[0][0]++;\na\n",
          stdout_all=["= 1", "= [[2]]"])
    check("increment dict element",
          'let d = {"x": 5};\nd["x"]++;\n++d["x"];\nd\n',
          stdout_all=["= 5", "= 7", "= {x: 7}"])
    check("increment bytes element",
          'let b = bytes_from_hex("00ff");\nb[0]++;\nb[1]--;\nb\n',
          stdout_all=["= 0", "= 255", "= <bytes: 2>"])
    check("increment evaluates index once",
          "let i = 0;\nlet a = [10];\na[i++]++;\ni\na\n",
          stdout_all=["= 10", "= 1", "= [11]"])
    check("increment const binding",
          "const x = 1;\nx++;\n",
          stderr="cannot assign to constant x")
    check("increment const container element",
          "const a = [1];\na[0]++;\na\n", stdout="= [2]")
    check("const scalar propagation",
          "func f() {\n"
          "  const A = 2;\n"
          "  const B = A * 3 + 1;\n"
          "  return B;\n"
          "}\n"
          "println(f());\n",
          stdout="7")
    check("const propagation shadowing",
          "func f() {\n"
          "  const A = 1;\n"
          "  { const A = 5; println(A); }\n"
          "  { let A = 9; println(A); }\n"
          "  println(A);\n"
          "}\n"
          "f();\n",
          stdout_all=["5", "9", "1"])
    check("const string concatenation",
          'const S = "a" + "b";\nprintln(S);\n',
          stdout="ab")
    check("const container is not propagated",
          "func f() {\n"
          "  const L = [1];\n"
          "  L[0] = 2;\n"
          "  return L[0];\n"
          "}\nprintln(f());\n",
          stdout="2")
    check("const propagation can be disabled",
          "func f() {\n"
          "  const A = 2;\n"
          "  const B = A * 3 + 1;\n"
          "  return B;\n"
          "}\nprintln(f());\n",
          stdout="7",
          env=dict(os.environ, SKEIN_VM="1", SKEIN_NO_CONST_PROP="1"))
    check("increment string", 'let s = "a";\ns++;\n',
          stderr="cannot increment string")
    check("decrement bool", "let b = true;\nb--;\n",
          stderr="cannot decrement bool")
    check("increment literal", "1++\n",
          stderr="cannot increment this expression")
    check("increment missing dict key",
          'let d = {};\nd["x"]++;\n', stderr="key not found")
    check("increment integer overflow",
          "let x = 9223372036854775807;\nx++;\n",
          stderr="integer overflow")
    check("increment precedence with unary minus",
          "let x = 5;\n-x++\nx\n", stdout_all=["= -5", "= 6"])
    check("compound assignment operators",
          "let x = 10;\n"
          "x += 5;\nx\n"
          "x -= 3;\nx\n"
          "x *= 2;\nx\n"
          "x /= 4;\nx\n"
          "x %= 4;\nx\n"
          "x &= 3;\nx\n"
          "x |= 4;\nx\n"
          "x ^= 1;\nx\n"
          "x <<= 2;\nx\n"
          "x >>= 1;\nx\n",
          stdout_all=["= 15", "= 12", "= 24", "= 6", "= 2", "= 2",
                      "= 6", "= 7", "= 28", "= 14"])
    check("compound string and list",
          'let s = "a";\ns += "b";\ns\n'
          "let a = [1];\na += [2, 3];\na\n",
          stdout_all=["= ab", "= [1, 2, 3]"])
    check("compound index and field",
          "let a = [10];\nlet i = 0;\na[i++] += 5;\ni\na\n"
          "let p = {\"x\": 1};\np[\"x\"] += 2;\np\n"
          "struct P { x: int; }\nlet q = P { x: 1 };\nq.x += 4;\nq\n",
          stdout_all=["= 15", "= 1", "= [15]", "= 3", "= {x: 3}",
                      "= 5", "= P { x: 5 }"])
    check("compound typed variable type error",
          "let x: int = 1;\nx += \"bad\";\n",
          stderr="cannot add int and string")
    check("compound const error",
          "const x = 1;\nx += 1;\n",
          stderr="cannot assign to constant x")
    check("list slice",
          "let xs = [0, 1, 2, 3, 4];\n"
          "xs[1:4]\n"
          "xs[:2]\n"
          "xs[2:]\n"
          "xs[:]\n",
          stdout_all=["= [1, 2, 3]", "= [0, 1]", "= [2, 3, 4]",
                      "= [0, 1, 2, 3, 4]"])
    check("list slice negative indices",
          "let xs = [0, 1, 2, 3, 4];\n"
          "xs[-2:]\n"
          "xs[:-1]\n"
          "xs[-100:100]\n"
          "xs[100:-100]\n",
          stdout_all=["= [3, 4]", "= [0, 1, 2, 3]", "= [0, 1, 2, 3, 4]",
                      "= []"])
    check("list slice step",
          "let xs = [0, 1, 2, 3, 4];\n"
          "let i = 1;\n"
          "let j = 2;\n"
          "xs[::2]\n"
          "xs[1::2]\n"
          "xs[i::j]\n"
          "xs[4:0:-2]\n"
          "xs[::-1]\n",
          stdout_all=["= [0, 2, 4]", "= [1, 3]", "= [1, 3]", "= [4, 2]",
                      "= [4, 3, 2, 1, 0]"])
    check("string slice uses codepoints",
          'println("你好世界"[1:3]);\n'
          'println("你好世界"[::-1]);\n',
          stdout_all=["好世", "界世好你"])
    check("bytes slice uses byte indices",
          'println(bytes_hex(bytes_from_string("abcd")[1:3]));\n'
          'println(bytes_hex(bytes_from_string("abcd")[::-1]));\n',
          stdout_all=["6263", "64636261"])
    check("slice step zero",
          "println([1, 2, 3][::0]);\n",
          stderr="slice step cannot be zero")
    check("slice bound must be integer",
          "println([1, 2, 3][1.0:]);\n",
          stderr="slice start must be an integer")
    check("slice unsupported type",
          "println(1[0:1]);\n",
          stderr="slice expects list, string, or bytes")
    check("logical and", "1 && 2\n", stdout="= true")
    check("logical and short-circuit", "0 && (1/0)\n", stdout="= false")
    check("logical or", "0 || 2\n", stdout="= true")
    check("logical or short-circuit", "1 || (1/0)\n", stdout="= true")
    check("logical or false", "0 || 0\n", stdout="= false")
    check("logical not", "!0\n", stdout="= true")
    check("logical not true", "!true\n", stdout="= false")
    check("logical not string", '!""\n', stdout="= true")
    check("logical result is bool",
          "func f(x: bool) { return x; }\nf(1 && 2)\nf(!0)\n",
          stdout_all=["= true", "= true"])
    check("logical result format", 'format("{}", true && false)\n',
          stdout="= false")
    check("bitwise and", "1 & 3\n", stdout="= 1")
    check("bitwise rejects bool", "1 & (1 == 1)\n",
          stderr="bitwise operator expects ints")
    check("bitwise or", "1 | 2\n", stdout="= 3")
    check("bitwise xor", "5 ^ 3\n", stdout="= 6")
    check("bitwise not", "~0\n", stdout="= -1")
    check("shift left", "1 << 4\n", stdout="= 16")
    check("shift right", "16 >> 2\n", stdout="= 4")
    check("int64 shift", "1 << 62\n", stdout="= 4611686018427387904")
    check("shift count out of range", "1 << 64\n",
          stderr="shift count 64 out of range")
    check("arithmetic vs equality precedence", "1 + 2 == 3\n",
          stdout="= true")
    check("equality result is bool",
          "func f(x: bool) { return x; }\nf(1 < 2)\n",
          stdout="= true")
    check("bool true literal", "true\n", stdout="= true")
    check("bool false literal", "false\n", stdout="= false")
    check("bool print", "println(true)\nprintln(false)\n",
          stdout_all=["true", "false"])
    check("bool truthiness",
          'if (true) { println("yes"); } else { println("no"); }\n',
          stdout="yes")
    check("bool false truthiness",
          'if (false) { println("yes"); } else { println("no"); }\n',
          stdout="no")
    check("bool equality",
          "true == true\ntrue == false\ntrue != false\n",
          stdout_all=["= true", "= false", "= true"])
    check("bool is distinct from int",
          "true == 1\ntrue != 1\n", stdout_all=["= false", "= true"])
    check("bool conversions",
          "str(true)\nint(true)\nint(false)\nfloat(true)\n",
          stdout_all=["= true", "= 1", "= 0", "= 1.0"])
    check("bool conversion builtin",
          'bool(0)\nbool(1)\nbool("")\nbool("x")\nbool(null)\nbool([])\n',
          stdout_all=["= false", "= true", "= false", "= true",
                      "= false", "= false"])
    check("bool as value", "let f = bool;\nf(1)\n", stdout="= true")
    check("bool format", 'format("{}", true)\n', stdout="= true")
    check("bool arithmetic error", "true + 1\n",
          stderr="cannot add bool and int")

    check("ternary true", "1 ? 2 : 3\n", stdout="= 2")
    check("ternary false", "0 ? 2 : 3\n", stdout="= 3")
    check("ternary string", 'true ? "yes" : "no"\n', stdout="= yes")
    check("ternary short-circuit then", "1 ? 5 : 1/0\n", stdout="= 5")
    check("ternary short-circuit else", "0 ? 1/0 : 5\n", stdout="= 5")
    check("ternary right associative", "0 ? 1 : 0 ? 2 : 3\n",
          stdout="= 3")
    check("ternary nested then", "1 ? 0 ? 2 : 3 : 4\n",
          stdout="= 3")
    check("ternary comparison condition", '1 < 2 ? "a" : "b"\n',
          stdout="= a")
    check("ternary assignment",
          "let x = 0;\nlet y = x ? 10 : 20;\ny\n", stdout="= 20")
    check("ternary list element", "[0 ? 1 : 2, 3]\n", stdout="= [2, 3]")
    check("ternary dict value", '{"a": 0 ? 1 : 2}\n', stdout="{a: 2}")
    check("ternary dict key", '{1 ? "a" : "b": 2}\n', stdout="{a: 2}")
    check("ternary bool result",
          "func f(x: bool) { return x; }\nf(1 < 2 ? true : false)\n",
          stdout="= true")

    check("switch int",
          'switch (2) { case 1 => println("one"); '
          'case 2, 3 => println("two or three"); '
          'default => println("other"); }\n',
          stdout="two or three")
    check("switch prefixed integer",
          'switch (0xff) { case 0b1111_1111 => println("match"); '
          'default => println("other"); }\n',
          stdout="match")
    check("switch default",
          'switch (9) { case 1 => println("one"); '
          'default => println("other"); }\n',
          stdout="other")
    check("switch no match no default",
          'switch (9) { case 1 => println("one"); }\n', code=0)
    check("switch empty body", "switch (1) {}\n", code=0)
    check("switch empty body spaced", "switch (1) { }\n", code=0)
    check("switch single statement",
          "let x = 0;\nswitch (1) { case 1 => x = 10; }\nx\n",
          stdout="= 10")
    check("switch block",
          "switch (1) { case 1 => { println(1); println(2); } }\n",
          stdout_all=["1", "2"])
    check("switch case scope",
          "switch (1) { case 1 => let y = 2; }\ny\n",
          stderr="symbol not found y")
    check("switch string",
          'switch ("a") { case "b" => println("bad"); '
          'case "a" => println("ok"); }\n', stdout="ok")
    check("switch bool",
          'switch (true) { case false => println("bad"); '
          'case true => println("ok"); }\n', stdout="ok")
    check("switch null",
          'switch (null) { case null => println("null"); }\n',
          stdout="null")
    check("switch bytes",
          'switch (bytes_from_hex("00ff")) { '
          'case bytes_from_hex("00ff") => println("bytes"); }\n',
          stdout="bytes")
    check("switch no fallthrough",
          'switch (1) { case 1 => println("one"); '
          'case 2 => println("two"); }\n', stdout="one")
    check("switch logical selector",
          'switch (1 < 2) { case true => println("ok"); }\n', stdout="ok")
    check("switch break exits switch",
          "let hits = 0;\n"
          "while (true) {\n"
          "  switch (1) { case 1 => break; }\n"
          "  hits = hits + 1;\n"
          "  if (hits == 2) { break; }\n"
          "}\n"
          "hits\n",
          stdout="= 2")
    check("switch continue outer loop",
          'let out = "";\n'
          "for (let i in [0, 1, 2]) {\n"
          "  switch (i) { case 1 => continue; }\n"
          "  out = out + str(i);\n"
          "}\n"
          "out\n",
          stdout="= 02")
    check("switch return",
          "func f(x) {\n"
          "  switch (x) { case 1 => return 10; default => return 20; }\n"
          "}\n"
          "f(1)\nf(2)\n",
          stdout_all=["= 10", "= 20"])
    check("switch nested",
          'switch (1) { case 1 => switch (1) { case 1 => println("nested"); } }\n',
          stdout="nested")
    check("switch default only",
          'switch (1) { default => println("only"); }\n', stdout="only")
    check("switch empty case block",
          "switch (1) { case 1 => {} }\n", code=0)
    check("switch empty statement case",
          "switch (1) { case 1 => ; }\n", code=0)
    check("switch float selector",
          'switch (1.0) { case 1 => println("bad"); }\n',
          stderr="switch does not support float")
    check("switch float case",
          'switch (1) { case 1.0 => println("bad"); }\n',
          stderr="switch does not support float case value")
    check("switch list case",
          'switch (1) { case [1] => println("bad"); }\n',
          stderr="switch does not support list case value")
    check("switch builtin case",
          'switch (1) { case sqrt => println("bad"); }\n',
          stderr="switch does not support builtin case value")
    check("switch type name is not pattern",
          'switch (1) { case int => println("bad"); }\n',
          stderr="switch does not support builtin case value")
    check("switch case condition rejected",
          'switch (true) { case 1 < 2 => println("bad"); }\n',
          stderr="logical or comparison expressions are not allowed in case values")
    check("switch list selector",
          'switch ([1]) { default => println("bad"); }\n',
          stderr="switch does not support list")

    check("match int default",
          'println(match (2) { case 1 => "one"; case 2 => "two"; '
          'default => "other"; });\n',
          stdout="two")
    check("match default",
          'println(match (9) { case 1 => "one"; default => "other"; });\n',
          stdout="other")
    check("match binding",
          'println(match (42) { case n => n + 1; });\n', stdout="43")
    check("match guard",
          'println(match (5) { case n if n > 10 => "big"; '
          'case n if n > 3 => "mid"; default => "small"; });\n',
          stdout="mid")
    check("match guard falls through",
          'println(match (1) { case n if n > 3 => "big"; '
          'default => "small"; });\n',
          stdout="small")
    check("match wildcard",
          'println(match (1) { case _ => "wild"; });\n', stdout="wild")
    check("match dict open",
          'let e = {"kind": "tool", "ok": true, "data": [1, 2]};\n'
          'println(match (e) { case {kind: "tool", ok: true, data: d} => d; '
          'default => null; });\n',
          stdout="[1, 2]")
    check("match dict ignores extra keys",
          'println(match ({"a": 1, "b": 2}) { case {a} => a; default => 0; });\n',
          stdout="1")
    check("match dict rest",
          'println(match ({"a": 1, "b": 2, "c": 3}) { '
          'case {a, ...rest} => rest; default => null; });\n',
          stdout="{b: 2, c: 3}")
    check("match dict missing key falls through",
          'println(match ({"a": 1}) { case {b} => "b"; default => "no b"; });\n',
          stdout="no b")
    check("match nested dict",
          'println(match ({"a": {"b": 2}}) { '
          'case {a: {b}} => b; default => 0; });\n',
          stdout="2")
    check("match list exact",
          'println(match ([1, 2]) { case [a] => "one"; '
          'case [a, b] => "two"; default => "other"; });\n',
          stdout="two")
    check("match list rest",
          'println(match ([1, 2, 3]) { case [h, ...t] => t; default => null; });\n',
          stdout="[2, 3]")
    check("match list ignore rest",
          'println(match ([1, 2, 3]) { case [h, ...] => h; default => 0; });\n',
          stdout="1")
    check("match empty list",
          'println(match ([]) { case [] => "empty"; default => "no"; });\n',
          stdout="empty")
    check("match struct pattern",
          'struct P { x: int; y: int; }\n'
          'let p = P { x: 1, y: 2 };\n'
          'println(match (p) { case P {x, y} => x + y; default => 0; });\n',
          stdout="3")
    check("match struct type only",
          'struct P { x: int; }\n'
          'println(match (P {x: 1}) { case P {} => "is P"; default => "no"; });\n',
          stdout="is P")
    check("match dict type only",
          'println(match ({"a": 1}) { case {...} => "dict"; default => "no"; });\n',
          stdout="dict")
    check("match expression value",
          'let x = match (1) { case 1 => {"ok": true}; '
          'default => {"ok": false}; };\n'
          'println(x["ok"]);\n',
          stdout="true")
    check("match nested",
          'println(match (1) { case 1 => match (2) { case 2 => "nested"; '
          'default => "no"; }; default => "no"; });\n',
          stdout="nested")
    check("match binding scope",
          'match (1) { case x => 0; }\nx\n', stderr="symbol not found x")
    check("match binding is read-only",
          'match (1) { case x => { x = 2; } }\n',
          stderr="cannot assign to constant x")
    check("match duplicate binding",
          'match ({"a": 1, "b": 2}) { case {a: x, b: x} => 1; default => 0; }\n',
          stderr="pattern binds x more than once")
    check("match no arm matched",
          'match (9) { case 1 => "one"; }\n',
          stderr="match: no arm matched")
    check("match empty body",
          'match (1) {}\n', stderr="match requires at least one arm")
    check("match default must be last",
          'match (1) { default => "d"; case 1 => "one"; }\n',
          stderr="default must be the last match arm")
    check("match no fallthrough",
          'println(match (1) { case 1 => "one"; case 2 => "two"; });\n',
          stdout="one")
    check("match return",
          'func f(x) { let y = match (x) { '
          'case 1 => { return 10; } default => 0; }; return y + 1; }\n'
          'f(1)\nf(2)\n',
          stdout_all=["= 10", "= 1"])
    check("match break outer loop",
          'for (let i in [1, 2, 3]) { '
          'let x = match (i) { case 2 => { break; } default => i; }; '
          'println(x); }\n',
          stdout="1\n")
    check("match keeps std.regex.match",
          'import std;\nprintln(std.regex.match("a+", "aaa"));\n',
          stdout="true")
    check("match as pattern",
          'match (1) { case 1 as x => println(x); }\n', stdout="1")

    check("expression lambda",
          'import std;\n'
          'println(std.list.map([1, 2, 3], func(x) -> x * 2));\n'
          'println(std.list.filter([1, 2, 3], func(x) -> x % 2 == 1));\n'
          'println(std.list.reduce([1, 2, 3], func(a, b) -> a + b));\n',
          stdout_all=["[2, 4, 6]", "[1, 3]", "6"])
    check("expression lambda precedence",
          'let f = func(x) -> x * 2 + 1;\nprintln(f(3));\n',
          stdout="7")
    check("nested expression lambdas",
          'let add = func(x) -> func(y) -> x + y;\nprintln(add(2)(3));\n',
          stdout="5")
    check("named expression function",
          'func double(x) -> x * 2;\nprintln(double(21));\n',
          stdout="42")
    check("expression lambda with match",
          'let describe = func(x) -> match (x) {\n'
          '  case 0 => "zero";\n'
          '  default => "other";\n'
          '};\n'
          'println(describe(0));\nprintln(describe(2));\n',
          stdout_all=["zero", "other"])
    check("method expression body",
          'struct Box { v: int; }\n'
          'impl Box { func twice(self) -> self.v * 2; }\n'
          'let b = Box { v: 4 };\nprintln(b.twice());\n',
          stdout="8")
    check("async expression function",
          'import std;\n'
          'async func inc(x) -> x + 1;\n'
          'println(await inc(1));\n',
          stdout="2")

    check("default parameter basic",
          'func f(a, b = a + 1) { return b; }\n'
          'println(f(10));\nprintln(f(10, 3));\n',
          stdout_all=["11", "3"])
    check("default parameter chain",
          'func f(a, b = a + 1, c = b + 1) { return c; }\n'
          'println(f(1));\n',
          stdout="3")
    check("default parameter multiple literals",
          'func f(a = 1, b = 2, c = 3) { return a + b + c; }\n'
          'println(f());\nprintln(f(10));\n',
          stdout_all=["6", "15"])
    check("default parameter typed",
          'func f(a: int, b: int = a + 1) { return b; }\n'
          'println(f(2));\n',
          stdout="3")
    check("default parameter nullable",
          'func f(x: int? = null) { return x; }\n'
          'println(f());\nprintln(f(5));\n',
          stdout_all=["null", "5"])
    check("default parameter method self",
          'struct S { v: int; }\n'
          'impl S { func add(self, x: int = self.v + 1) '
          '-> self.v + x; }\n'
          'println(S { v: 10 }.add());\n',
          stdout="21")
    check("default parameter anonymous",
          'let inc = func(x = 0) -> x + 1;\n'
          'println(inc());\nprintln(inc(10));\n',
          stdout_all=["1", "11"])
    check("default parameter variadic",
          'func f(a, b = a + 1, ...rest) { return [a, b, rest]; }\n'
          'println(f(1));\nprintln(f(1, 3, 4, 5));\n',
          stdout_all=["[1, 2, []]", "[1, 3, [4, 5]]"])
    check("default parameter fresh per call",
          'import std;\n'
          'func f(xs = []) { xs = std.list.append(xs, 1); return xs; }\n'
          'println(f());\nprintln(f());\n',
          stdout_all=["[1]", "[1]"])
    check("default parameter closure snapshot",
          'func f(a, cb = func() -> a) { a = 99; return cb(); }\n'
          'println(f(1));\n',
          stdout="1")
    check("default parameter forward reference",
          'func f(a = b + 1, b = 2) { return a; }\nf();\n',
          stderr="parameter b is used before it is initialized")
    check("default parameter self reference",
          'func f(a = a + 1) { return a; }\nf();\n',
          stderr="parameter a is used before it is initialized")
    check("default parameter order",
          'func f(a, b = 1, c) { return c; }\n',
          stderr="without default cannot follow")
    check("default parameter type mismatch",
          'func f(x: int = "a") { return x; }\nf();\n',
          stderr="parameter x default expects int, got string")
    check("default parameter too few",
          'func f(a, b = a + 1) { return b; }\nf();\n',
          stderr="expects at least 1 argument(s)")
    check("default parameter too many",
          'func f(a, b = 1) { return b; }\nf(1, 2, 3);\n',
          stderr="expects at most 2 argument(s)")
    check("default parameter self default rejected",
          'struct S {}\nimpl S { func m(self = S {}, x = 1) -> x; }\n',
          stderr="method self cannot have a default value")
    check("default parameter trait rejected",
          'trait T { func m(self, x: int = 1); }\n',
          stderr="trait method m cannot have default parameter values")
    check("default parameter async",
          'import std;\n'
          'async func f(a, b = a + 1) -> b;\n'
          'println(await f(1));\n',
          stdout="2")
    check("default parameter generator",
          'func gen(a, b = a + 1) { yield b; }\n'
          'for (let x in gen(3)) { println(x); }\n',
          stdout="4")
    check("default parameter spread",
          'func f(a, b = a + 1) { return b; }\n'
          'let xs = [10];\nprintln(f(...xs));\n',
          stdout="11")

    check("export expression function",
          'package p { export func twice(x) -> x * 2; }\n'
          'println(p.twice(21));\n',
          stdout="42")

    check("lambda shorthand",
          'let inc = x -> x + 1;\n'
          'let add = (a, b) -> a + b;\n'
          'let zero = () -> 7;\n'
          'println(inc(2));\nprintln(add(3, 4));\nprintln(zero());\n',
          stdout_all=["3", "7", "7"])
    check("lambda shorthand expression body",
          'import std;\n'
          'println([1, 2, 3] |> std.list.map(x -> x * 2));\n',
          stdout="[2, 4, 6]")
    check("pipe operator",
          'import std;\n'
          'println(range(0, 6) '
          '|> std.iter.filter(x -> x % 2 == 0) '
          '|> std.iter.collect());\n'
          'println([1, 2, 3] |> std.list.len);\n'
          'println(std.list.map([1, 2, 3], x -> x + 1) |> std.list.len);\n',
          stdout_all=["[0, 2, 4]", "3", "3"])
    check("pipe struct method",
          'struct Box { v: int; }\n'
          'impl Box { func add(self, x) -> self.v + x; }\n'
          'println(5 |> Box { v: 10 }.add());\n',
          stdout="15")

    check("pipe apply operator",
          'func f(x, y) { return x - y; }\n'
          'let h = x -> y -> f(x, y);\n'
          'println(4 |>> h(3));\n'
          'println(h(4)(3));\n'
          'println(1 |>> (x -> x + 1));\n',
          stdout_all=["-1", "1", "2"])
    check("call placeholders",
          'func f(a, b, c) { return [a, b, c]; }\n'
          'let g = f($, $, 3);\n'
          'println(g(1, 2));\n'
          'println(f($, 9, 0)(4));\n'
          'func add(a, b) { return a + b; }\n'
          'println(add($, 2)(1));\n',
          stdout_all=["[1, 2, 3]", "[4, 9, 0]", "3"])
    check("placeholder with pipe",
          'import std;\n'
          'println([1, 2, 3] |> std.list.map($, x -> x * 2));\n'
          'println([1, 2, 3] |>> std.list.map($, x -> x * 2));\n',
          stdout_all=["[2, 4, 6]", "[2, 4, 6]"])
    check("placeholder method",
          'struct Box { v: int; }\n'
          'impl Box { func add(self, x) -> self.v + x; }\n'
          'let b = Box { v: 10 };\n'
          'let m = b.add($);\n'
          'println(m(2));\n',
          stdout="12")
    check("placeholder nearest call",
          'func f(a, b, c) { return [a, b, c]; }\n'
          'func inc(x) { return x + 1; }\n'
          'let value = f(inc($), 9, 0);\n'
          'println(kind_of(value));\n',
          stdout="list")
