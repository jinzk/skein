# Skein 标准库指南

`import std;` 会加载内置标准库。本文件按模块介绍常用 API、参数、返回值和
行为细节；语言语法见 [SYNTAX.md](../SYNTAX.md)，入门内容见
[教程](tutorial.md)。

## 内置标准库 std

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

`tcp_listen("127.0.0.1", 0)` 后可以用 `local_addr` 获取实际监听端口。`tcp_listen` / `tcp_listen_task` 的 `options` 支持 `{"reuse_port": true}`（Linux/BSD）。

异步网络：

```text
let t = std.net.tcp_connect_task("127.0.0.1", 8080, 1000);
let socket = std.task.wait(t);
```

异步函数：

```text
tcp_connect_task(host, port[, timeout_ms[, family]]) → task
tcp_listen_task(host, port[, timeout_ms[, family[, options]]])  → task
accept_task(listener[, timeout_ms])                  → task
send_task(socket, data[, timeout_ms])                → task
recv_task(socket, size[, timeout_ms])                → task
resolve_task(host[, timeout_ms[, family]])           → task
shutdown_task(socket[, timeout_ms])                  → task
```

- task 完成值分别是 socket、listener、发送字节数、bytes、地址 list 或 null；
- `timeout_ms` 是异步操作自身的超时，不是 `std.task.wait` 的超时；
- `std.task.cancel(task)` 会取消底层网络操作；
- 可以同时启动多个 task，再用 `std.task.wait_all` 等待；
- `std.net.close` 仍是同步操作；
- `*_task` 是唯一名称；旧 `*_async` 名称已移除，文档统一使用 `*_task`。

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
std.task.scope(fn)            → value
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

`std.task.scope(fn)` 把 `fn` 放进一个子 task/fiber 中执行，并等待它以及所有未 detach 的子任务：

```skein
let v = std.task.scope(func() {
  let a = launch work(1);
  let b = launch work(2);
  return a.wait() + b.wait();
});
```

- `fn` 必须是可调用值，接收 0 个参数；
- scope 正常结束时等待所有未 detach 子任务；
- 子任务失败会取消同 scope 其他子任务并传播首个错误；
- `t.detach()` 后该任务不再由 scope 等待/取消；
- scope 可以嵌套；
- 顶层 scope 仍会等待自己的子任务。

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
- `std.net.*_task` 已经复用同一套 task 和事件循环。

`launch` 在当前 runtime 内启动新 fiber：

```text
import std;

func work(n) { return n + 1; }

let t = launch work(41);
println(t.wait());     // 42
println(t.done());     // true

func fetch() { std.task.sleep(10).wait(); return "ok"; }
let f = launch fetch();
println(f.wait());
```

Task 方法：

```text
t.wait([timeout_ms]) -> T
t.try_wait([timeout_ms]) -> result[T]
t.cancel() -> bool
t.done() -> bool
t.detach() -> task
t.detached() -> bool
```

结构化并发：

- `launch` 创建的任务默认挂到当前 task 的作用域；
- 当前 task/fiber 正常结束时，等待所有未 detach 的子任务；
- 子任务失败会取消同作用域其他子任务，并把首个错误传播给父任务；
- 父任务取消会递归取消未 detach 的子任务；
- `t.detach()` 后父作用域不再等待/取消它，但仍可 `wait()` / `cancel()`；
- 顶层脚本没有父 task，顶层 `launch` 默认 detached，需要显式 `wait()`；
- generator fiber 没有 task 父级，其中的 `launch` 也按 detached 处理。

当前限制：

- `launch` 是保留关键字；
- `launch` 推导为 `task[T]`；`func f(): void` 推导为 `task[void]`；
- 静态已知返回 `task[...]` 的调用不能 `launch`，会报
  `launch cannot start a call that already returns task[...]; call it directly and use .wait()`；
- 只有 `any`/未知 callee 才允许运行时 adopt/proxy，不会产生 `task[task[T]]`；
- `task[T]` 采用 invariant：`task[Dog]` 不能赋给 `task[Animal]`；
- 跨线程不能直接 `wait` task，继续使用 Channel/Message。

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

task-returning 客户端：

```text
let t = std.http.get_task("http://127.0.0.1:8080/api");
let r = t.wait();

let t2 = std.http.post_task("http://127.0.0.1:8080/items", "hello");
let r2 = t2.wait();

let t3 = std.http.request_task("DELETE", "http://127.0.0.1:8080/items/1");
t3.wait();
```

task-returning 客户端返回 task，可以 `.wait()`、`std.task.catch`、`std.task.wait` 或 `cancel`。

HTTPS 同步客户端：

```text
let r = std.http.get("https://example.com/", {
  "tls": {
    "ca_file": "/etc/ssl/certs/ca-certificates.crt",
    "cert_file": "client-cert.pem",
    "key_file": "client-key.pem"
  }
});
```

