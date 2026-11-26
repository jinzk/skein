# 用户自定义宏设计（设计方案）

- 状态：P1 / P2 / P3.1 / P4 同文件 v1 已完成；跨包宏待实施
- 关联：[`docs/macros.md`](macros.md)、[`docs/roadmap.md`](roadmap.md)
- 建议里程碑：M11，不回写已完成的 M10
- 拟改动：`src/interpreter/tokens.l`、`src/interpreter/parser.y`、新增
  `src/interpreter/user_macros.{h,cpp}`，以及 formatter / lint / LSP 配套
- 跨包前提：先为值/类型增加 selective import
  `import {name} from pkg;`，宏复用同一形式并加 `!`

## 1. 结论摘要

用户自定义宏应继续沿第一批内置宏的路线：**AST 级、受限、hygienic、在
typecheck 前展开**。它不应变成编译期任意执行，也不应触碰类型系统。

本提案建议的最小版本是：

- 宏声明是静态 AST 模板，宏体用 `#参数名` 标记占位符；
- 只支持 `expr`、`ident`、一个尾部 `expr...`、一个尾部 `block`；
- 宏调用保持现有 `name!(...)` / `name! { ... }` / `name!(...) { ... }` 形式；
  后续阶段只追加两种简写：`name!`（零参数）与 `name! x`（单个 `NAME`
  参数），所有其他参数仍必须写在括号内；
- 内置宏也按默认 import 处理：裸名是解析期的隐式绑定，每个宏都有规范路径，
  用户宏可以遮蔽裸名，但规范路径仍可显式调用（第 3.5 节）；
- 只在**同一文件、声明先于使用**的范围内可见；
- 跨包阶段先用 `import {retry, Point} from util;` 给值/类型增加 selective
  import，宏写作 `import {retry!} from util;`；`from` 是 contextual keyword；
- 禁止递归、跨模块宏、泛型语法生成、类型内省、一等宏值和核心控制流重写。

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

`macro` 建议作为新的保留声明关键字。宏名使用普通标识符。内置宏名不是全局
保留字，而是默认 import 的解析期绑定（第 3.5 节）：当前规范路径包括
`std.log.trace!`、`std.log.debug!`、`std.log.info!`、`std.log.warn!`、
`std.log.error!`、`std.log.log_to!`、`std.result.capture!`、
`std.assert.that!`、`std.assert.schema!`、`std.error.fail!` /
`std.error.recover!` / `std.error.pass!` / `std.task.wait!`。

### 3.2 参数片段

| 片段 | 声明写法 | 允许出现的位置 | 语义 |
| --- | --- | --- | --- |
| 表达式 | `x: expr` | 表达式位置 | 克隆调用点表达式，必要时加括号保持优先级 |
| 标识符 | `n: ident` | 名字位置，如 `let #n = ...` | 保留调用点拼写，故意不参与 hygiene 重命名 |
| 表达式序列 | `xs: expr...` | 最后一个表达式参数 | 展开为逗号分隔的表达式列表 |
| 块 | `b: block` | 语句列表位置，写成 `#b` 且不带分号 | 必须是最后一个参数；克隆调用点语句列表 |

约束：

- `expr...` 只能有一个，且必须是最后一个表达式参数；
- `block` 只能有一个，且必须是整个参数列表的最后一个；
- `block` 不能放进括号内；
- 不支持 `type`、`pattern`、`module`、`operator` 片段；
- `#名字` 只能引用当前宏的参数，未声明的占位符是定义期语法错误。

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

### 3.3 调用

调用形式与第一批内置宏完全一致：

```text
with_value!(x, 1 + 2) {
  println(str(x));
}

call_all!(add, 1, 2, 3);
```

`!` 仍然必须紧贴宏名；普通调用、泛型下标、一元 `!` 和 `!(expr)` 不受影响。

规划中的简写只追加两种形式：

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

### 3.4 跨包导入（值/类型已实现，宏待实现）

值/类型的 selective import 已落地；跨包宏导入仍属后续阶段。跨包阶段不引入
独立的 `use` 关键字，而是复用同一形式：

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

`from` 必须是 **contextual keyword**，不是全局保留字。当前 `from` 已被普通
标识符大量使用，例如 `std.path.relative(from, to)` 和 `std.graph` 的 `from`
字段。parser 只在 `import { ... }` 之后、`importref` 之前把拼写为 `from` 的
`NAME` token 当作分隔符；其他位置保持普通标识符。

### 3.5 内置宏的默认 import

