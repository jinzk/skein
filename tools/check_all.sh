#!/usr/bin/env bash
#
# Unified local/CI checks for the bytecode VM work.
#
# Covers:
#   - Debug build + core test suite (benchmarks run in Release)
#   - Release build + full test suite
#   - ASan + UBSan build (including the LSan test target)
#   - VM-only build (default backend without the AST reference path)
#   - runtime-only link check
#   - docs / boundary / whitespace checks
#   - optional -D_GLIBCXX_DEBUG build
#
# Usage:
#   tools/check_all.sh [options]
#
# Options:
#   --build-root DIR    Root for all build directories (default: build-ci)
#   --jobs N            Parallel build jobs (default: nproc)
#   --clean             Remove the build root before configuring
#   --skip-release      Skip the Release configuration
#   --skip-asan         Skip the sanitizer configuration
#   --skip-vm-only      Skip the VM-only configuration
#   --glibcxx-debug     Also run a Debug build with -D_GLIBCXX_DEBUG
#   -h, --help          Show this help
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_ROOT="build-ci"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
CLEAN=0
RUN_RELEASE=1
RUN_ASAN=1
RUN_VM_ONLY=1
RUN_GLIBCXX_DEBUG=0

usage() {
    sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-root)
            BUILD_ROOT="$2"
            shift 2
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --skip-release)
            RUN_RELEASE=0
            shift
            ;;
        --skip-asan)
            RUN_ASAN=0
            shift
            ;;
        --skip-vm-only)
            RUN_VM_ONLY=0
            shift
            ;;
        --glibcxx-debug)
            RUN_GLIBCXX_DEBUG=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$CLEAN" == "1" ]]; then
    rm -rf "$BUILD_ROOT"
fi
mkdir -p "$BUILD_ROOT"

GENERATOR=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR=(-G Ninja)
fi

# Keep fiber stacks large enough for sanitizer frames, and make the override
# visible to the generator/async test paths.
export SKEIN_FIBER_STACK_KB="${SKEIN_FIBER_STACK_KB:-8192}"

step() {
    printf '\n==> %s\n' "$1"
}

configure() {
    local dir="$1"
    shift
    cmake -S "$ROOT" -B "$dir" "${GENERATOR[@]}" \
        -DBUILD_TESTING=ON "$@"
}

build() {
    local dir="$1"
    shift
    cmake --build "$dir" --parallel "$JOBS" "$@"
}

test_dir() {
    local dir="$1"
    shift
    ctest --test-dir "$dir" --output-on-failure "$@"
}

step "Debug build"
DEBUG_DIR="$BUILD_ROOT/debug"
configure "$DEBUG_DIR" -DCMAKE_BUILD_TYPE=Debug
build "$DEBUG_DIR"

step "Runtime-only link check"
build "$DEBUG_DIR" --target runtime_link_test
"$DEBUG_DIR/runtime_link_test"

step "Debug tests (benchmarks are covered by Release)"
SKEIN_TEST_TIMEOUT=120 test_dir "$DEBUG_DIR" -E \
    'vm_bench_test|container_bench_test'

step "Docs / boundary / whitespace checks"
python3 tools/check_docs_sync.py
python3 tools/check_boundaries.py \
    --source-dir "$ROOT" --build-dir "$DEBUG_DIR"
python3 tools/gen_encoding_tables.py --check
git diff --check

if [[ "$RUN_RELEASE" == "1" ]]; then
    step "Release build"
    RELEASE_DIR="$BUILD_ROOT/release"
    configure "$RELEASE_DIR" -DCMAKE_BUILD_TYPE=Release
    build "$RELEASE_DIR"

    step "Release tests"
    test_dir "$RELEASE_DIR"
fi

if [[ "$RUN_ASAN" == "1" ]]; then
    step "ASan + UBSan build"
    ASAN_DIR="$BUILD_ROOT/asan"
    configure "$ASAN_DIR" -DCMAKE_BUILD_TYPE=Debug -DSKEIN_SANITIZE=ON
    build "$ASAN_DIR"

    step "ASan + UBSan / LSan tests"
    test_dir "$ASAN_DIR" -E 'container_bench_test'
fi

if [[ "$RUN_VM_ONLY" == "1" ]]; then
    step "VM-only build"
    VM_ONLY_DIR="$BUILD_ROOT/vm-only"
    configure "$VM_ONLY_DIR" -DCMAKE_BUILD_TYPE=Debug -DSKEIN_VM_ONLY=ON
    build "$VM_ONLY_DIR"

    step "VM-only tests"
    SKEIN_TEST_TIMEOUT=120 test_dir "$VM_ONLY_DIR" -E \
        'vm_bench_test|container_bench_test'
fi

if [[ "$RUN_GLIBCXX_DEBUG" == "1" ]]; then
    step "-D_GLIBCXX_DEBUG build"
    GLIBCXX_DIR="$BUILD_ROOT/glibcxx-debug"
    configure "$GLIBCXX_DIR" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-D_GLIBCXX_DEBUG"
    build "$GLIBCXX_DIR"

    step "-D_GLIBCXX_DEBUG core tests"
    SKEIN_TEST_TIMEOUT=120 test_dir "$GLIBCXX_DIR" -R \
        'runtime_link_test|bytecode_verifier_test|skein_tests|skein_vm_tests'
fi

printf '\nAll checks passed.\n'