`cert_file` / `key_file` 用于 mTLS；同步和 task-returning 客户端都会透传。

HTTPS 同步服务端：

```text
let s = std.http.listen_tls("127.0.0.1", 8443, "cert.pem", "key.pem");
let req = std.http.accept(s, 5000);
std.http.respond(req, 200, {"Content-Type": "text/plain"}, "secure");
std.http.close(s);
```

mTLS 服务端可以在 `listen_tls` 的第 6 个参数传入选项：

```text
let s = std.http.listen_tls("127.0.0.1", 8443, "cert.pem", "key.pem", 0, {
  "client_ca_file": "ca.pem",
  "client_auth": "required"
});
```

`std.http.serve` / `std.http.serve_threads` / `std.http.serve_processes` 会沿用服务端
`client_ca_file` / `client_auth`。

SNI 多证书通过 `certs` 选项传入，形状与 `std.tls.server` 相同：

```text
let s = std.http.listen_tls("127.0.0.1", 8443, "default.pem", "default-key.pem", 0, {
  "certs": [
    {"server_name": "api.example.com", "cert_file": "api.pem",
     "key_file": "api-key.pem"},
    {"server_name": "*.example.com", "cert_file": "wild.pem",
     "key_file": "wild-key.pem"}
  ]
});
```

`certs` 与证书轮换、mTLS、ALPN 以及 `serve_threads` / `serve_processes` /
reuse_port 组合生效。

HTTP/1.1 服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);

let req = std.http.accept(s, 5000);

std.http.respond(req, 200, {
  "Content-Type": "text/plain"
}, "hello " + req["path"]);

std.http.close(s);
```

task-returning 服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);

let req = std.http.accept_task(s, 5000).wait();

std.http.respond_task(req, 200, {
  "Content-Type": "text/plain"
}, "hello " + req["path"]).wait();

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
std.websocket.connect_task(url[, options])    → task
std.websocket.accept(request)                  → ws
std.websocket.send(ws, data[, timeout_ms])     → bool
std.websocket.send_task(ws, data[, timeout_ms]) → task
std.websocket.receive(ws[, timeout_ms])        → message
std.websocket.receive_task(ws[, timeout_ms])  → task
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

服务端（`std.http.serve` / `serve_threads` reuse_port）：

```text
let req = std.http.accept(s, 5000);
let ws = std.websocket.accept(req);

let msg = std.websocket.receive(ws);
std.websocket.send(ws, "echo:" + msg["data"]);
std.websocket.close(ws);
```

在 `serve` / `serve_threads` reuse_port 的 handler 中，升级后应返回
`{"status": 101}`；服务器不会再写普通 HTTP 响应，并在 handler 返回时关闭连接。

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
- handler 可以是普通函数；handler 等待期间其他连接可以继续处理；
- 同一个连接可以处理多个请求；
- 每个客户端连接在独立 fiber 中处理，多个连接可以并发服务；
- 请求允许 keep-alive 时继续复用；
- `max_requests = 0` 表示总请求数不限制；
- `max_per_connection` 默认 100，`0` 表示不限制；
- `idle_timeout_ms` 默认 0，表示不设置连接空闲超时；
- 达到限制或空闲超时后发送 `Connection: close` 并关闭连接；
- 返回已处理请求数。

主进程 + 进程 worker 处理：

```text
std.http.serve_processes(server, workers, script[, options])
  -> ProcessServer

