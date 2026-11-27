#!/usr/bin/env python3
"""Scripted stdio LSP session tests for `skein lsp --stdio`."""

import json
import pathlib
import queue
import subprocess
import tempfile
import threading

import harness


class LspSession:
    def __init__(self, *extra_args):
        self.process = subprocess.Popen(
            [harness.BIN, "lsp", "--stdio", *extra_args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self._queue = queue.Queue()
        self._pending = []
        self._next_id = 1
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_frame(self):
        length = None
        while True:
            line = self.process.stdout.readline()
            if not line:
                return None
            if line in (b"\r\n", b"\n"):
                break
            name, _, value = line.decode("ascii", "replace").partition(":")
            if name.strip().lower() == "content-length":
                length = int(value.strip())
        if length is None:
            raise RuntimeError("LSP response missing Content-Length")
        return json.loads(self.process.stdout.read(length).decode("utf-8"))

    def _read_loop(self):
        try:
            while True:
                message = self._read_frame()
                if message is None:
                    break
                self._queue.put(message)
        except Exception as error:  # surfaced by wait_for
            self._queue.put({"_reader_error": repr(error)})

    def send(self, message):
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
        self.process.stdin.write(header + payload)
        self.process.stdin.flush()

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)

    def request(self, method, params=None):
        request_id = self._next_id
        self._next_id += 1
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        response = self.wait_for(lambda item: item.get("id") == request_id)
        assert "error" not in response, response.get("error")
        return response["result"]

    def wait_for(self, predicate, timeout=15):
        for _ in range(len(self._pending)):
            message = self._pending[0]
            if predicate(message):
                return self._pending.pop(0)
            self._pending.append(self._pending.pop(0))
        while True:
            message = self._queue.get(timeout=timeout)
            assert "_reader_error" not in message, message
            if predicate(message):
                return message
            self._pending.append(message)

    def wait_for_diagnostics(self, uri):
        return self.wait_for(
            lambda item: item.get("method") == "textDocument/publishDiagnostics"
            and item.get("params", {}).get("uri") == uri
        )["params"]["diagnostics"]

    def stop(self):
        if self.process.poll() is None:
            try:
                self.request("shutdown")
            except Exception:
                pass
            self.notify("exit")
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
        stderr = self.process.stderr.read().decode("utf-8", "replace")
        self.process.stderr.close()
        return stderr


def did_change(session, uri, version, text):
    session.notify(
        "textDocument/didChange",
        {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [{"text": text}],
        },
    )


def did_change_edits(session, uri, version, edits):
    changes = []
    for (start, end, text) in edits:
        changes.append({
            "range": {
                "start": {"line": start[0], "character": start[1]},
                "end": {"line": end[0], "character": end[1]},
            },
            "text": text,
        })
    session.notify(
        "textDocument/didChange",
        {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": changes,
        },
    )


def run_workspace_cases():
    """Project index: cross-file definition and module/package completion."""
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        (root / "math").mkdir()
        (root / "math" / "module.skein").write_text(
            "module math {\n"
            "  export add.add;\n"
            "  export vector.sum as total;\n"
            "}\n",
            encoding="utf-8",
        )
        (root / "math" / "add.skein").write_text(
            "package add {\n"
            "  export func add(a, b) { return a + b; }\n"
            "}\n",
            encoding="utf-8",
        )
        (root / "math" / "vector.skein").write_text(
            "package vector {\n"
            "  export func sum(xs) { return 0; }\n"
            "}\n",
            encoding="utf-8",
        )
        main = root / "main.skein"
        uri = main.resolve().as_uri()

        session = LspSession("-I", str(root))
        try:
            result = session.request("initialize", {"capabilities": {}})
            assert result["capabilities"]["workspace"]["workspaceFolders"] is True
            session.notify("initialized", {})

            session.notify(
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "skein",
                        "version": 1,
                        "text": "import math;\nmath.\n",
                    }
                },
            )
            session.wait_for_diagnostics(uri)

            # Module members, including an `as` alias, appear in completion.
            items = session.request(
                "textDocument/completion",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 6},
                },
            )["items"]
            labels = [item["label"] for item in items]
            assert "add" in labels, labels
            assert "total" in labels, labels

            # Top-level module name appears in bare completion.
            bare = pathlib.Path(temporary) / "bare.skein"
            bare_uri = bare.resolve().as_uri()
            session.notify(
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": bare_uri,
                        "languageId": "skein",
                        "version": 1,
                        "text": "mat\n",
                    }
                },
            )
            session.wait_for_diagnostics(bare_uri)
            bare_labels = [
                item["label"]
                for item in session.request(
                    "textDocument/completion",
                    {
                        "textDocument": {"uri": bare_uri},
                        "position": {"line": 0, "character": 3},
                    },
                )["items"]
            ]
            assert "math" in bare_labels, bare_labels

            # Cross-file definition resolves the real declaration, not the
            # same-named `package add` statement.
            did_change(session, uri, 2, "import math;\nmath.add\n")
            session.wait_for_diagnostics(uri)
            definitions = session.request(
                "textDocument/definition",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 7},
                },
            )
            assert len(definitions) == 1, definitions
            target = definitions[0]
            assert target["uri"].endswith("/math/add.skein"), target
            assert target["range"]["start"]["line"] == 1, target

            # Selective imports resolve the alias statically, without running
            # the imported module.
            did_change(session, uri, 3, "import {total as t} from math;\nt\n")
            session.wait_for_diagnostics(uri)
            alias_hover = session.request(
                "textDocument/hover",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 0},
                },
            )
            assert "func t" in alias_hover["contents"]["value"], alias_hover
            alias_definitions = session.request(
                "textDocument/definition",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 0},
                },
            )
            assert len(alias_definitions) == 1, alias_definitions
            assert alias_definitions[0]["uri"].endswith(
                "/math/vector.skein"
            ), alias_definitions

            did_change(
                session, uri, 4, "import {total as t} from math;\n"
            )
            session.wait_for_diagnostics(uri)
            alias_items = session.request(
                "textDocument/completion",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 0},
                },
            )["items"]
            alias = next(
                item for item in alias_items if item["label"] == "t"
            )
            assert alias["detail"] == "selective import", alias

            stderr = session.stop()
            assert "SKEIN_LSP_SIDE_EFFECT" not in stderr, stderr
        finally:
            if session.process.poll() is None:
                session.process.kill()
                session.process.wait(timeout=10)


