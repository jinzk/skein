# Skein 总路线图

本文是当前唯一的总路线图。长期有效的技术设计见专项文档：

- [`package-manager.md`](package-manager.md)
- [`architecture.md`](architecture.md)
- [`bytecode.md`](bytecode.md)
- [`encoding.md`](encoding.md)
- [`unicode.md`](unicode.md)
- [`lsp.md`](lsp.md)
- [`key-semantics.md`](key-semantics.md)
- [`generics.md`](generics.md)

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

### 1.1 主线概览（2026-11-27）

| 主线 | 状态 | 下一步 |
| --- | --- | --- |
| JSON API 生产化（M6，P1） | 基础完成 | 账户锁定、OpenTelemetry exporter、完整 stack 捕获按部署需求推进 |
| 包管理安全与运维（M1） | 核心完成 | macOS `flock` 验证暂缓；交叉编译按需 |
| 包生态与信任（M3） | 按需 | 公共生态暂缓，先支持私有/本地生态；SumDB / proxy / 私有认证按私有需求推进 |
| JIT / VM 与 GC（M4） | 优化冻结 | moving GC 与 AOT / 字节码缓存均已确认不做；已完成记录见第 3 节 |
| 长尾能力与体验（M5） | 按需 | 旧编码、HTTP/2 CONNECT、REPL / LSP 体验、LevelDB / RocksDB |
| 对象抽象 / `proto` / `ref` / Channel 模型（M8） | 功能已完成 | Channel 端点对、结构化 `proto`、struct 内联方法、`_` 包私有成员，以及 `ref` / `weak` / GC / channel payload 拒绝已落地；文档同步完成，仅剩 Windows x64 MinGW/MSVC CI 发布门槛 |
| 用户自定义泛型（M9） | 已完成 | 泛型 struct/enum/函数/proto、显式实例化、约束、默认类型参数、裸 `Box` 补全、递归、VM/JIT 擦除、`std.meta` 泛型入口和 JSON envelope 已完成；通用二进制值格式不在当前范围，按需再评估 |
| 断言与内置语法宏（M10） | 已完成 | 第一批已完成：`std.assert` / `std.log.enabled` 与 assert、日志、`capture!` 内置 AST 宏已落地并通过全量验证；`list!` / `dict!` / `try!` 已决定不实现；用户自定义宏不在 M10 |
| 用户自定义宏与保留字清理（M11，P1 / P2 / P3.1 / P4 含跨包 / P5.1–P5.5 / G1–G5 已完成） | 已完成 | AST 级静态模板、受限片段、hygienic、声明先于使用。`proto`、`import {name} from pkg`、prelude 默认 import、`list` / `dict` 构造器、`fail!` / `recover!` / `pass!` / `wait!` 宏、`launch` / `wait` / `await` 表达式起始 contextual、用户宏 v1 与跨包 `export macro` / `import {name!} from pkg;`，以及 `on` / `error` handler-scoped contextual、`as` / `ref` / `weak` / `is` contextual 均已落地；G1–G5 完成泛型实参 AST 化、受限 `type` 片段、定义期边界强化、formatter / lint / LSP / VM-JIT 同步与核心约束验收；设计见 [`docs/macros_user_defined.md`](macros_user_defined.md) |
| 操作符重载（O0–O4） | O0–O4 已完成 | 自引用 proto 约束、`std.ops` 协议、算术/相等/比较运算符重写，以及标准库算法接入 `Comparable` 均已落地，见 2.9 |
| `std.cache`（M7）/ `std.kv` | 已完成 | 维护；Valkey 独立 CI 服务可按需补 |
| FFI 平台 CI / fuzz | 按需 | Windows x64（MinGW/MSVC）进入正式发布；32 位 / ARM 与 fuzz 按需，见 `architecture.md` |

最近完成：

- 服务 CI：PostgreSQL / MySQL / Valkey 集成服务与全量 ctest
- RBAC / 角色、权限 middleware、W3C trace context 与访问日志 trace 字段
- `std.cache` Redis / Valkey 客户端与共享 Session / 限流 store
- SQLite 单文件 `std.kv`
- 启动 profiling、AST arena、内存输入 fast path
- P1 / P2 / P3.1：`proto`、selective import、prelude 默认 import、`list` / `dict` 构造器、`fail!` / `recover!` / `pass!` / `wait!` 宏，以及 `launch` / `wait` / `await` contextual keyword
- P4：用户自定义宏 v1（四种片段、hygiene、嵌套调用、递归禁止）与跨包
  `export macro` / `import {name!}`（parse-only 包读取、宏命名空间隔离、循环检测）
- P5.1：`on` / `error` handler-scoped contextual；`error!` 走普通宏路径，`type error` / `.error` 不受影响
- P5.2：`as` contextual；cast、import 别名与模式绑定仍是分隔符，其他位置是普通标识符
- P5.3：`ref` contextual；`ref(x)`、`ref Point { ... }` 与 `ref T` 前缀保持，其他位置是普通标识符
- P5.4：`weak` contextual；只在 `weak T` 类型前缀位置作为关键字，其他位置是普通标识符
- P5.5：`is` contextual；只在 `expr is T` / `case is T` 类型测试位置作为运算符，其他位置是普通标识符
- G1–G5：泛型实参 AST 化、用户宏受限 `type` 片段与定义期边界强化；formatter /
  lint / LSP 覆盖、调用点与定义点双位置诊断、VM/JIT differential 同步，以及
  核心约束验收

