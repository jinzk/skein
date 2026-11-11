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

旧名 `ffi.call_async` 保留为 deprecated alias，行为与 `call_task` 一致；
新代码请使用 `call_task`，后续版本会移除旧名。

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

`skein_module` v1：

- 纯 C ABI、opaque handle；
- `skein_api` 只追加字段，使用新字段前检查 `size`；
- 支持静态注册、`std.module.load` 动态加载、`std.module.unload` 延迟卸载；
- 绑定函数和 userdata 持有模块状态，最后一份引用释放时调用 `fini` 并 `dlclose`；
- 静态模块不可卸载。

## 7. 稳定性策略

- manifest v1 冻结；破坏性变更走 v2；
- `skein_module` ABI v1 冻结，只追加字段；
- FFI Beta 期间允许修正边缘语义和错误信息，但避免破坏常见用法；
- 已知限制优先修正确性，不新增 ABI 功能。

## 8. 已知限制

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
