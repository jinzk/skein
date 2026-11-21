# Tuple 语义与解构设计计划

状态：**待决策 / 待实施**。本文是设计计划，描述目标语义、改动范围和验证方式；
在决策通过并实施前，`SYNTAX.md` 中的现有行为仍是实际行为。

## 1. 背景与现状

Skein 同时在静态类型层和运行时层区分序列与元组，但语法和解构规则没有跟上：

| 层面 | list | pair / tuple |
|---|---|---|
| 字面量 | `[1, 2]`，运行时为可变 list | `(1, 2)`，运行时为 frozen list |
| 静态类型 | `list[T]` | `pair[A, B]` / `tuple[A, B, ...]` |
| 解构模式 | `[a, b]` | 目前没有专用语法，`(a, b)` 是语法错误 |
| `match` subject | 普通表达式 | 已有专门的 `match (a, b)` 产生式 |

相关代码位置：

- `src/interpreter/parser.y:1835`：`'(' tuple_body ')'` 构造 `NODE_TUPLE`；
  `parser.y:2154-2162` 的 `tuple_body` 要求至少 2 个元素。
- `src/runtime/valuetype.cpp` / `src/interpreter/eval.cpp:562-565`：
  `newtupleval()` 创建 frozen list，`newlistval()` 创建可变 list。
- `src/interpreter/typecheck.cpp:543-569`：当期望类型是 `pair` / `tuple` 时，
  **list 字面量节点 `'l'` 与 `NODE_TUPLE` 都被接受**，只检查长度和元素类型。
- `src/interpreter/parser.y:1457-1485`（`let_pattern`）、`2045-2098`（`pattern`）、
  `2174-2228`（`param`）：三处解构语法都只接受 `'[' pattern_list ']'`，不接受 `'('`。
- `src/interpreter/parser.y:1847`：`match (a, b)` 的 tuple subject 已经存在。

由此产生的实际不一致（均已用当前构建验证）：

```text
let a = (1, 2);  a[0] = 9;   // 运行时错误：cannot modify an immutable tuple
let b = [1, 2];  b[0] = 9;   // 合法，得到 [9, 2]
let p: pair[int, int] = [1, 2];
p[0] = 9;                    // 合法 → [9, 2]，pair 的不可变性承诺不成立
let l: list[int] = [1, 2];
let p2: pair[int, int] = l;  // 类型错误：expected pair[int, int], got list[int]
```

即：**字面量长度匹配即可兼容 pair，但 list 变量不行**；类型标注承诺的是
frozen tuple，运行时拿到的却是可变 list。

## 2. 设计目标

1. 让字面量语法、静态类型、运行时对象和解构语法遵守同一套规则；
2. tuple 是固定长度、异构、不可变的值；list 是变长、同构、可变的值；
3. `(a, b)` 不是 `[a, b]` 的等价糖：它的语法差异必须对应静态类型差异；
4. 保持 `match (a, b)` 的 subject 与 pattern 对称；
5. 不引入单元素 tuple、`()` 空 tuple 或 tuple 字面量的新语法形式。

## 3. 决策

### 3.1 pair / tuple 字面量严格化

`pair[A, B]` / `tuple[...]` 的初始化只接受 tuple 字面量 `(a, b, ...)`。

- `typecheck.cpp:543-569` 去掉 `expr->nodetype == 'l'` 的接受分支，只保留
  `NODE_TUPLE`；
- `let p: pair[int, int] = [1, 2];` 变成类型错误，错误信息建议：
  `pair expects a tuple literal; use (a, b)`；
- 函数参数同理：`f(p: pair[int, int])` 只接受 `f((1, 2))`，不再接受 `f([1, 2])`；
- 不新增 list → tuple 的隐式转换；需要从 list 构造 tuple 时显式写出
  `(xs[0], xs[1])`；
- 该规则只影响**字面量**检查。已经具有 `pair` / `tuple` 原生静态类型的变量、
  参数、返回值照常参与类型检查。

