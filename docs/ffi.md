# FFI 支持范围与稳定性

状态：**Beta**。功能可用，但接口和边缘语义仍可能调整。

本文是 FFI 的正式支持范围、所有权契约和已知限制。API 用法见
[README](../README.md) 和 [SYNTAX](../SYNTAX.md)。

## 1. 支持边界

只支持 **C ABI**：

- C 编译的库；
- C++ 中以 `extern "C"` 导出的函数和全局变量；
- 纯 C 头文件，或只包含 C 声明的 C++ 头文件。

不支持 C++ ABI：类、模板、异常、RTTI、引用、重载、`std::` 类型。

## 2. 类型与调用

支持：

- 标量：`i8/u8/i16/u16/i32/u32/i64/u64/f32/f64/usize/isize/cstring/bytes/buffer/void`
- `struct`（标准布局）、`pointer`、定长 `array`、`opaque`、`callback`
- struct / array by value，指针参数和返回值
- 变参函数（必须显式提供变参类型）
- `c`/`cdecl`、`stdcall`、`fastcall`、`thiscall`（平台支持时）
- 全局变量读写（`"const": true` 为只读）
- 常量（`#define` 和 enum）
- 指针辅助：`ffi.symbol`、`ffi.string`、`ffi.address_of`、`ffi.offsetof`

ABI 校验：

- 加载 manifest 时校验类型、布局、递归 by-value 类型和参数索引；
- libffi >= 3.3 时用 `ffi_get_struct_offsets` 对比字段 offset/size/alignment；
- libffi < 3.3 时 CMake 配置阶段直接拒绝启用 FFI。

## 3. 所有权契约

### returns_owned

`returns_owned` 表示返回的指针由运行时负责释放：

```json
"point_new": {
  "result": "pointer",
  "args": ["f64", "f64"],
  "returns_owned": {"free": "point_free"}
}
```

- `free` 必须接收一个指针参数；
- 默认按 `void(void*)` 调用；
- 如果 free 返回非 void，可声明：
  ```json
  "returns_owned": {
    "free": "point_free",
    "free_result": "i32"
  }
  ```
- 目前不支持带额外参数的 free；`free_args` 只接受一个 pointer/opaque 参数。

### consumes

`consumes` 表示参数所有权转移给 C：

- 只允许 pointer / opaque 参数；
- 调用后被传递的 handle 标记为已消费；
- **同一块内存的其他 alias/view 在 C 接管后不应再访问**；
- 运行时不会替 C 释放这块内存；由 C 负责。

### retains

`retains` 表示 C 会在调用后继续保存参数：

- 支持 callback、pointer、struct、cstring、bytes、buffer；
- cstring/bytes 会保留参数存储，直到 library 释放；
- 如果 C 只保存 callback，优先在 manifest 声明 `retains`；手动调用可用
  `std.ffi.retain(lib, value)`。

## 4. 异步契约

`std.ffi.call_task`：

- 在 worker 线程执行 C 调用；
- 当前 fiber 阻塞，事件循环继续运行；
- **不可取消，也没有超时**；
- 每次调用创建一个 worker 线程，不适合高频短调用；
- 可能永久阻塞或不受信任的 C 调用应放到外部进程。

旧名 `ffi.call_async` 已移除；请使用 `call_task`。

同步 `ffi.call` 回调仍要求同线程、同 fiber、活动调用范围内。

## 5. C 头文件生成

`skein --ffi-gen` 支持：

- typedef、标准布局 struct、enum、指针、定长数组、函数声明、函数指针 typedef；
- struct 里的函数指针字段（inline callback）；
- `extern` 全局变量、`const` 只读全局变量；
- 对象式宏常量、整数/浮点表达式；
- 被包含头文件里的 typedef/struct/enum（只导出 `--header` 目标符号）；
- `extern "C" { ... }` 块；
- 目标 ABI 探测（`cc -dM`）。

拒绝或跳过：

