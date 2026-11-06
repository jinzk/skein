# Skein JIT 开发计划（Spike 与后续路线）

- 状态：Spike 进行中（Linux x86-64 `fib` JIT 已落地）
- 更新日期：2026-11-06
- 关联文档：
  - [bytecode.md](bytecode.md)
  - [bytecode-plan.md](bytecode-plan.md)
  - [architecture.md](architecture.md)

本文把 `bytecode-plan.md` 中“JIT/AOT 待决策”的部分收敛为可执行的 JIT 路线。
当前只批准 **4 周 spike**，不直接承诺完整 JIT 交付。

---

## 1. 决策摘要

1. **先做 scoped frame/call 重构，再做 JIT。**
2. **第一版 JIT 使用 asmjit**，不再写自研 x86-64 emitter。
3. **只支持 Linux x86-64 SysV 作为 spike 目标**；
   Windows/MinGW 在 Week 4 做 smoke，不阻塞 Linux spike。
4. **只 JIT `fib` 所需的最小 opcode 子集**；
   不支持的结构继续走现有 VM。
5. **asmjit 是可选依赖**，`SKEIN_ENABLE_JIT=OFF` 时核心构建不受影响。
6. **spike 的目标是验证收益和 ABI/所有权模型**，不是交付完整 JIT。

Go/No-Go 标准见 §10。

### 当前进展（2026-11-06）

已落地：

- `SKEIN_ENABLE_JIT` + `find_package(asmjit CONFIG REQUIRED)`；
- `skein_jit` target，使用系统 asmjit 1.11；
- `function_code::vm_cache` 直接指针，替代 per-call 哈希查找；
- `Proto::stack_depth`，由 verifier 写入；
- `fib` 的 JIT 子集：`LOAD_PARAM` / `LOAD_CONST` / `LOAD_GLOBAL` /
  `ADD` / `SUB` / `CMP_JUMP` / `CALL` / `RETURN` / `LOAD_NULL` /
  `PUSH_SCOPE` / `POP`；
- slot 子集：`DEFINE_SLOT` / `LOAD_SLOT` / `STORE_SLOT` /
  `BINARY_SLOTS` / `BINARY_SLOT_CONST` / `CLEAR_SLOT`，覆盖
  `slot_bench` 与 `scope_bench`；
- global 写入：`STORE_GLOBAL`，覆盖 `global_bench`；
- struct 构造与字段读取：`NEW_STRUCT` / `GET_FIELD_NAME`，覆盖
  `field_bench`；
- dict 索引读写：`GET_INDEX_CACHED` / `ASSIGN_INDEX`，覆盖
  `index_bench` / `index_write_bench`；
- method 调用：`T_method` 直接进入 JIT，覆盖 `method_bench`；
- 顶层语句：`LOAD_NAME` / `STORE_NAME`、真实 scope push/pop、
  顶层表达式结果语义，覆盖 `call_bench` 的顶层循环；
- list / dict 字面量：`NEW_LIST` / `NEW_DICT`，覆盖 `pattern_bench`
  的顶层循环；
- 算术/位运算：`MUL` / `DIV` / `MOD` / 位运算 / 移位；
- slot 复合赋值与自增自减：`COMPOUND_SLOT` / `INCDEC_SLOT`；
- global 复合赋值与自增自减：`COMPOUND_GLOBAL` / `INCDEC_GLOBAL`；
- 一元运算：`NEG` / `NOT` / `BIT_NOT`；
- pattern 参数：JIT 前绑定函数 scope 与 destructuring 名字，覆盖
  `pattern_bench`；
- params 直接 frame 化：eligible fast frame 不再创建参数 symbol；
- upvalue cell：captures 使用显式 open/closed `upvalue_cell`，支持直接
  param/slot 捕获，帧退出时 close；
- `TAIL_CALL`：
  - 自递归尾调用直接复用当前 JIT 机器帧，重绑参数并跳回 body；
  - 非自递归尾调用通过 tail request + driver loop 做通用 trampoline，
    不再增加 C++/JIT 调用栈；
  - 覆盖 100k 自尾递归、100k 互尾递归，以及尾调用非 JIT 函数；
