# Skein 总路线图

本文是当前唯一的总路线图。长期有效的技术设计见专项文档：

- [`package-manager.md`](package-manager.md)
- [`architecture.md`](architecture.md)
- [`bytecode.md`](bytecode.md)
- [`encoding.md`](encoding.md)
- [`unicode.md`](unicode.md)
- [`lsp.md`](lsp.md)
- [`key-semantics.md`](key-semantics.md)

已实现的专项计划（REPL、工具链、JIT、tuple 语义）中有长期价值的内容已合并到本文，其余实现细节不再单独维护，需要时可查 git 历史。

## 状态说明

- **已完成**：已经落地并有测试覆盖，不再作为主线计划。
- **进行中**：当前正在推进。
- **待实现**：设计已明确，可以排期。
- **待决策**：需要产品/架构选择后才能排期。
- **按需**：方向明确，但没有实际需求前不排期。
- **暂缓**：明确不作为近期目标。

---

## 1. 当前状态

### 1.1 主线概览（2026-11-23）

| 主线 | 状态 | 下一步 |
| --- | --- | --- |
| JSON API 生产化（M6，P1） | 基础完成 | 账户锁定、OpenTelemetry exporter、完整 stack 捕获按部署需求推进 |
| 包管理安全与运维（M1） | 核心完成 | macOS `flock` 验证暂缓；交叉编译按需 |
| 包生态与信任（M3） | 待决策 | 先确认是否做公共生态，再排 SumDB / proxy / 私有认证 |
| JIT / VM 与 GC（M4） | 优化冻结 | 等待 moving GC / AOT / 字节码缓存决策；已完成记录见第 3 节 |
| 长尾能力与体验（M5） | 按需 | 旧编码、HTTP/2 CONNECT、REPL / LSP 体验、LevelDB / RocksDB |
| `std.cache`（M7）/ `std.kv` | 已完成 | 维护；Valkey 独立 CI 服务可按需补 |
| FFI 平台 CI / fuzz | 按需 | Windows / 32 位 / ARM 和 fuzz，见 `architecture.md`；不阻塞 P1 |

最近完成：

- 服务 CI：PostgreSQL / MySQL / Valkey 集成服务与全量 ctest
- RBAC / 角色、权限 middleware、W3C trace context 与访问日志 trace 字段
- `std.cache` Redis / Valkey 客户端与共享 Session / 限流 store
- SQLite 单文件 `std.kv`
- 启动 profiling、AST arena、内存输入 fast path

### 1.2 推荐执行顺序

1. P1 JSON API 基础已完成；账户锁定、外部 OpenTelemetry exporter 和完整
   stack 捕获按实际部署需求推进。
2. M1 只剩 macOS `flock` 验证（暂缓）和交叉编译（按需）。
3. 先确认公共包生态目标，再决定 M3 的 SumDB / proxy 范围。
4. M4 的 GC root 审计、热点 opcode / frame 分配、字符串拼接、
   启动解析优化和 JIT 默认启用已完成；剩余是 moving GC / AOT / 字节码缓存
   的决策，以及移动 GC 决策后才会启动的 per-pc liveness / OSR / deopt。
5. 测试稳定性已完成：服务端测试改为子进程内绑定端口 0 并回读实际端口；
   `thread_bench` 门槛使用多次重复中的最好成绩。
6. 长尾能力按实际需求触发。

---

## 2. 主线与里程碑

### 2.1 JSON API 生产化（M6，P1；基础完成）

目标：只做 JSON API 后端，不做 SSR、静态文件、WebSocket / SSE、multipart
和模板引擎。

已确认决策：

- 认证同时支持 Cookie Session 与 Bearer Token；
- 密码哈希使用 Argon2id，vendor `libargon2`，新增 `std.password`；
- 多 worker 的 session / 限流首版使用内存 store，并保留可插拔存储接口；
  生产多进程部署使用 `std.cache` 共享 store；
- 数据库支持 SQLite、PostgreSQL、MySQL 和 MariaDB；四个驱动都通过
  `std.db` 统一 API，MySQL / MariaDB 共用一个后端实现；
- MySQL / MariaDB 优先使用 MariaDB Connector/C，兼容 libmysqlclient；
  `contrib/mysql` 只保留为 native module ABI 示例；
- Metrics 先输出 Prometheus 文本格式，OpenTelemetry 后续评估。

