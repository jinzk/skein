#!/usr/bin/env python3
"""Golden/smoke tests for the skein interpreter."""

import base64
import hashlib
import os
import random
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
import urllib.request


BIN = sys.argv[1] if len(sys.argv) > 1 else "./skein"
FFI_SHIM = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else None
DEFAULT_TIMEOUT = float(os.environ.get("SKEIN_TEST_TIMEOUT", "5"))


def _find_native_module():
    if not BIN:
        return None
    directory = os.path.dirname(os.path.abspath(BIN))
    for name in ("libskein_native_module.so",
                 "libskein_native_module.dylib",
                 "skein_native_module.dll"):
        candidate = os.path.join(directory, name)
        if os.path.exists(candidate):
            return candidate
    return None


NATIVE_MODULE = _find_native_module()


def skein_string(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def run(source, timeout=None, env=None):
    if timeout is None:
        timeout = DEFAULT_TIMEOUT
    try:
        proc = subprocess.run(
            [BIN],
            input=source.encode("utf-8"),
            capture_output=True,
            timeout=timeout,
            env=env,
        )
        proc.stdout = proc.stdout.decode("utf-8", errors="replace")
        proc.stderr = proc.stderr.decode("utf-8", errors="replace")
        # The Windows CRT translates "\n" to "\r\n" on text-mode pipes.
        # Goldens are written with LF, so normalize here.
        proc.stdout = proc.stdout.replace("\r\n", "\n")
        proc.stderr = proc.stderr.replace("\r\n", "\n")
        return proc
    except subprocess.TimeoutExpired as exc:
        raise AssertionError(f"interpreter timed out for input {source!r}") from exc


def run_file(path, timeout=None):
    if timeout is None:
        timeout = DEFAULT_TIMEOUT
    return subprocess.run(
        [BIN, path],
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=timeout,
    )


TEST_SKIP = {
    item.strip()
    for item in os.environ.get("SKEIN_TEST_SKIP", "").split(",")
    if item.strip()
}
TEST_SKIP_PREFIXES = tuple(
    item.strip()
    for item in os.environ.get("SKEIN_TEST_SKIP_PREFIX", "").split(",")
    if item.strip()
)
TEST_SKIP_SOURCE = os.environ.get("SKEIN_TEST_SKIP_SOURCE", "")


def check(name, source, *, stdout=None, stdout_all=None, stderr=None,
          not_stderr=None, code=0, env=None):
    if name in TEST_SKIP:
        return
    if TEST_SKIP_PREFIXES and name.startswith(TEST_SKIP_PREFIXES):
        return
    if TEST_SKIP_SOURCE and TEST_SKIP_SOURCE in source:
        return
    proc = run(source, env=env)
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


def check_encoding_two_byte_tables():
    """Round-trip every two-byte GBK/GB18030 table position."""
    for encoding in ("gbk", "gb18030"):
        lines = ["import std;"]
        expected = []
        for lead in range(0x81, 0xFF):
            for trail in list(range(0x40, 0x7F)) + list(range(0x80, 0xFF)):
                data = bytes((lead, trail))
                try:
                    data.decode(encoding)
                except UnicodeDecodeError:
                    continue
                hex_data = data.hex()
                lines.append(
                    f'bytes_hex(std.encoding.encode('
                    f'std.encoding.decode(bytes_from_hex("{hex_data}"), '
                    f'"{encoding}"), "{encoding}"))'
                )
                expected.append("= " + hex_data)
        proc = run("\n".join(lines) + "\n", timeout=30)
        assert proc.returncode == 0, proc.stderr
        actual = [line.strip() for line in proc.stdout.splitlines()
                  if line.strip()]
        assert actual == expected, f"{encoding} table round-trip mismatch"


def check_unicode_normalization_samples():
    """Compare std.unicode against Python's Unicode 16 normalization."""
    rng = random.Random(2026)
    assigned = [
        cp for cp in range(0x110000)
        if not (0xD800 <= cp <= 0xDFFF)
        and unicodedata.category(chr(cp)) != "Cn"
    ]
    samples = [chr(cp) for cp in rng.sample(assigned, 40)]
    marks = list(range(0x300, 0x370))
    for _ in range(20):
        base = rng.choice((0x41, 0x61, 0x65, 0x4E2D, 0x1100))
        samples.append(chr(base) + chr(rng.choice(marks)))
    for _ in range(10):
        lead = rng.randint(0x1100, 0x1112)
        vowel = rng.randint(0x1161, 0x1175)
        tail = rng.choice((0, rng.randint(0x11A8, 0x11C2)))
        samples.append(chr(lead) + chr(vowel) + (chr(tail) if tail else ""))

    forms = ("nfc", "nfd", "nfkc", "nfkd")
    lines = ["import std;"]
    expected = []
    for index, sample in enumerate(samples):
        hex_data = sample.encode("utf-8").hex()
        lines.append(
            f'let sample{index} = std.encoding.decode('
            f'bytes_from_hex("{hex_data}"), "utf-8");'
        )
        for form in forms:
            lines.append(
                f'bytes_hex(std.encoding.encode('
                f'std.unicode.normalize(sample{index}, "{form}"), "utf-8"))'
            )
            expected.append(
                "= " + unicodedata.normalize(
                    form.upper(), sample).encode("utf-8").hex()
            )

    proc = run("\n".join(lines) + "\n", timeout=30)
    assert proc.returncode == 0, proc.stderr
    actual = [line.strip() for line in proc.stdout.splitlines()
              if line.strip()]
    assert actual == expected, "unicode normalization mismatch"


def check_extended_stdlib():
    check(
        "std regex basics",
        "import std;\n"
        'std.regex.match("a+", "aaa")\n'
        'std.regex.match("abc", "ABC", "i")\n'
        'let m = std.regex.search("(a+)(b)", "xxaaabyy");\n'
        'print(m["text"]);\n'
        'print(m["start"]);\n'
        'print(m["groups"][1]);\n'
        'print(std.regex.replace("(a+)", "xxaaabyy", "[$1]"));\n'
        'print(std.regex.split(",", "a,b,c"));\n'
        'len(std.regex.find_all("[0-9]+", "a1b22c333"))\n',
        stdout_all=["= true", "= true", "aaab", "2", "b",
                    "xx[aaa]byy", "[a, b, c]", "= 3"],
    )
    check(
        "std regex unicode mode",
        "import std;\n"
        'std.regex.match(".", "你", "u")\n'
        'std.regex.match("...", "你", "u")\n'
        'std.regex.match("...", "你")\n'
        'std.regex.match("\\\\p{L}+", "中文", "u")\n'
        'std.regex.match("\\\\x{4f60}", "你", "u")\n'
        'std.regex.match("\\\\w+", "中文", "u")\n'
        'std.regex.match("\\\\d+", "１２３", "u")\n'
        'std.regex.match("é", "É", "iu")\n'
        'std.regex.match("a|ab", "ab")\n'
        'len(std.regex.find_all(".", "a你b", "u"))\n'
        'len(std.regex.find_all(".", "a你b"))\n'
        'len(std.regex.find_all("", "ab"))\n'
        'let u = std.regex.search("(\\\\p{L}+)", "123中文456", "u");\n'
        'print(u["text"]);\n'
        'print(u["start"]);\n'
        'print(u["end"]);\n'
        'print(u["groups"][0]);\n'
        'print(std.regex.replace("(\\\\p{Han}+)", "a你b", "[$1]", "u"));\n'
        'print(std.regex.split("\\\\s+", "a\u3000b", "u"));\n'
        'let xs = std.regex.find_all("你", "a你b你c", "u");\n'
        'xs[1]["start"]\n'
        'xs[1]["end"]\n'
        'let b = std.regex.search("你", "a你b");\n'
        'b["start"]\n'
        'b["end"]\n',
        stdout_all=["= true", "= false", "= true", "= true", "= true",
                    "= true", "= true", "= true", "= true", "= 3", "= 5",
                    "= 3", "中文", "= 3", "= 5", "中文", "a[你]b",
                    "[a, b]", "= 3", "= 4", "= 1", "= 4"],
    )
    check(
        "std regex invalid pattern",
        'import std;\nstd.regex.match("(", "x")\n',
        stderr="invalid pattern",
    )
    check(
        "std regex invalid flag",
        'import std;\nstd.regex.match("a", "a", "z")\n',
        stderr="unknown regex flag 'z'",
    )

    check(
        "std time basics",
        "import std;\n"
        'std.time.format(0, "%Y-%m-%d %H:%M:%S", true)\n'
        'std.time.parse("1970-01-01 00:00:00", '
        '"%Y-%m-%d %H:%M:%S", true)\n'
        'std.time.now() > 0\n'
        'std.time.monotonic_ms() >= 0\n'
        'print(std.time.sleep(0))\n',
        stdout_all=["= 1970-01-01 00:00:00", "= 0", "= true", "= true",
                    "null"],
    )
    check(
        "std time invalid parse",
        'import std;\nstd.time.parse("not-a-time")\n',
        stderr="does not match format",
    )

    check(
        "std random basics",
        "import std;\n"
        "std.random.seed(123);\n"
        'std.random.int(5, 5)\n'
        'let f = std.random.float();\n'
        'print(f >= 0.0 && f < 1.0);\n'
        'bytes_len(std.random.bytes(8))\n'
        'std.random.choice([7])\n'
        'len(std.random.shuffle([1, 2, 3]))\n'
        'len(std.random.uuid())\n',
        stdout_all=["= 5", "true", "= 8", "= 7", "= 3", "= 36"],
    )
    check(
        "std random invalid range",
        "import std;\nstd.random.int(2, 1)\n",
        stderr="minimum must not exceed maximum",
    )

    check(
        "std encoding base64 and url",
        "import std;\n"
        'std.encoding.base64_encode(bytes_from_string("hello"))\n'
        'bytes_to_string(std.encoding.base64_decode("aGVsbG8="))\n'
        'std.encoding.url_encode("a b/中")\n'
        'std.encoding.url_decode("a%20b%2F%E4%B8%AD")\n',
        stdout_all=["= aGVsbG8=", "= hello", "= a%20b%2F%E4%B8%AD",
                    "= a b/中"],
    )
    check(
        "std encoding invalid base64",
        'import std;\nstd.encoding.base64_decode("!")\n',
        stderr="invalid base64",
    )

    check(
        "std env basics",
        "import std;\n"
        'std.env.set("SKEIN_TEST_ENV", "abc");\n'
        'std.env.get("SKEIN_TEST_ENV")\n'
        'std.env.has("SKEIN_TEST_ENV")\n'
        'std.env.set("SKEIN_TEST_ENV", "changed");\n'
        'std.env.get("SKEIN_TEST_ENV")\n'
        'std.env.unset("SKEIN_TEST_ENV");\n'
        'std.env.has("SKEIN_TEST_ENV")\n'
        'std.env.get("SKEIN_TEST_ENV", "fallback")\n',
        stdout_all=["= abc", "= true", "= changed", "= false",
                    "= fallback"],
    )

    check(
        "std crypto sha256",
        "import std;\n"
        'std.crypto.sha256("abc")\n'
        'std.crypto.sha256("")\n'
        'std.crypto.hmac_sha256("key", '
        '"The quick brown fox jumps over the lazy dog")\n',
        stdout_all=[
            "= ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "= e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "= f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
        ],
    )

    with tempfile.TemporaryDirectory() as tmp:
        base = tmp.replace("\\", "\\\\").replace('"', '\\"')
        host_sep = "\\" if sys.platform == "win32" else "/"
        host_join = f"{tmp}{host_sep}a{host_sep}b{host_sep}x.txt"
        check(
            "std fs directory and path helpers",
            "import std;\n"
            f'std.fs.mkdir("{base}/a/b", true);\n'
            f'std.fs.write_file("{base}/a/b/x.txt", "hi");\n'
            f'print(std.fs.listdir("{base}/a/b"));\n'
            f'print(std.fs.stat("{base}/a/b/x.txt")["type"]);\n'
            f'print(std.fs.stat("{base}/a/b/x.txt")["size"]);\n'
            f'print(std.fs.path_join("{base}", "a", "b", "x.txt"));\n'
            f'print(std.fs.path_basename("{base}/a/b/x.txt"));\n'
            f'print(std.fs.path_dirname("{base}/a/b/x.txt"));\n'
            f'print(std.fs.path_extname("{base}/a/b/x.txt"));\n'
            f'print(std.fs.path_normalize("{base}/a/../a/b/./x.txt"));\n'
            f'std.fs.rename("{base}/a/b/x.txt", "{base}/a/b/y.txt");\n'
            f'print(std.fs.exists("{base}/a/b/y.txt"));\n'
            f'std.fs.remove("{base}/a/b/y.txt");\n'
            f'std.fs.exists("{base}/a/b/y.txt")\n',
            stdout_all=["[x.txt]", "file", "2",
                        host_join, "x.txt", f"{tmp}/a/b",
                        ".txt", host_join, "1", "= 0"],
        )

    if sys.platform != "win32":
        check(
            "std process run",
            "import std;\n"
            'let r = std.process.run("printf hello");\n'
            'print(r["status"]);\n'
            'print(bytes_to_string(r["stdout"]));\n'
            'let r2 = std.process.run("cat", {"stdin": "abc"});\n'
            'print(bytes_to_string(r2["stdout"]));\n'
            'let r3 = std.process.run("pwd", {"cwd": "/tmp"});\n'
            'print(bytes_to_string(r3["stdout"]));\n'
            'let r4 = std.process.run("printf %s \\"$FOO\\"", '
            '{"env": {"FOO": "bar"}});\n'
            'print(bytes_to_string(r4["stdout"]));\n'
            'let r5 = std.process.run("echo out; echo err >&2; exit 3");\n'
            'print(r5["status"]);\n'
            'print(bytes_to_string(r5["stdout"]));\n'
            'print(bytes_to_string(r5["stderr"]));\n',
            stdout_all=["0", "hello", "abc", "/tmp", "bar", "3", "out",
                        "err"],
        )
        check(
            "std process spawn stdout",
            "import std;\n"
            'let p = std.process.spawn("/bin/echo", {"args": ["spawn"]});\n'
            'print(std.process.pid(p) > 0);\n'
            'print(bytes_to_string(std.io.read(p["stdout"], 4096)));\n'
            'print(std.process.wait(p));\n',
            stdout_all=["true", "spawn", "0"],
        )
        check(
            "std process spawn stdin",
            "import std;\n"
            'let p = std.process.spawn("/bin/cat");\n'
            'std.io.write_all(p["stdin"], "abc");\n'
            'std.io.close(p["stdin"]);\n'
            'print(bytes_to_string(std.io.read(p["stdout"], 4096)));\n'
            'print(std.process.wait(p));\n',
            stdout_all=["abc", "0"],
        )
        check(
            "std process spawn stderr",
            "import std;\n"
            'let p = std.process.spawn("/bin/sh", '
            '{"args": ["-c", "echo out; echo err >&2; exit 3"]});\n'
            'print(bytes_to_string(std.io.read(p["stdout"], 4096)));\n'
            'print(bytes_to_string(std.io.read(p["stderr"], 4096)));\n'
            'print(std.process.wait(p));\n',
            stdout_all=["out", "err", "3"],
        )
        check(
            "std process spawn async",
            "import std;\n"
            'let p = std.process.spawn("/bin/echo", {"args": ["async"]});\n'
            'let data = await std.io.read_async(p["stdout"], 4096);\n'
            'print(bytes_to_string(data));\n'
            'print(std.process.wait(p));\n',
            stdout_all=["async", "0"],
        )
        check(
            "std process spawn async write",
            "import std;\n"
            'let p = std.process.spawn("/bin/cat");\n'
            'print(await std.io.write_async(p["stdin"], "abc"));\n'
            'std.io.close(p["stdin"]);\n'
            'let data = await std.io.read_async(p["stdout"], 4096);\n'
            'print(bytes_to_string(data));\n'
            'print(std.process.wait(p));\n',
            stdout_all=["3", "abc", "0"],
        )
        check(
            "std process spawn repeated close",
            "import std;\n"
            "for (let i = 0; i < 5; i++) {\n"
            '  let p = std.process.spawn("/bin/true");\n'
            "  std.process.wait(p);\n"
            "  std.process.close(p);\n"
            "}\n"
            "1\n",
            stdout="= 1",
        )
        check(
            "std process spawn kill",
            "import std;\n"
            'let p = std.process.spawn("/bin/sleep", {"args": ["5"]});\n'
            'std.process.kill(p);\n'
            'print(std.process.wait(p) > 0);\n',
            stdout="true",
        )
        check(
            "std io standard streams",
            "import std;\n"
            'std.io.write_all(std.io.stdout, "hello\\n");\n'
            'std.io.flush(std.io.stdout);\n',
            stdout="hello",
        )

    with tempfile.TemporaryDirectory() as worker_tmp:
        worker_script = os.path.join(worker_tmp, "worker.skein")
        with open(worker_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "std.io.write_all(std.io.stdout, bytes_from_string("
                "\"{\\\"ready\\\":true,\\\"version\\\":1}\\n\"));\n"
                "std.io.flush(std.io.stdout);\n"
                "while (true) {\n"
                "  let line = std.io.read_line(std.io.stdin);\n"
                "  if (line == null) { break; }\n"
                "  let request = std.json.parse(bytes_to_string(line));\n"
                "  let result = request[\"params\"][0] + "
                "request[\"params\"][1];\n"
                "  let response = {\"id\": request[\"id\"], "
                "\"ok\": true, \"result\": result};\n"
                "  std.io.write_all(std.io.stdout, bytes_from_string("
                "std.json.stringify(response) + \"\\n\"));\n"
                "  std.io.flush(std.io.stdout);\n"
                "}\n"
            )
        worker_path = worker_script.replace("\\", "\\\\")
        binary_path = os.path.abspath(BIN).replace("\\", "\\\\")
        check(
            "json lines worker over pipes",
            "import std;\n"
            f'let p = std.process.spawn("{binary_path}", '
            f'{{"args": ["{worker_path}"]}});\n'
            'print(bytes_to_string(std.io.read_line(p["stdout"])));\n'
            'std.io.write_all(p["stdin"], bytes_from_string('
            '"{\\"id\\":1,\\"params\\":[2,3]}\\n"));\n'
            'std.io.flush(p["stdin"]);\n'
            'let line = std.io.read_line(p["stdout"]);\n'
            'print(bytes_to_string(line));\n'
            'std.io.close(p["stdin"]);\n'
            'print(std.process.wait(p));\n',
            stdout_all=['{"ready":true,"version":1}',
                        '{"id":1,"ok":true,"result":5}', "0"],
        )
        check(
            "std.worker pool call and map",
            "import std;\n"
            f'let pool = std.worker.pool(2, "{worker_path}");\n'
            'let one = std.worker.call(pool, "add", [2, 3]);\n'
            'print(std.result.unwrap(one));\n'
            'print(std.worker.map(pool, "add", '
            '[[1, 2], [3, 4], [5, 6]]));\n'
            'std.worker.shutdown(pool);\n',
            stdout_all=["5", "[3, 7, 11]"],
        )
        restart_script = os.path.join(worker_tmp, "restart.skein")
        with open(restart_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "std.io.write_all(std.io.stdout, bytes_from_string("
                "\"{\\\"ready\\\":true,\\\"version\\\":1}\\n\"));\n"
                "std.io.flush(std.io.stdout);\n"
                "while (true) {\n"
                "  let line = std.io.read_line(std.io.stdin);\n"
                "  if (line == null) { break; }\n"
                "  let request = std.json.parse(bytes_to_string(line));\n"
                "  let method = request[\"method\"];\n"
                "  if (method == \"die\") { break; }\n"
                "  if (method == \"slow\") { std.time.sleep(500); }\n"
                "  let result = request[\"params\"][0] + "
                "request[\"params\"][1];\n"
                "  let response = {\"id\": request[\"id\"], "
                "\"ok\": true, \"result\": result};\n"
                "  std.io.write_all(std.io.stdout, bytes_from_string("
                "std.json.stringify(response) + \"\\n\"));\n"
                "  std.io.flush(std.io.stdout);\n"
                "}\n"
            )
        restart_path = restart_script.replace("\\", "\\\\")
        check(
            "std.worker submit and wait",
            "import std;\n"
            f'let pool = std.worker.pool(2, "{worker_path}");\n'
            'let a = std.worker.submit(pool, "add", [1, 2]);\n'
            'let b = std.worker.submit(pool, "add", [3, 4]);\n'
            'let c = std.worker.submit(pool, "add", [5, 6]);\n'
            'print(std.result.unwrap(std.worker.wait(a)));\n'
            'print(std.result.unwrap(std.worker.wait(b)));\n'
            'print(std.result.unwrap(std.worker.wait(c)));\n'
            'let stats = std.worker.stats(pool);\n'
            'print(stats["workers"]);\n'
            'print(stats["available"]);\n'
            'std.worker.shutdown(pool);\n',
            stdout_all=["3", "7", "11", "2", "2"],
        )
        check(
            "std.worker crash and timeout restart",
            "import std;\n"
            f'let pool = std.worker.pool(1, "{restart_path}");\n'
            'print(std.result.is_err('
            'std.worker.call(pool, "die", [1, 2])));\n'
            'print(std.result.unwrap('
            'std.worker.call(pool, "add", [2, 3])));\n'
            'print(std.result.is_err('
            'std.worker.call(pool, "slow", [1, 2], 100)));\n'
            'print(std.result.unwrap('
            'std.worker.call(pool, "add", [4, 5])));\n'
            'std.worker.shutdown(pool);\n',
            stdout_all=["true", "5", "true", "9"],
        )
        hybrid_script = os.path.join(worker_tmp, "hybrid_worker.skein")
        with open(hybrid_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "std.io.write_all(std.io.stdout, bytes_from_string("
                "\"{\\\"ready\\\":true,\\\"version\\\":1,"
                "\\\"protocol\\\":\\\"hybrid\\\"}\\n\"));\n"
                "std.io.flush(std.io.stdout);\n"
                "while (true) {\n"
                "  let line = std.io.read_line(std.io.stdin);\n"
                "  if (line == null) { break; }\n"
                "  let request = std.json.parse(bytes_to_string(line));\n"
                "  let body = bytes_from_string(\"\");\n"
                "  if (request[\"body_len\"] > 0) {\n"
                "    body = std.io.read_exact(std.io.stdin, "
                "request[\"body_len\"]);\n"
                "  }\n"
                "  let response = {\"id\": request[\"id\"], "
                "\"ok\": true, \"result\": request[\"params\"][0], "
                "\"body_len\": len(body)};\n"
                "  std.io.write_all(std.io.stdout, bytes_from_string("
                "std.json.stringify(response) + \"\\n\"));\n"
                "  std.io.write_all(std.io.stdout, body);\n"
                "  std.io.flush(std.io.stdout);\n"
                "}\n"
            )
        hybrid_path = hybrid_script.replace("\\", "\\\\")
        check(
            "std.worker hybrid call_bytes",
            "import std;\n"
            f'let pool = std.worker.pool(1, "{hybrid_path}", '
            '{"protocol": "hybrid"});\n'
            'let reply = std.result.unwrap(std.worker.call_bytes('
            'pool, "echo", [7], bytes_from_string("raw-body-123")));\n'
            'print(reply["value"]);\n'
            'print(bytes_to_string(reply["body"]));\n'
            'std.worker.shutdown(pool);\n',
            stdout_all=["7", "raw-body-123"],
        )
        check(
            "std.worker hybrid submit_bytes",
            "import std;\n"
            f'let pool = std.worker.pool(1, "{hybrid_path}", '
            '{"protocol": "hybrid"});\n'
            'let job = std.worker.submit_bytes('
            'pool, "echo", [9], bytes_from_string("two"));\n'
            'let reply = std.result.unwrap(std.worker.wait(job));\n'
            'print(reply["value"]);\n'
            'print(bytes_to_string(reply["body"]));\n'
            'std.worker.shutdown(pool);\n',
            stdout_all=["9", "two"],
        )
        serve_worker_script = os.path.join(
            worker_tmp, "serve_worker.skein")
        with open(serve_worker_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "std.io.write_all(std.io.stdout, bytes_from_string("
                "\"{\\\"ready\\\":true,\\\"version\\\":1,"
                "\\\"protocol\\\":\\\"hybrid\\\"}\\n\"));\n"
                "std.io.flush(std.io.stdout);\n"
                "while (true) {\n"
                "  let line = std.io.read_line(std.io.stdin);\n"
                "  if (line == null) { break; }\n"
                "  let request = std.json.parse(bytes_to_string(line));\n"
                "  let body = bytes_from_string(\"\");\n"
                "  if (request[\"body_len\"] > 0) {\n"
                "    body = std.io.read_exact(std.io.stdin, "
                "request[\"body_len\"]);\n"
                "  }\n"
                "  let result = {\"status\": 200, "
                "\"headers\": {\"Content-Type\": \"text/plain\"}};\n"
                "  let response = {\"id\": request[\"id\"], "
                "\"ok\": true, \"result\": result, "
                "\"body_len\": len(body)};\n"
                "  std.io.write_all(std.io.stdout, bytes_from_string("
                "std.json.stringify(response) + \"\\n\"));\n"
                "  std.io.write_all(std.io.stdout, body);\n"
                "  std.io.flush(std.io.stdout);\n"
                "}\n"
            )
        serve_worker_path = serve_worker_script.replace("\\", "\\\\")
        serve_script = os.path.join(worker_tmp, "serve_workers.skein")
        with open(serve_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                f"println(std.http.serve_workers(s, 1, "
                f"\"{serve_worker_path}\", 1));\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), serve_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "serve_workers did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            request = urllib.request.Request(
                f"http://127.0.0.1:{port_line}/echo",
                data=b"hello-body",
                method="POST",
            )
            with urllib.request.urlopen(request, timeout=10) as response:
                assert response.status == 200
                assert response.read() == b"hello-body"
            out, err = server.communicate(timeout=10)
            assert server.returncode == 0, err
            assert "1" in out
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        slow_script = os.path.join(
            worker_tmp, "slow_hybrid_worker.skein")
        with open(slow_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "std.io.write_all(std.io.stdout, bytes_from_string("
                "\"{\\\"ready\\\":true,\\\"version\\\":1,"
                "\\\"protocol\\\":\\\"hybrid\\\"}\\n\"));\n"
                "std.io.flush(std.io.stdout);\n"
                "while (true) {\n"
                "  let line = std.io.read_line(std.io.stdin);\n"
                "  if (line == null) { break; }\n"
                "  let request = std.json.parse(bytes_to_string(line));\n"
                "  let body = bytes_from_string(\"\");\n"
                "  if (request[\"body_len\"] > 0) {\n"
                "    body = std.io.read_exact(std.io.stdin, "
                "request[\"body_len\"]);\n"
                "  }\n"
                "  std.time.sleep(300);\n"
                "  let result = {\"status\": 200, "
                "\"headers\": {\"Content-Type\": \"text/plain\"}};\n"
                "  let response = {\"id\": request[\"id\"], "
                "\"ok\": true, \"result\": result, "
                "\"body_len\": len(body)};\n"
                "  std.io.write_all(std.io.stdout, bytes_from_string("
                "std.json.stringify(response) + \"\\n\"));\n"
                "  std.io.write_all(std.io.stdout, body);\n"
                "  std.io.flush(std.io.stdout);\n"
                "}\n"
            )
        slow_path = slow_script.replace("\\", "\\\\")
        concurrent_script = os.path.join(
            worker_tmp, "serve_workers_concurrent.skein")
        with open(concurrent_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                "s[\"worker_timeout_ms\"] = 10000;\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                f"println(std.http.serve_workers(s, 2, "
                f"\"{slow_path}\", 2));\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), concurrent_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "serve_workers did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            concurrent_results = []

            def concurrent_hit(index):
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port_line}/r{index}",
                    data=b"payload",
                    method="POST",
                )
                with urllib.request.urlopen(
                        request, timeout=10) as response:
                    concurrent_results.append(
                        (response.status, response.read()))

            threads = [
                threading.Thread(target=concurrent_hit, args=(index,))
                for index in range(2)
            ]
            started = time.monotonic()
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            elapsed = time.monotonic() - started
            assert len(concurrent_results) == 2
            assert all(status == 200 and body == b"payload"
                       for status, body in concurrent_results)
            assert elapsed < 0.55, (
                "serve_workers handled requests sequentially: "
                f"{elapsed:.3f}s"
            )
            out, err = server.communicate(timeout=10)
            assert server.returncode == 0, err
            assert "2" in out
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        check(
            "std.thread channel copy and close",
            "import std;\n"
            'let ch = std.thread.Channel(2);\n'
            'let msg = std.thread.Message.from({"x": 1, "items": [1, 2]});\n'
            'print("sent="); print(ch.try_send(msg));\n'
            'let r = ch.try_recv();\n'
            'print("x="); print(r["value"].payload["x"]);\n'
            'print("items="); print(r["value"].payload["items"]);\n'
            'ch.close();\n'
            'let r2 = ch.try_recv();\n'
            'print("empty_ok="); print(r2["ok"]);\n'
            'print("closed="); print(r2["closed"]);\n',
            stdout_all=["sent=true", "x=1", "items=[1, 2]",
                        "empty_ok=false", "closed=true"],
        )
        check(
            "std.thread message payload isolation",
            "import std;\n"
            'let source = {"x": 1};\n'
            'let msg = std.thread.Message.from(source);\n'
            'source["x"] = 2;\n'
            'print("after_source="); print(msg.payload["x"]);\n'
            'let copy = msg.payload;\n'
            'copy["x"] = 3;\n'
            'print("after_copy="); print(msg.payload["x"]);\n',
            stdout_all=["after_source=1", "after_copy=1"],
        )
        check(
            "std.thread message handles",
            "import std;\n"
            'let inner = std.thread.Channel(1);\n'
            'let msg = std.thread.Message.from({"x": 1}, '
            '{"reply": inner});\n'
            'let ch = std.thread.Channel(1);\n'
            'ch.send(msg);\n'
            'let got = ch.try_recv();\n'
            'got["value"].handles["reply"].send('
            'std.thread.Message.from({"ok": true}));\n'
            'let reply = inner.try_recv();\n'
            'print("ok="); print(reply["value"].payload["ok"]);\n',
            stdout_all=["ok=true"],
        )
        check(
            "std.thread unbounded channel",
            "import std;\n"
            'let ch = std.thread.unbounded_channel();\n'
            'for (let i = 0; i < 5; i++) {\n'
            '  ch.send(std.thread.Message.from(i));\n'
            '}\n'
            'print("capacity="); print(ch.capacity);\n'
            'print("length="); print(ch.length);\n',
            stdout_all=["capacity=-1", "length=5"],
        )

        with tempfile.TemporaryDirectory() as thread_tmp:
            thread_worker = os.path.join(thread_tmp, "thread_worker.skein")
            with open(thread_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "func handle(payload) {\n"
                    "    if (payload[\"n\"] < 0) { missing_function(); }\n"
                    "    return {\"n\": payload[\"n\"] * 2};\n"
                    "}\n"
                )
            thread_worker_path = thread_worker.replace("\\", "\\\\")
            check(
                "std.thread pool submit and error",
                "import std;\n"
                f'let pool = std.thread.pool(2, "{thread_worker_path}");\n'
                'let f1 = pool.submit({"n": 21});\n'
                'let f2 = pool.submit({"n": 5});\n'
                'let r1 = f1.wait();\n'
                'let r2 = f2.wait();\n'
                'print("r1="); print(r1["ok"]); print(":"); '
                'print(r1["value"]["n"]);\n'
                'print("r2="); print(r2["ok"]); print(":"); '
                'print(r2["value"]["n"]);\n'
                'let f3 = pool.submit({"n": -1});\n'
                'let r3 = f3.wait();\n'
                'print("fail="); print(r3["ok"]);\n'
                'pool.close();\n'
                'pool.join();\n',
                stdout_all=["r1=true:42", "r2=true:10", "fail=false"],
            )

            spawn_worker = os.path.join(
                thread_tmp, "thread_spawn_worker.skein")
            with open(spawn_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func main(ctx) {\n"
                    "    if (ctx.payload[\"fail\"]) { missing_function(); }\n"
                    "    let signal = ctx.handles[\"signal\"];\n"
                    "    signal.send(std.thread.Message.from("
                    "{\"x\": ctx.payload[\"x\"]}));\n"
                    "    return {\"x\": ctx.payload[\"x\"]};\n"
                    "}\n"
                )
            spawn_worker_path = spawn_worker.replace("\\", "\\\\")
            check(
                "std.thread spawn and join",
                "import std;\n"
                'let ch = std.thread.Channel(1);\n'
                f'let msg = std.thread.Message.from({{"x": 5, '
                f'"fail": false}}, {{"signal": ch}});\n'
                f'let t = std.thread.spawn("{spawn_worker_path}", msg);\n'
                'let signal = ch.recv();\n'
                'let r = t.join();\n'
                'print("signal="); print(signal.payload["x"]);\n'
                'print("ok="); print(r["ok"]); print(":"); '
                'print(r["value"]["x"]);\n'
                'print("alive="); print(t.is_alive());\n',
                stdout_all=["signal=5", "ok=true:5", "alive=false"],
            )
            check(
                "std.thread spawn error join",
                "import std;\n"
                f'let msg = std.thread.Message.from({{"x": 1, '
                f'"fail": true}});\n'
                f'let t = std.thread.spawn("{spawn_worker_path}", msg);\n'
                'let r = t.join();\n'
                'print("ok="); print(r["ok"]);\n',
                stdout_all=["ok=false"],
            )
            check(
                "std.thread select data and timeout",
                "import std;\n"
                'let a = std.thread.Channel(1);\n'
                'let b = std.thread.Channel(1);\n'
                'b.send(std.thread.Message.from({"v": "b"}));\n'
                'let r = std.thread.select([a, b], 1000);\n'
                'print("index="); print(r["index"]); print(":"); '
                'print(r["value"].payload["v"]);\n'
                'let empty = std.thread.Channel(1);\n'
                'let t = std.thread.select([empty], 10);\n'
                'print("timeout="); print(t["timeout"]);\n'
                'empty.close();\n'
                'let c = std.thread.select([empty], 1000);\n'
                'print("closed="); print(c["closed"]); print(":"); '
                'print(c["index"]);\n',
                stdout_all=["index=1:b", "timeout=true", "closed=true:0"],
            )
            check(
                "std.thread select blocks until send",
                "import std;\n"
                'let signal = std.thread.Channel(1);\n'
                'let empty = std.thread.Channel(1);\n'
                f'let msg = std.thread.Message.from({{"x": 7, '
                f'"fail": false}}, {{"signal": signal}});\n'
                f'let t = std.thread.spawn("{spawn_worker_path}", msg);\n'
                'let r = std.thread.select([empty, signal], 1000);\n'
                'print("index="); print(r["index"]); print(":"); '
                'print(r["value"].payload["x"]);\n'
                't.join();\n',
                stdout_all=["index=1:7"],
            )
            check(
                "std.thread try_broadcast",
                "import std;\n"
                'let x = std.thread.Channel(1);\n'
                'let y = std.thread.Channel(1);\n'
                'let sent = std.thread.try_broadcast([x, y], '
                'std.thread.Message.from({"v": 2}));\n'
                'print("sent="); print(sent["sent"]);\n'
                'let rx = x.try_recv();\n'
                'let ry = y.try_recv();\n'
                'print("x="); print(rx["value"].payload["v"]);\n'
                'print("y="); print(ry["value"].payload["v"]);\n',
                stdout_all=["sent=[0, 1]", "x=2", "y=2"],
            )

    probe = run('import std;\nstd.compress.gzip(bytes_from_string("x"))\n')
    if "zlib support is disabled" not in probe.stderr:
        check(
            "std compress roundtrips",
            "import std;\n"
            'let b = bytes_from_string("hello hello hello");\n'
            'print(bytes_hex(bytes_slice(std.compress.gzip(b), 0, 2)));\n'
            'print(bytes_to_string(std.compress.gunzip('
            'std.compress.gzip(b))));\n'
            'print(bytes_to_string(std.compress.unzlib('
            'std.compress.zlib(b))));\n'
            'print(bytes_to_string(std.compress.inflate('
            'std.compress.deflate(b))));\n',
            stdout_all=["1f8b", "hello hello hello", "hello hello hello",
                        "hello hello hello"],
        )


def check_source_encoding():
    """Verify BOM handling and --encoding for source files."""
    script = 'print("你好");\n'
    cases = [
        ("utf8", script.encode("utf-8"), ()),
        ("utf8 bom", b"\xef\xbb\xbf" + script.encode("utf-8"), ()),
        ("utf16le bom", b"\xff\xfe" + script.encode("utf-16le"), ()),
        ("utf16be bom", b"\xfe\xff" + script.encode("utf-16be"), ()),
        ("utf32le bom", b"\xff\xfe\x00\x00" + script.encode("utf-32le"), ()),
        ("utf32be bom", b"\x00\x00\xfe\xff" + script.encode("utf-32be"), ()),
        ("gbk explicit", script.encode("gbk"), ("--encoding", "gbk")),
        ("utf16le explicit", script.encode("utf-16le"),
         ("--encoding", "utf-16le")),
        ("utf16 explicit with bom",
         b"\xff\xfe" + script.encode("utf-16le"),
         ("--encoding", "utf-16")),
    ]
    for name, data, args in cases:
        with tempfile.NamedTemporaryFile(delete=False) as handle:
            handle.write(data)
            path = handle.name
        try:
            proc = subprocess.run(
                [BIN, *args, path],
                text=True,
                encoding="utf-8",
                capture_output=True,
                timeout=10,
            )
        finally:
            os.unlink(path)
        assert proc.returncode == 0, (name, proc.stderr)
        assert "你好" in proc.stdout, (name, proc.stdout, proc.stderr)

    with tempfile.NamedTemporaryFile(delete=False) as handle:
        handle.write(b'\xef\xbb\xbf' + script.encode("gbk"))
        path = handle.name
    try:
        proc = subprocess.run(
            [BIN, "--encoding", "gbk", path],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=10,
        )
    finally:
        os.unlink(path)
    assert proc.returncode == 2, proc.returncode
    assert "BOM" in proc.stderr and "gbk" in proc.stderr, proc.stderr

    with tempfile.NamedTemporaryFile(delete=False) as handle:
        handle.write(b"let a\x80b = 1;\n")
        path = handle.name
    try:
        proc = subprocess.run(
            [BIN, path],
            text=True,
            encoding="utf-8",
            errors="replace",
            capture_output=True,
            timeout=10,
        )
    finally:
        os.unlink(path)
    assert proc.returncode == 1, proc.returncode
    assert "invalid UTF-8 byte" in proc.stderr, proc.stderr
    assert "--encoding" in proc.stderr, proc.stderr

    with tempfile.TemporaryDirectory() as tmp:
        app = os.path.join(tmp, "app")
        package = os.path.join(app, "pkg")
        os.makedirs(package)
        with open(os.path.join(package, "pkg.skein"), "wb") as handle:
            handle.write('package pkg { export let value = "包"; }\n'
                         .encode("gbk"))
        main_path = os.path.join(app, "main.skein")
        with open(main_path, "wb") as handle:
            handle.write('import pkg;\nprint(pkg.value);\n'.encode("gbk"))
        proc = subprocess.run(
            [BIN, "--encoding", "gbk", main_path],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=10,
        )
        assert proc.returncode == 0, proc.stderr
        assert "包" in proc.stdout, (proc.stdout, proc.stderr)

__all__ = [
    "BIN",
    "FFI_SHIM",
    "NATIVE_MODULE",
    "skein_string",
    "run",
    "run_file",
    "check",
    "ws_recv_exact",
    "ws_read_frame",
    "ws_write_frame",
    "ws_accept_key",
    "check_interactive_continuation",
    "check_encoding_two_byte_tables",
    "check_unicode_normalization_samples",
    "check_extended_stdlib",
    "check_source_encoding",
    "base64",
    "hashlib",
    "os",
    "random",
    "signal",
    "subprocess",
    "sys",
    "tempfile",
    "time",
    "unicodedata",
]