- JIT `CALL` 直接调用已编译函数，未命中回退 VM；
- `SKEIN_JIT_DUMP` 输出 asmjit 反汇编；
- `tests/jit_smoke.py` 对比 VM/JIT 输出与错误位置。

本机 `fib(27)` 实测：

```text
fib(27)      SKEIN_JIT=0 ~0.21s   SKEIN_JIT=1 ~0.05s   （约 4x）
slot_bench   SKEIN_JIT=0 ~0.37s   SKEIN_JIT=1 ~0.20s   （约 1.8x）
scope_bench  SKEIN_JIT=0 ~0.19s   SKEIN_JIT=1 ~0.07s   （约 2.7x）
global_bench SKEIN_JIT=0 ~0.20s   SKEIN_JIT=1 ~0.10s   （约 2.0x）
field_bench  SKEIN_JIT=0 ~0.25s   SKEIN_JIT=1 ~0.16s   （约 1.6x）
index_bench  SKEIN_JIT=0 ~0.24s   SKEIN_JIT=1 ~0.15s   （约 1.6x）
index_write  SKEIN_JIT=0 ~0.16s   SKEIN_JIT=1 ~0.10s   （约 1.6x）
method_bench SKEIN_JIT=0 ~0.18s   SKEIN_JIT=1 ~0.06s   （约 3.0x）
call_bench   SKEIN_JIT=0 ~0.49s   SKEIN_JIT=1 ~0.25s   （约 2.0x）
pattern_bench SKEIN_JIT=0 ~0.30s  SKEIN_JIT=1 ~0.20s   （约 1.5x）
```

尚未落地：

- Win64 ABI 代码路径已接入（MinGW `sysv_abi` helper + Win64 entry thunk），
  本机无 MinGW/Windows 环境，仍需 CI 或真机验证；
- OSR / deopt；
- 非自递归尾调用依赖 driver loop；JIT entry 本身仍会分配临时帧，
  但不会形成调用栈增长；
- pattern 参数必须能编译为原生 PatternDesc，否则回退 VM；
- 更完整的 opcode 覆盖。

---

## 2. 当前基线

### 2.1 基准数据

以下数据来自 `RelWithDebInfo` 构建，`SKEIN_VM=1`：

| 基准 | AST | VM | VM/AST |
|---|---:|---:|---:|
| slot | 0.542s | 0.380s | 0.70 |
| fib | 0.193s | 0.245s | 1.27 |
| call | 0.475s | 0.519s | 1.09 |
| pattern | 0.240s | 0.351s | 1.47 |
| field | 0.406s | 0.278s | 0.68 |
| method | 0.201s | 0.228s | 1.13 |
| global | 0.249s | 0.206s | 0.83 |
| index | 0.345s | 0.305s | 0.88 |
| index_write | 0.207s | 0.165s | 0.80 |
| scope | 0.223s | 0.210s | 0.94 |

`fib(27)` 的 VM 统计：

```text
bytecode instructions = 7,309,643
calls                 =   635,622
host instructions     = 4,177,447,403   # 约 570 条宿主指令 / 1 条字节码
branch-miss           = 0.11%
IPC                   = 2.97
```

### 2.2 基线结论

- 当前 VM 在 `fib` / `call` / `pattern` 上仍慢于 AST；
- 瓶颈不只是 dispatch：fast-call 每次都会做
  `execution_cache[code]` 哈希查找、`shared_ptr` 拷贝/析构、
  `SavedVmState` 搬运和参数 symbol 绑定；
- 值表示仍是堆分配 `value_t*`；小整数 `[-512, 4096]` 已单例化，
  但 float、大整数、字符串和容器仍会分配；
- JIT 可以消掉 dispatch 和调用簿记，但不会自动消掉装箱。

因此 JIT 之前必须先清理 fast-call 热路径，否则 JIT 会继承同样的成本。

---

## 3. 目标与非目标

### 3.1 Spike 目标

- 验证 asmjit 在 Linux x86-64 和 MinGW 上可用；
- 验证 frame/参数直接内存化后 VM 至少提升 10%；
- 验证 `fib` 的 JIT 路径输出、错误、递归深度、引用计数与 VM 一致；
- 验证 JIT 相对重构后 VM 达到 2x；
- 形成可复用的 frame/调用/错误/所有权模型。

