# calc-demo

一个使用 Flex + Bison + C++17 实现的动态类型脚本语言解释器（REPL）。

完整语法参考见 [SYNTAX.md](SYNTAX.md)。

## 构建与运行

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/calc-demo
```

`std.net` 使用 libuv 作为同步网络后端，因此构建需要 libuv 开发文件；`async` / `await` / generator 使用 Boost.Context。例如 Debian/Ubuntu：

```bash
sudo apt install libuv1-dev libboost-context-dev
```

HTTP/1.1 解析器使用 vendored `llhttp`（`third_party/llhttp`），不需要额外的系统包。

TLS 是可选模块；启用时需要 OpenSSL 开发文件：

```bash
sudo apt install libssl-dev
```

没有 OpenSSL 时使用 `-DCALC_ENABLE_TLS=OFF`，`std.tls` 和 `https://` 不可用。

`std.ffi` 是可选模块，需要 libffi；找不到 libffi 时会自动禁用。可以显式关闭：

```bash
cmake -S . -B build -G Ninja -DCALC_ENABLE_FFI=OFF
```

`std.net` 提供同步 API 和 task 形式的异步 API；libuv 只在内部驱动 socket、DNS 和 `std.task`，不向脚本暴露事件循环对象。

运行测试：

```bash
ctest --test-dir build --output-on-failure
```

Windows / MinGW（64 位 MinGW-w64 GCC）：

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
build\calc-demo.exe
```

Windows 构建需要 MSYS2 中的 MinGW-w64 GCC、CMake、Bison、Flex、libuv 和 Boost.Context。平台相关代码集中在 `platform_win.cpp`：

- 设置控制台输入输出代码页为 UTF-8；
- 管道输入使用二进制模式，避免 CRLF / `0x1A` 转换；
- 使用 `SetConsoleCtrlHandler` 实现 Ctrl-C。

当前只面向 MinGW-w64/GCC，不包含 MSVC 专用适配。

脚本文件建议保存为 UTF-8；输入开头的 UTF-8 BOM 会被忽略。如果控制台无法切换到 UTF-8，中文下标、格式化和输出可能无法正常工作，需要在输入输出边界额外做代码页转换。

## 脚本模式与 shebang

可以直接把脚本文件作为第一个参数运行：

```bash
./build/calc-demo script.calc
```

也可以使用 shebang：

```text
#!/usr/bin/env calc-demo
print("hello");
```

```bash
chmod +x script.calc
./script.calc
```

使用 `/usr/bin/env calc-demo` 时，请确保 `calc-demo` 在 `PATH` 中；也可以直接写解释器的绝对路径。

脚本模式会：

- 跳过第一行的 `#!`；
- 跳过 UTF-8 BOM；
- 不打印 `> ` 提示符；
- 不打印顶层表达式的 `= result` 结果，需要输出时请使用 `print`；
- 忽略第一行之外的普通注释仍使用 `//`；
- 使用 `calc-demo -` 可以从 stdin 运行脚本模式；
- 脚本中出现词法、语法或运行时错误时，退出码为 `1`；
- 默认在第一处错误后停止脚本；使用 `-k` / `--keep-going` 可以继续执行后续顶层语句。

Windows/MinGW 没有原生 shebang，直接使用：

```bat
build\calc-demo.exe script.calc
```

shebang 行同样会被跳过。

交互模式：

- 只有 stdin 是终端时才显示 `> ` / `...> ` 提示符；管道输入不显示提示符；
- 使用 `calc-demo -i` 可以强制显示提示符；
- POSIX 终端支持方向键、Home/End、Delete、Ctrl-A/E/K/U/W、Ctrl-L；
- 支持上下方向键浏览历史；
- 历史默认保存到 `~/.calc_history`，可用 `CALC_HISTORY` 指定路径；
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
> :load demo.calc
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
      cc.calc
```

```text
import aa.bb.cc;
print(cc.value);
```

也支持入口文件名为 `index.calc`：

```text
aa/bb/cc/index.calc
```

如果目录中没有 `<name>.calc` 或 `index.calc`，但只有一个 `.calc` 文件，也会把它作为入口。

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
- 项目根通过从主脚本目录向上查找 `.calcroot` 文件确定；没有标记时使用主脚本目录；
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
tcp_listen(host, port[, timeout_ms[, family]])
accept(listener[, timeout_ms])
send(socket, data[, timeout_ms])
recv(socket, size[, timeout_ms])
resolve(host[, timeout_ms[, family]])
shutdown(socket[, timeout_ms])
```

