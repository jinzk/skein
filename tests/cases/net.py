#!/usr/bin/env python3
"""Networking, HTTP, HTTP/2, TLS and WebSocket integration.

Extracted from the original test_skein.py main() body, lines 3523-7537.
"""
from harness import *  # noqa: F401,F403


def run_cases():
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
            "println(bytes_to_string(data));\n"
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
            "println(bytes_to_string(std.io.read(s, 100, 1000)));\n"
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
            'await std.io.write_task(s, "hello", 1000)\n'
            "let data = await std.io.read_task(s, 100, 1000);\n"
            "println(bytes_to_string(data));\n"
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
            'len(await std.io.read_task(b, 2)) == 2\n'
            'len(await std.io.read_task(b, 100)) > 0\n'
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
            'println(r["status"]);\n'
            'println(r["headers"]["x-test"]);\n'
            'println(bytes_to_string(r["body"]));\n'
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
            'println(bytes_to_string(a["body"]));\n'
            'println(bytes_to_string(b["body"]));\n'
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
            f'let b = await std.http.get_task('
            f'"http://127.0.0.1:{async_keepalive_port}/b");\n'
            'println(bytes_to_string(a["body"]));\n'
            'println(bytes_to_string(b["body"]));\n'
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
            'println(bytes_to_string(a["body"]));\n'
            'println(bytes_to_string(b["body"]));\n'
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
            f'let a = await std.http.get_task('
            f'"http://127.0.0.1:{stale_async_port}/a");\n'
            f'let b = await std.http.get_task('
            f'"http://127.0.0.1:{stale_async_port}/b");\n'
            'println(bytes_to_string(a["body"]));\n'
            'println(bytes_to_string(b["body"]));\n'
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
            '  println("s=" + bytes_to_string(chunk));\n'
            "}});\n"
            'println("sb=" + bytes_to_string(a["body"]));\n'
            f'let b = await std.http.get_task('
            f'"http://127.0.0.1:{stream_port}/b", '
            '{"on_body": func(chunk) {\n'
            '  println("a=" + bytes_to_string(chunk));\n'
            "}});\n"
            'println("ab=" + bytes_to_string(b["body"]));\n',
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
            'println(bytes_to_string(a["body"]));\n'
            f'let b = await std.http.post_task('
            f'"http://127.0.0.1:{request_stream_port}/b", '
            '{"body_stream": async_parts()});\n'
            'println(bytes_to_string(b["body"]));\n',
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
                'println("served " + str(n));\n'
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
                'println(a["headers"]["transfer-encoding"]);\n'
                'println(bytes_to_string(a["body"]));\n'
                f'let b = await std.http.get_task('
                f'"http://127.0.0.1:{serve_stream_port}/b");\n'
                'println(bytes_to_string(b["body"]));\n',
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
                'println(a["headers"]["transfer-encoding"]);\n'
                'println(bytes_to_string(a["body"]));\n',
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
            f'let t = std.http.get_task('
            f'"http://127.0.0.1:{http_async_port}/");\n'
            'let r = await t;\n'
            'println(r["status"]);\n'
            'println(bytes_to_string(r["body"]));\n'
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
                'println("ready");\n'
                "std.io.flush(std.io.stdout);\n"
                "let req = std.http.accept(s, 10000);\n"
                'std.http.respond(req, 200, {"X-Test": "yes"}, '
                '"hello " + req["path"]);\n'
                "std.http.close(s);\n"
            )
            http_server_proc.stdin.close()
            ready_line = http_server_proc.stdout.readline().strip()
            assert ready_line == "ready", ready_line
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
                'println("ready");\n'
                "std.io.flush(std.io.stdout);\n"
                "let req = await std.http.accept_task(s, 10000);\n"
                'await std.http.respond_task(req, 200, '
                '{"X-Async": "yes"}, "async " + req["path"]);\n'
                "std.http.close(s);\n"
            )
            http_async_server_proc.stdin.close()
            async_ready_line = http_async_server_proc.stdout.readline().strip()
            assert async_ready_line == "ready", async_ready_line
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
                    'println("served " + str(n));\n'
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
                    'println(str(a["status"]) + " " + a["status_text"] '
                    '+ " " + a["protocol"]);\n'
                    'println(bytes_to_string(a["body"]));\n'
                    'println(a["headers"]["x-path"]);\n'
                    f'let b = std.http2.post('
                    f'"http://127.0.0.1:{http2_port}/post", "payload");\n'
                    'println(bytes_to_string(b["body"]));\n'
                    'let c = await std.http2.get_task('
                    f'"http://127.0.0.1:{http2_port}/async");\n'
                    'println(bytes_to_string(c["body"]));\n',
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

            def h2_free_port():
                probe = socket.socket()
                probe.bind(("127.0.0.1", 0))
                value = probe.getsockname()[1]
                probe.close()
                return value

            def h2_start_server(source, port):
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
                deadline = time.time() + 5
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", port), timeout=0.2)
                        probe.close()
                        return proc
                    except OSError:
                        time.sleep(0.05)
                raise AssertionError("http2 stress server did not listen")

            h2_chunk = "0123456789abcdef" * 64
            h2_build_body = (
                f'let chunk = "{h2_chunk}";\n'
                "let parts = [];\n"
                "for (let i = 0; i < 1024; i = i + 1) {\n"
                "  parts = std.list.append(parts, chunk);\n"
                "}\n"
                'let body = std.string.join(parts, "");\n'
            )

            flow_port = h2_free_port()
            flow_server = (
                "import std;\n"
                f'let s = std.http2.listen("127.0.0.1", {flow_port});\n'
                's["settings"] = {"max_concurrent_streams": 4, '
                '"initial_window_size": 16384};\n'
                "let n = std.http2.serve(s, func(req) {\n"
                '  if (req["path"] == "/download") {\n'
                + h2_build_body +
                '    return {"status": 200, "body": body};\n'
                "  }\n"
                '  return {"status": 200, "body": "len=" + '
                'str(len(req["body"]))};\n'
                "}, 2);\n"
                'println("served " + str(n));\n'
            )
            flow_proc = h2_start_server(flow_server, flow_port)
            try:
                flow_client = run(
                    "import std;\n"
                    + h2_build_body +
                    f'let up = std.http2.post('
                    f'"http://127.0.0.1:{flow_port}/upload", body,\n'
                    '  {"settings": {"initial_window_size": 16384}});\n'
                    'println("sent=" + str(len(body)));\n'
                    'println(str(up["status"]) + ":" + '
                    'bytes_to_string(up["body"]));\n'
                    f'let down = std.http2.get('
                    f'"http://127.0.0.1:{flow_port}/download",\n'
                    '  {"settings": {"initial_window_size": 16384}});\n'
                    'println("down=" + str(len(down["body"])));\n',
                    timeout=30,
                )
                assert flow_client.returncode == 0, flow_client.stderr
                assert "sent=1048576" in flow_client.stdout, (
                    flow_client.stdout)
                assert "200:len=1048576" in flow_client.stdout, (
                    flow_client.stdout)
                assert "down=1048576" in flow_client.stdout, (
                    flow_client.stdout)
                flow_proc.wait(timeout=20)
                assert "served 2" in flow_proc.stdout.read()
            finally:
                if flow_proc.poll() is None:
                    flow_proc.terminate()
                    flow_proc.wait(timeout=5)

            stream_port = h2_free_port()
            stream_server = (
                "import std;\n"
                f'let s = std.http2.listen("127.0.0.1", {stream_port});\n'
                's["settings"] = {"max_concurrent_streams": 4, '
                '"initial_window_size": 16384};\n'
                's["stream_request_body"] = true;\n'
                "let n = std.http2.serve(s, func(req) {\n"
                "  let total = 0;\n"
                "  let chunk = std.http.read_body(req);\n"
                "  while (chunk != null) {\n"
                "    total = total + len(chunk);\n"
                "    std.time.sleep(1);\n"
                "    chunk = std.http.read_body(req);\n"
                "  }\n"
                '  return {"status": 200, "body": "total=" + '
                'str(total)};\n'
                "}, 1);\n"
                'println("served " + str(n));\n'
            )
            stream_proc = h2_start_server(stream_server, stream_port)
            try:
                stream_client = run(
                    "import std;\n"
                    + h2_build_body +
                    f'let r = std.http2.post('
                    f'"http://127.0.0.1:{stream_port}/upload", body,\n'
                    '  {"settings": {"initial_window_size": 16384}});\n'
                    'println(str(r["status"]) + ":" + '
                    'bytes_to_string(r["body"]));\n',
                    timeout=30,
                )
                assert stream_client.returncode == 0, stream_client.stderr
                assert "200:total=1048576" in stream_client.stdout, (
                    stream_client.stdout)
                stream_proc.wait(timeout=20)
                assert "served 1" in stream_proc.stdout.read()
            finally:
                if stream_proc.poll() is None:
                    stream_proc.terminate()
                    stream_proc.wait(timeout=5)

            back_port = h2_free_port()
            back_server = (
                "import std;\n"
                f'let s = std.http2.listen("127.0.0.1", {back_port});\n'
                's["settings"] = {"max_concurrent_streams": 4, '
                '"initial_window_size": 16384};\n'
                "let n = std.http2.serve(s, func(req) {\n"
                + h2_build_body +
                '  return {"status": 200, "body": body};\n'
                "}, 1);\n"
                'println("served " + str(n));\n'
            )
            back_proc = h2_start_server(back_server, back_port)
            try:
                back_client = run(
                    "import std;\n"
                    "let total = 0;\n"
                    "let chunks = 0;\n"
                    f'let r = std.http2.get('
                    f'"http://127.0.0.1:{back_port}/big",\n'
                    '  {"settings": {"initial_window_size": 16384},\n'
                    '   "on_body": func(chunk) {\n'
                    "       total = total + len(chunk);\n"
                    "       chunks = chunks + 1;\n"
                    "       std.time.sleep(1);\n"
                    "   }});\n"
                    'println("status=" + str(r["status"]));\n'
                    'println("total=" + str(total));\n'
                    'println("chunks=" + str(chunks));\n',
                    timeout=30,
                )
                assert back_client.returncode == 0, back_client.stderr
                assert "status=200" in back_client.stdout, (
                    back_client.stdout)
                assert "total=1048576" in back_client.stdout, (
                    back_client.stdout)
                back_proc.wait(timeout=20)
                assert "served 1" in back_proc.stdout.read()
            finally:
                if back_proc.poll() is None:
                    back_proc.terminate()
                    back_proc.wait(timeout=5)

            many_port = h2_free_port()
            many_server = (
                "import std;\n"
                f'let s = std.http2.listen("127.0.0.1", {many_port});\n'
                's["settings"] = {"max_concurrent_streams": 2, '
                '"initial_window_size": 16384};\n'
                "let n = std.http2.serve(s, func(req) {\n"
                "  std.time.sleep(5);\n"
                '  return {"status": 200, "body": req["path"]};\n'
                "}, 16);\n"
                'println("served " + str(n));\n'
            )
            many_proc = h2_start_server(many_server, many_port)
            try:
                many_items = ", ".join(
                    '{"url": "http://127.0.0.1:%d/r%d"}' % (many_port, i)
                    for i in range(16))
                many_client = run(
                    "import std;\n"
                    f"let responses = std.http2.request_many(["
                    f"{many_items}],\n"
                    '  {"settings": {"max_concurrent_streams": 2, '
                    '"initial_window_size": 16384}});\n'
                    'println("count=" + str(len(responses)));\n'
                    "let ok = true;\n"
                    "for (let i = 0; i < len(responses); i = i + 1) {\n"
                    '  if (responses[i]["status"] != 200) { ok = false; }\n'
                    '  if (bytes_to_string(responses[i]["body"]) != '
                    '"/r" + str(i)) { ok = false; }\n'
                    "}\n"
                    'println("ok=" + str(ok));\n',
                    timeout=30,
                )
                assert many_client.returncode == 0, many_client.stderr
                assert "count=16" in many_client.stdout, many_client.stdout
                assert "ok=true" in many_client.stdout, many_client.stdout
                many_proc.wait(timeout=20)
                assert "served 16" in many_proc.stdout.read()
            finally:
                if many_proc.poll() is None:
                    many_proc.terminate()
                    many_proc.wait(timeout=5)

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
                    'println("served " + str(n));\n'
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
                    'println(bytes_to_string(a["body"]));\n'
                    'println(bytes_to_string(b["body"]));\n',
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
                    'println("served " + str(n));\n'
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
                    '  println("s=" + bytes_to_string(chunk));\n'
                    "}});\n"
                    'println("sb=" + bytes_to_string(a["body"]));\n'
                    f'let b = await std.http2.post_task('
                    f'"http://127.0.0.1:{http2_stream_port}/b", '
                    '{"body_stream": async_parts(), '
                    '"on_body": func(chunk) {\n'
                    '  println("a=" + bytes_to_string(chunk));\n'
                    "}});\n"
                    'println("ab=" + bytes_to_string(b["body"]));\n',
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
                    'println("served " + str(n));\n'
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
                    'println(bytes_to_string(a["body"]));\n'
                    f'let b = await std.http2.get_task('
                    f'"http://127.0.0.1:{http2_send_port}/b");\n'
                    'println(bytes_to_string(b["body"]));\n',
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
                    f"}}, {h2_cookie_requests});\n"
                    'println("served " + str(n));\n'
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
                    'println(bytes_to_string(rs[0]["body"]));\n'
                    'println(bytes_to_string(rs[1]["body"]));\n'
                    'println(bytes_to_string(rs[2]["body"]));\n'
                    "let rs2 = await std.http2.request_many_task([\n"
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many_port}/d"}},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many_port}/e", '
                    '"method": "DELETE"},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many_port}/f", '
                    '"method": "POST", "body": "async"}\n'
                    "]);\n"
                    'println(bytes_to_string(rs2[0]["body"]));\n'
                    'println(bytes_to_string(rs2[1]["body"]));\n'
                    'println(bytes_to_string(rs2[2]["body"]));\n',
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
                    'println("served " + str(n));\n'
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
                    '    println("s1=" + bytes_to_string(c));\n'
                    "  }},\n"
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_ms_port}/b", '
                    '"method": "POST", "body_stream": p2(), '
                    '"on_body": func(c) {\n'
                    '    println("s2=" + bytes_to_string(c));\n'
                    "  }}\n"
                    "]);\n"
                    'println("r1=" + bytes_to_string(rs[0]["body"]));\n'
                    'println("r2=" + bytes_to_string(rs[1]["body"]));\n',
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
                    'println("served " + str(n));\n'
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
                    'println(bytes_to_string(rs[0]["body"]));\n'
                    'println(bytes_to_string(rs[1]["body"]));\n',
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

            h2_comp_probe = run(
                "import std;\n"
                'std.http2.get("http://127.0.0.1:1/", '
                '{"decompress": true});\n'
            )
            h2_compression_enabled = (
                "compression support is disabled" not in h2_comp_probe.stderr)
            h2_cookie_requests = 6 if h2_compression_enabled else 4
            http2_cookie_probe = socket.socket()
            http2_cookie_probe.bind(("127.0.0.1", 0))
            http2_cookie_port = http2_cookie_probe.getsockname()[1]
            http2_cookie_probe.close()
            http2_cookie_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_cookie_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_cookie_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  if (req["path"] == "/set") {\n'
                    '    return {"status": 200, "headers": '
                    '{"Set-Cookie": "a=1; Path=/"}, "body": "set"};\n'
                    "  }\n"
                    '  if (req["path"] == "/check") {\n'
                    '    let c = std.dict.get(req["headers"], "cookie");\n'
                    '    let cs = "";\n'
                    "    if (c != null) { cs = c; }\n"
                    '    return {"status": 200, "body": "cookie=" + cs};\n'
                    "  }\n"
                    '  return {"status": 200, "headers": '
                    '{"Content-Encoding": "gzip"}, "body": '
                    'bytes_from_hex("1f8b08000000000002ffcb48cdc9c957c830'
                    '0200d243ada208000000")};\n'
                    "}, 6);\n"
                    'println("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_cookie_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_cookie_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 cookie server did not listen"
                h2_cookie_source = (
                    "import std;\n"
                    "let jar = {};\n"
                    f'let a = std.http2.get('
                    f'"http://127.0.0.1:{http2_cookie_port}/set", '
                    '{"cookie_jar": jar});\n'
                    f'let b = std.http2.get('
                    f'"http://127.0.0.1:{http2_cookie_port}/check", '
                    '{"cookie_jar": jar});\n'
                    'println(bytes_to_string(b["body"]));\n'
                    "let jar2 = {};\n"
                    f'let c = await std.http2.get_task('
                    f'"http://127.0.0.1:{http2_cookie_port}/set", '
                    '{"cookie_jar": jar2});\n'
                    f'let d = await std.http2.get_task('
                    f'"http://127.0.0.1:{http2_cookie_port}/check", '
                    '{"cookie_jar": jar2});\n'
                    'println(bytes_to_string(d["body"]));\n'
                )
                if h2_compression_enabled:
                    h2_cookie_source += (
                        f'let g = std.http2.get('
                        f'"http://127.0.0.1:{http2_cookie_port}/gzip", '
                        '{"decompress": true});\n'
                        'println(bytes_to_string(g["body"]));\n'
                        f'let h = await std.http2.get_task('
                        f'"http://127.0.0.1:{http2_cookie_port}/gzip", '
                        '{"decompress": true});\n'
                        'println(bytes_to_string(h["body"]));\n'
                    )
                http2_cookie_client = run(h2_cookie_source, timeout=15)
                assert http2_cookie_client.returncode == 0, (
                    http2_cookie_client.stderr)
                cookie_lines = http2_cookie_client.stdout.splitlines()
                assert cookie_lines[:2] == ["cookie=a=1", "cookie=a=1"], (
                    http2_cookie_client.stdout)
                if h2_compression_enabled:
                    assert cookie_lines == [
                        "cookie=a=1", "cookie=a=1", "hello h2",
                        "hello h2"], http2_cookie_client.stdout
                else:
                    assert len(cookie_lines) == 2, (
                        http2_cookie_client.stdout)
                http2_cookie_proc.wait(timeout=10)
                cookie_output = http2_cookie_proc.stdout.read()
                assert f"served {h2_cookie_requests}" in cookie_output, (
                    cookie_output)
            finally:
                if http2_cookie_proc.poll() is None:
                    http2_cookie_proc.terminate()
                    http2_cookie_proc.wait(timeout=5)

            h2_many_requests = 5 if h2_compression_enabled else 3
            http2_many2_probe = socket.socket()
            http2_many2_probe.bind(("127.0.0.1", 0))
            http2_many2_port = http2_many2_probe.getsockname()[1]
            http2_many2_probe.close()
            http2_many2_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                http2_many2_proc.stdin.write(
                    "import std;\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{http2_many2_port});\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  if (req["path"] == "/set") {\n'
                    '    return {"status": 200, "headers": '
                    '{"Set-Cookie": "a=1; Path=/"}, "body": "set"};\n'
                    "  }\n"
                    '  if (req["path"] == "/check") {\n'
                    '    let c = std.dict.get(req["headers"], "cookie");\n'
                    '    let cs = "";\n'
                    "    if (c != null) { cs = c; }\n"
                    '    return {"status": 200, "body": "cookie=" + cs};\n'
                    "  }\n"
                    '  return {"status": 200, "headers": '
                    '{"Content-Encoding": "gzip"}, "body": '
                    'bytes_from_hex("1f8b08000000000002ffcb48cdc9c957c830'
                    '0200d243ada208000000")};\n'
                    f"}}, {h2_many_requests});\n"
                    'println("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                http2_many2_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", http2_many2_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 many cookie server did not listen"
                h2_many_source = (
                    "import std;\n"
                    "let jar = {};\n"
                    f'let a = std.http2.get('
                    f'"http://127.0.0.1:{http2_many2_port}/set", '
                    '{"cookie_jar": jar});\n'
                    f'let rs = std.http2.request_many([\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many2_port}/check"}},\n'
                    f'  {{"url": '
                    f'"http://127.0.0.1:{http2_many2_port}/check"}}\n'
                    '], {"cookie_jar": jar});\n'
                    'println(bytes_to_string(rs[0]["body"]));\n'
                    'println(bytes_to_string(rs[1]["body"]));\n'
                )
                if h2_compression_enabled:
                    h2_many_source += (
                        f'let gs = std.http2.request_many([\n'
                        f'  {{"url": '
                        f'"http://127.0.0.1:{http2_many2_port}/gzip"}},\n'
                        f'  {{"url": '
                        f'"http://127.0.0.1:{http2_many2_port}/gzip"}}\n'
                        '], {"decompress": true});\n'
                        'println(bytes_to_string(gs[0]["body"]));\n'
                        'println(bytes_to_string(gs[1]["body"]));\n'
                    )
                http2_many2_client = run(h2_many_source, timeout=15)
                assert http2_many2_client.returncode == 0, (
                    http2_many2_client.stderr)
                many2_lines = http2_many2_client.stdout.splitlines()
                assert many2_lines[:2] == ["cookie=a=1", "cookie=a=1"], (
                    http2_many2_client.stdout)
                if h2_compression_enabled:
                    assert many2_lines == [
                        "cookie=a=1", "cookie=a=1", "hello h2",
                        "hello h2"], http2_many2_client.stdout
                http2_many2_proc.wait(timeout=10)
                many2_output = http2_many2_proc.stdout.read()
                assert f"served {h2_many_requests}" in many2_output, (
                    many2_output)
            finally:
                if http2_many2_proc.poll() is None:
                    http2_many2_proc.terminate()
                    http2_many2_proc.wait(timeout=5)

            h2_maxconn_probe = socket.socket()
            h2_maxconn_probe.bind(("127.0.0.1", 0))
            h2_maxconn_port = h2_maxconn_probe.getsockname()[1]
            h2_maxconn_probe.close()
            h2_maxconn_proc = subprocess.Popen(
                [BIN],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                h2_maxconn_proc.stdin.write(
                    "import std;\n"
                    "async func handler(req) {\n"
                    "  await std.task.sleep(300);\n"
                    '  return {"status": 200, "body": "ok"};\n'
                    "}\n"
                    f'let s = std.http2.listen("127.0.0.1", '
                    f"{h2_maxconn_port});\n"
                    's["max_connections"] = 1;\n'
                    "let n = std.http2.serve(s, handler, 2);\n"
                    'println("served " + str(n));\n'
                    "std.http2.close(s);\n"
                )
                h2_maxconn_proc.stdin.close()
                deadline = time.time() + 5
                ready = False
                while time.time() < deadline:
                    try:
                        probe = socket.create_connection(
                            ("127.0.0.1", h2_maxconn_port), timeout=0.2)
                        probe.close()
                        ready = True
                        break
                    except OSError:
                        time.sleep(0.05)
                assert ready, "http2 max_connections server did not listen"
                client_source = (
                    "import std;\n"
                    f'let a = std.http2.get('
                    f'"http://127.0.0.1:{h2_maxconn_port}/a");\n'
                    'println(bytes_to_string(a["body"]));\n'
                )
                started = time.time()
                h2mc1 = subprocess.Popen(
                    [BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, encoding="utf-8")
                time.sleep(0.03)
                h2mc2 = subprocess.Popen(
                    [BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, encoding="utf-8")
                h2mc1.stdin.write(client_source)
                h2mc1.stdin.close()
                h2mc2.stdin.write(client_source)
                h2mc2.stdin.close()
                h2out1, h2err1 = h2mc1.communicate(timeout=15)
                h2out2, h2err2 = h2mc2.communicate(timeout=15)
                elapsed = time.time() - started
                assert h2mc1.returncode == 0, h2err1
                assert h2mc2.returncode == 0, h2err2
                assert "ok" in h2out1, h2out1
                assert "ok" in h2out2, h2out2
                assert elapsed >= 0.50, (
                    f"http2 max_connections was not enforced: "
                    f"{elapsed:.3f}s")
                h2_maxconn_proc.wait(timeout=10)
                h2_output = h2_maxconn_proc.stdout.read()
                assert "served 2" in h2_output, h2_output
            finally:
                if h2_maxconn_proc.poll() is None:
                    h2_maxconn_proc.terminate()
                    h2_maxconn_proc.wait(timeout=5)

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
                    '  println("rb=" + bytes_to_string(chunk));\n'
                    "};\n"
                    "let n = std.http2.serve(s, func(req) {\n"
                    '  return {"status": 200, "body": "handler_body=" + '
                    'bytes_to_string(req["body"])};\n'
                    "}, 1);\n"
                    'println("served " + str(n));\n'
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
                    'println(bytes_to_string(a["body"]));\n',
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
                    'println("served " + str(n));\n'
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
                    'println(bytes_to_string(rs[0]["body"]));\n'
                    'println(bytes_to_string(rs[1]["body"]));\n'
                    'println(bytes_to_string(rs[2]["body"]));\n',
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
                    'println("served " + str(n));\n'
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
                    'println(bytes_to_string(a["body"]));\n'
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
                    'println("served " + str(n));\n'
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
                    f'let t1 = std.http2.get_task('
                    f'"http://127.0.0.1:{http2_shared_port}/a");\n'
                    f'let t2 = std.http2.get_task('
                    f'"http://127.0.0.1:{http2_shared_port}/b");\n'
                    f'let t3 = std.http2.get_task('
                    f'"http://127.0.0.1:{http2_shared_port}/c");\n'
                    "let a = await t1;\n"
                    "let b = await t2;\n"
                    "let c = await t3;\n"
                    'println(bytes_to_string(a["body"]));\n'
                    'println(bytes_to_string(b["body"]));\n'
                    'println(bytes_to_string(c["body"]));\n'
                    f'let p1 = std.http2.post_task('
                    f'"http://127.0.0.1:{http2_shared_port}/d", "one");\n'
                    f'let p2 = std.http2.post_task('
                    f'"http://127.0.0.1:{http2_shared_port}/e", "two");\n'
                    f'let p3 = std.http2.post_task('
                    f'"http://127.0.0.1:{http2_shared_port}/f", "three");\n'
                    "let d = await p1;\n"
                    "let e = await p2;\n"
                    "let f = await p3;\n"
                    'println(bytes_to_string(d["body"]));\n'
                    'println(bytes_to_string(e["body"]));\n'
                    'println(bytes_to_string(f["body"]));\n',
                    timeout=15,
                )
                elapsed = time.time() - started
                assert http2_shared_client.returncode == 0, (
                    http2_shared_client.stderr)
                shared_lines = http2_shared_client.stdout.splitlines()
                assert len(shared_lines) == 6, http2_shared_client.stdout
                shared_cids = [line.split()[0] for line in shared_lines]
                assert len(set(shared_cids)) == 1, (
                    "get_task/post_task did not share one connection: "
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
                    'println("served " + str(n));\n'
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
                    f'let a = await std.http2.get_task('
                    f'"http://127.0.0.1:{http2_shared_go_port}/a");\n'
                    f'let b = await std.http2.get_task('
                    f'"http://127.0.0.1:{http2_shared_go_port}/b");\n'
                    'println(bytes_to_string(a["body"]));\n'
                    'println(bytes_to_string(b["body"]));\n'
                    f'let t1 = std.http2.get_task('
                    f'"http://127.0.0.1:{http2_shared_go_port}/c");\n'
                    f'let t2 = std.http2.get_task('
                    f'"http://127.0.0.1:{http2_shared_go_port}/d");\n'
                    "let c = await t1;\n"
                    "let d = await t2;\n"
                    'println(bytes_to_string(c["body"]));\n'
                    'println(bytes_to_string(d["body"]));\n',
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
                    "get_task reused a GOAWAY connection: "
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
                    'println("served " + str(n));\n'
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
                    f'let t1 = std.http2.get_task('
                    f'"http://127.0.0.1:{http2_retry_port}/a");\n'
                    f'let t2 = std.http2.get_task('
                    f'"http://127.0.0.1:{http2_retry_port}/b");\n'
                    "let a = await t1;\n"
                    "let b = await t2;\n"
                    'println(bytes_to_string(a["body"]));\n'
                    'println(bytes_to_string(b["body"]));\n',
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
                    'println("served " + str(n));\n'
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
                    f'let t1 = std.http2.get_task('
                    f'"http://127.0.0.1:{http2_drain_port}/a");\n'
                    f'let t2 = std.http2.get_task('
                    f'"http://127.0.0.1:{http2_drain_port}/b");\n'
                    "let a = await t1;\n"
                    "let b = await t2;\n"
                    'println(bytes_to_string(a["body"]));\n'
                    'println(bytes_to_string(b["body"]));\n',
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
                    'println("served " + str(n));\n'
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
                    f'let t1 = std.http2.post_task('
                    f'"http://127.0.0.1:{http2_ss_port}/a", '
                    '{"body_stream": p1(), "on_body": func(c) {\n'
                    '  println("s1=" + bytes_to_string(c));\n'
                    "}});\n"
                    f'let t2 = std.http2.post_task('
                    f'"http://127.0.0.1:{http2_ss_port}/b", '
                    '{"body_stream": p2(), "on_body": func(c) {\n'
                    '  println("s2=" + bytes_to_string(c));\n'
                    "}});\n"
                    "let r1 = await t1;\n"
                    "let r2 = await t2;\n"
                    'println("r1=" + bytes_to_string(r1["body"]));\n'
                    'println("r2=" + bytes_to_string(r2["body"]));\n',
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
                    "println(std.tls.alpn(t));\n"
                    'std.io.write_all(t, "hello");\n'
                    "println(bytes_to_string(std.io.read(t, 100)));\n"
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
                        'println("served " + str(n));\n'
                        "std.http2.close(s);\n"
                    )
                    h2_tls_server_proc.stdin.close()
                    time.sleep(0.5)
                    h2_tls_client_proc = run(
                        "import std;\n"
                        f'let a = std.http2.get('
                        f'"https://127.0.0.1:{h2_tls_port}/secure", '
                        '{"tls": {"verify": false}});\n'
                        'println(str(a["status"]) + " " + a["protocol"]);\n'
                        'println(bytes_to_string(a["body"]));\n',
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
                tls_skein_server = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                tls_skein_client = None
                try:
                    tls_skein_server.stdin.write(
                        "import std;\n"
                        f'let l = std.net.tcp_listen("127.0.0.1", '
                        f"{tls_server_port}, 1000);\n"
                        "let c = std.net.accept(l, 5000);\n"
                        f'let t = std.tls.server(c, "{tls_cert}", '
                        f'"{tls_key}", '
                        '{"alpn": ["h2", "http/1.1"]});\n'
                        "println(std.tls.alpn(t));\n"
                        "let data = std.io.read(t, 100);\n"
                        'std.io.write_all(t, "server:" + '
                        "bytes_to_string(data));\n"
                        "std.io.close(t);\n"
                        "std.net.close(l);\n"
                    )
                    tls_skein_server.stdin.close()
                    deadline = time.time() + 5
                    while time.time() < deadline:
                        try:
                            tls_skein_client = socket.create_connection(
                                ("127.0.0.1", tls_server_port), timeout=0.2)
                            break
                        except OSError:
                            time.sleep(0.05)
                    assert tls_skein_client is not None, (
                        "tls server did not listen")
                    import ssl
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    context.check_hostname = False
                    context.verify_mode = ssl.CERT_NONE
                    context.set_alpn_protocols(["http/1.1"])
                    tls_wrapped = context.wrap_socket(
                        tls_skein_client, server_hostname="localhost")
                    assert tls_wrapped.selected_alpn_protocol() == "http/1.1"
                    tls_wrapped.sendall(b"hello")
                    tls_response = tls_wrapped.recv(100)
                    assert tls_response == b"server:hello", tls_response
                    tls_wrapped.close()
                finally:
                    if tls_skein_client:
                        tls_skein_client.close()
                    tls_skein_server.wait(timeout=10)
                    tls_skein_output = tls_skein_server.stdout.read()
                    assert "http/1.1" in tls_skein_output, tls_skein_output

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
                    'await std.io.write_task(t, "hello");\n'
                    "println(bytes_to_string(await std.io.read_task(t, 100)));\n"
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
                    'println(r["status"]);\n'
                    'println(bytes_to_string(r["body"]));\n'
                )
                https_thread.join(timeout=5)
                assert https_proc.returncode == 0, https_proc.stderr
                assert "200" in https_proc.stdout, https_proc.stdout
                assert "hello" in https_proc.stdout, https_proc.stdout

                https_probe = socket.socket()
                https_probe.bind(("127.0.0.1", 0))
                https_server_port = https_probe.getsockname()[1]
                https_probe.close()
                https_skein_server = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                https_skein_client = None
                try:
                    https_skein_server.stdin.write(
                        "import std;\n"
                        f'let s = std.http.listen_tls("127.0.0.1", '
                        f'{https_server_port}, "{tls_cert}", "{tls_key}");\n'
                        "let req = std.http.accept(s, 5000);\n"
                        'std.http.respond(req, 200, {"X-TLS": "yes"}, '
                        '"secure " + req["path"]);\n'
                        "std.http.close(s);\n"
                    )
                    https_skein_server.stdin.close()
                    deadline = time.time() + 5
                    while time.time() < deadline:
                        try:
                            https_skein_client = socket.create_connection(
                                ("127.0.0.1", https_server_port), timeout=0.2)
                            break
                        except OSError:
                            time.sleep(0.05)
                    assert https_skein_client is not None, (
                        "https server did not listen")
                    import ssl
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    context.check_hostname = False
                    context.verify_mode = ssl.CERT_NONE
                    https_wrapped = context.wrap_socket(
                        https_skein_client, server_hostname="localhost")
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
                    if https_skein_client:
                        https_skein_client.close()
                    https_skein_server.wait(timeout=10)

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
                    f'let r = await std.http.get_task('
                    f'"https://localhost:{async_https_port}/", '
                    f'{{"tls": {{"ca_file": "{tls_cert}"}}}});\n'
                    'println(r["status"]);\n'
                    'println(bytes_to_string(r["body"]));\n'
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
                async_https_skein = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                async_https_client = None
                try:
                    async_https_skein.stdin.write(
                        "import std;\n"
                        f'let s = std.http.listen_tls("127.0.0.1", '
                        f'{async_https_server_port}, "{tls_cert}", '
                        f'"{tls_key}");\n'
                        "let req = await std.http.accept_task(s, 10000);\n"
                        'await std.http.respond_task(req, 200, '
                        '{"X-Async": "yes"}, "async secure " + '
                        'req["path"]);\n'
                        "std.http.close(s);\n"
                    )
                    async_https_skein.stdin.close()
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
                    async_https_skein.wait(timeout=10)

                rotation_dir = os.path.join(tls_tmp, "rotation")
                os.makedirs(rotation_dir, exist_ok=True)

                def make_rotation_cert(name):
                    cert = os.path.join(rotation_dir, name + "_cert.pem")
                    key = os.path.join(rotation_dir, name + "_key.pem")
                    proc = subprocess.run(
                        ["openssl", "req", "-x509", "-newkey", "rsa:2048",
                         "-nodes", "-keyout", key, "-out", cert,
                         "-days", "1", "-subj", f"/CN={name}",
                         "-addext", "subjectAltName=DNS:localhost"],
                        capture_output=True, text=True)
                    assert proc.returncode == 0, proc.stderr
                    return cert, key

                rot_cert_a, rot_key_a = make_rotation_cert("rotation-a")
                rot_cert_b, rot_key_b = make_rotation_cert("rotation-b")
                active_cert = os.path.join(rotation_dir, "active_cert.pem")
                active_key = os.path.join(rotation_dir, "active_key.pem")
                shutil.copyfile(rot_cert_a, active_cert)
                shutil.copyfile(rot_key_a, active_key)

                def cert_der(path):
                    import ssl
                    with open(path, "r", encoding="utf-8") as handle:
                        pem = handle.read()
                    return ssl.PEM_cert_to_DER_cert(pem)

                der_a = cert_der(rot_cert_a)
                der_b = cert_der(rot_cert_b)

                rotation_probe = socket.socket()
                rotation_probe.bind(("127.0.0.1", 0))
                rotation_port = rotation_probe.getsockname()[1]
                rotation_probe.close()
                rotation_server = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                try:
                    rotation_server.stdin.write(
                        "import std;\n"
                        f'let l = std.net.tcp_listen("127.0.0.1", '
                        f"{rotation_port}, 1000);\n"
                        "for (let i = 0; i < 3; i = i + 1) {\n"
                        "  let c = std.net.accept(l, 5000);\n"
                        f'  let t = std.tls.server(c, "{active_cert}", '
                        f'"{active_key}");\n'
                        "  let data = std.io.read(t, 100);\n"
                        "  std.io.write_all(t, bytes_to_string(data));\n"
                        "  std.io.close(t);\n"
                        "}\n"
                        "std.net.close(l);\n"
                        'println("rotation served");\n'
                    )
                    rotation_server.stdin.close()

                    def rotation_peer_der():
                        import ssl
                        deadline = time.time() + 5
                        raw = None
                        while time.time() < deadline:
                            try:
                                raw = socket.create_connection(
                                    ("127.0.0.1", rotation_port), timeout=0.2)
                                break
                            except OSError:
                                time.sleep(0.05)
                        assert raw is not None, "rotation server did not listen"
                        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                        context.check_hostname = False
                        context.verify_mode = ssl.CERT_NONE
                        wrapped = context.wrap_socket(
                            raw, server_hostname="localhost")
                        peer = wrapped.getpeercert(binary_form=True)
                        wrapped.sendall(b"hello")
                        assert wrapped.recv(100) == b"hello"
                        wrapped.close()
                        return peer

                    assert rotation_peer_der() == der_a
                    time.sleep(1.1)
                    shutil.copyfile(rot_cert_b, active_cert)
                    shutil.copyfile(rot_key_b, active_key)
                    os.utime(active_cert, None)
                    os.utime(active_key, None)
                    assert rotation_peer_der() == der_b

                    time.sleep(1.1)
                    with open(active_cert, "w", encoding="utf-8") as handle:
                        handle.write("not a certificate\n")
                    os.utime(active_cert, None)
                    assert rotation_peer_der() == der_b

                    rotation_server.wait(timeout=10)
                    rotation_stderr = rotation_server.stderr.read()
                    assert "certificate reload failed" in rotation_stderr, (
                        rotation_stderr)
                    assert "rotation served" in rotation_server.stdout.read()
                finally:
                    if rotation_server.poll() is None:
                        rotation_server.terminate()
                        rotation_server.wait(timeout=5)

                mtls_dir = os.path.join(tls_tmp, "mtls")
                os.makedirs(mtls_dir, exist_ok=True)

                def openssl_run(args):
                    proc = subprocess.run(
                        ["openssl"] + args, capture_output=True, text=True)
                    assert proc.returncode == 0, proc.stderr
                    return proc

                ca_key = os.path.join(mtls_dir, "ca_key.pem")
                ca_cert = os.path.join(mtls_dir, "ca_cert.pem")
                openssl_run(["req", "-x509", "-newkey", "rsa:2048",
                             "-nodes", "-keyout", ca_key, "-out", ca_cert,
                             "-days", "1", "-subj", "/CN=skein-test-ca"])

                def signed_cert(name, common_name, san=None):
                    key = os.path.join(mtls_dir, name + "_key.pem")
                    csr = os.path.join(mtls_dir, name + ".csr")
                    cert = os.path.join(mtls_dir, name + "_cert.pem")
                    request = ["req", "-newkey", "rsa:2048", "-nodes",
                               "-keyout", key, "-out", csr, "-subj",
                               f"/CN={common_name}"]
                    if san:
                        request.extend(
                            ["-addext", f"subjectAltName=DNS:{san}"])
                    openssl_run(request)
                    sign = ["x509", "-req", "-in", csr, "-CA", ca_cert,
                            "-CAkey", ca_key, "-CAcreateserial", "-out",
                            cert, "-days", "1"]
                    if san:
                        sign.extend(["-copy_extensions", "copy"])
                    openssl_run(sign)
                    return cert, key

                mtls_server_cert, mtls_server_key = signed_cert(
                    "server", "localhost", "localhost")
                mtls_client_cert, mtls_client_key = signed_cert(
                    "client", "skein-client")

                def mtls_skein_server(auth):
                    probe = socket.socket()
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]
                    probe.close()
                    proc = subprocess.Popen(
                        [BIN],
                        stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        encoding="utf-8",
                    )
                    proc.stdin.write(
                        "import std;\n"
                        f'let l = std.net.tcp_listen("127.0.0.1", {port}, '
                        "1000);\n"
                        "let c = std.net.accept(l, 5000);\n"
                        f'let t = std.tls.server(c, "{mtls_server_cert}", '
                        f'"{mtls_server_key}", {{"client_ca_file": '
                        f'"{ca_cert}", "client_auth": "{auth}"}});\n'
                        "let data = std.io.read(t, 100);\n"
                        'std.io.write_all(t, "mtls:" + '
                        "bytes_to_string(data));\n"
                        "std.io.close(t);\n"
                        "std.net.close(l);\n"
                        'println("mtls served");\n'
                    )
                    proc.stdin.close()
                    return proc, port

                def mtls_python_client(port, with_cert):
                    import ssl as ssl_module
                    deadline = time.time() + 5
                    raw = None
                    while time.time() < deadline:
                        try:
                            raw = socket.create_connection(
                                ("127.0.0.1", port), timeout=0.2)
                            break
                        except OSError:
                            time.sleep(0.05)
                    assert raw is not None, "mtls server did not listen"
                    context = ssl_module.SSLContext(
                        ssl_module.PROTOCOL_TLS_CLIENT)
                    context.load_verify_locations(ca_cert)
                    if with_cert:
                        context.load_cert_chain(
                            mtls_client_cert, mtls_client_key)
                    wrapped = context.wrap_socket(
                        raw, server_hostname="localhost")
                    wrapped.sendall(b"hello")
                    result = wrapped.recv(100)
                    wrapped.close()
                    return result

                mtls_proc, mtls_port = mtls_skein_server("required")
                try:
                    assert mtls_python_client(
                        mtls_port, True) == b"mtls:hello"
                    mtls_proc.wait(timeout=10)
                    assert "mtls served" in mtls_proc.stdout.read()
                finally:
                    if mtls_proc.poll() is None:
                        mtls_proc.terminate()
                        mtls_proc.wait(timeout=5)

                missing_proc, missing_port = mtls_skein_server("required")
                try:
                    import ssl as ssl_module
                    rejected = False
                    try:
                        mtls_python_client(missing_port, False)
                    except ssl_module.SSLError:
                        rejected = True
                    assert rejected, "required client auth accepted no cert"
                    missing_proc.wait(timeout=10)
                    assert missing_proc.returncode != 0
                finally:
                    if missing_proc.poll() is None:
                        missing_proc.terminate()
                        missing_proc.wait(timeout=5)

                optional_proc, optional_port = mtls_skein_server("optional")
                try:
                    assert mtls_python_client(
                        optional_port, False) == b"mtls:hello"
                    optional_proc.wait(timeout=10)
                    assert "mtls served" in optional_proc.stdout.read()
                finally:
                    if optional_proc.poll() is None:
                        optional_proc.terminate()
                        optional_proc.wait(timeout=5)

                py_probe = socket.socket()
                py_probe.bind(("127.0.0.1", 0))
                py_port = py_probe.getsockname()[1]
                py_probe.close()
                py_listener = socket.socket()
                py_listener.setsockopt(
                    socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                py_listener.bind(("127.0.0.1", py_port))
                py_listener.listen(1)
                py_listener.settimeout(5)
                py_seen = []

                def mtls_python_server():
                    import ssl as ssl_module
                    context = ssl_module.SSLContext(
                        ssl_module.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(
                        mtls_server_cert, mtls_server_key)
                    context.verify_mode = ssl_module.CERT_REQUIRED
                    context.load_verify_locations(ca_cert)
                    try:
                        client, _ = py_listener.accept()
                        wrapped = context.wrap_socket(
                            client, server_side=True)
                        py_seen.append(wrapped.getpeercert())
                        data = wrapped.recv(100)
                        wrapped.sendall(b"client:" + data)
                        wrapped.close()
                    finally:
                        py_listener.close()

                py_thread = threading.Thread(target=mtls_python_server)
                py_thread.start()
                mtls_client_proc = run(
                    "import std;\n"
                    f'let s = std.net.tcp_connect("127.0.0.1", {py_port}, '
                    "1000);\n"
                    'let t = std.tls.client(s, "localhost", {\n'
                    f'  "ca_file": "{ca_cert}",\n'
                    f'  "cert_file": "{mtls_client_cert}",\n'
                    f'  "key_file": "{mtls_client_key}"\n'
                    "});\n"
                    'std.io.write_all(t, "hello");\n'
                    'println(bytes_to_string(std.io.read(t, 100)));\n'
                    "std.io.close(t);\n"
                )
                py_thread.join(timeout=5)
                assert mtls_client_proc.returncode == 0, (
                    mtls_client_proc.stderr)
                assert "client:hello" in mtls_client_proc.stdout, (
                    mtls_client_proc.stdout)
                assert py_seen, "python mtls server saw no client cert"
                subject = dict(entry[0] for entry in py_seen[0]["subject"])
                assert subject.get("commonName") == "skein-client", subject

                py_https_probe = socket.socket()
                py_https_probe.bind(("127.0.0.1", 0))
                py_https_port = py_https_probe.getsockname()[1]
                py_https_probe.close()
                py_https_listener = socket.socket()
                py_https_listener.setsockopt(
                    socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                py_https_listener.bind(("127.0.0.1", py_https_port))
                py_https_listener.listen(1)
                py_https_listener.settimeout(5)
                py_https_seen = []

                def mtls_https_python_server():
                    import ssl as ssl_module
                    context = ssl_module.SSLContext(
                        ssl_module.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(
                        mtls_server_cert, mtls_server_key)
                    context.verify_mode = ssl_module.CERT_REQUIRED
                    context.load_verify_locations(ca_cert)
                    try:
                        client, _ = py_https_listener.accept()
                        wrapped = context.wrap_socket(
                            client, server_side=True)
                        py_https_seen.append(wrapped.getpeercert())
                        data = b""
                        while b"\r\n\r\n" not in data:
                            chunk = wrapped.recv(4096)
                            if not chunk:
                                break
                            data += chunk
                        wrapped.sendall(
                            b"HTTP/1.1 200 OK\r\n"
                            b"Content-Length: 6\r\n"
                            b"\r\n"
                            b"secure")
                        wrapped.close()
                    finally:
                        py_https_listener.close()

                py_https_thread = threading.Thread(
                    target=mtls_https_python_server)
                py_https_thread.start()
                https_mtls_client = run(
                    "import std;\n"
                    "let r = std.http.get("
                    f'"https://localhost:{py_https_port}/secure", '
                    '{"tls": {'
                    f'"ca_file": "{ca_cert}", '
                    f'"cert_file": "{mtls_client_cert}", '
                    f'"key_file": "{mtls_client_key}"'
                    "}});\n"
                    'println(r["status"]);\n'
                    'println(bytes_to_string(r["body"]));\n'
                )
                py_https_thread.join(timeout=5)
                assert https_mtls_client.returncode == 0, (
                    https_mtls_client.stderr)
                assert "200" in https_mtls_client.stdout, (
                    https_mtls_client.stdout)
                assert "secure" in https_mtls_client.stdout, (
                    https_mtls_client.stdout)
                assert py_https_seen, "python https server saw no client cert"
                subject = dict(entry[0] for entry in py_https_seen[0]["subject"])
                assert subject.get("commonName") == "skein-client", subject

                serve_mtls_probe = socket.socket()
                serve_mtls_probe.bind(("127.0.0.1", 0))
                serve_mtls_port = serve_mtls_probe.getsockname()[1]
                serve_mtls_probe.close()
                serve_mtls_proc = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                serve_mtls_client = None
                try:
                    serve_mtls_proc.stdin.write(
                        "import std;\n"
                        f'let s = std.http.listen_tls("127.0.0.1", '
                        f'{serve_mtls_port}, "{mtls_server_cert}", '
                        f'"{mtls_server_key}", 0, {{"client_ca_file": '
                        f'"{ca_cert}", "client_auth": "required"}});\n'
                        "let n = std.http.serve(s, func(req) {\n"
                        '  return {"status": 200, "body": "http-mtls:" + '
                        'req["path"]};\n'
                        "}, 1);\n"
                        'println("served " + str(n));\n'
                        "std.http.close(s);\n"
                    )
                    serve_mtls_proc.stdin.close()
                    deadline = time.time() + 5
                    while time.time() < deadline:
                        try:
                            serve_mtls_client = socket.create_connection(
                                ("127.0.0.1", serve_mtls_port), timeout=0.2)
                            break
                        except OSError:
                            time.sleep(0.05)
                    assert serve_mtls_client is not None, (
                        "http serve mtls listener did not start")
                    import ssl as ssl_module
                    context = ssl_module.SSLContext(
                        ssl_module.PROTOCOL_TLS_CLIENT)
                    context.load_verify_locations(ca_cert)
                    context.load_cert_chain(
                        mtls_client_cert, mtls_client_key)
                    wrapped = context.wrap_socket(
                        serve_mtls_client, server_hostname="localhost")
                    wrapped.sendall(
                        b"GET /secure HTTP/1.1\r\n"
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
                    assert "http-mtls:/secure" in text, text
                    wrapped.close()
                    serve_mtls_proc.wait(timeout=10)
                    assert "served 1" in serve_mtls_proc.stdout.read()
                finally:
                    if serve_mtls_client:
                        serve_mtls_client.close()
                    if serve_mtls_proc.poll() is None:
                        serve_mtls_proc.terminate()
                        serve_mtls_proc.wait(timeout=5)

                h2_mtls_probe = socket.socket()
                h2_mtls_probe.bind(("127.0.0.1", 0))
                h2_mtls_port = h2_mtls_probe.getsockname()[1]
                h2_mtls_probe.close()
                h2_mtls_server = subprocess.Popen(
                    [BIN],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                )
                try:
                    h2_mtls_server.stdin.write(
                        "import std;\n"
                        f'let s = std.http2.listen_tls("127.0.0.1", '
                        f'{h2_mtls_port}, "{mtls_server_cert}", '
                        f'"{mtls_server_key}", 0, {{"client_ca_file": '
                        f'"{ca_cert}", "client_auth": "required"}});\n'
                        "let n = std.http2.serve(s, func(req) {\n"
                        '  return {"status": 200, "body": "h2-mtls:" + '
                        'req["path"]};\n'
                        "}, 1);\n"
                        'println("served " + str(n));\n'
                        "std.http2.close(s);\n"
                    )
                    h2_mtls_server.stdin.close()
                    time.sleep(0.5)
                    h2_mtls_client = run(
                        "import std;\n"
                        f'let a = std.http2.get('
                        f'"https://localhost:{h2_mtls_port}/secure", '
                        '{"tls": {'
                        f'"ca_file": "{ca_cert}", '
                        f'"cert_file": "{mtls_client_cert}", '
                        f'"key_file": "{mtls_client_key}"'
                        "}});\n"
                        'println(str(a["status"]) + " " + a["protocol"]);\n'
                        'println(bytes_to_string(a["body"]));\n',
                        timeout=15,
                    )
                    assert h2_mtls_client.returncode == 0, (
                        h2_mtls_client.stderr)
                    assert "200 h2" in h2_mtls_client.stdout, (
                        h2_mtls_client.stdout)
                    assert "h2-mtls:/secure" in h2_mtls_client.stdout, (
                        h2_mtls_client.stdout)
                    h2_mtls_server.wait(timeout=10)
                    assert "served 1" in h2_mtls_server.stdout.read()
                finally:
                    if h2_mtls_server.poll() is None:
                        h2_mtls_server.terminate()
                        h2_mtls_server.wait(timeout=5)

                import sys as sys_module
                cluster_cert = os.path.join(rotation_dir, "cluster_cert.pem")
                cluster_key = os.path.join(rotation_dir, "cluster_key.pem")
                cluster_worker = os.path.join(
                    rotation_dir, "cluster_worker.skein")
                with open(cluster_worker, "w", encoding="utf-8") as handle:
                    handle.write(
                        "import std;\n"
                        "func handle(req) {\n"
                        '  return {"status": 200, "body": "cluster:" + '
                        'req["path"]};\n'
                        "}\n"
                    )
                cluster_worker_path = cluster_worker.replace("\\", "\\\\")

                def reset_cluster_cert():
                    shutil.copyfile(rot_cert_a, cluster_cert)
                    shutil.copyfile(rot_key_a, cluster_key)
                    os.utime(cluster_cert, None)
                    os.utime(cluster_key, None)

                def cluster_fetch_der(port):
                    import ssl as ssl_module
                    raw = socket.create_connection(
                        ("127.0.0.1", port), timeout=5)
                    context = ssl_module.SSLContext(
                        ssl_module.PROTOCOL_TLS_CLIENT)
                    context.check_hostname = False
                    context.verify_mode = ssl_module.CERT_NONE
                    wrapped = context.wrap_socket(
                        raw, server_hostname="localhost")
                    der = wrapped.getpeercert(binary_form=True)
                    wrapped.sendall(
                        b"GET /cluster HTTP/1.1\r\n"
                        b"Host: localhost\r\n"
                        b"Connection: close\r\n\r\n")
                    response = b""
                    while True:
                        chunk = wrapped.recv(4096)
                        if not chunk:
                            break
                        response += chunk
                    wrapped.close()
                    assert b"cluster:/cluster" in response, response
                    return der

                def check_cluster_rotation(kind, strategy):
                    reset_cluster_cert()
                    server_script = os.path.join(
                        rotation_dir, f"{kind}_{strategy}_server.skein")
                    options = '{"max_requests": 2'
                    if strategy:
                        options += f', "strategy": "{strategy}"'
                    options += "}"
                    if kind == "threads":
                        serve = (
                            "let ts = std.http.serve_threads(s, 2, "
                            f"\"{cluster_worker_path}\", {options});\n"
                            'println(s["port"]);\n'
                            "std.io.flush(std.io.stdout);\n"
                            'println("served=" + str(ts.join(20000)));\n'
                        )
                    else:
                        serve = (
                            "let ps = std.http.serve_processes(s, 2, "
                            f"\"{cluster_worker_path}\", {options});\n"
                            'println(s["port"]);\n'
                            "std.io.flush(std.io.stdout);\n"
                            'println("served=" + str(ps.join(20000)));\n'
                        )
                    with open(server_script, "w", encoding="utf-8") as handle:
                        handle.write(
                            "import std;\n"
                            "let s = std.http.listen_tls("
                            f"\"127.0.0.1\", 0, \"{cluster_cert}\", "
                            f"\"{cluster_key}\");\n" + serve
                        )
                    proc = subprocess.Popen(
                        [BIN, server_script],
                        cwd=rotation_dir,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        encoding="utf-8",
                    )
                    try:
                        port_line = proc.stdout.readline().strip()
                        assert port_line.isdigit(), (
                            kind, strategy, port_line, proc.stderr.read())
                        assert cluster_fetch_der(int(port_line)) == der_a
                        time.sleep(1.1)
                        shutil.copyfile(rot_cert_b, cluster_cert)
                        shutil.copyfile(rot_key_b, cluster_key)
                        os.utime(cluster_cert, None)
                        os.utime(cluster_key, None)
                        assert cluster_fetch_der(int(port_line)) == der_b
                        proc.wait(timeout=20)
                        out = proc.stdout.read()
                        err = proc.stderr.read()
                        assert proc.returncode == 0, (kind, strategy, err)
                        assert "served=2" in out, (kind, strategy, out, err)
                    finally:
                        if proc.poll() is None:
                            proc.kill()
                            proc.wait()

                check_cluster_rotation("threads", "dispatch")
                check_cluster_rotation("workers", "dispatch")
                if sys_module.platform.startswith("linux"):
                    check_cluster_rotation("threads", "reuse_port")
                    check_cluster_rotation("workers", "reuse_port")

                if sys_module.platform.startswith("linux"):
                    rp_dir = os.path.join(tls_tmp, "reuse_tls")
                    os.makedirs(rp_dir, exist_ok=True)
                    rp_worker = os.path.join(rp_dir, "rp_worker.skein")
                    with open(rp_worker, "w", encoding="utf-8") as handle:
                        handle.write(
                            "import std;\n"
                            "func handle(req) {\n"
                            '  return {"status": 200, "headers": '
                            '{"X-Worker": str(req["worker_id"])}, '
                            '"body": "rp:" + req["path"]};\n'
                            "}\n"
                        )
                    rp_worker_path = rp_worker.replace("\\", "\\\\")
                    rp_requests = 2 if http2_enabled else 1
                    rp_server = os.path.join(rp_dir, "rp_server.skein")
                    with open(rp_server, "w", encoding="utf-8") as handle:
                        handle.write(
                            "import std;\n"
                            f'let s = std.http.listen_tls("127.0.0.1", 0, '
                            f'"{tls_cert}", "{tls_key}");\n'
                            'println(s["port"]);\n'
                            "std.io.flush(std.io.stdout);\n"
                            f'let ps = std.http.serve_processes(s, 2, '
                            f'"{rp_worker_path}", {{"strategy": '
                            f'"reuse_port", "max_requests": {rp_requests}}});\n'
                            'println("info=" + ps.strategy + ":" + '
                            'str(ps.capabilities["reuse_port"]) + ":" + '
                            'str(ps.capabilities["http2"]));\n'
                            "std.io.flush(std.io.stdout);\n"
                            "let served = ps.join(30000);\n"
                            'println("served=" + str(served));\n'
                        )
                    rp_proc = subprocess.Popen(
                        [BIN, rp_server],
                        cwd=rp_dir,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        encoding="utf-8",
                    )
                    try:
                        rp_port_line = rp_proc.stdout.readline().strip()
                        assert rp_port_line.isdigit(), (
                            rp_port_line, rp_proc.stderr.read())
                        rp_info = rp_proc.stdout.readline().strip()
                        assert "info=reuse_port:true:" in rp_info, rp_info
                        rp_port = int(rp_port_line)
                        h1 = run(
                            "import std;\n"
                            f'let r = std.http.get('
                            f'"https://127.0.0.1:{rp_port}/h1", '
                            '{"tls": {"verify": false}});\n'
                            'println(r["status"]);\n'
                            'println(bytes_to_string(r["body"]));\n'
                        )
                        assert h1.returncode == 0, h1.stderr
                        assert "200" in h1.stdout, h1.stdout
                        assert "rp:/h1" in h1.stdout, h1.stdout
                        if http2_enabled:
                            h2 = run(
                                "import std;\n"
                                f'let a = std.http2.get('
                                f'"https://127.0.0.1:{rp_port}/h2", '
                                '{"tls": {"verify": false}});\n'
                                'println(str(a["status"]) + " " + '
                                'a["protocol"]);\n'
                                'println(bytes_to_string(a["body"]));\n',
                                timeout=15,
                            )
                            assert h2.returncode == 0, h2.stderr
                            assert "200 h2" in h2.stdout, h2.stdout
                            assert "rp:/h2" in h2.stdout, h2.stdout
                        rp_proc.wait(timeout=30)
                        assert rp_proc.returncode == 0, rp_proc.stderr.read()
                        rp_out = rp_proc.stdout.read()
                        assert f"served={rp_requests}" in rp_out, rp_out
                    finally:
                        if rp_proc.poll() is None:
                            rp_proc.terminate()
                            rp_proc.wait(timeout=5)

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
                'println("served " + str(n));\n'
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
                'println("served " + str(n));\n'
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
                'println(bytes_to_string(a["body"]));\n'
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
            'println(str(a["status"]) + " " + str(a["upgraded"]));\n'
            'std.io.write_all(a["socket"], "one");\n'
            'println(bytes_to_string(std.io.read(a["socket"], 100)));\n'
            'std.io.close(a["socket"]);\n'
            f'let b = await std.http.get_task('
            f'"http://127.0.0.1:{upgrade_server_port}/", '
            '{"upgrade": true, "headers": {"Upgrade": "test"}});\n'
            'println(str(b["status"]) + " " + str(b["upgraded"]));\n'
            'std.io.write_all(b["socket"], "two");\n'
            'println(bytes_to_string(std.io.read(b["socket"], 100)));\n'
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
            'println(m["data"]);\n'
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
                'println("got:" + m["data"]);\n'
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
                'println(bytes_to_string(a["body"]));\n'
                f'let b = std.http.post('
                f'"http://127.0.0.1:{redirect_port}/post303", "data", '
                '{"follow_redirects": true});\n'
                'println(bytes_to_string(b["body"]));\n'
                f'let c = std.http.post('
                f'"http://127.0.0.1:{redirect_port}/post307", "payload", '
                '{"follow_redirects": true});\n'
                'println(bytes_to_string(c["body"]));\n'
                f'let d = await std.http.get_task('
                f'"http://127.0.0.1:{redirect_port}/a", '
                '{"follow_redirects": true});\n'
                'println(bytes_to_string(d["body"]));\n'
                f'let e = std.http.get('
                f'"http://127.0.0.1:{redirect_port}/dir/page", '
                '{"follow_redirects": true});\n'
                'println(bytes_to_string(e["body"]));\n',
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
                'println("count=" + str(len(cs)));\n'
                'println("check1=" + bytes_to_string(std.http.get('
                f'"http://127.0.0.1:{cookie_port}/check", '
                '{"cookie_jar": jar})["body"]));\n'
                'println("admin=" + bytes_to_string(std.http.get('
                f'"http://127.0.0.1:{cookie_port}/admin/check", '
                '{"cookie_jar": jar})["body"]));\n'
                f'let deleted = std.http.get('
                f'"http://127.0.0.1:{cookie_port}/delete", '
                '{"cookie_jar": jar});\n'
                'println("check2=" + bytes_to_string(std.http.get('
                f'"http://127.0.0.1:{cookie_port}/check", '
                '{"cookie_jar": jar})["body"]));\n'
                "let jar2 = {};\n"
                f'let ar = await std.http.get_task('
                f'"http://127.0.0.1:{cookie_port}/set", '
                '{"cookie_jar": jar2});\n'
                'println("acount=" + str(len(std.http.cookies(ar))));\n'
                f'let ac = await std.http.get_task('
                f'"http://127.0.0.1:{cookie_port}/check", '
                '{"cookie_jar": jar2});\n'
                'println("acheck=" + bytes_to_string(ac["body"]));\n',
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
                    'println(bytes_to_string(a["body"]) + " " + '
                    'str(a["decompressed"]));\n'
                    f'let b = std.http.get('
                    f'"http://127.0.0.1:{compression_port}/deflate", '
                    '{"decompress": true});\n'
                    'println(bytes_to_string(b["body"]));\n'
                    f'let c = std.http.get('
                    f'"http://127.0.0.1:{compression_port}/rawdeflate", '
                    '{"decompress": true});\n'
                    'println(bytes_to_string(c["body"]));\n'
                    f'let d = std.http.get('
                    f'"http://127.0.0.1:{compression_port}/br", '
                    '{"decompress": true});\n'
                    'println(bytes_to_string(d["body"]));\n'
                    f'let e = await std.http.get_task('
                    f'"http://127.0.0.1:{compression_port}/gzip", '
                    '{"decompress": true});\n'
                    'println(bytes_to_string(e["body"]));\n'
                    f'let f = std.http.get('
                    f'"http://127.0.0.1:{compression_port}/ae", '
                    '{"decompress": true});\n'
                    'println("ae=" + bytes_to_string(f["body"]));\n',
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

        limit_header_server = socket.socket()
        limit_header_server.setsockopt(
            socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        limit_header_server.bind(("127.0.0.1", 0))
        limit_header_server.listen(1)
        limit_header_server.settimeout(5)
        limit_header_port = limit_header_server.getsockname()[1]

        def limit_header_server_fn():
            try:
                client, _ = limit_header_server.accept()
                client.recv(4096)
                client.sendall(
                    b"HTTP/1.1 200 OK\r\nX-Big: " + b"a" * 2000 +
                    b"\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
                client.close()
            finally:
                limit_header_server.close()

        limit_header_thread = threading.Thread(target=limit_header_server_fn)
        limit_header_thread.start()
        limit_header_client = run(
            "import std;\n"
            f'let a = std.http.get("http://127.0.0.1:{limit_header_port}/", '
            '{"max_header_bytes": 512});\n'
        )
        limit_header_thread.join(timeout=5)
        assert "header bytes exceed limit" in limit_header_client.stderr, (
            limit_header_client.stderr)

        limit_body_probe = socket.socket()
        limit_body_probe.bind(("127.0.0.1", 0))
        limit_body_port = limit_body_probe.getsockname()[1]
        limit_body_probe.close()
        limit_body_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        try:
            limit_body_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", {limit_body_port});\n'
                's["max_body_bytes"] = 10;\n'
                "let req = std.http.accept(s, 5000);\n"
                'std.http.respond(req, 200, {}, "ok");\n'
                "std.http.close(s);\n"
            )
            limit_body_proc.stdin.close()
            deadline = time.time() + 5
            limit_body_client = None
            while time.time() < deadline:
                try:
                    limit_body_client = socket.create_connection(
                        ("127.0.0.1", limit_body_port), timeout=0.2)
                    break
                except OSError:
                    time.sleep(0.05)
            assert limit_body_client is not None, (
                "http body limit server did not listen")
            limit_body_client.sendall(
                b"POST / HTTP/1.1\r\nHost: x\r\n"
                b"Content-Length: 100\r\n\r\n" + b"a" * 100)
            response = b""
            while True:
                chunk = limit_body_client.recv(4096)
                if not chunk:
                    break
                response += chunk
            assert response == b"", response
            limit_body_proc.wait(timeout=10)
            limit_body_err = limit_body_proc.stderr.read()
            assert "body exceeds limit" in limit_body_err, limit_body_err
        finally:
            if limit_body_client:
                limit_body_client.close()
            if limit_body_proc.poll() is None:
                limit_body_proc.terminate()
                limit_body_proc.wait(timeout=5)

        maxconn_probe = socket.socket()
        maxconn_probe.bind(("127.0.0.1", 0))
        maxconn_port = maxconn_probe.getsockname()[1]
        maxconn_probe.close()
        maxconn_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        try:
            maxconn_proc.stdin.write(
                "import std;\n"
                "async func handler(req) {\n"
                "  await std.task.sleep(300);\n"
                '  return {"status": 200, "body": "ok"};\n'
                "}\n"
                f'let s = std.http.listen("127.0.0.1", {maxconn_port});\n'
                's["max_connections"] = 1;\n'
                "let n = std.http.serve(s, handler, 2);\n"
                'println("served " + str(n));\n'
                "std.http.close(s);\n"
            )
            maxconn_proc.stdin.close()
            deadline = time.time() + 5
            ready = False
            while time.time() < deadline:
                try:
                    probe = socket.create_connection(
                        ("127.0.0.1", maxconn_port), timeout=0.2)
                    probe.close()
                    ready = True
                    break
                except OSError:
                    time.sleep(0.05)
            assert ready, "http max_connections server did not listen"
            client_source = (
                "import std;\n"
                f'let a = std.http.get('
                f'"http://127.0.0.1:{maxconn_port}/a");\n'
                'println(bytes_to_string(a["body"]));\n'
            )
            started = time.time()
            mc1 = subprocess.Popen(
                [BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, encoding="utf-8")
            time.sleep(0.03)
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
            assert "ok" in out1, out1
            assert "ok" in out2, out2
            assert elapsed >= 0.50, (
                f"http max_connections was not enforced: {elapsed:.3f}s")
            maxconn_proc.wait(timeout=10)
            maxconn_output = maxconn_proc.stdout.read()
            assert "served 2" in maxconn_output, maxconn_output
        finally:
            if maxconn_proc.poll() is None:
                maxconn_proc.terminate()
                maxconn_proc.wait(timeout=5)

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
                '  println("rb=" + bytes_to_string(chunk));\n'
                "};\n"
                "let n = std.http.serve(s, func(req) {\n"
                '  return {"status": 200, "body": "handler_body=" + '
                'bytes_to_string(req["body"])};\n'
                "}, 1);\n"
                'println("served " + str(n));\n'
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
                'println(bytes_to_string(a["body"]));\n',
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

        pull1_probe = socket.socket()
        pull1_probe.bind(("127.0.0.1", 0))
        pull1_port = pull1_probe.getsockname()[1]
        pull1_probe.close()
        pull1_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        try:
            pull1_proc.stdin.write(
                "import std;\n"
                f'let s = std.http.listen("127.0.0.1", {pull1_port});\n'
                's["stream_request_body"] = true;\n'
                "let n = std.http.serve(s, func(req) {\n"
                '  let out = "";\n'
                '  let chunk = std.http.read_body(req);\n'
                "  while (chunk != null) {\n"
                "    out = out + bytes_to_string(chunk);\n"
                "    chunk = std.http.read_body(req);\n"
                "  }\n"
                '  return {"status": 200, "body": "pull=" + out + '
                '    "|body=" + bytes_to_string(req["body"])};\n'
                "}, 2);\n"
                'println("served " + str(n));\n'
                "std.http.close(s);\n"
            )
            pull1_proc.stdin.close()
            deadline = time.time() + 5
            ready = False
            while time.time() < deadline:
                try:
                    probe = socket.create_connection(
                        ("127.0.0.1", pull1_port), timeout=0.2)
                    probe.close()
                    ready = True
                    break
                except OSError:
                    time.sleep(0.05)
            assert ready, "http1 pull body server did not listen"
            pull1_client = run(
                "import std;\n"
                "func parts() {\n"
                '  yield "hello";\n'
                '  yield " ";\n'
                '  yield "world";\n'
                "}\n"
                f'let a = std.http.post('
                f'"http://127.0.0.1:{pull1_port}/a", '
                '{"body_stream": parts()});\n'
                'println(bytes_to_string(a["body"]));\n'
                f'let b = std.http.get('
                f'"http://127.0.0.1:{pull1_port}/b");\n'
                'println(bytes_to_string(b["body"]));\n',
                timeout=15,
            )
            assert pull1_client.returncode == 0, pull1_client.stderr
            assert "pull=hello world|body=" in pull1_client.stdout, (
                pull1_client.stdout)
            assert "pull=|body=" in pull1_client.stdout, (
                pull1_client.stdout)
            pull1_proc.wait(timeout=10)
            pull1_output = pull1_proc.stdout.read()
            assert "served 2" in pull1_output, pull1_output
        finally:
            if pull1_proc.poll() is None:
                pull1_proc.terminate()
                pull1_proc.wait(timeout=5)

        pull2_probe = socket.socket()
        pull2_probe.bind(("127.0.0.1", 0))
        pull2_port = pull2_probe.getsockname()[1]
        pull2_probe.close()
        pull2_proc = subprocess.Popen(
            [BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        try:
            pull2_proc.stdin.write(
                "import std;\n"
                f'let s = std.http2.listen("127.0.0.1", {pull2_port});\n'
                's["stream_request_body"] = true;\n'
                "let n = std.http2.serve(s, func(req) {\n"
                '  let out = "";\n'
                '  let chunk = std.http.read_body(req);\n'
                "  while (chunk != null) {\n"
                "    out = out + bytes_to_string(chunk);\n"
                "    chunk = std.http.read_body(req);\n"
                "  }\n"
                '  return {"status": 200, "body": "pull=" + out + '
                '    "|body=" + bytes_to_string(req["body"])};\n'
                "}, 2);\n"
                'println("served " + str(n));\n'
                "std.http2.close(s);\n"
            )
            pull2_proc.stdin.close()
            deadline = time.time() + 5
            ready = False
            while time.time() < deadline:
                try:
                    probe = socket.create_connection(
                        ("127.0.0.1", pull2_port), timeout=0.2)
                    probe.close()
                    ready = True
                    break
                except OSError:
                    time.sleep(0.05)
            assert ready, "http2 pull body server did not listen"
            pull2_client = run(
                "import std;\n"
                "func parts() {\n"
                '  yield "hello";\n'
                '  yield " ";\n'
                '  yield "world";\n'
                "}\n"
                f'let a = std.http2.post('
                f'"http://127.0.0.1:{pull2_port}/a", '
                '{"body_stream": parts()});\n'
                'println(bytes_to_string(a["body"]));\n'
                f'let b = std.http2.get('
                f'"http://127.0.0.1:{pull2_port}/b");\n'
                'println(bytes_to_string(b["body"]));\n',
                timeout=15,
            )
            assert pull2_client.returncode == 0, pull2_client.stderr
            assert "pull=hello world|body=" in pull2_client.stdout, (
                pull2_client.stdout)
            assert "pull=|body=" in pull2_client.stdout, (
                pull2_client.stdout)
            pull2_proc.wait(timeout=10)
            pull2_output = pull2_proc.stdout.read()
            assert "served 2" in pull2_output, pull2_output
        finally:
            if pull2_proc.poll() is None:
                pull2_proc.terminate()
                pull2_proc.wait(timeout=5)

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
            f'let t = std.net.tcp_connect_task('
            f'"127.0.0.1", {async_port}, 1000, "ipv4");\n'
            "let s = std.task.wait(t);\n"
            'let st = std.net.send_task(s, "hello", 1000);\n'
            "std.task.wait(st)\n"
            "let rt = std.net.recv_task(s, 100, 1000);\n"
            "let data = std.task.wait(rt);\n"
            "println(bytes_to_string(data));\n"
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
            assert client is not None, "skein tcp_listen did not accept"
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
                f'let lt = std.net.tcp_listen_task('
                f'"127.0.0.1", {async_listen_port}, 1000, "ipv4");\n'
                "let listener = std.task.wait(lt);\n"
                "let at = std.net.accept_task(listener, 1000);\n"
                "let client = std.task.wait(at);\n"
                "let rt = std.net.recv_task(client, 100, 1000);\n"
                "let data = std.task.wait(rt);\n"
                "let st = std.net.send_task(client, bytes_concat("
                'bytes_from_string("async:"), data), 1000);\n'
                "std.task.wait(st)\n"
                "let ht = std.net.shutdown_task(client, 1000);\n"
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
            assert client is not None, "skein async listener did not accept"
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

        if sys.platform.startswith("linux"):
            check(
                "std net tcp_listen_task reuse_port",
                "import std;\n"
                'let a = std.task.wait(std.net.tcp_listen_task('
                '"127.0.0.1", 0, 1000, "any", {"reuse_port": true}));\n'
                'let port = std.net.local_addr(a)["port"];\n'
                'let b = std.task.wait(std.net.tcp_listen_task('
                '"127.0.0.1", port, 1000, "any", '
                '{"reuse_port": true}));\n'
                'print("same=" + str(std.net.local_addr(a)["port"] == '
                'std.net.local_addr(b)["port"]));\n'
                "std.net.close(a);\n"
                "std.net.close(b);\n",
                stdout="same=true",
            )

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
            "let a = std.net.accept_task(listener, 50);\n"
            "std.task.wait(a);\n"
            "std.net.close(listener);\n",
            stderr="accept_async: timed out",
        )
        check(
            "std net async cancel",
            "import std;\n"
            'let listener = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
            "let a = std.net.accept_task(listener, 1000);\n"
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
            "let a = std.net.accept_task(listener, 1000);\n"
            "std.net.close(listener);\n"
            "std.task.wait(a);\n",
            stderr="accept_async: listener closed",
        )
        check(
            "std net async pending shutdown",
            "import std;\n"
            'let listener = std.net.tcp_listen("127.0.0.1", 0, 100);\n'
            "let pending = std.net.accept_task(listener, 10000);\n",
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
