# Skein 字节码 VM 开发计划

本文是 `docs/bytecode.md` 的配套实施计划，记录剩余工作、任务拆分、
依赖关系、里程碑和验收标准。

- 更新日期：2026-11-05
- 当前基线：`4e29575`；工作区另有未提交的 N1/N2 改动
- 目标：把 VM 从“实验后端”推进为默认后端，并移除运行期 AST 依赖
- 当前阶段：M1 完成；未完成部分按 §8 的 N1–N4 重排执行

## 1. 目标与非目标

### 1.1 目标

1. 常规语法在 VM 下 **零 AST 回退**。
2. VM 默认启用，`--no-vm` 作为参考后端保留。
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
- fast frame：普通函数直接压帧，支持 handler / defer / iterator。
- 尾调用优化：无 spread 的尾调用复用 fast frame。
- symbol-cell upvalue：闭包通过索引访问捕获变量。
- `for-in` / `switch` / `match` / destructuring / `is` / `fail`。
- `on error` / `recover` / `pass` / `defer`。
- struct / enum / trait / impl / package / import。
- `await` / `yield`。
- 字节码 verifier 与 `Proto::max_stack`。
- 全局名字 IC、struct/enum 字段索引表。
- 运算/比较快路径、`CMP_JUMP`、安全常量折叠。
- 函数体 AST、默认参数 AST、参数 pattern AST 在编译成功后释放。
- VM 统计、随机差分测试、基准回归测试。
- VM 执行缓存已迁移到 interpreter side table。

### 2.2 剩余执行路径 AST

| 字段 | 位置 | 说明 |
|---|---|---|
| `function_bodies` | `src/interpreter/ast.cpp` | L2 side table；编译成功后释放，失败时供 AST 回退 |
| `symlist_metadata_map` | `src/interpreter/ast.cpp` | L2 side table；保存默认参数 / 参数 pattern AST |

### 2.3 基线测试与基准

```text
ctest: 11/11 passed (52.49s)
bytecode_verifier_test: Passed 0.00s
skein_tests:    Passed 12.14s (SKEIN_VM=0)
skein_vm_tests: Passed 12.24s (SKEIN_VM=1)
vm_differential_test: Passed 1.17s (160 cases)
vm_bench_test:  Passed 10.73s
ASan/UBSan:     skein_vm_tests 67.21s
LSan:           skein_vm_lsan_tests Passed 80.31s
SKEIN_VM_ONLY:  11/11 passed (49.12s)
tools/check_all: clean pass (Debug/Release/ASan+UBSan/LSan/VM-only)
glibcxx-debug:  4/4 core tests passed
SKEIN_VM_DEBUG: 全量测试 0 条 fallback
KEEP_AST:       SKEIN_VM=1 SKEIN_KEEP_AST=1 全量测试通过
Diagnostics:     30 个 AST/VM 差分用例 0 差异
Fuzz:          seed 1/2/3 各 400 例 + 5s fuzz 745 例通过
```

| 基准 | AST | VM |
|---|---:|---:|
| 200 万次局部循环 | 0.494s | 0.407s |
| `fib(27)` | 0.172s | 0.167s |
| 100 万次函数调用 | 0.423s | 0.419s |
| 20 万次参数 pattern 调用 | 0.213s | 0.249s |
| 100 万次字段读取 | 0.373s | 0.261s |
| 30 万次方法调用 | 0.169s | 0.152s |

### 2.4 基线缺口

- VM 已默认启用，标准测试集零 fallback。
- fast frame 已覆盖普通函数、handler、defer、iterator；generator /
  async、默认参数、variadic、method、参数 pattern 调用仍回退
  `call_value_args` / C++ 递归。
- 尾调用只覆盖无 spread 的普通调用。
- 全局名字 IC、字段/方法 per-site IC 已完成；显式全局 slot、
  索引读取/赋值 IC 仍待完成。
- 值表示仍是堆分配 `value_t*`，尚无 tagged value / unboxed slot；
  调用参数已对小参数数量使用栈内缓冲，fast-frame 向量按深度复用。
- 指令优化已有运算快路径、`CMP_JUMP`、常量折叠、跳转线程化和
  不可达指令消除；仍缺更多超指令、computed goto、宽指令格式。
- handler / defer 仍是运行时动态结构，尚无编译期表。
- generator / async 仍依赖原生 fiber 栈。
- 函数体、默认参数、参数 pattern 的 AST 已移出 `function_code` /
  `symlist`，由 L2 side table 管理。
- 没有字节码序列化、AOT 缓存。
- 差分测试已有随机生成、错误/边界/协程覆盖、fallback 断言和 fuzz
  最小化；opcode 覆盖统计仍待扩展。
- sanitizer 已有 ASan/UBSan/LSan 目标；`tools/check_all.sh` 覆盖完整
  Debug / Release / sanitizer / VM-only / runtime-only 矩阵。

### 2.5 分层架构现状

硬边界仍然满足：

- L1 runtime 不 include L2 头文件；
- `libskein_runtime.a` 不引用 interpreter/builtins 定义；
- CMake include 目录按层收敛；
- `tools/check_boundaries.py` 通过。

软债务：无。L1 不再持有 L2 AST 指针。

已清理：

- `function_code::ast`
- `function_code::vm_proto`
- `function_code::vm_param_patterns`
- `function_code::vm_compile_failed`
- `symlist::default_value`
- `symlist::pattern`

VM 执行缓存和函数体 / 默认参数 / 参数 pattern AST 现在位于 L2 side table，
并由 `free_function_code()` / `skein_vm_forget_function_code()` 统一清理。
`runtime_link_test` 只链接 `skein_runtime`，验证 L1 可独立链接。

### 2.6 总体进度

| 状态 | 数量 | 任务 |
|---|---:|---|
| 完成 | 19 | W1-1..6、W2-1、W2-2、W2-5、W2-6、W3-1、W4-1、W4-2、W4-3、W4-5、W5-1、W5-3、W5-5、W5-6 |
| 部分完成 | 4 | W3-2、W3-3、W3-4、W5-2 |
| 进行中 | 0 | - |
| 暂缓（按需） | 4 | W2-3、W2-4、W4-4、W5-4 |
| TODO | 0 | - |

