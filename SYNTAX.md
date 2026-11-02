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
let const func async await yield defer
if else while do for
switch case default
struct impl trait
return break continue
fail on error recover pass
is label
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
struct
method
function
builtin
userdata
result
null
```

布尔字面量：

```text
true
false
```

真值规则：

- 假：`null`、`false`、`0`、`0.0`、空字符串、空 bytes、空列表、空字典；
- 真：其他值，包括 `true`、struct、方法值、函数、内置函数和 userdata（例如文件句柄）。

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
- 字段访问只用于 struct 字段和 `std`/包命名空间；普通 dict 的键必须用下标。

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
obj.field = 4
```

赋值是右结合表达式：

```text
a = b = 1
```

复合赋值：

```text
x += 1
x -= 1
x *= 2
x /= 2
x %= 2
x &= mask
x |= mask
x ^= mask
x <<= 1
x >>= 1
```

规则：

- 目标和普通赋值一样必须是可赋值位置；
- `a[i] += 1` 的下标表达式只求值一次；
- 字符串和 list 支持 `+=`；
- 复合赋值也遵守变量和字段的类型约束；
- `const` 变量不能复合赋值。

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
d["n"]++;

let b = bytes_from_hex("00ff");
b[1]--;
```

### 4.11 运行时类型

```text
kind_of(x)
type_name(x)
type_module(x)
type_source(x)
x is Type
```

- `kind_of(x)` 返回粗略运行时类别；
- `type_name(x)` 返回具体类型名；
- `type_module(x)` 返回逻辑模块/包名：主模块为 `main`，包内类型为包名；
- `type_source(x)` 返回调试用源文件路径；普通值返回空字符串；
- `is` 用类型名判断运行时值是否匹配。

`kind_of` 返回值：

```text
int float bool string bytes list dict
function builtin userdata struct null
```

示例：

```text
for (let value in values) {
  switch (kind_of(value)) {
    case "int" => print("int");
    case "string" => print("string");
    case "struct" => print("struct: " + type_name(value));
    default => print(kind_of(value));
  }
}
```

结构体：

```text
struct Point { x: int; }

let p = Point { x: 1 };

kind_of(p)        // "struct"
type_name(p)      // "Point"
type_module(p)    // "main" 或包名，例如 "aa.bb"
type_source(p)    // 调试用源文件路径

p is Point        // true
p is int          // false
```

trait：

```text
trait Drawable {
  func draw(self);
}

impl Drawable for Point {
  func draw(self) {
    print("point");
  }
}

p is Drawable     // true
```

`is` 只接受类型名或 trait 名，不接受任意表达式作为右操作数。类型名按模块解析，因此同名结构体不会混淆。

### 4.12 运算优先级

从低到高：

```text
= += -= *= /= %= &= |= ^= <<= >>=
? :
||
&&
|
^
&
比较运算 / is
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
list dict function builtin callable null userdata result error
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
result
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

break outer;
continue outer;
```

- `return` 只能用于函数；
- `break` 可用于循环或 switch；
- `continue` 只能用于循环；
- 都需要分号。

带标签的控制流：

```text
label outer: for (let i in [1, 2, 3]) {
  for (let j in [1, 2, 3]) {
    if (i == 2 && j == 2) {
      break outer;
    }
    print(str(i) + "," + str(j));
  }
}
```

规则：

- 标签写法为 `label NAME: loop_or_switch`；
- `label` 只能直接放在 `for`、`for ... in`、`while`、`do while` 或 `switch` 前；
- `break NAME;` 跳出指定标签；
- `continue NAME;` 继续指定标签对应的循环；
- 无标签的 `break` / `continue` 仍然作用于最近的循环或 switch；
- 标签找不到时按普通越界 `break` / `continue` 报错。

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

- `fail expr;` 抛出错误；
- `fail;` 抛出空错误；
- 普通值会被包装成 `error(kind="fail", message=...)`；
- `call() on error(e) { ... }` 捕获 `fail`、运行时错误和 `result.err`；
- `result.ok(v) on error(...)` 直接得到 `v`，不执行 handler；
- `e` 是 `error` 值，只在 handler 作用域内有效；
- `str(e)` 或 `std.error.message(e)` 取得消息；
- `std.error.kind(e)` 取得错误类别；
- handler 必须以 `recover`、`pass` 或 `fail` 结束；
- `recover expr;` 结束 handler，并把 `expr` 作为整个 `on error` 表达式的值；
- `recover;` 等价于 `recover null;`；
- `pass e;` 继续向外层传播错误；
- handler 内不能直接用 `return`、`break` 或 `continue` 结束 handler；
- `recover` / `pass` 只能出现在 handler 内；
- 未捕获的 `fail` 在脚本顶层报错并停止脚本。

`on error` 也直接消费 `result`：

```text
func parse(s) {
  if (s == "") { return std.result.err("empty"); }
  return std.result.ok(42);
}