### 1.2 推荐执行顺序

1. P1 JSON API 基础已完成；账户锁定、外部 OpenTelemetry exporter 和完整
   stack 捕获按实际部署需求推进。
2. M1 只剩 macOS `flock` 验证（暂缓）和交叉编译（按需）。
3. 公共生态已确认暂缓；M3 先支持私有/本地生态，SumDB / proxy / 私有认证
   按私有需求推进。
4. M8 已确认只保留 `proto` 并直接删除 `trait`，且以 duplex 端点对作为
   唯一 Channel 模型并删除旧共享队列；阶段 1（端点对）、阶段 2（结构化
   proto）、阶段 3（`ref` / `weak` / GC / channel payload 拒绝）、struct
   内联方法与 `_` 包私有成员均已完成；文档同步完成，仅剩 Windows x64
   MinGW/MSVC CI 发布门槛。
5. M4 的 GC root 审计、热点 opcode / frame 分配、字符串拼接、
   启动解析优化和 JIT 默认启用已完成；moving GC 与 AOT / 字节码缓存均已
   确认不做；per-pc liveness / OSR / deopt 保持不做。
6. 测试稳定性已完成：服务端测试改为子进程内绑定端口 0 并回读实际端口；
   `thread_bench` 门槛使用多次重复中的最好成绩。
7. M11 的 P1 / P2 / P3.1 / P4（含跨包 P4.3）与 P5.1–P5.5 已完成；M11 用户宏与
   contextual keyword 阶段全部落地。
8. 长尾能力按实际需求触发。

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

### 2.2 包生态与信任（M3；按需）

前提：公共生态已确认暂缓，先支持私有/本地生态；公共 proxy、公共 SumDB
和公共模块发现暂不启动。

范围按私有/本地需求推进：

1. SumDB / 首次下载信任
2. Go 风格 proxy
3. `direct` / `off` 回退
4. （暂不做）私有模块认证
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

前提：moving / compacting GC、AOT 与字节码序列化均已确认不做。

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

moving / compacting GC 不引入：

- 只做 GC root 边界审计（已完成）
- 继续优化 frame 分配与热点 opcode（已完成）
- per-pc liveness / OSR / deopt 不因 GC 启动

重启条件（满足其一）：

- 真实负载的 p50/p95 明确退化，且 profiling 指向 JIT 覆盖或生成代码；
- 具体程序的热点落在只有 OSR 才能覆盖的 VM 帧；
- 内存 / 延迟成为明确需求；
- 决定引入类型特化，需要 per-pc liveness / deopt。

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
- （暂不做）vendor / workspace
- （暂不做）AOT / 字节码序列化、预编译 stdlib、字节码 ABI 冻结
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
- 多行历史条目

### 2.6 对象抽象、`proto` 与引用模型（M8；阶段 3 已完成）

状态：结构化 `proto`、直接删除 `trait`、struct 内联 `func` 方法、`_`
前缀包私有成员，以及 `ref` / `weak` / GC 接入与 channel payload 拒绝均已
实现；顶层 `impl Type` 保留为补充方法容器。文档同步已完成，剩余工作仅为
Windows x64 MinGW/MSVC CI 发布门槛。

背景与问题：

- 未标注调用按运行时能力工作；原 `trait` 名义式契约设计已移除，当前只保留
  结构化 `proto`，文档已同步修正“动态类型”与鸭子类型的混淆；
- 可选自引用字段可以表达形状，例如 `next: Node?`，但 `struct` 是 COW
  值类型，`a.next = b; b.next = a;` 仍会报
  `recursive value would create a cycle`；
- 普通值互递归 struct 字段仍不支持（`struct A { b: B? }` 会报未知类型名并提示改用
  `ref B?` / `weak B?`）；`ref` / `weak` 字段支持前向互引用；
- `SYNTAX.md` 对 proto 默认方法的描述已统一：支持默认方法和冲突规则；此前
  trait 默认方法的前后矛盾已随 trait 删除一并修正。

产品方向：

- 已确认只保留结构化 `proto`，移除用户层 `trait` / `impl Trait for
  Type`；`impl Type` 保留为具体类型写方法的机制；
- 不把现有 `trait` 直接改成结构化语义，否则会破坏显式契约和已有诊断；
- 不提供 deprecated alias；`trait` 与 `impl Trait for Type` 直接删除，一次性迁移；
- `struct` 体内可直接写 `func` 方法，语法与 `impl Type` 内的方法一致；解析后
  等价于紧随 struct 的隐式 `impl Type`，因此后续顶层 `impl Type` 可以添加
  方法但不能覆盖同名方法，重名仍报 `method repeated <name>`。

`proto` 语义（已实现）：