## 3. 任务分解

优先级：P0 必须优先完成；P1 为默认后端前置；P2 为性能与产品化；
P3 为工具链完善。规模为相对估算：S / M / L / XL。

### 3.1 W1：语义完整性

#### W1-1 参数 destructuring pattern 编译为 `PatternDesc`（P0，M）

状态：**已完成（2026-11-05）**。参数 pattern 在首次调用时编译并缓存到
`function_code`，由 `skein_vm_match_parameter_pattern()` 原生匹配；成功后
从 L2 side table 释放参数 pattern AST。

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
- 参数 pattern AST 在成功编译后可释放。
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

状态：**已完成（2026-11-05）**。

实施结果：

1. `ensure_runtime_flags()` 的默认值改为启用 VM；
   `--no-vm` / `SKEIN_VM=0` 关闭。
2. `--vm` / `SKEIN_VM=1` 仍保留为显式开启。
3. CTest 明确分离两套后端：
   - `skein_tests`：`SKEIN_VM=0`
   - `skein_vm_tests`：`SKEIN_VM=1`
   - sanitizer 构建中 `skein_tests` 同样关闭 LSan。
4. 新增默认后端测试：移除 `SKEIN_VM` 环境变量并设置
   `SKEIN_VM_TRACE=1`，断言输出包含 VM trace。
5. 更新 README、`docs/bytecode.md`、`docs/architecture.md` 和帮助文本。

验收结果：

- `./build/skein script.skein` 默认使用 VM。
- `--no-vm` 与 `SKEIN_VM=0` 全量测试通过。
- 默认后端测试通过。
- 两后端 stdout / stderr / exit code 一致。

#### W1-4 AST 生命周期与回退解耦（P1，M）

状态：**已完成（2026-11-05）**。

实施结果：

1. 函数体、默认参数和参数 pattern 的 AST 位于 L2 side table，默认在成功
   编译后释放；设置 `SKEIN_KEEP_AST=1` 可保留。
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

状态：**已完成（2026-11-05）**。

实施结果：

1. 新增 `skein_vm_lsan_tests` CTest 目标（仅 `SKEIN_SANITIZE=ON`）：
   - `ASAN_OPTIONS=detect_leaks=1`
   - 运行完整 VM 测试套件
   - 普通 `skein_vm_tests` 显式使用 `detect_leaks=0`，专注于
     ASan/UBSan
2. 修复 lambda 参数 NAME token 泄漏（parser 未释放 `$1` / `$2`）。
3. `Executor` 析构函数增加 `free_stack(stack_)` 兜底，确保任何
   提前返回路径都不会遗留操作数栈值。
4. 审计并验证：
   - `throw_unwind` / `dispatch_unwind` 的 `value_t` 所有权
   - `handler_state` 的 capture snapshot
   - `defer_record` 捕获的 scope
   - `iterator_state.iterable`
   - `Proto` 常量中的 `shared_ptr<value_t>` / `function_code`
   - generator 挂起/关闭与闭包捕获
5. LSan 排除项：
   - `deep recursive struct print`：`freeval` 的深度保护会故意保留
     超过 1000 层的值，避免 C++ 栈溢出（L1 runtime 既有策略）；
   - `ffi_real` 模块和源码包含 `std.ffi` 的可选用例：FFI 内存块有
     独立生命周期，不属于 VM 资源路径。

验收结果：

- `skein_vm_lsan_tests` 通过（77.53s）。
- `skein_vm_tests` ASan/UBSan 通过（64.73s）。
- 标准 VM 用例在 LSan 下无 VM 相关泄漏。
- 长循环、递归、generator 创建/关闭无持续增长。

### 3.2 W2：VM 架构

#### W2-1 Frame-based VM（P1，XL）

状态：**完成（fast path，2026-11-05）**。非 fast path 调用仍保留
`call_value_args` 回退，generator/async 帧由 W2-4 继续。

已完成：

1. 新增 fast frame 路径：
   - `SavedVmState` 保存调用者的 `Proto*`、`pc`、scope mark、
     operand stack base、handler/defer/iterator 状态；
   - `Op::Call` 对符合条件的函数直接压入新帧，不再创建
     `Executor`，也不再走 C++ 递归；
   - `Op::Return` 和函数体自然结束都会恢复调用者帧，并把结果压回
     调用者的操作数栈；
   - fast frame 共用同一个操作数栈，`pop_checked()` 增加帧基址检查。
2. fast path 条件：
   - `T_func`；
   - 参数个数精确匹配；
   - 无默认参数、variadic、method、generator、async；
   - 无参数 destructuring pattern；
   - 函数体不含 `await` / `yield` / break/continue 控制指令。
3. fast frame 现在支持 handler、defer、iterator：
   - 每帧保存独立的 handler/defer/iterator 状态；
   - 帧返回时运行 defer、释放 handler capture 和 iterator；
   - 错误 unwinding 会逐帧恢复，遇到调用者帧中的 handler 时停止。
4. 不满足条件时回退现有 `call_value_args` 路径，保持语义。
5. 显式帧深度使用 `SKEIN_VM_MAX_DEPTH`，错误文本保持兼容。
6. 新增递归测试：`down(5000)` 在 `SKEIN_VM_MAX_DEPTH=10000` 下通过，
   不再受 1000 层 C++ 递归限制。

当前结果：

- 递归深度 5000 正常；AST 后端在相同输入下栈溢出。
- `SKEIN_VM_MAX_DEPTH=1000` 的错误文本保持不变。
- 全部现有 VM 测试通过。
- handler / defer / iterator 组合测试通过。
- 全局名字 IC 生效后，`fib(30)` 达到约 0.70s，与 AST 持平。

剩余：

1. generator / async 的显式帧挂起/恢复归入 W2-4。
2. 默认参数、variadic、method、参数 pattern 调用仍走
   `call_value_args` / C++ 递归，后续按需统一。