let n = parse(input) on error(e) {
  print(str(e));
  recover 0;
};
```

| `expr` 的结果 | `on error` 的行为 |
|---|---|
| `ok(v)` | 表达式值是 `v` |
| `err(e)` | 执行 handler |
| `fail` / 运行时错误 | 执行 handler |
| 普通值 | 原样返回 |

- `ok` 只解包一层；
- 不会自动解包嵌套在普通表达式里的 result；
- 如果只想拿到 result 本身，不要对它使用 `on error`。

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

`fail` 接受任意值，但统一转换成 `error`：

```text
fail 42;
fail {"code": 7, "message": "bad"};
fail std.error.new("io", "bad");
```

需要保留错误类别时，直接抛出 `error`：

```text
func load() {
  fail std.error.new("io", "open failed");
}
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
- 不支持默认参数；
- 支持最后一个参数为可变参数；
- 函数体使用 `{ list }`；
- 函数调用深度上限为 1000。

参数类型约束在调用时进行运行时检查，不做隐式转换。支持：

```text
any int float number bool string bytes
list dict function builtin callable null userdata result error
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
- 最多一个可变参数；
- `rest` 在函数内是 `list`；
- 没有额外参数时是 `[]`；
- 类型标注作用于每个额外参数；
- 固定参数不足会报错；
- 方法中 `self` 仍是第一个固定参数，例如 `func log(self, ...items)`；
- trait 方法暂不支持可变参数。

调用点展开：

```text
let xs = [1, 2, 3];

sum(...xs)       // 6
sum(0, ...xs)    // 6
```

- `...expr` 只能出现在调用参数列表；
- `expr` 必须是 `list`；
- 展开项可以和普通参数混用。

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
- 方法值可以提取：`let f = p.length; f()`；
- 方法值绑定提取时的结构体快照，之后修改原结构体不会影响它；
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

跨模块类型：

```text
// package aa
export struct P { x: int; }
export trait D { func d(self); }

// main
import aa;
let p: aa::P = aa.make();
p is aa::D

import aa as lib;
let q: lib::P = lib.make();
```

- 类型默认只在声明模块内可见；
- `export struct` / `export trait` 后才能通过模块路径引用；
- 限定类型名使用 `::`；
- 未导出的类型不能通过 `aa::Hidden` 引用，但它的值仍能正常传递和调用方法。

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
export struct Point { x: int; }
export trait Drawable { func draw(self); }

export x = 3;
```

形式：

```text
export let NAME = expr;
export const NAME = expr;
export func NAME(params) { ... }
export struct NAME { ... }
export trait NAME { ... }
export NAME = expr;
```

`export NAME = expr` 等价于导出可变的 `let` 绑定。
导出类型后，其他模块可以用 `包名::类型名` 作为类型标注或 `is` 的右操作数。

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
std.json
std.string
std.bytes
std.math
std.io
std.fs
std.process
std.type
std.net
std.http
std.tls（可选）
std.task
std.generator
std.result
std.error
std.ffi（可选）
```

常用示例：

```text
std.math.sqrt(9)
std.math.sin(0)
std.math.min(3, 1, 2)
std.io.print("hello")
std.io.read(stream, 4096)
std.io.write_all(stream, data)
std.io.read_all(stream, 16 * 1024 * 1024)
std.fs.read_file("data.txt")
std.process.command_status("true")
std.type.name(1)
std.net.resolve("localhost")

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
std.json.parse("{\"a\": 1}")
std.json.stringify({"a": 1, "b": [2, 3]})
std.string.join(["a", "b"], ",")
std.string.replace("a-b", "-", "_")
std.string.starts_with("hello", "he")
std.string.contains("hello", "ell")
std.string.repeat("ab", 2)
std.string.trim_start("  x")
std.bytes.hex(bytes_from_hex("00ff"))
```

JSON：

```text
import std;

let value = std.json.parse("{\"name\": \"alice\", \"tags\": [1, 2]}");
std.json.stringify(value)       // {"name":"alice","tags":[1,2]}

let r = std.json.try_parse("{bad}");
std.result.is_err(r)            // true
```

函数：

```text
std.json.parse(text)        → value
std.json.try_parse(text)    → result
std.json.stringify(value)   → string
```

`text` 可以是 `string` 或 `bytes`，HTTP body 可以直接传入。

```text
let data = std.json.parse(req["body"]);
std.http.respond(req, 200, {"Content-Type": "application/json"},
                 std.json.stringify({"ok": true}));
