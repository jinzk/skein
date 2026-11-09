# Skein

一个使用 Flex + Bison + C++17 实现的动态类型脚本语言解释器（REPL）。

完整语法参考见 [SYNTAX.md](SYNTAX.md)。

## 构建与运行

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/skein
```

字节码 VM 默认启用；`--no-vm` / `SKEIN_VM=0` 可切回 AST 解释器。
`--vm` / `SKEIN_VM=1` 仍可用于显式指定 VM。`SKEIN_VM_TRACE=1` 会打印
字节码和每步执行，`SKEIN_VM_DEBUG=1` 会说明哪些结构回退到了 AST。详见
[docs/bytecode.md](docs/bytecode.md)。JIT spike 的路线、依赖和验收标准见
[docs/jit-plan.md](docs/jit-plan.md)。

可选 JIT 构建使用仓库内置的 `third_party/asmjit`（固定 1.11.0，
版本与来源见 `third_party/asmjit/VERSION.md`），无需额外安装 asmjit：

```bash
cmake -S . -B build -G Ninja -DSKEIN_ENABLE_JIT=ON
cmake --build build
SKEIN_JIT=1 ./build/skein benchmarks/fib_bench.skein
```

`SKEIN_JIT_THRESHOLD` 控制首次编译前的调用次数，`SKEIN_JIT_DUMP=1`
会输出生成的汇编。JIT/VM 对比基准：

```bash
python3 tools/bench_jit.py --bin ./build/skein --repeats 5 --check
```

Windows 上可选用 mimalloc 降低小对象分配开销：

```bash
cmake -S . -B build -G Ninja -DSKEIN_ENABLE_JIT=ON -DSKEIN_USE_MIMALLOC=ON
cmake --build build
```

MinGW 下该选项会自动使用静态 libstdc++/libgcc，避免动态 libstdc++ 与
mimalloc 的 `operator new/delete` 分配器不一致。

只构建 VM 后端（不支持 `--no-vm`）：

```bash
cmake -S . -B build-vm-only -G Ninja -DSKEIN_VM_ONLY=ON
cmake --build build-vm-only
```

`std.net` 使用 libuv 作为同步网络后端，因此构建需要 libuv 开发文件；`async` / `await` / generator 使用 Boost.Context；`std.regex` 使用 PCRE2 8-bit。例如 Debian/Ubuntu：

```bash
sudo apt install libuv1-dev libboost-context-dev libpcre2-dev
```

HTTP/1.1 解析器使用 vendored `llhttp`（`third_party/llhttp`），Unicode 归一化使用 vendored `utf8proc`（`third_party/utf8proc`），都不需要额外的系统包。

TLS 是可选模块；启用时需要 OpenSSL 开发文件：

```bash
sudo apt install libssl-dev
```

没有 OpenSSL 时使用 `-DSKEIN_ENABLE_TLS=OFF`，`std.tls` 和 `https://` 不可用。

`std.ffi` 是可选模块，需要 libffi >= 3.3（用于布局校验）；找不到 libffi 时会自动禁用。可以显式关闭：

```bash
cmake -S . -B build -G Ninja -DSKEIN_ENABLE_FFI=OFF
```

`std.net` 提供同步 API 和 task 形式的异步 API；libuv 只在内部驱动 socket、DNS 和 `std.task`，不向脚本暴露事件循环对象。

运行测试：

```bash
ctest --test-dir build --output-on-failure
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
  ctest --test-dir build-asan --output-on-failure -E container_bench
```

`SKEIN_TEST_TIMEOUT` 只影响测试子进程的超时，默认 5 秒；sanitizer 构建需要放宽。
generator / async 的 fiber 栈默认 2 MiB，可通过 `SKEIN_FIBER_STACK_KB`
调整（sanitizer 构建建议 8192）。AST 参考后端的调用深度可用
`SKEIN_MAX_CALL_DEPTH` 调整（sanitizer 测试使用 128）。

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

运行时、解释器与 builtins 的分层边界见 [docs/architecture.md](docs/architecture.md)。

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

## 脚本模式与 shebang

可以直接把脚本文件作为第一个参数运行：

```bash
./build/skein script.skein
```

如果脚本不是 UTF-8，可以显式指定源文件编码：

```bash
./build/skein --encoding gbk script.skein
```

该编码同样应用于 `import` 加载的包文件和 `:load` 文件。带 BOM 的 UTF-16/UTF-32 文件不需要再指定编码。

也可以使用 shebang：

```text
#!/usr/bin/env skein
print("hello");
```

```bash
chmod +x script.skein
./script.skein
```

使用 `/usr/bin/env skein` 时，请确保 `skein` 在 `PATH` 中；也可以直接写解释器的绝对路径。

脚本模式会：

- 跳过第一行的 `#!`；
- 默认按 UTF-8 读取，并自动识别 UTF-8 / UTF-16 / UTF-32 BOM；
- 没有 BOM 时可用 `--encoding NAME` 指定 GBK、GB18030 等编码；
- 不打印 `> ` 提示符；
- 不打印顶层表达式的 `= result` 结果，需要输出时请使用 `print`；
- 忽略第一行之外的普通注释仍使用 `//`；
- 使用 `skein -` 可以从 stdin 运行脚本模式；
- 脚本中出现词法、语法或运行时错误时，退出码为 `1`；
- 默认在第一处错误后停止脚本；使用 `-k` / `--keep-going` 可以继续执行后续顶层语句。

Windows/MinGW 没有原生 shebang，直接使用：

```bat
build\skein.exe script.skein
```

shebang 行同样会被跳过。

交互模式：

- 只有 stdin 是终端时才显示 `> ` / `...> ` 提示符；管道输入不显示提示符；
- 使用 `skein -i` 可以强制显示提示符；
- POSIX 终端支持方向键、Home/End、Delete、Ctrl-A/E/K/U/W、Ctrl-L；
- 支持上下方向键浏览历史；
- 历史默认保存到 `~/.skein_history`，可用 `SKEIN_HISTORY` 指定路径；
- Windows 当前回退为基础行输入，历史文件仍会保存；
- `Ctrl-C` 取消当前输入，或中断正在执行的程序；
- `Ctrl-D` 退出解释器；
- 上一次非 `null` 结果保存为 `ans`，也可以使用 `_` 引用；
- REPL 命令：
  - `:help`
  - `:quit` / `:q`
  - `:load FILE`
  - `:history`

示例：

```text
> 1 + 2
= 3
> ans * 4
= 12
> _
= 12
> :load demo.skein
> :quit
```

## 包与导入

入口脚本的包固定是 `main`，可以显式写 `package main {}`，也可以省略。包文件使用显式包块：

```text
package cc {
  export let value = 42;
  export const fixed = 7;

  export func hello(name) {
    return "hi " + name;
  }

  func private_helper() {
    return 1;
  }
}
```

- `package name {}` 创建独立作用域；
- `export let` / `export const` / `export func` 导出标识符；
- 也可以使用 `export name = expr;` 简写；
- 没有 `export` 的定义是包私有的；
- 入口脚本的包固定是 `main`；`package main {}` 可以位于任意目录，也可以省略；
- 入口脚本声明其他包名会报错；普通包文件不能直接作为入口运行；
- 被 `import` 的包名必须与所在文件夹名相同，且不能是 `main`；可以多层嵌套；
- `main` 不能被 `import`；
- stdin / REPL 没有文件包身份，`package name {}` 仍可创建临时命名空间。

### 目录结构

导入路径按文件夹层级解析：

```text
aa/
  bb/
    cc/
      cc.skein
```

```text
import aa.bb.cc;
print(cc.value);
```

也支持入口文件名为 `index.skein`：

```text
aa/bb/cc/index.skein
```

如果目录中没有 `<name>.skein` 或 `index.skein`，但只有一个 `.skein` 文件，也会把它作为入口。

### import 规则

```text
import aa.bb.cc;    // 绑定 cc
import aa.bb;       // 绑定 bb；子包通过 bb.cc 访问
import aa;          // 绑定 aa；子包通过 aa.bb.cc 访问
import aa.bb.cc as c;  // 绑定到别名 c
import std as s;       // 标准库也可使用别名

import .sibling;    // 当前包目录
import ..common;    // 上一级目录
import @top.pkg;    // 项目根目录
```

- `import aa.bb.cc` 后可以直接使用 `cc`；
- `import aa.bb` 后使用 `bb.cc`；
- `import aa.bb.cc as c` 后使用 `c`，不再绑定 `cc`；
- `.name` 相对当前包目录，`..name` 相对上一级目录；
- `@name` 只从项目根目录解析，不回退到 `-I` / 环境变量；
- 项目根通过从主脚本目录向上查找 `.skeinroot` 文件确定；没有标记时使用主脚本目录；
- 裸 `std` 是内置标准库；`.std`、`..std`、`@std` 按文件系统包路径解析；
- `main` 不能被导入；`import main;` 和 `import main.*;` 会报错；
- 普通包文件不能声明 `package main {}`；
- 别名必须是普通标识符，`as` 是保留字；
- 父包没有自己的源文件也可以作为子包容器；
- 同一个包只加载一次，之后复用缓存；
- 循环导入会报错，不会递归挂起。

包内也可以导入其他包，既可以放在包块内，也可以放在文件顶层：

```text
import aa.util;

package bb {
  export func get() {
    return util.value;
  }
}
```

### 内置标准库 std

`import std;` 会加载内置标准库，不需要文件系统目录：

```text
import std;

std.list.slice([1, 2, 3, 4], 1, 2)
std.dict.get({"a": 1}, "a")
std.json.parse("{\"a\": 1}")
std.json.stringify({"a": 1})
std.string.join(["a", "b"], ",")
std.bytes.hex(bytes_from_hex("00ff"))
std.math.sqrt(9)
std.fs.read_file("data.txt")
std.process.command_status("true")
std.type.name(1)
std.net.resolve("localhost")
```

数学补充：

```text
std.math.pi / e / tau

std.math.sign(-3)          // -1
std.math.clamp(5, 1, 3)    // 3
std.math.mod(-7, 3)        // 2
std.math.gcd(12, 18)       // 6
std.math.lcm(4, 6)         // 12
std.math.degrees(std.math.pi)  // 180.0
std.math.is_nan(float("nan"))  // true
```

`sqrt(-1)`、`log(0)`、`log2(0)`、`log10(负数)` 返回 `nan`。`round` 的半值向远离零方向，例如 `round(-1.5) == -2`。`fmod` 始终返回 `float`；`mod` 的结果符号跟随除数。`gcd` / `lcm` 只接受整数并返回非负值。

TCP：

```text
import std;

let s = std.net.tcp_connect("127.0.0.1", 8080);
std.net.send(s, "hello");
let data = std.net.recv(s, 4096);
print(bytes_to_string(data));
std.net.close(s);
```

网络函数支持可选超时和地址族：

```text
tcp_connect(host, port[, timeout_ms[, family]])
tcp_listen(host, port[, timeout_ms[, family[, options]]])
accept(listener[, timeout_ms])
send(socket, data[, timeout_ms])
recv(socket, size[, timeout_ms])
resolve(host[, timeout_ms[, family]])
shutdown(socket[, timeout_ms])
```

`timeout_ms` 为 `0` 或省略时不设置超时；正数表示一次调用的总超时时间，检查精度约 10ms。
`family` 支持 `"any"`、`"ipv4"`、`"ipv6"`。
`"any"` 会使用解析结果中的第一个可绑定地址；需要同时监听 IPv4/IPv6 时应分别创建 listener。

`tcp_listen` 的 `options`：

```text
reuse_port  bool，默认 false
```

`reuse_port` 使用 `SO_REUSEPORT`，仅 Linux/FreeBSD/DragonFly/Solaris/AIX 支持；
其他平台传 `true` 会报错。多个 listener 绑定同一端口时，所有 listener
都必须启用 `reuse_port`。

地址查询：

```text
std.net.local_addr(socket)
std.net.peer_addr(socket)
```

返回：

```text
{address: "127.0.0.1", port: 8080, family: "ipv4"}
```

`tcp_listen("127.0.0.1", 0)` 后可以用 `local_addr` 获取实际监听端口。

异步网络：

```text
let t = std.net.tcp_connect_async("127.0.0.1", 8080, 1000);
let socket = std.task.wait(t);
```

异步函数：

```text
tcp_connect_async(host, port[, timeout_ms[, family]]) → task
tcp_listen_async(host, port[, timeout_ms[, family]])  → task
accept_async(listener[, timeout_ms])                  → task
send_async(socket, data[, timeout_ms])                → task
recv_async(socket, size[, timeout_ms])                → task
resolve_async(host[, timeout_ms[, family]])           → task
shutdown_async(socket[, timeout_ms])                  → task
```

- task 完成值分别是 socket、listener、发送字节数、bytes、地址 list 或 null；
- `timeout_ms` 是异步操作自身的超时，不是 `std.task.wait` 的超时；
- `std.task.cancel(task)` 会取消底层网络操作；
- 可以同时启动多个 task，再用 `std.task.wait_all` 等待；
- `std.net.close` 仍是同步操作。

Task：

```text
import std;

let a = std.task.sleep(100);
let b = std.task.yield();

std.task.done(a)
std.task.wait(a)
std.task.wait_all([a, b])
```

Task API：

```text
std.task.sleep(ms)            → task
std.task.yield()              → task
std.task.wait(task[, timeout_ms]) → result
std.task.wait_all(tasks[, timeout_ms]) → list
std.task.cancel(task)         → true / false
std.task.done(task)           → true / false
std.task.catch(task)          → result task
std.task.try_wait(task[, timeout_ms]) → result
std.task.try_wait_all(tasks[, timeout_ms]) → result
std.task.retry(factory[, options]) → result
std.task.timeout(task, timeout_ms) → result
std.task.rate_limit(rate, burst) → rate_limiter
std.task.acquire(limiter[, tokens]) → bool
std.task.bounded_parallel_map(items, limit, fn) → list[result]
```

可靠性工具：

```text
import std;

let r = std.task.retry(
  func() { return fetch_job(); },
  {"attempts": 5, "delay_ms": 100, "backoff": 2.0, "retry_on": "network"}
);

let t = std.task.timeout(std.task.sleep(100), 50);  // result

let limiter = std.task.rate_limit(10, 2);           // 10 次/秒，突发 2
std.task.acquire(limiter);

let results = std.task.bounded_parallel_map(
  jobs, 4, func(job) { return run(job); });
```

`retry` 的 options 支持 `attempts`、`delay_ms`、`backoff`、`max_delay_ms`、`retry_on`（错误 kind 或列表）。`retry` / `timeout` / `bounded_parallel_map` 都返回 `result`；`bounded_parallel_map` 保持输入顺序，单项失败记为 `err`。

- `sleep` / `yield` 是目前内置的原生 task；
- `std.task.yield` 是函数字段；单独的 `yield` 关键字用于生成器；
- `wait` 超时不会自动取消 task，需要显式 `cancel`；
- 退出解释器时会取消剩余 task；
- `std.net.*_async` 已经复用同一套 task 和事件循环。

HTTP/1.1 客户端：

```text
import std;

let r = std.http.get("http://127.0.0.1:8080/api");

r["status"]                         // 200
r["headers"]["content-type"]        // text/plain
bytes_to_string(r["body"])
```

带 headers 和 timeout：

```text
let r = std.http.get("http://127.0.0.1:8080/api", {
  "headers": {"Accept": "application/json"},
  "timeout_ms": 1000
});
```

POST：

```text
let r = std.http.post("http://127.0.0.1:8080/items", "hello", {
  "headers": {"Content-Type": "text/plain"},
  "timeout_ms": 1000
});
```

自定义方法：

```text
let r = std.http.request("DELETE", "http://127.0.0.1:8080/items/1", {
  "timeout_ms": 1000
});
```

异步客户端：

```text
let t = std.http.get_async("http://127.0.0.1:8080/api");
let r = await t;

let t2 = std.http.post_async("http://127.0.0.1:8080/items", "hello");
let r2 = await t2;

let t3 = std.http.request_async("DELETE", "http://127.0.0.1:8080/items/1");
await t3;
```

异步客户端返回 task，可以 `await`、`std.task.catch`、`std.task.wait` 或 `cancel`。

HTTPS 同步客户端：

```text
let r = std.http.get("https://example.com/", {
  "tls": {
    "ca_file": "/etc/ssl/certs/ca-certificates.crt"
  }
});
```

HTTPS 同步服务端：

```text
let s = std.http.listen_tls("127.0.0.1", 8443, "cert.pem", "key.pem");
let req = std.http.accept(s, 5000);
std.http.respond(req, 200, {"Content-Type": "text/plain"}, "secure");
std.http.close(s);
```

HTTP/1.1 服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);

let req = std.http.accept(s, 5000);

std.http.respond(req, 200, {
  "Content-Type": "text/plain"
}, "hello " + req["path"]);

std.http.close(s);
```

异步服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);

let req = await std.http.accept_async(s, 5000);

let sent = std.http.respond_async(req, 200, {
  "Content-Type": "text/plain"
}, "hello " + req["path"]);
await sent;

std.http.close(s);
```

协议升级（Upgrade）：

客户端：

```text
let r = std.http.get(url, {
  "upgrade": true,
  "headers": {"Upgrade": "websocket"}
});

if (r["upgraded"]) {
  std.io.write_all(r["socket"], "hello");
  bytes_to_string(std.io.read(r["socket"], 4096));
  std.io.close(r["socket"]);
}
```

服务端：

```text
let req = std.http.accept(s, 5000);
let stream = std.http.upgrade(req, {"X-Test": "yes"});

std.io.write_all(stream, "hello");
bytes_to_string(std.io.read(stream, 4096));
std.io.close(stream);
```

- `upgrade: true` 时客户端使用独立连接，不进入 keep-alive 连接池；
- 服务端返回 `101` 时，响应 dict 包含 `"upgraded": true` 和 `"socket"`；
- `std.http.upgrade(request[, headers])` 发送 `101 Switching Protocols` 并返回原始 stream；
- 升级后的读写使用 `std.io`；
- 当前只支持 HTTP/1.1 Upgrade；HTTP/2 extended CONNECT 尚未实现。

WebSocket：

```text
std.websocket.connect(url[, options])          → ws
std.websocket.connect_async(url[, options])    → task
std.websocket.accept(request)                  → ws
std.websocket.send(ws, data[, timeout_ms])     → bool
std.websocket.send_async(ws, data[, timeout_ms]) → task
std.websocket.receive(ws[, timeout_ms])        → message
std.websocket.receive_async(ws[, timeout_ms])  → task
std.websocket.ping(ws[, data])                 → bool
std.websocket.close(ws[, code[, reason]])      → bool
```

客户端：

```text
let ws = std.websocket.connect("ws://127.0.0.1:8080/");

std.websocket.send(ws, "hello");

let msg = std.websocket.receive(ws);
print(msg["type"] + ": " + msg["data"]);

std.websocket.close(ws);
```

服务端：

```text
let req = std.http.accept(s, 5000);
let ws = std.websocket.accept(req);

let msg = std.websocket.receive(ws);
std.websocket.send(ws, "echo:" + msg["data"]);
std.websocket.close(ws);
```

消息 dict：

```text
text   → {"type": "text", "data": string}
binary → {"type": "binary", "data": bytes}
pong   → {"type": "pong", "data": bytes}
close  → {"type": "close", "code": int, "reason": string}
```

规则：

- 支持 `ws://` 和 `wss://`；
- 客户端发出的帧自动 masking，服务端发出的帧不 masking；
- 自动回复 ping；
- 支持分片消息重组；
- 单条消息默认上限 16 MiB；
- `send` 对 string 使用 text 帧，对 bytes 使用 binary 帧；
- `close` 发送 close 帧并关闭底层 stream；
- 目前只支持 HTTP/1.1 WebSocket 握手，不支持 HTTP/2 extended CONNECT。

keep-alive 服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);

let served = std.http.serve(s, func(req) {
  return {
    "status": 200,
    "headers": {"Content-Type": "text/plain"},
    "body": "hello " + req["path"]
  };
}, 100);

std.http.close(s);
```

`std.http.serve(server, handler[, max_requests[, max_per_connection[, idle_timeout_ms]]])`：

- handler 接收 request dict；
- 必须返回 response dict；
- handler 可以是 `async func`，等待期间其他连接可以继续处理；
- 同一个连接可以处理多个请求；
- 每个客户端连接在独立 fiber 中处理，多个连接可以并发服务；
- 请求允许 keep-alive 时继续复用；
- `max_requests = 0` 表示总请求数不限制；
- `max_per_connection` 默认 100，`0` 表示不限制；
- `idle_timeout_ms` 默认 0，表示不设置连接空闲超时；
- 达到限制或空闲超时后发送 `Connection: close` 并关闭连接；
- 返回已处理请求数。

主进程 accept + 子进程处理：

```text
std.http.serve_workers(server, workers, script
                       [, max_requests[, max_per_connection
                       [, idle_timeout_ms]]]) -> served
```

- HTTP 连接、TLS、keep-alive 仍由主进程处理；
- 每个请求通过 hybrid IPC 发给 worker；
- worker 必须使用 `{"protocol":"hybrid"}`；
- worker 的 `params` 为 `[method, path, headers]`，请求 body 走 raw body；
- worker 返回的 `result` 应为 `{"status": ..., "headers": ...}`，
  response body 走 raw body；
- `server["worker_timeout_ms"]` 控制单次 worker 调用超时，
  默认 30000，`0` 表示不超时；
- worker 调用使用异步等待，多个连接可以并发占用不同 worker；
- 当前不支持 `stream_request_body`，该模式返回 501；
- WebSocket upgrade / SSE 仍应使用独立 HTTP 服务或后续 socket handoff。

```text
let s = std.http.listen("127.0.0.1", 8080);
let served = std.http.serve_workers(
  s, 4, "worker.skein", 1000);
```

进程内线程池处理：

```text
std.http.serve_threads(server, workers, script[, options])
  -> ThreadServer