### 3.2 Spike 非目标

- 全量 opcode；
- OSR / deopt；
- unboxed / NaN boxing；
- 完整 frame-based VM（移除 C++ 递归）；
- AOT / 字节码序列化；
- Windows 正式发布支持；
- tracing JIT。

---

## 4. 总体路线

```text
Phase 0  frame/call 重构 + asmjit 集成        （Week 1）
Phase 1  fib JIT spike                         （Week 2-4）
Phase 2  baseline JIT：slot/global/field/index （3-4 周）
Phase 3  OSR + 完整 frame VM                   （3-4 周）
Phase 4  unboxed / inline cache / 类型特化     （4-6 周）
```

JIT/AOT 序列化与 AOT 编译是独立决策，不阻塞 Phase 1。

---

## 5. 依赖策略

### 5.1 使用系统 asmjit

当前开发机已安装：

```text
libasmjit-dev  0.0~git20231115.1da9e4d
ASMJIT_LIBRARY_VERSION = 1.11.0
CMake target   asmjit::asmjit
```

CMake 直接使用系统包，不引入 `third_party/asmjit`：

```cmake
option(SKEIN_ENABLE_JIT "Enable experimental JIT (requires asmjit)" OFF)

if(SKEIN_ENABLE_JIT)
  find_package(asmjit CONFIG REQUIRED)

  if(NOT TARGET asmjit::asmjit)
    message(FATAL_ERROR "asmjit found but asmjit::asmjit target is missing")
  endif()

  add_library(skein_jit STATIC
    src/jit/jit_compile.cpp
    src/jit/jit_runtime.cpp
    src/jit/jit_host.cpp
    src/jit/jit_dump.cpp
  )

  target_include_directories(skein_jit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/jit
  )

  target_link_libraries(skein_jit
    PUBLIC
      skein_bytecode
      skein_value_runtime
      skein_runtime
      skein_platform
    PRIVATE
      asmjit::asmjit
  )

  target_link_libraries(skein_vm PRIVATE skein_jit)
endif()
```

安装方式：

- Debian/Ubuntu：`sudo apt install libasmjit-dev`
- MSYS2/MinGW：
  - `pacman -S mingw-w64-ucrt-x86_64-asmjit`
  - 或 `mingw-w64-x86_64-asmjit`
  - 或 `mingw-w64-clang-x86_64-asmjit`

### 5.2 不手动定义 asmjit 宏

系统包的 imported target 已经设置正确的宏（例如 `ASMJIT_STATIC`）。
不要额外定义：

```cmake
# 不要这样做
target_compile_definitions(skein_jit PRIVATE ASMJIT_STATIC)
target_compile_definitions(skein_jit PRIVATE ASMJIT_NO_COMPILER)
target_compile_definitions(skein_jit PRIVATE ASMJIT_NO_LOGGING)
```

`ASMJIT_NO_*` 会改变头文件中的类布局和 API，但预编译的 `libasmjit.a`
是按发行版默认配置编译的。混用可能导致 ABI 不一致。
如果需要裁剪 asmjit，必须从源码构建并保证头文件与库宏一致。

### 5.3 版本保护

Debian 的 CMake config 没有 `asmjit_VERSION` 变量，因此不能写
`find_package(asmjit 1.11 CONFIG REQUIRED)`。使用编译期检查：

```cpp
#include <asmjit/core/api-config.h>

static_assert(
    ASMJIT_LIBRARY_VERSION >= ASMJIT_LIBRARY_MAKE_VERSION(1, 11, 0),
    "Skein JIT requires asmjit >= 1.11.0");
```

### 5.4 asmjit 1.11 API 注意事项