理由：`[1, 2]` 产生的是可变 list，无法提供 tuple 的不可变性；允许它作为 pair
字面量会让静态类型和运行时对象脱节，也会给下面的 `(a, b)` 模式开后门。

### 3.2 新增 tuple 解构模式 `(a, b)`

在三处解构语法中新增 tuple 模式产生式：

```text
let (a, b) = (1, 2);                    // let / const 解构声明
func f((a, b)) { return a + b; }        // 命名函数 / func 参数
match (1, 2) {
  case (x, y) => x * y;                 // match 模式
  case _ => 0;
}
```

语法规则：

- 元素个数 **至少 2 个**，与 `tuple_body` 一致；
- `(a)` 继续是分组表达式，不构成单元素 tuple 模式；
- `()` 不引入；
- 暂不支持尾逗号 `(a, b,)`，避免与参数列表规则混淆；
- 模式内部禁止 `...rest`，因为 tuple 长度固定；
- 允许嵌套：`let (a, [b, c]) = (1, (2, 3));` 中外层 tuple 的元素 `[b, c]`
  仍是普通 list 模式；
- `match` 中允许沿用 `AS` / `|` 等外层组合：`case (x, y) as t => ...`。

实现上建议新增独立的 `newpatterntuple()` 节点/标志，而不是把 tuple 模式降级为
`'[' pattern_list ']'`；否则静态检查无法区分两种模式，3.3 的规则无法落地。
`src/interpreter/pattern.h` 中的模式描述需要携带该标志，供 compiler / VM /
JIT 共用。

### 3.3 tuple 模式的静态类型规则

`(a, b, ...)` 模式要求 subject 的**原生静态类型**是 `TYPE_PAIR` 或 `TYPE_TUPLE`：

| subject | 原生类型 | `let (a, b) = ...` |
|---|---|---|
| `(1, 2)` | `pair[int, int]` | 允许 |
| `[1, 2]` | `list[int]` | 拒绝 |
| `let p: pair[int, int] = (1, 2)` 后的 `p` | `pair[int, int]` | 允许 |
| `any` / 未收窄的 `union` | 无法证明是 tuple | 拒绝 |
| FFI / 动态返回值 | 无法证明是 tuple | 拒绝 |

补充规则：

- 元素绑定类型从 `pair/tuple` 的类型参数按位置推断，与 list 模式的元素推断一致；
- 静态类型已知 arity 时，模式元素个数必须一致，否则编译期报错；
- 这里的“原生静态类型”不是赋值兼容性：即使 `typecheck.cpp` 将来允许某种
  双向兼容，只要表达式本身是 `list[int]`，`(...)` 模式就不接受它。

理由：运行时 tuple 表现为 frozen list，无法事后区分来源；若允许 `list` 进入
tuple 模式，固定长度和不可变性保证都会在运行时失效。

### 3.4 `[a, b]` 模式保持宽松

`[a, b]` 继续作为**序列模式**：接受 `list`，也接受 `pair` / `tuple`，长度在
运行时校验（静态已知时可提前校验）。带 `...rest` 的写法也只属于 `[...]` 模式。

这是兼容既有代码的刻意保留：

```text
let [a, b] = (1, 2);              // 继续合法
func f([a, b]) { return a + b; }  // f((3, 4)) 继续合法
```

因此两套模式是互补关系，不是替换关系：

| 用途 | 写法 |
|---|---|
| 变长序列 / 需要 rest | `[a, b, ...rest]` |
| 任意固定长度序列（含 tuple） | `[a, b]` |
| 只接受 tuple，固定长度，无 rest | `(a, b)` |

### 3.5 `match` 对称性

`match (a, b) { case (x, y) => ... }` 成为完整对称写法。现有
`case [x, y]` 继续可用，语义为宽松序列模式；两种 pattern 可以同时出现在
不同 arm 中，不产生二义性：