- C++ ABI / C++ 头文件；
- union、packed、bitfield、柔性数组、匿名 struct/union；
- 影响布局的属性（`aligned`、`vector_size`、`mode` 等）；
- `long double`、`_Complex`、`__int128`；
- `_Thread_local` / `__thread`；
- 非受支持的调用约定、变参回调。

默认 fail-closed：遇到不支持的目标头声明直接失败；依赖头里的不支持声明只记
warning。`--tolerate-errors` 可降级为 warning 并继续生成。

`--allow-opaque-types` 是 opt-in 的 best-effort 模式：解析失败的
struct/union/typedef 会注册成 opaque，使用其指针的函数继续生成；按值使用
opaque 类型的函数、变量和 struct 会被跳过并记录 warning。它适合
`mysql.h` 这类内部类型复杂、但公开 API 主要传 opaque 指针的头文件；不能
替代人工确认 ABI。

## 6. 原生模块 ABI

`skein_module` v1 / v2 / v3 / v4：

- 纯 C ABI、opaque handle；
- `skein_api` 只追加字段，使用新字段前检查 `size`；v2 追加 `call_task` /
  `task_cancelled`，用于把耗时函数导出为标准 `task`（见 `docs/stdlib.md`）；
  v3 追加 `new_string_len`，从带长度的 buffer 构造字符串，不要求 NUL 结尾；
  v4 追加 `post_callback` / `cancel_post_callback`，让 worker 线程把 Skein
  函数投递到 owner 线程执行（见下文）；
- v1 描述符仍可加载：`abi_version` 接受 1、2、3 和 4；
- 支持静态注册、`std.native.load` 动态加载、`std.native.unload` 延迟卸载；
- 绑定函数和 userdata 持有模块状态，最后一份引用释放时调用 `fini` 并 `dlclose`；
- 静态模块不可卸载；
- `std.native.load` / `std.ffi.open` 在 check-only 模式下被禁止；
- 第三方 module 内的原生加载默认禁止，需 `SKEIN_ALLOW_NATIVE=1` 显式 opt-in；
  `0` / `false` / `off` / `no` 会全局禁用原生加载。源码形式发布的 module
  用 `skein build` 显式编译，详见 `docs/tutorial.md`；
- `contrib/mysql` 是用真实 MySQL C client 验证上述 ABI 的可选示例插件，
  也演示源码形式 native module 的 `skein get` / `skein build` 流程、
  ABI v2 异步 `query_task` 和 ABI v3 `new_string_len` 的安全文本传输，见
  [`contrib/mysql/README.md`](../contrib/mysql/README.md)。

## 7. 稳定性策略

- manifest v1 冻结；破坏性变更走 v2；
- `skein_module` ABI v1/v2/v3/v4 冻结，只追加字段；
- FFI Beta 期间允许修正边缘语义和错误信息，但避免破坏常见用法；
- 已知限制优先修正确性，不新增 ABI 功能。

## 8. 原生模块 ABI v4：owner 线程事件投递

### 8.1 归属：plugin，不是 FFI

两者都涉及"后台回调脚本"，但能力现状不同：

- FFI 组件**已有**跨线程回调：`ffi.callback` 把 Skein 函数包装成 C 函数指针，
  后台线程中触发时由 `ffi_callback_entry` 走 `background_queue` / 阻塞 job，
  在 owner 线程的 `background_drain` 里执行（见
  `src/builtins/ffi/ffi_callbacks.cpp`）。FFI 不需要 `post_callback`；
- plugin 组件原先**没有**对等机制：`call_task` 的 `work()` 在 worker 线程，ABI
  禁止调用任何 `skein_api`，`finish` 只在任务完成时一次性回调。v4 的
  `post_callback` 补上这个缺口，作为 `skein_api` 字段添加，只有 native
  module 能访问。

### 8.2 API