ThreadServer.stop()
ThreadServer.join() -> served
ThreadServer.stats() -> dict
ThreadServer.strategy -> string
ThreadServer.capabilities -> dict
ThreadServer.served -> int
```

`options`：

```text
entry              worker 入口，默认 "handle"
strategy           "auto" / "dispatch" / "reuse_port"，默认 "auto"
queue              pool 队列，默认 1024 或 "unbounded"
restart_limit      worker 重启次数，默认 0
worker_vm          默认 false
worker_timeout_ms  单请求 worker 超时，默认 30000，0 表示不超时
max_requests       默认 0
max_per_connection 默认 100
idle_timeout_ms    默认 0
max_connections    默认 0
max_header_bytes   默认 64 KiB
max_headers        默认 100
max_body_bytes     默认 16 MiB
h2_max_concurrent_streams 默认 16（reuse_port HTTP/2）
require            能力列表，默认空
on_unsupported     缺失能力时："error"（默认）/ "501" / "warn"
```

worker 脚本导出 `handle(req)`，返回 response dict：

```skein
func handle(req) {
    return {
        "status": 200,
        "headers": {"Content-Type": "text/plain"},
        "body": "hello " + req["path"]
    };
}
```

规则：

- `strategy: "auto"` 在 Linux/BSD 选择 `reuse_port`，在 Windows 选择 `dispatch`；
- `dispatch`：accept、TLS、HTTP/1.1 解析和 keep-alive 在主线程 event loop 中完成，每个请求通过线程池派发到 worker；
- `reuse_port`：每个 worker 线程独立 bind + listen + accept + TLS + HTTP 解析，并在本地执行 `handle(req)`；
- `reuse_port` 仅 Linux/FreeBSD/DragonFly/Solaris/AIX 支持；其他平台显式请求会报错；
- `reuse_port` 使用 `max_requests` 的共享原子计数，超过上限的请求返回 503；
- `reuse_port` + TLS 时通过 ALPN 同时支持 `h2` 和 `http/1.1`；明文 reuse_port 当前为 HTTP/1.1；
- `reuse_port` 支持 `stream_request_body`，worker 直接调用 `std.http.read_body(req)`，不跨线程传输 body；
- `dispatch` 不支持 `stream_request_body`，启动时报错；
- request dict 包含 `method/path/headers/body/http_major/http_minor/keep_alive/remote_addr/remote_port`，不包含 `socket`；
- reuse_port 的 request dict 额外包含 `worker_id`；
- worker 返回值必须是 `{status, headers, body}`；
- dispatch 下 worker 崩溃返回 502，worker 超时返回 504，worker 全部退出返回 503；
- `serve_threads` 立即返回 `ThreadServer`，`join()` 等待服务结束并返回已处理请求数；
- capabilities 报告 `http1` / `http2` / `stream_request_body` / `reuse_port` / `strategies` / `platform`；
- `require` 在启动时检查能力：缺失时按 `on_unsupported` 处理；
  - `"error"`：启动失败；
  - `"warn"`：输出警告并继续；
  - `"501"`：启动成功，但请求返回 501；
- WebSocket upgrade 与 SSE 请求在 `serve_threads` 下返回 501；
- `max_requests` 达到后停止 accept，连接任务排空后退出。

```skein
import std;

let s = std.http.listen("127.0.0.1", 8080);
let ts = std.http.serve_threads(
  s, 4, "worker.skein", {"max_requests": 1000});
print(ts.strategy);
print(ts.capabilities["http1"]);
let served = ts.join();
```

能力矩阵：

| 能力 | dispatch | reuse_port |
|---|---|---|
| HTTP/1.1 | 是 | 是 |
| HTTP/2 | 否 | TLS ALPN `h2` |
| TLS | 是 | 是 |
| keep-alive | 是 | 是 |
| `stream_request_body` | 否（启动失败） | 是 |
| `stream_response` | 否 | 是 |
| WebSocket | 否（501） | 否（501） |
| SSE | 否（501） | 否（501） |
| raw socket | 否 | 否 |
| `max_requests` | 单线程 | 全局 atomic |

`require` 示例：

```skein
let ts = std.http.serve_threads(
  s, 4, "worker.skein",
  {"strategy": "reuse_port",
   "require": ["http2", "stream_request_body"]});
```

流式 body：

```text
func parts() {
  yield "hello";
  yield " ";
  yield "world";
}

let sent = std.http.post(url, {
  "body_stream": parts()
});

let received = std.http.get(url, {
  "on_body": func(chunk) {
    print("chunk: " + bytes_to_string(chunk));
  }
});
```

- `body_stream` 必须是生成器，逐块产生 string/bytes；
- 使用 `body_stream` 时请求自动改为 `Transfer-Encoding: chunked`；
- `body` 与 `body_stream` 不能同时设置；
- `on_body` 逐块接收响应 body，此时响应 dict 的 `body` 为空；
- `on_body` 和 `body_stream` 都会关闭自动重试；
- `max_body_bytes` 默认 16 MiB，对 `on_body` 流式接收同样生效。

服务端也可以返回流式响应：

```text
func parts() {
  yield "hello";
  yield " ";
  yield "world";
}

let served = std.http.serve(s, func(req) {
  return {
    "status": 200,
    "headers": {"Content-Type": "text/plain"},
    "body_stream": parts()
  };
});
```

- `std.http.serve` 的响应 dict 支持 `body_stream`；
- `std.http.respond` 的 body 参数也可以是生成器；
- HTTP/1.1 流式响应自动使用 `Transfer-Encoding: chunked`；
- HTTP/2 流式响应通过 data provider 发送。

服务端请求 body 流式回调：

```text
let s = std.http.listen("127.0.0.1", 8080);

s["on_request_body"] = func(chunk) {
  print(bytes_to_string(chunk));
};
```

- `on_request_body` 在请求 body 到达时逐块调用；
- 设置后 handler 的 `req["body"]` 为空，需要完整 body 时应在回调中自行累积；
- HTTP/2 多个 stream 的请求 body 可能交错回调；
- 未设置时仍整体读取请求 body；
- `max_body_bytes` 同时限制流式请求 body。

handler 拉取请求 body：

```text
let s = std.http.listen("127.0.0.1", 8080);
s["stream_request_body"] = true;

let served = std.http.serve(s, func(req) {
  let body = "";
  let chunk = std.http.read_body(req);
  while (chunk != null) {
    body = body + bytes_to_string(chunk);
    chunk = std.http.read_body(req);
  }
  return {"status": 200, "body": "read " + body};
});
```

- `stream_request_body` 为 `true` 时，handler 在请求 headers 到达后立即执行；
- 此时 `req["body"]` 为空，使用 `std.http.read_body(req)` 逐块拉取；
- `std.http.read_body` 返回 `bytes`，请求 body 结束时返回 `null`；
- 不能与 `on_request_body` 同时设置；
- HTTP/1.1 下如果 handler 未读完 body 就返回响应，连接会在响应后关闭；
- HTTP/2 下未读取的 body 会被丢弃，其他 stream 不受影响；
- `max_body_bytes` 同时限制拉取式请求 body；
- `std.http2.serve` 使用相同的 `stream_request_body` 和 `std.http.read_body`。

请求 dict：

```text
method, path, headers, body, http_major, http_minor, socket
```

启用 `stream_request_body` 后请求 dict 还包含 `body_reader`，供
`std.http.read_body` 使用。

响应 dict：

```text
status, status_text, headers, body, http_major, http_minor
```

响应 dict 还可以使用 `body_stream` 代替 `body`。

限制：

- 支持 `http://` 和 `https://`；
- `https://` 支持同步和 async 客户端/服务端；
- 只支持 HTTP/1.1；HTTP/2 使用 `std.http2`；
- 同步和 async 客户端共用 HTTP/1.1 keep-alive 连接池（空闲 30 秒后关闭）；
- 复用连接失败时，幂等方法（GET/HEAD/OPTIONS/TRACE）会自动用新连接重试一次；
- `std.http.serve` 支持服务端 keep-alive；
- `std.http.accept` / `respond` 仍是一次连接一个请求；
- 默认整体读取请求和响应 body；可用 `body_stream` / `on_body` 流式处理；
- 客户端 `options` 还支持 `upgrade`，用于发起 HTTP/1.1 协议升级；
- `max_body_bytes` 默认 16 MiB；
- 同名 header 在 dict 中只保留最后一个；
- `std.http` 客户端和服务端都提供同步和 async 接口。

重定向：

```text
let r = std.http.get(url, {
  "follow_redirects": true,
  "max_redirects": 5
});
```

- `follow_redirects` 默认 `false`；
- `max_redirects` 默认 5，`0` 表示不跟随；
- 支持 `301` / `302` / `303` / `307` / `308`；
- `303` 和 `301/302` 的 POST 会改为 GET 并丢弃 body；
- `307` / `308` 保留方法和 body；
- `Location` 支持绝对 URL、`//host/path`、`/path`、`?query` 和相对路径；
- 跨 origin 重定向会删除 `Authorization` / `Cookie`；
- 重定向时删除 `Host`、`Content-Length`、`Transfer-Encoding`、`Connection`、`Upgrade`；
- `body_stream` 无法重放：`301/302/303` 会丢弃 body，`307/308` 会报错；
- 同步和 async 客户端都支持。

Cookie：

```text
let jar = {};

let r = std.http.get(url, {"cookie_jar": jar});
let cookies = std.http.cookies(r);

let next = std.http.get(url + "/next", {"cookie_jar": jar});
```

- `cookie_jar` 是普通 dict，可跨请求复用；
- 自动读取响应中的 `Set-Cookie`；
- 自动发送匹配 domain / path / secure / expires 的 `Cookie`；
- `Max-Age=0` 会删除对应 cookie；
- 用户显式提供 `Cookie` header 时不会覆盖；
- `std.http.cookies(response)` 返回解析后的 cookie 列表。

cookie dict：

```text
name
value
domain
path
secure
http_only
max_age
expires
same_site
host_only
```

- HTTP/1.1 客户端支持自动 cookie jar；
- HTTP/2 响应也提供 `set_cookie`，可用 `std.http.cookies` 解析；
- HTTP/2 单请求和 `request_many` 都支持自动 cookie jar。

压缩：

```text
let r = std.http.get(url, {"decompress": true});
print(bytes_to_string(r["body"]));
```

- `decompress` 默认 `false`；
- 开启后自动添加 `Accept-Encoding: gzip, deflate, br`（取决于构建支持）；
- 自动解码 `Content-Encoding`；
- 支持 `gzip`、`deflate`、`br`；
- 解码后响应带 `"decompressed": true`；
- 删除响应 header 中的 `Content-Encoding`，并更新 `Content-Length`；
- 不能与 `on_body` 同时使用；
- 同步和 async 客户端都支持；
- 构建选项 `-DSKEIN_ENABLE_COMPRESSION=OFF` 可关闭；
- HTTP/2 单请求和 `request_many` 都支持自动解压。


资源限制：

客户端：

```text
let r = std.http.get(url, {
  "max_header_bytes": 65536,
  "max_headers": 100,
  "max_body_bytes": 1048576
});
```

服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);

s["max_header_bytes"] = 65536;
s["max_headers"] = 100;
s["max_body_bytes"] = 1048576;
s["max_connections"] = 100;
```

- 超过限制时请求失败或连接关闭；
- `max_connections = 0` 表示不限制；
- `std.http.accept` / `accept_async` 也使用 server dict 中的限制；
- HTTP/2 使用 `max_body_bytes` / `max_connections`，header 限制通过 `settings.max_header_list_size`。

HTTP/2：

```text
std.http2.request(method, url[, options]) → response
std.http2.get(url[, options])           → response
std.http2.post(url[, body[, options]])  → response
std.http2.request_async(method, url[, options]) → task
std.http2.get_async(url[, options])             → task
std.http2.post_async(url[, body[, options]])    → task
std.http2.request_many(requests[, options])     → list
std.http2.request_many_async(requests[, options]) → task
std.http2.listen(host, port[, timeout_ms])      → server
std.http2.listen_tls(host, port, cert, key[, timeout_ms]) → server
std.http2.serve(server, handler[, max_requests[, max_per_connection[, idle_timeout_ms]]]) → int
std.http2.close(server)
```

客户端：

```text
let a = std.http2.get("https://example.com/", {
  "headers": {"Accept": "text/plain"},
  "tls": {"verify": true, "ca_file": "/etc/ssl/certs/ca-certificates.crt"}
});

print(str(a["status"]) + " " + a["protocol"]);
print(bytes_to_string(a["body"]));

let b = std.http2.post("https://example.com/echo", "payload");
let c = await std.http2.get_async("https://example.com/async");
```

批量并发请求（同一连接、多个 stream）：

```text
let responses = std.http2.request_many([
  {"url": "https://example.com/a"},
  {"url": "https://example.com/b", "method": "POST", "body": "payload"},
  {"url": "https://example.com/c", "method": "PUT", "body": "x"}
]);

let responses2 = await std.http2.request_many_async([
  {"url": "https://example.com/d"},
  {"url": "https://example.com/e", "method": "DELETE"}
]);
```

- 所有请求必须属于同一个 origin（scheme + host + port + TLS 配置）；
- 请求在同一个 HTTP/2 连接上同时提交为多个 stream；
- 返回的列表顺序与输入顺序一致；
- 每个请求项支持 `url`、`method`、`headers`、`body`、`body_stream`、`on_body`；
- 每个请求项可以独立流式发送请求 body 和流式接收响应 body；
- `options` 中的 `cookie_jar` / `decompress` 会分别应用到每个请求项的 URL / path。

服务端：

```text
let s = std.http2.listen("127.0.0.1", 8080);

let served = std.http2.serve(s, func(req) {
  return {
    "status": 200,
    "headers": {"Content-Type": "text/plain"},
    "body": "h2 " + req["method"] + " " + req["path"]
  };
}, 100);

std.http2.close(s);
```

规则：

- `http://` 使用 h2c prior knowledge；
- `https://` 通过 TLS ALPN 协商 `h2`，未协商成功会报错；
- 支持同步和 async 客户端；
- 支持 h2c / TLS 服务端；
- 默认整体读取请求和响应 body；可用 `body_stream` / `on_body` 流式处理；
- `max_body_bytes` 默认 16 MiB；
- request dict 额外包含 `connection_id`，便于标识所属连接；
- 支持 `body_stream` 生成器流式发送请求 body；
- 支持 `on_body` 回调流式接收响应 body；
- 服务端响应 dict 支持 `body_stream` 流式发送；
- async 服务端 handler 可在多个 stream 之间并发执行；
- 客户端 `options` 支持 `headers`、`body`、`body_stream`、`on_body`、`timeout_ms`、`max_body_bytes`、`tls`；
- 服务端 handler 接收 request dict，返回 response dict；
- 每个请求在独立 fiber 中执行，async handler 可以跨 stream 并发；
- 每个客户端连接也在独立 fiber 中处理，多个连接可以并发服务；
- 达到 `max_requests` 后进入 graceful drain：停止接受新连接和新 stream，已启动的 handler 会继续完成并发送响应；
- 达到 `max_per_connection` 后关闭当前连接，未处理的 stream 由客户端按幂等规则重试；
- `max_requests = 0` 表示总请求数不限制；
- `max_per_connection` 默认 100，`0` 表示不限制；
- `idle_timeout_ms` 默认 0，表示不设置连接空闲超时；
- 同步和 async 客户端共用 HTTP/2 连接池；
- 同步请求按顺序复用连接；
- async 的 `get_async` / `post_async` / `request_async` 可以共享同一连接并并发执行；
- async 流式请求（`on_body` / `body_stream`）也可以共享同一连接；
- `request_many` / `request_many_async` 在单连接上并发提交多个 stream；
- 收到 GOAWAY 或共享连接失败时，async 幂等请求会换新连接重试一次；
- 使用 `body_stream` / `on_body` 的请求不自动重试。

HTTP/2 SETTINGS 与 flow control：

客户端：

```text
let a = std.http2.get(url, {
  "settings": {"initial_window_size": 1048576}
});
```

服务端：

```text
let s = std.http2.listen("127.0.0.1", 8080);

s["settings"] = {
  "max_concurrent_streams": 100,
  "initial_window_size": 262144
};
```

支持的设置项：

```text
max_concurrent_streams   // > 0
initial_window_size      // 1 .. 2147483647
max_frame_size           // 16384 .. 16777215
max_header_list_size     // > 0
enable_push              // 仅客户端，bool
```

- 未设置时使用 nghttp2 默认值；
- `settings` 参与连接池 key，不同 settings 不会错误复用同一连接；
- 批量请求在提交前会等待服务端 SETTINGS，避免超过服务端并发 stream 限制；
- `initial_window_size` 可以用于调大单 stream 流控窗口。

TLS：

```text
std.tls.client(stream, server_name[, options]) → tls_stream
std.tls.server(stream, cert_file, key_file[, options]) → tls_stream
std.tls.alpn(tls_stream)                       → string
```

客户端：

```text
let socket = std.net.tcp_connect("127.0.0.1", 8443, 1000);

let tls = std.tls.client(socket, "example.com", {
  "verify": true,
  "ca_file": "/etc/ssl/certs/ca-certificates.crt",
  "alpn": ["h2", "http/1.1"]
});

std.io.write_all(tls, "hello");
bytes_to_string(std.io.read(tls, 4096));
std.io.close(tls);
```

服务端：

```text
let listener = std.net.tcp_listen("127.0.0.1", 8443);
let client = std.net.accept(listener, 5000);

let tls = std.tls.server(client, "cert.pem", "key.pem", {
  "alpn": ["h2", "http/1.1"]
});

let data = std.io.read(tls, 4096);
std.io.write_all(tls, data);
std.io.close(tls);
std.net.close(listener);
```

选项：

```text
verify    // bool，客户端默认 true
ca_file   // 客户端 CA 文件
alpn      // 客户端/服务端 ALPN 协议列表
```

`std.tls.alpn(stream)` 返回握手后协商出的协议：

```text
std.tls.alpn(tls)   // "h2" / "http/1.1" / ""
```

规则：

- 默认校验证书和主机名；
- 测试环境可显式 `"verify": false`；
- TLS 流实现 `std.io` 接口；
- TLS 流支持 `std.io.read_async` / `write_async`；
- TLS 握手在 `std.tls.client/server` 包装时同步完成；
- 服务端 ALPN 会按服务端列表选择第一个客户端也支持的协议；

async / await：

```text
import std;

async func fetch(port) {
  let socket = await std.net.tcp_connect_async("127.0.0.1", port, 1000);
  let data = await std.net.recv_async(socket, 4096, 1000);
  std.net.close(socket);
  return data;
}

let a = fetch(8080);
let b = fetch(8081);

let da = await a;
let db = await b;
```

规则：

- `async` 修饰函数，调用后返回 task；
- async 函数调用后立即开始运行，遇到第一个 `await` 时挂起；
- `await` 只能用于 async 函数或顶层；
- 普通函数不能使用 `await`；
- 多个 async 函数可以先启动，再分别 await，从而并发执行；
- async 函数内部错误会变成 task 失败；
- `await` 未捕获的 task 失败会报错，`await std.task.catch(task)` 可以恢复；
- `yield` 不用于 async；主动让出使用 `await std.task.yield()`；
- 协程栈由 Boost.Context 提供，支持 Boost.Context 覆盖的平台。

生成器：

```text
import std;

func numbers() {
  yield 1;
  yield 2;
  yield 3;
}

let g = numbers();

std.generator.next(g)
std.generator.next(g)
std.generator.next(g)
std.generator.next(g)
std.generator.done(g)
```

返回：

```text
{done: false, value: 1}
{done: false, value: 2}
{done: false, value: 3}
{done: true, value: null}
true
```

规则：

- 函数体中出现 `yield` 时，该函数自动成为生成器；
- 生成器调用后不会立即执行函数体，第一次 `next` 才开始；
- `yield expr` 把值交给调用方并挂起；
- `yield;` 等价于 `yield null;`；
- `yield` 表达式恢复时会返回 `next(value)` 发送的值；
- `return` 的返回值作为生成器结束时 `{done: true, value: ...}` 的 value；
- 可以直接迭代生成器：
  ```text
  for (let x in numbers()) {
    print(x);
  }
  ```
- `std.generator.next(g[, value])` 支持向生成器发送值；第一次 `next` 的发送值会被忽略；
- `std.generator.close(g)` 关闭生成器并执行 `defer`；
- 挂起生成器被直接丢弃且仍有待执行 `defer` 时，会打印 warning，但不会执行清理；
- 提前 `break` 出 `for (let x in g())` 时，临时生成器会被丢弃并触发 warning；需要清理时先绑定变量并 `defer std.generator.close(g)`；
- 未开始的生成器调用 `close` 不会执行函数体；
- 已结束的生成器调用 `close` 返回 `false`；
- `close` 期间 `defer` 抛出的 `fail` 会传给调用方，可被 `on error` / `std.result.capture` 捕获；
- 正在运行的生成器不能关闭自己；
- 不提供 `std.generator.throw`；生成器内部错误使用 `fail`，调用方使用 `on error` / `std.result.capture`；
- async 生成器暂不支持；
- 生成器依赖 Boost.Context 提供的协程栈。

向生成器发送值：

```text
func echo() {
  let x = yield 1;
  yield x + 1;
}

let g = echo();
std.generator.next(g)       // {done: false, value: 1}
std.generator.next(g, 10)   // {done: false, value: 11}
```

关闭生成器：

```text
func worker() {
  defer print("cleanup");
  yield 1;
  print("not reached after close");
}

let g = worker();
std.generator.next(g)
std.generator.close(g)      // cleanup，返回 true
std.generator.done(g)       // true
std.generator.next(g)       // {done: true, value: null}
```

错误传播：

```text
func broken() {
  fail std.error.new("io", "generator boom");
  yield 1;
}

let g = broken();
let v = std.generator.next(g) on error(e) {
  print(std.error.kind(e));    // io
  print(str(e));               // generator boom
  recover 0;
};
```

没有 `std.generator.throw`。需要从外部注入错误时，用 `next(g, error)` 发送一个 `error` 值，生成器内部再显式 `fail` 它：

```text
func worker() {
  let signal = yield 1;
  if (signal is error) {
    fail signal;
  }
  yield 2;
}

let g = worker();
std.generator.next(g)

let v = std.generator.next(g, std.error.new("cancel", "stop")) on error(e) {
  print(std.error.kind(e));    // cancel
  print(str(e));               // stop
  recover 0;
};
```

生成器内部错误使用 `fail`，调用方用 `on error` 或 `std.result.capture` 恢复。

错误值：

`fail`、运行时错误、`result`、task 失败和 generator 失败共用同一个 `error` 值：

```text
error(kind, message)
```

```text
import std;

let e = std.error.new("io", "connection refused");
e is error
std.error.kind(e)
std.error.message(e)
str(e)
```

`fail` 和 `std.result.err` 会自动把普通值包装成 `error`：

```text
fail "boom";              // error(kind="fail", message="boom")
std.result.err("boom");   // error(kind="error", message="boom")
1 / 0;                    // error(kind="runtime", message="division by zero")
```

同步错误捕获：

```text
import std;

let r = std.result.capture(func() {
  return std.net.tcp_connect("127.0.0.1", 1, 100);
});

std.result.is_err(r)
std.result.error(r)
```

`on error` 同时捕获显式 `fail`、运行时错误和 `result.err`：

```text
let v = (1 / 0) on error(e) {
  print(std.error.kind(e));   // runtime
  print(str(e));              // division by zero
  recover 42;
};
```

`on error` 也是 `result` 的统一消费语法：

```text
func parse(s) {
  if (s == "") { return std.result.err("empty"); }
  return std.result.ok(42);
}

let n = parse(input) on error(e) {
  print(str(e));
  recover 0;
};
```

规则：

| `expr` 的结果 | `on error` 的行为 |
|---|---|
| `ok(v)` | 表达式值是 `v` |
| `err(e)` | 执行 handler |
| `fail` / 运行时错误 | 执行 handler |
| 普通值 | 原样返回 |

- `ok` 只解包一层；
- `on error` 不会自动解包嵌套在普通表达式里的 result；
- 如果只想拿到 result 本身，不要对它使用 `on error`。

`std.result.unwrap` 在 `err` 时通过 `fail` 抛出，因此可以被 `on error` 捕获：

```text
let v = std.result.unwrap(std.result.err("boom")) on error(e) {
  recover str(e);
};
```

异步错误恢复：

```text
let t = std.net.tcp_connect_async("127.0.0.1", 1, 1000);
let r = await std.task.catch(t);

if (std.result.is_err(r)) {
  print(std.result.error(r));
}
```

Result API：

```text
std.result.ok(value)          → result
std.result.err(message)       → result
std.result.is_ok(result)      → bool
std.result.is_err(result)     → bool
std.result.unwrap(result)     → value / fail(error)
std.result.unwrap_or(result, default)
std.result.value(result)      → value / null
std.result.error(result)      → error / null
std.result.capture(fn)        → result

std.error.new(kind, message)  → error
std.error.new(message)        → error(kind="error")
std.error.kind(error)         → string
std.error.message(error)      → string
```