| 事项 | 1.11 写法 |
|---|---|
| 取环境 | `rt.environment()` |
| 取 CPU features | `rt.cpuFeatures()`，不是 `cpu_features()` |
| `CodeHolder::init` | `code.init(rt.environment(), rt.cpuFeatures())` |
| `JitRuntime` 构造 | `JitRuntime(const JitAllocator::CreateParams*)` |
| `Func` 类型 | 1.11 没有预定义 `Func`，使用自己的 `using JitEntry = ...` |
| 反汇编 | `asmjit::StringLogger`；1.11 中 `Formatter` 是 namespace，不是类 |
| UJIT | 1.11 没有 `ujit.h`，不要使用 |
| AArch64 | 头文件有 `a64.h`，spike 不使用 |

### 5.5 W^X

默认 `JitRuntime` 会在需要时自动使用 dual mapping。
spike 阶段可以使用默认策略；需要强制 W^X 时：

```cpp
asmjit::JitAllocator::CreateParams params{};
params.options = asmjit::JitAllocatorOptions::kUseDualMapping;
asmjit::JitRuntime rt(&params);
```

---

## 6. 四周 Spike 计划

### 6.1 总览

| 周 | 工作 | 交付物 | 门槛 |
|---|---|---|---|
| Week 1 | asmjit 集成 + frame/call 重构 | Linux/MinGW smoke；去 per-call 哈希/shared_ptr | 两条工具链 smoke 通过；VM `fib` 提升 ≥10% |
| Week 2 | `fib` JIT | asmjit 生成 prologue/body/epilogue | `fib(0..30)` 与 VM 一致 |
| Week 3 | 正确性 + 性能 | 错误路径、递归深度、引用计数、benchmark | `fib` ≥ 2x 重构后 VM |
| Week 4 | MinGW/Windows + 报告 | MSYS2 构建、Wine/真实 Windows smoke、Go/No-Go | Windows 下 `fib` 正确且不崩 |

### 6.2 Week 1 任务

1. `find_package(asmjit)` 接入，`SKEIN_ENABLE_JIT=ON` 可配置；
2. `return 42` smoke：
   - Linux GCC/Clang；
   - MSYS2 `mingw64` 或 `ucrt64`；
3. `function_code` 增加 opaque `void *vm_cache`；
4. `vm_bridge.cpp` 用 `function_vm_cache` 替代 per-call
   `execution_cache[code]` 哈希查找；
5. fast call 借用 `function_def*` / `Proto*`，避免 hot path
   `shared_ptr` 拷贝；
6. `frame_eligible` Proto 的 params 直接存 `value_t*`；
7. verifier 保存每条指令的栈深度，供 JIT 固定偏移使用；
8. 跑 `bench_vm.py`，记录重构后 VM 基线。

### 6.3 Week 2 任务

1. `src/jit/jit.h`、`jit_compile.cpp`、`jit_runtime.cpp`、`jit_host.cpp`；
2. `JitRuntime` + `CodeHolder` + `x86::Assembler` 生成 `fib`；
3. prologue/epilogue；
4. `LOAD_PARAM` / `LOAD_CONST` / `LOAD_GLOBAL`；
5. `ADD` / `SUB` 的 int/int 快路径；
6. `CMP_JUMP` / `JUMP` / `JUMP_IF_*`；
7. `CALL` / `RETURN`；
8. `LOAD_NULL` / `PUSH_SCOPE` / `POP`；
9. `SKEIN_JIT_DUMP` 使用 `StringLogger` 输出反汇编。

### 6.4 Week 3 任务

1. `fib(0..30)` 差分；
2. 递归深度超限错误；
3. 非 int 参数错误；
4. 引用计数与 LSan；
5. benchmark：
   - 旧 VM；
   - 重构后 VM；
   - JIT；
6. `perf stat` 对比。

### 6.5 Week 4 任务

1. MSYS2 构建；
2. Wine 或真实 Windows smoke；
3. 检查 Windows x64 ABI：
   - rcx/rdx/r8/r9；
   - 32 字节 shadow space；
   - 16 字节栈对齐；
   - 无 red zone；
4. 检查 `libasmjit.dll` 部署；
5. 形成 Go/No-Go 报告。

---

## 7. 技术设计

### 7.1 目标架构

```text
skein_bytecode
skein_value_runtime
skein_runtime
skein_platform
      │
      ▼
  skein_jit  ── asmjit::asmjit
      │
      ▼
  skein_vm（executor）
      │
      ▼
skein_interpreter
```

