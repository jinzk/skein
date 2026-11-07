#!/usr/bin/env python3
"""Golden/smoke tests for the skein interpreter.

The individual feature groups live in tests/cases/ and share tests/harness.py.
"""

import os

import harness

from cases import (
    async_errors,
    async_methods,
    collections,
    decorators,
    diagnostics,
    ffi_real,
    io_scripts,
    language_core,
    match_types,
    meta,
    net,
    paths,
    route,
    runtime,
    stdlib_builtins,
    typecheck,
)

MODULES = [
    language_core,
    match_types,
    meta,
    decorators,
    async_methods,
    collections,
    paths,
    route,
    runtime,
    diagnostics,
    async_errors,
    net,
    stdlib_builtins,
    ffi_real,
    io_scripts,
    typecheck,
]


SKIP_MODULES = {
    item.strip()
    for item in os.environ.get("SKEIN_TEST_SKIP_MODULES", "").split(",")
    if item.strip()
}

ONLY_MODULES = {
    item.strip()
    for item in os.environ.get("SKEIN_TEST_ONLY_MODULES", "").split(",")
    if item.strip()
}


def main():
    for module in MODULES:
        name = module.__name__.rsplit(".", 1)[-1]
        if name in SKIP_MODULES:
            continue
        if ONLY_MODULES and name not in ONLY_MODULES:
            continue
        module.run_cases()
    print(f"all tests passed for {harness.BIN}")


if __name__ == "__main__":
    main()
