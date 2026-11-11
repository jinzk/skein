#!/usr/bin/env python3
"""Keep README.md, SYNTAX.md, and the builtin registry in sync.

Checks:
  1. README and SYNTAX expose the same std module list, in the same order.
  2. Every embedded package listed in CMakeLists.txt is documented in both
     files and registered in package.cpp.
  3. Every builtin function registered in package.cpp or std_namespace.cpp
     appears in the README function list.
  4. Every README function-list entry belongs to a known module and is
     actually registered.

Exit status is non-zero when any check fails.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
README = ROOT / "README.md"
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

# LANG-2 keeps the old *_async names as deprecated aliases but documents only
# the *_task primary names.
DEPRECATED_ALIASES = {
    "process": {"run", "spawn"},
    "io": {"read_async", "write_async"},
    "net": {"tcp_connect_async", "tcp_listen_async", "accept_async",
            "send_async", "recv_async", "resolve_async", "shutdown_async"},
    "http": {"request_async", "get_async", "post_async", "accept_async",
             "respond_async"},
    "http2": {"request_async", "get_async", "post_async",
              "request_many_async"},
    "websocket": {"connect_async", "send_async", "receive_async"},
}


def function_list(readme_text):
    marker = "当前提供的函数："
    start = readme_text.find(marker)
    if start < 0:
        fail("README.md: cannot find the function list")
        return {}
    block = readme_text[start:]
    entries = re.findall(
        r"^std\.([a-z0-9_]+)\.([a-z0-9_]+)\s*$", block, re.M)
    listed = {}
    for module, function in entries:
        listed.setdefault(module, set()).add(function)
    return listed


def main():
    readme_text = read(README)
    syntax_text = read(SYNTAX)
    cmake_text = read(CMAKE)
    package_text = read(PACKAGE)
    std_namespace_text = read(STD_NAMESPACE)

    readme_modules = module_list(readme_text, "README.md")
    syntax_modules = module_list(syntax_text, "SYNTAX.md")

    if readme_modules != syntax_modules:
        only_readme = [m for m in readme_modules if m not in syntax_modules]
        only_syntax = [m for m in syntax_modules if m not in readme_modules]
        if only_readme:
            fail("README module list has entries missing from SYNTAX: "
                 + ", ".join(only_readme))
        if only_syntax:
            fail("SYNTAX module list has entries missing from README: "
                 + ", ".join(only_syntax))
        if not only_readme and not only_syntax:
            fail("README and SYNTAX module lists are in a different order")

    registry = builtin_registry(package_text)
    for module, functions in builtin_registry(std_namespace_text).items():
        registry.setdefault(module, set()).update(functions)
    listed = function_list(readme_text)

    for package in embedded_packages(cmake_text):
        module = f"std.{package}"
        if module not in readme_modules:
            fail(f"embedded package {module} missing from README module list")
        if module not in syntax_modules:
            fail(f"embedded package {module} missing from SYNTAX module list")
        if f'"std.{package}"' not in package_text:
            fail(f"embedded package {module} missing from package.cpp registry")

    for module, functions in sorted(registry.items()):
        documented = listed.get(module, set())
        aliases = DEPRECATED_ALIASES.get(module, set())
        missing = sorted(functions - documented - aliases)
        if missing:
            fail(f"README function list missing std.{module}."
                 + f"{{{','.join(missing)}}}")

    for module, functions in sorted(listed.items()):
        if module not in registry:
            fail(f"README documents unknown std module: std.{module}")
            continue
        stale = sorted(
            functions - registry[module]
            - NON_FUNCTION_EXPORTS.get(module, set()))
        if stale:
            fail(f"README documents unregistered std.{module}."
                 + f"{{{','.join(stale)}}}")

    if errors:
        for message in errors:
            print(f"docs sync: {message}", file=sys.stderr)
        return 1

    print("docs sync: README, SYNTAX, CMake, and package registry agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
