# 用户自定义宏设计（设计方案）

- 状态：P1 / P2 / P3.1 / P4 用户宏 v1（同文件 + 跨包 selective import）、G1 AST 泛型实参、G2 受限
  `type` 片段、G3 边界强化、G4 工具链与双后端同步、G5 核心约束验收已完成
- 关联：[`docs/macros.md`](macros.md)、[`docs/roadmap.md`](roadmap.md)
- 建议里程碑：M11，不回写已完成的 M10
- 拟改动：`src/interpreter/tokens.l`、`src/interpreter/parser.y`、新增
  `src/interpreter/user_macros.{h,cpp}`，以及 formatter / lint / LSP 配套
- 跨包宏与值/类型共用 selective import `import {name} from pkg;`，宏名加 `!`

## 1. 结论摘要

用户自定义宏应继续沿第一批内置宏的路线：**AST 级、受限、hygienic、在
typecheck 前展开**。它不应变成编译期任意执行，也不应触碰类型系统。

本提案建议的最小版本是：

- 宏声明是静态 AST 模板，宏体用 `#参数名` 标记占位符；
- 支持 `expr`、`ident`、一个尾部 `expr...`、一个尾部 `block`，以及只用于
  类型位置的 `type` 片段（G2）；
- 宏调用保持现有 `name!(...)` / `name! { ... }` / `name!(...) { ... }` 形式；
  后续阶段只追加两种简写：`name!`（零参数）与 `name! x`（单个 `NAME`
  参数），所有其他参数仍必须写在括号内；
- 内置宏是 parser 侧默认绑定：裸名是解析期的隐式别名，每个宏都有规范路径，
  用户宏可以遮蔽裸名，但规范路径仍可显式调用（第 3.5 节）；
- 同文件宏遵循声明先于使用；跨包宏通过 `export macro` 与
  `import {retry!} from util;` 显式导入，`from` 是 contextual keyword；
- 禁止递归、隐式跨模块宏、泛型语法生成、类型内省、一等宏值和核心控制流重写。

> 泛型负责类型抽象；宏负责语法抽象。这条边界在用户自定义宏中继续成立。

## 2. 目标

1. 让用户能用受限语法模板消除重复写法，而不是引入第二套类型语言。
2. 复用第一批宏已经验证过的调用语法和 AST 展开路径。
3. 展开结果必须是普通 AST，随后进入 name resolution、typecheck、bytecode/JIT、
   与普通代码完全相同的流程。
4. 诊断同时指向调用点和定义点，且不能出现占位符或内部隐藏名字泄漏。
5. formatter / lint / LSP 能无损处理声明和调用。

## 3. 语法提案

### 3.1 声明

```text
macro name(param: expr, label: ident, body: block) {
  let #label = #param;
  #body
}
```

`macro` 建议作为新的保留声明关键字。宏名使用普通标识符。保留的便利内置宏
名不是全局保留字，而是默认宏绑定（第 3.5 节）：规范路径包括
`std.result.capture!`、`std.error.fail!` / `std.error.recover!` /
`std.error.pass!` / `std.task.wait!`。`iter!` 已移除，迭代使用 `x.iter()`。
日志与断言宏
没有裸名别名，使用 `std.log.*!` / `std.assert.*!` 规范路径。

### 3.2 参数片段

| 片段 | 声明写法 | 允许出现的位置 | 语义 |
| --- | --- | --- | --- |
| 表达式 | `x: expr` | 表达式位置 | 克隆调用点表达式，必要时加括号保持优先级 |
| 标识符 | `n: ident` | 名字位置，如 `let #n = ...` | 保留调用点拼写，故意不参与 hygiene 重命名 |
| 表达式序列 | `xs: expr...` | 最后一个表达式参数，可紧接尾部 `block` | 展开为逗号分隔的表达式列表 |
| 块 | `b: block` | 语句列表位置，写成 `#b` 且不带分号 | 必须是最后一个参数；克隆调用点语句列表 |
| 类型 | `T: type` | 类型位置，如 `let x: #T`、`#v is #T` | 传入类型名；复合类型用 `type(...)` 包装；调用点拼写与 import 别名保留 |

约束：