ProcessServer.stop()
ProcessServer.join([timeout_ms]) -> served
ProcessServer.shutdown([timeout_ms]) -> served
ProcessServer.stats() -> dict
ProcessServer.strategy -> string
ProcessServer.capabilities -> dict
ProcessServer.served -> int
```

`options`：

```text
strategy            "auto" / "dispatch" / "reuse_port"，默认 "auto"
entry               worker 入口，默认 "handle"
require             []，能力列表
on_unsupported      "error"（默认）/ "501" / "warn"
max_requests        默认 0
max_per_connection  默认 100
idle_timeout_ms     默认 0
worker_timeout_ms   默认 30000
max_connections     默认 0
max_header_bytes    默认 64 KiB
max_headers         默认 100
max_body_bytes      默认 16 MiB
```

- `strategy: "auto"` 在 Linux/BSD 选择 `reuse_port`，在 Windows 选择 `dispatch`；
- `dispatch`：accept、TLS、keep-alive 由主进程处理，每个请求通过内部
  bootstrap + hybrid IPC 发给进程 worker；
- `reuse_port`：父进程创建 POSIX 共享内存并启动多个子进程；每个子进程内部
  运行 `serve_threads(1, ..., strategy:"reuse_port", shared_state_path:...)`，
  独立 bind + listen + accept；
- `reuse_port` 的 `max_requests` / `served` 通过共享 mmap atomic 跨进程精确计数；
- `reuse_port` 的 `max_connections` 同样通过共享 mmap atomic 跨进程限制；
- 当前仅 Linux/BSD 支持进程 `reuse_port`；其他平台显式请求会报错；
- 用户脚本导出 `handle(req)`，不再手写 IPC 协议：
  ```skein
  func handle(req) {
      return {"status": 200, "headers": {}, "body": "ok"};
  }
  ```
- request dict 与 `serve_threads` 一致，包含
  `method/path/headers/body/http_major/http_minor/keep_alive/remote_addr/remote_port`；
  reuse_port 额外包含 `worker_id`；
- worker 返回值必须是 `{status, headers, body}`；`status` 必须在 `100..999`，
  否则按 502 处理；
- worker 错误返回 502，worker 超时返回 504，worker 不可用返回 503，
  unsupported 返回 501；
- `body_stream` 在进程 dispatch 下返回 501；
- `worker_timeout_ms` 控制单次 worker 调用超时，默认 30000，`0` 表示不超时；
- worker 调用使用异步等待，多个连接可以并发占用不同 worker；
- `stream_request_body` 仅 `reuse_port` 支持；dispatch 下 capability 为
  false，请求返回 501；
- `reuse_port` + TLS 支持 HTTP/2 ALPN；明文 `reuse_port` 支持 h2c prior knowledge；`reuse_port` 支持 WebSocket upgrade 与 SSE，dispatch 下返回 501；
- `server["strategy"]` / `server["platform"]` / `server["capabilities"]`
  会在启动后写入；
- `require` / `on_unsupported` 语义与 `serve_threads` 一致；
- `serve_processes` 立即返回 `ProcessServer`，`join([timeout_ms])` 等待所有 worker 结束并返回已处理请求数；超时抛 `ProcessServer.join timed out`，可 `stop()` 后重试；
- `shutdown([timeout_ms])` 先优雅停止，超时后强制关闭：dispatch 终止 worker 进程并关闭活动连接，reuse_port 直接终止子进程；完成后返回已处理请求数；

```skein
import std;

let s = std.http.listen("127.0.0.1", 8080);
let ps = std.http.serve_processes(
  s, 4, "worker.skein", {"max_requests": 1000});
print(ps.strategy);
let served = ps.join();
```

进程内线程池处理：

```text
std.http.serve_threads(server, workers, script[, options])
  -> ThreadServer

ThreadServer.stop()
ThreadServer.join([timeout_ms]) -> served
ThreadServer.shutdown([timeout_ms]) -> served
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
worker_vm          默认 false；启用后 VM/JIT（已验证并发 stress/TSAN）
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
- `reuse_port` 的 `max_connections` 使用共享原子计数，按所有 worker 的活跃连接全局限制；
- `reuse_port` + TLS 时通过 ALPN 同时支持 `h2` 和 `http/1.1`；明文 reuse_port 通过 prior knowledge 检测 h2c，同时保留 HTTP/1.1；
- `reuse_port` 支持 `stream_request_body`，worker 直接调用 `std.http.read_body(req)`，不跨线程传输 body；
- `dispatch` 不支持 `stream_request_body`，启动时报错；
- request dict 包含 `method/path/headers/body/http_major/http_minor/keep_alive/remote_addr/remote_port`，不包含 `socket`；
- reuse_port 的 request dict 额外包含 `worker_id`；
- worker 返回值必须是 `{status, headers, body}`；
- dispatch 下 worker 崩溃返回 502，worker 超时返回 504，worker 全部退出返回 503；
- `serve_threads` 立即返回 `ThreadServer`，`join([timeout_ms])` 等待服务结束并返回已处理请求数；超时抛 `ThreadServer.join timed out`，可 `stop()` 后重试；
- `shutdown([timeout_ms])` 先优雅停止，超时后关闭活动连接并设置强制停止标记；线程内正在执行的 handler 不能被抢占，若仍无法退出则抛 `ThreadServer.shutdown timed out`；
- capabilities 报告 `http1` / `http2` / `stream_request_body` / `reuse_port` / `strategies` / `platform`；
- `require` 在启动时检查能力：缺失时按 `on_unsupported` 处理；
  - `"error"`：启动失败，抛 `error(kind="http.unsupported")`；
  - `"warn"`：输出警告并继续；
  - `"501"`：启动成功，但请求返回 501；
- `reuse_port` 支持 WebSocket upgrade，handler 内通过 `std.websocket.accept(req)` 完成升级并收发；升级后返回 `{"status": 101}`，dispatch 下返回 501；
- `reuse_port` 支持 SSE；dispatch 下返回 501；
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
| HTTP/2 | 否 | TLS ALPN `h2` / 明文 h2c prior knowledge |
| TLS | 是 | 是 |
| keep-alive | 是 | 是 |
| `stream_request_body` | 否（capability false，请求 501；`require` 启动失败） | 是 |
| `stream_response` | 否 | 是 |
| WebSocket | 否（501） | 是（handler 内完成） |
| SSE | 否（501） | 是 |
| raw socket | 否 | 否 |
| `max_requests` | 单线程 | 全局 atomic |