```skein
proto Drawable {
  func draw(self);
  func describe(self) { print("drawable"); }
}

struct Point {}

impl Point {
  func draw(self) { print("point"); }
}

func render(x: Drawable) { x.draw(); }

render(Point {});   // 有匹配的 draw(self)，自动满足
```

规则：

- 结构化匹配方法名、参数数量、参数类型、`self` 和返回类型；
- 不需要 `impl Drawable for Point`；
- 默认方法保留，具体类型写了同名方法则覆盖；
- 多个 proto 提供同名默认方法且具体类型未覆盖时报错，要求显式写方法；
- `is Proto` 做结构化运行时检查；
- `std.meta.protocols(type)` / `std.meta.implements(type, name)` 同步改为
  结构化语义；
- 如果 `T` 满足 proto，则 `ref T` 也自动满足。

`ref` / `weak` 语义（已实现）：

```skein
struct Node {
  value: int;
  next: ref Node? = null;   // 强引用，拥有后继
  prev: weak Node? = null;  // 弱引用，只观察前驱
}

let a = ref Node { value: 1 };
let b = ref Node { value: 2 };

a.next = b;       // 强边：a -> b
b.prev = a;       // 弱边：b -> a，不构成所有权环
b.value = 9;
print(a.next.value);  // 9，引用不是值快照
```

规则：

- `ref T` 是类型化、受 GC 管理的安全指针 / 对象句柄；不做地址运算、
  手工释放或裸指针暴露；
- `T { ... }` 仍是普通值；只有 `ref T { ... }` 创建 GC 管理的引用对象；
- 复制 `ref` 只复制句柄，别名看到同一对象；`let c = a` 不会复制目标；
- `a = b` 是局部变量重新绑定；替换整个目标内容用 `a.* = ...` 或
  `std.ref.set(a, ...)`；
- 通过 `ref` 调用方法时 `self` 绑定为引用，方法内修改持久化；普通值接收者
  仍保持“接收者副本”语义；
- `weak T` 不保活；读取用 `std.ref.upgrade(w)` 得到 `ref T?`，并同期提供
  通用 `?.` 可选链，可写 `b.prev?.value`；
- 弱引用保存 `{id, generation}`，目标释放后 generation 失效，不会误指向
  新对象；
- 跨线程或序列化边界使用 `std.ref.snapshot(r)` 显式得到普通值快照；
- 两个不同类型互引用时用 `ref T?` / `weak T?`，普通值字段必须是已声明类型；
- `_` 前缀字段/方法是包私有成员，定义所在包内的函数和方法可直接访问。

GC 接入（已实现）：

- 现有 `collect_cycles()` 已具备 tracing 骨架：`gc_roots` / thread root
  visitor、`for_each_value_child()` 递归 mark、upvalue cell 的 epoch sweep、
  thread 模块的 userdata 图 collector；不另起一套全值 GC；
- 新增 `ref_object` 注册表，作为现有 collector 的一类新 sweep 目标；
- GC mark 沿强 `ref` 边走，不沿 `weak` 边走；
- `value_reaches_any()` 的值环检查在 `T_ref` 边界停止：普通 `struct` /
  `list` / `dict` 值环仍拒绝，强引用对象环交给 GC；
- `freeval(ref)` 只释放句柄包装，不直接释放目标对象；
- sweep 未标记 `ref_object` 时先断开其普通值字段，再 `freeval()`，最后递增
  generation 使弱引用失效；
- 触发条件在现有语句数 / scope 增长阈值之外增加 ref 分配计数；
  `SKEIN_GC_STRESS` 覆盖该路径；
- 已确认 `ref` / `weak` 不可跨线程发送：ref heap 严格线程本地，不做共享
  ref heap、跨线程 GC root、锁或 generation 迁移；
- 跨线程唯一入口是 channel；在 `thread_channel.cpp` 的
  `validate_data_value()` 中拒绝 `T_ref` / weak payload，递归容器中的 ref
  一并拒绝，错误提示引导使用 `std.ref.snapshot()` 或 `std.arena` id；
- `std.arena` store 是值语义，发送 `(store, id)` 得到接收线程自己的快照，
  不表示共享可变状态；需要共享状态时使用 owner 线程 + 消息协议；
- `ref` 对象析构发生在 GC safepoint，不作为立即释放语义承诺。

阶段与验收：

1. ~~修 `SYNTAX.md` / `docs/tutorial.md` 的 trait 默认方法矛盾，并补
   “动态调用看运行时能力，接口看显式/结构化契约”的说明~~（已完成）；
2. ~~新增 `protocol`，一次性迁移测试、stdlib、LSP 和文档，并删除 `trait` /
   `impl Trait for Type`~~（已完成）；
3. ~~允许 `struct` 体内直接定义 `func` 方法；顶层 `impl Type` 仍可继续添加
   方法，二者共用方法命名空间且重名报错~~（已完成）；
4. ~~实现 `ref` / `weak` 内部值、`std.ref` API、通用 `?.` 可选链、GC sweep
   与 channel payload 拒绝~~（已完成）；
