# 类型、方法与函数缓存的线程安全

本文记录 P2.5 的并发模型和剩余假设。适用范围：

- struct / enum / protocol 类型注册表
- 类型方法表
- 跨线程方法传输
- VM proto / inline cache / JIT 缓存

## 结论

- 类型注册表由全局递归锁保护。
- 方法表读使用共享锁，声明/附加使用独占锁。
- 跨线程传输的方法安装为**接收线程的本地覆盖**，不修改共享类型定义。
- 函数 bytecode cache 和 inline cache 按线程隔离。
- 跨线程继承的 proto 会在接收线程复制，inline cache 不共享。
- JIT 环境开关使用 `std::call_once` 初始化。
- `ref` / `weak` 对象注册表严格线程本地，不跨线程共享，也不需要锁。
- TSan 覆盖线程压力、key 语义、worker VM、env/FFI 和网络测试。

## `ref` / `weak` 与 GC

实现：`src/runtime/refvalue.{h,cpp}`

- 每个线程维护独立的 `ref_object` 注册表、strong/weak handle 和 generation；
- GC mark 只沿强 `ref` 边走，sweep 后递增 generation 使 weak handle 失效；
- 不存在跨线程 GC root、共享 ref heap、generation 迁移或保护注册表用的锁；
- `value_reaches_any()` 在 `ref` 边界停止，普通值环仍按原逻辑拒绝；
- channel payload 校验和序列化路径递归拒绝 `ref` / weak，错误提示引导
  `std.ref.snapshot()` 或 `std.arena` id；跨线程传递的是接收侧值快照。

## 类型注册表

实现：`src/runtime/structtype.cpp`

全局结构：

```text
struct_types
enum_types
protocol_types
logical_structs
logical_enums
logical_protocols
source_aliases
```

锁：

```text
std::recursive_mutex type_registry_mutex
```

保护范围：

- 类型查找
- 类型创建 / prepare / finalize / rollback
- alias 注册
- protocol 关系检查
- 类型名解析
- shutdown

使用递归锁是因为类型解析和创建内部会互相调用。

## 方法表

锁：

```text
std::shared_mutex type_method_mutex
```

- `structtype_method()`、`structtype_method_names()`、
  `structtype_protocol_default()` 使用共享锁。
- `structtype_add_method()`、`structtype_add_protocol_default()` 使用独占锁。
- struct inline 方法与 `impl Type` 追加的方法写入同一 `type_def::methods`；
  重复名检查和 protocol 默认方法冲突解析也走这张表。
- `structtype_visit_functions()` 先在锁内收集 `shared_ptr`，释放锁后再回调，
  避免 GC 回调期间持锁。

### 线程本地方法覆盖

跨线程 payload 中的方法不会写回共享 `type_def::methods`。  
接收线程使用：

```text
thread_methods[type_def][method_name] = function_def
```

这些覆盖会被以下路径看到：

- 方法查找
- type info 反射
- GC 函数访问
- 嵌套线程方法传输
- 方法值提取

不同 worker 因此不会互相覆盖方法。

## 类型定义字段

`struct_def->fields`、`enum_def->variants`、`protocol_def->methods` 等字段在类型
finalize 后视为只读。

当前约定：

- 类型声明/解析由 parser/package 锁串行化；
- finalize 完成后不再修改字段和 variant 列表；
- 运行时直接读取这些字段，不再逐次加锁。

这是当前审计中保留的主要假设。若未来允许运行时动态修改类型字段，需要为
类型定义增加独立的读写锁或不可变快照。

## VM proto 与 inline cache

`bc::Proto` 含有可变 inline cache：

- `name_ic`
- `member_ic`
- `index_read_ic`

这些 cache 不能跨线程共享。

### 线程本地 function cache

`function_vm_cache` 存在 thread-local `vm_caches` 中：

- 每个线程有独立的 proto cache 和 JIT 状态；
- `function_code::vm_cache` 只作为跨线程 proto 交接的只读来源；
- 接收线程复制 proto 并清空 inline cache；
- `cache_transfer_mutex` 保护 `function_code::vm_cache` 的交接和释放；
- 释放 cache 时只清理属于当前线程的指针。

## JIT 开关

`jit_threshold()` 和 `jit_loop_entry_enabled()` 使用 `std::call_once` 初始化，
避免多 worker 首次调用时竞争静态变量。

## 测试

常规：

- `key_semantics`
- `thread_stress`
- `worker_vm_stress`

TSan：

```text
thread_tsan_test
key_semantics_tsan_test
worker_vm_tsan_test
env_ffi_tsan_test
net_tsan_test
```

全部在 `TSAN_OPTIONS=halt_on_error=1` 下通过。

## 剩余限制

- 类型字段/variant 的直接读取依赖 finalize 后只读约定。
- 当前 TSan 覆盖不等于全量并发证明。
- 新增共享全局缓存时，必须明确：
  - 是否线程本地；
  - 是否需要锁；
  - 是否包含可变 inline cache；
  - 生命周期是否可能跨越线程退出。