当前状态：P1-0、认证 / 会话基础、限流 / 安全、可观测性基础与 P1-D 数据库
（SQLite / PostgreSQL / MySQL / MariaDB、migration、连接池）已完成。
`std.cache` 已提供 Redis / Valkey 客户端并接入共享 Session / 限流 store；
单进程和多进程生产部署路径均已具备。

范围与状态：

1. P1-0 API 基础设施（已完成）
   - `query` / `cookies` / 多值 header / `request_id` / `client_ip`
     （HTTP/1.1 + HTTP/2 + thread/process worker 传递）
   - 统一 `json_response` / `json_error`
   - `route.use_after` 响应后处理与 JSON 错误边界
   - 请求级超时（task handler 超时；同步 CPU-bound handler 无法安全抢占，
     文档标注限制）
2. P1-A 认证与会话（基础完成）
   - Cookie 构造、内存 Session store、CSRF、HMAC token
   - Argon2id 密码哈希（vendor `libargon2` + `std.password`）
   - Cookie Session 与 Bearer Token 共用用户和权限模型
   - 标准 HS256 JWT、`require_bearer`、`authenticate` 与 401/403 helper
   - 多 worker 共享 Session store（通过 `std.cache`）
   - 完整权限/角色 middleware（`auth.require_roles` /
     `auth.require_permissions`）
3. P1-B 限流与安全（基础完成）
   - CORS before/after middleware、安全响应头、可信代理客户端 IP
   - IP / token / 用户 / 路由维度固定窗口限流
   - 登录失败保护可用限流组合实现；账户级锁定策略按需
   - 请求大小与超时限制
   - 多 worker 共享限流 store（通过 `std.cache`）
4. P1-C 可观测性（基础完成）
   - 结构化访问日志、JSON 错误边界与 request id 关联
   - `std.metrics`、Prometheus `/metrics`
   - `/healthz`、`/readyz`
   - W3C trace context、`X-Trace-Id`、`web.error_log`
   - 外部 OpenTelemetry exporter / 完整 stack 捕获按需
5. P1-D 数据库（已完成）
   - SQLite 驱动、参数化查询、事务（`std.db` 可选构建）
   - PostgreSQL 驱动：URI、`$n` 参数、基础类型映射、事务
   - MySQL / MariaDB 驱动：
     - `mysql://` / `mariadb://` URI，共用 `db_internal` 后端接口
     - prepared statement 参数绑定、`?` 参数占位
     - `changes` 来自 affected rows，`last_insert_rowid` 来自
       `mysql_insert_id`
     - integer / float / blob / null / text 显式映射；decimal / date /
       time / json 按 string 返回
   - migration：版本表、forward migration、事务回滚、幂等执行
   - 连接池：`std.db.pool`、acquire/release、max、stats、close
   - 暂不做完整 ORM

验收：

- 所有响应带 request id；handler 错误统一返回 JSON（已满足）
- 登录、登出、token 过期、CSRF、密码哈希可验证（已满足）
- 限流返回 429 与 `Retry-After`，伪造代理头不能绕过（已满足）
- 每个请求有一条结构化日志，`/metrics` 可被 Prometheus 抓取（已满足）
- SQLite、PostgreSQL、MySQL / MariaDB 的基础查询、事务、类型映射、
  migration 和连接池测试已通过

不包含：SSR、HTML 模板、静态文件服务器、WebSocket / SSE、multipart、
GraphQL、完整 ORM、OAuth 第三方登录。

### 2.2 包生态与信任（M3；待决策）

前提：确认是否要做公共包生态。

范围：

1. SumDB / 首次下载信任
2. Go 风格 proxy
3. `direct` / `off` 回退
4. 私有模块认证
5. `mod graph` / `mod why`
6. JSON 输出
7. `retract` / `exclude`

验收：

- 首次下载不只信任镜像
- 代理不可信时仍可验证
- CI 可输出机器可读依赖报告
- 可追踪模块被引入的原因

### 2.3 包管理安全与运维（M1；核心完成）

目标：让包管理在长期运行服务、并发 CI 和 native 缓存场景下安全可运维。

范围与状态：

1. 运行时模块加载持有缓存共享锁（已完成）
2. `mod clean` 与运行中脚本协调（已完成）
3. Native 产物完整性（已完成）：
   - 构建时写入 source/artifact hash、编译器、编译器版本
   - 写入构建 flags 和规范化构建命令 hash
   - 加载前校验 source/artifact hash
   - 编译器可用时检测版本变化
   - GCC/Clang 路径映射、MinGW deterministic PE、MSVC `/Brepro`
   - 相同输入连续构建产物字节一致