规则：

- `result` 和 `error` 是独立值类型，不是 dict；
- `ok(result)` 为真，`err(result)` 为假；
- `std.result.error(result)` 返回 `error` 值，`str(error)` 返回消息；
- `fail` 和 `std.result.err` 会把普通值包装成 `error`；
- `capture` 捕获同步 `fail` 和运行时错误，并转换成 result；
- `capture` 不打印被捕获的错误；
- `std.result.unwrap` 在 `err` 时通过 `fail` 抛出；
- `std.task.catch(task)` 返回一个总是以 result 完成的 task；
- async 函数内部使用 `await std.task.catch(task)` 恢复失败；
- `std.task.try_wait` / `try_wait_all` 是同步 result 等待；
- `on error` 捕获显式 `fail`、运行时错误和 `result.err`，并对 `result.ok` 解包。

defer：

```text
func work() {
  let h = file_open("x");
  defer file_close(h);

  ...
}
```

规则：

- `defer expr;` 在离开当前函数或代码块时执行；
- 多个 defer 按 LIFO 执行；
- 支持 return / break / continue / 运行时错误；
- 没有其他错误时，defer 内的错误会继续向外传播；
- 已有错误正在展开时，原错误优先，defer 错误以 `error in defer: ...` 作为附加诊断报告；
- 顶层 `defer` 不允许，必须放在函数或代码块中；
- 生成器正常结束时会执行 defer；
- 生成器被直接丢弃时不会执行脚本级 defer；如果仍有待执行 defer，析构/GC 时会打印 warning。

FFI：

> 状态：**Beta**。完整支持范围、所有权契约和已知限制见
> [docs/ffi.md](docs/ffi.md)。

```text
import std;

let lib = std.ffi.open("libm.so.6");
std.ffi.call(lib, "cos", "f64", ["f64"], [0.0])
std.ffi.close(lib)
```

二进制输入和输出：

```text
import std;

let lib = std.ffi.open("libc.so.6");
let data = bytes_from_string("hello");

let buf = std.ffi.buffer(8);
std.ffi.call(lib, "memcpy", "void",
             ["buffer", "bytes", "usize"],
             [buf, data, 5]);

print(bytes_to_string(std.ffi.buffer_bytes(buf, 5)));
std.ffi.buffer_free(buf);
std.ffi.close(lib);
```

FFI 字节参数：

```text
bytes        const void *（脚本 bytes 的临时副本）
buffer       ffi_buffer 的原生内存
usize        size_t
isize        ssize_t / intptr_t 宽度
```

FFI 结构体与指针：

```text
import std;

let Point = std.ffi.struct("Point", [["x", "f64"], ["y", "f64"]]);
let p = std.ffi.new(Point, {"x": 1.0, "y": 2.0});
p.x
p.x = 3.0
std.ffi.to_dict(p)

let raw = std.ffi.alloc(Point);   // 零初始化，返回指针
let view = std.ffi.deref(raw);
view.x = 1.0;

let lib = std.ffi.open("./libpoints.so");
std.ffi.call(lib, "point_len", "f64", [std.ffi.pointer(Point)], [raw]);
std.ffi.call(lib, "point_make", Point, ["f64", "f64"], [1.0, 2.0]);
```

- 结构体按标准 C ABI 自然对齐布局，加载时用 libffi 校验 size/offset/alignment；
- packed 和 bitfield 不由通用 FFI 支持，需要静态 C++ 绑定；
- `ffi.new` 创建由 C++ 管理内存的值，`ffi.alloc` 返回指针，`ffi.deref` 共享同一块内存；
- 字段读写使用 `p.x` / `p.x = v`，类型和布局检查在 C++ 完成；
- `ffi.get` / `ffi.set` 用于数组和指针索引，`ffi.offset` / `ffi.cast` 只做受控的指针运算；
- `ffi.offsetof` 查询字段偏移，`ffi.address_of` 取 struct/array/buffer 的地址并共享 owner；
- 脚本层没有 `free` / `release` / `own`；内存由引用计数和 GC 回收，C 返回的指针默认借用。

FFI 回调：

```text
import std;

let lib = std.ffi.open("./libalgo.so");
let Cmp = std.ffi.signature("i32", ["ptr", "ptr"]);

let cmp = std.ffi.callback(Cmp, func(a, b) {
  let left = std.ffi.deref(std.ffi.cast(a, std.ffi.pointer("i32")));
  let right = std.ffi.deref(std.ffi.cast(b, std.ffi.pointer("i32")));
  if (left < right) { return -1; }
  if (left > right) { return 1; }
  return 0;
});

std.ffi.call(lib, "sort", "void", [Cmp, "ptr", "usize"], [cmp, data, 8]);
```

- 回调必须是同步函数；`async` 函数和生成器在 `ffi.callback` 时直接拒绝；
- 只允许同步、同一 OS 线程、同一 fiber、且在活动的 `ffi.call` 范围内的回调；
- 回调里可以继续调用 `ffi.call`（受递归深度限制），也可以 `spawn` 后立即返回；
- 回调里不能 `await` / `yield`；
- 其他线程调用 `void` 回调时，回调会投递到 event loop，在脚本下次 pump
  （`await` / `std.task.wait` / `sleep`）时执行，C 侧立即返回默认值；
- 跨线程回调必须返回 `void`；需要返回值的回调必须在同步 `ffi.call` 内执行；
- 回调失败不会跨 C ABI 抛异常，错误在 `ffi.call` 返回后统一报告；
- C 侧保存函数指针时，用 `std.ffi.retain(lib, cb)` 把回调挂到库的生命周期，
  之后再释放脚本变量也仍然安全；
- C 调用后可以用 `std.ffi.errno()` / `std.ffi.set_errno(v)` 读写当前线程的 errno；
  建议在同一个函数或表达式中紧随 C 调用读取，顶层语句边界可能被解释器内部 I/O 改变。

需要从其他线程执行 C 调用并等待回调结果时，使用 `std.ffi.call_async`：

```text
import std;

let lib = std.ffi.open("./libalgo.so");
let Cmp = std.ffi.signature("i32", ["i32", "i32"]);
let cmp = std.ffi.callback(Cmp, func(a, b) { return a - b; });

// C 函数在 worker 线程执行；脚本阻塞，但事件循环继续运行，
// 回调会被投递回解释器线程并等待返回值。
std.ffi.call_async(lib, "compare", "i32",
                   [Cmp, "i32", "i32"], [cmp, 9, 4]);
```

- 比同步 `ffi.call` 多了线程切换，适合会阻塞或回调耗时较长的 C 调用；
- 回调仍然运行在解释器线程，可以安全访问脚本状态；
- 回调内可以继续 `ffi.call` / `ffi.call_async`（有递归深度限制）；
- `ffi.call_async` **不可取消，也没有超时**：当前 fiber 会一直等到 C 返回；
  每次调用创建一个 worker 线程，不适合高频短调用；
- 对可能永久阻塞或不受信任的 C 调用，建议放到外部进程，而不是依赖进程内取消。

C 函数指针（例如 C 返回的函数指针或 struct 里的 callback 字段）可以用
`ffi.call_address` 调用：

```text
import std;

let lib = std.ffi.open("./libalgo.so");
let fn = std.ffi.call(lib, "get_compare", "ptr", [], []);
let Cmp = std.ffi.signature("i32", ["i32", "i32"]);

std.ffi.call_address(fn, Cmp, [9, 4]);
std.ffi.call_address(fn, "i32", ["i32", "i32"], [9, 4]);
```

- 3 参数形式使用 `ffi.signature` 的 callback 类型；
- 4 参数形式显式提供返回类型、参数类型列表和参数值列表；
- 空指针会报错，不会尝试调用。

`ffi.symbol(lib, name)` 可以直接解析任意导出符号，配合 `ffi.call_address`
或 `ffi.cast` 使用：

```skein
let sym = std.ffi.symbol(lib, "strlen");
std.ffi.call_address(sym, "usize", ["cstring"], ["hello"]);
```

`ffi.string(pointer)` 把 `char*` 地址按 NUL 结尾字符串读成脚本 string；
`ffi.offsetof` 现在也接受 struct 值/指针；`ffi.address_of` 接受 pointer 并返回
同一地址的共享视图。

变参函数使用 `ffi.call_variadic`：

```text
import std;

let lib = std.ffi.open("./libutil.so");

// 显式形式：返回类型、完整参数类型、完整参数值、固定参数个数
std.ffi.call_variadic(lib, "log_message", "i32",
                      ["cstring", "i32", "f64"],
                      ["value", 7, 2.5], 1);
```

manifest 声明 `"variadic": true` 后，也可以只传固定参数：

```text
std.ffi.call(lib, "log_message", ["value"]);
std.ffi.call_variadic(lib, "log_message", ["value"],
                      ["i32", "f64"], [7, 2.5]);
```

- 变参部分必须显式提供类型；调用者负责 C 默认提升（`float`→`f64`、
  窄整数→`i32`）；
- 普通 `ffi.call` / `lib.fn` 只能传固定参数，变参部分会报错提示使用
  `ffi.call_variadic`。

manifest 函数可以声明调用约定：

```json
"shim_i32": {
  "result": "i32",
  "args": ["i32"],
  "calling_convention": "stdcall"
}
```

- 支持 `"c"`/`"cdecl"`、`"stdcall"`、`"fastcall"`，平台/架构不支持时
  manifest 加载或调用会报错；
- x86_64/ARM64 上 stdcall/fastcall 与默认约定一致，会按默认 ABI 调用；
- 回调目前固定使用默认 ABI，带调用约定的回调类型会被 bindgen 拒绝。

FFI manifest（可选）：

```text
// shapelib.ffi.json
// {
//   "ffi": 1,
//   "types": {"Point": {"kind": "struct", "fields": {"x": "f64", "y": "f64"}}},
//   "functions": {
//     "point_len": {"result": "f64", "args": ["Point"]},
//     "point_new": {"result": "pointer", "args": ["f64", "f64"],
//                   "returns_owned": {"free": "point_free"}}
//   },
//   "variables": {
//     "POINT_COUNT": {"type": "i32"},
//     "POINT_VERSION": {"type": "i32", "const": true}
//   },
//   "constants": {"POINT_MAX": 16}
// }

import std;

let lib = std.ffi.open("./libshape.so", "./shapelib.ffi.json");
let Point = std.ffi.type(lib, "Point");
let p = std.ffi.new(Point, {"x": 1.0, "y": 2.0});
std.ffi.call(lib, "point_len", [p]);
std.ffi.constant(lib, "POINT_MAX");
std.ffi.variable(lib, "POINT_COUNT");      // 读取全局变量
lib.POINT_COUNT;                           // 等价的点语法
lib.POINT_COUNT = 3;                       // 写入全局变量
std.ffi.set_variable(lib, "POINT_COUNT", 4);
```

- manifest 是纯数据，JSON 文件和脚本 dict 共用同一套校验与类型模型；
- 支持 struct / pointer / array / opaque / callback、函数签名、全局变量和常量；
- 全局变量通过 `ffi.variable` / `ffi.set_variable` 或 `lib.VAR` 读写；
  `"const": true` 的变量拒绝写入；
  `--check-ffi` 会同时校验函数、变量和 `returns_owned` 的 free 符号；
- `returns_owned` / `consumes` / `retains` 由 manifest 声明所有权，脚本层不出现 `free`；
  `returns_owned` 的 free 函数必须接收一个指针参数；默认按 `void(void*)` 调用，
  也支持 `{"free": "name", "free_result": "i32"}` 这类有返回值的 free；
  `consumes` 只保证被传递的那个 handle 被标记为已消费；同一块内存的其他
  alias/view 在 C 接管后不应再访问；对借用指针只标记为已被消费；
  `retains` 同时支持 userdata 和 `cstring`/`bytes` 参数存储；
- 加载时校验 ABI、布局、重复类型、递归 by-value 类型和参数索引；
- packed / bitfield 直接拒绝，交给静态 C++ 绑定；
- 显式传入 manifest，不做自动 sidecar 查找。

manifest 绑定函数：

```text
import std;

let lib = std.ffi.open("./libshape.so", "./shapelib.ffi.json");

let point_len = std.ffi.function(lib, "point_len");
point_len(p);

lib.point_len(p);      // 等价的点语法
lib.POINT_MAX;         // manifest 常量

// 也可以再打开库后增量声明
let lib2 = std.ffi.open("./libother.so");
std.ffi.declare(lib2, {"ffi": 1, "functions": {
  "add": {"result": "i32", "args": ["i32", "i32"]}
}});
lib2.add(1, 2);
```

- `std.ffi.function(lib, name)` 返回原生可调用值，`kind_of` 为 `native`；
- `lib.foo(...)` 和 `lib.CONST` 直接读取 manifest 声明的函数和常量；
- `ffi.declare(lib, spec)` 增量合并声明，重复类型/函数/常量会报错；
- 绑定函数持有库句柄；`ffi.close` 后再次调用会报 "ffi library is closed"。

FFI 支持范围（完整契约见 [docs/ffi.md](docs/ffi.md)）：

- 只支持 **C ABI 边界**：C 编译的库，或 C++ 中 `extern "C"` 导出的函数；
- 不支持 C++ ABI：类、模板、异常、RTTI、引用、`std::` 类型、重载；
- 即使合法 C，下列也拒绝：packed/bitfield、union、柔性数组、匿名
  struct/union、`long double`、`_Complex`、`__int128`、平台不支持的调用约定，
  以及 `aligned` / `vector_size` / `mode` 等影响布局的属性；
  变参函数通过 `ffi.call_variadic` 显式传入变参类型；
- `--ffi-gen` 遇到 C++ 头文件或影响布局的属性时直接失败，避免生成错误 manifest。

声明式 FFI 绑定（`extern lib`）：

```text
import std;

extern "c" lib shape from "./libshape.so" {
  type Point {
    x: f64;
    y: f64;
  }

  type PointPtr = *Point;

  callback Compare(a: i32, b: i32) => i32;

  func point_len(p: Point) => f64;
  func point_new(x: f64, y: f64) => Point;
  func sort(base: *Point, n: usize, cmp: Compare);

  const POINT_MAX: i32 = 16;
}

shape.point_len({"x": 1.0, "y": 2.0});
shape.POINT_MAX;
```

- 块内声明会 lowering 到 manifest，运行时走 `ffi.open`，不新增 FFI 语义；
- `type` 支持 struct、别名、指针和定长数组；`callback` 声明回调签名；
- 块内条目用 `;` 结束，返回类型箭头是 `=>`；语言函数的表达式体使用 `->`；
- 所有权注解：
  - `func make() => *Point owns "point_free";`：返回值由 C++ 自动释放；
  - `func destroy(p: *Point) consumes p;`：参数所有权转移给 C；
  - `func watch(cb: Cmp) retains cb;`：C 保留回调，库生命周期内保持存活。

```text
func point_new(x: f64, y: f64) => *Point owns "point_free";
func point_destroy(p: *Point) consumes p;
func point_sort(base: *Point, n: usize, cmp: Compare) retains cmp;
```

原生模块（C ABI）：

```text
// 插件导出唯一符号：
// const skein_module *skein_module_descriptor(void);

import std;

let name = std.module.load("./libexample.so");
print(name);              // example.math

import example.math;
example.math.add(1, 2);
example.math.ANSWER;
```

- 插件只包含 `skein_module.h`，不链接解释器符号；
- `skein_api` 由 host 通过 `init` 传入，值以 opaque handle 传递；
- `skein_api` v1 追加了 `new_list` / `new_dict` / `as_userdata`；使用新字段前
  应检查 `size`，旧 host 的 ABI 表会更短；
- 静态绑定在启动时注册，动态插件通过 `std.module.load` 加载；
- `std.module.unload(name)` 只移除注册表；已存在的绑定函数/userdata 会继续持有
  模块引用，最后一份引用释放时才调用 `fini` 并 `dlclose`；静态模块不能卸载；
- 模块名是建议 import path，内部 key 由 loader 生成；
- 重复 import path、模块与包同名、ABI 版本不匹配都会报错；
- packed/bitfield 不进通用 FFI，由 C++ 绑定通过 `get_field`/`set_field` 处理，
  `example.math` 里的 `PackedHeader` / `PackedFlags` 是示例。

从 C 头文件生成 manifest：

```text
skein --ffi-gen \
  --header include/shape.h \
  --ffi-include include \
  --ffi-define SHAPE_API= \
  --symbols "shape_*" \
  --module shape \
  --cc gcc \
  -o shape.ffi.json
```

- 使用系统 C 编译器（gcc/clang，Windows 为 MinGW gcc）的 `-E` 做预处理，
  不引入 libclang；
- 支持 struct、typedef、指针、数组、函数声明、函数指针 typedef、enum、
  `extern` 全局变量和对象式宏常量；`static` 函数/变量视为内部符号并跳过；
  struct 里的函数指针字段会生成 inline callback 类型，可用 `ffi.call_address` 调用；
- 支持无条件 `extern "C" { ... }` 块；`const` 全局变量标记为只读；
  `_Thread_local` / `__thread` 变量直接拒绝；
- `__stdcall` / `WINAPI` / `CALLBACK` / `APIENTRY` 生成
  `"calling_convention": "stdcall"`，`__fastcall` 生成 `"fastcall"`；
  `__vectorcall` 和带调用约定的回调类型会被拒绝；
- 只导出 `--header` 指定头文件里的符号，但会解析被包含头文件里的
  typedef/struct/enum，用于解析跨头文件类型；依赖头里的不支持构造只记 warning；
  `--no-include-types` 可以关闭依赖头解析；
- 常量支持整数表达式（算术、位运算、比较、逻辑、三元、标量强制转换、
  二进制字面量）以及浮点字面量（含 `f` 后缀）；
- 通过 `cc -dM` 探测目标 ABI 的 `long`/指针/`wchar_t` 宽度，支持 `--cc`、
  `-m32` 和 sysroot，而不是假设生成机就是目标机；
- `char*` 映射为 `cstring`，`unsigned char*` / `uint8_t*` 映射为 `bytes`；
  函数参数里的定长数组会按 C 规则退化为指针；
- packed、bitfield、柔性数组、匿名 struct/union、影响布局的属性、
  未知类型默认直接失败；`--tolerate-errors` 可显式降级为警告并继续；
- `--allow-opaque-types` 会把无法解析的 struct/union/typedef 恢复为 opaque
  类型，继续生成使用其指针的函数，并跳过按值使用该类型的函数/变量；
  这是 best-effort，默认仍然 fail-closed；
- `--symbols` 过滤函数、变量和常量，并裁剪未被引用到的类型（`*`/`?` 通配符）；
- `--check` 用于 CI 校验生成的 manifest 是否过期；
- 运行时不依赖 C 编译器和头文件，只读 manifest。

校验 manifest 与库是否匹配（不执行脚本）：

```text
skein --check-ffi ./libshape.so shape.ffi.json
```

- 加载库和 manifest，校验 ABI/布局，并解析所有声明的符号；
- 缺失符号会逐条列出并以非零状态退出；
- 适合作为构建/发布前的检查。

模块：

```text
std.list
std.fn
std.iter
std.dict
std.json
std.string
std.bytes
std.encoding
std.unicode
std.regex
std.time
std.random
std.env
std.crypto
std.compress
std.math
std.io
std.fs
std.process
std.worker
std.thread
std.type
std.meta
std.module
std.net
std.http
std.tls（可选）
std.task
std.generator
std.result
std.error
std.enum
std.struct
std.lru
std.ffi（可选）
std.fsm（内嵌 Skein 源码包）
std.set（原生实现）
std.deque（原生实现）
std.heap（原生实现）
std.route（内嵌 Skein 源码包）
std.graph（内嵌 Skein 源码包）
std.arena（内嵌 Skein 源码包）
std.path（内嵌 Skein 源码包）
```

`std.fsm` 是内嵌 Skein 源码标准库，源码位于 `lib/std/fsm.skein`，构建时嵌入解释器，不需要安装额外文件：

```text
import std.fsm;

let m = fsm.define(0, func(state, event) { return state + event; });
let ctx = fsm.start(m);
ctx = fsm.run(m, ctx, [1, 2, 3]);
print(ctx.state);          // 6
print(fsm.history(ctx));   // [0, 1, 3, 6]
ctx = fsm.undo(ctx);       // state 3
ctx = fsm.redo(ctx);       // state 6
```

API：`define` / `initial` / `start` / `step` / `run` / `history` / `undo` / `redo` / `snapshot` / `restore`。

`define(initial, step[, on_transition])` 支持可选的转换回调；enum / struct 状态会校验 `step` 返回值仍是同一类型。单步 `step` 每次 O(history)；批量 `run` 在单次调用内完成，整体 O(events)。

`snapshot` 返回普通 dict，可 JSON 持久化；`restore` 重建历史并校验。枚举状态在 JSON 往返时通过 `std.enum.decode` 自动恢复：

```text
let json = std.json.stringify(fsm.snapshot(ctx));
ctx = fsm.restore(std.json.parse(json));
```

struct 状态可以传解码回调：

```text
ctx = fsm.restore(std.json.parse(json), func(v) {
  return std.struct.decode(v, "S");
});
```

`std.struct.decode(value, type_name[, options])` 是 schema 驱动解码：JSON 保持普通 object，类型由调用方提供。

```text
import std;
struct Point { x: int; y: int; }
let p = std.struct.decode({"x": 1, "y": 2}, "Point");
```

`options` 支持 `{"defaults": true}`：缺失且有默认值的字段在声明包作用域中求值。
`std.enum.decode(value[, type_name][, options])` 同样支持默认值；可选类型名用于
校验 tag 或消除跨包同名歧义。

`std.enum.tag(value)` 返回 `module.Enum.Variant` 规范字符串，丢弃载荷，可直接作 dict key：

```text
enum E { A { n: int; }; }
std.enum.tag(E.A { n: 1 })   // "main.E.A"
std.enum.tag(E.A { n: 2 })   // "main.E.A"
```

无实例时可用 `std.enum.tag("E", "A")`。

`std.meta` 提供只读类型描述，内建类型和用户定义类型使用同一种形状：

```text
import std;

struct Point { x: int; y: int = 0; }
let info = std.meta.type_info("Point");
print(info["kind"]);                  // struct
print(info["fields"][0]["name"]);     // x
print(std.meta.fields(Point { x: 1 }));
print(std.meta.has_type("int"));      // true
```

`std.meta.type_info(type)` 接受 struct/enum 实例、内建类型值或类型名字符串；
字符串参数始终按类型名解释，查询 string 类型请用 `"string"`。
`std.meta` 只查询已加载且可见的类型：当前包/当前源文件的类型可见，其他包
的类型必须 `export`；不可见类型报错，`has_type` 返回 `false`。
`fields` / `variants` / `methods` / `traits` 对不适用的类型返回空列表。
`std.meta` 不返回值默认值，只提供 `has_default` 标记。

`std.lru` 是 C++ 值类型，每次更新返回新 lru：

```text
import std;

let l = std.lru.new(2);
let r = std.lru.put(l, "a", 1);
l = r["lru"];
r = std.lru.put(l, "b", 2);
l = r["lru"];
r = std.lru.get(l, "a");       // a 变成最近使用
l = r["lru"];
r = std.lru.put(l, "c", 3);    // 淘汰 b
r["evicted"]                   // {key: b, value: 2}
```

