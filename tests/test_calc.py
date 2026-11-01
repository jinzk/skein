#!/usr/bin/env python3
"""Golden/smoke tests for the calc-demo interpreter."""

import os
import signal
import subprocess
import sys
import tempfile
import time


BIN = sys.argv[1] if len(sys.argv) > 1 else "./calc-demo"
FFI_SHIM = sys.argv[2] if len(sys.argv) > 2 else None


def calc_string(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def run(source, timeout=5):
    try:
        return subprocess.run(
            [BIN],
            input=source,
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise AssertionError(f"interpreter timed out for input {source!r}") from exc


def run_file(path, timeout=5):
    return subprocess.run(
        [BIN, path],
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=timeout,
    )


def check(name, source, *, stdout=None, stdout_all=None, stderr=None, code=0):
    proc = run(source)
    combined = proc.stdout + proc.stderr
    assert "Segmentation fault" not in combined, f"{name}: crashed"
    assert proc.returncode == code, (
        f"{name}: expected exit {code}, got {proc.returncode}\n"
        f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
    )
    if stdout is not None:
        assert stdout in proc.stdout, (
            f"{name}: expected stdout to contain {stdout!r}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    if stdout_all is not None:
        for expected in stdout_all:
            assert expected in proc.stdout, (
                f"{name}: expected stdout to contain {expected!r}\n"
                f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
    if stderr is not None:
        assert stderr in proc.stderr, (
            f"{name}: expected stderr to contain {stderr!r}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )


def check_interactive_continuation():
    """Verify REPL continuation prompts for multi-line empty braces."""
    if sys.platform == "win32":
        return
    try:
        import pty
        import select
    except ImportError:
        return

    master, slave = pty.openpty()
    proc = subprocess.Popen(
        [BIN],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)

    try:
        def read_available(timeout=1.0):
            data = b""
            deadline = time.time() + timeout
            while time.time() < deadline:
                ready, _, _ = select.select([master], [], [], 0.05)
                if ready:
                    try:
                        chunk = os.read(master, 4096)
                    except OSError:
                        break
                    if not chunk:
                        break
                    data += chunk
                    deadline = time.time() + 0.15
                elif data:
                    break
            return data.decode("utf-8", "replace")

        assert ">" in read_available()

        os.write(master, b"if (1) {\n")
        assert ".." in read_available(), "missing empty-block continuation prompt"
        os.write(master, b"}\n")
        read_available()

        os.write(master, b"let d = {\n")
        assert ".." in read_available(), "missing empty-dict continuation prompt"
        os.write(master, b"// comment\n")
        assert ".." in read_available(), "missing comment continuation prompt"
        os.write(master, b"};\n")
        read_available()
        os.write(master, b"d\n")
        assert "= {}" in read_available(), "multi-line empty dict failed"

        os.write(master, b"1 + 2\n")
        assert "= 3" in read_available(), "REPL result failed"
        os.write(master, b"ans + 1\n")
        assert "= 4" in read_available(), "REPL ans failed"

        os.write(master, b":quit\n")
        proc.wait(timeout=2)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=2)
        os.close(master)


def main():
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
    check("increment float", "let x = 1.5;\nx++\nx\n++x\nx\n",
          stdout_all=["= 1.5", "= 2.5", "= 3.5", "= 3.5"])
    check("increment list element",
          "let a = [1, 2];\na[0]++;\n++a[1];\na\n",
          stdout_all=["= 1", "= 3", "= [2, 3]"])
    check("increment nested index",
          "let a = [[1]];\na[0][0]++;\na\n",
          stdout_all=["= 1", "= [[2]]"])
    check("increment dict field",
          'let d = {"x": 5};\nd.x++;\n++d["x"];\nd\n',
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
          "let p = {\"x\": 1};\np.x += 2;\np\n"
          "struct P { x: int; }\nlet q = P { x: 1 };\nq.x += 4;\nq\n",
          stdout_all=["= 15", "= 1", "= [15]", "= 3", "= {x: 3}",
                      "= 5", "= P { x: 5 }"])
    check("compound typed variable type error",
          "let x: int = 1;\nx += \"bad\";\n",
          stderr="cannot add int and string")
    check("compound const error",
          "const x = 1;\nx += 1;\n",
          stderr="cannot assign to constant x")
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
    check("bool print", "print(true)\nprint(false)\n",
          stdout_all=["true", "false"])
    check("bool truthiness",
          'if (true) { print("yes"); } else { print("no"); }\n',
          stdout="yes")
    check("bool false truthiness",
          'if (false) { print("yes"); } else { print("no"); }\n',
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
          'switch (2) { case 1 => print("one"); '
          'case 2, 3 => print("two or three"); '
          'default => print("other"); }\n',
          stdout="two or three")
    check("switch default",
          'switch (9) { case 1 => print("one"); '
          'default => print("other"); }\n',
          stdout="other")
    check("switch no match no default",
          'switch (9) { case 1 => print("one"); }\n', code=0)
    check("switch empty body", "switch (1) {}\n", code=0)
    check("switch empty body spaced", "switch (1) { }\n", code=0)
    check("switch single statement",
          "let x = 0;\nswitch (1) { case 1 => x = 10; }\nx\n",
          stdout="= 10")
    check("switch block",
          "switch (1) { case 1 => { print(1); print(2); } }\n",
          stdout_all=["1", "2"])
    check("switch case scope",
          "switch (1) { case 1 => let y = 2; }\ny\n",
          stderr="symbol not found y")
    check("switch string",
          'switch ("a") { case "b" => print("bad"); '
          'case "a" => print("ok"); }\n', stdout="ok")
    check("switch bool",
          'switch (true) { case false => print("bad"); '
          'case true => print("ok"); }\n', stdout="ok")
    check("switch null",
          'switch (null) { case null => print("null"); }\n',
          stdout="null")
    check("switch bytes",
          'switch (bytes_from_hex("00ff")) { '
          'case bytes_from_hex("00ff") => print("bytes"); }\n',
          stdout="bytes")
    check("switch no fallthrough",
          'switch (1) { case 1 => print("one"); '
          'case 2 => print("two"); }\n', stdout="one")
    check("switch logical selector",
          'switch (1 < 2) { case true => print("ok"); }\n', stdout="ok")
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
          'switch (1) { case 1 => switch (1) { case 1 => print("nested"); } }\n',
          stdout="nested")
    check("switch default only",
          'switch (1) { default => print("only"); }\n', stdout="only")
    check("switch empty case block",
          "switch (1) { case 1 => {} }\n", code=0)
    check("switch empty statement case",
          "switch (1) { case 1 => ; }\n", code=0)
    check("switch float selector",
          'switch (1.0) { case 1 => print("bad"); }\n',
          stderr="switch does not support float")
    check("switch float case",
          'switch (1) { case 1.0 => print("bad"); }\n',
          stderr="switch does not support float case value")
    check("switch list case",
          'switch (1) { case [1] => print("bad"); }\n',
          stderr="switch does not support list case value")
    check("switch builtin case",
          'switch (1) { case sqrt => print("bad"); }\n',
          stderr="switch does not support builtin case value")
    check("switch type name is not pattern",
          'switch (1) { case int => print("bad"); }\n',
          stderr="switch does not support builtin case value")
    check("switch case condition rejected",
          'switch (true) { case 1 < 2 => print("bad"); }\n',
          stderr="logical or comparison expressions are not allowed in case values")
    check("switch list selector",
          'switch ([1]) { default => print("bad"); }\n',
          stderr="switch does not support list")

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
    check("dict field read", 'let p = {"x": 1, "y": 2};\np.x + p.y\n',
          stdout="= 3")
    check("dict field assignment", 'let p = {"x": 1};\np.x = 5;\np\n',
          stdout="{x: 5}")
    check(
        "nested dict field assignment",
        'let p = {"a": {"b": 2}};\np.a.b = 3;\np\n',
        stdout="{a: {b: 3}}",
    )
    check("dict field callable", 'let d = {"f": sqrt};\nd.f(9)\n',
          stdout="= 3.0")
    check("dict field missing", 'let p = {"x": 1};\np.y\n',
          stderr="key not found")
    check("float literals unaffected by dot", ".5 + 1\n", stdout="= 1.5")
    check("dict equality", "{1:2} == {1:2}\n", stdout="= true")
    check("dict literal duplicate key last wins", '{1: "a", 1: "b"}\n',
          stdout="{1: b}")
    check("dict literal numeric duplicate key",
          '{1: "a", 1.0: "b"} == {1: "b"}\n', stdout="= true")
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
    check("empty dict as first dict key", "let d = {{}: 1};\nd\n",
          stdout="= {{}: 1}")
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
          stderr="len expects string, bytes, list, or dict")
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
    check(
        "generator return value",
        "import std;\n"
        "func value() {\n"
        "  yield 1;\n"
        "  return 9;\n"
        "}\n"
        "let g = value();\n"
        "std.generator.next(g)\n"
        "std.generator.next(g)\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "= {done: true, value: 9}",
        ],
    )
    check(
        "yield outside generator",
        "yield 1;\n",
        stderr="yield outside generator",
    )
    check(
        "generator error",
        "import std;\n"
        "func broken() {\n"
        "  yield 1;\n"
        "  let x = 1 / 0;\n"
        "}\n"
        "let g = broken();\n"
        "std.generator.next(g)\n"
        "std.generator.next(g);\n",
        stderr="generator failed",
    )
    check(
        "generator closure value",
        "import std;\n"
        "func make() {\n"
        "  let x = 5;\n"
        "  yield func() { return x; };\n"
        "}\n"
        "let g = make();\n"
        "let r = std.generator.next(g);\n"
        "let f = r[\"value\"];\n"
        "f()\n",
        stdout="= 5",
    )
    check(
        "generator unstarted cleanup",
        "import std;\n"
        "func pending() { yield 1; }\n"
        "let g = pending();\n",
    )
    check(
        "std net resolve",
        'import std;\nlen(std.net.resolve("localhost")) > 0\n',
        stdout="= true",
    )
    check(
        "std net resolve ipv4",
        'import std;\nlen(std.net.resolve("localhost", 1000, "ipv4")) > 0\n',
        stdout="= true",
    )
    check(
        "std net resolve async",
        "import std;\n"
        'let t = std.net.resolve_async("localhost", 1000, "ipv4");\n'
        "len(std.task.wait(t)) > 0\n",
        stdout="= true",
    )
    check(
        "std net concurrent resolve async",
        "import std;\n"
        'let a = std.net.resolve_async("localhost", 1000, "ipv4");\n'
        'let b = std.net.resolve_async("localhost", 1000, "ipv4");\n'
        "let values = std.task.wait_all([a, b]);\n"
        "len(values) == 2 && len(values[0]) > 0 && len(values[1]) > 0\n",
        stdout="= true",
    )
    check(
        "std net async resolve cancel",
        "import std;\n"
        'let t = std.net.resolve_async("localhost", 5000);\n'
        "std.task.cancel(t)\n"
        "std.task.wait(t);\n",
        stdout="= true",
        stderr="task was cancelled",
    )
    try:
        import socket
        import threading

        server = socket.socket()
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        server.settimeout(5)
        port = server.getsockname()[1]
        received = []

        def echo_server():
            try:
                client, _ = server.accept()
                data = client.recv(100)
                received.append(data)
                client.sendall(b"echo:" + data)
                client.close()
            finally:
                server.close()

        thread = threading.Thread(target=echo_server)
        thread.start()
        net_proc = run(
            "import std;\n"
            f'let s = std.net.tcp_connect("127.0.0.1", {port}, 1000, "ipv4");\n'
            'std.net.send(s, "hello", 1000);\n'
            "let data = std.net.recv(s, 100, 1000);\n"
            "print(bytes_to_string(data));\n"
            "std.net.close(s);\n"
        )
        thread.join(timeout=5)
        assert net_proc.returncode == 0, net_proc.stderr
        assert "echo:hello" in net_proc.stdout, net_proc.stdout
        assert received == [b"hello"], received

        async_server = socket.socket()
        async_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        async_server.bind(("127.0.0.1", 0))
        async_server.listen(1)
        async_server.settimeout(5)
        async_port = async_server.getsockname()[1]
        async_received = []

        def async_echo_server():
            try:
                client, _ = async_server.accept()
                data = client.recv(100)
                async_received.append(data)
                client.sendall(b"async:" + data)
                client.close()
            finally:
                async_server.close()

        async_thread = threading.Thread(target=async_echo_server)
        async_thread.start()
        async_proc = run(
            "import std;\n"
            f'let t = std.net.tcp_connect_async('
            f'"127.0.0.1", {async_port}, 1000, "ipv4");\n'
            "let s = std.task.wait(t);\n"
            'let st = std.net.send_async(s, "hello", 1000);\n'
            "std.task.wait(st)\n"
            "let rt = std.net.recv_async(s, 100, 1000);\n"
            "let data = std.task.wait(rt);\n"
            "print(bytes_to_string(data));\n"
            "std.net.close(s);\n"
        )
        async_thread.join(timeout=5)
        assert async_proc.returncode == 0, async_proc.stderr
        assert "async:hello" in async_proc.stdout, async_proc.stdout
        assert async_received == [b"hello"], async_received

        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        listen_port = probe.getsockname()[1]
        probe.close()
        server_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        client = None
        try:
            server_proc.stdin.write(
                "import std;\n"
                f'let listener = std.net.tcp_listen("127.0.0.1", {listen_port});\n'
                "let client = std.net.accept(listener);\n"
                "let data = std.net.recv(client, 100);\n"
                'std.net.send(client, bytes_concat('
                'bytes_from_string("echo:"), data));\n'
                "std.net.close(client);\n"
                "std.net.close(listener);\n"
            )
            server_proc.stdin.close()
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    client = socket.create_connection(
                        ("127.0.0.1", listen_port), timeout=0.2
                    )
                    break
                except OSError:
                    if server_proc.poll() is not None:
                        break
                    time.sleep(0.02)
            assert client is not None, "calc tcp_listen did not accept"
            client.sendall(b"hello")
            assert client.recv(100) == b"echo:hello"
            client.close()
            client = None
            server_proc.wait(timeout=5)
            err = server_proc.stderr.read()
            assert server_proc.returncode == 0, err
        finally:
            if client is not None:
                client.close()
            if server_proc.poll() is None:
                server_proc.kill()
                server_proc.wait()

        async_probe = socket.socket()
        async_probe.bind(("127.0.0.1", 0))
        async_listen_port = async_probe.getsockname()[1]
        async_probe.close()
        async_server_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        client = None
        try:
            async_server_proc.stdin.write(
                "import std;\n"
                f'let lt = std.net.tcp_listen_async('
                f'"127.0.0.1", {async_listen_port}, 1000, "ipv4");\n'
                "let listener = std.task.wait(lt);\n"
                "let at = std.net.accept_async(listener, 1000);\n"
                "let client = std.task.wait(at);\n"
                "let rt = std.net.recv_async(client, 100, 1000);\n"
                "let data = std.task.wait(rt);\n"
                "let st = std.net.send_async(client, bytes_concat("
                'bytes_from_string("async:"), data), 1000);\n'
                "std.task.wait(st)\n"
                "let ht = std.net.shutdown_async(client, 1000);\n"
                "std.task.wait(ht)\n"
                "std.net.close(client);\n"
                "std.net.close(listener);\n"
            )
            async_server_proc.stdin.close()
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    client = socket.create_connection(
                        ("127.0.0.1", async_listen_port), timeout=0.2
                    )
                    break
                except OSError:
                    if async_server_proc.poll() is not None:
                        break
                    time.sleep(0.02)
            assert client is not None, "calc async listener did not accept"
            client.sendall(b"hello")
            assert client.recv(100) == b"async:hello"
            assert client.recv(100) == b""
            client.close()
            client = None
            async_server_proc.wait(timeout=5)
            err = async_server_proc.stderr.read()
            assert async_server_proc.returncode == 0, err
        finally:
            if client is not None:
                client.close()
            if async_server_proc.poll() is None:
                async_server_proc.kill()
                async_server_proc.wait()

        check(
            "std net accept timeout",
            "import std;\n"
            'let s = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
            "std.net.local_addr(s)[\"family\"]\n"
            "std.net.accept(s, 100);\n"
            "std.net.close(s);\n",
            stdout="= ipv4",
            stderr="timed out",
        )
        check(
            "std net async accept timeout",
            "import std;\n"
            'let listener = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
            "let a = std.net.accept_async(listener, 50);\n"
            "std.task.wait(a);\n"
            "std.net.close(listener);\n",
            stderr="accept_async: timed out",
        )
        check(
            "std net async cancel",
            "import std;\n"
            'let listener = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
            "let a = std.net.accept_async(listener, 1000);\n"
            "std.task.cancel(a)\n"
            "std.task.wait(a);\n"
            "std.net.close(listener);\n",
            stdout="= true",
            stderr="task was cancelled",
        )
        check(
            "std net async close pending",
            "import std;\n"
            'let listener = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
            "let a = std.net.accept_async(listener, 1000);\n"
            "std.net.close(listener);\n"
            "std.task.wait(a);\n",
            stderr="accept_async: listener closed",
        )
        check(
            "std net async pending shutdown",
            "import std;\n"
            'let listener = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
            "let pending = std.net.accept_async(listener, 10000);\n",
        )
        check(
            "std net ipv4 family",
            "import std;\n"
            'let s = std.net.tcp_listen("localhost", 0, 100, "ipv4");\n'
            'std.net.local_addr(s)["family"]\n'
            "std.net.close(s);\n",
            stdout="= ipv4",
        )
        check(
            "std net listener misuse",
            "import std;\n"
            'let s = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
            'std.net.send(s, "x");\n'
            "std.net.close(s);\n",
            stderr="send expects a connected tcp socket",
        )
        check(
            "std net double close",
            "import std;\n"
            'let s = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
            "std.net.close(s);\n"
            "std.net.close(s);\n",
            stderr="tcp socket is closed",
        )
        check(
            "std net connect failure",
            "import std;\n"
            'std.net.tcp_connect("127.0.0.1", 1, 100);\n',
            stderr="tcp_connect:",
        )

        silent = socket.socket()
        silent.bind(("127.0.0.1", 0))
        silent.listen(1)
        silent_port = silent.getsockname()[1]

        def silent_server():
            try:
                client, _ = silent.accept()
                time.sleep(0.5)
                client.close()
            finally:
                silent.close()

        silent_thread = threading.Thread(target=silent_server)
        silent_thread.start()
        check(
            "std net recv timeout",
            "import std;\n"
            f'let s = std.net.tcp_connect("127.0.0.1", {silent_port}, 100);\n'
            "std.net.recv(s, 10, 100);\n"
            "std.net.close(s);\n",
            stderr="timed out",
        )
        silent_thread.join(timeout=5)

        close_server = socket.socket()
        close_server.bind(("127.0.0.1", 0))
        close_server.listen(1)
        close_port = close_server.getsockname()[1]

        def close_server_thread():
            try:
                client, _ = close_server.accept()
                while client.recv(100):
                    pass
                client.close()
            finally:
                close_server.close()

        close_thread = threading.Thread(target=close_server_thread)
        close_thread.start()
        check(
            "std net address and shutdown",
            "import std;\n"
            f'let s = std.net.tcp_connect("127.0.0.1", {close_port}, 100);\n'
            'std.net.local_addr(s)["family"]\n'
            'std.net.peer_addr(s)["family"]\n'
            "std.net.shutdown(s, 100);\n"
            "std.net.close(s);\n",
            stdout_all=["= ipv4", "= ipv4"],
        )
        close_thread.join(timeout=5)
    except OSError:
        pass

    ffi_probe = run("import std;\nkind_of(std.ffi)\n")
    has_ffi = "= dict" in ffi_probe.stdout
    if sys.platform.startswith("linux") and has_ffi:
        import ctypes.util
        libm = ctypes.util.find_library("m")
        libc = ctypes.util.find_library("c")
        if libm and libc:
            check(
                "std ffi basic calls",
                f'import std;\n'
                f'let m = std.ffi.open("{libm}");\n'
                f'std.ffi.call(m, "cos", "f64", ["f64"], [0.0])\n'
                f'let c = std.ffi.open("{libc}");\n'
                f'std.ffi.call(c, "strlen", "i64", ["cstring"], ["hello"])\n'
                f'let x = bytes_from_string("hello");\n'
                f'let y = bytes_from_string("hello");\n'
                f'std.ffi.call(c, "memcmp", "i32", '
                f'["bytes", "bytes", "usize"], [x, y, 5])\n'
                f'let buf = std.ffi.buffer(8);\n'
                f'std.ffi.call(c, "memcpy", "void", '
                f'["buffer", "bytes", "usize"], [buf, x, 5]);\n'
                f'bytes_to_string(std.ffi.buffer_bytes(buf, 5))\n'
                f'std.ffi.buffer_len(buf)\n'
                f'std.ffi.buffer_free(buf);\n'
                f'std.ffi.close(m);\nstd.ffi.close(c);\n',
                stdout_all=["= 1.0", "= 5", "= 0", "= hello", "= 8"],
            )

    if has_ffi and FFI_SHIM:
        shim = calc_string(FFI_SHIM)
        check(
            "std ffi scalar types",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call(lib, "shim_i8", "i8", ["i8"], [-5])\n'
            'std.ffi.call(lib, "shim_u8", "u8", ["u8"], [250])\n'
            'std.ffi.call(lib, "shim_i16", "i16", ["i16"], [-30000])\n'
            'std.ffi.call(lib, "shim_u16", "u16", ["u16"], [60000])\n'
            'std.ffi.call(lib, "shim_i32", "i32", ["i32"], [-100000])\n'
            'std.ffi.call(lib, "shim_u32", "u32", ["u32"], [4000000000])\n'
            'std.ffi.call(lib, "shim_i64", "i64", ["i64"], [-9000000000])\n'
            'std.ffi.call(lib, "shim_u64", "u64", ["u64"], [9000000000])\n'
            'std.ffi.call(lib, "shim_f32", "f32", ["f32"], [1.5])\n'
            'std.ffi.call(lib, "shim_f64", "f64", ["f64"], [2.5])\n'
            'std.ffi.call(lib, "shim_cstring", "cstring", '
            '["cstring"], ["hello"])\n'
            'std.ffi.call(lib, "shim_usize", "usize", ["usize"], [123])\n'
            'std.ffi.call(lib, "shim_isize", "isize", ["isize"], [-123])\n'
            "std.ffi.close(lib);\n",
            stdout_all=[
                "= -5", "= 250", "= -30000", "= 60000",
                "= -100000", "= 4000000000", "= -9000000000",
                "= 9000000000", "= 1.5", "= 2.5", "= hello",
                "= 123", "= -123",
            ],
        )
        check(
            "std ffi u64 overflow",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call(lib, "shim_u64_max", "u64", [], [])\n'
            "std.ffi.close(lib);\n",
            stderr="u64 return value exceeds int64 range",
        )
        if sys.maxsize > 2**31 - 1:
            check(
                "std ffi usize overflow",
                "import std;\n"
                f"let lib = std.ffi.open({shim});\n"
                'std.ffi.call(lib, "shim_usize_max", "usize", [], [])\n'
                "std.ffi.close(lib);\n",
                stderr="usize return value exceeds int64 range",
            )
        check(
            "std ffi library error",
            'import std;\nstd.ffi.open("/definitely/not/a/library.so")\n',
            stderr="cannot load",
        )
        check(
            "std ffi symbol error",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call(lib, "shim_missing_symbol", "void", [], [])\n'
            "std.ffi.close(lib);\n",
            stderr="symbol not found",
        )
        check(
            "std ffi direct bytes return",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            'std.ffi.call(lib, "shim_cstring", "bytes", '
            '["cstring"], ["hello"])\n'
            "std.ffi.close(lib);\n",
            stderr="cannot be returned directly",
        )
        check(
            "std ffi buffer output",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            "let buf = std.ffi.buffer(8);\n"
            'std.ffi.call(lib, "shim_buffer_write", "i32", '
            '["buffer", "usize"], [buf, 8])\n'
            "bytes_to_string(std.ffi.buffer_bytes(buf, 5))\n"
            "std.ffi.buffer_len(buf)\n"
            "std.ffi.buffer_free(buf);\n"
            "std.ffi.close(lib);\n",
            stdout_all=["= 5", "= hello", "= 8"],
        )
        check(
            "std ffi buffer bounds",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            "let buf = std.ffi.buffer(4);\n"
            "std.ffi.buffer_bytes(buf, 5)\n"
            "std.ffi.buffer_free(buf);\n"
            "std.ffi.buffer_len(buf)\n"
            "std.ffi.close(lib);\n",
            stderr="length out of range",
        )
        check(
            "std ffi freed buffer",
            "import std;\n"
            f"let lib = std.ffi.open({shim});\n"
            "let buf = std.ffi.buffer(4);\n"
            "std.ffi.buffer_free(buf);\n"
            "std.ffi.buffer_len(buf)\n"
            "std.ffi.close(lib);\n",
            stderr="ffi buffer is freed",
        )
    check(
        "std string format",
        'import std;\nstd.string.format("{}", 3)\n',
        stdout="= 3",
    )
    check(
        "std list helpers",
        "import std;\n"
        "std.list.len([1, 2, 3])\n"
        "std.list.slice([1, 2, 3, 4], 1, 2)\n"
        "std.list.contains([1, 2, 3], 2)\n"
        "std.list.index([1, 2, 3], 3)\n",
        stdout_all=["= 3", "= [2, 3]", "= 1", "= 2"],
    )
    check(
        "std list slice clamps",
        "import std;\nstd.list.slice([1, 2], 1, 10)\n",
        stdout="= [2]",
    )
    check(
        "std dict helpers",
        "import std;\n"
        'std.dict.get({"a": 1}, "a")\n'
        'std.dict.get({"a": 1}, "b")\n'
        'std.dict.get({"a": 1}, "b", 9)\n'
        'std.dict.has({"a": 1}, "a")\n'
        'std.dict.keys({"a": 1, "b": 2})\n'
        'std.dict.values({"a": 1, "b": 2})\n',
        stdout_all=["= 1", "= 9", "= 1", "= [a, b]", "= [1, 2]"],
    )
    check(
        "std string helpers",
        "import std;\n"
        'std.string.join(["a", "b", "c"], ",")\n'
        'std.string.replace("a-b-c", "-", "_")\n'
        'std.string.starts_with("hello", "he")\n'
        'std.string.ends_with("hello", "lo")\n'
        'std.string.find("hello", "ll")\n'
        'std.string.trim("  x  ")\n',
        stdout_all=["= a,b,c", "= a_b_c", "= 1", "= 1", "= 2", "= x"],
    )
    check(
        "std bytes aliases",
        "import std;\n"
        'std.bytes.hex(bytes_from_hex("00ff"))\n'
        'std.bytes.len(bytes_from_hex("00ff"))\n',
        stdout_all=["= 00ff", "= 2"],
    )
    check("std join type", 'import std;\nstd.string.join([1], ",")\n',
          stderr="expects string elements")
    check("std replace empty", 'import std;\nstd.string.replace("a", "", "b")\n',
          stderr="search must not be empty")
    check("std list slice type", "import std;\nstd.list.slice(1, 0)\n",
          stderr="std.list.slice expects a list")
    check("std list slice start", "import std;\nstd.list.slice([1], -1)\n",
          stderr="start must be a non-negative integer")
    check("std dict get type", 'import std;\nstd.dict.get([], "a")\n',
          stderr="std.dict.get expects a dict")
    check("std import missing module", "std.list.len([1])\n",
          stderr="symbol not found std")
    check(
        "std namespace copy is isolated",
        "import std;\n"
        "func mutate(ns) { ns.list = 1; }\n"
        "mutate(std);\n"
        "std.list.len([1, 2])\n",
        stdout="= 2",
    )
    check(
        "std list transformations",
        "import std;\n"
        "std.list.reverse([1, 2, 3])\n"
        "std.list.sort([3, 1, 2])\n"
        "std.list.sort([2.5, 1, 3])\n"
        'std.list.sort(["b", "a", "c"])\n'
        "std.list.append([1, 2], 3)\n",
        stdout_all=[
            "= [3, 2, 1]",
            "= [1, 2, 3]",
            "= [1, 2.5, 3]",
            "= [a, b, c]",
            "= [1, 2, 3]",
        ],
    )
    check(
        "std list functional helpers",
        "import std;\n"
        "std.list.map([1, 2, 3], func(x) { return x * 2; })\n"
        "std.list.filter([1, 2, 3, 4], func(x) { return x % 2 == 0; })\n"
        "std.list.reduce([1, 2, 3, 4], func(a, b) { return a + b; })\n"
        "std.list.reduce([1, 2, 3, 4], func(a, b) { return a + b; }, 10)\n",
        stdout_all=["= [2, 4, 6]", "= [2, 4]", "= 10", "= 20"],
    )
    check(
        "std dict items/delete/merge",
        "import std;\n"
        'std.dict.items({"a": 1, "b": 2})\n'
        'std.dict.delete({"a": 1, "b": 2}, "a")\n'
        'std.dict.delete({"a": 1}, "missing")\n'
        'std.dict.merge({"a": 1, "b": 2}, {"b": 9, "c": 3})\n',
        stdout_all=["= [[a, 1], [b, 2]]", "= {b: 2}", "= {a: 1}",
                    "= {a: 1, b: 9, c: 3}"],
    )
    check(
        "std string extras",
        "import std;\n"
        'std.string.contains("hello", "ell")\n'
        'std.string.contains("hello", "x")\n'
        'std.string.repeat("ab", 3)\n'
        'std.string.repeat("ab", 0) == ""\n'
        'std.string.trim_start("  x  ")\n'
        'std.string.trim_end("  x  ")\n',
        stdout_all=["= 1", "= 0", "= ababab", "= true", "= x  ", "=   x"],
    )
    check(
        "std list sort type error",
        'import std;\nstd.list.sort([1, "a"])\n',
        stderr="expects all numbers or all strings",
    )
    check(
        "std reduce empty error",
        "import std;\nstd.list.reduce([], func(a, b) { return a + b; })\n",
        stderr="empty list without initial value",
    )
    check(
        "std map callback error",
        "import std;\n"
        "std.list.map([1], func(x) { return 1/0; })\n",
        stderr="division by zero",
    )
    check(
        "std repeat negative",
        'import std;\nstd.string.repeat("a", -1)\n',
        stderr="repeat count must be non-negative",
    )
    check(
        "std dict merge type",
        "import std;\nstd.dict.merge({\"a\": 1}, 1)\n",
        stderr="std.dict.merge expects two dicts",
    )

    check(
        "break",
        "let i = 0;\n"
        "while (i < 10) { i = i + 1; if (i == 3) { break; } print(i); }\n",
        stdout="1\n2\n",
    )
    check(
        "continue",
        "let i = 0;\n"
        "while (i < 5) { i = i + 1; if (i == 2) { continue; } print(i); }\n",
        stdout="1\n3\n",
    )
    check(
        "labeled break outer loop",
        "label outer: for (let i in [1, 2, 3]) {\n"
        "  for (let j in [1, 2, 3]) {\n"
        "    if (i == 2 && j == 2) { break outer; }\n"
        "    print(str(i) + \",\" + str(j));\n"
        "  }\n"
        "}\n"
        'print("done");\n',
        stdout_all=["1,1", "1,2", "1,3", "2,1", "done"],
    )
    check(
        "labeled continue outer loop",
        "label outer: for (let i in [1, 2]) {\n"
        "  for (let j in [1, 2]) {\n"
        "    if (j == 1) { continue outer; }\n"
        "    print(\"bad\");\n"
        "  }\n"
        "}\n"
        'print("done");\n',
        stdout="done\n",
    )
    check(
        "labeled break switch",
        "label sw: switch (1) {\n"
        "  case 1 => { break sw; }\n"
        '  default => print("bad");\n'
        "}\n"
        'print("after");\n',
        stdout="after\n",
    )
    check(
        "label on non-loop rejected",
        "label bad: if (1) { print(1); }\n",
        stderr="label must be attached to a loop or switch",
    )
    check(
        "block scope",
        "let x = 1;\n{ let x = 2; }\nx\n",
        stdout="= 1",
    )
    check(
        "block assignment",
        "let x = 1;\n{ x = 2; }\nx\n",
        stdout="= 2",
    )

    check("fail caught with recover",
          'func f() { fail "boom"; }\n'
          "f() on error(e) { print(e); recover 0; }\n",
          stdout_all=["boom", "= 0"])
    check("on error handler not called on success",
          "func f() { return 1; }\n"
          'f() on error(e) { print("bad"); recover 0; }\n',
          stdout="= 1")
    check("pass propagates to outer handler",
          'func inner() { fail "x"; }\n'
          "func outer() { return inner() on error(e) { pass e; }; }\n"
          'outer() on error(e) { print("caught " + e); recover 7; }\n',
          stdout_all=["caught x", "= 7"])
    check("fail stops loop body",
          "func f() {\n"
          "  for (let i in [1, 2, 3]) {\n"
          '    if (i == 2) { fail "loop"; }\n'
          "    print(i);\n"
          "  }\n"
          "  return 0;\n"
          "}\n"
          'f() on error(e) { print("E " + e); recover 9; }\n',
          stdout_all=["1", "E loop", "= 9"])
    check("recover value is expression result",
          'func f() { fail "x"; }\n'
          "(f() on error(e) { recover 2; }) + 3\n",
          stdout="= 5")
    check("handler accesses outer scope",
          'let n = 3;\nfunc f() { fail "x"; }\n'
          "f() on error(e) { recover n + 1; }\n",
          stdout="= 4")
    check("recover outside handler",
          "recover 1;\n", stderr="recover outside error handler")
    check("pass outside handler",
          "pass 1;\n", stderr="pass outside error handler")
    check("handler without terminal statement",
          'func f() { fail "x"; }\n'
          "let x = f() on error(e) { print(e); };\n",
          stderr="error handler must end with recover, pass, or fail")
    check("handler return rejected",
          'func f() { fail "x"; }\n'
          "func g() { let x = f() on error(e) { return 1; }; return 2; }\n"
          "g()\n",
          stderr="error handler cannot return, break, or continue")
    check("method fail caught",
          "struct P {\n"
          "  x: int;\n"
          '  func check(self) { if (self.x < 0) { fail "negative"; } '
          "return self.x; }\n"
          "}\n"
          'P { x: -1 }.check() on error(e) { print(e); recover 0; }\n',
          stdout_all=["negative", "= 0"])
    check("recover does not cross function boundary",
          'func bad() { recover 1; }\n'
          'func f() { fail "x"; }\n'
          "f() on error(e) { bad(); recover 0; };\n",
          stderr="recover outside error handler")
    check("uncaught fail at top level",
          'fail "boom";\n', stderr="uncaught fail: boom")

    check("variadic function",
          "func sum(...nums: number) {\n"
          "  let total = 0;\n"
          "  for (let n in nums) { total = total + n; }\n"
          "  return total;\n"
          "}\n"
          "sum()\nsum(1, 2, 3)\n",
          stdout_all=["= 0", "= 6"])
    check("variadic with fixed parameters",
          "func f(a: int, ...rest: string) {\n"
          "  return a + len(rest);\n"
          "}\n"
          "f(1)\nf(1, \"a\", \"b\")\n",
          stdout_all=["= 1", "= 3"])
    check("variadic rest is list",
          "func f(...xs) {\n"
          "  if (len(xs) == 0) { return null; }\n"
          "  return xs[1];\n"
          "}\n"
          "f()\nf(10, 20)\n",
          stdout_all=["= 20"])
    check("variadic arity",
          "func f(a: int, ...rest) { return a; }\nf()\n",
          stderr="function f expects at least 1 argument(s), got 0")
    check("variadic element type",
          'func f(...rest: number) { return len(rest); }\nf(1, "x")\n',
          stderr="function f rest argument 2 expects number, got string")
    check("variadic must be last",
          "func f(...xs, y) { return y; }\n",
          stderr="variadic parameter must be last")
    check("variadic method",
          "func sum(...nums: number) {\n"
          "  let total = 0;\n"
          "  for (let n in nums) { total = total + n; }\n"
          "  return total;\n"
          "}\n"
          "struct P { x: int; }\n"
          "impl P {\n"
          "  func add(self, ...nums: int) { return sum(...nums); }\n"
          "}\n"
          "P { x: 1 }.add(1, 2, 3)\n",
          stdout="= 6")
    check("variadic anonymous function",
          "func make() { return func(...args: int) { return len(args); }; }\n"
          "make()(1, 2, 3)\n",
          stdout="= 3")
    check("call spread",
          "func sum(...nums: number) {\n"
          "  let total = 0;\n"
          "  for (let n in nums) { total = total + n; }\n"
          "  return total;\n"
          "}\n"
          "let xs = [1, 2, 3];\n"
          "sum(...xs)\nsum(0, ...xs)\n",
          stdout_all=["= 6", "= 6"])
    check("call spread requires list",
          "func f(...xs) { return len(xs); }\nf(...1)\n",
          stderr="spread expects a list, got int")
    check("trait variadic rejected",
          "trait T { func f(self, ...args); }\n",
          stderr="trait methods cannot be variadic")

    many_globals = "".join(f"let v{i} = {i};\n" for i in range(40))
    check("large symbol table lookup", many_globals + "v0 + v39\n",
          stdout="= 39")
    check("large symbol table duplicate", many_globals + "let v0 = 1;\n",
          stderr="symbol repeated")
    check(
        "large symbol table closure",
        many_globals
        + "func make() { let n = 5; func get() { return n + v39; } return get; }\n"
        + "let g = make();\ng()\n",
        stdout="= 44",
    )
    check(
        "large symbol table shadowing",
        many_globals + "let x = 1;\nfunc f() { let x = 2; return x; }\nf()\nx\n",
        stdout_all=["= 2", "= 1"],
    )

    check("call non-function", "let x = 1;\nx(2)\n", stderr="cannot call int")
    check("recursion depth limit",
          "func f(n) { return f(n + 1); }\nf(0)\n",
          stderr="maximum recursion depth")
    check("function without return yields null",
          "func noret() { }\nprint(noret())\n", stdout="null")
    check("null arithmetic", "func noret() { }\nnoret() + 1\n",
          stderr="cannot add null and int")
    check("null truthiness",
          "func noret() { }\nif (noret()) { print(1); } else { print(2); }\n",
          stdout="2")
    check("null as argument",
          "func noret() { }\nfunc add(a,b) { return a+b; }\nadd(noret(), 2)\n",
          stderr="cannot add null and int")
    check("null equality", "func noret() { }\nnoret() == noret()\n",
          stdout="= true")
    check("null literal", "print(null)\n", stdout="null")
    check("null literal equality", "null == null\n", stdout="= true")
    check("null literal inequality", "null == 0\n", stdout="= false")
    check("null literal falsy",
          "if (null) { print(1); } else { print(2); }\n", stdout="2")
    check("null literal arithmetic", "null + 1\n",
          stderr="cannot add null and int")
    check("null is not callable", "null()\n", stderr="cannot call null")
    check("str null", "str(null)\n", stdout="= null")
    check("format null", 'format("{}", null)\n', stdout="= null")

    check("typed parameter ok",
          "func f(x: int) { return x + 1; }\nf(2)\n", stdout="= 3")
    check("typed parameter error",
          'func f(x: int) { return x; }\nf("x")\n',
          stderr="argument 1 (x) expects int, got string")
    check("untyped parameter",
          'func f(x) { return x; }\nf("x")\n', stdout="= x")
    check("multiple typed parameters",
          "func g(a, b: number, c: string) { return b; }\n"
          'g(1, 2.5, "x")\n',
          stdout="= 2.5")
    check("typed number rejects string",
          'func g(b: number) { return b; }\ng("x")\n',
          stderr="expects number, got string")
    check("typed bool",
          "func h(x: bool) { return x; }\nh(true)\nh(false)\n",
          stdout_all=["= true", "= false"])
    check("typed bool rejects int",
          "func h(x: bool) { return x; }\nh(1)\n",
          stderr="expects bool, got int")
    check("typed bool accepts comparison",
          "func h(x: bool) { return x; }\nh(1 == 1)\n",
          stdout="= true")
    check("typed list",
          "func f(x: list) { return len(x); }\nf([1,2])\n",
          stdout="= 2")
    check("typed dict",
          "func f(x: dict) { return len(x); }\nf({1:2})\n",
          stdout="= 1")
    check("typed string",
          'func f(x: string) { return x; }\nf("a")\n', stdout="= a")
    check("typed bytes",
          'func f(x: bytes) { return bytes_len(x); }\n'
          'f(bytes_from_hex("00ff"))\n',
          stdout="= 2")
    check("typed callable",
          "func apply(fn: callable, x: int) { return fn(x); }\n"
          "apply(sqrt, 9)\n",
          stdout="= 3.0")
    check("typed function",
          "func apply(fn: function, x: int) { return fn(x); }\n"
          "let f = func(y) { return y * 2; };\n"
          "apply(f, 3)\n",
          stdout="= 6")
    check("typed builtin",
          "func apply(fn: builtin, x: int) { return fn(x); }\n"
          "apply(sqrt, 16)\n",
          stdout="= 4.0")
    check("typed callable rejects int",
          "func apply(fn: callable) { return fn; }\napply(1)\n",
          stderr="expects callable, got int")
    check("typed function rejects builtin",
          "func apply(fn: function) { return fn; }\napply(sqrt)\n",
          stderr="expects function, got builtin")
    check("typed builtin rejects function",
          "func apply(fn: builtin) { return fn; }\n"
          "let f = func() { return 1; };\napply(f)\n",
          stderr="expects builtin, got function")
    check("typed any",
          "func f(x: any) { return x; }\nf(true)\n", stdout="= true")
    check("typed null",
          "func f(x: null) { return x; }\nprint(f(null))\n",
          stdout="null")
    check("typed null rejects int",
          "func f(x: null) { return x; }\nf(1)\n",
          stderr="expects null, got int")
    check("typed number accepts int and float",
          "func f(x: number) { return x; }\nf(1)\nf(1.5)\n",
          stdout_all=["= 1", "= 1.5"])
    check("typed anonymous function",
          "let f = func(x: int) { return x; };\nf(3)\n", stdout="= 3")
    check("typed closure",
          "func make() { func get(x: int) { return x + 1; } return get; }\n"
          "let f = make();\nf(4)\n",
          stdout="= 5")
    check("typed parameter trailing comma",
          "func f(x: int,) { return x; }\nf(1)\n", stdout="= 1")
    check("typed parameter error prevents body",
          "let hits = 0;\n"
          "func f(x: int) { hits = hits + 1; }\n"
          'f("bad");\nhits\n',
          stdout="= 0",
          stderr="expects int, got string")
    check("unknown type name",
          "func f(x: nope) { return x; }\n",
          stderr="unknown type name nope")

    check("typed variable declaration",
          'let x: string = "hi";\nx\nx = "bye";\nx\n',
          stdout_all=["= hi", "= bye", "= bye"])
    check("typed variable initializer mismatch",
          'let x: int = "bad";\n',
          stderr="variable x expects int, got string")
    check("typed variable reassignment mismatch",
          'let x: int = 1;\nx = "bad";\n',
          stderr="variable x expects int, got string")
    check("typed variable number accepts int and float",
          "let x: number = 1;\nx\nx = 2.5;\nx\n",
          stdout_all=["= 1", "= 2.5", "= 2.5"])
    check("typed variable function",
          "let f: function = func(x) { return x + 1; };\nf(2)\n",
          stdout="= 3")
    check("typed variable builtin",
          "let f: builtin = sqrt;\nf(16)\n", stdout="= 4.0")
    check("typed variable callable",
          "let f: callable = sqrt;\nf(9)\n", stdout="= 3.0")
    check("typed variable requires initializer for function",
          "let f: function;\n",
          stderr="variable f of type function requires an initializer")
    check("typed const declaration",
          "const x: int = 1;\nx\n", stdout_all=["= 1"])
    check("typed const reassignment",
          "const x: int = 1;\nx = 2;\n",
          stderr="cannot assign to constant x")
    check("typed for initializer",
          "for (let i: int = 0; i < 2; i++) { print(i); }\n",
          stdout_all=["0", "1"])
    check("typed parameter reassignment",
          'func f(x: int) { x = "bad"; }\nf(1)\n',
          stderr="variable x expects int, got string")
    check("typed variable defaults",
          "let s: string;\n"
          "let i: int;\n"
          "let f: float;\n"
          "let b: bool;\n"
          "let l: list;\n"
          "let d: dict;\n"
          "let by: bytes;\n"
          "let a: any;\n"
          's == "" && i == 0 && f == 0.0 && !b && len(l) == 0 && '
          "len(d) == 0 && bytes_len(by) == 0 && a == null\n",
          stdout="= true")
    check("typed variable is shallow",
          'let a: list = [0];\na[0] = "x";\na\n', stdout='= [x]')

    check("struct construction and fields",
          "struct Point { x: int; y: int; }\n"
          "let p = Point { x: 3, y: 4 };\n"
          "p.x + p.y\n"
          "p.x = 10;\n"
          "p.x\n"
          "p\n",
          stdout_all=["= 7", "= 10", "= Point { x: 10, y: 4 }"])
    check("struct method in struct",
          "struct Point {\n"
          "  x: int;\n"
          "  y: int;\n"
          "  func length(self) {\n"
          "    return sqrt(self.x * self.x + self.y * self.y);\n"
          "  }\n"
          "}\n"
          "Point { x: 3, y: 4 }.length()\n",
          stdout="= 5.0")
    check("struct method in impl",
          "struct Point { x: int; y: int; }\n"
          "impl Point {\n"
          "  func moved(self, dx: int, dy: int) {\n"
          "    return Point { x: self.x + dx, y: self.y + dy };\n"
          "  }\n"
          "}\n"
          "let p = Point { x: 3, y: 4 };\n"
          "p.moved(1, 2)\n",
          stdout="= Point { x: 4, y: 6 }")
    check("struct method read-only self",
          "struct Counter {\n"
          "  n: int;\n"
          "  func bump(self) { self.n = self.n + 1; }\n"
          "}\n"
          "let c = Counter { n: 1 };\nc.bump();\n",
          stderr="cannot modify self in a read-only method")
    check("struct self reassignment",
          "struct A { func f(self) { self = null; } }\n"
          "A {}.f();\n",
          stderr="cannot assign to constant self")
    check("struct missing field",
          "struct Point { x: int; y: int; }\nPoint { x: 1 }\n",
          stderr="field y of Point is required")
    check("struct unknown field",
          "struct Point { x: int; }\nPoint { x: 1, z: 2 }\n",
          stderr="unknown field z in Point")
    check("struct repeated field",
          "struct Point { x: int; }\nPoint { x: 1, x: 2 }\n",
          stderr="field x repeated in Point")
    check("struct field initializer type",
          'struct Point { x: int; }\nPoint { x: "bad" }\n',
          stderr="field x of Point expects int, got string")
    check("struct field assignment type",
          'struct Point { x: int; }\nlet p = Point { x: 1 };\np.x = "bad";\n',
          stderr="field x of Point expects int, got string")
    check("typed struct variable",
          "struct Point { x: int; }\n"
          "let p: Point = Point { x: 7 };\n"
          "func get_x(p: Point) { return p.x; }\n"
          "get_x(p)\n",
          stdout="= 7")
    check("typed struct variable mismatch",
          "struct Point { x: int; }\nlet p: Point = 1;\n",
          stderr="variable p expects Point, got int")
    check("struct copy-on-write",
          "struct Point { x: int; }\n"
          "let p = Point { x: 1 };\n"
          "let q = p;\n"
          "q.x = 9;\n"
          "p.x\nq.x\n",
          stdout_all=["= 1", "= 9"])
    check("nested struct field",
          "struct Inner { n: int; }\n"
          "struct Outer { inner: Inner; }\n"
          "let o = Outer { inner: Inner { n: 1 } };\n"
          "o.inner.n = 5;\no.inner.n\n",
          stdout="= 5")
    check("struct in list",
          "struct Point { x: int; }\n"
          "let a = [Point { x: 1 }, Point { x: 2 }];\n"
          "a[1].x++;\na[1].x\n",
          stdout="= 3")
    check("struct function field",
          "struct Handler { callback: callable; }\n"
          "let h = Handler { callback: func(x) { return x + 1; } };\n"
          "h.callback(41)\n",
          stdout="= 42")
    check("struct equality",
          "struct Point { x: int; }\n"
          "Point { x: 1 } == Point { x: 1 }\n"
          "Point { x: 1 } == Point { x: 2 }\n",
          stdout_all=["= true", "= false"])
    check("empty struct",
          "struct Empty {}\nlet e = Empty {};\ne\n", stdout="= Empty {}")
    check("bound method value",
          "struct Point {\n"
          "  x: int;\n"
          "  func get(self) { return self.x; }\n"
          "  func add(self, n: int) { return self.x + n; }\n"
          "}\n"
          "let p = Point { x: 3 };\n"
          "let f = p.get;\n"
          "let g = p.add;\n"
          "f()\ng(4)\n",
          stdout_all=["= 3", "= 7"])
    check("bound method snapshot",
          "struct Point { x: int; func get(self) { return self.x; } }\n"
          "let p = Point { x: 1 };\n"
          "let f = p.get;\n"
          "p.x = 5;\n"
          "f()\np.get()\n",
          stdout_all=["= 5", "= 1", "= 5"])

    check("trait implementation",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point {\n"
          "  func draw(self) { print(\"point\"); }\n"
          "}\n"
          "let p = Point { x: 1 };\n"
          "p.draw()\n"
          "func render(x: Drawable) { x.draw(); }\n"
          "render(p)\n",
          stdout_all=["point", "point"])
    check("trait adapter with existing method",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Point { func draw(self) { print(\"existing\"); } }\n"
          "impl Drawable for Point;\n"
          "let p = Point { x: 1 };\n"
          "func render(x: Drawable) { x.draw(); }\n"
          "render(p)\n",
          stdout="existing")
    check("trait nominal only",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Point { func draw(self) { print(\"point\"); } }\n"
          "func render(x: Drawable) { x.draw(); }\n"
          "render(Point { x: 1 });\n",
          stderr="expects Drawable, got Point")
    check("trait typed variable",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point { func draw(self) { return 1; } }\n"
          "let d: Drawable = Point { x: 1 };\nd.draw()\n",
          stdout="= 1")
    check("trait adapter signature mismatch",
          "trait Drawable { func draw(self, x: int); }\n"
          "struct Point { x: int; }\n"
          "impl Point { func draw(self, x: string) { return x; } }\n"
          "impl Drawable for Point;\n",
          stderr="method draw does not match the trait signature")
    check("trait missing method",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point { }\n",
          stderr="does not implement method draw required by trait Drawable")
    check("trait extra method",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point {\n"
          "  func other(self) { return 1; }\n"
          "}\n",
          stderr="method other is not declared by trait Drawable")
    check("duplicate trait implementation",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "impl Drawable for Point { func draw(self) { return 1; } }\n"
          "impl Drawable for Point { func draw(self) { return 2; } }\n",
          stderr="method repeated draw")
    check("duplicate method",
          "struct Point { x: int; }\n"
          "impl Point { func get(self) { return 1; } }\n"
          "impl Point { func get(self) { return 2; } }\n",
          stderr="method repeated get")
    check("field and method name conflict",
          "struct Point { x: int; func x(self) { return 1; } }\n",
          stderr="field and method have the same name x")
    check("method first parameter self",
          "struct Point { x: int; }\n"
          "impl Point { func get(this) { return 1; } }\n",
          stderr="method first parameter must be self")
    check("unknown struct type",
          "let p = Missing { x: 1 };\n", stderr="unknown struct type Missing")
    check("unknown field type",
          "struct Point { x: Missing; }\n", stderr="unknown type name Missing")
    check("struct switch unsupported",
          "struct Point { x: int; }\n"
          "switch (Point { x: 1 }) { default => print(\"bad\"); }\n",
          stderr="switch does not support Point")

    check("runtime kind_of",
          "kind_of(1)\n"
          "kind_of(1.5)\n"
          "kind_of(true)\n"
          'kind_of("x")\n'
          "kind_of([1])\n"
          "kind_of({})\n"
          "kind_of(func() { return 1; })\n"
          "kind_of(sqrt)\n"
          "kind_of(null)\n",
          stdout_all=["= int", "= float", "= bool", "= string", "= list",
                      "= dict", "= function", "= builtin", "= null"])
    check("runtime type_name",
          "struct Point { x: int; }\n"
          'type_name(1)\ntype_name("x")\ntype_name(Point { x: 1 })\n'
          "type_name(null)\n",
          stdout_all=["= int", "= string", "= Point", "= null"])
    check("runtime type_module for primitive",
          'type_module(1) == ""\n', stdout="= true")
    check("is primitive types",
          "1 is int\n1 is number\n1 is string\n"
          "true is bool\n[1] is list\n{} is dict\n"
          'null is null\n1 is any\n',
          stdout_all=["= true", "= true", "= false", "= true",
                      "= true", "= true", "= true", "= true"])
    check("is struct and trait",
          "trait Drawable { func draw(self); }\n"
          "struct Point { x: int; }\n"
          "struct Other { x: int; }\n"
          "impl Drawable for Point { func draw(self) { return 1; } }\n"
          "let p = Point { x: 1 };\n"
          "let o = Other { x: 1 };\n"
          "p is Point\np is Other\np is Drawable\no is Drawable\n",
          stdout_all=["= true", "= false", "= true", "= false"])
    check("is in heterogeneous loop",
          "struct Point { x: int; }\n"
          'let values = [1, "x", [2], Point { x: 3 }, null];\n'
          "for (let value in values) {\n"
          '  if (kind_of(value) == "struct") {\n'
          '    print("struct " + type_name(value));\n'
          "  } else {\n"
          "    print(kind_of(value));\n"
          "  }\n"
          "}\n",
          stdout_all=["int", "string", "list", "struct Point", "null"])
    check("is unknown type",
          "1 is Missing\n", stderr="unknown type name Missing")

    check("explicit empty return", "func f() { return; }\nprint(f())\n",
          stdout="null")
    check("print returns null", "let x = print(1);\nprint(x)\n",
          stdout_all=["1", "null"])
    check("arity error after argument evaluation",
          "func one(a) { return a; }\none(print(1), 2)\n",
          stdout="1", stderr="expects 1 argument")
    check("non-callable evaluates arguments", "1(print(7))\n",
          stdout="7", stderr="cannot call int")

    check("builtin as value", "let f = sqrt;\nf(9)\n", stdout="= 3.0")
    check("builtin as callback",
          "func apply(f, x) { return f(x); }\napply(sqrt, 16)\n",
          stdout="= 4.0")
    check("builtin in list", "let fs = [sqrt, abs];\nfs[0](25)\n",
          stdout="= 5.0")
    check("builtin equality", "sqrt == sqrt\nsqrt == abs\n",
          stdout_all=["= true", "= false"])
    check("print as value", 'let p = print;\np("ok")\n', stdout="ok")
    check("format as value", 'let f = format;\nf("{} + {}", 1, 2)\n',
          stdout="= 1 + 2")
    check("builtin string form", "str(sqrt)\n", stdout="= <builtin>")
    check("command output", 'command("echo hi")\n', stdout="= hi")
    check("command in format", 'format("{}", command("echo hi"))\n',
          stdout="= hi")
    check("command as value", 'let run = command;\nrun("echo ok")\n',
          stdout="= ok")
    check("command type error", "command(1)\n",
          stderr="command expects a string")
    check("command arity error", "command()\n",
          stderr="expects 1 or 2 arguments")
    command_env_test = (
        'command("echo %FOO%", {"FOO": "bar"})\n'
        if sys.platform == "win32"
        else 'command("echo $FOO", {"FOO": "bar"})\n'
    )
    check("command environment override", command_env_test, stdout="= bar")
    command_status_env_test = (
        'command_status("exit %CODE%", {"CODE": "3"})\n'
        if sys.platform == "win32"
        else 'command_status("exit $CODE", {"CODE": "3"})\n'
    )
    check("command_status environment override", command_status_env_test,
          stdout="= 3")
    check("command empty environment", "command(\"echo hi\", dict())\n",
          stdout="= hi")
    check("command env type", "command(\"echo hi\", 1)\n",
          stderr="command env must be a dict")
    check("command env key type", 'command("echo hi", {1: "x"})\n',
          stderr="command env keys must be strings")
    check("command env value type", 'command("echo hi", {"A": 1})\n',
          stderr="command env values must be strings")
    check("command env invalid name", 'command("echo hi", {"A=B": "x"})\n',
          stderr="invalid environment variable name")
    check("command env arity error", 'command("echo hi", dict(), 1)\n',
          stderr="expects 1 or 2 arguments")
    check("command_status env type", "command_status(\"exit 0\", 1)\n",
          stderr="command_status env must be a dict")
    check("command_status success", 'command_status("exit 0")\n',
          stdout="= 0")
    check("command_status failure", 'command_status("exit 3")\n',
          stdout="= 3")
    check("command_status output", 'command_status("echo hi")\n',
          stdout_all=["hi", "= 0"])
    check("command_status as value",
          'let run = command_status;\nrun("exit 0")\n', stdout="= 0")
    check("command_status type error", "command_status(1)\n",
          stderr="command_status expects a string")
    check("command_status arity error", "command_status()\n",
          stderr="expects 1 or 2 arguments")
    check("input reads a line", "input()\nhello\n", stdout="= hello")
    check("input with prompt", 'input("Name: ")\nAlice\n',
          stdout_all=["Name: ", "= Alice"])
    check("input with print", 'print(input("> "))\nworld\n',
          stdout_all=["> ", "world"])
    check("input preserves spaces", "input()\n  hi  \n", stdout="=   hi  ")
    check("input CRLF", "input()\r\nhello\r\n", stdout="= hello")
    check("input backspace", "input()\nhello\x7f\x7f\n", stdout="= hel")
    check("input EOF is null", "input() == null\n", stdout="= true")
    check("input EOF print", "print(input())\n", stdout="null")
    check("input prompt type", "input(1)\n",
          stderr="input prompt must be a string")
    check("input arity", 'input("a", "b")\n',
          stderr="input expects 0 or 1 arguments")
    check("input as value", 'let f = input;\nf("> ")\nvalue\n',
          stdout_all=["> ", "= value"])
    check("read_file type error", "read_file(1)\n",
          stderr="read_file expects a string path")
    check("write_file type error", 'write_file("x", 1)\n',
          stderr="write_file expects a string path and data")
    check("write_file arity error", 'write_file("x")\n',
          stderr="write_file expects 2 arguments")
    check("append_file arity error", 'append_file("x")\n',
          stderr="append_file expects 2 arguments")
    check("file_exists type error", "file_exists(1)\n",
          stderr="file_exists expects a string path")
    check("bytes_from_string display", 'bytes_from_string("abc")\n',
          stdout="= <bytes: 3>")
    check("bytes_len", 'bytes_len(bytes_from_string("abc"))\n', stdout="= 3")
    check("bytes_get", 'bytes_get(bytes_from_string("abc"), 1)\n',
          stdout="= 98")
    check("bytes_get out of range",
          'bytes_get(bytes_from_string("a"), 5)\n',
          stderr="bytes_get index 5 out of range")
    check("bytes_get type", 'bytes_get("x", 0)\n',
          stderr="bytes_get expects bytes")
    check("bytes_slice", 'bytes_hex(bytes_slice('
          'bytes_from_string("abcdef"), 1, 3))\n', stdout="= 626364")
    check("bytes_slice past end",
          'bytes_len(bytes_slice(bytes_from_string("abc"), 10))\n',
          stdout="= 0")
    check("bytes_slice clamps length",
          'bytes_hex(bytes_slice(bytes_from_string("abc"), 1, 100))\n',
          stdout="= 6263")
    check("bytes_concat", 'bytes_hex(bytes_concat('
          'bytes_from_string("ab"), bytes_from_string("cd")))\n',
          stdout="= 61626364")
    check("bytes_hex", 'bytes_hex(bytes_from_hex("00ff10"))\n',
          stdout="= 00ff10")
    check("bytes_from_hex odd", 'bytes_from_hex("0")\n',
          stderr="even number of hex digits")
    check("bytes_from_hex invalid", 'bytes_from_hex("zz")\n',
          stderr="invalid hex digit")
    check("bytes_from_hex type", "bytes_from_hex(1)\n",
          stderr="bytes_from_hex expects a string")
    check("bytes_to_string", 'bytes_to_string(bytes_from_hex("68656c6c6f"))\n',
          stdout="= hello")
    check("bytes_to_string NUL", 'bytes_to_string(bytes_from_hex("00"))\n',
          stderr="bytes contain NUL byte")
    check("bytes_to_string invalid utf8",
          'bytes_to_string(bytes_from_hex("ff"))\n',
          stderr="not valid UTF-8")
    check("bytes_to_string overlong utf8",
          'bytes_to_string(bytes_from_hex("c080"))\n',
          stderr="not valid UTF-8")
    check("bytes_to_string surrogate utf8",
          'bytes_to_string(bytes_from_hex("eda080"))\n',
          stderr="not valid UTF-8")
    check("bytes_to_string utf8 roundtrip",
          'bytes_to_string(bytes_from_string("你好"))\n',
          stdout="= 你好")
    check("bytes_to_string emoji roundtrip",
          'bytes_to_string(bytes_from_string("😀"))\n',
          stdout="= 😀")
    check("bytes_to_int little",
          'bytes_to_int(bytes_from_hex("78563412"), 4, "little")\n',
          stdout="= 305419896")
    check("bytes_to_int big",
          'bytes_to_int(bytes_from_hex("78563412"), 4, "big")\n',
          stdout="= 2018915346")
    check("bytes_to_int signed 16",
          'bytes_to_int(bytes_from_hex("ffff"), 2, "little", 0, 1)\n',
          stdout="= -1")
    check("bytes_to_int unsigned 16",
          'bytes_to_int(bytes_from_hex("ffff"), 2, "little", 0, 0)\n',
          stdout="= 65535")
    check("bytes_to_int signed 8",
          'bytes_to_int(bytes_from_hex("80"), 1, "big", 0, 1)\n',
          stdout="= -128")
    check("bytes_to_int offset",
          'bytes_to_int(bytes_from_hex("00ff80"), 2, "big", 1)\n',
          stdout="= 65408")
    check("bytes_to_int signed 64",
          'bytes_to_int(bytes_from_hex("ffffffffffffffff"), 8, "little", 0, 1)\n',
          stdout="= -1")
    check("bytes_to_int unsigned 64 overflow",
          'bytes_to_int(bytes_from_hex("ffffffffffffffff"), 8, "little")\n',
          stderr="unsigned 64-bit value is too large")
    check("bytes_to_int truncated",
          'bytes_to_int(bytes_from_hex("0102"), 4, "little")\n',
          stderr="need 4 bytes at offset 0, got 2")
    check("bytes_to_int size range",
          'bytes_to_int(bytes_from_hex("01"), 0, "little")\n',
          stderr="size must be between 1 and 8")
    check("bytes_to_int byteorder",
          'bytes_to_int(bytes_from_hex("01"), 1, "middle")\n',
          stderr='byteorder must be "little" or "big"')
    check("bytes_to_int negative offset",
          'bytes_to_int(bytes_from_hex("01"), 1, "little", -1)\n',
          stderr="offset must be a non-negative integer")
    check("bytes_to_int type", 'bytes_to_int("x", 1, "little")\n',
          stderr="bytes_to_int expects bytes")
    check("bytes_to_int arity",
          'bytes_to_int(bytes_from_hex("01"))\n',
          stderr="bytes_to_int expects 3 to 5 arguments")
    check("int_to_bytes little",
          'bytes_hex(int_to_bytes(305419896, 4, "little"))\n',
          stdout="= 78563412")
    check("int_to_bytes big",
          'bytes_hex(int_to_bytes(305419896, 4, "big"))\n',
          stdout="= 12345678")
    check("int_to_bytes negative",
          'bytes_hex(int_to_bytes(-1, 2, "little"))\n', stdout="= ffff")
    check("int_to_bytes negative big",
          'bytes_hex(int_to_bytes(-2, 4, "big"))\n', stdout="= fffffffe")
    check("int_to_bytes unsigned 32",
          'bytes_hex(int_to_bytes(4294967295, 4, "little"))\n',
          stdout="= ffffffff")
    check("int_to_bytes range high",
          'int_to_bytes(256, 1, "little")\n',
          stderr="does not fit in 1 bytes")
    check("int_to_bytes range low",
          'int_to_bytes(-129, 1, "little")\n',
          stderr="does not fit in 1 bytes")
    check("int_to_bytes size range",
          'int_to_bytes(1, 9, "little")\n',
          stderr="size must be between 1 and 8")
    check("int_to_bytes byteorder",
          'int_to_bytes(1, 4, "middle")\n',
          stderr='byteorder must be "little" or "big"')
    check("int_to_bytes type", 'int_to_bytes("x", 4, "little")\n',
          stderr="int_to_bytes expects an integer value")
    check("int_to_bytes arity", 'int_to_bytes(1, 4)\n',
          stderr="int_to_bytes expects 3 arguments")
    check("bytes/int roundtrip",
          'bytes_to_int(int_to_bytes(-123456789, 8, "big"), '
          '8, "big", 0, 1)\n',
          stdout="= -123456789")
    check("bytes index read", 'bytes_from_string("hello")[1]\n',
          stdout="= 101")
    check("bytes index out of range", 'bytes_from_string("a")[5]\n',
          stderr="bytes index 5 out of range")
    check("bytes index assignment",
          'let b = bytes_from_string("abc");\n'
          'b[1] = 255;\n'
          'bytes_hex(b)\n',
          stdout="= 61ff63")
    check("bytes index assignment range",
          'let b = bytes_from_string("abc");\n'
          'b[0] = 256;\n',
          stderr="bytes value 256 out of range")
    check("bytes equality",
          'bytes_from_string("ab") == bytes_from_hex("6162")\n',
          stdout="= true")
    check("bytes arity", "bytes_len()\n",
          stderr="built-in function expects 1 argument")
    check("bytes_slice arity", "bytes_slice(bytes_from_string(\"a\"))\n",
          stderr="bytes_slice expects 2 or 3 arguments")
    check("read_bytes type", "read_bytes(1)\n",
          stderr="read_bytes expects a string path")
    check("write_bytes type", 'write_bytes("x", "text")\n',
          stderr="write_bytes expects a string path and bytes")
    check("bytes hex roundtrip type", "bytes_hex(1)\n",
          stderr="bytes_hex expects bytes")

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "io.txt")
        quoted = path.replace("\\", "\\\\").replace('"', '\\"')
        check("write and read file",
              f'write_file("{quoted}", "hello");\nread_file("{quoted}")\n',
              stdout="= hello")
        check("append file",
              f'append_file("{quoted}", " world");\nread_file("{quoted}")\n',
              stdout="= hello world")
        check("write file truncates",
              f'write_file("{quoted}", "reset");\nread_file("{quoted}")\n',
              stdout="= reset")
        check("file exists", f'file_exists("{quoted}")\n', stdout="= 1")
        check("file does not exist",
              f'file_exists("{quoted}.missing")\n', stdout="= 0")
        check("read missing file", f'read_file("{quoted}.missing")\n',
              stderr="read_file:")
        missing_dir = os.path.join(tmp, "missing", "file.txt")
        quoted_missing = missing_dir.replace("\\", "\\\\")
        check("write missing directory",
              f'write_file("{quoted_missing}", "x")\n',
              stderr="write_file:")

        empty = os.path.join(tmp, "empty.txt")
        quoted_empty = empty.replace("\\", "\\\\")
        check("empty file",
              f'write_file("{quoted_empty}", "");\nread_file("{quoted_empty}")\n',
              stdout="= ")

        nul_file = os.path.join(tmp, "nul.bin")
        with open(nul_file, "wb") as f:
            f.write(b"a\0b")
        quoted_nul = nul_file.replace("\\", "\\\\")
        check("read file with NUL", f'read_file("{quoted_nul}")\n',
              stderr="contains NUL byte")
        check("read_bytes keeps NUL",
              f'bytes_hex(read_bytes("{quoted_nul}"))\n',
              stdout="= 610062")
        binary_file = os.path.join(tmp, "binary.bin")
        quoted_binary = binary_file.replace("\\", "\\\\")
        check("write and read bytes",
              f'write_bytes("{quoted_binary}", bytes_from_hex("00ff10"));\n'
              f'bytes_hex(read_bytes("{quoted_binary}"))\n',
              stdout="= 00ff10")
        check("append bytes",
              f'append_bytes("{quoted_binary}", bytes_from_hex("20"));\n'
              f'bytes_hex(read_bytes("{quoted_binary}"))\n',
              stdout="= 00ff1020")
        check("read missing bytes",
              f'read_bytes("{quoted_binary}.missing")\n',
              stderr="read_bytes:")
        check(
            "file handle binary seek/write",
            f'let h = file_open("{quoted_binary}", "w+b");\n'
            f'file_write_bytes(h, bytes_from_hex("01020304"));\n'
            f'file_seek(h, 1);\n'
            f'file_write_bytes(h, bytes_from_hex("ff"));\n'
            f'file_close(h);\n'
            f'bytes_hex(read_bytes("{quoted_binary}"))\n',
            stdout="= 01ff0304",
        )
        check(
            "file handle read bytes",
            f'let h = file_open("{quoted_binary}", "rb");\n'
            f'bytes_hex(file_read_bytes(h, 2));\n'
            f'file_tell(h);\n'
            f'bytes_hex(file_read_bytes(h));\n'
            f'file_close(h);\n',
            stdout_all=["= 01ff", "= 2", "= 0304"],
        )
        check(
            "file handle read bytes keeps NUL",
            f'let h = file_open("{quoted_nul}", "rb");\n'
            f'bytes_hex(file_read_bytes(h))\n',
            stdout="= 610062",
        )
        check("file_read_bytes arity", "file_read_bytes()\n",
              stderr="file_read_bytes expects 1 or 2 arguments")
        check("file_read_bytes type", "file_read_bytes(1)\n",
              stderr="file_read_bytes expects a file")
        check("file_write_bytes type",
              f'let h = file_open("{quoted_binary}", "wb");\n'
              f'file_write_bytes(h, "x")\n',
              stderr="file_write_bytes expects bytes data")

        handle_path = os.path.join(tmp, "handle.txt")
        quoted_handle = handle_path.replace("\\", "\\\\")
        check(
            "file handle write/seek/read/tell",
            f'let h = file_open("{quoted_handle}", "w+");\n'
            f'file_write(h, "hello world");\n'
            f'file_seek(h, 0);\n'
            f'file_read(h, 5);\n'
            f'file_tell(h);\n'
            f'file_close(h);\n',
            stdout_all=["= hello", "= 5"],
        )
        check(
            "file handle read all and eof",
            f'let h = file_open("{quoted_handle}", "r");\n'
            f'file_read(h);\n'
            f'file_eof(h);\n'
            f'file_close(h);\n',
            stdout_all=["= hello world", "= 1"],
        )
        check(
            "file handle seek from end",
            f'let h = file_open("{quoted_handle}", "r");\n'
            f'file_seek(h, -1, 2);\n'
            f'file_read(h, 1)\n',
            stdout="= d",
        )
        check(
            "file handle NUL",
            f'let h = file_open("{quoted_nul}", "r");\n'
            f'file_read(h)\n',
            stderr="contains NUL byte",
        )
        check(
            "file handle close and copy",
            f'let h = file_open("{quoted_handle}", "r");\n'
            f'let g = h;\n'
            f'file_close(h);\n'
            f'file_read(g, 1)\n',
            stderr="file is closed",
        )
        check(
            "file handle factory",
            f'func open_file(path, mode) {{\n'
            f'  let h = file_open(path, mode);\n'
            f'  func read(n) {{ return file_read(h, n); }}\n'
            f'  func write(s) {{ return file_write(h, s); }}\n'
            f'  func seek(n) {{ return file_seek(h, n); }}\n'
            f'  func close() {{ return file_close(h); }}\n'
            f'  return {{"read": read, "write": write, '
            f'"seek": seek, "close": close}};\n'
            f'}}\n'
            f'let f = open_file("{quoted_handle}", "w+");\n'
            f'f.write("abcdef");\n'
            f'f.seek(0);\n'
            f'f.read(3);\n'
            f'f.close();\n',
            stdout="= abc",
        )
        check("file_open invalid mode", 'file_open("x", "bad")\n',
              stderr="invalid file mode")
        check("file_open arity", 'file_open("x")\n',
              stderr="file_open expects 2 arguments")
        check("file_read arity", "file_read()\n",
              stderr="file_read expects 1 or 2 arguments")
        check("file_write arity", 'file_write("x")\n',
              stderr="file_write expects 2 arguments")
        check("file_seek arity", "file_seek(1)\n",
              stderr="file_seek expects 2 or 3 arguments")
        check("file_read type", "file_read(1)\n",
              stderr="file_read expects a file")
        check("file_read size type",
              f'let h = file_open("{quoted_handle}", "r");\n'
              f'file_read(h, "x")\n',
              stderr="file_read size must be a non-negative integer")
        check("file_seek whence type",
              f'let h = file_open("{quoted_handle}", "r");\n'
              f'file_seek(h, 0, "x")\n',
              stderr="file_seek whence must be an integer")

    proc = run("func bad() { 1/0; }\nbad() + 1\n")
    assert "division by zero" in proc.stderr
    assert "cannot add null" not in proc.stderr

    proc = run(
        "func f(n) { if (n == 0) { return 1/0; } return f(n - 1); }\n"
        "f(3)\n"
    )
    assert "division by zero" in proc.stderr
    assert "maximum recursion depth" not in proc.stderr

    proc = run('1/0; print("after");\n')
    assert "division by zero" in proc.stderr
    assert "after" not in proc.stdout


    check("int conversion from float", "int(2.9)\n", stdout="= 2")
    check("int conversion truncates toward zero", "int(-2.9)\n", stdout="= -2")
    check("int conversion from string", 'int("  -42  ")\n', stdout="= -42")
    check("float conversion from int", "float(3)\n", stdout="= 3.0")
    check("float conversion from string", 'float("1.5")\n', stdout="= 1.5")
    check("str conversion of float", "str(1.0)\n", stdout="= 1.0")
    check("str conversion of list", "str([1, 2])\n", stdout="= [1, 2]")
    check("int conversion rejects float string", 'int("4.2")\n',
          stderr="int() cannot convert")
    check("int conversion out of range", "int(9223372036854775808.0)\n",
          stderr="int() value out of range")
    check("int conversion from int64 string",
          'int("9223372036854775807")\n', stdout="= 9223372036854775807")

    check("format basic", 'format("x = {}", 3)\n', stdout="= x = 3")
    check("format multiple", 'format("{} + {} = {}", 1, 2, 3)\n',
          stdout="= 1 + 2 = 3")
    check("format positional", 'format("{1} {0}", "a", "b")\n',
          stdout="= b a")
    check("format escaped braces", 'format("{{}} = {}", 1)\n',
          stdout="= {} = 1")
    check("format float precision", 'format("{:.2f}", 1.5)\n',
          stdout="= 1.50")
    check("format integer type", 'format("{:d}", 42)\n', stdout="= 42")
    check("format hex", 'format("{:x} {:X}", 255, 255)\n', stdout="= ff FF")
    check("format width", 'format("[{:>6}]", "hi")\n', stdout="= [    hi]")
    check("format center", 'format("[{:*^8}]", "hi")\n', stdout="= [***hi***]")
    check("format zero padding", 'format("{:08}", -42)\n', stdout="= -0000042")
    check("format utf8 width", 'format("[{:4}]", "你")\n', stdout="= [你   ]")
    check("format utf8 precision", 'format("{:.2s}", "你好ab")\n',
          stdout="= 你好")
    check("format extra args ignored", 'format("{}", 1, 2)\n', stdout="= 1")
    check("format too few args", 'format("{} {}", 1)\n',
          stderr="out of range")
    check("format mixed numbering", 'format("{} {0}", 1)\n',
          stderr="cannot mix")
    check("format unmatched brace", 'format("{")\n', stderr="unmatched")
    check("format unknown type", 'format("{:q}", 1)\n',
          stderr="unknown format type")
    check("format type mismatch", 'format("{:d}", "x")\n',
          stderr="requires an integer")
    check("format precision requires type", 'format("{:.2}", 1.5)\n',
          stderr="precision requires")
    check("print formatting", 'print("x = {}", 3)\n', stdout="x = 3")
    check("print raw single argument", 'print("{{}}")\n', stdout="{{}}")
    check("print multi-arg formatting", 'print("{} + {} = {}", 1, 2, 3)\n',
          stdout="1 + 2 = 3")
    check("print extra args ignored", 'print("x", 1)\n', stdout="x")
    check("print non-string format", 'print(1, 2)\n',
          stderr="first argument must be a string")
    check("builtin arity", "sqrt(4,9)\n", stderr="expects 1 argument")
    check("return outside", "return 5;\n", stderr="return outside function")
    check("break outside", "break;\n", stderr="break outside loop")
    check("mystery character", "1 $ 2\n",
          stderr="lexical error: unexpected character '$' (0x24)")

    # Unterminated strings must never hang or crash.
    check("unterminated string newline", 'print("unterminated)\n',
          stderr="unterminated string literal")
    proc = run('print("unterminated)', timeout=5)
    assert "unterminated string literal" in proc.stderr
    assert "Segmentation fault" not in proc.stderr

    check("empty input", "", code=0)
    check("empty block statement", "{ ; }\n", code=0)
    check("empty if block", "if (1) {}\n", code=0)
    check("empty if else blocks", "if (0) {} else {}\n", code=0)
    check("empty while body", "while (0) {}\n", code=0)
    check("empty for body", "for (let i = 0; i < 1; i = i + 1) {}\n", code=0)
    check("empty func body", "func f() {}\nf()\n", code=0)
    check("empty package body", "package pkg {}\n", code=0)
    check("empty dict in expression", "len({})\n", stdout="= 0")
    check("empty dict as value", 'let d = {"a": {}};\nd["a"]\n',
          stdout="= {}")
    check("empty block inside block", "if (1) { {} }\n", code=0)
    check("utf8 bom", "\ufeff1+2\n", stdout="= 3")
    check("crlf input", "1+2\r\n", stdout="= 3")

    with tempfile.TemporaryDirectory() as tmp:
        script = os.path.join(tmp, "script.calc")
        with open(script, "w", encoding="utf-8") as f:
            f.write(
                "#!/usr/bin/env calc-demo\n"
                'print("script");\n'
                "1 + 2\n"
            )
        proc = run_file(script)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "script\n", repr(proc.stdout)
        assert proc.stderr == "", proc.stderr

        bom_script = os.path.join(tmp, "bom.calc")
        with open(bom_script, "w", encoding="utf-8") as f:
            f.write("\ufeff#!/usr/bin/env calc-demo\nprint(\"bom\");\n")
        proc = run_file(bom_script)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "bom\n", repr(proc.stdout)

        input_script = os.path.join(tmp, "input.calc")
        with open(input_script, "w", encoding="utf-8") as f:
            f.write('print(input("? "));\n')
        proc = subprocess.run(
            [BIN, input_script],
            input="hello\n",
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "? hello\n", repr(proc.stdout)

        if sys.platform != "win32":
            executable = os.path.abspath(BIN)
            if " " not in executable:
                shebang_script = os.path.join(tmp, "shebang.calc")
                with open(shebang_script, "w", encoding="utf-8") as f:
                    f.write(f"#!{executable}\nprint(\"shebang\");\n")
                os.chmod(shebang_script, 0o755)
                proc = subprocess.run(
                    [shebang_script],
                    text=True,
                    encoding="utf-8",
                    capture_output=True,
                    timeout=5,
                )
                assert proc.returncode == 0, proc.stderr
                assert proc.stdout == "shebang\n", repr(proc.stdout)

    proc = subprocess.run(
        [BIN, "-"],
        input='print("stdin-script");\n',
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=5,
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout == "stdin-script\n", repr(proc.stdout)

    with tempfile.TemporaryDirectory() as tmp:
        runtime_script = os.path.join(tmp, "runtime.calc")
        with open(runtime_script, "w", encoding="utf-8") as f:
            f.write('print("a");\n1/0;\n')
        proc = run_file(runtime_script)
        assert proc.returncode == 1, proc.stderr
        assert "2:1: runtime error: division by zero" in proc.stderr, proc.stderr
        assert "  1/0;\n  ^" in proc.stderr, proc.stderr

        nested_script = os.path.join(tmp, "nested.calc")
        with open(nested_script, "w", encoding="utf-8") as f:
            f.write(
                "func g() {\n"
                "  return 10 / 0;\n"
                "}\n"
                "func f() {\n"
                "  return g();\n"
                "}\n"
                "f();\n"
            )
        proc = run_file(nested_script)
        assert proc.returncode == 1, proc.stderr
        assert "2:10: runtime error: division by zero" in proc.stderr, proc.stderr
        assert "in g() called from line 5" in proc.stderr, proc.stderr
        assert "in f() called from line 7" in proc.stderr, proc.stderr

        clean_script = os.path.join(tmp, "clean.calc")
        with open(clean_script, "w", encoding="utf-8") as f:
            f.write("print(\"ok\");\n")
        proc = run_file(clean_script)
        assert proc.returncode == 0, proc.stderr

        syntax_script = os.path.join(tmp, "syntax.calc")
        with open(syntax_script, "w", encoding="utf-8") as f:
            f.write("let x = ;\n")
        proc = run_file(syntax_script)
        assert proc.returncode == 1, proc.stderr
        assert "1:9: syntax error: unexpected ';'" in proc.stderr, proc.stderr

    check("syntax error detail", "let x = ;\n",
          stderr="syntax error: unexpected ';', expecting")
    check("syntax error snippet", "let x = ;\n",
          stderr="  let x = ;\n          ^")
    check("lexical error position", "1 $ 2\n",
          stderr="1:3: lexical error: unexpected character '$' (0x24)")
    check("unterminated string position", 'print("unterminated)\n',
          stderr="1:7: lexical error: unterminated string literal")
    check("definition error location", "func f(a, a) {\n  return a;\n}\n",
          stderr="1:1: runtime error: duplicate parameter")
    check("break error location", "{\n  break;\n}\n",
          stderr="2:3: runtime error: break outside loop")

    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write(
            "app/aa/aa.calc",
            'package aa { export let root = "aa"; }\n',
        )
        write(
            "app/aa/bb/bb.calc",
            'package bb { export let name = "bb"; }\n',
        )
        write(
            "app/aa/bb/cc/cc.calc",
            "package cc {\n"
            "  export let value = 42;\n"
            "  export const fixed = 7;\n"
            "  export func hello(name) { return \"hi \" + name; }\n"
            "  func hidden() { return 99; }\n"
            "}\n",
        )
        app_main = write(
            "app/main.calc",
            "import aa.bb.cc;\n"
            "print(cc.value);\n"
            "print(cc.fixed);\n"
            "print(cc.hello(\"x\"));\n"
            "import aa.bb;\n"
            "print(bb.name);\n"
            "print(bb.cc.value);\n"
            "import aa;\n"
            "print(aa.root);\n"
            "print(aa.bb.cc.value);\n"
            "import aa.bb.cc as c;\n"
            "print(c.value);\n"
            "import aa.bb as b;\n"
            "print(b.cc.value);\n"
            "import aa as a;\n"
            "print(a.root);\n"
            "print(a.bb.cc.value);\n",
        )
        proc = run_file(app_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == [
            "42",
            "7",
            "hi x",
            "bb",
            "42",
            "aa",
            "42",
            "42",
            "42",
            "aa",
            "42",
        ], proc.stdout
        assert proc.stderr == "", proc.stderr

        write(
            "app/aa/state/state.calc",
            "package state {\n"
            "  let n = 0;\n"
            "  export func inc() { n = n + 1; return n; }\n"
            "  export func get() { return n; }\n"
            "}\n",
        )
        state_main = write(
            "app/state_main.calc",
            "import aa.state;\n"
            "func again() {\n"
            "  import aa.state;\n"
            "  return state.inc();\n"
            "}\n"
            + "1;\n" * 70
            + "print(state.inc());\n"
            "print(again());\n"
            "print(state.get());\n",
        )
        proc = run_file(state_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["1", "2", "2"], proc.stdout

        write(
            "container/aa/bb/cc/cc.calc",
            'package cc { export let value = "deep"; }\n',
        )
        container_main = write(
            "container/main.calc",
            "import aa;\nprint(aa.bb.cc.value);\n",
        )
        proc = run_file(container_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "deep\n", repr(proc.stdout)

        # Package lookup paths: source dir > CLI dirs > environment dirs.
        write(
            "search/app/aa/bb/cc/cc.calc",
            'package cc { export let value = "source"; }\n',
        )
        search_main = write(
            "search/app/main.calc",
            "import aa.bb.cc;\nprint(cc.value);\n",
        )
        write(
            "search/lib1/aa/bb/cc/cc.calc",
            'package cc { export let value = "cli1"; }\n',
        )
        write(
            "search/lib2/aa/bb/cc/cc.calc",
            'package cc { export let value = "cli2"; }\n',
        )
        write(
            "search/env/aa/bb/cc/cc.calc",
            'package cc { export let value = "env"; }\n',
        )
        proc = run_file(search_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "source\n", repr(proc.stdout)

        app_without_local = write(
            "search/app2/main.calc",
            "import aa.bb.cc;\nprint(cc.value);\n",
        )
        lib1 = os.path.join(tmp, "search/lib1")
        lib2 = os.path.join(tmp, "search/lib2")
        envlib = os.path.join(tmp, "search/env")
        proc = subprocess.run(
            [BIN, "-I", lib1, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "cli1\n", repr(proc.stdout)

        proc = subprocess.run(
            [BIN, "-I", lib1, "-I", lib2, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "cli1\n", repr(proc.stdout)

        proc = subprocess.run(
            [BIN, "-I", lib2, "-I", lib1, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "cli2\n", repr(proc.stdout)

        env = os.environ.copy()
        env["CALC_PACKAGE_PATH"] = envlib
        proc = subprocess.run(
            [BIN, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
            env=env,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "env\n", repr(proc.stdout)

        # CLI dirs take priority over environment dirs.
        proc = subprocess.run(
            [BIN, "-I", lib1, app_without_local],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
            env=env,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "cli1\n", repr(proc.stdout)

        proc = subprocess.run(
            [BIN, "-I", lib1],
            input="import aa.bb.cc;\nprint(cc.value);\n",
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert "cli1\n" in proc.stdout, repr(proc.stdout)

        # Packages are isolated from the main package's globals.
        write(
            "isolated/aa/bb/cc/cc.calc",
            "package cc { export let leaked = main_secret; }\n",
        )
        isolated_main = write(
            "isolated/main.calc",
            "let main_secret = 1;\nimport aa.bb.cc;\n",
        )
        proc = run_file(isolated_main)
        assert proc.returncode == 1, proc.stderr
        assert "symbol not found main_secret" in proc.stderr, proc.stderr

        # Package names must match their containing folder.
        write(
            "wrongname/aa/bb/cc/cc.calc",
            "package wrong { export let value = 1; }\n",
        )
        wrong_main = write(
            "wrongname/main.calc",
            "import aa.bb.cc;\n",
        )
        proc = run_file(wrong_main)
        assert proc.returncode == 1, proc.stderr
        assert "must match folder" in proc.stderr, proc.stderr

        # Explicit main package and exported/private names.
        main_pkg = write(
            "mainpkg/main/main.calc",
            "package main {\n"
            "  export let x = 5;\n"
            "  func private() { return 9; }\n"
            "}\n"
            "print(main.x);\n",
        )
        proc = run_file(main_pkg)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "5\n", repr(proc.stdout)

        # Cyclic imports fail cleanly instead of hanging.
        write(
            "cycle/aa/bb/bb.calc",
            "package bb { import aa.cc; export let x = 1; }\n",
        )
        write(
            "cycle/aa/cc/cc.calc",
            "package cc { import aa.bb; export let y = 2; }\n",
        )
        cycle_main = write(
            "cycle/main.calc",
            "import aa.bb;\n",
        )
        proc = run_file(cycle_main)
        assert proc.returncode == 1, proc.stderr
        assert "cyclic package import" in proc.stderr, proc.stderr

        missing_main = write(
            "missing/main.calc",
            "import no.such;\n",
        )
        proc = run_file(missing_main)
        assert proc.returncode == 1, proc.stderr
        assert "package not found" in proc.stderr, proc.stderr

    # Relative imports (. and ..) and project-root imports (@).
    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.join(tmp, "proj")
        os.makedirs(root, exist_ok=True)

        def write(rel, text):
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        with open(os.path.join(root, ".calcroot"), "w", encoding="utf-8"):
            pass

        write("top/pkg/pkg.calc",
              'package pkg { export let value = "root"; }\n')
        write("lib/top/pkg/pkg.calc",
              'package pkg { export let value = "global"; }\n')
        write("src/common/common.calc",
              'package common { export let value = "common"; }\n')
        write("src/app/local/local.calc",
              'package local { export let value = "local"; }\n')
        write("src/app/sibling/sibling.calc",
              'package sibling { export let value = "sibling"; }\n')
        write("src/app/user/helper/helper.calc",
              'package helper { export let value = "helper"; }\n')
        write(
            "src/app/user/user.calc",
            "package user {\n"
            "  export func run() {\n"
            "    import .helper;\n"
            "    import ..sibling;\n"
            "    import @top.pkg;\n"
            '    return helper.value + ":" + sibling.value + ":" + pkg.value;\n'
            "  }\n"
            "}\n",
        )
        relative_main = write(
            "src/app/main.calc",
            "import @top.pkg;\n"
            "print(pkg.value);\n"
            "import .local;\n"
            "print(local.value);\n"
            "import ..common;\n"
            "print(common.value);\n"
            "import .local as l;\n"
            "print(l.value);\n"
            "import ..common as c;\n"
            "print(c.value);\n"
            "import @top.pkg as p;\n"
            "print(p.value);\n"
            "import user;\n"
            "print(user.run());\n"
            "func later() {\n"
            "  import .local;\n"
            "  return local.value;\n"
            "}\n"
            "print(later());\n",
        )
        proc = subprocess.run(
            [BIN, "-I", os.path.join(root, "lib"), relative_main],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == [
            "root",
            "local",
            "common",
            "local",
            "common",
            "root",
            "helper:sibling:root",
            "local",
        ], proc.stdout

        # Without a .calcroot marker, @ falls back to the main script dir.
        nomarker = os.path.join(tmp, "nomarker")
        os.makedirs(os.path.join(nomarker, "local"), exist_ok=True)
        with open(os.path.join(nomarker, "local", "local.calc"),
                  "w", encoding="utf-8") as f:
            f.write('package local { export let value = "local"; }\n')
        fallback_main = os.path.join(nomarker, "main.calc")
        with open(fallback_main, "w", encoding="utf-8") as f:
            f.write("import @local;\nprint(local.value);\n")
        proc = run_file(fallback_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "local\n", repr(proc.stdout)

        # @ never falls back to -I directories.
        os.makedirs(os.path.join(nomarker, "lib", "top", "pkg"),
                    exist_ok=True)
        with open(os.path.join(nomarker, "lib", "top", "pkg", "pkg.calc"),
                  "w", encoding="utf-8") as f:
            f.write('package pkg { export let value = "global"; }\n')
        nofallback_main = os.path.join(nomarker, "nofallback.calc")
        with open(nofallback_main, "w", encoding="utf-8") as f:
            f.write("import @top.pkg;\n")
        proc = subprocess.run(
            [BIN, "-I", os.path.join(nomarker, "lib"), nofallback_main],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 1, proc.stderr
        assert "package not found" in proc.stderr, proc.stderr

    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write(
            "app/aa/aa.calc",
            "package aa {\n"
            "  export struct P { x: int; }\n"
            "  struct Hidden { h: int; }\n"
            "  export trait D { func d(self); }\n"
            "  impl D for P { func d(self) { return self.x; } }\n"
            "  impl P { func get(self) { return self.x; } }\n"
            "  export func make() { return P { x: 7 }; }\n"
            "  export func hidden() { return Hidden { h: 9 }; }\n"
            "}\n",
        )
        main = write(
            "app/main.calc",
            "import aa;\n"
            "struct P { y: string; }\n"
            "let pkg = aa.make();\n"
            'let local: P = P { y: "ok" };\n'
            "let imported: aa::P = pkg;\n"
            "print(pkg.get());\n"
            "print(local.y);\n"
            "func local_get(p: P) { return p.y; }\n"
            "print(local_get(local));\n"
            "print(pkg is P);\n"
            "print(local is P);\n"
            "print(type_module(pkg) == type_module(local));\n"
            "print(type_module(pkg));\n"
            "print(type_module(local));\n"
            "print(type_source(pkg) != \"\");\n"
            "print(imported.x);\n"
            "print(pkg is aa::D);\n",
        )
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == [
            "7", "ok", "ok", "false", "true", "false",
            "aa", "main", "true", "7", "true",
        ], proc.stdout
        assert proc.stderr == "", proc.stderr

        main2 = write(
            "app/main2.calc",
            "import aa;\n"
            "let p = aa.make();\n"
            "let q: P = p;\n",
        )
        proc = run_file(main2)
        assert proc.returncode == 1, proc.stdout
        assert "unknown type name P" in proc.stderr, proc.stderr

        main3 = write(
            "app/main3.calc",
            "import aa;\n"
            'let h: aa::Hidden = aa.hidden();\n',
        )
        proc = run_file(main3)
        assert proc.returncode == 1, proc.stdout
        assert "unknown type name aa::Hidden" in proc.stderr, proc.stderr

    with tempfile.TemporaryDirectory() as tmp:
        failfast = os.path.join(tmp, "failfast.calc")
        with open(failfast, "w", encoding="utf-8") as f:
            f.write('print("before");\n1/0;\nprint("after");\n')
        proc = run_file(failfast)
        assert proc.returncode == 1, proc.stderr
        assert "before\n" in proc.stdout, proc.stdout
        assert "after\n" not in proc.stdout, proc.stdout

        proc = subprocess.run(
            [BIN, "-k", failfast],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 1, proc.stderr
        assert "before\n" in proc.stdout, proc.stdout
        assert "after\n" in proc.stdout, proc.stdout

        uncaught = os.path.join(tmp, "uncaught.calc")
        with open(uncaught, "w", encoding="utf-8") as f:
            f.write('print("before");\nfail "boom";\nprint("after");\n')
        proc = run_file(uncaught)
        assert proc.returncode == 1, proc.stderr
        assert "before\n" in proc.stdout, proc.stdout
        assert "after\n" not in proc.stdout, proc.stdout
        assert "uncaught fail: boom" in proc.stderr, proc.stderr

        check(
            "assignment failure preserves old value",
            "let x = 1;\n"
            "func f() { return 1/0; }\n"
            "x = f();\n"
            "x\n",
            stdout="= 1",
        )

        lexical_script = os.path.join(tmp, "lexical.calc")
        with open(lexical_script, "w", encoding="utf-8") as f:
            f.write('1 $ 2;\nprint("after");\n')
        proc = run_file(lexical_script)
        assert proc.returncode == 1, proc.stderr
        assert "lexical error" in proc.stderr, proc.stderr
        assert "syntax error" not in proc.stderr, proc.stderr
        assert "after\n" not in proc.stdout, proc.stdout

        proc = subprocess.run(
            [BIN, "--bad-option"],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 2, proc.stderr

        proc = subprocess.run(
            [BIN, os.path.join(tmp, "missing.calc")],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 2, proc.stderr

        if sys.platform != "win32":
            interrupt_script = os.path.join(tmp, "interrupt.calc")
            with open(interrupt_script, "w", encoding="utf-8") as f:
                f.write("while (1) { }\n")
            proc = subprocess.Popen(
                [BIN, interrupt_script],
                text=True,
                encoding="utf-8",
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            time.sleep(0.2)
            proc.send_signal(signal.SIGINT)
            stdout, stderr = proc.communicate(timeout=5)
            assert proc.returncode == 130, (proc.returncode, stdout, stderr)

    check_interactive_continuation()

    print(f"all tests passed for {BIN}")


if __name__ == "__main__":
    main()
