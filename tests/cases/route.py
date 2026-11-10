#!/usr/bin/env python3
"""std.route: explicit registration, trie dispatch and async middleware."""
from harness import *  # noqa: F401,F403


def run_cases():
    check("route basic",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/users/:id", func(req) { '
          'return {"status": 200, "body": req["params"]["id"]}; });\n'
          'app.get("/users/me", func(req) { '
          'return {"status": 200, "body": "me"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/users/42"})["body"]);\n'
          'print(app.dispatch({"method": "GET", "path": "/users/me"})["body"]);\n'
          'print(app.dispatch({"method": "POST", "path": "/users/42"})["status"]);\n'
          'print(app.dispatch({"method": "GET", "path": "/nope"})["status"]);\n',
          stdout_all=["42", "me", "405", "404"])

    check("route static params empty",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/static", func(req) { '
          'return {"status": 200, "body": str(len(req["params"]))}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/static"})["body"]);\n',
          stdout="0")

    check("route root and trailing slash",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/", func(req) { return {"status": 200, "body": "root"}; });\n'
          'app.get("/a/b", func(req) { return {"status": 200, "body": "ab"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/"})["body"]);\n'
          'print(app.dispatch({"method": "GET", "path": ""})["body"]);\n'
          'print(app.dispatch({"method": "GET", "path": "/a/b/"})["body"]);\n',
          stdout_all=["root", "ab"])

    check("route wildcard",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/files/*path", func(req) { '
          'return {"status": 200, "body": "[" + req["params"]["path"] + "]"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/files/a/b.txt"})["body"]);\n'
          'print(app.dispatch({"method": "GET", "path": "/files"})["body"]);\n'
          'print(app.dispatch({"method": "GET", "path": "/files/"})["body"]);\n',
          stdout_all=["[a/b.txt]", "[]"])

    check("route wildcard method",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.add("*", "/any", func(req) { '
          'return {"status": 200, "body": req["method"]}; });\n'
          'print(app.dispatch({"method": "PATCH", "path": "/any"})["body"]);\n',
          stdout="PATCH")

    check("route method specific beats wildcard method",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.add("*", "/any", func(req) { '
          'return {"status": 200, "body": "any"}; });\n'
          'app.get("/any", func(req) { '
          'return {"status": 200, "body": "get"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/any"})["body"]);\n'
          'print(app.dispatch({"method": "POST", "path": "/any"})["body"]);\n',
          stdout_all=["get", "any"])

    check("route middleware",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/private", func(req) { '
          'return {"status": 200, "body": "ok"}; });\n'
          'app.use(func(req) { '
          'if (req["path"] == "/private") { '
          'return {"status": 401, "body": "no"}; } return null; });\n'
          'print(app.dispatch({"method": "GET", "path": "/private"})["status"]);\n'
          'print(app.dispatch({"method": "GET", "path": "/public"})["status"]);\n',
          stdout_all=["401", "404"])

    check("route duplicate",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/x", func(req) { return null; });\n'
          'app.get("/x", func(req) { return null; });\n',
          stderr="duplicate route GET /x")

    check("route exact method fallthrough to param",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.post("/users/me", func(req) { '
          'return {"status": 200, "body": "post"}; });\n'
          'app.get("/users/:id", func(req) { '
          'return {"status": 200, "body": "get:" + req["params"]["id"]}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/users/me"})["body"]);\n',
          stdout="get:me")

    check("route exact method mismatch",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.post("/only", func(req) { return {"status": 200, "body": "p"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/only"})["status"]);\n',
          stdout="405")

    check("route wildcard zero beats terminal method mismatch",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.post("/files", func(req) { '
          'return {"status": 200, "body": "post"}; });\n'
          'app.get("/files/*path", func(req) { '
          'return {"status": 200, "body": "wild:[" + req["params"]["path"] + "]"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/files"})["body"]);\n',
          stdout="wild:[]")

    check("route static over param",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/a/:x", func(req) { '
          'return {"status": 200, "body": "param"}; });\n'
          'app.get("/a/b", func(req) { '
          'return {"status": 200, "body": "static"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/a/b"})["body"]);\n',
          stdout="static")

    check("route lexicographic priority",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/a/:x/c/d", func(req) { '
          'return {"status": 200, "body": "first"}; });\n'
          'app.get("/a/b/:y/:z", func(req) { '
          'return {"status": 200, "body": "second"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/a/b/c/d"})["body"]);\n',
          stdout="second")

    check("route param over wildcard",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/w/*rest", func(req) { '
          'return {"status": 200, "body": "wild"}; });\n'
          'app.get("/w/:id", func(req) { '
          'return {"status": 200, "body": "param"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/w/1"})["body"]);\n',
          stdout="param")

    check("route same path different methods",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/same", func(req) { return {"status": 200, "body": "g"}; });\n'
          'app.post("/same", func(req) { return {"status": 200, "body": "p"}; });\n'
          'print(app.dispatch({"method": "GET", "path": "/same"})["body"]);\n'
          'print(app.dispatch({"method": "POST", "path": "/same"})["body"]);\n',
          stdout_all=["g", "p"])

    check("route copy shares state",
          'import std;\nimport std.route;\n'
          'let a = route.new();\n'
          'let b = a;\n'
          'b.get("/x", func(req) { '
          'return {"status": 200, "body": "shared"}; });\n'
          'print(a.dispatch({"method": "GET", "path": "/x"})["body"]);\n',
          stdout="shared")

    check("route inspect",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/a", func(req) { return null; });\n'
          'app.post("/b/:id", func(req) { return null; });\n'
          'let items = app.inspect();\n'
          'print(len(items));\n'
          'print(items[0]["method"] + " " + items[0]["pattern"]);\n'
          'print(items[1]["method"] + " " + items[1]["pattern"]);\n',
          stdout_all=["2", "GET /a", "POST /b/:id"])

    check("route task handler",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/x", func(req) {\n'
          '  std.task.sleep(1).wait();\n'
          '  return {"status": 200, "body": "ok"};\n'
          '});\n'
          'let resp = app.dispatch({"method": "GET", "path": "/x"});\n'
          'print(resp["body"]);\n',
          stdout_all=["ok"])

    check("route task middleware continues",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.use(func(req) {\n'
          '  std.task.sleep(1).wait();\n'
          '  print("mw1");\n'
          '  return null;\n'
          '});\n'
          'app.use(func(req) { print("mw2"); return null; });\n'
          'app.get("/x", func(req) { '
          'return {"status": 200, "body": "ok"}; });\n'
          'let resp = app.dispatch({"method": "GET", "path": "/x"});\n'
          'print(resp["body"]);\n',
          stdout_all=["mw1", "mw2", "ok"])

    check("route sync then task middleware",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.use(func(req) { print("sync"); return null; });\n'
          'app.use(func(req) {\n'
          '  std.task.sleep(1).wait();\n'
          '  print("async");\n'
          '  return null;\n'
          '});\n'
          'app.get("/x", func(req) { '
          'return {"status": 200, "body": "ok"}; });\n'
          'let resp = app.dispatch({"method": "GET", "path": "/x"});\n'
          'print(resp["body"]);\n',
          stdout_all=["sync", "async", "ok"])

    check("route task middleware short circuit",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.use(func(req) {\n'
          '  std.task.sleep(1).wait();\n'
          '  return {"status": 401, "body": "no"};\n'
          '});\n'
          'app.get("/x", func(req) { '
          'return {"status": 200, "body": "ok"}; });\n'
          'let resp = app.dispatch({"method": "GET", "path": "/x"});\n'
          'print(resp["status"]);\n'
          'print(resp["body"]);\n',
          stdout_all=["401", "no"])

    check("route task middleware and handler",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.use(func(req) {\n'
          '  std.task.sleep(1).wait();\n'
          '  return null;\n'
          '});\n'
          'app.get("/x", func(req) {\n'
          '  std.task.sleep(1).wait();\n'
          '  return {"status": 200, "body": "both"};\n'
          '});\n'
          'let resp = app.dispatch({"method": "GET", "path": "/x"});\n'
          'print(resp["body"]);\n',
          stdout="both")

    check("route long wildcard path",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/files/*path", func(req) { '
          'return {"status": 200, "body": req["params"]["path"]}; });\n'
          'let path = "/files";\n'
          'for (let i in range(200)) { path = path + "/" + str(i); }\n'
          'let resp = app.dispatch({"method": "GET", "path": path});\n'
          'let expected = "";\n'
          'for (let i in range(200)) {\n'
          '  if (i > 0) { expected = expected + "/"; }\n'
          '  expected = expected + str(i);\n'
          '}\n'
          'print(resp["status"]);\n'
          'print(resp["body"] == expected);\n',
          stdout_all=["200", "true"])

    check("route many routes",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'for (let i in range(1000)) {\n'
          '  app.get("/r" + str(i), func(req) { '
          'return {"status": 200, "body": "ok"}; });\n'
          '}\n'
          'print(app.dispatch({"method": "GET", "path": "/r999"})["body"]);\n'
          'print(len(app.inspect()));\n',
          stdout_all=["ok", "1000"])

    check("route param name conflict",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/u/:id", func(req) { return null; });\n'
          'app.get("/u/:name", func(req) { return null; });\n',
          stderr="conflicting route parameter name")

    check("route wildcard name conflict",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/f/*a", func(req) { return null; });\n'
          'app.get("/f/*b", func(req) { return null; });\n',
          stderr="conflicting route wildcard name")

    check("route wildcard must be last",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/f/*p/x", func(req) { return null; });\n',
          stderr="route wildcard must be the last segment")

    check("route empty param name",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/u/:", func(req) { return null; });\n',
          stderr="route parameter name is required")

    check("route pattern depth limit",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'let pattern = "";\n'
          'for (let i in range(33)) { pattern = pattern + "/s" + str(i); }\n'
          'app.get(pattern, func(req) { return null; });\n',
          stderr="route pattern is too deep")

    check("route non-callable handler",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.get("/x", 1);\n',
          stderr="route handler must be callable")

    check("route non-callable middleware",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.use(1);\n',
          stderr="route middleware must be callable")

    check("route request validation",
          'import std;\nimport std.route;\n'
          'let app = route.new();\n'
          'app.dispatch({"method": "GET"});\n',
          stderr="route.dispatch requires a string path")
