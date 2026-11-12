# Skein

Skein 是一个使用 Flex + Bison + C++17 实现的动态类型脚本语言。它同时提供 REPL 和脚本运行模式，默认执行字节码 VM，可选 JIT；内置 task / generator、线程与 Channel、HTTP/1.1、HTTP/2、WebSocket、TLS、FFI，以及一套可以直接用 Skein 自身扩展的标准库。

- 想快速上手：[快速开始](#快速开始)
- 想系统学习：[Skein 教程](docs/tutorial.md)
- 想查语法：[语法参考](SYNTAX.md)
- 想查标准库：[标准库指南](docs/stdlib.md)

## AI / Agent skill

仓库提供一个面向 Skein 语言使用者的 agent skill：
[`.opencode/skills/skein/SKILL.md`](.opencode/skills/skein/SKILL.md)。OpenCode
会自动发现它；其他工具可以直接读取该文件，或安装到全局：

```bash
mkdir -p ~/.config/opencode/skills
cp -r .opencode/skills/skein ~/.config/opencode/skills/
```

该 skill 只覆盖「写 Skein 程序」：运行方式、换行和大括号规则、for-in、
`std.iter.for_each`、match、struct/enum、错误处理、常见报错速查，以及到
`SYNTAX.md` / `docs/stdlib.md` 的索引。

## 编辑器支持

- **Vim / Neovim**：`editors/vim`，提供 `.skein` 文件识别、语法高亮、注释、
  缩进，以及 `:SkeinRun` / `:SkeinRepl`。安装见
  [`editors/vim/README.md`](editors/vim/README.md)。
- **VS Code**：`editors/vscode`，提供语法高亮、代码片段、运行当前文件和
  REPL。安装见 [`editors/vscode/README.md`](editors/vscode/README.md)。

两个插件都只做编辑器集成，不包含 LSP、语义补全或诊断。

## 设计特点

### 表达力优先的语法

管道、占位符、模式匹配、解构、切片和一等函数让常见的数据处理保持紧凑：

```skein
import std;

let squares = (range(0, 10)
  |> std.iter.filter(x -> x % 2 == 0)
  |> std.iter.map(x -> x * x)
  |> std.iter.collect());

let desc = match (event) {
  case {kind: "tool", ok: true, data: d} => "ok: " + str(d);
  case [head, ...tail] => "list starts with " + str(head);
  case n if n > 10 => "big";
  default => "other";
};
```

- `|>` 把左侧值插入右侧调用的第一个参数；`|>>` 先把左侧应用到右侧求值结果；
- 调用参数中的 `$` 是占位符，`add($, 2)` 生成新的函数；
- `match` 是表达式，支持字面量、绑定、dict / list / struct 模式、or-pattern、整数范围和守卫；
- 支持 Python 风格切片、`for ... in`、标签跳转和 `switch`。

### 动态类型，渐进式检查

不写类型也能运行；需要时再加标注，类型检查器会沿着函数、容器、`match` 和 `union` 收窄：

```skein
let xs: list[int] = [1, 2, 3];
let name: string = "skein";

func add(a: int, b: int): int {
  return a + b;
}
```

- 可选参数 / 返回值 / 变量类型标注，运行时检查，不做隐式转换；
- 内建容器支持 `list[T]`、`dict[K, V]`、`set[T]`、`deque[T]`、`heap[T]`、`task[T]`、`generator[T, S]`、`pair`、`tuple`、`union`；
- `union` 支持 `is` / `match` / 稳定字段路径的类型收窄；
- 函数类型支持参数逆变、返回值协变；
- 检查强度可用 `SKEIN_TYPECHECK=off|loose|warn|strict` 或 `--typecheck=...` 调整。

### 双后端执行

同一份源码可以在 AST 解释器和字节码 VM 上运行，VM 默认启用，JIT 可选：

- `--no-vm` / `SKEIN_VM=0` 回退到 AST 解释器，`--vm` 显式选择 VM；
- 可选 asmjit JIT，`SKEIN_JIT_THRESHOLD` 控制首次编译前的调用次数；
- VM 暂不支持的结构会自动回退到 AST，`SKEIN_VM_DEBUG=1` 会说明回退点；
- AST、VM 和 JIT 共用同一套测试与语义，`SKEIN_VM_TRACE=1` 可查看字节码执行过程。

### 结构体、trait 与枚举

`struct` 声明数据，`impl` 声明方法，`trait` 声明接口；枚举支持带载荷变体和穷尽匹配：

```skein
trait Drawable {
  func draw(self);
}

struct Point {
  x: int;
  y: int;
}

impl Point {
  func area(self) {
    return self.x * self.y;
  }
}

impl Drawable for Point {
  func draw(self) {
    print("point " + str(self.area()));
  }
}
```

- struct 支持字段默认值和 copy-with：`Point { ...p, x: 9 }`；
- 方法支持默认参数、可变参数、装饰器和 generator；
- enum 支持带字段变体、`impl`、trait 和 `match` 穷尽性检查；
- list / dict / struct 是值语义，内部使用写时复制。

### 并发与并行

task / generator 使用 fiber，线程之间通过 Channel 和 Message 传递数据，不共享可变 `value_t*`：

```skein
import std;

func work(n) {
  std.task.sleep(10).wait();
  return n * 2;
}

let total = std.task.scope(func() {
  let a = launch work(1);
  let b = launch work(2);
  return a.wait() + b.wait();
});
```

- `std.task.scope` 等待并管理一组子任务，支持取消和错误传播；
- `std.thread.Channel` 支持有界、无界和 rendezvous 队列，以及 `select` / `broadcast`；
- 线程池、worker 池、进程入口 `std.entry` / `std.process.run_entry`；
- 同步 API 与 `*_task` 异步 API 共用同一套事件循环。

### 网络与协议

`std.net` 基于 libuv，HTTP/1.1 解析器使用 vendored llhttp；HTTP/2、WebSocket 和 TLS 都是内置模块：

```skein
import std;

let server = std.http.listen("127.0.0.1", 8080);

std.http.serve(server, func(req) {
  return {
    "status": 200,
    "headers": {"Content-Type": "text/plain"},
    "body": "hello " + req["path"]
  };
}, 100);

std.http.close(server);
```

- HTTP/1.1 和 HTTP/2 客户端 / 服务端，支持 keep-alive、cookie jar、mTLS、ALPN 和 `reuse_port`（具体能力随协议和方向不同）；
- WebSocket 客户端 / 服务端，基于 HTTP/1.1 Upgrade；
- TLS 支持 SNI 多证书、session reuse 和 `https://`；
- 所有主要网络操作都有同步和 task 两种形式；`close` 等仍是同步操作。

### 标准库

`import std;` 即可使用内置标准库，不需要文件系统目录：

- 数据：`std.json`、`std.regex`（PCRE2）、`std.crypto`、`std.compress`、`std.encoding`、`std.unicode`；
- 系统：`std.fs`、`std.process`、`std.env`、`std.sys`、`std.time`、`std.random`；
- 容器与工具：`std.list`、`std.dict`、`std.iter`、`std.set`、`std.deque`、`std.heap`、`std.lru`、`std.path`；
- 元编程与接口：`std.meta`、`std.type`、`std.enum`、`std.struct`、`std.result`、`std.error`；
- 部分标准库直接用 Skein 编写并嵌入解释器，例如 `std.fsm`、`std.route`、`std.graph`、`std.arena`；
- 可选 `std.ffi` 基于 libffi，支持原生模块、回调、manifest 和 bindgen。

### 包、模块与工程化

- 文件包：`package bb {}`，包名与文件名一致，`export` 控制可见性；项目内的包按 `project_root` 解析；
- Module：唯一的模块机制，目录 + `module.skein` 清单；内部仍然是文件包，支持公共导出、内部包和深导出；源码层不存在 Module v1 或兼容层；
- `include` 在声明层展开，适合共享常量、类型和函数声明；
- REPL 支持历史、`:load`、`ans` / `_`；脚本支持 shebang 和 `--encoding`；
- Unicode 标识符、UTF-8 / UTF-16 / UTF-32 BOM 自动识别、GBK / GB18030 等编码转换；
- 测试矩阵覆盖 Windows、Linux、ASan/UBSan/LSan、TSan、VM-only、JIT 和文档一致性检查。

## 快速开始

### 依赖

- CMake、Ninja、Flex、Bison 和支持 C++17 的编译器；
- libuv、Boost.Context、PCRE2；
- 可选：OpenSSL（TLS）、libffi（FFI）、mimalloc（Windows 分配器）；
- llhttp、utf8proc 和 asmjit 已随仓库提供，不需要额外安装。

Debian / Ubuntu 示例：

```bash
sudo apt install libuv1-dev libboost-context-dev libpcre2-dev
sudo apt install libssl-dev        # 需要 TLS 时
```

### 构建

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

可选 JIT 构建：

```bash
cmake -S . -B build -G Ninja -DSKEIN_ENABLE_JIT=ON
cmake --build build
SKEIN_JIT=1 ./build/skein benchmarks/fib_bench.skein
```

Windows / MinGW：

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
build\skein.exe
```

完整的构建选项、sanitizer / TSan 矩阵和平台说明见 [docs/building.md](docs/building.md)。

### 运行

```bash
./build/skein                 # REPL
./build/skein script.skein    # 脚本模式
./build/skein -               # 从 stdin 读取脚本
```

最小的脚本：

```skein
print("hello, skein");
```

### 测试

```bash
ctest --test-dir build --output-on-failure
```

日常开发使用快速回归循环：

```bash
tools/check_fast.sh
tools/check_fast.sh --vm-only
tools/check_fast.sh --ast-only
```

提交前运行完整矩阵：

```bash
tools/check_all.sh --clean
```

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/tutorial.md](docs/tutorial.md) | 从脚本运行、语言基础到并发、网络和标准库的教程 |
| [SYNTAX.md](SYNTAX.md) | 词法、语法、类型、内置函数的完整参考 |
| [docs/stdlib.md](docs/stdlib.md) | `std` 各模块的 API、参数和行为细节 |
| [docs/building.md](docs/building.md) | 构建选项、依赖、sanitizer / TSan 和平台说明 |
| [docs/architecture.md](docs/architecture.md) | 运行时、解释器和 builtins 的分层架构 |
| [docs/bytecode.md](docs/bytecode.md) | 字节码 VM、指令和 AST 回退策略 |
| [docs/jit-plan.md](docs/jit-plan.md) | JIT 路线、依赖和验收标准 |
| [docs/ffi.md](docs/ffi.md) | FFI 契约、类型映射和示例 |
| [docs/encoding.md](docs/encoding.md) | 源文件编码、转码和编码表 |
| [docs/unicode.md](docs/unicode.md) | Unicode 标识符和归一化 |

## 项目结构

```text
src/
  platform/      L0  OS 原语：platform.*、platform_posix.cpp、platform_win.cpp、
                 dynamic_library.*、runtime_state.*
  runtime/       L1  值/容器/编码/JSON/格式化：
                 valuetype.*、value_ops.*、dict.*、structtype.*、symbol.*、cycle.*、
                 serialization.*、typeinfo.*、json.*、format.*、encoding.*、unicode.*、
                 unicode_ident.*、stdio_stream.*、runtime_context.*、runtime_error.*、
                 binding_registry.*、builtin_ids.h、skein_module.h
    data/        L1  提交到仓库的生成表：encoding_gb18030_data.inc、
                 encoding_gbk_validity.inc、unicode_ident_data.inc
  interpreter/   L2  词法/语法/AST/求值/执行模型：
                 tokens.l、parser.y、ast.*、diagnostics.*、error_report.*、eval.*、
                 eval_control.*、eval_call.*、eval_assign.*、
                 eval_context.*、eval_helpers.h、value_runtime.*、bytecode.*、
                 compiler.*、vm.*、vm_bridge.*、vm_eval.h、vm_host.*、
                 vm_host_calls.h、gc.*、package.*、
                 std_namespace.*、pattern.*、scopestack.*、structeval.*、fiber.*、
                 generator.*、iter.*、async_support.*、task.*、event_loop.*、
                 repl.*、source_encoding.*
  builtins/      L3  内置函数与宿主模块：
                 builtins.*、builtins_*.cpp（math/meta/list/dict/string/convert/file/text/bytes/lru/deque/heap/set）、
                 *_builtin.*、compression.*、cookie.*、error.*、
                 filetype.*、fs_extra.*、http.*、http1_parser.*、http2.*、io.*、
                 net.*、process_*.*、result.*、tls.*、websocket.*、worker_pool.*、
                 example_binding.*
    ffi/         L3  可选的 std.ffi：ffi.*、ffi_types.*、ffi_value.*、ffi_marshal.*、
                 ffi_callbacks.*、ffi_manifest.*、ffi_check.*、ffi_bindgen.*
  app/           L4  main.cpp（参数、入口、hook 装配）
docs/            架构、编码、FFI、Unicode、字节码与 JIT 计划文档
lib/std/         以 Skein 编写的标准库包（构建时嵌入）
tests/           自动化回归测试（test_skein.py + cases/）
tools/           生成器与一致性检查脚本
third_party/     llhttp、utf8proc、asmjit
cmake/           构建辅助脚本
```

每层只向本层及以下层暴露 include 目录，配合 `tools/check_boundaries.py`
同时约束源码包含关系和静态库符号依赖。

## License

Skein 以 MIT License 发布，详见 [LICENSE](LICENSE)。随仓库分发的第三方组件及其许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
