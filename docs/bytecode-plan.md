# Skein 字节码 VM 开发计划

本文是 `docs/bytecode.md` 的配套实施计划，记录剩余工作、任务拆分、
依赖关系、里程碑和验收标准。

- 更新日期：2026-11-05
- 当前基线：`78b319c` 之后的参数 fast path、函数体 AST 释放、默认参数
  AST 释放改动（尚未提交）
- 目标：把 VM 从“实验后端”推进为默认后端，并移除运行期 AST 依赖

## 1. 目标与非目标

### 1.1 目标

1. 常规语法在 VM 下 **零 AST 回退**。
2. `--vm` 成为默认执行路径，`--no-vm` 仅作为参考后端。
3. 函数调用密集场景不慢于 AST，局部变量密集场景明显快于 AST。
4. 运行期不再需要 `function_code->ast`、`symlist::default_value`、
   `symlist::pattern`；`EVAL_AST` 已移除。
5. 字节码可验证、可诊断、可基准回归，并为序列化/AOT 留出稳定格式。

### 1.2 非目标

- 不新增语言语法或标准库功能。
- 不改变现有错误文本、行列诊断和退出码。
- 不在本计划内重写 AST 解析器或运行时值模型。

## 2. 当前基线

### 2.1 已落地

- 栈式字节码、`Proto`、固定 8 字节 `Instr`、反汇编。
- 顶层语句按语句编译，函数体首次调用时编译并缓存。
- 局部变量 slot、参数 fast path、默认参数 Proto。
- `for-in` / `switch` / `match` / destructuring / `is` / `fail`。
- `on error` / `recover` / `pass` / `defer`。
- struct / enum / trait / impl / package / import。
- `await` / `yield`。
- 函数体 AST 与默认参数 AST 在编译成功后释放。

### 2.2 剩余执行路径 AST

| 字段 | 位置 | 说明 |
|---|---|---|
| `function_code->ast` | `src/runtime/valuetype.h` | 编译成功后释放；编译失败时保留 |
| `symlist::default_value` | `src/runtime/symbol.h` | 默认参数 Proto 编译成功后释放 |
| `symlist::pattern` | `src/runtime/symbol.h` | 编译成功后释放；失败时回退 AST |

### 2.3 基线测试与基准

```text
ctest: 7/7 passed (40.31s)
skein_tests:    Passed 12.04s
skein_vm_tests: Passed 12.11s
ASan/UBSan:     skein_vm_tests Passed 64.01s (detect_leaks=0)
LSan:           pattern / DeclDesc / keep-AST 定向用例通过 (detect_leaks=1)
SKEIN_VM_DEBUG: 全量测试 0 条 fallback
KEEP_AST:       SKEIN_VM=1 SKEIN_KEEP_AST=1 全量测试通过
Diagnostics:     30 个 AST/VM 差分用例 0 差异
```

| 基准 | AST | VM |
|---|---:|---:|
| 500 万次局部循环 | 1.20s | 1.01s |
| `fib(30)` | 0.69s | 0.79s |
| 200 万次函数调用 | 0.84s | 0.84s |
| 参数 pattern 密集调用 | 0.50s | 0.53s |

### 2.4 基线缺口

- 参数解构 pattern 已编译为 `PatternDesc`（W1-1 完成）。
- `EVAL_AST`、`ConstantKind::Ast` 与 `ConstantKind::Decl` 均已移除；
  标准测试集零 fallback（W1-2 / W4-1 完成）。
- 函数体 AST 在成功编译后可无条件释放；运行期 AST 只剩编译失败回退。
- VM 默认关闭。
- 每帧新建 `Executor`，函数调用使用 C++ 递归。
- 闭包捕获仍按名字查找，没有 upvalue 索引。
- handler / defer 是运行时动态结构，没有编译期表。
- 没有字节码 verifier、序列化、AOT 缓存。
- ASan 未开启泄漏检测。
- `function_code` 位于 L1 runtime，但持有 L2 的 `vm_proto` /
  `vm_param_patterns` 类型擦除缓存，属于已知分层软债务。

### 2.5 分层架构现状

硬边界仍然满足：

- L1 runtime 不 include L2 头文件；
- `libskein_runtime.a` 不引用 interpreter/builtins 定义；
- CMake include 目录按层收敛；
- `tools/check_boundaries.py` 通过。