平台支持：

- `reuse_port` 由 libuv `UV_TCP_REUSEPORT` 提供，支持 Linux / FreeBSD /
  DragonFly / Solaris / AIX；
- Windows、macOS 不支持 `reuse_port`：`serve_threads` / `serve_processes`
  显式指定时立即报 `reuse_port is not supported on this platform`，
  `strategy: "auto"` 会退回 `dispatch`；`thread_capabilities` 同样返回不支持；
- `stream_request_body` 仅在 `reuse_port` 下可用；dispatch 下
  capability 为 false，请求返回 501，`require` 含该能力时启动失败；
- Windows 可以使用 `serve_processes` 的 `dispatch` 进程池，但不能使用
  `reuse_port`；macOS 的 `reuse_port` 需要 macOS runner 验证，当前按同一
  structured unsupported 路径处理。

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
- `https://` 支持同步和 task-returning 客户端/服务端；
- 只支持 HTTP/1.1；HTTP/2 使用 `std.http2`；
- 同步和 task-returning 客户端共用 HTTP/1.1 keep-alive 连接池（空闲 30 秒后关闭）；
- 复用连接失败时，幂等方法（GET/HEAD/OPTIONS/TRACE）会自动用新连接重试一次；
- `std.http.serve` 支持服务端 keep-alive；
- `std.http.accept` / `respond` 仍是一次连接一个请求；
- 默认整体读取请求和响应 body；可用 `body_stream` / `on_body` 流式处理；
- 客户端 `options` 还支持 `upgrade`，用于发起 HTTP/1.1 协议升级；
- `max_body_bytes` 默认 16 MiB；
- 同名 header 在 dict 中只保留最后一个；
- `std.http` 客户端和服务端都提供同步和 task-returning 接口。

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
- 同步和 task-returning 客户端都支持。

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
- 同步和 task-returning 客户端都支持；
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
- `std.http.accept` / `accept_task` 也使用 server dict 中的限制；
- HTTP/2 使用 `max_body_bytes` / `max_connections`，header 限制通过 `settings.max_header_list_size`。

HTTP/2：

```text
std.http2.request(method, url[, options]) → response
std.http2.get(url[, options])           → response
std.http2.post(url[, body[, options]])  → response
std.http2.request_task(method, url[, options]) → task
std.http2.get_task(url[, options])             → task
std.http2.post_task(url[, body[, options]])    → task
std.http2.request_many(requests[, options])     → list
std.http2.request_many_task(requests[, options]) → task
std.http2.listen(host, port[, timeout_ms])      → server
std.http2.listen_tls(host, port, cert, key[, timeout_ms[, options]]) → server
std.http2.serve(server, handler[, max_requests[, max_per_connection[, idle_timeout_ms]]]) → int
std.http2.close(server)
```

客户端：

```text
let a = std.http2.get("https://example.com/", {
  "headers": {"Accept": "text/plain"},
  "tls": {"verify": true, "ca_file": "/etc/ssl/certs/ca-certificates.crt",
          "cert_file": "client-cert.pem", "key_file": "client-key.pem"}
});

print(str(a["status"]) + " " + a["protocol"]);
print(bytes_to_string(a["body"]));

let b = std.http2.post("https://example.com/echo", "payload");
let c = std.http2.get_task("https://example.com/async").wait();
```

批量并发请求（同一连接、多个 stream）：