`timeout_ms` 为 `0` 或省略时不设置超时；正数表示一次调用的总超时时间，检查精度约 10ms。
`family` 支持 `"any"`、`"ipv4"`、`"ipv6"`。
`"any"` 会使用解析结果中的第一个可绑定地址；需要同时监听 IPv4/IPv6 时应分别创建 listener。

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
```

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

请求 dict：

```text
method, path, headers, body, http_major, http_minor, socket
```

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
- 每个请求项可以独立流式发送请求 body 和流式接收响应 body。

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

模块：

```text
std.list
std.dict
std.json
std.string
std.bytes
std.math
std.io
std.fs
std.process
std.type
std.net
std.http
std.tls（可选）
std.task
std.generator
std.result
std.error
std.ffi（可选）
```

当前提供的函数：

```text
std.list.len
std.list.slice
std.list.contains
std.list.index
std.list.reverse
std.list.sort
std.list.append
std.list.map
std.list.filter
std.list.reduce

std.dict.len
std.dict.get
std.dict.has
std.dict.keys
std.dict.values
std.dict.items
std.dict.delete
std.dict.merge

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

std.io.print
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

std.fs.read_file
std.fs.write_file
std.fs.append_file
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

std.process.command
std.process.status
std.process.command_status

std.type.kind_of
std.type.name
std.type.module
std.type.source

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
std.http.upgrade
std.http.close

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
std.ffi.buffer
std.ffi.buffer_len
std.ffi.buffer_bytes
std.ffi.buffer_free

std.bytes.len
std.bytes.get
std.bytes.slice
std.bytes.concat
std.bytes.hex
std.bytes.from_hex
std.bytes.to_int
std.bytes.from_int
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
- `std.list.reverse` / `std.list.sort` / `std.list.append` 返回新 list，不修改参数；
- `std.list.sort` 只接受全数字或全字符串 list；
- `std.list.map` / `std.list.filter` / `std.list.reduce` 接收函数值，回调出错时中止；
- `std.list.reduce` 回调参数顺序是 `(accumulator, element)`；没有初始值时不能用于空 list；
- `std.dict.items` 返回 `[[key, value], ...]`；
- `std.dict.delete` 删除缺失键时返回原 dict 的副本；
- `std.dict.merge` 中右侧 dict 覆盖同名键；
- `std.json.parse` 把 JSON object 映射为 `dict`，key 一律是 `string`；参数可以是 `string` 或 `bytes`；
- `std.json.try_parse` 返回 `result`，非法 JSON 不会直接抛错；
- `std.json.stringify` 只接受 JSON 子集，dict key 必须是 `string`；
- JSON 整数在 int64 范围内映射为 `int`，否则映射为 `float`；重复 key 以最后一个为准；
- `std.json.stringify` 拒绝 `bytes`、`function`、`struct`、`userdata`、`result`、`error` 和 `NaN` / `Inf`；
- `std.string.join` 只接受字符串元素；
- `std.string.replace` 替换全部匹配，空查找串会报错；
- `std.string.contains` 返回 `1` / `0`；
- `std.string.repeat` 的次数不能为负数；
- `std.string.trim_start` / `trim_end` 只按 ASCII 空白裁剪；
- `std.math` 与全局数学函数使用同一套实现；
- `std.io` 提供 `print`、`format`、`input`，以及文件和 socket 通用的流式读写；
- `std.fs` 提供文件读写和文件句柄操作；
- `std.process` 提供命令执行和状态查询；
- `std.type` 提供 `kind_of`、`type_name`、`type_module`、`type_source`；
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
3. 环境变量 `CALC_PACKAGE_PATH`，按声明顺序。

例如：

```bash
./calc-demo -I ./lib --package-path ./vendor app/main.calc
CALC_PACKAGE_PATH=./lib:./vendor ./calc-demo app/main.calc
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

- 每个包使用一个 `<name>.calc` 或 `index.calc` 作为入口；
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

变量可以带类型标注：

```text
let name: string = "alice";
let count: int;

