#!/usr/bin/env python3
"""Integration tests for the optional MySQL native plugin example.

The no-server checks validate the native module ABI itself (load, constants,
errors, unload) and the source-native module packaging path (`skein get` /
`skein build` / `std.module.native`).  When SKEIN_TEST_MYSQL_* is set, the
wrapper is also exercised against a real MySQL/MariaDB server.
"""

import os
import shutil
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1])
PLUGIN = os.path.abspath(sys.argv[2])
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MODULE_ROOT = os.path.join(ROOT, "contrib", "mysql")
WRAPPER = os.path.join(MODULE_ROOT, "api", "mysql.skein")
MODULE_PATH = "skein.contrib/mysql"
MODULE_TAG = "v0.1.0"


def skein_string(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def mysql_env(base=None, with_plugin=True):
    env = dict(os.environ if base is None else base)
    env.pop("SKEIN_TEST_SKIP", None)
    if with_plugin:
        env["SKEIN_MYSQL_PLUGIN"] = os.path.abspath(PLUGIN)
    else:
        env.pop("SKEIN_MYSQL_PLUGIN", None)
    return env


def run(name, source, *, with_plugin=True):
    with tempfile.TemporaryDirectory() as root:
        shutil.copyfile(WRAPPER, os.path.join(root, "mysql.skein"))
        main = os.path.join(root, "main.skein")
        with open(main, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(source)
        proc = subprocess.run(
            [BIN, main],
            text=True,
            encoding="utf-8",
            capture_output=True,
            cwd=root,
            env=mysql_env(with_plugin=with_plugin),
            timeout=30,
        )
        proc.stdout = proc.stdout.replace("\r\n", "\n")
        proc.stderr = proc.stderr.replace("\r\n", "\n")
        return proc


def check(name, source, *, with_plugin=True, code=0, stdout=None,
          stdout_all=None, stderr=None):
    proc = run(name, source, with_plugin=with_plugin)
    assert "Segmentation fault" not in proc.stdout + proc.stderr, (
        f"{name}: crashed\n{proc.stdout}\n{proc.stderr}")
    assert proc.returncode == code, (
        f"{name}: expected exit {code}, got {proc.returncode}\n"
        f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    if stdout is not None:
        assert stdout in proc.stdout, (
            f"{name}: missing {stdout!r}\n{proc.stdout}\n{proc.stderr}")
    if stdout_all is not None:
        for expected in stdout_all:
            assert expected in proc.stdout, (
                f"{name}: missing {expected!r}\n{proc.stdout}\n{proc.stderr}")
    if stderr is not None:
        assert stderr in proc.stderr, (
            f"{name}: missing {stderr!r}\n{proc.stdout}\n{proc.stderr}")
    return proc


def test_abi():
    check(
        "mysql plugin version",
        "import mysql;\n"
        "println(mysql.VERSION);\n"
        "println(mysql.library_version());\n",
        stdout_all=["0.1.0"],
    )
    check(
        "mysql plugin connect error",
        "import std;\n"
        "import mysql;\n"
        'mysql.connect("127.0.0.1", "nobody", "wrong", "missing", 1) '
        "on error(e) {\n"
        "  println(std.error.kind(e));\n"
        "  recover null;\n"
        "}\n",
        stdout="runtime",
    )
    check(
        "mysql plugin argument error",
        "import std;\n"
        f"std.plugin.load({skein_string(os.path.abspath(PLUGIN))});\n"
        "import skein.mysql as native;\n"
        "native.connect(1, 2, 3, 4) on error(e) {\n"
        "  println(std.error.kind(e));\n"
        "  recover null;\n"
        "}\n",
        stdout="runtime",
    )
    check(
        "mysql plugin async export",
        "import std;\n"
        f"std.plugin.load({skein_string(os.path.abspath(PLUGIN))});\n"
        "import skein.mysql as native;\n"
        "native.query_task(1) on error(e) {\n"
        "  println(std.error.kind(e));\n"
        "  recover null;\n"
        "}\n",
        stdout="runtime",
    )
    check(
        "mysql plugin unload keeps live bindings",
        "import std;\n"
        "import mysql;\n"
        "println(mysql.VERSION);\n"
        'std.plugin.unload("skein.mysql");\n'
        "println(mysql.library_version());\n",
        stdout="0.1.0",
    )
    check(
        "mysql wrapper requires plugin path or module",
        "import mysql;\n",
        with_plugin=False,
        code=1,
        stderr="std.module.native",
    )


def integration_config():
    names = (
        "SKEIN_TEST_MYSQL_HOST",
        "SKEIN_TEST_MYSQL_USER",
        "SKEIN_TEST_MYSQL_PASSWORD",
        "SKEIN_TEST_MYSQL_DATABASE",
    )
    if any(name not in os.environ for name in names):
        return None
    return {
        "host": os.environ["SKEIN_TEST_MYSQL_HOST"],
        "user": os.environ["SKEIN_TEST_MYSQL_USER"],
        "password": os.environ["SKEIN_TEST_MYSQL_PASSWORD"],
        "database": os.environ["SKEIN_TEST_MYSQL_DATABASE"],
        "port": int(os.environ.get("SKEIN_TEST_MYSQL_PORT", "3306")),
    }


def test_integration(config):
    connect_args = ", ".join([
        skein_string(config["host"]),
        skein_string(config["user"]),
        skein_string(config["password"]),
        skein_string(config["database"]),
        str(config["port"]),
    ])
    source = (
        "import std;\n"
        "import mysql;\n"
        f"let c = mysql.connect({connect_args});\n"
        "println(c.ping());\n"
        "c.execute(\"DROP TABLE IF EXISTS skein_plugin_test\");\n"
        "c.execute(\"CREATE TABLE skein_plugin_test ("
        "id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(64), "
        "note VARCHAR(64), payload BLOB) CHARACTER SET utf8mb4\");\n"
        "let quoted = c.quote(\"O'Reilly\");\n"
        "c.execute(\"INSERT INTO skein_plugin_test (name, note, payload) "
        "VALUES ('\" + quoted + \"', NULL, X'0102FF')\");\n"
        "println(c.affected_rows());\n"
        "println(c.insert_id());\n"
        "let r = c.query(\"SELECT id, name, note, payload "
        "FROM skein_plugin_test\");\n"
        "std.plugin.unload(\"skein.mysql\");\n"
        "println(r.fields());\n"
        "println(r.count());\n"
        "let rows = r.rows();\n"
        "println(r.count());\n"
        "println(rows[0][\"name\"]);\n"
        "println(rows[0][\"note\"] == null);\n"
        "println(type_name(rows[0][\"payload\"]));\n"
        "let at = c.query_task(\"SELECT 42 AS answer\");\n"
        "println(type_name(at));\n"
        "let ar = mysql.wrap_result(at);\n"
        "println(ar.rows()[0][\"answer\"]);\n"
        "ar.close();\n"
        "let failed = c.query_task(\"SELECT * FROM skein_missing_table\");\n"
        "let caught = std.task.catch(failed).wait();\n"
        "println(std.result.is_err(caught));\n"
        "println(c.last_error() != \"\");\n"
        "let cancelled = c.query_task(\"SELECT 1\");\n"
        "std.task.cancel(cancelled);\n"
        "let ccaught = std.task.catch(cancelled).wait();\n"
        "println(std.result.is_err(ccaught));\n"
        "c.execute(\"SELECT * FROM skein_missing_table\") on error(e) {\n"
        "  println(std.error.kind(e));\n"
        "  recover null;\n"
        "}\n"
        "println(c.last_error() != \"\");\n"
        "c.execute(\"DROP TABLE skein_plugin_test\");\n"
        "r.close();\n"
        "c.close();\n"
    )
    check(
        "mysql plugin integration",
        source,
        stdout_all=[
            "true", "1", "[id, name, note, payload]", "1",
            "O'Reilly", "true", "bytes", "task", "42", "runtime",
            "true",
        ],
    )


def test_vendored_abi_header():
    with open(os.path.join(ROOT, "src", "runtime", "skein_module.h"),
              encoding="utf-8") as handle:
        expected = handle.read()
    with open(os.path.join(MODULE_ROOT, "native", "skein_module.h"),
              encoding="utf-8") as handle:
        actual = handle.read()
    assert actual == expected, "contrib/mysql/native/skein_module.h is stale"


def test_source_module():
    compiler = os.environ.get("SKEIN_CXX") or (
        shutil.which("c++") or shutil.which("g++") or shutil.which("clang++"))
    if not compiler or shutil.which("git") is None:
        print("mysql source module check skipped (git or C++ compiler missing)")
        return

    with tempfile.TemporaryDirectory() as root:
        repo = os.path.join(root, "repo")
        shutil.copytree(MODULE_ROOT, repo)

        # CMake may discover headers in a non-default include directory.  Add
        # a source-local shim so `skein build` uses the same client headers.
        include_dir = os.environ.get("SKEIN_TEST_MYSQL_INCLUDE_DIR", "")
        if include_dir:
            shim_dir = os.path.join(repo, "native", "include", "mysql")
            os.makedirs(shim_dir)
            header = os.path.join(include_dir, "mysql.h").replace("\\", "/")
            with open(os.path.join(shim_dir, "mysql.h"), "w",
                      encoding="utf-8", newline="\n") as handle:
                handle.write(f'#include "{header}"\n')
            manifest = os.path.join(repo, "module.skein")
            with open(manifest, encoding="utf-8") as handle:
                text = handle.read()
            text = text.replace(
                '    sources "native/mysql_module.cpp";\n',
                '    includes "native/include";\n'
                '    sources "native/mysql_module.cpp";\n',
            )
            with open(manifest, "w", encoding="utf-8",
                      newline="\n") as handle:
                handle.write(text)

        def git(*args):
            proc = subprocess.run(["git", "-C", repo, *args], text=True,
                                  capture_output=True, timeout=120)
            assert proc.returncode == 0, proc.stderr

        git("init", "-q")
        git("config", "user.email", "test@example.com")
        git("config", "user.name", "Skein Test")
        git("add", "-A")
        git("commit", "-qm", "initial")
        git("tag", MODULE_TAG)

        project = os.path.join(root, "project")
        os.makedirs(project)
        env = dict(os.environ)
        env.pop("SKEIN_TEST_SKIP", None)
        env.pop("SKEIN_MYSQL_PLUGIN", None)
        env.pop("SKEIN_ALLOW_NATIVE", None)
        env["SKEIN_MODULE_URL"] = repo
        env["SKEINMODCACHE"] = os.path.join(root, "cache")
        env["SKEIN_CXX"] = compiler
        spec = f"{MODULE_PATH}@{MODULE_TAG}"

        def cli(*args, extra=None):
            return subprocess.run([BIN, *args], text=True, encoding="utf-8",
                                  capture_output=True, cwd=project,
                                  env=dict(env, **(extra or {})), timeout=120)

        init = cli("mod", "init", "-C", project, "example.test/app")
        assert init.returncode == 0, init.stderr
        fetched = cli("get", "-C", project, spec)
        assert fetched.returncode == 0, fetched.stderr
        built = cli("build", "-C", project, spec)
        assert built.returncode == 0, built.stderr
        assert "built " in built.stdout, built.stdout

        main = os.path.join(project, "main.skein")
        with open(main, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                'import "skein.contrib/mysql" as mysql;\n'
                "println(mysql.VERSION);\n"
                'println(mysql.library_version() != "");\n'
                "println(mysql.wrap_result != null);\n"
            )

        denied = cli(main)
        assert denied.returncode == 1, denied.stdout
        assert "SKEIN_ALLOW_NATIVE" in denied.stderr, denied.stderr
        allowed = cli(main, extra={"SKEIN_ALLOW_NATIVE": "1"})
        assert allowed.returncode == 0, allowed.stderr
        assert allowed.stdout.splitlines() == ["0.1.0", "true", "true"], (
            allowed.stdout)

    print("mysql source module check passed")


def main():
    if not os.path.exists(PLUGIN):
        raise SystemExit(f"plugin not found: {PLUGIN}")
    test_abi()
    test_vendored_abi_header()
    test_source_module()
    config = integration_config()
    if config:
        test_integration(config)
        print(f"mysql plugin integration passed for {config['host']}")
    else:
        print("mysql plugin ABI checks passed (set SKEIN_TEST_MYSQL_* "
              "for database integration)")


if __name__ == "__main__":
    main()