- `expr...` 只能有一个，且必须是最后一个表达式参数；若宏带尾部 `block`，
  则 `expr...` 必须紧接在该 `block` 之前；
- `block` 只能有一个，且必须是整个参数列表的最后一个；
- `block` 不能放进括号内；
- `type` 片段不支持 `...`，不能作为值或名字使用；
- 不支持 `pattern`、`module`、`operator` 片段；
- `#名字` 只用于宏定义模板体，只能引用当前宏的参数，未声明的占位符是定义期语法错误；
- 调用点的尾部 block 使用 `#N` 按位置引用实参，不能写 `#名字`；两者不混用。

示例：

```text
macro with_value(name: ident, value: expr, body: block) {
  let #name = #value;
  #body
}

macro call_all(f: expr, args: expr...) {
  #f(#args...);
}
```

`type` 片段只接受调用点的普通类型名、限定名（`pkg.Type` / `pkg.Proto`）和内建
类型名；`str` 与 `string` 等价，均按规范类型名 `string` 处理。表达式、旧的小写
约束标记 `hashable` / `iterable`、内部占位符拼写都会被拒绝；当前约束写作
`Hashable` / `Iterable` proto，它们作为类型名可以传入，但 `type` 片段不提供
专用约束语义。
复合类型不能只靠普通括号表达；`T: type` 实参使用显式的 `type(...)` 包装：

```text
make!(type(list[int]), xs)
make!(type((int) -> int), f)
```

`type(...)` 内只允许类型语法，可以写泛型类型、函数类型、`A | B`（`union[A, B]` 的
语法糖）、`(A | B)?`、`T?`、`ref T` 等；
不能写 `1 + 2` 或函数调用。它只告诉 parser 按类型片段解析，不引入编译期执行、
类型计算或一等类型值。简单类型名仍可裸写，`type(int)` 与 `int` 等价。表达式片段
保持普通表达式语法和括号优先级，不提供 `expr(...)` 包装。把 `type(...)` 传给非
`type` 参数会报 `type argument requires a type fragment`，内置宏也不接受类型实参。

传入的名字保留调用点拼写和 import 别名，因此 `is #T` 能识别调用点可见的 proto；
模板内部写出的字面类型名仍按定义点解析。类型片段可出现在变量/函数参数类型、
`as` 与 `is` 的类型位置，以及 `#T?` / `ref #T` / `weak #T` 前缀或后缀包装中；
G6 起也可进入显式泛型实参（`Box[#T]` / `f[#T](...)` / `Box[list[#T]]`）。它不能
用于生成泛型声明，`[V proto Drawable]` 这类约束仍由普通源码书写。

G3 边界在定义期执行：模板体不得生成 `struct` / `enum` / `proto` / `impl` 或具名
`func` 声明（`macro template cannot generate declarations`）。模板里的显式泛型应用
允许使用，例如 `f[int](...)`、`let g = f[int]`、`Box[int] { ... }`、`f[#T](...)`、
`Box[#T] { ... }` 和 `Option[#T].Some`；类型实参由 parser 在宏定义点捕获进 AST，
展开时递归替换占位符并复制，不回读源码文本。非 `type` 片段进入泛型实参仍报
`macro fragment cannot be used here`。匿名函数表达式 `func(x) {...}` 是普通表达式，
仍然允许。这些限制只作用于模板体：调用点传入的表达式片段可以是 `identity[int]`
这类已绑定泛型值，宏会原样克隆。类型片段不能进入反射 builtin，`std.type.kind_of(#T)` /
`std.type.name(#T)` 等报 `type fragment cannot be used as a value or name`。宏生成的显式
泛型应用仍走普通 typechecker 约束检查，类型实参不满足声明的约束时报错；宏不创建或
改变约束。

### 3.3 调用

调用形式与第一批内置宏完全一致：

```text
import std;
with_value!(x, 1 + 2) {
  std.io.println(str(x));
}

call_all!(add, 1, 2, 3);
```

宏调用的尾部 block 可以用 `#N` 按位置引用本次调用的实参：

```text
macro with_value(value: expr, body: block) {
  #body
}

let it = "outer";
with_value!(42) {
  println(str(it));   // outer
  println(str(#1));   // 42
}
```

