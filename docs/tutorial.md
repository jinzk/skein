# Skein 教程

本教程从运行脚本开始，依次介绍语言基础、包与模块、并发、网络和常用
标准库用法。语法细节查 [SYNTAX.md](../SYNTAX.md)，标准库 API 查
[标准库指南](stdlib.md)。

## 本教程内容

- [脚本模式与 REPL](#脚本模式与-repl)
- [包与导入](#包与导入)
- [语言概览](#语言概览)
- [语法约定](#语法约定)
- [类型与转换](#类型与转换)
- [对象生命周期](#对象生命周期)
- [格式化](#格式化)
- [外部命令](#外部命令)
- [字符串处理](#字符串处理)
- [控制台输入](#控制台输入)
- [文件读写](#文件读写)
- [字符、字节与编码](#字符字节与编码)
- [正则表达式](#正则表达式)
- [时间、随机数和 UUID](#时间随机数和-uuid)
- [Base64、URL 和哈希](#base64url-和哈希)
- [环境变量和进程捕获](#环境变量和进程捕获)
- [目录、路径和压缩](#目录路径和压缩)
- [用 dict + 闭包封装文件对象](#用-dict--闭包封装文件对象)
- [线程、Channel 与并发](#线程channel-与并发)
- [错误信息](#错误信息)

## 脚本模式与 REPL

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

交互模式：

- 只有 stdin 是终端时才显示 `> ` / `...> ` 提示符；管道输入不显示提示符；
- 使用 `skein -i` 可以强制显示提示符；
- POSIX 终端支持方向键、Home/End、Delete、Ctrl-A/E/K/U/W、Ctrl-L；
- 支持上下方向键浏览历史；
- 历史默认保存到 `~/.skein_history`，可用 `SKEIN_HISTORY` 指定路径；
- Windows 当前回退为基础行输入，历史文件仍会保存；
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

## 包与导入

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
- `export` 目前支持 `let`、`const`、命名 `func`、`struct` 和 `trait`；
- 没有 `export` 的定义是包私有的；
- 入口脚本的包固定是 `main`；`package main {}` 可以位于任意目录，也可以省略；
- 入口脚本声明其他包名会报错；普通包文件不能直接作为入口运行；
- 被 `import` 的包名必须与所在文件夹名相同，且不能是 `main`；可以多层嵌套；
- `main` 不能被 `import`；
- stdin / REPL 没有文件包身份，`package name {}` 仍可创建临时命名空间。

### 目录结构

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

### import 规则

```text
import aa.bb.cc;       // 绑定 cc
import aa.bb;          // 绑定 bb；子包通过 bb.cc 访问
import aa;             // 绑定 aa；子包通过 aa.bb 访问
import aa.bb.cc as c;  // 绑定到别名 c
import std as s;       // 标准库也可使用别名

import .sibling;       // 当前文件目录
import ..common;       // 上一级目录
import @top.pkg;       // 项目根目录
```

- `import aa.bb` 的普通包基准是主脚本目录；
- `import @aa.bb` 只从项目根解析；
- 项目根通过从主脚本目录向上查找 `.skeinroot` 确定；没有标记时使用主脚本目录；
- `project_root` 惰性解析：单文件脚本、只 import `std`、只 import module 时不需要；
- `-I` / `SKEIN_PACKAGE_PATH` 只作为 module 搜索路径，不解析普通文件包；
- 普通文件包必须在 `project_root` 内；外部复用请组织成 module；
- module 内相对 import 不能越过 module root；
- 裸 `std` 是内置标准库；`.std`、`..std`、`@std` 按文件系统包路径解析；
- `main` 不能被导入；`import main;` 和 `import main.*;` 会报错；
- 普通包文件不能声明 `package main {}`；
- 别名必须是普通标识符，`as` 是保留字；
- 同一个包只加载一次，之后复用缓存；
- 循环导入会报错，不会递归挂起；
- 当前不提供通配导入。

### include 声明包含

`include` 是给项目外文件留的声明级 escape hatch，行为类似 C 的声明包含：

```text
include "shared/helpers.skein";
```

- 只允许在声明层（顶层或 `package {}` 内），函数体内报错；
- 只导入声明类语句：`import`、`include`、`func`、`struct`、`enum`、
  `trait`、`impl`、`let` / `const`；
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

### Module

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


### 作用域与导出值

- 导入包在独立作用域中执行，不能读取主包的全局变量；
- 主文件中的 `package` 块也是独立作用域，需要的 `import` 应放在块内；
- 包内函数保留自己的闭包环境；
- `export let` / `export const` 的值在包加载时复制到命名空间；之后导出变量本身再变化，不会自动同步到已导入的命名空间；
- 导出函数仍然可以访问并修改包内部状态。

## 语言概览

### 基本运算

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

### 管道与占位符

管道 `|>` 把左侧值作为右侧调用的第一个参数：

```text
xs |> f              // f(xs)
xs |> f(a)           // f(xs, a)
range(0, 10) |> std.iter.filter(func(x) -> x % 2 == 0) |> std.iter.collect()
```

- `|>` 左结合，优先级高于算术和比较运算，低于一元运算和后缀调用；
- 右侧不是调用时按 `f(xs)` 处理；右侧是调用时插入为第一个参数；
- 适合 `std.list` / `std.iter` 这类“主体在第一个参数”的 API。

还有 apply-to-result 形式 `|>>`：先求值右侧，再把左侧应用到结果。

```text
func f(x, y) { return x - y; }
let h = x -> y -> f(x, y);

4 |>> h(3)     // (h(3))(4) = f(3, 4)
```

调用参数中的 `$` 是占位符：

```text
func add(a, b) { return a + b; }

let add2 = add($, 2);     // x -> add(x, 2)
add2(1)                   // 3

let three = add($, $);    // (a, b) -> add(a, b)
three(4, 5)               // 9

[1, 2, 3] |> std.list.map($, x -> x * 2)    // [2, 4, 6]
[1, 2, 3] |>> std.list.map($, x -> x * 2)   // [2, 4, 6]
```

占位符规则：

- `$` 只在调用参数中生效；每个 `$` 对应返回函数的一个参数，按从左到右填充；
- 多个 `$` 返回多参数函数，不是自动柯里化；
- 非占位参数在返回的函数被调用时才求值；
- 嵌套调用中的 `$` 绑定到最近的调用，需要外层占位时请显式写 lambda；
- `|>>` 会先求值右侧，副作用顺序与 `|>` 不同；
- `_` 仍然是 REPL 的上一次结果别名，不会被占位符占用。

### 三元表达式

三元表达式：

```text
x > 0 ? "positive" : "non-positive"
0 ? 1/0 : 5          // 5，只求值选中的分支
```

`? :` 右结合，优先级低于 `||`，高于赋值。

### 变量与常量

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

### 类型标注

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

### 容器泛型

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
- 只支持内建容器的类型参数，不支持 `struct Box<T>`、`func f<T>`、泛型 trait 或类型别名；
- 用户类型可以作为参数，例如 `list[Point]`；`list` 仍然表示无元素约束；
- 检查初始化（字面量、`from_list`、构造字段、函数参数、默认值）和插入（下标赋值、`append`、`+=`、`set.add`、`push`、`merge`）；
- 运行时只检查外层容器类型，不扫描元素；动态路径仍可通过 `any` 绕过；
- 模式：`SKEIN_TYPECHECK=off|loose|warn|strict` 或 `--typecheck=...`，默认 `warn`；
  - `loose` 只报确定冲突；
  - `warn` 额外对隐式 `any -> 容器[T]` 发 warning；
  - `strict` 把该 warning 提升为 error；
- `as` 只改变静态类型，运行时是恒等操作，不做转换；确定的元素冲突仍然报错；
- `as` 不能作为赋值目标；`x is list[int]` 和模式中的泛型参数会被拒绝；
- 函数和方法调用会检查显式参数类型；方法还要求接收者有静态类型；
- 导入包的导出函数会使用其参数/返回类型签名，导出结构体的字段/方法和导出变量的类型标注也能解析；
- 解构声明、`match` 模式和 `for` 绑定会从值的静态类型推断元素/字段类型；
- `std.list.map` 会推断简单表达式 lambda 或单 return lambda 的返回类型，`std.list.zip` 会检查第二个参数是 list；
- 未注解的 `let` 会从初始化表达式推断类型；它不是固定标注，重新赋值为其他类型时会更新推断。

### 函数类型

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
- `list.map/filter/reduce/sort_by/group_by/unique_by` 会检查回调签名，并从 `map` / `group_by` 的回调结果推断返回类型；
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
- `pair` / `tuple` / `union` 是静态类型：pair/tuple 在运行时仍是 list，union 的成员兼容性主要由静态检查器保证；`union[T, null]` 归一化为 `T?` 后同时保留运行时检查；
- `match (a, b)` 统一按 tuple/pair subject 处理，模式 `[x, y]` 按位置解构；传单个 tuple 参数使用 `f((a, b))`；
- `std.list.append` / `std.dict.keys` / `std.set.from_list` 等容器 builtin 作为一等值时保留类型变量模板：`let f = std.list.append; f([1], 2)` 得到 `list[int]`，`f([1], "a")` 会报错；该支持仅限已知容器 builtin，不扩展到用户自定义泛型函数；
- `is` 和模式类型不接受函数类型签名。

### 自增、自减与复合赋值

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
- 类型约束和 `const` 规则与普通赋值一致。

变量名可以包含下划线：`my_value`。

### 列表、字典与切片

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
```

支持链式下标赋值：`a[0][1] = 9`、`d["a"]["b"] = 1`。

切片使用 Python 风格的 `start:stop:step`，支持 list、string 和 bytes：

```text
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
bytes_from_string("abcd")[1:3] // 两个字节 "bc"
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

### 控制流

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
```

`for ... in` 规则：

- 单变量：list 遍历元素，dict 遍历键；deque / heap / set 也遍历元素（heap 按层序，set 按插入顺序）；
- 双变量只能用于 dict，得到 `key, value`；
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
label outer: for (let i in [1, 2, 3]) {
  for (let j in [1, 2, 3]) {
    if (i == 2 && j == 2) { break outer; }
    print(str(i) + "," + str(j));
  }
}
```

- 标签写成 `label NAME: loop_or_switch`；
- `break NAME;` 跳出指定循环或 switch；
- `continue NAME;` 继续指定循环的下一轮；
- 标签只能直接放在循环或 switch 前。

### switch

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

### match

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

- `match` 是表达式，返回第一个匹配 arm 的值；
- arm 体是单条语句或代码块，块的值是最后一条语句的值；
- 支持字面量、绑定、`_`、dict（开放匹配 + `...rest`）、list（定长 + `...rest`）、struct（按字段）；
- 支持 or-pattern：`case 1 | 2 | 3`、`case E.A | E.B`；
- 支持整数范围模式：`case 1..9`（包含两端）；
- `case is Type` 是类型模式，例如 `case is int`、`case is Point`、`case is callable`；
- enum 匹配在没有 `default` 且缺少变体时报告 `match is not exhaustive`；
- `case pattern as name` 把整个匹配值绑定到 `name`；
- `case == IDLE` 匹配常量、名字或无载荷枚举变体；
- `match (a, b)` 是 tuple/pair subject，模式 `[x, y]` 按位置解构；
- 守卫写成 `case pattern if expr => ...`；
- 绑定只读，作用域限于该 arm；
- 无 arm 命中且无 `default` 时运行时报错；
- 模式不能调用函数或写比较表达式；
- 不支持 float 字面量。

### 错误处理

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

- `fail expr;` 抛出错误，`fail;` 抛出空错误；
- 普通值会被包装成 `error(kind="fail", message=...)`；
- `call() on error(e) { ... }` 捕获 `fail`、运行时错误和 `result.err`；
- `result.ok(v) on error(...)` 直接得到 `v`，不执行 handler；
- `e` 是 `error` 值，只在 handler 作用域内有效；
- `str(e)` 或 `std.error.message(e)` 取得消息；
- `std.error.kind(e)` 取得错误类别；
- `recover expr;` 结束 handler 并返回恢复值；
- `pass e;` 把错误继续传给外层；
- handler 必须以 `recover`、`pass` 或 `fail` 结束；
- handler 内不能直接用 `return` / `break` / `continue` 结束；
- 未捕获的 `fail` 在脚本顶层报错并停止脚本。

### 函数与闭包

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
func apply(fn: callable, x: int) {
  return fn(x);
}

apply(sqrt, 16)
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
- trait 方法声明不能有默认值，`impl` 方法可以额外提供默认值；
- 可变参数必须最后，且不能有默认值。

函数是一等值，可以保存到变量、列表或字典中，并通过任意表达式调用：

```text
let fs = [adder(1), adder(2)];
fs[0](10)
d["handler"](42)
adder(3)(4)
```

匿名函数可以使用 `->` 简写；单参数、多参数和零参数都支持，函数体是单个表达式：

```text
let inc = x -> x + 1;
let add = (a, b) -> a + b;
let zero = () -> 7;

[1, 2, 3] |> std.list.map(x -> x * 2)
```

`->` 的函数体只支持表达式；需要多语句或显式 `return` 时用 `func(x) { ... }`。

匿名函数使用同一个 `func` 关键字，省略函数名：

```text
let double = func(x) { return x * 2; };
double(21)

let inc = func(x) -> x + 1;
inc(41)

(func(x) { return x + 1; })(41)
(func(x) -> x + 1)(41)
```

单表达式可以用 `->` 简写，表达式结果自动成为返回值。`->` 优先级最低，因此 `func(x) -> x * 2 + 1` 的函数体是 `x * 2 + 1`。

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
- `{ ... }` 函数体使用显式 `return`；
- `-> exp` 函数体隐含 `return exp`；
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
- trait 方法暂不支持可变参数。

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

### 结构体、impl 与 trait

结构体、`impl` 与 `trait`：

```text
struct Point {
  x: int;
  y: int;
}

impl Point {
  func length(self) {
    return sqrt(self.x * self.x + self.y * self.y);
  }

  func moved(self, dx: int, dy: int) {
    return Point { x: self.x + dx, y: self.y + dy };
  }
}

let p = Point { x: 3, y: 4 };
p.x
p.length()
let q = p.moved(1, 2);
```

- `struct` 声明字段；没有默认值的字段都必须初始化，未知字段或重复字段会报错；
- 构造使用 `Point { x: 3, y: 4 }`，字段顺序不限；
- 支持 copy-with：`Point { ...p, x: 9 }`，`...` 必须放在最前，后面的显式字段覆盖原字段；
- 字段可以声明默认值：`struct Config { host: string = "localhost"; port: int = 80; }`；省略字段时使用默认值；
- 默认值在定义包的作用域中求值，每次构造重新求值；不能引用 `self` 或其他字段；
- `std.struct.decode` 不自动填充默认值，缺失字段仍然报错；
- 字段可以读写：`p.x`、`p.x = 5`；
- 方法第一个参数必须是显式 `self`；
- 方法参数支持默认值，但 `self` 不能有默认值；默认值可以引用 `self` 和更早的参数；
- `struct` 只声明字段；所有方法必须写在 `impl Type` 中；
- 方法只能通过实例调用：`p.length()`；
- 方法本身可以作为值提取，例如 `let f = p.length; f()`；
- 方法值绑定提取时的结构体快照，之后修改原结构体不会影响已绑定的方法；
- 方法可以修改 `self` 的字段和下标；结构体是值类型，修改只作用于方法内的接收者副本，调用方原值不变。需要保留修改时返回新结构体，例如 `return self`；
- 结构体是值语义，赋值和作为参数传递时使用 copy-on-write；
- 结构体类型可以用在变量和参数类型标注中：`let p: Point = Point { x: 1, y: 2 }`、`func f(p: Point)`；结构体类型没有默认值，变量必须初始化；
- 字段类型必须是已声明的类型；支持自引用，例如 `struct Node { value: int; next: Node?; }`；
- 不允许创建环：`a.next = b; b.next = a;` 会报 `recursive value would create a cycle`；共享子结构（DAG）允许；
- 所有字段和方法都是公开的；暂不实现 `_` 私有语法；
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

- 不支持继承、动态添加方法、静态方法、用户自定义泛型或运算符重载；内建容器的类型参数见变量类型标注章节。

`trait` 定义接口，`impl Trait for Type` 显式实现：

```text
trait Drawable {
  func draw(self);
}

impl Drawable for Point {
  func draw(self) {
    print("point");
  }
}

func render(x: Drawable) {
  x.draw();
}

render(p)
```

- `trait` 可以包含方法签名和默认方法体，不包含字段或 trait 继承；
- 没有默认体的 trait 方法仍然必须由 `impl` 提供；
- trait 方法声明不能有默认参数值；`impl` 方法可以额外提供默认值；
- `impl` 提供同名方法时覆盖 trait 默认方法；
- 多个 trait 为同一类型提供同名默认方法时，必须在 `impl` 中显式覆盖，否则报冲突；
- 接口是名义式的：只有显式写了 `impl Drawable for Point`，`Point` 才能传给 `Drawable`；
- 如果方法已经存在于 `impl Point` 中，可以只写 `impl Drawable for Point;` 做接口适配；
- trait 方法名不能与类型已有方法冲突；enum 上也不能与任意变体字段重名；
- `impl` 必须写在接收者类型所在的包中；trait 可以来自其他包，但未导出的 trait 不能被外部实现；
- 一个类型对同一个 trait 只能实现一次；
- struct、enum 和 trait 名称共享类型命名空间，不能在同一个包中重名；
- 类型名按包注册，一个包对应一个源文件；不同包可以定义同名结构体或 trait；
- 类型默认不跨包可见；`export struct` / `export trait` 后可以按包路径使用；
- 导出的 trait 签名只能引用已导出的类型或内建类型，否则外部类型无法实现它；
- 导入之后用 `aa.Point`、`lib.Drawable` 这样的限定名引用：

默认方法示例：

```text
trait Drawable {
  func draw(self) { print("default draw"); }
  func area(self);
}

struct Point { x: int; }

impl Drawable for Point {
  func area(self) { return self.x; }
  // draw 使用 trait 默认实现
}

Point { x: 3 }.draw();
```

`struct Name with Trait1, Trait2 { ... }` / `enum Name with Trait { ... }`
是 `impl Trait for Name;` 的声明式简写，只建立接口实现关系，不是继承：

```text
trait Drawable { func draw(self) { print("draw"); } }
struct Point with Drawable { x: int; y: int; }
```

- `with` 后面的 trait 支持限定名，如 `lib.Drawable`；
- 多个 trait 用逗号分隔；纯签名方法仍需在类型体内或后续 `impl` 中实现；
- `with` 只在 `struct` / `enum` 名称后识别，其他位置仍是普通标识符；

```text
// package aa
export struct P { x: int; }
export trait D { func d(self); }

// main
import aa;
let p: aa.P = aa.make();
p is aa.D

import aa as lib;
let q: lib.P = lib.make();
```

- 其他包可以为自己的类型实现导入的 trait：

```text
// main
import aa;
struct Q { y: int; }
impl aa.D for Q {
  func d(self) { return self.y; }
}
```

- 未导出的类型不能通过包路径引用，但它的值仍可作为普通值传递和方法调用。

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

struct Point { x: int; y: int; }

impl Point {
  @trace
  func area(self, scale: int = 1) {
    return self.x * self.y * scale;
  }
}

Point { x: 3, y: 4 }.area()      // enter / exit / 12
```

- `@expr` 写在 `impl` 方法前，可以连续写多个；多个装饰器从下往上应用；
- 装饰器在 `impl` 声明执行时应用一次，不随方法调用重复执行；
- 装饰器是普通高阶函数，不需要专门的定义语法；
- 装饰器收到 unbound function，`self` 是第一个参数；推荐用 `...args` 转发；
- 支持固有方法和 `impl Trait for Type` 方法；调用点、方法值提取和 trait 分派不变；
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
- enum 方法和 trait 方法同样适用，所有方法语义一致；
- 需要显式 task 句柄时使用 `launch c.get(...)`，再 `task.wait()`；
- 装饰器包装的是普通方法；如果被包装方法会挂起，wrapper 调用它时也会在当前 fiber 挂起。

### 枚举

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
- `impl Trait for Event` 可以让 enum 参与 trait 接口；`Event` 也能用于 `is` 和类型标注；
- 方法名不能与任意变体的字段重名；
- `kind_of(e)` 是 `"enum"`，`type_name(e)` 是枚举名；
- `match` 支持变体模式、字段模式和守卫；
- JSON 编码为 `{"$enum":"Event.UserMsg","payload":{...}}`；
- 暂不支持互递归 enum（自引用 enum 已支持）、enum 作为 dict key、enum 的 JSON 自动解码。

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
}

impl Drawable for Shape {
  func draw(self) { return self.area(); }
}
```

### 运行时类型检查

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

`is` 只接受类型名或 trait 名作为右操作数；结构体类型名按包解析。

### 内置函数

内置函数：`sqrt`、`exp`、`log`、`abs`、`sin`、`cos`、`tan`、`asin`、`acos`、`atan`、`atan2`、`pow`、`floor`、`ceil`、`round`、`trunc`、`log2`、`log10`、`hypot`、`min`、`max`、`len`、`range`、`print`、`println`、`list`、`dict`、`int`、`float`、`bool`、`str`、`kind_of`、`type_name`、`type_package`、`type_module_name`、`type_source`、`format`、`read_file`、`write_file`、`append_file`、`file_exists`、`file_open`、`file_read`、`file_write`、`file_seek`、`file_tell`、`file_eof`、`file_close`、`file_flush`、`input`、`split`、`split_ws`、`trim`、`find`、`substr`、`bytes_from_string`、`bytes_to_string`、`bytes_len`、`bytes_get`、`bytes_slice`、`bytes_concat`、`bytes_hex`、`bytes_from_hex`、`bytes_to_int`、`int_to_bytes`、`read_bytes`、`write_bytes`、`append_bytes`、`file_read_bytes`、`file_write_bytes`。内置函数同样是一等值：

```text
let f = sqrt;
f(9)

func apply(fn, x) { return fn(x); }
apply(sqrt, 16)
```

`std.iter.for_each(iterable, fn)` 逐个调用 `fn`，忽略返回值并返回 `null`。回调可以写 1 个或 2 个参数：1 个参数时接收元素（dict 为 key）；2 个参数时 list / string / bytes / set / deque / heap / generator 接收 `(index, value)`，dict 接收 `(key, value)`。回调里的 `break` / `continue` 不可用，`return` 只退出回调本身；返回 task 的回调不会被自动等待。

函数调用会先按从左到右的顺序求值参数，再检查被调用者类型和参数个数，因此参数中的副作用总会执行。

参数表达式中的 `return` / `break` / `continue` 会向外传播，当前函数调用不会执行。

极深值有保护上限：`print` / `str` 超过 100 层输出 `...`，`value_equals` 超过 512 层返回 `false`，`freeval` 超过 1000 层停止释放，JSON 深度上限 512。

字符串支持常用转义：`\n`、`\t`、`\r`、`\\`、`\"`、`\'`。

## 语法约定

- 顶层可以直接写表达式，不需要分号。
- 块 `{ ... }` 内的普通表达式语句需要以 `;` 结尾。
- `[]` 和 `{}` 分别表示空列表和空字典；空字典可以跨行书写，也可以包含注释。
- `{}` 出现在控制流/函数体位置时仍表示空代码块，例如 `if (x) {}`、`func f() {}`、`while (x) {}`。
- 块内单独出现的 `{}` 按空字典表达式处理；需要空代码块语句时写 `{ ; }`。
- 空字典和空代码块都可以跨行书写，也可以包含注释。
- 比较运算符包括 `>`、`<`、`<>`/`!=`、`==`、`>=`、`<=`。
- `//` 开始单行注释。
- 花括号、方括号、圆括号内部的换行会被自动忽略，支持多行表达式。
- 控制流和声明体的 `{` 必须和头部在同一行；`else` 必须和前面的 `}` 在同一行；`do ... while` 的 `while` 必须和 `}` 在同一行。
- 闭包捕获定义处的词法作用域；闭包环境会在程序退出时统一清理。
- 用户函数调用深度上限为 1000，超出时报告 `maximum recursion depth`。

## 类型与转换

- `int` 是 64 位有符号整数；十进制整数字面量超出 64 位范围时按 `float` 解析，`0x` / `0b` / `0o` 字面量超出 `int64` 范围时报词法错误，整数运算溢出会报 `integer overflow`。
- 整数字面量支持 `0x` / `0X`、`0b` / `0B`、`0o` / `0O` 进制前缀，浮点字面量支持科学计数法（如 `1e3`、`1.5e-2`）；数字之间可以用 `_` 分隔，例如 `1_000_000`、`0xFF_FF`、`1.5e1_0`。下划线只能位于两个数字之间。
- `int` 与 `float` 混合运算提升为 `float`，例如 `1 + 2.5` 得到 `3.5`；`1 == 1.0` 为真。超过 2^53 的整数与 float 比较仍按数学值精确判断，但转成 `float`（混合运算或 `float()`）时会丢失精度。
- 整数除法向零截断，取模结果符号随被除数：`-7 / 4 == -1`，`-7 % 4 == -3`，`7 % -4 == 3`。
- 位运算按 64 位进行，移位次数必须在 `0..63` 之间。
- `bool` 类型有 `true` / `false` 两个字面量；`bool(x)` 按真值规则转换。
- 真值规则：`null`、`false`、`0`、`0.0`、空字符串、空 bytes、空列表、空字典、空 deque、空 heap、空 set 为假；其余值（包括 `true`、函数和内置函数）为真。
- `str(true)` 得到 `"true"`，`int(true)` 得到 `1`，`float(false)` 得到 `0.0`。
- `null` 是空值字面量，表示“没有值”：没有 `return` 的用户函数、`print` / `println` 和 `return;` 都返回 `null`。顶层 `null` 结果不会显示，但可以赋值、比较、传递，或通过 `print(null)` / `format("{}", null)` 输出。
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

## 对象生命周期

- 普通值由引用计数管理；`list` / `dict` / `struct` / `bytes` 的 payload 会被别名共享，写入时才分离；`string` / `bytes` 使用引用计数缓冲；
- 函数值复制时共享函数代码和闭包环境，文件句柄等原生资源共享底层句柄；
- 闭包按引用捕获定义处的作用域，因此局部变量会在闭包存活期间继续存在；
- 运行时使用**作用域粒度的标记式环回收器**（`gc.cpp`）：从全局作用域、当前作用域链和恢复栈出发，标记可达 scope，再清理不可达 scope 的符号表；
- 回收每 64 个顶层语句或 scope 数量明显增长时执行，只在顶层语句边界进行；
- 值遍历覆盖 `list` / `dict` / `struct` / `enum` / `lru` / `result` / `method`，所以闭包即使只被 enum、LRU 或 result 持有，也不会被误回收；
- 这不是完整的逐值 tracing GC：值本身仍由引用计数管理，没有压缩、移动或 finalizer；
- 如果未来原生资源也能保存语言值，需要扩展回收器的遍历规则。

## 格式化

`format(fmt, ...)` 返回格式化后的字符串；`print` 支持两种参数形式：

- `print(x)`：直接打印一个值；
- `print(fmt, ...)`：第一个参数是格式串，其余参数用于格式化；
- `print()`：输出一个空行。

示例：

```text
format("x = {}", 3)
format("{} + {} = {}", 1, 2, 3)
format("{1} {0}", "a", "b")
format("{{}} = {}", 1)
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

## 外部命令

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
bytes_to_string(r["stdout"])
```

### 编码进程入口

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
func worker(ctx) {
  return {"v": ctx.payload["n"] + 1};
}
let blob = std.entry.encode(worker);
let r = std.process.run_entry(blob, std.thread.Message.from({"n": 4}));
r["value"]["v"]   // 5
```

普通单参数函数即可作为进程入口：

```skein
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

### 长驻进程与管道

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

print(bytes_to_string(std.io.read(p["stdout"], 4096)));
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

#### JSON Lines worker 示例

worker 脚本：

```text
import std;

while (true) {
  let line = std.io.read_line(std.io.stdin);
  if (line == null) { break; }

  let request = std.json.parse(bytes_to_string(line));
  let result = request["params"][0] + request["params"][1];
  let response = {"id": request["id"], "ok": true, "result": result};

  std.io.write_all(std.io.stdout,
                   bytes_from_string(std.json.stringify(response) + "\n"));
  std.io.flush(std.io.stdout);
}
```

父进程：

```text
import std;

let p = std.process.spawn_command("skein", {"args": ["worker.skein"]});
std.io.write_all(p["stdin"],
                 bytes_from_string("{\"id\":1,\"params\":[2,3]}\n"));
std.io.flush(p["stdin"]);

print(bytes_to_string(std.io.read_line(p["stdout"])));
std.io.close(p["stdin"]);
print(std.process.wait(p));
```

### Worker 池

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
let line = std.io.read_line(std.io.stdin);
let request = std.json.parse(bytes_to_string(line));
let body = bytes_from_string("");
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
                 bytes_from_string(std.json.stringify(response) + "\n"));
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
                 bytes_from_string("{\"ready\":true,\"version\":1}\n"));
std.io.flush(std.io.stdout);

while (true) {
  let line = std.io.read_line(std.io.stdin);
  if (line == null) { break; }

  let request = std.json.parse(bytes_to_string(line));
  let result = request["params"][0] + request["params"][1];
  let response = {"id": request["id"], "ok": true, "result": result};

  std.io.write_all(std.io.stdout,
                   bytes_from_string(std.json.stringify(response) + "\n"));
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

## 字符串处理

```text
trim(text)                  → 去掉首尾空白
split(text, separator)      → list<string>
split_ws(text)              → list<string>
find(text, substring)       → 码点下标 / -1
substr(text, start[, length]) → string
```

示例：

```text
trim("  hello\t\n")          → "hello"
split("a,b,c", ",")          → ["a", "b", "c"]
split(",a,", ",")            → ["", "a", ""]
split_ws("  a  b\tc ")       → ["a", "b", "c"]
find("hello", "ll")          → 2
find("hello", "x")           → -1
substr("hello", 1, 3)        → "ell"
substr("你好世界", 1, 2)      → "好世"
```

规则：

- `trim` 和 `split_ws` 只把 ASCII 空白（空格、Tab、换行等）视为空白；
- `split` 使用非重叠匹配，保留首尾空字段；
- `split` 的分隔符不能为空字符串；
- `find` 返回 **UTF-8 码点下标**，找不到返回 `-1`，空子串返回 `0`；
- `substr` 的 `start` 和 `length` 都是码点单位；`start` 超出长度返回空字符串，`length` 超出末尾会截断；
- `start` 和 `length` 不能为负数。

组合使用：

```text
let parts = split_ws(input("请输入两个数: "));
let a = int(parts[0]);
let b = int(parts[1]);
print(a + b);
```

## 控制台输入

`input([prompt])` 从 stdin 读取一行：

```text
let name = input("Name: ");
print("hello " + name);
```

- 返回的字符串不包含末尾换行；
- 可选的 `prompt` 会原样输出到 stdout，不经过 `format`；
- 读到 EOF 时返回 `null`；
- 空行返回空字符串；
- CRLF 会被处理，末尾 `\r\n` 不会进入结果；
- 控制台负责行编辑和退格；管道输入中的退格会由 `input` 过滤；
- 脚本模式下仍然从原始 stdin 读取，不会读脚本文件本身；
- `input` 是普通一等内置函数，可以保存和传递。

```text
let read = input;
read("> ")
```

示例：

```text
let name = input("Name: ");
print(format("hello {}", name));
```

## 文件读写

```text
read_file(path)          → 文件全部内容
write_file(path, text)   → null，覆盖写入
append_file(path, text)  → null，追加写入
file_exists(path)        → 1 / 0
```

示例：

```text
write_file("out.txt", "hello\n");
append_file("out.txt", "world\n");
print(read_file("out.txt"));

file_exists("out.txt")
```

规则：

- `read_file` 读取整个文件并返回字符串；空文件返回空字符串；
- `write_file` 创建或截断文件；
- `append_file` 创建或追加；
- 文件以二进制模式打开，不会自动转换 CRLF，也不会去掉末尾换行；
- 读写内容按 UTF-8 文本处理；
- `read_file` 遇到 NUL 字节会报错，避免内部 C 字符串截断；读取二进制请使用 `read_bytes`；
- 不会自动创建父目录；
- 文件操作失败时报告 `read_file: ...` / `write_file: ...` / `append_file: ...`；
- Windows/MinGW 下文件路径会转换为 UTF-16 后使用 `_wfopen`。

### 带编码的文本文件

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
- 原有的 `read_file` / `write_file` / `file_read` / `file_write` 仍然只按 UTF-8 字节处理；
- 文件句柄 API 不携带编码，需要其它编码时使用 `read_bytes` / `write_bytes` 或 `std.fs.read_text` / `write_text`。

### 文件句柄

需要多次操作同一个文件时，可以使用低层文件句柄：

```text
file_open(path, mode)       → file handle
file_read(handle[, n])      → string
file_write(handle, text)    → null
file_seek(handle, offset[, whence]) → null
file_tell(handle)           → int
file_eof(handle)            → 1 / 0
file_close(handle)          → null
file_flush(handle)          → null
```

`mode` 支持 `r`、`w`、`a`、`r+`、`w+`、`a+`，也接受带 `b`/`t` 的写法；内部统一按二进制模式打开。

示例：

```text
let h = file_open("data.txt", "w+");
file_write(h, "hello world");
file_seek(h, 0);
file_read(h, 5)
file_close(h)
```

- 文件句柄是不可打印底层指针的资源对象，打印显示为 `<file: path>`；
- 句柄复制会共享同一个底层文件，关闭其中一个会影响所有副本；
- `file_read(h)` 读取剩余全部内容，`file_read(h, n)` 最多读取 n 字节；`n = 0` 等价于读取全部；
- `file_seek` 的 `whence`：`0` 文件开头、`1` 当前位置、`2` 文件末尾；
- 关闭后继续读写会报 `file is closed`；
- `file_read` 读取到 NUL 字节会报错；读取二进制请使用 `file_read_bytes`；
- 最终未关闭的句柄会随进程退出关闭。

## 字符、字节与编码


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
- `bytes_from_char(c)` / `char_from_bytes(b)` / `char(b)` 做 UTF-8 字节互转；
- JSON 编码为单字符字符串；
- 空、多字符、代理区、超过 `0x10FFFF` 报错。

### 字节值 byte

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
- `bytes[i]` / `bytes_get` 仍返回 `int`。

### 二进制 buffer

文本字符串按 UTF-8 处理，不能保存任意字节。二进制数据使用独立的 `bytes` 类型：

```text
bytes_from_string(text)        → bytes
bytes_to_string(bytes)         → string
bytes_len(bytes)               → int
bytes_get(bytes, index)        → 0..255
bytes_slice(bytes, start[, length]) → bytes
bytes_concat(a, b)             → bytes
bytes_hex(bytes)               → 十六进制字符串
bytes_from_hex(text)           → bytes
bytes_to_int(bytes, size, byteorder[, offset[, signed]]) → int
int_to_bytes(value, size, byteorder) → bytes
read_bytes(path)               → bytes
write_bytes(path, bytes)       → null，覆盖写入
append_bytes(path, bytes)      → null，追加写入
file_read_bytes(handle[, n])   → bytes
file_write_bytes(handle, bytes) → null
bytes[index]                   → 0..255
bytes[index] = 0..255
```

示例：

```text
let b = bytes_from_hex("00ff10");
bytes_len(b)          → 3
bytes_hex(b)          → "00ff10"
bytes_get(b, 1)       → 255
bytes_hex(bytes_slice(b, 1, 2)) → "ff10"

let c = bytes_from_string("abc");
c[1] = 255;
bytes_hex(c)          → "61ff63"

bytes_to_int(bytes_from_hex("78563412"), 4, "little")
                      → 305419896
bytes_to_int(bytes_from_hex("78563412"), 4, "big")
                      → 2018915346
bytes_to_int(bytes_from_hex("ffff"), 2, "little", 0, 1)
                      → -1
bytes_hex(int_to_bytes(305419896, 4, "little"))
                      → "78563412"
bytes_hex(int_to_bytes(-1, 2, "little"))
                      → "ffff"
```

规则：

- `bytes` 可以保存任意字节，包括 `0x00`，长度由字节数决定；
- 直接打印 `bytes` 时显示为 `<bytes: N>`，不会把原始二进制写到终端；需要查看内容时使用 `bytes_hex`；
- `bytes_len`、`bytes_get`、`bytes_slice`、索引和 `file_seek` 都使用**字节下标**，不是 UTF-8 码点；
- 索引读取返回 `0..255` 的整数，索引赋值只接受 `0..255`；
- `bytes_slice` 的 `start` / `length` 不能为负数，越界会截断；
- `bytes_from_hex` 只接受偶数个十六进制数字，大小写均可；
- `bytes_to_int` 的 `size` 为 1..8，`byteorder` 支持 `"little"` / `"big"`（也接受 `"le"` / `"be"`）；
- `bytes_to_int` 的 `offset` 默认 0，`signed` 默认 0；`offset + size` 超出 buffer 会报错，不会静默截断；
- `bytes_to_int(..., signed = 1)` 按二补码解释；无符号 64 位值超过 `int64` 上限时会报错；
- `int_to_bytes` 的 `size` 为 1..8；值可以落在有符号下限到无符号上限之间，按低 `size` 字节编码；
- 负数写入使用二补码，例如 `int_to_bytes(-1, 2, "little")` 得到 `ff ff`；
- `bytes_to_string` 要求内容是合法 UTF-8 且不含 NUL，否则报错；
- `read_bytes` / `write_bytes` / `append_bytes` 不做文本 NUL 检查；
- `file_read_bytes(h)` 读取剩余全部内容，`file_read_bytes(h, n)` 最多读取 n 字节；
- `bytes` 是值类型，赋值和复制都会复制内容。

### 编码转换

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

let s = std.encoding.decode(bytes_from_hex("d6d0b9fa"), "gbk");
print(s);                                      // 中国

print(bytes_hex(std.encoding.encode("中国", "gbk")));
                                               // d6d0b9fa

print(bytes_hex(std.encoding.encode("€", "gbk")));
                                               // 80

print(bytes_hex(std.encoding.encode("€", "gb18030")));
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
- `bytes_to_string` 仍然是严格 UTF-8 的快捷方式，不参与 GBK / GB18030 转换。

### Unicode 归一化

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

let decomposed = std.encoding.decode(bytes_from_hex("65cc81"), "utf-8");
let composed = std.unicode.normalize(decomposed, "nfc");
bytes_hex(std.encoding.encode(composed, "utf-8"))  // c3a9

std.unicode.is_normalized(decomposed, "nfc")       // false
std.unicode.is_normalized(composed, "nfc")         // true

let circled = std.encoding.decode(bytes_from_hex("e291a0"), "utf-8");
std.unicode.normalize(circled, "nfkc")             // "1"
std.unicode.version()                              // "16.0.0"
```

规则：

- 归一化只在显式调用 `std.unicode.normalize` 时发生；
- 字符串比较、拼接、下标和 `len` 不会自动归一化；
- 标识符按 NFC 归一化，不做 NFKC；
- 输入必须是合法 UTF-8 `string`，输出也保持合法 UTF-8；
- 归一化不会产生内嵌 NUL。

## 正则表达式


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

## 时间、随机数和 UUID


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

## Base64、URL 和哈希


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

## 环境变量和进程捕获


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
print(bytes_to_string(r["stdout"]));

let r2 = std.process.run_command("cat", {"stdin": "abc"});
bytes_to_string(r2["stdout"])
```

注意：当前实现通过 shell 重定向捕获输出，不提供超时和流式读写。

## 目录、路径和压缩


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
let h = file_open("data.bin", "r+b");
file_seek(h, 8);
file_write_bytes(h, bytes_from_hex("00ff"));
file_seek(h, 0);
bytes_hex(file_read_bytes(h, 16))
file_close(h)
```

读取固定宽度整数：

```text
let h = file_open("data.bin", "rb");
let magic = bytes_to_int(file_read_bytes(h, 4), 4, "big");
let length = bytes_to_int(file_read_bytes(h, 4), 4, "little");
file_close(h);
```

## 用 dict + 闭包封装文件对象


当前语言没有方法绑定，但可以用 dict + 闭包实现类似接口：

```text
func open_file(path) {
  let h = file_open(path, "r");

  func read(n) { return file_read(h, n); }
  func seek(n) { return file_seek(h, n); }
  func close() { return file_close(h); }

  return {"read": read, "seek": seek, "close": close};
}

let f = open_file("data.txt");
f["read"](10)
f["close"]()
```

底层文件资源使用通用的原生 userdata 生命周期管理；上层对象接口可以用语言本身扩展，不需要修改内置实现。

文件函数可以访问进程有权限的任意路径，只应在可信脚本中使用。


## 线程、Channel 与并发


```text
std.thread.Channel(capacity) -> channel
std.thread.Channel.bounded(capacity) -> channel
std.thread.Channel.new(capacity) -> channel
std.thread.Channel.unbounded() -> channel
std.thread.Message.from(payload[, handles]) -> message

channel.send(message)
channel.try_send(message) -> bool
channel.recv([timeout_ms]) -> message | null
channel.try_recv() -> {"ok": bool, "closed": bool, "value": message?}
channel.close()

channel.closed
channel.capacity
channel.length
channel.waiters
channel.send_waiters
channel.recv_waiters
channel.total_sends
channel.total_recvs
channel.send_timeouts
channel.recv_timeouts
channel.stats -> dict
```

- `capacity >= 1` 为有界队列；`capacity == 0` 为 rendezvous；`capacity == -1`
  为无界队列；
- `Channel.unbounded()` 返回无界 Channel，`capacity == -1`；
  `Channel.bounded(n)` / `Channel.new(n)` 等价于 `Channel(n)`；
  旧名 `unbounded_channel()` 已移除；
- Channel 只接收 `std.thread.Message`；
- `Message.from` 共享 payload（COW）；发送方后续写入会分离，不影响消息；
- payload 支持标量、`error` / `result`、`list` / `dict`、`lru` / `deque` / `set`、`struct` / `enum`；`heap` 因比较器函数不能跨线程，暂不支持；
- `handles` 是字符串到线程 handle 的 dict，目前支持 `Channel`；
- `send` 在队列满时阻塞；rendezvous（capacity 0）下阻塞到有接收者等待；
  `try_send` 不阻塞，无空间/无接收者时返回 `false`；
- `recv` 在队列空时阻塞；关闭且排空后返回 `null`；
- `recv(timeout_ms)` 超时抛错；
- `close()` 幂等，保留队列中未读消息，并唤醒所有 send/recv/select 等待者；
- fiber 中的 Channel send/recv/select 通过事件循环 async wake 挂起/唤醒；
  非 fiber 上下文使用 OS 线程等待；
- 等待者按 FIFO 唤醒（OS 线程等待者优先于 fiber 等待者）；
- `std.thread.select` 使用 round-robin 起点，避免固定顺序造成饥饿；
- `waiters` / `send_waiters` / `recv_waiters` 返回当前等待者数量；
- `total_sends` / `total_recvs` / `send_timeouts` / `recv_timeouts` 为累计统计；
- `stats` 返回包含上述字段的 dict；

示例：

```skein
import std;

let ch = std.thread.Channel(2);
let msg = std.thread.Message.from({"x": 1});
ch.send(msg);

let r = ch.try_recv();
print(r["value"].payload["x"]);
print(ch.stats["total_sends"]);

ch.close();
```

无界与 rendezvous：

```skein
let a = std.thread.Channel.unbounded();
let b = std.thread.Channel(0);   // send 等待 recv
```

### 线程池

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

### 单线程

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
  write_bytes("entry.bin", std.entry.encode(worker));
  # 另一个进程：
  let t = std.thread.spawn_entry(read_bytes("entry.bin"), msg);
  ```
  当前可序列化的 Proto 支持入口内的局部 `struct` / `enum` / `trait` / `impl` 声明、嵌套函数和 `defer`；结构/枚举字段默认值和 impl 方法会随声明元数据传递，局部方法参数默认值当前仍受限。data 值支持 null/bool/int/float/char/byte/string/bytes/error/result/list/dict/set/deque/lru、struct/enum（字段/variant 元数据随 payload 注册；字段默认值，以及带默认/模式/可变参数、数据捕获和全局数据依赖的方法也会传输；方法若捕获函数/heap/命名空间，当前会给出明确诊断并拒绝）以及 builtin 函数常量。`list` / `dict` / `set` / `deque` / `lru` 现在会递归携带嵌套值，因此可以跨线程/进程传递函数表、heap 容器和多层混合容器。

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

let ch = std.thread.Channel(1);
let msg = std.thread.Message.from(
    {"x": 5},
    {"signal": ch}
);

let t = std.thread.spawn_script("thread_worker.skein", msg);
let signal = ch.recv();
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

### 双向 duplex

```text
std.thread.duplex(capacity) -> link
link.child -> endpoint

link.send(message)
link.try_send(message) -> bool
link.recv([timeout_ms]) -> message | null
link.try_recv() -> {"ok": bool, "closed": bool, "value": message?}
link.close_send()
link.close_recv()
link.close()
link.closed
link.capacity
link.length

std.thread.spawn_script_with_channel(script, message, capacity[, options]) -> thread
std.thread.spawn_script_with_duplex(script, message, capacity[, options]) -> thread
thread.channel
thread.link
```

- `duplex` 内部有 `parent_to_child` 和 `child_to_parent` 两条 lane；
- `link.send` 发往 child，`link.recv` 接收 child 发来的消息；
- `link.child` 是反方向端点：`child.send` 发往 parent，`child.recv` 接收 parent 的消息；
- `close_send` 只关闭 out 方向；对端 `recv` 排空后得到 `null`；
- `close_recv` 只关闭 in 方向；`closed` 反映 in 方向是否关闭；
- `select` 接受 Channel 或 duplex endpoint，endpoint 使用其 in 方向；`broadcast` 仅接受 Channel；
- `spawn_script_with_channel` 自动创建 Channel 并注入 `ctx.handles["channel"]`，父端通过 `thread.channel` 访问；
- `spawn_script_with_duplex` 自动创建 duplex，子端注入 `ctx.handles["link"]`，父端通过 `thread.link` 访问。
- `std.thread.collect()` 主动回收不可达的 Message/Channel/duplex 环；仅在没有活动 worker 线程时执行。
- `std.thread.debug_counts()` 返回 `messages/channels/duplexes/endpoints/collected` 诊断计数。

### select / broadcast

```text
std.thread.select(channels[, timeout_ms]) -> dict
std.thread.try_broadcast(channels, message) -> dict
std.thread.broadcast(channels, message) -> null
```

`select` 返回：

```text
{"index": 0, "value": message}
{"index": 1, "closed": true}
{"index": -1, "timeout": true}
```

示例：

```skein
let a = std.thread.Channel(1);
let b = std.thread.Channel(1);
b.send(std.thread.Message.from({"v": "b"}));

let r = std.thread.select([a, b], 1000);
print(r["index"]);
print(r["value"].payload["v"]);
```

规则：

- 当前只支持 Channel；
- 多个 channel 同时就绪时使用轮转起点，避免总是选 0 号；
- 有数据优先，排空后才返回 closed；
- `timeout_ms = 0` 表示不超时；
- `try_broadcast` 返回：
  ```text
  {"sent": [0, 1], "full": [], "closed": []}
  ```
- `broadcast` 逐个阻塞发送；任一失败时抛错，已发送的不回滚。

### 能力查询

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

## 错误信息

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

### 错误恢复

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

### 退出码

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
