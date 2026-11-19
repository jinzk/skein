#!/usr/bin/env python3
"""CLI tests for `skein mod init` and `skein get`."""

import os
import shutil
import subprocess
import tempfile
import time

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


def run_skein(args, env=None, cwd=None, stdin=None, timeout=None):
    return subprocess.run(
        [harness.BIN, *args],
        input=stdin,
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=timeout or harness.DEFAULT_TIMEOUT,
        env=env,
        cwd=cwd,
    )


def module_lock_name(module, version):
    digest = 0xCBF29CE484222325
    for byte in f"{module}@{version}".encode("utf-8"):
        digest ^= byte
        digest = (digest * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{digest:016x}.lock"


def lock_handle(handle, shared=False):
    handle.seek(0)
    if os.name == "nt":
        import msvcrt
        msvcrt.locking(handle.fileno(), msvcrt.LK_LOCK, 1)
    else:
        import fcntl
        fcntl.flock(
            handle.fileno(), fcntl.LOCK_SH if shared else fcntl.LOCK_EX)


def unlock_handle(handle):
    handle.seek(0)
    if os.name == "nt":
        import msvcrt
        msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
    else:
        import fcntl
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


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

        verified = run_skein(["mod", "verify", "-C", project], env=env)
        assert verified.returncode == 0, verified.stderr
        assert "verified 1 module(s)" in verified.stdout, verified.stdout

        # get --offline must not touch the network once cache and sums are valid.
        offline_env = env.copy()
        offline_env["SKEIN_MODULE_URL"] = "Z:/missing-skein-mirror"
        offline_get = run_skein(
            ["get", "-C", project, f"{MODULE}@{TAG}", "--offline"],
            env=offline_env,
        )
        assert offline_get.returncode == 0, offline_get.stderr
        offline_latest = run_skein(
            ["get", "-C", project, f"{MODULE}@latest", "--offline"],
            env=offline_env,
        )
        assert offline_latest.returncode != 0, offline_latest.stdout
        assert "cannot resolve @latest in offline mode" in offline_latest.stderr, (
            offline_latest.stderr)
        offline_force = run_skein(
            ["get", "-C", project, f"{MODULE}@{TAG}", "--offline", "--force"],
            env=offline_env,
        )
        assert offline_force.returncode != 0, offline_force.stdout
        assert "mutually exclusive" in offline_force.stderr, offline_force.stderr

        # mod download prefetches exact require entries without changing skein.mod.
        download_project = os.path.join(root, "download-project")
        os.makedirs(download_project)
        download_mod = (
            f"module example.test/download\nrequire {MODULE} {TAG}\n")
        write(download_project, "skein.mod", download_mod)
        download_env = env.copy()
        download_env["SKEINMODCACHE"] = os.path.join(root, "download-cache")
        downloaded = run_skein(
            ["mod", "download", "-C", download_project], env=download_env)
        assert downloaded.returncode == 0, downloaded.stderr
        assert f"downloaded {MODULE}@{TAG}" in downloaded.stdout, (
            downloaded.stdout)
        assert os.path.exists(
            os.path.join(download_project, "skein.sum"))
        with open(os.path.join(download_project, "skein.mod"),
                  encoding="utf-8") as handle:
            assert handle.read() == download_mod
        download_main = write(
            download_project,
            "main.skein",
            'import "example.test/util/json";\nprintln(json.answer());\n',
        )
        download_run = run_skein(
            [download_main], env=download_env, cwd=download_project)
        assert download_run.returncode == 0, download_run.stderr
        assert download_run.stdout == "42\n", download_run.stdout
        downloaded_offline = run_skein(
            ["mod", "download", "-C", download_project, "--offline"],
            env=download_env,
        )
        assert downloaded_offline.returncode == 0, downloaded_offline.stderr

        # mod tidy drops unused indirect requirements and keeps direct ones.
        tidy_project = os.path.join(root, "tidy-project")
        os.makedirs(tidy_project)
        write(
            tidy_project,
            "skein.mod",
            f"module example.test/tidy\n"
            f"require {MODULE} {TAG}\n"
            "require example.test/stale v9.9.9 // indirect\n",
        )
        tidied = run_skein(["mod", "tidy", "-C", tidy_project], env=env)
        assert tidied.returncode == 0, tidied.stderr
        with open(os.path.join(tidy_project, "skein.mod"),
                  encoding="utf-8") as handle:
            tidy_text = handle.read()
        assert f"require {MODULE} {TAG}" in tidy_text, tidy_text
        assert "example.test/stale" not in tidy_text, tidy_text
        tidy_verify = run_skein(
            ["mod", "verify", "-C", tidy_project], env=env)
        assert tidy_verify.returncode == 0, tidy_verify.stderr
        tidy_offline = run_skein(
            ["mod", "tidy", "-C", tidy_project, "--offline"], env=env)
        assert tidy_offline.returncode == 0, tidy_offline.stderr

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
        verify_tampered = run_skein(
            ["mod", "verify", "-C", project], env=env)
        assert verify_tampered.returncode != 0, verify_tampered.stdout
        assert "checksum mismatch" in verify_tampered.stderr, (
            verify_tampered.stderr)

        # `skein get` must not heal a checksum mismatch by rewriting skein.sum.
        with open(sum_path, encoding="utf-8") as handle:
            good_sum = handle.read()
        refused = run_skein(
            ["get", "-C", project, f"{MODULE}@{TAG}"], env=env)
        assert refused.returncode != 0, refused.stdout
        assert "refusing to overwrite skein.sum" in refused.stderr, (
            refused.stderr)
        with open(sum_path, encoding="utf-8") as handle:
            assert handle.read() == good_sum

        refreshed = run_skein(
            ["get", "-C", project, f"{MODULE}@{TAG}", "--force"], env=env)
        assert refreshed.returncode == 0, refreshed.stderr
        restored = run_skein([main], env=env, cwd=project)
        assert restored.returncode == 0, restored.stderr

        # A stale committed checksum must also reject freshly downloaded code.
        cache_dir = os.path.join(root, "cache", "example.test", TAG, "util")
        shutil.rmtree(cache_dir)
        stale_sum = f"{MODULE} {TAG} h1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n"
        with open(sum_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(stale_sum)
        stale = run_skein(["get", "-C", project, f"{MODULE}@{TAG}"], env=env)
        assert stale.returncode != 0, stale.stdout
        assert "downloaded content does not match skein.sum" in stale.stderr, (
            stale.stderr)
        assert not os.path.exists(cache_dir), stale.stderr
        with open(sum_path, encoding="utf-8") as handle:
            assert handle.read() == stale_sum
        with open(sum_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(good_sum)
        re_fetched = run_skein(
            ["get", "-C", project, f"{MODULE}@{TAG}"], env=env)
        assert re_fetched.returncode == 0, re_fetched.stderr

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

        # Diamond dependencies: A needs C v1.0.0, B needs C v1.2.0;
        # minimal version selection must choose v1.2.0 for the whole graph.
        diamond_root = os.path.join(root, "diamond")
        os.makedirs(diamond_root)
        urls = {}

        def dep_repo(name, full_name, skeleton, extra=None, modfile=None):
            repo = os.path.join(diamond_root, name)
            os.makedirs(repo)
            git(repo, "init", "-q")
            git(repo, "config", "user.email", "test@example.com")
            git(repo, "config", "user.name", "Skein Test")
            write(repo, "module.skein", skeleton)
            if modfile:
                write(repo, "skein.mod", modfile)
            if extra:
                for rel, text in extra.items():
                    write(repo, rel, text)
            git(repo, "add", "-A")
            git(repo, "commit", "-qm", name)
            urls[full_name] = os.path.abspath(repo).replace(os.sep, "/")
            return repo

        c_repo = dep_repo(
            "c", "example.test/c",
            "module c {\n  export package value;\n}\n",
            {"value.skein": "package value {}\n"},
        )

        def tag_value(repo, version, value):
            write(
                repo,
                "value.skein",
                f"package value {{ export func get() {{ return {value}; }} }}\n",
            )
            git(repo, "add", "-A")
            git(repo, "commit", "-qm", f"value {value}")
            git(repo, "tag", version)

        tag_value(c_repo, "v1.0.0", 10)
        tag_value(c_repo, "v1.2.0", 12)

        a_repo = dep_repo(
            "a", "example.test/a",
            "module a {\n  export package dep;\n}\n",
            {"dep.skein": "package dep { export func get() { return 1; } }\n"},
            "require example.test/c v1.0.0\n",
        )
        git(a_repo, "tag", "v1.0.0")

        b_repo = dep_repo(
            "b", "example.test/b",
            "module b {\n  export package dep;\n}\n",
            {"dep.skein": "package dep { export func get() { return 2; } }\n"},
            "require example.test/c v1.2.0\n",
        )
        git(b_repo, "tag", "v1.0.0")

        url_map = ",".join(f"{name}={url}" for name, url in urls.items())
        diamond_env = os.environ.copy()
        diamond_env["SKEIN_MODULE_URL"] = url_map
        diamond_env["SKEINMODCACHE"] = os.path.join(root, "diamond-cache")
        diamond_project = os.path.join(root, "diamond-project")
        os.makedirs(diamond_project)
        run_skein(["mod", "init", "-C", diamond_project, "example.test/app"],
                  env=diamond_env)
        diamond_get = run_skein(
            ["get", "-C", diamond_project,
             "example.test/a@v1.0.0", "example.test/b@v1.0.0"],
            env=diamond_env,
            timeout=30,
        )
        assert diamond_get.returncode == 0, diamond_get.stderr
        with open(os.path.join(diamond_project, "skein.mod"), encoding="utf-8") as handle:
            diamond_mod = handle.read()
        assert "require example.test/c v1.2.0 // indirect" in diamond_mod, diamond_mod
        assert "require example.test/a v1.0.0\n" in diamond_mod, diamond_mod
        assert "require example.test/b v1.0.0\n" in diamond_mod, diamond_mod

        diamond_main = write(
            diamond_project,
            "main.skein",
            'import "example.test/c/value";\nprintln(value.get());\n',
        )
        diamond_run = run_skein([diamond_main], env=diamond_env, cwd=diamond_project)
        assert diamond_run.returncode == 0, diamond_run.stderr
        assert diamond_run.stdout == "12\n", diamond_run.stdout

        # A /vN module path is a distinct module and keeps its major version.
        v2_repo = dep_repo(
            "c-v2", "example.test/c/v2",
            "module c {\n  export package value;\n}\n",
            {"value.skein": "package value { export func get() { return 20; } }\n"},
        )
        git(v2_repo, "tag", "v2.0.0")
        v2_env = diamond_env.copy()
        v2_env["SKEIN_MODULE_URL"] = ",".join(
            f"{name}={url}" for name, url in urls.items())
        v2_get = run_skein(
            ["get", "-C", diamond_project, "example.test/c/v2@v2.0.0"],
            env=v2_env,
        )
        assert v2_get.returncode == 0, v2_get.stderr
        with open(os.path.join(diamond_project, "skein.mod"), encoding="utf-8") as handle:
            v2_mod = handle.read()
        assert "require example.test/c/v2 v2.0.0\n" in v2_mod, v2_mod
        v2_main = write(
            diamond_project,
            "v2main.skein",
            'import "example.test/c/v2/value";\nprintln(value.get());\n',
        )
        v2_run = run_skein([v2_main], env=v2_env, cwd=diamond_project)
        assert v2_run.returncode == 0, v2_run.stderr
        assert v2_run.stdout == "20\n", v2_run.stdout

        # A single URL template can mirror every module path.
        mirror = os.path.join(root, "mirror")
        mirror_repo = os.path.join(mirror, "example.test", "util")
        os.makedirs(mirror_repo)
        git(mirror_repo, "init", "-q")
        git(mirror_repo, "config", "user.email", "test@example.com")
        git(mirror_repo, "config", "user.name", "Skein Test")
        write(
            mirror_repo,
            "module.skein",
            "module util {\n  export package json;\n}\n",
        )
        write(
            mirror_repo,
            "json.skein",
            "package json {\n  export func answer() { return 7; }\n}\n",
        )
        git(mirror_repo, "add", "-A")
        git(mirror_repo, "commit", "-qm", "mirror")
        git(mirror_repo, "tag", TAG)

        template_project = os.path.join(root, "template-project")
        os.makedirs(template_project)
        init_template = run_skein(
            ["mod", "init", "-C", template_project, "example.test/template"],
            env=env,
        )
        assert init_template.returncode == 0, init_template.stderr
        template_env = os.environ.copy()
        template_env["SKEIN_MODULE_URL"] = (
            os.path.abspath(mirror).replace(os.sep, "/") + "/{path}")
        template_env["SKEINMODCACHE"] = os.path.join(root, "template-cache")
        templated = run_skein(
            ["get", "-C", template_project, f"{MODULE}@{TAG}"],
            env=template_env,
        )
        assert templated.returncode == 0, templated.stderr
        template_main = write(
            template_project,
            "main.skein",
            'import "example.test/util/json";\nprintln(json.answer());\n',
        )
        template_run = run_skein(
            [template_main], env=template_env, cwd=template_project)
        assert template_run.returncode == 0, template_run.stderr
        assert template_run.stdout == "7\n", template_run.stdout

        # {version} is expanded for explicit versions.
        version_mirror = os.path.join(root, "version-mirror")
        shutil.copytree(
            mirror_repo,
            os.path.join(version_mirror, "example.test", "util", TAG),
        )
        version_project = os.path.join(root, "version-project")
        os.makedirs(version_project)
        init_version = run_skein(
            ["mod", "init", "-C", version_project, "example.test/version"],
            env=env,
        )
        assert init_version.returncode == 0, init_version.stderr
        version_env = os.environ.copy()
        version_env["SKEIN_MODULE_URL"] = (
            os.path.abspath(version_mirror).replace(os.sep, "/") +
            "/{path}/{version}")
        version_env["SKEINMODCACHE"] = os.path.join(root, "version-cache")
        versioned = run_skein(
            ["get", "-C", version_project, f"{MODULE}@{TAG}"],
            env=version_env,
        )
        assert versioned.returncode == 0, versioned.stderr
        version_main = write(
            version_project,
            "main.skein",
            'import "example.test/util/json";\nprintln(json.answer());\n',
        )
        version_run = run_skein(
            [version_main], env=version_env, cwd=version_project)
        assert version_run.returncode == 0, version_run.stderr
        assert version_run.stdout == "7\n", version_run.stdout

        bad_latest = run_skein(
            ["get", "-C", version_project, f"{MODULE}@latest"],
            env=version_env,
        )
        assert bad_latest.returncode != 0, bad_latest.stdout
        assert "{version} cannot resolve @latest" in bad_latest.stderr, (
            bad_latest.stderr)

        # A per-module lock blocks a concurrent get for the same module@version.
        lock_repo = os.path.join(root, "lock-repo")
        os.makedirs(lock_repo)
        git(lock_repo, "init", "-q")
        git(lock_repo, "config", "user.email", "test@example.com")
        git(lock_repo, "config", "user.name", "Skein Test")
        write(
            lock_repo,
            "module.skein",
            "module util {\n  export package json;\n}\n",
        )
        write(
            lock_repo,
            "json.skein",
            "package json { export func answer() { return 9; } }\n",
        )
        git(lock_repo, "add", "-A")
        git(lock_repo, "commit", "-qm", "lock")
        git(lock_repo, "tag", TAG)

        lock_project = os.path.join(root, "lock-project")
        os.makedirs(lock_project)
        init_lock = run_skein(
            ["mod", "init", "-C", lock_project, "example.test/lock"], env=env)
        assert init_lock.returncode == 0, init_lock.stderr
        lock_cache = os.path.join(root, "lock-cache")
        lock_env = env.copy()
        lock_env["SKEIN_MODULE_URL"] = os.path.abspath(lock_repo).replace(
            os.sep, "/")
        lock_env["SKEINMODCACHE"] = lock_cache

        lock_path = os.path.join(
            lock_cache, ".locks", module_lock_name(MODULE, TAG))
        os.makedirs(os.path.dirname(lock_path), exist_ok=True)

        with open(lock_path, "wb") as lock_file:
            lock_file.write(b"0")
            lock_file.flush()
            lock_handle(lock_file)

            child = subprocess.Popen(
                [harness.BIN, "get", "-C", lock_project,
                 f"{MODULE}@{TAG}"],
                env=lock_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                time.sleep(0.5)
                blocked = child.poll() is None
            finally:
                unlock_handle(lock_file)

        child_out, child_err = child.communicate(
            timeout=harness.DEFAULT_TIMEOUT)
        assert blocked, child_out + child_err
        assert child.returncode == 0, child_err
        assert os.path.exists(
            os.path.join(lock_cache, "example.test", TAG, "util",
                         "json.skein"))

        # A second project sharing the cache must not trust bytes without its
        # own checksum; it refreshes and records its own skein.sum entry.
        shared_project = os.path.join(root, "shared-project")
        os.makedirs(shared_project)
        init_shared = run_skein(
            ["mod", "init", "-C", shared_project, "example.test/shared"],
            env=env,
        )
        assert init_shared.returncode == 0, init_shared.stderr
        shared = run_skein(
            ["get", "-C", shared_project, f"{MODULE}@{TAG}"], env=lock_env)
        assert shared.returncode == 0, shared.stderr
        assert os.path.exists(os.path.join(shared_project, "skein.sum"))

        # The project lock serializes writers of skein.mod / skein.sum.
        project_lock_path = os.path.join(lock_project, ".skein", "lock")
        os.makedirs(os.path.dirname(project_lock_path), exist_ok=True)
        with open(project_lock_path, "wb") as lock_file:
            lock_file.write(b"0")
            lock_file.flush()
            lock_handle(lock_file)
            child = subprocess.Popen(
                [harness.BIN, "get", "-C", lock_project,
                 f"{MODULE}@{TAG}"],
                env=lock_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                time.sleep(0.5)
                blocked = child.poll() is None
            finally:
                unlock_handle(lock_file)

        child_out, child_err = child.communicate(
            timeout=harness.DEFAULT_TIMEOUT)
        assert blocked, child_out + child_err
        assert child.returncode == 0, child_err

        # get/build take the cache lock shared, so an exclusive holder blocks them.
        cache_guard_path = lock_cache + ".lock"
        with open(cache_guard_path, "wb") as lock_file:
            lock_file.write(b"0")
            lock_file.flush()
            lock_handle(lock_file)
            child = subprocess.Popen(
                [harness.BIN, "get", "-C", lock_project,
                 f"{MODULE}@{TAG}"],
                env=lock_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                time.sleep(0.5)
                blocked = child.poll() is None
            finally:
                unlock_handle(lock_file)

        child_out, child_err = child.communicate(
            timeout=harness.DEFAULT_TIMEOUT)
        assert blocked, child_out + child_err
        assert child.returncode == 0, child_err

        # Transactional sums: a failed get must not publish earlier hashes.
        transaction_valid = os.path.join(root, "transaction-valid")
        os.makedirs(transaction_valid)
        git(transaction_valid, "init", "-q")
        git(transaction_valid, "config", "user.email", "test@example.com")
        git(transaction_valid, "config", "user.name", "Skein Test")
        write(
            transaction_valid,
            "module.skein",
            "module z {\n  export package json;\n}\n",
        )
        write(
            transaction_valid,
            "json.skein",
            "package json { export func answer() { return 1; } }\n",
        )
        git(transaction_valid, "add", "-A")
        git(transaction_valid, "commit", "-qm", "valid")
        git(transaction_valid, "tag", TAG)

        transaction_bad = os.path.join(root, "transaction-bad")
        os.makedirs(transaction_bad)
        git(transaction_bad, "init", "-q")
        git(transaction_bad, "config", "user.email", "test@example.com")
        git(transaction_bad, "config", "user.name", "Skein Test")
        write(transaction_bad, "README.md", "no module manifest\n")
        git(transaction_bad, "add", "-A")
        git(transaction_bad, "commit", "-qm", "bad")
        git(transaction_bad, "tag", TAG)

        transaction_project = os.path.join(root, "transaction-project")
        os.makedirs(transaction_project)
        init_transaction = run_skein(
            ["mod", "init", "-C", transaction_project, "example.test/txn"],
            env=env,
        )
        assert init_transaction.returncode == 0, init_transaction.stderr
        transaction_env = env.copy()
        transaction_env["SKEIN_MODULE_URL"] = (
            "example.test/z=" +
            os.path.abspath(transaction_valid).replace(os.sep, "/") +
            ",example.test/a=" +
            os.path.abspath(transaction_bad).replace(os.sep, "/"))
        transaction_env["SKEINMODCACHE"] = os.path.join(root, "txn-cache")
        failed = run_skein(
            ["get", "-C", transaction_project,
             f"example.test/z@{TAG}", f"example.test/a@{TAG}"],
            env=transaction_env,
        )
        assert failed.returncode != 0, failed.stdout
        with open(os.path.join(transaction_project, "skein.mod"),
                  encoding="utf-8") as handle:
            assert "require " not in handle.read()
        transaction_sum = os.path.join(transaction_project, "skein.sum")
        if os.path.exists(transaction_sum):
            with open(transaction_sum, encoding="utf-8") as handle:
                entries = handle.read()
            assert f"example.test/z {TAG} " in entries, entries
            assert "example.test/a " not in entries, entries

        # Successful downloads keep valid checksums, so retry needs no --force.
        retried = run_skein(
            ["get", "-C", transaction_project, f"example.test/z@{TAG}"],
            env=transaction_env,
        )
        assert retried.returncode == 0, retried.stderr
        assert os.path.exists(
            os.path.join(transaction_project, "skein.sum"))

        # `skein mod clean` only touches the cache root.
        clean_cache = os.path.join(root, "clean-cache")
        marker = write(clean_cache, "stale/marker", "x")
        clean_env = os.environ.copy()
        clean_env["SKEINMODCACHE"] = clean_cache
        dry = run_skein(["mod", "clean", "--dry-run"], env=clean_env)
        assert dry.returncode == 0, dry.stderr
        assert "would clean" in dry.stdout, dry.stdout
        assert os.path.exists(marker)

        # clean takes the cache exclusively, so it waits for active users.
        cache_lock_path = clean_cache + ".lock"
        with open(cache_lock_path, "wb") as lock_file:
            lock_file.write(b"0")
            lock_file.flush()
            lock_handle(lock_file)
            child = subprocess.Popen(
                [harness.BIN, "mod", "clean"],
                env=clean_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
            )
            try:
                time.sleep(0.5)
                blocked = child.poll() is None
            finally:
                unlock_handle(lock_file)

        child_out, child_err = child.communicate(
            timeout=harness.DEFAULT_TIMEOUT)
        assert blocked, child_out + child_err
        assert child.returncode == 0, child_err
        assert "cleaned" in child_out, child_out
        assert not os.path.exists(clean_cache)

        absent = run_skein(["mod", "clean"], env=clean_env)
        assert absent.returncode == 0, absent.stderr
        assert "nothing to clean" in absent.stdout, absent.stdout

        unsafe_env = os.environ.copy()
        unsafe_env["SKEINMODCACHE"] = os.getcwd()
        unsafe = run_skein(["mod", "clean"], env=unsafe_env)
        assert unsafe.returncode != 0, unsafe.stdout
        assert "refusing to clean unsafe cache root" in unsafe.stderr, (
            unsafe.stderr)
