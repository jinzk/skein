# calc-demo 语法参考

本文只描述当前实现支持的语言语法和基本语义。包查找、文件 API 等细节可结合 `README.md` 阅读。

## 1. 词法

### 1.1 标识符

```text
[a-zA-Z_][a-zA-Z0-9_]*
```

标识符区分大小写，只能使用 ASCII 字母、数字和下划线。

### 1.2 关键字

```text
let const func
if else while do for
switch case default
struct impl trait
return break continue
fail on error recover pass
null true false
package import export as
```

`def` 已不再使用，匿名/命名函数统一使用 `func`。

内置函数名也会被词法器识别为函数值，因此不能用作普通变量名，例如 `sqrt`、`print`、`read_file`。

### 1.3 注释

```text
// 单行注释
```

注释到行尾结束。

### 1.4 空白和换行

- 空格、Tab、CR 会被忽略；
- 在 `()`、`[]`、`{}` 内部，换行会被忽略，因此可以多行书写表达式和字面量；
- 在括号外的换行通常表示语句结束；
- 同级语句可以用分号分隔；
- 脚本第一行的 `#!` shebang 会被跳过；
- 文件开头的 UTF-8 BOM 会被跳过。

## 2. 字面量

### 2.1 整数

```text
123
0
-42
```

整数是 64 位有符号整数。超出 `int64` 范围的整数字面量会按浮点数解析。

### 2.2 浮点数

```text
1.0
1.
.5
1e3
1.5e-2
```

### 2.3 字符串

```text
"hello"
"line\nnext"
"tab\there"
'单引号字符串'
```

支持的转义：

```text
\n
\t
\r
\\
\"
\'
```

字符串按 UTF-8 解释，内部不能包含 NUL 字节。二进制数据请使用 `bytes`。

### 2.4 null

```text
null
```

`null` 是正常值，不等于“错误”。没有 `return` 的函数和 `print` 返回 `null`。

### 2.5 列表

```text
[]
[1, 2, 3]
[1, [2, 3], "x"]
```

列表字面量允许尾随逗号。函数参数和调用参数也允许尾随逗号；字典字面量不允许。

### 2.6 字典

```text
{"x": 1, "y": 2}
{1: "one", 2: "two"}
```

空字典可以直接使用 `{}`，也可以使用 `dict()`：

```text
{}
dict()
```

`{}` 在表达式/值位置表示空字典；在控制流或函数体位置表示空代码块：

```text
let d = {};      // 空字典
if (x) {}        // 空代码块
func f() {}      // 空函数体
while (x) {}     // 空循环体
```

需要显式的空代码块时也可以写 `{ ; }`。空字典可以跨行书写，也可以包含注释：

```text
let d = {
  // empty dict
};
```

### 2.7 bytes

`bytes` 没有字面量，通过内置函数创建：

```text
bytes_from_string("abc")
bytes_from_hex("00ff10")
read_bytes("data.bin")
```

`bytes` 可以包含任意字节，包括 `0x00`。

### 2.8 函数值

命名函数和匿名函数都是一等值：

```text
func add(a, b) { return a + b; }
let double = func(x) { return x * 2; };
```

## 3. 类型和真值

当前类型：

```text
int
float
bool
string
bytes
list
dict
function
builtin
userdata
null
```

布尔字面量：

```text
true
false
```

真值规则：

- 假：`null`、`false`、`0`、`0.0`、空字符串、空 bytes、空列表、空字典；
- 真：其他值，包括 `true`、函数、内置函数和 userdata（例如文件句柄）。

`bool(x)` 按真值规则转换为 `true` / `false`。`str(true)` 得到 `"true"`，`int(true)` 得到 `1`，`float(false)` 得到 `0.0`。

## 4. 表达式

### 4.1 算术

```text
1 + 2
3 - 4
5 * 6
7 / 2
7 % 2
```

规则：

- `int` 与 `float` 混合运算提升为 `float`；
- 整数除法向零截断；
- 整数取模结果符号随被除数；
- 浮点取模使用 `fmod` 语义；
- 除零和模零会报运行时错误；
- `+` 支持字符串拼接和列表拼接。

### 4.2 比较

```text
a == b
a != b
a <> b
a < b
a <= b
a > b
a >= b
```