```

规则：

- JSON object 映射为 `dict`，key 一律是 `string`；array 映射为 `list`；
- number：在 int64 范围内的整数映射为 `int`，否则映射为 `float`；
- `true` / `false` / `null` 映射为 `bool` / `null`；
- `parse` 遇到非法 JSON 会报运行时错误，`try_parse` 返回 `result`；
- 重复 key 以最后一个为准；
- `stringify` 只接受 JSON 子集；object key 必须是 `string`，否则报错；
- `stringify` 拒绝 `bytes`、`function`、`struct`、`userdata`、`result`、`error` 和 `NaN` / `Inf`；
- 字符串按 UTF-8 校验并转义控制字符；JSON 的 `\u0000` 不支持；
- 嵌套深度上限为 512；
- 默认输出紧凑 JSON，并保持 dict 的插入序。

流式读写：

```text
import std;

let f = std.fs.open("data.bin", "rb");
defer std.io.close(f);

std.io.readable(f)
std.io.writable(f)
std.io.seekable(f)

let chunk = std.io.read(f, 4096);       // bytes；EOF 为 null
let rest = std.io.read_all(f, 16 * 1024 * 1024);
std.io.eof(f)
```

socket 异步流：

```text
let s = std.net.tcp_connect("127.0.0.1", 8080);

await std.io.write_async(s, "hello", 1000);
let data = await std.io.read_async(s, 4096, 1000);
std.io.close(s);
```

文件异步流：

```text
let f = std.fs.open("data.bin", "rb");
let first = await std.io.read_async(f, 4096);
let rest = await std.io.read_async(f, 4096);
std.io.close(f)
```

流函数：

```text
std.io.read(stream, max_bytes[, timeout_ms])
std.io.write(stream, data[, timeout_ms])
std.io.write_all(stream, data[, timeout_ms])
std.io.flush(stream)
std.io.close(stream)
std.io.eof(stream)
std.io.readable(stream)
std.io.writable(stream)
std.io.seekable(stream)
std.io.seek(stream, offset[, whence])
std.io.tell(stream)
std.io.read_exact(stream, n[, timeout_ms])
std.io.read_all(stream[, limit])
std.io.read_line(stream[, limit])
std.io.copy(dst, src[, limit])
std.io.buffered(stream[, size])
std.io.read_async(stream, max[, timeout_ms])
std.io.write_async(stream, data[, timeout_ms])
```

规则：

- EOF 返回 `null`；
- `read_all` 默认上限 16 MiB；
- `close` 幂等，返回是否实际关闭；
- `seek` / `tell` 只适用于可 seek 的流；
- `buffered` 同时提供读缓冲和写缓冲，`close` 会先 flush；
- `read_async` / `write_async` 返回 task，socket 和文件都支持；
- 文件异步 I/O 使用 libuv 线程池；
- 同一文件同时只允许一个异步操作；异步未完成时同步操作会报错；
- `write_async` 可能部分写入，返回实际写入字节数；
- buffered 流会代理底层流的异步读写；
- 文件和 socket 共用 `std.io`；
- 文件仍使用 `std.fs.seek` / `std.fs.tell`。

新增模块说明：

- `std.math`：数学函数，和全局 `sqrt`、`sin`、`min` 等共用实现；
- `std.io`：`print`、`format`、`input`，以及文件和 socket 通用的流式读写；
- `std.fs`：文件读写和文件句柄操作；
- `std.process`：命令执行、`status` / `command_status`；
- `std.json`：JSON 解析与序列化，object 映射为 `dict`；
- `std.type`：`kind_of`、`name`、`module`、`source`；
- `std.net`：同步和 task 形式异步的 TCP/DNS，底层使用 libuv；
- `std.http`：基于 llhttp 的 HTTP/1.1 客户端和服务端，客户端支持 sync/async；
- `std.tls`：基于 OpenSSL 的同步 TLS 流包装（可选）；
- `std.task`：原生 task、等待、批量等待、取消和完成状态；
- `std.generator`：生成器的 `next` / `done` / `close`；
- `std.result`：result 值、错误检查和错误捕获；
- `std.error`：error 值和 `kind` / `message` 访问；
- `std.ffi`：可选的 libffi C ABI 调用。

TCP 示例：

```text
import std;

let s = std.net.tcp_connect("127.0.0.1", 8080);
std.net.send(s, "hello");
let data = std.net.recv(s, 4096);
print(bytes_to_string(data));
std.net.close(s);
```

网络函数签名：

```text
tcp_connect(host, port[, timeout_ms[, family]])
tcp_listen(host, port[, timeout_ms[, family]])
accept(listener[, timeout_ms])
send(socket, data[, timeout_ms])
recv(socket, size[, timeout_ms])
resolve(host[, timeout_ms[, family]])
shutdown(socket[, timeout_ms])
local_addr(socket)
peer_addr(socket)
```

`timeout_ms = 0` 或省略表示不设置超时；正数表示一次调用的总超时时间，检查精度约 10ms。`family` 支持 `"any"`、`"ipv4"`、`"ipv6"`；`"any"` 使用解析结果中的第一个可绑定地址。

地址查询返回：

```text
{address: "127.0.0.1", port: 8080, family: "ipv4"}
```

`std.net` 同时提供同步接口和 task 形式的异步接口；libuv 事件循环只在内部运行，不会作为对象暴露给脚本。

异步网络：

```text
tcp_connect_async(host, port[, timeout_ms[, family]]) → task
tcp_listen_async(host, port[, timeout_ms[, family]])  → task
accept_async(listener[, timeout_ms])                  → task
send_async(socket, data[, timeout_ms])                → task
recv_async(socket, size[, timeout_ms])                → task
resolve_async(host[, timeout_ms[, family]])           → task
shutdown_async(socket[, timeout_ms])                  → task
```

```text
import std;