软债务：

| 位置 | 问题 | 目标 |
|---|---|---|
| `function_code::vm_proto` | L1 结构体持有 L2 执行缓存 | W4-5 移入 L2 side table |
| `function_code::vm_param_patterns` | 同上，本轮 W1-1 新增 | W4-5 一并迁移 |
| `symlist::default_value` | L1 持有 L2 AST 指针 | W4-3 删除 |
| `symlist::pattern` | L1 持有 L2 AST 指针 | W4-3 删除 |

约束：在 W4-5 完成前，不得让 `skein_runtime` 直接实例化或销毁
`function_code` 的 VM 缓存；所有缓存释放必须继续经过
`free_function_code()` / `skein_vm_forget_function_code()`。

## 3. 任务分解

优先级：P0 必须优先完成；P1 为默认后端前置；P2 为性能与产品化；
P3 为工具链完善。规模为相对估算：S / M / L / XL。

### 3.1 W1：语义完整性

#### W1-1 参数 destructuring pattern 编译为 `PatternDesc`（P0，M）

状态：**已完成（2026-11-05）**。参数 pattern 在首次调用时编译并缓存到
`function_code`，由 `skein_vm_match_parameter_pattern()` 原生匹配；成功后
释放 `symlist::pattern`。

原现状：`src/interpreter/eval_call.cpp` 在绑定参数时调用
`match_pattern(sl->pattern, ...)`。

实施：

1. 在 `function_code` 中增加参数 pattern 的编译缓存，或在函数 Proto
   编译时把 pattern 写入 `Proto::patterns`。
2. 参数 Proto 记录每个参数的 `pattern_index` 与 `binding_slots`。
3. VM 调用入口在建立新帧后执行 pattern 匹配，把绑定值写入参数 slot。
4. 保留 AST 解释器路径，直到 W1-3 切换默认后端。
5. `symlistfree_full()` 继续负责未编译 pattern 的释放。

验收：

- 参数解构、嵌套解构、rest、default + pattern 组合通过。
- 参数 pattern 失败时错误文本与 AST 后端一致。
- `symlist::pattern` 在成功编译后可释放。
- 标准测试集在 VM 下不再因参数 pattern 触发 AST 执行。

#### W1-2 消除 `EVAL_AST` 的真实触发点（P0，L）

状态：**已完成（2026-11-05）**。

实施结果：

1. **复杂 lvalue**：确认 AST 语义本身拒绝非变量 base 的赋值
   （`f().x = 1`）。编译器不再回退，而是发射 `RUNTIME_ERROR`，
   保持原有错误文本：
   - `cannot assign through this expression`
   - `cannot assign to this expression`
   - `cannot increment/decrement this expression`
2. **list spread**：list 字面量语法本身不允许 spread；编译器遇到
   内部异常节点时显式失败，不再整条 list 回退。
3. **标签语句**：非循环/switch 的 label 通过 `RUNTIME_ERROR` 报告
   `label must be attached to a loop or switch`。
4. **未知节点**：`compile()` 的 `default` 改为显式编译失败。
5. 删除 `compile_fallback()`、`fallback_safe()`、`escapes_control()`；
   同时删除 `Op::EvalAst`、`ConstantKind::Ast`、`Constant::ast`。
6. 声明常量随后由 W4-1 迁移到 `Proto::decls`，`has_ast_constants` /
   `borrows_ast` 已删除。

验收结果：

- 标准测试集在 `SKEIN_VM=1 SKEIN_VM_DEBUG=1` 下 **0 条 fallback**。
- 新增复杂 lvalue 专项用例，覆盖 `=` / 复合赋值 / 自增。
- `skein_tests` 与 `skein_vm_tests` 均通过。
- 编译器不再生成任何 AST 执行指令。

#### W1-3 VM 设为默认后端（P1，M）

实施：

1. `vm.h` 默认值改为开启；`--no-vm` / `SKEIN_VM=0` 关闭。
2. 保留 `--vm` 为显式开启，便于脚本兼容。
3. `skein_tests` 继续跑 AST 后端，`skein_vm_tests` 跑 VM；
   新增默认后端测试，确保无参数启动即 VM。
4. 更新 README、`docs/bytecode.md`、帮助文本和测试说明。
5. 默认后端切换前必须完成 W1-1、W1-2、W1-4、W1-5。