内置宏不再是硬编码的裸名，而是 parser 侧的默认宏绑定：裸名是隐式导入的别名，
每个宏都有规范路径。当前映射为：

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

`std.assert.that!` / `std.assert.schema!` 本身就是规范路径，没有裸名别名。

宏解析发生在语法分析阶段，顺序为：

```text
同文件用户宏 > 显式 import 的宏 > 默认 import 的内置宏 > 未知宏诊断
```

规范路径始终可以显式写，不受裸名遮蔽影响。例如同文件用户定义了同名 `info`
宏后，`info!` 指向用户宏，`std.log.info!` 仍指向内置宏。

## 4. 展开模型

### 4.1 解析期宏表

Parser 维护一个 `MacroTable`，键为宏名，值为已验证的 `MacroTemplate`。

处理顺序：

1. 解析到 `macro` 声明时，把宏体解析成内部模板 AST，校验片段位置和占位符；
2. 校验通过后注册到 `MacroTable`；该声明本身不产生运行时 AST；
3. 解析到 `name!(...)` 时，先查 `MacroTable`，再按默认 import 映射解析内置
   宏；显式 import 的宏在跨包阶段位于两者之间；
4. 找到用户宏后，在 grammar action 中实例化模板；
5. 实例化后的普通 AST 才进入逐 `top_statement` 的 typecheck。

这不是整文件可变重写 pass：展开仍发生在语法分析阶段，保持第一批宏的实现
边界。为了让自递归在定义期就被拒绝，当前宏在模板校验完成前不注册。

### 4.2 可见性

v1 只支持同文件、声明先于使用：

- 宏声明之后的代码可以调用；
- 声明之前的代码不能调用；
- import / export 宏、包级宏、跨模块宏都明确不支持；
- 同文件用户宏可以遮蔽内置宏裸名（以第 3.5 节为准）；规范路径仍解析到内置
  宏，不受遮蔽影响；
- 同文件重复定义同一宏名仍报错。

跨包阶段按第 3.4 节推进：值/类型的 selective import 已完成，下一步再支持
`export macro` 与 `import {name!} from pkg;`。宏依赖图必须无环，且只读取
导出宏的源码模板，不执行包代码；这部分不纳入 v1。

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
5. 展开完成后不得留下占位符节点或 `#m...` 内部名字出现在面向用户的诊断中。

例：

```text
macro wrap(value: expr, body: block) {
  let tmp = #value;
  #body
  return tmp;
}

let tmp = 1;
wrap!(2) {
  println(str(tmp));   // 指向调用点的 tmp，而不是宏内部的 tmp
}
```

宏内部的 `tmp` 会被改成 `#m17_tmp`；block 中的 `tmp` 仍是调用点的 `tmp`。

## 6. 与非目标边界

明确不做：

- 用户宏值、宏作为一等值传递或赋值；
- 编译期任意执行、编译期 I/O、反射或类型内省；
- `type` / `pattern` / `module` / `operator` 片段；
- 宏递归、互递归或展开次数参数；
- 生成泛型声明或泛型调用，如 `f[int](...)` / `Box[int]{}`；
- 声明生成宏；
- 对 `if` / `for` / `match` / `defer` / `on error` 等核心语义的重写；用户宏不得
  重写 `fail` / `recover` / `pass`，这三者由后续阶段的内置宏 `fail!` /
  `recover!` / `pass!` 提供；
- `shape` 类型、proto 字段或鸭子类型；
- 运算符宏；
- 专用宏 opcode；
- v1 跨模块宏可见性；后续按第 3.4 节的 selective import 设计；
- 把 `from` 设为全局保留字；它只作为 selective import 的 contextual keyword；
- 改变装饰器语义：装饰器继续是 `@expr` 普通高阶函数。

特别注意：typechecker 目前仍有若干站点从原始源码文本恢复泛型实参。v1 模板
必须禁止生成泛型语法，否则展开后源码位置与类型实参不再对应。

## 7. 诊断

诊断必须包含调用点位置；涉及模板格式或占位符时还要包含定义点位置。

至少覆盖：

- `is not a known built-in or user-defined macro`；
- 未知占位符；
- 片段类型不匹配，例如把 `expr` 放进 `ident` 位置；
- `expr...` 不是最后一个表达式参数；
- `block` 不是最后一个参数，或出现在括号内；
- 同文件宏重名；遮蔽内置裸名不是错误，但需说明实际命中的来源；
- 自递归 / 未注册宏调用；
- 模板中出现泛型语法；
- 无参数宏收到表达式参数，或需要参数的宏缺少参数；
- 内置宏裸名与规范路径解析到不同定义时，诊断说明实际命中的来源；
- 块片段中的控制流位置错误；
- selective import 缺少 `from` 或路径非法；
- selective import 的名字未导出、重复或 alias 冲突；
- `import {name!} from pkg;` 找不到对应宏导出。