API：`new` / `get` / `peek` / `put` / `delete` / `has` / `keys` / `values` / `len` / `capacity` / `clear`。key 必须是合法 dict key；`keys` / `values` 从最久未使用到最近使用排列。

`std.set` / `std.deque` / `std.heap` 是原生实现（`import std.set` / `import std.deque` / `import std.heap` 仍然可用，也可以直接使用对应的 `std.*`）：

```text
import std.set;
let s = set.from_list([1, 2, 2, 3]);
set.size(s)          // 3
set.has(s, 2)        // true
set.values(s)        // [1, 2, 3]

import std.deque;
let d = deque.from_list([2, 3]);
d = deque.push_front(d, 1);
deque.to_list(d)     // [1, 2, 3]

import std.heap;
let h = heap.from_list([3, 1, 2], func(a, b) { return a < b; });
heap.peek(h)         // 1
```

`set` 元素必须是合法 dict key（`int/bool/string/char/byte/bytes/null`）。`set` / `deque` / `heap` 都是原生值类型：`set` 的 add/remove/has 为 O(log n)、`from_list` 为 O(n log n)，`values` 保持插入顺序；`deque` 的 push/pop/size/peek 为均摊 O(1)、`to_list` 为 O(n)；`heap` 的 push/pop 为 O(log n)、`from_list` 为 O(n log n)。`deque.pop_*` 和 `heap.pop` 返回 `{ok, ..., value}`。

`std.route` 是显式注册的 Web 路由：

```text
import std.route;

let app = route.new();
app.get("/users/:id", func(req) {
  return {"status": 200, "body": req["params"]["id"]};
});
app.use(func(req) {
  if (req["path"] == "/private") {
    return {"status": 401, "body": "no"};
  }
  return null;
});

app.dispatch({"method": "GET", "path": "/users/42"});
```

`Router` 是可变引用句柄，复制后共享路由表；注册方法原地更新，不返回新 Router。支持 `:name` 参数、`*name` 通配、404/405、全局 middleware 和 async middleware。静态路由走精确表，动态路由走段级 trie：静态 dispatch 为 O(1)，动态 dispatch 与路由总数基本无关。`*name` 必须是最后一段，并且可以匹配 0 段。装饰器可用于包装 handler，例如 `@logged func user(req) {...}` 后再 `app.get("/users/:id", user)`。

`std.graph` 提供有向图和常用算法：

```text
import std.graph;

let g = graph.new();
let r = graph.add_node(g, "a");
g = r["graph"];
let a = r["id"];

r = graph.add_node(g, "b");
g = r["graph"];
let b = r["id"];

r = graph.add_edge(g, a, b, "next", 1);
g = r["graph"];

graph.bfs(g, a)               // [1, 2]
graph.topo_sort(g)            // {ok: true, order: [1, 2]}
graph.has_cycle(g)            // false
graph.path(g, a, b)           // {ok: true, path: [1, 2]}
graph.shortest_path(g, a, b)  // {ok: true, path: [1, 2], cost: 1}
graph.components(g)           // [[1, 2]]
graph.reverse(g)              // 反向图
graph.subgraph(g, [a])        // 指定节点子图
graph.to_dot(g)               // Graphviz 文本
```

API：`new` / `add_node` / `add_nodes` / `remove_node` / `has_node` / `get_node` / `set_node` / `node_count` / `edge_count` / `node_ids` / `nodes` / `add_edge` / `add_edges` / `from_edges` / `remove_edge` / `edges` / `neighbors` / `bfs` / `dfs` / `topo_sort` / `has_cycle` / `path` / `shortest_path` / `components` / `reverse` / `subgraph` / `to_dot`。

`add_nodes(g, values)` 返回 `{graph, ids}`；`add_edges(g, edges)` 批量添加 `[from, to[, tag[, weight]]]`；`from_edges(count, edges)` 直接构造 1..count 节点的图。批量入口只触发一次 COW，O(V+E)。

`add_node` / `add_edge` 返回 `{graph, id}`；`add_edge(g, from, to, tag = "", weight = 1)`，`weight` 必须非负；删除节点会级联删除相关边；边标签字段是 `tag`。`shortest_path(g, from, to, weight_fn = null)` 默认用边权重，也可传 `func(edge) -> number`。

性能提示：`add_node` / `add_edge` 每次为值语义复制对应 dict（O(V) / O(E)/次），大图请用 `add_nodes` / `add_edges` / `from_edges` 批量构造；`bfs` / `dfs` / `topo_sort` / `path` / `components` / `shortest_path` 会先构建一次邻接表，遍历为 O(V+E)，单次 `neighbors` 仍扫描全部边 O(E)。

`std.arena` 提供带 generation 的稳定句柄：

```text
import std.arena;

let s = arena.new();
let r = arena.add(s, "a");
s = r["arena"];
let a = r["id"];

arena.get(s, a)        // {ok: true, value: a}
arena.set(s, a, "A")   // {arena, ok: true}
arena.remove(s, a)     // {arena, removed: true}
arena.has(s, a)        // false
```

API：`new` / `from_list` / `add` / `get` / `set` / `remove` / `has` / `size` / `capacity` / `ids` / `values` / `clear`。槽位复用时 generation 递增，旧句柄永久失效。`from_list` 批量构造 O(n)；逐条 `add` / `set` / `remove` 每次 O(capacity)，适合中小规模。

`std.path` 是纯词法的跨平台路径库，不访问文件系统；`style` 可以显式指定
`"posix"` 或 `"windows"`，因此可以在 Linux 上完整测试 Windows 路径：

```text
import std.path;

let p = path.parse("C:/Users/alice/../notes.txt", "windows");
p["style"]       // "windows"
p["root"]        // "C:\\"
p["drive"]       // "C:"
p["base"]        // "notes.txt"
p["ext"]         // ".txt"
p["parts"]       // ["Users", "alice", "..", "notes.txt"]
p["absolute"]    // true
p["normalized"]  // "C:\\Users\\notes.txt"

path.normalize("/a/./b/../c")                    // "/a/c"
path.join("C:/a", "b", {"style": "windows"})     // "C:\\a\\b"
path.relative("/a/b", "/a/c/d")                  // "../c/d"
path.with_ext("a/b.txt", "md")                   // "a/b.md"
path.to_posix("C:/a/b")                          // "C:/a/b"
```

API：

```text
path.host()                                  -> "posix" | "windows"
path.detect(path)                            -> "posix" | "windows"
path.sep(style = "auto")                     -> "/" | "\\"
path.delimiter(style = "auto")               -> ":" | ";"
path.parse(path, style = "auto")             -> dict
path.join(parts..., options?)                -> string
path.join_list(parts, style = "auto")        -> string
path.normalize(path, style = "auto")         -> string
path.is_absolute(path, style = "auto")       -> bool
path.is_relative(path, style = "auto")       -> bool
path.absolute(path, style = "auto", cwd = null) -> string
path.resolve(base, path, style = "auto")     -> string
path.relative(from, to, style = "auto")      -> string
path.basename(path, style = "auto")          -> string
path.dirname(path, style = "auto")           -> string
path.extname(path, style = "auto")           -> string
path.stem(path, style = "auto")              -> string
path.with_ext(path, ext, style = "auto")     -> string
path.equals(a, b, style = "auto")            -> bool
path.to_posix(path, style = "auto")          -> string
path.to_windows(path, style = "auto")        -> string
```

`parse` 返回：

```text
style / input / root / drive / dir / base / stem / ext
parts / absolute / rooted / normalized / sep
```

`parts` 保留原始 `.` 和 `..`；`normalized` 是规范化后的结果。`rooted` 表示路径
带可钳制 `..` 的根（如 `/`、`C:\\`、`\\server\share\`、`\\?\C:\\`），
drive-relative 的 `C:foo` 和普通相对路径为 `false`。

规则：

- `style = "auto"` 时，出现盘符、`\\` 前缀或反斜杠按 Windows 处理，否则使用宿主风格；
- POSIX 只把 `/` 当分隔符，反斜杠是普通文件名字符；
- Windows 输入同时接受 `/` 和 `\`，输出统一为 `\`；支持盘符、UNC、`\\?\`、`\\?\UNC\`、
  drive-relative（`C:foo`）和 rooted（`\foo`）；
- `..` 在根处钳制；drive-relative 路径可以保留开头的 `..`；
- `join` 遇到带根的后续片段会重置累积结果；`join` 的最后一个 dict 参数可传
  `{"style": "windows"}`；
- `absolute` 在宿主风格下读取 `std.fs.cwd()`；非宿主风格必须显式传 `cwd`；
- `resolve` 要求 `base` 是绝对路径；跨盘符的 drive-relative 路径会报错；
- `relative` 在根不同（不同盘符 / UNC / verbatim 前缀）时返回规范化后的目标绝对路径；
- `\\?\` 只作为词法前缀处理，不会调用 Win32 长路径或 `GetFullPathName` 规范化；
- `equals` 的 Windows 比较只做 ASCII 大小写不敏感，不做 Unicode 大小写折叠；
- `std.path` 只做路径字符串运算；真实文件 I/O 仍由 `std.fs` 完成，现有
  `std.fs.path_*` 是宿主原生接口，行为随平台变化。

`std.list` 新增常用算法：

```text
std.list.slice(xs, start, length)         // length 省略时到末尾
std.list.unique([1, 2, 1, 3, 2])        // [1, 2, 3]
std.list.unique_by(xs, key)             // 按 key 保序去重
std.list.group_by(xs, key)              // {key: [items]}
std.list.chunk([1, 2, 3, 4, 5], 2)      // [[1, 2], [3, 4], [5]]
std.list.window([1, 2, 3, 4], 2)        // [[1, 2], [2, 3], [3, 4]]
std.list.zip([1, 2], ["a", "b"])        // [[1, a], [2, b]]
std.list.top_k([5, 1, 4, 2, 3], 3)      // [1, 2, 3]
std.list.binary_search([1, 3, 5], 4)    // {found: false, index: 1}
```

`zip` 以最短输入为准；`binary_search` 要求升序，未找到时 `index` 是插入点；`group_by` 的 key 必须是合法 dict key。`unique` / `unique_by` 的 key 可哈希时用哈希去重，O(n)；否则回退到 O(n²) 比较。`top_k` 使用 partial sort，O(n log k)。

`std.list` / `std.dict` / `std.bytes` / `std.lru` 由内部适配层提供，原生入口保留在 `std.raw.list` / `std.raw.dict` / `std.raw.bytes` / `std.raw.lru`；当前使用语言级默认参数的有 `list.slice`、`list.sort`、`list.sort_by`、`list.is_sorted`、`list.top_k`、`list.binary_search`、`list.reduce`、`dict.get`、`lru.get`、`lru.peek` 和 `bytes.slice`，其余函数仍直接使用原生 builtin。`import std.list` 和 `import std; std.list` 现在指向同一个命名空间。

`std.fn` 提供函数组合与部分应用：

```text
import std;

let inc = func(x) -> x + 1;
let twice = func(x) -> x * 2;

std.fn.compose(inc, twice)(5)       // 11，先 twice 再 inc
std.fn.pipe(inc, twice)(5)          // 12，先 inc 再 twice
std.fn.partial(func(a, b) -> a + b, 10)(5)   // 15
std.fn.identity(7)                  // 7
std.fn.always("ok")()               // "ok"
```

`compose` / `pipe` 接受至少两个可调用值；`partial` 固定前若干个参数；`always` 返回一个忽略参数、总是返回给定值的函数。

`std.iter` 提供惰性迭代器，输入可以是 list/dict/string/bytes/deque/heap/set 或生成器：

```text
import std;

std.iter.collect(
  std.iter.map(
    std.iter.filter(range(0, 10), func(x) -> x % 2 == 0),
    func(x) -> x * x))              // [0, 4, 16, 36, 64]

std.iter.collect(std.iter.take(range(0, 100), 3))  // [0, 1, 2]
std.iter.collect(std.iter.drop([1, 2, 3, 4], 2))   // [3, 4]
std.iter.collect(std.iter.enumerate(["a", "b"]))   // [[0, a], [1, b]]

std.iter.for_each([10, 20], func(i, x) { println(str(i) + ":" + str(x)); })
std.iter.for_each({"a": 1}, func(k, v) { println(k + "=" + str(v)); })

func gen() { yield 1; yield 2; yield 3; }
std.iter.collect(std.iter.map(gen(), func(x) -> x * 10))
                                                     // [10, 20, 30]
```

`map` / `filter` / `take` / `drop` / `enumerate` 返回惰性生成器，可以继续用 `std.generator.*` 操作；`collect` 会立即消费输入并返回 list。`take` 提前结束时会在底层是生成器时调用 `close`。

当前提供的函数：

```text
std.list.len
std.list.slice
std.list.contains
std.list.index
std.list.reverse
std.list.sort
std.list.sort_by
std.list.sort_desc
std.list.is_sorted
std.list.append
std.list.map
std.list.filter
std.list.reduce
std.list.unique
std.list.unique_by
std.list.group_by
std.list.chunk
std.list.window
std.list.zip
std.list.top_k
std.list.binary_search

std.fn.compose
std.fn.pipe
std.fn.partial
std.fn.identity
std.fn.always

std.iter.map
std.iter.filter
std.iter.take
std.iter.drop
std.iter.enumerate
std.iter.collect
std.iter.for_each

std.dict.len
std.dict.get
std.dict.has
std.dict.keys
std.dict.values
std.dict.items
std.dict.delete
std.dict.merge

std.enum.decode
std.enum.tag

std.struct.decode

std.lru.new
std.lru.get
std.lru.peek
std.lru.put
std.lru.delete
std.lru.has
std.lru.keys
std.lru.values
std.lru.len
std.lru.capacity
std.lru.clear

std.deque.new
std.deque.from_list
std.deque.size
std.deque.push_front
std.deque.push_back
std.deque.peek_front
std.deque.peek_back
std.deque.pop_front
std.deque.pop_back
std.deque.to_list

std.heap.new
std.heap.from_list
std.heap.size
std.heap.peek
std.heap.push
std.heap.pop
std.heap.to_list

std.set.new
std.set.from_list
std.set.add
std.set.remove
std.set.has
std.set.size
std.set.values
std.set.union
std.set.intersect
std.set.difference
std.set.is_subset

std.json.parse
std.json.try_parse
std.json.stringify

std.string.len
std.string.find
std.string.substr
std.string.split
std.string.split_ws
std.string.trim
std.string.trim_start
std.string.trim_end
std.string.join
std.string.replace
std.string.starts_with
std.string.ends_with
std.string.contains
std.string.repeat
std.string.format

std.math.sqrt
std.math.exp
std.math.log
std.math.abs
std.math.sin
std.math.cos
std.math.tan
std.math.asin
std.math.acos
std.math.atan
std.math.atan2
std.math.pow
std.math.floor
std.math.ceil
std.math.round
std.math.trunc
std.math.log2
std.math.log10
std.math.hypot
std.math.min
std.math.max
std.math.sign
std.math.clamp
std.math.fmod
std.math.mod
std.math.is_nan
std.math.is_inf
std.math.is_finite
std.math.cbrt
std.math.gcd
std.math.lcm
std.math.degrees
std.math.radians

std.io.print
std.io.println
std.io.format
std.io.input

std.io.read
std.io.write
std.io.write_all
std.io.flush
std.io.close
std.io.eof
std.io.readable
std.io.writable
std.io.seekable
std.io.seek
std.io.tell
std.io.read_exact
std.io.read_all
std.io.read_line
std.io.copy
std.io.buffered
std.io.read_async
std.io.write_async

std.io.stdin
std.io.stdout
std.io.stderr

std.fs.read_file
std.fs.write_file
std.fs.append_file
std.fs.read_text
std.fs.write_text
std.fs.exists
std.fs.open
std.fs.read
std.fs.write
std.fs.seek
std.fs.tell
std.fs.eof
std.fs.close
std.fs.flush
std.fs.read_bytes
std.fs.write_bytes
std.fs.append_bytes
std.fs.file_read_bytes
std.fs.file_write_bytes

std.process.command
std.process.status
std.process.command_status

std.type.kind_of
std.type.name
std.type.module
std.type.source

std.meta.type_info
std.meta.fields
std.meta.variants
std.meta.methods
std.meta.traits
std.meta.implements
std.meta.has_type

std.module.load
std.module.unload

std.net.tcp_connect
std.net.tcp_listen
std.net.accept
std.net.send
std.net.recv
std.net.close
std.net.shutdown
std.net.resolve
std.net.local_addr
std.net.peer_addr
std.net.tcp_connect_async
std.net.tcp_listen_async
std.net.accept_async
std.net.send_async
std.net.recv_async
std.net.resolve_async
std.net.shutdown_async

std.http.request
std.http.get
std.http.post
std.http.request_async
std.http.get_async
std.http.post_async
std.http.listen
std.http.listen_tls
std.http.accept
std.http.accept_async
std.http.respond
std.http.respond_async
std.http.serve
std.http.serve_workers
std.http.serve_threads
std.http.upgrade
std.http.cookies
std.http.read_body
std.http.close
std.http.thread_capabilities

std.http2.request
std.http2.get
std.http2.post
std.http2.request_async
std.http2.get_async
std.http2.post_async
std.http2.request_many
std.http2.request_many_async
std.http2.listen
std.http2.listen_tls
std.http2.serve
std.http2.close

std.tls.client
std.tls.server
std.tls.alpn

std.websocket.connect
std.websocket.connect_async
std.websocket.accept
std.websocket.send
std.websocket.send_async
std.websocket.receive
std.websocket.receive_async
std.websocket.ping
std.websocket.close

std.task.sleep
std.task.yield
std.task.wait
std.task.wait_all
std.task.cancel
std.task.done
std.task.catch
std.task.try_wait
std.task.try_wait_all
std.task.retry
std.task.timeout
std.task.rate_limit
std.task.acquire
std.task.bounded_parallel_map

std.generator.next
std.generator.done
std.generator.close

std.result.ok
std.result.err
std.result.is_ok
std.result.is_err
std.result.unwrap
std.result.unwrap_or
std.result.value
std.result.error
std.result.capture

std.error.new
std.error.kind
std.error.message

std.ffi.open
std.ffi.close
std.ffi.call
std.ffi.call_async
std.ffi.call_address
std.ffi.call_variadic
std.ffi.buffer
std.ffi.buffer_len
std.ffi.buffer_bytes
std.ffi.buffer_free
std.ffi.struct
std.ffi.pointer
std.ffi.array
std.ffi.signature
std.ffi.new
std.ffi.alloc
std.ffi.ptr
std.ffi.deref
std.ffi.offset
std.ffi.cast
std.ffi.get
std.ffi.set
std.ffi.to_dict
std.ffi.sizeof
std.ffi.alignof
std.ffi.offsetof
std.ffi.address_of
std.ffi.callback
std.ffi.manifest
std.ffi.constant
std.ffi.symbol
std.ffi.string
std.ffi.variable
std.ffi.set_variable
std.ffi.retain
std.ffi.errno
std.ffi.set_errno
std.ffi.function
std.ffi.declare
std.ffi.type

std.bytes.len
std.bytes.get
std.bytes.slice
std.bytes.concat
std.bytes.hex
std.bytes.from_hex
std.bytes.to_int
std.bytes.from_int
std.bytes.to_string
std.bytes.from_string

std.encoding.decode
std.encoding.encode
std.encoding.supports

std.unicode.normalize
std.unicode.is_normalized
std.unicode.version

std.encoding.base64_encode
std.encoding.base64_decode
std.encoding.url_encode
std.encoding.url_decode

std.regex.match
std.regex.search
std.regex.find_all
std.regex.replace
std.regex.split

std.time.now
std.time.now_ms
std.time.monotonic_ms
std.time.utc_offset
std.time.format
std.time.parse
std.time.sleep

std.random.seed
std.random.int
std.random.float
std.random.bytes
std.random.choice
std.random.shuffle
std.random.uuid

std.env.get
std.env.has
std.env.set
std.env.unset
std.env.all

std.crypto.sha256
std.crypto.hmac_sha256

std.compress.gzip
std.compress.gunzip
std.compress.zlib
std.compress.unzlib
std.compress.deflate
std.compress.inflate

std.fs.listdir
std.fs.mkdir
std.fs.remove
std.fs.rename
std.fs.stat
std.fs.cwd
std.fs.chdir
std.fs.path_join
std.fs.path_basename
std.fs.path_dirname
std.fs.path_extname
std.fs.path_abs
std.fs.path_normalize

std.process.run
std.process.spawn
std.process.wait
std.process.kill
std.process.close
std.process.pid

std.worker.pool
std.worker.call
std.worker.call_bytes
std.worker.map
std.worker.submit
std.worker.submit_bytes
std.worker.wait
std.worker.shutdown
std.worker.stats