验收：

- `./build/skein script.skein` 默认使用 VM。
- `--no-vm` 与 `SKEIN_VM=0` 仍能完整运行测试集。
- 两后端 stdout / stderr / exit code 一致。

#### W1-4 AST 生命周期与回退解耦（P1，M）

状态：**已完成（2026-11-05）**。

实施结果：

1. `function_code->ast` 默认在成功编译后释放；设置
   `SKEIN_KEEP_AST=1` 可保留函数体、默认参数和参数 pattern 的 AST。
2. 删除运行期切换后端的隐式假设：
   - 已有 `vm_proto` 的函数即使之后 `skein_vm_set_enabled(false)`，
     仍继续执行缓存的 Proto；
   - 禁用 VM 只影响尚未编译的函数；
   - 默认参数和参数 pattern 的缓存也遵循同样规则。
3. `function_code` 记录定义时的模块名 `module`。函数体、默认参数、
   参数 pattern 的懒编译都使用该模块，不再使用调用点模块；嵌套函数
   编译时继承父 Proto 的模块名。
4. `vm_compile_failed` 与 `function_code` 同生命周期；源码重新解析或
   函数重新定义会创建新的 `function_code`，自然清除失败缓存。
5. trait 元数据统一使用 `param->has_default`。
6. 审计嵌套函数、package、defer、default Proto 的 AST 释放路径；
   `SKEIN_KEEP_AST=1` 下全量测试通过。

验收结果：

- `SKEIN_VM=1`、`SKEIN_VM=1 SKEIN_KEEP_AST=1`、`SKEIN_VM=0`
  三套全量测试均通过。
- 编译失败后再次调用仍使用 AST 回退。
- 默认参数/参数 pattern AST 释放后，重复调用、递归调用、闭包调用均正确。
- LSan 在 `SKEIN_KEEP_AST=1` 下无泄漏。

#### W1-5 诊断一致性（P1，S）

状态：**已完成（2026-11-05）**。

实施结果：

1. 辅助指令源位置：
   - `CHECK_RESULT`、handler 清理、隐式 `POP`、`PUSH_SCOPE` /
     `POP_SCOPE` 已使用对应 AST 位置；
   - `CLEAR_SLOT` 也改为携带作用域退出位置；
   - `Proto::locs` 继续作为逐指令位置表。
2. 修正未捕获控制值的诊断位置：
   - AST 在控制值逃出语句后会恢复源码位置；
   - VM 现在在顶层语句返回未捕获 `fail` / `recover` 时同样清除
     `g_error_line` / `g_error_column`；
   - 因此 `pass`、未捕获 `fail`、defer 中失败等的行列与 AST 一致。
3. 新增 `tests/cases/diagnostics.py`，对以下场景断言精确行列：
   - 类型错误、未定义变量
   - 参数缺失/多余
   - match 失败
   - `pass` / `recover` / defer 失败
   - 参数 pattern、const 赋值
   - `break`、复杂 lvalue、非法 label

验收结果：

- 专项用例在 `SKEIN_VM=0` 与 `SKEIN_VM=1` 下均通过，stderr 行列一致。
- 额外 30 个手工差分用例：0 差异。
- 现有错误文本未改写。

#### W1-6 资源与泄漏审计（P1，M）

实施：

1. 增加 LSan 测试目标，至少覆盖 VM 标准测试集。
2. 审计 `throw_unwind` 路径上的 `value_t`、`handler_state`、
   `defer_record`、`iterator_state` 释放。
3. 审计 `Proto` 常量中的 `shared_ptr<value_t>` 与
   `shared_ptr<function_code>` 是否存在环。
4. 审计函数值、闭包、defer 捕获 scope、generator 挂起/关闭的
   生命周期。
5. 为 VM 栈、slot cell、param symbol 增加析构断言或调试统计。

验收：

- ASan + LSan + UBSan 下 `skein_vm_tests` 通过。
- 长循环、递归、generator 反复创建/关闭无持续增长。

### 3.2 W2：VM 架构

#### W2-1 Frame-based VM（P1，XL）

现状：`execute_proto()` 每次新建 `Executor`，函数调用通过 C++ 递归。

实施：

