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
- Unicode 标识符、NFC、常见源文件编码
- GBK / GB18030-2005 编解码

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
   - 构建时写入 source/artifact hash、编译器和编译器版本
   - 加载前校验 source/artifact hash
   - 编译器可用时检测版本变化
   - 编译 flags 的显式元数据仍待补充
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
4. 长行水平滚动
5. 运算符/逗号 continuation 缩进
6. LSP incremental sync / debounce
7. resolution-only import 校验

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

### Milestone 5：长尾能力

按实际需求推进：

- GB18030-2022
- Big5 / Shift_JIS / EUC-JP 等旧编码后端
- Unicode quick-check / case folding / grapheme
- HTTP/2 extended CONNECT
- `std.net` TLS / HTTP
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

---

## 4. 暂缓事项

- 没有可信镜像运营方前，不先做完整 proxy。
- 没有明确性能目标前，不引入 moving GC。
- GC root map 稳定前，不做 Handler JIT / OSR / deopt。
- 没有实际用户需求前，不扩展旧编码后端。
- 未确认公共生态前，不先做 vendor / workspace。

---

## 5. 推荐执行顺序

1. 完成 Milestone 1 的 native 产物完整性与 `mod prune` 安全设计。
2. 推进 Milestone 2 的共享补全索引与 LSP incremental sync。
3. 明确包生态目标，再决定 Milestone 3 的范围。
4. 明确 GC/AOT 决策，再进入 Milestone 4。
5. 长尾能力按需求触发。