规则：

- `#N` 从 `1` 开始，只计括号内实参，尾部 block 自身不占编号；
- `expr...` 展开后的每个表达式各占一个连续位置；
- 没有 `1..9` 上限，越界、 `#0` 和非数字形式都是错误；
- `#N` 只能出现在用户宏调用的尾部 block 中；模板体、普通代码和 block 外都报错；
- block 内只能写 `#N`，写 `#名字` 报错；
- `expr` 实参只能进入表达式位置，`ident` 只能进入名字位置，`type` 只能进入类型位置；
- 嵌套宏的 block 中，`#N` 绑定最近的宏调用；内层调用实参中的 `#N` 仍属于外层 block；
- `#N` 不是普通名字绑定，不会遮蔽 block 内外的同名变量；需要调用点自定义名字时仍用
  `ident` 片段。

`!` 仍然必须紧贴宏名；普通调用、泛型下标、一元 `!` 和 `!(expr)` 不受影响。

已支持的简写追加两种形式：

| 写法 | 含义 |
| --- | --- |
| `name!` | 零参数，等价于 `name!()` |
| `name! x` | 单个 `NAME` 参数 |

其余参数必须写括号：

```text
name!(x + y)
name!([1, 2])
name!({"a": 1})
name!(recover!())
```

限制：`foo! x + y` 解析为 `(foo!(x)) + y`；`foo! x.y` 解析为
`(foo!(x)).y`；`foo! [1, 2]` 解析为 `(foo!())[1, 2]`（裸列表参数不支持）；
`foo! { ... }` 始终是 block，不能当 dict。

### 3.4 跨包导入（值/类型与宏均已实现）

值/类型与宏的 selective import 都已落地。跨包阶段不引入独立的 `use` 关键字，
而是复用同一形式：

```text
import {retry} from util;
import {retry as r} from util;
import {Point, parse as parse_point} from util;

import {parse} from .jsonutil;
import {parse} from ..common;
import {parse} from @top.jsonutil;
import {parse} from "github.com/acme/util/json";
```

宏导入把 `!` 放在被导入的宏名上；alias 继承宏属性，alias 本身仍是普通标识符：

```text
import {retry!} from util;
import {retry! as r} from util;   // 调用时写 r!(...)
```

导出方使用 `export macro` 发布到包的宏导出表；该表与值/类型命名空间分离，
因此 `func retry` 与 `macro retry` 可以共存。`import {retry} from util;` 只导入
值命名空间中的 `retry`，`import {retry!} from util;` 只导入宏命名空间中的
`retry`，互不影响。selective import 不会绑定 `util` 包本身；如需写
`util.retry(...)`，仍要另外写 `import util;`。

实现上，宏 import 在语法分析阶段绑定：解析到 `import {name!} from pkg;` 时会以
**parse-only** 方式读取被导入包的源码，只收集 `export macro` 模板，不执行任何
包代码、不构建值命名空间。导出模板会携带定义点可见的宏表快照，因此模板内部
调用的辅助宏（包括该包自己导入的宏）会随之迁移；宏依赖图必须无环，跨包循环
导入会被拒绝。宏选择项不会进入值/类型绑定或 bytecode。syntax-only 工具
（`skein fmt` 的语法校验）不做包解析，因此不会因找不到包而误报。
当前支持普通包、相对包、项目根包与内嵌标准库包；引号形式的外部 module 宏导入
明确报错，后续再与 manifest 一起扩展。

`from` 必须是 **contextual keyword**，不是全局保留字。当前 `from` 已被普通
标识符大量使用，例如 `std.path.relative(from, to)` 和 `std.graph` 的 `from`
字段。parser 只在 `import { ... }` 之后、`importref` 之前把拼写为 `from` 的
`NAME` token 当作分隔符；其他位置保持普通标识符。

### 3.5 保留的默认宏绑定

只有下列便利宏保留 parser 侧的默认绑定；裸名是解析期别名，每个宏都有规范
路径。日志宏不在这个白名单里；`iter!` 已移除，迭代使用 `x.iter()`。当前映射为：

