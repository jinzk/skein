#!/usr/bin/env python3
"""Golden/smoke tests for the skein interpreter.

The individual feature groups live in tests/cases/ and share tests/harness.py.
"""

import harness

from cases import (
    async_errors,
    collections,
    diagnostics,
    ffi_real,
    io_scripts,
    language_core,
    match_types,
    net,
    runtime,
    stdlib_builtins,
)

MODULES = [
    language_core,
    match_types,
    collections,
    runtime,
    diagnostics,
    async_errors,
    net,
    stdlib_builtins,
    ffi_real,
    io_scripts,
]


def main():
    for module in MODULES:
        module.run_cases()
    print(f"all tests passed for {harness.BIN}")


if __name__ == "__main__":
    main()