let t = std.net.tcp_connect_async("127.0.0.1", 8080, 1000);
let socket = std.task.wait(t);
```

- 操作自身的 `timeout_ms` 和 `std.task.wait` 的超时是两回事；
- `std.task.cancel` 会取消底层网络操作；
- 多个 task 可以用 `std.task.wait_all` 并发等待；
- `std.net.close` 仍然同步。

Task 示例：

```text
import std;

let a = std.task.sleep(100);
let b = std.task.yield();

std.task.done(a)
std.task.wait(a)
std.task.wait_all([a, b])
```

Task 函数：

```text
sleep(ms)                  → task
yield()                    → task
wait(task[, timeout_ms])   → result
wait_all(tasks[, timeout_ms]) → list
cancel(task)               → bool
done(task)                 → bool
catch(task)                → result task
try_wait(task[, timeout_ms]) → result
try_wait_all(tasks[, timeout_ms]) → result
```

`std.task.yield` 是函数字段；单独的 `yield` 关键字用于生成器。`wait` 超时不会自动取消 task；需要显式调用 `cancel`。

task 是协作式调度的：只有运行事件循环时才会推进，例如 `wait`、`wait_all`、`await` 或同步网络调用期间。

目前 task 覆盖原生定时器、async 函数和 `std.net.*_async` 异步 I/O。

### HTTP/1.1

客户端：

```text
import std;

let r = std.http.get("http://127.0.0.1:8080/api");

r["status"]
r["headers"]["content-type"]
bytes_to_string(r["body"])
```

带 headers / timeout：

```text
let r = std.http.get("http://127.0.0.1:8080/api", {
  "headers": {"Accept": "application/json"},
  "timeout_ms": 1000
});
```

POST 和自定义方法：

```text
let r = std.http.post("http://127.0.0.1:8080/items", "hello", {
  "headers": {"Content-Type": "text/plain"}
});

let d = std.http.request("DELETE", "http://127.0.0.1:8080/items/1");
```

异步客户端：

```text
let r = await std.http.get_async("http://127.0.0.1:8080/api");
let p = await std.http.post_async("http://127.0.0.1:8080/items", "hello");
let d = await std.http.request_async(
    "DELETE", "http://127.0.0.1:8080/items/1");
```

HTTPS 同步客户端：

```text
let r = std.http.get("https://example.com/", {
  "tls": {"ca_file": "/etc/ssl/certs/ca-certificates.crt"}
});
```

HTTPS 同步服务端：

```text
let s = std.http.listen_tls("127.0.0.1", 8443, "cert.pem", "key.pem");
let req = std.http.accept(s, 5000);
std.http.respond(req, 200, {"Content-Type": "text/plain"}, "secure");
std.http.close(s);
```

HTTP/1.1 服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);
let req = std.http.accept(s, 5000);

std.http.respond(req, 200, {
  "Content-Type": "text/plain"
}, "hello " + req["path"]);

std.http.close(s);
```

异步服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);
let req = await std.http.accept_async(s, 5000);
await std.http.respond_async(req, 200, {"Content-Type": "text/plain"},
                             "hello " + req["path"]);
std.http.close(s);
```

keep-alive 服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);

let served = std.http.serve(s, func(req) {
  return {
    "status": 200,
    "headers": {"Content-Type": "text/plain"},
    "body": "hello " + req["path"]
  };
}, 100);

std.http.close(s);
```

`std.http.serve(server, handler[, max_requests[, max_per_connection[, idle_timeout_ms]]])`：

- handler 接收 request dict；
- 返回 response dict；
- handler 可以是 `async func`，等待期间其他连接可以继续处理；
- 每个客户端连接在独立 fiber 中处理，多个连接可以并发服务；
- 同一个连接可以处理多个请求；
- `max_requests = 0` 表示总请求数不限制；
- `max_per_connection` 默认 100，`0` 表示不限制；
- `idle_timeout_ms` 默认 0，表示不设置空闲超时；
- 达到限制或空闲超时后关闭连接；
- 返回已处理请求数。

函数：

