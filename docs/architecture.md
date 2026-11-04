# Skein 运行时与解释器边界

本文定义 Skein 的运行时、解释器、宿主模块之间的职责与依赖方向，作为后续嵌入、插件和 AOT 的架构基础。

## 1. 目标与非目标

目标：

- 明确运行时（值、容器、核心 builtins）与解释器（parser/eval/package）的边界；
- 允许解释器和未来的 AOT 生成代码共享同一套运行时；
- 允许可选原生模块（TLS/HTTP2/FFI/压缩等）按需链接或加载；
- 保持当前单文件 CLI 的默认体验不变。

非目标（当前阶段）：

- 不实现 AOT 代码生成；
- 不默认把 builtins 或运行时做成 `.so`；
- 不承诺外部 ABI 的长期二进制兼容；
- 不把全部 builtins 拆成独立插件。

## 2. 分层

```text
L4  host/CLI          main.cpp（参数、入口、hook 装配）
      │
L3  builtins          builtins.cpp + 宿主/可选模块（net/http/tls/ffi/...）
      │
L2  interpreter       lexer/parser/AST/eval/package/执行模型（fiber/task）
      │
L1  runtime           值表示、容器、GC 生命周期、unicode/encoding、
      │               json/format、核心 builtins、模块注册表
      │
L0  platform          OS 原语（文件/时间/线程/进程/socket 抽象）
```

依赖规则：

- 只允许上层依赖下层，禁止反向依赖；
- `L1 runtime` 不得 include `eval.h` / `ast.h` / `parser.hpp` / `package.h` / `repl.h` / `builtins.h`；
- `L1 runtime` 的错误通过 `runtime_error.cpp` 的 `yyerror` hook 上报，不依赖 diagnostics/eval；
- `L1 runtime` 不得直接调用 `call_value_args` / `eval_ctx()`；
- `L2 interpreter` 通过 `skein_context` 的 hook 调用 builtins；
- `L3 builtins` 可以依赖 `L2`（错误上报、回调），并通过 hook 回调用户函数；
- 可选模块只能依赖 `L1`/`L2` 的公开内部接口，不得依赖 `L4`。

## 3. 两层 ABI

| 边界 | 使用者 | 类型 | 稳定性 |
|---|---|---|---|
| 内部 ABI | interpreter、核心 builtins、静态模块 | C++（`value_t*`、`std::vector`） | 与源码版本强绑定 |
| 外部 ABI | 插件、嵌入、未来 AOT | 纯 C、opaque handle、固定宽度整数 | 版本号 + 兼容承诺（未来） |

内部 ABI 保留 `value_t*` 和 `dupval/freeval` 的性能；外部 ABI 使用 `skein_value*` 不透明句柄，不暴露 C++ 类型。

## 4. 文件归属

| 层 | 内容 |
|---|---|
| L1 runtime | `valuetype.*`、`value_ops.*`、`dict.*`、`structtype.*`、`symbol.*`、`cycle.*`、`serialization.*`、`json.*`、`format.*`、`encoding.*`、`unicode.*`、`unicode_ident.*`、`stdio_stream.*`、`runtime_context.*`、`runtime_error.*` |
| L2 interpreter | `tokens.l`、`parser.y`、`ast.*`、`diagnostics.*`、`error_report.*`、`eval.*`、`eval_context.*`、`gc.*`、`package.*`、`pattern.*`、`scopestack.*`、`structeval.*`、`fiber.*`、`generator.*`、`async_support.*`、`task.*`、`event_loop.*`、`repl.*`、`source_encoding.*` |
| L3 builtins | `builtins.cpp`、`compress_builtin.*`、`compression.*`、`cookie.*`、`crypto_builtin.*`、`env_builtin.*`、`error.*`、`filetype.*`、`fs_extra.*`、`http.*`、`http1_parser.*`、`io.*`、`net.*`、`process_builtin.*`、`process_spawn.*`、`random_builtin.*`、`regex_builtin.*`、`result.*`、`time_builtin.*`、`websocket.*`、`worker_pool.*`、`tls.*`、`http2.*`、`ffi_types.*`、`ffi_value.*`、`ffi_marshal.*`、`ffi_callbacks.*`、`ffi_manifest.*`、`ffi.cpp`（可选） |
| L0 platform | `platform_posix.cpp`、`platform_win.cpp`、`dynamic_library.*`、`runtime_state.*` |
| L4 CLI | `main.cpp` |