def run_cases():
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary) / "lsp_input.skein"
        path.write_text("", encoding="utf-8")
        uri = path.resolve().as_uri()

        session = LspSession("--typecheck=strict")
        try:
            result = session.request("initialize", {"capabilities": {}})
            assert result["capabilities"]["publishDiagnosticsProvider"] is True
            assert result["capabilities"]["completionProvider"]["triggerCharacters"] == ["."]
            assert result["capabilities"]["textDocumentSync"]["change"] == 2
            session.notify("initialized", {})

            messy = (
                "import std;\n"
                'println("SKEIN_LSP_SIDE_EFFECT");\n'
                'let xs:list[int]=[1,"a"];\n'
            )
            session.notify(
                "textDocument/didOpen",
                {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "skein",
                        "version": 1,
                        "text": messy,
                    }
                },
            )
            diagnostics = session.wait_for_diagnostics(uri)
            assert diagnostics, "expected a type diagnostic"
            assert diagnostics[0]["code"] in ("PAR0001", "TYP0001", "TYP0002")

            edits = session.request(
                "textDocument/formatting", {"textDocument": {"uri": uri}}
            )
            assert len(edits) == 1, edits
            expected = (
                "import std;\n"
                'println("SKEIN_LSP_SIDE_EFFECT");\n'
                'let xs: list[int] = [1, "a"];\n'
            )
            assert edits[0]["newText"] == expected, repr(edits[0]["newText"])

            completion_source = "import std;\nlet local:int=1;\nstd.i"
            did_change(session, uri, 2, completion_source)
            session.wait_for_diagnostics(uri)
            items = session.request(
                "textDocument/completion",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 5},
                },
            )["items"]
            labels = [item["label"] for item in items]
            assert "io" in labels, labels
            assert "iter" in labels, labels
            assert "local" not in labels, labels

            local_items = session.request(
                "textDocument/completion",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 7},
                },
            )["items"]
            assert "local" in [item["label"] for item in local_items]

            struct_source = (
                "struct Point {\n"
                "  x: float;\n"
                "  func new(x) { return Point { x: x }; }\n"
                "  func norm(self): float { return self.x; }\n"
                "}\n"
                "Point.\n"
            )
            did_change(session, uri, 4, struct_source)
            session.wait_for_diagnostics(uri)
            member_items = session.request(
                "textDocument/completion",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 5, "character": 6},
                },
            )["items"]
            member_labels = [item["label"] for item in member_items]
            assert "new" in member_labels, member_labels
            assert "norm" in member_labels, member_labels
            assert "x" in member_labels, member_labels
            constructor = next(
                item for item in member_items if item["label"] == "new")
            assert constructor["detail"] == "constructor", constructor

            did_change(session, uri, 5, "import std;\nlet local:int=1;\nstd.io\n")
            session.wait_for_diagnostics(uri)
            hover = session.request(
                "textDocument/hover",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 6},
                },
            )
            assert "std.io" in hover["contents"]["value"], hover

            did_change(session, uri, 6, "let x:int=1;\nprint(x);\n")
            session.wait_for_diagnostics(uri)
            symbols = session.request(
                "textDocument/documentSymbol", {"textDocument": {"uri": uri}}
            )
            assert any(item["name"] == "x" for item in symbols), symbols
            definitions = session.request(
                "textDocument/definition",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 7},
                },
            )
            assert definitions, definitions
            assert definitions[0]["range"]["start"]["line"] == 0
            local_hover = session.request(
                "textDocument/hover",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 7},
                },
            )
            assert "let x: int" in local_hover["contents"]["value"], local_hover

            # Incremental edits replace UTF-16 ranges and are debounced.
            did_change(session, uri, 7, "let x:int=1;\nprint(x);\n")
            session.wait_for_diagnostics(uri)
            did_change_edits(
                session,
                uri,
                6,
                [
                    ((0, 4), (0, 5), "value"),
                    ((1, 6), (1, 7), "value"),
                ],
            )
            session.wait_for_diagnostics(uri)
            symbols = session.request(
                "textDocument/documentSymbol", {"textDocument": {"uri": uri}}
            )
            assert any(item["name"] == "value" for item in symbols), symbols

            # `do` block returns surface as warnings; lambda block returns do not.
            did_change(
                session,
                uri,
                7,
                "func f() {\n"
                "  let v = do { return 5; };\n"
                "  return v;\n"
                "}\n"
                "f();\n",
            )
            diagnostics = session.wait_for_diagnostics(uri)
            assert any(
                "return inside a do block" in item["message"]
                for item in diagnostics
            ), diagnostics
            assert any(item["severity"] == 2 for item in diagnostics), diagnostics

            did_change(
                session,
                uri,
                8,
                "func f() {\n"
                "  let g = (x) -> { return x + 1; };\n"
                "  return g(1);\n"
                "}\n"
                "f();\n",
            )
            diagnostics = session.wait_for_diagnostics(uri)
            assert not any(
                "return inside a do block" in item["message"]
                for item in diagnostics
            ), diagnostics

            # Formatting keeps lambda block bodies intact.
            did_change(
                session,
                uri,
                9,
                "let f=(x)->{\n"
                "let y=x+1;\n"
                "y*2;\n"
                "};\n",
            )
            session.wait_for_diagnostics(uri)
            edits = session.request(
                "textDocument/formatting", {"textDocument": {"uri": uri}}
            )
            assert len(edits) == 1, edits
            assert edits[0]["newText"] == (
                "let f = (x) -> {\n"
                "  let y = x + 1;\n"
                "  y * 2;\n"
                "};\n"
            ), repr(edits[0]["newText"])

            # Formatting validation is syntax-only: a selective macro import
            # must not force loading the imported package.
            did_change(
                session,
                uri,
                10,
                "import {twice! as t} from util;\n"
                "func f(){return t!(1);}\n",
            )
            session.wait_for_diagnostics(uri)
            edits = session.request(
                "textDocument/formatting", {"textDocument": {"uri": uri}}
            )
            assert len(edits) == 1, edits
            assert edits[0]["newText"] == (
                "import {twice! as t} from util;\n"
                "func f() { return t!(1); }\n"
            ), repr(edits[0]["newText"])

            # Macro diagnostics point to both the call site and definition.
            did_change(
                session,
                uri,
                11,
                "macro bad(T: type) { let x: #T = 1; return x; }\n"
                "func test(): int { return bad!(1 + 2); }\n",
            )
            diagnostics = session.wait_for_diagnostics(uri)
            macro_error = next(
                item for item in diagnostics
                if item["message"] == "type fragment expects a type name"
            )
            assert macro_error["range"]["start"]["line"] == 1, macro_error
            related = macro_error["relatedInformation"]
            assert len(related) == 1, macro_error
            assert related[0]["location"]["range"]["start"]["line"] == 0, related
            assert "macro bad defined here" in related[0]["message"], related

            # Local macro declarations support completion, signature hover and
            # jump-to-definition from a call site.
            did_change(
                session,
                uri,
                12,
                "macro wrap(T: type, v: expr) { let x: #T = #v; return x; }\n"
                "func use(v) { return wrap!(int, v); }\n"
                "wra\n",
            )
            session.wait_for_diagnostics(uri)
            items = session.request(
                "textDocument/completion",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 3},
                },
            )["items"]
            assert "wrap!" in [item["label"] for item in items], items
            macro_hover = session.request(
                "textDocument/hover",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 22},
                },
            )
            assert "macro wrap(T: type, v: expr)" in (
                macro_hover["contents"]["value"]
            ), macro_hover
            macro_definitions = session.request(
                "textDocument/definition",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 22},
                },
            )
            assert macro_definitions, macro_definitions
            assert macro_definitions[0]["range"]["start"]["line"] == 0, (
                macro_definitions
            )

            # Selective macro imports remain visible without loading the
            # imported package during tooling analysis.
            did_change(
                session,
                uri,
                13,
                "import {twice! as t} from util;\n"
                "t\n",
            )
            session.wait_for_diagnostics(uri)
            items = session.request(
                "textDocument/completion",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 0},
                },
            )["items"]
            assert "t!" in [item["label"] for item in items], items
            alias_hover = session.request(
                "textDocument/hover",
                {
                    "textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 0},
                },
            )
            assert "macro t" in alias_hover["contents"]["value"], alias_hover

            stderr = session.stop()
            assert "SKEIN_LSP_SIDE_EFFECT" not in stderr, stderr
            assert session.process.returncode == 0, session.process.returncode
        finally:
            if session.process.poll() is None:
                session.process.kill()
                session.process.wait(timeout=10)

    run_workspace_cases()