5. 验收：强引用环可被回收，weak 在目标释放后升级为 `null`，普通值环仍
   被拒绝，channel 发送 `ref` / weak（含嵌套）报错且 snapshot 可发送，
   `SKEIN_GC_STRESS` 无漏根，JIT/VM 差分与语言核心测试稳定。

阶段 2 验收已通过：结构化签名匹配、默认方法覆盖与冲突、`is Proto`、
`std.meta.protocols` / `implements`，以及 AST/VM/JIT 与线程 transfer 差分均稳定。

阶段 3 验收已通过：`ref` 别名与替换、ref 方法、proto 转发、`?.`、
强环 sweep / weak 失效、普通值环拒绝、channel 嵌套拒绝与 snapshot 发送，
在 AST / VM / JIT 与 `SKEIN_GC_STRESS` 下一致。

### 2.7 Channel 模型调整（M8；阶段 1 已完成）

状态：已确认以 duplex 端点对为唯一 Channel 模型；删除旧共享队列，不做兼容别名。阶段 1（端点对迁移）已实现。

背景与问题：

- 原 `std.thread.Channel` 是无方向共享队列，`std.thread.duplex` 是有方向
  端点对；两者是两个独立 userdata 类型；
- 现有跨线程用法主要是请求 / 响应、signal / reply，本身就带方向；共享队列
  的方向完全靠约定，容易误用；
- `broadcast` 的“多消费者共享队列”语义没有实际用户场景，测试中只有一处；
- 之前确定的“跨线程只能通过 channel + owner 线程消息协议”更贴近端点对模型。

新模型：

```skein
let ends = std.thread.Channel(2);
let parent = ends[0];
let child = ends[1];

parent.send(req);        // parent -> child
let res = parent.recv(); // parent <- child

child.send(evt);         // child -> parent
let got = parent.recv();
```

规则：

- `std.thread.Channel(n)` 返回一对有方向端点（`parent` / `child`）；
- 单向通信只使用其中一个方向，不再需要单独的共享队列类型；
- 每条 lane 对应一个 `channel_state`；`capacity == 0` rendezvous 与
  `-1` unbounded 对每条 lane 生效，放开原 duplex 的 `>= 1` 限制；
- `send` / `try_send` 沿 out 方向，`recv` / `try_recv` 沿 in 方向；
  `close_send` / `close_recv` 保留，`close()` 关闭该端点的两个方向；
- `select` 接受 endpoint，使用 in 方向；`broadcast` 重定义为“向一组
  endpoint 的 out 方向各发一份”，不再表示多消费者共享队列；
- `Message.handles` 携带 endpoint；`spawn_script_with_channel` 与
  `spawn_script_with_duplex` 合并为同一路径：创建端点对，子端注入
  `ctx.handles["channel"]`，父端通过 `thread.channel` 访问；
- `std.thread.pool` 与 process result channel 内部改用端点对，脚本侧
  `pool.submit` / `join` 不变；
- 删除旧 `thread_channel` 共享队列 userdata、旧 `unbounded_channel` 等兼容
  名，以及 `thread_duplex` 独立类型名；脚本可见类型统一为一种 endpoint；
- `proto` 落地后可定义通用 `Endpoint` 协议（`send` / `recv` / `try_*`），
  但不是合并类型的必经步骤；
- 与 `ref` 边界一致：`validate_data_value()` 继续拒绝 `ref` / weak payload
  （含嵌套），channel / endpoint 句柄只放在 `handles`。

阶段与验收（阶段 1 已完成）：

1. 实现统一 endpoint 内部表示（in / out lane + peer），底层复用
   `channel_state` 作为每条 lane 的队列；
2. 一次性迁移 spawn helpers、select / broadcast、pool、文档、测试与 LSP，
   删除旧共享队列 API 和两个 userdata meta 名；
3. 验收：请求 / 响应双向通路、半关闭、rendezvous / unbounded、`select`
   和重定义后的 `broadcast`、channel 发送 `ref` / weak 报错均通过。

### 2.8 用户自定义宏与保留字清理（M11，P1 / P2 / P3.1 / P4 / P5.1–P5.5 已完成）

方向：宏继续是 AST 级、受限、hygienic、在 typecheck 前展开的静态模板；不引入
文本宏、编译期任意执行、类型内省、一等宏值或泛型语法生成。低风险 contextual
keyword（`in` / `export` / `import` / `package` / `include`）已完成。其余按
“文档一致性 → 内置值 / 内置宏默认化 → 任务语法 → 用户宏 → 高风险 contextual”
的顺序推进，详见 [`docs/macros_user_defined.md`](macros_user_defined.md) 第 10 节。

阶段：

1. **P1.1（已完成）** 协议声明关键字 `protocol` 重命名为 `proto`，作为全局保留字；
   泛型约束列表里的 `proto` / `type` 保持 contextual 标记，反射 kind 字符串同步改为
   `"proto"`；不保留 `"protocol"` 别名。
2. **P1.2（已完成）** 值/类型的 selective import：`import {name} from pkg;`，
   `from` 为 contextual keyword；formatter / lint / LSP、文档与测试已同步，
   check-only 工具不执行被导入的包。