说明：

- `repl.*`/`source_encoding.*` 暂时归入 L2，因为 lexer 的交互式读取直接依赖 REPL；后续可拆为独立的 input provider。
- `std.set` / `deque` / `heap` 的值类型在 L1，`lib/std/*.skein` 别名包由 L2 的 package loader 加载。
- `std.regex` 依赖 PCRE2、`std.compress` 依赖 zlib/brotli、`std.net` 依赖 libuv、`std.tls` 依赖 OpenSSL、`std.http2` 依赖 nghttp2、`std.ffi` 依赖 libffi：都属于 L3 可选模块。

## 5. 关键接口

### 5.1 运行时上下文

```cpp
struct skein_context {
    value_t *(*call)(value_t *callee, std::vector<value_t *> args);
    value_t *(*call_builtin)(int functype,
                             const std::vector<value_t *> &args);
};

skein_context *skein_context_current();
void skein_context_set(skein_context *ctx);
```

- `call`：调用用户函数/闭包；由 interpreter 或 AOT dispatcher 提供。
- `call_builtin`：按 builtin id 调用 builtins；由 L3 提供。
- context 每线程一个；L1 不持有可变的进程全局状态。

### 5.2 错误

- `runtime_error.cpp` 提供 `yyerror`：格式化后转发到 `skein_context::error`；
- interpreter 安装 `runtime_error_report`，负责源码位置、捕获错误和 diagnostics；
- L1 不再依赖 diagnostics/eval；
- 外部 ABI 使用 `skein_status` + `skein_error { kind, message }`；
- 不允许 C++ 异常跨外部 ABI。

### 5.3 模块注册（未来）

```c
typedef struct {
    const char *name;
    uint32_t abi_version;
    skein_status (*init)(skein_context *ctx);
    const skein_builtin *builtins;
    size_t builtin_count;
} skein_module;
```

- 核心 builtins 可以先使用内部 enum/表；
- 可选模块通过描述符注册；
- 动态加载器（`dlopen`/`LoadLibrary`）当前不实现。

### 5.4 值所有权

- 内部：`value_t*` 由 `dupval/freeval` 管理；
- 外部：`skein_value*` 为 opaque handle，runtime 拥有；参数借用、返回值 owned；
- 禁止跨外部 ABI 直接 `delete` 或修改 `value_t`。

## 6. 当前迁移状态

已完成：

- 原生 deque/heap/set；
- `dict` 读取快路径；
- graph 邻接表与批量构造；
- fsm 内部表示与批量 run；
- P5 易用性、P6 基准；
- 静态库拆分：`skein_platform` / `skein_runtime` / `skein_interpreter` / `skein_builtins` / `skein`；
- `skein_context` 的 `call` / `call_builtin` / `error` hook；
- `builtin_ids.h`、`runtime_state.*`、`runtime_error.*`、`value_ops.*`；
- `tools/check_boundaries.py` 与 CTest `boundary_check_test`；
- 关闭 TLS/HTTP2/FFI/压缩后核心仍可构建。

未开始：

- 外部 C ABI 的正式冻结；
- 动态插件加载；
- AOT 代码生成与 GC 栈图。

## 7. 验收标准

- `skein_runtime` 可单独链接（依赖 `skein_platform` / `utf8proc`），不依赖 `eval.o` / `parser.o` / `main.o`；
- `skein_interpreter` 只依赖 runtime + platform + libuv/Boost；
- `skein_builtins` 依赖 interpreter，但 interpreter 不反向依赖 builtins；
- 关闭所有可选模块后，core + interpreter + CLI 仍可构建并通过核心测试；
- L1 源文件不 include `eval.h` / `eval_context.h` / `eval_helpers.h` / `ast.h` / `parser.hpp` / `package.h` / `repl.h` / `builtins.h` / `diagnostics.h`；
- `tools/check_boundaries.py` 通过；
- `ctest` 全绿。
