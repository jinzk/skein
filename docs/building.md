# 构建与测试

本文件收录完整的构建选项、依赖、测试矩阵和平台说明。只想快速跑起来时，
先看根目录的 [README](../README.md#快速开始)。

## 构建与运行

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/skein
```

字节码 VM 默认启用；`--no-vm` / `SKEIN_VM=0` 可切回 AST 解释器。
`--vm` / `SKEIN_VM=1` 仍可用于显式指定 VM。`SKEIN_VM_TRACE=1` 会打印
字节码和每步执行，`SKEIN_VM_DEBUG=1` 会说明哪些结构回退到了 AST。详见
[bytecode.md](bytecode.md)。JIT spike 的路线、依赖和验收标准见
[roadmap.md](roadmap.md)。

JIT 使用仓库内置的 `third_party/asmjit`（固定 1.11.0，版本与来源见
`third_party/asmjit/VERSION.md`），无需额外安装 asmjit。在 Linux / Windows 的 x86-64
上 `SKEIN_ENABLE_JIT` 默认 ON，普通配置即可编译 JIT；其他架构默认 OFF，
可用 `-DSKEIN_ENABLE_JIT=ON` 显式开启（要求 64-bit x86）：

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/skein benchmarks/fib_bench.skein
```

JIT 在运行时默认启用；`SKEIN_JIT=0` 关闭 JIT、回到纯 VM，
`SKEIN_JIT_THRESHOLD` 控制首次编译前的调用次数（默认 100），
`SKEIN_JIT_LOOPS=0` 关闭「含循环的函数首次调用即编译」，
`SKEIN_JIT_TOP_LEVEL=0` 只关闭顶层语句的 JIT，
`SKEIN_JIT_DUMP=1` 会输出生成的汇编。JIT/VM 对比基准：

```bash
python3 tools/bench_jit.py --bin ./build/skein --repeats 5 --check
```

基准脚本在进程内预热后只对工作负载计时，并输出 `SKEIN_BENCH_MS=`，因此不包含
进程启动和 JIT 编译时间。工具会先探测目标二进制是否真正进入 JIT；如果构建时
`SKEIN_ENABLE_JIT=OFF`，会直接报错而不是给出误导性的 1.0x 数据。
各平台收益见 [roadmap.md](roadmap.md)。

Windows 上可选用 mimalloc 降低小对象分配开销：

```bash
cmake -S . -B build -G Ninja -DSKEIN_ENABLE_JIT=ON -DSKEIN_USE_MIMALLOC=ON
cmake --build build
```

MinGW 下该选项会自动使用静态 libstdc++/libgcc，避免动态 libstdc++ 与
mimalloc 的 `operator new/delete` 分配器不一致。

Windows（MSYS2 / MinGW）下，构建 `skein` 后会自动把运行时依赖 DLL
（`libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll`、OpenSSL、libuv、
PCRE2 等）复制到可执行文件所在目录；System32 / SysWOW64 / WinSxS 中的系统 DLL
不会复制。这样 build 目录可以直接运行或打包，不依赖 MSYS2 的 `PATH`。该行为由
`SKEIN_COPY_RUNTIME_DLLS`（Windows 上默认 `ON`）控制：

```bash
cmake -S . -B build -DSKEIN_COPY_RUNTIME_DLLS=OFF
```

实现见 `tools/copy_windows_dlls.cmake`；无法解析的第三方 DLL 只警告，不中断构建。

只构建 VM 后端（不支持 `--no-vm`）：

```bash
cmake -S . -B build-vm-only -G Ninja -DSKEIN_VM_ONLY=ON
cmake --build build-vm-only
```

`std.net` 使用 libuv 作为同步网络后端，因此构建需要 libuv 开发文件；task / generator 使用 Boost.Context；`std.regex` 使用 PCRE2 8-bit。例如 Debian/Ubuntu：

```bash
sudo apt install libuv1-dev libboost-context-dev libpcre2-dev
```

HTTP/1.1 解析器使用 vendored `llhttp`（`third_party/llhttp`），Unicode 归一化使用 vendored `utf8proc`（`third_party/utf8proc`），密码哈希使用 vendored `libargon2`（`third_party/argon2`），都不需要额外的系统包。

`std.db` 的 SQLite 驱动使用系统 SQLite 3（`SKEIN_ENABLE_SQLITE=ON`，默认），PostgreSQL 驱动使用 libpq（`SKEIN_ENABLE_POSTGRES=ON`，默认），MySQL / MariaDB 驱动使用 MariaDB Connector/C 或 libmysqlclient（`SKEIN_ENABLE_MYSQL=ON`，默认）。找不到某个驱动的头文件或库时只禁用该驱动；全部不可用时 `std.db` 整体禁用。Debian/Ubuntu 可安装 `libsqlite3-dev libpq-dev libmariadb-dev`。

TLS 是可选模块；启用时需要 OpenSSL 开发文件：

```bash
sudo apt install libssl-dev
```

没有 OpenSSL 时使用 `-DSKEIN_ENABLE_TLS=OFF`，`std.tls` 和 `https://` 不可用。

`std.ffi` 是可选模块，需要 libffi >= 3.3（用于布局校验）；找不到 libffi 时会自动禁用。可以显式关闭：

```bash
cmake -S . -B build -G Ninja -DSKEIN_ENABLE_FFI=OFF
```

用于验证 native module / plugin ABI 的 MySQL 示例插件是可选的独立目标，默认不构建。需要 MySQL/MariaDB 客户端头文件与库：

```bash
cmake -S . -B build -G Ninja -DSKEIN_BUILD_MYSQL_PLUGIN=ON
cmake --build build
ctest --test-dir build -R mysql_plugin_test --output-on-failure
```

找不到客户端开发文件时该目标自动跳过。测试覆盖 ABI、源码 native module 构建、
ABI v2 异步查询和 ABI v3 `new_string_len` 安全文本传输；构建、脚本封装和
环境变量见 [`contrib/mysql/README.md`](../contrib/mysql/README.md)。

`std.net` 提供同步 API 和 task 形式的异步 API；libuv 只在内部驱动 socket、DNS 和 `std.task`，不向脚本暴露事件循环对象。

运行测试：

```bash
ctest --test-dir build --output-on-failure
```

PostgreSQL 集成测试默认跳过；设置 `SKEIN_TEST_POSTGRES_DSN` 后会执行：

```bash
SKEIN_TEST_POSTGRES_DSN=postgresql://user:password@127.0.0.1:5432/skein_test \
  ctest --test-dir build -R skein_tests --output-on-failure
```

MySQL / MariaDB 集成测试同理，设置 `SKEIN_TEST_MYSQL_DSN`：

```bash
SKEIN_TEST_MYSQL_DSN=mysql://user:password@127.0.0.1:3306/skein_test \
  ctest --test-dir build -R skein_tests --output-on-failure
```

Redis / Valkey `std.cache` 集成测试通过 `SKEIN_TEST_REDIS_DSN` 显式启用：

```bash
SKEIN_TEST_REDIS_DSN=redis://127.0.0.1:6379 \
  ctest --test-dir build -R skein_tests --output-on-failure
```

日常开发使用快速回归循环（增量构建 + core 标签，不跑基准和 sanitizer）：

```bash
tools/check_fast.sh                 # 约 30s
tools/check_fast.sh --vm-only       # 约 15s，VM 改动
tools/check_fast.sh --ast-only      # 约 15s，AST 改动
tools/check_fast.sh --bench         # 额外跑基准
tools/check_fast.sh --docs          # docs / data 检查
tools/check_fast.sh --coverage      # opcode 覆盖
tools/check_fast.sh --fallback      # VM fallback 回归
```

提交前再跑完整矩阵即可。opcode 覆盖检查：

```bash
python3 tools/check_opcode_coverage.py --bin ./build/skein --check
python3 tools/check_vm_fallback.py --bin ./build/skein --check
```

ASan/UBSan 构建：

```bash
cmake -S . -B build-asan -G Ninja -DSKEIN_SANITIZE=ON
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  SKEIN_TEST_TIMEOUT=120 \
  ctest --test-dir build-asan --output-on-failure -j 6 -E container_bench
```

sanitize 构建会把 Python 套件按模块分成 ASan/UBSan 分片和 LSan 分片；
`-j 6`（或接近 CPU 核数）可让 AST/VM/LSan 三组并行，避免整份套件串行跑三遍。
只需要 leak 检查时可以单独运行 `-R skein_vm_lsan_tests_shard`。

`SKEIN_TEST_TIMEOUT` 只影响测试子进程的超时，默认 5 秒；sanitizer 构建需要放宽。
task / generator 的 fiber 栈默认 1 MiB，带 guard page，栈按线程复用
（最多缓存 32 个）；可用 `SKEIN_FIBER_STACK_KB` 调整大小，sanitizer 构建建议
8192。`SKEIN_FIBER_STACK_POOL=0` 可关闭栈池，`SKEIN_FIBER_STACK_STATS=1`
会在 shutdown 时输出 `fiber stack pool: allocated=... reused=... pooled=...`。
栈即将耗尽时会在挂起点报 `fiber_stack` 错误；越过 guard page 会立即崩溃。
AST 参考后端的调用深度可用 `SKEIN_MAX_CALL_DEPTH` 调整（sanitizer 测试使用 128）。

TSan 构建（不能与 `SKEIN_SANITIZE` 同时启用）：

```bash
cmake -S . -B build-tsan -G Ninja -DSKEIN_TSAN=ON
cmake --build build-tsan
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan \
  -R 'thread_tsan|net_tsan' --output-on-failure
```

Linux JIT + mimalloc 构建与全量测试：

```bash
MIMALLOC_PREFIX=/path/to/mimalloc tools/check_linux_jit_mimalloc.sh
```

未设置 `MIMALLOC_PREFIX` 时要求系统 include/library 路径中已有 mimalloc。

统一的本地/CI 检查脚本覆盖 Debug、Release、ASan+UBSan、LSan、VM-only、
runtime-only 链接以及 docs / boundary / diff 检查：

```bash
tools/check_all.sh --clean
tools/check_all.sh --glibcxx-debug   # 额外运行 -D_GLIBCXX_DEBUG
```

容器基准与增长回归（不做绝对耗时断言，只检查 n 翻倍时是否出现超线性增长）：

```bash
python3 tools/bench_containers.py --bin ./build/skein --check
```

`--json` 输出 JSON，`--repeat` 控制每个规模的采样次数；该工具也注册为 CTest 的 `container_bench_test`。

运行时、解释器与 builtins 的分层边界见 [architecture.md](architecture.md)。

Windows / MinGW（64 位 MinGW-w64 GCC）：

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
build\skein.exe
```

Windows 构建需要 MSYS2 中的 MinGW-w64 GCC、CMake、Bison、Flex、libuv、PCRE2 和 Boost.Context。可选安装 `mingw-w64-ucrt-x86_64-mimalloc`，并用 `-DSKEIN_USE_MIMALLOC=ON` 降低分配密集型负载的开销。平台相关代码集中在 `platform_win.cpp`：

- 设置控制台输入输出代码页为 UTF-8；
- 命令行参数通过 `GetCommandLineW` / `CommandLineToArgvW` 转换为 UTF-8，脚本路径和 `-I` / `--package-path` 支持中文路径；
- `SKEIN_PACKAGE_PATH`、`SKEIN_HISTORY`、`USERPROFILE` 等环境变量按 UTF-16 读取后转换为 UTF-8；
- 管道输入使用二进制模式，避免 CRLF / `0x1A` 转换；
- 使用 `SetConsoleCtrlHandler` 实现 Ctrl-C。

当前只面向 MinGW-w64/GCC，不包含 MSVC 专用适配。

脚本文件默认按 UTF-8 读取；UTF-8 / UTF-16LE / UTF-16BE / UTF-32LE / UTF-32BE 的 BOM 会被自动识别并转成 UTF-8。没有 BOM 的其它编码可以用 `--encoding NAME` 显式指定，例如 `--encoding gbk`。显式编码与 BOM 冲突时会直接报错。如果控制台无法切换到 UTF-8，中文下标、格式化和输出可能无法正常工作，需要在输入输出边界额外做代码页转换。

源码在转码后必须是合法 UTF-8，非法 UTF-8 字节会报词法错误。标识符首字符支持 Unicode `XID_Start`（以及 `_`），后续字符支持 `XID_Continue`、`_` 和 ASCII 数字，例如 `let 变量 = 1;`、`let café = 2;`、`let foo变量 = 3;`。当前使用 Unicode 16.0.0 的 XID 数据。关键字和内置函数名仍只识别 ASCII；标识符不支持 `\u` 转义，非 ASCII 标识符按 NFC 归一化（不做 NFKC），规范等价写法视为同一标识符；字符串可以通过 `std.unicode.normalize` 显式归一化。函数形参、变长参数名、结构体字段、类型名、label、包名和 import 路径组件使用相同的 NFC 规则。

