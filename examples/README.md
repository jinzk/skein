# Skein 示例

这个目录集中存放可运行的特性示例。每个子目录对应一个主题，包含简短说明和
可以直接执行的 `.skein` 文件。测试代码仍然放在 `tests/`，但测试可以引用这里的
示例，避免同一份用法在两处漂移。

## 当前示例

- [`mysql/`](mysql/README.md)：用真实 MySQL/MariaDB C client 验证 native
  module / plugin ABI，演示源码形式 module 的 `skein get` / `skein build`、
  ABI v2 异步 `query_task` 和 ABI v3 `new_string_len`。

## 新增示例约定

- 目录名使用小写短名，例如 `mysql/`、`concurrency/`、`ffi/`；
- 每个示例至少包含 `README.md` 和一个可运行的 `.skein` 入口；
- README 写明运行命令、需要的外部依赖和相关环境变量；
- 需要数据库、网络服务或原生库的示例要说明如何在没有服务时跳过；
- 后续语言特性、标准库模块、并发、网络、FFI / native module 的用法都优先在
  这里补充示例，再由 `tests/` 中的回归测试引用或验证。
