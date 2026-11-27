#!/usr/bin/env python3
"""Tests for std.log."""

import json
import os
import re
import tempfile

from harness import *  # noqa: F401,F403


def run_cases():
    proc = run(
        "import std;\n"
        'let lg = std.log.new({"name": "app", "level": "info", '
        '"time": "utc"});\n'
        'lg.debug("hidden");\n'
        'lg.info("hello", {"count": 3, "ok": true});\n'
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout == "", proc.stdout
    lines = proc.stderr.strip().splitlines()
    assert len(lines) == 1, proc.stderr
    assert re.match(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z ", lines[0]), \
        lines[0]
    assert " INFO  app: hello count=3 ok=true" in lines[0], lines[0]
    assert "hidden" not in proc.stderr, proc.stderr

    proc = run(
        "import std;\n"
        'let lg = std.log.new({"name": "app", "fields": {"service": "demo"}});\n'
        'lg.with({"request_id": "r1"}).warn("scoped");\n'
        'lg.info("plain");\n'
    )
    lines = proc.stderr.strip().splitlines()
    assert len(lines) == 2, proc.stderr
    assert "WARN  app: scoped service=demo request_id=r1" in lines[0], lines[0]
    assert "INFO  app: plain service=demo" in lines[1], lines[1]
    assert "request_id" not in lines[1], lines[1]

    proc = run(
        "import std;\n"
        'let lg = std.log.new({"format": "json", "name": "j", "time": "utc"});\n'
        'lg.info("msg", {"s": "x\\"y", "n": null, "l": [1, 2], '
        '"b": true, "f": 1.5});\n'
    )
    payload = json.loads(proc.stderr.strip())
    assert payload["level"] == "info", payload
    assert payload["logger"] == "j", payload
    assert payload["message"] == "msg", payload
    assert payload["time"].endswith("Z"), payload
    assert payload["fields"] == {
        "s": 'x"y', "n": None, "l": [1, 2], "b": True, "f": 1.5,
    }, payload

    proc = run(
        "import std;\n"
        'let lg = std.log.new({"writer": func(record) { std.io.print(record); }, '
        '"name": "w"});\n'
        'lg.info("captured", {"x": 1});\n'
        'lg.debug("hidden");\n'
    )
    assert proc.returncode == 0, proc.stderr
    assert "INFO  w: captured x=1" in proc.stdout, proc.stdout
    assert "hidden" not in proc.stdout, proc.stdout

    proc = run(
        "import std;\n"
        'let lg = std.log.new({});\n'
        'lg.log("warn", "warned");\n'
        'lg.log("trace", "filtered");\n'
        'lg.set_level("trace");\n'
        'lg.log("trace", "visible");\n'
    )
    assert "WARN" in proc.stderr and "warned" in proc.stderr, proc.stderr
    assert "visible" in proc.stderr, proc.stderr
    assert "filtered" not in proc.stderr, proc.stderr

    proc = run(
        "import std;\n"
        'std.log.info("default line");\n'
        'std.log.set_level("debug");\n'
        'std.log.debug("debug line");\n'
        "std.log.flush();\n"
    )
    assert "INFO  default line" in proc.stderr, proc.stderr
    assert "DEBUG debug line" in proc.stderr, proc.stderr

    proc = run(
        "import std;\n"
        'let lg = std.log.new({"name": "app"});\n'
        "std.io.println(std.type.kind_of(lg));\n"
        "std.io.println(str(lg));\n"
    )
    assert "userdata" in proc.stdout, proc.stdout
    assert "logger(app)" in proc.stdout, proc.stdout

    with tempfile.TemporaryDirectory() as temporary:
        path = os.path.join(temporary, "app.log")
        proc = run(
            "import std;\n"
            f'let lg = std.log.new({{"output": "file", '
            f'"path": {skein_string(path)}, "format": "json", '
            '"name": "file"});\n'
            'lg.info("one", {"x": 1});\n'
            'lg.warn("two");\n'
            "lg.flush();\n"
        )
        assert proc.returncode == 0, proc.stderr
        with open(path, encoding="utf-8") as handle:
            lines = [json.loads(line) for line in handle if line.strip()]
        assert [item["message"] for item in lines] == ["one", "two"], lines
        assert lines[0]["fields"]["x"] == 1, lines

        proc = run(
            "import std;\n"
            f'let lg = std.log.new({{"output": "file", '
            f'"path": {skein_string(path)}, "format": "json", '
            '"append": false});\n'
            'lg.info("truncated");\n'
            "lg.flush();\n"
        )
        assert proc.returncode == 0, proc.stderr
        with open(path, encoding="utf-8") as handle:
            lines = [json.loads(line) for line in handle if line.strip()]
        assert len(lines) == 1, lines
        assert lines[0]["message"] == "truncated", lines

    check(
        "std log unknown option",
        'import std;\nstd.log.new({"levle": "debug"});\n',
        stderr='unknown option "levle"',
    )
    check(
        "std log unknown level",
        'import std;\nstd.log.new({"level": "nope"});\n',
        stderr='unknown level "nope"',
    )
    check(
        "std log file needs path",
        'import std;\nstd.log.new({"output": "file"});\n',
        stderr='requires a path',
    )
    check(
        "std log writer conflict",
        'import std;\n'
        'std.log.new({"writer": func(r) { }, "output": "stderr"});\n',
        stderr="cannot be combined",
    )
    check(
        "std log message type",
        "import std;\nstd.log.new({}).info(1);\n",
        stderr="message must be a string",
    )

    proc = run(
        "import std;\n"
        'std.log.set_level("warn");\n'
        'std.io.println(str(std.log.enabled("debug")));\n'
        'std.io.println(str(std.log.enabled("error")));\n'
        'let lg = std.log.new({"level": "trace"});\n'
        'std.io.println(str(lg.enabled("trace")));\n'
        'std.io.println(str(lg.enabled("info")));\n'
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.splitlines() == [
        "false",
        "true",
        "true",
        "true",
    ], proc.stdout
