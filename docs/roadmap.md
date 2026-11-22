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
- **待决策**：需要产品/架构选择后才能排期。
- **暂缓**：明确不作为近期目标。

---

## 1. 已完成基线

### 语言与运行时

- 词法、语法、类型检查、AST/VM 双后端
- `pair` / `tuple`：字面量要求 `(a, b)`，`(a, b)` 可用于 let/const、函数参数和 match；带注解的值在运行时校验不可变性与 arity
- `dict` / `set` 支持 float key：NaN 拒绝，`-0.0` 归一化，整数值 float 与同值 int 等价
- 10 种复合赋值运算符；静态检查按运算符推导结果类型，JIT 覆盖 param / index 目标
- task / generator、线程、Channel
- HTTP/1.1、HTTP/2、WebSocket、TLS、FFI
- Unicode 标识符、NFC、归一化 quick-check、完整大小写映射与折叠、常见源文件编码
- GBK / GB18030-2005 编解码
- `error` 值保留抛出点位置与调用栈，支持 `std.error.location` / `trace` / `format_trace`
- `std.log` 结构化日志：级别过滤、text/JSON、stderr/stdout/file、自定义 writer

### 工具链

- `skein lint`
- `skein fmt`
- `skein lsp --stdio`
- VS Code / Vim / Neovim 接入
- diagnostics / completion / hover / definition / document symbol 基础能力

### 包管理

- `skein.mod` / `skein.sum`
- `skein get` / `skein build`
- MVS、菱形依赖、`/vN`
- checksum 硬校验、`--force`
- 项目锁、缓存锁、模块锁
- `mod clean` / `mod verify` / `mod download` / `mod tidy`
- `get --offline`
- 原子写入 `skein.mod` / `skein.sum`
- 运行时模块加载持有缓存共享锁

### REPL

- 历史、行编辑、Windows 控制台编辑
- 词法高亮
- Tab 补全：关键字 / REPL 命令 / 全局符号 / `std.*`
- 自动缩进、闭合符回退

### JIT / VM

- baseline JIT
- Win64 ABI
- GC root map 第一版
- `SKEIN_GC_STRESS`
- 尾调用 trampoline

### Native / FFI

- `std.ffi`
- plugin async ABI v2
- source-native module MVP
- `SKEIN_ALLOW_NATIVE`
- check-only 模式下禁止加载/执行 native

---

## 2. 里程碑

### Milestone 1：包管理安全与运维（进行中）

目标：让包管理在长期运行服务、并发 CI 和 native 缓存场景下安全可运维。

范围：

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

### Milestone 2：REPL / LSP 体验统一

目标：REPL 与 LSP 共享补全数据源和编辑体验。

范围：

1. 共享 `completion_index`（已完成）
2. REPL struct 字段补全（已完成）
3. REPL `:load` 路径补全（已完成）
4. 长行水平滚动（已完成）
5. 运算符/逗号 continuation 缩进（已完成）
6. LSP incremental sync / debounce（已完成）
7. resolution-only import 校验（已完成，基于 skein.mod + 缓存 manifest）

验收：

- `user.<Tab>` 能列出字段
- `:load <Tab>` 能列出文件
- 长行编辑光标位置正确
- LSP 不再每次全量同步文档

### Milestone 3：包生态与信任

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

### Milestone 4：JIT / VM 性能与 GC

前提：先决定 moving GC、AOT、字节码序列化是否在目标范围内。

已确认的优化点：

- benchmark 改为进程内稳态计时（`SKEIN_BENCH_MS`），排除启动和 JIT 编译
- 函数调用帧不再复制函数名/源路径（改为 intern 指针）
- JIT 在 Linux 与 Windows 上收益一致（fib 约 5x，overall 1.5–3.5x）
- `obj.method(args)` 使用 `CALL_METHOD` 快路径，VM 的 method 调用已快于 AST
- `std.list.builder` / `std.string.builder`，以及 `xs = std.list.append(xs, v)` 的 COW 原地增长
- JIT 运行时默认启用（`SKEIN_JIT=0` 可关闭；`SKEIN_JIT_TOP_LEVEL=0`
  关闭顶层语句 JIT；`SKEIN_JIT_THRESHOLD` 默认 100 次调用）
- 含回边（循环）的函数在首次调用即编译，不再等调用次数阈值；
  `SKEIN_JIT_LOOPS=0` 可关闭该策略
- JIT 支持闭包 / upvalue 访问（`LOAD/STORE/COMPOUND/INCDEC_UPVALUE`）
  与闭包创建（`CLOSURE` + cell 生命周期）