```text
match value {
  case (x, y) => ...   // 只匹配 tuple
  case [x, y] => ...   // 匹配任意二元素序列
}
```

## 4. 改动范围

### 解析器

- `src/interpreter/parser.y`
  - 新增 `tuple_pattern_items`（≥2 个 `pattern`，禁止 rest / 尾逗号）；
  - `let_pattern`、`pattern`、`param` 各新增 `'(' tuple_pattern_items ')'`；
  - 注意与既有 `'(' exp ')'` 分组、`tuple_body` 表达式和 lambda 参数列表的
    grammar conflict；必要时用已存在的 `HEADER_LPAREN` 式消歧 token。

### 类型检查

- `src/interpreter/typecheck.cpp:543-569`：字面量接受条件收紧为 `NODE_TUPLE`，
  补充专用错误信息；
- tuple 模式检查：确认 subject 原生类型是 `TYPE_PAIR` / `TYPE_TUPLE`，
  校验 arity，并把元素类型绑定到模式变量；
- 确保 list 字面量不会被“赋值兼容”路径间接接受为 tuple 模式 subject。

### 模式表示与运行时

- `src/interpreter/pattern.h` 及模式构造处：增加 tuple 模式标志；
- compiler / VM / JIT 的模式匹配路径：tuple 模式复用按位置绑定逻辑，
  但必须保留固定长度检查；失败时沿用现有解构失败错误路径
  （如 `function parameter does not match its pattern`）；
- 不需要新的运行时值类型：tuple 仍是 frozen list。

### 文档与测试

- `SYNTAX.md`：更新参数解构（约 495-506 行）、类型系统说明（约 1095-1097 行）、
  解构声明限制（约 1124-1145 行）以及 `match` 说明（约 1421 行）；
- `docs/stdlib.md` 中如出现 `pair`/`tuple` 示例，同步检查；
- 新增 `tests/skein/tuple_semantics.skein`，覆盖第 5 节的合法与非法用例；
- `docs/roadmap.md`：链接本文档，决策通过后从“待决策”移入实施项。

## 5. 验证用例

合法：

```text
let (a, b) = (1, 2);
let (a, [b, c]) = (1, (2, 3));
func f((x, y)) { return x + y; }
print(f((3, 4)));
match (1, 2) { case (x, y) => x * y; }
let p: pair[int, int] = (1, 2);
let [x, y] = p;
```

非法（均应有明确诊断）：

```text
let (a, b) = [1, 2];                       // 模式类型不匹配
let p: pair[int, int] = [1, 2];            // 需要 tuple 字面量
func f((a, b)) {...}; f([1, 2]);           // 同上
let (a, b, c) = (1, 2);                    // arity 不匹配
let (a) = (1, 2);                          // 分组，不是 tuple 模式
let (a, ...) = (1, 2);                     // tuple 模式禁止 rest
let x: any = (1, 2); let (a, b) = x;       // 无法证明是 tuple
```

运行时校验：

```text
let a = (1, 2); a[0] = 9;    // 仍报 cannot modify an immutable tuple
```

## 6. 兼容性

这是**破坏性变更**，需在发布说明中列出：

- `let p: pair[int, int] = [1, 2];` 及函数参数的同类写法会开始报错；
- 当前仓库 `tests/skein` 中未发现此类写法；
- `[a, b]` 解构 tuple 的行为不变，因此主要迁移成本是把 pair/tuple 字面量
  从 `[...]` 改成 `(...)`；
- 建议实施时同时提供一个过渡期诊断（warning 或带修复建议的 error），
  但不在运行时做隐式 frozen 转换。

## 7. 非目标

- 不引入单元素 tuple / 空 tuple；
- 不改变 tuple 的运行时表示（仍是 frozen list）；
- 不新增 `list` 与 `tuple` 之间的转换函数；
- 不改变 `for` 循环绑定语法（当前 `for` 只支持变量名或双变量，不支持解构）；
- 不把 `[a, b]` 模式的行为改严格，避免破坏已有代码。
