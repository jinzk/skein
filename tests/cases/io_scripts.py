#!/usr/bin/env python3
"""Files, scripts, imports, formatting and REPL behavior.

Extracted from the original test_skein.py main() body, lines 8922-10145.
"""
from harness import *  # noqa: F401,F403


def run_cases():
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

        text_path = os.path.join(tmp, "text-encoding.txt")
        quoted_text = text_path.replace("\\", "\\\\").replace('"', '\\"')
        check(
            "std.fs read_text/write_text gbk",
            "import std;\n"
            f'std.fs.write_text("{quoted_text}", "中国", "gbk");\n'
            f'print(bytes_hex(std.fs.read_bytes("{quoted_text}")));\n'
            f'print(std.fs.read_text("{quoted_text}", "gbk"));\n',
            stdout_all=["d6d0b9fa", "中国"],
        )
        check(
            "std.fs read_text/write_text utf16",
            "import std;\n"
            f'std.fs.write_text("{quoted_text}", "中", "utf-16");\n'
            f'print(bytes_hex(std.fs.read_bytes("{quoted_text}")));\n'
            f'print(std.fs.read_text("{quoted_text}", "utf-16"));\n',
            stdout_all=["fffe2d4e", "中"],
        )
        check(
            "std.fs write_text errors replace",
            "import std;\n"
            f'std.fs.write_text("{quoted_text}", "é", "ascii", "replace");\n'
            f'bytes_hex(std.fs.read_bytes("{quoted_text}"))\n',
            stdout="= 3f",
        )
        invalid_text = os.path.join(tmp, "invalid-text.bin")
        with open(invalid_text, "wb") as f:
            f.write(b"\xff")
        quoted_invalid_text = invalid_text.replace("\\", "\\\\")
        check(
            "std.fs read_text strict",
            f'import std;\nstd.fs.read_text("{quoted_invalid_text}", "gbk")\n',
            stderr="invalid gbk",
        )
        check(
            "std.fs read_text replace",
            f'import std;\nstd.fs.read_text("{quoted_invalid_text}", '
            '"gbk", "replace")\n',
            stdout="= �",
        )
        check(
            "std.fs read_text ignore",
            f'import std;\nlen(std.fs.read_text("{quoted_invalid_text}", '
            '"gbk", "ignore"))\n',
            stdout="= 0",
        )
        check(
            "std.fs read_text type",
            'import std;\nstd.fs.read_text(1, "gbk")\n',
            stderr="expects a string path and encoding",
        )
        check(
            "std.fs write_text type",
            'import std;\nstd.fs.write_text("x", 1, "gbk")\n',
            stderr="expects a string path, text and encoding",
        )
        check(
            "std.fs read_text arity",
            'import std;\nstd.fs.read_text("x")\n',
            stderr="expects 2 or 3 arguments",
        )
        check(
            "std.fs write_text arity",
            'import std;\nstd.fs.write_text("x", "y")\n',
            stderr="expects 3 or 4 arguments",
        )
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
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write("app/aa/aa.skein",
              "package aa { export struct Point { x: int; y: int; } }\n")
        main = write("app/main.skein",
              "import std;\nimport aa;\n"
              "let p = std.struct.decode({\"x\": 1, \"y\": 2}, "
              "\"aa::Point\");\n"
              "print(p);\nprint(kind_of(p));\n"
              "let q = std.struct.decode({\"x\": 3, \"y\": 4}, "
              "\"Point\");\n"
              "print(q.x);\n")
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == [
            "Point { x: 1, y: 2 }",
            "struct",
            "3",
        ], proc.stdout
        assert proc.stderr == "", proc.stderr

    with tempfile.TemporaryDirectory() as tmp:
        script = os.path.join(tmp, "script.skein")
        with open(script, "w", encoding="utf-8") as f:
            f.write(
                "#!/usr/bin/env skein\n"
                'print("script");\n'
                "1 + 2\n"
            )
        proc = run_file(script)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "script\n", repr(proc.stdout)
        assert proc.stderr == "", proc.stderr

        bom_script = os.path.join(tmp, "bom.skein")
        with open(bom_script, "w", encoding="utf-8") as f:
            f.write("\ufeff#!/usr/bin/env skein\nprint(\"bom\");\n")
        proc = run_file(bom_script)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "bom\n", repr(proc.stdout)

        input_script = os.path.join(tmp, "input.skein")
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
                shebang_script = os.path.join(tmp, "shebang.skein")
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
        runtime_script = os.path.join(tmp, "runtime.skein")
        with open(runtime_script, "w", encoding="utf-8") as f:
            f.write('print("a");\n1/0;\n')
        proc = run_file(runtime_script)
        assert proc.returncode == 1, proc.stderr
        assert "2:1: runtime error: division by zero" in proc.stderr, proc.stderr
        assert "  1/0;\n  ^" in proc.stderr, proc.stderr

        nested_script = os.path.join(tmp, "nested.skein")
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

        clean_script = os.path.join(tmp, "clean.skein")
        with open(clean_script, "w", encoding="utf-8") as f:
            f.write("print(\"ok\");\n")
        proc = run_file(clean_script)
        assert proc.returncode == 0, proc.stderr

        syntax_script = os.path.join(tmp, "syntax.skein")
        with open(syntax_script, "w", encoding="utf-8") as f:
            f.write("let x = ;\n")
        proc = run_file(syntax_script)
        assert proc.returncode == 1, proc.stderr
        assert "1:9: syntax error: unexpected ';'" in proc.stderr, proc.stderr

        invalid_utf8_script = os.path.join(tmp, "invalid_utf8.skein")
        with open(invalid_utf8_script, "wb") as f:
            f.write(b"let a\x80b = 1;\n")
        proc = subprocess.run(
            [BIN, invalid_utf8_script],
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 1, proc.stderr
        assert b"invalid UTF-8 byte 0x80" in proc.stderr, proc.stderr

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
            "app/aa/aa.skein",
            'package aa { export let root = "aa"; }\n',
        )
        write(
            "app/aa/bb/bb.skein",
            'package bb { export let name = "bb"; }\n',
        )
        write(
            "app/aa/bb/cc/cc.skein",
            "package cc {\n"
            "  export let value = 42;\n"
            "  export const fixed = 7;\n"
            "  export func hello(name) { return \"hi \" + name; }\n"
            "  func hidden() { return 99; }\n"
            "}\n",
        )
        app_main = write(
            "app/main.skein",
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
            "app/aa/state/state.skein",
            "package state {\n"
            "  let n = 0;\n"
            "  export func inc() { n = n + 1; return n; }\n"
            "  export func get() { return n; }\n"
            "}\n",
        )
        state_main = write(
            "app/state_main.skein",
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
            "container/aa/bb/cc/cc.skein",
            'package cc { export let value = "deep"; }\n',
        )
        container_main = write(
            "container/main.skein",
            "import aa;\nprint(aa.bb.cc.value);\n",
        )
        proc = run_file(container_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "deep\n", repr(proc.stdout)

        # Package lookup paths: source dir > CLI dirs > environment dirs.
        write(
            "search/app/aa/bb/cc/cc.skein",
            'package cc { export let value = "source"; }\n',
        )
        search_main = write(
            "search/app/main.skein",
            "import aa.bb.cc;\nprint(cc.value);\n",
        )
        write(
            "search/lib1/aa/bb/cc/cc.skein",
            'package cc { export let value = "cli1"; }\n',
        )
        write(
            "search/lib2/aa/bb/cc/cc.skein",
            'package cc { export let value = "cli2"; }\n',
        )
        write(
            "search/env/aa/bb/cc/cc.skein",
            'package cc { export let value = "env"; }\n',
        )
        proc = run_file(search_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "source\n", repr(proc.stdout)

        app_without_local = write(
            "search/app2/main.skein",
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
        env["SKEIN_PACKAGE_PATH"] = envlib
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
            "isolated/aa/bb/cc/cc.skein",
            "package cc { export let leaked = main_secret; }\n",
        )
        isolated_main = write(
            "isolated/main.skein",
            "let main_secret = 1;\nimport aa.bb.cc;\n",
        )
        proc = run_file(isolated_main)
        assert proc.returncode == 1, proc.stderr
        assert "symbol not found main_secret" in proc.stderr, proc.stderr

        # Package names must match their containing folder.
        write(
            "wrongname/aa/bb/cc/cc.skein",
            "package wrong { export let value = 1; }\n",
        )
        wrong_main = write(
            "wrongname/main.skein",
            "import aa.bb.cc;\n",
        )
        proc = run_file(wrong_main)
        assert proc.returncode == 1, proc.stderr
        assert "must match folder" in proc.stderr, proc.stderr

        # Explicit main package and exported/private names.
        main_pkg = write(
            "mainpkg/main/main.skein",
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
            "mainpkg/anywhere/entry.skein",
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
            "entrypkg/entry/entry.skein",
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
        reserved_import = write("reservedimport/main.skein", "import main;\n")
        proc = run_file(reserved_import)
        assert proc.returncode == 1, proc.stderr
        assert "reserved for the entry script" in proc.stderr, proc.stderr

        reserved_import_sub = write(
            "reservedimport/sub.skein", "import main.sub;\n")
        proc = run_file(reserved_import_sub)
        assert proc.returncode == 1, proc.stderr
        assert "reserved for the entry script" in proc.stderr, proc.stderr

        # A normal package cannot declare itself as main.
        write(
            "reservedmain/aa/bb/bb.skein",
            "package main { export let x = 1; }\n",
        )
        reserved_main = write(
            "reservedmain/main.skein",
            "import aa.bb;\n",
        )
        proc = run_file(reserved_main)
        assert proc.returncode == 1, proc.stderr
        assert "reserved for the entry script" in proc.stderr, proc.stderr

        # Cyclic imports fail cleanly instead of hanging.
        write(
            "cycle/aa/bb/bb.skein",
            "package bb { import aa.cc; export let x = 1; }\n",
        )
        write(
            "cycle/aa/cc/cc.skein",
            "package cc { import aa.bb; export let y = 2; }\n",
        )
        cycle_main = write(
            "cycle/main.skein",
            "import aa.bb;\n",
        )
        proc = run_file(cycle_main)
        assert proc.returncode == 1, proc.stderr
        assert "cyclic package import" in proc.stderr, proc.stderr

        missing_main = write(
            "missing/main.skein",
            "import no.such;\n",
        )
        proc = run_file(missing_main)
        assert proc.returncode == 1, proc.stderr
        assert "package not found" in proc.stderr, proc.stderr

        # Unicode package names and directories.
        write(
            "unicode/包/包.skein",
            "package 包 {\n"
            "  export let 值 = 42;\n"
            "}\n",
        )
        write(
            "unicode/包/子包/子包.skein",
            "package 子包 {\n"
            "  export let 名称 = \"子\";\n"
            "}\n",
        )
        unicode_main = write(
            "unicode/main.skein",
            "import 包;\n"
            "import 包.子包;\n"
            "print(包.值);\n"
            "print(包.子包.名称);\n",
        )
        proc = run_file(unicode_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["42", "子"], proc.stdout

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

        with open(os.path.join(root, ".skeinroot"), "w", encoding="utf-8"):
            pass

        write("top/pkg/pkg.skein",
              'package pkg { export let value = "root"; }\n')
        write("lib/top/pkg/pkg.skein",
              'package pkg { export let value = "global"; }\n')
        write("src/common/common.skein",
              'package common { export let value = "common"; }\n')
        write("src/app/local/local.skein",
              'package local { export let value = "local"; }\n')
        write("src/app/sibling/sibling.skein",
              'package sibling { export let value = "sibling"; }\n')
        write("src/app/user/helper/helper.skein",
              'package helper { export let value = "helper"; }\n')
        write(
            "src/app/user/user.skein",
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
            "src/app/main.skein",
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

        # Without a .skeinroot marker, @ falls back to the main script dir.
        nomarker = os.path.join(tmp, "nomarker")
        os.makedirs(os.path.join(nomarker, "local"), exist_ok=True)
        with open(os.path.join(nomarker, "local", "local.skein"),
                  "w", encoding="utf-8") as f:
            f.write('package local { export let value = "local"; }\n')
        fallback_main = os.path.join(nomarker, "main.skein")
        with open(fallback_main, "w", encoding="utf-8") as f:
            f.write("import @local;\nprint(local.value);\n")
        proc = run_file(fallback_main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "local\n", repr(proc.stdout)

        # @ never falls back to -I directories.
        os.makedirs(os.path.join(nomarker, "lib", "top", "pkg"),
                    exist_ok=True)
        with open(os.path.join(nomarker, "lib", "top", "pkg", "pkg.skein"),
                  "w", encoding="utf-8") as f:
            f.write('package pkg { export let value = "global"; }\n')
        nofallback_main = os.path.join(nomarker, "nofallback.skein")
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
            "app/aa/aa.skein",
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
            "app/main.skein",
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
            "app/main2.skein",
            "import aa;\n"
            "let p = aa.make();\n"
            "let q: P = p;\n",
        )
        proc = run_file(main2)
        assert proc.returncode == 1, proc.stdout
        assert "unknown type name P" in proc.stderr, proc.stderr

        main3 = write(
            "app/main3.skein",
            "import aa;\n"
            'let h: aa::Hidden = aa.hidden();\n',
        )
        proc = run_file(main3)
        assert proc.returncode == 1, proc.stdout
        assert "unknown type name aa::Hidden" in proc.stderr, proc.stderr

    with tempfile.TemporaryDirectory() as tmp:
        failfast = os.path.join(tmp, "failfast.skein")
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

        uncaught = os.path.join(tmp, "uncaught.skein")
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

        lexical_script = os.path.join(tmp, "lexical.skein")
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
            [BIN, os.path.join(tmp, "missing.skein")],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
        )
        assert proc.returncode == 2, proc.stderr

        if sys.platform != "win32":
            interrupt_script = os.path.join(tmp, "interrupt.skein")
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

    check_encoding_two_byte_tables()
    check_unicode_normalization_samples()
    check_extended_stdlib()
    check_source_encoding()
    check_interactive_continuation()

    print(f"all tests passed for {BIN}")