std.thread.unbounded_channel
std.thread.pool
std.thread.spawn
std.thread.select
std.thread.try_broadcast
std.thread.broadcast
std.thread.capabilities
```

规则：

- `std` 是内置包，不参与文件系统查找，也不需要满足目录名规则；
- `std` 名称保留，不能通过源码包覆盖；
- 使用 `import std;` 后通过 `std.list`、`std.string` 等方式访问；
- 原有全局函数（`find`、`substr`、`split`、`trim`、`bytes_*` 等）保持可用；
- `std` 导入得到的是命名空间副本，修改自己的副本不会污染内置缓存；
- 新函数只通过 `std.*` 提供，不再增加全局函数名；
- `std.list.slice` 的 `start` / `length` 不能为负数；
- `std.list.index` 找不到时返回 `-1`，`std.list.contains` 返回 `1` / `0`；
- `std.dict.get` 缺少键时返回 `null` 或提供的默认值；
- `std.list.reverse` / `std.list.sort` / `std.list.sort_by` / `std.list.append` 返回新 list，不修改参数；
- 排序稳定；`std.list.sort(xs)` 接受全数字、全字符串或全 `char`；`std.list.sort(xs, cmp)` 接受自定义比较器；
- 默认排序和 `sort_by` 中，`NaN` 永远排在最后；`char` / `byte` 按码点 / 数值排序；
- `std.list.sort_by(xs, key[, {"desc": true}])` 的 key 每个元素只调用一次；
- `std.list.is_sorted(xs[, cmp])` 返回 bool；
- `std.list.map` / `std.list.filter` / `std.list.reduce` 接收函数值，回调出错时中止；
- `std.list.reduce` 回调参数顺序是 `(accumulator, element)`；没有初始值时不能用于空 list；
- `std.dict.items` 返回 `[[key, value], ...]`；
- `std.dict.delete` 删除缺失键时返回原 dict 的副本；单次删除 O(n)（重建），批量删除请尽量一次构造；
- `std.dict.merge` 中右侧 dict 覆盖同名键；`dict` 的 `==` 使用哈希索引逐键比较，O(n)；
- JSON 编码中 `deque` / `heap` / `set` 按数组输出；
- 循环拼接字符串是 O(n²)，建议用 list/deque 收集片段后 `std.string.join`；
- `std.json.parse` 把 JSON object 映射为 `dict`，key 一律是 `string`；参数可以是 `string` 或 `bytes`；
- `std.json.try_parse` 返回 `result`，非法 JSON 不会直接抛错；
- `std.json.stringify` 只接受 JSON 子集，dict key 必须是 `string`；
- JSON 整数在 int64 范围内映射为 `int`，否则映射为 `float`；重复 key 以最后一个为准；
- `std.json.stringify` 拒绝 `bytes`、`function`、`userdata`、`result`、`error` 和 `NaN` / `Inf`；
- `struct` 编码为普通 object，`enum` 编码为 `{"$enum":"Type.Variant","payload":{...}}`，`char` 编码为单字符字符串，`byte` 编码为整数；
- `std.string.join` 只接受字符串元素；
- `std.string.replace` 替换全部匹配，空查找串会报错；
- `std.string.contains` 返回 `1` / `0`；
- `std.string.repeat` 的次数不能为负数；
- `std.string.trim_start` / `trim_end` 只按 ASCII 空白裁剪；
- `std.math` 与全局数学函数使用同一套实现；
- `std.io` 提供 `print`、`println`、`format`、`input`，以及文件和 socket 通用的流式读写；`print` 不追加换行，`println` 输出后自动追加换行符；
- `std.fs` 提供文件读写和文件句柄操作；
- `std.process` 提供命令执行和状态查询；
- `std.type` 提供 `kind_of`、`type_name`、`type_module`、`type_source`；
- `std.meta` 提供只读类型反射，查询字段、变体、方法和 trait；
- `std.net` 提供同步和 task 形式异步的 TCP/DNS，数据使用 `bytes`，底层由 libuv 驱动；
- `std.net` 的阻塞操作支持可选 `timeout_ms`，超时是运行时错误；
- `std.net` 的 `local_addr` / `peer_addr` 返回 `address`、`port`、`family` 三项 dict；
- `std.net.*_async` 返回 task，底层复用同一事件循环；
- `std.http` 提供基于 llhttp 的 HTTP/1.1 客户端和服务端；客户端支持 sync 和 async；
- `std.tls` 提供基于 OpenSSL 的同步 TLS 流包装（可选）；
- 异步网络操作的 `timeout_ms` 是操作自身超时，`std.task.wait` 的超时是等待超时；
- `std.task` 提供原生 task 句柄、等待、批量等待、取消和完成状态；
- task 是协作式调度的，只有在 `wait` / `wait_all`、`await` 或同步网络等待运行事件循环时才推进；
- `std.task.wait` / `wait_all` 超时不会取消 task，剩余 task 需要显式 `cancel`；
- async 函数可以挂起和恢复；普通函数仍不能通过 `spawn` 作为独立 task 启动；
- `yield` 用于生成器；生成器通过 `std.generator.next` 拉取值，`std.generator.close` 关闭并清理；
- `std.result` 提供独立 result 值和错误捕获；
- `std.error` 提供独立 error 值和 `kind` / `message` 访问；
- `defer` 是块级清理语句，按 LIFO 执行；
- 目前不支持 async 生成器；
- Ctrl-C 或超时中断时，`accept` / `recv` 保留 socket，`connect` / `send` 关闭当前 socket；
- `std.ffi` 使用 libffi 调用 C ABI 函数，支持整数、浮点数、`cstring`、`bytes` 输入和 `buffer` 输出；
- `std.ffi` 的 `u64` / `usize` 返回值必须在脚本 `int` 范围内，否则报错；
- `bytes` 传给 C 的是调用期间的临时副本，C 侧不能保存指针；
- `buffer` 的容量和写入边界由调用方保证，C 侧越界写入会破坏内存；
- `std.ffi` 是可选模块，缺少 libffi 时不可用；
- 网络 I/O 和 FFI 错误是运行时错误，可以通过 `on error` 或 `std.result.capture` 捕获；
- FFI 可以执行任意本地代码，默认不建议对不受信任脚本开放；
- `std.net` 暂不支持 TLS、HTTP；
- async/await 和生成器使用 Boost.Context 提供的有栈协程；
- `std.result.capture` 捕获同步 `fail` 和运行时错误；async 使用 `std.task.catch`；
- 显式 `std.generator.close(g)` 会执行生成器的 `defer`；被直接丢弃的挂起生成器不会执行，但会打印 warning；
- DNS 超时依赖 libuv 取消系统解析请求，极端情况下可能延迟返回；
- 全局函数目前仍作为兼容别名保留，新代码建议使用 `std.*`。

### 流式读写

文件句柄和 TCP socket 共用 `std.io` 流接口：

```text
import std;

let f = std.fs.open("data.bin", "rb");
defer std.io.close(f);

std.io.readable(f)        // true
std.io.writable(f)        // false
std.io.seekable(f)        // true

let first = std.io.read(f, 4096);     // bytes，EOF 时是 null
let rest = std.io.read_all(f, 16 * 1024 * 1024);
std.io.eof(f)

std.io.seek(f, 0, 0)
std.io.tell(f)

let buffered = std.io.buffered(f, 8192);
bytes_to_string(std.io.read(buffered, 4096))
std.io.close(buffered)
```

socket 异步流：

```text
let s = std.net.tcp_connect("127.0.0.1", 8080);

await std.io.write_async(s, "hello", 1000);

let data = await std.io.read_async(s, 4096, 1000);
if (data == null) {
  print("closed");
} else {
  print(bytes_to_string(data));
}

std.io.close(s);
```

文件异步流：

```text
let f = std.fs.open("data.bin", "rb");

let first = await std.io.read_async(f, 4096);
let rest = await std.io.read_async(f, 4096);

std.io.close(f);
```

```text
let f = std.fs.open("out.bin", "w+b");

let n = await std.io.write_async(f, bytes_from_string("hello"));
std.io.close(f);
```

```text
std.io.read(stream, max_bytes[, timeout_ms])  → bytes / null
std.io.write(stream, data[, timeout_ms])      → int
std.io.write_all(stream, data[, timeout_ms])  → int
std.io.flush(stream)
std.io.close(stream)                          → bool
std.io.eof(stream)                            → bool
std.io.readable(stream)                       → bool
std.io.writable(stream)                       → bool
std.io.seekable(stream)                       → bool
std.io.seek(stream, offset[, whence])         → int
std.io.tell(stream)                           → int
std.io.read_exact(stream, n[, timeout_ms])    → bytes
std.io.read_all(stream[, limit])              → bytes
std.io.read_line(stream[, limit])             → bytes
std.io.copy(dst, src[, limit])                → int
std.io.buffered(stream[, size])               → stream
std.io.read_async(stream, max[, timeout_ms])  → task
std.io.write_async(stream, data[, timeout_ms]) → task
```

规则：

- `read` 最多读取 `max_bytes`；
- EOF 返回 `null`，不是空 `bytes`；
- `write` 返回实际写入字节数，`write_all` 循环写满；
- `close` 幂等，返回是否实际关闭；
- `read_all` 默认上限 16 MiB；
- `seek` / `tell` 只适用于可 seek 的流；
- `buffered` 包装读缓冲和写缓冲，`close` 会先 flush；
- `read_async` / `write_async` 返回 task，socket 和文件都支持；
- 文件异步 I/O 使用 libuv 线程池，不阻塞事件循环；
- 同一文件同时只允许一个异步操作；异步未完成时同步 read/write/seek/tell/close 会报错；
- `write_async` 返回实际写入字节数，可能部分写入；
- buffered 流会代理底层流的异步读写；
- 文件和 socket 共用 `std.io`，文件仍可用 `std.fs.seek` / `std.fs.tell`。

### 查找路径

查找顺序：

1. 入口脚本所在目录；
2. 命令行 `-I DIR` / `--package-path DIR`，按出现顺序；
3. 环境变量 `SKEIN_PACKAGE_PATH`，按声明顺序。

例如：

```bash
./skein -I ./lib --package-path ./vendor app/main.skein
SKEIN_PACKAGE_PATH=./lib:./vendor ./skein app/main.skein
```

POSIX 使用 `:` 分隔环境变量路径，Windows 使用 `;`。

优先级固定为：

```text
源文件目录 > 命令行目录 > 环境变量目录
```

### 作用域与导出值

- 导入包在独立作用域中执行，不能读取主包的全局变量；
- 主文件中的 `package` 块也是独立作用域，需要的 `import` 应放在块内；
- 包内函数保留自己的闭包环境；
- `export let` / `export const` 的值在包加载时复制到命名空间；之后导出变量本身再变化，不会自动同步到已导入的命名空间；
- 导出函数仍然可以访问并修改包内部状态。

### 限制

- 每个包使用一个 `<name>.skein` 或 `index.skein` 作为入口；
- 相对导入支持 `.` / `..`，项目根导入支持 `@`；包别名通过 `as` 支持；
- 当前不提供通配导入；
- 子包会被父包加载时自动加载；
- `export` 目前支持 `let`、`const`、命名 `func`、`struct` 和 `trait`。

## 语言概览

```text
1 + 2
-3 + 5
10 / 4.0
7 % 4
1 & 3
1 << 4
"hello" + " world"
[1, 2, 3][1]
{"a": 1, "b": 2}["b"]
```

支持 `%`、`&&`、`||`、`!`、`&`、`|`、`^`、`~`、`<<`、`>>`。
`&&` / `||` 短路求值。

管道 `|>` 把左侧值作为右侧调用的第一个参数：

```text
xs |> f              // f(xs)
xs |> f(a)           // f(xs, a)
range(0, 10) |> std.iter.filter(func(x) -> x % 2 == 0) |> std.iter.collect()
```

- `|>` 左结合，优先级高于算术和比较运算，低于一元运算和后缀调用；
- 右侧不是调用时按 `f(xs)` 处理；右侧是调用时插入为第一个参数；
- 适合 `std.list` / `std.iter` 这类“主体在第一个参数”的 API。

还有 apply-to-result 形式 `|>>`：先求值右侧，再把左侧应用到结果。

```text
func f(x, y) { return x - y; }
let h = x -> y -> f(x, y);

4 |>> h(3)     // (h(3))(4) = f(3, 4)
```

调用参数中的 `$` 是占位符：

```text
func add(a, b) { return a + b; }

let add2 = add($, 2);     // x -> add(x, 2)
add2(1)                   // 3

let three = add($, $);    // (a, b) -> add(a, b)
three(4, 5)               // 9

[1, 2, 3] |> std.list.map($, x -> x * 2)    // [2, 4, 6]
[1, 2, 3] |>> std.list.map($, x -> x * 2)   // [2, 4, 6]
```

占位符规则：

- `$` 只在调用参数中生效；每个 `$` 对应返回函数的一个参数，按从左到右填充；
- 多个 `$` 返回多参数函数，不是自动柯里化；
- 非占位参数在返回的函数被调用时才求值；
- 嵌套调用中的 `$` 绑定到最近的调用，需要外层占位时请显式写 lambda；
- `|>>` 会先求值右侧，副作用顺序与 `|>` 不同；
- `_` 仍然是 REPL 的上一次结果别名，不会被占位符占用。

三元表达式：

```text
x > 0 ? "positive" : "non-positive"
0 ? 1/0 : 5          // 5，只求值选中的分支
```

`? :` 右结合，优先级低于 `||`，高于赋值。

变量：

```text
let x = 10;
x = x + 1;
x

const y = 20;
y = 21;        // 错误：cannot assign to constant y
```

`const` 只约束绑定，不是深不可变：

```text
const a = [1, 2];
a[0] = 9;      // 允许
a              // [9, 2]
```

不可变标量 `const`（`int` / `float` / `bool` / `string` / `char` / `byte` /
`null`）在同一个编译单元内可参与编译期常量折叠；不改变运行时语义，可用
`SKEIN_NO_CONST_PROP=1` 关闭。

变量可以带类型标注：

```text
let name: string = "alice";
let count: int;

count = 3;          // 允许
count = "three";    // 错误：variable count expects int, got string
```

- 类型在运行时的初始化和重新赋值时检查，不做隐式转换；
- 支持 `any int float number bool string char byte bytes list dict deque heap set function builtin callable null userdata result error task generator socket listener websocket stream pair tuple union`；
- 带类型标注的 `let` 可以省略初始值，使用默认值：

```text
int / number  → 0
float         → 0.0
bool          → false
string        → ""
char          → '\0'
byte          → byte(0)
bytes         → 空 bytes
list          → []
dict          → {}
any / null    → null
```

- `function`、`builtin`、`callable`、`userdata`、`task`、`generator`、`socket`、`listener`、`websocket`、`stream`、`pair`、`tuple`、`union` 必须提供初始值；
- `const` 也可以带类型，例如 `const x: int = 1;`；
- 普通类型标注只约束变量绑定，不约束容器内部元素，例如 `let a: list = [0]; a[0] = "x";` 仍允许；
- `for` 的初始化部分也支持类型标注：`for (let i: int = 0; i < 3; i++)`。

容器可以写泛型参数，参数只参与静态检查，运行时擦除：

```text
let xs: list[int] = [1, 2];
let d: dict[int, list[int]] = {1: [2]};
let s: set[int] = std.set.from_list([1, 2]);
let q: deque[int] = std.deque.from_list([1, 2]);
let h: heap[int] = std.heap.from_list([1, 2], less);

let raw = std.json.parse(text);
let ys = raw as list[int];          // 显式信任边界
let first = (raw as list[int])[0];  // 对 cast 结果取下标要加括号
```

- 支持 `list[T]` / `dict[K,V]` / `set[T]` / `deque[T]` / `heap[T]` / `task[T]` / `generator[T]` / `generator[T, S]` / `pair[A, B]` / `tuple[A, B, ...]` / `union[A, B, ...]`，支持嵌套和可空形式；
- 只支持内建容器的类型参数，不支持 `struct Box<T>`、`func f<T>`、泛型 trait 或类型别名；
- 用户类型可以作为参数，例如 `list[Point]`；`list` 仍然表示无元素约束；
- 检查初始化（字面量、`from_list`、构造字段、函数参数、默认值）和插入（下标赋值、`append`、`+=`、`set.add`、`push`、`merge`）；
- 运行时只检查外层容器类型，不扫描元素；动态路径仍可通过 `any` 绕过；
- 模式：`SKEIN_TYPECHECK=off|loose|warn|strict` 或 `--typecheck=...`，默认 `warn`；
  - `loose` 只报确定冲突；
  - `warn` 额外对隐式 `any -> 容器[T]` 发 warning；
  - `strict` 把该 warning 提升为 error；
- `as` 只改变静态类型，运行时是恒等操作，不做转换；确定的元素冲突仍然报错；
- `as` 不能作为赋值目标；`x is list[int]` 和模式中的泛型参数会被拒绝；
- 函数和方法调用会检查显式参数类型；方法还要求接收者有静态类型；
- 导入包的导出函数会使用其参数/返回类型签名，导出结构体的字段/方法和导出变量的类型标注也能解析；
- 解构声明、`match` 模式和 `for` 绑定会从值的静态类型推断元素/字段类型；
- `std.list.map` 会推断简单表达式 lambda 或单 return lambda 的返回类型，`std.list.zip` 会检查第二个参数是 list；
- 未注解的 `let` 会从初始化表达式推断类型；它不是固定标注，重新赋值为其他类型时会更新推断。

函数值可以写函数类型：

```text
let f: (int) -> int;
let g: () -> bool;
let nested: ((int) -> int) -> int;
let callbacks: list[(int) -> int];

struct Handler {
  callback: (Event) -> result;
}
```

- 支持空参数、嵌套函数类型和容器内的函数类型；
- 运行时只检查值是否可调用，不检查参数/返回值；
- 静态检查会校验函数赋值、参数/返回类型，以及通过函数类型发起的调用；
- 参数按逆变、返回值按协变比较；`any` 仍然作为渐进边界；
- 常见 math / string / bytes / 转换 / 文件 builtin 已有函数签名；可变参数 builtin 仍退化为 `callable`，部分带可选参数的容器 builtin 保留末尾可选参数信息；
- 不提供 `(int, int = 1) -> int` 或 `(...int) -> int` 显式语法；
- 默认参数函数可以作为参数传递，并可赋给更短的兼容函数类型；
- 变参函数保留 rest 类型，可以赋给能接受相应参数的函数类型；例如 `(...int) -> int` 可赋给 `(int, int) -> int`；
- 静态检查会拒绝多传参数；少传参数仍交给默认值或运行时处理；
- `obj.method` 会生成去掉 `self` 的函数类型；
- `list.map/filter/reduce/sort_by/group_by/unique_by` 会检查回调签名，并从 `map` / `group_by` 的回调结果推断返回类型；
- `std.iter.map/filter` 会检查回调签名；`std.fn.compose/pipe` 只接受两个函数并推导完整组合类型，更多函数请嵌套组合；
- async 函数的类型为 `(参数) -> task[结果]`，例如 `async func fetch(x: int): string` 的类型是 `(int) -> task[string]`；`await task[T]` 推导为 `T`；
- `task[T]` 可用于变量、参数和返回类型；`std.task.wait` / `std.task.wait_all` 会保留任务结果类型；
- 部分 `std.net` / `std.io` / `std.http` / `std.http2` / `std.websocket` 的 `*_async` builtin 已返回 `task[...]`，例如 `std.http.get_async` 返回 `task[dict[string, any]]`；其余未覆盖的异步 builtin 仍可能退化为 `any`；
- 提供不可构造的静态 userdata 类型 `socket`、`listener`、`websocket`、`stream`；网络、I/O 和文件接口会返回这些类型，`socket` 可传给接受 `stream` 的接口；
- `std.task.catch` 返回 `task[result[T, error]]`；`std.task.try_wait` / `std.task.try_wait_all` / `std.task.timeout` 返回 `result[...]`；
- generator 函数的类型为 `(参数) -> generator[T]`，`T` 由函数体中的 `yield` 表达式合并得到；发送值类型可以写成 `generator[T, S]`；
- 发送类型 `S` 可由返回注解 `generator[T, S]` 指定，也可以由 `let x: S = yield ...` 推断；`std.generator.next(g, value)` 会检查 `value` 是否符合 `S`；
- `for ... in generator[T]` 的元素类型为 `T`；
- `std.iter.collect` 对 `generator[T]` 返回 `list[T]`，`std.iter.map` / `std.iter.filter` 对 generator 返回新的 generator；`std.generator.next(generator[T])` 的 `"value"` 字段类型为 `T`；
- `pair[A, B]` 表示二元异构结构，可用 `(a, b)` 初始化；`p[0]` / `p[1]` 分别得到 `A` / `B`；`zip`、`dict.items`、`iter.enumerate` 使用 pair；
- `tuple[A, B, C, ...]` 表示三元及以上异构结构，可用 `(a, b, c, ...)` 初始化；索引和解构按位置得到类型；
- `union[A, B]` 表示成员之一，支持平铺多个成员，例如 `union[int, string, bool]`；异构 list 字面量会推断为 `list[union[...]]`；赋值和参数按成员检查；
- union 支持常见控制流收窄：`if (x is T)` 的 then/else、`!`、`&&`、`match` 的 `case is T` 以及后续 `default` 分支；
- 支持稳定字段路径收窄，例如 `if (b.value is int) { b.value + 1; }`，包括嵌套字段；
- 对字段/变量的写入、接收者方法调用会撤销相关路径收窄；`||` 只做安全的负向收窄；
- 收窄绑定在发生赋值后回退到声明类型，避免在分支中继续使用过期的窄化类型；
- `pair` / `tuple` / `union` 是静态类型：pair/tuple 在运行时仍是 list，union 的成员兼容性主要由静态检查器保证；
- `match (a, b)` 统一按 tuple/pair subject 处理，模式 `[x, y]` 按位置解构；传单个 tuple 参数使用 `f((a, b))`；
- `std.list.append` / `std.dict.keys` / `std.set.from_list` 等容器 builtin 作为一等值时保留类型变量模板：`let f = std.list.append; f([1], 2)` 得到 `list[int]`，`f([1], "a")` 会报错；该支持仅限已知容器 builtin，不扩展到用户自定义泛型函数；
- `is` 和模式类型不接受函数类型签名。

自增和自减：

```text
let i = 0;
i++            // 0，i 变为 1
++i            // 2，i 变为 2
i--            // 2，i 变为 1
--i            // 0，i 变为 0
```

- `x++` / `x--` 返回修改前的值，`++x` / `--x` 返回修改后的值；
- 只支持 `int` 和 `float`；
- 目标可以是变量、list/dict/bytes 下标或 struct 字段，例如 `a[0]++`、`d["x"]--`、`p.x++`；
- 下标表达式只求值一次；
- `const` 变量不能自增/自减，但 `const` 容器中的元素可以；
- 整数溢出、bytes 超出 `0..255`、dict 键不存在都会报错。

复合赋值：

```text
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 2;
x &= mask;
x |= mask;
x ^= mask;
x <<= 1;
x >>= 1;
```

- 支持变量、下标和字段；
- `a[i] += 1` 的下标只求值一次；
- 字符串和 list 支持 `+=`；
- 类型约束和 `const` 规则与普通赋值一致。

变量名可以包含下划线：`my_value`。

列表和字典：

```text
let a = [1, 2, 3];
a[0] = 9;
a

let d = {"a": 1};
d["b"] = 2;
d

let empty_list = [];
let empty_dict = {};
let empty_list_alt = list();
let empty_dict_alt = dict();
```

支持链式下标赋值：`a[0][1] = 9`、`d["a"]["b"] = 1`。

切片使用 Python 风格的 `start:stop:step`，支持 list、string 和 bytes：

```text
let xs = [0, 1, 2, 3, 4];
xs[1:4]        // [1, 2, 3]
xs[:3]         // [0, 1, 2]
xs[2:]         // [2, 3, 4]
xs[:]          // [0, 1, 2, 3, 4]
xs[-2:]        // [3, 4]
xs[::2]        // [0, 2, 4]
xs[::-1]       // [4, 3, 2, 1, 0]
xs[4:0:-2]     // [4, 2]
"你好世界"[1:3] // "好世"
bytes_from_string("abcd")[1:3] // 两个字节 "bc"
```

- `stop` 不包含在结果中；`step` 省略时为 `1`，不能为 `0`；
- string 按 UTF-8 码点、bytes 按字节、list 按元素切片；
- 边界和 step 必须是整数；边界可省略，负数下标从末尾计数，越界会截断；
- 切片返回新值，不修改原值；暂不支持切片赋值；
- 省略 `stop` 时可直接写 `xs[start::step]`，例如 `xs[i::j]`、`xs[2::2]`。

字典键一律使用下标访问：`p["x"]`。点号用于 struct 字段、enum 变体以及 `std`/包命名空间，普通 dict 写 `p.x` 会报错。

```text
let p = {"x": 1, "y": 2};
p["x"]           // 读取
p["y"] = 3;      // 赋值
p["x"] + p["y"]
```

嵌套访问同样使用下标：`p["a"]["b"] = 1`。

控制流：

```text
if (x > 5) {
  print("big");
} else {
  print("small");
}

while (x > 0) {
  x = x - 1;
}

do {
  x = x - 1;
} while (x > 0);

for (let i = 0; i < 3; i = i + 1) {
  print(i);
}

for (let x in [1, 2, 3]) {
  print(x);
}

for (let key, value in {"a": 1, "b": 2}) {
  print(key + "=" + str(value));
}

for (let i in range(0, 3)) {
  print(i);
}

