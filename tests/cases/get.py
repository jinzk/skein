#!/usr/bin/env python3
"""CLI tests for `skein mod init` and `skein get`."""

import os
import shutil
import subprocess
import tempfile

import harness


MODULE = "example.test/util"
TAG = "v1.0.0"


def write(root, rel, text):
    path = os.path.join(root, rel) if rel else root
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return path


def git(repo, *args):
    proc = subprocess.run(
        ["git", "-C", repo, *args],
        text=True,
        capture_output=True,
        timeout=harness.DEFAULT_TIMEOUT,
    )
    assert proc.returncode == 0, proc.stderr


def run_skein(args, env=None, cwd=None, stdin=None):
    return subprocess.run(
        [harness.BIN, *args],
        input=stdin,
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=harness.DEFAULT_TIMEOUT,
        env=env,
        cwd=cwd,
    )


def make_module_repo(root):
    repo = os.path.join(root, "repo")
    os.makedirs(repo)
    git(repo, "init", "-q")
    git(repo, "config", "user.email", "test@example.com")
    git(repo, "config", "user.name", "Skein Test")
    write(
        repo,
        "module.skein",
        "module util {\n"
        "  export package json;\n"
        "  export package json as api;\n"
        "}\n",
    )
    write(
        repo,
        "json.skein",
        "package json {\n  export func answer() { return 42; }\n}\n",
    )
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "initial")
    git(repo, "tag", TAG)
    # A plain absolute path works with both Windows Git and MSYS2 Git; a
    # file:// URL would need drive-letter normalization for MSYS2.
    return repo, os.path.abspath(repo).replace(os.sep, "/")


def base_env(root, repo_url):
    env = os.environ.copy()
    env["SKEIN_MODULE_URL"] = repo_url
    env["SKEINMODCACHE"] = os.path.join(root, "cache")
    return env


def run_cases():
    if shutil.which("git") is None:
        return

    with tempfile.TemporaryDirectory() as root:
        repo, repo_url = make_module_repo(root)
        env = base_env(root, repo_url)
        project = os.path.join(root, "project")
        os.makedirs(project)

        init = run_skein(
            ["mod", "init", "-C", project, "example.test/app"], env=env)
        assert init.returncode == 0, init.stderr
        mod_path = os.path.join(project, "skein.mod")
        with open(mod_path, encoding="utf-8") as handle:
            assert handle.read() == "module example.test/app\n"

        again = run_skein(
            ["mod", "init", "-C", project, "example.test/other"], env=env)
        assert again.returncode != 0
        assert "already exists" in again.stderr, again.stderr

        bad_init = run_skein(
            ["mod", "init", "-C", os.path.join(root, "bad"), "util"],
            env=env,
        )
        assert bad_init.returncode == 2, bad_init.stderr
        assert "invalid module path" in bad_init.stderr, bad_init.stderr

        fetched = run_skein(
            ["get", "-C", project, f"{MODULE}@{TAG}"], env=env)
        assert fetched.returncode == 0, fetched.stderr
        assert fetched.stdout.strip() == f"{MODULE}@{TAG}", fetched.stdout

        with open(mod_path, encoding="utf-8") as handle:
            mod_text = handle.read()
        assert f"require {MODULE} {TAG}" in mod_text, mod_text

        sum_path = os.path.join(project, "skein.sum")
        with open(sum_path, encoding="utf-8") as handle:
            sum_text = handle.read()
        assert sum_text.startswith(f"{MODULE} {TAG} h1:"), sum_text

        main = write(
            project,
            "main.skein",
            'import "example.test/util/json";\nprintln(json.answer());\n',
        )
        executed = run_skein([main], env=env, cwd=project)
        assert executed.returncode == 0, executed.stderr
        assert executed.stdout == "42\n", executed.stdout

        alias_main = write(
            project,
            "alias.skein",
            'import "example.test/util/api";\nprintln(api.answer());\n',
        )
        alias_executed = run_skein([alias_main], env=env, cwd=project)
        assert alias_executed.returncode == 0, alias_executed.stderr
        assert alias_executed.stdout == "42\n", alias_executed.stdout

        linted = run_skein(["lint", main], env=env, cwd=project)
        assert linted.returncode == 0, linted.stderr

        formatted = run_skein(["fmt", main], env=env, cwd=project)
        assert formatted.returncode == 0, formatted.stderr
        assert 'import "example.test/util/json";' in formatted.stdout

        # Check-only tools must not evaluate (or download) external imports.
        offline = os.path.join(root, "offline")
        os.makedirs(offline)
        write(offline, "skein.mod", f"module example.test/app\nrequire {MODULE} {TAG}\n")
        offline_main = write(
            offline,
            "main.skein",
            'import "example.test/util/json" as json;\nprintln("ok");\n',
        )
        offline_env = env.copy()
        offline_env["SKEINMODCACHE"] = os.path.join(root, "empty-cache")
        lint_offline = run_skein(["lint", offline_main], env=offline_env)
        assert lint_offline.returncode == 0, lint_offline.stderr
        fmt_offline = run_skein(["fmt", offline_main], env=offline_env)
        assert fmt_offline.returncode == 0, fmt_offline.stderr

        # A required module is needed at run time; get is never implicit.
        nodeps = os.path.join(root, "nodeps")
        os.makedirs(nodeps)
        write(nodeps, "skein.mod", "module example.test/nodeps\n")
        nodeps_main = write(
            nodeps,
            "main.skein",
            'import "example.test/util/json" as json;\nprintln("unreachable");\n',
        )
        missing = run_skein([nodeps_main], env=env, cwd=nodeps)
        assert missing.returncode != 0, missing.stdout
        assert "external module not required" in missing.stderr, missing.stderr
        assert not os.path.exists(os.path.join(nodeps, "skein.sum"))

        cached_source = os.path.join(
            root, "cache", "example.test", TAG, "util", "json.skein")
        write(cached_source, "", "package json { export func answer() { return -1; } }\n")
        tampered = run_skein([main], env=env, cwd=project)
        assert tampered.returncode != 0, tampered.stdout
        assert "checksum mismatch" in tampered.stderr, tampered.stderr

        invalid = write(
            root,
            "invalid.skein",
            'import "util";\nprintln("unreachable");\n',
        )
        invalid_run = run_skein([invalid], env=env)
        assert invalid_run.returncode != 0, invalid_run.stdout
        assert "invalid external module path" in invalid_run.stderr, (
            invalid_run.stderr)

        # @latest resolves tags through git ls-remote and updates the require.
        git(repo, "tag", "v1.1.0")
        latest = run_skein(["get", "-C", project, f"{MODULE}@latest"], env=env)
        assert latest.returncode == 0, latest.stderr
        assert latest.stdout.strip() == f"{MODULE}@v1.1.0", latest.stdout
        with open(mod_path, encoding="utf-8") as handle:
            assert f"require {MODULE} v1.1.0" in handle.read()
        upgraded = run_skein([main], env=env, cwd=project)
        assert upgraded.returncode == 0, upgraded.stderr
        assert upgraded.stdout == "42\n", upgraded.stdout
