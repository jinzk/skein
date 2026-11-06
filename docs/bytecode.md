# Skein 字节码与 VM

本文描述当前已经落地的字节码执行器，以及后续演进方向。具体任务拆分、
里程碑、依赖和验收标准见 [bytecode-plan.md](bytecode-plan.md)。

## 1. 当前状态

- 新增 L2 组件：
  - `src/interpreter/bytecode.*`：`Instr`、`Proto`、常量池、反汇编；
  - `src/interpreter/compiler.*`：AST → 字节码；
  - `src/interpreter/vm.*`：栈式执行器、函数体接入、函数 Proto 缓存。
- VM 默认启用；`--no-vm` 或 `SKEIN_VM=0` 可切换到 AST 后端。
- AST 解释器作为差分测试的参考实现保留。
- `ctest` 同时运行 AST 与 VM 两套用例（`skein_tests` 与
  `skein_vm_tests`）。

## 2. 使用方式

```bash
# 默认使用 VM
./build/skein script.skein

# 显式开关
./build/skein --vm script.skein
./build/skein --no-vm script.skein

# 环境变量
SKEIN_VM=1 ./build/skein script.skein
SKEIN_VM=0 ./build/skein script.skein

# 打印编译后的字节码和每步执行
SKEIN_VM=1 SKEIN_VM_TRACE=1 ./build/skein script.skein

# 查看无法编译、回退到 AST 的原因
SKEIN_VM=1 SKEIN_VM_DEBUG=1 ./build/skein script.skein

# 限制 VM 递归深度（默认 1000；sanitizer 测试使用更小的值）
SKEIN_VM=1 SKEIN_VM_MAX_DEPTH=128 ./build/skein script.skein

# 限制 AST 参考后端调用深度（默认 1000；sanitizer 帧更大）
SKEIN_VM=0 SKEIN_MAX_CALL_DEPTH=128 ./build/skein script.skein

# 对比 / 调试：关闭参数 symbol 复用
SKEIN_VM=1 SKEIN_VM_PARAM_POOL=0 ./build/skein script.skein

# 对比 / 调试：关闭局部 slot symbol 复用
SKEIN_VM=1 SKEIN_VM_SLOT_POOL=0 ./build/skein script.skein

# 保留函数体/默认参数/参数 pattern 的 AST，便于调试或运行期切换后端
SKEIN_VM=1 SKEIN_KEEP_AST=1 ./build/skein script.skein

# 输出 VM 统计：编译/回退数、调用数、IC、栈深、opcode 直方图
./build/skein --vm-stats script.skein
SKEIN_VM_STATS=1 ./build/skein script.skein

# VM-only 构建：不编译 AST 参考后端，--no-vm 不可用
cmake -S . -B build-vm-only -DSKEIN_VM_ONLY=ON
cmake --build build-vm-only

# generator / async fiber 栈大小（KB，默认 2048）
SKEIN_FIBER_STACK_KB=8192 ./build/skein script.skein

# 统一检查：Debug/Release、ASan+UBSan、LSan、VM-only、
# runtime-only 链接、docs/boundary/diff
tools/check_all.sh --clean

# 日常快速回归：增量构建 + core 测试（不含基准/sanitizer）
tools/check_fast.sh
tools/check_fast.sh --vm-only

# opcode 覆盖：运行标准测试集并输出未覆盖清单
python3 tools/check_opcode_coverage.py --bin ./build/skein --check

# VM fallback 回归：已知回退白名单，意外回退失败
python3 tools/check_vm_fallback.py --bin ./build/skein --check

# 差分 / 长时间 fuzz；失败时自动逐行最小化
python3 tests/vm_differential.py ./build/skein 400 --seed 1
python3 tests/vm_differential.py ./build/skein --fuzz-seconds 60
```

## 3. 执行模型

当前实现采用**按需编译 + 局部回退**：

```text
顶层语句
  → 编译为 Proto
  → 执行
  → 无法安全编译时，整条语句回退 AST

函数定义
  → 仍由现有 AST 路径创建 function_code（保留验证与默认参数语义）
  → 首次调用时把函数体编译为 Proto
  → 后续调用复用缓存的 Proto
  → 函数体无法安全编译时，整个函数体回退 AST
```