3. 继续减少 fast frame 的 per-call 临时分配。

验收：

- 递归深度可配置到 10000 而不爆 C++ 栈：**已满足**。
- 所有现有 VM 测试通过：**已满足**。
- handler / defer / iterator：**已满足**。
- `SKEIN_VM_MAX_DEPTH` 语义和错误文本兼容：**已满足**。
- `fib(30)` 不慢于 AST，目标 ≤ 0.70s：**已满足**。

#### W2-2 真正的 upvalue 索引（P1，L）

状态：**完成（symbol-cell upvalue，2026-11-05）**。

实施结果：

1. `function_code` 记录捕获名列表 `upvalues`；`Proto::upvalues`
   保存索引到名字的映射。
2. 编译器在父函数中为嵌套函数建立捕获集合：
   - 父参数、父局部声明、父 upvalue；
   - 使用捕获集合的保守超集，避免遗漏；
   - 子函数局部声明/参数仍优先使用 slot/param，不会误用 upvalue。
3. `CLOSURE` 创建时按名字解析为 `symbol*` cell，存入
   `function_def::upvalues`。
4. 新增指令：
   - `LOAD_UPVALUE`
   - `STORE_UPVALUE`
   - `COMPOUND_UPVALUE`
   - `INCDEC_UPVALUE`
5. 运行时按索引访问捕获 cell，不再每次读取都走名字查找。
6. 当 upvalue 为空（全局、方法、超出捕获范围或过度捕获）时，
   仍可回退到名字查找，保证语义正确。
7. 闭包继续持有创建 scope 的 `shared_ptr`，因此 symbol cell 的
   生命周期与原有闭包语义一致。

验收结果：

- 普通嵌套函数读写捕获变量使用 upvalue 指令。
- 多层闭包、循环中创建闭包、defer 捕获变量测试通过。
- `LOAD_UPVALUE` / `STORE_UPVALUE` 已在 trace 中验证。
- 现有闭包测试全部通过。

剩余优化：

- 当前 upvalue 指向 symbol cell，而不是 open frame slot；
  open/closed upvalue 的显式转换可作为后续性能项。
- 顶层块内定义的函数仍使用名字查找。
- 方法闭包尚未携带 upvalue 向量，会回退名字查找。
- 默认参数 / defer 子 Proto 暂未使用 upvalue 索引。

#### W2-3 Handler / defer 表（P1，L）

状态：**暂缓（按需）**，见 §8.4 / §8.6。

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

状态：**暂缓（按需）**，见 §8.4 / §8.6。

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

状态：**已完成（2026-11-05）**。

实施结果：

1. 新增 `src/interpreter/verifier.*`：
   - `verify(const Proto &, std::string &, uint32_t *max_stack)`
   - `verify_tree(const Proto &, std::string &)`
2. 编译器在每个 Proto 完成编译后调用 verifier，并写入
   `Proto::max_stack`：
   - 顶层语句
   - 函数体
   - 默认参数表达式
   - 参数 pattern Proto
   - defer / package 子 Proto
3. 验证范围：
   - 指令 opcode 合法
   - 跳转目标在范围内
   - 栈下溢、负栈深、分支汇合处栈深一致
   - slot / 参数 / 常量 / declaration / pattern / iterator /
     construct / import / call descriptor 索引
   - `CALL` / `CALL_SPREAD` 参数个数与栈深
   - handler 与 handler body 嵌套平衡
   - `DeferPush` 标志和 descriptor
   - package / defer / pattern 子 Proto 图无环
4. 新增 `bytecode_verifier_test` C++ 测试，覆盖正常 Proto、
   非法跳转、栈下溢、栈深冲突、handler 下溢、缺失常量/声明、
   子 Proto 图验证。
5. 运行时 `invalid bytecode ...` 检查保留，作为防御性兜底。

验收结果：

- 所有正常 Proto 通过验证。
- 手工破坏的 Proto 被拒绝且不崩溃。
- 标准测试集零 fallback，verifier 未误拒绝合法字节码。
- `bytecode_verifier_test` 在普通与 sanitizer 构建下通过。

#### W2-6 尾调用优化（TCO）（P2，M）

状态：**完成（2026-11-05，fast-frame TCO）**。

实现结果：

1. 编译器识别 `return f(args);` 并发射 `TAIL_CALL`：
   - 仅无 spread 的普通调用；
   - `CALL_SPREAD` 尾返回仍走普通 `CALL_SPREAD` + `RETURN`。
2. VM `TAIL_CALL` 优先尝试复用当前 fast frame：
   - 参数/callee 求值顺序保持不变；
   - 复用当前 `SavedVmState`，不再压入新 fast frame；
   - 不增加 `SKEIN_VM_MAX_DEPTH` 计数；
   - 重新绑定 callee 的参数、closure、Proto 和缓存。
3. 安全条件：
   - 当前帧没有非空 VM defer scope；
   - 没有活跃 handler / handler body；
   - 没有活跃 iterator；
   - 不满足时回退普通 call + return。
4. 诊断调用栈：
   - TCO 链保留诊断帧（最多 64 层），错误仍能看到尾调用路径；
   - 超过上限后截断，避免无限尾递归导致诊断栈无限增长。
5. 测试：
   - `down(500)` 在 `SKEIN_VM_MAX_DEPTH=8` 下通过；
   - 相互尾递归 `even/odd(400)` 在深度 8 下通过；
   - defer 阻止帧复用并保持清理顺序；
   - 非尾递归仍触发 `maximum recursion depth`；
   - naive `fib(30)` 行为不变。

说明：

- naive `fib(n-1) + fib(n-2)` 不是尾递归，TCO 不会减少其递归树。
- 当前未优化 spread、method、默认参数、variadic、
  generator / async 的尾调用。

验收：

- 尾递归在低 `SKEIN_VM_MAX_DEPTH` 下运行：**已满足**。
- 尾调用不增长 fast frame 栈：**已满足**。
- defer / handler 语义不变：**已满足**。
- 全部现有测试通过：**已满足**。