- `skein_jit` 只 include `<asmjit/...>`；
- `skein_bytecode` / `skein_runtime` / `skein_interpreter` 不出现 asmjit；
- `skein_vm` 通过 `PRIVATE skein_jit` 使用 JIT；
- `skein_jit` 不反向 include `vm.h`，通过 `jit_host` 回调调用 VM 能力。

### 7.2 Frame / 调用路径重构

目标：让解释器和 JIT 共享同一套 slot/参数直接内存语义。

```cpp
struct function_vm_cache {
    skein::bc::ProtoPtr proto;
    JitCode *jit = nullptr;
    uint32_t hits = 0;
    bool compile_failed = false;
};
```

- `function_code` 增加 `void *vm_cache`，由 `vm_bridge` 管理；
- `free_function_code()` 通过 `skein_vm_forget_function_code()` 清理；
- `frame_eligible` 条件：

```cpp
bool frame_eligible =
    params_direct &&
    !has_closures &&
    !uses_defers &&
    !uses_handlers &&
    !variadic &&
    !is_generator &&
    !is_async &&
    !is_method;
```

- params 和 `slots_direct_safe` 的 fast frame slots 已迁移到 `value_t*` 数组；
- 声明/impl 方法、嵌套函数声明、for-in/match/bind/import 的捕获已走
  frame slot；
- defer 表达式在 `DEFER_PUSH` 时把引用到的局部/参数/upvalue 捕获为
  显式 `upvalue_cell`，defer 捕获不再依赖 push 时的 scope 快照；
- handler 帧仍保留 `symbol*` 兼容路径；
- JIT 始终使用自己的 frame slots。

### 7.3 Verifier 元数据

`verify()` 已经计算 CFG 栈深度。需要把它保存到 `Proto`：

```cpp
std::vector<uint32_t> stack_depth;
std::vector<uint32_t> loop_headers;  // Phase 3 使用
std::vector<uint32_t> back_edges;    // Phase 3 使用
```

JIT 使用 `stack_depth[pc]` 计算操作数栈的固定 frame 偏移，
不需要运行时维护 `sp`。

### 7.4 JIT 可编译范围

第一版只编译：

- `frame_eligible`；
- opcode 全部在支持列表内；
- 无 `fail` / `recover` / `defer`；
- 无 generator / async / await / yield；
- 无 pattern / variadic / method；
- 无 `ControlBreak` / `ControlContinue`。

`fib` 的 opcode 子集：

```text
LOAD_PARAM
LOAD_CONST
LOAD_GLOBAL
ADD
SUB
CMP_JUMP
CALL
RETURN
LOAD_NULL
PUSH_SCOPE
POP
```

`PUSH_SCOPE` 在无 name/closure 的 Proto 中可以作为 no-op。
`POP_SCOPE` 如果出现，同样处理。

### 7.5 ABI 与 frame 布局

Entry：

```cpp
using JitEntry = value_t *(*)(jit_thread_state *ts,
                              function_def *def,
                              value_t **args,
                              uint32_t argc);
```

SysV：

```text
rdi = ts
rsi = def
rdx = args
rcx = argc
rax = result / NULL(error)
```

Win64：

```text
rcx = ts
rdx = def
r8  = args
r9  = argc
rax = result / NULL(error)
```

Frame：

```text
push rbp
mov  rbp, rsp
sub  rsp, frame_size

[rbp -   8]  saved rbp
[rbp -  16]  jit_thread_state*
[rbp -  24]  function_def*
[rbp -  32]  error/status
[rbp -  40]  param[0]
...
[rbp - ...]  operand stack[max_stack]
```

- `frame_size` 编译期计算，向上对齐到 16 字节；
- Win64 需要预留 32 字节 shadow space；
- JIT helper 调用统一使用 SysV ABI（MinGW `sysv_abi`），只有 JIT entry
  本身遵循 Win64 ABI；
- 不使用 red zone；
- frame > 4KB 时回退 VM，避免 Windows stack probe。

### 7.6 Opcode lowering

**LOAD_PARAM**

```asm
mov rax, [rbp - param_base + b*8]
test rax, rax
jz abort
call dupval
push rax
```

