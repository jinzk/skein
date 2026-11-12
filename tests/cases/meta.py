#!/usr/bin/env python3
"""std.meta type introspection."""
import os
import tempfile

from harness import *  # noqa: F401,F403


def run_cases():
    check(
        "meta struct info",
        'import std;\n'
        'struct P { x: int; y: int? = null; }\n'
        'impl P {\n'
        '  func moved(self, dx: int, dy: int = 0) { return self; }\n'
        '}\n'
        'let info = std.meta.type_info("P");\n'
        'println(info["kind"]);\n'
        'println(info["name"]);\n'
        'println(info["fields"][0]["name"]);\n'
        'println(info["fields"][1]["type"]);\n'
        'println(info["fields"][1]["nullable"]);\n'
        'println(info["fields"][1]["has_default"]);\n'
        'println(info["fields"][0]["readonly"]);\n'
        'println(info["methods"][0]["name"]);\n'
        'println(info["methods"][0]["params"]);\n'
        'println(info["methods"][0]["has_defaults"]);\n',
        stdout_all=[
            "struct", "P", "x", "int?", "true", "true", "false",
            "moved", "[int, int]", "true",
        ],
    )

    check(
        "meta struct from value",
        'import std;\n'
        'struct P { x: int; }\n'
        'println(std.meta.type_info(P { x: 1 })["name"]);\n'
        'println(std.meta.fields(P { x: 1 })[0]["type"]);\n',
        stdout_all=["P", "int"],
    )

    check(
        "meta enum info",
        'import std;\n'
        'enum E { A { n: int = 3; }; B; }\n'
        'let info = std.meta.type_info(E.B);\n'
        'println(info["kind"]);\n'
        'println(info["variants"][0]["name"]);\n'
        'println(info["variants"][0]["fields"][0]["readonly"]);\n'
        'println(info["variants"][0]["fields"][0]["has_default"]);\n'
        'println(std.meta.fields(E.B));\n'
        'println(std.meta.variants("E")[1]["name"]);\n',
        stdout_all=["enum", "A", "true", "true", "[]", "B"],
    )

    check(
        "meta trait info",
        'import std;\n'
        'trait D { func draw(self); }\n'
        'struct P { x: int; }\n'
        'impl D for P { func draw(self) { return self.x; } }\n'
        'println(std.meta.implements(P { x: 1 }, "D"));\n'
        'println(std.meta.implements("P", "D"));\n'
        'println(std.meta.traits("P"));\n'
        'println(std.meta.type_info("D")["kind"]);\n'
        'println(len(std.meta.type_info("D")["methods"][0]["params"]));\n'
        'println(std.meta.fields("D"));\n'
        'println(std.meta.variants("D"));\n',
        stdout_all=["true", "[main.D]", "trait", "0", "[]"],
    )

    check(
        "meta builtin types",
        'import std;\n'
        'println(std.meta.type_info("str")["name"]);\n'
        'println(std.meta.type_info("func")["name"]);\n'
        'println(std.meta.type_info(1)["kind"]);\n'
        'println(std.meta.type_info([1])["name"]);\n'
        'println(std.meta.type_info(sqrt)["name"]);\n'
        'println(std.meta.type_info(null)["name"]);\n'
        'println(std.meta.has_type("list"));\n'
        'println(std.meta.has_type("nope"));\n'
        'println(std.meta.fields("int"));\n'
        'println(std.meta.variants("int"));\n'
        'println(std.meta.methods("int"));\n'
        'println(std.meta.traits("int"));\n',
        stdout_all=[
            "string", "function", "builtin", "list", "builtin", "null",
            "true", "false", "[]",
        ],
    )

    check(
        "meta builtin descriptor fields",
        'import std;\n'
        'let info = std.meta.type_info("int");\n'
        'println(info["module"]);\n'
        'println(info["source"]);\n'
        'println(info["exported"]);\n'
        'println(info["fields"]);\n'
        'println(info["variants"]);\n'
        'println(info["methods"]);\n'
        'println(info["traits"]);\n',
        stdout_all=["true", "[]"],
    )

    check(
        "meta builtin name table",
        'import std;\n'
        'let names = [\n'
        '  "any", "int", "float", "number", "bool", "string", "str",\n'
        '  "char", "byte", "bytes", "list", "dict", "deque", "heap", "set",\n'
        '  "function", "func", "builtin", "callable", "null", "userdata",\n'
        '  "result", "error"\n'
        '];\n'
        'let missing = [];\n'
        'for (let name in names) {\n'
        '  if (!std.meta.has_type(name)) {\n'
        '    missing = std.list.append(missing, name);\n'
        '  }\n'
        '}\n'
        'println(missing);\n',
        stdout="[]",
    )

    check(
        "meta kind tag is not a type name",
        'import std;\n'
        'struct P { x: int; }\n'
        'std.meta.type_info(kind_of(P { x: 1 }));\n',
        stderr="unknown type struct",
    )

    check(
        "meta unknown type",
        'import std;\nstd.meta.type_info("Nope");\n',
        stderr="unknown type Nope",
    )

    check(
        "meta unknown trait",
        'import std;\nstd.meta.implements("int", "Nope");\n',
        stderr="unknown trait Nope",
    )

    check(
        "meta result isolation",
        'import std;\n'
        'struct P { x: int; }\n'
        'let fields = std.meta.fields("P");\n'
        'fields[0]["name"] = "z";\n'
        'println(std.meta.fields("P")[0]["name"]);\n',
        stdout="x",
    )

    check(
        "meta methods sorted",
        'import std;\n'
        'struct P { x: int; }\n'
        'impl P {\n'
        '  func zed(self) { return 1; }\n'
        '  func alpha(self) { return 2; }\n'
        '}\n'
        'let methods = std.meta.methods("P");\n'
        'println(methods[0]["name"]);\n'
        'println(methods[1]["name"]);\n',
        stdout_all=["alpha", "zed"],
    )

    check(
        "meta import submodule",
        'import std.meta;\n'
        'println(meta.type_info("int")["name"]);\n',
        stdout="int",
    )

    with tempfile.TemporaryDirectory() as tmp:
        def write(rel, text):
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return path

        write("aa.skein",
              "package aa {\n"
              "  import std;\n"
              "  export struct Pub { x: int; }\n"
              "  struct Hidden { y: int; }\n"
              "  export func make_hidden() { return Hidden { y: 2 }; }\n"
              "  export func local_meta() {\n"
              '    return std.meta.type_info("Hidden");\n'
              "  }\n"
              "}\n")
        main = write("main_vis.skein",
                     "import std;\n"
                     "import aa;\n"
                     'println(std.meta.type_info("Pub")["name"]);\n'
                     'println(std.meta.type_info("aa.Pub")["name"]);\n'
                     'println(std.meta.has_type("aa.Hidden"));\n'
                     'println(aa.local_meta()["name"]);\n'
                     'std.meta.type_info("aa.Hidden");\n')
        proc = run_file(main)
        assert proc.returncode != 0, proc.stdout
        assert "Pub" in proc.stdout, proc.stdout
        assert "false" in proc.stdout, proc.stdout
        assert "Hidden" in proc.stdout, proc.stdout
        assert "type aa.Hidden is not visible" in proc.stderr, proc.stderr

        main = write("main_value.skein",
                     "import std;\n"
                     "import aa;\n"
                     "let hidden = aa.make_hidden();\n"
                     "std.meta.type_info(hidden);\n")
        proc = run_file(main)
        assert proc.returncode != 0, proc.stdout
        assert "type aa.Hidden is not visible" in proc.stderr, proc.stderr

        main = write("main_alias.skein",
                     "import std;\n"
                     "import aa as lib;\n"
                     'println(std.meta.type_info("lib.Pub")["name"]);\n'
                     'println(std.meta.has_type("lib.Hidden"));\n')
        proc = run_file(main)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.splitlines() == ["Pub", "false"], proc.stdout

        write("bb.skein",
              "package bb { export struct Pub { y: int; } }\n")
        main = write("main_ambiguous.skein",
                     "import std;\n"
                     "import aa;\n"
                     "import bb;\n"
                     'std.meta.type_info("Pub");\n')
        proc = run_file(main)
        assert proc.returncode != 0, proc.stdout
        assert "ambiguous type Pub" in proc.stderr, proc.stderr
