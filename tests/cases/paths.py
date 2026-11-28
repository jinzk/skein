#!/usr/bin/env python3
"""Cross-platform lexical paths provided by std.path.

The cases live in tests/skein/paths.skein; this module keeps the existing
ctest module name and shard wiring.  One Python golden stays here as an
independent check of list/string formatting.  Windows additionally exercises
non-ASCII paths through the CLI, FFI generation, SQLite, and std.log.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

from harness import BIN, check, run, run_file, run_skein, skein_string


def _windows_utf8_cases():
    with tempfile.TemporaryDirectory(prefix="skein_路径_") as directory:
        script = os.path.join(directory, "脚本.skein")
        with open(script, "w", encoding="utf-8") as handle:
            handle.write('std.io.print("中文脚本");\n')
        proc = run_file(script)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.strip() == "中文脚本", proc.stdout

        header = os.path.join(directory, "头文件.h")
        with open(header, "w", encoding="utf-8") as handle:
            handle.write("int skein_utf8_probe(void);\n")
        manifest_path = os.path.join(directory, "清单.json")
        if shutil.which("cc"):
            proc = subprocess.run(
                [BIN, "--ffi-gen", "--header", header, "-o", manifest_path],
                capture_output=True, timeout=60)
            assert proc.returncode == 0, proc.stderr.decode(
                "utf-8", errors="replace")
            with open(manifest_path, encoding="utf-8") as handle:
                manifest = json.load(handle)
            assert manifest["library"] == "头文件", manifest

        probe = run('std.db.open(":memory:");')
        if probe.returncode == 0:
            db_path = os.path.join(directory, "数据库.db")
            proc = run(
                'let db = std.db.open(%s);\n'
                'std.db.exec(db, "CREATE TABLE t (v TEXT)");\n'
                'std.db.exec(db, "INSERT INTO t VALUES (?)", ["中文"]);\n'
                'std.io.print(std.db.query(db, "SELECT v FROM t")[0]["v"]);\n'
                'std.db.close(db);\n' % skein_string(db_path))
            assert proc.returncode == 0, proc.stderr
            assert "中文" in proc.stdout, proc.stdout

        log_path = os.path.join(directory, "日志.txt")
        proc = run(
            'let lg = std.log.new({"output": "file", "path": %s, '
            '"name": "utf8", "level": "info", "flush": true});\n'
            'lg.info("中文");\n' % skein_string(log_path))
        assert proc.returncode == 0, proc.stderr
        with open(log_path, encoding="utf-8") as handle:
            assert "中文" in handle.read()


def run_cases():
    run_skein("paths.skein")
    check(
        "path golden output",
        "import std.path;\n"
        'let p = path.parse("/a/b/../c.txt", "posix");\n'
        'std.io.print(p["style"]);\n'
        'std.io.print(p["parts"]);\n'
        'std.io.print(p["normalized"]);\n',
        stdout_all=["posix", "[a, b, .., c.txt]", "/a/c.txt"],
    )
    if sys.platform == "win32":
        _windows_utf8_cases()
