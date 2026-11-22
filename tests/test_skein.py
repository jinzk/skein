#!/usr/bin/env python3
"""Golden/smoke tests for the skein interpreter.

The individual feature groups live in tests/cases/ and share tests/harness.py.
"""

import os

import harness

from cases import (
    auth,
    build,
    collections,
    decorators,
    diagnostics,
    ffi_real,
    fmt,
    get,
    http_api,
    include_files,
    io_scripts,
    key_semantics,
    language_core,
    lint,
    logging,
    lsp,
    match_types,
    meta,
    modules,
    net,
    paths,
    route,
    runtime,
    stdlib_builtins,
    task_errors,
    task_methods,
    tuple_semantics,
    typecheck,
    web,
)

MODULES = [
    auth,
    build,
    language_core,
    match_types,
    meta,
    modules,
    decorators,
    task_methods,
    collections,
    key_semantics,
    paths,
    route,
    runtime,
    diagnostics,
    task_errors,
    net,
    http_api,
    stdlib_builtins,
    ffi_real,
    fmt,
    get,
    include_files,
    io_scripts,
    lint,
    logging,
    lsp,
    tuple_semantics,
    typecheck,
    web,
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