```text
std.http.request(method, url[, options])
std.http.get(url[, options])
std.http.post(url[, body][, options])
std.http.request_async(method, url[, options])
std.http.get_async(url[, options])
std.http.post_async(url[, body][, options])
std.http.listen(host, port[, timeout_ms])
std.http.listen_tls(host, port, cert_file, key_file[, timeout_ms])
std.http.accept(server[, timeout_ms])
std.http.accept_async(server[, timeout_ms])
std.http.respond(request, status[, headers[, body]])
std.http.respond_async(request, status[, headers[, body]])
std.http.serve(server, handler[, max_requests[, max_per_connection[, idle_timeout_ms]]])
std.http.close(server_or_request)
```

流式 body：

```text
func parts() {
  yield "hello";
  yield "world";
}

let sent = std.http.post(url, {"body_stream": parts()});

let received = std.http.get(url, {
  "on_body": func(chunk) {
    print(bytes_to_string(chunk));
  }
});
```

- `body_stream` 必须是生成器，逐块产生 string/bytes；
- 使用 `body_stream` 时请求使用 `Transfer-Encoding: chunked`；
- `body` 与 `body_stream` 不能同时设置；
- `on_body` 逐块接收响应 body，响应 dict 的 `body` 为空；
- 使用流式 body 时不会自动重试；
- `max_body_bytes` 默认 16 MiB，对 `on_body` 同样生效。

服务端流式响应：

```text
func parts() {
  yield "hello";
  yield "world";
}

let served = std.http.serve(s, func(req) {
  return {"status": 200, "body_stream": parts()};
});
```

- `std.http.serve` 响应 dict 支持 `body_stream`；
- `std.http.respond` 的 body 参数也可以是生成器；
- HTTP/1.1 使用 `Transfer-Encoding: chunked`；
- HTTP/2 使用 data provider。

服务端请求 body 流式回调：

```text
let s = std.http.listen("127.0.0.1", 8080);
s["on_request_body"] = func(chunk) {
  print(bytes_to_string(chunk));
};
```

- 设置后 handler 的 `req["body"]` 为空，需要完整 body 时应在回调中自行累积；
- HTTP/2 多 stream 的 body 回调可能交错；
- 未设置时仍整体读取请求 body；
- `max_body_bytes` 同时限制流式请求 body。

请求 dict：

```text
method, path, headers, body, http_major, http_minor, socket
```

响应 dict：

```text
status, status_text, headers, body, http_major, http_minor
```

响应 dict 还可以使用 `body_stream` 代替 `body`。

限制：

- 支持 `http://` 和 `https://`；
- `https://` 支持同步和 async 客户端/服务端；
- 只支持 HTTP/1.1；HTTP/2 使用 `std.http2`；
- 同步和 async 客户端共用 HTTP/1.1 keep-alive 连接池；
- 复用连接失败时，幂等方法（GET/HEAD/OPTIONS/TRACE）会用新连接重试一次；
- `std.http.serve` 支持服务端 keep-alive；
- `std.http.accept` / `respond` 仍是一次连接一个请求；
- 默认整体读取 body；可用 `body_stream` / `on_body` 流式处理；
- `max_body_bytes` 默认 16 MiB；
- 同名 header 只保留最后一个；
- 客户端和服务端都支持同步和 async。

### HTTP/2

```text
std.http2.request(method, url[, options]) → response
std.http2.get(url[, options])           → response
std.http2.post(url[, body[, options]])  → response
std.http2.request_async(method, url[, options]) → task
std.http2.get_async(url[, options])             → task
std.http2.post_async(url[, body[, options]])    → task
std.http2.request_many(requests[, options])     → list
std.http2.request_many_async(requests[, options]) → task
std.http2.listen(host, port[, timeout_ms])      → server
std.http2.listen_tls(host, port, cert_file, key_file[, timeout_ms]) → server
std.http2.serve(server, handler[, max_requests[, max_per_connection[, idle_timeout_ms]]]) → int
std.http2.close(server)
```

客户端：

```text
let a = std.http2.get("https://example.com/", {
  "headers": {"Accept": "text/plain"},
  "tls": {"verify": true}
});

print(str(a["status"]) + " " + a["protocol"]);
print(bytes_to_string(a["body"]));

let b = std.http2.post("https://example.com/echo", "payload");
let c = await std.http2.get_async("https://example.com/async");
```

批量并发请求：

```text
let responses = std.http2.request_many([
  {"url": "https://example.com/a"},
  {"url": "https://example.com/b", "method": "POST", "body": "payload"}
]);

let responses2 = await std.http2.request_many_async([
  {"url": "https://example.com/c"},
  {"url": "https://example.com/d", "method": "DELETE"}
]);
```

- 所有请求必须属于同一个 origin；
- 同一连接上同时提交多个 stream；
- 返回列表顺序与输入一致；
- 请求项支持 `url`、`method`、`headers`、`body`、`body_stream`、`on_body`；
- 每个请求项可以独立流式发送请求 body 和流式接收响应 body。

