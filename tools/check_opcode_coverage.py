#!/usr/bin/env python3
"""Report VM opcode coverage from the standard test suite.

The interpreter appends each process's nonzero opcode counters to the file
named by SKEIN_VM_COVERAGE_FILE.  This tool runs tests/test_skein.py with that
environment and aggregates the result against the Op enum in bytecode.h.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BYTECODE_H = ROOT / "src" / "interpreter" / "bytecode.h"
EXEMPT = {
    # Nop is removed by the optimizer and never needs a source-level case.
    "Nop",
    # Legacy opcodes retained for verifier/disassembly compatibility; the
    # current compiler emits GetIndexCached / GetFieldName instead.
    "GetIndex",
    "GetField",
    # Dynamic control fallback for invalid/edge control flow; the parser
    # rejects source-level continue outside a loop.
    "ControlContinue",
}


def opcode_names():
    text = BYTECODE_H.read_text(encoding="utf-8")
    match = re.search(r"enum class Op : uint8_t \{(.*?)\n\};", text, re.S)
    if not match:
        raise SystemExit(f"cannot find Op enum in {BYTECODE_H}")
    names = []
    for line in match.group(1).splitlines():
        line = line.split("//", 1)[0].strip()
        if not line or line.startswith("Count"):
            continue
        item = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*[^,]+)?,?$",
                        line)
        if item:
            names.append(item.group(1))
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="build/skein")
    parser.add_argument("--check", action="store_true",
                        help="fail if any non-exempt opcode is uncovered")
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()

    binary = Path(args.bin)
    if not binary.is_absolute():
        binary = ROOT / binary
    if not binary.exists():
        print(f"missing binary: {binary}", file=sys.stderr)
        return 2

    expected = opcode_names()
    with tempfile.NamedTemporaryFile(prefix="skein-opcodes-", delete=False) as f:
        coverage_path = f.name
    try:
        env = dict(os.environ)
        env["SKEIN_VM"] = "1"
        env["SKEIN_VM_STATS"] = "1"
        env["SKEIN_VM_STATS_QUIET"] = "1"
        env["SKEIN_VM_COVERAGE_FILE"] = coverage_path
        env.setdefault("SKEIN_TEST_TIMEOUT", "120")
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "test_skein.py"),
             str(binary)],
            cwd=ROOT,
            env=env,
            text=True,
            capture_output=True,
            timeout=args.timeout,
        )
        if proc.returncode != 0:
            print(proc.stdout, file=sys.stderr)
            print(proc.stderr, file=sys.stderr)
            print("test suite failed; coverage is incomplete", file=sys.stderr)
            return 1

        covered = set()
        try:
            with open(coverage_path, encoding="utf-8") as f:
                for line in f:
                    parts = line.split()
                    if parts:
                        covered.add(int(parts[0]))
        except FileNotFoundError:
            pass

        missing = [expected[i] for i in range(len(expected))
                   if i not in covered and expected[i] not in EXEMPT]
        print(f"opcode coverage: {len(expected) - len(missing)}"
              f"/{len(expected)}")
        if missing:
            print("uncovered:")
            for name in missing:
                print(f"  {name}")
            if args.check:
                return 1
        elif args.check:
            print("opcode coverage check passed")
        return 0
    finally:
        try:
            os.unlink(coverage_path)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    sys.exit(main())
