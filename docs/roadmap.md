# Skein 总路线图

本文是当前唯一的总路线图。详细技术设计仍保留在专项文档中：

- [`package-manager.md`](package-manager.md)
- [`repl-plan.md`](repl-plan.md)
- [`tooling-plan.md`](tooling-plan.md)
- [`jit-plan.md`](jit-plan.md)
- [`bytecode.md`](bytecode.md)
- [`encoding.md`](encoding.md)
- [`unicode.md`](unicode.md)

## 状态说明

- **已完成**：已经落地并有测试覆盖，不再作为主线计划。
- **进行中**：当前正在推进。
- **待决策**：需要产品/架构选择后才能排期。
- **暂缓**：明确不作为近期目标。

---

## 1. 已完成基线

### 语言与运行时

- 词法、语法、类型检查、AST/VM 双后端
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

若暂不做 moving GC：

- 只做 GC root 边界审计
- 继续优化 frame 分配与热点 opcode
- per-pc liveness 不阻塞主线

若计划 moving GC 或 OSR/deopt：

- per-pc liveness
- safepoint 表
- frame reuse
- Handler JIT / OSR / deopt

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
- AOT / 字节码序列化

---

## 3. 待决策事项

1. 是否把 Skein 包生态做成公共生态？
2. 是否引入 moving / compacting GC？
3. 是否需要 AOT 或字节码磁盘缓存？
4. `SKEIN_JIT` 运行时是否默认开启？
5. Windows/MinGW 是否进入正式发布范围？
6. 是否支持 vendor 和多模块 workspace？
7. 私有模块认证与 proxy 鉴权做到什么程度？
8. 是否需要 rope / 分块字符串表示来覆盖所有拼接与切片（路线 2，暂不做）？

---

## 4. 暂缓事项

- 没有可信镜像运营方前，不先做完整 proxy。
- 没有明确性能目标前，不引入 moving GC。
- GC root map 稳定前，不做 Handler JIT / OSR / deopt。
- 没有实际用户需求前，不扩展旧编码后端。
- 未确认公共生态前，不先做 vendor / workspace。
- 没有明确的全量拼接/切片需求前，不引入 rope / 分块字符串表示；透明 `s = s + x` 已完成，批量构建仍推荐 `std.string.builder`。

---

## 5. 推荐执行顺序

1. Milestone 1 只剩 macOS `flock` 验证（暂缓）和交叉编译（按需）。
2. Milestone 2 已完成。
3. 先确认公共包生态目标，再决定 Milestone 3 的 SumDB / proxy 范围。
4. Milestone 4 先做决策（moving GC / AOT / JIT 默认开启），当前优先：
   - GC root 边界审计
   - 热点 opcode 与 frame 分配
5. 测试稳定性已完成：
   - 服务端测试改为子进程内绑定端口 0 并回读实际端口，去掉
     probe-then-bind 竞态和定时就绪探测
   - `thread_bench` 门槛改用多次重复中的最好成绩，基线按实测重设
6. 长尾能力按实际需求触发。