### 3.3 W3：性能

#### W3-1 运算与比较快路径（P2，M）

状态：**已完成（2026-11-05）**。

实施结果：

1. `binary_op()` 增加 int/int 快路径：
   - `Add` / `Sub` / `Mul` 使用 `new_int_checked`
   - `Div` / `Mod` 处理除零、`INT64_MIN / -1` 溢出
   - 位运算、左右移位直接内联，保留原有错误文本
2. 增加 int/float 混合数值浮点快路径：
   - `Add` / `Sub` / `Mul` / `Div` / `Mod`
   - 除零与 `fmod` 语义保持一致
3. `compare_op()` 增加：
   - int/int 直接比较
   - float/float 直接比较，保留 NaN unordered 语义
   - int/float 混合使用 `compare_numeric`，保持 int64 精确比较
4. 非数值类型继续回退 `apply_binary_value()` / `do_compare()`。

验收结果：

- 全部现有测试通过，错误文本与边界行为不变。
- 局部循环、调用和 `fib` 基准均未退化。
- `fib(30)` 从约 0.78s 降到约 0.75s，但仍略慢于 AST 的 0.70s。
- `call_bench` 改为 VM 约 0.81s，AST 约 0.82s。

#### W3-2 全局 slot 与内联缓存（P2，L）

状态：**部分完成（2026-11-05，全局名字 IC + 字段/方法 per-site IC）**。

已完成：

1. `Proto` 增加 per-name `NameCacheEntry`，在多个 `Executor`
   之间共享全局名字解析结果。
2. 缓存使用 `global_scope` 指针 + `global_generation` 校验；
   全局符号新增、重新定义、作用域销毁时递增 generation。
3. `cached_symbol()` 优先命中 Proto 级全局 IC，未命中时再走
   `lookupsym()`；局部符号仍使用每帧 name cache。
4. 修复全局作用域重建时 generation 回绕导致旧缓存命中的问题。
5. 结构体 / enum variant 字段增加名称到索引的 `unordered_map`：
   - `struct_def::field_index`
   - `variant_def::field_index`
   - `enum_def::variant_index`
   - `structtype_field_index()` / `variant_field_index()` /
     `variant_index()` 优先查表，并在 map 不完整时回退线性扫描。
6. VM 统计已覆盖全局名字 IC 命中/未命中。
7. 局部 name cache 从“任意声明后整表失效”改为“只失效同名条目”：
   - `DefineSlot` / `DefineName`
   - `BindFunc` / `BindName` / `Import`
   - for-in 变量
   - match / destructuring binding
   - `PopScope` 仍保留整表失效，保证作用域退出安全
8. `obj.field` 常量字段名编译为 `GET_FIELD_NAME`：
   - 新增 `Proto::member_ic` per-site `MemberCacheEntry`；
   - 缓存 `struct_def` / `enum_def` 指针、variant、字段索引或方法；
   - 类型定义重新创建时通过 owner 指针自然失效；
   - dict namespace / userdata / 其他成员访问保留通用路径；
   - 方法读取也使用同一 per-site IC。
9. VM 统计新增 `member-ic hits/misses`；新增 `field` / `method` 基准。

效果：

- `fib(30)` 从约 0.75s 降到约 0.70s，与 AST 持平。
- `call_bench` VM 约 0.82s，AST 约 0.85s。
- 结构体字段 / enum variant 查找从线性扫描改为哈希查找。
- `field_bench` VM/AST 约 0.71；`method_bench` VM/AST 约 0.94。

剩余：

1. 显式 `LOAD_GLOBAL` / `STORE_GLOBAL` 槽位与模块级符号表。
2. 索引读取 / 赋值的 per-call-site IC。
3. 局部名字缓存改为 per-call-site，而不是每帧整表失效。

验收：

- 调用密集基准达到或超过 AST：**已满足**。
- 其余 IC 项仍待完成。

#### W3-3 值表示与分配（P2，XL）

状态：**部分完成（2026-11-05，调用参数与帧缓存复用）**。

已完成：

1. `CALL` / `TAIL_CALL` 参数数量不超过 8 个时使用栈内
   `std::array<value_t *, 8>`，只在回退路径构造 `std::vector`。
2. fast call 不再每次重新分配 `name_cache_` / `param_symbols_` /
   `slot_symbols_`：
   - callee 返回时把帧向量回收进池；
   - 下一次同深度调用复用容量；
   - `assign` 仍负责清空有效槽位，保证语义不变。
3. 参数绑定直接使用 `value_t **` + count，避免为 fast call
   构造临时参数向量。
4. `call` / `fib` 基准接近或持平 AST。

仍未完成：

- tagged value 或 NaN boxing；
- 帧内值栈不强制堆分配；
- 小整数/布尔内联；
- 对象分配器/arena；
- 参数 symbol 分配复用。

验收：基准提升且内存占用不显著上升。

#### W3-4 指令优化（P2，M）

状态：**部分完成（2026-11-05，比较跳转融合 + 跳转线程化/DCE）**。

已完成：

1. 新增 `CMP_JUMP` 超指令：
   - flags 低 7 位保存比较操作
   - bit 7 表示 jump-if-true / jump-if-false
   - `b` 为目标地址
2. 编译器条件编译改为 `compile_condition()`：
   - `if` / `while` / `for` / `do while` / 三元表达式
   - match arm guard
   - 条件为比较表达式时直接发射 `CMP_JUMP`
   - 其他条件仍使用 `JumpIfFalse` / `JumpIfTrue`
3. VM 比较逻辑提取为 `compare_values()`，`CMP_JUMP` 与
   `CMP_*` 共用 int / float / 混合数值 / 通用比较语义。
4. verifier 支持 `CMP_JUMP` 的双后继 CFG。
5. 编译器增加安全常量折叠：
   - int / float 的 `+` / `-` / `*` / `/` / `%`
   - 位运算和移位
   - 数值比较
   - 一元负号和按位取反
   - 溢出、除零、非法移位时不折叠，保留运行时错误路径
   - 字符串、容器和混合数值比较不折叠