服务端：

```text
let s = std.http2.listen("127.0.0.1", 8080);

let served = std.http2.serve(s, func(req) {
  return {
    "status": 200,
    "headers": {"Content-Type": "text/plain"},
    "body": "h2 " + req["method"] + " " + req["path"]
  };
}, 100);

std.http2.close(s);
```

规则：

- `http://` 使用 h2c prior knowledge；
- `https://` 通过 TLS ALPN 协商 `h2`，未协商成功会报错；
- 支持同步和 async 客户端；
- 支持 h2c / TLS 服务端；
- 默认整体读取 body；可用 `body_stream` / `on_body` 流式处理；
- `max_body_bytes` 默认 16 MiB；
- request dict 额外包含 `connection_id`；
- 支持 `body_stream` 生成器流式发送请求 body；
- 支持 `on_body` 回调流式接收响应 body；
- 服务端响应 dict 支持 `body_stream` 流式发送；
- async 服务端 handler 可在多个 stream 之间并发执行；
- 每个客户端连接在独立 fiber 中处理，多个连接可以并发服务；
- `options` 支持 `headers`、`body`、`body_stream`、`on_body`、`timeout_ms`、`max_body_bytes`、`tls`；
- `max_requests = 0` 表示总请求数不限制；
- `max_per_connection` 默认 100，`0` 表示不限制；
- `idle_timeout_ms` 默认 0，表示不设置连接空闲超时；
- 同步和 async 客户端共用 HTTP/2 连接池；
- 同步请求按顺序复用连接；
- async 的 `get_async` / `post_async` / `request_async` 可以共享同一连接并并发执行；
- async 流式请求（`on_body` / `body_stream`）也可以共享同一连接；
- `request_many` / `request_many_async` 在单连接上并发提交多个 stream；
- 收到 GOAWAY 或共享连接失败时，async 幂等请求会换新连接重试一次；
- 使用 `body_stream` / `on_body` 的请求不自动重试。

HTTP/2 SETTINGS 与 flow control：

客户端：

```text
let a = std.http2.get(url, {
  "settings": {"initial_window_size": 1048576}
});
```

服务端：

```text
let s = std.http2.listen("127.0.0.1", 8080);

s["settings"] = {
  "max_concurrent_streams": 100,
  "initial_window_size": 262144
};
```

支持的设置项：

```text
max_concurrent_streams   // > 0
initial_window_size      // 1 .. 2147483647
max_frame_size           // 16384 .. 16777215
max_header_list_size     // > 0
enable_push              // 仅客户端，bool
```

- 未设置时使用 nghttp2 默认值；
- `settings` 参与连接池 key，不同 settings 不会错误复用同一连接；
- 批量请求在提交前会等待服务端 SETTINGS，避免超过服务端并发 stream 限制；
- `initial_window_size` 可以用于调大单 stream 流控窗口。

### TLS

```text
std.tls.client(stream, server_name[, options]) → tls_stream
std.tls.server(stream, cert_file, key_file[, options]) → tls_stream
std.tls.alpn(tls_stream)                       → string
```

客户端：

```text
let socket = std.net.tcp_connect("127.0.0.1", 8443, 1000);
let tls = std.tls.client(socket, "example.com", {
  "verify": true,
  "ca_file": "/etc/ssl/certs/ca-certificates.crt",
  "alpn": ["h2", "http/1.1"]
});

std.io.write_all(tls, "hello");
bytes_to_string(std.io.read(tls, 4096));
std.io.close(tls);
```

服务端：

```text
let listener = std.net.tcp_listen("127.0.0.1", 8443);
let client = std.net.accept(listener, 5000);
let tls = std.tls.server(client, "cert.pem", "key.pem");
let data = std.io.read(tls, 4096);
std.io.write_all(tls, data);
std.io.close(tls);
std.net.close(listener);
```

选项：

```text
verify    // 客户端默认 true
ca_file   // 客户端 CA 文件
alpn      // 客户端/服务端 ALPN 列表
```

`std.tls.alpn(tls)` 返回协商出的 ALPN：

```text
std.tls.alpn(tls)   // "h2" / "http/1.1" / ""
```

规则：

- 默认校验证书和主机名；
- TLS 流实现 `std.io` 接口；
- TLS 流支持 `std.io.read_async` / `write_async`；
- TLS 握手在包装时同步完成；
- 服务端 ALPN 会按服务端列表选择第一个客户端也支持的协议；

### async / await

```text
import std;

async func fetch(port) {
  let socket = await std.net.tcp_connect_async("127.0.0.1", port, 1000);
  let data = await std.net.recv_async(socket, 4096, 1000);
  std.net.close(socket);
  return data;
}

let a = fetch(8080);
let b = fetch(8081);

let da = await a;
let db = await b;
```

规则：