- JIT 支持名字模式局部变量（`*_NAME`、`DEFINE_NAME`、`CHECK_REDEFINE`）
  与 `for-in` 迭代器（`ITER_NEW/NEXT/END`，含 list/dict/string/bytes/
  deque/heap/set/generator），并放开 Slot/Param/Upvalue base 的
  `ASSIGN_INDEX`、值比较、`DUP`、`GET_FIELD_OR_ENUM` 与 `FAIL`
- JIT 支持 `on error` handler（`PUSH/POP_HANDLER`、`CHECK_RESULT`、
  `BIND_NAME`、`ENTER/LEAVE_HANDLER_BODY`、`PASS`、`RECOVER`）
- JIT 支持 `defer`（`DEFER_PUSH`、块级 defer 作用域、返回/fail/abort 时
  执行，捕获 Slot/Param/Upvalue）；顶层与 handler+defer 组合仍回退 VM
- JIT 支持常规顶层语句（`IMPORT`、`BIND_FUNC`、循环、调用等）；
  `DECLARE_PACKAGE`/`DECLARE_STRUCT`/`DECLARE_ENUM` 与顶层 handler/defer
  仍回退 VM
- builtin 回调（map/filter/sort/reduce/compose/task）通过 `invoke_value`
  直接进入 JIT，不再因为通用调用路径绕过 JIT

若暂不做 moving GC：

- 只做 GC root 边界审计（已完成，见下）
- 继续优化 frame 分配与热点 opcode（已完成，见下）
- per-pc liveness 不阻塞主线

#### 热点 opcode / frame 分配（已完成）

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

#### GC root 边界审计（已完成）

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

若重启 JIT / GC 优化：

- per-pc liveness
- safepoint 表
- frame reuse / 零帧分配
- OSR / deopt（Handler JIT 已完成）
- 类型特化 / unboxed int / 单态字段与调用缓存内联
- 如仍有收益，再评估 tracing
- asmjit `x86::Compiler` 的 prologue / regalloc 能力

验收：

- `SKEIN_GC_STRESS` 下无漏根
- JIT/VM 差分测试稳定
- 有明确性能回归门槛

#### 字符串增量拼接（路线 1，已完成）

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

后续可选：

- **rope / 分块字符串表示**：能让所有拼接/切片 O(1)，但会波及 `len`、下标、
  `find`、序列化、FFI 和 Unicode 索引；除非同时规划 moving GC / 值表示重构，
  否则不做。

风险与约束：

- `value` 是缓存指针；任何原地修改都必须同步所有已知共享者
- 字符串会进入序列化、FFI 和跨线程边界，表示变更影响面大
- AST 的 COW 阈值与 VM 不同（无栈副本时只允许 1 个 owner）

### Milestone 5：长尾能力

按实际需求推进：

- GB18030-2022
- Big5 / Shift_JIS / EUC-JP 等旧编码后端
- Unicode 完整大小写映射与 case folding（已完成）/ grapheme cluster
- HTTP/2 extended CONNECT
- vendor / workspace
- AOT / 字节码序列化、预编译 stdlib、字节码 ABI 冻结

#### REPL / 工具链体验（按需）

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

## 3. 待决策事项

1. 是否把 Skein 包生态做成公共生态？
2. 是否引入 moving / compacting GC？
3. 是否需要 AOT 或字节码磁盘缓存？
4. ~~`SKEIN_JIT` 运行时是否默认开启？~~ 已决定：默认开启（JIT 构建），
   `SKEIN_JIT=0` 保留 kill switch。
5. Windows/MinGW 是否进入正式发布范围？
6. 是否支持 vendor 和多模块 workspace？
7. 私有模块认证与 proxy 鉴权做到什么程度？
8. 是否需要 rope / 分块字符串表示来覆盖所有拼接与切片（路线 2，暂不做）？
9. ~~是否严格化 pair/tuple 字面量并引入 `(a, b)` 解构模式？~~ 已实施：
   pair/tuple 字面量要求 `(a, b)`，`(a, b)` 可用于 let/const、函数参数和
   match；运行时同时校验不可变性与 arity。`[a, b]` 保持宽松的依据是结构化
   读取：它只按位置读取元素并创建新绑定，不改写源 tuple；因此
   `let l: list[int] = (1, 2);` 仍被 checker 拒绝。
