# Vendored asmjit

This directory contains a pinned copy of [asmjit](https://github.com/asmjit/asmjit)
used by the optional `SKEIN_ENABLE_JIT` build.

- Upstream commit: `1da9e4d957fc8c48d1eac68c76c1b139b3691a24`
- `ASMJIT_LIBRARY_VERSION`: `1.11.0` (2023-11-15)
- Source: `https://codeload.github.com/asmjit/asmjit/tar.gz/1da9e4d`
- Tarball SHA-256: `55f587823502112028c25b0d36e2e7237dadc31755f3e354470ad2265fc9ca86`
- Vendored contents: `CMakeLists.txt`, `src/`, `LICENSE.md`, `README.md`

Only the library sources are vendored; asmjit's `test/`, `tools/`, `db/`
and `.github/` directories are intentionally omitted.

The Skein JIT currently targets the asmjit 1.11 API.  Newer asmjit
versions (for example the 1.21 snapshot shipped by MSYS2) renamed many
public methods to snake_case, so upgrading this directory requires a JIT
API migration.

asmjit is distributed under the zlib license; see `LICENSE.md`.