1. 定义共享 `Frame`：
   - `Proto *`
   - `pc`
   - 操作数栈基址/栈顶
   - 局部 slot 区
   - 参数区
   - handler / defer 记录
   - 返回地址与调用者帧
2. `CALL` 只压入新帧并跳转，`RETURN` 恢复调用者帧。
3. 把 `stack_`、`handlers_`、`defer_scopes_`、`iterators_` 从
   `Executor` 成员改为帧/执行上下文成员。
4. 去掉 `vm_depth_guard` 的 C++ 递归限制，改为显式帧深度上限。
5. 保留一个 `Executor` 作为整次顶层执行的调度器。

验收：

- `fib(30)` 不慢于 AST，目标 ≤ 0.70s。
- 递归深度可配置到 10000 而不爆 C++ 栈。
- 所有现有 VM 测试通过。
- `SKEIN_VM_MAX_DEPTH` 语义和错误文本保持兼容。

#### W2-2 真正的 upvalue 索引（P1，L）

实施：

1. 编译期捕获分析：标记每个函数的自由变量。
2. `Proto` 增加 `upvalues` 描述（来自父局部 slot 或父 upvalue）。
3. `CLOSURE` 创建捕获环境；`Proto` 增加
   `LOAD_UPVALUE` / `STORE_UPVALUE` / `COMPOUND_UPVALUE` /
   `INCDEC_UPVALUE`。
4. 支持 open upvalue（指向栈/帧）与 closed upvalue（堆 cell）。
5. 与 defer、match binding、destructuring、参数 pattern 的
   slot 生命周期对齐。

验收：

- 闭包读写捕获变量不再走名字查找。
- 多层闭包、循环中创建闭包、defer 捕获变量语义正确。
- 现有闭包测试全部通过。

#### W2-3 Handler / defer 表（P1，L）

实施：

1. 编译期为每个 Proto 生成 handler table：
   - 起止 pc
   - handler 目标
   - 栈/作用域深度
   - fail/recover 类型
2. 生成 defer table，defer 记录绑定帧与作用域退出点。
3. 用显式 unwind 循环替代跨帧 `throw_unwind`。
4. `recover` / `pass` / `break` / `continue` / `return` 统一走
   unwind 结果。
5. 保留现有错误文本与 handler 深度语义。

验收：

- 嵌套 `on error`、defer 中 fail、generator close 全部通过。
- handler/defer 不再依赖每帧 `std::vector<std::vector<...>>`。

#### W2-4 Generator / async 显式状态（P2，L）

现状：依赖 `boost::context` 原生 2MB 栈保存 C++ 栈。

实施：

1. frame-based VM 后，把挂起点保存为显式帧栈。
2. generator 保存 pc、帧、handler/defer 状态。
3. `await` 挂起只保存必要状态，恢复时重建执行循环。
4. 评估是否减少或移除原生 fiber 栈依赖。

验收：

- generator 创建/挂起/恢复/关闭不依赖 C++ 递归深度。
- 现有 generator/async 测试通过。
- 挂起状态可被调试工具枚举。

#### W2-5 字节码 verifier（P1，M）

实施：

1. 新增 `verify(const Proto &)`，在编译后、执行前调用。
2. 验证：
   - 跳转目标在范围内
   - 栈深度不超过 `max_stack`
   - 不出现栈下溢
   - slot / 常量 / descriptor 索引合法
   - `CALL` 参数个数与 `CALL_SPREAD` 描述一致
   - handler / defer 嵌套合法
   - `DeclDesc` / slot / pattern descriptor 合法
   - Proto 图无非法环
3. 失败时输出稳定错误文本，不执行半成品字节码。
4. 为 verifier 增加负向测试。

验收：

- 所有正常 Proto 通过验证。
- 手工破坏的 Proto 被拒绝且不崩溃。
- 运行时 `invalid bytecode ...` 检查显著减少或只作为断言。

### 3.3 W3：性能

#### W3-1 运算与比较快路径（P2，M）

- `Add` / `Sub` / `Mul` / `Div` / `Mod` 对 int/float 直接内联。
- `CmpLt` / `CmpLe` / `CmpGt` / `CmpGe` 对 int/float/string 快路径。
- 避免所有二元运算都走 `apply_binary_value()` 字符分派。

验收：局部循环与 `fib` 基准均不退化。