4. `mod prune` 安全语义（已完成）：
   - 默认只列出未引用版本
   - 显式 `--all` 才删除
   - 同步清理 native `.build` 产物
5. 锁超时与等待诊断（已完成）：
   - `SKEIN_LOCK_TIMEOUT_MS`
   - 等待提示和超时错误
6. Linux/macOS `flock` 并发测试（Linux 通过 WSL 全量 35/35；macOS 暂缓）

验收：

- 运行中的服务不会被 `mod clean` 删除依赖
- native 缓存被篡改或编译器升级后不会静默加载旧产物
- `mod prune` 有明确安全边界
- 多进程 `get` / `build` / `clean` 压力测试通过

### 2.4 JIT / VM 性能与 GC（M4；优化冻结）

前提：先决定 moving GC、AOT、字节码序列化是否在目标范围内。

当前状态：

- JIT 运行时默认启用；`SKEIN_JIT=0` 可完全关闭
- 测试基线：Windows JIT 35/35、Windows 非 JIT 33/33、WSL 35/35，0 警告
- JIT 覆盖率与性能工作自 2026-11-20 起冻结，遇到具体场景再重启
- 已完成：帧分配 / 热点 opcode、GC root 审计、字符串增量拼接、
  启动解析优化（详见第 3 节）

明确保留 VM、不再主动实现：

- 顶层 `DECLARE_PACKAGE` / `DECLARE_STRUCT` / `DECLARE_ENUM`
- 顶层 handler / defer，以及 handler + defer 组合
- variadic / generator 等边缘函数
- OSR / deopt、per-pc liveness

理由：声明与顶层 handler/defer 都是一次性元数据 / 初始化操作，没有热路径；
JIT 编译成本高于执行收益，且顶层 `global scope` 与 handler 记账更容易引入
语义回归。

若暂不做 moving GC：

- 只做 GC root 边界审计（已完成）
- 继续优化 frame 分配与热点 opcode（已完成）
- per-pc liveness 不阻塞主线

重启条件（满足其一）：

- 真实负载的 p50/p95 明确退化，且 profiling 指向 JIT 覆盖或生成代码；
- 具体程序的热点落在只有 OSR 才能覆盖的 VM 帧；
- 内存 / 延迟成为明确需求；
- 决定引入精确 / 移动 GC 或类型特化，需要 per-pc liveness / deopt。

重启时先量化再写代码：

```bash
SKEIN_VM_STATS=1 SKEIN_JIT_DEBUG=1 ./build/skein <script>
```

关注 `[vm-stats] jit compiled/declined`、`jit declines` 原因、
`[jit] unsupported op` / `reject` 与 `statements compiled`，再对照第 3.5 节
的 JIT 覆盖与缺口决定是否值得投入。

重启时评估：

- per-pc liveness、safepoint 表
- frame reuse / 零帧分配
- OSR / deopt（Handler JIT 已完成）
- 类型特化 / unboxed int / 单态字段与调用缓存内联
- 如仍有收益，再评估 tracing
- asmjit `x86::Compiler` 的 prologue / regalloc 能力

验收：

- `SKEIN_GC_STRESS` 下无漏根
- JIT/VM 差分测试稳定
- 有明确性能回归门槛

### 2.5 长尾能力与体验（M5；按需）

能力：

- GB18030-2022
- Big5 / Shift_JIS / EUC-JP 等旧编码后端
- Unicode 完整大小写映射与 case folding（已完成）/ grapheme cluster
- HTTP/2 extended CONNECT
- vendor / workspace
- AOT / 字节码序列化、预编译 stdlib、字节码 ABI 冻结
- `std.kv`：SQLite 单文件默认后端已完成；LevelDB / RocksDB 仅作为可选
  目录后端，暂不实施，不放进 `std.db` 或 `std.cache`

REPL / 工具链体验：

- 可配置缩进宽度
- 同一行连续多个闭合符逐级回退
- 语义高亮：类型、函数、builtin
- 括号匹配
- 签名提示 / hover
- Ctrl-R 历史搜索
- 模糊补全排序
- 补全候选弹窗 / 分栏
- 多行历史条目

---

## 3. 已完成基线与技术记录

### 3.1 语言与运行时