| 裸名 | 规范路径 |
| --- | --- |
| `capture!` | `std.result.capture!` |
| `fail!` | `std.error.fail!` |
| `recover!` | `std.error.recover!` |
| `pass!` | `std.error.pass!` |
| `wait!` | `std.task.wait!` |

`std.assert.that!` / `std.assert.schema!` / `std.log.*!` 本身就是规范路径，
没有裸名别名。

宏解析发生在语法分析阶段，顺序为：

```text
同文件用户宏 > 显式 import 的宏 > 保留的默认宏绑定 > 未知宏诊断
```

规范路径始终可以显式写，不受裸名遮蔽影响。例如同文件用户定义了同名 `fail`
宏后，`fail!` 指向用户宏，`std.error.fail!` 仍指向内置宏。`error!` 也走普通
`NAME MACRO_BANG` 路径，但不再默认指向日志宏；日志请写 `std.log.error!`。
`error` 本身可以是变量、字段或类型名。

## 4. 展开模型

### 4.1 解析期宏表

Parser 维护一个 `MacroTable`，键为宏名，值为已验证的 `MacroTemplate`。

处理顺序：

1. 解析到 `macro` 声明时，把宏体解析成内部模板 AST，校验片段位置和占位符；
2. 校验通过后注册到 `MacroTable`；该声明本身不产生运行时 AST；
3. 解析到 `name!(...)` 时，先查 `MacroTable`，再按保留的默认绑定解析内置
   宏；显式 import 的宏在跨包阶段位于两者之间；
4. 找到用户宏后，在 grammar action 中实例化模板；
5. 实例化后的普通 AST 才进入逐 `top_statement` 的 typecheck。

这不是整文件可变重写 pass：展开仍发生在语法分析阶段，保持第一批宏的实现
边界。为了让自递归在定义期就被拒绝，当前宏在模板校验完成前不注册。

### 4.2 可见性

同文件 v1 与跨包阶段都遵守声明先于使用：

- 宏声明之后的代码可以调用；声明之前的代码不能调用；
- 跨包宏通过 `export macro` / `import {name!} from pkg;` 按第 3.4 节显式导入；
- 同文件用户宏与显式导入宏可以遮蔽内置宏裸名（以第 3.5 节为准）；规范路径仍
  解析到内置宏，不受遮蔽影响；本地绑定优先于显式导入；
- 同文件重复定义同一宏名仍报错；同一选择列表里同名的 `foo` 与 `foo!` 属于
  不同命名空间，不冲突。

跨包宏依赖图必须无环，且只读取导出宏的源码模板，不执行包代码；check-only
工具同样只在语法层校验宏调用，不执行被导入的包。

### 4.3 模板 AST

`MacroTemplate` 只在 parser / expander 内部存在，typechecker、VM 和 JIT 永不
看到它。模板节点包含：

- 普通 AST 节点；
- 占位符节点：`expr`、`ident`、`block`、`expr...`；
- 定义点源码位置，用于诊断。

展开时对模板做深拷贝并把占位符替换为调用点 AST 的深拷贝。块片段复用
`capture!` 已经验证过的函数上下文处理（`start_function()` /
`finish_function()` / include-depth），保证 `return`、`yield` 和 `do` 语义正确。

## 5. Hygiene

每次展开分配一个唯一 expansion id。

1. 模板体引入的局部变量、标签和其他绑定名，统一改写为内部名字：
   `#m<id>_<original>`。该前缀不是合法用户标识符，避免碰撞。
2. `expr` 和 `block` 片段从调用点克隆，保留其原始名字和源码位置。
3. `ident` 片段保留调用点拼写，用于 `let #name = ...` 这类有意引入绑定的场景。
4. 模板体是封闭的：不能引用定义处局部变量；需要外部值必须通过参数传入。
5. 调用点 block 的 `#N` 展开为对应实参 AST，不引入绑定，因此不会遮蔽同名变量。
6. 展开完成后不得留下占位符节点或 `#m...` 内部名字出现在面向用户的诊断中。

例：

```text
import std;
macro wrap(value: expr, body: block) {
  let tmp = #value;
  #body
  return tmp;
}

let tmp = 1;
wrap!(2) {
  std.io.println(str(tmp));   // 指向调用点的 tmp，而不是宏内部的 tmp
}
```