3. **P2.1（已完成）** 普通内置函数改为默认 import：`len` / `range` 放进
   `std.utils`（`std.utils.len` / `std.utils.range`），`print` / `println` /
   `input` → `std.io.*`，类型内省 → `std.type.*`；lexer 不再发
   `SKEIN_TOK_BUILTIN`；解析优先级为局部绑定 > 显式 import > 默认 import。
4. **P2.2（已完成）** 内置宏改为 parser 侧默认宏绑定，裸名映射到 `std.log.*!` /
   `std.result.capture!` / `std.error.*!` / `std.task.wait!` 等规范路径；
   `fail` / `recover` / `pass` / `wait` 的规范形式改为宏，旧关键字形式暂时并行；
   追加 `name!` 与 `name! x` 两种宏简写；用户宏可遮蔽裸名。
5. **P2.3（已完成）** `list` / `dict` 改为默认 import，规范路径为
   `std.list.new` / `std.dict.new`；`list(x)` / `dict(x)` 解析为对应构造器，
   值作用域可遮蔽且类型命名空间保持独立；不做 `list!` / `dict!` / `try!`。
6. **P3.1（已完成）** `wait` 退出保留字，`launch` / `wait` / `await` 改为表达式
    起始 contextual keyword，
   `await f()` 等价 `wait launch f()`，要求后面是调用表达式，保留 `on error`
   语义；postfix 优先级高于前缀，`.` 后仍是普通名字。
7. **P4（已完成）** 用户宏 v1 一次落地：`macro` 声明、`expr` / `ident` / 尾部
   `expr...` / 尾部 `block`、同文件声明先于使用、hygiene、嵌套调用和递归禁止。
8. **P4.3（已完成）** `export macro` 与 `import {name!} from pkg;`（含 alias）落地；
   宏与值/类型命名空间隔离、定义点宏表快照、跨包 hygiene、循环检测、parse-only
   包读取与测试矩阵均已完成；引号形式的外部 module 宏导入暂不支持。
9. **P5.1（已完成）** `on` / `error` 改为 handler-scoped contextual，保留 `%prec ON`；
   `error!` 改为普通 `NAME MACRO_BANG` 路径，`type error` 走类型命名空间。
10. **P5.2（已完成）** `as` contextual：cast、import 别名与模式绑定位置为分隔符，其他位置仍是普通标识符。
11. **P5.3（已完成）** `ref` contextual：`ref(x)`、`ref Point { ... }` 与 `ref T` 前缀保持，其他位置仍是普通标识符。
12. **P5.4（已完成）** `weak` contextual：只在 `weak T` 类型前缀位置作为关键字，其他位置仍是普通标识符。
13. **P5.5（已完成）** `is` contextual：只在 `expr is T` / `case is T` 类型测试位置作为运算符，其他位置仍是普通标识符。

验收：每阶段保持第一批宏与全量测试全绿；`tools/check_docs_sync.py` 通过；
遮蔽规则统一为“局部绑定 / 同文件用户宏 > 显式 import > 默认 import”，类型命名
空间保持独立。
设计细节见 [`docs/macros_user_defined.md`](macros_user_defined.md)。

#### 后续计划：宏与泛型结合（G1–G5 已完成）

M11 继续保持“宏不生成泛型语法”的边界。当前可用组合是：泛型实例化写在宏外，
宏只接收已绑定泛型值；宏体生成对泛型函数的普通调用并交给类型推断。不要依靠宏
生成 `Box[int] { ... }` 或 `f[int](...)`。若未来要让二者更紧密协作，按以下顺序推进：

1. **G1 AST 化泛型实参（已完成）**：parser 一次解析显式类型实参并存入 AST；
   typechecker 不再从原始源码文本恢复 `Box[int]` / `f[int](...)` / `let g = f[int]`
   的实参。AST 节点同时保存源码位置和解析错误，用户宏 clone 会复制这些信息。
2. **G2 受限 `type` 片段（已完成）**：在现有 `expr` / `ident` / `expr...` / `block` 之外
   增加只出现在类型位置的 `T: type`；片段按类型命名空间解析并参与 hygiene：
   调用点传入的名字保留调用点拼写与 import 别名，模板内字面类型名按定义点解析。
   `proto` 名称（`Drawable`、`pkg.Keyed`）作为类型命名空间实体一并支持：可用于
    类型标注与 `is #T`；`[V proto Drawable]` 在普通源码继续可用，但宏模板仍
    不得生成泛型声明或实参，因此 `Box[#P]` / `f[#T](...)` 明确拒绝。
    `Hashable` / `Iterable` 等 proto 约束仍需专用约束片段，不混入 `type` 片段。