- 词法、语法、类型检查、AST/VM 双后端
- `pair` / `tuple`：字面量要求 `(a, b)`，`(a, b)` 可用于 let/const、
  函数参数和 match；带注解的值在运行时校验不可变性与 arity
- `dict` / `set` 支持 float key：NaN 拒绝，`-0.0` 归一化，整数值 float
  与同值 int 等价
- 10 种复合赋值运算符；静态检查按运算符推导结果类型，JIT 覆盖
  param / index 目标
- task / generator、线程、Channel
- `wait e` task 前缀等待语法：等价于 `e.wait()`，支持 `wait launch f()`；
  `.` 后的关键字与内置名按普通成员名解析
- `launch` 遵循 `.` 的优先级，作用于完整调用链；`launch f().wait()` 解析为
  `launch (f().wait())`，不再隐式下钻到 `f()`
- HTTP/1.1、HTTP/2、WebSocket、TLS、FFI
- Unicode 标识符、NFC、归一化 quick-check、完整大小写映射与折叠、
  常见源文件编码
- GBK / GB18030-2005 编解码
- `error` 值保留抛出点位置与调用栈，支持 `std.error.location` /
  `trace` / `format_trace`
- `std.log` 结构化日志：级别过滤、text/JSON、stderr/stdout/file、
  自定义 writer
- `std.kv` 嵌入式 KV：SQLite 单文件默认后端，BLOB key/value、prefix scan、
  count / clear

### 3.2 工具链、REPL 与 LSP

工具链：

- `skein lint`、`skein fmt`、`skein lsp --stdio`
- VS Code / Vim / Neovim 接入
- diagnostics / completion / hover / definition / document symbol 基础能力
- GitHub Actions CI：Valkey / PostgreSQL / MySQL 集成服务与全量 ctest

REPL：

- 历史、行编辑、Windows 控制台编辑
- 词法高亮
- Tab 补全：关键字 / REPL 命令 / 全局符号 / `std.*`
- 自动缩进、闭合符回退

REPL / LSP 体验统一（M2，已完成）：

- 共享 `completion_index`
- REPL struct 字段补全
- REPL `:load` 路径补全
- 长行水平滚动
- 运算符/逗号 continuation 缩进
- LSP incremental sync / debounce
- resolution-only import 校验（基于 `skein.mod` + 缓存 manifest）

### 3.3 包管理

- `skein.mod` / `skein.sum`
- `skein get` / `skein build`
- MVS、菱形依赖、`/vN`
- checksum 硬校验、`--force`
- 项目锁、缓存锁、模块锁
- `mod clean` / `mod verify` / `mod download` / `mod tidy`
- `get --offline`
- 原子写入 `skein.mod` / `skein.sum`
- 运行时模块加载持有缓存共享锁

### 3.4 数据库、缓存与 KV

- `std.db`：SQLite / PostgreSQL / MySQL / MariaDB、migration、连接池
- `std.cache`（M7，已完成）：
  - 独立 Redis / Valkey 客户端，不并入 `std.db`，避免与 SQL 事务、
    结果集、参数绑定模型混合
  - `cache.drivers()`、`cache.open("redis://...")` /
    `open("valkey://...")` / `rediss://` / `valkeys://`
  - `get` / `set` / `delete` / `exists` / `incr` / `expire` / `ttl` /
    `pttl` / `incr_window` / `ping` / `close`；`set` 支持 TTL、NX、XX
  - 值支持 string / bytes，保持二进制安全
  - AUTH、TLS、DB index、连接 / 读写超时；单句柄串行访问
  - 纯 Skein 内嵌包 `lib/std/cache.skein`，RESP2 基于 `std.net` /
    `std.tls`，不引入外部客户端库或额外 CMake 开关
  - Session store 与限流共享 store 已接入；默认仍是进程内 store
  - Redis 8.0 集成覆盖协议、二进制值、TTL、NX/XX、AUTH；Valkey 使用同一
    RESP2 协议，独立 CI 服务按需补
  - 默认测试在未设置 `SKEIN_TEST_REDIS_DSN` 时跳过外部服务，集成测试
    显式启用
- `std.kv`：SQLite 单文件、BLOB key/value、prefix scan、count / clear

### 3.5 JIT / VM / Native 基线

