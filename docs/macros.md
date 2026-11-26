# Skein 宏（第一批：内置语法宏）

- 状态：第一批已完成；用户自定义宏暂不纳入
- 相关模块：`std.assert`、`std.log`、`std.result`
- 实现位置：`src/interpreter/macros.cpp`、`src/interpreter/parser.y`

## 1. 目标

Skein 的第一批宏只做**受限 AST 级语法脱糖**。宏在语法分析阶段展开，展开结果
随后进入普通的 name resolution、typecheck、bytecode/JIT 流程。

宏不是文本替换，不是编译期任意执行，宏名也不是一等值。宏调用展开后是普通
表达式。宏不能访问类型信息，
不能创建新的类型约束、protocol 能力、specialization 或转换规则。

> 泛型负责类型抽象；宏负责语法抽象。

## 2. 调用形式

```text
name!(expr, ...)
name! { statements }
name!(expr, ...) { statements }
```

规则：

- `!` 紧跟被调用的宏名；
- 括号中只能是表达式参数；
- `{}` 中是一个尾部 block；
- 最多一个尾部 block；
- block 不能放在括号内；
- 普通调用、泛型下标和一元 `!` 不受影响。

示例：

```text
info!("ready", {"service": "api"});
capture! { return int("42"); }
```

## 3. 第一批内置宏

### 3.1 断言

```text
std.assert.that!(condition[, message])
std.assert.schema!(value, spec)
```

展开为普通运行时调用：

```text
std.assert.that(condition[, message])
std.assert.schema(value, spec)
```

断言失败产生 `kind = "assert"` 的可捕获错误。断言不受 release 构建或
`SKEIN_TYPECHECK` 模式影响。

schema 是纯数据描述，不是类型标注，也不是 `shape` 类型。

### 3.2 默认 logger

```text
trace!("message"[, fields])
debug!("message"[, fields])
info!("message"[, fields])
warn!("message"[, fields])
error!("message"[, fields])
```

`info!("done", {"ms": elapsed()})` 展开为：

```text
if (std.log.enabled("info")) {
  std.log.info("done", {"ms": elapsed()});
}
```

级别关闭时，`message` 和 `fields` 表达式完全不求值，宏表达式的结果是 `null`。
`error!` 是保留字特例，裸写 `error!` 与 `std.log.error!` 都可用。

### 3.3 指定 logger

```text
log_to!(logger, level, message[, fields])
```

示例：

```text
log_to!(lg, "debug", "cache miss", {"key": key});
```

展开为：

```text
if (lg.enabled("debug")) {
  lg.log("debug", "cache miss", {"key": key});
}
```

### 3.4 捕获为 result

```text
capture! { statements }
```

展开为：

```text
std.result.capture(func() { statements })
```

语义：

- 捕获 block 中的同步 `fail` 和运行时错误；
- 成功得到 `result.ok`，失败得到 `result.err`；
- 不打印错误；
- 不自动展平函数返回的 `result.err`；
- 不捕获 task 错误，task 仍使用 `std.task.catch`；
- block 内的 `return` 返回生成的 thunk，不直接返回外层函数；
- 可与 `on error` 组合。

## 4. 边界

第一批不包含：

- 用户自定义 `macro` 声明；
- `list!` / `dict!` / `try!`：已决定不实现；列表和字典直接使用普通字面量，
  错误捕获使用 `std.result.capture` 或 `on error`；
- `type` / `pattern` / `module` / `operator` 片段；
- 宏递归；
- 一等宏值；
- 声明生成宏；
- 泛型声明或泛型调用生成；
- `shape` 类型、protocol 字段、鸭子类型；
- 运算符宏；
- 对 `if` / `for` / `match` / `defer` / `on error` 等核心语义的重写；
- 专用宏 opcode。

装饰器保持 `@expr` 普通高阶函数语义；本计划不改变用户写法。

## 5. 与泛型的关系

宏可以生成普通表达式和普通 block，但第一批不生成泛型语法。
宏不得创建或改变：

- 类型参数；
- 类型约束；
- protocol 匹配；
- 类型推断；
- 类型实参；
- 默认类型元数据；
- 序列化 type descriptor。

如果未来允许宏生成泛型语法，必须先让 typechecker 直接使用 AST 中的泛型
实参，而不是从原始源码文本重新解析。

## 6. 测试

第一批测试位于：

```text
tests/skein/assert_runtime.skein
tests/skein/macros_assert.skein
tests/skein/macros_capture.skein
tests/cases/macros.py
tests/cases/logging.py
tests/jit_differential.py
```
