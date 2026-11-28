# Skein 宏（第一批：内置语法宏）

- 状态：第一批内置宏已完成；用户自定义宏 v1、G1 AST 泛型实参、G2 受限
  `type` 片段、G3 边界强化、G4 工具链与双后端同步、G5 核心约束验收已落地（见
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
std.log.info!("ready", {"service": "api"});
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

只保留下列便利宏的默认解析期绑定；右侧规范路径始终可以显式调用。用户自定义
宏可以遮蔽裸名，规范路径不受影响，见
[`docs/macros_user_defined.md`](macros_user_defined.md)。日志宏没有裸名别名，
必须写 `std.log.*!`。`iter!` 已移除，迭代使用 `x.iter()`。

| 裸名 | 规范路径 |
| --- | --- |
| `capture!` | `std.result.capture!` |
| `fail!` | `std.error.fail!` |
| `recover!` | `std.error.recover!` |
| `pass!` | `std.error.pass!` |
| `wait!` | `std.task.wait!` |

`std.assert.that!` / `std.assert.schema!` 本身就是规范路径，没有裸名别名。

### 3.1 断言

```text
import std;
std.assert.that!(condition[, message])
std.assert.schema!(value, spec)
```

展开为普通运行时调用：

```text
import std;
std.assert.that(condition[, message])
std.assert.schema(value, spec)
```

断言失败产生 `kind = "assert"` 的可捕获错误。断言不受 release 构建或
`SKEIN_TYPECHECK` 模式影响。

schema 是纯数据描述，不是类型标注，也不是 `shape` 类型。

### 3.2 默认 logger

```text
std.log.trace!("message"[, fields])
std.log.debug!("message"[, fields])
std.log.info!("message"[, fields])
std.log.warn!("message"[, fields])
std.log.error!("message"[, fields])
```

`std.log.info!("done", {"ms": elapsed()})` 展开为：

```text
import std;
if (std.log.enabled("info")) {
  std.log.info("done", {"ms": elapsed()});
}
```

级别关闭时，`message` 和 `fields` 表达式完全不求值，宏表达式的结果是 `null`。
`std.log.error!` 与普通 `NAME MACRO_BANG` 走同一条解析路径；`error!` 不再默认
指向日志宏，只有用户自定义同名宏时才会解析。`error` 本身已不是保留字，
`: error` 走类型命名空间，`.error` 是普通字段。

### 3.3 指定 logger

```text
std.log.log_to!(logger, level, message[, fields])
```

示例：

```text
std.log.log_to!(lg, "debug", "cache miss", {"key": key});
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
import std;
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
`std.error.fail!(expr)` / `std.error.recover!(expr)` / `std.error.pass!(expr)` 一致；`std.task.wait!(t)` 展开为 `t.wait()`。

```text
import std;
let r = std.result.capture! { std.error.fail!("boom"); };
let v = std.task.wait!(launch work());
let w = wait!(t);
```

零参数写法 `fail!` / `recover!` / `pass!` 与单 NAME 写法 `recover! value`
均可用；其他表达式必须放在括号内。裸关键字形式 `fail` / `recover` / `pass` /
`wait` 已移除。

### 3.6 迭代器方法

`iter!` 宏已移除；统一使用内置容器、迭代器或用户类型的 `iter()` 方法：

```text
(xs).iter()
```

接收者可以是具体内置容器类型、`std.traits.Iterable` 类型的值，或声明了
`iter()` 方法的用户类型。

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
for x in ([1, 2, 3]).iter() { ... }
for (k, v) in ({"a": 1}).iter() { ... }
```

`for x in (xs).iter()` 与 `for (let x in (xs).iter())` 语义等价：两者都使用一个新的
块级循环变量，外层同名变量不受影响。

## 4. 边界

早期第一批不包含：

- 用户自定义 `macro` 声明：现已实现，模板用 `#name`，调用点尾部 block 用
  `#N` 按位置读取实参，详见 [`docs/macros_user_defined.md`](macros_user_defined.md)；
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

宏可以生成普通表达式、普通 block 和显式泛型应用；泛型实参既可以是模板里的
字面类型，也可以是调用点传入的 `type` 片段。宏仍不得创建或改变：

- 类型参数；
- 类型约束；
- proto 匹配；
- 类型推断；
- 新类型名或默认类型元数据；
- 序列化 type descriptor。

typechecker 已直接使用 AST 中的泛型实参（roadmap G1），不再从原始源码文本
重新解析。模板里的字面 `f[int](...)` / `Box[int] { ... }` 由 parser 在定义点捕获
类型实参，展开时复制 AST。G2 的受限 `type` 片段把调用点已有名字放进类型位置；
G6 允许它填充泛型实参（`f[#T](...)` / `Box[#T] { ... }` / `Box[list[#T]]`），
展开时对捕获的类型图递归替换。G3 在定义期拒绝模板生成声明
（`struct` / `enum` / `proto` / `impl` / 具名 `func`）；类型片段不能进入反射
builtin。调用点已有的泛型值仍可作为 `expr` 片段传入。宏生成的显式泛型应用仍走
普通 typechecker 约束检查：类型实参不满足声明的约束时照常报错，宏本身不创建或改变
约束。

G4 只补齐工具链与双后端覆盖，不改变语义：formatter 无损往返类型片段及其
nullable / `ref` / `weak` / `as` / `is` / 函数签名形态；lint 报告未使用片段与
非法片段位置，宏展开错误同时显示调用点和模板定义点；LSP 提供宏补全、签名
hover、跳转与 `relatedInformation`；新增 VM/JIT differential 覆盖。

G5 只做核心约束验收，不增加语法或语义：字符串与注释里的 `name!(...)` 文本不会
被展开，未调用的模板既不执行也不触发类型检查，宏名不能作为一等值使用；另补齐
`enum` / `proto` 声明、`type_name` 反射入口与片段位置误用负例。这些用例与已有的
hygiene、声明先于使用、递归禁止、模板声明禁止和反射边界用例共同锁定“AST 级受限
模板”的边界。

## 6. 测试

第一批测试位于：

```text
tests/skein/assert_runtime.skein
tests/skein/macros_assert.skein
tests/skein/macros_capture.skein
tests/cases/macros.py
tests/cases/logging.py
tests/cases/fmt.py
tests/cases/lint.py
tests/cases/lsp.py
tests/jit_differential.py
```
