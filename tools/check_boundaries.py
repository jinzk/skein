#!/usr/bin/env python3
"""Check the runtime/interpreter/builtins dependency boundaries.

Two checks:

1. Source-level: runtime sources must not include evaluator/parser/CLI headers.
2. Link-level (when `nm` is available): symbols undefined in libskein_runtime
   must not be provided by libskein_interpreter or libskein_builtins.
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

RUNTIME_HEADERS = [
    "valuetype.h", "value_ops.h", "dict.h", "structtype.h", "symbol.h",
    "cycle.h", "serialization.h", "json.h", "format.h", "encoding.h",
    "unicode.h", "unicode_ident.h", "runtime_context.h", "runtime_state.h",
]

FORBIDDEN_RUNTIME_INCLUDES = {
    "eval.h", "eval_context.h", "eval_helpers.h", "ast.h", "parser.hpp",
    "package.h", "repl.h", "builtins.h", "diagnostics.h", "main.cpp",
}

RUNTIME_TARGETS = ("skein_runtime", "skein_platform", "skein_utf8proc")


def cmake_sources(cmake_text, target):
    match = re.search(
        rf"add_library\({re.escape(target)} STATIC(.*?)\n\)", cmake_text, re.S)
    if not match:
        return []
    sources = []
    for line in match.group(1).splitlines():
        line = line.strip()
        if line.endswith(".cpp"):
            sources.append(line)
    return sources


def scan_includes(path, errors):
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        errors.append(f"cannot read {path}: {exc}")
        return
    for match in re.finditer(r'^\s*#\s*include\s+"([^"]+)"', text, re.M):
        include = match.group(1)
        if include in FORBIDDEN_RUNTIME_INCLUDES:
            errors.append(f"{path}: runtime file includes {include}")


def nm_symbols(archive, defined, errors):
    if not archive.exists():
        return set()
    command = ["nm", "-C", "--defined-only" if defined else "-u", str(archive)]
    try:
        proc = subprocess.run(command, capture_output=True, text=True)
    except OSError as exc:
        errors.append(f"cannot run nm: {exc}")
        return set()
    if proc.returncode != 0:
        errors.append(f"nm failed for {archive}: {proc.stderr.strip()}")
        return set()
    symbols = set()
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 2)
        if defined:
            if len(parts) == 3:
                symbols.add(parts[2])
        else:
            # "                 U symbol"
            if len(parts) >= 2 and parts[0] == "U":
                symbols.add(parts[1])
    return set(symbols)


def check_link_boundary(build_dir, errors, warnings):
    if shutil.which("nm") is None:
        warnings.append("nm not found; skipping link-level boundary check")
        return
    runtime_archive = build_dir / "libskein_runtime.a"
    if not runtime_archive.exists():
        warnings.append("libskein_runtime.a not built; skipping link check")
        return

    runtime_defined = set()
    for target in RUNTIME_TARGETS:
        runtime_defined |= nm_symbols(build_dir / f"lib{target}.a", True, errors)
    runtime_undefined = nm_symbols(runtime_archive, False, errors)
    interp_defined = nm_symbols(
        build_dir / "libskein_interpreter.a", True, errors)
    builtins_defined = nm_symbols(
        build_dir / "libskein_builtins.a", True, errors)

    leaked = (runtime_undefined & (interp_defined | builtins_defined))
    leaked -= runtime_defined
    for symbol in sorted(leaked):
        errors.append(
            f"runtime depends on upper-layer symbol: {symbol}")

    interpreter_undefined = nm_symbols(
        build_dir / "libskein_interpreter.a", False, errors)
    if "call_builtin" in interpreter_undefined or any(
            name.startswith("call_builtin(") for name in interpreter_undefined):
        errors.append(
            "interpreter must call builtins through skein_context, not call_builtin")

    bytecode_archive = build_dir / "libskein_bytecode.a"
    if bytecode_archive.exists():
        bytecode_undefined = nm_symbols(bytecode_archive, False, errors)
        leaked = (bytecode_undefined & (interp_defined | builtins_defined))
        leaked -= runtime_defined
        for symbol in sorted(leaked):
            errors.append(
                f"skein_bytecode depends on upper-layer symbol: {symbol}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", default=".")
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args()

    source_dir = Path(args.source_dir).resolve()
    build_dir = Path(args.build_dir).resolve()
    cmake = source_dir / "CMakeLists.txt"
    errors = []
    warnings = []

    try:
        cmake_text = cmake.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"boundary check: cannot read {cmake}: {exc}", file=sys.stderr)
        return 1

    runtime_sources = cmake_sources(cmake_text, "skein_runtime")
    if not runtime_sources:
        errors.append("cannot find skein_runtime sources in CMakeLists.txt")
    for name in runtime_sources:
        scan_includes(source_dir / name, errors)
    runtime_dirs = (source_dir / "src/runtime", source_dir / "src/platform")
    for name in RUNTIME_HEADERS:
        path = next(
            (d / name for d in runtime_dirs if (d / name).exists()),
            runtime_dirs[0] / name,
        )
        if not path.exists():
            errors.append(f"missing runtime header: {name}")
            continue
        scan_includes(path, errors)

    check_link_boundary(build_dir, errors, warnings)

    for message in warnings:
        print(f"boundary check: warning: {message}", file=sys.stderr)
    if errors:
        for message in errors:
            print(f"boundary check: {message}", file=sys.stderr)
        return 1
    print("boundary check: runtime/interpreter/builtins boundaries OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