for (let key in {"a": 1, "b": 2}) {
  print(key);
}
```

控制流的条件圆括号也可以省略（Go 风格）：

```text
if x > 5 { print("big"); } else { print("small"); }
while x > 0 { x = x - 1; }
do { x = x - 1; } while x > 0;
for let i = 0; i < 3; i = i + 1 { print(i); }
switch x { case 1 => print("one"); default => print("other"); }
match x { case 1 => "one"; default => "other"; }
```

条件中的 dict、struct、函数或 `match` 等复合字面量必须加括号，例如 `if (d == {"a": 1}) { ... }`。省略括号的 C 风格 `for` 里，`for i = 0; ...` 是赋值，`i` 必须已声明；要新建变量请写 `for let i = 0; ...`。如果条件本身以 `(` 开头，会按带括号形式解析。

省略 `let` 和圆括号：

```text
for x in [1, 2, 3] {
  print(x);
}

for key, value in {"a": 1, "b": 2} {
  print(key + "=" + str(value));
}
```

`for ... in` 规则：

- 单变量：list 遍历元素，dict 遍历键；deque / heap / set 也遍历元素（heap 按层序，set 按插入顺序）；
- 双变量只能用于 dict，得到 `key, value`；
- list 需要下标时用 `range(len(list))` 或 C 风格 `for`；
- string 遍历 UTF-8 字符；bytes 遍历 `0..255` 的字节值；
- 迭代遍历对象在循环开始时求值一次，循环期间修改原对象不会改变本次遍历；
- 循环变量只在循环作用域内有效；省略 `let` 时同样是块级新变量，会遮蔽外层同名变量；
- 省略 `let` 和圆括号时，循环体必须是 `{ ... }`；操作数以 dict 字面量开头时可以省略括号（如 `for k in {"a": 1}`），但 struct / 函数 / match 等复合字面量作操作数时仍要加括号；
- 支持 `break` / `continue` / `return`。

`do ... while` 至少执行一次循环体；`while` 必须和 `}` 在同一行，结尾分号可省略。

`break` / `continue` 可用于循环；`break` 也可用于 `switch`。

带标签的 `break` / `continue`：

```text
label outer: for (let i in [1, 2, 3]) {
  for (let j in [1, 2, 3]) {
    if (i == 2 && j == 2) { break outer; }
    print(str(i) + "," + str(j));
  }
}
```

- 标签写成 `label NAME: loop_or_switch`；
- `break NAME;` 跳出指定循环或 switch；
- `continue NAME;` 继续指定循环的下一轮；
- 标签只能直接放在循环或 switch 前。

`switch` 值匹配：

```text
switch (x) {
  case 1 => print("one");

  case 2, 3 => {
    print("two or three");
  }

  default => print("other");
}
```

- 选择值只求值一次；
- 无 fallthrough，第一个匹配的 case 执行后退出 switch；
- case 体可以是单条语句或代码块，并有独立作用域；
- `case` 支持多值，如 `case 1, 2, 3`；
- `default` 可选且必须最后；
- `break` 退出 switch，`continue` 作用于外层循环，`return` 退出函数；
- 支持 `int`、`bool`、`string`、`bytes`、`null`；
- 不支持 `float`、`list`、`dict`、`function`、`builtin`、`userdata`；
- case 值必须是值，不能写 `case x > 1 && x < 5` 这种条件；条件判断用 `if/else`；
- `case int` 不是类型模式，而是内置函数值，会作为不支持的 case 值报错。

`match` 模式匹配（表达式）：

```text
let next = match (state) {
  case "idle" => "planning";

  case {kind: "tool_result", ok: true, data: d} => d;

  case [head, ...tail] => head;

  case n if n > 10 => n;

  case Point {x, y} => x + y;

  default => state;
};
```

- `match` 是表达式，返回第一个匹配 arm 的值；
- arm 体是单条语句或代码块，块的值是最后一条语句的值；
- 支持字面量、绑定、`_`、dict（开放匹配 + `...rest`）、list（定长 + `...rest`）、struct（按字段）；
- 支持 or-pattern：`case 1 | 2 | 3`、`case E.A | E.B`；
- 支持整数范围模式：`case 1..9`（包含两端）；
- `case is Type` 是类型模式，例如 `case is int`、`case is Point`、`case is callable`；
- enum 匹配在没有 `default` 且缺少变体时报告 `match is not exhaustive`；
- `case pattern as name` 把整个匹配值绑定到 `name`；
- `case == IDLE` 匹配常量、名字或无载荷枚举变体；
- `match (a, b)` 是 tuple/pair subject，模式 `[x, y]` 按位置解构；
- 守卫写成 `case pattern if expr => ...`；
- 绑定只读，作用域限于该 arm；
- 无 arm 命中且无 `default` 时运行时报错；
- 模式不能调用函数或写比较表达式；
- 不支持 float 字面量。

错误处理：

```text
func parse_positive(text) {
  let n = int(text);

  if (n <= 0) {
    fail "must be positive: " + str(n);
  }

  return n;
}

let n = parse_positive("0") on error(e) {
  print("bad input: " + str(e));
  recover 0;
};
```

- `fail expr;` 抛出错误，`fail;` 抛出空错误；
- 普通值会被包装成 `error(kind="fail", message=...)`；
- `call() on error(e) { ... }` 捕获 `fail`、运行时错误和 `result.err`；
- `result.ok(v) on error(...)` 直接得到 `v`，不执行 handler；
- `e` 是 `error` 值，只在 handler 作用域内有效；
- `str(e)` 或 `std.error.message(e)` 取得消息；
- `std.error.kind(e)` 取得错误类别；
- `recover expr;` 结束 handler 并返回恢复值；
- `pass e;` 把错误继续传给外层；
- handler 必须以 `recover`、`pass` 或 `fail` 结束；
- handler 内不能直接用 `return` / `break` / `continue` 结束；
- 未捕获的 `fail` 在脚本顶层报错并停止脚本。

函数、匿名函数与闭包：

```text
func add(a, b) {
  return a + b;
}

func typed_add(a: int, b: int) {
  return a + b;
}

func double(x) -> x * 2;

func fib(n) {
  if (n < 2) { return n; }
  return fib(n - 1) + fib(n - 2);
}

func make_counter() {
  let n = 0;
  func inc() {
    n = n + 1;
    return n;
  }
  return inc;
}

let c = make_counter();
c()
c()
```

参数可以带可选的类型约束：

```text
func apply(fn: callable, x: int) {
  return fn(x);
}

apply(sqrt, 16)
```

- 语法为 `参数名: 类型`，不写类型表示 `any`；
- 支持 `any`、`int`、`float`、`number`、`bool`、`string`、`bytes`、`list`、`dict`、`function`、`builtin`、`callable`、`null`、`userdata`、`task`、`generator`、`socket`、`listener`、`websocket`、`stream`、`pair`、`tuple`、`union`；
- 调用时做运行时检查，不做隐式转换；
- 类型错误不会进入函数体；
- 逻辑运算和比较运算都返回 `bool`。

函数可以带返回类型标注：

```text
func numbers(): list[int] {
  return [1, 2];
}

func numbers_fast(): list[int] -> [1, 2];

let xs: list[int] = numbers();
xs[0] = "a";   // 错误：expected int, got string
```

- 返回类型只用于静态检查，运行时不做转换；
- 检查器检查每个 `return` 表达式；
- `return;` 对非可空返回类型报错；
- 返回类型会传播给调用方，减少 `any` warning；
- 方法也支持返回类型标注；
- 匿名函数也支持返回类型标注，例如 `func(x: int): int -> x * 2`；短 lambda 不支持，需要时请使用 `func`；
- 函数/lambda 的表达式体统一使用 `->`；`=>` 只用于 match/switch arm 和 FFI 返回类型。

参数可以使用默认值：

```text
func greet(name: string, greeting: string = "hello") {
  return greeting + ", " + name;
}

func offset(a: int, b: int = a + 1) -> b;

greet("alice");          // "hello, alice"
greet("alice", "hi");    // "hi, alice"
offset(10);              // 11
```

- 默认参数必须排在必填参数之后；
- 省略参数时才求值，每次调用重新求值；
- 默认值可以引用更早的参数，例如 `b = a + 1`；
- 默认值不能引用后面的参数，否则报 `parameter ... is used before it is initialized`；
- 方法中的默认值可以引用 `self`，但 `self` 本身不能有默认值；
- trait 方法声明不能有默认值，`impl` 方法可以额外提供默认值；
- 可变参数必须最后，且不能有默认值。

函数是一等值，可以保存到变量、列表或字典中，并通过任意表达式调用：

```text
let fs = [adder(1), adder(2)];
fs[0](10)
d["handler"](42)
adder(3)(4)
```

匿名函数可以使用 `->` 简写；单参数、多参数和零参数都支持，函数体是单个表达式：

```text
let inc = x -> x + 1;
let add = (a, b) -> a + b;
let zero = () -> 7;

[1, 2, 3] |> std.list.map(x -> x * 2)
```

`->` 的函数体只支持表达式；需要多语句或显式 `return` 时用 `func(x) { ... }`。

匿名函数使用同一个 `func` 关键字，省略函数名：

```text
let double = func(x) { return x * 2; };
double(21)

let inc = func(x) -> x + 1;
inc(41)

(func(x) { return x + 1; })(41)
(func(x) -> x + 1)(41)
```

单表达式可以用 `->` 简写，表达式结果自动成为返回值。`->` 优先级最低，因此 `func(x) -> x * 2 + 1` 的函数体是 `x * 2 + 1`。

匿名函数同样可以捕获定义处的作用域，并作为参数或返回值：

```text
func make_adder(n) {
  return func(x) { return x + n; };
}

let add5 = make_adder(5);
add5(3)                 → 8
```

也可以通过变量绑定实现递归：

```text
let fact = func(n) {
  if (n <= 1) { return 1; }
  return n * fact(n - 1);
};

fact(5)                 → 120
```

匿名函数规则：

- 没有名字，诊断和调用栈中显示为 `<anonymous>`；
- 每次求值都会创建新的闭包；
- `{ ... }` 函数体使用显式 `return`；
- `-> exp` 函数体隐含 `return exp`；
- 支持空参数和空函数体；
- 参数支持类型约束、默认值和可变参数，规则与命名函数一致；
- 立即调用时建议给匿名函数加括号，例如 `(func(x) { ... })(1)`。

没有显式 `return` 的函数，以及 `print`，返回 `null`。`null` 是假值，可用于条件判断，也可以作为参数传递。

函数参数也可以直接解构：

```text
func sum([a, b]) { return a + b; }
func pair({x, y}) { return x * y; }
struct P { x: int; y: int; }
func point(P {x, y}) { return x + y; }
func first([head, ...tail]) { return head; }
```

- 解构参数占一个位置，不参与命名查找；
- 支持 list、dict、struct、enum 变体模式；
- 解构失败会报 `function parameter does not match its pattern`。

可变参数：

```text
func sum(...nums: number) {
  let total = 0;
  for (let n in nums) {
    total = total + n;
  }
  return total;
}

sum()           // 0
sum(1, 2, 3)    // 6
```

规则：

- `...rest` 只能是最后一个参数；
- 最多一个；
- `rest` 在函数内是 `list`，没有额外参数时是 `[]`；
- 类型标注表示每个额外参数的类型，例如 `...nums: number`；
- 固定参数不足会报错；
- 方法也可以使用，`self` 仍然是第一个固定参数；
- trait 方法暂不支持可变参数。

调用点可以用 `...` 展开列表：

```text
func sum(...nums: number) {
  let total = 0;
  for (let n in nums) { total = total + n; }
  return total;
}

let xs = [1, 2, 3];
sum(...xs)       // 6
sum(0, ...xs)    // 6
```

展开项必须是 `list`，可以和普通参数混用。

结构体、`impl` 与 `trait`：

```text
struct Point {
  x: int;
  y: int;
}

impl Point {
  func length(self) {
    return sqrt(self.x * self.x + self.y * self.y);
  }

  func moved(self, dx: int, dy: int) {
    return Point { x: self.x + dx, y: self.y + dy };
  }
}

let p = Point { x: 3, y: 4 };
p.x
p.length()
let q = p.moved(1, 2);
```

- `struct` 声明字段；没有默认值的字段都必须初始化，未知字段或重复字段会报错；
- 构造使用 `Point { x: 3, y: 4 }`，字段顺序不限；
- 支持 copy-with：`Point { ...p, x: 9 }`，`...` 必须放在最前，后面的显式字段覆盖原字段；
- 字段可以声明默认值：`struct Config { host: string = "localhost"; port: int = 80; }`；省略字段时使用默认值；
- 默认值在定义包的作用域中求值，每次构造重新求值；不能引用 `self` 或其他字段；
- `std.struct.decode` 不自动填充默认值，缺失字段仍然报错；
- 字段可以读写：`p.x`、`p.x = 5`；
- 方法第一个参数必须是显式 `self`；
- 方法参数支持默认值，但 `self` 不能有默认值；默认值可以引用 `self` 和更早的参数；
- `struct` 只声明字段；所有方法必须写在 `impl Type` 中；
- 方法只能通过实例调用：`p.length()`；
- 方法本身可以作为值提取，例如 `let f = p.length; f()`；
- 方法值绑定提取时的结构体快照，之后修改原结构体不会影响已绑定的方法；
- 方法可以修改 `self` 的字段和下标；结构体是值类型，修改只作用于方法内的接收者副本，调用方原值不变。需要保留修改时返回新结构体，例如 `return self`；
- 结构体是值语义，赋值和作为参数传递时使用 copy-on-write；
- 结构体类型可以用在变量和参数类型标注中：`let p: Point = Point { x: 1, y: 2 }`、`func f(p: Point)`；结构体类型没有默认值，变量必须初始化；
- 字段类型必须是已声明的类型；支持自引用，例如 `struct Node { value: int; next: Node?; }`；
- 不允许创建环：`a.next = b; b.next = a;` 会报 `recursive value would create a cycle`；共享子结构（DAG）允许；
- 所有字段和方法都是公开的；暂不实现 `_` 私有语法；
- 字段可以保存函数值，作为普通回调使用，不会自动绑定 `self`：

```text
struct Handler {
  callback: callable;
}

let h = Handler {
  callback: func(x) { return x + 1; }
};

h.callback(41)      // 42
```

```text
struct Config {
  host: string = "localhost";
  port: int = 80;
}

let a = Config {};
let b = Config { port: 8080 };
let c = Config { ...a, port: 9090 };
```

- 不支持继承、动态添加方法、静态方法、用户自定义泛型或运算符重载；内建容器的类型参数见变量类型标注章节。

`trait` 定义接口，`impl Trait for Type` 显式实现：

```text
trait Drawable {
  func draw(self);
}

impl Drawable for Point {
  func draw(self) {
    print("point");
  }
}

func render(x: Drawable) {
  x.draw();
}

render(p)
```

- `trait` 只包含方法签名，不包含字段、默认方法或 trait 继承；
- trait 方法声明不能有默认值；`impl` 方法可以额外提供默认值；
- 接口是名义式的：只有显式写了 `impl Drawable for Point`，`Point` 才能传给 `Drawable`；
- 如果方法已经存在于 `impl Point` 中，可以只写 `impl Drawable for Point;` 做接口适配；
- trait 方法名不能与类型已有方法冲突；enum 上也不能与任意变体字段重名；
- `impl` 必须写在接收者类型所在的包中；trait 可以来自其他包，但未导出的 trait 不能被外部实现；
- 一个类型对同一个 trait 只能实现一次；
- struct、enum 和 trait 名称共享类型命名空间，不能在同一个包中重名；
- 类型名按包注册，一个包对应一个源文件；不同包可以定义同名结构体或 trait；
- 类型默认不跨包可见；`export struct` / `export trait` 后可以按包路径使用；
- 导出的 trait 签名只能引用已导出的类型或内建类型，否则外部类型无法实现它；
- 导入之后用 `aa.Point`、`lib.Drawable` 这样的限定名引用：

```text
// package aa
export struct P { x: int; }
export trait D { func d(self); }

// main
import aa;
let p: aa.P = aa.make();
p is aa.D

import aa as lib;
let q: lib.P = lib.make();
```

- 其他包可以为自己的类型实现导入的 trait：

```text
// main
import aa;
struct Q { y: int; }
impl aa.D for Q {
  func d(self) { return self.y; }
}
```

- 未导出的类型不能通过包路径引用，但它的值仍可作为普通值传递和方法调用。

命名函数装饰器：

```text
func trace(fn) {
  return func(...args) {
    print("enter");
    let result = fn(...args);
    print("exit");
    return result;
  };
}

@trace
func add(a, b) {
  return a + b;
}

add(1, 2)      // enter / exit / 3
```

- `@expr` 写在命名 `func` 前，可以连续写多个；多个装饰器从下往上应用；
- 支持普通函数、`async func`、generator 函数和 `export func`；
- 装饰器表达式在函数绑定前求值，应用在函数创建后完成；
- 装饰器是普通高阶函数，最终结果必须 callable；
- 函数名绑定到装饰后的值，递归调用会经过装饰器；
- 匿名函数和 `let f = func(...)` 仍然手动包装：`f = dec(f)`。

方法装饰器：

```text
func trace(fn) {
  return func(...args) {
    print("enter");
    let result = fn(...args);
    print("exit");
    return result;
  };
}

struct Point { x: int; y: int; }

impl Point {
  @trace
  func area(self, scale: int = 1) {
    return self.x * self.y * scale;
  }
}

Point { x: 3, y: 4 }.area()      // enter / exit / 12
```

- `@expr` 写在 `impl` 方法前，可以连续写多个；多个装饰器从下往上应用；
- 装饰器在 `impl` 声明执行时应用一次，不随方法调用重复执行；
- 装饰器是普通高阶函数，不需要专门的定义语法；
- 装饰器收到 unbound function，`self` 是第一个参数；推荐用 `...args` 转发；
- 支持固有方法和 `impl Trait for Type` 方法；调用点、方法值提取和 trait 分派不变；
- 默认值由生成的 wrapper 解析，装饰器看到的是补全后的参数；
- 装饰器必须返回 callable；不支持类型声明装饰器和解构参数方法；
- generator 方法和 async generator 方法也支持装饰器；生成的 wrapper 是普通函数，
  直接返回装饰器链产生的 generator，是否惰性由装饰器决定；async generator 的
  wrapper 不 `await`，迭代、发送值和 `close` / `defer` 语义保持。

async 方法：

```text
import std;

struct Client { host: string; }

impl Client {
  async func get(self, path: string) {
    return await std.http.get_async(self.host + path);
  }
}

let c = Client { host: "http://127.0.0.1:8080" };
let result = await c.get("/api");
```

- `p.m()` 返回 task，`await p.m()` 等待结果；
- 方法值 `let f = p.m; await f()` 同样返回 task；
- async 方法支持默认参数、可变参数、`defer` 和递归；
- `self` 是接收者副本；共享 userdata 句柄正常共享，普通字段修改不会回写调用方；
- enum 方法也支持 `async func`；
- async generator 也支持：函数体内可以 `await`，类型仍为 `generator[T]`，`next` / 迭代会驱动等待。

async trait 方法：

```text
trait Service {
  async func handle(self, request);
}

impl Service for Client {
  async func handle(self, request) {
    return await self.get(request["path"]);
  }
}

async func dispatch(service: Service, request) {
  return await service.handle(request);
}
```

- trait 声明 `async func`，实现也必须是 async；反之亦然；
- `impl Service for Client;` 可以适配已有的 async 方法；
- async trait 方法不能有默认值或可变参数；
- `std.meta.methods` 的方法描述包含 `async` 字段。

async 方法装饰器：

```text
func timed(fn) {
  return async func(...args) {
    print("before");
    let result = await fn(...args);
    print("after");
    return result;
  };
}

impl Client {
  @timed
  async func get(self, path) {
    return await std.http.get_async(self.host + path);
  }
}
```

- 装饰器是普通函数，返回 async callable；
- 生成的 wrapper 是 async，并 `await` 装饰后的方法；
- 直接把 `async func` 当装饰器会返回 task，不是 callable；
- 取消外层 task 不会自动取消被 await 的内层 task。

枚举（带载荷变体）：

```text
enum Event {
  UserMsg { text: string; };
  ToolResult { ok: bool; data: any; };
  Timeout;
}

let a = Event.UserMsg { text: "hi" };
let b = Event.Timeout;

match (a) {
  case Event.UserMsg { text } => text;
  case Event.ToolResult { ok: true, data: d } => d;
  case Event.Timeout => "timeout";
  default => "other";
}
```

- 有载荷的变体必须写 `Event.Variant { field: value, ... }`；
- 无载荷变体直接写 `Event.Variant`；
- 变体字段可以声明默认值；所有字段都有默认值时允许 `Event.Variant {}`；
- 字段默认值在定义包作用域求值，每次构造重新求值；不能引用 `self` 或其他字段；
- copy-with 目前只支持 struct，不支持 enum；
- 载荷字段只读，字段访问按当前变体校验；
- 方法只能写在 `impl Event` 中；enum 方法内 `self` 只读，修改状态时返回新 enum；
- `impl Trait for Event` 可以让 enum 参与 trait 接口；`Event` 也能用于 `is` 和类型标注；
- 方法名不能与任意变体的字段重名；
- `kind_of(e)` 是 `"enum"`，`type_name(e)` 是枚举名；
- `match` 支持变体模式、字段模式和守卫；
- JSON 编码为 `{"$enum":"Event.UserMsg","payload":{...}}`；
- 暂不支持互递归 enum（自引用 enum 已支持）、enum 作为 dict key、enum 的 JSON 自动解码。

```text
enum Shape {
  Circle { r: int; };
  Rect { w: int; h: int; };
}

impl Shape {
  func area(self) {
    return match (self) {
      case Shape.Circle { r } => r * r;
      case Shape.Rect { w, h } => w * h;
    };
  }
}

impl Drawable for Shape {
  func draw(self) { return self.area(); }
}
```

运行时类型检查：

```text
kind_of(x)        // "int" / "list" / "struct" / ...
type_name(x)      // "int" / "Point" / ...
type_module(x)    // 逻辑包名：入口为 "main"，包类型为包名
type_source(x)    // 调试用源文件路径

x is int
x is Point
x is Drawable
```

`any` 只是类型约束，不会丢失运行时类型。遍历异构列表时可以这样分派：

```text
for (let value in values) {
  if (kind_of(value) == "struct") {
    print(type_name(value));
  } else if (value is int) {
    print("int");
  }
}
```

`is` 只接受类型名或 trait 名作为右操作数；结构体类型名按包解析。

内置函数：`sqrt`、`exp`、`log`、`abs`、`sin`、`cos`、`tan`、`asin`、`acos`、`atan`、`atan2`、`pow`、`floor`、`ceil`、`round`、`trunc`、`log2`、`log10`、`hypot`、`min`、`max`、`len`、`range`、`print`、`println`、`list`、`dict`、`int`、`float`、`bool`、`str`、`kind_of`、`type_name`、`type_module`、`type_source`、`format`、`read_file`、`write_file`、`append_file`、`file_exists`、`file_open`、`file_read`、`file_write`、`file_seek`、`file_tell`、`file_eof`、`file_close`、`file_flush`、`input`、`split`、`split_ws`、`trim`、`find`、`substr`、`bytes_from_string`、`bytes_to_string`、`bytes_len`、`bytes_get`、`bytes_slice`、`bytes_concat`、`bytes_hex`、`bytes_from_hex`、`bytes_to_int`、`int_to_bytes`、`read_bytes`、`write_bytes`、`append_bytes`、`file_read_bytes`、`file_write_bytes`。内置函数同样是一等值：

```text
let f = sqrt;
f(9)