## 8. 工具链

- **formatter**：无损往返 `macro` 声明、`#param` 占位符、`expr...`、宏调用和
  selective import；
- **lint**：报告未使用参数、未知宏、模板中的非法占位符；
- **LSP**：宏名补全、签名 hover、调用点/定义点跳转；v1 不做跨文件 rename；
- **snippets**：可选添加 `macro` 声明模板；
- **docs sync**：`README.md`、`SYNTAX.md`、`docs/macros.md` 与本文件同步。

## 9. 测试计划

### 9.1 parser / 展开

- 四种片段单独与组合使用；
- `expr...` 后接尾部 `block`；
- 内置宏与用户宏同名冲突；
- 声明前调用、未知宏、递归宏；
- 模板中的非法占位符和非法片段位置；
- 表达式上下文中的展开结果；
- 块片段中的 `return` / `yield` / `do` 语义；
- selective import 的值/类型/宏形式、alias、`from` contextual keyword；
- `import {retry} from util;` 与 `import {retry!} from util;` 命名空间隔离。

### 9.2 hygiene / 运行时

- 宏内部局部名与调用点局部名冲突；
- `ident` 片段引入调用点绑定；
- `expr` 优先级保持，例如 `#value + 1` 不会改变结合性；
- 展开结果与手写代码行为一致；
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
| P4（同文件 v1 已完成） | 用户宏 v1：`expr` / `ident` / 尾部 `expr...` / 尾部 `block` | 复用 P2 建立的宏分发路径与调用简写；跨包阶段单独推进 |
| P5 | `on` / `error`、`as`、`ref` / `weak`、`is` | 高风险 contextual，冲突面大，必须各自独立 |

具体顺序：

1. **P1.1（已完成）**：`protocol` → `proto` 实现落地。lexer / parser / AST / typecheck /
   LSP / formatter / lint / 反射 kind 字符串同步改为 `"proto"`；泛型约束列表里的
   `proto` / `type` 仍是 contextual 标记；不保留 `"protocol"` 别名。
2. **P1.2（已完成）**：值/类型的 selective import `import {name} from pkg;`。
   `from` 只在 `import { ... }` 后 contextual；formatter / lint / LSP、文档与
   测试已同步，check-only 工具不执行被导入的包。
3. **P2.1（已完成）**：普通内置函数改为默认 import 的 prelude 值。`len` /
   `range` 放入 `std.utils`；`print` / `println` / `input` → `std.io.*`；
   `kind_of` / `type_*` → `std.type.*`。lexer 不再发 `SKEIN_TOK_BUILTIN`，初始
   作用域注入绑定；VM / JIT 的 builtin id 不变。解析优先级为局部绑定 > 显式
   import > 默认 import。
4. **P2.2（已完成）**：内置宏改为 parser 侧默认宏绑定（第 3.5 节）。裸名只是别名，规范
   路径仍可显式调用；加入 `fail!` / `recover!` / `pass!`（`std.error.*!`）和
   `wait!`（`std.task.wait!`，展开为 `#expr.wait()`）。落地第 3.3 节的
   `name!` / `name! x` 两种简写；旧 bare 形式暂时并行，后续阶段再 deprecate。
5. **P2.3（已完成）**：`list` / `dict` 改为默认 import，新增 `std.list.new` /
   `std.dict.new`；`list(x)` / `dict(x)` 解析为对应构造器，值作用域可遮蔽，
   类型命名空间保持独立。不做 `list!` / `dict!` / `try!`。
6. **P3.1（已完成）**：`wait` 退出保留字；`launch` / `wait` / `await` 改为表达式起始 contextual
   keyword。`launch f()` 启动 task，`await f()` 等价 `wait launch f()` 并要求
   后面是调用表达式；已有 task 写 `wait!(t)`。contextual 槽位覆盖语句开头、
   `=` / `return` / `yield` / `(` / `[` / `{` / `,` / `:` 和二元运算符之后；`.`
   之后仍是普通名字；`await(f())` 是前缀形式，同名变量写 `(await)(f())`；
   postfix 优先级高于前缀。`await f() on error(...)` 沿用当前 handler 语义。