```text
let responses = std.http2.request_many([
  {"url": "https://example.com/a"},
  {"url": "https://example.com/b", "method": "POST", "body": "payload"},
  {"url": "https://example.com/c", "method": "PUT", "body": "x"}
]);

let responses2 = std.http2.request_many_task([
  {"url": "https://example.com/d"},
  {"url": "https://example.com/e", "method": "DELETE"}
]).wait();
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
- 支持同步和 task-returning 客户端；
- 支持 h2c / TLS 服务端；
- flow control 由 nghttp2 管理：大 body 在 `initial_window_size` 较小时通过 WINDOW_UPDATE 推进；
- 服务端 draining 时会等待响应 body 发送完成后再发送 GOAWAY；
- 客户端允许在响应完成前收到 GOAWAY，只要对应 stream 已被处理；
- `stream_request_body` + `read_body` 与客户端 `on_body` / `body_stream` 形成双向背压；
- priority/weight 交给 nghttp2 默认调度，当前未暴露显式权重 API；
- 默认整体读取请求和响应 body；可用 `body_stream` / `on_body` 流式处理；
- `max_body_bytes` 默认 16 MiB；
- request dict 额外包含 `connection_id`，便于标识所属连接；
- 支持 `body_stream` 生成器流式发送请求 body；
- 支持 `on_body` 回调流式接收响应 body；
- 服务端响应 dict 支持 `body_stream` 流式发送；
- task-returning 服务端 handler 可在多个 stream 之间并发执行；
- 客户端 `options` 支持 `headers`、`body`、`body_stream`、`on_body`、`timeout_ms`、`max_body_bytes`、`tls`；
- 服务端 handler 接收 request dict，返回 response dict；
- 每个请求在独立 fiber 中执行，task-returning handler 可以跨 stream 并发；
- 每个客户端连接也在独立 fiber 中处理，多个连接可以并发服务；
- 达到 `max_requests` 后进入 graceful drain：停止接受新连接和新 stream，已启动的 handler 会继续完成并发送响应；
- 达到 `max_per_connection` 后关闭当前连接，未处理的 stream 由客户端按幂等规则重试；
- `max_requests = 0` 表示总请求数不限制；
- `max_per_connection` 默认 100，`0` 表示不限制；
- `idle_timeout_ms` 默认 0，表示不设置连接空闲超时；
- 同步和 task-returning 客户端共用 HTTP/2 连接池；
- 同步请求按顺序复用连接；
- task-returning 的 `get_task` / `post_task` / `request_task` 可以共享同一连接并并发执行；
- task 流式请求（`on_body` / `body_stream`）也可以共享同一连接；
- `request_many` / `request_many_task` 在单连接上并发提交多个 stream；
- 收到 GOAWAY 或共享连接失败时，task 幂等请求会换新连接重试一次；
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
std.tls.reused(tls_stream)                     → bool
std.tls.peer_subject(tls_stream)               → string
std.tls.server_name(tls_stream)                → string
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
verify          // bool，客户端默认 true
ca_file         // 客户端 CA 文件
cert_file       // 客户端证书（mTLS）
key_file        // 客户端私钥（mTLS）
client_ca_file  // 服务端用于校验客户端证书的 CA 文件
client_auth     // 服务端客户端证书策略：none（默认）/ optional / required
alpn            // 客户端/服务端 ALPN 协议列表
session_reuse   // 客户端会话复用，默认 true
certs           // 服务端 SNI 证书列表，见下节
```

mTLS：

```text
// 服务端要求客户端证书
let tls = std.tls.server(client, "cert.pem", "key.pem", {
  "client_ca_file": "ca.pem",
  "client_auth": "required"
});

// 客户端提供证书
let tls = std.tls.client(socket, "example.com", {
  "ca_file": "ca.pem",
  "cert_file": "client-cert.pem",
  "key_file": "client-key.pem"
});
```

- `client_auth = "optional"`：请求客户端证书，但客户端不提供时仍继续握手；
- `client_auth = "required"`：客户端必须提供可验证证书，否则握手失败；
- `client_auth` 为 `optional`/`required` 时必须提供 `client_ca_file`；
- 客户端 `cert_file` 与 `key_file` 必须成对提供；
- 证书校验失败会报告 `tls: certificate verification failed: ...`。

SNI 多证书（服务端）：

```text
let tls = std.tls.server(client, "default-cert.pem", "default-key.pem", {
  "certs": [
    {"server_name": "api.example.com", "cert_file": "api.pem",
     "key_file": "api-key.pem"},
    {"server_name": "*.example.com", "cert_file": "wild.pem",
     "key_file": "wild-key.pem"}
  ]
});
```

- 按 ClientHello 的 server name 选择证书：先精确匹配，再匹配单层通配符
  （`*.example.com` 只匹配一个标签）；
- 没有匹配项时使用默认证书（`cert_file` / `key_file` 参数）；
- `certs` 条目只覆盖证书/私钥，其余选项（`client_auth`、`client_ca_file`、
  `alpn`）继承默认上下文；
- 任意一个证书/私钥文件变化都会重建整个上下文组；重建失败保留旧证书；
- `std.http.listen_tls` / `std.tls.server` 的 `certs` 选项形状相同。

会话复用与内省：

```text
std.tls.reused(tls)          // 本次握手是否复用了会话
std.tls.peer_subject(tls)    // 对端证书 CN；无证书时为空字符串
std.tls.server_name(tls)     // 服务端收到的 SNI；客户端为空字符串
```

- 服务端默认启用会话缓存和 TLS ticket；
- 客户端进程内按 server name 缓存可复用会话，默认启用；可用
  `"session_reuse": false` 关闭；
- 会话缓存只保存 OpenSSL 判定为 resumable 的会话，最多 64 条；
- 跨进程 worker 之间不共享客户端会话缓存。

