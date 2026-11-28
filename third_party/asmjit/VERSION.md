# Vendored asmjit

This directory contains a pinned copy of [asmjit](https://github.com/asmjit/asmjit)
used by the optional `SKEIN_ENABLE_JIT` build.

- Upstream commit: `c5e044a02494a0f01c2399940b7cd50e59880d67`
- `ASMJIT_LIBRARY_VERSION`: `1.25.0` (2026-11-25)
- Source: `https://codeload.github.com/asmjit/asmjit/tar.gz/c5e044a`
- Tarball SHA-256: `e28998d5f5ad2272a382902d68764355be4dfde0b44bd48b6050814e22136250`
- Vendored contents: `CMakeLists.txt`, `asmjit/`, `LICENSE.md`, `README.md`

Only the library sources are vendored; asmjit's `asmjit-testing/`, `tools/`,
`db/` and `.github/` directories are intentionally omitted.  This is the same
upstream snapshot as the `mingw-w64-ucrt-x86_64-asmjit` package shipped by
MSYS2.

The Skein JIT targets the asmjit 1.25 API (snake_case methods, `asmjit/x86.h`
and friends).  Do not downgrade this directory without migrating `src/jit/`.

asmjit is distributed under the zlib license; see `LICENSE.md`.