3. **G3 宏类型参数边界（已完成）**：在定义期固化“宏不替代泛型”的边界，不新增
   语法。模板体不得生成 `struct` / `enum` / `proto` / `impl` 或具名 `func` 声明
   （`macro template cannot generate declarations`），也不得生成显式泛型应用，
   包括模板里写死的 `f[int](...)` / `Box[int] { ... }` 与 `f[#T](...)`
   （`macro template cannot generate generic syntax`）；`impl Box[int]` 本身在
   parser 阶段就是语法错误。类型片段不能进入反射 builtin（`kind_of(#T)` 等报
   `type fragment cannot be used as a value or name`），不做类型计算或类型内省。
   限制只针对模板体：调用点已有的泛型值仍可经 `expr` 片段传入，匿名函数表达式
   仍可作为普通表达式出现在模板里。
4. **G4 工具链与双后端同步（已完成）**：formatter 覆盖 `T: type`、类型位置 `#T`
   的 nullable / `ref` / `weak` / `as` / `is` / 函数签名形态并保持幂等；LSP 格式化
   校验与 `skein fmt` 一样只做语法检查。lint 对未使用片段给出定义点 warning，
   非法片段位置仍为定义点 error。宏展开诊断同时携带调用点（主位置）与模板定义点
   （related information），lint JSON 与 LSP `relatedInformation` 均输出。LSP 支持
   本地宏名补全、签名 hover、调用点到声明的跳转，以及 selective import 宏别名的
   补全与 hover。新增运行时测试覆盖 `#T?` / `ref #T` / `weak #T` / 函数参数与返回
   类型，并补齐三条 VM/JIT differential case。
5. **G5 保持核心约束（已完成）**：继续 parse-before-typecheck、AST 级受限模板、
   hygienic、声明先于使用；不引入文本宏、编译期任意执行、反射/类型内省、一等宏值
   或宏递归。新增负例确认字符串/注释里的宏调用文本不会被替换、未调用模板既不会
   执行也不会触发类型检查、宏名不能作为一等值使用，并补齐 `enum` / `proto`
   声明、`type_name` 反射入口与片段位置误用；与已有的 hygiene、声明先于使用、
   递归禁止、泛型语法/声明禁止、类型片段非法位置测试一起构成 G5 验收矩阵。

### 2.9 操作符重载（O0–O4 已完成）

状态：**O0–O4 已完成**。当前语言已支持用户类型的算术、相等与比较运算符重载，
标准库默认排序与 `min` / `max` 也会使用 `Comparable.op_cmp`。本节记录已确认的
分阶段设计。

方向：先补 proto 约束基础设施和标准协议，再把运算符在 typecheck 期重写为普通方法
调用。保持 parser 的优先级/结合性不变，不新增 AST 节点或 opcode；AST eval、VM、JIT
复用现有 `CallMethod` 分派。

已确认决策：

- 从 proto 约束开始，不采用新语法或宏 `operator` 片段；
- 标准协议放在新包 `std.ops`，都是普通结构式 proto，不设置 `builtin_kind`；
- 方法名用 `op_add` / `op_sub` / … / `op_eq` / `op_cmp`，不复用 `key_eq`；
- `Comparable` 用单一 `func op_cmp(self, rhs: R): int`，不拆成四个比较方法；
- `std.ops.Equatable` 与算术协议同批定义，但相等/比较运算符晚于算术运算符接线；
  它与 `std.dict_key.KeyEquatable` 是不同协议；
- `KeyEquatable.key_eq` 继续只服务 dict/set，且不改变普通 `==`；
- `op_eq` 不自动作为 dict/set key 相等；需要复用时由用户显式让 `key_eq`
  委托 `op_eq`，并继续由 `dict_hash` 保证 hash 一致性；
- 标准库 `std.list.sort` / `std.list.sort_desc` 和 `std.math.min` / `std.math.max`
  在全部参数为 Comparable 用户值时使用 `op_cmp`；显式比较器仍然优先，内置数值、
  字符串与 NaN 语义不变；
- 不支持 `any` 动态重载、复合赋值、下标、`++` / `--`、链式比较或宏生成重载方法。

阶段：

1. **O0 自引用 proto 约束（已完成）**：`[T Add[T, T]]` / `[T Keyed[T]]` 已生效。
   parser 在解析泛型参数约束时把当前参数名识别为类型变量；typecheck 在函数调用、
   泛型实例化、显式类型实参和默认类型校验时用实际 bindings 替换约束里的类型变量。
   不改运算符语义，只补正例/负例测试。
2. **O1 标准 operator proto（已完成）**：新增 `std.ops`，定义
   `Add[R, O]` / `Sub[R, O]` / `Mul[R, O]` / `Div[R, O]` / `Mod[R, O]`，以及
   `Equatable[R]`（`std.ops.Equatable`）的 `op_eq` 与 `Comparable[R]` 的 `op_cmp`。
   协议通过显式 import 进入泛型约束；只定义协议和文档，不改任何表达式行为。
3. **O2 算术运算符接线（已完成）**：`+ - * / %` 在左操作数为已知 struct/enum 且
   有对应方法时，由 typecheck 重写成 `a.op_add(b)` 等调用；内置类型仍走原
   fast path。静态检查返回方法声明类型；AST / VM / JIT 复用现有 `CallMethod`
   分派，differential 验证三路一致。