#### W3-2 全局 slot 与内联缓存（P2，L）

- 模块级全局符号槽位表。
- `LOAD_GLOBAL` / `STORE_GLOBAL`。
- per-call-site 名字缓存，避免 `savesym` 后整表失效。
- 字段/索引/方法调用 IC。
- 增加 IC 命中率统计。

验收：调用密集基准达到或超过 AST。

#### W3-3 值表示与分配（P2，XL）

- tagged value 或 NaN boxing。
- 帧内值栈不强制堆分配。
- 小整数/布尔内联。
- 对象分配器/arena。
- 保持循环引用检查和现有值语义。

验收：基准提升且内存占用不显著上升。

#### W3-4 指令优化（P2，M）

- 常量折叠。
- 死代码消除。
- 跳转线程化。
- `INC_SLOT`、`CMP_LT_JUMP` 等超指令。
- computed goto 或 table dispatch。
- 评估宽指令格式，解决 16 位操作数上限。

验收：优化可开关，关闭时语义一致；基准有可量化收益。

### 3.4 W4：运行期 AST、元数据与分层清理

#### W4-1 完成声明常量去 AST（P1，M）

状态：**已完成（2026-11-05）**。

实施结果：

1. 删除 `compile_fallback()`、`fallback_safe()`、`escapes_control()`；
2. 删除 `Op::EvalAst`、`ConstantKind::Ast`、`Constant::ast`；
3. `compile()` 对未覆盖节点显式失败；
4. 新增 `DeclDesc`：

   ```cpp
   struct DeclDesc {
       std::string name;
       type_constraint type;
       bool exported = false;
       bool is_const = false;
   };
   ```

5. `add_decl()` 复制 `symdef` 元数据，不再保存 `symdef*`；
6. `DefineName` 从 `Proto::decls` 读取名称、类型、`const` / `exported`；
7. 删除 `ConstantKind::Decl`、`Constant::decl`、`Proto::borrows_ast`；
8. 函数体 AST 在成功编译后无条件释放。

验收结果：

- `Proto` 常量不再包含任何 `ast*` / `symdef*` 借用指针。
- 函数体 AST 在成功编译后无条件释放。
- VM 标准测试集零 fallback；`--no-vm` 仍全量通过。
- ASan/UBSan 与 LSan 定向用例通过。

#### W4-2 移除 `function_code` 的运行期 AST（P1，M）

前置：W4-1、W2-2。

- 默认构建不保留 `function_code->ast`。
- 调试构建可通过 `SKEIN_KEEP_AST=1` 保留。
- `free_function_code()` 只释放 Proto、参数元数据和调试信息。
- 评估把 `function_code` 移出 runtime 层，或改成纯 pimpl。
- 与 W4-5 协同：`function_code` 最终只保留签名与调试元数据。

验收：`valuetype.h` 不再依赖 `struct ast` 作为执行状态。

#### W4-3 参数与 default 元数据 Proto 化（P2，M）

- 参数 pattern 使用 `PatternDesc`（W1-1）。
- 默认参数使用 Proto，删除 `symlist::default_value`。
- `symlist` 只保留签名元数据。
- trait 参数元数据只保留名称/类型/可变参数/是否有默认值。

验收：运行期不再出现 `default_value` / `pattern` 的 AST 指针。

#### W4-4 调试信息与 source map（P3，M）

- 压缩 `Proto::locs` 为行表/列增量。
- 保留函数名、参数名、slot 名用于调试。
- VM 栈回溯输出函数名、文件、行列。
- 与 AST 后端诊断格式对齐。

#### W4-5 把 VM 执行缓存移出 runtime（P1，M）

现状：`function_code` 属于 L1 runtime，但持有 L2 的执行缓存：

```cpp
std::shared_ptr<const void> vm_proto;
std::shared_ptr<const void> vm_param_patterns;
```

这不会产生静态链接反向依赖，但让 L1 结构体知道 VM 的执行模型，属于
分层软债务。

实施：

1. 在 `vm.cpp` 建立 L2 side table：

   ```cpp
   std::unordered_map<function_code *, function_execution_cache> caches;
   ```

2. `function_execution_cache` 持有：
   - 函数体 `Proto`
   - 参数 pattern 编译结果
   - 默认参数 Proto
   - 编译失败标记