- `==` / `!=` 支持所有类型；
- 大小比较只支持数值之间或字符串之间；
- 所有比较运算返回 `bool`；
- `<>` 是 `!=` 的别名；
- 比较运算符不能连续使用，例如 `a < b < c` 是语法错误。

### 4.3 逻辑运算

```text
a && b
a || b
!a
```

- `&&` / `||` / `!` 返回 `bool`，并短路求值；
- 真值规则见上一节。

### 4.4 位运算

```text
a & b
a | b
a ^ b
~a
a << b
a >> b
```

- 只支持整数；
- 移位次数必须在 `0..63`；
- 按 64 位整数处理。

### 4.5 三元表达式

```text
condition ? when_true : when_false
```

示例：

```text
x > 0 ? "positive" : "non-positive"

1 ? 2 : 3                 // 2
0 ? 2 : 3                 // 3
0 ? 1/0 : 5               // 5，未选中的分支不会求值
```

- 条件按真值规则判断；
- 只求值被选中的分支；
- `? :` 是右结合，可以嵌套：

```text
a ? b : c ? d : e
// 等价于 a ? b : (c ? d : e)
```

- 优先级低于 `||`，高于赋值。

### 4.6 索引和字段

```text
a[0]
d["key"]
obj.field
a[0][1]
d["x"].field
```

- list 下标必须是整数；
- string 下标按 UTF-8 码点计数，返回单个字符的字符串，字符串索引不能赋值；
- dict 键按值相等判断；
- bytes 下标是字节下标，返回 `0..255`，索引赋值接受 `0..255`；
- 字段访问等价于字符串键索引。

### 4.7 函数调用

```text
f()
f(1, 2)
f(1, 2,)
obj["fn"](3)
```

函数调用参数从左到右求值。

### 4.8 匿名函数

```text
let add = func(a, b) { return a + b; };

(func(x) { return x + 1; })(41)
```

匿名函数体使用 `list` 语法，支持多语句和显式 `return`。

### 4.9 赋值

```text
x = 1
a[0] = 2
d["key"] = 3
d.key = 4
```

赋值是右结合表达式：

```text
a = b = 1
```

不能给字面量或表达式结果赋值。

`const` 绑定不能重新赋值：

```text
const x = 1;
x = 2;              // cannot assign to constant x
```

`const` 只约束绑定，容器内容仍可修改：

```text
const a = [1, 2];
a[0] = 9;           // 允许
```

### 4.10 自增和自减

```text
x++
++x
x--
--x
```

示例：

```text
let i = 0;
i++               // 返回 0，i 变为 1
++i               // 返回 2，i 变为 2
i--               // 返回 2，i 变为 1
--i               // 返回 0，i 变为 0
```

规则：

- 后缀 `x++` / `x--` 返回修改前的值；前缀 `++x` / `--x` 返回修改后的值；
- 只支持 `int` 和 `float`；
- 目标必须是可赋值的位置：变量、list/dict/bytes 下标或字段；
- 下标表达式只求值一次，例如 `a[i++]++` 中的 `i++` 只执行一次；
- `const` 变量不能自增/自减；`const` 容器中的元素可以；
- 整数溢出、bytes 超出 `0..255`、dict 键不存在会报运行时错误。

```text
let a = [1, 2];
a[0]++;

let d = {"n": 1};
d.n++;

let b = bytes_from_hex("00ff");
b[1]--;
```

### 4.11 运算优先级

从低到高：

```text
=
? :
||
&&
|
^
&
比较运算
<< >>
+ -
* / %
一元 - ! ~
前缀 ++ --
索引/字段/调用
后缀 ++ --
```

## 5. 语句

### 5.1 表达式语句

顶层表达式可以省略结尾分号：

```text
1 + 2
```

代码块中的表达式语句需要分号：

```text
{
  1 + 2;
  print("ok");
}
```

`if` / `while` / `for` / `func` / `package` 的块结束处不需要分号。

### 5.2 变量声明

```text
let x = 1;
let name: string = "alice";
let count: int;
const y = 2;
const limit: int = 10;
```

- 无类型标注时必须提供初始值；
- 带类型标注时可以省略初始值，使用该类型的默认值；
- 初始化和重新赋值都会进行运行时类型检查，不做隐式转换；
- 同一作用域不能重复声明同名变量；
- 嵌套作用域允许遮蔽外层变量；
- `const` 绑定不可重新赋值；
- `let` / `const` 都是块级作用域；
- 初始化失败或类型不匹配时不会创建绑定。

