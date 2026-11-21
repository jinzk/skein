#!/usr/bin/env python3
"""std.route: explicit registration, trie dispatch and async middleware.

The cases live in tests/skein/route.skein; this module keeps the existing
ctest module name and shard wiring.  One Python golden stays here as an
independent check of dispatch output.
"""
from harness import check, run_skein


def run_cases():
    run_skein("route.skein")
    check(
        "route golden output",
        "import std;\n"
        "import std.route;\n"
        "let app = route.new();\n"
        'app.get("/users/:id", func(req) { '
        'return {"status": 200, "body": req["params"]["id"]}; });\n'
        'print(app.dispatch({"method": "GET", "path": "/users/42"})["body"]);\n'
        'print(app.dispatch({"method": "POST", "path": "/users/42"})["status"]);\n',
        stdout_all=["42", "405"],
    )