宏内部的 `tmp` 会被改成 `#m17_tmp`；block 中的 `tmp` 仍是调用点的 `tmp`。

## 6. 与非目标边界

明确不做：

- 用户宏值、宏作为一等值传递或赋值；
- 编译期任意执行、编译期 I/O、反射或类型内省；
- `pattern` / `module` / `operator` 片段，以及类型计算、类型内省和专用约束片段；
- 宏递归、互递归或展开次数参数；
- 生成泛型声明；模板里的字面/占位符泛型应用允许，但不创建类型参数或约束；
- 声明生成宏：`struct` / `enum` / `proto` / `impl` 和具名 `func` 都在定义期拒绝；
- 对 `if` / `for` / `match` / `defer` / `on error` 等核心语义的重写；用户宏不得
  重写错误控制流，这三者由内置宏 `std.error.fail!` / `std.error.recover!` /
  `std.error.pass!` 提供；
- `shape` 类型、proto 字段或鸭子类型；
- 运算符宏；
- 专用宏 opcode；
- 隐式跨模块宏可见性；跨包只按第 3.4 节的 selective import 显式导入；
- 把 `from` 设为全局保留字；它只作为 selective import 的 contextual keyword；
- 改变装饰器语义：装饰器继续是 `@expr` 普通高阶函数。

显式泛型实参已由 parser 一次解析并保存在 AST 节点上（roadmap G1），typechecker
不再回读原始源码文本。用户宏片段支持 `expr` / `ident` / `expr...` / `block` 与
受限 `type`（G2）。G3 把这些边界固化为定义期诊断：模板不得生成声明，字面显式
泛型应用（`f[int](...)` / `Box[int] { ... }`）可以克隆定义点捕获的实参；G6 允许
`type` 片段进入这些实参并递归替换嵌套类型图。类型片段不能被当作值传给反射
builtin，调用点已有的泛型值仍可通过 `expr` 片段传入。

## 7. 诊断

诊断必须包含调用点位置；涉及模板格式或占位符时还要包含定义点位置。

至少覆盖：

- `is not a known built-in or user-defined macro`；
- 未知占位符；
- 片段类型不匹配，例如把 `expr` 放进 `ident` 位置，或把 `expr` / `ident` /
  `block` 放进类型位置；
- `type` 片段收到表达式或约束标记；非 `type` 片段进入类型或泛型实参位置；
- `expr...` 不是最后一个表达式参数，或作为普通值使用
  （`expr... fragment must be used in an argument list`）；
- `block` 不是最后一个参数、出现在括号内，或在表达式位置使用
  （`unexpected macro block fragment`）；
- 同文件宏重名；遮蔽内置裸名不是错误，但需说明实际命中的来源；
- 自递归 / 未注册宏调用；
- 模板中出现声明定义：`macro template cannot generate declarations`；
- 模板中把非 `type` 片段放进泛型实参，如 `Box[#T]` 且 `T: expr`：
  `macro fragment cannot be used here`；
- 无参数宏收到表达式参数，或需要参数的宏缺少参数；
- 内置宏裸名与规范路径解析到不同定义时，诊断说明实际命中的来源；
- 块片段中的控制流位置错误；
- 调用点 block 中的 `#0`、越界 `#N`、`#名字`，或 block 外 / 模板体中的 `#N`
  （`invalid macro argument placeholder #0`、
  `macro argument placeholder #N is out of range`、
  `named macro placeholder #name is not valid in a macro block; use #N`、
  `macro argument placeholder #N is only valid in a macro block`）；
- selective import 缺少 `from` 或路径非法；
- selective import 的名字未导出、重复或 alias 冲突；
- `import {name!} from pkg;` 找不到对应宏导出。

## 8. 工具链

- **formatter**：无损往返 `macro` 声明、`#param` 占位符、调用点 block 的 `#N`、
  `expr...`、`T: type`、类型位置的 `#T`、宏调用和 selective import；CLI 与 LSP 的
  格式化校验都只做语法检查，不解析宏调用；
- **lint**：报告未使用片段、未知宏、模板中的非法占位符与非法片段位置；宏展开
  错误同时给出调用点（主位置）与模板定义点（related location）；
