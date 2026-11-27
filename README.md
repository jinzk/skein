# Skein

Skein 是一门动态类型脚本语言，面向数据处理、并发和网络服务。它默认运行在字节码 VM 上（可选 JIT），同时保留 AST 解释器；标准库覆盖文件、网络、编码、容器与模块管理。语法上强调管道、模式匹配和表达式级 `on error`。

按目标阅读：

- 第一次运行：[快速开始](#快速开始)
- 学语言基础：[语言速览](#语言速览)
- 看它能做什么：[能力一览](#能力一览)
- 系统学习：[Skein 教程](docs/tutorial.md)
- 查完整语法：[SYNTAX.md](SYNTAX.md)
- 查标准库 API：[docs/stdlib.md](docs/stdlib.md)
- 参与解释器开发：[项目结构](#项目结构)

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

Linux / Windows 的 x86-64 默认会编译 JIT，运行时默认启用：

```bash
./build/skein benchmarks/fib_bench.skein
# 对比纯 VM：SKEIN_JIT=0 ./build/skein benchmarks/fib_bench.skein
```

其他架构默认不编译 JIT；64-bit x86 可用 `-DSKEIN_ENABLE_JIT=ON` 显式开启。

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
./build/skein --watch script.skein   # 改源码后自动重启
```

`--watch` 监听脚本目录下的 `.skein` 文件；用 `--watch-dir DIR` 增加目录，
用 `--watch-dir=DIR` 也可以。每次变化先停止旧子进程，再从干净运行时启动新进程，
适合开发期使用。它不是进程内热更新，模块状态和内存数据不会保留。

工具链子命令不会执行脚本：

```bash
./build/skein lint --typecheck=strict script.skein   # 静态检查，JSON 输出加 --format=json
./build/skein fmt --check script.skein               # 检查格式；-w 原地格式化
./build/skein lsp --stdio                            # 编辑器通过 stdio 调用的语言服务器
```

### 第一个程序

把下面内容保存为 `hello.skein`：

```skein
import std;
std.io.print("hello, skein");
```

然后运行：

```bash
./build/skein hello.skein
```

### 执行后端与调试

同一份源码可以在 AST 解释器和字节码 VM 上运行，VM 默认启用，JIT 可选：

- `--no-vm` / `SKEIN_VM=0` 回退到 AST 解释器，`--vm` 显式选择 VM；
- Linux x86-64 默认编译 asmjit JIT，运行时默认启用；`SKEIN_JIT=0`
  可关闭，`SKEIN_JIT_THRESHOLD` 控制首次编译前的调用次数，
  `SKEIN_JIT_TOP_LEVEL=0` 只关闭顶层语句的 JIT；
- VM 暂不支持的结构会自动回退到 AST，`SKEIN_VM_DEBUG=1` 会说明回退点；
- AST、VM 和 JIT 共用同一套测试与语义，`SKEIN_VM_TRACE=1` 可查看字节码执行过程。

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

测试矩阵覆盖 Windows、Linux、ASan/UBSan/LSan、TSan、VM-only、JIT 和文档一致性检查。

### 编辑器支持

- **Vim / Neovim**：`editors/vim`，提供 `.skein` 文件识别、语法高亮、注释、
  缩进，以及 `:SkeinRun` / `:SkeinRepl`。安装见
  [`editors/vim/README.md`](editors/vim/README.md)。
- **VS Code**：`editors/vscode`，提供语法高亮、代码片段、LSP 诊断/补全/格式化、
  运行当前文件和 REPL。安装见 [`editors/vscode/README.md`](editors/vscode/README.md)。

两个插件都通过 `skein lsp --stdio` 接入语言服务器，配置细节见
[`docs/lsp.md`](docs/lsp.md)。

## 语言速览

下面几段覆盖日常写 Skein 最常用到的语法。完整规则见 [SYNTAX.md](SYNTAX.md)。

### 变量、函数与渐进式类型

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
- `T?` 归一化为 `union[T, null]`，两者可互换；`union[A, B]?` 等价于 `union[A, B, null]`；
- 函数类型支持参数逆变、返回值协变；
- 检查强度可用 `SKEIN_TYPECHECK=off|loose|warn|strict` 或 `--typecheck=...` 调整。

### 数据处理与模式匹配

管道、占位符、模式匹配、解构、切片和一等函数让常见的数据处理保持紧凑：

```skein
import std;

let squares = (std.utils.range(0, 10)
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

- `|>` 把左侧值插入右侧调用的第一个参数；
- 调用参数中的 `$` / `$1`–`$9` 是占位符，`add($, 2)` 生成新函数，`add($1, $1)` 复用同一参数；
- `match` 是表达式，支持字面量、绑定、dict / list / tuple / struct 模式、or-pattern、整数范围和守卫；
- 支持 Python 风格切片、`for ... in`、标签跳转和 `switch`。

### 组织代码：struct、proto、`ref` 与枚举

`proto` 是协议声明关键字，属于全局保留字；泛型约束列表里的 `proto` / `type` 是
地位相同的 contextual 标记，`protocol` 不再作为关键字使用。

`struct` 声明数据和方法，`impl` 可追加方法，`proto` 声明结构式接口；枚举支持带载荷变体和穷尽匹配：

```skein
import std;
proto Drawable {
  func draw(self);
}

struct Point {
  x: int;
  y: int;

  func area(self) {
    return self.x * self.y;
  }

  func draw(self) {
    std.io.print("point " + str(self.area()));
  }
}

impl Point {
  func scaled(self, k: int) -> self.area() * k;
}
```

- struct 支持字段默认值、内联方法和 copy-with：`Point { ...p, x: 9 }`；
- `impl Type` 给已有类型追加方法，与内联方法共享方法命名空间；同名重复会报错；
- 方法支持默认参数、可变参数、装饰器和 generator；
- enum 支持带字段变体、`impl`、proto 和 `match` 穷尽性检查；enum 方法仍写在 `impl` 中；
- list / dict / struct 是值语义，内部使用写时复制；
- 需要共享可变对象时用 `ref T`，弱观察用 `weak T`；强引用环由 GC 回收，`?.` 处理 `null` 和失效弱引用；
- `ref` / `weak` 不能跨线程发送，用 `std.ref.snapshot()` 取快照，或发送 `std.arena` id。

### 并发：task 与 launch

task / generator 使用 fiber，线程之间通过 Channel 和 Message 传递数据，不共享可变 `value_t*`：

```skein
import std;

func work(n) {
  std.task.sleep(10).wait();
  return n * 2;
}

let t = launch work(21);
let value = std.task.wait!(t);       // 等价于 t.wait()
std.io.print(value);

let total = std.task.scope(func() {
  let a = launch work(1);
  let b = launch work(2);
  return a.wait() + b.wait();
});
```

- `std.task.wait!(t)` 是 `t.wait()` 的宏写法；`std.task.wait!(launch f())` 等价于 `(launch f()).wait()`；
- `launch` 作用于完整调用链，`.` 优先级高于前缀 `wait` / `launch`：`launch f().wait()` 启动的是 `f().wait()`，不是“启动后再等待”；
- `std.task.scope` 等待并管理一组子任务，支持取消和错误传播；
- `std.thread.Channel` 返回有方向端点对，支持有界、无界和 rendezvous lane，以及 `select` / `broadcast`；
- 线程池、worker 池、进程入口 `std.entry` / `std.process.run_entry`；
- 同步 API 与 `*_task` 异步 API 共用同一套事件循环。

### 错误处理：on error

`fail` 抛错，`on error` 在表达式边界恢复，`recover` 的值成为整个表达式的结果：

```skein
import std;
let n = parse_positive("0") on error(e) {
  std.io.print("bad input: " + str(e));
  std.error.recover!(0);
};
```

- 捕获 `fail`、运行时错误和 `result.err`；`result.ok(v)` 直接解包为 `v`；
- `e` 是 `error` 值，可用 `str(e)` / `std.error.message(e)` / `std.error.kind(e)` 检查；
- `on error()` 可以不绑定错误值；handler 以 `recover`、`pass` 或 `fail` 结束；
- task 内的错误在观察点抛出：`std.task.wait!(t) on error(e) { ... }`、`std.task.wait!(launch f()) on error(e) { ... }`；
- `launch f() on error(...)` 不能捕获任务体错误，会产生 warning；
- `on` / `error` 是 handler-scoped contextual keyword，其他位置仍是普通标识符
  （变量、字段、类型名均可）；日志宏使用 `std.log.error!` 等规范路径；
- 详见 [SYNTAX.md](SYNTAX.md) 和 [docs/stdlib.md](docs/stdlib.md)。

## 能力一览

### 标准库

`std` 是语言级默认绑定，无需 `import std;` 即可使用内置标准库；显式导入仍然合法且幂等：

- 数据：`std.json`、`std.regex`（PCRE2）、`std.crypto`、`std.compress`、`std.encoding`、`std.unicode`、`std.log`；
- 系统：`std.fs`、`std.process`、`std.env`、`std.sys`、`std.time`、`std.random`；
- 容器与工具：`std.list`、`std.dict`、`std.iter`、`std.set`、`std.deque`、`std.heap`、`std.lru`、`std.path`；
- 元编程与接口：`std.meta`、`std.type`、`std.enum`、`std.struct`、`std.result`、`std.error`；
- 部分标准库直接用 Skein 编写并嵌入解释器，例如 `std.fsm`、`std.route`、`std.graph`、`std.arena`；
- 可选 `std.ffi` 基于 libffi，支持原生模块、回调、manifest 和 bindgen。

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
- 生产环境支持优雅关闭：POSIX 用 `SIGTERM`、Windows 用控制台关闭/关机事件触发，停止 accept、等待在途请求、到期强制关闭，`SKEIN_DRAIN_TIMEOUT_MS` 控制截止时间（默认 20000ms）；
- WebSocket 客户端 / 服务端，基于 HTTP/1.1 Upgrade；
- TLS 支持 SNI 多证书、session reuse 和 `https://`；
- 所有主要网络操作都有同步和 task 两种形式；`close` 等仍是同步操作。

### Unicode 与源文件编码

字符串按 UTF-8 处理，支持 Unicode 标识符；归一化是显式的，不会在比较或拼接时隐式发生：

```skein
import std;
let café = "café";
let nfc = std.unicode.normalize(café, "nfc");
let folded = std.unicode.casefold("Straße");
```

- Unicode 16.0.0，基于 vendored `utf8proc`；
- 标识符使用 NFC quick-check，防止视觉上相同但码位不同的名字混淆；
- `std.unicode` 提供 NFC / NFD / NFKC / NFKD、归一化检查和 casefold；
- 源文件支持 UTF-8 / UTF-16 / UTF-32 BOM 自动识别，以及 GBK / GB18030 转码；
- 详见 [docs/unicode.md](docs/unicode.md) 和 [docs/encoding.md](docs/encoding.md)。

### 包、模块与工程化

- 文件包：`package bb {}`，包名与文件名一致，`export` 控制可见性；项目内的包按 `project_root` 解析；
- Module：唯一的模块机制，目录 + `module.skein` 清单；内部仍然是文件包，支持公共导出、内部包和深导出；源码层不存在 Module v1 或兼容层；
- 第三方包使用字符串 import：`import "github.com/acme/util/json" as json;`；`skein mod init` 创建 `skein.mod`，`skein get module/path@version` 通过 git 下载、按 MVS 解析传递依赖并写入 `skein.sum`；`skein mod verify` 校验缓存，`skein mod download` 预取，`skein mod tidy` 清理未使用依赖，`skein mod prune` 安全裁剪未引用缓存，`skein mod clean` 清理全局缓存，`get --offline` 禁止联网；下载/构建使用项目锁、缓存锁和模块锁，支持 `SKEIN_LOCK_TIMEOUT_MS`，运行时只读校验缓存且不会隐式联网；`/vN` 路径支持不兼容 major 版本并存；
- `include` 在声明层展开，适合共享常量、类型和函数声明；
- REPL 支持历史、行编辑、水平滚动、自动缩进和运算符 continuation、词法高亮、Tab 补全（关键字 / REPL 命令 / 全局符号 / `std.*` / struct 字段 / 文件路径）、`:load`、`ans` / `_`；脚本支持 shebang 和 `--encoding`。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/roadmap.md](docs/roadmap.md) | 总路线图、里程碑和待决策事项 |
| [docs/tutorial.md](docs/tutorial.md) | 从脚本运行、语言基础到并发、网络和标准库的教程 |
| [SYNTAX.md](SYNTAX.md) | 词法、语法、类型、内置函数的完整参考 |
| [docs/stdlib.md](docs/stdlib.md) | `std` 各模块的 API、参数和行为细节 |
| [docs/package-manager.md](docs/package-manager.md) | module、缓存、锁、信任模型和包管理计划 |
| [docs/lsp.md](docs/lsp.md) | LSP 协议、配置和编辑器接入 |
| [docs/key-semantics.md](docs/key-semantics.md) | dict / set key 的类型、哈希和相等规则 |
| [docs/building.md](docs/building.md) | 构建选项、依赖、sanitizer / TSan 和平台说明 |
| [docs/architecture.md](docs/architecture.md) | 运行时、解释器和 builtins 的分层架构 |
| [docs/bytecode.md](docs/bytecode.md) | 字节码 VM、指令和 AST 回退策略 |
| [docs/ffi.md](docs/ffi.md) | FFI 契约、类型映射和示例 |
| [contrib/mysql/README.md](contrib/mysql/README.md) | 用真实 MySQL client 验证 native module / plugin ABI 的示例 |
| [docs/encoding.md](docs/encoding.md) | 源文件编码、转码和编码表 |
| [docs/unicode.md](docs/unicode.md) | Unicode 标识符和归一化 |
| [docs/logging.md](docs/logging.md) | `std.log` 的 API、格式和写入语义 |
| [docs/macros.md](docs/macros.md) | 第一批内置语法宏的规则、展开和边界 |
| [docs/macros_user_defined.md](docs/macros_user_defined.md) | 用户自定义宏的 AST 模板、hygiene 与范围提案 |

## AI / Agent skill

仓库提供一个面向 Skein 语言使用者的独立 agent skill：
[`skills/skein/SKILL.md`](skills/skein/SKILL.md)。其他工具可以直接读取该文件；
OpenCode 用户可以把它复制到全局或项目的 skills 目录：

```bash
mkdir -p ~/.config/opencode/skills
cp -r skills/skein ~/.config/opencode/skills/
```

该 skill 只覆盖「写 Skein 程序」：运行方式、换行和大括号规则、for-in、
`std.iter.for_each`、match、struct/enum、错误处理、常见报错速查，以及到
`SYNTAX.md` / `docs/stdlib.md` 的索引。

## 项目结构

```text
import std;
src/
  platform/      L0  OS 原语：platform.*、platform_posix.cpp、platform_win.cpp、
                  dynamic_library.*、runtime_state.*
  runtime/       L1  值/容器/编码/JSON/格式化：
                  valuetype.*、value_ops.*、dict.*、structtype.*、symbol.*、cycle.*、
                  serialization.*、typeinfo.*、json.*、format.*、encoding.*、unicode.*、
                  unicode_ident.*、stdio_stream.*、runtime_context.*、runtime_error.*、
                  binding_registry.*、builtin_ids.h、skein_module.h
    data/        L1  提交到仓库的生成表：encoding_gb18030_data.inc、
                  encoding_gbk_validity.inc、unicode_ident_data.inc、
                  unicode_quickcheck.inc、unicode_case_data.inc
  interpreter/   L2  词法/语法/AST/求值/执行模型：
                  tokens.l、parser.y、ast.*、diagnostics.*、error_report.*、eval.*、
                  eval_control.*、eval_call.*、eval_assign.*、
                  eval_context.*、eval_helpers.h、value_runtime.*、bytecode.*、
                  compiler.*、vm.*、vm_bridge.*、vm_eval.h、vm_host.*、
                  vm_host_calls.h、gc.*、package.*、
                  std_namespace.*、pattern.*、scopestack.*、structeval.*、fiber.*、
                  generator.*、iter.*、async_support.*、task.*、event_loop.*、
                  file_lock.*、native_artifact.*、completion_index.*、repl.*、repl_ui.*、source_encoding.*
  builtins/      L3  内置函数与宿主模块：
                  builtins.*、builtins_*.cpp（math/meta/list/dict/string/convert/file/text/bytes/deque/heap/set）、
                  *_builtin.*、compression.*、cookie.*、error.*、
                  filetype.*、fs_extra.*、http.*、http1_parser.*、http2.*、io.*、
                  net.*、process_*.*、result.*、tls.*、websocket.*、worker_pool.*、
                  example_binding.*
    ffi/         L3  可选的 std.ffi：ffi.*、ffi_types.*、ffi_value.*、ffi_marshal.*、
                  ffi_callbacks.*、ffi_manifest.*、ffi_check.*、ffi_bindgen.*
  app/           L4  main.cpp（参数、入口、hook 装配）
docs/            架构、编码、FFI、Unicode、字节码与 JIT 计划文档
lib/std/         以 Skein 编写的标准库包（构建时嵌入）
contrib/mysql/   可选的 MySQL native plugin 示例与 Skein 封装
tests/           自动化回归测试（test_skein.py + cases/ + skein/）
tools/           生成器与一致性检查脚本
third_party/     llhttp、utf8proc、asmjit
cmake/           构建辅助脚本
```

每层只向本层及以下层暴露 include 目录，配合 `tools/check_boundaries.py`
同时约束源码包含关系和静态库符号依赖。

## License

Skein 以 MIT License 发布，详见 [LICENSE](LICENSE)。随仓库分发的第三方组件及其许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
