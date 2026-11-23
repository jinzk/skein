# Skein test cases

Self-checking test files for language and stdlib behavior.  The Python suite
still owns the outer runner, timeouts, crash isolation, ctest sharding, and the
independent golden checks for output formatting and diagnostics.

## Running

```powershell
# One module through the normal Python entry point:
$env:SKEIN_TEST_ONLY_MODULES = "paths"
python tests/test_skein.py build-msys2/skein.exe

# One file directly:
build-msys2/skein.exe tests/skein/paths.skein

# One case while debugging:
$env:SKEIN_TEST_CASE = "path parse posix"
build-msys2/skein.exe tests/skein/paths.skein

# Skip by exact name or prefix:
$env:SKEIN_TEST_SKIP = "path parse posix"
$env:SKEIN_TEST_SKIP_PREFIX = "path parse "
```

`tests/harness.py:run_skein` runs each file in its own process with a timeout
(`SKEIN_FILE_TEST_TIMEOUT`, default 300 seconds) and requires the `ok:` marker
printed by `ts_finish()`.  A file that exits early without running anything is
reported as a failure.

The Python module for a migrated file stays as a thin shim, so the existing
ctest shards and `SKEIN_TEST_ONLY_MODULES` keep working:

```python
# tests/cases/paths.py
from harness import run_skein

def run_cases():
    run_skein("paths.skein")
```

`SKEIN_TEST_SKIP_SOURCE` only applies to Python `check` cases.  Skein cases
have no source string; use exact names via `SKEIN_TEST_SKIP` or prefixes via
`SKEIN_TEST_SKIP_PREFIX`.

## Writing a case

```skein
import std;
import std.path;
include "_support.skein";

ts_test("path parse posix", func() {
  let p = path.parse("/a/b/../c.txt", "posix");
  ts_eq(p["style"], "posix");
  ts_same(p["parts"], ["a", "b", "..", "c.txt"]);
});

ts_test("path wrong type", func() {
  ts_error(func() { return path.normalize(1); },
           "expects a path string");
});

ts_finish();
```

Helpers:

| Helper | Purpose |
|---|---|
| `ts_test(name, body)` | Run one case; unexpected errors are reported and the rest still run. |
| `ts_eq(got, want)` | Value equality. |
| `ts_same(got, want)` | Kind + value equality; use for `int` versus `float`. |
| `ts_true(value)`, `ts_false(value)` | Boolean checks. |
| `ts_contains(text, fragment)` | Substring check. |
| `ts_error(thunk, fragment)` | Expect a runtime error whose message contains `fragment`. |
| `ts_finish()` | Print the summary and exit non-zero on failure. |

## Rules

- One feature per file.  Use the Python module name for single-file modules
  (`paths.skein`); large modules may use `<module>_<area>.skein`
  (`collections_containers.skein`) with one Python shim.  Keep files under
  roughly 150 cases; split larger groups.
- Test names match the old Python `check` names so skip filters keep working.
- Declarations share the file's global scope.  Give structs, enums, and helper
  functions unique names, or keep type-heavy stateful cases in Python.
- Cases should be pure.  Leave cwd/env mutation, temp files, subprocesses,
  threads, sockets, FFI, REPL/pipe behavior, and parser/type-check diagnostics
  in the Python suite.
- Remove the Python `check` in the same change that adds the Skein case.  Do not
  run both versions.
- Keep at least one Python golden case per migrated module for output/error
  formatting, so `==` / `str` bugs cannot hide behind self-consistent
  assertions.

## Migration ledger

