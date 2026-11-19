# 包管理设计

本文记录当前包管理实现、锁与信任模型，以及后续计划。

## 1. 文件

- `skein.mod`：项目依赖和完整构建列表
- `skein.sum`：模块内容 checksum（唯一信任根）
- `SKEINMODCACHE`：全局模块缓存根

默认缓存：

```text
Linux/macOS: ~/.skein/mod
Windows:     %LOCALAPPDATA%\skein\mod
```

## 2. 命令

```text
skein mod init [module/path]
skein get <module/path>[@version]
skein get <module/path>@<version> --offline
skein build
skein mod download [--offline] [--force]
skein mod verify
skein mod tidy [--offline] [--force]
skein mod prune [--dry-run] [--all]
skein mod clean [--dry-run] [--force]
```

## 3. 依赖解析

- 字符串 import 只解析 `skein.mod` 中 `require` 的第三方 module。
- MVS（最小版本选择）：同一 module path 选择最高 semver。
- `/vN` 是 module path 的一部分，不同 major 版本独立存在。
- `skein.mod` 是最终完整构建列表，非直接依赖标记为 `// indirect`。
- 运行时只读 `skein.mod` / `skein.sum` / 缓存，不隐式联网。

## 4. 信任模型

- `skein.sum` 使用 `h1:` dirhash。
- 缓存内容与 checksum 不一致时硬失败。
- 已有 checksum 时，下载内容不匹配也硬失败。
- `--force` 是显式刷新并重写 checksum 的唯一入口。
- 缓存存在但当前项目没有 checksum 时，不直接信任缓存；在线重新下载后写入。
- 首次下载仍然信任网络/镜像，这是 SumDB 需要解决的问题。

## 5. 锁

### 项目锁

```text
<project>/.skein/lock
```

- `get` / `tidy` / `download`：独占
- `build`：共享
- `mod init`：独占

`.skein/` 是本地锁状态，不应提交。

### 缓存锁

```text
<SKEINMODCACHE>.lock
```

- `get` / `build`：共享
- 运行时模块加载：进程生命周期共享锁
- `mod clean`：独占

运行时在第一次加载外部 module 时懒获取共享锁，因此运行中的脚本会阻止 `mod clean` 删除缓存。

默认锁等待不设上限；设置 `SKEIN_LOCK_TIMEOUT_MS=<毫秒>` 后，等待超时会返回错误，而不是无限阻塞。

### 模块锁

```text
<SKEINMODCACHE>/.locks/<module@version hash>.lock
```

- 同一 `module@version` 的下载/构建串行化。

## 6. 原子写入

`skein.mod` / `skein.sum` 先写临时文件，再替换目标文件。

成功路径：

1. 解析完整依赖图
2. 准备所有模块
3. 批量写入 checksum
4. 更新 `skein.mod`

失败路径：

- `skein.mod` 保持原样
- 已完成模块的有效 checksum 尽量保留
- 不留下半写文件

## 7. 缓存裁剪

```text
skein mod prune
skein mod prune --all
```

- 默认只列出当前项目 `skein.mod` 未引用的缓存 `module@version`，不会删除。
- 必须显式传 `--all` 才会删除。
- `--all` 同时清理对应的 native `.build` 产物。
- 全局缓存可能被其他项目共享；`--all` 是显式接受该风险的操作。
- `--dry-run` 只预览。
- 不修改项目 `skein.mod` / `skein.sum`。

## 8. Native module

源码 native module 由 `skein build` 显式构建，产物位于：

```text
<SKEINMODCACHE>/.../.build/<os>-<arch>-<compiler>/
```

策略：

- 第三方 module 的 native 加载要求 `SKEIN_ALLOW_NATIVE=1`
- check-only 工具不加载/执行 native
- 产物不进入 `dirhash` / `skein.sum`
- `skein build` 写入 `artifact.meta`：
  - 源码 hash
  - 产物 hash
  - 编译器
  - 编译器版本
- `std.module.native` 加载前校验源码 hash 与产物 hash
- 编译器可用时，版本变化会使旧产物失效
- 部署环境没有编译器时仍可加载已通过 hash 校验的产物

尚未完成：

- 编译 flags 的显式元数据
- 可复现构建
- 交叉编译

## 9. 后续计划

### Milestone 1（当前）

- native 产物完整性（已完成）
- `mod prune` 安全语义（已完成）
- 锁超时与诊断（已完成）
- Linux/macOS 并发锁测试

### Milestone 3

- SumDB / 首次下载信任
- Go 风格 proxy
- 私有模块认证
- `mod graph` / `mod why`
- JSON 输出
- `retract` / `exclude`

### 暂缓

- 没有可信镜像运营方前的完整 proxy
- 未确认公共生态前的 vendor / workspace
