# 用户自定义宏设计（提案）

- 状态：待决策，尚未实现
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

`macro` 建议作为新的保留声明关键字。宏名使用普通标识符，但不能与第一批
内置宏名冲突：`trace!`、`debug!`、`info!`、`warn!`、`error!`、`log_to!`、
`capture!`、`std.assert.that!`、`std.assert.schema!`。

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

### 3.4 跨包导入（后续阶段）

v1 只做文件内可见性。跨包阶段不引入独立的 `use` 关键字，而是先给值/类型
增加 selective import，再让宏复用同一形式：

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

## 4. 展开模型

### 4.1 解析期宏表

Parser 维护一个 `MacroTable`，键为宏名，值为已验证的 `MacroTemplate`。

处理顺序：

1. 解析到 `macro` 声明时，把宏体解析成内部模板 AST，校验片段位置和占位符；
2. 校验通过后注册到 `MacroTable`；该声明本身不产生运行时 AST；
3. 解析到 `name!(...)` 时，先按现有规则尝试内置宏，再查 `MacroTable`；
4. 找到用户宏后，在 grammar action 中实例化模板；
5. 实例化后的普通 AST 才进入逐 `top_statement` 的 typecheck。

这不是整文件可变重写 pass：展开仍发生在语法分析阶段，保持第一批宏的实现
边界。为了让自递归在定义期就被拒绝，当前宏在模板校验完成前不注册。

### 4.2 可见性

v1 只支持同文件、声明先于使用：

- 宏声明之后的代码可以调用；
- 声明之前的代码不能调用；
- import / export 宏、包级宏、跨模块宏都明确不支持；
- 内置宏名不能被用户宏覆盖。

跨包阶段按第 3.4 节推进：先实现值的 selective import，再支持
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
- 对 `if` / `for` / `match` / `defer` / `on error` 等核心语义的重写；
- `shape` 类型、protocol 字段或鸭子类型；
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
- 宏重名、覆盖内置宏名；
- 自递归 / 未注册宏调用；
- 模板中出现泛型语法；
- 无参数宏收到表达式参数，或需要参数的宏缺少参数；
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

实施顺序遵循“先低风险、后中风险”：低风险 contextual keyword 和 selective
import 先做；中风险关键字在宏实现完成后再分独立步骤处理。

1. **Phase 0**：本 ADR 评审并确认第 11 节的待定项。
2. **Phase 1**：contextual keyword 基础设施；先把低风险的 `in`、`export`、
   `package`、`import`、`include` 改为 contextual keyword；`module` 在
   `.skein` 源码中本来就不是保留字。
3. **Phase 2**：值的 selective import `import {name} from pkg;`，`from` 从一开
   始就是 contextual keyword；配套 formatter / lint / LSP、文档与测试。
4. **Phase 3**：`macro` 声明语法、签名校验、宏表、未知宏诊断；先只支持一个
   `expr` 片段的模板展开与 hygiene 基础设施。
5. **Phase 4**：补齐 `ident`、`expr...`、`block` 与块函数上下文；完整测试矩阵。
6. **Phase 5**：`export macro` 与 `import {name!} from pkg;`，跨包 hygiene、
   循环检测和完整测试矩阵。
7. **Phase 6+**：中风险关键字（`as`、`ref` / `weak`、`launch` / `wait`、`is`）
   按独立步骤处理，均排在宏实现完成之后。

每个阶段都要保持旧的内置宏测试全绿；用户宏不得改变第一批宏的行为。

## 11. 待确认的默认决策

以下为提案默认值，实现前需要明确批准：

1. 使用 `#参数名` 作为模板占位符标记；
2. `macro` 作为新保留关键字；
3. v1 只支持同文件、声明先于使用；
4. 禁止递归和跨模块宏；
5. 禁止用户宏覆盖内置宏名；
6. 用户宏作为 M11，不回写已完成的 M10；
7. 跨包 selective import 采用 `import {name} from pkg;`，宏写作
   `import {name!} from pkg;`；
8. `from` 只作为 selective import 的 contextual keyword，不设为全局保留字；
9. 先做低风险 contextual keyword / selective import，再实现宏；`as`、`ref` /
   `weak`、`launch` / `wait`、`is` 等中风险关键字在宏实现之后分独立步骤处理；
10. `in`、`export`、`package`、`import`、`include` 同属低风险 contextual
   keyword，在 Phase 1 处理；`module` 仅用于 manifest，不占 `.skein` 保留字。