OCSP / 吊销检查：Skein 不实现自定义 OCSP 拉取或 stapling，证书吊销检查
遵循当前操作系统的信任库和 OpenSSL 默认策略；需要固定 CA 时通过 `ca_file`
或 `client_ca_file` 指定。

`std.tls.alpn(stream)` 返回握手后协商出的协议：

```text
std.tls.alpn(tls)   // "h2" / "http/1.1" / ""
```

规则：

- 默认校验证书和主机名；
- 测试环境可显式 `"verify": false`；
- TLS 流实现 `std.io` 接口；
- TLS 流支持 `std.io.read_task` / `write_task`；
- TLS 握手在 `std.tls.client/server` 包装时同步完成；
- 服务端 ALPN 会按服务端列表选择第一个客户端也支持的协议；
- `std.tls.server` 按证书/私钥（含 SNI 列表）和选项缓存 `SSL_CTX`；每次接受新连接时检查所有相关文件
  时间戳，变化时重建上下文，因此新连接使用新证书，旧连接继续使用握手时的旧证书；
- 证书/私钥重建失败时保留旧上下文并输出 `tls: certificate reload failed ...`
  诊断，不中断监听，也不重试直到文件时间戳再次变化；
- `serve_threads` / `serve_processes` / `reuse_port` 使用同一套证书上下文缓存，
  轮换对所有 worker 生效；

并发：`launch` / `task`（推荐）：

```text
import std;

func fetch(port) {
  let socket = std.net.tcp_connect_task("127.0.0.1", port, 1000).wait();
  let data = std.net.recv_task(socket, 4096, 1000).wait();
  std.net.close(socket);
  return data;
}

let a = launch fetch(8080);
let b = launch fetch(8081);

let da = a.wait();
let db = b.wait();
```

规则：

- 普通函数可以直接调用会挂起的 API，并在当前 fiber 上隐式挂起；
- `launch` 创建新 task，立即返回 `task[T]`，不阻塞、不自动 `.wait()`；
- `t.wait()` 等待并返回值；`std.task.wait(t)` 保留为兼容别名；
- 多个 task 可以先 `launch`，再分别 `wait()`，从而并发执行；
- task 内部错误会变成 task 失败，在 `wait()` 处抛出；
- `t.try_wait()` 返回 `result[T]`，用于不抛出地检查结果；
- `yield` 不用于 task；主动让出使用 `std.task.yield()`；
- 协程栈由 Boost.Context 提供，支持 Boost.Context 覆盖的平台。

`async` / `await` 已移除：

```text
// 旧写法不再解析
// async func old_fetch(port) { return await ...; }

// 新写法
func fetch(port) {
  return std.net.tcp_connect_task("127.0.0.1", port, 1000).wait();
}
let t = launch fetch(8080);
println(t.wait());
```

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

task 错误恢复：

```text
let t = std.net.tcp_connect_task("127.0.0.1", 1, 1000);
let r = std.task.catch(t).wait();

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
- 使用 `std.task.catch(task).wait()` 恢复 task 失败；
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
> [ffi.md](ffi.md)。

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

- 回调必须是同步函数；生成器在 `ffi.callback` 时直接拒绝；
- 回调不能在执行期间挂起（不要调用会挂起的 task 操作）；
- 只允许同步、同一 OS 线程、同一 fiber、且在活动的 `ffi.call` 范围内的回调；
- 回调里可以继续调用 `ffi.call`（受递归深度限制），也可以 `spawn` 后立即返回；
- 回调里不能 `yield`；
- 其他线程调用 `void` 回调时，回调会投递到 event loop，在脚本下次 pump
  （`std.task.wait` / `sleep`）时执行，C 侧立即返回默认值；
- 跨线程回调必须返回 `void`；需要返回值的回调必须在同步 `ffi.call` 内执行；
- 回调失败不会跨 C ABI 抛异常，错误在 `ffi.call` 返回后统一报告；
- C 侧保存函数指针时，用 `std.ffi.retain(lib, cb)` 把回调挂到库的生命周期，
  之后再释放脚本变量也仍然安全；
- C 调用后可以用 `std.ffi.errno()` / `std.ffi.set_errno(v)` 读写当前线程的 errno；
  建议在同一个函数或表达式中紧随 C 调用读取，顶层语句边界可能被解释器内部 I/O 改变。

需要从其他线程执行 C 调用并等待回调结果时，使用 `std.ffi.call_task`：

```text
import std;

let lib = std.ffi.open("./libalgo.so");
let Cmp = std.ffi.signature("i32", ["i32", "i32"]);
let cmp = std.ffi.callback(Cmp, func(a, b) { return a - b; });

// C 函数在 worker 线程执行；脚本阻塞，但事件循环继续运行，
// 回调会被投递回解释器线程并等待返回值。
std.ffi.call_task(lib, "compare", "i32",
                   [Cmp, "i32", "i32"], [cmp, 9, 4]);