- `async` 修饰函数，调用后返回 task；
- async 函数调用后立即开始运行，遇到第一个 `await` 时挂起；
- `await` 可用于 async 函数或顶层；
- 普通函数中使用 `await` 会报错；
- 多个 async 函数可以先启动再分别 await，从而并发执行；
- async 函数内部错误会变成 task 失败；
- `await` 未捕获的 task 失败会报错，`await std.task.catch(task)` 可以恢复；
- `yield` 不用于 async；主动让出使用 `await std.task.yield()`；
- 使用 Boost.Context 提供有栈协程。

生成器：

```text
import std;

func numbers() {
  yield 1;
  yield 2;
  yield 3;
}

let g = numbers();

std.generator.next(g)
std.generator.next(g)
std.generator.next(g)
std.generator.next(g)
std.generator.done(g)
```

规则：

- 函数体中出现 `yield` 时，该函数自动成为生成器；
- 第一次 `std.generator.next` 才开始执行函数体；
- `std.generator.next(g[, value])` 返回 `{done: bool, value: ...}`，第一次 `next` 的发送值会被忽略；
- `yield;` 等价于 `yield null;`；
- `yield` 恢复时返回 `next(value)` 发送的值；
- `return` 的返回值作为结束时 `{done: true, value: ...}` 的 value；
- 可以直接 `for (let x in generator)`；
- `std.generator.close(g)` 关闭生成器并执行 `defer`；
- 挂起生成器被直接丢弃且仍有待执行 `defer` 时，会打印 warning，但不会执行清理；
- 提前 `break` 出 `for (let x in g())` 时，临时生成器会被丢弃并触发 warning；需要清理时先绑定变量并 `defer std.generator.close(g)`；
- 未开始的生成器调用 `close` 不会执行函数体；
- 已结束的生成器调用 `close` 返回 `false`；
- `close` 期间 `defer` 抛出的 `fail` 会传给调用方，可被 `on error` / `std.result.capture` 捕获；
- 正在运行的生成器不能关闭自己；
- 不提供 `std.generator.throw`；生成器内部错误使用 `fail`，调用方使用 `on error` / `std.result.capture`；
- async 生成器暂不支持；
- 生成器依赖 Boost.Context 提供的有栈协程。

生成器可以直接迭代：

```text
for (let x in numbers()) {
  print(x);
}
```

向生成器发送值：

```text
func echo() {
  let x = yield 1;
  yield x + 1;
}

let g = echo();
std.generator.next(g)       // {done: false, value: 1}
std.generator.next(g, 10)   // {done: false, value: 11}
```

关闭生成器：

```text
func worker() {
  defer print("cleanup");
  yield 1;
  print("not reached after close");
}

let g = worker();
std.generator.next(g)
std.generator.close(g)      // cleanup，返回 true
std.generator.done(g)       // true
std.generator.next(g)       // {done: true, value: null}
```

错误传播：

```text
func broken() {
  fail std.error.new("io", "generator boom");
  yield 1;
}

let g = broken();
let v = std.generator.next(g) on error(e) {
  print(std.error.kind(e));    // io
  print(str(e));               // generator boom
  recover 0;
};
```

没有 `std.generator.throw`。需要从外部注入错误时，用 `next(g, error)` 发送一个 `error` 值，生成器内部再显式 `fail` 它：

```text
func worker() {
  let signal = yield 1;
  if (signal is error) {
    fail signal;
  }
  yield 2;
}

let g = worker();
std.generator.next(g)

let v = std.generator.next(g, std.error.new("cancel", "stop")) on error(e) {
  print(std.error.kind(e));    // cancel
  print(str(e));               // stop
  recover 0;
};
```

生成器内部错误使用 `fail`，调用方用 `on error` 或 `std.result.capture` 恢复。

Result 示例：

```text
import std;

let r = std.result.capture(func() {
  return std.net.tcp_connect("127.0.0.1", 1, 100);
});

std.result.is_err(r)
std.result.error(r)
```

异步错误恢复：

```text
let t = std.net.tcp_connect_async("127.0.0.1", 1, 1000);
let r = await std.task.catch(t);
std.result.is_err(r)
```

Result 函数：

```text
std.result.ok(value)
std.result.err(message)
std.result.is_ok(result)
std.result.is_err(result)
std.result.unwrap(result)
std.result.unwrap_or(result, default)
std.result.value(result)
std.result.error(result)
std.result.capture(fn)

std.error.new(kind, message)
std.error.new(message)
std.error.kind(error)
std.error.message(error)
```

规则：