6. 新增编译后优化 pass：
   - 无条件 / 条件跳转线程化，跳过链式 `JUMP`；
   - 不可达指令消除（DCE），同步重映射跳转目标与源码位置；
   - 含 handler/defer 静态配对的 Proto 保守跳过 DCE；
   - `SKEIN_VM_OPT=0` 可关闭优化。

效果：

```text
slot ratio  0.82
fib ratio   0.97
call ratio  0.99
pattern     1.17
field       0.70
method      0.90
```

剩余：

- 更多超指令
- computed goto / table dispatch
- 宽指令格式与 16 位操作数上限

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

状态：**完成（2026-11-05）**。

实施结果：

1. `function_code` 删除 `struct ast *ast` 字段，不再依赖 `struct ast`。
2. 函数体 AST 迁移到 `ast.cpp` 的 L2 side table：
   - `function_body_ast()`
   - `set_function_body_ast()`
   - `clear_function_body_ast()`
3. 空函数体使用合成的 `return null` AST，保证 VM 编译可用。
4. 默认构建仍支持：
   - `--no-vm`
   - 编译失败时 AST 回退
   - `SKEIN_KEEP_AST=1`
5. 新增 CMake 选项 `SKEIN_VM_ONLY`：
   - VM-only 构建中 `--no-vm` 不可用
   - `SKEIN_VM=0` 不会关闭 VM
   - 函数体 AST 在成功编译后释放
6. `free_function_code()` 统一清理：
   - VM execution cache
   - 默认参数 cache
   - 函数体 AST side table

验收结果：

- `valuetype.h` 不再包含 `struct ast` 字段或 forward declaration。
- 默认构建 11/11 测试通过。
- `SKEIN_VM_ONLY=ON` 构建 11/11 测试通过。
- VM-only 构建下 `--no-vm` 返回 2。
- ASan/UBSan/LSan 通过。

#### W4-3 参数与 default 元数据 Proto 化（P2，M）

状态：**完成（2026-11-05）**。

实施结果：

1. `symbol.h` 删除 `symlist::default_value` / `symlist::pattern`。
2. 参数 default/pattern AST 迁移到 `ast.cpp` 的 L2 side table：
   - `symlist_default_ast()` / `set_symlist_default_ast()`
   - `symlist_pattern_ast()` / `set_symlist_pattern_ast()`
   - `clear_symlist_metadata()`
3. `symlist` 只保留签名元数据：
   - 名称、类型、variadic；
   - `has_default` / `has_pattern`。
4. VM 成功编译默认参数 / pattern 后释放对应 AST，但保留
   `has_default` / `has_pattern` 语义。
5. trait 参数元数据只保留名称/类型/可变参数/是否有默认值。

验收结果：

- VM-only 构建运行期不再出现 `default_value` / `pattern` AST 指针。
- 多默认参数、`std.graph` 默认参数、参数 pattern 用例通过。
- 默认构建、VM-only、ASan/UBSan/LSan 全部通过。

#### W4-4 调试信息与 source map（P3，M）

状态：**暂缓（按需）**，见 §8.4 / §8.6。

- 压缩 `Proto::locs` 为行表/列增量。
- 保留函数名、参数名、slot 名用于调试。
- VM 栈回溯输出函数名、文件、行列。
- 与 AST 后端诊断格式对齐。

#### W4-5 把 VM 执行缓存移出 runtime（P1，M）

状态：**完成（2026-11-05）**。

实施结果：

1. `function_code` 不再持有：
   - `vm_proto`
   - `vm_param_patterns`
   - `vm_compile_failed`
2. `vm.cpp` 新增 L2 side table：

   ```cpp
   struct function_execution_cache {
       ProtoPtr proto;
       std::shared_ptr<const compiled_pattern_set> param_patterns;
       bool compile_failed = false;
   };
   std::unordered_map<function_code *, function_execution_cache>
       execution_cache;
   ```

3. 默认参数 Proto 继续使用独立的 `default_cache`，同样以
   `function_code*` 为键。
4. `skein_vm_forget_function_code()` 同时清理：
   - `execution_cache`
   - `default_cache`
5. `free_function_code()` 仍是唯一释放入口，删除 `function_code`
   前会调用 forget hook。
6. `function_code` 现在只包含签名、行号、模块和 upvalue 名称等
   执行无关元数据。

验收结果：

- `valuetype.h` 不再包含 VM 执行缓存字段。
- `boundary_check` 通过。
- VM 标准测试集与 LSan 通过。
- 工作区 `function_code*` 生命周期由 `free_function_code()` 统一管理。
- 释放函数后 side table 无残留条目。

### 3.5 W5：工具链与产品化

#### W5-1 VM 统计（P2，S）

状态：**完成（2026-11-05）**。

新增：

```bash
./build/skein --vm-stats script.skein
SKEIN_VM_STATS=1 ./build/skein script.skein
```

已实现：

- 编译语句数 / 语句 fallback 数
- 编译函数数 / 函数 fallback 数
- 指令数、调用数
- fast frame 调用数
- tail call 数
- fallback call 数
- 全局名字 IC 命中/未命中
- 字段 / 方法 per-site member IC 命中/未命中
- 最大 VM 调用深度
- 最大操作数栈深
- 最大 handler / defer / active iterator 深度
- 编译耗时 / 执行耗时
- fallback 原因聚合
- Top 10 opcode 直方图

输出示例：

```text
[vm-stats] statements compiled=3 fallback=0
[vm-stats] functions compiled=2 fallback=0
[vm-stats] instructions=32 calls=4 fast=2 tail=0 fallback=2
[vm-stats] name-ic hits=0 misses=3
[vm-stats] member-ic hits=1998 misses=2
[vm-stats] max-depth=2 max-stack=4 max-handler=1 max-defer=0 max-iterator=0
[vm-stats] compile=0.016ms execute=0.068ms
[vm-stats] opcodes LOAD_CONST=5 CALL=4 ...
```

