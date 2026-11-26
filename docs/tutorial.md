# Skein 教程

本教程按「先跑起来，再学语言，再查标准库，最后做工程化」的顺序组织。第一次阅读建议按第一、二部分顺序走一遍；后面几部分更像是按场景查阅的手册。

语法细节查 [SYNTAX.md](../SYNTAX.md)，标准库 API 查 [标准库指南](stdlib.md)，只想快速浏览语言特性可以看 [README](../README.md)。

协议声明关键字为 `proto`，属于全局保留字；`protocol` 不再是关键字。泛型约束
列表里的 `proto` / `type` 是地位相同的 contextual 标记，只在
`[V proto Drawable]` 这类位置生效。反射 kind 字符串为 `"proto"`。worker 配置里的
`protocol` 字段是普通 API 名，不受此重命名影响。

## 本教程内容

- [第一部分：入门](#第一部分入门)
  - [脚本模式与 REPL](#脚本模式与-repl)
  - [交互模式](#交互模式)
- [第二部分：语言基础](#第二部分语言基础)
  - [语言概览](#语言概览)
  - [语法约定](#语法约定)
  - [类型与转换](#类型与转换)
  - [对象生命周期](#对象生命周期)
  - [格式化](#格式化)
  - [错误信息](#错误信息)
- [第三部分：常用标准库](#第三部分常用标准库)
  - [字符串处理](#字符串处理)
  - [控制台输入](#控制台输入)
  - [文件读写](#文件读写)
  - [用 dict + 闭包封装文件对象](#用-dict--闭包封装文件对象)
  - [字符、字节与编码](#字符字节与编码)
  - [正则表达式](#正则表达式)
  - [时间、随机数和 UUID](#时间随机数和-uuid)
  - [Base64、URL 和哈希](#base64url-和哈希)
  - [目录、路径和压缩](#目录路径和压缩)
- [第四部分：并发与进程](#第四部分并发与进程)
  - [launch 与 wait](#launch-与-wait)
  - [线程、Channel 与并发](#线程channel-与并发)
  - [环境变量和进程捕获](#环境变量和进程捕获)
  - [外部命令](#外部命令)
- [第五部分：工程化](#第五部分工程化)
  - [包与导入](#包与导入)
  - [开发期自动重启](#开发期自动重启)
  - [生产部署与优雅关闭](#生产部署与优雅关闭)

## 第一部分：入门

目标：把脚本跑起来，知道 REPL、源文件编码和最小脚本写法。

### 脚本模式与 REPL

可以直接把脚本文件作为第一个参数运行：

```bash
./build/skein script.skein
```

如果脚本不是 UTF-8，可以显式指定源文件编码：

```bash
./build/skein --encoding gbk script.skein
```

该编码同样应用于 `import` 加载的包文件和 `:load` 文件。带 BOM 的 UTF-16/UTF-32 文件不需要再指定编码。

也可以使用 shebang：

```text
#!/usr/bin/env skein
print("hello");
```

```bash
chmod +x script.skein
./script.skein
```

使用 `/usr/bin/env skein` 时，请确保 `skein` 在 `PATH` 中；也可以直接写解释器的绝对路径。

脚本模式会：

- 跳过第一行的 `#!`；
- 默认按 UTF-8 读取，并自动识别 UTF-8 / UTF-16 / UTF-32 BOM；
- 没有 BOM 时可用 `--encoding NAME` 指定 GBK、GB18030 等编码；
- 不打印 `> ` 提示符；
- 不打印顶层表达式的 `= result` 结果，需要输出时请使用 `print`；
- 忽略第一行之外的普通注释仍使用 `//`；
- 使用 `skein -` 可以从 stdin 运行脚本模式；
- 脚本中出现词法、语法或运行时错误时，退出码为 `1`；
- 默认在第一处错误后停止脚本；使用 `-k` / `--keep-going` 可以继续执行后续顶层语句。

Windows/MinGW 没有原生 shebang，直接使用：

```bat
build\skein.exe script.skein
```

shebang 行同样会被跳过。


最小示例：

```skein
print("hello, skein");
```

保存为 `hello.skein` 后运行 `./build/skein hello.skein`。


### 读取输入

`input([prompt])` 从 stdin 读取一行，返回字符串（不含末尾换行）：

```skein
let name = input("Name: ");
if (name == null) {
  println("no input");
} else {
  println("hello " + name);
}
```

- 可选的 `prompt` 必须是字符串，会原样输出到 stdout；
- 空行返回空字符串 `""`；
- 读到 EOF 且没有剩余内容时返回 `null`；最后一行没有换行符时仍返回该行；
- 读取多个值时，可以用 `std.string.split` 拆分后再转换类型（需要 `import std;`）。

更完整的规则见后面的“控制台输入”一节。


### 交互模式

- 只有 stdin 是终端时才显示 `> ` / `...> ` 提示符；管道输入不显示提示符；
- 使用 `skein -i` 可以强制显示提示符；
- `print` 不会自动补换行；REPL 会在下一轮提示符前按需补换行，避免提示符紧跟在无换行输出后面。脚本模式的输出字节不受影响；
- POSIX 终端和 Windows 控制台都支持方向键、Home/End、Delete、Ctrl-A/E/K/U/W、Ctrl-L；
- 支持上下方向键浏览历史；
- 历史默认保存到 `~/.skein_history`，可用 `SKEIN_HISTORY` 指定路径；
- Tab 补全支持语言关键字、REPL 命令、当前全局符号和 `std.*`；多个候选时会在输入行下方弹出候选窗（分栏显示）并扩展公共前缀；
- 多行输入会按当前语法层级自动缩进；行尾是运算符、逗号或 `.` 时额外缩进一级；行首输入 `}`、`)` 或 `]` 时自动回退一级；
- 输入行支持词法高亮：关键字、字符串、注释、数字和 REPL 命令；
- 高亮默认只在 stdin/stdout 都是终端时开启；`SKEIN_REPL_COLOR=always|auto|never` 可覆盖，设置 `NO_COLOR` 可强制关闭；
- 当前补全覆盖关键字、REPL 命令、全局符号、`std.*`、struct / enum variant 字段和 `:load` 文件路径；长行编辑使用水平滚动；
- 补全候选窗随终端宽度分栏，最多显示 6 行，超出时给出剩余数量；`↑` / `↓` 或 `Ctrl-P` / `Ctrl-N` 切换候选，`Tab` 接受当前候选，`Esc` / `Ctrl-G` 关闭弹窗；回车会先关闭弹窗再提交输入；
- `Ctrl-C` 取消当前输入，或中断正在执行的程序；
- `Ctrl-D` 退出解释器；
- 上一次非 `null` 结果保存为 `ans`，也可以使用 `_` 引用；
- REPL 命令：
  - `:help`
  - `:quit` / `:q`
  - `:load FILE`
  - `:history`

示例：

```text
> 1 + 2
= 3
> ans * 4
= 12
> _
= 12
> :load demo.skein
> :quit
```


## 第二部分：语言基础

目标：掌握日常写 Skein 需要的语法、类型系统和错误模型。

### 语言概览

#### 基本运算

```text
1 + 2
-3 + 5
10 / 4.0
7 % 4
1 & 3
1 << 4
"hello" + " world"
[1, 2, 3][1]
{"a": 1, "b": 2}["b"]
```

支持 `%`、`&&`、`||`、`!`、`&`、`|`、`^`、`~`、`<<`、`>>`。
`&&` / `||` 短路求值。

#### 管道与占位符

管道 `|>` 把左侧值作为右侧调用的第一个参数：

```text
xs |> f              // f(xs)
xs |> f(a)           // f(xs, a)
range(0, 10) |> std.iter.filter(func(x) -> x % 2 == 0) |> std.iter.collect()
```

- `|>` 左结合，优先级高于算术和比较运算，低于一元运算和后缀调用；
- 右侧不是调用时按 `f(xs)` 处理；右侧是调用时插入为第一个参数；
- 适合 `std.list` / `std.iter` 这类“主体在第一个参数”的 API；
- 需要把左值应用到右侧调用的返回值时，直接写 `(f(a))(x)`；`|>` 只负责插入第一个参数。

调用参数中的 `$` 是占位符：

```text
func add(a, b) { return a + b; }

let add2 = add($, 2);     // x -> add(x, 2)
add2(1)                   // 3

let three = add($, $);    // (a, b) -> add(a, b)
three(4, 5)               // 9

[1, 2, 3] |> std.list.map($, x -> x * 2)    // [2, 4, 6]

let twice = add($1, $1);  // x -> add(x, x)
twice(3)                  // 6

let swapped = add($2, $1); // (a, b) -> add(b, a)
swapped(1, 2)              // 3
```

占位符规则：

- `$` 只在调用参数中生效；每个裸 `$` 对应返回函数的一个新参数，按从左到右填充；
- `$1`–`$9` 显式引用生成函数的第 n 个参数，可重复（`add($1, $1)`）或换序（`add($2, $1)`）；
- 同一调用里的编号必须连续覆盖 `1..N`，不允许跳号：`f($3)`、`f($1, $3)` 都是语法错误；
- 裸 `$` 和 `$n` 不能混用；`$0`、`$10`、`${n}` 均不支持；
- 多占位符返回多参数函数，不是自动柯里化；
- 非占位参数在返回的函数被调用时才求值；
- 嵌套调用中的占位符绑定到最近的调用，需要外层占位时请显式写 lambda；
- `_` 仍然是 REPL 的上一次结果别名，不会被占位符占用。

#### 三元表达式

三元表达式：

```text
x > 0 ? "positive" : "non-positive"
0 ? 1/0 : 5          // 5，只求值选中的分支
```

`? :` 右结合，优先级低于 `||`，高于赋值。

#### 变量与常量

变量：

```text
let x = 10;
x = x + 1;
x

const y = 20;
y = 21;        // 错误：cannot assign to constant y
```

`const` 只约束绑定，不是深不可变：

```text
const a = [1, 2];
a[0] = 9;      // 允许
a              // [9, 2]
```

不可变标量 `const`（`int` / `float` / `bool` / `string` / `char` / `byte` /
`null`）在同一个编译单元内可参与编译期常量折叠；不改变运行时语义，可用
`SKEIN_NO_CONST_PROP=1` 关闭。

#### 类型标注

变量可以带类型标注：

```text
let name: string = "alice";
let count: int;

count = 3;          // 允许
count = "three";    // 错误：variable count expects int, got string
```

- 类型在运行时的初始化和重新赋值时检查，不做隐式转换；
- 支持 `any int float number bool string char byte bytes list dict deque heap set function builtin callable null userdata result error task generator socket listener websocket stream pair tuple union`；
- 带类型标注的 `let` 可以省略初始值，使用默认值：

```text
int / number  → 0
float         → 0.0
bool          → false
string        → ""
char          → '\0'
byte          → byte(0)
bytes         → 空 bytes
list          → []
dict          → {}
any / null    → null
T? / 包含 null 的 union → null
```

- `function`、`builtin`、`callable`、`userdata`、`task`、`generator`、`socket`、`listener`、`websocket`、`stream`、`pair`、`tuple`，以及不包含 `null` 的 `union` 必须提供初始值；
- `const` 也可以带类型，例如 `const x: int = 1;`；
- 普通类型标注只约束变量绑定，不约束容器内部元素，例如 `let a: list = [0]; a[0] = "x";` 仍允许；
- `for` 的初始化部分也支持类型标注：`for (let i: int = 0; i < 3; i++)`。

`T?` 等价于 `union[T, null]`：`int?` 与 `union[int, null]` 可以互相赋值，函数类型也相同；
两者都接受 `null`，省略初始值时默认为 `null`。`union[A, B]?` 等价于
`union[A, B, null]`。

```text
let a: int? = null;
let b: union[int, null] = a;   // 允许
let c: int? = b;               // 允许
let d: union[int, string]? = null;
```

#### 容器泛型

容器可以写泛型参数，参数只参与静态检查，运行时擦除：

```text
let xs: list[int] = [1, 2];
let d: dict[int, list[int]] = {1: [2]};
let s: set[int] = std.set.from_list([1, 2]);
let q: deque[int] = std.deque.from_list([1, 2]);
let h: heap[int] = std.heap.from_list([1, 2], less);

let raw = std.json.parse(text);
let ys = raw as list[int];          // 显式信任边界
let first = (raw as list[int])[0];  // 对 cast 结果取下标要加括号
```

- 支持 `list[T]` / `dict[K,V]` / `set[T]` / `deque[T]` / `heap[T]` / `task[T]` / `generator[T]` / `generator[T, S]` / `pair[A, B]` / `tuple[A, B, ...]` / `union[A, B, ...]`，支持嵌套和可空形式；
- 支持内建容器类型参数，也支持用户自定义泛型 `struct` / `enum` / `proto` / 函数，见下文“用户自定义泛型”；
- 用户类型可以作为参数，例如 `list[Point]`；`list` 仍然表示无元素约束；
- 检查初始化（字面量、`from_list`、构造字段、函数参数、默认值）和插入（下标赋值、`append`、`+=`、`set.add`、`push`、`merge`）；
- 运行时只检查外层容器类型，不扫描元素；动态路径仍可通过 `any` 绕过；
- 模式：`SKEIN_TYPECHECK=off|loose|warn|strict` 或 `--typecheck=...`，默认 `warn`；
  - `loose` 只报确定冲突；
  - `warn` 额外对隐式 `any -> 容器[T]` 发 warning；
  - `strict` 把该 warning 提升为 error；
- 内嵌在解释器里的 std 包启动时不做类型检查（它们由构建和测试覆盖）；
  设置 `SKEIN_TYPECHECK_EMBEDDED=1` 可以恢复检查，用于验证标准库源码；
- `as` 只改变静态类型，运行时是恒等操作，不做转换；确定的元素冲突仍然报错；
- `as` 不能作为赋值目标；`x is list[int]` 和模式中的泛型参数会被拒绝；
- 函数和方法调用会检查显式参数类型；方法还要求接收者有静态类型；
- 导入包的导出函数会使用其参数/返回类型签名，导出结构体的字段/方法和导出变量的类型标注也能解析；
- 解构声明、`match` 模式和 `for` 绑定会从值的静态类型推断元素/字段类型；
- `std.list.map` 会推断简单表达式 lambda 或单 return lambda 的返回类型，`std.list.zip` 会检查第二个参数是 list；
- 未注解的 `let` 会从初始化表达式推断类型；它不是固定标注，重新赋值为其他类型时会更新推断。

#### 用户自定义泛型

`struct`、`enum`、`proto` 和自由函数都可以声明类型参数：

```text
struct Box[T any] { value: T; }
enum Option[T any] { None; Some { value: T; } }

proto Keyed[K any] {
  func key(self): K;
}

func identity[T any](x: T): T {
  return x;
}
```

约束写法：

```text
[T any]
[T = any]                 // 默认类型实参
[K type int | string]     // 具体类型集合
[V proto Drawable]        // proto 组
[T hashable]
[T iterable]
[T proto hashable & Drawable]
[T proto iterable & Drawable]
```

`std.traits` 提供 `Hashable` marker proto 和 `Iterable[T]` proto；内置容器自动满足它们，用户类型通过 `dict_hash` / `iter(self): generator[T]` 满足。小写 `hashable` / `iterable` 是始终可用的内置约束别名。

实例化：

```text
let a: Box[int] = Box { value: 1 };
let b = Box[int] { value: 2 };
let c = Option[int].Some { value: 3 };
let d = Option[int].None;

identity(1);
identity[int](1);

let g = identity[int];
g(1);
```

规则：

- 泛型实例 invariant，`Box[int]` 不能赋给 `Box[any]`；
- 类型实参只参与静态检查，运行时擦除；
- `is Box` 合法，`is Box[int]` 报错；
- 不支持泛型方法、`impl Box[int]` 特化或泛型继承；
- 泛型 struct 支持递归字段，例如 `next: Node[T]?`；
- `impl Box` 中的方法使用 struct 的类型参数。

泛型 proto 按结构式签名匹配：

```text
func need[T Keyed](x: T): T { return x; }
func need_int[T Keyed[int]](x: T): T { return x; }

x is Keyed        // 允许
x is Keyed[int]   // 错误
```

元数据和序列化：

```text
std.meta.generic_info("Box[int]")
std.meta.type_args("Box[int]")       // ["int"]
std.meta.type_params("Box[int]")     // ["T"]

std.struct.decode({"value": 1}, "Box[int]")
std.struct.encode(box, "Box[int]")   // envelope，包含 $type
```

普通 payload 不保存类型实参；动态消息使用 envelope 的 `$type`。

#### 内置容器方法

内置容器支持值方法调用，方法名和对应命名空间函数一致：

```text
let xs: list[int] = [1, 2, 3];
xs.len();
xs.contains(2);
xs.map(func(x) { return x * 2; });
for x in xs.iter() { ... }

let d: dict[string, int] = {"a": 1};
d.len();
d.has("a");
d.keys();
for k in d.iter_keys() { ... }
for (k, v) in d.iter() { ... }

let q: deque[int] = std.deque.from_list([1, 2]);
q.size();
q = q.push_back(3);

for c in "ab".iter() { ... }
for b in std.bytes.from_hex("00ff").iter() { ... }
for x in gen().iter() { ... }
for x in (1, 2).iter() { ... }
```

方法调用等价于把接收者作为第一个参数：

```text
xs.len()        == std.list.len(xs)
d.iter_keys()   == std.dict.iter_keys(d)
q.push_back(3)  == std.deque.push_back(q, 3)
```

方法调度基于接收者的静态容器类型；静态类型是 `any` 时需要先标注具体容器类型，
或直接使用命名空间函数。

#### 函数类型

函数值可以写函数类型：

```text
let f: (int) -> int;
let g: () -> bool;
let nested: ((int) -> int) -> int;
let callbacks: list[(int) -> int];

struct Handler {
  callback: (Event) -> result;
}
```

- 支持空参数、嵌套函数类型和容器内的函数类型；
- 运行时只检查值是否可调用，不检查参数/返回值；
- 静态检查会校验函数赋值、参数/返回类型，以及通过函数类型发起的调用；
- 参数按逆变、返回值按协变比较；`any` 仍然作为渐进边界；
- 常见 math / string / bytes / 转换 / 文件 builtin 已有函数签名；可变参数 builtin 仍退化为 `callable`，部分带可选参数的容器 builtin 保留末尾可选参数信息；
- 不提供 `(int, int = 1) -> int` 或 `(...int) -> int` 显式语法；
- 默认参数函数可以作为参数传递，并可赋给更短的兼容函数类型；
- 变参函数保留 rest 类型，可以赋给能接受相应参数的函数类型；例如 `(...int) -> int` 可赋给 `(int, int) -> int`；
- 静态检查会拒绝多传参数；少传参数仍交给默认值或运行时处理；
- `obj.method` 会生成去掉 `self` 的函数类型；
- `list.map/filter/reduce/sort_by/group_by/unique/unique_by` 会检查回调签名，并从 `map` / `group_by` 的回调结果推断返回类型；
- `std.iter.map/filter` 会检查回调签名；`std.fn.compose/pipe` 只接受两个函数并推导完整组合类型，更多函数请嵌套组合；
- `task[T]` 可用于变量、参数和返回类型；`launch f()` 推导为 `task[T]`；
- task 方法有静态类型：`t.wait() -> T`、`t.try_wait() -> result[T, error]`、`t.detach() -> task[T]`；`task[T]` 采用 invariant；
- 部分 `std.net` / `std.io` / `std.http` / `std.http2` / `std.websocket` 的 `*_task` builtin 返回 `task[...]`，例如 `std.http.get_task` 返回 `task[dict[string, any]]`；其余未覆盖的异步 builtin 仍可能退化为 `any`；
- 提供不可构造的静态 userdata 类型 `socket`、`listener`、`websocket`、`stream`；网络、I/O 和文件接口会返回这些类型，`socket` 可传给接受 `stream` 的接口；
- `std.task.catch` 返回 `task[result[T, error]]`；`std.task.try_wait` / `std.task.try_wait_all` / `std.task.timeout` 返回 `result[...]`；
- generator 函数的类型为 `(参数) -> generator[T]`，`T` 由函数体中的 `yield` 表达式合并得到；发送值类型可以写成 `generator[T, S]`；
- 发送类型 `S` 可由返回注解 `generator[T, S]` 指定，也可以由 `let x: S = yield ...` 推断；`std.generator.next(g, value)` 会检查 `value` 是否符合 `S`；
- `for ... in generator[T]` 的元素类型为 `T`；
- `std.iter.collect` 对 `generator[T]` 返回 `list[T]`，`std.iter.map` / `std.iter.filter` 对 generator 返回新的 generator；`std.generator.next(generator[T])` 的 `"value"` 字段类型为 `T`；
- `pair[A, B]` 表示二元异构结构，可用 `(a, b)` 初始化；`p[0]` / `p[1]` 分别得到 `A` / `B`；`zip`、`dict.items`、`iter.enumerate` 使用 pair；
- `tuple[A, B, C, ...]` 表示三元及以上异构结构，可用 `(a, b, c, ...)` 初始化；索引和解构按位置得到类型；
- `union[A, B]` 表示成员之一，支持平铺多个成员，例如 `union[int, string, bool]`；异构 list 字面量会推断为 `list[union[...]]`；赋值和参数按成员检查；`union[T, null]` 与 `T?` 完全等价，`union[A, B]?` 与 `union[A, B, null]` 等价；
- union 支持常见控制流收窄：`if (x is T)` 的 then/else、`!`、`&&`、`match` 的 `case is T` 以及后续 `default` 分支；
- 支持稳定字段路径收窄，例如 `if (b.value is int) { b.value + 1; }`，包括嵌套字段；
- 对字段/变量的写入、接收者方法调用会撤销相关路径收窄；`||` 只做安全的负向收窄；
- 收窄绑定在发生赋值后回退到声明类型，避免在分支中继续使用过期的窄化类型；
- `pair` / `tuple` / `union` 是静态类型：pair/tuple 在运行时仍是 list，但 tuple/pair 字面量以及 `zip`、`dict.items`、`iter.enumerate` 产生的 pair 是不可变 list，不能索引赋值；`union` 的成员兼容性主要由静态检查器保证；`union[T, null]` 归一化为 `T?` 后同时保留运行时检查；
- 带 `pair` / `tuple` 注解的变量、参数、字段和返回值在运行时会校验值是不可变 tuple，且元素数量与注解一致；
- tuple/pair 可以作为 `dict` key 或 `set` 元素，前提是所有元素递归地属于合法 key 类型（`int/float/bool/string/char/byte/bytes/null`、嵌套 tuple/pair、可哈希 struct/enum）；含可变 list/dict、NaN 等元素的 tuple 不能作为 key；
- struct 也可以作为 `dict` key 或 `set` 元素：所有字段递归地属于合法 key 类型时可以自动哈希；否则可以实现 `std.dict_key.DictKey` 并提供纯函数 `dict_hash(self) -> int` 来自定义哈希；如果还需要自定义 key 相等，再实现 `std.dict_key.KeyEq` 的 `key_eq(self, other) -> bool`，它只影响 dict/set 的 key 语义，不改变普通 `==`；含可变 list/dict、NaN 等字段且没有自定义 hash 的 struct 不能作为 key；
- 当 key/元素类型静态已知时，类型检查器会在 `dict[K, V]` / `set[T]` 注解、参数、返回值、字段和 `std.dict.*` / `std.set.*` builtin 调用处提前拒绝不可哈希类型；`dict[any, V]` / `set[any]` 仍由运行时检查；
- key 的完整规则、错误和 `std.dict.index_by` 用法见 [key-semantics.md](key-semantics.md)；
- `match (a, b)` 的 subject 是 tuple/pair；tuple 模式 `(x, y)` 只匹配 pair/tuple，序列模式 `[x, y]` 匹配任意二元素序列；传单个 tuple 参数使用 `f((a, b))`；
- `std.list.append` / `std.dict.keys` / `std.set.from_list` 等容器 builtin 作为一等值时保留类型变量模板：`let f = std.list.append; f([1], 2)` 得到 `list[int]`，`f([1], "a")` 会报错；该模板行为仅限已知容器 builtin，用户自定义泛型函数见“用户自定义泛型”；
- `is` 和模式类型不接受函数类型签名。

#### 自增、自减与复合赋值

自增和自减：

```text
let i = 0;
i++            // 0，i 变为 1
++i            // 2，i 变为 2
i--            // 2，i 变为 1
--i            // 0，i 变为 0
```

- `x++` / `x--` 返回修改前的值，`++x` / `--x` 返回修改后的值；
- 只支持 `int` 和 `float`；
- 目标可以是变量、list/dict/bytes 下标或 struct 字段，例如 `a[0]++`、`d["x"]--`、`p.x++`；
- 下标表达式只求值一次；
- `const` 变量不能自增/自减，但 `const` 容器中的元素可以；
- 整数溢出、bytes 超出 `0..255`、dict 键不存在都会报错。

复合赋值：

```text
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 2;
x &= mask;
x |= mask;
x ^= mask;
x <<= 1;
x >>= 1;
```

- 支持变量、下标和字段；
- `a[i] += 1` 的下标只求值一次；
- 字符串和 list 支持 `+=`；
- 静态检查会按运算符推导结果类型；例如 `int += float` 的结果是 float，会因目标类型为 int 而报错；
- 类型约束和 `const` 规则与普通赋值一致。

变量名可以包含下划线：`my_value`。

#### 列表、字典与切片

列表和字典：

```text
let a = [1, 2, 3];
a[0] = 9;
a

let d = {"a": 1};
d["b"] = 2;
d

let empty_list = [];
let empty_dict = {};
let empty_list_alt = list();
let empty_dict_alt = dict();
let copied_list = list([1, 2]);
let copied_dict = dict({"a": 1});
```

`list()` / `dict()` 是 `std.list.new()` / `std.dict.new()` 的 prelude 别名；单参数
形式从可迭代对象构造，`dict` 接受 dict 或由长度 2 的 pair/list 组成的可迭代对象。
裸名可以遮蔽，但类型注解里的 `list[T]` / `dict[K, V]` 不受影响。

支持链式下标赋值：`a[0][1] = 9`、`d["a"]["b"] = 1`。

切片使用 Python 风格的 `start:stop:step`，支持 list、string 和 bytes：

```text
import std;

let xs = [0, 1, 2, 3, 4];
xs[1:4]        // [1, 2, 3]
xs[:3]         // [0, 1, 2]
xs[2:]         // [2, 3, 4]
xs[:]          // [0, 1, 2, 3, 4]
xs[-2:]        // [3, 4]
xs[::2]        // [0, 2, 4]
xs[::-1]       // [4, 3, 2, 1, 0]
xs[4:0:-2]     // [4, 2]
"你好世界"[1:3] // "好世"
std.bytes.from_string("abcd")[1:3] // 两个字节 "bc"
```

- `stop` 不包含在结果中；`step` 省略时为 `1`，不能为 `0`；
- string 按 UTF-8 码点、bytes 按字节、list 按元素切片；
- 边界和 step 必须是整数；边界可省略，负数下标从末尾计数，越界会截断；
- 切片返回新值，不修改原值；暂不支持切片赋值；
- 省略 `stop` 时可直接写 `xs[start::step]`，例如 `xs[i::j]`、`xs[2::2]`。

字典键一律使用下标访问：`p["x"]`。点号用于 struct 字段、enum 变体以及 `std`/包命名空间，普通 dict 写 `p.x` 会报错。

```text
let p = {"x": 1, "y": 2};
p["x"]           // 读取
p["y"] = 3;      // 赋值
p["x"] + p["y"]
```

嵌套访问同样使用下标：`p["a"]["b"] = 1`。

#### 控制流

控制流：

```text
if (x > 5) {
  print("big");
} else {
  print("small");
}

while (x > 0) {
  x = x - 1;
}

do {
  x = x - 1;
} while (x > 0);

for (let i = 0; i < 3; i = i + 1) {
  print(i);
}

for (let x in [1, 2, 3]) {
  print(x);
}

for (let key, value in {"a": 1, "b": 2}) {
  print(key + "=" + str(value));
}

for (let i in range(0, 3)) {
  print(i);
}

for (let key in {"a": 1, "b": 2}) {
  print(key);
}
```

控制流的条件圆括号也可以省略（Go 风格）：

```text
if x > 5 { print("big"); } else { print("small"); }
while x > 0 { x = x - 1; }
do { x = x - 1; } while x > 0;
for let i = 0; i < 3; i = i + 1 { print(i); }
switch x { case 1 => print("one"); default => print("other"); }
match x { case 1 => "one"; default => "other"; }
```

条件中的 dict、struct、函数或 `match` 等复合字面量必须加括号，例如 `if (d == {"a": 1}) { ... }`。省略括号的 C 风格 `for` 里，`for i = 0; ...` 是赋值，`i` 必须已声明；要新建变量请写 `for let i = 0; ...`。如果条件本身以 `(` 开头，会按带括号形式解析。

省略 `let` 和圆括号：

```text
for x in [1, 2, 3] {
  print(x);
}

for key, value in {"a": 1, "b": 2} {
  print(key + "=" + str(value));
}

for a, b in [(1, "x"), (2, "y")] {
  print(str(a) + ":" + b);
}

for a, b, c in [(1, "x", true)] {
  print(str(a) + ":" + b + ":" + str(c));
}
```

tuple 解构形式；没有歧义时括号可省略：

```text
for (a, b) in [(1, "x"), (2, "y")] {
  print(str(a) + ":" + b);
}

for (a, b, c) in [(1, "x", true), (2, "y", false)] {
  print(str(a) + ":" + b + ":" + str(c));
}

func triples(): list[tuple[int, string, bool]] {
  return [(1, "x", true)];
}

for (a, b, c) in triples() {
  print(str(a) + ":" + b + ":" + str(c));
}

for a, b, c in triples() {
  print(str(a) + ":" + b + ":" + str(c));
}
```

`for a, b in iterable` 在 iterable 不是 dict 时按 tuple 解构；
`for (a, b, ...) in iterable` 始终按显式 tuple 解构。元素静态类型必须是 pair 或 tuple，
元素个数必须一致；可以用于 list、generator 等单值迭代。它和 `for k, v in dict` 不同，
后者绑定 dict 的 key/value。

`for ... in` 规则：

- 单变量：list 遍历元素，dict 遍历键；deque / heap / set 也遍历元素（heap 按层序，set 按插入顺序）；
- 双变量：iterable 是 dict 时得到 `key, value`；非 dict 时按 tuple 解构；静态类型是 `any` 时保留运行时 dict key/value 行为；
- `for (a, b, ...) in iterable` 或 `for a, b, ... in iterable` 对每个元素做 tuple 解构，支持 list、generator 等单值迭代；元素静态类型必须是 pair 或 tuple，元素个数必须一致；
- 显式惰性迭代使用 `std.list.iter(xs)`、`std.dict.iter_keys(d)`、`std.dict.iter_values(d)`、`std.dict.iter_items(d)` 或 `std.dict.iter(d)`；这些返回 generator，可直接用于 `for` 或 `std.iter.*`；
- list 需要下标时用 `range(len(list))` 或 C 风格 `for`；
- string 遍历 UTF-8 字符；bytes 遍历 `0..255` 的字节值；
- 迭代遍历对象在循环开始时求值一次，循环期间修改原对象不会改变本次遍历；
- 循环变量只在循环作用域内有效；省略 `let` 时同样是块级新变量，会遮蔽外层同名变量；
- 省略 `let` 和圆括号时，循环体必须是 `{ ... }`；操作数以 dict 字面量开头时可以省略括号（如 `for k in {"a": 1}`），但 struct / 函数 / match 等复合字面量作操作数时仍要加括号；
- 支持 `break` / `continue` / `return`。

`do ... while` 至少执行一次循环体；`while` 必须和 `}` 在同一行，结尾分号可省略。

`break` / `continue` 可用于循环；`break` 也可用于 `switch`。

带标签的 `break` / `continue`：

```text
outer: for (let i in [1, 2, 3]) {
  for (let j in [1, 2, 3]) {
    if (i == 2 && j == 2) { break outer; }
    print(str(i) + "," + str(j));
  }
}
```

- 标签写成 `NAME: statement`，可以修饰任意语句；
- 标签必须是行首（允许缩进）的第一个代码 token；
- `label` 是普通标识符，不再是关键字；
- `break NAME;` 跳出指定的外层循环或 switch；
- `continue NAME;` 继续指定的外层循环的下一轮；
- `goto NAME;` 跳到当前块或外层块的标签；不能跳入内层块或兄弟块：

  ```text
  func f() {
    goto done;
    print("skipped");
    done: return 1;
  }
  ```

#### switch

`switch` 值匹配：

```text
switch (x) {
  case 1 => print("one");

  case 2, 3 => {
    print("two or three");
  }

  default => print("other");
}
```

- 选择值只求值一次；
- 无 fallthrough，第一个匹配的 case 执行后退出 switch；
- case 体可以是单条语句或代码块，并有独立作用域；
- `case` 支持多值，如 `case 1, 2, 3`；
- `default` 可选且必须最后；
- `break` 退出 switch，`continue` 作用于外层循环，`return` 退出函数；
- 支持 `int`、`bool`、`string`、`bytes`、`null`；
- 不支持 `float`、`list`、`dict`、`function`、`builtin`、`userdata`；
- case 值必须是值，不能写 `case x > 1 && x < 5` 这种条件；条件判断用 `if/else`；
- `case int` 不是类型模式，而是内置函数值，会作为不支持的 case 值报错。

#### match

`match` 模式匹配（表达式）：

```text
let next = match (state) {
  case "idle" => "planning";

  case {kind: "tool_result", ok: true, data: d} => d;

  case [head, ...tail] => head;

  case n if n > 10 => n;

  case Point {x, y} => x + y;

  default => state;
};
```

tuple/pair subject 可以用 tuple 模式严格按位置解构：

```skein
let point = (3, 4);

let sum = match point {
  case (x, y) => x + y;
  default => 0;
};
```

- `match` 是表达式，返回第一个匹配 arm 的值；
- arm 体是单条语句或代码块，块的值是最后一条语句的值；
- 支持字面量、绑定、`_`、dict（开放匹配 + `...rest`）、list（定长 + `...rest`）、tuple（固定长度，只匹配 pair/tuple，禁止 `...rest`）、struct（按字段）；
- 支持 or-pattern：`case 1 | 2 | 3`、`case E.A | E.B`；
- 支持整数范围模式：`case 1..9`（包含两端）；
- `case is Type` 是类型模式，例如 `case is int`、`case is Point`、`case is callable`；
- enum 匹配在没有 `default` 且缺少变体时报告 `match is not exhaustive`；
- `case pattern as name` 把整个匹配值绑定到 `name`；
- `case == IDLE` 匹配常量、名字或无载荷枚举变体；
- `match (a, b)` 的 subject 是 tuple/pair，不是两个匹配值；传单个 tuple 参数使用 `f((a, b))`；
- tuple 模式 `(x, y, ...)` 至少两个元素，只匹配 pair/tuple，长度必须一致，禁止 `...rest`；`(a)` 是分组表达式，不是单元素 tuple 模式；这些规则也适用于 `let (x, y) = ...` 和函数参数模式；
- `[x, y]` 是宽松序列模式，可以匹配 list、pair 和 tuple；需要限定为 pair/tuple 时使用 `(x, y)`；
- 守卫写成 `case pattern if expr => ...`；
- 绑定只读，作用域限于该 arm；
- 无 arm 命中且无 `default` 时运行时报错；
- 模式不能调用函数或写比较表达式；
- 不支持 float 字面量。

#### 错误处理

错误处理：

```text
func parse_positive(text) {
  let n = int(text);

  if (n <= 0) {
    fail "must be positive: " + str(n);
  }

  return n;
}

let n = parse_positive("0") on error(e) {
  print("bad input: " + str(e));
  recover 0;
};
```

错误值会记录抛出点：

```text
func inner() { fail "boom"; }
func outer() { inner(); }

let v = outer() on error(e) {
  println(std.error.format_trace(e));
  // example.skein:1:20: fail: boom
  //   in inner() called from line 2
  //   in outer() called from line 3
  let where = std.error.location(e);
  println(str(where["line"]));    // 1
  recover 0;
};
```

- `fail expr;` 抛出错误，`fail;` 抛出空错误；
- 普通值会被包装成 `error(kind="fail", message=...)`；
- 错误会在抛出点记录源位置和调用栈，捕获后仍然可读；
- `std.error.location(e)` 返回 `{source, line, column}`，`std.error.new` 创建的错误返回 `null`；
- `std.error.trace(e)` 返回调用栈（最多 64 帧），最内层帧在前；
- `std.error.format_trace(e)` 返回与 CLI 诊断格式一致的多行文本；
- `call() on error(e) { ... }` 捕获 `fail`、运行时错误和 `result.err`；
  `on` / `error` 只在紧贴表达式的 handler 位置是 contextual keyword，其他位置
  仍是普通标识符，`: error` 走类型命名空间、`.error` 是普通字段；
- `result.ok(v) on error(...)` 直接得到 `v`，不执行 handler；
- `e` 是 `error` 值，只在 handler 作用域内有效；
- `str(e)` 或 `std.error.message(e)` 取得消息；
- `std.error.kind(e)` 取得错误类别；
- `recover expr;` 结束 handler 并返回恢复值；
- `pass e;` 把错误继续传给外层，保留原始位置和调用栈；
- handler 必须以 `recover`、`pass`、`fail` 或 `return` / `break` / `continue` / `goto` 退出；
- 控制流退出 handler 时，原始错误视为已处理；
- 默认参数值和 `defer` 表达式中的 handler 只允许 `recover` / `pass` / `fail`；
- 未捕获的 `fail` 在抛出点报告，并打印调用栈；
- 跨 task / thread 传递的 `error` 会保留位置和调用栈。

`do { ... }` 是块表达式，值为最后一个语句的值：

```text
let v = do {
  let a = 1;
  a + 1;
};                                  // v = 2
```

块内语句必须写分号；声明、循环和空块的值是 `null`。

配合 `on error` 使用：

```text
let n = do {
  let a = parse_positive(input);
  a * 2;
} on error(e) {
  print("bad input: " + str(e));
  recover 0;
};
```

`return` / `break` / `continue` 会直接穿出块；在 `do { ... }` 块表达式中使用 `return` 会发出警告，推荐让最后一条表达式作为值。`do { ... }` 用于需要块值的场景（如 `let v = do { ... };`），普通代码块（函数体、`if` / `while` / `for` 体、`-> { ... }` lambda 体）继续用裸 `{ ... }`。`do { ... } while (...)` 仍是 do-while 语句，`while` 必须和 `}` 同一行；如果要写“块表达式后接 while 循环”，用分号分开。

#### 日志

设计说明见 [logging.md](logging.md)。

```text
std.log.new(options?)                  → logger
std.log.info(message[, fields])        // 默认 logger
logger.debug / info / warn / error(message[, fields])
logger.log(level, message[, fields])
logger.with(fields)                    → logger
```

示例：

```text
import std;

let lg = std.log.new({
  "name": "app",
  "level": "info",
  "format": "text",
  "fields": {"service": "demo"}
});

lg.info("started", {"port": 8080});
lg.with({"request_id": "r1"}).warn("slow request", {"ms": 1200});
lg.error("failed", {"code": 500});
```

输出：

```text
2026-11-20T12:34:56.789+08:00 INFO  app: started service=demo port=8080
2026-11-20T12:34:57.001+08:00 WARN  app: slow request service=demo request_id=r1 ms=1200
```

JSON 输出：

```text
let lg = std.log.new({"format": "json", "time": "utc", "name": "app"});
lg.info("done", {"ms": 12});
```

```json
{"time":"2026-11-20T04:34:56.789Z","level":"info","logger":"app","message":"done","fields":{"ms":12}}
```

写入文件：

```text
let lg = std.log.new({
  "output": "file",
  "path": "logs/app.log",
  "append": true
});
```

规则：

- 默认写 stderr，不污染 `print` 输出；
- 级别低于 `level` 的调用不格式化、不输出；
- 字段是最后一个 `dict` 参数，与 `with` 上下文合并，同名键以调用参数为准；
- `with` 返回新 logger，共享输出目标；
- `output` 支持 `stderr` / `stdout` / `file`；`writer` 可接管整行记录；
- 写入失败默认 `fail`，可用 `on_error` 改为 `warn` / `ignore`；
- `logger` 不能跨线程传递，`writer` 也是线程本地值。

> 数学函数 `log` 位于 `std.math`（需要 `import std;`），示例中的 logger 变量使用 `lg`。

#### 函数与闭包

函数、匿名函数与闭包：

```text
func add(a, b) {
  return a + b;
}

func typed_add(a: int, b: int) {
  return a + b;
}

func double(x) -> x * 2;

func fib(n) {
  if (n < 2) { return n; }
  return fib(n - 1) + fib(n - 2);
}

func make_counter() {
  let n = 0;
  func inc() {
    n = n + 1;
    return n;
  }
  return inc;
}

let c = make_counter();
c()
c()
```

参数可以带可选的类型约束：

```text
import std;

func apply(fn: callable, x: int) {
  return fn(x);
}

apply(std.math.sqrt, 16)
```

- 语法为 `参数名: 类型`，不写类型表示 `any`；
- 支持 `any`、`int`、`float`、`number`、`bool`、`string`、`bytes`、`list`、`dict`、`function`、`builtin`、`callable`、`null`、`userdata`、`task`、`generator`、`socket`、`listener`、`websocket`、`stream`、`pair`、`tuple`、`union`；
- 调用时做运行时检查，不做隐式转换；
- 类型错误不会进入函数体；
- 逻辑运算和比较运算都返回 `bool`。

函数可以带返回类型标注：

```text
func numbers(): list[int] {
  return [1, 2];
}

func numbers_fast(): list[int] -> [1, 2];

let xs: list[int] = numbers();
xs[0] = "a";   // 错误：expected int, got string
```

- 返回类型只用于静态检查，运行时不做转换；
- 检查器检查每个 `return` 表达式；
- `return;` 对非可空返回类型报错；
- 返回类型会传播给调用方，减少 `any` warning；
- 方法也支持返回类型标注；
- 匿名函数也支持返回类型标注，例如 `func(x: int): int -> x * 2`；短 lambda 不支持，需要时请使用 `func`；
- 函数/lambda 的表达式体统一使用 `->`；`=>` 只用于 match/switch arm 和 FFI 返回类型。

参数可以使用默认值：

```text
func greet(name: string, greeting: string = "hello") {
  return greeting + ", " + name;
}

func offset(a: int, b: int = a + 1) -> b;

greet("alice");          // "hello, alice"
greet("alice", "hi");    // "hi, alice"
offset(10);              // 11
```

- 默认参数必须排在必填参数之后；
- 省略参数时才求值，每次调用重新求值；
- 默认值可以引用更早的参数，例如 `b = a + 1`；
- 默认值不能引用后面的参数，否则报 `parameter ... is used before it is initialized`；
- 方法中的默认值可以引用 `self`，但 `self` 本身不能有默认值；
- proto 方法声明不能有默认值，`impl` 方法可以额外提供默认值；
- 可变参数必须最后，且不能有默认值。

函数是一等值，可以保存到变量、列表或字典中，并通过任意表达式调用：

```text
let fs = [adder(1), adder(2)];
fs[0](10)
d["handler"](42)
adder(3)(4)
```

匿名函数可以使用 `->` 简写；单参数、多参数和零参数都支持。函数体可以是单个表达式，也可以是用 `{ ... }` 包住的块：

```text
let inc = x -> x + 1;
let add = (a, b) -> a + b;
let zero = () -> 7;

[1, 2, 3] |> std.list.map(x -> x * 2)

let normalize = (x) -> {
  let y = x * 2;
  y + 1;
};

let empty = () -> {};
```

- `-> { ... }` 是块体 lambda，块的最后一个表达式是返回值；
- 块体里可以直接写 `return`，它会退出该 lambda，和 `func` 一致；
- `-> do { ... }` 仍然可用，但 `do` 会按块表达式处理，块内的 `return` 会触发警告；
- `->` 后第一个 `{` 固定按块解析，因此 dict 作为函数体时写 `({ ... })`；
- 普通代码块（函数体、`if` / `while` / `for` 体）继续用裸 `{}`；`do { ... }` 只用于需要块值的表达式场景。

匿名函数使用同一个 `func` 关键字，省略函数名：

```text
let double = func(x) { return x * 2; };
double(21)

let inc = func(x) -> x + 1;
inc(41)

(func(x) { return x + 1; })(41)
(func(x) -> x + 1)(41)
```

单表达式可以用 `->` 简写，表达式结果自动成为返回值；需要多语句时用 `-> { ... }`。`->` 优先级最低，因此 `func(x) -> x * 2 + 1` 的函数体是 `x * 2 + 1`。

匿名函数同样可以捕获定义处的作用域，并作为参数或返回值：

```text
func make_adder(n) {
  return func(x) { return x + n; };
}

let add5 = make_adder(5);
add5(3)                 → 8
```

也可以通过变量绑定实现递归：

```text
let fact = func(n) {
  if (n <= 1) { return 1; }
  return n * fact(n - 1);
};

fact(5)                 → 120
```

匿名函数规则：

- 没有名字，诊断和调用栈中显示为 `<anonymous>`；
- 每次求值都会创建新的闭包；
- `func(x) { ... }` 和 `x -> { ... }` 都是块体，块体中的 `return` 退出该函数或 lambda；
- `-> exp` 函数体隐含 `return exp`；
- `-> do { ... }` 的运行时语义与 `-> { ... }` 相同，但 `do` 块内的 `return` 会触发警告；
- 支持空参数和空函数体；
- 参数支持类型约束、默认值和可变参数，规则与命名函数一致；
- 立即调用时建议给匿名函数加括号，例如 `(func(x) { ... })(1)`。

没有显式 `return` 的函数，以及 `print`，返回 `null`。`null` 是假值，可用于条件判断，也可以作为参数传递。

函数参数也可以直接解构：

```text
func sum([a, b]) { return a + b; }
func pair({x, y}) { return x * y; }
struct P { x: int; y: int; }
func point(P {x, y}) { return x + y; }
func first([head, ...tail]) { return head; }
```

- 解构参数占一个位置，不参与命名查找；
- 支持 list、dict、struct、enum 变体模式；
- 解构失败会报 `function parameter does not match its pattern`。

可变参数：

```text
func sum(...nums: number) {
  let total = 0;
  for (let n in nums) {
    total = total + n;
  }
  return total;
}

sum()           // 0
sum(1, 2, 3)    // 6
```

规则：

- `...rest` 只能是最后一个参数；
- 最多一个；
- `rest` 在函数内是 `list`，没有额外参数时是 `[]`；
- 类型标注表示每个额外参数的类型，例如 `...nums: number`；
- 固定参数不足会报错；
- 方法也可以使用，`self` 仍然是第一个固定参数；
- proto 方法暂不支持可变参数。

调用点可以用 `...` 展开列表：

```text
func sum(...nums: number) {
  let total = 0;
  for (let n in nums) { total = total + n; }
  return total;
}

let xs = [1, 2, 3];
sum(...xs)       // 6
sum(0, ...xs)    // 6
```

展开项必须是 `list`，可以和普通参数混用。

#### 结构体、impl 与 proto

结构体、`impl` 与 `proto`：

```text
import std;

struct Point {
  x: int;
  y: int;

  func length(self) {
    return std.math.sqrt(self.x * self.x + self.y * self.y);
  }

  func moved(self, dx: int, dy: int) {
    return Point { x: self.x + dx, y: self.y + dy };
  }
}

// impl 是补充：继续给 Point 追加方法
impl Point {
  func sum(self) { return self.x + self.y; }
}

let p = Point { x: 3, y: 4 };
p.x
p.length()
let q = p.moved(1, 2);
p.sum()
```

- `struct` 声明字段；没有默认值的字段都必须初始化，未知字段或重复字段会报错；
- 构造使用 `Point { x: 3, y: 4 }`，字段顺序不限；
- 支持 copy-with：`Point { ...p, x: 9 }`，`...` 必须放在最前，后面的显式字段覆盖原字段；
- 字段可以声明默认值：`struct Config { host: string = "localhost"; port: int = 80; }`；省略字段时使用默认值；
- 默认值在定义包的作用域中求值，每次构造重新求值；不能引用 `self` 或其他字段；
- `std.struct.decode` 不自动填充默认值，缺失字段仍然报错；
- 字段可以读写：`p.x`、`p.x = 5`；
- 所有字段递归可哈希时，结构体可以自动作 `dict` key 或 `set` 元素；也可以实现 `std.dict_key.DictKey` 的 `dict_hash(self) -> int` 跳过字段检查并使用自定义 hash；key 保存插入时的字段快照，之后修改原变量不会改变已插入的 key；
- `struct` 可以直接写方法，也可以只写字段；inline 方法使用与 `impl Type` 方法相同的 `func` 语法；
- `impl Type` 保留为补充手段，用来给同一类型追加方法；inline 方法与 `impl` 方法共享方法命名空间，同名重复（inline / inline、inline / impl、impl / impl）报 `method repeated <name>`，`impl` 不能覆盖 inline 方法；
- 方法第一个参数必须是显式 `self`；例外是构造函数 `func new(...)`（无 `self`，只能通过 `Type.new(...)` 调用，返回类型隐式固定为 `Type`，且不能声明返回类型或带装饰器）；
- 目标类型是 `ref T` 时，构造函数调用和 `T { ... }` 字面量会隐式包装为引用；已经持有 struct 值的变量必须用 `ref(value)` 或 `value as ref T` 显式转换，容器元素同样需要显式 `ref(...)`；
- 方法参数支持默认值，但 `self` 不能有默认值；默认值可以引用 `self` 和更早的参数；
- 方法只能通过实例调用：`p.length()`；构造函数是唯一例外，用 `Point.new(...)` 调用；
- 方法本身可以作为值提取，例如 `let f = p.length; f()`；
- 方法值绑定提取时的结构体快照，之后修改原结构体不会影响已绑定的方法；
- 方法可以修改 `self` 的字段和下标；结构体是值类型，修改只作用于方法内的接收者副本，调用方原值不变。需要保留修改时返回新结构体，例如 `return self`；
- 结构体是值语义，赋值和作为参数传递时使用 copy-on-write；
- 结构体类型可以用在变量和参数类型标注中：`let p: Point = Point { x: 1, y: 2 }`、`func f(p: Point)`；结构体类型没有默认值，变量必须初始化；
- 字段类型必须是已声明的类型；支持自引用，例如 `struct Node { value: int; next: Node?; }`；
  两个 `struct` 互引用需使用 `ref T?` / `weak T?`；
- 不允许创建环：`a.next = b; b.next = a;` 会报 `recursive value would create a cycle`；共享子结构（DAG）允许；
- 字段和方法默认公开；`_` 前缀成员是包私有：定义所在包内的函数和方法可直接访问，其他包直接访问报 `private member`；构造、反射、序列化和 snapshot 仍包含这些成员；
- 字段可以保存函数值，作为普通回调使用，不会自动绑定 `self`：

```text
struct Handler {
  callback: callable;
}

let h = Handler {
  callback: func(x) { return x + 1; }
};

h.callback(41)      // 42
```

```text
struct Config {
  host: string = "localhost";
  port: int = 80;
}

let a = Config {};
let b = Config { port: 8080 };
let c = Config { ...a, port: 9090 };
```

- 不支持继承、动态添加方法、静态方法或运算符重载；泛型 struct/enum/proto/函数见“用户自定义泛型”，内建容器类型参数见变量类型标注章节。

`proto` 定义结构式接口：

```text
proto Drawable {
  func draw(self);
}

struct DrawPoint {
  x: int;
  func draw(self) {
    print("point");
  }
}

func render(x: Drawable) {
  x.draw();
}

render(DrawPoint { x: 1 })
```

- `proto` 可以包含方法签名和默认方法体，不包含字段或 proto 继承；
- 一致性是结构式的：方法名、参数个数、参数类型、`self` 接收者和返回类型都要匹配，不需要声明实现关系；
- 没有默认体的 proto 方法必须由 inline 方法或 `impl Type` 方法匹配，否则不满足 proto；
- inline 方法和 `impl Type` 的同名方法都覆盖 proto 默认方法；多个 proto
  提供同名默认方法而类型没有自己的实现时报冲突，需要在 `struct` 中直接实现，
  或在 `impl Type` 中显式实现；
- proto 方法声明不能有默认值；inline 方法和 `impl` 方法可以额外提供默认值；
- 未标注类型的方法调用按值的运行时能力查找；proto 标注和 `is Proto` 是结构式契约；
- enum 上的方法名不能与任意变体字段重名；
- struct、enum 和 proto 名称共享类型命名空间，不能在同一个包中重名；
- 类型名按包注册，一个包对应一个源文件；不同包可以定义同名结构体或 proto；
- 类型默认不跨包可见；`export struct` / `export enum` 后可以按包路径使用；
- proto 名称按包解析，不使用 `export` 建立实现关系；
- `trait`、`impl Proto for Type`、`export proto`、`struct Name with Proto` / `enum Name with Proto` 简写均已移除，没有兼容别名；`with` 仍是普通标识符；
- 导入之后用 `aa.Point`、`lib.Drawable` 这样的限定名引用：

默认方法示例：

```text
proto Drawable {
  func draw(self) { print("default draw"); }
  func area(self);
}

struct Point {
  x: int;
  func area(self) { return self.x; }
  // draw 使用 proto 默认实现
}

Point { x: 3 }.draw();
```

```text
// package aa
export struct P { x: int; }
proto D { func d(self); }

// main
import aa;
let p: aa.P = aa.make();
p is aa.D

import aa as lib;
let q: lib.P = lib.make();
```

- 其他包的类型只要方法签名匹配就满足导入的 proto，不需要声明实现关系：

```text
// main
import aa;
struct Q { y: int; }
impl Q {
  func d(self) { return self.y; }
}
```

- 未导出的类型不能通过包路径引用，但它的值仍可作为普通值传递和方法调用。

#### `ref` / `weak` 与可选链

需要共享可变对象或表达环形引用时，显式使用 `ref T`；普通 `T { ... }`
仍是 COW 值：

```text
struct Node {
  value: int;
  next: ref Node? = null;   // 强边
  prev: weak Node? = null;  // 弱边，不保活
  func bump(self) { self.value = self.value + 1; }
}

let a = ref Node { value: 1 };
let b = ref Node { value: 2 };
a.next = b;
b.prev = a;

b.bump();                 // ref 接收者上的修改会持久化
print(a.next.value);      // 3
print(b.prev?.value);     // 3；弱引用失效时为 null
print(std.ref.upgrade(b.prev).value);  // 3
```

规则：

- `let c = a` 复制句柄而不是目标；`a = b` 只是重新绑定变量；
- 替换目标整体内容写 `a.* = Node { value: 9 }` 或
  `std.ref.set(a, Node { value: 9 })`；
- `weak T` 保存 `{id, generation}`，目标被 GC 回收后升级为 `null`；
- `weak` 只在 `weak T` 类型前缀位置是 contextual keyword；其他位置可作为变量、
  字段或宏名；
- `?.` 是通用可选字段访问，左侧为 `null` 时结果为 `null`；目前只支持字段，
  不支持 `?.()` 或 `?.[index]`；
- 不同类型互引用时，`ref T?` 可以指向尚未声明的类型，`weak T?` 用于不保活的反向边；
- 强 `ref` 环交给 GC，普通 struct / list / dict 的值环仍报
  `recursive value would create a cycle`；
- ref heap 严格线程本地。`ref` / `weak`（含嵌套）不能作为 channel payload，
  跨线程前用 `std.ref.snapshot(a)` 取普通值快照，或发送
  `std.arena` id / owner 线程消息。

命名函数装饰器：

```text
func trace(fn) {
  return func(...args) {
    print("enter");
    let result = fn(...args);
    print("exit");
    return result;
  };
}

@trace
func add(a, b) {
  return a + b;
}

add(1, 2)      // enter / exit / 3
```

- `@expr` 写在命名 `func` 前，可以连续写多个；多个装饰器从下往上应用；
- 支持普通函数、generator 函数和 `export func`；
- 装饰器表达式在函数绑定前求值，应用在函数创建后完成；
- 装饰器是普通高阶函数，最终结果必须 callable；
- 函数名绑定到装饰后的值，递归调用会经过装饰器；
- 匿名函数和 `let f = func(...)` 仍然手动包装：`f = dec(f)`。

方法装饰器：

```text
func trace(fn) {
  return func(...args) {
    print("enter");
    let result = fn(...args);
    print("exit");
    return result;
  };
}

struct Point {
  x: int;
  y: int;

  @trace
  func area(self, scale: int = 1) {
    return self.x * self.y * scale;
  }
}

Point { x: 3, y: 4 }.area()      // enter / exit / 12
```

- `@expr` 写在 `struct` 内的方法或 `impl` 方法前，可以连续写多个；多个装饰器从下往上应用；
- 装饰器在 `struct` / `impl` 声明执行时应用一次，不随方法调用重复执行；
- 装饰器是普通高阶函数，不需要专门的定义语法；
- 装饰器收到 unbound function，`self` 是第一个参数；推荐用 `...args` 转发；
- 支持 inline 结构体方法和 `impl Type` 方法；调用点、方法值提取和 proto
  默认方法查找不变；
- 默认值由生成的 wrapper 解析，装饰器看到的是补全后的参数；
- 装饰器必须返回 callable；不支持类型声明装饰器和解构参数方法；
- generator 方法也支持装饰器；生成的 wrapper 是普通函数，直接返回装饰器链产生的
  generator，是否惰性由装饰器决定；迭代、发送值和 `close` / `defer` 语义保持。

挂起方法：

```text
import std;

struct Client { host: string; }

impl Client {
  func get(self, path: string) {
    return std.http.get_task(self.host + path).wait();
  }
}

let c = Client { host: "http://127.0.0.1:8080" };
let result = c.get("/api");
```

- 方法可以调用会挂起的 API；调用方在普通函数或 task 中都会在当前 fiber 挂起；
- 方法支持默认参数、可变参数、`defer` 和递归；
- `self` 是接收者副本；共享 userdata 句柄正常共享，普通字段修改不会回写调用方；
- enum 方法和 proto 默认方法同样适用，所有方法语义一致；
- 需要显式 task 句柄时使用 `launch c.get(...)`，再 `task.wait()`；
- 装饰器包装的是普通方法；如果被包装方法会挂起，wrapper 调用它时也会在当前 fiber 挂起。

#### 枚举

枚举（带载荷变体）：

```text
enum Event {
  UserMsg { text: string; };
  ToolResult { ok: bool; data: any; };
  Timeout;
}

let a = Event.UserMsg { text: "hi" };
let b = Event.Timeout;

match (a) {
  case Event.UserMsg { text } => text;
  case Event.ToolResult { ok: true, data: d } => d;
  case Event.Timeout => "timeout";
  default => "other";
}
```

- 有载荷的变体必须写 `Event.Variant { field: value, ... }`；
- 无载荷变体直接写 `Event.Variant`；
- 变体字段可以声明默认值；所有字段都有默认值时允许 `Event.Variant {}`；
- 字段默认值在定义包作用域求值，每次构造重新求值；不能引用 `self` 或其他字段；
- copy-with 目前只支持 struct，不支持 enum；
- 载荷字段只读，字段访问按当前变体校验；
- 方法只能写在 `impl Event` 中；enum 方法内 `self` 只读，修改状态时返回新 enum；
- enum 方法按结构式规则满足 proto：只要 `impl Event` 的方法签名匹配即可；`Event` 也能用于 `is` 和类型标注；
- 方法名不能与任意变体的字段重名；
- `kind_of(e)` 是 `"enum"`，`type_name(e)` 是枚举名；
- `match` 支持变体模式、字段模式和守卫；
- JSON 编码为 `{"$enum":"Event.UserMsg","payload":{...}}`；
- enum 可以作为 `dict` key 或 `set` 元素：所有载荷字段递归可哈希时自动哈希；也可以实现 `std.dict_key.DictKey` 自定义 `dict_hash`；含可变 list/dict、NaN 等字段且没有自定义 hash 的 enum 不能作为 key；
- 暂不支持互递归 enum（自引用 enum 已支持）、enum 的 JSON 自动解码。

```text
enum Shape {
  Circle { r: int; };
  Rect { w: int; h: int; };
}

impl Shape {
  func area(self) {
    return match (self) {
      case Shape.Circle { r } => r * r;
      case Shape.Rect { w, h } => w * h;
    };
  }

  func draw(self) { return self.area(); }
}
```

#### 运行时类型检查

运行时类型检查：

```text
kind_of(x)        // "int" / "list" / "struct" / ...
type_name(x)      // "int" / "Point" / ...
type_package(x)   // 逻辑包名：入口为 "main"，包类型为包名
type_module_name(x) // Module 容器名；普通包和入口为空字符串
type_source(x)    // 调试用源文件路径

x is int
x is Point
x is Drawable
```

`any` 只是类型约束，不会丢失运行时类型。遍历异构列表时可以这样分派：

```text
for (let value in values) {
  if (kind_of(value) == "struct") {
    print(type_name(value));
  } else if (value is int) {
    print("int");
  }
}
```

`is` 只接受类型名或 proto 名作为右操作数，并按结构式签名匹配做运行时检查；类型名按包解析。
`is` 是 contextual keyword：只在 `expr is T` / `case is T` 右侧接类型时作为运算符；
其他位置仍是普通标识符，可作变量、字段、类型名或宏名。函数类型签名仍不允许作为右操作数。

#### 内置函数

无需 `import` 的 prelude 值：`len`、`range`、`print`、`println`、`input`、`list`、`dict`、`kind_of`、`type_name`、`type_package`、`type_module_name`、`type_source`，规范路径分别为 `std.utils.*`、`std.io.*`、`std.list.new` / `std.dict.new`、`std.type.*`；它们可被局部绑定或显式 import 遮蔽。`int`、`float`、`bool`、`str`、`char`、`byte` 仍是词法识别的转换函数与类型名。数学、字符串、格式化、文件与 bytes 函数位于 `std.math`、`std.string`、`std.fs`、`std.bytes`，使用前需要 `import std;`。内置函数同样是一等值：

```text
import std;

let f = std.math.sqrt;
f(9)

func apply(fn, x) { return fn(x); }
apply(std.math.sqrt, 16)
```

`std.iter.for_each(iterable, fn)` 逐个调用 `fn`，忽略返回值并返回 `null`。回调可以写 1 个或 2 个参数：1 个参数时接收元素（dict 为 key）；2 个参数时 list / string / bytes / set / deque / heap / generator 接收 `(index, value)`，dict 接收 `(key, value)`。回调里的 `break` / `continue` 不可用，`return` 只退出回调本身；返回 task 的回调不会被自动等待。

函数调用会先按从左到右的顺序求值参数，再检查被调用者类型和参数个数，因此参数中的副作用总会执行。

参数表达式中的 `return` / `break` / `continue` 会向外传播，当前函数调用不会执行。

极深值有保护上限：`print` / `str` 超过 100 层输出 `...`，`value_equals` 超过 512 层返回 `false`，`freeval` 超过 1000 层停止释放，JSON 深度上限 512。

字符串支持常用转义：`\n`、`\t`、`\r`、`\\`、`\"`、`\'`。


### 语法约定

- 顶层可以直接写表达式，不需要分号。
- 块 `{ ... }` 内的普通表达式语句需要以 `;` 结尾。
- `[]` 和 `{}` 分别表示空列表和空字典；空字典可以跨行书写，也可以包含注释。
- `{}` 出现在控制流/函数体位置时仍表示空代码块，例如 `if (x) {}`、`func f() {}`、`while (x) {}`。
- `-> {}` 是空块体 lambda（返回 `null`）；`->` 后的第一个 `{` 固定按块体解析，因此要返回字典字面量时写 `({...})`，例如 `x -> ({ "v": x })`。
- 块内单独出现的 `{}` 按空字典表达式处理；需要空代码块语句时写 `{ ; }`。
- 空字典和空代码块都可以跨行书写，也可以包含注释。
- 比较运算符包括 `>`、`<`、`<>`/`!=`、`==`、`>=`、`<=`。
- `//` 开始单行注释。
- 花括号、方括号、圆括号内部的换行会被自动忽略，支持多行表达式。
- 控制流和声明体的 `{` 必须和头部在同一行；`else` 必须和前面的 `}` 在同一行；`do ... while` 的 `while` 必须和 `}` 在同一行。
- 闭包捕获定义处的词法作用域；闭包环境会在程序退出时统一清理。
- 用户函数调用深度上限为 1000，超出时报告 `maximum recursion depth`。


### 类型与转换

- `int` 是 64 位有符号整数；十进制整数字面量超出 64 位范围时按 `float` 解析，`0x` / `0b` / `0o` 字面量超出 `int64` 范围时报词法错误，整数运算溢出会报 `integer overflow`。
- 整数字面量支持 `0x` / `0X`、`0b` / `0B`、`0o` / `0O` 进制前缀，浮点字面量支持科学计数法（如 `1e3`、`1.5e-2`）；数字之间可以用 `_` 分隔，例如 `1_000_000`、`0xFF_FF`、`1.5e1_0`。下划线只能位于两个数字之间。
- `int` 与 `float` 混合运算提升为 `float`，例如 `1 + 2.5` 得到 `3.5`；`1 == 1.0` 为真。超过 2^53 的整数与 float 比较仍按数学值精确判断，但转成 `float`（混合运算或 `float()`）时会丢失精度。
- 整数除法向零截断，取模结果符号随被除数：`-7 / 4 == -1`，`-7 % 4 == -3`，`7 % -4 == 3`。
- 位运算按 64 位进行，移位次数必须在 `0..63` 之间。
- `bool` 类型有 `true` / `false` 两个字面量；`bool(x)` 按真值规则转换。
- 真值规则：`null`、`false`、`0`、`0.0`、空字符串、空 bytes、空列表、空字典、空 deque、空 heap、空 set 为假；其余值（包括 `true`、函数和内置函数）为真。
- `str(true)` 得到 `"true"`，`int(true)` 得到 `1`，`float(false)` 得到 `0.0`。
- `null` 是空值字面量，表示“没有值”：没有 `return` 的用户函数、`print` / `println` 和 `return;` 都返回 `null`。顶层 `null` 结果不会显示，但可以赋值、比较、传递，或通过 `print(null)` / `std.string.format("{}", null)` 输出。
- `&&`、`||`、`!` 返回 `bool`，并短路求值。
- `==` / `!=` 对所有类型有效，返回 `bool`；大小比较只支持数值之间或字符串之间，也返回 `bool`。
- 列表和字符串下标必须是 `int`，`1.0` 不会被隐式截断；字符串下标按 UTF-8 码点计数，`"你好"[0]` 得到 `你`。
- `len(x)` 支持字符串、char、bytes、list、dict、deque、heap、set：字符串按 UTF-8 码点计数，char 为 1，bytes 按字节数，其余按元素数。
- `range(length)` / `range(start, length[, step])` 生成整数序列，第二参数是长度而不是结束值；`step` 不能为 0。
- 字典键按值相等判断，`1` 和 `1.0` 是同一个键；字面量中的重复键以最后一个为准。
- `list`、`dict` 和 `struct` 是值类型：赋值、传参和返回在语义上复制，但内部使用写时复制（COW），只读或复制时共享底层数据，真正修改时才分离。
- `method` 是绑定到结构体实例的方法值；当前版本捕获提取时的实例快照。
- `bytes` 和字符串目前仍然复制内容；函数值共享闭包环境，文件句柄等原生资源共享底层句柄。
- 显式转换：`int(x)`、`float(x)`、`bool(x)`、`str(x)`。`int` / `float` 接受整数、浮点数、布尔值或字符串；`bool` 按真值规则转换；`str` 接受任意值。
- 浮点数输出使用可往返的最短表示并保留 `.0`：`1.0` 显示为 `1.0`，`1.23456` 显示为 `1.23456`。
- 没有字符串与数字之间的隐式转换，`1 + "2"` 会报错；需要时请使用 `int()` / `float()` / `str()`。


### 对象生命周期

- 普通值由引用计数管理；`list` / `dict` / `struct` / `bytes` 的 payload 会被别名共享，写入时才分离；`string` / `bytes` 使用引用计数缓冲；
- 函数值复制时共享函数代码和闭包环境，文件句柄等原生资源共享底层句柄；
- 闭包按引用捕获定义处的作用域，因此局部变量会在闭包存活期间继续存在；
- 运行时使用**作用域粒度的标记式环回收器**（`gc.cpp`）：从全局作用域、当前作用域链和恢复栈出发，标记可达 scope，再清理不可达 scope 的符号表；
- 回收每 64 个顶层语句或 scope 数量明显增长时执行，只在顶层语句边界进行；
- 值遍历覆盖 `list` / `dict` / `struct` / `enum` / `result` / `method`，所以闭包即使只被 enum、LRU（struct）或 result 持有，也不会被误回收；
- 这不是完整的逐值 tracing GC：值本身仍由引用计数管理，没有压缩、移动或 finalizer；
- 如果未来原生资源也能保存语言值，需要扩展回收器的遍历规则。


### 格式化

`std.string.format(fmt, ...)` 返回格式化后的字符串（需要 `import std;`）；`print` 支持两种参数形式：

- `print(x)`：直接打印一个值；
- `print(fmt, ...)`：第一个参数是格式串，其余参数用于格式化；
- `print()`：输出一个空行。

示例：

```text
import std;

std.string.format("x = {}", 3)
std.string.format("{} + {} = {}", 1, 2, 3)
std.string.format("{1} {0}", "a", "b")
std.string.format("{{}} = {}", 1)
print("x = {:.2f}", 1.5)
```

支持的格式：

| 写法 | 含义 |
| --- | --- |
| `{}` / `{0}` | 默认字符串化 |
| `{:s}` | 字符串 |
| `{:d}` | 整数 |
| `{:f}` / `{:.2f}` | 浮点，默认 6 位小数 |
| `{:x}` / `{:X}` | 十六进制整数 |
| `{:10}` / `{:<10}` / `{:>10}` / `{:^10}` | 宽度和对齐 |
| `{:*^10}` | 自定义填充字符 |
| `{:08}` | 零填充 |
| `{{` / `}}` | 字面量 `{` / `}` |

自动编号 `{}` 和显式编号 `{0}` 不能混用；参数不足会报错，多余参数会被忽略。注意 `print("x", 1)` 按格式模式处理，输出 `x` 并忽略多余的 `1`。


### 错误信息

错误输出统一包含位置、类别和源码片段：

```text
3:1: runtime error: division by zero
  1/0;
  ^
```

- 行号和列号从 `1` 开始；
- 列号按终端显示宽度计算，中文、日文、韩文等宽字符占两列；
- 类别分为 `lexical error`、`syntax error`、`runtime error`；
- 运行时错误指向实际出错的 AST 节点，而不是语句结束位置；
- 源码行下的 `^` 指向出错列；
- 源码必须是合法 UTF-8，非法 UTF-8 字节会报 `invalid UTF-8 byte 0x..`；
- 词法错误会显示字符和十六进制字节，例如 `unexpected character '`' (0x60)`；非法 Unicode 字符使用 `U+....` 表示；
- 语法错误会列出出错 token 和部分期望 token：

```text
1:9: syntax error: unexpected ';', expecting float, integer, identifier, ...
  let x = ;
          ^
```

函数调用中的运行时错误会附带调用栈：

```text
2:10: runtime error: division by zero
    return 10 / 0;
           ^
  in g() called from line 5
  in f() called from line 7
```

调用栈最多显示最近的 8 层。

#### 错误恢复

- 运行时错误中止当前语句/函数，不回滚已经发生的副作用；
- 显式 `fail`、运行时错误和 `result.err` 可以被 `on error` 捕获；未捕获的 `fail` 在脚本顶层报错并停止；
- `on error` 对 `result.ok(v)` 解包为 `v`；
- 除零、越界、网络和 FFI 错误会转换成 `error`，并可由 `on error` 或 `std.result.capture` 捕获；
- 语法错误、内部不变量错误和控制流误用不会转换成 `result`；
- 函数出错时不返回部分结果；
- `let` 初始化失败不会创建绑定；普通赋值失败会保留旧值；
- 给 `const` 绑定重新赋值会报 `cannot assign to constant`，但修改其容器内容仍然允许；
- 脚本模式默认在第一条错误后停止；
- `-k` / `--keep-going` 让脚本继续执行下一条顶层语句，但最终退出码仍为 `1`；
- REPL 在错误后回到提示符，正常 EOF 退出码为 `0`；
- 语法错误和词法错误按行恢复；
- 同一行最多报告一条词法错误，并抑制由此产生的重复语法错误；
- 包加载失败是原子失败：`import` 不会绑定部分命名空间；
- `input()` 的 EOF 返回 `null`，不是错误；
- `std.process.command()` 的退出状态不是解释器错误，需要状态时使用 `std.process.command_status()`。

#### 退出码

| 情况 | 退出码 |
|---|---:|
| 正常结束 | `0` |
| REPL 正常 EOF | `0` |
| 脚本语言错误（词法/语法/运行时/包） | `1` |
| 命令行参数错误 | `2` |
| 脚本文件无法打开 | `2` |
| 源文件编码声明冲突或转码失败 | `2` |
| 脚本被 Ctrl-C 中断 | `130` |

外部命令的退出状态通过 `std.process.command_status()` / `std.process.run_command()` 返回，不会直接成为解释器退出码。

## 第三部分：常用标准库

目标：按任务查找字符串、文件、编码、正则、时间和哈希等常用 API。

### 字符串处理

```text
import std;

std.string.trim(text)                     → 去掉首尾空白
std.string.split(text[, separator])       → list<string>
std.string.split_ws(text)                 → list<string>（同 split(text)）
std.string.find(text, substring)          → 码点下标 / -1
std.string.substr(text, start[, length])  → string
```

示例：

```text
import std;

std.string.trim("  hello\t\n")          → "hello"
std.string.split("a,b,c", ",")          → ["a", "b", "c"]
std.string.split(",a,", ",")            → ["", "a", ""]
std.string.split("  a  b\tc ")          → ["a", "b", "c"]
std.string.find("hello", "ll")          → 2
std.string.find("hello", "x")           → -1
std.string.substr("hello", 1, 3)        → "ell"
std.string.substr("你好世界", 1, 2)      → "好世"
```

规则：

- `std.string.trim` 和 `std.string.split(text)` 只把 ASCII 空白（空格、Tab、换行等）视为空白；
- `std.string.split(text)` 按连续空白切分，并丢弃首尾空字段；
- `std.string.split(text, separator)` 按精确分隔符切分，保留首尾空字段；
- `separator` 不能为空字符串；
- `std.string.split("")` 返回 `[]`，`std.string.split("", ",")` 返回 `[""]`；
- `std.string.split_ws(text)` 是 `split(text)` 的兼容别名；
- `std.string.find` 返回 **UTF-8 码点下标**，找不到返回 `-1`，空子串返回 `0`；
- `std.string.substr` 的 `start` 和 `length` 都是码点单位；`start` 超出长度返回空字符串，`length` 超出末尾会截断；
- `start` 和 `length` 不能为负数。

组合使用：

```text
import std;

let parts = std.string.split(input("请输入两个数: "));
let a = int(parts[0]);
let b = int(parts[1]);
print(a + b);
```

#### 构建器

频繁追加时使用 builder，避免每次复制整个 list / string：

```text
import std;

let lb = std.list.builder();
for (let i in range(1000)) {
  lb.push(i);
}
let xs = lb.build();

let sb = std.string.builder();
for (let i in range(1000)) {
  sb.push("x");
}
let text = sb.build();
```

规则：

- `push` / `extend` 是 O(1) 摊销，`build()` 每次返回独立值；
- `std.list.append(xs, v)` 在 `xs = std.list.append(xs, v)` 形式下（VM/JIT 后端）使用 COW 原地增长；
- `s = s + "x"` / `s = s + 'c'` 在 VM 与 AST 下使用 COW 原地追加，别名仍保留旧值；
- 复杂拼接（例如 `s + str(i)`）仍建议使用 `std.string.builder`；
- 其他调用形式仍保持“返回新 list / 新字符串，不修改参数”的语义；
- builder 是可变对象，赋值会共享同一个 builder。


### 控制台输入

`input([prompt])` 从 stdin 读取一行：

```text
let name = input("Name: ");
print("hello " + name);
```

- 返回的字符串不包含末尾换行；
- 可选的 `prompt` 必须是字符串，会原样输出到 stdout，不经过 `std.string.format`；
- 读到 EOF 且没有剩余内容时返回 `null`；最后一行没有换行符时仍返回该行；
- 空行返回空字符串；
- CRLF 会被处理，`\r` 不会进入结果；
- 控制台负责行编辑和退格；管道输入中的退格会由 `input` 过滤；
- 脚本模式下仍然从原始 stdin 读取，不会读脚本文件本身；
- `input` 是普通一等内置函数，可以保存和传递；`std.io.input` 是同一个函数。

```text
let read = input;
read("> ")
```

读取多个值：

```text
import std;

let parts = std.string.split(input("x y: "));
let x = int(parts[0]);
let y = int(parts[1]);
println(x + y);
```

示例：

```text
import std;

let name = input("Name: ");
print(std.string.format("hello {}", name));
```


### 文件读写

```text
std.fs.read_file(path)          → 文件全部内容
std.fs.write_file(path, text)   → null，覆盖写入
std.fs.append_file(path, text)  → null，追加写入
std.fs.exists(path)             → 1 / 0
```

需要 `import std;`。

示例：

```text
import std;

std.fs.write_file("out.txt", "hello\n");
std.fs.append_file("out.txt", "world\n");
print(std.fs.read_file("out.txt"));

std.fs.exists("out.txt")
```

规则：

- `std.fs.read_file` 读取整个文件并返回字符串；空文件返回空字符串；
- `std.fs.write_file` 创建或截断文件；
- `std.fs.append_file` 创建或追加；
- 文件以二进制模式打开，不会自动转换 CRLF，也不会去掉末尾换行；
- 读写内容按 UTF-8 文本处理；
- `std.fs.read_file` 遇到 NUL 字节会报错，避免内部 C 字符串截断；读取二进制请使用 `std.fs.read_bytes`；
- 不会自动创建父目录；
- 文件操作失败时报告 `read_file: ...` / `write_file: ...` / `append_file: ...`；
- Windows/MinGW 下文件路径会转换为 UTF-16 后使用 `_wfopen`。

#### 带编码的文本文件

需要在 UTF-8 之外的文本编码之间读写时，使用：

```text
std.fs.read_text(path, encoding[, errors])       → string
std.fs.write_text(path, text, encoding[, errors]) → null
```

`encoding` 使用 `std.encoding` 支持的编码名，`errors` 支持 `strict`（默认）、
`replace`、`ignore`。

示例：

```text
import std;

std.fs.write_text("out-gbk.txt", "中国", "gbk");
std.fs.read_text("out-gbk.txt", "gbk")          // 中国

std.fs.write_text("out-utf16.txt", "中", "utf-16");
std.fs.read_text("out-utf16.txt", "utf-16")     // 中
```

规则：

- `read_text` 先读取字节，再按指定编码解码为 UTF-8 `string`；
- `write_text` 先把 `string` 编码成字节，再覆盖写入文件；
- `errors = "replace"` 时，解码用 `U+FFFD` 替换，编码用 `?` 替换；
- `errors = "ignore"` 时跳过非法序列或不可表示字符；
- 解码结果包含 `U+0000` 时报错；
- `utf-16` / `utf-32` 会按 BOM 规则处理；显式字节序版本不自动剥离 BOM；
- `utf-8` 不自动剥离 UTF-8 BOM，BOM 会作为 `U+FEFF` 保留；
- `std.fs.read_file` / `std.fs.write_file` / `std.fs.read` / `std.fs.write` 仍然只按 UTF-8 字节处理；
- 文件句柄 API 不携带编码，需要其它编码时使用 `std.fs.read_bytes` / `std.fs.write_bytes` 或 `std.fs.read_text` / `write_text`。

#### 文件句柄

需要多次操作同一个文件时，可以使用低层文件句柄：

```text
std.fs.open(path, mode)              → file handle
std.fs.read(handle[, n])             → string
std.fs.write(handle, text)           → null
std.fs.seek(handle, offset[, whence]) → null
std.fs.tell(handle)                  → int
std.fs.eof(handle)                   → 1 / 0
std.fs.close(handle)                 → null
std.fs.flush(handle)                 → null
```

`mode` 支持 `r`、`w`、`a`、`r+`、`w+`、`a+`，也接受带 `b`/`t` 的写法；内部统一按二进制模式打开。

示例：

```text
import std;

let h = std.fs.open("data.txt", "w+");
std.fs.write(h, "hello world");
std.fs.seek(h, 0);
std.fs.read(h, 5)
std.fs.close(h)
```

- 文件句柄是不可打印底层指针的资源对象，打印显示为 `<file: path>`；
- 句柄复制会共享同一个底层文件，关闭其中一个会影响所有副本；
- `std.fs.read(h)` 读取剩余全部内容，`std.fs.read(h, n)` 最多读取 n 字节；`n = 0` 等价于读取全部；
- `std.fs.seek` 的 `whence`：`0` 文件开头、`1` 当前位置、`2` 文件末尾；
- 关闭后继续读写会报 `file is closed`；
- `std.fs.read` 读取到 NUL 字节会报错；读取二进制请使用 `std.fs.file_read_bytes`；
- 最终未关闭的句柄会随进程退出关闭。


### 用 dict + 闭包封装文件对象


当前语言没有方法绑定，但可以用 dict + 闭包实现类似接口：

```text
func open_file(path) {
  let h = std.fs.open(path, "r");

  func read(n) { return std.fs.read(h, n); }
  func seek(n) { return std.fs.seek(h, n); }
  func close() { return std.fs.close(h); }

  return {"read": read, "seek": seek, "close": close};
}

let f = open_file("data.txt");
f["read"](10)
f["close"]()
```

底层文件资源使用通用的原生 userdata 生命周期管理；上层对象接口可以用语言本身扩展，不需要修改内置实现。

文件函数可以访问进程有权限的任意路径，只应在可信脚本中使用。


### 字符、字节与编码


`char` 表示一个 Unicode 标量值，使用单引号字面量：

```text
'a'
'你'
'\n'
'\u0041'
'\u{1F600}'
```

- `len('a') == 1`；
- 字符串下标和 `for ... in` 字符串返回 `char`；
- `int('A') == 65`，`str('A') == "A"`，`char(65) == 'A'`；
- `char` 可与字符串拼接：`'a' + "bc"`；
- `std.bytes.from_char(c)` / `std.bytes.to_char(b)` / `char(b)` 做 UTF-8 字节互转；
- JSON 编码为单字符字符串；
- 空、多字符、代理区、超过 `0x10FFFF` 报错。

#### 字节值 byte

`byte` 表示 `0..255` 的字节值，没有字面量：

```text
byte(0)
byte(65)
byte(255)
```

- `int(byte)` 取回 `0..255`；
- `byte(int)` 超出范围报错；
- `byte` 可以作 dict key、`switch` 值；
- JSON 编码为整数；
- `bytes[i]` / `std.bytes.get` 仍返回 `int`。

#### 二进制 buffer

文本字符串按 UTF-8 处理，不能保存任意字节。二进制数据使用独立的 `bytes` 类型：

```text
std.bytes.from_string(text)        → bytes
std.bytes.to_string(bytes)         → string
std.bytes.len(bytes)               → int
std.bytes.get(bytes, index)        → 0..255
std.bytes.slice(bytes, start[, length]) → bytes
std.bytes.concat(a, b)             → bytes
std.bytes.hex(bytes)               → 十六进制字符串
std.bytes.from_hex(text)            → bytes
std.bytes.to_int(bytes, size, byteorder[, offset[, signed]]) → int
std.bytes.from_int(value, size, byteorder) → bytes
std.fs.read_bytes(path)            → bytes
std.fs.write_bytes(path, bytes)    → null，覆盖写入
std.fs.append_bytes(path, bytes)   → null，追加写入
std.fs.file_read_bytes(handle[, n]) → bytes
std.fs.file_write_bytes(handle, bytes) → null
bytes[index]                       → 0..255
bytes[index] = 0..255
```

需要 `import std;`。

示例：

```text
import std;

let b = std.bytes.from_hex("00ff10");
std.bytes.len(b)                     → 3
std.bytes.hex(b)                     → "00ff10"
std.bytes.get(b, 1)                  → 255
std.bytes.hex(std.bytes.slice(b, 1, 2)) → "ff10"

let c = std.bytes.from_string("abc");
c[1] = 255;
std.bytes.hex(c)                     → "61ff63"

std.bytes.to_int(std.bytes.from_hex("78563412"), 4, "little")
                                     → 305419896
std.bytes.to_int(std.bytes.from_hex("78563412"), 4, "big")
                                     → 2018915346
std.bytes.to_int(std.bytes.from_hex("ffff"), 2, "little", 0, 1)
                                     → -1
std.bytes.hex(std.bytes.from_int(305419896, 4, "little"))
                                     → "78563412"
std.bytes.hex(std.bytes.from_int(-1, 2, "little"))
                                     → "ffff"
```

规则：

- `bytes` 可以保存任意字节，包括 `0x00`，长度由字节数决定；
- 直接打印 `bytes` 时显示为 `<bytes: N>`，不会把原始二进制写到终端；需要查看内容时使用 `std.bytes.hex`；
- `std.bytes.len`、`std.bytes.get`、`std.bytes.slice`、索引和 `std.fs.seek` 都使用**字节下标**，不是 UTF-8 码点；
- 索引读取返回 `0..255` 的整数，索引赋值只接受 `0..255`；
- `std.bytes.slice` 的 `start` / `length` 不能为负数，越界会截断；
- `std.bytes.from_hex` 只接受偶数个十六进制数字，大小写均可；
- `std.bytes.to_int` 的 `size` 为 1..8，`byteorder` 支持 `"little"` / `"big"`（也接受 `"le"` / `"be"`）；
- `std.bytes.to_int` 的 `offset` 默认 0，`signed` 默认 0；`offset + size` 超出 buffer 会报错，不会静默截断；
- `std.bytes.to_int(..., signed = 1)` 按二补码解释；无符号 64 位值超过 `int64` 上限时会报错；
- `std.bytes.from_int` 的 `size` 为 1..8；值可以落在有符号下限到无符号上限之间，按低 `size` 字节编码；
- 负数写入使用二补码，例如 `std.bytes.from_int(-1, 2, "little")` 得到 `ff ff`；
- `std.bytes.to_string` 要求内容是合法 UTF-8 且不含 NUL，否则报错；
- `std.fs.read_bytes` / `std.fs.write_bytes` / `std.fs.append_bytes` 不做文本 NUL 检查；
- `std.fs.file_read_bytes(h)` 读取剩余全部内容，`std.fs.file_read_bytes(h, n)` 最多读取 n 字节；
- `bytes` 是值类型，赋值和复制都会复制内容。

#### 编码转换

设计说明见 [encoding.md](encoding.md)。

`std.encoding` 在 `bytes` 和 UTF-8 `string` 之间做显式转码：

```text
std.encoding.decode(bytes, encoding[, errors])  → string
std.encoding.encode(string, encoding[, errors]) → bytes
std.encoding.supports(encoding)                 → bool
```

支持的编码：

```text
utf-8  ascii  latin-1  windows-1252
utf-16le  utf-16be  utf-16
utf-32le  utf-32be  utf-32
gbk / cp936
gb18030
```

`errors` 支持：

```text
"strict"   默认，非法序列或不可表示字符报错
"replace"  decode 用 U+FFFD 替换；encode 用 "?" 替换
"ignore"   跳过非法序列或不可表示字符
```

示例：

```text
import std;

let s = std.encoding.decode(std.bytes.from_hex("d6d0b9fa"), "gbk");
print(s);                                      // 中国

print(std.bytes.hex(std.encoding.encode("中国", "gbk")));
                                               // d6d0b9fa

print(std.bytes.hex(std.encoding.encode("€", "gbk")));
                                               // 80

print(std.bytes.hex(std.encoding.encode("€", "gb18030")));
                                               // a2e3

std.encoding.supports("gbk")                   // true
std.encoding.supports("gb18030")               // true
```

规则：

- GBK 和 GB18030 使用内置码表，不依赖 iconv 或 Windows 代码页；
- `gbk` 的 `0x80` 解码为 `€`；`gb18030` 的 `€` 是 `A2 E3`；
- `std.encoding` 的 `utf-16` / `utf-32` 要求 BOM，显式字节序版本不自动处理 BOM；
- `utf-16` / `utf-32` 编码固定输出 BOM + 小端；
- 解码结果包含 `U+0000` 时始终报错，因为 `string` 不能保存内嵌 NUL；
- `std.bytes.to_string` 仍然是严格 UTF-8 的快捷方式，不参与 GBK / GB18030 转换。

#### Unicode 归一化

设计说明见 [unicode.md](unicode.md)。

`std.unicode` 提供显式的 NFC / NFD / NFKC / NFKD 归一化：

```text
std.unicode.normalize(text, form)      → string
std.unicode.is_normalized(text, form)  → bool
std.unicode.version()                  → string
```

`form` 支持 `nfc`、`nfd`、`nfkc`、`nfkd`，大小写不敏感。底层使用 vendored
`utf8proc` 2.10.0，Unicode 版本为 16.0.0。

示例：

```text
import std;

let decomposed = std.encoding.decode(std.bytes.from_hex("65cc81"), "utf-8");
let composed = std.unicode.normalize(decomposed, "nfc");
std.bytes.hex(std.encoding.encode(composed, "utf-8"))  // c3a9

std.unicode.is_normalized(decomposed, "nfc")       // false
std.unicode.is_normalized(composed, "nfc")         // true

let circled = std.encoding.decode(std.bytes.from_hex("e291a0"), "utf-8");
std.unicode.normalize(circled, "nfkc")             // "1"
std.unicode.version()                              // "16.0.0"
```

规则：

- 归一化只在显式调用 `std.unicode.normalize` 时发生；
- 字符串比较、拼接、下标和 `len` 不会自动归一化；
- 标识符按 NFC 归一化，不做 NFKC；
- 输入必须是合法 UTF-8 `string`，输出也保持合法 UTF-8；
- 归一化不会产生内嵌 NUL。

#### 大小写转换与大小写折叠

```text
std.string.to_lower(text)              → string
std.string.to_upper(text)              → string
std.unicode.casefold(text)             → string
```

`to_lower` / `to_upper` 使用 Unicode **默认完整大小写映射**（与 locale 无关），
用于展示；`casefold` 是**完整大小写折叠**，用于大小写不敏感比较。

```text
import std;

std.string.to_lower("ABC")        // "abc"
std.string.to_upper("ß")          // "SS"
std.string.to_upper("Straße")     // "STRASSE"
std.string.to_upper("ﬁ")          // "FI"
std.string.to_lower("İ")          // "i" + U+0307
std.string.to_lower("ΟΣ")         // "ος"（词尾 ς）
std.string.to_lower("ΟΣΑ")        // "οσα"

std.unicode.casefold("Straße")    // "strasse"
std.unicode.casefold("STRASSE")   // "strasse"
std.unicode.casefold("ς")         // "σ"
```

规则：

- 完整映射允许 `1 → N`，输出长度可能与输入不同；
- 映射是**有损、不可逆**的：`to_upper("ß") == to_upper("ss")`，
  且 `to_lower(to_upper("ß")) == "ss"` 而不是 `"ß"`；
- 不要用它们做大小写不敏感比较：`"Straße"` 与 `"STRASSE"` 转换后仍不相等；
- 比较应先 NFC，再 `casefold`，再比较；
- `casefold` 的结果同样可能变长，仅用于比较和查找，不要直接展示；
- 两者都拒绝非法 UTF-8，且都不会顺带做归一化。


### 正则表达式


```text
std.regex.match(pattern, text[, flags])       → bool（整串匹配）
std.regex.search(pattern, text[, flags])      → dict / null
std.regex.find_all(pattern, text[, flags[, limit]]) → list
std.regex.replace(pattern, text, replacement[, flags]) → string
std.regex.split(pattern, text[, flags])       → list
```

示例：

```text
import std;

std.regex.match("a+", "aaa")                  // true
std.regex.match("abc", "ABC", "i")            // true
std.regex.match(".", "你", "u")               // true
std.regex.match("\\p{L}+", "中文", "u")        // true

let m = std.regex.search("(a+)(b)", "xxaaabyy");
m["text"]                                     // aaab
m["start"]                                    // 2
m["groups"]                                   // [aaa, b]

let u = std.regex.search("(\\p{L}+)", "123中文456", "u");
u["text"]                                     // 中文
u["start"]                                    // 3（码点偏移）
u["end"]                                      // 5（码点偏移）

std.regex.replace("(a+)", "xxaaabyy", "[$1]") // xx[aaa]byy
std.regex.split(",", "a,b,c")                  // [a, b, c]
```

规则：

- 使用 PCRE2 正则语法，兼容常用 ECMAScript 写法；Unicode 码点转义使用 `\x{...}`（如 `\x{4f60}`），不是 `\uXXXX`；
- `flags` 支持 `i`（忽略大小写）、`m`（多行）和 `u`（Unicode 模式）；
- 不带 `u` 时按 UTF-8 字节匹配，`search` / `find_all` 的 `start` / `end` 是字节偏移；
- 带 `u` 时按 Unicode 码点匹配，支持 `\p{...}`、Unicode 字符类和 Unicode 大小写折叠，`start` / `end` 是码点偏移；
- `u` 模式要求模式和文本是合法 UTF-8；
- `replace` 使用 `$1` / `${1}` 形式引用捕获组；
- 非法正则会报运行时错误。


### 时间、随机数和 UUID


```text
std.time.now()                    → Unix 秒
std.time.now_ms()                 → Unix 毫秒
std.time.monotonic_ms()           → 进程内单调毫秒
std.time.utc_offset()             → 本地时区偏移秒数
std.time.format(ts[, format[, utc]]) → string
std.time.parse(text[, format[, utc]]) → int
std.time.sleep(ms)                → null

std.random.seed(n)
std.random.int(min, max)          → 闭区间整数
std.random.float()                → [0, 1)
std.random.bytes(n)               → bytes
std.random.choice(list)
std.random.shuffle(list)          → 新 list
std.random.uuid()                 → UUID v4 字符串
```

默认时间格式是 `%Y-%m-%d %H:%M:%S`，默认使用本地时间；第三个参数传 `true`
使用 UTC。


### Base64、URL 和哈希


```text
std.encoding.base64_encode(bytes) → string
std.encoding.base64_decode(string) → bytes
std.encoding.url_encode(string) → string
std.encoding.url_decode(string) → string

std.crypto.sha256(data)            → 小写 hex
std.crypto.hmac_sha256(key, data)  → 小写 hex
```

`sha256` / `hmac_sha256` 接受 `string` 或 `bytes`。`url_decode` 会把 `+`
当作空格，并校验解码结果是合法 UTF-8。


### 目录、路径和压缩


```text
std.fs.listdir(path)              → list[string]
std.fs.mkdir(path[, recursive])
std.fs.remove(path)
std.fs.rename(old, new)
std.fs.stat(path)                 → dict
std.fs.cwd()
std.fs.chdir(path)

std.fs.path_join(parts...)
std.fs.path_basename(path)
std.fs.path_dirname(path)
std.fs.path_extname(path)
std.fs.path_abs(path)
std.fs.path_normalize(path)
std.fs.temp_dir([prefix])         → string
std.fs.remove_all(path)
std.fs.copy(src, dst)
std.fs.which(name)                → string
std.fs.glob(pattern)              → list[string]
```

- `std.fs.temp_dir` 在系统临时目录下创建唯一目录；`prefix` 不能包含路径分隔符；
- `std.fs.remove_all` 递归删除，路径不存在时视为成功；
- `std.fs.copy` 支持文件或目录：目录目标不能已存在；文件目标已有内容时覆盖；
  如果目标是目录，则复制到 `dst/src_name`；
- `std.fs.which` 按当前线程的 `PATH` 查找可执行文件，Windows 会尝试 `PATHEXT`；
  找不到返回空字符串；
- `std.fs.glob` 只支持最后一段路径中的 `*` / `?`，返回排序后的绝对路径；
  `std.fs.mkdir(path, true)` 可递归创建目录。

`std.fs.cwd` / `std.fs.chdir` 是线程局部工作目录：相对路径的 `std.fs` 操作、`std.fs.open` / 文件读写、`std.process.spawn_command` / `std.process.run_command` / `std.process.command` 默认使用当前线程的工作目录；不会改变进程级 OS cwd。新线程在启动时继承创建线程的工作目录。

std.compress.gzip(bytes)          → bytes
std.compress.gunzip(bytes)        → bytes
std.compress.zlib(bytes)          → bytes
std.compress.unzlib(bytes)        → bytes
std.compress.deflate(bytes)       → bytes
std.compress.inflate(bytes)       → bytes
```

`std.fs.stat` 返回：

```text
{"type": "file" / "dir" / "other", "size": int, "mtime": int}
```

压缩函数需要构建时启用 zlib；未启用时调用会报告 `zlib support is disabled`。

二进制文件读写：

```text
let h = std.fs.open("data.bin", "r+b");
std.fs.seek(h, 8);
std.fs.file_write_bytes(h, std.bytes.from_hex("00ff"));
std.fs.seek(h, 0);
std.bytes.hex(std.fs.file_read_bytes(h, 16))
std.fs.close(h)
```

读取固定宽度整数：

```text
let h = std.fs.open("data.bin", "rb");
let magic = std.bytes.to_int(std.fs.file_read_bytes(h, 4), 4, "big");
let length = std.bytes.to_int(std.fs.file_read_bytes(h, 4), 4, "little");
std.fs.close(h);
```


## 第四部分：并发与进程

目标：理解 fiber/task、线程 Channel、环境变量和外部命令的边界与用法。

### launch、await 与 `wait!`

`wait` 的规范写法是内置宏 `wait!`，但旧 `wait` 形式仍暂时兼容；`launch` /
`wait` / `await` 都是表达式起始 contextual keyword，不是全局保留字。

`launch f()` 创建新 task 并立即返回 `task[T]`；`wait!(t)` 等价于 `t.wait()`；
`await f()` 等价于 `wait launch f()`，启动调用并立即等待：

```skein
import std;

func work(n) {
  std.task.sleep(10).wait();
  return n * 2;
}

let t = launch work(21);
let value = wait!(t);       // 等价于 t.wait()，也可写 wait! t
let same = await work(21);  // 启动后立即等待
print(value + same);
```

规则：

- `launch` / `wait` / `await` 在表达式起始位置识别：语句开头，`=`、`return`、
  `yield`、`(`、`[`、`{`、`,`、`:` 和二元运算符之后；`.` 之后仍是普通名字。例如：

  ```skein
  d = await f();
  int(await f());
  (await f()).xxx;
  ```

- `await` 要求后面是调用表达式；已有 task 写 `wait!(t)`，`await t` 报错；
- `await(f())` 是前缀形式；调用同名变量写 `(await)(f())`；
- `launch` 与 `await` 作用于其后的完整调用链，且 postfix 优先级更高：
  `launch make().get()` 启动 `make().get()`，`await f().xxx` 是
  `await (f().xxx)`；先 await 再取字段写 `(await f()).xxx`；
- `await f() on error(e) { ... }` 与 `wait launch f() on error(...)` 等价，
  handler 覆盖 task 内失败；`launch f() on error(...)` 仍只覆盖启动调用并告警；
- `wait!` 不接受超时参数；需要超时请写 `t.wait(ms)` 或
  `std.task.wait(t, ms)`；
- `t.try_wait()` 返回 `result[T, error]`，可在不抛出的情况下检查结果。

### 线程、Channel 与并发


```text
std.thread.Channel(capacity) -> (parent_endpoint, child_endpoint)
std.thread.Channel.unbounded() -> (parent_endpoint, child_endpoint)
std.thread.Message.from(payload[, handles]) -> message

endpoint.send(message)
endpoint.try_send(message) -> bool
endpoint.recv([timeout_ms]) -> message | null
endpoint.try_recv() -> {"ok": bool, "closed": bool, "value": message?}
endpoint.close_send()
endpoint.close_recv()
endpoint.close()

endpoint.child / endpoint.peer -> endpoint
endpoint.closed
endpoint.send_closed
endpoint.recv_closed
endpoint.capacity
endpoint.length
endpoint.waiters
endpoint.send_waiters
endpoint.recv_waiters
endpoint.total_sends
endpoint.total_recvs
endpoint.send_timeouts
endpoint.recv_timeouts
endpoint.stats -> dict
```

- `Channel` 返回一对有方向端点；每条 lane 独立：`parent.send` 发往 `child.recv`，
  `child.send` 发往 `parent.recv`；
- `capacity >= 1` 为有界队列；`capacity == 0` 为 rendezvous；`capacity == -1`
  为无界队列；`Channel.unbounded()` 等价于 `Channel(-1)`；
  旧名 `Channel.bounded(n)` / `Channel.new(n)` / `unbounded_channel()` 已移除；
- Channel 只接收 `std.thread.Message`；
- `Message.from` 共享 payload（COW）；发送方后续写入会分离，不影响消息；
- payload 支持标量、`error` / `result`、`list` / `dict`、`lru` / `deque` / `set`、`struct` / `enum`；`heap` 因比较器函数不能跨线程，暂不支持；LRU 现在是普通 struct，函数 / heap 等特殊值不能嵌套在 LRU 字段中传输；
- `handles` 是字符串到线程 handle 的 dict，目前支持 `thread_endpoint`；
- `send` 写入本端 out lane，队列满时阻塞；rendezvous（capacity 0）下阻塞到对端接收者等待；
  `try_send` 不阻塞，无空间/无接收者时返回 `false`；
- `recv` 读取本端 in lane，队列空时阻塞；关闭且排空后返回 `null`；
- `recv(timeout_ms)` 超时抛错；
- `close_send` 关闭 out lane，`close_recv` 关闭 in lane，`close` 同时关闭；幂等，
  保留队列中未读消息，并唤醒所有等待者；
- fiber 中的端点 send/recv/select 通过事件循环 async wake 挂起/唤醒；
  非 fiber 上下文使用 OS 线程等待；
- 等待者按 FIFO 唤醒（OS 线程等待者优先于 fiber 等待者）；
- `std.thread.select` 使用 round-robin 起点，避免固定顺序造成饥饿；
- `waiters` / `send_waiters` / `recv_waiters` 返回当前等待者数量；
- `total_sends` / `total_recvs` / `send_timeouts` / `recv_timeouts` 为累计统计；
- `stats` 返回包含上述字段的 dict；

示例：

```skein
import std;

let ends = std.thread.Channel(2);
let parent = ends[0];
let child = ends[1];
let msg = std.thread.Message.from({"x": 1});
parent.send(msg);

let r = child.try_recv();
print(r["value"].payload["x"]);
print(parent.stats["total_sends"]);

parent.close();
```

无界与 rendezvous：

```skein
let a = std.thread.Channel.unbounded();   // 每条 lane capacity == -1
let b = std.thread.Channel(0);            // send 等待 recv
```

#### 线程池

```text
std.thread.pool(size, script[, options]) -> pool

pool.submit(value[, timeout_ms]) -> future
future.wait([timeout_ms]) -> {"ok": bool, "value"?: any, "error"?: error}
future.try_wait() -> {"ok": bool, "closed": bool, "canceled"?: bool, "value"?: message}
future.cancel() -> bool

pool.close()
pool.join()
pool.stats() -> dict
```

`options`：

```text
entry         默认 "handle"
queue         默认 1024；正整数或 "unbounded"
restart_limit 默认 0；worker 异常退出后的重启次数
worker_vm     默认 false；启用后 worker 使用 VM/JIT（已验证并发 stress/TSAN）
```

worker 脚本导出 `handle(payload)`：

```skein
# worker.skein
func handle(payload) {
    return {"n": payload["n"] * 2};
}
```

使用：

```skein
import std;

let pool = std.thread.pool(2, "worker.skein");
let f1 = pool.submit({"n": 21});
let f2 = pool.submit({"n": 5});
let r1 = f1.wait();
let r2 = f2.wait();
print(r1["value"]["n"]);
print(r2["value"]["n"]);
pool.close();
pool.join();
```

规则：

- 每个 worker 是独立 runtime，加载一次脚本并循环处理 job；
- `handle` 抛错时返回 `{"ok": false, "error": ...}`，worker 继续；
- worker 返回值必须是 data-only；generator / userdata / 函数值等会返回错误；
- worker 发生未捕获的原生异常时，当前 job 的 future 返回 `error("thread.worker_crashed", ...)`；
- `restart_limit > 0` 时异常退出的 worker 会被重启，最多重启该次数；
- 所有 worker 死亡后，新的 `submit` 报 `thread.pool_failed`；
- `pool.submit` 共享经过校验的 payload（容器 COW，string/bytes 共享底层 buffer），写入时分离；`submit(value, timeout_ms)` 超时抛 `thread.timeout`；
- `future.cancel()` 关闭结果通道；之后的 `wait()` 抛 `thread.cancelled`，`try_wait()` 返回 `canceled: true`；尚未开始的排队 job 会被 worker 跳过，正在执行的 job 无法中断；
- `pool.close()` 停止接收新 job，worker 排空后退出；
- `pool.join()` 等待 worker 线程结束；
- `stats()` 返回 `size/workers/ready/alive/dead/restarts/restart_limit/submitted/completed/failed/active/queued`；
- 支持在被导入模块的顶层创建 pool/spawn；解析器在求值顶层语句时会释放解析锁，worker 可以在该窗口解析入口脚本。

#### 单线程

```text
std.thread.spawn_script(script, message[, options]) -> thread
std.thread.spawn_source(source, message[, options]) -> thread
std.thread.spawn_function(function, message) -> thread
std.entry.encode(function) -> bytes
std.thread.spawn_entry(bytes, message) -> thread

旧名 `std.thread.spawn` 已移除；请使用 `spawn_script`。

thread.join([timeout_ms]) -> {"ok": bool, "value"?: any, "error"?: error}
thread.detach()
thread.is_alive() -> bool
thread.cancel() -> bool
thread.id
```

`options`：

```text
entry     默认 "main"
```

`spawn_source` 直接接收脚本文本，内部写入当前线程工作目录下的临时文件，等待子线程解析完成后删除；相对导入按当前线程 cwd 解析，仍会在子线程重新解析源码。需要复用 Proto/bytecode 时使用 `func` 或 `std.thread.spawn_function`。

Proto 入口：

```skein
func worker(ctx) {
    return {"v": ctx.payload["n"] + 1};
}

let t = std.thread.spawn_function(worker, std.thread.Message.from({"n": 4}));
```

- `Message.from` 的 payload 允许 `heap`，包括容器内的 heap；heap comparator 必须是不捕获闭包的函数；
- heap 是持久化数据结构，跨线程/进程入口共享只读结构；payload 仍不允许 standalone function 值；
- `std.thread.pool` 的 submit payload 和线程返回值同样支持 heap。

- 任何满足入口签名的函数都可以作为线程入口，不需要额外声明标记；
- 入口签名要求固定的单个参数；入口内可以使用嵌套函数和 `defer`；捕获的 upvalue 在 spawn 时按值做 data-only 快照；
- 函数体在父线程编译成不可变 Proto，子线程克隆后通过 VM 执行，不再解析源码；
- 函数体可使用参数、语言 builtin、自动注入的基础 `std`、捕获的 upvalue 和父线程全局数据值；父线程函数值会按 Proto 传递，并递归带上其捕获值与依赖的全局数据/函数；
- 嵌套函数和 `defer` 以 Proto/声明元数据传递，子线程在自身栈帧上重新捕获局部变量；用户 `import` 会在父线程加载并把命名空间快照传入子线程，子线程通过路径种子复用命名空间，不再要求模块源码存在；模块导出需能按 data/function 规则传递；
- `std.thread.spawn_function` 可直接传入满足条件的普通函数，作为低层 Proto 入口；
- `std.entry.encode(fn)` 把入口及依赖图（Proto、upvalue、全局、模块命名空间）序列化为 `bytes`；`std.thread.spawn_entry(bytes, Message)` 可在另一个进程反序列化并启动线程：
  ```skein
  std.fs.write_bytes("entry.bin", std.entry.encode(worker));
  # 另一个进程：
  let t = std.thread.spawn_entry(std.fs.read_bytes("entry.bin"), msg);
  ```
  当前可序列化的 Proto 支持入口内的局部 `struct` / `enum` / `proto` / `impl` 声明、嵌套函数和 `defer`；结构/枚举字段默认值、struct inline 方法和 impl 方法会随声明元数据传递，局部方法参数默认值当前仍受限。data 值支持 null/bool/int/float/char/byte/string/bytes/error/result/list/dict/set/deque、struct/enum（字段/variant 元数据随 payload 注册；字段默认值，以及带默认/模式/可变参数、数据捕获和全局数据依赖的方法也会传输；方法若捕获函数/heap/命名空间，当前会给出明确诊断并拒绝）以及 builtin 函数常量。`list` / `dict` / `set` / `deque` 现在会递归携带嵌套值，因此可以跨线程/进程传递函数表、heap 容器和多层混合容器；LRU 是普通 struct，遵循 struct 的传输规则，不能嵌套函数 / heap 等特殊值。

线程入口接收一个 Message：

```skein
# thread_worker.skein
func main(ctx) {
    let signal = ctx.handles["signal"];
    signal.send(std.thread.Message.from({"x": ctx.payload["x"]}));
    return {"x": ctx.payload["x"]};
}
```

使用：

```skein
import std;

let ends = std.thread.Channel(1);
let msg = std.thread.Message.from(
    {"x": 5},
    {"signal": ends[1]}
);

let t = std.thread.spawn_script("thread_worker.skein", msg);
let signal = ends[0].recv();
let result = t.join();

print(signal.payload["x"]);
print(result["ok"]);
print(result["value"]["x"]);
```

规则：

- `spawn_script` 等待脚本加载和入口解析完成后返回；
- 子线程拥有自己的 runtime、eval context、AST/function 缓存；
- 入口函数默认 `main`，接收一个 Message 参数；
- `join()` 返回 `{ok, value}` 或 `{ok:false, error}`；
- `detach()` 后线程自行运行和退出；
- `cancel()` 只设置协作取消标记，不能抢占执行中的线程；
- 子线程结果必须是 data-only 值；返回值经校验后按 COW 共享，调用方写入时分离。

#### 有方向端点对

Channel 是唯一的有方向模型；旧的共享队列与 `std.thread.duplex` 已移除。
`Channel(n)` 返回 `(parent, child)`，每个端点各有一条 in lane 和一条 out lane：
`parent.send` 发往 `child.recv`，`child.send` 发往 `parent.recv`。
`endpoint.child` / `endpoint.peer` 返回对端端点。

```text
std.thread.spawn_script_with_channel(script, message, capacity[, options]) -> thread
thread.channel -> endpoint
```

- `spawn_script_with_channel` 创建端点对，子端注入 `ctx.handles["channel"]`，父端通过 `thread.channel` 访问；
- `close_send` 只关闭 out 方向；对端 `recv` 排空后得到 `null`；
- `close_recv` 只关闭 in 方向；`closed` 反映任一方向关闭；
- `select` 接受端点并使用其 in 方向；`broadcast` / `try_broadcast` 向端点列表的 out 方向各发一份；
- 脚本可见的 handle 类型统一为 `thread_endpoint`；
- `std.thread.collect()` 主动回收不可达的 Message / Channel / 端点环；仅在没有活动 worker 线程时执行。
- `std.thread.debug_counts()` 返回 `messages/channels/pairs/endpoints/collected` 诊断计数。

#### select / broadcast

```text
std.thread.select(endpoints[, timeout_ms]) -> dict
std.thread.try_broadcast(endpoints, message) -> dict
std.thread.broadcast(endpoints, message) -> null
```

`select` 返回：

```text
{"index": 0, "value": message}
{"index": 1, "closed": true}
{"index": -1, "timeout": true}
```

示例：

```skein
let a = std.thread.Channel(1)[0];
let ends = std.thread.Channel(1);
ends[0].send(std.thread.Message.from({"v": "b"}));
let b = ends[1];

let r = std.thread.select([a, b], 1000);
print(r["index"]);
print(r["value"].payload["v"]);
```

规则：

- 当前只支持 `thread_endpoint`；`select` 从每个端点的 in lane 取消息；
- 多个端点同时就绪时使用轮转起点，避免总是选 0 号；
- 有数据优先，排空后才返回 closed；
- `timeout_ms = 0` 表示不超时；
- `try_broadcast` 返回：
  ```text
  {"sent": [0, 1], "full": [], "closed": []}
  ```
- `broadcast` 逐个阻塞发送；任一失败时抛错，已发送的不回滚。

#### 能力查询

```text
std.thread.capabilities() -> dict
```

返回全局能力：

```text
{
  "schema": 1,
  "platform": "linux",
  "strategy": "auto",
  "strategies": ["dispatch", "reuse_port"],
  "reuse_port": true,
  "dispatch": true,
  "http1": true,
  "http2": true,
  "tls": true,
  "keep_alive": true,
  "stream_request_body": true,
  "stream_response": true,
  "websocket": false,
  "sse": false,
  "raw_socket": false
}
```

`ThreadServer.capabilities` 返回实际策略的能力，并包含同样的
`schema` / `platform` / `strategy` / `strategies` 字段。

HTTP 侧也可以按策略查询：

```skein
let caps = std.http.thread_capabilities({"strategy": "auto"});
let dispatch = std.http.thread_capabilities({"strategy": "dispatch"});
```

- `"auto"`：Linux 解析为 `reuse_port`，Windows 解析为 `dispatch`；
- `"reuse_port"`：不支持的平台会报错；
- 返回字段与 `std.thread.capabilities()` 一致。

`serve_threads` 的 `require` / `on_unsupported`：

```skein
let ts = std.http.serve_threads(
  s, 4, "worker.skein",
  {"require": ["http2"],
   "on_unsupported": "501"});
```

- `"error"`：缺失能力时启动失败；
- `"warn"`：输出警告并继续；
- `"501"`：启动成功，但请求返回 501。


### 环境变量和进程捕获


```text
std.env.get(name[, default])
std.env.has(name)
std.env.set(name, value)
std.env.unset(name)
std.env.all()                     → dict

std.process.run_command(command[, options]) → dict
```

`std.env` 的读写是线程局部覆盖层：读取先查当前线程覆盖，再回退到进程环境；`set` / `unset` 不修改进程环境。新线程在启动时继承创建线程的环境快照；`std.process.spawn_command` / `std.process.run_command` / `std.process.command` 会把当前线程的环境覆盖应用到子进程。

线程局部 env/cwd、FFI 回调的 owner 线程约束和 `ffi.call_task` 的环境可见性限制见
[env-ffi-thread-safety.md](env-ffi-thread-safety.md)。

`std.sys` 暴露脚本调用信息：

```text
std.sys.args()        → list[string]  // 脚本路径之后的参数
std.sys.script()      → string        // 脚本路径；stdin/REPL 为空
std.sys.executable()  → string        // 解释器可执行文件
std.sys.exit([code])                  // 停止脚本并以 code（0–255）退出
std.sys.is_main()     → bool          // 当前代码是否属于入口 main 包
```

解释器选项必须写在脚本路径之前；脚本路径之后的所有参数（包括以 `-` 开头的）
都原样传给脚本。示例：

```text
skein tool.skein --bin build/skein --check "a b"
```

脚本内 `std.sys.args()` 返回 `["--bin", "build/skein", "--check", "a b"]`。
`std.sys.exit(code)` 用于命令行工具：立即停止当前脚本（不再执行后续语句），
刷新标准流后以 `code` 退出；省略 `code` 等价于 `0`。
`std.sys.is_main()` 按定义代码的包判断：入口脚本、stdin/REPL 和被入口脚本调用的
main 包函数返回 `true`；被 import 的包及其函数返回 `false`。

旧名 `std.process.run` 已移除；请使用 `run_command`。

`std.process.run_command` 的 `options` 支持：

```text
stdin   string / bytes，默认空
cwd     工作目录
env     dict，覆盖子进程环境
```

返回：

```text
{"status": int, "stdout": bytes, "stderr": bytes}
```

示例：

```text
import std;

let r = std.process.run_command("printf hello");
print(std.bytes.to_string(r["stdout"]));

let r2 = std.process.run_command("cat", {"stdin": "abc"});
std.bytes.to_string(r2["stdout"])
```

注意：当前实现通过 shell 重定向捕获输出，不提供超时和流式读写。


### 外部命令

通过 `std.process` 执行 shell 命令。全局 `command` / `command_status` 已移除。

`std.process.command(cmd[, env])` 执行命令并返回标准输出字符串：

```text
import std;

std.process.command("echo hi")                    → "hi"
std.process.command("echo $FOO", {"FOO": "bar"})  → "bar"
print(std.process.command("ls"))
```

- POSIX 使用 `/bin/sh -c`；
- Windows/MinGW 使用 `cmd.exe /C`；
- 可选的 `env` 是 dict，键和值必须是字符串；
- `env` 在父进程环境基础上覆盖，只对当前命令生效；
- 标准错误继承到当前进程，不会被捕获；
- 返回值末尾的 `\r` / `\n` 会被去掉；
- 命令无法启动时报告 `command failed to start`；
- 非零退出码不会自动报错，当前返回值只有标准输出，没有退出码。

如果只需要退出码，使用 `std.process.command_status(cmd[, env])`：

```text
import std;

std.process.command_status("exit 0")                     → 0
std.process.command_status("exit 3")                     → 3
std.process.command_status("exit $CODE", {"CODE": "3"})  → 3
```

`std.process.command_status` 会直接继承标准输入/输出/错误，因此命令输出会正常显示；它返回退出码整数。

如果需要同时获取 status、stdout、stderr，或控制 stdin/cwd/env，使用
`std.process.run_command`：

```text
import std;

let r = std.process.run_command("printf hello");
r["status"]
std.bytes.to_string(r["stdout"])
```

#### 编码进程入口

```text
std.process.run_entry(blob, message[, options]) → envelope
std.process.spawn_entry(blob, message[, options]) → process
std.process.run_function(function, message[, options]) → envelope
std.process.spawn_function(function, message[, options]) → process
std.process.result(process[, timeout_ms]) → envelope
```

- `run_function` / `spawn_function` 内部先执行 `std.entry.encode(function)`；
- `blob` 来自 `std.entry.encode(entry)`；
- Message payload 以 data-only 值序列化后通过 stdin 发送；
- 子进程用 `--run-entry` 读取请求、反序列化 Proto 入口并执行；
- 返回值为 `{"ok": bool, ...}` envelope；
- `options` 支持 `env`、`cwd`、`stderr`、`timeout_ms`；
- 子进程不依赖入口源码，也不需要重新解析脚本；
- 进程 IPC 不支持 Channel / Thread / Task handle。

```skein
import std;

func worker(ctx) {
  return {"v": ctx.payload["n"] + 1};
}
let blob = std.entry.encode(worker);
let r = std.process.run_entry(blob, std.thread.Message.from({"n": 4}));
r["value"]["v"]   // 5
```

普通单参数函数即可作为进程入口：

```skein
import std;

func entry(ctx) {
  return {"v": ctx.payload["n"] * 2};
}

let blob = std.entry.encode(entry);
let r = std.process.run_entry(blob, std.thread.Message.from({"n": 21}));
r["value"]["v"]   // 42
```

- 入口必须是单参数、非 generator、非 variadic、非 method 的函数；
- 通过 `std.entry.encode` 编码后用 `std.process.spawn_entry` / `run_entry` 启动，
  或直接用 `spawn_function` / `run_function`；
- 子进程通过 `--run-entry` 反序列化执行，不依赖入口源码。

#### 长驻进程与管道

```text
std.process.spawn_command(file[, options]) → process
std.process.wait(process[, timeout_ms]) → status
std.process.kill(process[, signal])
std.process.close(process)
std.process.pid(process)
std.process.rss(process_or_pid) → int（字节）
std.process.children(process_or_pid) → list[int]
std.process.tree_rss(process_or_pid) → int（字节）
```

`rss` 返回进程当前 RSS（字节）；进程句柄已退出时返回 `0`。
`children` 返回直接子进程 pid；`tree_rss` 汇总进程及其所有后代的 RSS，
用于进程池内存观测。参数可以是进程句柄或正整数 pid。

旧名 `std.process.spawn` 已移除；请使用 `spawn_command`。

`options`：

```text
args   list[string]，不包含 argv[0]；程序路径由 file 提供
env    dict[string, string]；省略时继承父进程环境
cwd    string
stderr "pipe"（默认）或 "inherit"
```

`spawn_command` 直接执行 `file`，不经过 shell。返回：

```text
{
  "pid": int,
  "stdin": stream,
  "stdout": stream,
  "stderr": stream,
  "handle": process
}
```

管道是普通 `std.io` 流，支持同步和异步读写：

```text
import std;

let p = std.process.spawn_command("/bin/cat");

std.io.write_all(p["stdin"], "hello");
std.io.close(p["stdin"]);

print(std.bytes.to_string(std.io.read(p["stdout"], 4096)));
print(std.process.wait(p));
```

task 形式：

```text
let data = std.io.read_task(p["stdout"], 4096).wait();
```

`std.io.stdin`、`std.io.stdout`、`std.io.stderr` 也作为标准流暴露，方便子进程侧实现自己的协议。

标准流是进程级共享资源；多个 worker 线程并发输出时，单次 `print` / `println` 以及 `std.io.write_all` / `std.io.flush` 调用会通过进程级锁串行化，避免一次调用内部被其他线程插入。

注意：

- `std.process.close` 只关闭管道，不杀死进程；需要终止时先 `kill`；
- `stderr` 始终是管道，父进程应读取，避免子进程因 stderr 写满而阻塞；
- 管道不支持 `seek` / `tell`；
- `wait` 超时后不会自动杀死进程。

##### JSON Lines worker 示例

worker 脚本：

```text
import std;

while (true) {
  let line = std.io.read_line(std.io.stdin);
  if (line == null) { break; }

  let request = std.json.parse(std.bytes.to_string(line));
  let result = request["params"][0] + request["params"][1];
  let response = {"id": request["id"], "ok": true, "result": result};

  std.io.write_all(std.io.stdout,
                   std.bytes.from_string(std.json.stringify(response) + "\n"));
  std.io.flush(std.io.stdout);
}
```

父进程：

```text
import std;

let p = std.process.spawn_command("skein", {"args": ["worker.skein"]});
std.io.write_all(p["stdin"],
                 std.bytes.from_string("{\"id\":1,\"params\":[2,3]}\n"));
std.io.flush(p["stdin"]);

print(std.bytes.to_string(std.io.read_line(p["stdout"])));
std.io.close(p["stdin"]);
print(std.process.wait(p));
```

#### Worker 池

在 spawn + JSON Lines 协议之上提供进程池：

```text
std.worker.pool(size, script[, options]) -> pool
std.worker.call(pool, method, args[, timeout_ms]) -> result
std.worker.call_bytes(pool, method, args, body[, timeout_ms]) -> result
std.worker.map(pool, method, items[, timeout_ms]) -> list
std.worker.submit(pool, method, args) -> job
std.worker.submit_bytes(pool, method, args, body) -> job
std.worker.wait(job[, timeout_ms]) -> result
std.worker.shutdown(pool)
std.worker.stats(pool) -> dict
```

`options`：

```text
queue_limit       默认 1024
restart_limit     默认 3
ready_timeout_ms  默认 5000
protocol          "jsonl"（默认）或 "hybrid"
```

`protocol = "hybrid"` 使用 JSON header + raw body，避免大 body 的 JSON
转义和 base64 开销。请求帧：

```text
{"id":1,"method":"handle","params":[...],"body_len":N}\n
<N 字节原始 body>
```

响应帧：

```text
{"id":1,"ok":true,"result":...,"body_len":M}\n
<M 字节原始 body>
```

hybrid worker 启动时发送：

```text
{"ready":true,"version":1,"protocol":"hybrid"}
```

并新增：

```text
std.worker.call_bytes(pool, method, args, body[, timeout_ms]) -> result
std.worker.submit_bytes(pool, method, args, body) -> job
```

`call_bytes` / `wait` 成功时返回的 value 是：

```text
{"value": <result>, "body": <bytes>}
```

单个 body 上限为 256 MiB。默认 JSON Lines worker 脚本不受影响。

worker 脚本读取 hybrid 请求：

```text
import std;

let line = std.io.read_line(std.io.stdin);
let request = std.json.parse(std.bytes.to_string(line));
let body = std.bytes.from_string("");
if (request["body_len"] > 0) {
  body = std.io.read_exact(std.io.stdin, request["body_len"]);
}

let response = {
  "id": request["id"],
  "ok": true,
  "result": request["params"][0],
  "body_len": len(body)
};
std.io.write_all(std.io.stdout,
                 std.bytes.from_string(std.json.stringify(response) + "\n"));
std.io.write_all(std.io.stdout, body);
std.io.flush(std.io.stdout);
```

- `size`：worker 进程数
- `script`：worker 脚本路径
- `method`：worker 脚本中负责分发的逻辑
- `args` / `items`：JSON 可序列化数据
- `timeout_ms`：可选，0 或省略表示不设置超时
- `call` 返回 `result`，worker 错误、超时和管道断开都不会直接中断进程
- `submit` 立即提交任务并返回 `job`；`wait` 阻塞等待结果
- 任务多于 worker 时进入队列，由 `wait` 驱动调度
- worker 崩溃或超时后会关闭并重新启动该 worker
- 单个 worker 连续重启超过 3 次后会被标记为不可用
- `map` 把任务分批分发给多个 worker，保持输入顺序；任一任务失败时报告错误

worker 脚本约定使用 JSON Lines，并在启动后先发送 ready 帧：

```text
import std;

std.io.write_all(std.io.stdout,
                 std.bytes.from_string("{\"ready\":true,\"version\":1}\n"));
std.io.flush(std.io.stdout);

while (true) {
  let line = std.io.read_line(std.io.stdin);
  if (line == null) { break; }

  let request = std.json.parse(std.bytes.to_string(line));
  let result = request["params"][0] + request["params"][1];
  let response = {"id": request["id"], "ok": true, "result": result};

  std.io.write_all(std.io.stdout,
                   std.bytes.from_string(std.json.stringify(response) + "\n"));
  std.io.flush(std.io.stdout);
}
```

主进程：

```text
import std;

let pool = std.worker.pool(4, "worker.skein");
let r = std.worker.call(pool, "add", [2, 3]);
print(std.result.unwrap(r));

print(std.worker.map(pool, "add", [[1, 2], [3, 4], [5, 6]]));

let a = std.worker.submit(pool, "add", [7, 8]);
let b = std.worker.submit(pool, "add", [9, 10]);
print(std.result.unwrap(std.worker.wait(a)));
print(std.result.unwrap(std.worker.wait(b)));

std.worker.shutdown(pool);
```

`std.worker.stats(pool)` 返回：

```text
{
  "workers": int,
  "busy": int,
  "available": int,
  "queued": int,
  "restarts": int
}
```

当前限制：

- 使用 JSON Lines，不支持原始 `bytes`；
- 超时和崩溃后只重启当前 worker，没有重启次数上限和池级熔断；
- worker 脚本需要自己实现 `method` 分发和错误响应；
- Windows worker 行为待单独验证。

环境变量名不能为空，也不能包含 `=`。在 Windows cmd 中引用变量使用 `%FOO%`，POSIX shell 使用 `$FOO`。

Windows/MinGW 下命令使用 `CreateProcessW` 传递 Unicode 命令行和 Unicode 环境块；捕获的 stdout 约定为 UTF-8。

**安全提示**：这些函数会执行任意 shell 命令，只应对可信输入使用。


## 第五部分：工程化

目标：组织多文件项目、导入依赖，并处理开发期重启与生产部署。

### 包与导入

入口脚本的包固定是 `main`，可以显式写 `package main {}`，也可以省略。包文件使用显式包块：

```text
package cc {
  export let value = 42;
  export const fixed = 7;

  export func hello(name) {
    return "hi " + name;
  }

  func private_helper() {
    return 1;
  }
}
```

- `package name {}` 创建独立作用域；
- `export let` / `export const` / `export func` 导出标识符；
- 也可以使用 `export name = expr;` 简写；
- `export` 目前支持 `let`、`const`、命名 `func`、`struct` 和 `enum`；
- 没有 `export` 的定义是包私有的；
- 入口脚本的包固定是 `main`；`package main {}` 可以位于任意目录，也可以省略；
- 入口脚本声明其他包名会报错；普通包文件不能直接作为入口运行；
- 被 `import` 的包名必须与所在文件夹名相同，且不能是 `main`；可以多层嵌套；
- `main` 不能被 `import`；
- stdin / REPL 没有文件包身份，`package name {}` 仍可创建临时命名空间。

#### 目录结构

每个可导入包必须是真实存在的 `.skein` 文件：

```text
aa/
  bb.skein            # package bb
  bb/
    cc.skein          # package cc
    cc/
      dd.skein        # package dd
```

```text
import aa.bb;
print(bb.value);
```

- 包块名只写叶子名：`aa/bb/cc.skein` 写 `package cc`；
- 可导入包必须有对应文件；`<name>/<name>.skein`、`index.skein`、单文件目录都不再支持；
- 目录只用于组织路径，不能作为包或隐式命名空间；
- 子包默认随父包暴露：`import aa.bb` 后可用 `bb.cc`，前提是
  `aa/bb/cc.skein` 存在；
- `bb.cc.dd` 需要 `aa/bb/cc.skein` 和 `aa/bb/cc/dd.skein` 都存在；
- 直接 `import aa.bb.cc.dd` 按文件路径解析，不要求中间包文件存在。

#### import 规则

```text
import aa.bb.cc;       // 绑定 cc
import aa.bb;          // 绑定 bb；子包通过 bb.cc 访问
import aa;             // 绑定 aa；子包通过 aa.bb 访问
import aa.bb.cc as c;  // 绑定到别名 c
import std as s;       // 标准库也可使用别名

import .sibling;       // 当前文件目录
import ..common;       // 上一级目录
import @top.pkg;       // 项目根目录

import {answer} from util;              // 只绑定 answer，不绑定 util
import {answer as a} from util;         // 绑定别名 a
import {Point, make} from @top.shapes;  // 导出值和导出类型都可选择
import {parse} from .jsonutil;
import {parse} from ..common;
import {parse} from "github.com/acme/util/json";
```

- `import aa.bb` 的普通包基准是主脚本目录；
- `import @aa.bb` 只从项目根解析；
- 项目根优先通过从主脚本目录向上查找 `skein.mod` 确定，其次兼容 `.skeinroot`；都没有时使用主脚本目录；
- `project_root` 惰性解析：单文件脚本、只 import `std`、只 import module 时不需要；
- `-I` / `SKEIN_PACKAGE_PATH` 只作为 module 搜索路径，不解析普通文件包；
- 普通文件包必须在 `project_root` 内；外部复用请组织成 module；
- module 内相对 import 不能越过 module root；
- 裸 `std` 是内置标准库；`.std`、`..std`、`@std` 按文件系统包路径解析；
- `main` 不能被导入；`import main;` 和 `import main.*;` 会报错；
- 普通包文件不能声明 `package main {}`；
- 别名必须是普通标识符，`as` 只在别名分隔符位置是 contextual keyword，
  其他位置仍是普通标识符；
- `import {name} from pkg;` 只绑定选中的导出值或导出类型，不绑定 `pkg` 本身；
  选择项可写 `name as alias`，至少写一项，重复名字或 alias 会报错；
- `from` 只在 `import { ... }` 后作 contextual keyword，其他位置仍是普通标识符或
  字段名；check-only / lint / LSP 不执行被导入的包来验证选择项；
- 同一个包只加载一次，之后复用缓存；
- 循环导入会报错，不会递归挂起；
- 当前不提供通配导入。

#### 第三方包

第三方 module 使用带引号的导入路径，和本地点号路径明确区分：

```text
import "github.com/acme/util/json";              // 默认绑定 json
import "github.com/acme/util/json" as jsonutil;  // 显式别名
```

先初始化工程，再用 `skein get` 拉取并登记版本：

```text
skein mod init github.com/acme/myapp
skein get github.com/acme/util@v1.2.3
```

`get` 通过 git 克隆仓库 tag，遍历所有依赖的 `skein.mod` 并用 MVS 选出每个 module path 的最高 semver 版本，然后写回主项目 `skein.mod`（非直接依赖标记为 `// indirect`）与 `skein.sum` 的 `h1:` 校验和；不写版本时选择最高的 semver tag。不兼容的 major 版本使用 `/vN` 路径并存，例如 `example.com/c` 与 `example.com/c/v2` 是两个独立 module。缓存默认在 `~/.skein/mod`（Windows 为 `%LOCALAPPDATA%\skein\mod`），可用 `SKEINMODCACHE` 覆盖。运行时按最长 `require` 前缀解析路径并校验缓存；缺少依赖或校验失败会直接报错，不会隐式下载。`lint`、`fmt`、`lsp` 也不下载或执行外部包。

`skein.sum` 是校验和的唯一信任根：`skein get` **不会**在校验失败时静默重新下载并改写它。缓存内容与 `skein.sum` 不一致、或已有校验和但缓存未下载而新下载内容不匹配，都会硬失败；只有显式 `skein get --force <module>@<version>` 才会在已有校验和时刷新缓存并重写对应校验和。缓存存在但当前项目没有校验和时不会直接信任它，而是重新下载并写入新的 `h1:`。

网络不可达时可用 `SKEIN_MODULE_URL` 把模块改写到镜像。它既支持单个 URL，也支持 `path=url,path=url` 映射；两种形式都支持 `{path}` 和 `{version}` 模板，例如：

```text
SKEIN_MODULE_URL=https://mirror.example/skein/{path}
SKEIN_MODULE_URL=github.com/acme/util=https://mirror.example/acme/util,example.test/x=https://mirror.example/x
```

`{version}` 在 `@latest` 的 tag 列举阶段不可用，会直接报错并要求显式版本。模板只解决“从哪里拉取”，完整性仍由 `skein.sum` 校验。

并发操作通过多层文件锁协调：`skein get` 持有项目级独占锁 `<project>/.skein/lock`，`skein build` 持有项目共享锁；两者同时持有缓存共享锁，并按 `module@version` 在缓存根 `.locks/` 下加模块锁。运行中的脚本首次加载外部 module 时也会懒获取缓存共享锁，因此 `skein mod clean` 会等待正在运行的 `get` / `build` 和脚本退出，不会在读取、下载或构建过程中删除缓存。锁随进程退出自动释放；需要等待时会在 stderr 提示。`.skein/` 是本地锁状态目录，不应提交到版本库。

`skein.mod` 和 `skein.sum` 通过临时文件替换写入。`skein get` 成功时批量写入新的 `skein.sum` 条目，然后更新 `skein.mod`；如果后续模块失败，`skein.mod` 保持原样，`skein.sum` 最多保留已经完成模块的有效校验和，不会出现半写文件或未校验缓存。

`skein mod clean` 删除整个全局缓存根（`SKEINMODCACHE` 或默认目录），不会影响项目里的 `skein.mod` 和 `skein.sum`。`--dry-run` 只打印将要清理的目录；如果缓存根被显式设成 home 目录、当前目录或文件系统根目录，需要额外传 `--force` 才允许清理。

常用维护命令：

```text
skein mod verify                 # 校验 skein.mod 中每个 require 的缓存与 skein.sum
skein mod download               # 按 skein.mod 预取缓存，不修改 skein.mod
skein get <module>@<version> --offline
skein mod tidy [--offline]       # 从直接依赖重算传递依赖，移除不再使用的 // indirect
skein mod prune [--all]           # 默认只列出当前项目未引用的缓存版本
```

`--offline` 完全禁止网络：`@latest` 必须改成显式版本；缓存缺失，或缓存存在但当前项目没有校验和时，会直接失败并要求先在线执行一次 `skein get`。`mod download` 会为首次下载的模块补写 `skein.sum`，但不会改写 `skein.mod`。`mod prune` 默认只预览；全局缓存可能被其他项目共享，只有显式 `--all` 才会删除未引用版本及对应 native 构建产物。锁等待默认无限；设置 `SKEIN_LOCK_TIMEOUT_MS=<毫秒>` 可让等待超时并报错。

#### 源码形式发布的原生 module（MVP）

module 可以在 `module.skein` 里声明可选的 `native { ... }` 块，随源码一起
发布 C/C++ 实现，由宿主显式编译成动态库：

```text
module myutil {
  export package api;
  native {
    sources "native/add.cpp" "native/extra.cpp";
    includes "native/include";
    defines "MYUTIL_TRACE=1";
    libs "m";
    output "myutil";
  }
}
```

- `skein get` 仍然只下载、校验并登记源码；**不会**在 import 时编译；
- `skein build [<module/path>[@version]]` 在已下载的缓存上编译，不传参数时
  编译 `skein.mod` 中全部依赖；编译器默认取 `SKEIN_CXX`、`CXX`，否则 `c++`；
- 产物放在源码缓存旁边的 `.build/<平台>-<架构>-<编译器>/` 目录，**不进入**
  `skein.sum` 的源码 dirhash，因此构建不会破坏校验和；
- `skein build` 会在产物旁写入 `artifact.meta`，记录源码 hash、产物 hash、编译器、
  编译器版本、构建 flags 和规范化命令 hash；`std.module.native` 只返回通过源码
  hash、产物 hash（以及可用时的编译器版本）校验的产物，篡改或源码变更后必须重新
  `skein build`；
- 构建默认启用可复现选项：GCC/Clang 使用 `-ffile-prefix-map`，MinGW 使用
  `--no-insert-timestamp`，MSVC 使用 `/Brepro`，并设置 `SOURCE_DATE_EPOCH=0`；
  相同输入连续构建应产生相同产物字节；
- Skein 封装包通过 `std.module.dir(path)` / `std.module.native(path)` 取得源码
  目录和构建产物路径，再用 `std.native.load(...)` 加载；`std.ffi.open(...)`
  同理；
- 主脚本里的原生调用保持原行为；**第三方 module 内**的 `std.native.load` /
  `std.ffi.open` 默认禁止，必须显式设置 `SKEIN_ALLOW_NATIVE=1`
  （设为 `0` / `false` / `off` / `no` 则全局禁止）；
- `skein_module.h` 是纯 C ABI 头，module 应自带一份，不需要链接 Skein 符号；
  ABI v2 的异步导出规则见 `docs/stdlib.md`。

#### include 声明包含

`include` 是给项目外文件留的声明级 escape hatch，行为类似 C 的声明包含：

```text
include "shared/helpers.skein";
```

- 只允许在声明层（顶层或 `package {}` 内），函数体内报错；
- 只导入声明类语句：`import`、`include`、`func`、`struct`、`enum`、
  `proto`、`impl`、`let` / `const`；
- 被包含文件可以 `import std`；相对 import 以被包含文件目录为基准；
- 被包含文件不能声明 `package` / `module`，不能包含顶层可执行代码；
- 默认 include-once：按 `(package, module, 规范化路径)` 去重；
- 自动检测循环并限制嵌套深度；
- 相对路径基于当前包含文件目录；允许 `..` 和绝对路径；
  不受 `project_root` / `-I` / `SKEIN_PACKAGE_PATH` 限制；
- 被包含的声明继承包含者的 package/module，`source` 指向被包含文件；
- 同一个头文件被 main 和包分别 include 会产生两份声明，身份分别属于各自包。

`include` 不做命名空间隔离，不建议作为长期库接口；外部库仍应使用 module。

包内也可以导入其他包，既可以放在包块内，也可以放在文件顶层：

```text
import aa.util;

package bb {
  export func get() {
    return util.value;
  }
}
```

#### Module

Module 是唯一的模块机制；源码层不存在 Module v1 或兼容层。顶层目录包含 `module.skein` 时，该目录成为 module。module 内仍然只使用文件包：

```text
math/                 # module: math
  module.skein        # 公共导出清单
  add.skein           # package: add      → math.add
  vector.skein        # package: vector   → math.vector
  internal/
    helpers.skein     # package: helpers  → math.internal.helpers
```

不再支持 `math/add/add.skein` 这种目录包布局。

```skein
// math/module.skein
module math {
  export add.add;
  export vector.sum as sum;
  export internal.helpers.normalize as normalize;
  export package internal.helpers as helpers;
}
```

外部：

```skein
import math;

math.add(1, 2);
math.sum([1, 2]);
math.normalize(x);
math.helpers.normalize(x);

import math.helpers as helpers;
```

规则：

- module 名必须与顶层目录名一致；
- package 名必须与文件名一致（只写叶子名）；
- 无 `export` 的符号是 package 私有；
- package 的 `export` 只提升到 module 内部；
- 只有 `module.skein` 中声明的符号/package 才能从外部访问；
- `export package internal.helpers as helpers;` 把 `math/internal/helpers.skein`
  暴露为 `math.helpers`；
- 外部不能通过物理路径 `import math.internal.helpers`；
- module 内部使用普通相对导入，例如 `import .internal.helpers`；
- module 内部写绝对 `import math` / `import math.helpers` 时也解析为公共 API；
  访问未导出的内部 package 请使用相对导入；
- module 内相对导入不能越过 module root；
- module 根目录不能包含 `<module>.skein` 或 `index.skein`；
- 禁止嵌套 `module.skein`；
- 直接运行 module 内的文件会按独立 `main` 入口处理，不继承 module 身份；
- 清单中的符号必须已在目标 package 中 `export`，否则启动报错；
- 重复别名、缺失 package、module 名不匹配都会报错；
- 同一个 module 只加载一次并复用缓存；
- `python3 tools/check_modules.py <project-root>` 可以离线校验 manifest 结构，
  `--list` 列出公共导出；它不执行 package 代码。

module 内类型和包的元数据：

```text
type_package(x)      // "math.add" / "math.internal.helpers"
type_module_name(x)  // "math"
type_source(x)       // 源文件路径
```

入口脚本和普通文件包的 `type_module_name(x)` 为空字符串。


#### 作用域与导出值

- 导入包在独立作用域中执行，不能读取主包的全局变量；
- 主文件中的 `package` 块也是独立作用域，需要的 `import` 应放在块内；
- 包内函数保留自己的闭包环境；
- `export let` / `export const` 的值在包加载时复制到命名空间；之后导出变量本身再变化，不会自动同步到已导入的命名空间；
- 导出函数仍然可以访问并修改包内部状态。


### 开发期自动重启

`--watch` 用子进程方式做开发期热重启，不需要修改脚本：

```bash
./build/skein --watch server.skein
./build/skein --watch --watch-dir ../shared server.skein
```

- 默认递归监听脚本所在目录下的 `.skein` 文件；
- 每 200ms 轮询一次 `mtime` / `size`，不依赖平台的 inotify / FSEvents；
- `--watch-dir DIR` / `--watch-dir=DIR` 可以增加监听目录；
- 已有的 `-I` / `--package-path` 目录也会加入监听；
- 文件变化后先停止旧进程，再从干净运行时启动新进程；
- 编译或运行失败时新进程退出，watcher 保留并等待下一次文件变化；
- `Ctrl-C` 会停止子进程并退出 watcher，`SIGTERM` 同样会先停止子进程再退出；
- `.git`、`.cache`、`node_modules`、`build*` 等目录会被跳过。

`--watch` 是进程级重启：模块状态、全局变量和内存数据都会重置。需要保留的状态请放到
外部存储或由父进程持有。它不改变语言运行时，也不是进程内热更新。


### 生产部署与优雅关闭

生产环境使用“平台滚动更新 + 运行时 SIGTERM drain”，不需要 `--watch` 或
`reuse_port` 监听句柄交接：

```bash
SKEIN_DRAIN_TIMEOUT_MS=15000 ./build/skein server.skein
```

- 收到 `SIGTERM` 后停止 accept、关闭监听 socket，等待在途请求完成；
- 超过 `SKEIN_DRAIN_TIMEOUT_MS`（默认 20000，`0` 表示不设截止时间）后强制
  关闭剩余连接；
- `serve_processes` 会把 `SIGTERM` 转发给 worker 并等待其退出；
- drain 完成时退出码为 `0`；`Ctrl-C`（`SIGINT`）仍然是中断语义；
- Windows 没有可捕获的 `SIGTERM`，drain 由控制台关闭/关机/注销事件触发
  （关闭窗口最多等待 5 秒）；`taskkill` 不会触发 drain；
- Windows 不支持 `reuse_port`，`serve_processes` 自动使用 `dispatch`，
  关闭时通过关闭 worker 的 stdin 让其在完成当前请求后退出；
- Kubernetes 中让 `terminationGracePeriodSeconds` 大于 drain 截止时间，
  并用 `preStop` 留出摘流时间；readiness 探针可以直接用 `std.http` 实现。

完整行为、`ProcessServer` / `ThreadServer` 的超时规则和 Kubernetes 示例见
`docs/stdlib.md` 的“优雅关闭（SIGTERM drain）”。
