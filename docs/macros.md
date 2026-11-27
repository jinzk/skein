# Skein 宏（第一批：内置语法宏）

- 状态：第一批内置宏已完成；用户自定义宏 v1、G1 AST 泛型实参、G2 受限
  `type` 片段、G3 边界强化、G4 工具链与双后端同步已落地（见
  [`docs/macros_user_defined.md`](macros_user_defined.md)）
- 相关模块：`std.assert`、`std.log`、`std.result`
- 实现位置：`src/interpreter/macros.cpp`、`src/interpreter/user_macros.cpp`、
  `src/interpreter/parser.y`

## 1. 目标

Skein 的第一批宏只做**受限 AST 级语法脱糖**。宏在语法分析阶段展开，展开结果
随后进入普通的 name resolution、typecheck、bytecode/JIT 流程。

宏不是文本替换，不是编译期任意执行，宏名也不是一等值。宏调用展开后是普通
表达式。用户宏可以接收调用点已有的类型名作为受限 `type` 片段，但不能做类型
计算或类型内省，不能创建新的类型约束、proto 能力、specialization 或转换规则。

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

同时支持两种简写：

```text
name!       // 零参数，等价于 name!()
name! x     // 单个 NAME 参数
```

所有其他参数仍必须写在括号内：`name!(x + y)`、`name!([1, 2])`、
`name!({"a": 1})`、`name!(recover!())`。`name! [1, 2]` 会解析为
`(name!())[1, 2]`，`name! { ... }` 始终是 block。详见
[`docs/macros_user_defined.md`](macros_user_defined.md)。

## 3. 第一批内置宏

下面的裸名都是默认 import 的解析期别名；右侧规范路径始终可以显式调用。用户
自定义宏可以遮蔽裸名，规范路径不受影响，见
[`docs/macros_user_defined.md`](macros_user_defined.md)。

| 裸名 | 规范路径 |
| --- | --- |
| `trace!` | `std.log.trace!` |
| `debug!` | `std.log.debug!` |
| `info!` | `std.log.info!` |
| `warn!` | `std.log.warn!` |
| `error!` | `std.log.error!` |
| `log_to!` | `std.log.log_to!` |
| `capture!` | `std.result.capture!` |
| `fail!` | `std.error.fail!` |
| `recover!` | `std.error.recover!` |
| `pass!` | `std.error.pass!` |
| `wait!` | `std.task.wait!` |
| `iter!` | `std.utils.iter!` |

`std.assert.that!` / `std.assert.schema!` 本身就是规范路径，没有裸名别名。

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
`error!` 现在与普通 `NAME MACRO_BANG` 走同一条解析路径：裸名是默认宏绑定，
规范路径 `std.log.error!` 始终可用；`error` 本身已不是保留字，`: error` 走类型
命名空间，`.error` 是普通字段。

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

### 3.5 控制流与任务宏

```text
fail!([expr])
recover!([expr])
pass!([expr])
wait!(task)
```

`fail!` / `recover!` / `pass!` 展开为对应的控制流 AST 节点，语义与旧关键字形式
`fail expr` / `recover expr` / `pass expr` 一致；`wait!(t)` 展开为 `t.wait()`。

```text
let r = std.result.capture! { std.error.fail!("boom"); };
let v = wait launch work();
let w = wait!(t);
```

零参数写法 `fail!` / `recover!` / `pass!` 与单 NAME 写法 `recover! value`
均可用；其他表达式必须放在括号内。旧关键字形式暂时保留作为兼容别名。

### 3.6 迭代器宏

```text
iter!(xs)
```

展开为：

```text
xs.iter()
```

要求：

- 恰好一个表达式参数；
- 不接受 block；
- 不接受 spread；
- 等价于对接收者调用 `iter()`；接收者可以是具体内置容器类型、`std.traits.Iterable` 类型的值，或声明了 `iter()` 方法的用户类型。

支持的内置可迭代类型：

```text
list、dict、set、deque、heap
string、bytes
generator
pair、tuple
```

dict 的 `iter()` 产出 entry pair；string / bytes 分别产出 char / byte；
generator 产出自身元素；pair / tuple 产出成员。

```text
for x in iter!([1, 2, 3]) { ... }
for (k, v) in iter!({"a": 1}) { ... }
```

`for x in iter!(xs)` 与 `for (let x in iter!(xs))` 语义等价：两者都使用一个新的
块级循环变量，外层同名变量不受影响。

## 4. 边界

第一批不包含：

- 用户自定义 `macro` 声明：不在第一批，设计提案见
  [`docs/macros_user_defined.md`](macros_user_defined.md)；
- `list!` / `dict!` / `try!`：已决定不实现；列表和字典直接使用普通字面量，
  错误捕获使用 `std.result.capture` 或 `on error`；
- `pattern` / `module` / `operator` 片段；第一类 `type` 片段见用户宏文档；
- 宏递归；
- 一等宏值；
- 声明生成宏；
- 泛型声明或泛型调用生成；
- `shape` 类型、proto 字段、鸭子类型；
- 运算符宏；
- 对 `if` / `for` / `match` / `defer` / `on error` 等核心语义的重写；
- `launch` / `await`：走表达式起始 contextual，不按宏处理；
- 专用宏 opcode。

装饰器保持 `@expr` 普通高阶函数语义；本计划不改变用户写法。

## 5. 与泛型的关系

宏可以生成普通表达式和普通 block，但第一批不生成泛型语法。
宏不得创建或改变：

- 类型参数；
- 类型约束；
- proto 匹配；
- 类型推断；
- 类型实参；
- 默认类型元数据；
- 序列化 type descriptor。

typechecker 已直接使用 AST 中的泛型实参（roadmap G1），不再从原始源码文本
重新解析。G2 的受限 `type` 片段只把调用点已有名字放进类型位置。G3 在定义期
拒绝模板生成声明（`struct` / `enum` / `proto` / `impl` / 具名 `func`）和显式泛型
应用（含字面 `f[int](...)` / `Box[int] { ... }` 与 `f[#T](...)`）；类型片段不能
进入反射 builtin。调用点已有的泛型值仍可作为 `expr` 片段传入。

G4 只补齐工具链与双后端覆盖，不改变语义：formatter 无损往返类型片段及其
nullable / `ref` / `weak` / `as` / `is` / 函数签名形态；lint 报告未使用片段与
非法片段位置，宏展开错误同时显示调用点和模板定义点；LSP 提供宏补全、签名
hover、跳转与 `relatedInformation`；新增 VM/JIT differential 覆盖。

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