**LOAD_CONST**

```asm
mov rax, [rip + const_ptr]
call dupval
push rax
```

**LOAD_GLOBAL**

spike 先走 helper：

```asm
mov rdi, ts
mov rsi, proto
mov edx, name_index
call skein_jit_load_global
test rax, rax
jz abort
push rax
```

Phase 2 再内联 global IC。

**ADD / SUB**

```asm
mov rax, [left]
mov rcx, [right]
cmp dword ptr [rax], T_int
jne slow
cmp dword ptr [rcx], T_int
jne slow
mov rax, [rax + 8]
sub rax, [rcx + 8]
jo overflow
call skein_intern_or_new_int
push rax
```

**CMP_JUMP**

```asm
mov rax, [left]
mov rcx, [right]
cmp dword ptr [rax], T_int
jne slow
cmp dword ptr [rcx], T_int
jne slow
mov rax, [rax + 8]
cmp rax, [rcx + 8]
jl target
```

**CALL**

spike 先走统一 helper：

```asm
mov rdi, ts
mov rsi, callee
mov rdx, args
mov ecx, argc
call skein_jit_call
test rax, rax
jz abort
push rax
```

`skein_jit_call()`：

1. 检查 callee 是否 `T_func`；
2. 查 `function_code -> vm_cache -> jit`；
3. 命中且参数匹配 → 直接调用 JIT entry；
4. 否则 → `skein_vm_call_value_args()`。

Phase 2 改为单态 inline cache。

#### 7.6.1 尾调用与字节码 TCO 的结合

字节码编译器已经把 `return f(args)` 编译成 `TAIL_CALL`，所以 JIT 不需要修改
前端或字节码格式，只需要把 `TAIL_CALL` 当成独立的终止指令处理：

1. 计算 callee 和参数，和普通 `CALL` 一样放在固定 frame 偏移；
2. 先尝试 **自尾递归 frame 复用**：
   - callee 是 `T_func`；
   - callee 的 `function_code` 与当前 `function_code` 相同；
   - `argc == nparams`；
   - 无 pattern 参数、非 method；
   - 类型约束匹配。
3. 命中时：
   - 释放当前帧的旧参数值；
   - 把参数指针移动到参数 slot；
   - pop 到函数入口的 scope mark；
   - 直接 `jmp` 到 body 起点，不再次执行 `skein_jit_enter`；
   - 不增加 `vm_call_depth`，不重复 `diag_push_call`。
4. 未命中时走通用 trampoline：
   - JIT 生成 `skein_jit_tail_request`，把 callee/args 所有权交给
     `thread_state`；
   - 当前 JIT entry 返回后，`skein_vm_try_jit_call_impl` 的 driver loop
     解析下一个 callee：
     - 若仍是 JIT 函数，直接进入下一个 JIT entry；
     - 否则回退 VM；
   - driver 在循环中保留 tail 链的模块上下文和诊断帧，结束时恢复根上下文；
   - 因此互尾递归和任意 JIT 函数之间的尾调用都不会增长 C++ 调用栈。

这样 JIT 与字节码 TCO 的语义一致：都是复用当前逻辑帧，只是 VM 复用
`Executor` 的 fast-frame，JIT 复用机器栈上的 JIT frame。

### 7.7 所有权与错误

- 每个 operand stack 槽、param 槽持有 1 个引用；
- `Load*` 产生 owned 引用；
- `Store/Pop/Return` 消费引用；
- `NULL` 只表示错误；
- `null` 使用 `newnull()` 单例；
- 统一 `.abort` 路径释放整个 frame；
- 所有 JIT 可调用 helper 使用 `extern "C" noexcept`；
- 不允许 C++ 异常穿过 JIT 帧。

错误路径：

```text
opcode helper 返回 NULL / 设置 runtime_error_count
  → jump .abort
  → epilogue 释放 slots/operand stack
  → 返回 NULL
  → 调用者检查 runtime_error_count
```

### 7.8 热点与缓存

```text
hits++
  → 达到 SKEIN_JIT_THRESHOLD
  → jit_compile(proto)
  → JitRuntime::add
  → function_vm_cache->jit
  → 后续直接调用
```

