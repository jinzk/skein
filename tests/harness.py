#!/usr/bin/env python3
"""Golden/smoke tests for the skein interpreter."""

import base64
import hashlib
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
import urllib.error
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
    reuse_platform = (
        sys.platform.startswith("linux") or
        sys.platform.startswith("freebsd") or
        sys.platform.startswith("dragonfly")
    )
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
    with tempfile.TemporaryDirectory() as env_tmp:
        env_worker = os.path.join(env_tmp, "env_worker.skein")
        with open(env_worker, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "func main(ctx) {\n"
                "    let before = std.env.get(\"SKEIN_THREAD_ENV\");\n"
                "    std.env.set(\"SKEIN_THREAD_ENV\", \"child\");\n"
                "    return {\"before\": before == null ? \"null\" : "
                "before, \"after\": "
                "std.env.get(\"SKEIN_THREAD_ENV\")};\n"
                "}\n"
            )
        check(
            "std env thread isolation",
            "import std;\n"
            'std.env.set("SKEIN_THREAD_ENV", "parent");\n'
            'let msg = std.thread.Message.from({"x": 1});\n'
            f'let t = std.thread.spawn({skein_string(env_worker)}, msg);\n'
            'let r = t.join();\n'
            'print("child_before=" + r["value"]["before"]);\n'
            'print("|child_after=" + r["value"]["after"]);\n'
            'print("|parent=" + std.env.get("SKEIN_THREAD_ENV"));\n',
            stdout_all=["child_before=parent", "child_after=child",
                        "parent=parent"],
        )

        cwd_parent = os.path.join(env_tmp, "cwd_parent")
        cwd_child = os.path.join(env_tmp, "cwd_child")
        os.makedirs(cwd_parent)
        os.makedirs(cwd_child)
        with open(os.path.join(cwd_child, "marker.txt"), "w",
                  encoding="utf-8") as handle:
            handle.write("child-data")
        cwd_worker = os.path.join(env_tmp, "cwd_worker.skein")
        with open(cwd_worker, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "func main(ctx) {\n"
                "    std.fs.chdir(ctx.payload[\"dir\"]);\n"
                "    return {\"cwd\": std.fs.cwd(), "
                "\"exists\": std.fs.exists(\"marker.txt\"), "
                "\"text\": std.fs.read_text(\"marker.txt\", "
                "\"utf-8\")};\n"
                "}\n"
            )
        check(
            "std fs chdir thread isolation",
            "import std;\n"
            f"std.fs.chdir({skein_string(cwd_parent)});\n"
            "let before = std.fs.cwd();\n"
            f'let msg = std.thread.Message.from({{"dir": '
            f"{skein_string(cwd_child)}}});\n"
            f'let t = std.thread.spawn({skein_string(cwd_worker)}, msg);\n'
            'let r = t.join();\n'
            'print("child_text=" + r["value"]["text"]);\n'
            'print("|child_exists=" + str(r["value"]["exists"]));\n'
            'print("|parent_same=" + str(std.fs.cwd() == before));\n',
            stdout_all=["child_text=child-data", "child_exists=1",
                        "parent_same=true"],
        )

        spawn_child = os.path.join(env_tmp, "spawn_child.skein")
        with open(spawn_child, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "println(\"env=\" + (std.env.get(\"SKEIN_SPAWN_ENV\") "
                "== null ? \"null\" : "
                "std.env.get(\"SKEIN_SPAWN_ENV\")));\n"
                "println(\"cwd_ok=\" + str(std.fs.cwd() == "
                "std.env.get(\"SKEIN_SPAWN_CWD\")));\n"
            )
        check(
            "std process spawn thread env and cwd",
            "import std;\n"
            'std.env.set("SKEIN_SPAWN_ENV", "from-thread");\n'
            f"std.env.set(\"SKEIN_SPAWN_CWD\", "
            f"{skein_string(cwd_child)});\n"
            f"std.fs.chdir({skein_string(cwd_child)});\n"
            f'let p = std.process.spawn('
            f"{skein_string(os.path.abspath(BIN))}, "
            f'{{"args": [{skein_string(spawn_child)}]}});\n'
            'print(bytes_to_string(std.io.read_line(p["stdout"])));\n'
            'print(bytes_to_string(std.io.read_line(p["stdout"])));\n'
            "std.process.wait(p);\n",
            stdout_all=["env=from-thread", "cwd_ok=true"],
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
                "func handle(req) {\n"
                "  return {\"status\": 200,\n"
                "          \"headers\": {\"Content-Type\": \"text/plain\"},\n"
                "          \"body\": bytes_to_string(req[\"body\"])};\n"
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
                f"let ps = std.http.serve_workers(s, 1, "
                f"\"{serve_worker_path}\", "
                f"{{\"strategy\": \"dispatch\", \"max_requests\": 1}});\n"
                "println(\"server=\" + s[\"strategy\"] + \":\" + "
                "s[\"platform\"] + \":\" + "
                "str(s[\"capabilities\"][\"http1\"]));\n"
                "println(\"ps=\" + ps.strategy + \":\" + "
                "str(ps.capabilities[\"dispatch\"]) + \":\" + "
                "str(ps.stats()[\"workers\"]));\n"
                "println(ps.join(5000));\n"
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
            assert "server=dispatch:" in out, out
            assert "ps=dispatch:true:1" in out, out
            assert ":true" in out, out
            assert out.rstrip().endswith("1"), out
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        check(
            "std.http serve_workers require fails fast",
            "import std;\n"
            'let s = std.http.listen("127.0.0.1", 0);\n'
            f'let n = std.http.serve_workers(s, 1, "{serve_worker_path}", '
            '{"strategy": "dispatch", "require": ["http2"]});\n',
            stderr="missing capabilities: http2",
        )
        check(
            "std.http require raises structured unsupported error",
            "import std;\n"
            'let s = std.http.listen("127.0.0.1", 0);\n'
            f'let n = std.http.serve_workers(s, 1, "{serve_worker_path}", '
            '{"strategy": "dispatch", "require": ["http2"]}) '
            "on error(e) {\n"
            '    print("kind=" + std.error.kind(e));\n'
            "    recover null;\n"
            "};\n",
            stdout="kind=http.unsupported",
        )
        check(
            "std.http ProcessServer shutdown",
            "import std;\n"
            'let s = std.http.listen("127.0.0.1", 0);\n'
            f'let ps = std.http.serve_workers(s, 1, '
            f'"{serve_worker_path}", '
            '{"strategy": "dispatch", "max_requests": 0});\n'
            'print(ps.shutdown(2000));\n',
            stdout="0",
        )

        slow_worker_script = os.path.join(
            worker_tmp, "serve_slow_worker.skein")
        with open(slow_worker_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "func handle(req) {\n"
                "  std.time.sleep(30000);\n"
                "  return {\"status\": 200, \"body\": \"slow\"};\n"
                "}\n"
            )
        slow_worker_path = slow_worker_script.replace("\\", "\\\\")
        client_result = {}

        def slow_request(port_text):
            try:
                with urllib.request.urlopen(
                        f"http://127.0.0.1:{port_text}/slow",
                        timeout=20) as response:
                    client_result["status"] = response.status
            except Exception as exc:
                client_result["error"] = repr(exc)

        shutdown_script = os.path.join(
            worker_tmp, "serve_workers_shutdown.skein")
        with open(shutdown_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                f"let ps = std.http.serve_workers(s, 1, "
                f"\"{slow_worker_path}\", "
                "{\"strategy\": \"dispatch\", \"max_requests\": 0});\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                "std.task.wait(std.task.sleep(700));\n"
                "let started = std.time.monotonic_ms();\n"
                "let served = ps.shutdown(100);\n"
                "println(\"shutdown=\" + str(served) + \":\" + "
                "str(std.time.monotonic_ms() - started));\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), shutdown_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "serve_workers shutdown did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            client = threading.Thread(target=slow_request, args=(port_line,))
            client.start()
            out, err = server.communicate(timeout=20)
            assert server.returncode == 0, err
            shutdown_lines = [
                line for line in out.splitlines()
                if line.startswith("shutdown=")
            ]
            assert shutdown_lines, out
            elapsed = int(shutdown_lines[0].rsplit(":", 1)[1])
            assert elapsed < 5000, shutdown_lines[0]
            client.join(timeout=10)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        if reuse_platform:
            workers_reuse_worker = os.path.join(
                worker_tmp, "serve_workers_reuse_worker.skein")
            with open(workers_reuse_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(req) {\n"
                    "  return {\"status\": 200,\n"
                    "          \"headers\": {\"X-Worker\": "
                    "str(req[\"worker_id\"])},\n"
                    "          \"body\": \"ok\"};\n"
                    "}\n"
                )
            workers_reuse_path = workers_reuse_worker.replace("\\", "\\\\")
            workers_reuse_script = os.path.join(
                worker_tmp, "serve_workers_reuse.skein")
            with open(workers_reuse_script, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                    "println(s[\"port\"]);\n"
                    "std.io.flush(std.io.stdout);\n"
                    f"let ps = std.http.serve_workers(s, 4, "
                    f"\"{workers_reuse_path}\", {{\"strategy\": "
                    f"\"reuse_port\", \"max_requests\": 8}});\n"
                    "println(\"info=\" + ps.strategy + \":\" + "
                    "str(ps.capabilities[\"reuse_port\"]) + \":\" + "
                    "str(ps.stats()[\"workers\"]));\n"
                    "std.io.flush(std.io.stdout);\n"
                    "let served = ps.join(30000);\n"
                    "println(\"served=\" + str(served));\n"
                    "println(\"served_after=\" + str(ps.served));\n"
                )
            server = subprocess.Popen(
                [os.path.abspath(BIN), workers_reuse_script],
                cwd=worker_tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=os.environ.copy(),
            )
            try:
                port_line = server.stdout.readline().strip()
                assert port_line.isdigit(), (
                    "serve_workers reuse_port did not report a port: "
                    f"{port_line!r} / {server.stderr.read()}"
                )
                reuse_results = []
                reuse_errors = []
                reuse_lock = threading.Lock()

                def workers_reuse_hit(index):
                    last_error = None
                    for _ in range(200):
                        try:
                            request = urllib.request.Request(
                                f"http://127.0.0.1:{port_line}/r{index}")
                            with urllib.request.urlopen(
                                    request, timeout=5) as response:
                                body = response.read()
                                worker = response.headers.get("X-Worker")
                            with reuse_lock:
                                reuse_results.append(
                                    (response.status, body, worker))
                            return
                        except Exception as exc:
                            last_error = exc
                            time.sleep(0.02)
                    with reuse_lock:
                        reuse_errors.append((index, repr(last_error)))

                reuse_threads = [
                    threading.Thread(target=workers_reuse_hit, args=(index,))
                    for index in range(8)
                ]
                for thread in reuse_threads:
                    thread.start()
                for thread in reuse_threads:
                    thread.join()
                assert not reuse_errors, reuse_errors
                assert len(reuse_results) == 8, reuse_results
                assert all(status == 200 and body == b"ok"
                           for status, body, _ in reuse_results), reuse_results
                out, err = server.communicate(timeout=30)
                assert server.returncode == 0, err
                assert "info=reuse_port:true:4" in out, out
                assert "served=8" in out, out
                assert "served_after=8" in out, out
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()

            workers_shutdown_script = os.path.join(
                worker_tmp, "serve_workers_reuse_shutdown.skein")
            with open(workers_shutdown_script, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                    f"let ps = std.http.serve_workers(s, 2, "
                    f"\"{slow_worker_path}\", "
                    "{\"strategy\": \"reuse_port\", \"max_requests\": 0});\n"
                    "println(s[\"port\"]);\n"
                    "std.io.flush(std.io.stdout);\n"
                    "std.time.sleep(1500);\n"
                    "let started = std.time.monotonic_ms();\n"
                    "let served = ps.shutdown(100);\n"
                    "println(\"shutdown=\" + str(served) + \":\" + "
                    "str(std.time.monotonic_ms() - started));\n"
                )
            server = subprocess.Popen(
                [os.path.abspath(BIN), workers_shutdown_script],
                cwd=worker_tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=os.environ.copy(),
            )
            try:
                port_line = server.stdout.readline().strip()
                assert port_line.isdigit(), (
                    "serve_workers reuse shutdown did not report a port: "
                    f"{port_line!r} / {server.stderr.read()}"
                )
                client = threading.Thread(target=slow_request,
                                          args=(port_line,))
                client.start()
                out, err = server.communicate(timeout=30)
                assert server.returncode == 0, err
                assert "shutdown=" in out, out
                client.join(timeout=10)
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()

            check(
                "std.http serve_threads reuse join timeout",
                "import std;\n"
                'let s = std.http.listen("127.0.0.1", 0);\n'
                f'let ts = std.http.serve_threads(s, 1, '
                f'"{workers_reuse_path}", {{"strategy": "reuse_port", '
                f'"max_requests": 0}});\n'
                "ts.join(10);\n",
                stderr="ThreadServer.join timed out",
            )
            check(
                "std.http serve_threads reuse shutdown",
                "import std;\n"
                'let s = std.http.listen("127.0.0.1", 0);\n'
                f'let ts = std.http.serve_threads(s, 1, '
                f'"{workers_reuse_path}", {{"strategy": "reuse_port", '
                f'"max_requests": 0}});\n'
                'print(ts.shutdown(2000));\n',
                stdout="0",
            )

            workers_stop_script = os.path.join(
                worker_tmp, "serve_workers_stop.skein")
            with open(workers_stop_script, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                    "println(s[\"port\"]);\n"
                    "std.io.flush(std.io.stdout);\n"
                    f"let ps = std.http.serve_workers(s, 2, "
                    f"\"{workers_reuse_path}\", {{\"strategy\": "
                    f"\"reuse_port\"}});\n"
                    "println(\"ready\");\n"
                    "std.io.flush(std.io.stdout);\n"
                    "std.time.sleep(1500);\n"
                    "ps.stop();\n"
                    "println(\"stopping=\" + str(ps.stats()[\"stopping\"]));\n"
                    "std.io.flush(std.io.stdout);\n"
                    "println(\"served=\" + str(ps.join()));\n"
                )
            server = subprocess.Popen(
                [os.path.abspath(BIN), workers_stop_script],
                cwd=worker_tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=os.environ.copy(),
            )
            try:
                port_line = server.stdout.readline().strip()
                assert port_line.isdigit(), (
                    "serve_workers stop did not report a port: "
                    f"{port_line!r} / {server.stderr.read()}"
                )
                ready_line = server.stdout.readline().strip()
                assert ready_line == "ready", (
                    "serve_workers stop did not become ready: "
                    f"{ready_line!r} / {server.stderr.read()}"
                )
                stop_result = None
                for _ in range(100):
                    try:
                        with urllib.request.urlopen(
                                f"http://127.0.0.1:{port_line}/one",
                                timeout=5) as response:
                            stop_result = (response.status, response.read())
                        break
                    except Exception:
                        time.sleep(0.02)
                assert stop_result == (200, b"ok"), stop_result
                out, err = server.communicate(timeout=30)
                assert server.returncode == 0, err
                assert "stopping=true" in out, out
                served_values = [
                    int(line.split("=", 1)[1])
                    for line in out.splitlines()
                    if line.startswith("served=")
                ]
                assert served_values and served_values[-1] >= 1, out
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()

            workers_stream_worker = os.path.join(
                worker_tmp, "serve_workers_stream_worker.skein")
            with open(workers_stream_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(req) {\n"
                    "  let out = \"\";\n"
                    "  let chunk = std.http.read_body(req);\n"
                    "  while (chunk != null) {\n"
                    "    out = out + bytes_to_string(chunk);\n"
                    "    chunk = std.http.read_body(req);\n"
                    "  }\n"
                    "  return {\"status\": 200, \"body\": \"pull=\" + out};\n"
                    "}\n"
                )
            workers_stream_path = workers_stream_worker.replace("\\", "\\\\")
            workers_stream_script = os.path.join(
                worker_tmp, "serve_workers_stream.skein")
            with open(workers_stream_script, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                    f"let ps = std.http.serve_workers(s, 2, "
                    f"\"{workers_stream_path}\", {{\"strategy\": "
                    f"\"reuse_port\", \"stream_request_body\": true, "
                    f"\"max_requests\": 1}});\n"
                    "println(\"info=\" + str(s[\"port\"]) + \":\" + "
                    "str(ps.capabilities[\"stream_request_body\"]));\n"
                    "std.io.flush(std.io.stdout);\n"
                    "println(\"served=\" + str(ps.join()));\n"
                )
            server = subprocess.Popen(
                [os.path.abspath(BIN), workers_stream_script],
                cwd=worker_tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=os.environ.copy(),
            )
            try:
                info_line = server.stdout.readline().strip()
                assert info_line.startswith("info="), (
                    "serve_workers stream body did not report info: "
                    f"{info_line!r} / {server.stderr.read()}"
                )
                port_text, stream_flag = (
                    info_line.split("=", 1)[1].split(":"))
                assert stream_flag == "true", info_line
                client = socket.create_connection(
                    ("127.0.0.1", int(port_text)), timeout=10)
                client.sendall(
                    b"POST /stream HTTP/1.1\r\n"
                    b"Host: 127.0.0.1\r\n"
                    b"Transfer-Encoding: chunked\r\n"
                    b"Connection: close\r\n\r\n"
                    b"6\r\nhello \r\n"
                    b"8\r\nchunked \r\n"
                    b"5\r\nworld\r\n"
                    b"0\r\n\r\n")
                response = b""
                while True:
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    response += chunk
                client.close()
                assert b"pull=hello chunked world" in response, response
                out, err = server.communicate(timeout=30)
                assert server.returncode == 0, err
                assert "served=1" in out, out
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()
        else:
            check(
                "std.http serve_workers reuse_port unsupported",
                "import std;\n"
                'let s = std.http.listen("127.0.0.1", 0);\n'
                f'let n = std.http.serve_workers(s, 1, '
                f'"{serve_worker_path}", '
                '{"strategy": "reuse_port"});\n',
                stderr="reuse_port is not supported",
            )

        workers_501_script = os.path.join(
            worker_tmp, "serve_workers_require_501.skein")
        with open(workers_501_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                f"let ps = std.http.serve_workers(s, 1, "
                f"\"{serve_worker_path}\", {{\"strategy\": \"dispatch\", "
                f"\"require\": [\"http2\"], "
                f"\"on_unsupported\": \"501\", "
                f"\"max_requests\": 1}});\n"
                "println(ps.join());\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), workers_501_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "serve_workers 501 did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            try:
                urllib.request.urlopen(
                    f"http://127.0.0.1:{port_line}/x", timeout=10)
                raise AssertionError(
                    "expected 501 for missing process capability")
            except urllib.error.HTTPError as exc:
                assert exc.code == 501, (exc.code, exc.read())
            server.communicate(timeout=10)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        workers_features_script = os.path.join(
            worker_tmp, "serve_workers_features.skein")
        with open(workers_features_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                f"let ps = std.http.serve_workers(s, 1, "
                f"\"{serve_worker_path}\", {{\"strategy\": \"dispatch\", "
                f"\"max_requests\": 2}});\n"
                "println(ps.join());\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), workers_features_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "serve_workers features did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            for headers, label in (
                ({"Upgrade": "websocket", "Connection": "Upgrade"},
                 "websocket"),
                ({"Accept": "text/event-stream"}, "sse"),
            ):
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port_line}/feature",
                    headers=headers)
                try:
                    urllib.request.urlopen(request, timeout=10)
                    raise AssertionError(
                        f"expected 501 for process {label}")
                except urllib.error.HTTPError as exc:
                    assert exc.code == 501, (label, exc.code, exc.read())
            server.communicate(timeout=10)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        slow_script = os.path.join(
            worker_tmp, "slow_hybrid_worker.skein")
        with open(slow_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "func handle(req) {\n"
                "  std.time.sleep(300);\n"
                "  return {\"status\": 200,\n"
                "          \"headers\": {\"Content-Type\": \"text/plain\"},\n"
                "          \"body\": bytes_to_string(req[\"body\"])};\n"
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
                f"let ps = std.http.serve_workers(s, 2, "
                f"\"{slow_path}\", {{\"strategy\": \"dispatch\", "
                f"\"max_requests\": 2}});\n"
                "println(ps.join());\n"
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

        threads_worker = os.path.join(
            worker_tmp, "serve_threads_worker.skein")
        with open(threads_worker, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "func handle(req) {\n"
                "  let body = bytes_to_string(req[\"body\"]);\n"
                "  return {\"status\": 200,\n"
                "          \"headers\": {\n"
                "            \"X-Has-Socket\": "
                "str(std.dict.has(req, \"socket\") != 0),\n"
                "            \"X-Remote\": str(req[\"remote_addr\"]),\n"
                "            \"X-Method\": req[\"method\"]},\n"
                "          \"body\": req[\"path\"] + \":\" + body};\n"
                "}\n"
            )
        threads_worker_path = threads_worker.replace("\\", "\\\\")
        threads_script = os.path.join(worker_tmp, "serve_threads.skein")
        with open(threads_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                f"let ts = std.http.serve_threads(s, 2, "
                f"\"{threads_worker_path}\", "
                f"{{\"max_requests\": 1, \"strategy\": \"dispatch\"}});\n"
                "println(s[\"port\"]);\n"
                "println(\"server=\" + s[\"strategy\"] + \":\" + "
                "s[\"platform\"] + \":\" + "
                "str(s[\"capabilities\"][\"http1\"]));\n"
                "println(\"caps=\" + str(ts.capabilities[\"http1\"]) + "
                "\":\" + str(ts.capabilities[\"http2\"]));\n"
                "println(\"strategy=\" + ts.strategy);\n"
                "std.io.flush(std.io.stdout);\n"
                "println(\"served=\" + str(ts.join(5000)));\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), threads_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "serve_threads did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            server_line = server.stdout.readline().strip()
            caps_line = server.stdout.readline().strip()
            strategy_line = server.stdout.readline().strip()
            assert server_line.startswith("server=dispatch:"), server_line
            assert server_line.endswith(":true"), server_line
            assert caps_line == "caps=true:false", caps_line
            assert strategy_line == "strategy=dispatch", strategy_line
            request = urllib.request.Request(
                f"http://127.0.0.1:{port_line}/echo",
                data=b"hello-threads",
                method="POST",
            )
            with urllib.request.urlopen(request, timeout=10) as response:
                assert response.status == 200
                assert response.read() == b"/echo:hello-threads"
                assert response.headers.get("X-Has-Socket") == "false"
                assert response.headers.get("X-Remote") == "127.0.0.1"
                assert response.headers.get("X-Method") == "POST"
            out, err = server.communicate(timeout=10)
            assert server.returncode == 0, err
            assert "served=1" in out, out
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        slow_threads_worker = os.path.join(
            worker_tmp, "serve_threads_slow.skein")
        with open(slow_threads_worker, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "func handle(req) {\n"
                "  std.time.sleep(300);\n"
                "  return {\"status\": 200, \"body\": req[\"path\"]};\n"
                "}\n"
            )
        slow_threads_path = slow_threads_worker.replace("\\", "\\\\")
        concurrent_threads_script = os.path.join(
            worker_tmp, "serve_threads_concurrent.skein")
        with open(concurrent_threads_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                f"let ts = std.http.serve_threads(s, 2, "
                f"\"{slow_threads_path}\", "
                f"{{\"max_requests\": 2, \"strategy\": \"dispatch\"}});\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                "ts.join();\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), concurrent_threads_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "serve_threads concurrent did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            results = []

            def threads_hit(index):
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port_line}/r{index}"
                )
                with urllib.request.urlopen(
                        request, timeout=10) as response:
                    results.append((response.status, response.read()))

            threads = [
                threading.Thread(target=threads_hit, args=(index,))
                for index in range(2)
            ]
            started = time.monotonic()
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            elapsed = time.monotonic() - started
            assert len(results) == 2
            assert all(status == 200 for status, _ in results)
            assert elapsed < 0.55, (
                "serve_threads handled requests sequentially: "
                f"{elapsed:.3f}s"
            )
            out, err = server.communicate(timeout=10)
            assert server.returncode == 0, err
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        timeout_threads_script = os.path.join(
            worker_tmp, "serve_threads_timeout.skein")
        with open(timeout_threads_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                f"let ts = std.http.serve_threads(s, 1, "
                f"\"{slow_threads_path}\", {{\"max_requests\": 1, "
                f"\"worker_timeout_ms\": 50, "
                f"\"strategy\": \"dispatch\"}});\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                "ts.join();\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), timeout_threads_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "serve_threads timeout did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            try:
                urllib.request.urlopen(
                    f"http://127.0.0.1:{port_line}/slow", timeout=10)
                raise AssertionError("expected 504 for worker timeout")
            except urllib.error.HTTPError as exc:
                assert exc.code == 504, (exc.code, exc.read())
            server.communicate(timeout=10)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        check(
            "std.http serve_threads stream body rejected",
            "import std;\n"
            'let s = std.http.listen("127.0.0.1", 0);\n'
            's["stream_request_body"] = true;\n'
            'let ts = std.http.serve_threads(s, 1, "missing.skein", '
            '{"strategy": "dispatch"});\n',
            stderr="stream_request_body requires reuse_port",
        )

        check(
            "std.thread capabilities",
            "import std;\n"
            'let c = std.thread.capabilities();\n'
            'print("schema="); print(c["schema"]);\n'
            'print("platform="); print(c["platform"]);\n'
            'print("http1="); print(c["http1"]);\n'
            'print("websocket="); print(c["websocket"]);\n'
            'print("strategies="); print(len(c["strategies"]));\n',
            stdout_all=["schema=1", "platform=", "http1=true",
                        "websocket=false", "strategies="],
        )
        check(
            "std.http thread_capabilities dispatch",
            "import std;\n"
            'let c = std.http.thread_capabilities('
            '{"strategy": "dispatch"});\n'
            'print(c["strategy"] + ":" + str(c["http2"]) + ":" + '
            'str(c["stream_request_body"]));\n',
            stdout="dispatch:false:false",
        )
        if reuse_platform:
            check(
                "std.http thread_capabilities reuse_port",
                "import std;\n"
                'let c = std.http.thread_capabilities('
                '{"strategy": "reuse_port"});\n'
                'print(c["strategy"] + ":" + str(c["http2"]) + ":" + '
                'str(c["stream_request_body"]));\n',
                stdout="reuse_port:true:true",
            )
        else:
            check(
                "std.http thread_capabilities reuse_port unsupported",
                "import std;\n"
                'std.http.thread_capabilities('
                '{"strategy": "reuse_port"});\n',
                stderr="reuse_port is not supported",
            )
        check(
            "std.http serve_threads require fails fast",
            "import std;\n"
            'let s = std.http.listen("127.0.0.1", 0);\n'
            'let ts = std.http.serve_threads(s, 1, "missing.skein", '
            '{"strategy": "dispatch", "require": ["http2"]});\n',
            stderr="missing capabilities: http2",
        )

        require_501_script = os.path.join(
            worker_tmp, "serve_threads_require_501.skein")
        with open(require_501_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                f"let ts = std.http.serve_threads(s, 1, "
                f"\"{threads_worker_path}\", {{\"strategy\": "
                f"\"dispatch\", \"require\": [\"http2\"], "
                f"\"on_unsupported\": \"501\", "
                f"\"max_requests\": 1}});\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                "ts.join();\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), require_501_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "require 501 server did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            try:
                urllib.request.urlopen(
                    f"http://127.0.0.1:{port_line}/x", timeout=10)
                raise AssertionError("expected 501 for missing capability")
            except urllib.error.HTTPError as exc:
                assert exc.code == 501, (exc.code, exc.read())
            server.communicate(timeout=10)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        require_warn_script = os.path.join(
            worker_tmp, "serve_threads_require_warn.skein")
        with open(require_warn_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                f"let ts = std.http.serve_threads(s, 1, "
                f"\"{threads_worker_path}\", {{\"strategy\": "
                f"\"dispatch\", \"require\": [\"http2\"], "
                f"\"on_unsupported\": \"warn\", "
                f"\"max_requests\": 1}});\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                "ts.join();\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), require_warn_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "require warn server did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            request = urllib.request.Request(
                f"http://127.0.0.1:{port_line}/x")
            with urllib.request.urlopen(request, timeout=10) as response:
                assert response.status == 200
            out, err = server.communicate(timeout=10)
            assert "missing capabilities: http2" in err, err
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        feature_script = os.path.join(
            worker_tmp, "serve_threads_unsupported_features.skein")
        with open(feature_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                f"let ts = std.http.serve_threads(s, 1, "
                f"\"{threads_worker_path}\", {{\"strategy\": "
                f"\"dispatch\", \"max_requests\": 2}});\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                "ts.join();\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), feature_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "feature 501 server did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            for headers, label in (
                ({"Upgrade": "websocket", "Connection": "Upgrade"},
                 "websocket"),
                ({"Accept": "text/event-stream"}, "sse"),
            ):
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port_line}/feature",
                    headers=headers)
                try:
                    urllib.request.urlopen(request, timeout=10)
                    raise AssertionError(
                        f"expected 501 for {label}")
                except urllib.error.HTTPError as exc:
                    assert exc.code == 501, (label, exc.code, exc.read())
            server.communicate(timeout=10)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        stream_response_worker = os.path.join(
            worker_tmp, "serve_threads_stream_response_worker.skein")
        with open(stream_response_worker, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "func parts() { yield \"a\"; yield \"b\"; }\n"
                "func handle(req) {\n"
                "  return {\"status\": 200, \"body_stream\": parts()};\n"
                "}\n"
            )
        stream_response_worker_path = stream_response_worker.replace(
            "\\", "\\\\")
        stream_response_script = os.path.join(
            worker_tmp, "serve_threads_stream_response.skein")
        with open(stream_response_script, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                f"let ts = std.http.serve_threads(s, 1, "
                f"\"{stream_response_worker_path}\", {{\"strategy\": "
                f"\"dispatch\", \"max_requests\": 1}});\n"
                "println(s[\"port\"]);\n"
                "std.io.flush(std.io.stdout);\n"
                "ts.join();\n"
            )
        server = subprocess.Popen(
            [os.path.abspath(BIN), stream_response_script],
            cwd=worker_tmp,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=os.environ.copy(),
        )
        try:
            port_line = server.stdout.readline().strip()
            assert port_line.isdigit(), (
                "stream_response server did not report a port: "
                f"{port_line!r} / {server.stderr.read()}"
            )
            try:
                urllib.request.urlopen(
                    f"http://127.0.0.1:{port_line}/stream", timeout=10)
                raise AssertionError("expected 501 for dispatch stream_response")
            except urllib.error.HTTPError as exc:
                assert exc.code == 501, (exc.code, exc.read())
            server.communicate(timeout=10)
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

        if FFI_SHIM:
            crash_manifest = os.path.join(
                worker_tmp, "serve_threads_crash.ffi.json")
            with open(crash_manifest, "w", encoding="utf-8") as handle:
                handle.write(
                    '{"ffi": 1, "types": {}, "functions": '
                    '{"shim_throw": {"result": "void", "args": []}}}\n'
                )
            crash_threads_worker = os.path.join(
                worker_tmp, "serve_threads_crash_worker.skein")
            with open(crash_threads_worker, "w",
                      encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    f"let lib = std.ffi.open("
                    f"{skein_string(os.path.abspath(FFI_SHIM))}, "
                    f"{skein_string(crash_manifest)});\n"
                    "func handle(req) {\n"
                    "  if (req[\"path\"] == \"/crash\") {\n"
                    "    std.ffi.call(lib, \"shim_throw\", []);\n"
                    "  }\n"
                    "  return {\"status\": 200, \"body\": \"ok\"};\n"
                    "}\n"
                )
            crash_threads_path = crash_threads_worker.replace("\\", "\\\\")
            crash_threads_script = os.path.join(
                worker_tmp, "serve_threads_crash.skein")
            with open(crash_threads_script, "w",
                      encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                    f"let ts = std.http.serve_threads(s, 1, "
                    f"\"{crash_threads_path}\", {{\"max_requests\": 2, "
                    f"\"restart_limit\": 1, "
                    f"\"strategy\": \"dispatch\"}});\n"
                    "println(s[\"port\"]);\n"
                    "std.io.flush(std.io.stdout);\n"
                    "ts.join();\n"
                )
            server = subprocess.Popen(
                [os.path.abspath(BIN), crash_threads_script],
                cwd=worker_tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=os.environ.copy(),
            )
            try:
                port_line = server.stdout.readline().strip()
                assert port_line.isdigit(), (
                    "serve_threads crash did not report a port: "
                    f"{port_line!r} / {server.stderr.read()}"
                )
                try:
                    urllib.request.urlopen(
                        f"http://127.0.0.1:{port_line}/crash", timeout=10)
                    raise AssertionError("expected 502 for worker crash")
                except urllib.error.HTTPError as exc:
                    assert exc.code == 502, (exc.code, exc.read())
                with urllib.request.urlopen(
                        f"http://127.0.0.1:{port_line}/ok",
                        timeout=10) as response:
                    assert response.status == 200
                    assert response.read() == b"ok"
                out, err = server.communicate(timeout=10)
                assert server.returncode == 0, err
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()

        reuse_supported = reuse_platform
        if reuse_supported:
            reuse_worker = os.path.join(
                worker_tmp, "serve_threads_reuse_worker.skein")
            with open(reuse_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(req) {\n"
                    "  return {\"status\": 200,\n"
                    "          \"headers\": {\"X-Worker\": "
                    "str(req[\"worker_id\"])},\n"
                    "          \"body\": \"ok\"};\n"
                    "}\n"
                )
            reuse_worker_path = reuse_worker.replace("\\", "\\\\")
            reuse_script = os.path.join(
                worker_tmp, "serve_threads_reuse.skein")
            with open(reuse_script, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                    f"let ts = std.http.serve_threads(s, 4, "
                    f"\"{reuse_worker_path}\", {{\"strategy\": "
                    f"\"reuse_port\", \"max_requests\": 8}});\n"
                    "println(\"info=\" + str(s[\"port\"]) + \":\" + "
                    "ts.strategy + \":\" + "
                    "str(ts.capabilities[\"reuse_port\"]));\n"
                    "std.io.flush(std.io.stdout);\n"
                    "println(\"served=\" + str(ts.join(30000)));\n"
                )
            server = subprocess.Popen(
                [os.path.abspath(BIN), reuse_script],
                cwd=worker_tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=os.environ.copy(),
            )
            try:
                info_line = server.stdout.readline().strip()
                assert info_line.startswith("info="), (
                    "serve_threads reuse_port did not report info: "
                    f"{info_line!r} / {server.stderr.read()}"
                )
                port_text, strategy, reuse_flag = (
                    info_line.split("=", 1)[1].split(":"))
                assert strategy == "reuse_port", info_line
                assert reuse_flag == "true", info_line
                reuse_results = []

                def reuse_hit(index):
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port_text}/r{index}")
                    with urllib.request.urlopen(
                            request, timeout=10) as response:
                        reuse_results.append(
                            (response.status, response.read(),
                             response.headers.get("X-Worker")))

                reuse_threads = [
                    threading.Thread(target=reuse_hit, args=(index,))
                    for index in range(8)
                ]
                for thread in reuse_threads:
                    thread.start()
                for thread in reuse_threads:
                    thread.join()
                assert len(reuse_results) == 8, reuse_results
                assert all(status == 200 and body == b"ok"
                           for status, body, _ in reuse_results), reuse_results
                workers_seen = {item[2] for item in reuse_results}
                assert len(workers_seen) >= 2, reuse_results
                out, err = server.communicate(timeout=10)
                assert server.returncode == 0, err
                assert "served=8" in out, out
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()

            sse_worker = os.path.join(
                worker_tmp, "serve_threads_reuse_sse_worker.skein")
            with open(sse_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func events() {\n"
                    "  yield \"data: one\\n\\n\";\n"
                    "  yield \"data: two\\n\\n\";\n"
                    "}\n"
                    "func handle(req) {\n"
                    "  return {\"status\": 200,\n"
                    "          \"headers\": {\"Content-Type\": "
                    "\"text/event-stream\"},\n"
                    "          \"body_stream\": events()};\n"
                    "}\n"
                )
            sse_worker_path = sse_worker.replace("\\", "\\\\")
            sse_script = os.path.join(
                worker_tmp, "serve_threads_reuse_sse.skein")
            with open(sse_script, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                    f"let ts = std.http.serve_threads(s, 1, "
                    f"\"{sse_worker_path}\", {{\"strategy\": "
                    f"\"reuse_port\", \"max_requests\": 1}});\n"
                    "println(\"info=\" + str(s[\"port\"]) + \":\" + "
                    "str(ts.capabilities[\"stream_response\"]));\n"
                    "std.io.flush(std.io.stdout);\n"
                    "ts.join();\n"
                )
            server = subprocess.Popen(
                [os.path.abspath(BIN), sse_script],
                cwd=worker_tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=os.environ.copy(),
            )
            try:
                info_line = server.stdout.readline().strip()
                assert info_line.startswith("info="), (
                    "reuse_port sse did not report info: "
                    f"{info_line!r} / {server.stderr.read()}"
                )
                assert info_line.endswith(":true"), info_line
                port_text = info_line.split("=", 1)[1].split(":")[0]
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port_text}/events",
                    headers={"Accept": "text/event-stream"})
                with urllib.request.urlopen(request, timeout=10) as response:
                    assert response.status == 200
                    body = response.read()
                    assert b"data: one" in body, body
                    assert b"data: two" in body, body
                out, err = server.communicate(timeout=10)
                assert server.returncode == 0, err
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()

            stream_worker = os.path.join(
                worker_tmp, "serve_threads_stream_worker.skein")
            with open(stream_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(req) {\n"
                    "  let out = \"\";\n"
                    "  let chunk = std.http.read_body(req);\n"
                    "  while (chunk != null) {\n"
                    "    out = out + bytes_to_string(chunk);\n"
                    "    chunk = std.http.read_body(req);\n"
                    "  }\n"
                    "  return {\"status\": 200, \"body\": \"pull=\" + out};\n"
                    "}\n"
                )
            stream_worker_path = stream_worker.replace("\\", "\\\\")
            stream_script = os.path.join(
                worker_tmp, "serve_threads_stream.skein")
            with open(stream_script, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "let s = std.http.listen(\"127.0.0.1\", 0);\n"
                    f"let ts = std.http.serve_threads(s, 2, "
                    f"\"{stream_worker_path}\", {{\"strategy\": "
                    f"\"reuse_port\", \"stream_request_body\": true, "
                    f"\"max_requests\": 1}});\n"
                    "println(\"info=\" + str(s[\"port\"]) + \":\" + "
                    "str(ts.capabilities[\"stream_request_body\"]));\n"
                    "std.io.flush(std.io.stdout);\n"
                    "println(\"served=\" + str(ts.join()));\n"
                )
            server = subprocess.Popen(
                [os.path.abspath(BIN), stream_script],
                cwd=worker_tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=os.environ.copy(),
            )
            try:
                info_line = server.stdout.readline().strip()
                assert info_line.startswith("info="), (
                    "serve_threads stream body did not report info: "
                    f"{info_line!r} / {server.stderr.read()}"
                )
                port_text = info_line.split("=", 1)[1].split(":")[0]
                assert info_line.endswith(":true"), info_line
                client = socket.create_connection(
                    ("127.0.0.1", int(port_text)), timeout=10)
                client.sendall(
                    b"POST /stream HTTP/1.1\r\n"
                    b"Host: 127.0.0.1\r\n"
                    b"Transfer-Encoding: chunked\r\n"
                    b"Connection: close\r\n\r\n"
                    b"6\r\nhello \r\n"
                    b"8\r\nchunked \r\n"
                    b"5\r\nworld\r\n"
                    b"0\r\n\r\n")
                response = b""
                while True:
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    response += chunk
                client.close()
                assert b"pull=hello chunked world" in response, response
                out, err = server.communicate(timeout=10)
                assert server.returncode == 0, err
                assert "served=1" in out, out
            finally:
                if server.poll() is None:
                    server.kill()
                    server.wait()

            maxconn_threads_worker = os.path.join(
                worker_tmp, "maxconn_threads_worker.skein")
            with open(maxconn_threads_worker, "w",
                      encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(req) {\n"
                    "  std.time.sleep(300);\n"
                    '  return {"status": 200, "body": "ok"};\n'
                    "}\n"
                )
            maxconn_threads_path = maxconn_threads_worker.replace(
                "\\", "\\\\")
            maxconn_process_worker = os.path.join(
                worker_tmp, "maxconn_process_worker.skein")
            with open(maxconn_process_worker, "w",
                      encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(req) {\n"
                    "  std.time.sleep(300);\n"
                    '  return {"status": 200, "body": "ok"};\n'
                    "}\n"
                )
            maxconn_process_path = maxconn_process_worker.replace(
                "\\", "\\\\")

            def run_connection_limit(label, mode, worker_path):
                script = os.path.join(
                    worker_tmp, f"maxconn_{mode}.skein")
                call = ("serve_threads" if mode == "threads"
                        else "serve_workers")
                with open(script, "w", encoding="utf-8") as handle:
                    handle.write(
                        "import std;\n"
                        'let s = std.http.listen("127.0.0.1", 0);\n'
                        'println(s["port"]);\n'
                        "std.io.flush(std.io.stdout);\n"
                        f"let srv = std.http.{call}(s, 2, "
                        f"\"{worker_path}\", {{\"strategy\": "
                        f"\"reuse_port\", \"max_connections\": 1, "
                        f"\"max_requests\": 2}});\n"
                        'println("ready");\n'
                        "std.io.flush(std.io.stdout);\n"
                        'println("served=" + str(srv.join()));\n'
                    )
                server = subprocess.Popen(
                    [os.path.abspath(BIN), script],
                    cwd=worker_tmp,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    env=os.environ.copy(),
                )
                try:
                    port_line = server.stdout.readline().strip()
                    assert port_line.isdigit(), (
                        f"{label} did not report a port: "
                        f"{port_line!r} / {server.stderr.read()}"
                    )
                    ready_line = server.stdout.readline().strip()
                    assert ready_line == "ready", (
                        f"{label} did not become ready: "
                        f"{ready_line!r} / {server.stderr.read()}"
                    )
                    port = int(port_line)
                    results = []
                    results_lock = threading.Lock()

                    def hit(index):
                        last_error = None
                        for _ in range(200):
                            try:
                                with urllib.request.urlopen(
                                        f"http://127.0.0.1:{port}/r{index}",
                                        timeout=10) as response:
                                    status = response.status
                                    body = response.read()
                                with results_lock:
                                    results.append((status, body))
                                return
                            except Exception as exc:
                                last_error = exc
                                time.sleep(0.02)
                        with results_lock:
                            results.append((None, repr(last_error)))

                    started = time.monotonic()
                    hits = [
                        threading.Thread(target=hit, args=(index,))
                        for index in range(2)
                    ]
                    for thread in hits:
                        thread.start()
                    for thread in hits:
                        thread.join()
                    elapsed = time.monotonic() - started
                    assert len(results) == 2, results
                    assert all(status == 200 and body == b"ok"
                               for status, body in results), results
                    assert elapsed >= 0.5, (
                        f"{label} allowed concurrent connections: "
                        f"{elapsed:.3f}s"
                    )
                    out, err = server.communicate(timeout=20)
                    assert server.returncode == 0, (label, err)
                    assert "served=2" in out, (label, out)
                finally:
                    if server.poll() is None:
                        server.kill()
                        server.wait()

            run_connection_limit(
                "serve_threads max_connections", "threads",
                maxconn_threads_path)
            run_connection_limit(
                "serve_workers max_connections", "workers",
                maxconn_process_path)

            ws_worker = os.path.join(
                worker_tmp, "serve_ws_worker.skein")
            with open(ws_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(req) {\n"
                    "  let ws = std.websocket.accept(req);\n"
                    "  let m = std.websocket.receive(ws);\n"
                    '  std.websocket.send(ws, "echo:" + m["data"]);\n'
                    "  std.websocket.close(ws);\n"
                    '  return {"status": 101};\n'
                    "}\n"
                )
            ws_worker_path = ws_worker.replace("\\", "\\\\")

            def run_ws_case(label, source, wait_ready):
                script = os.path.join(
                    worker_tmp, "ws_" + label + ".skein")
                with open(script, "w", encoding="utf-8") as handle:
                    handle.write(source)
                server = subprocess.Popen(
                    [os.path.abspath(BIN), script],
                    cwd=worker_tmp,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    env=os.environ.copy(),
                )
                try:
                    port_line = server.stdout.readline().strip()
                    assert port_line.isdigit(), (
                        f"{label} did not report a port: "
                        f"{port_line!r} / {server.stderr.read()}"
                    )
                    if wait_ready:
                        ready_line = server.stdout.readline().strip()
                        assert ready_line == "ready", (
                            f"{label} did not become ready: "
                            f"{ready_line!r} / {server.stderr.read()}"
                        )
                    port = int(port_line)
                    client = None
                    last_error = None
                    for _ in range(200):
                        try:
                            client = socket.create_connection(
                                ("127.0.0.1", port), timeout=5)
                            break
                        except OSError as exc:
                            last_error = exc
                            time.sleep(0.02)
                    assert client is not None, (label, last_error)
                    key = base64.b64encode(b"0123456789abcdef").decode()
                    client.sendall(
                        b"GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                        b"Connection: Upgrade\r\nUpgrade: websocket\r\n"
                        b"Sec-WebSocket-Version: 13\r\n"
                        b"Sec-WebSocket-Key: " + key.encode() +
                        b"\r\n\r\n")
                    response = b""
                    while b"\r\n\r\n" not in response:
                        chunk = client.recv(4096)
                        if not chunk:
                            break
                        response += chunk
                    assert b"101 Switching Protocols" in response, response
                    assert ("Sec-WebSocket-Accept: " +
                            ws_accept_key(key)).encode() in response
                    ws_write_frame(client, 1, b"hello", True)
                    opcode, payload = ws_read_frame(client)
                    assert opcode == 1 and payload == b"echo:hello", (
                        label, opcode, payload)
                    opcode, _ = ws_read_frame(client)
                    assert opcode == 8, (label, opcode)
                    client.close()
                    out, err = server.communicate(timeout=20)
                    assert server.returncode == 0, (label, err)
                    assert "served=1" in out, (label, out)
                finally:
                    if server.poll() is None:
                        server.kill()
                        server.wait()

            run_ws_case(
                "serve",
                "import std;\n"
                'let s = std.http.listen("127.0.0.1", 0);\n'
                'println(s["port"]);\n'
                "std.io.flush(std.io.stdout);\n"
                "let n = std.http.serve(s, func(req) {\n"
                "  let ws = std.websocket.accept(req);\n"
                "  let m = std.websocket.receive(ws);\n"
                '  std.websocket.send(ws, "echo:" + m["data"]);\n'
                "  std.websocket.close(ws);\n"
                '  return {"status": 101};\n'
                "}, 1);\n"
                'println("served=" + str(n));\n',
                False)
            run_ws_case(
                "threads",
                "import std;\n"
                'let s = std.http.listen("127.0.0.1", 0);\n'
                'println(s["port"]);\n'
                "std.io.flush(std.io.stdout);\n"
                f"let ts = std.http.serve_threads(s, 2, "
                f"\"{ws_worker_path}\", {{\"strategy\": \"reuse_port\", "
                f"\"max_requests\": 1}});\n"
                'println("ready");\n'
                "std.io.flush(std.io.stdout);\n"
                'println("served=" + str(ts.join()));\n',
                True)
            run_ws_case(
                "workers",
                "import std;\n"
                'let s = std.http.listen("127.0.0.1", 0);\n'
                'println(s["port"]);\n'
                "std.io.flush(std.io.stdout);\n"
                f"let ps = std.http.serve_workers(s, 2, "
                f"\"{ws_worker_path}\", {{\"strategy\": \"reuse_port\", "
                f"\"max_requests\": 1}});\n"
                'println("ready");\n'
                "std.io.flush(std.io.stdout);\n"
                'println("served=" + str(ps.join()));\n',
                True)

            h2c_worker = os.path.join(
                worker_tmp, "serve_h2c_worker.skein")
            with open(h2c_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(req) {\n"
                    '  return {"status": 200, "body": "ok:" + '
                    'req["path"]};\n'
                    "}\n"
                )
            h2c_worker_path = h2c_worker.replace("\\", "\\\\")

            def run_h2c_case(label, call):
                script = os.path.join(
                    worker_tmp, "h2c_" + label + ".skein")
                with open(script, "w", encoding="utf-8") as handle:
                    handle.write(
                        "import std;\n"
                        'let s = std.http.listen("127.0.0.1", 0);\n'
                        "std.io.flush(std.io.stdout);\n"
                        f"let srv = std.http.{call}(s, 2, "
                        f"\"{h2c_worker_path}\", {{\"strategy\": "
                        f"\"reuse_port\", \"max_requests\": 2}});\n"
                        'println("info=" + str(s["port"]) + ":" + '
                        'str(srv.capabilities["http2"]));\n'
                        "std.io.flush(std.io.stdout);\n"
                        'println("served=" + str(srv.join()));\n'
                    )
                server = subprocess.Popen(
                    [os.path.abspath(BIN), script],
                    cwd=worker_tmp,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    env=os.environ.copy(),
                )
                try:
                    info_line = server.stdout.readline().strip()
                    assert info_line.startswith("info="), (
                        f"{label} did not report info: "
                        f"{info_line!r} / {server.stderr.read()}"
                    )
                    port_text, http2_flag = (
                        info_line.split("=", 1)[1].split(":"))
                    assert http2_flag == "true", info_line
                    client = run(
                        "import std;\n"
                        f'let a = std.http2.get('
                        f'"http://127.0.0.1:{port_text}/h2");\n'
                        'println(str(a["status"]) + " " + '
                        'a["protocol"]);\n'
                        'println(bytes_to_string(a["body"]));\n'
                        f'let b = std.http.get('
                        f'"http://127.0.0.1:{port_text}/h1");\n'
                        'println(str(b["status"]));\n'
                        'println(bytes_to_string(b["body"]));\n',
                        timeout=20,
                    )
                    assert client.returncode == 0, client.stderr
                    assert "200 h2" in client.stdout, client.stdout
                    assert "ok:/h2" in client.stdout, client.stdout
                    assert "ok:/h1" in client.stdout, client.stdout
                    out, err = server.communicate(timeout=20)
                    assert server.returncode == 0, (label, err)
                    assert "served=2" in out, (label, out)
                finally:
                    if server.poll() is None:
                        server.kill()
                        server.wait()

            run_h2c_case("threads", "serve_threads")
            run_h2c_case("workers", "serve_workers")

            tls_probe = run(
                'import std;\nstd.dict.has(std, "tls")\n')
            tls_enabled = "= true" in tls_probe.stdout
            if tls_enabled and shutil.which("openssl"):
                tls_cert = os.path.join(worker_tmp, "h2_cert.pem")
                tls_key = os.path.join(worker_tmp, "h2_key.pem")
                cert_proc = subprocess.run(
                    ["openssl", "req", "-x509", "-newkey", "rsa:2048",
                     "-nodes", "-keyout", tls_key, "-out", tls_cert,
                     "-days", "1", "-subj", "/CN=localhost",
                     "-addext", "subjectAltName=DNS:localhost"],
                    capture_output=True, text=True)
                assert cert_proc.returncode == 0, cert_proc.stderr
                h2_script = os.path.join(
                    worker_tmp, "serve_threads_h2_tls.skein")
                with open(h2_script, "w", encoding="utf-8") as handle:
                    handle.write(
                        "import std;\n"
                        f"let s = std.http.listen_tls(\"127.0.0.1\", 0, "
                        f"\"{tls_cert.replace(chr(92), chr(92) * 2)}\", "
                        f"\"{tls_key.replace(chr(92), chr(92) * 2)}\");\n"
                        f"let ts = std.http.serve_threads(s, 2, "
                        f"\"{threads_worker_path}\", {{\"strategy\": "
                        f"\"reuse_port\", \"max_requests\": 2}});\n"
                        "println(\"info=\" + str(s[\"port\"]) + \":\" + "
                        "str(ts.capabilities[\"http2\"]));\n"
                        "std.io.flush(std.io.stdout);\n"
                        "println(\"served=\" + str(ts.join()));\n"
                    )
                server = subprocess.Popen(
                    [os.path.abspath(BIN), h2_script],
                    cwd=worker_tmp,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    env=os.environ.copy(),
                )
                try:
                    info_line = server.stdout.readline().strip()
                    assert info_line.startswith("info="), (
                        "serve_threads h2 did not report info: "
                        f"{info_line!r} / {server.stderr.read()}"
                    )
                    port_text = info_line.split("=", 1)[1].split(":")[0]
                    assert info_line.endswith(":true"), info_line
                    client = run(
                        "import std;\n"
                        f'let a = std.http2.get('
                        f'"https://127.0.0.1:{port_text}/h2", '
                        '{"tls": {"verify": false}});\n'
                        'println(str(a["status"]) + " " + '
                        'a["protocol"]);\n'
                        'println(bytes_to_string(a["body"]));\n'
                        f'let b = std.http.get('
                        f'"https://127.0.0.1:{port_text}/h1", '
                        '{"tls": {"verify": false}});\n'
                        'println(str(b["status"]));\n'
                        'println(bytes_to_string(b["body"]));\n',
                        timeout=20,
                    )
                    assert client.returncode == 0, client.stderr
                    assert "200 h2" in client.stdout, client.stdout
                    assert "ok:/h2" in client.stdout, client.stdout
                    assert "ok:/h1" in client.stdout, client.stdout
                    out, err = server.communicate(timeout=10)
                    assert server.returncode == 0, err
                    assert "served=2" in out, out
                finally:
                    if server.poll() is None:
                        server.kill()
                        server.wait()

                process_h2_script = os.path.join(
                    worker_tmp, "serve_workers_h2_tls.skein")
                with open(process_h2_script, "w",
                          encoding="utf-8") as handle:
                    handle.write(
                        "import std;\n"
                        f"let s = std.http.listen_tls(\"127.0.0.1\", 0, "
                        f"\"{tls_cert.replace(chr(92), chr(92) * 2)}\", "
                        f"\"{tls_key.replace(chr(92), chr(92) * 2)}\");\n"
                        f"let ps = std.http.serve_workers(s, 2, "
                        f"\"{workers_reuse_path}\", {{\"strategy\": "
                        f"\"reuse_port\", \"max_requests\": 2}});\n"
                        "println(\"info=\" + str(s[\"port\"]) + \":\" + "
                        "str(ps.capabilities[\"http2\"]));\n"
                        "std.io.flush(std.io.stdout);\n"
                        "println(\"served=\" + str(ps.join()));\n"
                    )
                server = subprocess.Popen(
                    [os.path.abspath(BIN), process_h2_script],
                    cwd=worker_tmp,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    env=os.environ.copy(),
                )
                try:
                    info_line = server.stdout.readline().strip()
                    assert info_line.startswith("info="), (
                        "serve_workers h2 did not report info: "
                        f"{info_line!r} / {server.stderr.read()}"
                    )
                    port_text = info_line.split("=", 1)[1].split(":")[0]
                    assert info_line.endswith(":true"), info_line
                    client = run(
                        "import std;\n"
                        f'let a = std.http2.get('
                        f'"https://127.0.0.1:{port_text}/h2", '
                        '{"tls": {"verify": false}});\n'
                        'println(str(a["status"]) + " " + '
                        'a["protocol"]);\n'
                        'println(bytes_to_string(a["body"]));\n'
                        f'let b = std.http.get('
                        f'"https://127.0.0.1:{port_text}/h1", '
                        '{"tls": {"verify": false}});\n'
                        'println(str(b["status"]));\n'
                        'println(bytes_to_string(b["body"]));\n',
                        timeout=30,
                    )
                    assert client.returncode == 0, client.stderr
                    assert "200 h2" in client.stdout, client.stdout
                    assert client.stdout.count("ok") >= 2, client.stdout
                    out, err = server.communicate(timeout=30)
                    assert server.returncode == 0, err
                    assert "served=2" in out, out
                finally:
                    if server.poll() is None:
                        server.kill()
                        server.wait()
        else:
            check(
                "std.http serve_threads reuse_port unsupported",
                "import std;\n"
                'let s = std.http.listen("127.0.0.1", 0);\n'
                'let ts = std.http.serve_threads(s, 1, "missing.skein", '
                '{"strategy": "reuse_port"});\n',
                stderr="reuse_port is not supported",
            )

        with tempfile.TemporaryDirectory() as deadlock_tmp:
            package_dir = os.path.join(deadlock_tmp, "deadlockpkg")
            os.makedirs(package_dir)
            deadlock_worker = os.path.join(
                deadlock_tmp, "deadlock_worker.skein")
            with open(deadlock_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(payload) { return payload; }\n"
                )
            deadlock_worker_path = deadlock_worker.replace("\\", "\\\\")
            with open(os.path.join(package_dir, "deadlockpkg.skein"),
                      "w", encoding="utf-8") as handle:
                handle.write(
                    "package deadlockpkg {\n"
                    "  import std;\n"
                    f"  let pool = std.thread.pool(1, "
                    f"\"{deadlock_worker_path}\");\n"
                    '  let f = pool.submit({"v": 7});\n'
                    "  let r = f.wait();\n"
                    '  export let value = r["value"]["v"];\n'
                    "  pool.close();\n"
                    "  pool.join();\n"
                    "}\n"
                )
            deadlock_main = os.path.join(deadlock_tmp, "main.skein")
            with open(deadlock_main, "w", encoding="utf-8") as handle:
                handle.write(
                    "import deadlockpkg;\n"
                    "print(deadlockpkg.value);\n"
                )
            proc = subprocess.run(
                [os.path.abspath(BIN), deadlock_main],
                cwd=deadlock_tmp,
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert proc.returncode == 0, (
                "pool inside imported module failed: "
                f"{proc.stdout!r} {proc.stderr!r}"
            )
            assert proc.stdout.strip() == "7", proc.stdout

            spawn_dir = os.path.join(deadlock_tmp, "spawnpkg")
            os.makedirs(spawn_dir)
            spawn_worker = os.path.join(
                deadlock_tmp, "deadlock_spawn_worker.skein")
            with open(spawn_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func main(ctx) {\n"
                    '    return {"x": ctx.payload["x"] + 1};\n'
                    "}\n"
                )
            spawn_worker_path = spawn_worker.replace("\\", "\\\\")
            with open(os.path.join(spawn_dir, "spawnpkg.skein"),
                      "w", encoding="utf-8") as handle:
                handle.write(
                    "package spawnpkg {\n"
                    "  import std;\n"
                    f"  let t = std.thread.spawn(\"{spawn_worker_path}\", "
                    'std.thread.Message.from({"x": 41}));\n'
                    "  let r = t.join();\n"
                    '  export let value = r["value"]["x"];\n'
                    "}\n"
                )
            spawn_main = os.path.join(deadlock_tmp, "spawn_main.skein")
            with open(spawn_main, "w", encoding="utf-8") as handle:
                handle.write(
                    "import spawnpkg;\n"
                    "print(spawnpkg.value);\n"
                )
            proc = subprocess.run(
                [os.path.abspath(BIN), spawn_main],
                cwd=deadlock_tmp,
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert proc.returncode == 0, (
                "spawn inside imported module failed: "
                f"{proc.stdout!r} {proc.stderr!r}"
            )
            assert proc.stdout.strip() == "42", proc.stdout

            transfer_pkg_dir = os.path.join(deadlock_tmp, "threadpkg")
            os.makedirs(transfer_pkg_dir)
            with open(os.path.join(transfer_pkg_dir, "threadpkg.skein"),
                      "w", encoding="utf-8") as handle:
                handle.write(
                    "package threadpkg {\n"
                    "  let base = 10;\n"
                    "  func helper(x) { return x + base; }\n"
                    "  export let offset = 3;\n"
                    "  export func calc(x) { return helper(x) + offset; }\n"
                    "}\n"
                )
            transfer_main = os.path.join(deadlock_tmp,
                                         "thread_import_main.skein")
            with open(transfer_main, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "import threadpkg;\n"
                    "func make_worker() {\n"
                    "    thread func worker(ctx) {\n"
                    '        return {"v": threadpkg.calc('
                    'ctx.payload["n"])};\n'
                    "    }\n"
                    "    return worker;\n"
                    "}\n"
                    "let w = make_worker();\n"
                    "let t = std.thread.spawn(w, "
                    'std.thread.Message.from({"n": 4}));\n'
                    "let r = t.join();\n"
                    'print(r["value"]["v"]);\n'
                )
            proc = subprocess.run(
                [os.path.abspath(BIN), transfer_main],
                cwd=deadlock_tmp,
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert proc.returncode == 0, (
                "thread func with imported module failed: "
                f"{proc.stdout!r} {proc.stderr!r}"
            )
            assert proc.stdout.strip() == "17", proc.stdout

            encode_path = os.path.join(deadlock_tmp, "encode_main.skein")
            with open(encode_path, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "import threadpkg;\n"
                    "func make_worker() {\n"
                    "    thread func worker(ctx) {\n"
                    '        return {"v": threadpkg.calc('
                    'ctx.payload["n"])};\n'
                    "    }\n"
                    "    return worker;\n"
                    "}\n"
                    'write_bytes("thread_payload.bin", '
                    "std.thread.encode(make_worker()));\n"
                    'print("encoded");\n'
                )
            decode_path = os.path.join(deadlock_tmp, "decode_main.skein")
            with open(decode_path, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    'let blob = read_bytes("thread_payload.bin");\n'
                    "let t = std.thread.spawn_encoded(blob, "
                    'std.thread.Message.from({"n": 4}));\n'
                    "let r = t.join();\n"
                    'print(r["value"]["v"]);\n'
                )
            enc = subprocess.run(
                [os.path.abspath(BIN), encode_path],
                cwd=deadlock_tmp,
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert enc.returncode == 0, (
                "thread encode failed: "
                f"{enc.stdout!r} {enc.stderr!r}"
            )
            shutil.rmtree(os.path.join(deadlock_tmp, "threadpkg"))
            dec = subprocess.run(
                [os.path.abspath(BIN), decode_path],
                cwd=deadlock_tmp,
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert dec.returncode == 0, (
                "thread decode failed: "
                f"{dec.stdout!r} {dec.stderr!r}"
            )
            assert dec.stdout.strip() == "17", dec.stdout

            typed_encode = os.path.join(deadlock_tmp,
                                        "typed_encode_main.skein")
            with open(typed_encode, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "struct Point { x: int = 1; y: int = 2; }\n"
                    "impl Point { func add(self, n) { "
                    "return self.x + n; } }\n"
                    "enum E { A { n: int; }; B; }\n"
                    "let p = Point { x: 3, y: 4 };\n"
                    "let e = E.A { n: 5 };\n"
                    "func make_worker(pv, ev) {\n"
                    "    thread func worker(ctx) {\n"
                    "        let q = Point {};\n"
                    "        let f = E.A { n: ev.n + 1 };\n"
                    '        return {"x": q.x, "y": q.y, "n": f.n, '
                    '"m": pv.add(ev.n)};\n'
                    "    }\n"
                    "    return worker;\n"
                    "}\n"
                    'write_bytes("typed_payload.bin", '
                    "std.thread.encode(make_worker(p, e)));\n"
                )
            typed_decode = os.path.join(deadlock_tmp,
                                        "typed_decode_main.skein")
            with open(typed_decode, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    'let blob = read_bytes("typed_payload.bin");\n'
                    "let t = std.thread.spawn_encoded(blob, "
                    'std.thread.Message.from({"n": 0}));\n'
                    "let r = t.join();\n"
                    'print(str(r["value"]["x"]) + ":" + '
                    'str(r["value"]["y"]) + ":" + '
                    'str(r["value"]["n"]) + ":" + '
                    'str(r["value"]["m"]));\n'
                )
            enc = subprocess.run(
                [os.path.abspath(BIN), typed_encode],
                cwd=deadlock_tmp,
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert enc.returncode == 0, (
                "typed thread encode failed: "
                f"{enc.stdout!r} {enc.stderr!r}"
            )
            dec = subprocess.run(
                [os.path.abspath(BIN), typed_decode],
                cwd=deadlock_tmp,
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert dec.returncode == 0, (
                "typed thread decode failed: "
                f"{dec.stdout!r} {dec.stderr!r}"
            )
            assert dec.stdout.strip() == "1:2:6:8", dec.stdout

            http_pkg_dir = os.path.join(deadlock_tmp, "httpservepkg")
            os.makedirs(http_pkg_dir)
            http_worker = os.path.join(
                deadlock_tmp, "deadlock_http_worker.skein")
            with open(http_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(req) {\n"
                    '    return {"status": 200, "body": "ok"};\n'
                    "}\n"
                )
            http_worker_path = http_worker.replace("\\", "\\\\")
            with open(os.path.join(http_pkg_dir, "httpservepkg.skein"),
                      "w", encoding="utf-8") as handle:
                handle.write(
                    "package httpservepkg {\n"
                    "  import std;\n"
                    '  let s = std.http.listen("127.0.0.1", 0);\n'
                    '  export let port = s["port"];\n'
                    f"  export let ts = std.http.serve_threads(s, 1, "
                    f"\"{http_worker_path}\", {{\"max_requests\": 1}});\n"
                    "}\n"
                )
            http_main = os.path.join(deadlock_tmp, "http_main.skein")
            with open(http_main, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "import httpservepkg;\n"
                    "println(httpservepkg.port);\n"
                    'let r = std.http.get("http://127.0.0.1:" + '
                    'str(httpservepkg.port) + "/x");\n'
                    'println(r["status"]);\n'
                    "println(httpservepkg.ts.join());\n"
                )
            proc = subprocess.run(
                [os.path.abspath(BIN), http_main],
                cwd=deadlock_tmp,
                capture_output=True,
                text=True,
                timeout=20,
            )
            assert proc.returncode == 0, (
                "serve_threads inside imported module failed: "
                f"{proc.stdout!r} {proc.stderr!r}"
            )
            http_lines = proc.stdout.splitlines()
            assert len(http_lines) >= 3, proc.stdout
            assert http_lines[0].isdigit(), proc.stdout
            assert http_lines[1] == "200", proc.stdout
            assert http_lines[2] == "1", proc.stdout

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
            "std.thread message copy on write",
            "import std;\n"
            'let body = bytes_from_string("ab");\n'
            'let items = [1, 2];\n'
            'let msg = std.thread.Message.from('
            '{"x": 1, "items": items, "body": body});\n'
            'items[0] = 9;\n'
            'body[0] = 65;\n'
            'print("items="); print(msg.payload["items"][0]);\n'
            'print("body="); print(bytes_to_string(msg.payload["body"]));\n'
            'let view = msg.payload;\n'
            'view["items"][0] = 7;\n'
            'print("view="); print(msg.payload["items"][0]);\n',
            stdout_all=["items=1", "body=ab", "view=1"],
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
            check(
                "std.http serve_threads join timeout",
                "import std;\n"
                'let s = std.http.listen("127.0.0.1", 0);\n'
                f'let ts = std.http.serve_threads(s, 1, '
                f'"{thread_worker_path}", '
                '{"strategy": "dispatch", "max_requests": 0});\n'
                "ts.join(10);\n",
                stderr="ThreadServer.join timed out",
            )
            check(
                "std.http serve_threads shutdown",
                "import std;\n"
                'let s = std.http.listen("127.0.0.1", 0);\n'
                f'let ts = std.http.serve_threads(s, 1, '
                f'"{thread_worker_path}", '
                '{"strategy": "dispatch", "max_requests": 0});\n'
                'print(ts.shutdown(2000));\n',
                stdout="0",
            )

            message_worker = os.path.join(
                thread_tmp, "thread_message_worker.skein")
            with open(message_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "func handle(payload) {\n"
                    "    return payload;\n"
                    "}\n"
                )
            message_worker_path = message_worker.replace("\\", "\\\\")
            check(
                "std.thread message container payloads",
                "import std;\n"
                "struct P { x: int; text: string; }\n"
                "enum E { A; B { n: int; }; }\n"
                'let l = std.lru.new(4);\n'
                'l = std.lru.put(l, "a", 1)["lru"];\n'
                'l = std.lru.put(l, "b", 2)["lru"];\n'
                'let d = std.deque.from_list([1, 2, 3]);\n'
                'let s = std.set.from_list([7, 8, 9]);\n'
                'let payload = {"p": P { x: 42, text: "hi" },\n'
                '               "e": E.B { n: 5 }, "l": l, "d": d, "s": s};\n'
                'let msg = std.thread.Message.from(payload);\n'
                f'let pool = std.thread.pool(1, "{message_worker_path}");\n'
                'let r = pool.submit(msg.payload).wait();\n'
                'let v = r["value"];\n'
                'print("p="); print(v["p"].x); print(":"); '
                'print(v["p"].text); print("|");\n'
                'let en = match (v["e"]) { case E.B { n } => n; '
                'default => 0; };\n'
                'print("e="); print(en); print("|");\n'
                'let lr = std.lru.get(v["l"], "a");\n'
                'print("l="); print(lr["found"]); print(":"); '
                'print(lr["value"]); print(":"); '
                'print(std.lru.len(v["l"])); print("|");\n'
                'print("d="); print(std.deque.size(v["d"])); print(":"); '
                'print(std.deque.to_list(v["d"])[2]); print("|");\n'
                'print("s="); print(std.set.size(v["s"])); print(":"); '
                'print(std.set.has(v["s"], 8)); print("|");\n'
                'pool.close();\n'
                'pool.join();\n',
                stdout="p=42:hi|e=5|l=true:1:2|d=3:3|s=3:true|",
            )
            pool_cow_worker = os.path.join(
                thread_tmp, "thread_pool_cow.skein")
            with open(pool_cow_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "func handle(payload) {\n"
                    "    payload[\"nested\"][\"items\"][0] = 99;\n"
                    "    payload[\"nested\"][\"inner\"][\"v\"] = 42;\n"
                    "    return payload;\n"
                    "}\n"
                )
            pool_cow_worker_path = pool_cow_worker.replace("\\", "\\\\")
            check(
                "std.thread pool result copy on write",
                "import std;\n"
                f'let pool = std.thread.pool(1, '
                f'"{pool_cow_worker_path}");\n'
                'let items = [1, 2];\n'
                'let payload = {"nested": {"items": items, '
                '"inner": {"v": 7}}};\n'
                'let r = pool.submit(payload).wait();\n'
                'print("child="); '
                'print(r["value"]["nested"]["items"][0]); print(":"); '
                'print(r["value"]["nested"]["inner"]["v"]);\n'
                'print("parent="); '
                'print(payload["nested"]["items"][0]); print(":"); '
                'print(payload["nested"]["inner"]["v"]);\n'
                'pool.close();\n'
                'pool.join();\n',
                stdout_all=["child=99:42", "parent=1:7"],
            )
            stdio_worker = os.path.join(
                thread_tmp, "thread_stdio_worker.skein")
            with open(stdio_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func main(ctx) {\n"
                    "    let id = ctx.payload[\"id\"];\n"
                    "    for (let i = 0; i < 100; i++) {\n"
                    "        println(\"line:\" + str(id) + \":\" + "
                    "str(i));\n"
                    "    }\n"
                    "    return {\"id\": id};\n"
                    "}\n"
                )
            stdio_worker_path = stdio_worker.replace("\\", "\\\\")
            stdio_source = (
                "import std;\n"
                "let threads = [];\n"
                "for (let i = 0; i < 4; i++) {\n"
                '    let msg = std.thread.Message.from({"id": i});\n'
                f'    let t = std.thread.spawn("{stdio_worker_path}", '
                f'msg);\n'
                "    threads = std.list.append(threads, t);\n"
                "}\n"
                "for (let t in threads) {\n"
                "    let r = t.join();\n"
                "}\n"
                'println("done");\n'
            )
            proc = run(stdio_source, timeout=60)
            assert proc.returncode == 0, (proc.stdout, proc.stderr)
            stdio_lines = [
                line for line in proc.stdout.splitlines()
                if line.startswith("line:")
            ]
            assert len(stdio_lines) == 400, (
                len(stdio_lines), proc.stdout[:2000])
            for line in stdio_lines:
                parts = line.split(":")
                assert (len(parts) == 3 and parts[0] == "line" and
                        parts[1].isdigit() and parts[2].isdigit()), line
            assert "done" in proc.stdout, proc.stdout

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
                "std.thread spawn_source",
                "import std;\n"
                'let src = "func main(ctx) {\\n'
                '    return {\\"v\\": ctx.payload[\\"n\\"] + 1};\\n'
                '}\\n";\n'
                'let t = std.thread.spawn_source(src, '
                'std.thread.Message.from({"n": 4}));\n'
                'let r = t.join();\n'
                'print("v1=" + str(r["value"]["v"]) + "|");\n'
                'let src2 = "func run(ctx) {\\n'
                '    return {\\"v\\": ctx.payload[\\"n\\"] * 3};\\n'
                '}\\n";\n'
                'let t2 = std.thread.spawn_source(src2, '
                'std.thread.Message.from({"n": 5}), {"entry": "run"});\n'
                'let r2 = t2.join();\n'
                'print("v2=" + str(r2["value"]["v"]) + "|");\n',
                stdout_all=["v1=5|", "v2=15|"],
            )
            check(
                "std.thread thread func proto entry",
                "import std;\n"
                "thread func worker(ctx) {\n"
                '    return {"v": ctx.payload["n"] + 1};\n'
                "}\n"
                "let t = std.thread.spawn(worker, "
                'std.thread.Message.from({"n": 4}));\n'
                "let r = t.join();\n"
                'print("v=" + str(r["value"]["v"]) + "|");\n'
                "func plain(ctx) {\n"
                '    return {"v": ctx.payload["n"]};\n'
                "}\n"
                "let t2 = std.thread.spawn_function(plain, "
                'std.thread.Message.from({"n": 7}));\n'
                "let r2 = t2.join();\n"
                'print("f=" + str(r2["value"]["v"]) + "|");\n'
                "thread func std_worker(ctx) {\n"
                "    let items = std.list.append([1], "
                'ctx.payload["n"]);\n'
                '    return {"v": items[1]};\n'
                "}\n"
                "let t3 = std.thread.spawn(std_worker, "
                'std.thread.Message.from({"n": 9}));\n'
                "let r3 = t3.join();\n"
                'print("s=" + str(r3["value"]["v"]) + "|");\n'
                "let base = 10;\n"
                'let config = {"factor": 3};\n'
                "thread func glob_worker(ctx) {\n"
                '    return {"v": ctx.payload["n"] + base};\n'
                "}\n"
                "thread func dict_worker(ctx) {\n"
                "    return {\"v\": ctx.payload[\"n\"] * "
                "config[\"factor\"]};\n"
                "}\n"
                "let t4 = std.thread.spawn(glob_worker, "
                'std.thread.Message.from({"n": 4}));\n'
                "let r4 = t4.join();\n"
                "let t5 = std.thread.spawn(dict_worker, "
                'std.thread.Message.from({"n": 4}));\n'
                "let r5 = t5.join();\n"
                'print("g=" + str(r4["value"]["v"]) + ":" + '
                'str(r5["value"]["v"]) + "|");\n'
                "let field_helper = func() { return 1; };\n"
                "thread func shadow_worker(ctx) {\n"
                '    return {"v": ctx.payload["field_helper"] + base};\n'
                "}\n"
                "let t6 = std.thread.spawn(shadow_worker, "
                'std.thread.Message.from({"field_helper": 4}));\n'
                "let r6 = t6.join();\n"
                'print("h=" + str(r6["value"]["v"]) + "|");\n',
                stdout_all=["v=5|", "f=7|", "s=9|", "g=14:12|", "h=14|"],
            )
            check(
                "std.thread thread func captures upvalues",
                "import std;\n"
                "let base = 10;\n"
                "func make_worker(n) {\n"
                "    thread func worker(ctx) {\n"
                '        return {"v": ctx.payload["n"] + n};\n'
                "    }\n"
                "    return worker;\n"
                "}\n"
                "let w = make_worker(10);\n"
                "let t = std.thread.spawn(w, "
                'std.thread.Message.from({"n": 4}));\n'
                "let r = t.join();\n"
                'print("c=" + str(r["value"]["v"]) + "|");\n'
                "func make_data() {\n"
                "    let data = [1, 2, 3];\n"
                "    thread func worker(ctx) {\n"
                "        return data;\n"
                "    }\n"
                "    return worker;\n"
                "}\n"
                "let w2 = make_data();\n"
                "let t2 = std.thread.spawn(w2, "
                'std.thread.Message.from({"n": 0}));\n'
                "let r2 = t2.join();\n"
                'print("d=" + str(r2["value"][1]) + "|");\n'
                "func make_mut() {\n"
                '    let state = {"n": 1};\n'
                "    thread func worker(ctx) {\n"
                '        state["n"] = state["n"] + 1;\n'
                '        return {"v": state["n"]};\n'
                "    }\n"
                '    return {"run": worker, "state": state};\n'
                "}\n"
                "let p = make_mut();\n"
                "let t3 = std.thread.spawn(p[\"run\"], "
                'std.thread.Message.from({"n": 0}));\n'
                "let r3 = t3.join();\n"
                'print("e=" + str(r3["value"]["v"]) + ":" + '
                'str(p["state"]["n"]) + "|");\n'
                "let shadow_fn = func() { return 1; };\n"
                "func make_shadow(n) {\n"
                "    let shadow_fn = n + 1;\n"
                "    thread func worker(ctx) {\n"
                '        return {"v": shadow_fn + base};\n'
                "    }\n"
                "    return worker;\n"
                "}\n"
                "let w3 = make_shadow(7);\n"
                "let t4 = std.thread.spawn(w3, "
                'std.thread.Message.from({"n": 0}));\n'
                "let r4 = t4.join();\n"
                'print("f=" + str(r4["value"]["v"]) + "|");\n',
                stdout_all=["c=14|", "d=2|", "e=2:1|", "f=18|"],
            )
            check(
                "std.thread transfers function values",
                "import std;\n"
                "let base = 100;\n"
                "func make_adder(n) {\n"
                "    return func(x) { return x + n + base; };\n"
                "}\n"
                "let f1 = make_adder(1);\n"
                "let f2 = make_adder(2);\n"
                "func make_worker() {\n"
                "    thread func worker(ctx) {\n"
                "        return {\"v\": f1(ctx.payload[\"n\"]) + "
                "f2(ctx.payload[\"n\"])};\n"
                "    }\n"
                "    return worker;\n"
                "}\n"
                "let w = make_worker();\n"
                "let t = std.thread.spawn(w, "
                'std.thread.Message.from({"n": 10}));\n'
                "let r = t.join();\n"
                'print("two=" + str(r["value"]["v"]) + "|");\n'
                "func helper(x) { return x * 2; }\n"
                "func helper2(x) { return helper(x) + 1; }\n"
                "thread func global_worker(ctx) {\n"
                '    return {"v": helper2(ctx.payload["n"])};\n'
                "}\n"
                "let t2 = std.thread.spawn(global_worker, "
                'std.thread.Message.from({"n": 5}));\n'
                "let r2 = t2.join();\n"
                'print("chain=" + str(r2["value"]["v"]) + "|");\n'
                "func even(n) { if (n == 0) { return true; } "
                "return odd(n - 1); }\n"
                "func odd(n) { if (n == 0) { return false; } "
                "return even(n - 1); }\n"
                "thread func parity(ctx) {\n"
                '    return {"v": even(ctx.payload["n"])};\n'
                "}\n"
                "let t3 = std.thread.spawn(parity, "
                'std.thread.Message.from({"n": 10}));\n'
                "let r3 = t3.join();\n"
                'print("mutual=" + str(r3["value"]["v"]) + "|");\n',
                stdout_all=["two=223|", "chain=11|", "mutual=true|"],
            )
            check(
                "std.thread encode roundtrip",
                "import std;\n"
                "func helper(x) { return x * 2; }\n"
                "func make_adder(n) { return func(x) { return x + n; }; }\n"
                "let f = make_adder(5);\n"
                "func make_worker(fn) {\n"
                "    thread func worker(ctx) {\n"
                "        return {\"v\": fn(ctx.payload[\"n\"]) + "
                "helper(ctx.payload[\"n\"])};\n"
                "    }\n"
                "    return worker;\n"
                "}\n"
                "let blob = std.thread.encode(make_worker(f));\n"
                "let t = std.thread.spawn_encoded(blob, "
                'std.thread.Message.from({"n": 3}));\n'
                "let r = t.join();\n"
                'print("encoded=" + str(r["value"]["v"]) + "|");\n',
                stdout_all=["encoded=14|"],
            )
            check(
                "std.thread encode containers and patterns",
                "import std;\n"
                "let s = std.set.from_list([1, 2, 3]);\n"
                "let d = std.deque.from_list([4, 5]);\n"
                "let l = std.lru.new(2);\n"
                'let r0 = std.lru.put(l, "a", 7);\n'
                'l = r0["lru"];\n'
                'r0 = std.lru.put(l, "b", 8);\n'
                'l = r0["lru"];\n'
                "let h = std.heap.from_list([3, 1, 2], "
                "func(a, b) { return a < b; });\n"
                "func make_worker(sv, dv, lv, hv) {\n"
                "    thread func worker(ctx) {\n"
                '        let n = ctx.payload["n"];\n'
                "        let m = match n { case 1 => 10; "
                "default => 20; };\n"
                '        let got = std.lru.get(lv, "a");\n'
                '        let popped = std.heap.pop(hv);\n'
                '        return {"s": std.set.values(sv), '
                '"d": std.deque.to_list(dv), '
                '"l": got["value"], "m": m, '
                '"h": popped["value"], '
                '"hs": std.heap.size(popped["heap"])};\n'
                "    }\n"
                "    return worker;\n"
                "}\n"
                "let blob = std.thread.encode(make_worker(s, d, l, h));\n"
                "let t = std.thread.spawn_encoded(blob, "
                'std.thread.Message.from({"n": 1}));\n'
                "let r = t.join();\n"
                'print("s=" + str(r["value"]["s"]) + "|d=" + '
                'str(r["value"]["d"]) + "|l=" + str(r["value"]["l"]) + '
                '"|m=" + str(r["value"]["m"]) + "|h=" + '
                'str(r["value"]["h"]) + ":" + '
                'str(r["value"]["hs"]) + "|");\n',
                stdout_all=["s=[1, 2, 3]|d=[4, 5]|l=7|m=10|h=1:2|"],
            )
            check(
                "std.thread transfer rejects defaults",
                "import std;\n"
                "func with_default(x, y = 2) { return x + y; }\n"
                "func make_worker() {\n"
                "    thread func worker(ctx) {\n"
                '        return {"v": with_default(ctx.payload["n"])};\n'
                "    }\n"
                "    return worker;\n"
                "}\n"
                "let w = make_worker();\n"
                "std.thread.spawn(w, "
                'std.thread.Message.from({"n": 10}));\n',
                stderr="cannot transfer a function with default parameters",
            )
            check(
                "std.thread spawn plain function rejected",
                "import std;\n"
                "func plain(ctx) {\n"
                '    return {"v": ctx.payload["n"]};\n'
                "}\n"
                "std.thread.spawn(plain, "
                'std.thread.Message.from({"n": 7}));\n',
                stderr="must be declared thread func",
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
                "std.thread duplex basic",
                "import std;\n"
                'let link = std.thread.duplex(2);\n'
                'let child = link.child;\n'
                'link.send(std.thread.Message.from({"v": "p2c"}));\n'
                'print("child="); print(child.recv().payload["v"]); '
                'print("|");\n'
                'child.send(std.thread.Message.from({"v": "c2p"}));\n'
                'print("parent="); print(link.recv().payload["v"]); '
                'print("|");\n'
                'print("closed="); print(link.closed); print("|");\n'
                'link.close_send();\n'
                'print("recv_null="); print(child.recv() == null); '
                'print("|");\n'
                'link.close_recv();\n'
                'print("closed2="); print(link.closed); print("|");\n',
                stdout="child=p2c|parent=c2p|closed=false|"
                       "recv_null=true|closed2=true|",
            )
            check(
                "std.thread select duplex endpoint",
                "import std;\n"
                'let link = std.thread.duplex(1);\n'
                'let child = link.child;\n'
                'child.send(std.thread.Message.from({"v": 7}));\n'
                'let r = std.thread.select([link], 1000);\n'
                'print("index="); print(r["index"]); print(":");\n'
                'print(r["value"].payload["v"]);\n',
                stdout="index=0:7",
            )

            duplex_worker = os.path.join(
                thread_tmp, "thread_duplex_worker.skein")
            with open(duplex_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func main(ctx) {\n"
                    "    let link = ctx.handles[\"link\"];\n"
                    "    link.send(std.thread.Message.from(\n"
                    "        {\"reply\": ctx.payload[\"v\"] + 1}));\n"
                    "    return {\"done\": true};\n"
                    "}\n"
                )
            duplex_worker_path = duplex_worker.replace("\\", "\\\\")
            channel_worker = os.path.join(
                thread_tmp, "thread_channel_worker.skein")
            with open(channel_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func main(ctx) {\n"
                    "    let ch = ctx.handles[\"channel\"];\n"
                    "    ch.send(std.thread.Message.from(\n"
                    "        {\"reply\": ctx.payload[\"v\"] * 2}));\n"
                    "    return {\"done\": true};\n"
                    "}\n"
                )
            channel_worker_path = channel_worker.replace("\\", "\\\\")
            check(
                "std.thread spawn with duplex",
                "import std;\n"
                f'let t = std.thread.spawn_with_duplex('
                f'"{duplex_worker_path}", '
                f'std.thread.Message.from({{"v": 41}}), 1);\n'
                'let reply = t.link.recv(1000);\n'
                'print("reply="); print(reply.payload["reply"]); '
                'print("|");\n'
                'let r = t.join();\n'
                'print("ok="); print(r["ok"]); print("|");\n',
                stdout="reply=42|ok=true|",
            )
            check(
                "std.thread spawn with channel",
                "import std;\n"
                f'let t = std.thread.spawn_with_channel('
                f'"{channel_worker_path}", '
                f'std.thread.Message.from({{"v": 21}}), 1);\n'
                'let reply = t.channel.recv(1000);\n'
                'print("reply="); print(reply.payload["reply"]); '
                'print("|");\n'
                'let r = t.join();\n'
                'print("ok="); print(r["ok"]); print("|");\n',
                stdout="reply=42|ok=true|",
            )
            check(
                "std.thread message cycle collection",
                "import std;\n"
                'let base = std.thread.debug_counts();\n'
                'let ch = std.thread.Channel(1);\n'
                'let msg = std.thread.Message.from({"x": 1}, '
                '{"ch": ch});\n'
                'ch.send(msg);\n'
                'ch = null;\n'
                'msg = null;\n'
                'std.thread.collect();\n'
                'let after = std.thread.debug_counts();\n'
                'print("m="); '
                'print(after["messages"] - base["messages"]); '
                'print("|");\n'
                'print("c="); '
                'print(after["channels"] - base["channels"]); '
                'print("|");\n'
                'print("g="); '
                'print(after["collected"] > base["collected"]); '
                'print("|");\n'
                'let base2 = std.thread.debug_counts();\n'
                'let link = std.thread.duplex(1);\n'
                'let dmsg = std.thread.Message.from({"x": 2}, '
                '{"link": link});\n'
                'link.send(dmsg);\n'
                'link = null;\n'
                'dmsg = null;\n'
                'std.thread.collect();\n'
                'let after2 = std.thread.debug_counts();\n'
                'print("dm="); '
                'print(after2["messages"] - base2["messages"]); '
                'print("|");\n'
                'print("dc="); '
                'print(after2["channels"] - base2["channels"]); '
                'print("|");\n'
                'print("dd="); '
                'print(after2["duplexes"] - base2["duplexes"]); '
                'print("|");\n'
                'print("de="); '
                'print(after2["endpoints"] - base2["endpoints"]); '
                'print("|");\n',
                stdout="m=0|c=0|g=true|dm=0|dc=0|dd=0|de=0|",
            )
            auto_cycle_source = (
                "import std;\n"
                'let base = std.thread.debug_counts();\n'
                'let ch = std.thread.Channel(1);\n'
                'let msg = std.thread.Message.from({"x": 1}, '
                '{"ch": ch});\n'
                'ch.send(msg);\n'
                'ch = null;\n'
                'msg = null;\n'
            )
            for index in range(80):
                auto_cycle_source += f"let z{index} = {index};\n"
            auto_cycle_source += (
                'let after = std.thread.debug_counts();\n'
                'print("m="); '
                'print(after["messages"] - base["messages"]); '
                'print("|");\n'
                'print("c="); '
                'print(after["channels"] - base["channels"]); '
                'print("|");\n'
                'print("g="); '
                'print(after["collected"] > base["collected"]); '
                'print("|");\n'
            )
            check(
                "std.thread automatic cycle collection",
                auto_cycle_source,
                stdout="m=0|c=0|g=true|",
            )

            cow_worker = os.path.join(
                thread_tmp, "thread_cow_worker.skein")
            with open(cow_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func main(ctx) {\n"
                    "    let items = ctx.payload[\"items\"];\n"
                    "    items[0] = 99;\n"
                    "    let body = ctx.payload[\"body\"];\n"
                    "    body[0] = 90;\n"
                    "    let copy = ctx.payload;\n"
                    "    copy[\"x\"] = 42;\n"
                    "    return {\"items\": items[0], "
                    "\"body\": bytes_to_string(body), "
                    "\"x\": copy[\"x\"]};\n"
                    "}\n"
                )
            cow_worker_path = cow_worker.replace("\\", "\\\\")
            check(
                "std.thread message cross-thread copy on write",
                "import std;\n"
                'let body = bytes_from_string("ab");\n'
                'let items = [1, 2];\n'
                'let msg = std.thread.Message.from('
                '{"x": 1, "items": items, "body": body});\n'
                f'let t = std.thread.spawn("{cow_worker_path}", msg);\n'
                'let r = t.join();\n'
                'print("child="); print(r["value"]["items"]); print(":"); '
                'print(r["value"]["body"]); print(":"); '
                'print(r["value"]["x"]);\n'
                'let view = msg.payload;\n'
                'print("parent="); print(view["items"][0]); print(":"); '
                'print(bytes_to_string(view["body"])); print(":"); '
                'print(view["x"]);\n',
                stdout_all=["child=99:Zb:42", "parent=1:ab:1"],
            )
            cow_result_worker = os.path.join(
                thread_tmp, "thread_cow_result_worker.skein")
            with open(cow_result_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func main(ctx) {\n"
                    "    let shared = {\"items\": [1, 2], "
                    "\"inner\": {\"v\": 7}};\n"
                    "    ctx.handles[\"signal\"].send("
                    "std.thread.Message.from({\"shared\": shared}));\n"
                    "    return {\"shared\": shared};\n"
                    "}\n"
                )
            cow_result_worker_path = cow_result_worker.replace("\\", "\\\\")
            check(
                "std.thread result copy on write",
                "import std;\n"
                'let ch = std.thread.Channel(1);\n'
                'let msg = std.thread.Message.from({"x": 0}, '
                '{"signal": ch});\n'
                f'let t = std.thread.spawn("{cow_result_worker_path}", '
                f'msg);\n'
                'let signal = ch.recv();\n'
                'let r = t.join();\n'
                'let out = r["value"]["shared"];\n'
                'out["items"][0] = 99;\n'
                'out["inner"]["v"] = 42;\n'
                'print("result="); print(out["items"][0]); print(":"); '
                'print(out["inner"]["v"]);\n'
                'print("signal="); '
                'print(signal.payload["shared"]["items"][0]); print(":"); '
                'print(signal.payload["shared"]["inner"]["v"]);\n',
                stdout_all=["result=99:42", "signal=1:7"],
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

            delay_worker = os.path.join(
                thread_tmp, "thread_delay_worker.skein")
            with open(delay_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func main(ctx) {\n"
                    "    std.time.sleep(50);\n"
                    "    ctx.handles[\"signal\"].send("
                    "std.thread.Message.from({\"x\": 9}));\n"
                    "    return {\"ok\": true};\n"
                    "}\n"
                )
            delay_worker_path = delay_worker.replace("\\", "\\\\")
            check(
                "std.thread fiber channel wait",
                "import std;\n"
                "async func wait_channel(ch) {\n"
                "    let msg = ch.recv();\n"
                "    return msg.payload[\"x\"];\n"
                "}\n"
                'let ch = std.thread.Channel(1);\n'
                f'let t = std.thread.spawn("{delay_worker_path}", '
                f'std.thread.Message.from({{}}, {{"signal": ch}}));\n'
                'let task = wait_channel(ch);\n'
                'print("before|");\n'
                'std.io.flush(std.io.stdout);\n'
                'await std.task.sleep(10);\n'
                'print("after|");\n'
                'std.io.flush(std.io.stdout);\n'
                'print("value="); print(await task);\n'
                't.join();\n',
                stdout="before|after|value=9",
            )
            check(
                "std.thread fiber select wait",
                "import std;\n"
                "async func watch(empty, signal) {\n"
                "    return std.thread.select([empty, signal], 2000);\n"
                "}\n"
                "async func wait_timeout(ch) {\n"
                "    return std.thread.select([ch], 10);\n"
                "}\n"
                'let signal = std.thread.Channel(1);\n'
                'let empty = std.thread.Channel(1);\n'
                'let task = watch(empty, signal);\n'
                'await std.task.sleep(10);\n'
                'signal.send(std.thread.Message.from({"x": 11}));\n'
                'let r = await task;\n'
                'print("index="); print(r["index"]); print(":");\n'
                'print(r["value"].payload["x"]);\n'
                'let timeout = await wait_timeout(empty);\n'
                'print("timeout="); print(timeout["timeout"]);\n',
                stdout_all=["index=1:11", "timeout=true"],
            )

            slow_worker = os.path.join(thread_tmp, "thread_pool_slow.skein")
            with open(slow_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(payload) {\n"
                    "    std.time.sleep(200);\n"
                    "    return {\"n\": payload[\"n\"]};\n"
                    "}\n"
                )
            slow_worker_path = slow_worker.replace("\\", "\\\\")
            check(
                "std.thread pool stats and future cancel",
                "import std;\n"
                f'let pool = std.thread.pool(1, "{slow_worker_path}", '
                '{"entry": "handle", "queue": 1});\n'
                'let f = pool.submit({"n": 1});\n'
                'print("cancel="); print(f.cancel());\n'
                'print("cancel2="); print(f.cancel());\n'
                'let r = f.try_wait();\n'
                'print("canceled="); print(r["canceled"]);\n'
                'let s = pool.stats();\n'
                'print("workers="); print(s["workers"]); print(":"); '
                'print("alive="); print(s["alive"]);\n'
                'pool.close();\n'
                'pool.join();\n',
                stdout_all=["cancel=true", "cancel2=false", "canceled=true",
                            "workers=1:alive=1"],
            )
            cancel_worker = os.path.join(
                thread_tmp, "thread_pool_cancel_worker.skein")
            with open(cancel_worker, "w", encoding="utf-8") as handle:
                handle.write(
                    "import std;\n"
                    "func handle(payload) {\n"
                    "    std.time.sleep(300);\n"
                    "    std.fs.write_text(payload[\"path\"], "
                    "str(payload[\"n\"]), \"utf-8\");\n"
                    "    return {\"n\": payload[\"n\"]};\n"
                    "}\n"
                )
            cancel_worker_path = cancel_worker.replace("\\", "\\\\")
            cancel_paths = [
                os.path.join(thread_tmp, f"thread_cancel_{index}.txt")
                for index in range(1, 4)
            ]
            check(
                "std.thread future cancel skips queued job",
                "import std;\n"
                f'let pool = std.thread.pool(1, '
                f'"{cancel_worker_path}", '
                '{"entry": "handle", "queue": 8});\n'
                f'let f1 = pool.submit({{"n": 1, "path": '
                f"{skein_string(cancel_paths[0])}}});\n"
                f'let f2 = pool.submit({{"n": 2, "path": '
                f"{skein_string(cancel_paths[1])}}});\n"
                'print("cancel=" + str(f2.cancel()) + "|");\n'
                'let r1 = f1.wait();\n'
                'print("r1=" + str(r1["value"]["n"]) + "|");\n'
                f'let f3 = pool.submit({{"n": 3, "path": '
                f"{skein_string(cancel_paths[2])}}});\n"
                'let r3 = f3.wait();\n'
                'print("r3=" + str(r3["value"]["n"]) + "|");\n'
                f'print("p1=" + str(std.fs.exists('
                f"{skein_string(cancel_paths[0])})) + \"|\");\n"
                f'print("p2=" + str(std.fs.exists('
                f"{skein_string(cancel_paths[1])})) + \"|\");\n"
                f'print("p3=" + str(std.fs.exists('
                f"{skein_string(cancel_paths[2])})) + \"|\");\n"
                'pool.close();\n'
                'pool.join();\n',
                stdout_all=["cancel=true|", "r1=1|", "r3=3|",
                            "p1=1|", "p2=0|", "p3=1|"],
            )
            check(
                "std.thread pool submit timeout",
                "import std;\n"
                f'let pool = std.thread.pool(1, "{slow_worker_path}", '
                '{"entry": "handle", "queue": 1});\n'
                'let f1 = pool.submit({"n": 1});\n'
                'let f2 = pool.submit({"n": 2});\n'
                'let f3 = pool.submit({"n": 3}, 10);\n',
                stderr="thread.timeout",
            )
            check(
                "std.thread pool worker vm",
                "import std;\n"
                f'let pool = std.thread.pool(2, "{slow_worker_path}", '
                '{"entry": "handle", "worker_vm": true});\n'
                'let f = pool.submit({"n": 21});\n'
                'let r = f.wait();\n'
                'print("ok="); print(r["ok"]); print(":"); '
                'print(r["value"]["n"]);\n'
                'let s = pool.stats();\n'
                'print("alive="); print(s["alive"]);\n'
                'pool.close();\n'
                'pool.join();\n',
                stdout_all=["ok=true:21", "alive=2"],
            )
            check(
                "std.thread fiber future wake latency",
                "import std;\n"
                "async func run(pool) {\n"
                "    let t0 = std.time.monotonic_ms();\n"
                "    for (let i = 0; i < 100; i = i + 1) {\n"
                '        let r = pool.submit({"n": i}).wait();\n'
                "    }\n"
                "    return std.time.monotonic_ms() - t0;\n"
                "}\n"
                f'let pool = std.thread.pool(1, "{thread_worker_path}");\n'
                "let elapsed = await run(pool);\n"
                'print("elapsed="); print(elapsed); print("|");\n'
                'print("under="); print(elapsed < 900); print("|");\n'
                "pool.close();\n"
                "pool.join();\n",
                stdout="under=true|",
            )

            if FFI_SHIM:
                manifest = os.path.join(thread_tmp, "shim_throw.ffi.json")
                with open(manifest, "w", encoding="utf-8") as handle:
                    handle.write(
                        '{"ffi": 1, "types": {}, "functions": '
                        '{"shim_throw": {"result": "void", "args": []}}}\n'
                    )
                crash_worker = os.path.join(
                    thread_tmp, "thread_pool_crash.skein")
                with open(crash_worker, "w", encoding="utf-8") as handle:
                    handle.write(
                        "import std;\n"
                        f"let lib = std.ffi.open({skein_string(os.path.abspath(FFI_SHIM))}, "
                        f"{skein_string(manifest)});\n"
                        "func main(payload) {\n"
                        "    if (payload[\"crash\"]) {\n"
                        "        std.ffi.call(lib, \"shim_throw\", []);\n"
                        "    }\n"
                        "    return {\"n\": payload[\"n\"] * 2};\n"
                        "}\n"
                    )
                crash_worker_path = crash_worker.replace("\\", "\\\\")
                check(
                    "std.thread pool worker crash restart",
                    "import std;\n"
                    f'let pool = std.thread.pool(1, "{crash_worker_path}", '
                    '{"entry": "main", "restart_limit": 1});\n'
                    'let r1 = pool.submit({"n": 3, "crash": false}).wait();\n'
                    'let r2 = pool.submit({"n": 4, "crash": true}).wait();\n'
                    'let r3 = pool.submit({"n": 5, "crash": false}).wait();\n'
                    'print("r1="); print(r1["ok"]); print(":"); '
                    'print(r1["value"]["n"]);\n'
                    'print("r2="); print(r2["ok"]); print(":"); '
                    'print(std.error.kind(r2["error"]));\n'
                    'print("r3="); print(r3["ok"]); print(":"); '
                    'print(r3["value"]["n"]);\n'
                    'let s = pool.stats();\n'
                    'print("alive="); print(s["alive"]); print(":"); '
                    'print("dead="); print(s["dead"]); print(":"); '
                    'print("restarts="); print(s["restarts"]); print(":"); '
                    'print("ready="); print(s["ready"]);\n'
                    'pool.close();\n'
                    'pool.join();\n',
                    stdout_all=["r1=true:6", "r2=false:thread.worker_crashed",
                                "r3=true:10",
                                "alive=1:dead=0:restarts=1:ready=1"],
                )
                check(
                    "std.thread pool worker crash no restart",
                    "import std;\n"
                    f'let pool = std.thread.pool(1, "{crash_worker_path}", '
                    '{"entry": "main", "restart_limit": 0});\n'
                    'let r = pool.submit({"n": 4, "crash": true}).wait();\n'
                    'print("r="); print(r["ok"]); print(":"); '
                    'print(std.error.kind(r["error"]));\n'
                    'let f = pool.submit({"n": 5, "crash": false});\n',
                    stdout="r=false:thread.worker_crashed",
                    stderr="thread.pool_failed",
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
