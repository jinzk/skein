# Skein 字节码与 VM（实验阶段）

本文描述当前已经落地的字节码执行器，以及后续演进方向。

## 1. 当前状态

- 新增 L2 组件：
  - `src/interpreter/bytecode.*`：`Instr`、`Proto`、常量池、反汇编；
  - `src/interpreter/compiler.*`：AST → 字节码；
  - `src/interpreter/vm.*`：栈式执行器、函数体接入、函数 Proto 缓存。
- VM 默认关闭，可通过 `--vm` 或 `SKEIN_VM=1` 启用。
- AST 解释器仍是默认后端，并作为差分测试的参考实现。
- `ctest` 同时运行 AST 与 VM 两套用例（`skein_tests` 与
  `skein_vm_tests`）。

## 2. 使用方式

```bash
# 命令行开关
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
- 三元表达式
- `if` / `while` / `do while` / C 风格 `for`
- `for ... in`（list/dict/string/bytes/deque/heap/set/generator）
- `switch`（case 值列表、default、break/continue 传播）
- `break` / `continue` / 标签（循环和 switch）
- `return`
- 函数调用（普通函数、builtin、方法值）
- list / dict 字面量
- 索引与字段读取
- 索引/字段赋值、复合赋值、自增自减（含 COW 与循环引用检查）
- `is` 类型检查
- `fail`
- `await` / `yield`
- `import`
- 块作用域（`PUSH_SCOPE` / `POP_SCOPE`）
- 自增/自减、复合赋值（变量目标）
- 名字查找的内联缓存（`LOAD_NAME` / `STORE_NAME` 等）

### 3.2 当前回退到 AST 的结构

这些结构会通过 `EVAL_AST` 在**当前运行时作用域**中执行，或让整个
语句/函数体回退：

- `match` / destructuring
- `on error` / `recover` / `pass`
- `defer`
- struct / enum / trait / impl 声明
- package 声明
- 带 spread 的调用/字面量
- 函数定义本身（函数体在首次调用时编译）

### 3.3 回退安全规则

AST 回退会分析回退子树中的循环控制：

- 子树内部的 `break` / `continue` 若被它自己的循环或 `switch` 消费，则允许回退；
- 会逃逸回退边界、进入外层 VM 循环的 `break` / `continue` 会导致整个
  语句/函数体回退；
- 带标签的循环控制保守地视为逃逸；
- 因此 `match` / `on error` 等结构只要不向外传播控制值，就可以在 VM
  循环内部通过 `EVAL_AST` 执行。

无法编译时，`SKEIN_VM_DEBUG=1` 会打印原因。

## 4. 指令集（当前）

```text
NOP
LOAD_NULL / LOAD_CONST / LOAD_NAME
STORE_NAME / COMPOUND_NAME / INCDEC_NAME / DEFINE_NAME
POP / DUP / SWAP
ADD / SUB / MUL / DIV / MOD / NEG
BIT_AND / BIT_OR / BIT_XOR / SHL / SHR / BIT_NOT / NOT
CMP_EQ / CMP_NE / CMP_LT / CMP_LE / CMP_GT / CMP_GE
IS_TYPE
JUMP / JUMP_IF_FALSE / JUMP_IF_TRUE
NEW_LIST / NEW_DICT / GET_INDEX / GET_FIELD
ITER_NEW / ITER_NEXT / ITER_END
SWITCH_CHECK / CASE_MATCH
ASSIGN_INDEX / COMPOUND_INDEX / INCDEC_INDEX
AWAIT / YIELD / FAIL
IMPORT
CALL / RETURN
PUSH_SCOPE / POP_SCOPE
EVAL_AST
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

## 5. 运行时接入点

- `parser.y` 的 `top_statement` 调用 `skein_vm_eval_statement()`；
- `eval_call.cpp` 的 `invoke_function_body()` 在函数作用域和参数绑定完成后
  尝试 `skein_vm_try_execute_function_body()`；
- `ast.cpp` 的 `free_function_code()` 调用
  `skein_vm_forget_function_code()` 清理 Proto 缓存；
- 所有 AST 回退都复用现有 `eval()`，因此值语义、错误捕获、defer、fiber
  行为保持一致。

## 6. 已知限制

- 当前仍是**按名字查找局部变量**（带 per-constant cache），没有
  slot/upvalue；小整数/`true`/`false`/`null` 已单例化，简单循环基准已略快
  于 AST 后端，但整体性能仍未达到最终目标。
- 顶层语句和函数体在无法安全编译时整体回退，VM 覆盖率取决于语法结构。
- 函数定义仍由 AST 路径创建 `function_code`；最终目标是由字节码直接创建
  闭包并移除 runtime 中的 `ast*`。
- 尚未实现 handler table、defer 表、精确 GC 栈图、字节码序列化。

## 7. 后续阶段

1. 局部变量 slot + 捕获环境/upvalue；
2. `for-in`、`switch`、`match` 原生编译；
3. `on error` / `recover` / `pass` / `defer` 的 handler 表；
4. 函数定义改为 `CLOSURE proto`，移除 `function_code->ast`；
5. struct/enum/trait/impl、package/import 编译为元数据 + Proto；
6. generator/async 与 VM 状态整合；
7. unboxed slot、内联缓存、超指令、computed goto；
8. AST 后端降级为可选参考实现。

## 8. 验收标准

- `ctest` 中 `skein_tests` 与 `skein_vm_tests` 均通过；
- `tools/check_boundaries.py` 通过；
- `SKEIN_VM=1` 下所有现有用例的 stdout/stderr/exit code 与 AST 后端一致；
- ASan/UBSan 下 VM 路径无新增问题。