- baseline JIT、Win64 ABI、GC root map 第一版
- `SKEIN_GC_STRESS`、尾调用 trampoline
- benchmark 改为进程内稳态计时（`SKEIN_BENCH_MS`），排除启动和 JIT 编译
- 函数调用帧不再复制函数名/源路径（改为 intern 指针）
- JIT 在 Linux 与 Windows 上收益一致（fib 约 5x，overall 1.5–3.5x）
- `obj.method(args)` 使用 `CALL_METHOD` 快路径，VM 的 method 调用已快于 AST
- `std.list.builder` / `std.string.builder`，以及
  `xs = std.list.append(xs, v)` 的 COW 原地增长
- JIT 运行时默认启用；`SKEIN_JIT_TOP_LEVEL=0` 关闭顶层语句 JIT；
  `SKEIN_JIT_THRESHOLD` 默认 100 次调用
- 含回边（循环）的函数在首次调用即编译；`SKEIN_JIT_LOOPS=0` 可关闭
- JIT 支持闭包 / upvalue、名字模式局部变量、`for-in` 迭代器、enum、
  `fail`、`on error` handler、`defer`、常规顶层语句（`IMPORT` /
  `BIND_FUNC` / 循环 / 调用）、builtin 回调（map/filter/sort/reduce/
  compose/task）；顶层声明与 handler + defer 组合仍回退 VM
- `std.ffi`、plugin async ABI v2、source-native module MVP、
  `SKEIN_ALLOW_NATIVE`；check-only 模式下禁止加载/执行 native

### 3.6 热点 opcode / frame 分配（已完成）

VM fast frame 的每调用开销：

- `proto_can_frame` 从“每次调用扫描整段字节码”改为编译器写入的
  `fast_frame_blocked` 标志
- 纯 slot/param 帧不再分配函数级 scope；块作用域在没有名字、闭包、
  handler、defer、pattern、iterator 时也不物化（`PushScope`/`PopScope`
  成对跳过）
- `LoadGlobal` 的模块名回退改为从函数定义时的 closure 作用域解析，
  不再依赖动态调用链，从而允许更多帧省略 scope；全局/名字查找增加
  frame-local IC
- 只在 package/source 真正变化时保存/恢复诊断名字，避免每次调用复制
  两个 `std::string`
- `caller_source` 的 intern 查找移到确认进入 fast frame 之后

顺带修复：嵌套闭包内同名参数捕获错误。VM 的 `capture_upvalue` 优先返回
`def->upvalues` 中的继承 cell，导致同名参数被外层变量替换；现在参数、
本地 slot 与当前函数作用域内的符号优先于继承 cell。

实测（Windows，`bench_min` min-of-9，数值随机器状态有 ±5% 波动）：

```text
fib        约 1.16s → 约 0.75s
call       约 1.21s → 约 0.72s

bench_vm VM（改前 → 改后，约值）
slot 0.371→0.205   fib 1.149→0.78   call 1.180→0.74
field 0.569→0.42   method 0.420→0.30  global 0.389→0.26
index 0.537→0.40   index_write 0.421→0.32  scope 0.353→0.21
pattern 0.735→0.67
```

VM 现在在所有 `bench_vm` 用例上都快于 AST（VM/AST ratio 0.36–0.90）。

测试补充：`tests/cases/runtime.py` 增加循环块作用域遮蔽、`for` 循环变量
遮蔽、嵌套闭包参数按调用捕获。

已知（既有问题，不在测试矩阵内）：`SKEIN_VM_SLOT_POOL=0` 与嵌套闭包组合时
VM 会丢失全局定义；该开关仅用于对比调试，默认路径不受影响。

### 3.7 GC root 边界审计（已完成）

根来源清单：

- scope 树：`global_scope`、`current`、`restore_stack`、`scope->ret`
- `eval_context.control_value`
- VM frame：operand stack、param/slot 符号与直接值、open upvalue cell、
  `current_def` 的 upvalue、iterator、defer scope、快帧 `SavedVmState`
- JIT frame：整个 value slot 数组（`skein_jit_push_roots`）
- 挂起 fiber：scope 树、其 `gc_roots`、yield/final/error/resume 值
- task 注册表、struct 方法表、FFI callback / logger userdata
- 显式 `gc_root_scope`（C++ 局部值的临时根）

审计修复：

- 新增 `gc_root_scope`：builtin 在回调期间只存在于 C++ 局部变量里的值必须
  显式登记，否则 cycle collector 会清掉其闭包 scope
- `call_value_args` 统一把 callee 与实参登记为根，覆盖 compose/pipe/partial、
  `std.result.capture`、log writer、heap comparator 等回调入口