```

- 比同步 `ffi.call` 多了线程切换，适合会阻塞或回调耗时较长的 C 调用；
- 回调仍然运行在解释器线程，可以安全访问脚本状态；
- 回调内可以继续 `ffi.call` / `ffi.call_task`（有递归深度限制）；
- `ffi.call_task` **不可取消，也没有超时**：当前 fiber 会一直等到 C 返回；
  每次调用创建一个 worker 线程，不适合高频短调用；
- C 调用运行在独立 worker 线程，看到的是进程环境和进程 cwd，不继承当前线程的
  `std.env` / `std.fs.cwd` 覆盖；需要的值请通过参数传入；
- 对可能永久阻塞或不受信任的 C 调用，建议放到外部进程，而不是依赖进程内取消。

旧名 `std.ffi.call_async` 已移除；请使用 `call_task`。

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

FFI 支持范围（完整契约见 [ffi.md](ffi.md)）：

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

let name = std.plugin.load("./libexample.so");
print(name);              // example.math

import example.math;
example.math.add(1, 2);
example.math.ANSWER;
```

- 插件只包含 `skein_module.h`，不链接解释器符号；
- `skein_api` 由 host 通过 `init` 传入，值以 opaque handle 传递；
- `skein_api` v1 追加了 `new_list` / `new_dict` / `as_userdata`；使用新字段前
  应检查 `size`，旧 host 的 ABI 表会更短；
- 静态绑定在启动时注册，动态插件通过 `std.plugin.load` 加载；
- `std.plugin.unload(name)` 只移除注册表；已存在的绑定函数/userdata 会继续持有
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
std.sys
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
std.plugin
std.entry
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
返回字典同时包含 `package`、`module`、`source`：
`package` 是完整包身份，`module` 是 Module 容器名（普通包和入口为空字符串），
`source` 是源文件路径。
`std.meta` 不返回值默认值，只提供 `has_default` 标记。字段信息的 `nullable`
对 `T?` 和包含 `null` 的 `union` 都为 `true`，两者显示为同一个可空类型。

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

`Router` 是可变引用句柄，复制后共享路由表；注册方法原地更新，不返回新 Router。支持 `:name` 参数、`*name` 通配、404/405、全局 middleware 和 返回 task 的 middleware。静态路由走精确表，动态路由走段级 trie：静态 dispatch 为 O(1)，动态 dispatch 与路由总数基本无关。`*name` 必须是最后一段，并且可以匹配 0 段。装饰器可用于包装 handler，例如 `@logged func user(req) {...}` 后再 `app.get("/users/:id", user)`。

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
std.io.read_task
std.io.write_task

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
std.process.run_command
std.process.spawn_command
std.process.run_entry
std.process.spawn_entry
std.process.run_function
std.process.spawn_function
std.process.result

std.type.kind_of
std.type.name
std.type.package_name
std.type.module_name
std.type.source

std.meta.type_info
std.meta.fields
std.meta.variants
std.meta.methods
std.meta.traits
std.meta.implements
std.meta.has_type

std.plugin
std.plugin.load
std.plugin.unload

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
std.net.tcp_connect_task
std.net.tcp_listen_task
std.net.accept_task
std.net.send_task
std.net.recv_task
std.net.resolve_task
std.net.shutdown_task

std.http.request
std.http.get
std.http.post
std.http.request_task
std.http.get_task
std.http.post_task
std.http.listen
std.http.listen_tls
std.http.accept
std.http.accept_task
std.http.respond
std.http.respond_task
std.http.serve
std.http.serve_processes
std.http.serve_threads
std.http.upgrade
std.http.cookies
std.http.read_body
std.http.close
std.http.thread_capabilities

std.http2.request
std.http2.get
std.http2.post
std.http2.request_task
std.http2.get_task
std.http2.post_task
std.http2.request_many
std.http2.request_many_task
std.http2.listen
std.http2.listen_tls
std.http2.serve
std.http2.close

std.tls.client
std.tls.server
std.tls.alpn
std.tls.reused
std.tls.peer_subject
std.tls.server_name

std.websocket.connect
std.websocket.connect_task
std.websocket.accept
std.websocket.send
std.websocket.send_task
std.websocket.receive
std.websocket.receive_task
std.websocket.ping
std.websocket.close

std.task.sleep
std.task.yield
std.task.scope
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
std.ffi.call_task
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

std.sys.args
std.sys.script
std.sys.executable
std.sys.exit
std.sys.is_main

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
std.fs.temp_dir
std.fs.remove_all
std.fs.copy
std.fs.which
std.fs.glob

std.process.run_command
std.process.spawn_command
std.process.run_entry
std.process.spawn_entry
std.process.run_function
std.process.spawn_function
std.process.result
std.process.wait
std.process.kill
std.process.close
std.process.pid
std.process.rss
std.process.children
std.process.tree_rss