count = 3;          // 允许
count = "three";    // 错误：variable count expects int, got string
```

- 类型在运行时的初始化和重新赋值时检查，不做隐式转换；
- 支持 `any int float number bool string bytes list dict function builtin callable null userdata result error`；
- 带类型标注的 `let` 可以省略初始值，使用默认值：

```text
int / number  → 0
float         → 0.0
bool          → false
string        → ""
bytes         → 空 bytes
list          → []
dict          → {}
any / null    → null
```

- `function`、`builtin`、`callable`、`userdata` 必须提供初始值；
- `const` 也可以带类型，例如 `const x: int = 1;`；
- 类型标注只约束变量绑定，不约束容器内部元素，例如 `let a: list = [0]; a[0] = "x";` 仍允许；
- `for` 的初始化部分也支持类型标注：`for (let i: int = 0; i < 3; i++)`。

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

字典键一律使用下标访问：`p["x"]`。点号只用于 struct 字段以及 `std`/包命名空间，普通 dict 写 `p.x` 会报错。

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

`for ... in` 规则：

- 单变量：list 遍历元素，dict 遍历键；
- 双变量只能用于 dict，得到 `key, value`；
- list 需要下标时用 `range(len(list))` 或 C 风格 `for`；
- string 遍历 UTF-8 字符；bytes 遍历 `0..255` 的字节值；
- 迭代遍历对象在循环开始时求值一次，循环期间修改原对象不会改变本次遍历；
- 循环变量只在循环作用域内有效；
- 支持 `break` / `continue` / `return`。

`do ... while` 至少执行一次循环体；`while` 可以写在 `}` 的下一行，结尾分号可省略。

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
- 支持 `any`、`int`、`float`、`number`、`bool`、`string`、`bytes`、`list`、`dict`、`function`、`builtin`、`callable`、`null`、`userdata`；
- 调用时做运行时检查，不做隐式转换；
- 类型错误不会进入函数体；
- 逻辑运算和比较运算都返回 `bool`。

函数是一等值，可以保存到变量、列表或字典中，并通过任意表达式调用：

```text
let fs = [adder(1), adder(2)];
fs[0](10)
d["handler"](42)
adder(3)(4)
```

匿名函数使用同一个 `func` 关键字，省略函数名：

```text
let double = func(x) { return x * 2; };
double(21)

(func(x) { return x + 1; })(41)
```

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
- 仍然使用显式 `return`；
- 支持空参数和空函数体；
- 立即调用时建议给匿名函数加括号，例如 `(func(x) { ... })(1)`。

没有显式 `return` 的函数，以及 `print`，返回 `null`。`null` 是假值，可用于条件判断，也可以作为参数传递。

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

- `struct` 声明字段；所有字段都必须初始化，未知字段或重复字段会报错；
- 构造使用 `Point { x: 3, y: 4 }`，字段顺序不限；
- 字段可以读写：`p.x`、`p.x = 5`；
- 方法第一个参数必须是显式 `self`；
- 方法可以在 `struct` 内声明，也可以在同一个包的 `impl Type` 中补充；
- 方法只能通过实例调用：`p.length()`；
- 方法本身可以作为值提取，例如 `let f = p.length; f()`；
- 方法值绑定提取时的结构体快照，之后修改原结构体不会影响已绑定的方法；
- 第一版方法是只读的：方法内不能写 `self.x = ...`，需要修改时返回新结构体；
- 结构体是值语义，赋值和作为参数传递时使用 copy-on-write；
- 结构体类型可以用在变量和参数类型标注中：`let p: Point = Point { x: 1, y: 2 }`、`func f(p: Point)`；结构体类型没有默认值，变量必须初始化；
- 字段类型必须是已声明的类型；暂不支持递归/自引用结构体；
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

- 不支持继承、动态添加方法、静态方法、泛型或运算符重载。

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
- 接口是名义式的：只有显式写了 `impl Drawable for Point`，`Point` 才能传给 `Drawable`；
- 如果方法已经存在于 `impl Point` 中，可以只写 `impl Drawable for Point;` 做接口适配；
- trait 方法名不能与类型已有方法冲突；
- trait 实现和类型必须在同一个包中；
- 一个类型对同一个 trait 只能实现一次；
- 结构体和 trait 名称共享类型命名空间，不能在同一个包中重名；
- 类型名按包注册，一个包对应一个源文件；不同包可以定义同名结构体或 trait；
- 类型默认不跨包可见；`export struct` / `export trait` 后可以按包路径使用；
- 导入之后用 `aa::Point`、`lib::Drawable` 这样的限定名引用：

```text
// package aa
export struct P { x: int; }
export trait D { func d(self); }

// main
import aa;
let p: aa::P = aa.make();
p is aa::D