支持的类型：

```text
any int float number bool string bytes
list dict function builtin callable null userdata
```

默认值：

| 类型 | 默认值 |
|---|---|
| `int` / `number` | `0` |
| `float` | `0.0` |
| `bool` | `false` |
| `string` | `""` |
| `bytes` | 空 bytes |
| `list` | `[]` |
| `dict` | `{}` |
| `any` / `null` | `null` |

`function`、`builtin`、`callable`、`userdata` 没有默认值，必须提供初始值。

已声明的 `struct` / `trait` 类型也可以用于类型标注，但没有默认值，必须提供初始值：

```text
let x: int = 1;
x = 2;              // 允许
x = "two";          // 错误：variable x expects int, got string

let s: string;
s == ""             // true
```

类型标注只约束绑定，不约束容器内部元素：

```text
let a: list = [0];
a[0] = "x";         // 允许
```

`for` 初始化部分也支持类型标注：

```text
for (let i: int = 0; i < 3; i++) {
  print(i);
}
```

### 5.3 条件语句

```text
if (condition) {
  ...
}

if (condition) {
  ...
} else {
  ...
}

if (a) {
  ...
} else if (b) {
  ...
} else {
  ...
}
```

条件必须放在圆括号中。`if` / `while` / `for` / `do` 的主体可以是代码块，也可以是单条语句；单条表达式语句需要分号。

### 5.4 while / do while

```text
while (condition) {
  ...
}

do {
  ...
} while (condition);
```

`do ... while` 至少执行一次循环体。`while` 可以写在 `}` 的下一行；结尾分号可以省略，也可以保留（会当作空语句）。

### 5.5 for

```text
for (let i = 0; i < 10; i = i + 1) {
  print(i);
}

for (;;) {
  break;
}
```

- 三个部分都可以为空；
- 初始化部分支持空、表达式或 `let` 声明；
- 条件和步进部分支持空或表达式。

`for ... in`：

```text
for (let x in [1, 2, 3]) {
  print(x);
}

for (let key in {"a": 1, "b": 2}) {
  print(key);
}

for (let key, value in {"a": 1, "b": 2}) {
  print(key + "=" + str(value));
}
```

规则：

- 单变量形式：`for (let NAME in exp)`；
- 双变量形式：`for (let NAME1, NAME2 in exp)`，只支持 dict；
- list 单变量遍历元素；需要下标时用 `range(len(list))` 或 C 风格 `for`；
- dict 单变量遍历 key，双变量得到 `key, value`；
- string 单变量遍历 UTF-8 字符；
- bytes 单变量遍历 `0..255` 的字节值；
- 可以配合 `range()` 做计数循环；
- 被遍历对象在循环开始时求值一次；
- 循环变量只存在于循环作用域；
- 支持 `break` / `continue` / `return`。

### 5.6 switch

```text
switch (value) {
  case 1 => print("one");

  case 2, 3 => {
    print("two or three");
  }

  default => print("other");
}
```

规则：

- `switch` 的选择值只求值一次；
- 按 case 顺序匹配，匹配成功后不再尝试后续 case；
- 没有 fallthrough，不需要用 `break` 阻止贯穿；
- `case` 值用逗号分隔，例如 `case 1, 2, 3`；
- case 体可以是单条语句或代码块，并且有独立作用域；
- `default` 可选，只能出现一次，且必须放在最后；
- `break` 退出当前 switch；
- `continue` 作用于外层循环；
- `return` 退出函数。

支持的 switch 值类型：

```text
int
bool
string
bytes
null
```

不支持：

```text
float
list
dict
function
builtin
userdata
```

浮点相等容易产生 `0.1 + 0.2 != 0.3`、`NaN` 等问题，因此 switch 不支持 float；浮点判断请使用 `if/else`。

case 值按值比较，使用 `==` 语义；case 值本身不能是逻辑或比较表达式：

```text
switch (x) {
  case x > 1 && x < 5 => ...   // 错误：case 值不能是条件表达式
}
```

需要条件判断时使用 `if / else if`。当前不支持 `case int` 这种类型模式；`int` 只是内置函数值，会作为不支持的 case 值报错。

示例：