func apply(fn, x) { return fn(x); }
apply(sqrt, 16)
```

`std.iter.for_each(iterable, fn)` 逐个调用 `fn`，忽略返回值并返回 `null`。回调可以写 1 个或 2 个参数：1 个参数时接收元素（dict 为 key）；2 个参数时 list / string / bytes / set / deque / heap / generator 接收 `(index, value)`，dict 接收 `(key, value)`。回调里的 `break` / `continue` 不可用，`return` 只退出回调本身；async 回调不会被自动 `await`。

函数调用会先按从左到右的顺序求值参数，再检查被调用者类型和参数个数，因此参数中的副作用总会执行。

参数表达式中的 `return` / `break` / `continue` 会向外传播，当前函数调用不会执行。

极深值有保护上限：`print` / `str` 超过 100 层输出 `...`，`value_equals` 超过 512 层返回 `false`，`freeval` 超过 1000 层停止释放，JSON 深度上限 512。

字符串支持常用转义：`\n`、`\t`、`\r`、`\\`、`\"`、`\'`。

## 语法约定

- 顶层可以直接写表达式，不需要分号。
- 块 `{ ... }` 内的普通表达式语句需要以 `;` 结尾。
- `[]` 和 `{}` 分别表示空列表和空字典；空字典可以跨行书写，也可以包含注释。
- `{}` 出现在控制流/函数体位置时仍表示空代码块，例如 `if (x) {}`、`func f() {}`、`while (x) {}`。
- 块内单独出现的 `{}` 按空字典表达式处理；需要空代码块语句时写 `{ ; }`。
- 空字典和空代码块都可以跨行书写，也可以包含注释。
- 比较运算符包括 `>`、`<`、`<>`/`!=`、`==`、`>=`、`<=`。
- `//` 开始单行注释。
- 花括号、方括号、圆括号内部的换行会被自动忽略，支持多行表达式。
- 控制流和声明体的 `{` 必须和头部在同一行；`else` 必须和前面的 `}` 在同一行；`do ... while` 的 `while` 必须和 `}` 在同一行。
- 闭包捕获定义处的词法作用域；闭包环境会在程序退出时统一清理。
- 用户函数调用深度上限为 1000，超出时报告 `maximum recursion depth`。

## 类型与转换

- `int` 是 64 位有符号整数；十进制整数字面量超出 64 位范围时按 `float` 解析，`0x` / `0b` / `0o` 字面量超出 `int64` 范围时报词法错误，整数运算溢出会报 `integer overflow`。
- 整数字面量支持 `0x` / `0X`、`0b` / `0B`、`0o` / `0O` 进制前缀，浮点字面量支持科学计数法（如 `1e3`、`1.5e-2`）；数字之间可以用 `_` 分隔，例如 `1_000_000`、`0xFF_FF`、`1.5e1_0`。下划线只能位于两个数字之间。
- `int` 与 `float` 混合运算提升为 `float`，例如 `1 + 2.5` 得到 `3.5`；`1 == 1.0` 为真。超过 2^53 的整数与 float 比较仍按数学值精确判断，但转成 `float`（混合运算或 `float()`）时会丢失精度。
- 整数除法向零截断，取模结果符号随被除数：`-7 / 4 == -1`，`-7 % 4 == -3`，`7 % -4 == 3`。
- 位运算按 64 位进行，移位次数必须在 `0..63` 之间。
- `bool` 类型有 `true` / `false` 两个字面量；`bool(x)` 按真值规则转换。
- 真值规则：`null`、`false`、`0`、`0.0`、空字符串、空 bytes、空列表、空字典、空 deque、空 heap、空 set 为假；其余值（包括 `true`、函数和内置函数）为真。
- `str(true)` 得到 `"true"`，`int(true)` 得到 `1`，`float(false)` 得到 `0.0`。
- `null` 是空值字面量，表示“没有值”：没有 `return` 的用户函数、`print` / `println` 和 `return;` 都返回 `null`。顶层 `null` 结果不会显示，但可以赋值、比较、传递，或通过 `print(null)` / `format("{}", null)` 输出。
- `&&`、`||`、`!` 返回 `bool`，并短路求值。
- `==` / `!=` 对所有类型有效，返回 `bool`；大小比较只支持数值之间或字符串之间，也返回 `bool`。
- 列表和字符串下标必须是 `int`，`1.0` 不会被隐式截断；字符串下标按 UTF-8 码点计数，`"你好"[0]` 得到 `你`。
- `len(x)` 支持字符串、char、bytes、list、dict、deque、heap、set：字符串按 UTF-8 码点计数，char 为 1，bytes 按字节数，其余按元素数。
- `range(length)` / `range(start, length[, step])` 生成整数序列，第二参数是长度而不是结束值；`step` 不能为 0。
- 字典键按值相等判断，`1` 和 `1.0` 是同一个键；字面量中的重复键以最后一个为准。
- `list`、`dict` 和 `struct` 是值类型：赋值、传参和返回在语义上复制，但内部使用写时复制（COW），只读或复制时共享底层数据，真正修改时才分离。
- `method` 是绑定到结构体实例的方法值；当前版本捕获提取时的实例快照。
- `bytes` 和字符串目前仍然复制内容；函数值共享闭包环境，文件句柄等原生资源共享底层句柄。
- 显式转换：`int(x)`、`float(x)`、`bool(x)`、`str(x)`。`int` / `float` 接受整数、浮点数、布尔值或字符串；`bool` 按真值规则转换；`str` 接受任意值。
- 浮点数输出使用可往返的最短表示并保留 `.0`：`1.0` 显示为 `1.0`，`1.23456` 显示为 `1.23456`。
- 没有字符串与数字之间的隐式转换，`1 + "2"` 会报错；需要时请使用 `int()` / `float()` / `str()`。

## 对象生命周期

- 普通值由引用计数管理；`list` / `dict` / `struct` / `bytes` 的 payload 会被别名共享，写入时才分离；`string` / `bytes` 使用引用计数缓冲；
- 函数值复制时共享函数代码和闭包环境，文件句柄等原生资源共享底层句柄；
- 闭包按引用捕获定义处的作用域，因此局部变量会在闭包存活期间继续存在；
- 运行时使用**作用域粒度的标记式环回收器**（`gc.cpp`）：从全局作用域、当前作用域链和恢复栈出发，标记可达 scope，再清理不可达 scope 的符号表；
- 回收每 64 个顶层语句或 scope 数量明显增长时执行，只在顶层语句边界进行；
- 值遍历覆盖 `list` / `dict` / `struct` / `enum` / `lru` / `result` / `method`，所以闭包即使只被 enum、LRU 或 result 持有，也不会被误回收；
- 这不是完整的逐值 tracing GC：值本身仍由引用计数管理，没有压缩、移动或 finalizer；
- 如果未来原生资源也能保存语言值，需要扩展回收器的遍历规则。

## 格式化

`format(fmt, ...)` 返回格式化后的字符串；`print` 支持两种参数形式：

- `print(x)`：直接打印一个值；
- `print(fmt, ...)`：第一个参数是格式串，其余参数用于格式化；
- `print()`：输出一个空行。

示例：

```text
format("x = {}", 3)
format("{} + {} = {}", 1, 2, 3)
format("{1} {0}", "a", "b")
format("{{}} = {}", 1)
print("x = {:.2f}", 1.5)
```

支持的格式：

| 写法 | 含义 |
| --- | --- |
| `{}` / `{0}` | 默认字符串化 |
| `{:s}` | 字符串 |
| `{:d}` | 整数 |
| `{:f}` / `{:.2f}` | 浮点，默认 6 位小数 |
| `{:x}` / `{:X}` | 十六进制整数 |
| `{:10}` / `{:<10}` / `{:>10}` / `{:^10}` | 宽度和对齐 |
| `{:*^10}` | 自定义填充字符 |
| `{:08}` | 零填充 |
| `{{` / `}}` | 字面量 `{` / `}` |

自动编号 `{}` 和显式编号 `{0}` 不能混用；参数不足会报错，多余参数会被忽略。注意 `print("x", 1)` 按格式模式处理，输出 `x` 并忽略多余的 `1`。

## 外部命令

通过 `std.process` 执行 shell 命令。全局 `command` / `command_status` 已移除。

`std.process.command(cmd[, env])` 执行命令并返回标准输出字符串：

```text
import std;

std.process.command("echo hi")                    → "hi"
std.process.command("echo $FOO", {"FOO": "bar"})  → "bar"
print(std.process.command("ls"))
```

- POSIX 使用 `/bin/sh -c`；
- Windows/MinGW 使用 `cmd.exe /C`；
- 可选的 `env` 是 dict，键和值必须是字符串；
- `env` 在父进程环境基础上覆盖，只对当前命令生效；
- 标准错误继承到当前进程，不会被捕获；
- 返回值末尾的 `\r` / `\n` 会被去掉；
- 命令无法启动时报告 `command failed to start`；
- 非零退出码不会自动报错，当前返回值只有标准输出，没有退出码。

如果只需要退出码，使用 `std.process.command_status(cmd[, env])`：

```text
import std;

std.process.command_status("exit 0")                     → 0
std.process.command_status("exit 3")                     → 3
std.process.command_status("exit $CODE", {"CODE": "3"})  → 3
```

`std.process.command_status` 会直接继承标准输入/输出/错误，因此命令输出会正常显示；它返回退出码整数。

如果需要同时获取 status、stdout、stderr，或控制 stdin/cwd/env，使用
`std.process.run`：

```text
import std;

let r = std.process.run("printf hello");
r["status"]
bytes_to_string(r["stdout"])
```

### 长驻进程与管道

```text
std.process.spawn(file[, options]) → process
std.process.wait(process[, timeout_ms]) → status
std.process.kill(process[, signal])
std.process.close(process)
std.process.pid(process)
```

`options`：

```text
args   list[string]，不包含 argv[0]；程序路径由 file 提供
env    dict[string, string]；省略时继承父进程环境
cwd    string
stderr "pipe"（默认）或 "inherit"
```

`spawn` 直接执行 `file`，不经过 shell。返回：

```text
{
  "pid": int,
  "stdin": stream,
  "stdout": stream,
  "stderr": stream,
  "handle": process
}
```

管道是普通 `std.io` 流，支持同步和异步读写：

```text
import std;

let p = std.process.spawn("/bin/cat");

std.io.write_all(p["stdin"], "hello");
std.io.close(p["stdin"]);

print(bytes_to_string(std.io.read(p["stdout"], 4096)));
print(std.process.wait(p));
```

异步形式：

```text
let data = await std.io.read_async(p["stdout"], 4096);
```

`std.io.stdin`、`std.io.stdout`、`std.io.stderr` 也作为标准流暴露，方便子进程侧实现自己的协议。

注意：

- `std.process.close` 只关闭管道，不杀死进程；需要终止时先 `kill`；
- `stderr` 始终是管道，父进程应读取，避免子进程因 stderr 写满而阻塞；
- 管道不支持 `seek` / `tell`；
- `wait` 超时后不会自动杀死进程。

#### JSON Lines worker 示例

worker 脚本：

```text
import std;

while (true) {
  let line = std.io.read_line(std.io.stdin);
  if (line == null) { break; }

  let request = std.json.parse(bytes_to_string(line));
  let result = request["params"][0] + request["params"][1];
  let response = {"id": request["id"], "ok": true, "result": result};

  std.io.write_all(std.io.stdout,
                   bytes_from_string(std.json.stringify(response) + "\n"));
  std.io.flush(std.io.stdout);
}
```

父进程：

```text
import std;

let p = std.process.spawn("skein", {"args": ["worker.skein"]});
std.io.write_all(p["stdin"],
                 bytes_from_string("{\"id\":1,\"params\":[2,3]}\n"));
std.io.flush(p["stdin"]);

print(bytes_to_string(std.io.read_line(p["stdout"])));
std.io.close(p["stdin"]);
print(std.process.wait(p));
```

### Worker 池

在 spawn + JSON Lines 协议之上提供进程池：

```text
std.worker.pool(size, script[, options]) -> pool
std.worker.call(pool, method, args[, timeout_ms]) -> result
std.worker.call_bytes(pool, method, args, body[, timeout_ms]) -> result
std.worker.map(pool, method, items[, timeout_ms]) -> list
std.worker.submit(pool, method, args) -> job
std.worker.submit_bytes(pool, method, args, body) -> job
std.worker.wait(job[, timeout_ms]) -> result
std.worker.shutdown(pool)
std.worker.stats(pool) -> dict
```

`options`：

```text
queue_limit       默认 1024
restart_limit     默认 3
ready_timeout_ms  默认 5000
protocol          "jsonl"（默认）或 "hybrid"
```

`protocol = "hybrid"` 使用 JSON header + raw body，避免大 body 的 JSON
转义和 base64 开销。请求帧：

```text
{"id":1,"method":"handle","params":[...],"body_len":N}\n
<N 字节原始 body>
```

响应帧：

```text
{"id":1,"ok":true,"result":...,"body_len":M}\n
<M 字节原始 body>
```

hybrid worker 启动时发送：

```text
{"ready":true,"version":1,"protocol":"hybrid"}
```

并新增：

```text
std.worker.call_bytes(pool, method, args, body[, timeout_ms]) -> result
std.worker.submit_bytes(pool, method, args, body) -> job
```

`call_bytes` / `wait` 成功时返回的 value 是：

```text
{"value": <result>, "body": <bytes>}
```

单个 body 上限为 256 MiB。默认 JSON Lines worker 脚本不受影响。

worker 脚本读取 hybrid 请求：

```text
let line = std.io.read_line(std.io.stdin);
let request = std.json.parse(bytes_to_string(line));
let body = bytes_from_string("");
if (request["body_len"] > 0) {
  body = std.io.read_exact(std.io.stdin, request["body_len"]);
}

let response = {
  "id": request["id"],
  "ok": true,
  "result": request["params"][0],
  "body_len": len(body)
};
std.io.write_all(std.io.stdout,
                 bytes_from_string(std.json.stringify(response) + "\n"));
std.io.write_all(std.io.stdout, body);
std.io.flush(std.io.stdout);
```

- `size`：worker 进程数
- `script`：worker 脚本路径
- `method`：worker 脚本中负责分发的逻辑
- `args` / `items`：JSON 可序列化数据
- `timeout_ms`：可选，0 或省略表示不设置超时
- `call` 返回 `result`，worker 错误、超时和管道断开都不会直接中断进程
- `submit` 立即提交任务并返回 `job`；`wait` 阻塞等待结果
- 任务多于 worker 时进入队列，由 `wait` 驱动调度
- worker 崩溃或超时后会关闭并重新启动该 worker
- 单个 worker 连续重启超过 3 次后会被标记为不可用
- `map` 把任务分批分发给多个 worker，保持输入顺序；任一任务失败时报告错误

worker 脚本约定使用 JSON Lines，并在启动后先发送 ready 帧：

```text
import std;

std.io.write_all(std.io.stdout,
                 bytes_from_string("{\"ready\":true,\"version\":1}\n"));
std.io.flush(std.io.stdout);

while (true) {
  let line = std.io.read_line(std.io.stdin);
  if (line == null) { break; }

  let request = std.json.parse(bytes_to_string(line));
  let result = request["params"][0] + request["params"][1];
  let response = {"id": request["id"], "ok": true, "result": result};

  std.io.write_all(std.io.stdout,
                   bytes_from_string(std.json.stringify(response) + "\n"));
  std.io.flush(std.io.stdout);
}
```

主进程：

```text
import std;

let pool = std.worker.pool(4, "worker.skein");
let r = std.worker.call(pool, "add", [2, 3]);
print(std.result.unwrap(r));

print(std.worker.map(pool, "add", [[1, 2], [3, 4], [5, 6]]));

let a = std.worker.submit(pool, "add", [7, 8]);
let b = std.worker.submit(pool, "add", [9, 10]);
print(std.result.unwrap(std.worker.wait(a)));
print(std.result.unwrap(std.worker.wait(b)));

std.worker.shutdown(pool);
```

`std.worker.stats(pool)` 返回：

```text
{
  "workers": int,
  "busy": int,
  "available": int,
  "queued": int,
  "restarts": int
}
```

当前限制：

- 使用 JSON Lines，不支持原始 `bytes`；
- 超时和崩溃后只重启当前 worker，没有重启次数上限和池级熔断；
- worker 脚本需要自己实现 `method` 分发和错误响应；
- Windows worker 行为待单独验证。

环境变量名不能为空，也不能包含 `=`。在 Windows cmd 中引用变量使用 `%FOO%`，POSIX shell 使用 `$FOO`。

Windows/MinGW 下命令使用 `CreateProcessW` 传递 Unicode 命令行和 Unicode 环境块；捕获的 stdout 约定为 UTF-8。

**安全提示**：这些函数会执行任意 shell 命令，只应对可信输入使用。

## 字符串处理

```text
trim(text)                  → 去掉首尾空白
split(text, separator)      → list<string>
split_ws(text)              → list<string>
find(text, substring)       → 码点下标 / -1
substr(text, start[, length]) → string
```

示例：

```text
trim("  hello\t\n")          → "hello"
split("a,b,c", ",")          → ["a", "b", "c"]
split(",a,", ",")            → ["", "a", ""]
split_ws("  a  b\tc ")       → ["a", "b", "c"]
find("hello", "ll")          → 2
find("hello", "x")           → -1
substr("hello", 1, 3)        → "ell"
substr("你好世界", 1, 2)      → "好世"
```

规则：

- `trim` 和 `split_ws` 只把 ASCII 空白（空格、Tab、换行等）视为空白；
- `split` 使用非重叠匹配，保留首尾空字段；
- `split` 的分隔符不能为空字符串；
- `find` 返回 **UTF-8 码点下标**，找不到返回 `-1`，空子串返回 `0`；
- `substr` 的 `start` 和 `length` 都是码点单位；`start` 超出长度返回空字符串，`length` 超出末尾会截断；
- `start` 和 `length` 不能为负数。

组合使用：

```text
let parts = split_ws(input("请输入两个数: "));
let a = int(parts[0]);
let b = int(parts[1]);
print(a + b);
```

## 控制台输入

`input([prompt])` 从 stdin 读取一行：

```text
let name = input("Name: ");
print("hello " + name);
```

- 返回的字符串不包含末尾换行；
- 可选的 `prompt` 会原样输出到 stdout，不经过 `format`；
- 读到 EOF 时返回 `null`；
- 空行返回空字符串；
- CRLF 会被处理，末尾 `\r\n` 不会进入结果；
- 控制台负责行编辑和退格；管道输入中的退格会由 `input` 过滤；
- 脚本模式下仍然从原始 stdin 读取，不会读脚本文件本身；
- `input` 是普通一等内置函数，可以保存和传递。

```text
let read = input;
read("> ")
```

示例：

```text
let name = input("Name: ");
print(format("hello {}", name));
```

## 文件读写

```text
read_file(path)          → 文件全部内容
write_file(path, text)   → null，覆盖写入
append_file(path, text)  → null，追加写入
file_exists(path)        → 1 / 0
```

示例：

```text
write_file("out.txt", "hello\n");
append_file("out.txt", "world\n");
print(read_file("out.txt"));

file_exists("out.txt")
```

规则：

- `read_file` 读取整个文件并返回字符串；空文件返回空字符串；
- `write_file` 创建或截断文件；
- `append_file` 创建或追加；
- 文件以二进制模式打开，不会自动转换 CRLF，也不会去掉末尾换行；
- 读写内容按 UTF-8 文本处理；
- `read_file` 遇到 NUL 字节会报错，避免内部 C 字符串截断；读取二进制请使用 `read_bytes`；
- 不会自动创建父目录；
- 文件操作失败时报告 `read_file: ...` / `write_file: ...` / `append_file: ...`；
- Windows/MinGW 下文件路径会转换为 UTF-16 后使用 `_wfopen`。

### 线程 Channel

```text
std.thread.Channel(capacity) -> channel
std.thread.unbounded_channel() -> channel
std.thread.Message.from(payload[, handles]) -> message

channel.send(message)
channel.try_send(message) -> bool
channel.recv([timeout_ms]) -> message | null
channel.try_recv() -> {"ok": bool, "closed": bool, "value": message?}
channel.close()

channel.closed
channel.capacity
channel.length
```

- `capacity >= 1`；`Channel(0)` 报错；
- 无界 Channel 使用 `std.thread.unbounded_channel()`，`capacity == -1`；
- Channel 只接收 `std.thread.Message`；
- `Message.from` 共享 payload（COW）；发送方后续写入会分离，不影响消息；
- `handles` 是字符串到线程 handle 的 dict，目前支持 `Channel`；
- `send` 在队列满时阻塞；`try_send` 不阻塞；
- `recv` 在队列空时阻塞；关闭且排空后返回 `null`；
- `recv(timeout_ms)` 超时抛错；
- `close()` 幂等，保留队列中未读消息；
- 当前 Channel 阻塞在 fiber 中通过 task 轮询挂起，非 fiber 上下文使用 OS 线程等待；

示例：

```skein
import std;

let ch = std.thread.Channel(2);
let msg = std.thread.Message.from({"x": 1});
ch.send(msg);

let r = ch.try_recv();
print(r["value"].payload["x"]);

ch.close();
```

### 线程池

```text
std.thread.pool(size, script[, options]) -> pool

pool.submit(value[, timeout_ms]) -> future
future.wait([timeout_ms]) -> {"ok": bool, "value"?: any, "error"?: error}
future.try_wait() -> {"ok": bool, "closed": bool, "canceled"?: bool, "value"?: message}
future.cancel() -> bool

pool.close()
pool.join()
pool.stats() -> dict
```

`options`：

```text
entry         默认 "handle"
queue         默认 1024；正整数或 "unbounded"
restart_limit 默认 0；worker 异常退出后的重启次数
worker_vm     默认 false；实验中，启用后 worker 使用 VM/JIT
```

worker 脚本导出 `handle(payload)`：

```skein
# worker.skein
func handle(payload) {
    return {"n": payload["n"] * 2};
}
```

使用：

```skein
import std;

let pool = std.thread.pool(2, "worker.skein");
let f1 = pool.submit({"n": 21});
let f2 = pool.submit({"n": 5});
let r1 = f1.wait();
let r2 = f2.wait();
print(r1["value"]["n"]);
print(r2["value"]["n"]);
pool.close();
pool.join();
```

规则：

- 每个 worker 是独立 runtime，加载一次脚本并循环处理 job；
- `handle` 抛错时返回 `{"ok": false, "error": ...}`，worker 继续；
- worker 返回值必须是 data-only；generator / userdata / 函数值等会返回错误；
- worker 发生未捕获的原生异常时，当前 job 的 future 返回 `error("thread.worker_crashed", ...)`；
- `restart_limit > 0` 时异常退出的 worker 会被重启，最多重启该次数；
- 所有 worker 死亡后，新的 `submit` 报 `thread.pool_failed`；
- `pool.submit` 会复制容器结构，string/bytes 共享底层 buffer；`submit(value, timeout_ms)` 超时抛 `thread.timeout`；
- `future.cancel()` 关闭结果通道；之后的 `wait()` 抛 `thread.cancelled`，`try_wait()` 返回 `canceled: true`；
- `pool.close()` 停止接收新 job，worker 排空后退出；
- `pool.join()` 等待 worker 线程结束；
- `stats()` 返回 `size/workers/ready/alive/dead/restarts/restart_limit/submitted/completed/failed/active/queued`；
- 不要在被导入模块的顶层创建 pool/spawn；当前会报
  `worker script parser is busy`，用于避免 parser 死锁。

### 单线程

```text
std.thread.spawn(script, message[, options]) -> thread

thread.join([timeout_ms]) -> {"ok": bool, "value"?: any, "error"?: error}
thread.detach()
thread.is_alive() -> bool
thread.cancel() -> bool
thread.id
```

`options`：