import aa as lib;
let q: lib::P = lib.make();
```

- 未导出的类型不能通过包路径引用，但它的值仍可作为普通值传递和方法调用。

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

内置函数：`sqrt`、`exp`、`log`、`abs`、`sin`、`cos`、`tan`、`asin`、`acos`、`atan`、`atan2`、`pow`、`floor`、`ceil`、`round`、`trunc`、`log2`、`log10`、`hypot`、`min`、`max`、`len`、`range`、`print`、`list`、`dict`、`int`、`float`、`bool`、`str`、`kind_of`、`type_name`、`type_module`、`type_source`、`format`、`command`、`command_status`、`read_file`、`write_file`、`append_file`、`file_exists`、`file_open`、`file_read`、`file_write`、`file_seek`、`file_tell`、`file_eof`、`file_close`、`file_flush`、`input`、`split`、`split_ws`、`trim`、`find`、`substr`、`bytes_from_string`、`bytes_to_string`、`bytes_len`、`bytes_get`、`bytes_slice`、`bytes_concat`、`bytes_hex`、`bytes_from_hex`、`bytes_to_int`、`int_to_bytes`、`read_bytes`、`write_bytes`、`append_bytes`、`file_read_bytes`、`file_write_bytes`。内置函数同样是一等值：

```text
let f = sqrt;
f(9)

func apply(fn, x) { return fn(x); }
apply(sqrt, 16)
```

函数调用会先按从左到右的顺序求值参数，再检查被调用者类型和参数个数，因此参数中的副作用总会执行。

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
- 花括号、方括号、圆括号内的换行会被自动忽略，支持多行表达式。
- 闭包捕获定义处的词法作用域；闭包环境会在程序退出时统一清理。
- 用户函数调用深度上限为 1000，超出时报告 `maximum recursion depth`。

## 类型与转换

- `int` 是 64 位有符号整数；超出 64 位的整数字面量按 `float` 解析，整数运算溢出会报 `integer overflow`。
- `int` 与 `float` 混合运算提升为 `float`，例如 `1 + 2.5` 得到 `3.5`；`1 == 1.0` 为真。超过 2^53 的整数与 float 比较仍按数学值精确判断，但转成 `float`（混合运算或 `float()`）时会丢失精度。
- 整数除法向零截断，取模结果符号随被除数：`-7 / 4 == -1`，`-7 % 4 == -3`，`7 % -4 == 3`。
- 位运算按 64 位进行，移位次数必须在 `0..63` 之间。
- `bool` 类型有 `true` / `false` 两个字面量；`bool(x)` 按真值规则转换。
- 真值规则：`null`、`false`、`0`、`0.0`、空字符串、空 bytes、空列表、空字典为假；其余值（包括 `true`、函数和内置函数）为真。
- `str(true)` 得到 `"true"`，`int(true)` 得到 `1`，`float(false)` 得到 `0.0`。
- `null` 是空值字面量，表示“没有值”：没有 `return` 的用户函数、`print` 和 `return;` 都返回 `null`。顶层 `null` 结果不会显示，但可以赋值、比较、传递，或通过 `print(null)` / `format("{}", null)` 输出。
- `&&`、`||`、`!` 返回 `bool`，并短路求值。
- `==` / `!=` 对所有类型有效，返回 `bool`；大小比较只支持数值之间或字符串之间，也返回 `bool`。
- 列表和字符串下标必须是 `int`，`1.0` 不会被隐式截断；字符串下标按 UTF-8 码点计数，`"你好"[0]` 得到 `你`。
- `len(x)` 支持字符串、bytes、list、dict：字符串按 UTF-8 码点计数，bytes 按字节数，list/dict 按元素数。
- `range(length)` / `range(start, length[, step])` 生成整数序列，第二参数是长度而不是结束值；`step` 不能为 0。
- 字典键按值相等判断，`1` 和 `1.0` 是同一个键；字面量中的重复键以最后一个为准。
- `list`、`dict` 和 `struct` 是值类型：赋值、传参和返回在语义上复制，但内部使用写时复制（COW），只读或复制时共享底层数据，真正修改时才分离。
- `method` 是绑定到结构体实例的方法值；当前版本捕获提取时的实例快照。
- `bytes` 和字符串目前仍然复制内容；函数值共享闭包环境，文件句柄等原生资源共享底层句柄。
- 显式转换：`int(x)`、`float(x)`、`bool(x)`、`str(x)`。`int` / `float` 接受整数、浮点数、布尔值或字符串；`bool` 按真值规则转换；`str` 接受任意值。
- 浮点数输出使用可往返的最短表示并保留 `.0`：`1.0` 显示为 `1.0`，`1.23456` 显示为 `1.23456`。
- 没有字符串与数字之间的隐式转换，`1 + "2"` 会报错；需要时请使用 `int()` / `float()` / `str()`。

## 对象生命周期

- 普通值由引用计数管理；`list` / `dict` 的 payload 会被别名共享，写入时才分离；
- 函数值复制时共享函数代码和闭包环境，文件句柄等原生资源共享底层句柄；
- 闭包按引用捕获定义处的作用域，因此局部变量会在闭包存活期间继续存在；
- 运行时会在顶层语句边界执行作用域级循环回收：每 64 个顶层语句或 scope 数量明显增长时，清理从活动作用域不可达的闭包环；
- 回收只在顶层语句边界进行。单个超长顶层语句内部创建的循环引用，可能要到该语句结束后才会被回收；
- 当前没有完整 GC；如果未来原生资源也能保存语言值，需要扩展循环回收的遍历规则。

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

`command(cmd[, env])` 执行一个 shell 命令，并返回其标准输出字符串：

```text
command("echo hi")                    → "hi"
command("echo $FOO", {"FOO": "bar"})  → "bar"
print(command("ls"))
```

- POSIX 使用 `/bin/sh -c`；
- Windows/MinGW 使用 `cmd.exe /C`；
- 可选的 `env` 是 dict，键和值必须是字符串；
- `env` 在父进程环境基础上覆盖，只对当前命令生效；
- 标准错误继承到当前进程，不会被捕获；
- 返回值末尾的 `\r` / `\n` 会被去掉；
- 命令无法启动时报告 `command failed to start`；
- 非零退出码不会自动报错，当前返回值只有标准输出，没有退出码。

如果只需要退出码，使用 `command_status(cmd[, env])`：

```text
command_status("exit 0")                     → 0
command_status("exit 3")                     → 3
command_status("exit $CODE", {"CODE": "3"})  → 3
```

`command_status` 会直接继承标准输入/输出/错误，因此命令输出会正常显示；它返回退出码整数。命令处理器无法启动时报告 `command_status failed to start`。

环境变量名不能为空，也不能包含 `=`。在 Windows cmd 中引用变量使用 `%FOO%`，POSIX shell 使用 `$FOO`。

Windows/MinGW 下 `command` 使用 `CreateProcessW` 传递 Unicode 命令行和 Unicode 环境块；`command` 捕获的 stdout 约定为 UTF-8，`command_status` 直接继承控制台输出，不做额外编码转换。

示例：

```text
let files = command("ls");
print(files);

