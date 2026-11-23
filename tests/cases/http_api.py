#!/usr/bin/env python3
"""JSON API request context, response helpers and route after middleware."""

import http.client
import json
import os
import subprocess
import tempfile
import time

from harness import *  # noqa: F401,F403


def start_server(source, label):
    proc = subprocess.Popen(
        [BIN],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    proc.stdin.write(source)
    proc.stdin.close()
    line = proc.stdout.readline().strip()
    if not line.isdigit():
        proc.terminate()
        proc.wait(timeout=5)
        raise AssertionError(f"{label} did not listen: {line!r}")
    # The port is printed before worker pools finish binding their handlers.
    time.sleep(0.1)
    return proc, int(line)


def stop_server(proc):
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
    stderr = proc.stderr.read()
    return stderr


def request(port, path, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request("GET", path, headers=headers or {})
    response = conn.getresponse()
    body = response.read().decode("utf-8")
    values = response.getheaders()
    conn.close()
    return response.status, values, body


def request_headers(port, path, headers):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.putrequest("GET", path)
    for name, value in headers:
        conn.putheader(name, value)
    conn.endheaders()
    response = conn.getresponse()
    body = response.read().decode("utf-8")
    values = response.getheaders()
    conn.close()
    return response.status, values, body


def header_values(headers, name):
    target = name.lower()
    return [value for key, value in headers if key.lower() == target]


def run_cases():
    source = """
import std;

let s = std.http.listen("127.0.0.1", 0);
println(str(s["port"]));
std.io.flush(std.io.stdout);
let n = std.http.serve(s, func(req) {
  return std.http.json_response(200, {
    "path": req["path"],
    "target": req["target"],
    "query": req["query"],
    "cookies": req["cookies"],
    "request_id": req["request_id"],
    "client_ip": req["client_ip"],
    "x_values": req["header_values"]["x-multi"],
    "trace_id": req["trace_id"],
    "span_id": req["span_id"],
    "parent_span_id": req["parent_span_id"],
    "trace_flags": req["trace_flags"],
    "traceparent": req["traceparent"]
  }, {
    "Set-Cookie": ["a=1; Path=/", "b=2; Path=/"]
  });
}, 1);
println("served " + str(n));
"""
    proc, port = start_server(source, "http api context")
    try:
        status, headers, body = request_headers(
            port,
            "/hello?x=1&x=2&y=hi%20there",
            [
                ("Host", "127.0.0.1"),
                ("Cookie", "a=1; b=two"),
                ("X-Request-Id", "req-123"),
                ("X-Multi", "one"),
                ("X-Multi", "two"),
                ("traceparent",
                 "00-4bf92f3577b34da6a3ce929d0e0e4736-"
                 "00f067aa0ba902b7-01"),
            ],
        )
        assert status == 200, status
        cookies = header_values(headers, "Set-Cookie")
        assert cookies == ["a=1; Path=/", "b=2; Path=/"], cookies
        assert header_values(headers, "X-Request-Id") == ["req-123"], headers
        payload = json.loads(body)
        assert payload["path"] == "/hello", payload
        assert payload["target"] == "/hello?x=1&x=2&y=hi%20there", payload
        assert payload["query"] == {"x": ["1", "2"], "y": "hi there"}, payload
        assert payload["cookies"] == {"a": "1", "b": "two"}, payload
        assert payload["request_id"] == "req-123", payload
        assert payload["client_ip"] == "127.0.0.1", payload
        assert payload["x_values"] == ["one", "two"], payload
        assert payload["trace_id"] == (
            "4bf92f3577b34da6a3ce929d0e0e4736"), payload
        assert payload["parent_span_id"] == "00f067aa0ba902b7", payload
        assert len(payload["span_id"]) == 16, payload
        assert payload["span_id"] != payload["parent_span_id"], payload
        assert payload["trace_flags"] == "01", payload
        assert payload["traceparent"] == (
            "00-4bf92f3577b34da6a3ce929d0e0e4736-"
            + payload["span_id"] + "-01"), payload
        assert header_values(headers, "X-Trace-Id") == [
            "4bf92f3577b34da6a3ce929d0e0e4736"], headers
    finally:
        stop_server(proc)

    source = """
import std;
import std.route;

let app = route.new();
app.use_after(func(req, res) {
  res["headers"]["X-After"] = "yes";
  return res;
});
app.get("/err", func(req) {
  return std.http.json_error(422, "BAD_INPUT", "name is required",
                             {"field": "name"});
});
app.get("/ok", func(req) {
  return std.http.json_response(200, {"ok": true});
});
app.get("/boom", func(req) { fail("boom"); });

let s = std.http.listen("127.0.0.1", 0);
println(str(s["port"]));
std.io.flush(std.io.stdout);
let n = std.http.serve(s, func(req) { return app.dispatch(req); }, 3);
println("served " + str(n));
"""
    proc, port = start_server(source, "http api route")
    try:
        status, headers, body = request(port, "/err")
        assert status == 422, status
        assert header_values(headers, "X-After") == ["yes"], headers
        payload = json.loads(body)
        assert payload["error"]["code"] == "BAD_INPUT", payload
        assert payload["error"]["details"] == {"field": "name"}, payload
        assert header_values(headers, "X-Request-Id"), headers

        status, headers, body = request(port, "/ok")
        assert status == 200, status
        assert header_values(headers, "X-After") == ["yes"], headers
        assert json.loads(body) == {"ok": True}, body

        status, headers, body = request(port, "/boom")
        assert status == 500, status
        payload = json.loads(body)
        assert payload["error"]["code"] == "internal_error", payload
        assert header_values(headers, "X-Request-Id"), headers
    finally:
        stop_server(proc)

    source = """
import std;

let s = std.http.listen("127.0.0.1", 0);
println(str(s["port"]));
std.io.flush(std.io.stdout);
s["handler_timeout_ms"] = 50;
let n = std.http.serve(s, func(req) {
  return std.task.sleep(500);
}, 1);
println("served " + str(n));
"""
    proc, port = start_server(source, "http api timeout")
    try:
        status, headers, body = request(port, "/slow")
        assert status == 504, status
        payload = json.loads(body)
        assert payload["error"]["code"] == "timeout", payload
        assert header_values(headers, "X-Request-Id"), headers
    finally:
        stop_server(proc)

    source = """
import std;

let s = std.http2.listen("127.0.0.1", 0);
println(str(s["port"]));
std.io.flush(std.io.stdout);
s["handler_timeout_ms"] = 50;
let n = std.http2.serve(s, func(req) {
  if (req["path"] == "/boom") {
    fail("boom");
  }
  if (req["path"] == "/slow") {
    return std.task.sleep(500);
  }
  return std.http.json_response(200, {
    "path": req["path"],
    "query": req["query"],
    "cookies": req["cookies"],
    "request_id": req["request_id"]
  }, {
    "Set-Cookie": ["x=1; Path=/", "y=2; Path=/"]
  });
}, 3);
println("served " + str(n));
"""
    proc, port = start_server(source, "http2 api context")
    try:
        client = run(
            "import std;\n"
            f'let a = std.http2.get("http://127.0.0.1:{port}/ctx?a=1",\n'
            '  {"headers": {"Cookie": "sid=abc"}});\n'
            'println(str(a["status"]));\n'
            'println(str(len(a["set_cookie"])));\n'
            'println(std.bytes.to_string(a["body"]));\n'
            f'let b = std.http2.get("http://127.0.0.1:{port}/boom");\n'
            'println(str(b["status"]));\n'
            'println(std.bytes.to_string(b["body"]));\n'
            f'let c = std.http2.get("http://127.0.0.1:{port}/slow");\n'
            'println(str(c["status"]));\n'
            'println(std.bytes.to_string(c["body"]));\n'
        )
        assert client.returncode == 0, client.stderr
        lines = client.stdout.strip().splitlines()
        assert lines[0] == "200", lines
        assert lines[1] == "2", lines
        payload = json.loads(lines[2])
        assert payload["path"] == "/ctx", payload
        assert payload["query"] == {"a": "1"}, payload
        assert payload["cookies"] == {"sid": "abc"}, payload
        assert payload["request_id"], payload
        assert lines[3] == "500", lines
        payload = json.loads(lines[4])
        assert payload["error"]["code"] == "internal_error", payload
        assert lines[5] == "504", lines
        payload = json.loads(lines[6])
        assert payload["error"]["code"] == "timeout", payload
    finally:
        stop_server(proc)

    with tempfile.TemporaryDirectory(prefix="skein-http-api-") as tmp:
        worker = os.path.join(tmp, "worker.skein")
        with open(worker, "w", encoding="utf-8") as handle:
            handle.write(
                "import std;\n"
                "func handle(req) {\n"
                "  return std.http.json_response(200, {\n"
                '    "path": req["path"],\n'
                '    "query": req["query"],\n'
                '    "cookies": req["cookies"],\n'
                '    "request_id": req["request_id"],\n'
                '    "client_ip": req["client_ip"]\n'
                "  });\n"
                "}\n"
            )
        worker_text = worker.replace("\\", "\\\\")
        source = (
            "import std;\n"
            'let s = std.http.listen("127.0.0.1", 0);\n'
            'println(s["port"]);\n'
            "std.io.flush(std.io.stdout);\n"
            f'let ts = std.http.serve_threads(s, 2, "{worker_text}", '
            '{"max_requests": 1});\n'
            "ts.join(20000);\n"
        )
        proc, port = start_server(source, "http api threads")
        try:
            status, headers, body = request(
                port,
                "/thread?q=1",
                {"Cookie": "sid=t", "X-Request-Id": "thread-1"},
            )
            assert status == 200, status
            payload = json.loads(body)
            assert payload["path"] == "/thread", payload
            assert payload["query"] == {"q": "1"}, payload
            assert payload["cookies"] == {"sid": "t"}, payload
            assert payload["request_id"] == "thread-1", payload
            assert payload["client_ip"] == "127.0.0.1", payload
        finally:
            stop_server(proc)

        source = (
            "import std;\n"
            'let s = std.http.listen("127.0.0.1", 0);\n'
            'println(s["port"]);\n'
            "std.io.flush(std.io.stdout);\n"
            f'let ps = std.http.serve_processes(s, 1, "{worker_text}", '
            '{"max_requests": 1});\n'
            "ps.join(20000);\n"
        )
        proc, port = start_server(source, "http api processes")
        try:
            status, headers, body = request(
                port,
                "/process?q=2",
                {"Cookie": "sid=p", "X-Request-Id": "process-1"},
            )
            assert status == 200, status
            payload = json.loads(body)
            assert payload["path"] == "/process", payload
            assert payload["query"] == {"q": "2"}, payload
            assert payload["cookies"] == {"sid": "p"}, payload
            assert payload["request_id"] == "process-1", payload
            assert payload["client_ip"] == "127.0.0.1", payload
        finally:
            stop_server(proc)