```text
entry     默认 "main"
```

线程入口接收一个 Message：

```skein
# thread_worker.skein
func main(ctx) {
    let signal = ctx.handles["signal"];
    signal.send(std.thread.Message.from({"x": ctx.payload["x"]}));
    return {"x": ctx.payload["x"]};
}
```

使用：

```skein
import std;

let ch = std.thread.Channel(1);
let msg = std.thread.Message.from(
    {"x": 5},
    {"signal": ch}
);

let t = std.thread.spawn("thread_worker.skein", msg);
let signal = ch.recv();
let result = t.join();

print(signal.payload["x"]);
print(result["ok"]);
print(result["value"]["x"]);
```

规则：

- `spawn` 等待脚本加载和入口解析完成后返回；
- 子线程拥有自己的 runtime、eval context、AST/function 缓存；
- 入口函数默认 `main`，接收一个 Message 参数；
- `join()` 返回 `{ok, value}` 或 `{ok:false, error}`；
- `detach()` 后线程自行运行和退出；
- `cancel()` 只设置协作取消标记，不能抢占执行中的线程；
- 子线程结果必须是 data-only 值。

### select / broadcast

```text
std.thread.select(channels[, timeout_ms]) -> dict
std.thread.try_broadcast(channels, message) -> dict
std.thread.broadcast(channels, message) -> null
```

`select` 返回：

```text
{"index": 0, "value": message}
{"index": 1, "closed": true}
{"index": -1, "timeout": true}
```

示例：

```skein
let a = std.thread.Channel(1);
let b = std.thread.Channel(1);
b.send(std.thread.Message.from({"v": "b"}));

let r = std.thread.select([a, b], 1000);
print(r["index"]);
print(r["value"].payload["v"]);
```

规则：

- 当前只支持 Channel；
- 多个 channel 同时就绪时使用轮转起点，避免总是选 0 号；
- 有数据优先，排空后才返回 closed；
- `timeout_ms = 0` 表示不超时；
- `try_broadcast` 返回：
  ```text
  {"sent": [0, 1], "full": [], "closed": []}
  ```
- `broadcast` 逐个阻塞发送；任一失败时抛错，已发送的不回滚。

### 能力查询

```text
std.thread.capabilities() -> dict
```

返回全局能力：

```text
{
  "schema": 1,
  "platform": "linux",
  "strategy": "auto",
  "strategies": ["dispatch", "reuse_port"],
  "reuse_port": true,
  "dispatch": true,
  "http1": true,
  "http2": true,
  "tls": true,
  "keep_alive": true,
  "stream_request_body": true,
  "stream_response": true,
  "websocket": false,
  "sse": false,
  "raw_socket": false
}
```

`ThreadServer.capabilities` 返回实际策略的能力，并包含同样的
`schema` / `platform` / `strategy` / `strategies` 字段。

HTTP 侧也可以按策略查询：

```skein
let caps = std.http.thread_capabilities({"strategy": "auto"});
let dispatch = std.http.thread_capabilities({"strategy": "dispatch"});
```

- `"auto"`：Linux 解析为 `reuse_port`，Windows 解析为 `dispatch`；
- `"reuse_port"`：不支持的平台会报错；
- 返回字段与 `std.thread.capabilities()` 一致。

`serve_threads` 的 `require` / `on_unsupported`：

```skein
let ts = std.http.serve_threads(
  s, 4, "worker.skein",
  {"require": ["http2"],
   "on_unsupported": "501"});
```

- `"error"`：缺失能力时启动失败；
- `"warn"`：输出警告并继续；
- `"501"`：启动成功，但请求返回 501。

### 带编码的文本文件

需要在 UTF-8 之外的文本编码之间读写时，使用：

```text
std.fs.read_text(path, encoding[, errors])       → string
std.fs.write_text(path, text, encoding[, errors]) → null
```

`encoding` 使用 `std.encoding` 支持的编码名，`errors` 支持 `strict`（默认）、
`replace`、`ignore`。

示例：

```text
import std;

std.fs.write_text("out-gbk.txt", "中国", "gbk");
std.fs.read_text("out-gbk.txt", "gbk")          // 中国

std.fs.write_text("out-utf16.txt", "中", "utf-16");
std.fs.read_text("out-utf16.txt", "utf-16")     // 中
```

规则：

- `read_text` 先读取字节，再按指定编码解码为 UTF-8 `string`；
- `write_text` 先把 `string` 编码成字节，再覆盖写入文件；
- `errors = "replace"` 时，解码用 `U+FFFD` 替换，编码用 `?` 替换；
- `errors = "ignore"` 时跳过非法序列或不可表示字符；
- 解码结果包含 `U+0000` 时报错；
- `utf-16` / `utf-32` 会按 BOM 规则处理；显式字节序版本不自动剥离 BOM；
- `utf-8` 不自动剥离 UTF-8 BOM，BOM 会作为 `U+FEFF` 保留；
- 原有的 `read_file` / `write_file` / `file_read` / `file_write` 仍然只按 UTF-8 字节处理；
- 文件句柄 API 不携带编码，需要其它编码时使用 `read_bytes` / `write_bytes` 或 `std.fs.read_text` / `write_text`。

### 文件句柄

需要多次操作同一个文件时，可以使用低层文件句柄：

```text
file_open(path, mode)       → file handle
file_read(handle[, n])      → string
file_write(handle, text)    → null
file_seek(handle, offset[, whence]) → null
file_tell(handle)           → int
file_eof(handle)            → 1 / 0
file_close(handle)          → null
file_flush(handle)          → null
```

`mode` 支持 `r`、`w`、`a`、`r+`、`w+`、`a+`，也接受带 `b`/`t` 的写法；内部统一按二进制模式打开。

示例：

```text
let h = file_open("data.txt", "w+");
file_write(h, "hello world");
file_seek(h, 0);
file_read(h, 5)
file_close(h)
```

- 文件句柄是不可打印底层指针的资源对象，打印显示为 `<file: path>`；
- 句柄复制会共享同一个底层文件，关闭其中一个会影响所有副本；
- `file_read(h)` 读取剩余全部内容，`file_read(h, n)` 最多读取 n 字节；`n = 0` 等价于读取全部；
- `file_seek` 的 `whence`：`0` 文件开头、`1` 当前位置、`2` 文件末尾；
- 关闭后继续读写会报 `file is closed`；
- `file_read` 读取到 NUL 字节会报错；读取二进制请使用 `file_read_bytes`；
- 最终未关闭的句柄会随进程退出关闭。

### 字符

`char` 表示一个 Unicode 标量值，使用单引号字面量：

```text
'a'
'你'
'\n'
'\u0041'
'\u{1F600}'
```

- `len('a') == 1`；
- 字符串下标和 `for ... in` 字符串返回 `char`；
- `int('A') == 65`，`str('A') == "A"`，`char(65) == 'A'`；
- `char` 可与字符串拼接：`'a' + "bc"`；
- `bytes_from_char(c)` / `char_from_bytes(b)` / `char(b)` 做 UTF-8 字节互转；
- JSON 编码为单字符字符串；
- 空、多字符、代理区、超过 `0x10FFFF` 报错。

### 字节值 byte

`byte` 表示 `0..255` 的字节值，没有字面量：

```text
byte(0)
byte(65)
byte(255)
```

- `int(byte)` 取回 `0..255`；
- `byte(int)` 超出范围报错；
- `byte` 可以作 dict key、`switch` 值；
- JSON 编码为整数；
- `bytes[i]` / `bytes_get` 仍返回 `int`。

### 二进制 buffer

文本字符串按 UTF-8 处理，不能保存任意字节。二进制数据使用独立的 `bytes` 类型：

```text
bytes_from_string(text)        → bytes
bytes_to_string(bytes)         → string
bytes_len(bytes)               → int
bytes_get(bytes, index)        → 0..255
bytes_slice(bytes, start[, length]) → bytes
bytes_concat(a, b)             → bytes
bytes_hex(bytes)               → 十六进制字符串
bytes_from_hex(text)           → bytes
bytes_to_int(bytes, size, byteorder[, offset[, signed]]) → int
int_to_bytes(value, size, byteorder) → bytes
read_bytes(path)               → bytes
write_bytes(path, bytes)       → null，覆盖写入
append_bytes(path, bytes)      → null，追加写入
file_read_bytes(handle[, n])   → bytes
file_write_bytes(handle, bytes) → null
bytes[index]                   → 0..255
bytes[index] = 0..255
```

示例：

```text
let b = bytes_from_hex("00ff10");
bytes_len(b)          → 3
bytes_hex(b)          → "00ff10"
bytes_get(b, 1)       → 255
bytes_hex(bytes_slice(b, 1, 2)) → "ff10"

let c = bytes_from_string("abc");
c[1] = 255;
bytes_hex(c)          → "61ff63"

bytes_to_int(bytes_from_hex("78563412"), 4, "little")
                      → 305419896
bytes_to_int(bytes_from_hex("78563412"), 4, "big")
                      → 2018915346
bytes_to_int(bytes_from_hex("ffff"), 2, "little", 0, 1)
                      → -1
bytes_hex(int_to_bytes(305419896, 4, "little"))
                      → "78563412"
bytes_hex(int_to_bytes(-1, 2, "little"))
                      → "ffff"
```

规则：

- `bytes` 可以保存任意字节，包括 `0x00`，长度由字节数决定；
- 直接打印 `bytes` 时显示为 `<bytes: N>`，不会把原始二进制写到终端；需要查看内容时使用 `bytes_hex`；
- `bytes_len`、`bytes_get`、`bytes_slice`、索引和 `file_seek` 都使用**字节下标**，不是 UTF-8 码点；
- 索引读取返回 `0..255` 的整数，索引赋值只接受 `0..255`；
- `bytes_slice` 的 `start` / `length` 不能为负数，越界会截断；
- `bytes_from_hex` 只接受偶数个十六进制数字，大小写均可；
- `bytes_to_int` 的 `size` 为 1..8，`byteorder` 支持 `"little"` / `"big"`（也接受 `"le"` / `"be"`）；
- `bytes_to_int` 的 `offset` 默认 0，`signed` 默认 0；`offset + size` 超出 buffer 会报错，不会静默截断；
- `bytes_to_int(..., signed = 1)` 按二补码解释；无符号 64 位值超过 `int64` 上限时会报错；
- `int_to_bytes` 的 `size` 为 1..8；值可以落在有符号下限到无符号上限之间，按低 `size` 字节编码；
- 负数写入使用二补码，例如 `int_to_bytes(-1, 2, "little")` 得到 `ff ff`；
- `bytes_to_string` 要求内容是合法 UTF-8 且不含 NUL，否则报错；
- `read_bytes` / `write_bytes` / `append_bytes` 不做文本 NUL 检查；
- `file_read_bytes(h)` 读取剩余全部内容，`file_read_bytes(h, n)` 最多读取 n 字节；
- `bytes` 是值类型，赋值和复制都会复制内容。

### 编码转换

设计说明见 [docs/encoding.md](docs/encoding.md)。

`std.encoding` 在 `bytes` 和 UTF-8 `string` 之间做显式转码：

```text
std.encoding.decode(bytes, encoding[, errors])  → string
std.encoding.encode(string, encoding[, errors]) → bytes
std.encoding.supports(encoding)                 → bool
```

支持的编码：

```text
utf-8  ascii  latin-1  windows-1252
utf-16le  utf-16be  utf-16
utf-32le  utf-32be  utf-32
gbk / cp936
gb18030
```

`errors` 支持：

```text
"strict"   默认，非法序列或不可表示字符报错
"replace"  decode 用 U+FFFD 替换；encode 用 "?" 替换
"ignore"   跳过非法序列或不可表示字符
```

示例：

```text
import std;

let s = std.encoding.decode(bytes_from_hex("d6d0b9fa"), "gbk");
print(s);                                      // 中国

print(bytes_hex(std.encoding.encode("中国", "gbk")));
                                               // d6d0b9fa

print(bytes_hex(std.encoding.encode("€", "gbk")));
                                               // 80

print(bytes_hex(std.encoding.encode("€", "gb18030")));
                                               // a2e3

std.encoding.supports("gbk")                   // true
std.encoding.supports("gb18030")               // true
```

规则：

- GBK 和 GB18030 使用内置码表，不依赖 iconv 或 Windows 代码页；
- `gbk` 的 `0x80` 解码为 `€`；`gb18030` 的 `€` 是 `A2 E3`；
- `std.encoding` 的 `utf-16` / `utf-32` 要求 BOM，显式字节序版本不自动处理 BOM；
- `utf-16` / `utf-32` 编码固定输出 BOM + 小端；
- 解码结果包含 `U+0000` 时始终报错，因为 `string` 不能保存内嵌 NUL；
- `bytes_to_string` 仍然是严格 UTF-8 的快捷方式，不参与 GBK / GB18030 转换。

### Unicode 归一化

设计说明见 [docs/unicode.md](docs/unicode.md)。

`std.unicode` 提供显式的 NFC / NFD / NFKC / NFKD 归一化：

```text
std.unicode.normalize(text, form)      → string
std.unicode.is_normalized(text, form)  → bool
std.unicode.version()                  → string
```

`form` 支持 `nfc`、`nfd`、`nfkc`、`nfkd`，大小写不敏感。底层使用 vendored
`utf8proc` 2.10.0，Unicode 版本为 16.0.0。

示例：

```text
import std;

let decomposed = std.encoding.decode(bytes_from_hex("65cc81"), "utf-8");
let composed = std.unicode.normalize(decomposed, "nfc");
bytes_hex(std.encoding.encode(composed, "utf-8"))  // c3a9

std.unicode.is_normalized(decomposed, "nfc")       // false
std.unicode.is_normalized(composed, "nfc")         // true

let circled = std.encoding.decode(bytes_from_hex("e291a0"), "utf-8");
std.unicode.normalize(circled, "nfkc")             // "1"
std.unicode.version()                              // "16.0.0"
```

规则：

- 归一化只在显式调用 `std.unicode.normalize` 时发生；
- 字符串比较、拼接、下标和 `len` 不会自动归一化；
- 标识符按 NFC 归一化，不做 NFKC；
- 输入必须是合法 UTF-8 `string`，输出也保持合法 UTF-8；
- 归一化不会产生内嵌 NUL。

### 正则表达式

```text
std.regex.match(pattern, text[, flags])       → bool（整串匹配）
std.regex.search(pattern, text[, flags])      → dict / null
std.regex.find_all(pattern, text[, flags[, limit]]) → list
std.regex.replace(pattern, text, replacement[, flags]) → string
std.regex.split(pattern, text[, flags])       → list
```

示例：

```text
import std;

std.regex.match("a+", "aaa")                  // true
std.regex.match("abc", "ABC", "i")            // true
std.regex.match(".", "你", "u")               // true
std.regex.match("\\p{L}+", "中文", "u")        // true

let m = std.regex.search("(a+)(b)", "xxaaabyy");
m["text"]                                     // aaab
m["start"]                                    // 2
m["groups"]                                   // [aaa, b]

let u = std.regex.search("(\\p{L}+)", "123中文456", "u");
u["text"]                                     // 中文
u["start"]                                    // 3（码点偏移）
u["end"]                                      // 5（码点偏移）

std.regex.replace("(a+)", "xxaaabyy", "[$1]") // xx[aaa]byy
std.regex.split(",", "a,b,c")                  // [a, b, c]
```

规则：

- 使用 PCRE2 正则语法，兼容常用 ECMAScript 写法；Unicode 码点转义使用 `\x{...}`（如 `\x{4f60}`），不是 `\uXXXX`；
- `flags` 支持 `i`（忽略大小写）、`m`（多行）和 `u`（Unicode 模式）；
- 不带 `u` 时按 UTF-8 字节匹配，`search` / `find_all` 的 `start` / `end` 是字节偏移；
- 带 `u` 时按 Unicode 码点匹配，支持 `\p{...}`、Unicode 字符类和 Unicode 大小写折叠，`start` / `end` 是码点偏移；
- `u` 模式要求模式和文本是合法 UTF-8；
- `replace` 使用 `$1` / `${1}` 形式引用捕获组；
- 非法正则会报运行时错误。

### 时间、随机数和 UUID

```text
std.time.now()                    → Unix 秒
std.time.now_ms()                 → Unix 毫秒
std.time.monotonic_ms()           → 进程内单调毫秒
std.time.utc_offset()             → 本地时区偏移秒数
std.time.format(ts[, format[, utc]]) → string
std.time.parse(text[, format[, utc]]) → int
std.time.sleep(ms)                → null

std.random.seed(n)
std.random.int(min, max)          → 闭区间整数
std.random.float()                → [0, 1)
std.random.bytes(n)               → bytes
std.random.choice(list)
std.random.shuffle(list)          → 新 list
std.random.uuid()                 → UUID v4 字符串
```

默认时间格式是 `%Y-%m-%d %H:%M:%S`，默认使用本地时间；第三个参数传 `true`
使用 UTC。

### Base64、URL 和哈希

```text
std.encoding.base64_encode(bytes) → string
std.encoding.base64_decode(string) → bytes
std.encoding.url_encode(string) → string
std.encoding.url_decode(string) → string

std.crypto.sha256(data)            → 小写 hex
std.crypto.hmac_sha256(key, data)  → 小写 hex
```

`sha256` / `hmac_sha256` 接受 `string` 或 `bytes`。`url_decode` 会把 `+`
当作空格，并校验解码结果是合法 UTF-8。

### 环境变量和进程捕获

```text
std.env.get(name[, default])
std.env.has(name)
std.env.set(name, value)
std.env.unset(name)
std.env.all()                     → dict

std.process.run(command[, options]) → dict
```

`std.process.run` 的 `options` 支持：

```text
stdin   string / bytes，默认空
cwd     工作目录
env     dict，覆盖子进程环境
```

返回：

```text
{"status": int, "stdout": bytes, "stderr": bytes}
```

示例：

```text
import std;

let r = std.process.run("printf hello");
print(bytes_to_string(r["stdout"]));

let r2 = std.process.run("cat", {"stdin": "abc"});
bytes_to_string(r2["stdout"])
```

注意：当前实现通过 shell 重定向捕获输出，不提供超时和流式读写。

### 目录、路径和压缩

```text
std.fs.listdir(path)              → list[string]
std.fs.mkdir(path[, recursive])
std.fs.remove(path)
std.fs.rename(old, new)
std.fs.stat(path)                 → dict
std.fs.cwd()
std.fs.chdir(path)

std.fs.path_join(parts...)
std.fs.path_basename(path)
std.fs.path_dirname(path)
std.fs.path_extname(path)
std.fs.path_abs(path)
std.fs.path_normalize(path)

std.compress.gzip(bytes)          → bytes
std.compress.gunzip(bytes)        → bytes
std.compress.zlib(bytes)          → bytes
std.compress.unzlib(bytes)        → bytes
std.compress.deflate(bytes)       → bytes
std.compress.inflate(bytes)       → bytes
```

`std.fs.stat` 返回：

```text
{"type": "file" / "dir" / "other", "size": int, "mtime": int}
```

压缩函数需要构建时启用 zlib；未启用时调用会报告 `zlib support is disabled`。

二进制文件读写：

```text
let h = file_open("data.bin", "r+b");
file_seek(h, 8);
file_write_bytes(h, bytes_from_hex("00ff"));
file_seek(h, 0);
bytes_hex(file_read_bytes(h, 16))
file_close(h)
```

读取固定宽度整数：

```text
let h = file_open("data.bin", "rb");
let magic = bytes_to_int(file_read_bytes(h, 4), 4, "big");
let length = bytes_to_int(file_read_bytes(h, 4), 4, "little");
file_close(h);
```

### 用 dict + 闭包封装文件对象

当前语言没有方法绑定，但可以用 dict + 闭包实现类似接口：

```text
func open_file(path) {
  let h = file_open(path, "r");

  func read(n) { return file_read(h, n); }
  func seek(n) { return file_seek(h, n); }
  func close() { return file_close(h); }

  return {"read": read, "seek": seek, "close": close};
}

let f = open_file("data.txt");
f["read"](10)
f["close"]()
```

底层文件资源使用通用的原生 userdata 生命周期管理；上层对象接口可以用语言本身扩展，不需要修改内置实现。

文件函数可以访问进程有权限的任意路径，只应在可信脚本中使用。

## 错误信息

错误输出统一包含位置、类别和源码片段：

```text
3:1: runtime error: division by zero
  1/0;
  ^
```

- 行号和列号从 `1` 开始；
- 列号按终端显示宽度计算，中文、日文、韩文等宽字符占两列；
- 类别分为 `lexical error`、`syntax error`、`runtime error`；
- 运行时错误指向实际出错的 AST 节点，而不是语句结束位置；
- 源码行下的 `^` 指向出错列；
- 源码必须是合法 UTF-8，非法 UTF-8 字节会报 `invalid UTF-8 byte 0x..`；
- 词法错误会显示字符和十六进制字节，例如 `unexpected character '`' (0x60)`；非法 Unicode 字符使用 `U+....` 表示；
- 语法错误会列出出错 token 和部分期望 token：

```text
1:9: syntax error: unexpected ';', expecting float, integer, identifier, ...
  let x = ;
          ^
```

函数调用中的运行时错误会附带调用栈：

```text
2:10: runtime error: division by zero
    return 10 / 0;
           ^
  in g() called from line 5
  in f() called from line 7
```

调用栈最多显示最近的 8 层。

### 错误恢复

- 运行时错误中止当前语句/函数，不回滚已经发生的副作用；
- 显式 `fail`、运行时错误和 `result.err` 可以被 `on error` 捕获；未捕获的 `fail` 在脚本顶层报错并停止；
- `on error` 对 `result.ok(v)` 解包为 `v`；
- 除零、越界、网络和 FFI 错误会转换成 `error`，并可由 `on error` 或 `std.result.capture` 捕获；
- 语法错误、内部不变量错误和控制流误用不会转换成 `result`；
- 函数出错时不返回部分结果；
- `let` 初始化失败不会创建绑定；普通赋值失败会保留旧值；
- 给 `const` 绑定重新赋值会报 `cannot assign to constant`，但修改其容器内容仍然允许；
- 脚本模式默认在第一条错误后停止；
- `-k` / `--keep-going` 让脚本继续执行下一条顶层语句，但最终退出码仍为 `1`；
- REPL 在错误后回到提示符，正常 EOF 退出码为 `0`；
- 语法错误和词法错误按行恢复；
- 同一行最多报告一条词法错误，并抑制由此产生的重复语法错误；
- 包加载失败是原子失败：`import` 不会绑定部分命名空间；
- `input()` 的 EOF 返回 `null`，不是错误；
- `std.process.command()` 的退出状态不是解释器错误，需要状态时使用 `std.process.command_status()`。

### 退出码

| 情况 | 退出码 |
|---|---:|
| 正常结束 | `0` |
| REPL 正常 EOF | `0` |
| 脚本语言错误（词法/语法/运行时/包） | `1` |
| 命令行参数错误 | `2` |
| 脚本文件无法打开 | `2` |
| 源文件编码声明冲突或转码失败 | `2` |
| 脚本被 Ctrl-C 中断 | `130` |

外部命令的退出状态通过 `std.process.command_status()` / `std.process.run()` 返回，不会直接成为解释器退出码。

## 目录结构

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
third_party/     llhttp、utf8proc
cmake/           构建辅助脚本
```

每层只向本层及以下层暴露 include 目录，配合 `tools/check_boundaries.py`
同时约束源码包含关系和静态库符号依赖。