- **LSP**：宏名与 selective import 宏别名补全、签名 hover、调用点/定义点跳转，
  并在 `relatedInformation` 中显示模板定义点；document/workspace symbol 能识别
  `macro` 声明；v1 不做跨文件 rename；
- **snippets**：可选添加 `macro` 声明模板；
- **docs sync**：`README.md`、`SYNTAX.md`、`docs/macros.md` 与本文件同步。

## 9. 测试计划

### 9.1 parser / 展开

- 五种片段（`expr` / `ident` / `expr...` / `block` / `type`）单独与组合使用；
- `expr...` 后接尾部 `block`；
- 内置宏与用户宏同名冲突；
- 声明前调用、未知宏、递归宏；
- 模板中的非法占位符和非法片段位置；
- 表达式上下文中的展开结果；
- 块片段中的 `return` / `yield` / `do` 语义；
- selective import 的值/类型/宏形式、alias、`from` contextual keyword；
- `import {retry} from util;` 与 `import {retry!} from util;` 命名空间隔离；
- `type` 片段的标注、`as` / `is`、`#T?` / `ref #T` 包装，以及调用点限定名和
  import alias；模板内字面类型名按定义点解析；
- `type` 片段拒绝表达式、约束标记及非类型位置；非 `type` 片段拒绝进入泛型实参；
- 模板拒绝声明生成，字面与 `type` 片段泛型实参均可用；调用点 `expr` 片段中的
  泛型值保持可用；
- 类型片段进入反射 builtin 与匿名函数表达式的边界。

### 9.2 hygiene / 运行时

- 宏内部局部名与调用点局部名冲突；
- `ident` 片段引入调用点绑定；
- `expr` 优先级保持，例如 `#value + 1` 不会改变结合性；
- 展开结果与手写代码行为一致；
- G5 核心约束负例：字符串/注释中的宏调用文本不展开、未调用模板既不执行也不做
  类型检查、宏名不能作为一等值使用；
- 第一批内置宏继续通过原有测试，不被用户宏路径影响。

### 9.3 工具链

- `skein fmt` 幂等；
- lint 诊断位置；
- LSP completion / hover / definition，包含 selective import；
- 文档同步与 boundary 检查。

## 10. 分阶段落地

实施顺序按**依赖关系、风险、收益和文档一致性**重新排定：先落地已经写进文档但
代码尚未跟上的机械变更，再做内置值 / 内置宏默认化，然后做任务语法，最后做用户
宏与高风险 contextual。M11 不回写已完成的 M10。

优先级分组：

| 优先级 | 内容 | 理由 |
| --- | --- | --- |
| P0（已完成） | 低风险 contextual：`in` / `export` / `import` / `package` / `include`；`module` 仅 manifest | 基线不动 |
| P1（已完成） | `protocol` → `proto` 实现落地；值/类型的 selective import | 已消除文档漂移；selective import 是宏 import 的前置 |
| P2（已完成） | 普通内置函数默认 import；内置宏默认绑定与 `fail!` / `recover!` / `pass!` / `wait!`；`list` / `dict` 默认 import | 一次性移除大量保留名，并为宏表和类型构造器统一打底 |
| P3（已完成） | `launch` / `await` 表达式起始 contextual | 依赖 `wait!`；保住“启动并立即等待”的写法，退出 `wait` 保留字 |
| P4（已完成） | 用户宏 v1：`expr` / `ident` / 尾部 `expr...` / 尾部 `block`；跨包 `export macro` / `import {name!}` | 复用 P2 建立的宏分发路径与调用简写；宏依赖图保持无环 |
| P5 | `on` / `error`、`as`、`ref` / `weak`、`is` | 高风险 contextual，冲突面大，必须各自独立 |

具体顺序：

1. **P1.1（已完成）**：`protocol` → `proto` 实现落地。lexer / parser / AST / typecheck /
   LSP / formatter / lint / 反射 kind 字符串同步改为 `"proto"`；泛型约束列表里的
   `proto` / `type` 仍是 contextual 标记；不保留 `"protocol"` 别名。
2. **P1.2（已完成）**：值/类型的 selective import `import {name} from pkg;`。
   `from` 只在 `import { ... }` 后 contextual；formatter / lint / LSP、文档与
   测试已同步，check-only 工具不执行被导入的包。