剩余：

- 每调用站点 IC 统计
- 更细的编译/执行阶段拆分

#### W5-2 差分测试与模糊测试（P2，L）

状态：**部分完成（2026-11-05，基础差分测试）**。

已完成：

1. 新增 `tests/vm_differential.py`：
   - 固定随机种子
   - 生成表达式、赋值、if/while、for-in、match、函数、闭包
   - 同一程序分别用 `SKEIN_VM=0` / `SKEIN_VM=1` 执行
   - 逐字节比较 stdout / stderr / exit code
   - 失败时打印 case 编号、源码和两后端结果
2. 新增普通构建 CTest：

   ```text
   vm_differential_test
   ```

   默认 160 个生成程序，约 0.8s。
3. 差分测试发现并修复了重复声明初始化顺序差异：
   - AST 在求值 initializer 前检查重复符号
   - VM 原先在 initializer 求值后检查
   - 新增 `CHECK_REDEFINE` 指令，在 initializer 前执行检查
   - AST / VM 现在的错误位置和文本一致
4. 生成器扩展覆盖：
   - 错误路径：除零、缺字段、越界、未捕获 fail、参数数量、类型错误；
   - 空容器与空字符串；
   - 整数 / 浮点边界；
   - struct 字段默认值、copy-with、方法读取；
   - enum variant 字段默认值；
   - generator 的 next / return / close；
   - async / await。
5. VM 默认运行增加 fallback 断言：生成程序在 `SKEIN_VM_DEBUG=1`
   下不得出现 `[vm] fallback`。
6. 新增 `--seed`、`--skip-fallback-check` 和 `--fuzz-seconds`：
   - 长时间 fuzz 按时间循环生成；
   - 失败时逐行删除语句做最小化，再打印最小复现源码。
7. 额外以 seed 1/2/3 各跑 400 个用例，全部一致。

剩余：

- opcode 级覆盖统计
- 更复杂的作用域 / 闭包 / 错误嵌套生成器

#### W5-3 基准回归（P2，M）

状态：**完成（2026-11-05）**。

已完成：

1. 仓库新增 `benchmarks/`：
   - `slot_bench.skein`
   - `fib_bench.skein`
   - `call_bench.skein`
   - `pattern_bench.skein`
   - `field_bench.skein`
   - `method_bench.skein`
2. 新增 `tools/bench_vm.py`：
   - 每个基准分别用 `SKEIN_VM=0` / `SKEIN_VM=1` 运行
   - 默认 3 次，报告中位数
   - 输出 AST / VM 耗时和 VM/AST ratio
   - `--check` 模式检查 ratio 和绝对时间上限
3. 新增普通构建 CTest：

   ```text
   vm_bench_test
   ```

   使用 `--check`，阈值默认 VM/AST ≤ 1.5，绝对上限 10s。

当前结果：

```text
benchmark         AST       VM    ratio
slot            0.495    0.419     0.85
fib             0.168    0.181     1.08
call            0.417    0.433     1.04
pattern         0.208    0.249     1.20
vm benchmark regression check passed
```

剩余：

- 冷启动 / 首次编译 / 稳态拆分
- 方差和更细的 CI 阈值
- 历史基线持久化

#### W5-4 序列化与 AOT 缓存（P3，XL）

状态：**暂缓（按需）**，见 §8.5 / §8.6。

- 稳定字节码格式与版本号。
- 字符串表、常量表、源码位置表、import/module 描述。
- 替换不可序列化的 `shared_ptr<value_t>` / `function_code`。
- 源码哈希 + 编译器版本缓存键。
- 磁盘缓存与预编译 stdlib。
- 缓存损坏时安全回退到源码编译。

#### W5-5 CI 与 sanitizer（P2，M）

状态：**完成（2026-11-05）**。

实施结果：

1. 新增 `tools/check_all.sh`，覆盖：
   - Debug / Release 构建；
   - ASan + UBSan；
   - LSan 定向测试；
   - VM-only 构建；
   - runtime-only 链接检查；
   - docs / boundary / diff check。
2. 新增 `runtime_link_test`，只链接 `skein_runtime`。
3. 新增可选 `--glibcxx-debug`，使用 `-D_GLIBCXX_DEBUG` 跑核心测试。
4. generator / fiber 栈大小支持 `SKEIN_FIBER_STACK_KB`，sanitizer
   测试默认使用 8192 KB。
5. AST 参考后端调用深度支持 `SKEIN_MAX_CALL_DEPTH`，sanitizer
   测试默认使用 128。
6. 可选 `--glibcxx-debug` 已通过 4/4 核心测试。

验收结果：`tools/check_all.sh --clean` 在干净构建目录中通过。

#### W5-6 文档维护（P2，S）

状态：**完成（2026-11-05）**。

实施结果：

- 更新 README、`docs/bytecode.md`、`docs/architecture.md`。
- 记录当前支持范围、已知限制、基准数据。
- 明确 VM-only / 默认构建 / `--no-vm` 的说明。
- 补充统一检查脚本与 fiber 栈配置说明。

## 4. 里程碑

> 2026-11-05 重排：M1 已完成。未完成部分按 §8 的 N1–N4 执行，
> 原 M2–M6 保留为能力清单；W2-3、W2-4、W4-4、W5-4 暂缓。

### M1：语义收口

状态：**完成（2026-11-05）**。

任务：W1-1、W1-2、W1-4、W1-5、W1-6

退出标准：

- 参数 pattern 原生执行。
- 标准测试集零 `EVAL_AST`。
- 诊断差分与泄漏审计通过。
- AST 后端通过 `--no-vm` / `SKEIN_VM=0` 保留为参考实现。

### M2：Frame VM

状态：**进行中**（W2-1、W2-5 完成；W2-3 待做）。

任务：W2-1、W2-3、W2-5

退出标准：

- [x] 普通 VM 调用不再新建 `Executor`。
- [x] 递归深度可达 10000。
- [ ] handler/defer 使用编译期表。
- [x] verifier 接入编译流程。
- [x] `fib(30)` 不慢于 AST。