4. **O3 相等与比较接线（已完成）**：`a == b` / `a != b` 走 `std.ops.Equatable.op_eq`
   （`!=` 取反），`< <= > >=` 由 `Comparable.op_cmp` 的符号推导；类型检查要求
   `op_eq` 返回 `bool`、`op_cmp` 返回 `int`。内置数值、字符串和 NaN 语义保持
   不变；`pattern`、`list.contains`、dict/set key 判定继续使用各自的既有相等逻辑。
5. **O4 标准库算法接线（已完成）**：`std.list.sort` / `std.list.sort_desc` 和
   `std.math.min` / `std.math.max` 在全部参数为 Comparable 用户值时调用
   `op_cmp`，运行时要求它返回 `int`；显式比较器优先。内置数值、字符串与 NaN
   语义保持不变，AST / VM / JIT 通过同一 builtin 路径保持一致。

验收：每阶段保持全量测试与 VM/JIT differential 全绿，`tools/check_docs_sync.py`
与 `git diff --check` 通过；新增文档需同步 `SYNTAX.md`、`docs/tutorial.md`、
`docs/stdlib.md`。

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
- 默认类型检查启用时，`for ... in range(...)` 的参数是编译期整数常量表达式
  时走计数循环，不物化 list；`range()` 在其他上下文仍返回 list
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
- 补全候选弹窗 / 分栏（方向键或 Ctrl-P/N 选择，Tab 接受，Esc/Ctrl-G 关闭）
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
- `thread.Message` / Channel endpoint 载荷跨线程序列化，不允许函数值，因此
  不需要主 collector 遍历通道缓冲
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

当前无待决策项。

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
  `wait all` / `wait any`、`async` 关键字、取消或恢复函数染色；`await` 已作为表达式
  起始 contextual keyword 落地（见 P3.1）。
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
- `on error` handler 控制流（2026-11-24）：handler 可以通过
  `return` / `break` / `continue` / `goto` 退出，原始错误视为已处理；
  handler 作用域 defer 正常执行，VM 使用 handler 清理指令支持中途退出。
  默认参数值和 `defer` 表达式中的 handler 仍只允许 `recover` / `pass` /
  `fail`，控制流退出会报错。
- goto / label（2026-11-24）：只支持裸 `NAME:` label，移除 `label`
  关键字，`label` 恢复为普通标识符；label 必须是行首（允许缩进）的第一个
  代码 token，且后必须跟可执行语句，不能用空语句，跳到函数末尾使用
  `return;`。label 是函数级命名空间，嵌套函数 /
  lambda 独立，同一函数内不允许重名；允许前向引用，未定义或重复 label
  是错误，未使用 label 报 warning。`break NAME;` / `continue NAME;` 只能
  解析到当前位置外层且接受该控制流的循环 / switch label，否则报错。goto 只能跳到同块或外层块，禁止跳入
  内层 / 兄弟块、`switch` / `match` arm 和跨函数；跳入 `on error`
  handler 由“不能跳入内层块”统一禁止。同块内前向和后向都不能跨越
  `let` / `const` 声明。跳出块时沿用块级 defer，像 `break` / `continue`
  一样执行被退出块的 defer 和循环 iterator 清理；goto 可以跳出
  `on error` handler，原始错误视为已处理。含 `yield` 的函数禁止 goto；
  含 `wait` / `launch` 的函数允许 goto，普通 `.wait()`、
  `std.task.wait(...)`、`std.task.scope(...)` 不受限制。实现时只在行首识别
  `NAME ':' non_empty_stmt`，按函数收集 label 并做两遍解析。当前已
  实现裸 `NAME:` + `goto NAME;`，支持跳到当前或外层块并在退出时执行
  defer / iterator / handler 清理（AST/VM）；已实现函数级 label 唯一性、
  重复 label 报错、未使用 label warning、生成器函数禁止 goto，以及同块内
  跨越 `let` / `const` 的检查。
- 对象抽象方向（2026-11-25 已确认，P1.1 后关键字重命名为 `proto`）：只保留结构化
  `proto`，移除用户层 `trait` / `impl Trait for Type`；`impl Type` 保留为具体类型
  方法容器。
- `trait` 过渡策略（2026-11-25，已确认）：不提供 deprecated alias，直接删除
  `trait` 与 `impl Trait for Type`，迁移一次性完成。
- `ref` 跨线程（2026-11-25，已确认）：不可跨线程发送；ref heap 严格线程
  本地。利用现有 channel payload 校验拒绝 `T_ref` / weak（含嵌套），不做
  共享 ref heap、跨线程 GC root、锁或 generation 迁移；跨线程用
  `std.ref.snapshot()` 或 `std.arena` id / owner 线程消息协议。
- Channel 模型（2026-11-25，已确认）：以 duplex 端点对为唯一 Channel，
  删除旧无方向共享队列，不做 `Queue` 兼容层；每条 lane 支持
  `capacity 0 / -1`，`broadcast` 改为向 endpoint 列表的 out 方向发送。
- `?.` 可选链（2026-11-25，已确认）：与 `ref` / `weak` 同期实现通用版本，
  可写 `b.prev?.value`；不只做 weak 专用糖。
