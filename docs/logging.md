# Skein 日志方案（std.log）

> 当前状态与总优先级见 [`roadmap.md`](roadmap.md)。

- 状态：已实现（v1，同步写入）
- 相关模块：`std.log`
- 实现位置：`src/builtins/log.cpp`

## 1. 目标

为 Skein 脚本提供结构化日志，而不是让每个项目自己拼接 `println`：

- 级别过滤
- 结构化字段与上下文继承
- 文本 / JSON 两种输出
- 标准流、文件、自定义 writer
- 与普通程序输出隔离（默认 stderr）

不在 v1 范围内：

- 文件轮转
- 异步写入
- 采样、限流
- 远程 sink

## 2. API

```text
std.log.new(options?)                  → logger
std.log.trace/debug/info/warn/error(message[, fields])
std.log.flush()
std.log.set_level(level)

logger.trace/debug/info/warn/error(message[, fields])
logger.log(level, message[, fields])
logger.with(fields)                    → logger
logger.set_level(level)
logger.flush()
```

`logger` 是内置 `userdata` 类型：

```text
kind_of(logger)   // "userdata"
str(logger)       // "logger(app)" 或 "logger"
```

模块级函数写入默认 logger（stderr、`info`、文本格式）。

> 数学函数 `std.math.log`（需要 `import std;`）与 logger 概念容易混淆，
> 示例中的 logger 变量使用 `lg`。

## 3. 选项

| 选项 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `level` | string | `info` | `trace` / `debug` / `info` / `warn` / `error` |
| `format` | string | `text` | `text` 或 `json` |
| `output` | string | `stderr` | `stderr` / `stdout` / `file` |
| `path` | string | — | `output=file` 必填；其他输出禁止 |
| `append` | bool | `true` | 文件是否追加 |
| `time` | string | `local` | `local` 或 `utc` |
| `name` | string | `""` | logger 名称 |
| `fields` | dict | `{}` | 基础上下文字段 |
| `flush` | bool | `true` | 每条记录后 flush |
| `on_error` | string | `fail` | `fail` / `warn` / `ignore` |
| `writer` | function | — | 接收格式化后的整行；与 `output` / `path` 互斥 |

未知选项、未知级别、错误类型都会立即报错，避免配置拼写错误被静默忽略。

## 4. 输出格式

### text

```text
2026-11-20T12:34:56.789+08:00 INFO  app: request done service=demo ms=12
```

- 时间是 ISO 8601，本地时间带 `+08:00` 这类偏移，UTC 以 `Z` 结尾；
- 级别右补空格到 5 列；
- `name` 非空时输出 `name: `；
- 字段按插入顺序输出为 `key=value`。

### json

```json
{"time":"2026-11-20T12:34:56.789Z","level":"info","logger":"app","message":"request done","fields":{"service":"demo","ms":12}}
```

- 每行一个完整 JSON 对象，便于采集；
- `bytes` 编码为 `"hex:..."`；
- `float` 使用最短可逆表示；`NaN` / `Inf` 退化为字符串；
- 嵌套 `list` / `dict` 递归转换，深度上限 8；
- 无法表示的值（函数、userdata 等）退化为 `str(value)`。

## 5. 语义

### 级别过滤

```text
TRACE < DEBUG < INFO < WARN < ERROR
```

低于当前级别的调用不会格式化字段，也不会写输出。

### 字段与上下文

```text
lg.with({"request_id": "r1"}).info("done", {"ms": 12});
```

- 输出同时包含基础字段、`with` 字段和调用字段；
- 同名键覆盖；
- `with` 返回新 logger，不修改原 logger；
- 派生 logger 与父 logger 共享输出目标（文件只打开一次）。

### 输出与错误

- 默认写 stderr，保证 `print` / 脚本结果不被污染；
- `output=file` 在创建 logger 时打开文件，失败立即报错；
- 写入失败按 `on_error` 处理：`fail` 抛错，`warn` 写 stderr 警告，`ignore` 丢弃；
- `writer` 同步调用，抛出的错误直接传播给调用方；
- `flush()` 显式刷盘；`flush=false` 时由用户负责。

### 线程

- sink 内部使用互斥锁，多个 logger 可安全共享同一文件目标；
- `writer` 是脚本函数值，不能跨线程序列化；
- `logger` 不参与线程传输。

## 6. 测试

`tests/cases/logging.py` 覆盖：

- text 格式、级别过滤、`with` 上下文与字段覆盖
- json 格式与转义（引号、null、list、bool、float）
- 文件输出、追加与覆盖模式
- 自定义 writer 与 `output` 冲突
- 模块级默认 logger 与 `set_level`
- 未知选项 / 级别、缺少 path、消息类型错误

## 7. 后续

- 文件轮转（按大小 / 按天）
- 异步写入与有界队列
- 采样与限流
- 自定义 sink 的跨线程桥接