3. **P2.1（已完成，后续清理）**：普通内置函数曾改为默认 prelude 值；最终清理后
   `std` 成为语言级默认根绑定，默认裸值只保留 `list` / `dict` / `input` /
   `print` / `println`。`len` / `iter!` 已完全移除，`range` → `std.utils.range`；
   `kind_of` / `type_*` → `std.type.*`。lexer 不再发 `SKEIN_TOK_BUILTIN`；默认
   绑定安装在根作用域，VM / JIT 的 builtin id 不变。解析优先级为局部绑定 >
   显式 import > 默认根绑定。
4. **P2.2（已完成）**：内置宏改为 parser 侧默认宏绑定（第 3.5 节）。裸名只是别名，规范
   路径仍可显式调用；加入 `fail!` / `recover!` / `pass!`（`std.error.*!`）和
   `wait!`（`std.task.wait!`，展开为 `#expr.wait()`）。落地第 3.3 节的
   `name!` / `name! x` 两种简写；旧 bare 关键字形式已移除，统一使用规范宏路径。
5. **P2.3（已完成，后续恢复）**：`list` / `dict` 是默认根绑定，规范路径为
   `std.list.new` / `std.dict.new`；`list(x)` / `dict(x)` 解析为对应构造器，值作用域可遮蔽，
   类型命名空间保持独立。不做 `list!` / `dict!` / `try!`。
6. **P3.1（已完成）**：`wait` 退出保留字；`launch` / `wait` / `await` 改为表达式起始 contextual
   keyword。`launch f()` 启动 task，`await f()` 等价 `std.task.wait!(launch f())` 并要求
   后面是调用表达式；已有 task 写 `std.task.wait!(t)`。contextual 槽位覆盖语句开头、
   `=` / `return` / `yield` / `(` / `[` / `{` / `,` / `:` 和二元运算符之后；`.`
   之后仍是普通名字；`await(f())` 是前缀形式，同名变量写 `(await)(f())`；
   postfix 优先级高于前缀。`await f() on error(...)` 沿用当前 handler 语义。
7. **P4（已完成）**：`macro` 声明语法、签名校验、同文件宏表、未知宏诊断，以及
   `expr` / `ident` / 尾部 `expr...` / 尾部 `block` 四种片段的完整 v1 展开；
   hygiene、块函数上下文、嵌套调用与递归禁止均已落地。
8. **P4.3（已完成）**：`export macro` 与 `import {name!} from pkg;`（含 alias）、
   宏与值/类型命名空间的隔离、定义点宏表快照、跨包 hygiene、循环检测、
   parse-only 包读取和完整测试矩阵；引号形式的外部 module 宏导入暂不支持。
9. **P5.1（已完成）**：`on` / `error` 评估 handler-scoped contextual；`std.log.error!` /
    用户宏走普通 `NAME MACRO_BANG` 路径，`type error` 走类型命名空间，`.error` 仍是普通字段；
   保留 `%prec ON`，补齐 handler body / defer / 默认参数诊断。
10. **P5.2（已完成）**：`as` 改为 contextual；cast、import 别名与模式绑定仍是
    分隔符，其他位置是普通标识符。
11. **P5.3（已完成）**：`ref` contextual；`ref(x)`、`ref Point { ... }` 与
     `ref T` 前缀仍是关键字位置，其他位置是普通标识符。
12. **P5.4（已完成）**：`weak` contextual；只在 `weak T` 类型前缀位置作为关键字，
    其他位置是普通标识符。
13. **P5.5（已完成）**：`is` contextual；只在 `expr is T` / `case is T` 类型测试
    位置作为运算符，其他位置是普通标识符。

宏与泛型结合阶段 G1–G6（已完成）紧随 P5.5：parser 先把显式泛型实参存入 AST，
用户宏随后获得只用于类型位置的受限 `type` 片段，定义期拒绝生成声明；字面显式
泛型应用可在模板中使用，G6 起 `type` 片段也能进入泛型实参并递归替换。G4 补齐
formatter / lint / LSP 的片段覆盖、双位置诊断与 VM/JIT differential；G5 用负例
验收非文本展开、无编译期执行/类型检查、非一等宏值等核心边界，语义边界保持不变。

