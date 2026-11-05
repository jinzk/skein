#!/usr/bin/env python3
"""Cross-platform lexical paths provided by std.path."""
from harness import *  # noqa: F401,F403


def run_cases():
    check(
        "path parse posix",
        "import std.path;\n"
        'let p = path.parse("/a/b/../c.txt");\n'
        'print(p["style"]);\n'
        'print(p["root"]);\n'
        'print(p["dir"]);\n'
        'print(p["base"]);\n'
        'print(p["stem"]);\n'
        'print(p["ext"]);\n'
        'print(p["parts"]);\n'
        'print(p["absolute"]);\n'
        'print(p["normalized"]);\n',
        stdout_all=[
            "posix", "/", "/a/b/..", "c.txt", "c", ".txt",
            "[a, b, .., c.txt]", "true", "/a/c.txt",
        ],
    )
    check(
        "path parse windows drive",
        "import std.path;\n"
        'let p = path.parse("C:/Users/alice/../notes.txt", "windows");\n'
        'print(p["style"]);\n'
        'print(p["root"]);\n'
        'print(p["drive"]);\n'
        'print(p["dir"]);\n'
        'print(p["base"]);\n'
        'print(p["stem"]);\n'
        'print(p["ext"]);\n'
        'print(p["parts"]);\n'
        'print(p["absolute"]);\n'
        'print(p["normalized"]);\n',
        stdout_all=[
            "windows", "C:\\", "C:", "C:\\Users\\alice\\..",
            "notes.txt", "notes", ".txt",
            "[Users, alice, .., notes.txt]", "true",
            "C:\\Users\\notes.txt",
        ],
    )
    check(
        "path parse windows backslash",
        "import std.path;\n"
        f'let p = path.parse({skein_string(r"C:\Users\alice\a.txt")}, '
        '"windows");\n'
        'print(p["root"]);\n'
        'print(p["dir"]);\n'
        'print(p["base"]);\n'
        'print(p["normalized"]);\n',
        stdout_all=["C:\\", "C:\\Users\\alice", "a.txt",
                    "C:\\Users\\alice\\a.txt"],
    )
    check(
        "path parse unc",
        "import std.path;\n"
        'let p = path.parse("//server/share/a/b.txt", "windows");\n'
        'print(p["root"]);\n'
        'print(p["drive"]);\n'
        'print(p["dir"]);\n'
        'print(p["base"]);\n'
        'print(p["absolute"]);\n'
        'print(p["normalized"]);\n',
        stdout_all=["\\\\server\\share\\", "", "\\\\server\\share\\a",
                    "b.txt", "true", "\\\\server\\share\\a\\b.txt"],
    )
    check(
        "path parse verbatim",
        "import std.path;\n"
        'let p = path.parse("//?/C:/a/b.txt", "windows");\n'
        'print(p["root"]);\n'
        'print(p["normalized"]);\n'
        'let u = path.parse("//?/UNC/server/share/a", "windows");\n'
        'print(u["root"]);\n'
        'print(u["normalized"]);\n',
        stdout_all=["\\\\?\\C:\\", "\\\\?\\C:\\a\\b.txt",
                    "\\\\?\\UNC\\server\\share\\",
                    "\\\\?\\UNC\\server\\share\\a"],
    )
    check(
        "path parse drive relative and rooted",
        "import std.path;\n"
        'let d = path.parse("C:foo/bar", "windows");\n'
        'print(d["root"]);\n'
        'print(d["absolute"]);\n'
        'print(d["rooted"]);\n'
        'print(d["normalized"]);\n'
        'let r = path.parse("/foo", "windows");\n'
        'print(r["root"]);\n'
        'print(r["absolute"]);\n'
        'print(r["rooted"]);\n'
        'print(r["normalized"]);\n',
        stdout_all=["C:", "false", "false", "C:foo\\bar",
                    "\\", "false", "true", "\\foo"],
    )
    check(
        "path normalize posix",
        "import std.path;\n"
        'print(path.normalize("/a/./b/../c"));\n'
        'print(path.normalize("/../.."));\n'
        'print(path.normalize("a/../../b"));\n'
        'print(path.normalize(""));\n'
        'print(path.normalize("/"));\n',
        stdout_all=["/a/c", "/", "../b", ".", "/"],
    )
    check(
        "path normalize windows",
        "import std.path;\n"
        'print(path.normalize("C:/a/./b/../c", "windows"));\n'
        'print(path.normalize("C:/../../a", "windows"));\n'
        'print(path.normalize("C:/", "windows"));\n'
        'print(path.normalize("C:", "windows"));\n'
        'print(path.normalize("C:foo/../bar", "windows"));\n'
        'print(path.normalize("//server/share/a/../../b", "windows"));\n'
        'print(path.normalize("//?/C:/a/../b", "windows"));\n',
        stdout_all=[
            "C:\\a\\c", "C:\\a", "C:\\", "C:", "C:bar",
            "\\\\server\\share\\b", "\\\\?\\C:\\b",
        ],
    )
    check(
        "path join basic",
        "import std.path;\n"
        'print(path.join());\n'
        'print(path.join("a"));\n'
        'print(path.join("a", "b", "c"));\n'
        'print(path.join("a", "/b"));\n'
        'print(path.join("C:/a", "b", {"style": "windows"}));\n'
        'print(path.join("C:/a", "D:/b", {"style": "windows"}));\n'
        'print(path.join("C:/a", "/b", {"style": "windows"}));\n'
        'print(path.join_list(["a", "b"], "posix"));\n'
        'print(path.join_list(["C:/a", "D:/b"], "windows"));\n',
        stdout_all=[
            ".", "a", "a/b/c", "/b", "C:\\a\\b", "D:\\b", "\\b",
            "a/b", "D:\\b",
        ],
    )
    check(
        "path absolute",
        "import std.path;\n"
        'print(path.absolute("a/b", "posix", "/work"));\n'
        'print(path.absolute("a/b", "windows", "C:/work"));\n'
        'print(path.absolute("C:/x", "windows", "C:/work"));\n'
        'print(path.absolute(".", "posix", "/work"));\n'
        'print(path.is_absolute(path.absolute("a/b")));\n',
        stdout_all=[
            "/work/a/b", "C:\\work\\a\\b", "C:\\x", "/work", "true",
        ],
    )
    check(
        "path resolve",
        "import std.path;\n"
        'print(path.resolve("/work/a", "../b", "posix"));\n'
        'print(path.resolve("C:/work", "/foo", "windows"));\n'
        'print(path.resolve("C:/work", "C:foo", "windows"));\n'
        'print(path.resolve("C:/work", "C:../foo", "windows"));\n'
        'print(path.resolve("C:/work", "\\\\foo", "windows"));\n'
        'print(path.resolve("//server/share/a", "\\\\foo", "windows"));\n'
        'print(path.resolve("/work", "a", "posix"));\n',
        stdout_all=[
            "/work/b", "C:\\foo", "C:\\work\\foo", "C:\\foo",
            "C:\\foo", "\\\\server\\share\\foo", "/work/a",
        ],
    )
    check(
        "path relative",
        "import std.path;\n"
        'print(path.relative("/a/b", "/a/c/d"));\n'
        'print(path.relative("/a/b", "/a/b"));\n'
        'print(path.relative("/a/b/c", "/a"));\n'
        'print(path.relative("/a/b", "/"));\n'
        'print(path.relative("", "a"));\n'
        'print(path.relative(".", "a"));\n'
        'print(path.relative("C:/A/b", "c:/a/c", "windows"));\n'
        'print(path.relative("C:/a", "D:/b", "windows"));\n',
        stdout_all=[
            "../c/d", ".", "../..", "../..", "a", "a",
            "..\\c", "D:\\b",
        ],
    )
    check(
        "path basename dirname",
        "import std.path;\n"
        'print(path.basename("/"));\n'
        'print(path.basename("a/b/"));\n'
        'print(path.basename("."));\n'
        'print(path.dirname("/"));\n'
        'print(path.dirname("a"));\n'
        'print(path.dirname("a/b"));\n'
        'print(path.dirname("C:/foo", "windows"));\n'
        'print(path.dirname("C:foo", "windows"));\n'
        'print(path.basename("//server/share", "windows"));\n'
        'print(path.dirname("//server/share", "windows"));\n',
        stdout_all=[
            "", "b", ".", "/", ".", "a", "C:\\", "C:", "",
            "\\\\server\\share\\",
        ],
    )
    check(
        "path extname stem",
        "import std.path;\n"
        'print(path.extname(".bashrc"));\n'
        'print(path.extname("foo."));\n'
        'print(path.extname("a.tar.gz"));\n'
        'print(path.stem("a.tar.gz"));\n'
        'print(path.stem(".bashrc"));\n'
        'print(path.extname("C:/a.txt", "windows"));\n',
        stdout_all=["", ".", ".gz", "a.tar", ".bashrc", ".txt"],
    )
    check(
        "path with_ext",
        "import std.path;\n"
        'print(path.with_ext("a/b.txt", "md"));\n'
        'print(path.with_ext("a/b.txt", ".md"));\n'
        'print(path.with_ext("a/b.txt", ""));\n'
        'print(path.with_ext("a/../b.txt", "md"));\n'
        'print(path.with_ext("目录/😀.txt", "md"));\n'
        'print(path.with_ext("C:/a.txt", "md", "windows"));\n',
        stdout_all=["a/b.md", "a/b.md", "a/b", "b.md", "目录/😀.md",
                    "C:\\a.md"],
    )
    check(
        "path equals",
        "import std.path;\n"
        'print(path.equals("/A/b", "/a/b", "posix"));\n'
        'print(path.equals("C:/A/b", "c:/a/b", "windows"));\n'
        'print(path.equals("C:/a/../b", "C:/b", "windows"));\n'
        'print(path.equals("C:/a/b", "c:\\\\a\\\\b", "windows"));\n',
        stdout_all=["false", "true", "true", "true"],
    )
    check(
        "path separator helpers",
        "import std.path;\n"
        'print(path.sep("posix"));\n'
        'print(path.sep("windows"));\n'
        'print(path.delimiter("posix"));\n'
        'print(path.delimiter("windows"));\n'
        'print(path.detect("C:/a"));\n'
        'print(path.detect("a\\\\b"));\n'
        'print(path.detect("a/b") == path.host());\n'
        'print(path.is_absolute("C:/a", "windows"));\n'
        'print(path.is_relative("a/b", "posix"));\n'
        'print(path.to_posix("C:\\\\a\\\\b"));\n'
        'print(path.to_windows("/a/b"));\n'
        'print(path.to_windows("C:/a/b", "windows"));\n',
        stdout_all=[
            "/", "\\", ":", ";", "windows", "windows", "true", "true",
            "true", "C:/a/b", "\\a\\b", "C:\\a\\b",
        ],
    )
    check(
        "path unicode components",
        "import std.path;\n"
        'print(path.basename("目录/😀.txt"));\n'
        'print(path.parse("目录/😀.txt")["parts"]);\n'
        'print(path.normalize("目录/../😀.txt"));\n'
        'print(path.equals("/café/😀", "/café/😀", "posix"));\n',
        stdout_all=["😀.txt", "[目录, 😀.txt]", "😀.txt", "true"],
    )

    check(
        "path wrong type",
        'import std.path;\npath.normalize(1)\n',
        stderr="expects a path string",
    )
    check(
        "path parse wrong type",
        'import std.path;\npath.parse(1)\n',
        stderr="expects a path string",
    )
    check(
        "path invalid style",
        'import std.path;\npath.normalize("a", "unix")\n',
        stderr='style must be "auto", "posix" or "windows"',
    )
    check(
        "path join invalid options",
        'import std.path;\npath.join("a", {})\n',
        stderr="options must contain a style key",
    )
    check(
        "path join invalid style",
        'import std.path;\npath.join("a", {"style": "unix"})\n',
        stderr='style must be "auto", "posix" or "windows"',
    )
    check(
        "path with_ext no file name",
        'import std.path;\npath.with_ext("/", "txt")\n',
        stderr="path has no file name",
    )
    check(
        "path relative mixed",
        'import std.path;\npath.relative("/a", "b")\n',
        stderr="both be absolute or both be relative",
    )
    check(
        "path resolve relative base",
        'import std.path;\npath.resolve("work", "a")\n',
        stderr="base must be an absolute path",
    )
    check(
        "path resolve drive mismatch",
        'import std.path;\npath.resolve("C:/work", "D:foo", "windows")\n',
        stderr="needs a matching base drive",
    )
    check(
        "path absolute cwd required",
        "import std.path;\n"
        'let other = "windows";\n'
        'if (path.host() == "windows") { other = "posix"; }\n'
        'path.absolute("a", other)\n',
        stderr="cwd is required for non-host style",
    )
    check(
        "path absolute relative cwd",
        'import std.path;\npath.absolute("a", "posix", "work")\n',
        stderr="cwd must be an absolute path",
    )