```text
switch (bytes_from_hex("00ff")) {
  case bytes_from_hex("00ff") => print("bytes match");
}

switch (null) {
  case null => print("null");
  default => print("other");
}
```

### 5.7 代码块

```text
{
  let x = 1;
  print(x);
}

{ ; }
```

空代码块可以写在控制流/函数体位置（如 `if (x) {}`、`func f() {}`），也可以显式写成 `{ ; }`。块内单独出现的 `{}` 按空字典表达式处理；表达式位置的 `{}` 也是空字典。

### 5.8 return / break / continue

```text
return;
return value;
break;
continue;
```

- `return` 只能用于函数；
- `break` 可用于循环或 switch；
- `continue` 只能用于循环；
- 都需要分号。

### 5.9 错误处理

抛出错误：

```text
func parse_positive(text) {
  let n = int(text);

  if (n <= 0) {
    fail "must be positive: " + str(n);
  }

  return n;
}
```

捕获错误：

```text
let n = parse_positive("0") on error(e) {
  print("bad input: " + str(e));
  recover 0;
};
```

规则：

- `fail expr;` 抛出错误值；
- `fail;` 抛出 `null`；
- `call() on error(e) { ... }` 捕获该调用表达式产生的显式 `fail`；
- `e` 是传给 `fail` 的原始值，只在 handler 作用域内有效；
- handler 必须以 `recover`、`pass` 或 `fail` 结束；
- `recover expr;` 结束 handler，并把 `expr` 作为整个 `on error` 表达式的值；
- `recover;` 等价于 `recover null;`；
- `pass e;` 继续向外层传播错误；
- handler 内不能直接用 `return`、`break` 或 `continue` 结束 handler；
- 第一版只捕获显式 `fail`，`1/0`、越界、类型错误等运行时错误仍然直接报错；
- `recover` / `pass` 只能出现在 handler 内；
- 未捕获的 `fail` 在脚本顶层报错并停止脚本。

示例：

```text
func load() {
  let n = parse_positive("-1") on error(e) {
    print("load failed: " + str(e));
    pass e;
  };

  return n;
}

load() on error(e) {
  print("top: " + str(e));
  recover 0;
};
```

错误值不要求是字符串，可以是任意值：

```text
fail 42;
fail {"code": 7, "message": "bad"};
```

### 5.10 命名函数

```text
func add(a, b) {
  return a + b;
}

func typed_add(a: int, b: int) {
  return a + b;
}

func hello() {
  print("hello");
}
```

- 参数用逗号分隔；
- 参数可以带可选的类型约束：`name: type`；
- 支持空参数；
- 不支持默认参数和可变参数；
- 函数体使用 `{ list }`；
- 函数调用深度上限为 1000。

参数类型约束在调用时进行运行时检查，不做隐式转换。支持：

```text
any int float number bool string bytes
list dict function builtin callable null userdata
```

示例：

```text
func apply(fn: callable, x: int) {
  return fn(x);
}

apply(sqrt, 16)
```

类型不匹配会报告运行时错误，例如：

```text
function add argument 1 (a) expects int, got string
```

匿名函数：

```text
let add = func(a, b) { return a + b; };
```

### 5.11 结构体、impl 与 trait

结构体：

```text
struct Point {
  x: int;
  y: int;

  func length(self) {
    return sqrt(self.x * self.x + self.y * self.y);
  }
}

impl Point {
  func moved(self, dx: int, dy: int) {
    return Point { x: self.x + dx, y: self.y + dy };
  }
}
```

构造和访问：

```text
let p = Point { x: 3, y: 4 };

p.x
p.x = 5;
p.length()
```

规则：

- 字段必须全部初始化；
- 未知字段、重复字段、字段类型不匹配都会报错；
- 字段顺序不影响构造；
- `struct` 内可以声明字段和方法；
- `impl Type` 用于在结构体外部补充该类型的固有方法；
- 方法第一个参数必须是显式 `self`；
- `self` 的类型隐式为当前结构体；
- 方法通过实例调用：`p.length()`，调用时不传 `self`；
- 暂不支持方法值提取，例如 `let f = p.length`；
- 第一版方法是只读的，不允许 `self.x = ...`；
- 结构体使用 copy-on-write 值语义；
- 所有成员都是公开的，`_` 前缀没有特殊含义；
- 字段类型必须是已声明的类型；暂不支持递归/自引用结构体；
- 字段可以保存 `callable` / `function` 值，作为普通回调使用，不会自动绑定 `self`；
- 不支持继承、静态方法、泛型、动态添加方法或运算符重载。