每个阶段都要保持第一批宏与全量测试全绿；用户宏不得改变第一批宏的行为；每阶段
运行 `tools/check_docs_sync.py` 与 `git diff --check`。

## 11. 已确认的默认决策

以下决策已确认，作为实现约束：

1. 使用 `#参数名` 作为模板占位符标记；
2. `macro` 作为新保留关键字；
3. 同文件宏声明先于使用；跨包宏通过 `export macro` / `import {name!} from pkg;` 显式导入；
4. 禁止递归和隐式跨模块宏；
5. 内置宏是 parser 侧默认绑定；同文件用户宏可遮蔽裸名，规范路径仍可
   显式调用（已确认，以第 3.5 节为准）；
6. 用户宏作为 M11，不回写已完成的 M10；
7. 跨包 selective import 采用 `import {name} from pkg;`，宏写作
   `import {name!} from pkg;`；
8. `from` 只作为 selective import 的 contextual keyword，不设为全局保留字；
9. 先做低风险 contextual keyword / selective import，再实现宏；`as`、`ref` /
   `weak`、`is` 等中风险关键字在宏实现之后分独立步骤处理（P5.2–P5.5 已完成）；`wait` 走内置宏，
   `launch` / `await` 走表达式起始 contextual；
10. `in`、`export`、`package`、`import`、`include` 同属低风险 contextual
   keyword，在 Phase 1 处理；`module` 仅用于 manifest，不占 `.skein` 保留字；
11. `std` 是语言级默认根绑定，显式 `import std;` 幂等；默认裸值只有
    `list` / `dict` / `input` / `print` / `println`；`len` / `iter!` 已移除，
   `range` 只放在 `std.utils` 模块；解析优先级为局部绑定 > 显式 import >
   默认根绑定；
12. `list` / `dict` 是默认根绑定的构造器，降糖为 `std.list.new` /
   `std.dict.new`；类型命名空间独立；
13. `fail` / `recover` / `pass` 通过内置宏消除保留字，不再采用 statement-start
   或 handler-scoped contextual 方案；诊断仍由现有 handler 栈检查产生；
14. 宏简写只支持 `name!` 与 `name! x`；列表、字典、表达式、嵌套宏和成员链
   一律写括号；`foo! { ... }` 始终是 block；
15. `on` / `error` 已按 handler-scoped contextual 落地：`std.log.error!` 与用户宏走普通宏路径，类型与字段位置不受影响；
16. 保留的默认宏绑定采用第 3.5 节的表；`std.error.fail!` /
    `std.error.recover!` / `std.error.pass!` 是规范路径，裸关键字形式与 `iter!`
    已移除；
17. 协议声明关键字 `protocol` 重命名为 `proto`，作为全局保留字；泛型约束列表中
    的 `proto` / `type` 是地位相同的 contextual 标记，只在类型参数约束位置
    生效；反射 kind 字符串同步改为 `"proto"`，不保留 `"protocol"` 别名；
18. `wait` 不再是关键字；等待 task 使用内置宏 `std.task.wait!`，展开为
    `#expr.wait()`。`launch` 与新增的 `await` 是表达式起始 contextual keyword，
    不是全局保留字。
    contextual 槽位覆盖语句开头、`=` / `return` / `yield` / `(` / `[` / `{` /
    `,` / `:` 和二元运算符之后；`.` 之后仍是普通名字；
19. `launch f()` 启动 task；`await f()` 等价 `std.task.wait!(launch f())`，要求后面是调用
    表达式，已有 task 写 `std.task.wait!(t)`。`await(f())` 是前缀形式；调用同名变量写
    `(await)(f())`。postfix 优先级高于前缀：`await f().xxx` 是
    `await (f().xxx)`，先 await 再取字段写 `(await f()).xxx`。`await f() on error(...)`
    沿用当前 `std.task.wait!(launch f()) on error(...)` 的 handler 语义。
20. 用户宏调用尾部 block 用 `#N` 按实参位置读取，从 `1` 开始，没有 `1–9` 上限；
    模板体继续用 `#名字`，不引入 `expose`，`#N` 不创建普通名字绑定。