- 公共包生态（2026-11-25，已确认）：暂缓，先支持私有/本地生态；公共 proxy、
  公共 SumDB 和公共模块发现暂不启动。
- moving / compacting GC（2026-11-25，已确认）：不引入；保持非移动引用计数
  + 现有 scope / `ref` 对象 collector；per-pc liveness / OSR / deopt 不因
  GC 启动。
- AOT / 字节码磁盘缓存（2026-11-25，已确认）：都不做；不冻结字节码 ABI，
  不预编译 stdlib，冷启动优化以现有解析 fast path 为止。
- Windows 正式发布（2026-11-25，已确认）：Windows x64 进入发布矩阵，
  MinGW / MSVC 构建与 CI 视为发布门槛；32 位、ARM 和 fuzz 仍按需。
- vendor / workspace（2026-11-25，已确认）：暂不做；保持单模块项目 +
  全局缓存，不为本地目录集合引入新的依赖模型。
- 私有模块认证（2026-11-25，已确认）：暂不做；保持本地路径 / 全局缓存 /
  离线模式，不引入 token、login 流程或 proxy 鉴权。
- rope / 分块字符串表示（2026-11-25，已确认）：不做；保持现有字符串表示，
  透明 `s = s + x` 与 `std.string.builder` 已覆盖实际需求。
- `ref` / `weak` 语义（2026-11-25）：`ref T` 是类型化、受 GC 管理的指针/
  句柄，进入 GC 图；`weak T` 不保活并通过 generation 失效；普通值继续
  引用计数，`T { ... }` 不会隐式变成引用对象。
- `_` 前缀成员（2026-11-25，已决定）：字段/方法是包私有的，定义所在包内的
  函数和方法可直接访问，其他包直接访问报 `private member`；struct literal、
  `std.meta`、序列化和 `std.ref.snapshot()` 不变；不做名称改写、跨包符号硬
  隔离或反射隐藏。
- GC 接入（2026-11-25）：复用现有 `collect_cycles()` / `gc_roots` / epoch
  sweep，强 `ref` 参与 mark，`weak` 不参与；值环检查在 `ref` 边界停止，
  不把整个值系统改成 tracing GC。

---

## 5. 暂缓与不做

本节按领域汇总明确不做或暂缓的能力，仅用于归类，不是里程碑划分。

语言与对象模型：

- 不把默认 `struct` / `list` / `dict` 改成 GC 对象；引用必须显式写 `ref`；
- 不做地址运算、手工释放或裸指针暴露，`ref` 只提供类型化安全句柄；
- 不做 `trait` deprecated alias；`trait` 与 `impl Trait for Type` 直接删除；
- 不做全值 tracing GC，只把 `ref_object` 接入现有 collector；
- 不做跨线程共享 ref heap；channel payload 校验拒绝 `ref` / weak，跨线程用
  `std.ref.snapshot()` 或 `std.arena` id / owner 线程消息协议；
- 不保留无方向共享队列 / `unbounded_channel` / `thread_channel` 旧类型；
  Channel 统一为端点对，不做 `Queue` 兼容层；
- 不做“多消费者共享一个 Channel”语义；一对多由用户建立多条端点链路；
- 不自动把回边降级为 `weak`；回边是否保活由字段类型显式表达；
- `_` 前缀成员只做包私有访问控制，不做名称改写、跨包符号硬隔离或反射隐藏；
- 暂不实现 `trait` 继承、关联类型、泛型 trait 和动态添加方法；运算符重载当前
  仍不支持，已按 O0–O4 形成设计（见 2.9），未排期前保持现状。

包管理与生态：

- 公共生态暂缓；不做面向公众的完整 proxy / SumDB / 模块发现。
- vendor / workspace 已确认暂不做；保持单模块项目 + 全局缓存。
- 私有模块认证与 proxy 鉴权已确认暂不做；保持本地路径 / 全局缓存 / 离线模式。
- 没有实际用户需求前，不扩展旧编码后端。
- SQLite 单文件 `std.kv` 已完成；没有明确的高写入/大数据量需求前，
  不实现 LevelDB / RocksDB 目录后端，也不承诺单文件。

性能与运行时：

- moving / compacting GC 已确认不引入；保持非移动引用计数 + scope / `ref`
  对象 collector。
- AOT 与字节码磁盘缓存已确认不做；不冻结字节码 ABI。
- JIT 优化已暂停；OSR/deopt、per-pc liveness 与顶层声明类 JIT 等明确
  场景再重启。
- rope / 分块字符串表示已确认不做；透明 `s = s + x` 已完成，批量构建仍
  推荐 `std.string.builder`。
- 冷启动已完成 profiling、内存扫描、内嵌 std 包类型检查跳过、入口目录
  规范化、AST arena 和内存输入 fast path；没有明确的冷启动目标前，不再
  继续拆分解析输入或诊断行存储，也不单独做跨进程 AST 缓存。
- 不做延迟解析函数体、把 std 包改成动态插件、为解析速度裁剪 `std.web`
  API；预编译 stdlib 已确认不做（见 M5 / AOT 决策）。

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