结构体类型可用于类型标注：

```text
let p: Point = Point { x: 1, y: 2 };

func show(p: Point) {
  print(p.x);
}
```

trait 定义接口：

```text
trait Drawable {
  func draw(self);
}
```

实现接口：

```text
impl Drawable for Point {
  func draw(self) {
    print("point");
  }
}
```

如果方法已在 `impl Point` 中定义，可以只做接口适配：

```text
impl Drawable for Point;
```

接口使用：

```text
func render(x: Drawable) {
  x.draw();
}

render(Point { x: 1, y: 2 });
```

规则：

- trait 只包含方法签名；
- 接口是名义式的，必须显式 `impl Trait for Type`；
- 实现块只能定义 trait 中声明的方法；
- 实现方法的参数数量和除 `self` 外的参数类型必须与 trait 一致；
- 一个类型对同一个 trait 只能实现一次；
- trait 方法名不能与类型已有方法冲突；
- trait 和类型必须在同一模块；
- 不支持 trait 继承、默认方法、泛型、静态方法；
- 结构体和 trait 名称共享类型命名空间，不能在同一模块中重名；
- 类型名按源模块注册，模块通常对应一个源文件；不同模块可以定义同名结构体或 trait；
- 类型名不会自动跨模块可见；跨模块返回的结构体值仍可以通过字段和方法使用。

### 5.12 导出声明

只能在包中有实际意义：

```text
export let value = 1;
export const fixed = 2;
export func hello() { return "hi"; }

export x = 3;
```

形式：

```text
export let NAME = expr;
export const NAME = expr;
export func NAME(params) { ... }
export NAME = expr;
```

`export NAME = expr` 等价于导出可变的 `let` 绑定。

## 6. 包和导入

### 6.1 package

```text
package cc {
  export let value = 42;

  export func hello(name) {
    return "hi " + name;
  }

  func private_helper() {
    return 1;
  }
}
```

- 默认包是 `main`；
- `package name {}` 创建独立作用域；
- 包名必须与所在文件夹名相同；
- 没有 `export` 的定义是包私有；
- 包块是独立作用域；导出值在加载时写入命名空间，导出函数保留包闭包；
- 包块本身不需要分号。

### 6.2 import

```text
import aa.bb.cc;
import aa.bb;
import aa;
import aa.bb.cc as c;
import std as s;

import .sibling;        // 当前包目录
import ..common;        // 上一级目录
import @top.pkg;        // 项目根目录
```

- `import aa.bb.cc;` 绑定最后一个包名 `cc`；
- `import aa.bb;` 绑定 `bb`，子包通过 `bb.cc` 访问；
- `import aa;` 绑定 `aa`，子包通过 `aa.bb.cc` 访问；
- `import aa.bb.cc as c;` 使用别名 `c` 绑定，后续通过 `c.value` 访问；
- `.name` 相对于导入语句所在文件的包目录；
- `..name` 相对于该包目录的上一级；
- `@name` 只从项目根目录解析，不回退到 `-I` 或环境变量；
- 项目根由从主脚本目录向上查找的 `.calcroot` 标记文件确定；没有标记时使用主脚本目录（stdin 模式使用当前目录）；
- 裸 `std` 才是内置标准库；`.std`、`..std`、`@std` 都按文件系统包路径解析；
- 别名必须是普通标识符；`as` 是保留字；
- import 语句需要分号；
- 包名绑定到当前作用域。

### 6.3 目录约定

```text
aa/
  bb/
    cc/
      cc.calc
```

导入：

```text
import aa.bb.cc;
```

入口文件优先使用 `<name>.calc`，其次 `index.calc`；如果目录中只有一个 `.calc` 文件，也会使用它。

父目录没有源文件时，也可以作为纯子包容器。

### 6.4 内置标准库 std

```text
import std;
```

`std` 是内置包，不需要文件系统目录，也不参与包目录名规则；`std` 名称保留，不能被源码包覆盖。

模块：

```text
std.list
std.dict
std.string
std.bytes
```

常用示例：

