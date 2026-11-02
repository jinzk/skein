#!/usr/bin/env python3
"""Golden/smoke tests for the calc-demo interpreter."""

import base64
import hashlib
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


def check(name, source, *, stdout=None, stdout_all=None, stderr=None,
          not_stderr=None, code=0):
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
    if not_stderr is not None:
        assert not_stderr not in proc.stderr, (
            f"{name}: expected stderr not to contain {not_stderr!r}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )


def ws_recv_exact(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise AssertionError("websocket: unexpected EOF")
        data += chunk
    return data


def ws_read_frame(sock):
    first, second = ws_recv_exact(sock, 2)
    opcode = first & 0x0f
    masked = bool(second & 0x80)
    length = second & 0x7f
    if length == 126:
        length = int.from_bytes(ws_recv_exact(sock, 2), "big")
    elif length == 127:
        length = int.from_bytes(ws_recv_exact(sock, 8), "big")
    mask = ws_recv_exact(sock, 4) if masked else b""
    payload = ws_recv_exact(sock, length) if length else b""
    if masked:
        payload = bytes(
            byte ^ mask[i % 4] for i, byte in enumerate(payload))
    return opcode, payload


def ws_write_frame(sock, opcode, payload, masked):
    header = bytes([0x80 | opcode])
    length = len(payload)
    mask_bit = 0x80 if masked else 0
    if length <= 125:
        header += bytes([mask_bit | length])
    elif length <= 65535:
        header += bytes([mask_bit | 126]) + length.to_bytes(2, "big")
    else:
        header += bytes([mask_bit | 127]) + length.to_bytes(8, "big")
    if masked:
        mask = b"\x11\x22\x33\x44"
        header += mask
        payload = bytes(
            byte ^ mask[i % 4] for i, byte in enumerate(payload))
    sock.sendall(header + payload)


def ws_accept_key(key):
    guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    digest = hashlib.sha1((key + guid).encode()).digest()
    return base64.b64encode(digest).decode()


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
    check("dict key read", 'let p = {"x": 1, "y": 2};\np["x"] + p["y"]\n',
          stdout="= 3")
    check("dict key assignment", 'let p = {"x": 1};\np["x"] = 5;\np\n',
          stdout="{x: 5}")
    check(
        "nested dict key assignment",
        'let p = {"a": {"b": 2}};\np["a"]["b"] = 3;\np\n',
        stdout="{a: {b: 3}}",
    )
    check("dict key callable", 'let d = {"f": sqrt};\nd["f"](9)\n',
          stdout="= 3.0")
    check("dict key missing", 'let p = {"x": 1};\np["y"]\n',
          stderr="key not found")
    check("dict dot access rejected", 'let p = {"x": 1};\np.x\n',
          stderr='cannot access dict key with \'.\'')
    check("dict dot assignment rejected", 'let p = {"x": 1};\np.x = 2;\n',
          stderr='cannot access dict key with \'.\'')
    check("dict dot increment rejected", 'let p = {"x": 1};\np.x++;\n',
          stderr='cannot access dict key with \'.\'')
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
        "async defer cleanup",
        "import std;\n"
        "async func answer() {\n"
        "  defer print(\"cleanup\");\n"
        "  return 42;\n"
        "}\n"
        "await answer()\n",
        stdout_all=["cleanup", "= 42"],
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
        "generator fail from sent error",
        "import std;\n"
        "func worker() {\n"
        "  let signal = yield 1;\n"
        "  if (signal is error) { fail signal; }\n"
        "  yield 2;\n"
        "}\n"
        "let g = worker();\n"
        "std.generator.next(g);\n"
        "let v = std.generator.next(g, "
        "std.error.new(\"cancel\", \"stop\")) on error(e) {\n"
        "  print(std.error.kind(e));\n"
        "  print(str(e));\n"
        "  recover 0;\n"
        "};\n"
        "v\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "cancel",
            "stop",
            "= 0",
        ],
    )
    check(
        "generator close",
        "import std;\n"
        "func g() {\n"
        "  defer print(\"cleanup\");\n"
        "  yield 1;\n"
        "  print(\"after\");\n"
        "  yield 2;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen)\n"
        "std.generator.close(gen)\n"
        "std.generator.done(gen)\n"
        "std.generator.next(gen)\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "cleanup",
            "= true",
            "= true",
            "= {done: true, value: null}",
        ],
    )
    check(
        "generator drop warning",
        "import std;\n"
        "func g() {\n"
        "  defer print(\"cleanup\");\n"
        "  yield 1;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n"
        "gen = null;\n"
        "print(\"after drop\");\n",
        stdout="after drop",
        stderr="suspended generator dropped without close",
        code=0,
    )
    check(
        "generator close no warning",
        "import std;\n"
        "func g() {\n"
        "  defer print(\"cleanup\");\n"
        "  yield 1;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n"
        "std.generator.close(gen);\n"
        "gen = null;\n"
        "print(\"after close\");\n",
        stdout="cleanup",
        not_stderr="dropped without close",
    )
    check(
        "generator drop without defers",
        "import std;\n"
        "func g() { yield 1; }\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n"
        "gen = null;\n"
        "print(\"after\");\n",
        stdout="after",
        not_stderr="dropped without close",
    )
    check(
        "generator shutdown no warning",
        "import std;\n"
        "func g() {\n"
        "  defer print(\"cleanup\");\n"
        "  yield 1;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n",
        not_stderr="dropped without close",
    )
    check(
        "generator close defer error",
        "import std;\n"
        "func boom() { fail \"defer boom\"; }\n"
        "func g() {\n"
        "  defer boom();\n"
        "  yield 1;\n"
        "}\n"
        "let gen = g();\n"
        "std.generator.next(gen);\n"
        "let v = std.generator.close(gen) on error(e) {\n"
        "  print(std.error.kind(e));\n"
        "  print(str(e));\n"
        "  recover false;\n"
        "};\n"
        "v\n"
        "std.generator.done(gen)\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "fail",
            "defer boom",
            "= false",
            "= true",
        ],
    )
    check(
        "generator close before start",
        "import std;\n"
        "func never() {\n"
        "  print(\"started\");\n"
        "  defer print(\"never cleanup\");\n"
        "  yield 1;\n"
        "}\n"
        "let gen = never();\n"
        "std.generator.close(gen)\n"
        "std.generator.done(gen)\n",
        stdout_all=["= true", "= true"],
    )
    check(
        "generator close done",
        "import std;\n"
        "func g() { yield 1; }\n"
        "let gen = g();\n"
        "std.generator.next(gen)\n"
        "std.generator.next(gen)\n"
        "std.generator.close(gen)\n",
        stdout="= false",
    )
    check(
        "generator close running",
        "import std;\n"
        "let target = null;\n"
        "func g() {\n"
        "  std.generator.close(target);\n"
        "  yield 1;\n"
        "}\n"
        "target = g();\n"
        "std.generator.next(target);\n",
        stderr="cannot close a running generator",
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
        stderr="division by zero",
    )
    check(
        "error value type",
        "import std;\n"
        "let e = std.error.new(\"io\", \"boom\");\n"
        "kind_of(e)\n"
        "std.error.kind(e)\n"
        "std.error.message(e)\n"
        "e is error\n"
        "str(e)\n"
        "e == \"boom\"\n"
        "e == std.error.new(\"io\", \"boom\")\n",
        stdout_all=[
            "= error", "= io", "= boom", "= true",
            "= boom", "= true", "= true",
        ],
    )
    check(
        "on error runtime",
        "import std;\n"
        "let v = (1 / 0) on error(e) {\n"
        "  print(std.error.kind(e));\n"
        "  print(str(e));\n"
        "  recover 42;\n"
        "};\n"
        "v\n",
        stdout_all=["runtime", "division by zero", "= 42"],
    )
    check(
        "on error fail payload",
        "import std;\n"
        "func f() { fail \"boom\"; }\n"
        "let v = f() on error(e) {\n"
        "  print(kind_of(e));\n"
        "  print(std.error.kind(e));\n"
        "  print(str(e));\n"
        "  recover 7;\n"
        "};\n"
        "v\n",
        stdout_all=["error", "fail", "boom", "= 7"],
    )
    check(
        "capture fail",
        "import std;\n"
        "let r = std.result.capture(func() { fail \"boom\"; });\n"
        "std.result.is_err(r)\n"
        "std.error.kind(std.result.error(r))\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= fail", "= boom"],
    )
    check(
        "unwrap raises",
        "import std;\n"
        "let v = std.result.unwrap(std.result.err(\"boom\")) on error(e) {\n"
        "  recover str(e);\n"
        "};\n"
        "v\n",
        stdout="= boom",
    )
    check(
        "async fail payload",
        "import std;\n"
        "async func f() { fail \"async boom\"; }\n"
        "let r = await std.task.catch(f());\n"
        "std.result.is_err(r)\n"
        "std.error.kind(std.result.error(r))\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= fail", "= async boom"],
    )
    check(
        "generator error payload",
        "import std;\n"
        "func broken() {\n"
        "  fail \"generator boom\";\n"
        "  yield 1;\n"
        "}\n"
        "let v = std.generator.next(broken()) on error(e) {\n"
        "  print(std.error.kind(e));\n"
        "  recover str(e);\n"
        "};\n"
        "v\n",
        stdout_all=["fail", "= generator boom"],
    )
    check(
        "task error kind",
        "import std;\n"
        "let t = std.task.sleep(1000);\n"
        "std.task.cancel(t);\n"
        "let r = std.result.capture(func() { return std.task.wait(t); });\n"
        "std.error.kind(std.result.error(r))\n",
        stdout="= cancelled",
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
        "generator send value",
        "import std;\n"
        "func echo() {\n"
        "  let x = yield 1;\n"
        "  yield x + 1;\n"
        "}\n"
        "let g = echo();\n"
        "std.generator.next(g)\n"
        "std.generator.next(g, 10)\n",
        stdout_all=[
            "= {done: false, value: 1}",
            "= {done: false, value: 11}",
        ],
    )
    check(
        "generator yield null shorthand",
        "import std;\n"
        "func echo() {\n"
        "  let x = yield;\n"
        "  yield x;\n"
        "}\n"
        "let g = echo();\n"
        "std.generator.next(g)\n"
        "std.generator.next(g, 5)\n"
        "func bare() {\n"
        "  yield;\n"
        "  yield 1;\n"
        "}\n"
        "let b = bare();\n"
        "std.generator.next(b)\n"
        "std.generator.next(b)\n",
        stdout_all=[
            "= {done: false, value: null}",
            "= {done: false, value: 5}",
            "= {done: false, value: null}",
            "= {done: false, value: 1}",
        ],
    )
    check(
        "generator for-in",
        "import std;\n"
        "func count(n) {\n"
        "  for (let i in range(n)) { yield i; }\n"
        "}\n"
        "for (let x in count(3)) { print(x); }\n",
        stdout_all=["0", "1", "2"],
    )
    check(
        "std result basics",
        "import std;\n"
        "let a = std.result.ok(42);\n"
        "std.result.is_ok(a)\n"
        "std.result.unwrap(a)\n"
        "let b = std.result.err(\"boom\");\n"
        "std.result.is_err(b)\n"
        "std.result.unwrap_or(b, 7)\n"
        "std.result.error(b)\n"
        "kind_of(a)\n"
        "a is result\n",
        stdout_all=[
            "= true", "= 42", "= true", "= 7", "= boom",
            "= result", "= true",
        ],
    )
    check(
        "std result capture",
        "import std;\n"
        "let good = std.result.capture(func() { return 5; });\n"
        "std.result.unwrap(good)\n"
        "let bad = std.result.capture(func() { return 1 / 0; });\n"
        "std.result.is_err(bad)\n"
        "std.result.error(bad)\n",
        stdout_all=["= 5", "= true", "= division by zero"],
    )
    check(
        "std result capture net",
        "import std;\n"
        "let r = std.result.capture(func() {\n"
        "  return std.net.tcp_connect(\"127.0.0.1\", 1, 100);\n"
        "});\n"
        "std.result.is_err(r)\n",
        stdout="= true",
    )
    check(
        "result on error unwrap",
        "import std;\n"
        "let v = std.result.ok(42) on error(e) { recover 0; };\n"
        "v\n",
        stdout="= 42",
    )
    check(
        "result on error handler",
        "import std;\n"
        "let v = std.result.err(\"boom\") on error(e) {\n"
        "  print(std.error.kind(e));\n"
        "  print(str(e));\n"
        "  recover 7;\n"
        "};\n"
        "v\n",
        stdout_all=["error", "boom", "= 7"],
    )
    check(
        "result on error pass",
        "import std;\n"
        "func f() { return std.result.err(\"inner\"); }\n"
        "let v = (f() on error(e) { pass e; }) on error(e2) {\n"
        "  print(\"outer \" + str(e2));\n"
        "  recover 9;\n"
        "};\n"
        "v\n",
        stdout_all=["outer inner", "= 9"],
    )
    check(
        "result on error normal value",
        "import std;\n"
        "let v = 5 on error(e) { recover 0; };\n"
        "v\n",
        stdout="= 5",
    )
    check(
        "result on error nested",
        "import std;\n"
        "let v = std.result.ok(std.result.ok(1)) on error(e) {\n"
        "  recover 0;\n"
        "};\n"
        "v\n",
        stdout="= ok(1)",
    )
    check(
        "result on error recover result",
        "import std;\n"
        "let r = std.result.err(\"x\") on error(e) {\n"
        "  recover std.result.err(\"y\");\n"
        "};\n"
        "std.result.is_err(r)\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= y"],
    )
    check(
        "result on error async",
        "import std;\n"
        "async func bad() { fail \"async boom\"; }\n"
        "let v = (await std.task.catch(bad())) on error(e) {\n"
        "  print(\"async \" + str(e));\n"
        "  recover 3;\n"
        "};\n"
        "v\n",
        stdout_all=["async async boom", "= 3"],
    )
    check(
        "std result capture task",
        "import std;\n"
        "let t = std.task.sleep(1000);\n"
        "std.task.cancel(t);\n"
        "let r = std.result.capture(func() { return std.task.wait(t); });\n"
        "std.result.is_err(r)\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= task was cancelled"],
    )
    check(
        "std task catch",
        "import std;\n"
        "let t = std.task.sleep(1000);\n"
        "let c = std.task.catch(t);\n"
        "std.task.cancel(t);\n"
        "let r = std.task.wait(c);\n"
        "std.result.is_err(r)\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= task was cancelled"],
    )
    check(
        "std task try_wait",
        "import std;\n"
        "let t = std.task.sleep(10);\n"
        "let r = std.task.try_wait(t, 100);\n"
        "std.result.is_ok(r)\n"
        "let x = std.task.sleep(1000);\n"
        "std.result.is_err(std.task.try_wait(x, 10))\n",
        stdout_all=["= true", "= true"],
    )
    check(
        "async catch",
        "import std;\n"
        "async func bad() {\n"
        "  let x = 1 / 0;\n"
        "  return x;\n"
        "}\n"
        "let t = bad();\n"
        "let r = await std.task.catch(t);\n"
        "std.result.is_err(r)\n"
        "std.result.error(r)\n",
        stdout_all=["= true", "= division by zero"],
    )
    check(
        "defer order",
        "import std;\n"
        "func f() {\n"
        "  defer print(\"a\");\n"
        "  defer print(\"b\");\n"
        "  print(\"x\");\n"
        "}\n"
        "f();\n",
        stdout_all=["x", "b", "a"],
    )
    check(
        "defer block scope",
        "import std;\n"
        "func f() {\n"
        "  { defer print(\"block\"); print(\"in\"); }\n"
        "  print(\"after\");\n"
        "}\n"
        "f();\n",
        stdout_all=["in", "block", "after"],
    )
    check(
        "defer return",
        "import std;\n"
        "func f() {\n"
        "  defer print(\"cleanup\");\n"
        "  return 1;\n"
        "}\n"
        "print(f());\n",
        stdout_all=["cleanup", "1"],
    )
    check(
        "defer on error",
        "import std;\n"
        "func f() {\n"
        "  defer print(\"cleanup\");\n"
        "  let x = 1 / 0;\n"
        "}\n"
        "f();\n",
        stdout="cleanup",
        stderr="division by zero",
    )
    check(
        "defer preserves fail payload",
        "import std;\n"
        "func f() {\n"
        "  defer print(\"cleanup\");\n"
        "  fail \"boom\";\n"
        "}\n"
        "let v = f() on error(e) {\n"
        "  print(std.error.kind(e));\n"
        "  print(str(e));\n"
        "  recover 1;\n"
        "};\n"
        "v\n",
        stdout_all=["cleanup", "fail", "boom", "= 1"],
    )
    check(
        "defer error propagates",
        "import std;\n"
        "func boom() { fail \"defer boom\"; }\n"
        "func f() {\n"
        "  defer boom();\n"
        "  return 1;\n"
        "}\n"
        "let v = f() on error(e) {\n"
        "  print(\"caught \" + str(e));\n"
        "  recover 2;\n"
        "};\n"
        "v\n",
        stdout_all=["caught defer boom", "= 2"],
    )
    check(
        "defer keeps original error",
        "import std;\n"
        "func boom() { fail \"defer boom\"; }\n"
        "func f() {\n"
        "  defer boom();\n"
        "  fail \"main boom\";\n"
        "}\n"
        "let v = f() on error(e) {\n"
        "  print(\"caught \" + str(e));\n"
        "  recover 3;\n"
        "};\n"
        "v\n",
        stdout_all=["caught main boom", "= 3"],
        stderr="error in defer: defer boom",
    )
    check(
        "defer loop break",
        "import std;\n"
        "for (let i in range(5)) {\n"
        "  defer print(\"done \" + str(i));\n"
        "  if (i == 1) { break; }\n"
        "  print(\"body \" + str(i));\n"
        "}\n",
        stdout_all=["body 0", "done 0", "done 1"],
    )
    check(
        "defer root rejected",
        "import std;\n"
        "defer print(\"root\");\n"
        "print(\"body\");\n",
        stdout="body",
        stderr="defer is only allowed inside a function or block",
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

        stream_server = socket.socket()
        stream_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        stream_server.bind(("127.0.0.1", 0))
        stream_server.listen(1)
        stream_server.settimeout(5)
        stream_port = stream_server.getsockname()[1]
        stream_received = []

        def stream_server_fn():
            try:
                client, _ = stream_server.accept()
                data = client.recv(100)
                stream_received.append(data)
                client.sendall(b"stream:" + data)
                client.close()
            finally:
                stream_server.close()

        stream_thread = threading.Thread(target=stream_server_fn)
        stream_thread.start()
        stream_proc = run(
            "import std;\n"
            f'let s = std.net.tcp_connect("127.0.0.1", {stream_port}, '
            '1000, "ipv4");\n'
            'std.io.write_all(s, "hello", 1000);\n'
            "print(bytes_to_string(std.io.read(s, 100, 1000)));\n"
            "std.io.close(s);\n"
        )
        stream_thread.join(timeout=5)
        assert stream_proc.returncode == 0, stream_proc.stderr
        assert "stream:hello" in stream_proc.stdout, stream_proc.stdout
        assert stream_received == [b"hello"], stream_received

        async_stream_server = socket.socket()
        async_stream_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        async_stream_server.bind(("127.0.0.1", 0))
        async_stream_server.listen(1)
        async_stream_server.settimeout(5)
        async_stream_port = async_stream_server.getsockname()[1]
        async_stream_received = []

        def async_stream_server_fn():
            try:
                client, _ = async_stream_server.accept()
                data = client.recv(100)
                async_stream_received.append(data)
                client.sendall(b"async-stream:" + data)
                client.close()
            finally:
                async_stream_server.close()

        async_stream_thread = threading.Thread(target=async_stream_server_fn)
        async_stream_thread.start()
        async_stream_proc = run(
            "import std;\n"
            f'let s = std.net.tcp_connect("127.0.0.1", '
            f'{async_stream_port}, 1000, "ipv4");\n'
            'await std.io.write_async(s, "hello", 1000)\n'
            "let data = await std.io.read_async(s, 100, 1000);\n"
            "print(bytes_to_string(data));\n"
            "std.io.close(s);\n"
        )
        async_stream_thread.join(timeout=5)
        assert async_stream_proc.returncode == 0, async_stream_proc.stderr
        assert "async-stream:hello" in async_stream_proc.stdout, (
            async_stream_proc.stdout)
        assert async_stream_received == [b"hello"], async_stream_received
        test_file = os.path.abspath(__file__).replace("\\", "\\\\")
        test_file = test_file.replace('"', '\\"')
        check(
            "io buffered async",
            'import std;\n'
            f'let f = std.fs.open("{test_file}", "rb");\n'
            'let b = std.io.buffered(f, 4);\n'
            'len(await std.io.read_async(b, 2)) == 2\n'
            'len(await std.io.read_async(b, 100)) > 0\n'
            'std.io.close(b)\n',
            stdout_all=["= true", "= true", "= true"],
        )

        http_test_server = socket.socket()
        http_test_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        http_test_server.bind(("127.0.0.1", 0))
        http_test_server.listen(1)
        http_test_server.settimeout(5)
        http_test_port = http_test_server.getsockname()[1]

        def http_test_server_fn():
            try:
                client, _ = http_test_server.accept()
                data = b""
                while b"\r\n\r\n" not in data:
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                client.sendall(
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Length: 5\r\n"
                    b"X-Test: yes\r\n"
                    b"\r\n"
                    b"hello")
                client.close()
            finally:
                http_test_server.close()

        http_test_thread = threading.Thread(target=http_test_server_fn)
        http_test_thread.start()
        http_client_proc = run(
            "import std;\n"
            f'let r = std.http.get("http://127.0.0.1:{http_test_port}/");\n'
            'print(r["status"]);\n'
            'print(r["headers"]["x-test"]);\n'
            'print(bytes_to_string(r["body"]));\n'
        )
        http_test_thread.join(timeout=5)
        assert http_client_proc.returncode == 0, http_client_proc.stderr
        assert "200" in http_client_proc.stdout, http_client_proc.stdout
        assert "yes" in http_client_proc.stdout, http_client_proc.stdout
        assert "hello" in http_client_proc.stdout, http_client_proc.stdout

        keepalive_server = socket.socket()
        keepalive_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        keepalive_server.bind(("127.0.0.1", 0))
        keepalive_server.listen(1)
        keepalive_server.settimeout(5)
        keepalive_port = keepalive_server.getsockname()[1]

        def keepalive_server_fn():
            try:
                client, _ = keepalive_server.accept()
                for body in (b"one", b"two"):
                    request = b""
                    while b"\r\n\r\n" not in request:
                        chunk = client.recv(4096)
                        if not chunk:
                            return
                        request += chunk
                    connection = (
                        b"keep-alive" if body == b"one" else b"close")
                    client.sendall(
                        b"HTTP/1.1 200 OK\r\n"
                        b"Content-Length: 3\r\n"
                        b"Connection: " + connection + b"\r\n"
                        b"\r\n" + body)
                client.close()
            finally:
                keepalive_server.close()

        keepalive_thread = threading.Thread(target=keepalive_server_fn)
        keepalive_thread.start()
        keepalive_proc = run(
            "import std;\n"
            f'let a = std.http.get("http://127.0.0.1:{keepalive_port}/a");\n'
            f'let b = std.http.get("http://127.0.0.1:{keepalive_port}/b");\n'
            'print(bytes_to_string(a["body"]));\n'
            'print(bytes_to_string(b["body"]));\n'
        )
        keepalive_thread.join(timeout=5)
        assert keepalive_proc.returncode == 0, keepalive_proc.stderr
        assert "one" in keepalive_proc.stdout, keepalive_proc.stdout
        assert "two" in keepalive_proc.stdout, keepalive_proc.stdout

        async_keepalive_server = socket.socket()
        async_keepalive_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        async_keepalive_server.bind(("127.0.0.1", 0))
        async_keepalive_server.listen(1)
        async_keepalive_server.settimeout(5)
        async_keepalive_port = async_keepalive_server.getsockname()[1]

        def async_keepalive_server_fn():
            try:
                client, _ = async_keepalive_server.accept()
                for body in (b"one", b"two"):
                    request = b""
                    while b"\r\n\r\n" not in request:
                        chunk = client.recv(4096)
                        if not chunk:
                            return
                        request += chunk
                    connection = (
                        b"keep-alive" if body == b"one" else b"close")
                    client.sendall(
                        b"HTTP/1.1 200 OK\r\n"
                        b"Content-Length: 3\r\n"
                        b"Connection: " + connection + b"\r\n"
                        b"\r\n" + body)
                client.close()
            finally:
                async_keepalive_server.close()

        async_keepalive_thread = threading.Thread(
            target=async_keepalive_server_fn)
        async_keepalive_thread.start()
        async_keepalive_proc = run(
            "import std;\n"
            f'let a = await std.http.get_async('
            f'"http://127.0.0.1:{async_keepalive_port}/a");\n'
            f'let b = await std.http.get_async('
            f'"http://127.0.0.1:{async_keepalive_port}/b");\n'
            'print(bytes_to_string(a["body"]));\n'
            'print(bytes_to_string(b["body"]));\n'
        )
        async_keepalive_thread.join(timeout=5)
        assert async_keepalive_proc.returncode == 0, (
            async_keepalive_proc.stderr)
        assert "one" in async_keepalive_proc.stdout, (
            async_keepalive_proc.stdout)
        assert "two" in async_keepalive_proc.stdout, (
            async_keepalive_proc.stdout)

        def stale_server(port, bodies):
            try:
                for body in bodies:
                    client, _ = stale_server.socket.accept()
                    request = b""
                    while b"\r\n\r\n" not in request:
                        chunk = client.recv(4096)
                        if not chunk:
                            break
                        request += chunk
                    client.sendall(
                        b"HTTP/1.1 200 OK\r\n"
                        b"Content-Length: 3\r\n"
                        b"Connection: keep-alive\r\n"
                        b"\r\n" + body)
                    client.close()
            finally:
                stale_server.socket.close()

        stale_sync_server = socket.socket()
        stale_sync_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        stale_sync_server.bind(("127.0.0.1", 0))
        stale_sync_server.listen(2)
        stale_sync_server.settimeout(5)
        stale_sync_port = stale_sync_server.getsockname()[1]
        stale_server.socket = stale_sync_server
        stale_sync_thread = threading.Thread(
            target=stale_server, args=(stale_sync_port, (b"one", b"two")))
        stale_sync_thread.start()
        stale_sync_proc = run(
            "import std;\n"
            f'let a = std.http.get("http://127.0.0.1:{stale_sync_port}/a");\n'
            f'let b = std.http.get("http://127.0.0.1:{stale_sync_port}/b");\n'
            'print(bytes_to_string(a["body"]));\n'
            'print(bytes_to_string(b["body"]));\n'
        )
        stale_sync_thread.join(timeout=5)
        assert stale_sync_proc.returncode == 0, stale_sync_proc.stderr
        assert "one" in stale_sync_proc.stdout, stale_sync_proc.stdout
        assert "two" in stale_sync_proc.stdout, stale_sync_proc.stdout

        stale_async_server = socket.socket()
        stale_async_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        stale_async_server.bind(("127.0.0.1", 0))
        stale_async_server.listen(2)
        stale_async_server.settimeout(5)
        stale_async_port = stale_async_server.getsockname()[1]
        stale_server.socket = stale_async_server
        stale_async_thread = threading.Thread(
            target=stale_server, args=(stale_async_port, (b"one", b"two")))
        stale_async_thread.start()
        stale_async_proc = run(
            "import std;\n"
            f'let a = await std.http.get_async('
            f'"http://127.0.0.1:{stale_async_port}/a");\n'
            f'let b = await std.http.get_async('
            f'"http://127.0.0.1:{stale_async_port}/b");\n'
            'print(bytes_to_string(a["body"]));\n'
            'print(bytes_to_string(b["body"]));\n'
        )
        stale_async_thread.join(timeout=5)
        assert stale_async_proc.returncode == 0, stale_async_proc.stderr
        assert "one" in stale_async_proc.stdout, stale_async_proc.stdout
        assert "two" in stale_async_proc.stdout, stale_async_proc.stdout

        stream_server = socket.socket()
        stream_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        stream_server.bind(("127.0.0.1", 0))
        stream_server.listen(2)
        stream_server.settimeout(5)
        stream_port = stream_server.getsockname()[1]

        def stream_server_fn():
            try:
                for _ in range(2):
                    client, _ = stream_server.accept()
                    request = b""
                    while b"\r\n\r\n" not in request:
                        chunk = client.recv(4096)
                        if not chunk:
                            return
                        request += chunk
                    client.sendall(
                        b"HTTP/1.1 200 OK\r\n"
                        b"Transfer-Encoding: chunked\r\n"
                        b"Connection: close\r\n\r\n")
                    for part in (b"alpha", b"-", b"beta"):
                        client.sendall(
                            ("%x\r\n" % len(part)).encode()
                            + part + b"\r\n")
                    client.sendall(b"0\r\n\r\n")
                    client.close()
            finally:
                stream_server.close()

        stream_thread = threading.Thread(target=stream_server_fn)
        stream_thread.start()
        stream_client_proc = run(
            "import std;\n"
            f'let a = std.http.get('
            f'"http://127.0.0.1:{stream_port}/a", '
            '{"on_body": func(chunk) {\n'
            '  print("s=" + bytes_to_string(chunk));\n'
            "}});\n"
            'print("sb=" + bytes_to_string(a["body"]));\n'
            f'let b = await std.http.get_async('
            f'"http://127.0.0.1:{stream_port}/b", '
            '{"on_body": func(chunk) {\n'
            '  print("a=" + bytes_to_string(chunk));\n'
            "}});\n"
            'print("ab=" + bytes_to_string(b["body"]));\n',
            timeout=15,
        )
        stream_thread.join(timeout=5)
        assert stream_client_proc.returncode == 0, stream_client_proc.stderr
        stream_lines = stream_client_proc.stdout.splitlines()
        sync_streamed = "".join(line[2:] for line in stream_lines
                               if line.startswith("s="))
        async_streamed = "".join(line[2:] for line in stream_lines
                                if line.startswith("a="))
        assert sync_streamed == "alpha-beta", stream_client_proc.stdout
        assert async_streamed == "alpha-beta", stream_client_proc.stdout
        assert "sb=" in stream_lines, stream_client_proc.stdout
        assert "ab=" in stream_lines, stream_client_proc.stdout

        request_stream_server = socket.socket()
        request_stream_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        request_stream_server.bind(("127.0.0.1", 0))
        request_stream_server.listen(2)
        request_stream_server.settimeout(5)
        request_stream_port = request_stream_server.getsockname()[1]

        def request_stream_server_fn():
            try:
                for _ in range(2):
                    client, _ = request_stream_server.accept()
                    data = b""
                    while b"\r\n\r\n" not in data:
                        chunk = client.recv(4096)
                        if not chunk:
                            return
                        data += chunk
                    head, rest = data.split(b"\r\n\r\n", 1)
                    ok = b"transfer-encoding: chunked" in head.lower()
                    body = b""
                    while True:
                        while b"\r\n" not in rest:
                            rest += client.recv(4096)
                        line, rest = rest.split(b"\r\n", 1)
                        size = int(line, 16)
                        if size == 0:
                            break
                        while len(rest) < size + 2:
                            rest += client.recv(4096)
                        body += rest[:size]
                        rest = rest[size + 2:]
                    prefix = b"ok:" if ok else b"bad:"
                    response = prefix + body
                    client.sendall(
                        b"HTTP/1.1 200 OK\r\n"
                        b"Content-Length: " +
                        str(len(response)).encode() +
                        b"\r\nConnection: close\r\n\r\n" +
                        response)
                    client.close()
            finally:
                request_stream_server.close()

        request_stream_thread = threading.Thread(
            target=request_stream_server_fn)
        request_stream_thread.start()
        request_stream_client = run(
            "import std;\n"
            "func parts() {\n"
            '  yield "hello";\n'
            '  yield " ";\n'
            '  yield "world";\n'
            "}\n"
            "func async_parts() {\n"
            '  yield "async-";\n'
            '  yield "stream";\n'
            "}\n"
            f'let a = std.http.post('
            f'"http://127.0.0.1:{request_stream_port}/a", '
            '{"body_stream": parts()});\n'
            'print(bytes_to_string(a["body"]));\n'
            f'let b = await std.http.post_async('
            f'"http://127.0.0.1:{request_stream_port}/b", '
            '{"body_stream": async_parts()});\n'
            'print(bytes_to_string(b["body"]));\n',
            timeout=15,
        )
        request_stream_thread.join(timeout=5)
        assert request_stream_client.returncode == 0, (
            request_stream_client.stderr)
        assert "ok:hello world" in request_stream_client.stdout, (
            request_stream_client.stdout)
        assert "ok:async-stream" in request_stream_client.stdout, (
            request_stream_client.stdout)

        serve_stream_probe = socket.socket()
        serve_stream_probe.bind(("127.0.0.1", 0))
        serve_stream_port = serve_stream_probe.getsockname()[1]
        serve_stream_probe.close()
        serve_stream_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        try:
            serve_stream_proc.stdin.write(
                "import std;\n"
                "func parts() {\n"
                '  yield "serve-";\n'
                '  yield "stream";\n'
                "}\n"
                f'let s = std.http.listen("127.0.0.1", '
                f"{serve_stream_port});\n"
                "let n = std.http.serve(s, func(req) {\n"
                '  return {"status": 200, "headers": '
                '{"Content-Type": "text/plain"}, '
                '"body_stream": parts()};\n'
                "}, 2);\n"
                'print("served " + str(n));\n'
                "std.http.close(s);\n"
            )
            serve_stream_proc.stdin.close()
            deadline = time.time() + 5
            ready = False
            while time.time() < deadline:
                try:
                    probe = socket.create_connection(
                        ("127.0.0.1", serve_stream_port), timeout=0.2)
                    probe.close()
                    ready = True
                    break
                except OSError:
                    time.sleep(0.05)
            assert ready, "http serve stream did not listen"
            serve_stream_client = run(
                "import std;\n"
                f'let a = std.http.get('
                f'"http://127.0.0.1:{serve_stream_port}/a");\n'
                'print(a["headers"]["transfer-encoding"]);\n'
                'print(bytes_to_string(a["body"]));\n'
                f'let b = await std.http.get_async('
                f'"http://127.0.0.1:{serve_stream_port}/b");\n'
                'print(bytes_to_string(b["body"]));\n',
                timeout=15,
            )
            assert serve_stream_client.returncode == 0, (
                serve_stream_client.stderr)
            assert "chunked" in serve_stream_client.stdout, (
                serve_stream_client.stdout)
            assert serve_stream_client.stdout.count("serve-stream") == 2, (
                serve_stream_client.stdout)
            serve_stream_proc.wait(timeout=10)
        finally:
            if serve_stream_proc.poll() is None:
                serve_stream_proc.terminate()
                serve_stream_proc.wait(timeout=5)

        respond_stream_probe = socket.socket()
        respond_stream_probe.bind(("127.0.0.1", 0))
        respond_stream_port = respond_stream_probe.getsockname()[1]
        respond_stream_probe.close()
        respond_stream_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        try:
            respond_stream_proc.stdin.write(
                "import std;\n"
                "func parts() {\n"
                '  yield "respond-";\n'
                '  yield "stream";\n'
                "}\n"
                f'let s = std.http.listen("127.0.0.1", '
                f"{respond_stream_port});\n"
                "let req = std.http.accept(s, 5000);\n"
                "std.http.respond(req, 200, {}, parts());\n"
                "std.http.close(s);\n"
            )
            respond_stream_proc.stdin.close()
            time.sleep(0.5)
            respond_stream_client = run(
                "import std;\n"
                f'let a = std.http.get('
                f'"http://127.0.0.1:{respond_stream_port}/a");\n'
                'print(a["headers"]["transfer-encoding"]);\n'
                'print(bytes_to_string(a["body"]));\n',
                timeout=15,
            )
            assert respond_stream_client.returncode == 0, (
                respond_stream_client.stderr)
            assert "chunked" in respond_stream_client.stdout, (
                respond_stream_client.stdout)
            assert "respond-stream" in respond_stream_client.stdout, (
                respond_stream_client.stdout)
            respond_stream_proc.wait(timeout=10)
        finally:
            if respond_stream_proc.poll() is None:
                respond_stream_proc.terminate()
                respond_stream_proc.wait(timeout=5)

        http_async_server = socket.socket()
        http_async_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        http_async_server.bind(("127.0.0.1", 0))
        http_async_server.listen(1)
        http_async_server.settimeout(5)
        http_async_port = http_async_server.getsockname()[1]

        def http_async_server_fn():
            try:
                client, _ = http_async_server.accept()
                data = b""
                while b"\r\n\r\n" not in data:
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                client.sendall(
                    b"HTTP/1.1 201 Created\r\n"
                    b"Content-Length: 5\r\n"
                    b"\r\n"
                    b"async")
                client.close()
            finally:
                http_async_server.close()

        http_async_thread = threading.Thread(target=http_async_server_fn)
        http_async_thread.start()
        http_async_proc = run(
            "import std;\n"
            f'let t = std.http.get_async('
            f'"http://127.0.0.1:{http_async_port}/");\n'
            'let r = await t;\n'
            'print(r["status"]);\n'
            'print(bytes_to_string(r["body"]));\n'
        )
        http_async_thread.join(timeout=5)
        assert http_async_proc.returncode == 0, http_async_proc.stderr
        assert "201" in http_async_proc.stdout, http_async_proc.stdout
        assert "async" in http_async_proc.stdout, http_async_proc.stdout

        http_probe = socket.socket()
        http_probe.bind(("127.0.0.1", 0))
        http_port = http_probe.getsockname()[1]
        http_probe.close()
        http_server_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        http_client = None
        try:
            http_server_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", {http_port});\n'
                "let req = std.http.accept(s, 10000);\n"
                'std.http.respond(req, 200, {"X-Test": "yes"}, '
                '"hello " + req["path"]);\n'
                "std.http.close(s);\n"
            )
            http_server_proc.stdin.close()
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    http_client = socket.create_connection(
                        ("127.0.0.1", http_port), timeout=0.2)
                    break
                except OSError:
                    time.sleep(0.05)
            assert http_client is not None, "http server did not listen"
            http_client.sendall(
                b"GET /world HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\n"
                b"Connection: close\r\n"
                b"\r\n")
            response = b""
            while True:
                chunk = http_client.recv(4096)
                if not chunk:
                    break
                response += chunk
            text = response.decode("utf-8", errors="replace")
            assert "200 OK" in text, text
            assert "hello /world" in text, text
            assert "x-test" in text.lower(), text
        finally:
            if http_client:
                http_client.close()
            http_server_proc.wait(timeout=10)

        http_async_probe = socket.socket()
        http_async_probe.bind(("127.0.0.1", 0))
        http_async_port = http_async_probe.getsockname()[1]
        http_async_probe.close()
        http_async_server_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        http_async_client = None
        try:
            http_async_server_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", {http_async_port});\n'
                "let req = await std.http.accept_async(s, 10000);\n"
                'await std.http.respond_async(req, 200, '
                '{"X-Async": "yes"}, "async " + req["path"]);\n'
                "std.http.close(s);\n"
            )
            http_async_server_proc.stdin.close()
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    http_async_client = socket.create_connection(
                        ("127.0.0.1", http_async_port), timeout=0.2)
                    break
                except OSError:
                    time.sleep(0.05)
            assert http_async_client is not None, (
                "async http server did not listen")
            http_async_client.sendall(
                b"GET /async-world HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\n"
                b"Connection: close\r\n"
                b"\r\n")
            async_response = b""
            while True:
                chunk = http_async_client.recv(4096)
                if not chunk:
                    break
                async_response += chunk
            async_text = async_response.decode("utf-8", errors="replace")
            assert "200 OK" in async_text, async_text
            assert "async /async-world" in async_text, async_text
            assert "x-async" in async_text.lower(), async_text
        finally:
            if http_async_client:
                http_async_client.close()
            http_async_server_proc.wait(timeout=10)

        http2_probe = run('import std;\nstd.dict.has(std, "http2")\n')
        http2_enabled = "= true" in http2_probe.stdout
        if http2_enabled:
            http2_probe_sock = socket.socket()
            http2_probe_sock.bind(("127.0.0.1", 0))
            http2_port = http2_probe_sock.getsockname()[1]
            http2_probe_sock.close()
            http2_server_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_server_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  let body = bytes_to_string(req["body"]);\n'
                    "  return {\n"
                    '    "status": 200,\n'
                    '    "headers": {"Content-Type": "text/plain", '
                    '"X-Path": req["path"]},\n'
                    '    "body": "h2:" + req["method"] + ":" + '
                    'req["path"] + ":" + body + "|cid=" + '
                    'str(req["connection_id"])\n'
                    "  };\n"
                    "}, 3);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_server_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 server did not listen"
                http2_client_proc = run(
                    "import std;\n"
                    f'let a = std.http2.get('
                    f'"http://127.0.0.1:{http2_port}/hello");\n'
                    'print(str(a["status"]) + " " + a["status_text"] '
                    '+ " " + a["protocol"]);\n'
                    'print(bytes_to_string(a["body"]));\n'
                    'print(a["headers"]["x-path"]);\n'
                    f'let b = std.http2.post('
                    f'"http://127.0.0.1:{http2_port}/post", "payload");\n'
                    'print(bytes_to_string(b["body"]));\n'
                    'let c = await std.http2.get_async('
                    f'"http://127.0.0.1:{http2_port}/async");\n'
                    'print(bytes_to_string(c["body"]));\n',
                    timeout=15,
                )
                assert http2_client_proc.returncode == 0, (
                    http2_client_proc.stderr)
                assert "200 OK h2" in http2_client_proc.stdout, (
                    http2_client_proc.stdout)
                assert "h2:GET:/hello:" in http2_client_proc.stdout, (
                    http2_client_proc.stdout)
                assert "/hello" in http2_client_proc.stdout, (
                    http2_client_proc.stdout)
                assert "h2:POST:/post:payload" in http2_client_proc.stdout, (
                    http2_client_proc.stdout)
                assert "h2:GET:/async:" in http2_client_proc.stdout, (
                    http2_client_proc.stdout)
                http2_server_proc.wait(timeout=10)
                http2_output = http2_server_proc.stdout.read()
                assert "served 3" in http2_output, http2_output
                cids = [line.rsplit("cid=", 1)[1]
                        for line in http2_client_proc.stdout.splitlines()
                        if "cid=" in line]
                assert len(cids) == 3, http2_client_proc.stdout
                assert len(set(cids)) == 1, (
                    "http2 client did not reuse its connection: "
                    + http2_client_proc.stdout)
            finally:
                if http2_server_proc.poll() is None:
                    http2_server_proc.terminate()
                    http2_server_proc.wait(timeout=5)

            http2_goaway_probe = socket.socket()
            http2_goaway_probe.bind(("127.0.0.1", 0))
            http2_goaway_port = http2_goaway_probe.getsockname()[1]
            http2_goaway_probe.close()
            http2_goaway_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_goaway_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_goaway_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "cid=" + '
                    'str(req["connection_id"])};\n'
                    "}, 2, 1, 0);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_goaway_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_goaway_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 goaway server did not listen"
                http2_goaway_client = run(
                    "import std;\n"
                    f'let a = std.http2.get('
                    f'"http://127.0.0.1:{http2_goaway_port}/a");\n'
                    f'let b = std.http2.get('
                    f'"http://127.0.0.1:{http2_goaway_port}/b");\n'
                    'print(bytes_to_string(a["body"]));\n'
                    'print(bytes_to_string(b["body"]));\n',
                    timeout=15,
                )
                assert http2_goaway_client.returncode == 0, (
                    http2_goaway_client.stderr)
                goaway_cids = [line.rsplit("cid=", 1)[1]
                               for line in
                               http2_goaway_client.stdout.splitlines()
                               if "cid=" in line]
                assert len(goaway_cids) == 2, http2_goaway_client.stdout
                assert goaway_cids[0] != goaway_cids[1], (
                    "http2 client reused a GOAWAY connection: "
                    + http2_goaway_client.stdout)
                http2_goaway_proc.wait(timeout=10)
                goaway_output = http2_goaway_proc.stdout.read()
                assert "served 2" in goaway_output, goaway_output
            finally:
                if http2_goaway_proc.poll() is None:
                    http2_goaway_proc.terminate()
                    http2_goaway_proc.wait(timeout=5)

            http2_stream_probe = socket.socket()
            http2_stream_probe.bind(("127.0.0.1", 0))
            http2_stream_port = http2_stream_probe.getsockname()[1]
            http2_stream_probe.close()
            http2_stream_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_stream_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_stream_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "echo:" + '
                    'bytes_to_string(req["body"])};\n'
                    "}, 2);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_stream_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_stream_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 stream server did not listen"
                http2_stream_client = run(
                    "import std;\n"
                    "func parts() {\n"
                    '  yield "h2-";\n'
                    '  yield "body";\n'
                    "}\n"
                    "func async_parts() {\n"
                    '  yield "h2-async";\n'
                    "}\n"
                    f'let a = std.http2.post('
                    f'"http://127.0.0.1:{http2_stream_port}/a", '
                    '{"body_stream": parts(), "on_body": func(chunk) {\n'
                    '  print("s=" + bytes_to_string(chunk));\n'
                    "}});\n"
                    'print("sb=" + bytes_to_string(a["body"]));\n'
                    f'let b = await std.http2.post_async('
                    f'"http://127.0.0.1:{http2_stream_port}/b", '
                    '{"body_stream": async_parts(), '
                    '"on_body": func(chunk) {\n'
                    '  print("a=" + bytes_to_string(chunk));\n'
                    "}});\n"
                    'print("ab=" + bytes_to_string(b["body"]));\n',
                    timeout=15,
                )
                assert http2_stream_client.returncode == 0, (
                    http2_stream_client.stderr)
                stream_lines = http2_stream_client.stdout.splitlines()
                sync_streamed = "".join(line[2:] for line in stream_lines
                                       if line.startswith("s="))
                async_streamed = "".join(line[2:] for line in stream_lines
                                        if line.startswith("a="))
                assert sync_streamed == "echo:h2-body", (
                    http2_stream_client.stdout)
                assert async_streamed == "echo:h2-async", (
                    http2_stream_client.stdout)
                assert "sb=" in stream_lines, http2_stream_client.stdout
                assert "ab=" in stream_lines, http2_stream_client.stdout
                http2_stream_proc.wait(timeout=10)
                stream_output = http2_stream_proc.stdout.read()
                assert "served 2" in stream_output, stream_output
            finally:
                if http2_stream_proc.poll() is None:
                    http2_stream_proc.terminate()
                    http2_stream_proc.wait(timeout=5)

            http2_send_probe = socket.socket()
            http2_send_probe.bind(("127.0.0.1", 0))
            http2_send_port = http2_send_probe.getsockname()[1]
            http2_send_probe.close()
            http2_send_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_send_proc.stdin.write(
                    "import std;\n"
                    "func parts() {\n"
                    '  yield "h2-";\n'
                    '  yield "server";\n'
                    '  yield "-stream";\n'
                    "}\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_send_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body_stream": parts()};\n'
                    "}, 2);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_send_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_send_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 send stream server did not listen"
                http2_send_client = run(
                    "import std;\n"
                    f'let a = std.http2.get('
                    f'"http://127.0.0.1:{http2_send_port}/a");\n'
                    'print(bytes_to_string(a["body"]));\n'
                    f'let b = await std.http2.get_async('
                    f'"http://127.0.0.1:{http2_send_port}/b");\n'
                    'print(bytes_to_string(b["body"]));\n',
                    timeout=15,
                )
                assert http2_send_client.returncode == 0, (
                    http2_send_client.stderr)
                assert http2_send_client.stdout.count(
                    "h2-server-stream") == 2, http2_send_client.stdout
                http2_send_proc.wait(timeout=10)
                send_output = http2_send_proc.stdout.read()
                assert "served 2" in send_output, send_output
            finally:
                if http2_send_proc.poll() is None:
                    http2_send_proc.terminate()
                    http2_send_proc.wait(timeout=5)

            http2_many_probe = socket.socket()
            http2_many_probe.bind(("127.0.0.1", 0))
            http2_many_port = http2_many_probe.getsockname()[1]
            http2_many_probe.close()
            http2_many_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_many_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_many_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "cid=" + '
                    'str(req["connection_id"]) + " " + req["method"] + '
                    '" " + req["path"] + " " + '
                    'bytes_to_string(req["body"])};\n'
                    "}, 6);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_many_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_many_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 many server did not listen"
                http2_many_client = run(
                    "import std;\n"
                    "let rs = std.http2.request_many([\n"
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many_port}/a"}},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many_port}/b", '
                    '"method": "POST", "body": "payload"},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many_port}/c", '
                    '"method": "PUT", "body": "x"}\n'
                    "]);\n"
                    'print(bytes_to_string(rs[0]["body"]));\n'
                    'print(bytes_to_string(rs[1]["body"]));\n'
                    'print(bytes_to_string(rs[2]["body"]));\n'
                    "let rs2 = await std.http2.request_many_async([\n"
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many_port}/d"}},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many_port}/e", '
                    '"method": "DELETE"},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many_port}/f", '
                    '"method": "POST", "body": "async"}\n'
                    "]);\n"
                    'print(bytes_to_string(rs2[0]["body"]));\n'
                    'print(bytes_to_string(rs2[1]["body"]));\n'
                    'print(bytes_to_string(rs2[2]["body"]));\n',
                    timeout=15,
                )
                assert http2_many_client.returncode == 0, (
                    http2_many_client.stderr)
                many_lines = http2_many_client.stdout.splitlines()
                assert len(many_lines) == 6, http2_many_client.stdout
                many_cids = [line.split()[0] for line in many_lines]
                assert len(set(many_cids)) == 1, http2_many_client.stdout
                assert "GET /a" in many_lines[0], http2_many_client.stdout
                assert "POST /b payload" in many_lines[1], (
                    http2_many_client.stdout)
                assert "PUT /c x" in many_lines[2], http2_many_client.stdout
                assert "GET /d" in many_lines[3], http2_many_client.stdout
                assert "DELETE /e" in many_lines[4], (
                    http2_many_client.stdout)
                assert "POST /f async" in many_lines[5], (
                    http2_many_client.stdout)
                http2_many_proc.wait(timeout=10)
                many_output = http2_many_proc.stdout.read()
                assert "served 6" in many_output, many_output
            finally:
                if http2_many_proc.poll() is None:
                    http2_many_proc.terminate()
                    http2_many_proc.wait(timeout=5)

            http2_ms_probe = socket.socket()
            http2_ms_probe.bind(("127.0.0.1", 0))
            http2_ms_port = http2_ms_probe.getsockname()[1]
            http2_ms_probe.close()
            http2_ms_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_ms_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_ms_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "cid=" + '
                    'str(req["connection_id"]) + " echo:" + '
                    'bytes_to_string(req["body"])};\n'
                    "}, 2);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_ms_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_ms_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 many stream server did not listen"
                http2_ms_client = run(
                    "import std;\n"
                    "func p1() { yield \"one\"; }\n"
                    "func p2() { yield \"two\"; }\n"
                    "let rs = std.http2.request_many([\n"
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_ms_port}/a", '
                    '"method": "POST", "body_stream": p1(), '
                    '"on_body": func(c) {\n'
                    '    print("s1=" + bytes_to_string(c));\n'
                    "  }},\n"
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_ms_port}/b", '
                    '"method": "POST", "body_stream": p2(), '
                    '"on_body": func(c) {\n'
                    '    print("s2=" + bytes_to_string(c));\n'
                    "  }}\n"
                    "]);\n"
                    'print("r1=" + bytes_to_string(rs[0]["body"]));\n'
                    'print("r2=" + bytes_to_string(rs[1]["body"]));\n',
                    timeout=15,
                )
                assert http2_ms_client.returncode == 0, (
                    http2_ms_client.stderr)
                ms_lines = http2_ms_client.stdout.splitlines()
                ms1 = "".join(line[3:] for line in ms_lines
                              if line.startswith("s1="))
                ms2 = "".join(line[3:] for line in ms_lines
                              if line.startswith("s2="))
                assert ms1 == "cid=1 echo:one", http2_ms_client.stdout
                assert ms2 == "cid=1 echo:two", http2_ms_client.stdout
                assert "r1=" in ms_lines, http2_ms_client.stdout
                assert "r2=" in ms_lines, http2_ms_client.stdout
                http2_ms_proc.wait(timeout=10)
                ms_output = http2_ms_proc.stdout.read()
                assert "served 2" in ms_output, ms_output
            finally:
                if http2_ms_proc.poll() is None:
                    http2_ms_proc.terminate()
                    http2_ms_proc.wait(timeout=5)

            http2_set_probe = socket.socket()
            http2_set_probe.bind(("127.0.0.1", 0))
            http2_set_port = http2_set_probe.getsockname()[1]
            http2_set_probe.close()
            http2_set_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_set_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_set_port});\n"
                    's["settings"] = {"initial_window_size": 16384, '
                    '"max_concurrent_streams": 1};\n'
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "len=" + '
                    'str(bytes_len(req["body"]))};\n'
                    "}, 2);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_set_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_set_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 settings server did not listen"
                http2_set_client = run(
                    "import std;\n"
                    "func big() {\n"
                    '  let chunk = "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789";\n'
                    "  for (let i = 0; i < 1000; i = i + 1) {\n"
                    "    yield chunk;\n"
                    "  }\n"
                    "}\n"
                    "let rs = std.http2.request_many([\n"
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_set_port}/a", '
                    '"method": "POST", "body_stream": big()},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_set_port}/b", '
                    '"method": "POST", "body_stream": big()}\n'
                    '], {"settings": {"initial_window_size": 16384}});\n'
                    'print(bytes_to_string(rs[0]["body"]));\n'
                    'print(bytes_to_string(rs[1]["body"]));\n',
                    timeout=30,
                )
                assert http2_set_client.returncode == 0, (
                    http2_set_client.stderr)
                assert http2_set_client.stdout.splitlines() == [
                    "len=100000", "len=100000"], http2_set_client.stdout
                http2_set_proc.wait(timeout=10)
                set_output = http2_set_proc.stdout.read()
                assert "served 2" in set_output, set_output
            finally:
                if http2_set_proc.poll() is None:
                    http2_set_proc.terminate()
                    http2_set_proc.wait(timeout=5)

            bad_settings = run(
                "import std;\n"
                'std.http2.get("http://127.0.0.1:1/", '
                '{"settings": {"max_frame_size": 100}});\n'
            )
            assert "max_frame_size must be between" in bad_settings.stderr, (
                bad_settings.stderr)
            bad_server_settings = run(
                "import std;\n"
                'let s = std.http2.listen("127.0.0.1", 0);\n'
                's["settings"] = {"enable_push": true};\n'
                "std.http2.serve(s, func(req) { "
                'return {"status": 200, "body": "x"}; }, 1);\n'
            )
            assert "enable_push is only valid for clients" in (
                bad_server_settings.stderr), bad_server_settings.stderr

            rb2_probe = socket.socket()
            rb2_probe.bind(("127.0.0.1", 0))
            rb2_port = rb2_probe.getsockname()[1]
            rb2_probe.close()
            rb2_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                rb2_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", {rb2_port});\n'
                    's["on_request_body"] = func(chunk) {\n'
                    '  print("rb=" + bytes_to_string(chunk));\n'
                    "};\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "handler_body=" + '
                    'bytes_to_string(req["body"])};\n'
                    "}, 1);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                rb2_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", rb2_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 request body server did not listen"
                rb2_client = run(
                    "import std;\n"
                    "func parts() {\n"
                    '  yield "hello";\n'
                    '  yield " ";\n'
                    '  yield "world";\n'
                    "}\n"
                    f'let a = std.http2.post('
                    f'"http://127.0.0.1:{rb2_port}/a", '
                    '{"body_stream": parts()});\n'
                    'print(bytes_to_string(a["body"]));\n',
                    timeout=15,
                )
                assert rb2_client.returncode == 0, rb2_client.stderr
                assert "handler_body=" in rb2_client.stdout, rb2_client.stdout
                rb2_proc.wait(timeout=10)
                rb2_output = rb2_proc.stdout.read()
                rb2_chunks = "".join(
                    line[3:] for line in rb2_output.splitlines()
                    if line.startswith("rb="))
                assert rb2_chunks == "hello world", rb2_output
                assert "served 1" in rb2_output, rb2_output
            finally:
                if rb2_proc.poll() is None:
                    rb2_proc.terminate()
                    rb2_proc.wait(timeout=5)

            http2_conc_probe = socket.socket()
            http2_conc_probe.bind(("127.0.0.1", 0))
            http2_conc_port = http2_conc_probe.getsockname()[1]
            http2_conc_probe.close()
            http2_conc_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_conc_proc.stdin.write(
                    "import std;\n"
                    "async func handler(req) {\n"
                    "  await std.task.sleep(120);\n"
                    '  return {"status": 200, "body": "done " + '
                    'req["path"]};\n'
                    "}\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_conc_port});\n"
                    "let n = std.http2.serve(s, handler, 3);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_conc_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_conc_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 concurrent server did not listen"
                started = time.time()
                http2_conc_client = run(
                    "import std;\n"
                    "let rs = std.http2.request_many([\n"
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_conc_port}/a"}},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_conc_port}/b"}},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_conc_port}/c"}}\n'
                    "]);\n"
                    'print(bytes_to_string(rs[0]["body"]));\n'
                    'print(bytes_to_string(rs[1]["body"]));\n'
                    'print(bytes_to_string(rs[2]["body"]));\n',
                    timeout=15,
                )
                elapsed = time.time() - started
                assert http2_conc_client.returncode == 0, (
                    http2_conc_client.stderr)
                assert "done /a" in http2_conc_client.stdout, (
                    http2_conc_client.stdout)
                assert "done /b" in http2_conc_client.stdout, (
                    http2_conc_client.stdout)
                assert "done /c" in http2_conc_client.stdout, (
                    http2_conc_client.stdout)
                assert elapsed < 0.30, (
                    f"http2 handlers did not run concurrently: {elapsed:.3f}s")
                http2_conc_proc.wait(timeout=10)
                conc_output = http2_conc_proc.stdout.read()
                assert "served 3" in conc_output, conc_output
            finally:
                if http2_conc_proc.poll() is None:
                    http2_conc_proc.terminate()
                    http2_conc_proc.wait(timeout=5)

            http2_mc_probe = socket.socket()
            http2_mc_probe.bind(("127.0.0.1", 0))
            http2_mc_port = http2_mc_probe.getsockname()[1]
            http2_mc_probe.close()
            http2_mc_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_mc_proc.stdin.write(
                    "import std;\n"
                    "async func handler(req) {\n"
                    "  await std.task.sleep(300);\n"
                    '  return {"status": 200, "body": "ok " + '
                    'req["path"]};\n'
                    "}\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_mc_port});\n"
                    "let n = std.http2.serve(s, handler, 2);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_mc_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_mc_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 multi-conn server did not listen"
                client_source = (
                    "import std;\n"
                    f'let a = std.http2.get('
                    f'"http://127.0.0.1:{http2_mc_port}/a");\n'
                    'print(bytes_to_string(a["body"]));\n'
                )
                started = time.time()
                mc1 = subprocess.Popen(
                    [BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, encoding="utf-8")
                time.sleep(0.02)
                mc2 = subprocess.Popen(
                    [BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, encoding="utf-8")
                mc1.stdin.write(client_source)
                mc1.stdin.close()
                mc2.stdin.write(client_source)
                mc2.stdin.close()
                out1, err1 = mc1.communicate(timeout=15)
                out2, err2 = mc2.communicate(timeout=15)
                elapsed = time.time() - started
                assert mc1.returncode == 0, err1
                assert mc2.returncode == 0, err2
                assert "ok /a" in out1, out1
                assert "ok /a" in out2, out2
                assert elapsed < 0.55, (
                    f"http2 connections were not served concurrently: "
                    f"{elapsed:.3f}s")
                http2_mc_proc.wait(timeout=10)
                mc_output = http2_mc_proc.stdout.read()
                assert "served 2" in mc_output, mc_output
            finally:
                if http2_mc_proc.poll() is None:
                    http2_mc_proc.terminate()
                    http2_mc_proc.wait(timeout=5)

            http2_shared_probe = socket.socket()
            http2_shared_probe.bind(("127.0.0.1", 0))
            http2_shared_port = http2_shared_probe.getsockname()[1]
            http2_shared_probe.close()
            http2_shared_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_shared_proc.stdin.write(
                    "import std;\n"
                    "async func handler(req) {\n"
                    "  await std.task.sleep(100);\n"
                    '  return {"status": 200, "body": "cid=" + '
                    'str(req["connection_id"]) + " " + req["method"] + '
                    '" " + req["path"] + " " + '
                    'bytes_to_string(req["body"])};\n'
                    "}\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_shared_port});\n"
                    "let n = std.http2.serve(s, handler, 6);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_shared_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_shared_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 shared async server did not listen"
                started = time.time()
                http2_shared_client = run(
                    "import std;\n"
                    f'let t1 = std.http2.get_async('
                    f'"http://127.0.0.1:{http2_shared_port}/a");\n'
                    f'let t2 = std.http2.get_async('
                    f'"http://127.0.0.1:{http2_shared_port}/b");\n'
                    f'let t3 = std.http2.get_async('
                    f'"http://127.0.0.1:{http2_shared_port}/c");\n'
                    "let a = await t1;\n"
                    "let b = await t2;\n"
                    "let c = await t3;\n"
                    'print(bytes_to_string(a["body"]));\n'
                    'print(bytes_to_string(b["body"]));\n'
                    'print(bytes_to_string(c["body"]));\n'
                    f'let p1 = std.http2.post_async('
                    f'"http://127.0.0.1:{http2_shared_port}/d", "one");\n'
                    f'let p2 = std.http2.post_async('
                    f'"http://127.0.0.1:{http2_shared_port}/e", "two");\n'
                    f'let p3 = std.http2.post_async('
                    f'"http://127.0.0.1:{http2_shared_port}/f", "three");\n'
                    "let d = await p1;\n"
                    "let e = await p2;\n"
                    "let f = await p3;\n"
                    'print(bytes_to_string(d["body"]));\n'
                    'print(bytes_to_string(e["body"]));\n'
                    'print(bytes_to_string(f["body"]));\n',
                    timeout=15,
                )
                elapsed = time.time() - started
                assert http2_shared_client.returncode == 0, (
                    http2_shared_client.stderr)
                shared_lines = http2_shared_client.stdout.splitlines()
                assert len(shared_lines) == 6, http2_shared_client.stdout
                shared_cids = [line.split()[0] for line in shared_lines]
                assert len(set(shared_cids)) == 1, (
                    "get_async/post_async did not share one connection: "
                    + http2_shared_client.stdout)
                assert "GET /a" in shared_lines[0], http2_shared_client.stdout
                assert "GET /b" in shared_lines[1], http2_shared_client.stdout
                assert "GET /c" in shared_lines[2], http2_shared_client.stdout
                assert "POST /d one" in shared_lines[3], (
                    http2_shared_client.stdout)
                assert "POST /e two" in shared_lines[4], (
                    http2_shared_client.stdout)
                assert "POST /f three" in shared_lines[5], (
                    http2_shared_client.stdout)
                assert elapsed < 0.45, (
                    f"shared async requests did not overlap: {elapsed:.3f}s")
                http2_shared_proc.wait(timeout=10)
                shared_output = http2_shared_proc.stdout.read()
                assert "served 6" in shared_output, shared_output
            finally:
                if http2_shared_proc.poll() is None:
                    http2_shared_proc.terminate()
                    http2_shared_proc.wait(timeout=5)

            http2_shared_go_probe = socket.socket()
            http2_shared_go_probe.bind(("127.0.0.1", 0))
            http2_shared_go_port = http2_shared_go_probe.getsockname()[1]
            http2_shared_go_probe.close()
            http2_shared_go_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_shared_go_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_shared_go_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "cid=" + '
                    'str(req["connection_id"]) + " " + req["path"]};\n'
                    "}, 4, 2);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_shared_go_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_shared_go_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 shared goaway server did not listen"
                http2_shared_go_client = run(
                    "import std;\n"
                    f'let a = await std.http2.get_async('
                    f'"http://127.0.0.1:{http2_shared_go_port}/a");\n'
                    f'let b = await std.http2.get_async('
                    f'"http://127.0.0.1:{http2_shared_go_port}/b");\n'
                    'print(bytes_to_string(a["body"]));\n'
                    'print(bytes_to_string(b["body"]));\n'
                    f'let t1 = std.http2.get_async('
                    f'"http://127.0.0.1:{http2_shared_go_port}/c");\n'
                    f'let t2 = std.http2.get_async('
                    f'"http://127.0.0.1:{http2_shared_go_port}/d");\n'
                    "let c = await t1;\n"
                    "let d = await t2;\n"
                    'print(bytes_to_string(c["body"]));\n'
                    'print(bytes_to_string(d["body"]));\n',
                    timeout=15,
                )
                assert http2_shared_go_client.returncode == 0, (
                    http2_shared_go_client.stderr)
                go_lines = http2_shared_go_client.stdout.splitlines()
                assert len(go_lines) == 4, http2_shared_go_client.stdout
                go_cids = [line.split()[0] for line in go_lines]
                assert go_cids[0] == go_cids[1], http2_shared_go_client.stdout
                assert go_cids[2] == go_cids[3], http2_shared_go_client.stdout
                assert go_cids[0] != go_cids[2], (
                    "get_async reused a GOAWAY connection: "
                    + http2_shared_go_client.stdout)
                http2_shared_go_proc.wait(timeout=10)
                go_output = http2_shared_go_proc.stdout.read()
                assert "served 4" in go_output, go_output
            finally:
                if http2_shared_go_proc.poll() is None:
                    http2_shared_go_proc.terminate()
                    http2_shared_go_proc.wait(timeout=5)

            http2_retry_probe = socket.socket()
            http2_retry_probe.bind(("127.0.0.1", 0))
            http2_retry_port = http2_retry_probe.getsockname()[1]
            http2_retry_probe.close()
            http2_retry_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_retry_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_retry_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "cid=" + '
                    'str(req["connection_id"]) + " " + req["path"]};\n'
                    "}, 2, 1);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_retry_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_retry_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 retry server did not listen"
                http2_retry_client = run(
                    "import std;\n"
                    f'let t1 = std.http2.get_async('
                    f'"http://127.0.0.1:{http2_retry_port}/a");\n'
                    f'let t2 = std.http2.get_async('
                    f'"http://127.0.0.1:{http2_retry_port}/b");\n'
                    "let a = await t1;\n"
                    "let b = await t2;\n"
                    'print(bytes_to_string(a["body"]));\n'
                    'print(bytes_to_string(b["body"]));\n',
                    timeout=15,
                )
                assert http2_retry_client.returncode == 0, (
                    http2_retry_client.stderr)
                retry_lines = http2_retry_client.stdout.splitlines()
                assert len(retry_lines) == 2, http2_retry_client.stdout
                retry_cids = [line.split()[0] for line in retry_lines]
                assert retry_cids[0] != retry_cids[1], (
                    "http2 client did not retry the refused stream: "
                    + http2_retry_client.stdout)
                assert " /a" in retry_lines[0], http2_retry_client.stdout
                assert " /b" in retry_lines[1], http2_retry_client.stdout
                http2_retry_proc.wait(timeout=10)
                retry_output = http2_retry_proc.stdout.read()
                assert "served 2" in retry_output, retry_output
            finally:
                if http2_retry_proc.poll() is None:
                    http2_retry_proc.terminate()
                    http2_retry_proc.wait(timeout=5)

            http2_drain_probe = socket.socket()
            http2_drain_probe.bind(("127.0.0.1", 0))
            http2_drain_port = http2_drain_probe.getsockname()[1]
            http2_drain_probe.close()
            http2_drain_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_drain_proc.stdin.write(
                    "import std;\n"
                    "async func handler(req) {\n"
                    "  await std.task.sleep(100);\n"
                    '  return {"status": 200, "body": "cid=" + '
                    'str(req["connection_id"]) + " " + req["path"]};\n'
                    "}\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_drain_port});\n"
                    "let n = std.http2.serve(s, handler, 2);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_drain_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_drain_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 drain server did not listen"
                http2_drain_client = run(
                    "import std;\n"
                    f'let t1 = std.http2.get_async('
                    f'"http://127.0.0.1:{http2_drain_port}/a");\n'
                    f'let t2 = std.http2.get_async('
                    f'"http://127.0.0.1:{http2_drain_port}/b");\n'
                    "let a = await t1;\n"
                    "let b = await t2;\n"
                    'print(bytes_to_string(a["body"]));\n'
                    'print(bytes_to_string(b["body"]));\n',
                    timeout=15,
                )
                assert http2_drain_client.returncode == 0, (
                    http2_drain_client.stderr)
                drain_lines = http2_drain_client.stdout.splitlines()
                assert len(drain_lines) == 2, http2_drain_client.stdout
                drain_cids = [line.split()[0] for line in drain_lines]
                assert len(set(drain_cids)) == 1, (
                    "http2 max_requests drain did not finish in-flight "
                    "streams: " + http2_drain_client.stdout)
                assert " /a" in drain_lines[0], http2_drain_client.stdout
                assert " /b" in drain_lines[1], http2_drain_client.stdout
                http2_drain_proc.wait(timeout=10)
                drain_output = http2_drain_proc.stdout.read()
                assert "served 2" in drain_output, drain_output
            finally:
                if http2_drain_proc.poll() is None:
                    http2_drain_proc.terminate()
                    http2_drain_proc.wait(timeout=5)

            http2_ss_probe = socket.socket()
            http2_ss_probe.bind(("127.0.0.1", 0))
            http2_ss_port = http2_ss_probe.getsockname()[1]
            http2_ss_probe.close()
            http2_ss_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_ss_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_ss_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "cid=" + '
                    'str(req["connection_id"]) + " echo:" + '
                    'bytes_to_string(req["body"])};\n'
                    "}, 2);\n"
                    'print("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_ss_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_ss_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 shared stream server did not listen"
                http2_ss_client = run(
                    "import std;\n"
                    "func p1() { yield \"hello\"; }\n"
                    "func p2() { yield \"world\"; }\n"
                    f'let t1 = std.http2.post_async('
                    f'"http://127.0.0.1:{http2_ss_port}/a", '
                    '{"body_stream": p1(), "on_body": func(c) {\n'
                    '  print("s1=" + bytes_to_string(c));\n'
                    "}});\n"
                    f'let t2 = std.http2.post_async('
                    f'"http://127.0.0.1:{http2_ss_port}/b", '
                    '{"body_stream": p2(), "on_body": func(c) {\n'
                    '  print("s2=" + bytes_to_string(c));\n'
                    "}});\n"
                    "let r1 = await t1;\n"
                    "let r2 = await t2;\n"
                    'print("r1=" + bytes_to_string(r1["body"]));\n'
                    'print("r2=" + bytes_to_string(r2["body"]));\n',
                    timeout=15,
                )
                assert http2_ss_client.returncode == 0, (
                    http2_ss_client.stderr)
                ss_lines = http2_ss_client.stdout.splitlines()
                s1 = "".join(line[3:] for line in ss_lines
                             if line.startswith("s1="))
                s2 = "".join(line[3:] for line in ss_lines
                             if line.startswith("s2="))
                assert s1 == "cid=1 echo:hello", http2_ss_client.stdout
                assert s2 == "cid=1 echo:world", http2_ss_client.stdout
                assert "r1=" in ss_lines, http2_ss_client.stdout
                assert "r2=" in ss_lines, http2_ss_client.stdout
                http2_ss_proc.wait(timeout=10)
                ss_output = http2_ss_proc.stdout.read()
                assert "served 2" in ss_output, ss_output
            finally:
                if http2_ss_proc.poll() is None:
                    http2_ss_proc.terminate()
                    http2_ss_proc.wait(timeout=5)

        import shutil
        tls_probe = run('import std;\nstd.dict.has(std, "tls")\n')
        tls_enabled = "= true" in tls_probe.stdout
        if tls_enabled and shutil.which("openssl"):
            with tempfile.TemporaryDirectory() as tls_tmp:
                tls_cert = os.path.join(tls_tmp, "cert.pem")
                tls_key = os.path.join(tls_tmp, "key.pem")
                cert_proc = subprocess.run(
                    ["openssl", "req", "-x509", "-newkey", "rsa:2048",
                     "-nodes", "-keyout", tls_key, "-out", tls_cert,
                     "-days", "1", "-subj", "/CN=localhost",
                     "-addext", "subjectAltName=DNS:localhost"],
                    capture_output=True, text=True)
                assert cert_proc.returncode == 0, cert_proc.stderr

                tls_server = socket.socket()
                tls_server.setsockopt(
                    socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                tls_server.bind(("127.0.0.1", 0))
                tls_server.listen(1)
                tls_server.settimeout(5)
                tls_port = tls_server.getsockname()[1]

                def tls_echo_server():
                    import ssl
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(tls_cert, tls_key)
                    context.set_alpn_protocols(["http/1.1"])
                    try:
                        client, _ = tls_server.accept()
                        tls_client = context.wrap_socket(
                            client, server_side=True)
                        data = tls_client.recv(100)
                        tls_client.sendall(b"tls:" + data)
                        tls_client.close()
                    finally:
                        tls_server.close()

                tls_thread = threading.Thread(target=tls_echo_server)
                tls_thread.start()
                tls_client_proc = run(
                    "import std;\n"
                    f'let s = std.net.tcp_connect("127.0.0.1", {tls_port}, '
                    '1000);\n'
                    'let t = std.tls.client(s, "localhost", '
                    f'{{"ca_file": "{tls_cert}", '
                    '"alpn": ["h2", "http/1.1"]});\n'
                    "print(std.tls.alpn(t));\n"
                    'std.io.write_all(t, "hello");\n'
                    "print(bytes_to_string(std.io.read(t, 100)));\n"
                    "std.io.close(t);\n"
                )
                tls_thread.join(timeout=5)
                assert tls_client_proc.returncode == 0, (
                    tls_client_proc.stderr)
                assert "tls:hello" in tls_client_proc.stdout, (
                    tls_client_proc.stdout)
                assert "http/1.1" in tls_client_proc.stdout, (
                    tls_client_proc.stdout)

                h2_tls_probe = socket.socket()
                h2_tls_probe.bind(("127.0.0.1", 0))
                h2_tls_port = h2_tls_probe.getsockname()[1]
                h2_tls_probe.close()
                h2_tls_server_proc = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                try:
                    h2_tls_server_proc.stdin.write(
                        "import std;\n"
                        f'let s = std.http2.listen_tls("127.0.0.1", '
                        f'{h2_tls_port}, "{tls_cert}", "{tls_key}");\n'
                        "let n = std.http2.serve(s, func(req) {\n"
                        '  return {"status": 200, "body": "tls-h2:" + '
                        'req["path"]};\n'
                        "}, 1);\n"
                        'print("served " + str(n));\n'
                        "std.http2.close(s);\n"
                    )
                    h2_tls_server_proc.stdin.close()
                    time.sleep(0.5)
                    h2_tls_client_proc = run(
                        "import std;\n"
                        f'let a = std.http2.get('
                        f'"https://127.0.0.1:{h2_tls_port}/secure", '
                        '{"tls": {"verify": false}});\n'
                        'print(str(a["status"]) + " " + a["protocol"]);\n'
                        'print(bytes_to_string(a["body"]));\n',
                        timeout=15,
                    )
                    assert h2_tls_client_proc.returncode == 0, (
                        h2_tls_client_proc.stderr)
                    assert "200 h2" in h2_tls_client_proc.stdout, (
                        h2_tls_client_proc.stdout)
                    assert "tls-h2:/secure" in h2_tls_client_proc.stdout, (
                        h2_tls_client_proc.stdout)
                    h2_tls_server_proc.wait(timeout=10)
                    h2_tls_output = h2_tls_server_proc.stdout.read()
                    assert "served 1" in h2_tls_output, h2_tls_output
                finally:
                    if h2_tls_server_proc.poll() is None:
                        h2_tls_server_proc.terminate()
                        h2_tls_server_proc.wait(timeout=5)

                tls_server_probe = socket.socket()
                tls_server_probe.bind(("127.0.0.1", 0))
                tls_server_port = tls_server_probe.getsockname()[1]
                tls_server_probe.close()
                tls_calc_server = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                tls_calc_client = None
                try:
                    tls_calc_server.stdin.write(
                        "import std;\n"
                        f'let l = std.net.tcp_listen("127.0.0.1", '
                        f"{tls_server_port}, 1000);\n"
                        "let c = std.net.accept(l, 5000);\n"
                        f'let t = std.tls.server(c, "{tls_cert}", '
                        f'"{tls_key}", '
                        '{"alpn": ["h2", "http/1.1"]});\n'
                        "print(std.tls.alpn(t));\n"
                        "let data = std.io.read(t, 100);\n"
                        'std.io.write_all(t, "server:" + '
                        "bytes_to_string(data));\n"
                        "std.io.close(t);\n"
                        "std.net.close(l);\n"
                    )
                    tls_calc_server.stdin.close()
                    deadline = time.time() + 5
                    while time.time() < deadline:
                        try:
                            tls_calc_client = socket.create_connection(
                                ("127.0.0.1", tls_server_port), timeout=0.2)
                            break
                        except OSError:
                            time.sleep(0.05)
                    assert tls_calc_client is not None, (
                        "tls server did not listen")
                    import ssl
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    context.check_hostname = False
                    context.verify_mode = ssl.CERT_NONE
                    context.set_alpn_protocols(["http/1.1"])
                    tls_wrapped = context.wrap_socket(
                        tls_calc_client, server_hostname="localhost")
                    assert tls_wrapped.selected_alpn_protocol() == "http/1.1"
                    tls_wrapped.sendall(b"hello")
                    tls_response = tls_wrapped.recv(100)
                    assert tls_response == b"server:hello", tls_response
                    tls_wrapped.close()
                finally:
                    if tls_calc_client:
                        tls_calc_client.close()
                    tls_calc_server.wait(timeout=10)
                    tls_calc_output = tls_calc_server.stdout.read()
                    assert "http/1.1" in tls_calc_output, tls_calc_output

                tls_async_server = socket.socket()
                tls_async_server.setsockopt(
                    socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                tls_async_server.bind(("127.0.0.1", 0))
                tls_async_server.listen(1)
                tls_async_server.settimeout(5)
                tls_async_port = tls_async_server.getsockname()[1]

                def tls_async_echo_server():
                    import ssl
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(tls_cert, tls_key)
                    try:
                        client, _ = tls_async_server.accept()
                        tls_client = context.wrap_socket(
                            client, server_side=True)
                        data = tls_client.recv(100)
                        tls_client.sendall(b"async-tls:" + data)
                        tls_client.close()
                    finally:
                        tls_async_server.close()

                tls_async_thread = threading.Thread(
                    target=tls_async_echo_server)
                tls_async_thread.start()
                tls_async_proc = run(
                    "import std;\n"
                    f'let s = std.net.tcp_connect("127.0.0.1", '
                    f'{tls_async_port}, 1000);\n'
                    'let t = std.tls.client(s, "localhost", '
                    f'{{"ca_file": "{tls_cert}"}});\n'
                    'await std.io.write_async(t, "hello");\n'
                    "print(bytes_to_string(await std.io.read_async(t, 100)));\n"
                    "std.io.close(t);\n"
                )
                tls_async_thread.join(timeout=5)
                assert tls_async_proc.returncode == 0, tls_async_proc.stderr
                assert "async-tls:hello" in tls_async_proc.stdout, (
                    tls_async_proc.stdout)

                https_server = socket.socket()
                https_server.setsockopt(
                    socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                https_server.bind(("127.0.0.1", 0))
                https_server.listen(1)
                https_server.settimeout(5)
                https_port = https_server.getsockname()[1]

                def https_echo_server():
                    import ssl
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(tls_cert, tls_key)
                    try:
                        client, _ = https_server.accept()
                        tls_client = context.wrap_socket(
                            client, server_side=True)
                        request = b""
                        while b"\r\n\r\n" not in request:
                            chunk = tls_client.recv(4096)
                            if not chunk:
                                break
                            request += chunk
                        tls_client.sendall(
                            b"HTTP/1.1 200 OK\r\n"
                            b"Content-Length: 5\r\n"
                            b"\r\n"
                            b"hello")
                        tls_client.close()
                    finally:
                        https_server.close()

                https_thread = threading.Thread(target=https_echo_server)
                https_thread.start()
                https_proc = run(
                    "import std;\n"
                    f'let r = std.http.get("https://localhost:{https_port}/", '
                    f'{{"tls": {{"ca_file": "{tls_cert}"}}}});\n'
                    'print(r["status"]);\n'
                    'print(bytes_to_string(r["body"]));\n'
                )
                https_thread.join(timeout=5)
                assert https_proc.returncode == 0, https_proc.stderr
                assert "200" in https_proc.stdout, https_proc.stdout
                assert "hello" in https_proc.stdout, https_proc.stdout

                https_probe = socket.socket()
                https_probe.bind(("127.0.0.1", 0))
                https_server_port = https_probe.getsockname()[1]
                https_probe.close()
                https_calc_server = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                https_calc_client = None
                try:
                    https_calc_server.stdin.write(
                        "import std;\n"
                        f'let s = std.http.listen_tls("127.0.0.1", '
                        f'{https_server_port}, "{tls_cert}", "{tls_key}");\n'
                        "let req = std.http.accept(s, 5000);\n"
                        'std.http.respond(req, 200, {"X-TLS": "yes"}, '
                        '"secure " + req["path"]);\n'
                        "std.http.close(s);\n"
                    )
                    https_calc_server.stdin.close()
                    deadline = time.time() + 5
                    while time.time() < deadline:
                        try:
                            https_calc_client = socket.create_connection(
                                ("127.0.0.1", https_server_port), timeout=0.2)
                            break
                        except OSError:
                            time.sleep(0.05)
                    assert https_calc_client is not None, (
                        "https server did not listen")
                    import ssl
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    context.check_hostname = False
                    context.verify_mode = ssl.CERT_NONE
                    https_wrapped = context.wrap_socket(
                        https_calc_client, server_hostname="localhost")
                    https_wrapped.sendall(
                        b"GET /secure HTTP/1.1\r\n"
                        b"Host: localhost\r\n"
                        b"Connection: close\r\n"
                        b"\r\n")
                    https_response = b""
                    while True:
                        chunk = https_wrapped.recv(4096)
                        if not chunk:
                            break
                        https_response += chunk
                    https_text = https_response.decode(
                        "utf-8", errors="replace")
                    assert "200 OK" in https_text, https_text
                    assert "secure /secure" in https_text, https_text
                    assert "x-tls" in https_text.lower(), https_text
                    https_wrapped.close()
                finally:
                    if https_calc_client:
                        https_calc_client.close()
                    https_calc_server.wait(timeout=10)

                async_https_server = socket.socket()
                async_https_server.setsockopt(
                    socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                async_https_server.bind(("127.0.0.1", 0))
                async_https_server.listen(1)
                async_https_server.settimeout(5)
                async_https_port = async_https_server.getsockname()[1]

                def async_https_echo_server():
                    import ssl
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(tls_cert, tls_key)
                    try:
                        client, _ = async_https_server.accept()
                        tls_client = context.wrap_socket(
                            client, server_side=True)
                        data = b""
                        while b"\r\n\r\n" not in data:
                            chunk = tls_client.recv(4096)
                            if not chunk:
                                break
                            data += chunk
                        tls_client.sendall(
                            b"HTTP/1.1 201 Created\r\n"
                            b"Content-Length: 5\r\n"
                            b"\r\n"
                            b"async")
                        tls_client.close()
                    finally:
                        async_https_server.close()

                async_https_thread = threading.Thread(
                    target=async_https_echo_server)
                async_https_thread.start()
                async_https_proc = run(
                    "import std;\n"
                    f'let r = await std.http.get_async('
                    f'"https://localhost:{async_https_port}/", '
                    f'{{"tls": {{"ca_file": "{tls_cert}"}}}});\n'
                    'print(r["status"]);\n'
                    'print(bytes_to_string(r["body"]));\n'
                )
                async_https_thread.join(timeout=5)
                assert async_https_proc.returncode == 0, (
                    async_https_proc.stderr)
                assert "201" in async_https_proc.stdout, (
                    async_https_proc.stdout)
                assert "async" in async_https_proc.stdout, (
                    async_https_proc.stdout)

                async_https_probe = socket.socket()
                async_https_probe.bind(("127.0.0.1", 0))
                async_https_server_port = async_https_probe.getsockname()[1]
                async_https_probe.close()
                async_https_calc = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                async_https_client = None
                try:
                    async_https_calc.stdin.write(
                        "import std;\n"
                        f'let s = std.http.listen_tls("127.0.0.1", '
                        f'{async_https_server_port}, "{tls_cert}", '
                        f'"{tls_key}");\n'
                        "let req = await std.http.accept_async(s, 10000);\n"
                        'await std.http.respond_async(req, 200, '
                        '{"X-Async": "yes"}, "async secure " + '
                        'req["path"]);\n'
                        "std.http.close(s);\n"
                    )
                    async_https_calc.stdin.close()
                    deadline = time.time() + 5
                    while time.time() < deadline:
                        try:
                            async_https_client = socket.create_connection(
                                ("127.0.0.1", async_https_server_port),
                                timeout=0.2)
                            break
                        except OSError:
                            time.sleep(0.05)
                    assert async_https_client is not None, (
                        "async https server did not listen")
                    import ssl
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    context.check_hostname = False
                    context.verify_mode = ssl.CERT_NONE
                    wrapped = context.wrap_socket(
                        async_https_client, server_hostname="localhost")
                    wrapped.sendall(
                        b"GET /async HTTPS HTTP/1.1\r\n"
                        b"Host: localhost\r\n"
                        b"Connection: close\r\n"
                        b"\r\n")
                    response = b""
                    while True:
                        chunk = wrapped.recv(4096)
                        if not chunk:
                            break
                        response += chunk
                    text = response.decode("utf-8", errors="replace")
                    assert "200 OK" in text, text
                    assert "async secure /async HTTPS" in text, text
                    wrapped.close()
                finally:
                    if async_https_client:
                        async_https_client.close()
                    async_https_calc.wait(timeout=10)

        serve_probe = socket.socket()
        serve_probe.bind(("127.0.0.1", 0))
        serve_port = serve_probe.getsockname()[1]
        serve_probe.close()
        serve_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        serve_client = None
        try:
            serve_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", {serve_port});\n'
                "let n = std.http.serve(s, func(req) {\n"
                '  return {"status": 200, "headers": {"Content-Type": '
                '"text/plain"}, "body": "serve:" + req["path"]};\n'
                "}, 2);\n"
                'print("served " + str(n));\n'
                "std.http.close(s);\n"
            )
            serve_proc.stdin.close()
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    serve_client = socket.create_connection(
                        ("127.0.0.1", serve_port), timeout=0.2)
                    break
                except OSError:
                    time.sleep(0.05)
            assert serve_client is not None, "http serve did not listen"
            responses = []
            for path in ("/one", "/two"):
                serve_client.sendall(
                    f"GET {path} HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n".encode())
                data = b""
                while b"\r\n\r\n" not in data:
                    chunk = serve_client.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                responses.append(data)
            assert b"serve:/one" in responses[0], responses[0]
            assert b"serve:/two" in responses[1], responses[1]
            assert b"Connection: keep-alive" in responses[0]
            assert b"Connection: close" in responses[1]
            serve_proc.wait(timeout=10)
            serve_output = serve_proc.stdout.read()
            assert "served 2" in serve_output, serve_output
        finally:
            if serve_client:
                serve_client.close()
            if serve_proc.poll() is None:
                serve_proc.terminate()
                serve_proc.wait(timeout=5)

        serve_limit_probe = socket.socket()
        serve_limit_probe.bind(("127.0.0.1", 0))
        serve_limit_port = serve_limit_probe.getsockname()[1]
        serve_limit_probe.close()
        serve_limit_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        serve_limit_clients = []
        try:
            serve_limit_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", {serve_limit_port});\n'
                "std.http.serve(s, func(req) {\n"
                '  return {"status": 200, "body": "limit"};\n'
                "}, 2, 1, 1000);\n"
                "std.http.close(s);\n"
            )
            serve_limit_proc.stdin.close()
            deadline = time.time() + 5
            connected = False
            while time.time() < deadline:
                try:
                    probe = socket.create_connection(
                        ("127.0.0.1", serve_limit_port), timeout=0.2)
                    probe.close()
                    connected = True
                    break
                except OSError:
                    time.sleep(0.05)
            assert connected, "http serve limit did not listen"
            for i in range(2):
                client = socket.create_connection(
                    ("127.0.0.1", serve_limit_port), timeout=5)
                serve_limit_clients.append(client)
                client.sendall(
                    b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    b"Connection: keep-alive\r\n\r\n")
                response = b""
                while b"\r\n\r\n" not in response:
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    response += chunk
                assert b"Connection: close" in response, response
            serve_limit_proc.wait(timeout=10)
        finally:
            for client in serve_limit_clients:
                client.close()
            if serve_limit_proc.poll() is None:
                serve_limit_proc.terminate()
                serve_limit_proc.wait(timeout=5)

        serve_idle_probe = socket.socket()
        serve_idle_probe.bind(("127.0.0.1", 0))
        serve_idle_port = serve_idle_probe.getsockname()[1]
        serve_idle_probe.close()
        serve_idle_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        serve_idle_client = None
        serve_idle_client2 = None
        try:
            serve_idle_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", {serve_idle_port});\n'
                "std.http.serve(s, func(req) {\n"
                '  return {"status": 200, "body": "idle"};\n'
                "}, 2, 0, 200);\n"
                "std.http.close(s);\n"
            )
            serve_idle_proc.stdin.close()
            deadline = time.time() + 5
            connected = False
            while time.time() < deadline:
                try:
                    probe = socket.create_connection(
                        ("127.0.0.1", serve_idle_port), timeout=0.2)
                    probe.close()
                    connected = True
                    break
                except OSError:
                    time.sleep(0.05)
            assert connected, "http serve idle did not listen"
            serve_idle_client = socket.create_connection(
                ("127.0.0.1", serve_idle_port), timeout=5)
            serve_idle_client.sendall(
                b"GET /1 HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                b"Connection: keep-alive\r\n\r\n")
            response = b""
            while b"\r\n\r\n" not in response:
                chunk = serve_idle_client.recv(4096)
                if not chunk:
                    break
                response += chunk
            time.sleep(0.5)
            serve_idle_client.settimeout(1)
            assert serve_idle_client.recv(100) == b""

            serve_idle_client2 = socket.create_connection(
                ("127.0.0.1", serve_idle_port), timeout=5)
            serve_idle_client2.sendall(
                b"GET /2 HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                b"Connection: keep-alive\r\n\r\n")
            response2 = b""
            while b"\r\n\r\n" not in response2:
                chunk = serve_idle_client2.recv(4096)
                if not chunk:
                    break
                response2 += chunk
            assert b"idle" in response2, response2
            serve_idle_proc.wait(timeout=10)
        finally:
            if serve_idle_client:
                serve_idle_client.close()
            if serve_idle_client2:
                serve_idle_client2.close()
            if serve_idle_proc.poll() is None:
                serve_idle_proc.terminate()
                serve_idle_proc.wait(timeout=5)

        http1_mc_probe = socket.socket()
        http1_mc_probe.bind(("127.0.0.1", 0))
        http1_mc_port = http1_mc_probe.getsockname()[1]
        http1_mc_probe.close()
        http1_mc_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        try:
            http1_mc_proc.stdin.write(
                "import std;\n"
                "async func handler(req) {\n"
                "  await std.task.sleep(300);\n"
                '  return {"status": 200, "body": "ok " + req["path"]};\n'
                "}\n"
                f'let s = std.http.listen("127.0.0.1", {http1_mc_port});\n'
                "let n = std.http.serve(s, handler, 2);\n"
                'print("served " + str(n));\n'
                "std.http.close(s);\n"
            )
            http1_mc_proc.stdin.close()
            deadline = time.time() + 5
            ready = False
            while time.time() < deadline:
                try:
                    probe = socket.create_connection(
                        ("127.0.0.1", http1_mc_port), timeout=0.2)
                    probe.close()
                    ready = True
                    break
                except OSError:
                    time.sleep(0.05)
            assert ready, "http1 multi-conn server did not listen"
            client_source = (
                "import std;\n"
                f'let a = std.http.get('
                f'"http://127.0.0.1:{http1_mc_port}/a");\n'
                'print(bytes_to_string(a["body"]));\n'
            )
            started = time.time()
            mc1 = subprocess.Popen(
                [BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, encoding="utf-8")
            time.sleep(0.02)
            mc2 = subprocess.Popen(
                [BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, encoding="utf-8")
            mc1.stdin.write(client_source)
            mc1.stdin.close()
            mc2.stdin.write(client_source)
            mc2.stdin.close()
            out1, err1 = mc1.communicate(timeout=15)
            out2, err2 = mc2.communicate(timeout=15)
            elapsed = time.time() - started
            assert mc1.returncode == 0, err1
            assert mc2.returncode == 0, err2
            assert "ok /a" in out1, out1
            assert "ok /a" in out2, out2
            assert elapsed < 0.55, (
                f"http1 connections were not served concurrently: "
                f"{elapsed:.3f}s")
            http1_mc_proc.wait(timeout=10)
            mc_output = http1_mc_proc.stdout.read()
            assert "served 2" in mc_output, mc_output
        finally:
            if http1_mc_proc.poll() is None:
                http1_mc_proc.terminate()
                http1_mc_proc.wait(timeout=5)

        upgrade_probe = socket.socket()
        upgrade_probe.bind(("127.0.0.1", 0))
        upgrade_port = upgrade_probe.getsockname()[1]
        upgrade_probe.close()
        upgrade_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        upgrade_client = None
        try:
            upgrade_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", {upgrade_port});\n'
                "let req = std.http.accept(s, 5000);\n"
                "let stream = std.http.upgrade(req);\n"
                "let data = std.io.read(stream, 100);\n"
                'std.io.write_all(stream, "echo:" + '
                "bytes_to_string(data));\n"
                "std.io.close(stream);\n"
                "std.http.close(s);\n"
            )
            upgrade_proc.stdin.close()
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    upgrade_client = socket.create_connection(
                        ("127.0.0.1", upgrade_port), timeout=0.2)
                    break
                except OSError:
                    time.sleep(0.05)
            assert upgrade_client is not None, (
                "http upgrade server did not listen")
            upgrade_client.sendall(
                b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                b"Connection: Upgrade\r\nUpgrade: test\r\n\r\n")
            response = b""
            while b"\r\n\r\n" not in response:
                chunk = upgrade_client.recv(4096)
                if not chunk:
                    break
                response += chunk
            assert b"101 Switching Protocols" in response, response
            assert b"Connection: Upgrade" in response, response
            assert b"Upgrade: test" in response, response
            upgrade_client.sendall(b"hello")
            assert upgrade_client.recv(100) == b"echo:hello"
            upgrade_proc.wait(timeout=10)
        finally:
            if upgrade_client:
                upgrade_client.close()
            if upgrade_proc.poll() is None:
                upgrade_proc.terminate()
                upgrade_proc.wait(timeout=5)

        upgrade_server_sock = socket.socket()
        upgrade_server_sock.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        upgrade_server_sock.bind(("127.0.0.1", 0))
        upgrade_server_sock.listen(2)
        upgrade_server_sock.settimeout(5)
        upgrade_server_port = upgrade_server_sock.getsockname()[1]

        def upgrade_echo_server():
            try:
                for _ in range(2):
                    client, _ = upgrade_server_sock.accept()
                    request = b""
                    while b"\r\n\r\n" not in request:
                        chunk = client.recv(4096)
                        if not chunk:
                            return
                        request += chunk
                    client.sendall(
                        b"HTTP/1.1 101 Switching Protocols\r\n"
                        b"Connection: Upgrade\r\n"
                        b"Upgrade: test\r\n\r\n")
                    data = client.recv(100)
                    client.sendall(b"echo:" + data)
                    client.close()
            finally:
                upgrade_server_sock.close()

        upgrade_thread = threading.Thread(target=upgrade_echo_server)
        upgrade_thread.start()
        upgrade_client_proc = run(
            "import std;\n"
            f'let a = std.http.get('
            f'"http://127.0.0.1:{upgrade_server_port}/", '
            '{"upgrade": true, "headers": {"Upgrade": "test"}});\n'
            'print(str(a["status"]) + " " + str(a["upgraded"]));\n'
            'std.io.write_all(a["socket"], "one");\n'
            'print(bytes_to_string(std.io.read(a["socket"], 100)));\n'
            'std.io.close(a["socket"]);\n'
            f'let b = await std.http.get_async('
            f'"http://127.0.0.1:{upgrade_server_port}/", '
            '{"upgrade": true, "headers": {"Upgrade": "test"}});\n'
            'print(str(b["status"]) + " " + str(b["upgraded"]));\n'
            'std.io.write_all(b["socket"], "two");\n'
            'print(bytes_to_string(std.io.read(b["socket"], 100)));\n'
            'std.io.close(b["socket"]);\n',
            timeout=15,
        )
        upgrade_thread.join(timeout=5)
        assert upgrade_client_proc.returncode == 0, (
            upgrade_client_proc.stderr)
        assert "101 true" in upgrade_client_proc.stdout, (
            upgrade_client_proc.stdout)
        assert "echo:one" in upgrade_client_proc.stdout, (
            upgrade_client_proc.stdout)
        assert "echo:two" in upgrade_client_proc.stdout, (
            upgrade_client_proc.stdout)

        ws_client_server = socket.socket()
        ws_client_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ws_client_server.bind(("127.0.0.1", 0))
        ws_client_server.listen(1)
        ws_client_server.settimeout(5)
        ws_client_port = ws_client_server.getsockname()[1]
        ws_client_error = []

        def ws_client_server_fn():
            try:
                client, _ = ws_client_server.accept()
                request = b""
                while b"\r\n\r\n" not in request:
                    chunk = client.recv(4096)
                    if not chunk:
                        return
                    request += chunk
                key = ""
                for line in request.split(b"\r\n")[1:]:
                    if line.lower().startswith(b"sec-websocket-key:"):
                        key = line.split(b":", 1)[1].strip().decode()
                if not key:
                    ws_client_error.append("missing websocket key")
                    return
                client.sendall(
                    b"HTTP/1.1 101 Switching Protocols\r\n"
                    b"Connection: Upgrade\r\n"
                    b"Upgrade: websocket\r\n"
                    b"Sec-WebSocket-Accept: " +
                    ws_accept_key(key).encode() + b"\r\n\r\n")
                opcode, payload = ws_read_frame(client)
                if opcode != 1 or payload != b"hello":
                    ws_client_error.append(
                        f"bad client frame: {opcode} {payload!r}")
                    return
                ws_write_frame(client, 1, b"echo:hello", False)
                opcode, _ = ws_read_frame(client)
                if opcode != 8:
                    ws_client_error.append(
                        f"expected close, got {opcode}")
                client.close()
            finally:
                ws_client_server.close()

        ws_client_thread = threading.Thread(target=ws_client_server_fn)
        ws_client_thread.start()
        ws_client_proc = run(
            "import std;\n"
            f'let ws = std.websocket.connect('
            f'"ws://127.0.0.1:{ws_client_port}/");\n'
            'std.websocket.send(ws, "hello");\n'
            "let m = std.websocket.receive(ws);\n"
            'print(m["data"]);\n'
            "std.websocket.close(ws);\n",
            timeout=15,
        )
        ws_client_thread.join(timeout=5)
        assert not ws_client_error, ws_client_error
        assert ws_client_proc.returncode == 0, ws_client_proc.stderr
        assert "echo:hello" in ws_client_proc.stdout, (
            ws_client_proc.stdout)

        ws_server_probe = socket.socket()
        ws_server_probe.bind(("127.0.0.1", 0))
        ws_server_port = ws_server_probe.getsockname()[1]
        ws_server_probe.close()
        ws_server_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        ws_server_client = None
        try:
            ws_server_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", '
                f"{ws_server_port});\n"
                "let req = std.http.accept(s, 5000);\n"
                "let ws = std.websocket.accept(req);\n"
                "let m = std.websocket.receive(ws);\n"
                'print("got:" + m["data"]);\n'
                'std.websocket.send(ws, "echo:" + m["data"]);\n'
                "std.websocket.close(ws);\n"
                "std.http.close(s);\n"
            )
            ws_server_proc.stdin.close()
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    ws_server_client = socket.create_connection(
                        ("127.0.0.1", ws_server_port), timeout=0.2)
                    break
                except OSError:
                    time.sleep(0.05)
            assert ws_server_client is not None, (
                "websocket server did not listen")
            key = base64.b64encode(b"0123456789abcdef").decode()
            ws_server_client.sendall(
                b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                b"Connection: Upgrade\r\nUpgrade: websocket\r\n"
                b"Sec-WebSocket-Version: 13\r\n"
                b"Sec-WebSocket-Key: " + key.encode() + b"\r\n\r\n")
            response = b""
            while b"\r\n\r\n" not in response:
                chunk = ws_server_client.recv(4096)
                if not chunk:
                    break
                response += chunk
            assert b"101 Switching Protocols" in response, response
            assert ("Sec-WebSocket-Accept: " + ws_accept_key(key)).encode() \
                in response, response
            ws_write_frame(ws_server_client, 1, b"hello", True)
            opcode, payload = ws_read_frame(ws_server_client)
            assert opcode == 1 and payload == b"echo:hello", (
                opcode, payload)
            opcode, _ = ws_read_frame(ws_server_client)
            assert opcode == 8, opcode
            ws_server_proc.wait(timeout=10)
            ws_server_output = ws_server_proc.stdout.read()
            assert "got:hello" in ws_server_output, ws_server_output
        finally:
            if ws_server_client:
                ws_server_client.close()
            if ws_server_proc.poll() is None:
                ws_server_proc.terminate()
                ws_server_proc.wait(timeout=5)

        import http.server

        class RedirectHandler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def _send(self, code, body=b"", headers=None):
                self.send_response(code)
                for key, value in (headers or {}).items():
                    self.send_header(key, value)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/a":
                    self._send(302, headers={"Location": "/b"})
                elif self.path == "/b":
                    self._send(200, b"b")
                elif self.path == "/get":
                    self._send(200, b"method=GET")
                elif self.path == "/dir/page":
                    self._send(302, headers={"Location": "next"})
                elif self.path == "/dir/next":
                    self._send(200, b"next")
                elif self.path == "/loop":
                    self._send(302, headers={"Location": "/loop"})
                else:
                    self._send(404)

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                if self.path == "/post303":
                    self._send(303, headers={"Location": "/get"})
                elif self.path == "/post307":
                    self._send(307, headers={"Location": "/echo"})
                elif self.path == "/echo":
                    self._send(200, b"method=POST body=" + body)
                else:
                    self._send(404)

        redirect_server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), RedirectHandler)
        redirect_port = redirect_server.server_address[1]
        redirect_thread = threading.Thread(
            target=redirect_server.serve_forever)
        redirect_thread.start()
        try:
            redirect_client = run(
                "import std;\n"
                f'let a = std.http.get('
                f'"http://127.0.0.1:{redirect_port}/a", '
                '{"follow_redirects": true});\n'
                'print(bytes_to_string(a["body"]));\n'
                f'let b = std.http.post('
                f'"http://127.0.0.1:{redirect_port}/post303", "data", '
                '{"follow_redirects": true});\n'
                'print(bytes_to_string(b["body"]));\n'
                f'let c = std.http.post('
                f'"http://127.0.0.1:{redirect_port}/post307", "payload", '
                '{"follow_redirects": true});\n'
                'print(bytes_to_string(c["body"]));\n'
                f'let d = await std.http.get_async('
                f'"http://127.0.0.1:{redirect_port}/a", '
                '{"follow_redirects": true});\n'
                'print(bytes_to_string(d["body"]));\n'
                f'let e = std.http.get('
                f'"http://127.0.0.1:{redirect_port}/dir/page", '
                '{"follow_redirects": true});\n'
                'print(bytes_to_string(e["body"]));\n',
                timeout=15,
            )
            assert redirect_client.returncode == 0, redirect_client.stderr
            assert redirect_client.stdout.splitlines() == [
                "b", "method=GET", "method=POST body=payload", "b",
                "next"], redirect_client.stdout
            redirect_loop = run(
                "import std;\n"
                f'std.http.get("http://127.0.0.1:{redirect_port}/loop", '
                '{"follow_redirects": true, "max_redirects": 2});\n'
            )
            assert "too many redirects" in redirect_loop.stderr, (
                redirect_loop.stderr)
        finally:
            redirect_server.shutdown()
            redirect_server.server_close()
            redirect_thread.join(timeout=5)

        class CookieHandler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def _send(self, code, body=b"", cookies=None):
                self.send_response(code)
                for cookie in (cookies or []):
                    self.send_header("Set-Cookie", cookie)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/set":
                    self._send(200, b"set", [
                        "a=1; Path=/",
                        "b=2; Path=/admin",
                        "c=3; HttpOnly; Path=/",
                        "s=4; Secure; Path=/",
                        "d=5; Domain=127.0.0.1; Path=/",
                    ])
                elif self.path == "/check":
                    self._send(
                        200, self.headers.get("Cookie", "").encode())
                elif self.path == "/admin/check":
                    self._send(
                        200, self.headers.get("Cookie", "").encode())
                elif self.path == "/delete":
                    self._send(200, b"deleted",
                               ["a=; Max-Age=0; Path=/"])
                else:
                    self._send(404)

        cookie_server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), CookieHandler)
        cookie_port = cookie_server.server_address[1]
        cookie_thread = threading.Thread(
            target=cookie_server.serve_forever)
        cookie_thread.start()
        try:
            cookie_client = run(
                "import std;\n"
                "let jar = {};\n"
                f'let r = std.http.get('
                f'"http://127.0.0.1:{cookie_port}/set", '
                '{"cookie_jar": jar});\n'
                "let cs = std.http.cookies(r);\n"
                'print("count=" + str(len(cs)));\n'
                'print("check1=" + bytes_to_string(std.http.get('
                f'"http://127.0.0.1:{cookie_port}/check", '
                '{"cookie_jar": jar})["body"]));\n'
                'print("admin=" + bytes_to_string(std.http.get('
                f'"http://127.0.0.1:{cookie_port}/admin/check", '
                '{"cookie_jar": jar})["body"]));\n'
                f'let deleted = std.http.get('
                f'"http://127.0.0.1:{cookie_port}/delete", '
                '{"cookie_jar": jar});\n'
                'print("check2=" + bytes_to_string(std.http.get('
                f'"http://127.0.0.1:{cookie_port}/check", '
                '{"cookie_jar": jar})["body"]));\n'
                "let jar2 = {};\n"
                f'let ar = await std.http.get_async('
                f'"http://127.0.0.1:{cookie_port}/set", '
                '{"cookie_jar": jar2});\n'
                'print("acount=" + str(len(std.http.cookies(ar))));\n'
                f'let ac = await std.http.get_async('
                f'"http://127.0.0.1:{cookie_port}/check", '
                '{"cookie_jar": jar2});\n'
                'print("acheck=" + bytes_to_string(ac["body"]));\n',
                timeout=15,
            )
            assert cookie_client.returncode == 0, cookie_client.stderr
            assert cookie_client.stdout.splitlines() == [
                "count=5",
                "check1=a=1; c=3; d=5",
                "admin=b=2; a=1; c=3; d=5",
                "check2=c=3; d=5",
                "acount=5",
                "acheck=a=1; c=3; d=5",
            ], cookie_client.stdout
        finally:
            cookie_server.shutdown()
            cookie_server.server_close()
            cookie_thread.join(timeout=5)

        comp_probe = run(
            "import std;\n"
            'std.http.get("http://127.0.0.1:1/", '
            '{"decompress": true});\n'
        )
        compression_enabled = (
            "compression support is disabled" not in comp_probe.stderr)
        if compression_enabled:
            import gzip
            import zlib

            class CompressionHandler(http.server.BaseHTTPRequestHandler):
                protocol_version = "HTTP/1.1"

                def log_message(self, *args):
                    pass

                def _send(self, body, encoding=None):
                    self.send_response(200)
                    if encoding:
                        self.send_header("Content-Encoding", encoding)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)

                def do_GET(self):
                    if self.path == "/gzip":
                        self._send(gzip.compress(b"hello gzip"), "gzip")
                    elif self.path == "/deflate":
                        self._send(zlib.compress(b"hello deflate"), "deflate")
                    elif self.path == "/rawdeflate":
                        compressor = zlib.compressobj(wbits=-15)
                        self._send(
                            compressor.compress(b"hello raw") +
                            compressor.flush(), "deflate")
                    elif self.path == "/br":
                        self._send(b"\x8b\x03\x80hello br\x03", "br")
                    elif self.path == "/plain":
                        self._send(b"hello plain")
                    elif self.path == "/ae":
                        self._send(self.headers.get(
                            "Accept-Encoding", "").encode())
                    else:
                        self._send(b"missing")

            compression_server = http.server.ThreadingHTTPServer(
                ("127.0.0.1", 0), CompressionHandler)
            compression_port = compression_server.server_address[1]
            compression_thread = threading.Thread(
                target=compression_server.serve_forever)
            compression_thread.start()
            try:
                compression_client = run(
                    "import std;\n"
                    f'let a = std.http.get('
                    f'"http://127.0.0.1:{compression_port}/gzip", '
                    '{"decompress": true});\n'
                    'print(bytes_to_string(a["body"]) + " " + '
                    'str(a["decompressed"]));\n'
                    f'let b = std.http.get('
                    f'"http://127.0.0.1:{compression_port}/deflate", '
                    '{"decompress": true});\n'
                    'print(bytes_to_string(b["body"]));\n'
                    f'let c = std.http.get('
                    f'"http://127.0.0.1:{compression_port}/rawdeflate", '
                    '{"decompress": true});\n'
                    'print(bytes_to_string(c["body"]));\n'
                    f'let d = std.http.get('
                    f'"http://127.0.0.1:{compression_port}/br", '
                    '{"decompress": true});\n'
                    'print(bytes_to_string(d["body"]));\n'
                    f'let e = await std.http.get_async('
                    f'"http://127.0.0.1:{compression_port}/gzip", '
                    '{"decompress": true});\n'
                    'print(bytes_to_string(e["body"]));\n'
                    f'let f = std.http.get('
                    f'"http://127.0.0.1:{compression_port}/ae", '
                    '{"decompress": true});\n'
                    'print("ae=" + bytes_to_string(f["body"]));\n',
                    timeout=15,
                )
                assert compression_client.returncode == 0, (
                    compression_client.stderr)
                lines = compression_client.stdout.splitlines()
                assert lines[0] == "hello gzip true", lines
                assert lines[1] == "hello deflate", lines
                assert lines[2] == "hello raw", lines
                assert lines[3] == "hello br", lines
                assert lines[4] == "hello gzip", lines
                assert lines[5].startswith("ae="), lines
                assert "gzip" in lines[5] and "deflate" in lines[5], lines
                bad_compression = run(
                    "import std;\n"
                    f'std.http.get("http://127.0.0.1:{compression_port}/gzip", '
                    '{"decompress": true, "on_body": func(c) {}});\n'
                )
                assert "decompress cannot be combined with on_body" in (
                    bad_compression.stderr), bad_compression.stderr
            finally:
                compression_server.shutdown()
                compression_server.server_close()
                compression_thread.join(timeout=5)

        rb1_probe = socket.socket()
        rb1_probe.bind(("127.0.0.1", 0))
        rb1_port = rb1_probe.getsockname()[1]
        rb1_probe.close()
        rb1_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        try:
            rb1_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", {rb1_port});\n'
                's["on_request_body"] = func(chunk) {\n'
                '  print("rb=" + bytes_to_string(chunk));\n'
                "};\n"
                "let n = std.http.serve(s, func(req) {\n"
                '  return {"status": 200, "body": "handler_body=" + '
                'bytes_to_string(req["body"])};\n'
                "}, 1);\n"
                'print("served " + str(n));\n'
                "std.http.close(s);\n"
            )
            rb1_proc.stdin.close()
            deadline = time.time() + 5
            ready = False
            while time.time() < deadline:
                try:
                    probe = socket.create_connection(
                        ("127.0.0.1", rb1_port), timeout=0.2)
                    probe.close()
                    ready = True
                    break
                except OSError:
                    time.sleep(0.05)
            assert ready, "http1 request body server did not listen"
            rb1_client = run(
                "import std;\n"
                "func parts() {\n"
                '  yield "hello";\n'
                '  yield " ";\n'
                '  yield "world";\n'
                "}\n"
                f'let a = std.http.post('
                f'"http://127.0.0.1:{rb1_port}/a", '
                '{"body_stream": parts()});\n'
                'print(bytes_to_string(a["body"]));\n',
                timeout=15,
            )
            assert rb1_client.returncode == 0, rb1_client.stderr
            assert "handler_body=" in rb1_client.stdout, rb1_client.stdout
            rb1_proc.wait(timeout=10)
            rb1_output = rb1_proc.stdout.read()
            rb1_chunks = "".join(
                line[3:] for line in rb1_output.splitlines()
                if line.startswith("rb="))
            assert rb1_chunks == "hello world", rb1_output
            assert "served 1" in rb1_output, rb1_output
        finally:
            if rb1_proc.poll() is None:
                rb1_proc.terminate()
                rb1_proc.wait(timeout=5)

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
        "std json parse",
        r'''import std;
let v = std.json.parse("{\"a\": 1, \"b\": [true, null, \"x\"], \"c\": 1.5}");
print(v["a"]);
print(v["b"][0]);
print(v["b"][1]);
print(v["b"][2]);
print(v["c"]);
''',
        stdout_all=["1", "true", "null", "x", "1.5"],
    )
    check(
        "std json stringify",
        r'''import std;
std.json.stringify({"a": 1, "b": [true, null, "x"], "c": 1.5})
std.json.stringify({"s": "a\"b\n\\c"})
std.json.stringify(1.0)
''',
        stdout_all=[r'= {"a":1,"b":[true,null,"x"],"c":1.5}',
                    r'= {"s":"a\"b\n\\c"}', "= 1.0"],
    )
    check(
        "std json numbers",
        r'''import std;
std.type.kind_of(std.json.parse("1"))
std.type.kind_of(std.json.parse("1.0"))
std.type.kind_of(std.json.parse("9223372036854775808"))
std.json.parse("-9223372036854775808")
''',
        stdout_all=["= int", "= float", "= float",
                    "= -9223372036854775808"],
    )
    check(
        "std json duplicate keys and unicode",
        r'''import std;
std.json.parse("{\"a\":1,\"a\":2}")["a"]
std.json.parse("\"\\u4f60\\u597d\"")
''',
        stdout_all=["= 2", "= 你好"],
    )
    check(
        "std json roundtrip",
        r'''import std;
let text = "{\"a\": [1, 2], \"b\": {\"c\": true}, \"s\": \"\\u4f60\"}";
std.json.stringify(std.json.parse(text))
''',
        stdout=r'= {"a":[1,2],"b":{"c":true},"s":"你"}',
    )
    check(
        "std json try_parse",
        'import std;\n'
        'let r = std.json.try_parse("{bad}");\n'
        'std.result.is_err(r)\n'
        'std.result.is_ok(std.json.try_parse("{\\"a\\":1}"))\n',
        stdout_all=["= true", "= true"],
    )
    check(
        "std json parse error",
        'import std;\nstd.json.parse("{\\"a\\":1")\n',
        stderr="JSON parse error",
    )
    check(
        "std json parse bytes",
        'import std;\n'
        'std.json.parse(bytes_from_string("{\\"a\\": 7}"))["a"]\n',
        stdout="= 7",
    )
    check(
        "std json parse type",
        "import std;\nstd.json.parse(1)\n",
        stderr="std.json.parse expects a string or bytes",
    )
    check(
        "std json parse invalid utf8",
        'import std;\nstd.json.parse(bytes_from_hex("ff"))\n',
        stderr="not valid UTF-8",
    )
    check(
        "std json stringify key error",
        "import std;\nstd.json.stringify({1: 2})\n",
        stderr="JSON object keys must be strings",
    )
    check(
        "std json stringify unsupported",
        'import std;\nstd.json.stringify({"a": bytes_from_hex("00")})\n',
        stderr="cannot encode bytes as JSON",
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
          'outer() on error(e) { print("caught " + str(e)); recover 7; }\n',
          stdout_all=["caught x", "= 7"])
    check("fail stops loop body",
          "func f() {\n"
          "  for (let i in [1, 2, 3]) {\n"
          '    if (i == 2) { fail "loop"; }\n'
          "    print(i);\n"
          "  }\n"
          "  return 0;\n"
          "}\n"
          'f() on error(e) { print("E " + str(e)); recover 9; }\n',
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
        stream_path = os.path.join(tmp, "stream.bin")
        quoted_stream = stream_path.replace("\\", "\\\\")
        stream_copy = os.path.join(tmp, "stream-copy.bin")
        quoted_stream_copy = stream_copy.replace("\\", "\\\\")
        check(
            "io file stream",
            f'import std;\n'
            f'let f = std.fs.open("{quoted_stream}", "w+b");\n'
            f'std.io.readable(f)\n'
            f'std.io.writable(f)\n'
            f'std.io.seekable(f)\n'
            f'std.io.write_all(f, "hello\\nworld\\n")\n'
            f'std.io.flush(f)\n'
            f'std.fs.seek(f, 0, 0)\n'
            f'bytes_to_string(std.io.read(f, 5))\n'
            f'std.fs.seek(f, 6, 0)\n'
            f'bytes_to_string(std.io.read_line(f))\n'
            f'std.fs.seek(f, 0, 0)\n'
            f'len(std.io.read_all(f, 100))\n'
            f'std.io.eof(f)\n'
            f'std.fs.seek(f, 0, 0)\n'
            f'bytes_to_string(std.io.read_exact(f, 5))\n'
            f'std.io.close(f)\n'
            f'std.io.close(f)\n',
            stdout_all=[
                "= true", "= true", "= true", "= 12",
                "= hello", "= world", "= 12", "= true",
                "= hello", "= true", "= false",
            ],
        )
        check(
            "io file eof read",
            f'import std;\n'
            f'let f = std.fs.open("{quoted_stream}", "rb");\n'
            f'len(std.io.read(f, 100))\n'
            f'std.io.eof(f)\n'
            f'std.io.read(f, 100) == null\n'
            f'std.io.close(f)\n',
            stdout_all=["= 12", "= true", "= true", "= true"],
        )
        check(
            "io copy",
            f'import std;\n'
            f'let src = std.fs.open("{quoted_stream}", "rb");\n'
            f'let dst = std.fs.open("{quoted_stream_copy}", "w+b");\n'
            f'std.io.copy(dst, src)\n'
            f'std.io.close(dst)\n'
            f'std.io.close(src)\n'
            f'let check = std.fs.open("{quoted_stream_copy}", "rb");\n'
            f'bytes_to_string(std.io.read_all(check, 100)) == "hello\\nworld\\n"\n'
            f'std.io.close(check)\n',
            stdout_all=["= 12", "= true", "= true", "= true", "= true"],
        )
        check(
            "io buffered read",
            f'import std;\n'
            f'let f = std.fs.open("{quoted_stream}", "rb");\n'
            f'let r = std.io.buffered(f, 4);\n'
            f'bytes_hex(std.io.read(r, 2))\n'
            f'bytes_hex(std.io.read(r, 2))\n'
            f'bytes_hex(std.io.read(r, 2))\n'
            f'bytes_hex(std.io.read(r, 2))\n'
            f'bytes_hex(std.io.read(r, 2))\n'
            f'bytes_hex(std.io.read(r, 2))\n'
            f'std.io.read(r, 2) == null\n'
            f'std.io.eof(r)\n'
            f'std.io.close(r)\n',
            stdout_all=[
                "= 6865", "= 6c6c", "= 6f0a", "= 776f", "= 726c",
                "= 640a", "= true", "= true", "= true",
            ],
        )
        check(
            "io buffered write close flush",
            f'import std;\n'
            f'let f = std.fs.open("{quoted_stream_copy}", "w+b");\n'
            f'let w = std.io.buffered(f, 4);\n'
            f'std.io.write_all(w, "buffered data")\n'
            f'std.io.close(w)\n'
            f'let check = std.fs.open("{quoted_stream_copy}", "rb");\n'
            f'bytes_to_string(std.io.read_all(check, 100))\n'
            f'std.io.close(check)\n',
            stdout_all=["= 13", "= true", "= buffered data", "= true"],
        )
        check(
            "io seek tell",
            f'import std;\n'
            f'let f = std.fs.open("{quoted_stream}", "rb");\n'
            f'std.io.tell(f)\n'
            f'std.io.seek(f, 6, 0)\n'
            f'std.io.tell(f)\n'
            f'bytes_to_string(std.io.read_exact(f, 1))\n'
            f'std.io.seek(f, 0, 0)\n'
            f'std.io.tell(f)\n'
            f'std.io.close(f)\n',
            stdout_all=["= 0", "= 6", "= 6", "= w", "= 0", "= 0", "= true"],
        )
        check(
            "io buffered seek tell",
            f'import std;\n'
            f'let f = std.fs.open("{quoted_stream}", "rb");\n'
            f'let b = std.io.buffered(f, 4);\n'
            f'std.io.tell(b)\n'
            f'bytes_to_string(std.io.read(b, 2))\n'
            f'std.io.tell(b)\n'
            f'std.io.seek(b, 6, 0)\n'
            f'std.io.tell(b)\n'
            f'bytes_to_string(std.io.read_exact(b, 1))\n'
            f'std.io.close(b)\n',
            stdout_all=["= 0", "= he", "= 2", "= 6", "= 6", "= w", "= true"],
        )
        check(
            "io file async",
            f'import std;\n'
            f'let w = std.fs.open("{quoted_stream_copy}", "w+b");\n'
            f'await std.io.write_async(w, "file async data")\n'
            f'std.io.close(w)\n'
            f'let r = std.fs.open("{quoted_stream_copy}", "rb");\n'
            f'bytes_to_string(await std.io.read_async(r, 4))\n'
            f'len(await std.io.read_async(r, 100))\n'
            f'await std.io.read_async(r, 100) == null\n'
            f'std.io.eof(r)\n'
            f'std.io.close(r)\n',
            stdout_all=[
                "= 15", "= true", "= file", "= 11", "= true",
                "= true", "= true",
            ],
        )
        check(
            "io file async pending",
            f'import std;\n'
            f'let f = std.fs.open("{quoted_stream}", "rb");\n'
            f'let t = std.io.read_async(f, 4);\n'
            f'std.io.read(f, 1)\n',
            stderr="pending async operation",
        )
        check(
            "io stream type",
            "import std;\nstd.io.read(42, 10)\n",
            stderr="std.io.read expects a stream",
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
            f'f["write"]("abcdef");\n'
            f'f["seek"](0);\n'
            f'f["read"](3);\n'
            f'f["close"]();\n',
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

        # The entry package may be named main in any directory.
        main_anywhere = write(
            "mainpkg/anywhere/entry.calc",
            "package main {\n"
            "  export let x = 7;\n"
            "}\n"
            "print(main.x);\n",
        )
        proc = run_file(main_anywhere)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "7\n", repr(proc.stdout)

        # A file entry may only use package main.
        entry_named = write(
            "entrypkg/entry/entry.calc",
            "package entry { export let x = 3; }\n"
            "print(entry.x);\n",
        )
        proc = run_file(entry_named)
        assert proc.returncode == 1, proc.stderr
        assert "entry package must be \"main\"" in proc.stderr, proc.stderr

        # stdin/REPL have no file package identity, so scratch names still work.
        proc = run(
            "package scratch { export let x = 3; }\n"
            "print(scratch.x);\n"
        )
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "3\n", repr(proc.stdout)

        # main is reserved for the entry package and cannot be imported.
        reserved_import = write("reservedimport/main.calc", "import main;\n")
        proc = run_file(reserved_import)
        assert proc.returncode == 1, proc.stderr
        assert "reserved for the entry script" in proc.stderr, proc.stderr

        reserved_import_sub = write(
            "reservedimport/sub.calc", "import main.sub;\n")
        proc = run_file(reserved_import_sub)
        assert proc.returncode == 1, proc.stderr
        assert "reserved for the entry script" in proc.stderr, proc.stderr

        # A normal package cannot declare itself as main.
        write(
            "reservedmain/aa/bb/bb.calc",
            "package main { export let x = 1; }\n",
        )
        reserved_main = write(
            "reservedmain/main.calc",
            "import aa.bb;\n",
        )
        proc = run_file(reserved_main)
        assert proc.returncode == 1, proc.stderr
        assert "reserved for the entry script" in proc.stderr, proc.stderr

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
