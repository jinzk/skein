#!/usr/bin/env python3
"""Check that the standard test suite performs no VM compile fallbacks."""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Known, intentional fallback paths in the current standard suite.  Each
# entry is a reason substring plus the reason it is accepted.
KNOWN_FALLBACKS = [
    ("switch case values contain conditions",
     "switch selector edge case intentionally handled by the AST backend"),
    ("bytecode verify: inconsistent stack depth",
     "break/continue inside call/match temporaries currently falls back"),
    ("default value cannot reference",
     "compile-time rejection test for field defaults"),
    ("cannot compile statement",
     "compile-time rejection test"),
]


def is_known(reason):
    return any(marker in reason for marker, _ in KNOWN_FALLBACKS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="build/skein")
    parser.add_argument("--check", action="store_true",
                        help="fail if any unexpected fallback reason is recorded")
    parser.add_argument("--strict", action="store_true",
                        help="treat every fallback as a failure")
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()

    binary = Path(args.bin)
    if not binary.is_absolute():
        binary = ROOT / binary
    if not binary.exists():
        print(f"missing binary: {binary}", file=sys.stderr)
        return 2

    with tempfile.NamedTemporaryFile(prefix="skein-fallback-", delete=False) as f:
        fallback_path = f.name
    try:
        env = dict(os.environ)
        env["SKEIN_VM"] = "1"
        env["SKEIN_VM_FALLBACK_FILE"] = fallback_path
        env.pop("SKEIN_VM_STATS_QUIET", None)
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
            print("test suite failed; fallback check incomplete", file=sys.stderr)
            return 1

        reasons = []
        try:
            with open(fallback_path, encoding="utf-8") as f:
                reasons = [line.rstrip("\n") for line in f if line.strip()]
        except FileNotFoundError:
            pass

        if reasons:
            known = [reason for reason in reasons
                     if not args.strict and is_known(reason)]
            unexpected = [reason for reason in reasons
                          if args.strict or not is_known(reason)]
            print(f"VM fallbacks: {len(reasons)} "
                  f"({len(known)} known, {len(unexpected)} unexpected)")
            for reason in known[:50]:
                print(f"  known: {reason}")
            for reason in unexpected[:50]:
                print(f"  unexpected: {reason}")
            if args.check and unexpected:
                return 1
        else:
            print("VM fallback check: 0 fallbacks")
        return 0
    finally:
        try:
            os.unlink(fallback_path)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    sys.exit(main())