10. 是否引入 `wait t` 前缀写法并统一上下文相关词法？方案要点：
    - 语义：`wait e == e.wait()`，要求 `e: task[T]`，结果为 `T`；只等待已有
      task，不 launch、不恢复 `async/await`、不引入函数染色。
      `wait launch f()` 合法且等价于 `(launch f()).wait()`；`wait f()` 在
      `f()` 不返回 task 时是类型错误。
    - 词法：`.` 后的关键字一律作为 `NAME` 返回，适用于 `wait` 及现有
      `match` / `struct` / `enum` / `yield` 等；非点号分支保留原有 header
      context 与 declaration skip 副作用。这样 `t.wait()`、`std.task.wait`、
      变体名、import 路径无需逐个打补丁。
    - 语法：parser 新增 `WAIT exp %prec UMINUS`，在 parse 阶段 desugar 成
      `.wait()` 成员调用；不改 typecheck、VM、JIT、ABI。
    - 兼容：`wait` 在 `.` 之外成为保留字；当前仓库无脚本将其用作独立标识符。
      编辑器高亮需同步加入 `wait`。
    - 验证：`wait t` / `wait launch f()` / 非 task 操作数错误诊断、点号后
      关键字回归、AST/VM 差分与 JIT task-wait smoke。

---

## 4. 暂缓事项

- 没有可信镜像运营方前，不先做完整 proxy。
- 没有明确性能目标前，不引入 moving GC。
- JIT 优化已暂停（2026-11-20，见第 6 节），Handler JIT 已完成；
  OSR/deopt、per-pc liveness 与顶层声明类 JIT 等明确场景再重启。
- 没有实际用户需求前，不扩展旧编码后端。
- 未确认公共生态前，不先做 vendor / workspace。
- 没有明确的全量拼接/切片需求前，不引入 rope / 分块字符串表示；透明 `s = s + x` 已完成，批量构建仍推荐 `std.string.builder`。

---

## 5. 推荐执行顺序

1. Milestone 1 只剩 macOS `flock` 验证（暂缓）和交叉编译（按需）。
2. Milestone 2 已完成。
3. 先确认公共包生态目标，再决定 Milestone 3 的 SumDB / proxy 范围。
4. Milestone 4 的 GC root 审计、热点 opcode / frame 分配与 JIT 默认启用
   已完成；剩余是 moving GC / AOT / 字节码缓存的决策，以及移动 GC 决策后
   才会启动的 per-pc liveness / OSR / deopt。
5. 测试稳定性已完成：
   - 服务端测试改为子进程内绑定端口 0 并回读实际端口，去掉
     probe-then-bind 竞态和定时就绪探测
   - `thread_bench` 门槛改用多次重复中的最好成绩，基线按实测重设
6. 长尾能力按实际需求触发。

---

## 6. 性能看板（优化冻结后）

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
```

### JIT 优化暂停（2026-11-20）

JIT 覆盖率 / 性能工作暂时冻结，遇到具体场景再重启。

冻结时状态：

- JIT 默认开启；`SKEIN_JIT=0` 可完全关闭
- 测试基线：Windows JIT 35/35、Windows 非 JIT 33/33、WSL 35/35，0 警告
- 已覆盖：函数、闭包 / upvalue、名字模式、`for-in` 迭代器、enum、
  `fail`、`on error` handler、`defer`、常规顶层语句（`IMPORT` /
  `BIND_FUNC` / 循环 / 调用）、builtin 回调（map/filter/sort/reduce/
  compose/task）
- 明确保留 VM、不再主动实现：
  - 顶层 `DECLARE_PACKAGE` / `DECLARE_STRUCT` / `DECLARE_ENUM`
  - 顶层 handler / defer
  - variadic / generator 等边缘函数
  - OSR / deopt、per-pc liveness

  理由：声明与顶层 handler/defer 都是一次性元数据 / 初始化操作，没有热
  路径；JIT 编译成本高于执行收益，且顶层 `global scope` 与 handler 记账
  更容易引入语义回归。

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
`[jit] unsupported op` / `reject` 与 `statements compiled`，再对照本文件 JIT
章节的覆盖与缺口决定是否值得投入。

已知既有问题（不是本次 JIT 闭包支持引入）：在单条顶层语句内的长循环中创建
闭包时，`scope ↔ closure` 环只能等 cycle collector 在语句边界回收，循环
期间 `all_scopes` 持续增长。`register_scope` 原先固定在 4096 触发全量 prune，
导致每次 push 都扫描整个列表，表现为二次复杂度（40k 次闭包创建约 2.2s）。
现已把 prune 触发阈值改为几何增长（每次扫描后取 `max(4096, size * 2)`），
40k 次闭包创建降到约 36ms，10k/20k/40k 呈线性（VM 与 AST 相同）。
残留：单条语句内的峰值内存仍随累计闭包数线性增长，后续可在循环回边加入
低频 cycle collection 来收敛。