这样可以在不破坏现有语义和测试的前提下逐步提高 VM 覆盖率。

### 3.1 已原生编译的结构

- 字面量：int / float / bool / null / string / char / byte / bytes / builtin
- 变量读取与赋值（按名字查找，复用现有 `scope_t`）
- `let` / `const` 声明（`DEFINE_NAME`）
- 算术、位运算、比较、`&&` / `||` / `!`
- int/int 与 int/float 混合的运算/比较快路径（溢出、除零、NaN、
  int64 精确比较语义保持不变）
- 条件比较融合（`CMP_JUMP`：比较后直接跳转，用于 if/while/for/guard）
- 安全常量折叠（数值运算、位运算、数值比较；溢出/除零时不折叠）
- 常量条件折叠（删除编译期确定的 `if` / 条件分支）
- slot 算术超指令（`BINARY_SLOTS` / `BINARY_SLOT_CONST`：
  `LOAD_SLOT/LOAD_CONST + 算术 + STORE_SLOT` 融合）
- 三元表达式
- `if` / `while` / `do while` / C 风格 `for`
- `for ... in`（list/dict/string/bytes/deque/heap/set/generator）
- `switch`（case 值列表、default、break/continue 传播）
- `match` / destructuring（pattern 编译为 `PatternDesc`，arm/guard/body 为字节码）
- `break` / `continue` / 标签（循环和 switch）
- `return`
- 函数定义 / 闭包（`CLOSURE` / `BIND_FUNC`，函数体首次调用时编译并缓存在 `function_code`）
- 参数 fast path（`LOAD_PARAM` / `STORE_PARAM` / `COMPOUND_PARAM` / `INCDEC_PARAM`；参数未被本函数局部声明遮蔽时）
- 参数 symbol 复用（无闭包/defer 且 `params_direct` 时按名字复用；
  `SKEIN_VM_PARAM_POOL=0` 可关闭）
- 局部 slot symbol 复用（无闭包/defer/handler 时跳过 `savesym()`，
  `CLEAR_SLOT` / 帧清理回收；`SKEIN_VM_SLOT_POOL=0` 可关闭）
- 局部变量 slot fast path（`DEFINE_SLOT` / `LOAD_SLOT` / `STORE_SLOT` / `COMPOUND_SLOT` / `INCDEC_SLOT` / `CLEAR_SLOT`；通过 `symbol*` cell 兼容闭包/defer/方法/for-in/match/destructuring/on-error/import）
- 参数 destructuring pattern（编译为 `PatternDesc`，调用绑定时原生匹配）
- 默认参数表达式（编译为独立 Proto，调用时执行）
- 函数调用（普通函数、builtin、方法值、spread 参数）
- VM fast frame（普通函数直接压帧，支持 handler / defer / iterator；
  `RETURN` 恢复调用者帧，不再创建 `Executor`）
- 尾调用优化（`TAIL_CALL`：无 spread 的 `return f(args)` 复用当前
  fast frame；有 defer/handler/iterator 时回退普通调用）
- list / dict 字面量
- 索引与字段读取（struct / enum variant 字段名通过索引表查找）
- 索引/字段赋值、复合赋值、自增自减（含 COW 与循环引用检查）
- `is` 类型检查
- struct / enum 字面量构造
- struct / enum / trait / impl 声明
- package 声明
- `fail`
- `on error` / `recover` / `pass`（handler 栈 + 局部 unwind）
- `defer`（独立 Proto + 捕获作用域，在 scope exit/unwind 时执行）
- `await` / `yield`
- `import`
- 块作用域（`PUSH_SCOPE` / `POP_SCOPE`）
- 自增/自减、复合赋值（变量目标）
- 名字查找的内联缓存（`LOAD_NAME` / `STORE_NAME` 等）；全局名字使用
  Proto 级缓存，跨 `Executor` 复用，并以 global scope + generation 校验
- 局部 name cache 使用 scope epoch：作用域退出只递增 epoch，
  缓存条目在下次访问时惰性失效
