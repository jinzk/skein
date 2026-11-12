#!/usr/bin/env python3
"""Validate Skein module manifests without executing package code.

Usage:
    python tools/check_modules.py [project-root] [--list]

Checks, for every module.skein found under the root:
- module name matches the containing directory;
- the module root does not contain <module>.skein or index.skein;
- every export target resolves to a package file.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

EXPORT_RE = re.compile(
    r"export\s+(package\s+)?([A-Za-z_][A-Za-z0-9_.]*"
    r"(?:\s+as\s+[A-Za-z_][A-Za-z0-9_]*)?)\s*;")
MODULE_RE = re.compile(r"module\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def find_modules(root: Path):
    return sorted(root.rglob("module.skein"))


def target_exists(root: Path, package_path: str) -> bool:
    relative = Path(*package_path.split("."))
    file_path = root / relative.with_suffix(".skein")
    return file_path.is_file()


def nested_modules(root: Path):
    return sorted(
        path for path in root.rglob("module.skein")
        if path.parent != root
    )


def check_manifest(path: Path, list_only: bool):
    root = path.parent
    text = strip_comments(path.read_text(encoding="utf-8"))
    match = MODULE_RE.search(text)
    if not match:
        return [f"{path}: missing 'module <name> {{' declaration"]
    name = match.group(1)
    errors = []
    if name != root.name:
        errors.append(
            f"{path}: module '{name}' does not match directory '{root.name}'")
    if name != "module" and (root / f"{name}.skein").is_file():
        errors.append(
            f"{path}: module root contains package file '{name}.skein'")
    if (root / "index.skein").is_file():
        errors.append(f"{path}: module root contains index.skein")
    nested = nested_modules(root)
    if nested:
        errors.append(
            f"{path}: nested module.skein is not supported: {nested[0]}")

    exports = []
    for export in EXPORT_RE.finditer(text):
        is_package = bool(export.group(1))
        spec = export.group(2)
        parts = re.split(r"\s+as\s+", spec, maxsplit=1)
        target = parts[0].strip()
        alias = parts[1].strip() if len(parts) == 2 else target.rsplit(
            ".", 1)[-1]
        if is_package:
            package_path = target
        else:
            if "." not in target:
                errors.append(
                    f"{path}: symbol export '{target}' must be "
                    "<package>.<symbol>")
                continue
            package_path = target.rsplit(".", 1)[0]
        if not target_exists(root, package_path):
            errors.append(
                f"{path}: export '{alias}' target package "
                f"'{package_path}' was not found")
        exports.append((is_package, target, alias))
    if list_only:
        for is_package, target, alias in exports:
            kind = "package" if is_package else "symbol"
            print(f"{name}.{alias}\t{kind}\t{target}")
    return errors


def main(argv):
    list_only = "--list" in argv
    args = [arg for arg in argv[1:] if arg != "--list"]
    root = Path(args[0]) if args else Path(".")
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 2
    manifests = find_modules(root)
    if not manifests:
        print(f"no module.skein found under {root}", file=sys.stderr)
        return 1
    errors = []
    for path in manifests:
        errors.extend(check_manifest(path, list_only))
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    if not list_only:
        print(f"module manifests ok: {len(manifests)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