let name = command("echo world");
format("hello {}", name)
```

`command` 是普通内置函数，因此也可以保存和传递：

```text
let run = command;
run("echo ok")
```

**安全提示**：`command()` 会执行任意 shell 命令，只应对可信输入使用。

Windows 下命令输出需要是 UTF-8 字节流，否则中文内容可能与解释器内部编码不一致。

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
- 类别分为 `lexical error`、`syntax error`、`runtime error`；
- 运行时错误指向实际出错的 AST 节点，而不是语句结束位置；
- 源码行下的 `^` 指向出错列；
- 词法错误会显示字符和十六进制字节，例如 `unexpected character '$' (0x24)`；
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
- `command()` 的退出状态不是解释器错误，需要状态时使用 `command_status()`。

### 退出码

| 情况 | 退出码 |
|---|---:|
| 正常结束 | `0` |
| REPL 正常 EOF | `0` |
| 脚本语言错误（词法/语法/运行时/包） | `1` |
| 命令行参数错误 | `2` |
| 脚本文件无法打开 | `2` |
| 脚本被 Ctrl-C 中断 | `130` |

外部命令的退出状态通过 `command_status()` 返回，不会直接成为解释器退出码。

## 目录结构

```text
tokens.l      Flex 词法分析
parser.y      Bison 语法分析
ast.*         AST 定义、函数代码共享与释放
diagnostics.* 错误位置、源码片段与调用栈
eval.*        树遍历求值
eval_context.* 每个求值上下文的 scope、控制流和错误状态
eval_helpers.h 求值器与内置函数共享的辅助函数
event_loop.*  libuv 事件循环与等待
http.*        std.http 客户端与服务端
http1_parser.* llhttp 封装与增量 HTTP/1.x 解析
tls.*         OpenSSL TLS 流包装
io.*          std.io 通用流式读写与辅助函数
fiber.*       Boost.Context 有栈协程、async/await 与生成器
generator.*   生成器迭代函数
result.*      result 值与错误捕获
error.*       error 值与 kind / message
builtins.*    内置函数实现
filetype.*    原生文件资源类型
format.*      Python 风格格式化
net.*         std.net TCP / DNS
ffi.*         可选的 std.ffi
package.*     包查找、加载、缓存与导入
repl.*        REPL 输入、历史、命令和上次结果
scopestack.*  词法作用域链与闭包环境
symbol.*      符号表
task.*        std.task 原生 task
valuetype.*   运行时值类型、深拷贝与释放
platform.*    POSIX / Windows 平台适配
tests/        自动化回归测试
```