- `result` 和 `error` 是独立类型；
- `ok(result)` 为真，`err(result)` 为假；
- `std.result.error(result)` 返回 `error` 值，`str(error)` 返回消息；
- `fail` 和 `std.result.err` 会把普通值包装成 `error`；
- `capture` 捕获同步 `fail` 和运行时错误，不打印错误；
- `std.result.unwrap` 在 `err` 时通过 `fail` 抛出；
- `std.task.catch(task)` 返回总会以 result 完成的 task；
- `await std.task.catch(task)` 可以恢复 async 失败；
- `std.task.try_wait` / `try_wait_all` 是同步 result 等待；
- `on error` 捕获显式 `fail`、运行时错误和 `result.err`，并对 `result.ok` 解包。

defer：

```text
func work() {
  let h = file_open("x");
  defer file_close(h);
  ...
}
```

规则：

- 块级或函数级作用域退出时执行；
- LIFO；
- 支持 return / break / continue / 运行时错误；
- 没有其他错误时，defer 内的错误会继续向外传播；
- 已有错误正在展开时，原错误优先，defer 错误以 `error in defer: ...` 报告；
- 顶层 `defer` 不允许；
- 生成器正常结束会执行 defer；
- `std.generator.close(g)` 会执行生成器的 defer；
- 直接丢弃挂起生成器不会执行脚本级 defer；如果仍有待执行 defer，会打印 warning。

FFI 示例：

```text
import std;

let lib = std.ffi.open("libm.so.6");
std.ffi.call(lib, "cos", "f64", ["f64"], [0.0])
std.ffi.close(lib)
```

FFI 支持的类型：

```text
void i8 u8 i16 u16 i32 u32 i64 u64 f32 f64
cstring bytes buffer usize isize
```

`bytes` 作为输入参数时，FFI 会复制脚本 bytes 的内容并传 `const void *`。
`buffer` 是 FFI 管理的原生内存，可作为 C 的输出缓冲区。

```text
let buf = std.ffi.buffer(1024);
let n = std.ffi.call(lib, "shim_read", "i64",
                     ["buffer", "usize"], [buf, 1024]);
let data = std.ffi.buffer_bytes(buf, n);
std.ffi.buffer_free(buf);
```

`buffer` 相关函数：

```text
std.ffi.buffer
std.ffi.buffer_len
std.ffi.buffer_bytes
std.ffi.buffer_free
```

`bytes` / `buffer` 不能直接作为 `std.ffi.call` 的返回类型；二进制输出请使用 `buffer` 或 C shim 返回字符串/句柄。

`bytes` 传递的是调用期间的临时副本，C 侧不能保存该指针。
`buffer` 的容量和写入边界由调用方保证，C 侧越界写入会破坏内存。

`u64` / `usize` 返回值必须在脚本有符号 64 位整数范围内，否则 `std.ffi.call` 报错。

限制：

- 只支持 C ABI；
- 不支持结构体、回调、可变参数；
- 网络和 FFI 错误是运行时错误，可以通过 `on error` 或 `std.result.capture` 捕获；
- Ctrl-C 或超时中断时，`accept` / `recv` 保留 socket，`connect` / `send` 关闭当前 socket；
- `std.net` 暂不支持 TLS、HTTP；
- `std.net.*_async` 的 `timeout_ms` 是操作自身超时，`std.task.wait` 的超时是等待超时；
- async 函数可以挂起和恢复；普通函数仍不能通过通用 `spawn` 启动；
- `std.task.wait` / `wait_all` 超时不会自动取消 task，需要显式 `cancel`；
- `std.result.capture` 捕获同步 `fail` 和运行时错误；async 使用 `std.task.catch`；
- `defer` 只允许在函数或代码块内；
- 直接丢弃挂起的生成器不会执行脚本级 defer，但会打印 warning；
- async/await 和生成器使用 Boost.Context 提供的有栈协程；
- 暂不支持 async 生成器；
- DNS 超时依赖 libuv 取消系统解析请求，极端情况下可能延迟返回。

原有全局函数仍可使用，作为兼容别名保留；新代码建议优先使用 `std.*`。

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
sqrt exp log abs sin cos tan asin acos atan atan2
pow floor ceil round trunc log2 log10 hypot min max
```

规则：

- `sqrt`、`exp`、`log`、三角函数、`pow`、`log2`、`log10`、`hypot` 返回 `float`；
- `floor`、`ceil`、`round`、`trunc` 返回 `int`，超出整数范围会报错；
- `round` 按远离零的方向处理半值，例如 `round(-1.5) == -2`；
- `min` / `max` 至少需要一个数值参数；全为 `int` 时返回 `int`，否则返回 `float`；
- `atan2(y, x)` 返回 `atan(y / x)` 的角度；
- 所有数学函数的参数必须是数值，不做隐式转换。

### 输出和转换

```text
print list dict int float bool str format len range
kind_of type_name type_module type_source
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
for ... of
try/catch
通配导入
默认参数 / 命名参数
静态方法 / 关联函数
可变 self
私有成员
struct 继承
trait 继承 / 默认方法
泛型 / Option / 联合类型
```

动态链接库加载目前是设计方向，尚未作为语言语法实现。