- `std.list.map/filter/reduce/unique_by/group_by/sort_by/top_k/sort` 的累加器
  与 key 缓存登记为根（`result` list、accumulator、keys、working copy）
- lazy iterator / generator 参数这类只存在于 C++ 捕获里的值通过
  `generator_attach_roots` 挂到 generator 值上（持有引用，generator 销毁时
  释放），覆盖 `std.iter.map/filter` 与生成器实参
- http / http2 serve 任务与 thread pool worker 在任务 / worker 生命周期内
  登记 handler 与 `on_request_body`
- JIT 尾调用 trampoline 在每次进入 callee 前登记 pending callee 与实参

验证：

- `tests/test_gc_roots.cpp`：guard 存在时闭包 scope 存活，guard 销毁后可回收
- `tests/gc_stress.py`：在 `SKEIN_GC_STRESS=1` 下覆盖 map/filter/reduce/
  unique_by/sort/top_k/group_by/compose/capture、lazy iterator 与 generator
  实参，VM 与 JIT 各跑一遍

剩余边界（已知且明确）：

- 长期存活的 builtin 回调（http / http2 serve handler、thread pool worker）
  在任务 / worker 生命周期内由 `gc_root_scope` 登记，调用间隙也不会被清理
- JIT 尾调用 trampoline 在进入下一个 callee 前登记 pending callee 与实参
- FFI callback 仍然要求脚本保持 callback 值存活（既有生命周期契约）；
  userdata 的 `for_each_child` 会标记其中的脚本函数
- 新增“C++ 长期持有脚本值”的代码必须显式登记，不能依赖调用时临时根
- `thread.Message` / Channel 载荷跨线程序列化，不允许函数值，因此不需要主
  collector 遍历通道缓冲
- per-pc liveness / safepoint 表仍未做，属于 moving GC 前置工作

已知问题：在单条顶层语句内的长循环中创建闭包时，`scope ↔ closure` 环只能
等 cycle collector 在语句边界回收，循环期间 `all_scopes` 持续增长。
`register_scope` 原先固定在 4096 触发全量 prune，导致每次 push 都扫描整个
列表，表现为二次复杂度（40k 次闭包创建约 2.2s）。现已把 prune 触发阈值改为
几何增长（每次扫描后取 `max(4096, size * 2)`），40k 次闭包创建降到约 36ms，
10k/20k/40k 呈线性（VM 与 AST 相同）。残留：单条语句内的峰值内存仍随累计
闭包数线性增长，后续可在循环回边加入低频 cycle collection 来收敛。

### 3.8 字符串增量拼接（已完成）

目标：让 `s = s + expr` 在 VM/JIT 下变成 O(1) 摊销，语义保持不变。

原问题：

```cpp
struct string_value_t {
    char *value;                              // 指向 storage->c_str()
    std::shared_ptr<const std::string> storage;
};
```

原 `storage` 不可变且可共享；追加后 `c_str()` 可能重新分配，只更新栈上的
副本会让另一个变量持有悬空 `value` 指针。

实现（路线 1）：

- `storage` 改为可写 `shared_ptr<std::string>`，`value` 仍指向 `storage->data()`
- 新增 `STRING_APPEND_SELF`：目标是 string 且 buffer 无额外别名时原地
  `append`；有别名时先 COW 复制
- 赋值 helper（symbol / slot / param / upvalue）在释放旧值前同步旧值的
  `value` 缓存，避免重新分配后悬空
- AST 后端实现等价路径（只对 symbol 目标生效，COW 阈值相应收紧）
- JIT 通过 `skein_jit_string_append_self` helper 支持该指令

实测（100k 次）：

```text
改前 s = s + "x"           680 ms
改后 VM / AST              20–26 ms
std.string.builder          29 ms
```

风险与约束：

- `value` 是缓存指针；任何原地修改都必须同步所有已知共享者
- 字符串会进入序列化、FFI 和跨线程边界，表示变更影响面大
- AST 的 COW 阈值与 VM 不同（无栈副本时只允许 1 个 owner）

### 3.9 启动解析优化（已完成）

已完成：`SKEIN_STARTUP_STATS` + `tools/startup_profile.py`、去掉
`tmpfile()`、跳过内嵌 std 包类型检查、避免入口目录的 eager
`weakly_canonical()`、AST arena 和内存输入 fast path。Windows
RelWithDebInfo 下空脚本 `main()` 约 0.4 ms，`import std` 约 0.85 ms，
`import std.web` 约 1.55 ms。

