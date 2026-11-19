#!/usr/bin/env python3
"""CLI tests for source-distributed native modules (`skein build`)."""

import glob
import os
import shutil
import subprocess
import tempfile

import harness

MODULE = "example.test/nativeutil"
TAG = "v1.0.0"

SOURCE = r'''
#include "skein_module.h"

namespace {

const skein_api *g_api = nullptr;

skein_status twice(skein_context *ctx, const skein_value *const *args,
                   size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "twice expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "twice expects an integer");
    *result = g_api->new_int(ctx, value * 2);
    return SKEIN_OK;
}

const skein_function functions[] = {{"twice", twice}};

skein_status init(skein_context *, const skein_api *api) {
    g_api = api;
    return SKEIN_OK;
}

void fini(skein_context *) {}

const skein_module module = {
    SKEIN_MODULE_ABI_VERSION, "nativeutil", init, fini, functions, 1,
    nullptr, 0, nullptr, 0,
};

} // namespace

SKEIN_MODULE_EXPORT const skein_module *skein_module_descriptor(void) {
    return &module;
}
'''


def write(root, rel, text):
    path = os.path.join(root, rel) if rel else root
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return path


def git(repo, *args):
    proc = subprocess.run(["git", "-C", repo, *args], text=True,
                          capture_output=True, timeout=harness.DEFAULT_TIMEOUT)
    assert proc.returncode == 0, proc.stderr


def run_skein(args, env=None, cwd=None, timeout=None):
    return subprocess.run([harness.BIN, *args], text=True, encoding="utf-8",
                          capture_output=True, env=env, cwd=cwd,
                          timeout=timeout or harness.DEFAULT_TIMEOUT)


def run_cases():
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not compiler or shutil.which("git") is None:
        return

    skein_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..",
                                              ".."))
    with tempfile.TemporaryDirectory() as root:
        repo = os.path.join(root, "repo")
        os.makedirs(repo)
        git(repo, "init", "-q")
        git(repo, "config", "user.email", "test@example.com")
        git(repo, "config", "user.name", "Skein Test")
        write(repo, "module.skein",
              "module nativeutil {\n"
              "  export package api;\n"
              "  native {\n"
              '    sources "native/add.cpp";\n'
              '    output "nativeutil";\n'
              "  }\n"
              "}\n")
        write(repo, "api.skein",
              "package api {\n"
              "  import std;\n"
              '  std.plugin.load(std.module.native("' + MODULE + '"));\n'
              "  export func marker() { return 1; }\n"
              "}\n")
        write(repo, "native/add.cpp", SOURCE)
        header = os.path.join(skein_root, "src", "runtime", "skein_module.h")
        shutil.copyfile(header, os.path.join(repo, "native", "skein_module.h"))
        git(repo, "add", "-A")
        git(repo, "commit", "-qm", "initial")
        git(repo, "tag", TAG)

        repo_url = os.path.abspath(repo).replace(os.sep, "/")
        env = os.environ.copy()
        env["SKEIN_MODULE_URL"] = repo_url
        env["SKEINMODCACHE"] = os.path.join(root, "cache")
        env["SKEIN_CXX"] = compiler

        project = os.path.join(root, "project")
        os.makedirs(project)
        init = run_skein(["mod", "init", "-C", project, "example.test/app"], env)
        assert init.returncode == 0, init.stderr
        fetched = run_skein(["get", "-C", project, MODULE + "@" + TAG], env)
        assert fetched.returncode == 0, fetched.stderr

        main = write(project, "main.skein",
                     'import "' + MODULE + '/api";\n'
                     "import nativeutil;\n"
                     "println(nativeutil.twice(21));\n")

        # Before building there is no artifact.
        missing = run_skein([main], dict(env, SKEIN_ALLOW_NATIVE="1"),
                            cwd=project)
        assert missing.returncode == 1, missing.stdout
        assert "run 'skein build" in missing.stderr, missing.stderr

        built = run_skein(["build", "-C", project, MODULE + "@" + TAG], env)
        assert built.returncode == 0, built.stderr
        artifacts = [name for name in os.listdir(os.path.join(
            env["SKEINMODCACHE"], "example.test", TAG, ".build"))
            if name.startswith("win-") or name.startswith("linux-") or
               name.startswith("macos-")]
        assert artifacts, built.stdout
        assert "built " in built.stdout, built.stdout

        build_dirs = glob.glob(os.path.join(
            env["SKEINMODCACHE"], "example.test", TAG, ".build", "*",
            "nativeutil"))
        assert build_dirs, built.stdout
        metadata = os.path.join(build_dirs[0], "artifact.meta")
        assert os.path.isfile(metadata), built.stdout
        with open(metadata, encoding="utf-8") as handle:
            metadata_text = handle.read()
        assert "source_hash=" in metadata_text, metadata_text
        assert "artifact_hash=" in metadata_text, metadata_text
        assert "compiler_version=" in metadata_text, metadata_text
        assert "build_flags=" in metadata_text, metadata_text
        assert "build_command_hash=" in metadata_text, metadata_text
        artifacts = glob.glob(os.path.join(build_dirs[0], "*.dll")) + \
            glob.glob(os.path.join(build_dirs[0], "*.so")) + \
            glob.glob(os.path.join(build_dirs[0], "*.dylib"))
        assert len(artifacts) == 1, artifacts
        artifact = artifacts[0]
        with open(artifact, "rb") as handle:
            first_bytes = handle.read()

        # The source checksum still describes source only, and two builds
        # produce identical bytes.
        run_again = run_skein(["build", "-C", project], env)
        assert run_again.returncode == 0, run_again.stderr
        with open(artifact, "rb") as handle:
            assert handle.read() == first_bytes, "native build is not reproducible"

        allowed = dict(env, SKEIN_ALLOW_NATIVE="1")
        result = run_skein([main], allowed, cwd=project)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "42", result.stdout

        # A tampered artifact must not load, even with native allowed.
        with open(artifact, "ab") as handle:
            handle.write(b"tampered")
        tampered = run_skein([main], allowed, cwd=project)
        assert tampered.returncode == 1, tampered.stdout
        assert "run 'skein build" in tampered.stderr, tampered.stderr

        rebuilt = run_skein(["build", "-C", project, MODULE + "@" + TAG], env)
        assert rebuilt.returncode == 0, rebuilt.stderr
        restored = run_skein([main], allowed, cwd=project)
        assert restored.returncode == 0, restored.stderr
        assert restored.stdout.strip() == "42", restored.stdout

        # Metadata is part of the artifact trust boundary.
        os.remove(metadata)
        unverified = run_skein([main], allowed, cwd=project)
        assert unverified.returncode == 1, unverified.stdout
        assert "run 'skein build" in unverified.stderr, unverified.stderr
        rebuilt_again = run_skein(
            ["build", "-C", project, MODULE + "@" + TAG], env)
        assert rebuilt_again.returncode == 0, rebuilt_again.stderr

        denied = run_skein([main], env, cwd=project)
        assert denied.returncode == 1, denied.stdout
        assert "SKEIN_ALLOW_NATIVE" in denied.stderr, denied.stderr