7. **P4（已完成）**：`macro` 声明语法、签名校验、同文件宏表、未知宏诊断，以及
   `expr` / `ident` / 尾部 `expr...` / 尾部 `block` 四种片段的完整 v1 展开；
   hygiene、块函数上下文、嵌套调用与递归禁止均已落地。
8. **P4.2（后续）**：`export macro` 与 `import {name!} from pkg;`；跨包 hygiene、
   循环检测和完整测试矩阵。
9. **P5.1**：`on` / `error` 评估 handler-scoped contextual；`error!` 改为普通
    `NAME MACRO_BANG` 路径，`type error` 走类型命名空间，`.error` 仍是普通字段；
   保留 `%prec ON`，补齐 handler body / defer / 默认参数诊断。
10. **P5.2**：`as` contextual，独立步骤。
11. **P5.3**：`ref` contextual，独立步骤。
12. **P5.4**：`weak` contextual，独立步骤。
13. **P5.5**：`is` contextual，独立步骤。

每个阶段都要保持第一批宏与全量测试全绿；用户宏不得改变第一批宏的行为；每阶段
运行 `tools/check_docs_sync.py` 与 `git diff --check`。

## 11. 已确认的默认决策

以下决策已确认，作为实现约束：

1. 使用 `#参数名` 作为模板占位符标记；
2. `macro` 作为新保留关键字；
3. v1 只支持同文件、声明先于使用；
4. 禁止递归和跨模块宏；
5. 内置宏改为默认 import 的解析期绑定；同文件用户宏可遮蔽裸名，规范路径仍可
   显式调用（已确认，以第 3.5 节为准）；
6. 用户宏作为 M11，不回写已完成的 M10；
7. 跨包 selective import 采用 `import {name} from pkg;`，宏写作
   `import {name!} from pkg;`；
8. `from` 只作为 selective import 的 contextual keyword，不设为全局保留字；
9. 先做低风险 contextual keyword / selective import，再实现宏；`as`、`ref` /
   `weak`、`is` 等中风险关键字在宏实现之后分独立步骤处理；`wait` 走内置宏，
   `launch` / `await` 走表达式起始 contextual；
10. `in`、`export`、`package`、`import`、`include` 同属低风险 contextual
   keyword，在 Phase 1 处理；`module` 仅用于 manifest，不占 `.skein` 保留字；
11. 普通内置函数改为默认 import；`len` / `range` 放进规划中的 `std.utils`
   模块，而不是 `std.prelude`；解析优先级为局部绑定 > 显式 import > 默认
   import；
12. `list` / `dict` 作为默认 import 的 `std.list` / `std.dict`，裸构造调用与
   其他类型构造器统一，降糖为 `std.list.new` / `std.dict.new`；
13. `fail` / `recover` / `pass` 通过内置宏消除保留字，不再采用 statement-start
   或 handler-scoped contextual 方案；诊断仍由现有 handler 栈检查产生；
14. 宏简写只支持 `name!` 与 `name! x`；列表、字典、表达式、嵌套宏和成员链
   一律写括号；`foo! { ... }` 始终是 block；
15. `on` / `error` 留到 Phase 9 再决定是 handler-scoped contextual 还是宏形式；
16. 内置宏默认 import 映射采用第 3.5 节的表；`fail!` / `recover!` / `pass!`
    的规范路径为 `std.error.fail!` / `std.error.recover!` / `std.error.pass!`；
17. 协议声明关键字 `protocol` 重命名为 `proto`，作为全局保留字；泛型约束列表中
    的 `proto` / `type` 是地位相同的 contextual 标记，只在类型参数约束位置
    生效；反射 kind 字符串同步改为 `"proto"`，不保留 `"protocol"` 别名；
18. `wait` 改为内置宏 `wait!`，规范路径 `std.task.wait!`，展开为 `#expr.wait()`；
    `launch` 与新增的 `await` 是表达式起始 contextual keyword，不是全局保留字。
    contextual 槽位覆盖语句开头、`=` / `return` / `yield` / `(` / `[` / `{` /
    `,` / `:` 和二元运算符之后；`.` 之后仍是普通名字；
19. `launch f()` 启动 task；`await f()` 等价 `wait launch f()`，要求后面是调用
    表达式，已有 task 写 `wait!(t)`。`await(f())` 是前缀形式；调用同名变量写
    `(await)(f())`。postfix 优先级高于前缀：`await f().xxx` 是
    `await (f().xxx)`，先 await 再取字段写 `(await f()).xxx`。`await f() on error(...)`
    沿用当前 `wait launch f() on error(...)` 的 handler 语义。