| Module | Status | Notes |
|---|---|---|
| `paths` | Skein | 29 Skein cases; 1 Python golden for list/string formatting. |
| `route` | Skein | 32 Skein cases; 1 Python golden for dispatch output. |
| `key_semantics` | Skein (non-thread) | 22 Skein cases; 5 thread/transfer cases and 1 golden stay in Python for TSan. |
| `collections` (set/deque/heap) | Skein (partial) | 24 Skein cases in `collections_containers.skein`; the rest stay in Python. |
| `collections` (graph/lru/list utilities) | Skein (partial) | 35 Skein cases in `collections_more.skein`; the rest stay in Python. |
| `meta` | Skein (pure) | 13 Skein cases; package-visibility tests and 1 golden stay in Python. |
| `language_core` (numeric literals/operators) | Skein (partial) | 48 Skein cases in `language_core_numbers.skein`; parse errors, REPL, and control-flow cases stay in Python. |
| `language_core` (for-in / for_each) | Skein (partial) | 26 Skein cases in `language_core_iteration.skein`; syntax/type-error cases stay in Python. |
| `language_core` (logical/bitwise/bool/ternary) | Skein (partial) | 45 Skein cases in `language_core_operators.skein`; print-format goldens stay in Python. |
| `language_core` (switch) | Skein (partial) | 21 Skein cases in `language_core_switch.skein`; invalid-case diagnostics stay in Python. |
| `language_core` (match) | Skein (partial) | 25 Skein cases in `language_core_match.skein`; binding/compile diagnostics stay in Python. |
| `language_core` (increment/compound/slice) | Skein (partial) | 20 Skein cases in `language_core_updates.skein`; update error diagnostics stay in Python. |
| `language_core` (default parameters/closures) | Skein (partial) | 15 Skein cases in `language_core_defaults.skein`; default-parameter diagnostics stay in Python. |
| `language_core` (lambdas/pipe/placeholders/string append) | Skein (partial) | 21 Skein cases in `language_core_functions.skein`; placeholder syntax errors and append errors stay in Python. |
| `language_core` (traits) | Skein (partial) | 6 Skein cases in `language_core_traits.skein`; trait conflict diagnostics stay in Python. |
| `language_core` (control-flow headers) | Skein (partial) | 12 Skein cases in `language_core_headers.skein`; brace/paren syntax errors stay in Python. |
| `decorators` | Skein (partial) | 21 Skein cases in `decorators_behavior.skein`; generator/typecheck/temp-package cases stay in Python. |
| `collections` (arena/builder/append/dict equality) | Skein (partial) | 12 Skein cases in `collections_mutation.skein`; error diagnostics stay in Python. |
| `match_types` (sorting) | Skein (partial) | 13 Skein cases in `match_types_sorting.skein`; sort error diagnostics stay in Python. |
| `match_types` (char/byte) | Skein (partial) | 22 Skein cases in `match_types_char_byte.skein`; literal/conversion diagnostics stay in Python. |
| `match_types` (enum/struct decode) | Skein (partial) | 14 Skein cases in `match_types_decode.skein`; decode diagnostics and temp-package tests stay in Python. |
| `task_methods` | Skein (partial) | 22 Skein cases in `task_methods_basic.skein` and `task_methods_launch.skein`; GC/error/temp-package cases stay in Python. |
| `runtime` (strings) | Skein (partial) | 23 Skein cases in `runtime_strings.skein`; string error/arity diagnostics stay in Python. |
| `runtime` (range) | Skein (partial) | 6 Skein cases in `runtime_range.skein`; range error diagnostics stay in Python. |
| `runtime` (list/dict copy-on-write) | Skein (partial) | 31 Skein cases in `runtime_collections.skein`; access/assignment diagnostics stay in Python. |
| `runtime` (len/list/math) | Skein (partial) | 16 Skein cases in `runtime_values.skein`; math/len error diagnostics stay in Python. |
| `runtime` (loop/control flow) | Skein (partial) | 20 Skein cases in `runtime_control.skein`; loop/range/import diagnostics stay in Python. |
| `runtime` (const/value semantics) | Skein (partial) | 8 Skein cases in `runtime_misc.skein`; const/package/error cases stay in Python. |

Current coverage: 602 Skein cases across the files above.

`diagnostics`, `typecheck`, `lint`, `fmt`, `lsp`, `build`, `get`, `net`,
`io_scripts`, `ffi_real`, stress/differential tests, and all host/REPL checks
stay in Python.