```c
typedef uint64_t skein_post_id;

/* 可从任意线程调用，包括 call_task 的 worker。callee 与 args 被复制，
 * 函数返回后调用方仍持有原句柄。返回值非 0 表示成功，0 表示失败。 */
skein_post_id (*post_callback)(skein_context *ctx, const skein_value *callee,
                               const skein_value *const *args, size_t argc);

/* 撤销尚未执行的回调；线程安全。返回 false 表示已执行/已撤销/id 未知。 */
bool (*cancel_post_callback)(skein_context *ctx, skein_post_id id);
```

回调在 owner 线程执行，可正常使用值 API。脚本 pump 事件循环时投递队列先于
task 完成队列被 drain，因此 worker "先投递再返回" 能保证回调顺序。脚本退出时
仍未执行的回调被丢弃而不是执行。

### 8.3 实现要点

- 投递队列在 `src/interpreter/native_async.cpp`，与 `call_task` 共用
  `drain_native_tasks` 钩子（由 `fiber_pump` 驱动）；FFI 的 `drain_background`
  是另一条队列，两者数据类型不同，不合并；
- `post_callback` 在投递线程复制参数（值分配器与缓存均为 thread-local），
  但 `skein_context` 只复制函数指针表，不跨线程持有原指针；
- `call_task` 的 worker 线程会临时安装 module owner guard，投递的回调因此能
  保持插件动态库存活到执行结束；
- 可取消 id 为进程内单调递增的 `uint64_t`，取消只设置标记，值始终由 owner
  线程释放；
- 回调内的脚本错误通过 `eval_capture_*` 捕获并报告，不会中断宿主事件循环。

### 8.4 验证

`tests/native_module.cpp` 增加 `post_events` / `post_now` / `cancel_post`
导出；`tests/cases/stdlib_builtins.py` 覆盖 worker 线程投递、owner 线程取消、
参数生命周期与回调错误隔离。

### 8.5 后续计划

- **P2：惰性迭代协议（候选）**：新增 native iterator userdata，约定
  `next()` / `close()`；优先用现有 userdata + wrapper 实现，不改 ABI；
- **P3：C++ 实现 Skein protocol（暂不计划）**：需要 vtable 双向绑定和
  跨语言类型检查，复杂度高、收益场景少。继续推荐 wrapper 模式：Skein struct
  定义接口，方法体调用 native 函数；方法可以直接写在 struct 内，也可以随后用
  `impl Type` 追加，两种写法对 FFI 调用点没有区别。

### 8.6 兼容性与发布约束

- 所有新增字段只能 append 到 `skein_api` 末尾并 bump `abi_version`，旧模块
  通过 `size` 检查安全忽略；
- `post_callback` 需要 v4 描述符或至少 v4 的 `api->size`；v1–v3 模块继续按
  现有方式加载，不受影响；
- ABI 变更必须同步更新 `contrib/mysql/native/skein_module.h` 副本，并保持
  `tests/mysql_plugin.py` 的 vendored header 一致性检查通过；
- 在无真实用例驱动前不新增 ABI 字段，避免为假设需求扩大契约。

## 9. 已知限制

- 没有 Windows / 32 位 / ARM 的 CI 验证；
- 回调固定使用默认 ABI；
- `returns_owned` 不支持带额外参数的 free；
- `consumes` 后 alias/view 的安全性由调用者保证；
- `ffi.call_task` 不可取消、无超时；
- `ffi.errno` 在顶层语句之间可能被解释器内部 I/O 重置；
- 没有 manifest/bindgen fuzz；
- 没有自动 sidecar manifest 查找。

## 9. 验证

- `ctest` 全绿；
- FFI 用例覆盖 struct/pointer/array/callback/manifest/ownership/global/variadic；
- `sqlite3` 和 `zlib` 真实库集成测试（库存在时运行）；
- ASan/UBSan 下 FFI 用例通过。

## 10. 安全说明

FFI 可以执行任意本地代码，默认不应向不受信任脚本开放。解析 manifest 和 C 头文件
的代码仍应视为处理不可信输入，长期建议补 fuzz。
