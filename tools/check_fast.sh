#!/usr/bin/env bash
#
# Fast development regression loop.
#
# The full matrix (Debug/Release/ASan+UBSan/LSan/VM-only) lives in
# tools/check_all.sh.  This script is for the inner loop: incremental build
# plus the core labeled tests, without benchmarks or sanitizer runs.
#
# Usage:
#   tools/check_fast.sh                 # core tests in ./build
#   tools/check_fast.sh --vm-only       # VM suite + differential only
#   tools/check_fast.sh --ast-only      # AST/reference suite only
#   tools/check_fast.sh --bench         # core + benchmarks
#   tools/check_fast.sh --docs          # docs / data checks
#   tools/check_fast.sh --coverage      # opcode coverage check
#   tools/check_fast.sh --fallback      # VM fallback regression check
#   tools/check_fast.sh --no-build      # skip the build step
#   tools/check_fast.sh --build-dir DIR --jobs N
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="build"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
MODE="core"
DO_BUILD=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --no-build)
            DO_BUILD=0
            shift
            ;;
        --bench)
            MODE="bench"
            shift
            ;;
        --docs)
            MODE="docs"
            shift
            ;;
        --coverage)
            MODE="coverage"
            shift
            ;;
        --fallback)
            MODE="fallback"
            shift
            ;;
        --vm-only)
            MODE="vm"
            shift
            ;;
        --ast-only)
            MODE="ast"
            shift
            ;;
        -h|--help)
            sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            exit 2
            ;;
    esac
done

if [[ "$DO_BUILD" == "1" ]]; then
    printf '==> incremental build (%s)\n' "$BUILD_DIR"
    cmake --build "$BUILD_DIR" --parallel "$JOBS"
fi

start="$(date +%s)"
case "$MODE" in
    core)
        printf '==> core tests (no benchmarks, no sanitizers)\n'
        ctest --test-dir "$BUILD_DIR" -L core -j "$JOBS" \
            --output-on-failure
        ;;
    bench)
        printf '==> core + benchmark tests\n'
        ctest --test-dir "$BUILD_DIR" -L 'core|bench' -j "$JOBS" \
            --output-on-failure
        ;;
    docs)
        printf '==> docs / data checks\n'
        ctest --test-dir "$BUILD_DIR" -L 'docs|data' -j "$JOBS" \
            --output-on-failure
        ;;
    coverage)
        printf '==> opcode coverage\n'
        python3 "$ROOT/tools/check_opcode_coverage.py" \
            --bin "$BUILD_DIR/skein" --check
        ;;
    fallback)
        printf '==> VM fallback regression\n'
        python3 "$ROOT/tools/check_vm_fallback.py" \
            --bin "$BUILD_DIR/skein" --check
        ;;
    vm)
        printf '==> VM tests + differential\n'
        ctest --test-dir "$BUILD_DIR" -j "$JOBS" --output-on-failure \
            -R '^(bytecode_verifier_test|skein_vm_tests|vm_differential_test)$'
        ;;
    ast)
        printf '==> AST/reference tests\n'
        ctest --test-dir "$BUILD_DIR" -j "$JOBS" --output-on-failure \
            -R '^(bytecode_verifier_test|skein_tests)$'
        ;;
esac

elapsed=$(( $(date +%s) - start ))
printf '\nfast checks passed in %ss. Run tools/check_all.sh before commit.\n' \
    "$elapsed"