`import std.web` 的剩余成本大致为：解析 + 包求值约 0.78 ms、`std` 命名空间
约 0.09 ms、adapter 约 0.17 ms。**解析器优化只影响冷启动，不影响 HTTP
请求解析或请求处理热路径。**

1. **AST arena**
   - `ast.cpp` 的节点改由 size-class arena 分配；`treefree` 通过
     `ast_node_free()` 归还，节点不再逐次 `malloc` / `free`
   - 块来自进程级 64 KiB chunk，按线程 free list 复用；chunk 保持到进程
     退出，以支持跨线程释放且不需要每次加锁
   - `SKEIN_AST_ARENA=0` 可回退到逐节点 `malloc`，用于排查
   - 实测（Windows RelWithDebInfo，60 次中位数）：`parse std.web`
     0.83 → 0.80 ms，`parse std.auth` 0.94 → 0.89 ms，
     `parse std.cache` 0.92 → 0.86 ms；整体 `main()` 下降约 0.03–0.06 ms
   - 结论：分配不是主要瓶颈，收益低于原估计；保留该实现，但不再扩展
2. **内存输入 fast path**
   - 内存输入不再把每行复制到 `input_line`：`input_line` 直接引用源缓冲区，
     `YY_INPUT` 从视图读取
   - `diag_record_line` 接受 `string_view`，去掉中间 `std::string` 拷贝
   - scanner checkpoint 保存/恢复行视图，嵌套解析和 REPL 路径不变
   - 实测：`parse std.web` 0.80 → 0.78 ms，`std.auth` 0.89 → 0.87 ms，
     `std.cache` 0.86 → 0.84 ms；`main()` 再降约 0.02–0.03 ms
   - 结论：解析和诊断行存储仍是剩余成本；除非冷启动成为硬指标，否则
     不再继续拆分 `read_input_line` / `source_lines`

---

## 4. 决策记录

### 4.1 待决策

1. 是否把 Skein 包生态做成公共生态？
2. 是否引入 moving / compacting GC？
3. 是否需要 AOT 或字节码磁盘缓存？
4. Windows / MinGW 是否进入正式发布范围？
5. 是否支持 vendor 和多模块 workspace？
6. 私有模块认证与 proxy 鉴权做到什么程度？
7. 是否需要 rope / 分块字符串表示来覆盖所有拼接与切片（路线 2，暂不做）？

### 4.2 已决定

- `SKEIN_JIT` 运行时默认开启（JIT 构建）；`SKEIN_JIT=0` 保留 kill switch。
- pair/tuple 字面量要求 `(a, b)`，可用于 let/const、函数参数和 match；
  运行时校验不可变性与 arity。`[a, b]` 保持宽松的依据是结构化读取：
  它只按位置读取元素并创建新绑定，不改写源 tuple；因此
  `let l: list[int] = (1, 2);` 仍被 checker 拒绝。
- `wait t` 前缀写法（2026-11-22）：`wait e` 等价于 `e.wait()`，要求
  `e: task[T]` 并返回 `T`；`wait launch f()` 合法，`launch wait f()` 被拒绝。
  `.` 后的关键字与内置名统一按 `NAME` 解析，因此 `t.wait()`、
  `std.task.wait`、`std.regex.match`、枚举变体和 import 路径均无需特判。
  非 task 操作数在类型检查阶段报 `wait expects a task, got <type>`；
  `any` / 泛型 / union 保持宽松。不做：超时版 `wait e, ms`、
  `wait all` / `wait any`、async/await 关键字、取消或恢复函数染色。
- `launch` 直接作用于完整调用链（2026-11-22），不再下钻到最内层 call。
  `launch make().get()` 启动整个 `make().get()`；`launch f().wait()` 解析为
  `launch (f().wait())`。需要“启动后等待”时统一写 `wait launch f()` 或
  `(launch f()).wait()`。已迁移相关测试和 `SYNTAX.md`。
- Redis / Valkey 作为独立的 `std.cache`（2026-11-23），不放入 `std.db`。
  采用纯 Skein 内嵌包，基于 `std.net` / `std.tls` 实现 RESP2，不依赖动态
  native plugin。Session 和限流共享 store 已接入；Valkey 独立 CI 服务
  按需补。