```text
std.list.slice([1, 2, 3], 1, 2)
std.list.contains([1, 2, 3], 2)
std.list.reverse([1, 2, 3])
std.list.sort([3, 1, 2])
std.list.append([1, 2], 3)
std.list.map([1, 2], func(x) { return x * 2; })
std.list.filter([1, 2, 3], func(x) { return x % 2 == 1; })
std.list.reduce([1, 2, 3], func(a, b) { return a + b; })
std.dict.get({"a": 1}, "b", 0)
std.dict.keys({"a": 1, "b": 2})
std.dict.items({"a": 1, "b": 2})
std.dict.delete({"a": 1}, "a")
std.dict.merge({"a": 1}, {"b": 2})
std.string.join(["a", "b"], ",")
std.string.replace("a-b", "-", "_")
std.string.starts_with("hello", "he")
std.string.contains("hello", "ell")
std.string.repeat("ab", 2)
std.string.trim_start("  x")
std.bytes.hex(bytes_from_hex("00ff"))
```

原有全局函数仍可使用。

### 6.5 查找路径

优先级：

```text
入口脚本所在目录
> 命令行 -I / --package-path
> 环境变量 CALC_PACKAGE_PATH
```

示例：

```bash
./calc-demo -I ./lib --package-path ./vendor app/main.calc
CALC_PACKAGE_PATH=./lib:./vendor ./calc-demo app/main.calc
```

POSIX 使用 `:`，Windows 使用 `;`。

## 7. 内置函数

### 数学

```text
sqrt exp log abs
```

### 输出和转换

```text
print list dict int float bool str format len range
```

### 进程

```text
command command_status
```

### 文件

```text
read_file write_file append_file file_exists
file_open file_read file_write
file_seek file_tell file_eof file_close file_flush
```

### 控制台和字符串

```text
input
split split_ws trim find substr
```

### bytes

```text
bytes_from_string bytes_to_string
bytes_len bytes_get bytes_slice bytes_concat
bytes_hex bytes_from_hex
bytes_to_int int_to_bytes
read_bytes write_bytes append_bytes
file_read_bytes file_write_bytes
```

### 常用示例

```text
print("hello");
print("x = {}", 3);
let s = format("{:.2}", 1.234);
let n = int("42");
let b = read_bytes("data.bin");
let h = file_open("data.bin", "rb");
```

### range

```text
range(length)                 → [0, 1, ..., length-1]
range(start, length)          → [start, ..., start+length-1]
range(start, length, step)    → 长度为 length 的等差数列
```

示例：

```text
range(5)             → [0, 1, 2, 3, 4]
range(-2, 3)         → [-2, -1, 0]
range(5, 3, -2)      → [5, 3, 1]
```

规则：

- 第二个参数是**长度**，不是结束值；
- `length` 必须是非负整数；
- `step` 不能为 0；
- 结果始终是 list；
- 元素计算超出 `int64` 范围时报错。

## 8. 值语义

- `int` / `float`：复制值；
- `string`：复制内容；
- `bytes`：复制内容；
- `list` / `dict`：值语义，内部使用写时复制；
- 函数值：共享函数代码和闭包环境；
- 文件句柄等 userdata：共享底层资源。

示例：

```text
let a = [1, 2];
let b = a;
b[0] = 9;

a               // [1, 2]
b               // [9, 2]
```

函数值共享闭包：

```text
func make_counter() {
  let n = 0;
  func inc() { n = n + 1; return n; }
  return inc;
}

let c = make_counter();
c()             // 1
c()             // 2
```

## 9. 错误和退出码

脚本默认在第一条错误后停止；REPL 继续交互。

```text
calc-demo -k script.calc
```

`-k` / `--keep-going` 会继续执行后续顶层语句，但最终退出码仍为 `1`。

| 情况 | 退出码 |
|---|---:|
| 正常结束 | `0` |
| REPL 正常 EOF | `0` |
| 脚本语言错误（词法/语法/运行时/包） | `1` |
| 命令行参数错误 | `2` |
| 脚本文件无法打开 | `2` |
| 脚本被 Ctrl-C 中断 | `130` |

## 10. 当前不支持

```text
def
var
解构声明
多变量声明
复合赋值
for ... of
try/catch
通配导入
私有成员
struct 继承
trait 继承 / 默认方法
```

动态链接库加载目前是设计方向，尚未作为语言语法实现。
