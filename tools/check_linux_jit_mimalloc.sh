#!/usr/bin/env bash
# Build and test the Linux JIT + mimalloc configuration.
#
# Usage:
#   MIMALLOC_PREFIX=/path/to/mimalloc tools/check_linux_jit_mimalloc.sh [build-dir]
#
# If MIMALLOC_PREFIX is omitted, CMake must find mimalloc through the normal
# system include/library paths.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build-linux-jit-mi}"

cmake_args=(
  -S "$root"
  -B "$build"
  -G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DSKEIN_ENABLE_JIT=ON
  -DSKEIN_USE_MIMALLOC=ON
)
if [[ -n "${MIMALLOC_PREFIX:-}" ]]; then
  cmake_args+=(-DCMAKE_PREFIX_PATH="$MIMALLOC_PREFIX")
fi

cmake "${cmake_args[@]}"
cmake --build "$build" -j "${JOBS:-8}"
ctest --test-dir "$build" --output-on-failure -j "${CTEST_JOBS:-2}"