### M3：闭包与协程

状态：**进行中**（W2-2 完成；W2-4 待做）。

任务：W2-2、W2-4

退出标准：

- [x] 闭包通过 upvalue 索引访问。
- [ ] generator/async 状态显式化。
- [x] 多层闭包与 defer 捕获测试通过。

### M4：性能

状态：**进行中**（W2-6、W3-1 完成；W3-2、W3-4 部分完成；W3-3 待做）。

任务：W3-1、W3-2、W3-3、W3-4、W2-6

退出标准：

- [x] 基线基准达到目标（`fib` 与调用密集不慢于 AST）。
- [x] 尾调用复用当前帧。
- [x] 性能回归测试纳入 CI。
- [x] 关闭优化时语义一致。
- [ ] 值表示与分配优化。

### M5：默认后端与 AST 清理

状态：**完成（2026-11-05）**。

任务：W1-3、W4-1、W4-2、W4-3、W4-5

退出标准：

- [x] 默认启动即 VM。
- [x] 运行期不再需要函数体 / 默认参数 / 参数 pattern AST。
- [x] `function_code` 不再持有 VM 执行缓存，L1 可独立链接。
- [x] `--no-vm` 仍全量通过。

### M6：产品化

状态：**进行中**（W5-1、W5-3、W5-5、W5-6 完成；W5-2 部分完成；
W5-4 按需暂缓）。

任务：W5-1 至 W5-6

退出标准：

- [x] 字节码可验证。
- [x] VM 统计与基准回归可用。
- [x] CI 与 sanitizer 矩阵完整。
- [ ] 字节码可序列化 / AOT。
- [x] 发布构建默认启用 VM。

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
| 尾调用跳过 defer/handler 清理 | 清理语义丢失 | 只在无活跃 defer/handler 时复用帧，否则走普通调用 |

## 7. 任务跟踪

| 任务 | 优先级 | 规模 | 状态 | 依赖 |
|---|---|---|---|---|
| W1-1 参数 pattern | P0 | M | 完成（2026-11-05） | - |
| W1-2 消除 EVAL_AST | P0 | L | 完成（2026-11-05） | - |
| W1-3 VM 默认后端 | P1 | M | 完成（2026-11-05） | W1-1/W1-2/W1-4/W1-5 |
| W1-4 AST 生命周期 | P1 | M | 完成（2026-11-05） | - |
| W1-5 诊断一致性 | P1 | S | 完成（2026-11-05） | - |
| W1-6 资源泄漏审计 | P1 | M | 完成（2026-11-05） | - |
| W2-1 Frame VM | P1 | XL | 完成（fast path） | W1-2 |
| W2-2 Upvalue | P1 | L | 完成（symbol-cell） | W2-1 |
| W2-3 Handler/defer 表 | P1 | L | 暂缓（按需） | W2-1 |
| W2-4 Generator 状态 | P2 | L | 暂缓（按需） | W2-1 |
| W2-5 Verifier | P1 | M | 完成（2026-11-05） | W1-2 |
| W2-6 尾调用优化 | P2 | M | 完成（fast-frame） | W2-1 |
| W3-1 运算快路径 | P2 | M | 完成（2026-11-05） | W2-1 |
| W3-2 全局 slot + IC | P2 | L | 部分完成（全局名字 IC + 字段/方法 per-site IC + 精确失效） | W2-1 |
| W3-3 值表示 | P2 | XL | 部分完成（调用参数与帧缓存复用） | W2-1 |
| W3-4 指令优化 | P2 | M | 部分完成（CMP_JUMP + 常量折叠 + 跳转线程化/DCE） | W3-1 |
| W4-1 声明常量去 AST | P1 | M | 完成（2026-11-05） | W1-2 |
| W4-2 移除 function AST | P1 | M | 完成（2026-11-05） | W4-1/W2-2 |
| W4-3 参数元数据 Proto 化 | P2 | M | 完成（2026-11-05） | W1-1 |
| W4-4 调试信息 | P3 | M | 暂缓（按需） | W2-1 |
| W4-5 VM 缓存移出 runtime | P1 | M | 完成（2026-11-05） | W2-1 |
| W5-1 VM 统计 | P2 | S | 完成（2026-11-05） | - |
| W5-2 差分/模糊测试 | P2 | L | 部分完成（基础差分 + 错误/边界/协程 + fallback 断言） | W1-2 |
| W5-3 基准回归 | P2 | M | 完成（2026-11-05） | W2-1 |
| W5-4 序列化/AOT | P3 | XL | 暂缓（按需） | W2-5/W4 |
| W5-5 CI/Sanitizer | P2 | M | 完成（2026-11-05） | - |
| W5-6 文档维护 | P2 | S | 完成（2026-11-05） | 各里程碑 |

状态约定：`TODO` / `进行中` / `阻塞` / `完成` / `暂缓（按需）`。
每个任务完成时同步更新本表和 `docs/bytecode.md`。

## 8. 未完成部分重排（2026-11-05）

### 8.1 原则

1. 先完成 M5（运行期 AST/元数据）与 M6（产品化）的收口工作。
2. 不再无界扩展后端能力；只做能被差分、基准或统计验证的任务。
3. 大任务拆成可独立验收的小步，每步保持：
   - 标准测试集零 fallback
   - AST / VM 差分通过
   - verifier 通过
   - benchmark 不退化
4. 动态 handler/defer 栈和原生 fiber 已能通过全部测试，相关重写暂缓。

### 8.2 N1：收口阶段（P0）

#### N1-1 W4-2 VM-only runtime AST 清理

状态：**完成（2026-11-05）**。

- 新增 CMake 选项 `SKEIN_VM_ONLY`。
- 默认构建保留 AST 回退路径，支持 `--no-vm` 和编译失败回退；
  AST 位于 L2 side table，`function_code` 不再含 `ast*`。