- `SKEIN_JIT_THRESHOLD` 默认 100；
- `SKEIN_JIT_THRESHOLD=1` 用于测试；
- 编译失败置 `compile_failed`，永久回退 VM；
- 代码缓存设总大小上限，超限停止编译；
- `JitRuntime::add/release` 用全局 mutex 保护；
- 执行路径不加锁。

### 7.9 诊断

- JIT 只在可能失败的 helper 前更新 `diag_set_location`；
- 纯 int 运算错误走冷路径，先设置位置再 `yyerror`；
- 进入/退出 JIT 函数时维护 `diag_frames`；
- 错误文本、行列和退出码必须与 VM 一致。

### 7.10 Code cache 生命周期

- `JitCode` 由 `function_vm_cache` 持有；
- `skein_vm_forget_function_code()` 调用 `JitRuntime::release`；
- `skein_vm_shutdown()` 统一清理所有 JIT 代码；
- `JitRuntime` 使用函数局部静态对象，避免静态析构顺序问题。

---

## 8. Windows / MinGW

### 8.1 依赖

MSYS2：

```bash
pacman -S mingw-w64-ucrt-x86_64-asmjit
```

如果 CMake 找不到：

```bash
cmake ... -DCMAKE_PREFIX_PATH=$MINGW_PREFIX
```

MSYS2 的 asmjit 包默认构建共享库，运行时需要 `libasmjit.dll`：

- 开发/CI：把 `$MINGW_PREFIX/bin` 加入 `PATH`；
- 发布单文件：随包附带 DLL，或自己构建静态 asmjit。

### 8.2 Win64 ABI

- 参数：rcx、rdx、r8、r9；
- 返回：rax；
- 调用 helper 前预留 32 字节 shadow space；
- 调用前 `rsp % 16 == 0`；
- 不使用 red zone；
- 保存 rdi/rsi/rbx/rbp/r12-r15（如果使用）；
- 大帧使用 stack probe，或直接回退 VM。

### 8.3 调用 helper

Windows x64 地址空间很大，`call rel32` 可能不够：

```asm
mov r11, imm64
call r11
```

### 8.4 Unwind / 调试

- asmjit 不生成 Windows `.pdata` / SEH unwind 表；
- spike 不承诺异常跨 JIT；
- 调试器/性能工具可能无法展开 JIT 栈；
- 生产阶段如需完整栈回溯，使用 `RtlAddFunctionTable` 或自定义 unwind 信息。

### 8.5 asmjit 的 MinGW 支持

asmjit 官方文档：MinGW **“Reported to work, but not tested in our CI environment”**。
MSYS2 提供 `mingw64` / `ucrt64` / `clang64` / `clangarm64` 包。
因此可行，但必须在 Week 1 自己做 smoke test，不能假设。

---

## 9. 测试与验收

### 9.1 开关

```text
SKEIN_JIT=0/1
SKEIN_JIT_THRESHOLD=100
SKEIN_JIT_DUMP=1
SKEIN_JIT_STRESS=1
SKEIN_JIT_DEBUG=1
```

### 9.2 功能

- `fib(0..30)` Linux/MinGW 输出一致；
- 递归深度错误文本一致；
- 类型错误一致；
- `SKEIN_JIT_THRESHOLD=1` 全量 `fib` 通过；
- `SKEIN_JIT=0` 与旧 VM 完全一致。

### 9.3 性能

| 指标 | 目标 |
|---|---:|
| `fib(27)` JIT vs 重构后 VM | ≥ 2x |
| `fib(27)` JIT vs 旧 VM | ≥ 2.5x |
| 启动开销增加 | < 5ms |
| 单函数代码大小 | < 4KB |

asmjit 不会自动让代码更快；如果只有 1.3x，说明瓶颈在装箱/调用 helper，
不是编码器。

### 9.4 Sanitizer

- JIT-off：ASan/UBSan/LSan 全量；
- JIT-on：LSan 检查堆泄漏（JIT 代码本身不被 ASan 插桩）；
- JIT 代码使用边界检查/guard page 作为补充。

### 9.5 边界检查

