#!/usr/bin/env python3
"""Keep docs/stdlib.md, SYNTAX.md, and the builtin registry in sync.

Checks:
  1. docs/stdlib.md and SYNTAX expose the same std module list, in the same
     order.
  2. Every embedded package listed in CMakeLists.txt is documented in both
     files and registered in package.cpp.
  3. Every builtin function registered in package.cpp or std_namespace.cpp
     appears in the stdlib function list.
  4. Every stdlib function-list entry belongs to a known module and is
     actually registered.

Exit status is non-zero when any check fails.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STDLIB = ROOT / "docs" / "stdlib.md"
SYNTAX = ROOT / "SYNTAX.md"
CMAKE = ROOT / "CMakeLists.txt"
PACKAGE = ROOT / "src/interpreter/package.cpp"
STD_NAMESPACE = ROOT / "src/interpreter/std_namespace.cpp"

errors = []


def fail(message):
    errors.append(message)


def read(path):
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"cannot read {path}: {exc}")
        return ""


def module_list(text, label):
    match = re.search(r"模块：\s*```text\n(.*?)```", text, re.S)
    if not match:
        fail(f"{label}: cannot find the std module list")
        return []
    modules = []
    for line in match.group(1).splitlines():
        line = line.strip()
        entry = re.match(r"(std\.[a-z0-9_.]+)", line)
        if entry:
            modules.append(entry.group(1))
    return modules


def embedded_packages(cmake_text):
    return re.findall(r"^\s*([a-z0-9_]+)\.skein:", cmake_text, re.M)


def builtin_registry(package_text):
    registry = {}
    pattern = re.compile(
        r'newstrval\("([a-z0-9_]+)"\),\s*\n\s*builtin_module\(\{(.*?)\}\)\)',
        re.S,
    )
    for match in pattern.finditer(package_text):
        module = match.group(1)
        functions = re.findall(
            r'\{"([a-z0-9_]+)",\s*B_[a-z0-9_]+\}', match.group(2))
        registry[module] = set(functions)

    # Some modules are assembled into local variables first, for example
    # `value_t *math_module = builtin_module({...});`.
    variable_pattern = re.compile(
        r"(\w+)_module\s*=\s*(?:\(value_t \*\))?builtin_module\(\{(.*?)\}\);",
        re.S)
    for match in variable_pattern.finditer(package_text):
        module = match.group(1)
        functions = re.findall(
            r'\{"([a-z0-9_]+)",\s*B_[a-z0-9_]+\}', match.group(2))
        registry.setdefault(module, set()).update(functions)
    return registry


# std.io also exports stdin/stdout/stderr values, not functions.
NON_FUNCTION_EXPORTS = {
    "io": {"stdin", "stdout", "stderr"},
}


def function_list(stdlib_text):
    marker = "当前提供的函数："
    start = stdlib_text.find(marker)
    if start < 0:
        fail("docs/stdlib.md: cannot find the function list")
        return {}
    block = stdlib_text[start:]
    entries = re.findall(
        r"^std\.([a-z0-9_]+)\.([a-z0-9_]+)\s*$", block, re.M)
    listed = {}
    for module, function in entries:
        listed.setdefault(module, set()).add(function)
    return listed


def main():
    stdlib_text = read(STDLIB)
    syntax_text = read(SYNTAX)
    cmake_text = read(CMAKE)
    package_text = read(PACKAGE)
    std_namespace_text = read(STD_NAMESPACE)

    stdlib_modules = module_list(stdlib_text, "docs/stdlib.md")
    syntax_modules = module_list(syntax_text, "SYNTAX.md")

    if stdlib_modules != syntax_modules:
        only_stdlib = [m for m in stdlib_modules if m not in syntax_modules]
        only_syntax = [m for m in syntax_modules if m not in stdlib_modules]
        if only_stdlib:
            fail("docs/stdlib.md module list has entries missing from SYNTAX: "
                 + ", ".join(only_stdlib))
        if only_syntax:
            fail("SYNTAX module list has entries missing from docs/stdlib.md: "
                 + ", ".join(only_syntax))
        if not only_stdlib and not only_syntax:
            fail("docs/stdlib.md and SYNTAX module lists are in a different order")

    registry = builtin_registry(package_text)
    for module, functions in builtin_registry(std_namespace_text).items():
        registry.setdefault(module, set()).update(functions)
    listed = function_list(stdlib_text)

    for package in embedded_packages(cmake_text):
        module = f"std.{package}"
        if module not in stdlib_modules:
            fail(f"embedded package {module} missing from docs/stdlib.md "
                 "module list")
        if module not in syntax_modules:
            fail(f"embedded package {module} missing from SYNTAX module list")
        if f'"std.{package}"' not in package_text:
            fail(f"embedded package {module} missing from package.cpp registry")

    for module, functions in sorted(registry.items()):
        documented = listed.get(module, set())
        missing = sorted(functions - documented)
        if missing:
            fail(f"docs/stdlib.md function list missing std.{module}."
                 + f"{{{','.join(missing)}}}")

    for module, functions in sorted(listed.items()):
        if module not in registry:
            fail(f"docs/stdlib.md documents unknown std module: std.{module}")
            continue
        stale = sorted(
            functions - registry[module]
            - NON_FUNCTION_EXPORTS.get(module, set()))
        if stale:
            fail(f"docs/stdlib.md documents unregistered std.{module}."
                 + f"{{{','.join(stale)}}}")

    if errors:
        for message in errors:
            print(f"docs sync: {message}", file=sys.stderr)
        return 1

    print("docs sync: docs/stdlib.md, SYNTAX, CMake, and package registry agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