- VM-only 构建：
  - 不保留函数体 / 默认参数 / 参数 pattern 的执行期 AST
  - `symlist` 不含 AST 字段
  - `--no-vm` 在 VM-only 构建中不可用
- 验收：
  - VM-only 构建无 `ast*` 执行状态
  - `SKEIN_VM_ONLY=ON` 下标准测试集通过
  - runtime-only 链接检查通过

#### N1-2 W4-3 参数/default 元数据 Proto 化

状态：**完成（2026-11-05）**。

- `symlist::default_value` / `pattern` 已删除。
- 默认参数、参数 pattern AST 保存在 `ast.cpp` 的 L2 side table。
- VM-only 构建运行期只保留编译后的 Proto；默认构建保留 AST 回退。
- `symlist` 只保留名称、类型、variadic、`has_default` / `has_pattern`。
- 验收：
  - VM-only 构建运行期无 `default_value` / `pattern` AST 指针
  - 默认参数、pattern、trait 校验测试通过

#### N1-3 W5-5 CI / Sanitizer 矩阵

状态：**完成（2026-11-05）**。

- `tools/check_all.sh` 覆盖：
  - Debug 构建
  - Release 构建
  - ASan + UBSan
  - LSan 定向测试
  - VM-only 构建
  - runtime-only 链接检查
  - docs / boundary / diff check
- `--glibcxx-debug` 可选运行 `-D_GLIBCXX_DEBUG` 核心测试。
- generator/fiber 栈通过 `SKEIN_FIBER_STACK_KB` 单独配置。
- 验收：脚本在干净构建目录中可重复通过。

#### N1-4 W5-6 文档与发布说明

状态：**完成（2026-11-05）**。

- 已更新 README、`docs/bytecode.md`、`docs/architecture.md`。
- 已记录当前支持范围、已知限制、基准数据。
- 已给出 VM-only / 默认构建 / `--no-vm` 的明确说明。

### 8.3 N2：性能阶段（P1）

#### N2-1 W3-2 全局 slot 与 per-site IC

状态：**部分完成（2026-11-05）**。

已完成：

- `obj.field`（常量字段名）使用 `GET_FIELD_NAME` 与 per-site
  `MemberCacheEntry`，覆盖 struct/enum 字段与方法读取；
- IC 命中/未命中统计；
- 新增 `field` / `method` 基准。

仍未完成：

- 显式 `LOAD_GLOBAL` / `STORE_GLOBAL` 槽位；
- 索引读取 / 赋值的 per-call-site IC。

验收：调用密集、字段访问密集基准不退化；`vm_bench_test` 通过。

#### N2-2 W3-3 值表示与分配

状态：**部分完成（2026-11-05）**。

已完成：

- `CALL` / `TAIL_CALL` 小参数数量走栈内数组；
- fast call 的 name/slot/param 向量按深度复用；
- 参数绑定不再构造临时 `std::vector`。

仍未完成：

- tagged value 或等价的小值内联方案；
- 非捕获局部 slot 尽量不经过 `symbol*` cell；
- 减少每帧 `value_t` / `symbol` 分配；
- 对象分配器/arena。

验收：`fib`、slot、call 基准提升；内存不显著上升。

#### N2-3 W3-4 指令优化收尾

状态：**部分完成（2026-11-05）**。

已完成：

- 跳转线程化；
- 不可达指令消除（含跳转目标/源码位置重映射）；
- `SKEIN_VM_OPT=0` 优化开关。

仍未完成：

- 更多超指令；
- computed goto / table dispatch；
- 宽指令格式，解决 16 位操作数上限。

验收：优化可开关，关闭时语义一致；基准有可量化收益。

#### N2-4 W5-2 差分与模糊测试扩展

状态：**部分完成（2026-11-05）**。

已完成：

- 错误路径、空容器、边界溢出、generator / async 生成；
- VM fallback 原因断言；
- 可选长时间 fuzz 与失败最小化；
- 3 个额外 seed 各 400 例通过。

仍未完成：

- 每个 opcode 至少一个直接或间接用例；
- opcode 覆盖统计。

验收：随机差分稳定，失败时能最小化复现。

### 8.4 N3：架构阶段（P2，按需）

#### N3-1 W2-3 handler/defer 编译期表

仅在统计显示 handler/defer 开销明显时执行：

- 编译期 handler table / defer table。
- 统一 unwind 路径。
- 保留现有 defer / recover / pass 语义。

#### N3-2 W2-4 generator/async 显式帧

仅在需要轻量协程或序列化挂起状态时执行：

- 显式保存 pc、帧、handler/defer。
- 减少原生 fiber 栈依赖。
- generator 创建/挂起/关闭压力测试。

#### N3-3 W4-4 调试信息

- 压缩 `Proto::locs`。
- 调试用的 slot / 参数名。
- VM 栈回溯统一格式。

### 8.5 N4：产品化阶段（P3）

#### N4-1 W5-4 序列化 / AOT

前置条件：

- verifier 稳定
- 常量表示可序列化
- 字节码 ABI 版本化
- IC / handler 表格式确定

实施：

- 稳定字节码格式
- 字符串表、常量表、源码位置表
- 源码哈希 + 编译器版本缓存键
- 磁盘缓存与预编译 stdlib
- 缓存损坏安全回退

### 8.6 暂缓说明

| 任务 | 暂缓原因 | 重新启用条件 |
|---|---|---|
| W2-3 handler/defer 表 | 动态 per-frame 结构已通过全部测试 | stats 显示 handler/defer 热点 |
| W2-4 generator/async 帧 | 原生 fiber 工作稳定 | 需要轻量协程/序列化状态 |
| W4-4 调试信息 | 不阻塞默认后端 | 需要调试器或更完整栈回溯 |
| W5-4 序列化/AOT | 字节码格式尚未冻结 | N1/N2 完成后 |

### 8.7 建议排期

```text
N1 收口      1–2 轮
N2 性能      2–3 轮
N3 架构      2–4 轮（按需）
N4 产品化    2+ 轮
```

每轮结束必须更新本计划的任务表和测试/基准数据。
