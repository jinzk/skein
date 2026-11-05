# Skein 字节码与 VM（实验阶段）

本文描述当前已经落地的字节码执行器，以及后续演进方向。具体任务拆分、
里程碑、依赖和验收标准见 [bytecode-plan.md](bytecode-plan.md)。

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

# 保留函数体/默认参数/参数 pattern 的 AST，便于调试或运行期切换后端
SKEIN_VM=1 SKEIN_KEEP_AST=1 ./build/skein script.skein
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
- `match` / destructuring（pattern 编译为 `PatternDesc`，arm/guard/body 为字节码）
- `break` / `continue` / 标签（循环和 switch）
- `return`
- 函数定义 / 闭包（`CLOSURE` / `BIND_FUNC`，函数体首次调用时编译并缓存在 `function_code`）
- 参数 fast path（`LOAD_PARAM` / `STORE_PARAM` / `COMPOUND_PARAM` / `INCDEC_PARAM`；参数未被本函数局部声明遮蔽时）
- 局部变量 slot fast path（`DEFINE_SLOT` / `LOAD_SLOT` / `STORE_SLOT` / `COMPOUND_SLOT` / `INCDEC_SLOT` / `CLEAR_SLOT`；通过 `symbol*` cell 兼容闭包/defer/方法/for-in/match/destructuring/on-error/import）
- 参数 destructuring pattern（编译为 `PatternDesc`，调用绑定时原生匹配）
- 默认参数表达式（编译为独立 Proto，调用时执行）
- 函数调用（普通函数、builtin、方法值、spread 参数）
- list / dict 字面量
- 索引与字段读取
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
- 名字查找的内联缓存（`LOAD_NAME` / `STORE_NAME` 等）

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
对应的 `symlist::pattern` AST 会被释放。

仍使用 AST 的运行时代码：

- 函数体 AST（编译失败、`--no-vm` 或 `SKEIN_KEEP_AST=1` 时保留）；
- 默认参数 AST（默认参数 Proto 编译失败时用于回退）；
- 参数 pattern AST（编译失败或 `SKEIN_KEEP_AST=1` 时保留）。

默认参数表达式已经编译为独立 Proto，由 `fill_default_arguments` 在调用时
通过 VM 执行，并复用参数 fast path。

函数体成功编译为 Proto 后会释放 `function_code->ast`（除非设置了
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
- `SKEIN_VM_DEBUG=1` 会打印回退的语句/函数和原因；
- 回退到 AST 后复用现有 `eval()`，因此值语义、错误捕获、defer、fiber
  行为保持一致。

## 4. 指令集（当前）

```text
NOP
LOAD_NULL / LOAD_CONST / LOAD_NAME
STORE_NAME / COMPOUND_NAME / INCDEC_NAME / DEFINE_NAME
LOAD_PARAM / STORE_PARAM / COMPOUND_PARAM / INCDEC_PARAM
DEFINE_SLOT / LOAD_SLOT / STORE_SLOT / COMPOUND_SLOT / INCDEC_SLOT / CLEAR_SLOT
POP / DUP / SWAP
ADD / SUB / MUL / DIV / MOD / NEG
BIT_AND / BIT_OR / BIT_XOR / SHL / SHR / BIT_NOT / NOT
CMP_EQ / CMP_NE / CMP_LT / CMP_LE / CMP_GT / CMP_GE
IS_TYPE
JUMP / JUMP_IF_FALSE / JUMP_IF_TRUE
NEW_LIST / NEW_DICT / GET_INDEX / GET_FIELD
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

## 5. 运行时接入点

- `parser.y` 的 `top_statement` 调用 `skein_vm_eval_statement()`；
- `eval_call.cpp` 的 `invoke_function_body()` 在函数作用域和参数绑定完成后
  尝试 `skein_vm_try_execute_function_body()`；
- `ast.cpp` 的 `free_function_code()` 调用
  `skein_vm_forget_function_code()` 清理 Proto 缓存；
- 所有 AST 回退都复用现有 `eval()`，因此值语义、错误捕获、defer、fiber
  行为保持一致。

## 6. 已知限制

- 局部变量使用 slot fast path；函数参数在未被局部声明遮蔽时使用参数 fast
  path。slot 通过 `symbol*` cell 与闭包、defer、方法、for-in 循环变量、
  match binding、destructuring、on-error 绑定和 import 绑定共享。还没有
  真正的 upvalue 索引，闭包内部对捕获变量的访问仍走名字查找。
- 小整数/`true`/`false`/`null` 已单例化；局部变量密集的循环基准已明显
  快于 AST，但函数调用密集的基准仍受 C++ 递归和每帧分配的拖累。
- 常规语法结构已经全部有字节码路径，编译失败时整条语句/函数回退 AST。
- 函数体 AST 在成功编译后释放；默认参数表达式和参数 destructuring pattern
  成功编译后也会释放对应 AST。`function_code->sl` 仍保留参数/默认值/trait
  签名元数据，以及编译失败时用于回退的默认值 AST 和 pattern AST。
- 还没有真正的 upvalue 索引和 frame-based VM。
- 尚未实现精确 GC 栈图、字节码序列化。

## 7. 后续阶段

1. 真正的 upvalue 索引（闭包直接访问捕获 cell）；
2. frame-based VM，去掉每帧 Executor 分配和 C++ 递归；
3. 把默认参数、参数 pattern 等剩余 AST 元数据移出 `function_code`；
4. 精确 GC 栈图与字节码序列化；
5. generator/async 与 VM 状态整合；
6. unboxed slot、内联缓存、超指令、computed goto；
7. AST 后端降级为可选参考实现。

## 8. 验收标准

- `ctest` 中 `skein_tests` 与 `skein_vm_tests` 均通过；
- `tools/check_boundaries.py` 通过；
- `SKEIN_VM=1` 下所有现有用例的 stdout/stderr/exit code 与 AST 后端一致；
- ASan/UBSan 下 VM 路径无新增问题。