- MySQL / MariaDB 纳入 `std.db`（2026-11-22），共用一个 `db_internal` 后端，
  优先 MariaDB Connector/C，兼容 libmysqlclient；支持 `mysql://` /
  `mariadb://` URI、prepared statement、`?` 参数和基础类型映射。
  `contrib/mysql` 保留为 native module ABI 示例。
- 嵌入式 KV（2026-11-23）：新增 `std.kv`，默认 SQLite 单文件后端；
  key/value 使用 BLOB，支持 prefix scan。LevelDB / RocksDB 只保留为未来
  可选目录后端。
- `wait t on error(...)` / `wait launch f() on error(...)` 与 `on error()`
  （2026-11-23）：`on error` 绑定高于前缀 `wait`/`launch`，parser 通过
  AST 重排把 handler 移到 `wait` 外侧，使任务内 `fail` 能被捕获；错误
  绑定可省略。detached 诊断改为任务销毁时才上报。裸写
  `launch f() on error(...)` 仍可解析，但会给出 `launch` warning，提示
  handler 不能覆盖任务体。保留 `launch` 关键字：不改为 `task`、
  `spawn` 或 `job`。`launch` 是动作，结果是 `task[T]`；`job` 更像
  排队/后台作业，预留给未来的作业队列抽象。不新增 `std.task.call`。
  需要结构化并发边界时使用 `std.task.scope(fn)`：它在作用域结束时
  等待所有未 detach 的任务，并返回 `fn` 的结果。

---

## 5. 暂缓与不做

本节按领域汇总明确不做或暂缓的能力，仅用于归类，不是里程碑划分。

包管理与生态：

- 没有可信镜像运营方前，不先做完整 proxy。
- 未确认公共生态前，不先做 vendor / workspace。
- 没有实际用户需求前，不扩展旧编码后端。
- SQLite 单文件 `std.kv` 已完成；没有明确的高写入/大数据量需求前，
  不实现 LevelDB / RocksDB 目录后端，也不承诺单文件。

性能与运行时：

- 没有明确性能目标前，不引入 moving GC。
- JIT 优化已暂停；OSR/deopt、per-pc liveness 与顶层声明类 JIT 等明确
  场景再重启。
- 没有明确的全量拼接/切片需求前，不引入 rope / 分块字符串表示；透明
  `s = s + x` 已完成，批量构建仍推荐 `std.string.builder`。
- 冷启动已完成 profiling、内存扫描、内嵌 std 包类型检查跳过、入口目录
  规范化、AST arena 和内存输入 fast path；没有明确的冷启动目标前，不再
  继续拆分解析输入或诊断行存储，也不单独做跨进程 AST 缓存。
- 不做延迟解析函数体、把 std 包改成动态插件、为解析速度裁剪 `std.web`
  API；预编译 stdlib 属于 M5。

Web 服务与数据层（JSON API、`std.cache`）：

- JSON API 不包含 SSR、HTML 模板、静态文件服务器、WebSocket / SSE、
  multipart、GraphQL、完整 ORM、OAuth 第三方登录。
- `std.cache` 不做 Redis Cluster / Sentinel、pub/sub、Streams、任意命令
  透传、通用 Lua 暴露、RedisJSON / RediSearch、复制与故障转移编排；
  仓库不捆绑 Redis/Valkey 服务端。后续如需要 pub/sub，另立 `std.pubsub`
  或底层 `std.redis`。

---

## 6. 性能看板与回归

解释器/JIT 性能优化暂停后，用以下命令做固定回归检查：

```bash
# VM/AST 比值（所有用例 VM 应不慢于 AST）
./build/skein tools/bench_vm.skein --bin ./build/skein

# JIT 开关对比（JIT 构建）
SKEIN_JIT=0 ./build/skein benchmarks/fib_bench.skein
./build/skein benchmarks/fib_bench.skein

# 容器增长与线程吞吐
python tools/bench_containers.py --bin ./build/skein --check
python tools/bench_threads.py --bin ./build/skein --check

# 冷启动各阶段（wall / main / import / span）
python3 tools/startup_profile.py --bin ./build/skein --runs 12
```

JIT 重启前先量化：

```bash
SKEIN_VM_STATS=1 SKEIN_JIT_DEBUG=1 ./build/skein <script>
```

固定基线（JIT 冻结时）：

- Windows JIT 35/35、Windows 非 JIT 33/33、WSL 35/35，0 警告
- VM 在所有 `bench_vm` 用例上快于 AST（ratio 0.36–0.90）
- `thread_bench` 使用多次重复中的最好成绩作为门槛