- 显式全局槽位：函数体内已解析的全局名使用
  `LOAD_GLOBAL` / `STORE_GLOBAL` / `COMPOUND_GLOBAL` / `INCDEC_GLOBAL`；
  槽位失效或名称不一致时回退名字查找

### 3.2 回退到 AST 的结构

编译器不再生成 `EVAL_AST`，也没有子树级 AST 执行路径。常规用户语法结构
全部有字节码路径；无法编译时，整条语句或整个函数体回退 AST。

复杂 lvalue（例如 `f().x = 1`）保持原有语义：不执行求值，直接产生
`cannot assign through this expression` 的运行时错误，但由
`RUNTIME_ERROR` 指令原生抛出，不再经过 AST。

VM 的 `match` pattern 已经编译为 `PatternDesc` 描述树，不再在运行时遍历
pattern AST；literal pattern 的表达式会编译为独立 Proto 执行。

参数 destructuring pattern 已经编译为 `PatternDesc`，由
`skein_vm_match_parameter_pattern()` 在参数绑定时原生匹配；匹配成功后，
对应参数的 pattern AST 会从 L2 side table 释放。

仍使用 AST 的运行时代码：

- 函数体 AST（编译失败、`--no-vm` 或 `SKEIN_KEEP_AST=1` 时保留）；
- 默认参数 AST（默认参数 Proto 编译失败时用于回退）；
- 参数 pattern AST（编译失败或 `SKEIN_KEEP_AST=1` 时保留）。

`function_code` 与 `symlist` 本身不再包含 AST 指针。函数体、默认参数和
参数 pattern 的 AST 分别保存在 `ast.cpp` 的 `function_bodies` 和
`symlist_metadata_map` side table 中；VM 成功编译后释放对应 AST，
`symlist::has_default` / `has_pattern` 仍保留签名语义。

默认参数表达式已经编译为独立 Proto，由 `fill_default_arguments` 在调用时
通过 VM 执行，并复用参数 fast path。

函数体成功编译为 Proto 后会释放 side table 中的函数体 AST（除非设置了
`SKEIN_KEEP_AST=1`）。`let` / `const` 的 `DEFINE_NAME` 元数据已迁移到
`Proto::decls` 的 `DeclDesc`，不再借用 `symdef*`。

每个 `function_code` 记录定义时的模块名，函数体、默认参数和参数 pattern
的懒编译都使用该模块，而不是调用点所在模块；编译嵌套函数时会继承父
Proto 的模块名。`vm_compile_failed` 与
`function_code` 同生命周期；源码重新解析或函数重新定义会创建新的
`function_code`，从而自然清除失败缓存。

诊断位置规则与 AST 保持一致：普通运行时错误使用触发错误的指令位置；
未捕获的 `fail` / `recover` 在逃出顶层语句后使用语句 token 位置，
defer 中失败也遵循同一规则。`CHECK_RESULT`、handler 清理、隐式 `POP`、
`PUSH_SCOPE` / `POP_SCOPE`、`CLEAR_SLOT` 都携带对应的源位置。

### 3.3 编译失败回退规则

- 编译以整条顶层语句或整个函数体为单位回退，不做子树级混合执行；
- 编译器对未知节点、非法 list spread 等直接报告失败；
- 编译完成后执行跳转线程化和不可达指令消除；`SKEIN_VM_OPT=0`
  可关闭这些优化；
- `SKEIN_VM_DEBUG=1` 会打印回退的语句/函数和原因；
- 回退到 AST 后复用现有 `eval()`，因此值语义、错误捕获、defer、fiber
  行为保持一致。

## 4. 指令集（当前）