3. `skein_vm_forget_function_code()` 清理 side table；`free_function_code()`
   继续作为唯一释放入口。
4. 从 `function_code` 删除 `vm_proto` 与 `vm_param_patterns`。
5. 审计函数值生命周期：`function_code*` 作为 key 必须在其释放前移除。
6. 增加回归检查：只链接 `skein_runtime` 时不出现 interpreter 符号。

验收：

- `valuetype.h` 不再包含任何 VM 执行缓存字段。
- `boundary_check` 与 runtime-only 链接检查通过。
- VM 标准测试集通过。
- 释放函数后 side table 无残留条目。

### 3.5 W5：工具链与产品化

#### W5-1 VM 统计（P2，S）

新增 `--vm-stats` 或 `SKEIN_VM_STATS=1`：

- 编译语句/函数数
- AST fallback 次数与原因
- opcode 直方图
- 最大栈深/最大 handler 深度
- 调用次数、IC 命中率
- 编译耗时/执行耗时

#### W5-2 差分测试与模糊测试（P2，L）

- 随机生成表达式/语句，比较 AST 与 VM 输出。
- 覆盖错误路径、边界溢出、空容器、generator。
- 每个 opcode 至少一个直接或间接用例。
- 增加 fallback 原因断言，防止覆盖回退。

#### W5-3 基准回归（P2，M）

- 把 `slot_bench`、`fib_bench`、`call_bench` 纳入仓库。
- 使用多次运行中位数，记录方差。
- 设置性能回归阈值（例如不超过基线 10%）。
- 区分冷启动、首次编译、稳态执行。

#### W5-4 序列化与 AOT 缓存（P3，XL）

- 稳定字节码格式与版本号。
- 字符串表、常量表、源码位置表、import/module 描述。
- 替换不可序列化的 `shared_ptr<value_t>` / `function_code`。
- 源码哈希 + 编译器版本缓存键。
- 磁盘缓存与预编译 stdlib。
- 缓存损坏时安全回退到源码编译。

#### W5-5 CI 与 sanitizer（P2，M）

- ASan + LSan + UBSan 组合。
- Debug / Release 两套构建。
- 可选 `-D_GLIBCXX_DEBUG`。
- generator/fiber 路径单独配置栈大小。
- runtime-only 链接检查：`skein_runtime` 不引用 interpreter/builtins 符号。
- 文档同步与边界检查继续作为必跑项。

#### W5-6 文档维护（P2，S）

- `docs/bytecode.md` 与本文同步更新。
- README 的 VM 使用说明更新为默认后端。
- 记录每个里程碑的基准与测试结果。
- 明确剩余限制，避免“已全量原生”的误解。

## 4. 里程碑

### M1：语义收口

任务：W1-1、W1-2、W1-4、W1-5、W1-6

退出标准：

- 参数 pattern 原生执行。
- 标准测试集零 `EVAL_AST`。
- 诊断差分与泄漏审计通过。
- 仍保留 AST 默认后端，便于对照。

### M2：Frame VM

任务：W2-1、W2-3、W2-5

退出标准：

- 每帧不再新建 `Executor`。
- 递归深度可达 10000。
- handler/defer 使用编译期表。
- verifier 接入编译流程。
- `fib(30)` 不慢于 AST。

### M3：闭包与协程

任务：W2-2、W2-4

退出标准：

- 闭包通过 upvalue 索引访问。
- generator/async 状态显式化。
- 多层闭包与 defer 捕获测试通过。

### M4：性能

任务：W3-1、W3-2、W3-3、W3-4

退出标准：

- 三个基线基准均达到目标。
- 性能回归测试纳入 CI。
- 关闭优化时语义一致。

### M5：默认后端与 AST 清理

任务：W1-3、W4-1、W4-2、W4-3、W4-5

退出标准：

- 默认启动即 VM。
- 运行期不再需要函数体/默认参数/参数 pattern AST。
- `function_code` 不再持有 VM 执行缓存，L1 可独立链接。
- `--no-vm` 仍全量通过。

### M6：产品化

任务：W5-1 至 W5-6

退出标准：

- 可验证、可统计、可序列化。
- 文档、基准、CI 完整。
- 发布构建默认启用 VM。

## 5. 验收矩阵