std.worker.pool
std.worker.call
std.worker.call_bytes
std.worker.map
std.worker.submit
std.worker.submit_bytes
std.worker.wait
std.worker.shutdown
std.worker.stats

std.thread.pool
std.thread.spawn_script
std.thread.spawn_source
std.thread.spawn_function
std.entry.encode
std.thread.spawn_entry
std.thread.duplex
std.thread.spawn_script_with_channel
std.thread.spawn_script_with_duplex
std.thread.collect
std.thread.debug_counts
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
- `std.type` 提供 `kind_of`、`type_name`、`type_package`、`type_module_name`、`type_source`；
- `std.meta` 提供只读类型反射，查询字段、变体、方法和 trait；
- `std.net` 提供同步和 task 形式异步的 TCP/DNS，数据使用 `bytes`，底层由 libuv 驱动；
- `std.net` 的阻塞操作支持可选 `timeout_ms`，超时是运行时错误；
- `std.net` 的 `local_addr` / `peer_addr` 返回 `address`、`port`、`family` 三项 dict；
- `std.net.*_task` 返回 task，底层复用同一事件循环；
- `std.http` 提供基于 llhttp 的 HTTP/1.1 客户端和服务端；客户端支持 sync 和 `*_task`；
- `std.tls` 提供基于 OpenSSL 的同步 TLS 流包装（可选）；
- task 网络操作的 `timeout_ms` 是操作自身超时，`std.task.wait` 的超时是等待超时；
- `std.task` 提供原生 task 句柄、等待、批量等待、取消和完成状态；
- task 是协作式调度的，只有在 `wait` / `wait_all` 或同步网络等待运行事件循环时才推进；
- `std.task.wait` / `wait_all` 超时不会取消 task，剩余 task 需要显式 `cancel`；
- 普通函数可以挂起和恢复；可用 `launch` 把函数作为独立 task 启动；
- `yield` 用于生成器；生成器通过 `std.generator.next` 拉取值，`std.generator.close` 关闭并清理；
- `std.result` 提供独立 result 值和错误捕获；
- `std.error` 提供独立 error 值和 `kind` / `message` 访问；
- `defer` 是块级清理语句，按 LIFO 执行；
- Ctrl-C 或超时中断时，`accept` / `recv` 保留 socket，`connect` / `send` 关闭当前 socket；
- `std.ffi` 使用 libffi 调用 C ABI 函数，支持整数、浮点数、`cstring`、`bytes` 输入和 `buffer` 输出；
- `std.ffi` 的 `u64` / `usize` 返回值必须在脚本 `int` 范围内，否则报错；
- `bytes` 传给 C 的是调用期间的临时副本，C 侧不能保存指针；
- `buffer` 的容量和写入边界由调用方保证，C 侧越界写入会破坏内存；
- `std.ffi` 是可选模块，缺少 libffi 时不可用；
- 网络 I/O 和 FFI 错误是运行时错误，可以通过 `on error` 或 `std.result.capture` 捕获；
- FFI 可以执行任意本地代码，默认不建议对不受信任脚本开放；
- `std.net` 暂不支持 TLS、HTTP；
- task 和生成器使用 Boost.Context 提供的有栈协程；
- `std.result.capture` 捕获同步 `fail` 和运行时错误；task 使用 `std.task.catch`；
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

std.io.write_task(s, "hello", 1000).wait();

let data = std.io.read_task(s, 4096, 1000).wait();
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

let first = std.io.read_task(f, 4096).wait();
let rest = std.io.read_task(f, 4096).wait();

std.io.close(f);
```

```text
let f = std.fs.open("out.bin", "w+b");

let n = std.io.write_task(f, bytes_from_string("hello")).wait();
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
std.io.read_task(stream, max[, timeout_ms])  → task
std.io.write_task(stream, data[, timeout_ms]) → task
```

规则：

- `read` 最多读取 `max_bytes`；
- EOF 返回 `null`，不是空 `bytes`；
- `write` 返回实际写入字节数，`write_all` 循环写满；
- `close` 幂等，返回是否实际关闭；
- `read_all` 默认上限 16 MiB；
- `seek` / `tell` 只适用于可 seek 的流；
- `buffered` 包装读缓冲和写缓冲，`close` 会先 flush；
- `read_task` / `write_task` 返回 task，socket 和文件都支持；
- 文件异步 I/O 使用 libuv 线程池，不阻塞事件循环；
- 同一文件同时只允许一个异步操作；异步未完成时同步 read/write/seek/tell/close 会报错；
- `write_task` 返回实际写入字节数，可能部分写入；
- buffered 流会代理底层流的异步读写；
- 文件和 socket 共用 `std.io`，文件仍可用 `std.fs.seek` / `std.fs.tell`。