```text
NOP
LOAD_NULL / LOAD_CONST / LOAD_NAME
STORE_NAME / COMPOUND_NAME / INCDEC_NAME / DEFINE_NAME
LOAD_GLOBAL / STORE_GLOBAL / COMPOUND_GLOBAL / INCDEC_GLOBAL
LOAD_PARAM / STORE_PARAM / COMPOUND_PARAM / INCDEC_PARAM
DEFINE_SLOT / LOAD_SLOT / STORE_SLOT / COMPOUND_SLOT / INCDEC_SLOT / CLEAR_SLOT
BINARY_SLOTS / BINARY_SLOT_CONST
POP / DUP / SWAP
ADD / SUB / MUL / DIV / MOD / NEG
BIT_AND / BIT_OR / BIT_XOR / SHL / SHR / BIT_NOT / NOT
CMP_EQ / CMP_NE / CMP_LT / CMP_LE / CMP_GT / CMP_GE
IS_TYPE
JUMP / JUMP_IF_FALSE / JUMP_IF_TRUE
NEW_LIST / NEW_DICT / GET_INDEX / GET_INDEX_CACHED / GET_FIELD / GET_FIELD_NAME / GET_FIELD_OR_ENUM
ITER_NEW / ITER_NEXT / ITER_END
SWITCH_CHECK / CASE_MATCH
MATCH_BEGIN / MATCH_TEST / MATCH_BIND
ASSIGN_INDEX / COMPOUND_INDEX / INCDEC_INDEX
AWAIT / YIELD / FAIL / PASS / RECOVER
PUSH_FAIL_HANDLER / PUSH_RECOVER_HANDLER / POP_HANDLER
CHECK_RESULT / BIND_NAME / ENTER_HANDLER_BODY / LEAVE_HANDLER_BODY
DEFER_PUSH
NEW_STRUCT / NEW_ENUM
DECLARE_STRUCT / DECLARE_ENUM / DECLARE_TRAIT / DECLARE_IMPL
DECLARE_PACKAGE
CLOSURE / BIND_FUNC
IMPORT
CALL / CALL_SPREAD / RETURN
PUSH_SCOPE / POP_SCOPE
CONTROL_BREAK / CONTROL_CONTINUE / RUNTIME_ERROR
```

指令固定 8 字节：

```cpp
struct Instr {
    uint8_t op;
    uint8_t flags;
    uint16_t a;
    uint32_t b;
};
```

`b` 用于常量索引、跳转目标或参数个数；`Proto::locs` 为每条指令保存
源码行列，用于保持现有诊断输出。

每个 Proto 编译完成后会经过 `skein::bc::verify()`：

- 校验 opcode、跳转目标、栈下溢和 CFG 汇合处的栈深；
- 校验 slot / 参数 / 常量 / declaration / pattern / iterator /
  construct / import / call descriptor 索引；
- 校验 handler、handler body 和 defer 标志；
- 递归校验 package、defer 和 pattern 子 Proto，检测 Proto 图环；
- 计算并写入 `Proto::max_stack`。

验证失败时整条语句/函数回退 AST；运行时 `invalid bytecode ...`
检查仍作为防御性兜底保留。

## 5. 运行时接入点

- `bytecode.*` / `verifier.*` 已拆为 `skein_bytecode` target，
  只依赖 `skein_runtime`；
- `vm.cpp` 现在是执行器；编译、fallback、默认参数和 pattern 接入位于
  `vm_bridge.cpp`，执行器不 include `ast.h` / `compiler.h`；
- `parser.y` 的 `top_statement` 调用 `skein_vm_eval_statement()`；
- `eval_call.cpp` 的 `invoke_function_body()` 在函数作用域和参数绑定完成后
  尝试 `skein_vm_try_execute_function_body()`；
- `ast.cpp` 的 `free_function_code()` 调用
  `skein_vm_forget_function_code()` 清理 VM side table；
- 函数体 AST 保存在 `ast.cpp` 的 L2 `function_bodies` side table，
  `function_code` 不再包含 `ast*` 字段；
- 默认参数 / 参数 pattern AST 保存在 `ast.cpp` 的
  `symlist_metadata_map` side table，`symlist` 只保留
  `has_default` / `has_pattern` 等签名元数据；
- 函数体 Proto、参数 pattern 和默认参数 Proto 保存在 `vm.cpp` 的
  `execution_cache` / `default_cache` 中，`function_code` 本身不再持有
  VM 执行缓存；
- 所有 AST 回退都复用现有 `eval()`，因此值语义、错误捕获、defer、fiber
  行为保持一致。