`tools/check_boundaries.py` 增加：

- `asmjit/` 只允许出现在 `src/jit/`；
- `src/jit/` 不允许 include `ast.h` / `compiler.h` / `eval.h` / `eval_internal.h`；
- `skein_jit` 只依赖 bytecode/value runtime/runtime/platform + asmjit。

### 9.6 CI

- Linux：GCC + Clang，Debug/Release；
- Windows：MSYS2 `mingw64` 或 `ucrt64`，Release；
- 可选：Linux 交叉编译 + Wine smoke；
- `SKEIN_JIT=0/1` 两套都跑。

---

## 10. Go / No-Go

### Go

- asmjit 在 Linux 和 MinGW 上可构建、可执行；
- `fib` ≥ 2x 重构后 VM；
- 输出、错误、递归深度完全一致；
- 无泄漏、无崩溃；
- frame/call 重构本身让 VM 提升 ≥10%；
- 代码生成/缓存/错误路径可复用到 Phase 2。

### No-Go

- `fib` < 1.3x；
- asmjit 在 MinGW 上无法在 2 天内修复；
- codegen 调试超过 3 天仍不稳定；
- frame 重构破坏现有语义且无法修复；
- JIT 维护成本明显高于解释器优化收益。

No-Go 时保留 Week 1 的重构，转去做 frame-based VM、
computed goto、unboxed slot，这些本身可带来 1.5–2x。

---

## 11. 风险

| 风险 | 对策 |
|---|---|
| asmjit MinGW 非官方 CI 支持 | Week 1 smoke；失败则暂时关闭 Windows JIT |
| 系统包版本旧（1.11.0） | 按 1.11 写；`static_assert` 最低版本 |
| 不同发行版 asmjit 版本不一致 | CMake 只要求 config；编译期版本检查 |
| 手动 `ASMJIT_NO_*` 导致 ABI 不一致 | 不手动定义；使用 imported target |
| MSYS2 asmjit 是 shared | 开发时带 DLL；发布时静态构建或随包分发 |
| Win64 ABI 细节 | smoke 分层：return 42 → add → call helper → fib |
| 代码生命周期 | `function_vm_cache` 释放时 `JitRuntime::release` |
| JitRuntime 非线程安全 | add/release 加锁；执行不加锁 |
| Windows unwind | spike 不承诺异常跨 JIT；后续 `RtlAddFunctionTable` |
| sanitizer 盲区 | JIT-off 跑 ASan/UBSan；JIT-on 跑 LSan + 差分 |
| W^X | 默认策略；需要时 `kUseDualMapping` |
| 代码缓存增长 | 设置总大小上限，超限退化为 VM |

---

## 12. 后续阶段（Go 之后）

### Phase 2：baseline JIT（3–4 周）

- slot / param / global / 常量；
- int/float/bool 运算与比较；
- 字段读取、dict 索引 IC；
- 普通调用、尾调用；
- 单态 inline cache；
- 覆盖 `slot` / `field` / `index` / `global` 基准。

### Phase 3：OSR + 完整 frame VM（3–4 周）

- 移除每帧 `Executor` 分配和 C++ 递归；
- frame slot 统一；
- loop header / back-edge 元数据；
- OSR 进入 JIT；
- deopt 基础。

### Phase 4：类型特化 / tracing（4–6 周）

- unboxed int 寄存器运算；
- 小整数表 inline；
- 单态字段/调用缓存内联；
- 如仍有收益，再评估 tracing。

### 独立决策

- AOT / 字节码序列化；
- 预编译 stdlib；
- 字节码 ABI 冻结。

---

## 13. 待确认事项

1. `SKEIN_ENABLE_JIT` 默认值：保持 OFF，还是 Linux 上默认 ON？
2. Windows/MinGW 是否进入正式发布范围，还是只做 beta smoke？
3. Phase 2 是否继续使用 asmjit，还是先评估 `x86::Compiler` 的 prologue/regalloc 能力？
4. 是否把本文链接加入 `README.md` 的文档列表？
5. 是否需要在 Phase 1 就加入 `SKEIN_JIT_DUMP` 的生产开关，还是只保留 debug 构建？