| 维度 | 标准 |
|---|---|
| 功能 | 标准测试集 AST/VM 全通过 |
| 差分 | stdout / stderr / exit code 完全一致 |
| 覆盖 | VM 标准测试集 0 次 AST fallback |
| 诊断 | 错误文本与行列不变 |
| 性能 | 局部循环明显快于 AST；调用密集不慢于 AST |
| 健壮性 | verifier 拒绝非法 Proto |
| 内存 | ASan + LSan + UBSan 通过 |
| 分层 | `boundary_check` 通过；L1 runtime 不持有 L2 执行缓存 |
| 文档 | `docs_sync` / `boundary_check` / `git diff --check` 通过 |
| 可维护性 | 新 opcode / 新节点必须有测试与文档 |

## 6. 风险与应对

| 风险 | 影响 | 应对 |
|---|---|---|
| 释放 AST 后仍有隐式回退 | 运行期崩溃 | W1-4 生命周期审计 + `--no-vm` 测试 |
| `Proto` 与 `function_code` 循环引用 | 内存泄漏 | LSan + 弱引用/显式清理 |
| frame VM 改动面大 | 回归难定位 | 分阶段：先帧化调用，再帧化 handler/defer |
| upvalue 与现有 scope 语义冲突 | 闭包行为变化 | 保留 AST 差分测试，逐项切换 |
| 性能优化破坏错误文本 | 兼容性下降 | 优化开关 + 差分测试 |
| generator 依赖原生栈 | 资源开销高 | 先统计，再决定是否显式化 |
| 字节码格式未稳定就做 AOT | 缓存失效/兼容问题 | 放到 M6，先完成 verifier |
| L1 runtime 持有 L2 执行缓存 | 嵌入/AOT 时职责混乱 | W4-5 迁移到 L2 side table |
| side table 使用裸 `function_code*` | 释放后悬空/残留 | 统一由 `skein_vm_forget_function_code()` 清理并加断言 |

## 7. 任务跟踪

| 任务 | 优先级 | 规模 | 状态 | 依赖 |
|---|---|---|---|---|
| W1-1 参数 pattern | P0 | M | 完成（2026-11-05） | - |
| W1-2 消除 EVAL_AST | P0 | L | 完成（2026-11-05） | - |
| W1-3 VM 默认后端 | P1 | M | TODO | W1-1/W1-2/W1-4/W1-5 |
| W1-4 AST 生命周期 | P1 | M | 完成（2026-11-05） | - |
| W1-5 诊断一致性 | P1 | S | 完成（2026-11-05） | - |
| W1-6 资源泄漏审计 | P1 | M | TODO | - |
| W2-1 Frame VM | P1 | XL | TODO | W1-2 |
| W2-2 Upvalue | P1 | L | TODO | W2-1 |
| W2-3 Handler/defer 表 | P1 | L | TODO | W2-1 |
| W2-4 Generator 状态 | P2 | L | TODO | W2-1 |
| W2-5 Verifier | P1 | M | TODO | W1-2 |
| W3-1 运算快路径 | P2 | M | TODO | W2-1 |
| W3-2 全局 slot + IC | P2 | L | TODO | W2-1 |
| W3-3 值表示 | P2 | XL | TODO | W2-1 |
| W3-4 指令优化 | P2 | M | TODO | W3-1 |
| W4-1 声明常量去 AST | P1 | M | 完成（2026-11-05） | W1-2 |
| W4-2 移除 function AST | P1 | M | TODO | W4-1/W2-2 |
| W4-3 参数元数据 Proto 化 | P2 | M | TODO | W1-1 |
| W4-4 调试信息 | P3 | M | TODO | W2-1 |
| W4-5 VM 缓存移出 runtime | P1 | M | TODO | W2-1/W4-2 |
| W5-1 VM 统计 | P2 | S | TODO | - |
| W5-2 差分/模糊测试 | P2 | L | TODO | W1-2 |
| W5-3 基准回归 | P2 | M | TODO | W2-1 |
| W5-4 序列化/AOT | P3 | XL | TODO | W2-5/W4 |
| W5-5 CI/Sanitizer | P2 | M | TODO | - |
| W5-6 文档维护 | P2 | S | 进行中 | 各里程碑 |

状态约定：`TODO` / `进行中` / `阻塞` / `完成`。每个任务完成时同步更新
本表和 `docs/bytecode.md`。