## 6. 已知限制

- 局部变量使用 slot fast path；函数参数在未被局部声明遮蔽时使用参数 fast
  path。slot 通过 `symbol*` cell 与闭包、defer、方法、for-in 循环变量、
  match binding、destructuring、on-error 绑定和 import 绑定共享。
- 普通嵌套函数使用 `LOAD_UPVALUE` / `STORE_UPVALUE` 等索引访问捕获
  变量；upvalue 指向 `symbol*` cell，而不是显式 open/closed frame slot。
- 小整数/`true`/`false`/`null` 已单例化；普通调用走 fast frame，
  `CALL` / `TAIL_CALL` 的小参数数量使用栈内缓冲，非 fast path 调用
  仍受 C++ 递归和每帧分配拖累。
- 常规语法结构已经全部有字节码路径，编译失败时整条语句/函数回退 AST。
- 函数体 AST 在成功编译后释放；默认参数表达式和参数 destructuring pattern
  成功编译后也会释放对应 AST。`function_code` / `symlist` 不再持有 AST
  指针，回退用的 AST 保存在 L2 side table；`function_code->sl` 只保留
  参数/trait 签名元数据。
- fast frame 支持普通函数以及 handler、defer、iterator；generator、
  async、默认参数、variadic、method 和参数 pattern 调用仍回退
  `call_value_args` / C++ 递归。
- 全局名字 IC 生效后，`fib(30)` 与 AST 基本持平；已解析的全局访问使用
  显式 `LOAD_GLOBAL` / `STORE_GLOBAL` 槽位；`obj.field` 的
  per-site 字段/方法 IC 已覆盖 struct/enum；dict 索引读取使用
  `GET_INDEX_CACHED` per-site 缓存；索引写入使用 `IndexDesc` 中已解析的
  slot / param / upvalue / global 基址；默认参数、variadic、
  method、参数 pattern 等非 fast path 调用仍有 per-call 分配开销。
- 顶层块内函数、方法闭包仍使用名字查找；默认参数 / defer 子 Proto
  暂未使用 upvalue 索引。
- TCO 当前只覆盖无 spread 的普通尾调用；有 defer / handler / iterator
  时回退普通调用。诊断调用栈最多保留 64 层尾调用帧。
- AST 参考后端调用深度默认 1000，可用 `SKEIN_MAX_CALL_DEPTH` 调整；
  sanitizer 构建使用更小的值以适配更大的 C++ 帧。
- 尚未实现精确 GC 栈图；JIT/AOT、字节码序列化与磁盘缓存待决策。

## 7. 后续阶段

1. 将 symbol-cell upvalue 进一步改为 open/closed frame slot；
2. frame-based VM，去掉每帧 Executor 分配和 C++ 递归；
3. 扩展尾调用优化到 spread / method / 默认参数 / variadic；
4. 把 VM 执行缓存、函数体/默认参数/pattern AST 全部收敛到 L2 side table
   （已完成，后续只保留签名元数据）；
5. 精确 GC 栈图；JIT/AOT、字节码序列化待决策；
6. generator/async 与 VM 状态整合；
7. unboxed slot、内联缓存、超指令、computed goto；
8. AST 后端降级为可选参考实现。

## 8. 验收标准

- `ctest` 中 `skein_tests` 与 `skein_vm_tests` 均通过；
- `tools/check_all.sh` 在干净构建目录中通过 Debug / Release /
  ASan+UBSan / LSan / VM-only / runtime-only 链接检查；
- `tools/check_boundaries.py` 通过；
- `SKEIN_VM=1` 下所有现有用例的 stdout/stderr/exit code 与 AST 后端一致；
- `vm_differential_test` 的随机生成程序在 AST / VM 下输出一致；
- `tools/bench_vm.py --check` 的 VM/AST 基准回归通过；
- ASan/UBSan 下 VM 路径无新增问题；
- sanitizer 构建中 `skein_vm_lsan_tests` 通过；已知的非 VM 泄漏
  （`freeval` 深度保护、可选 FFI 内存块）在测试环境中显式排除。
