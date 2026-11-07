# Skein 语法参考

本文只描述当前实现支持的语言语法和基本语义。包查找、文件 API 等细节可结合 `README.md` 阅读。

## 1. 词法

### 1.1 标识符

标识符首字符必须是 Unicode `XID_Start` 字符或下划线 `_`；后续字符必须是 Unicode `XID_Continue` 字符、下划线 `_` 或 ASCII 数字 `0-9`。

```text
let 变量 = 1;
let café = 2;
let λ = 3;
let foo变量 = 4;
```

- 标识符区分大小写；
- 当前使用 Unicode 16.0.0 的 `XID_Start` / `XID_Continue` 数据；
- 关键字和内置函数名仍只识别 ASCII；
- 源码默认按 UTF-8 读取；UTF-16/UTF-32 BOM 会自动转码，无 BOM 时可用 `--encoding NAME` 指定；转码后必须是合法 UTF-8；
- 标识符只使用源码中的字面 UTF-8 字符，不支持 `\u` / `\u{...}` 转义；
- 非 ASCII 标识符按 NFC 归一化（不做 NFKC），规范等价写法视为同一标识符；字符串可通过 `std.unicode.normalize` 显式归一化；
- 函数形参、变长参数名以及类型标注中的用户类型名使用与普通变量相同的标识符规则。

例如，`if` 是关键字，但 `if变量` 和全角的 `ｉｆ` 都是合法标识符；`😀`、`。` 等非 `XID` 字符不能出现在标识符开头。

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

内置函数名也会被词法器识别为函数值，因此不能用作普通变量名，例如 `sqrt`、`print`、`println`、`read_file`。

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
- 文件开头会按 BOM 识别 UTF-8 / UTF-16 / UTF-32 并转成 UTF-8；
- 没有 BOM 时默认 UTF-8，也可以用 `--encoding NAME` 显式指定其它编码。

## 2. 字面量

### 2.1 整数

```text
123
0
-42
0xff
0XFF
0b1010
0o17
1_000_000
0xFF_FF
```

整数是 64 位有符号整数。十进制整数字面量超出 `int64` 范围时会按浮点数解析；`0x` / `0b` / `0o` 字面量超出 `int64` 范围会报词法错误。

进制前缀大小写均可：`0x` / `0X`（十六进制）、`0b` / `0B`（二进制）、`0o` / `0O`（八进制）。十六进制数字也支持大小写。

数字之间可以用 `_` 分隔，例如 `1_000_000`、`0b1010_0101`。下划线只能出现在两个数字之间，不能连续、不能出现在末尾，也不能紧跟在进制前缀、小数点或指数符号之后。

### 2.2 浮点数

```text
1.0
1.
.5
1e3
1E+3
1.5e-2
1_000.5_5
1.5e1_0
```

浮点字面量支持小数和科学计数法；指数部分可带 `+` / `-` 符号。小数部分和指数部分同样可以用 `_` 分隔数字，规则与整数一致。

### 2.3 字符串

```text
"hello"
"line\nnext"
"tab\there"
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

### 2.4 字符

```text
'a'
'你'
'\n'
'\u0041'
'\u{1F600}'
```

`char` 表示一个 Unicode 标量值。

规则：

- 字符字面量使用**单引号**；
- 支持 `\n`、`\t`、`\r`、`\\`、`\'`、`\"`、`\0`、`\uXXXX`、`\u{...}`；
- 必须恰好包含一个字符；
- 空字符、多个字符、代理区 `D800..DFFF`、超过 `0x10FFFF` 都会报错；
- `len(char)` 为 `1`；
- 字符串下标和 `for ... in` 字符串返回 `char`；
- `char` 可以与 `string` 拼接，例如 `'a' + "bc"`；
- `int(char)` 得到码点，`str(char)` 得到单字符字符串，`char(int)` / `char(string)` 构造字符；
- `bytes_from_char(c)` 得到 UTF-8 字节，`char_from_bytes(b)` / `char(b)` 从恰好一个 UTF-8 字符还原；
- JSON 编码为单字符字符串。

### 2.5 字节值 byte

```text
byte(0)
byte(65)
byte(255)
```

`byte` 表示一个 `0..255` 的字节值，没有字面量，使用 `byte(int)` 构造。

规则：

- `int(byte)` 取回 `0..255`；
- `byte(int)` 超出范围报错；
- `byte` 可以作 dict key、`switch` 值；
- JSON 编码为整数；
- `bytes[i]` / `bytes_get` 仍返回 `int`，需要时用 `byte(int)` 转换。

### 2.6 null

```text
null
```

`null` 是正常值，不等于“错误”。没有 `return` 的函数和 `print` / `println` 返回 `null`。

### 2.7 列表

```text
[]
[1, 2, 3]
[1, [2, 3], "x"]
```

列表字面量允许尾随逗号。函数参数和调用参数也允许尾随逗号；字典字面量不允许。

### 2.8 字典

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

字典 key 只允许：

```text
int / bool / string / char / byte / bytes / null
```

`float`、`list`、`dict`、`struct`、`enum`、`function` 等不能作为 key，会报错。

- key 按值相等比较；
- 字典保持插入顺序；
- 删除后重新插入同一个 key 会排到末尾；
- 大字典内部使用哈希索引，但遍历、`keys`、`values`、`items`、`print`、JSON 始终按插入顺序。

### 2.9 bytes

`bytes` 没有字面量，通过内置函数创建：

```text
bytes_from_string("abc")
bytes_from_hex("00ff10")
read_bytes("data.bin")
```

`bytes` 可以包含任意字节，包括 `0x00`。

### 2.10 函数值

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
- string 下标按 UTF-8 码点计数，返回 `char`，字符串索引不能赋值；
- dict 键按值相等判断；
- bytes 下标是字节下标，返回 `0..255`，索引赋值接受 `0..255`；
- 字段访问用于 struct 字段、enum 变体和 `std`/包命名空间；普通 dict 的键必须用下标。

切片使用 Python 风格的 `start:stop:step`：

```text
a[1:4]        // 从下标 1 到 3，不含 4
a[:3]         // 从开头到下标 2
a[2:]         // 从下标 2 到末尾
a[:]          // 完整副本
a[-2:]        // 最后两个元素
a[::2]        // 每隔一个取一个
a[::-1]       // 反向
a[4:0:-2]     // 反向且步长为 -2
```

- 支持 list、string 和 bytes，结果类型与输入相同；
- list 按元素、string 按 UTF-8 码点、bytes 按字节切片；
- `stop` 不包含在结果中；`step` 省略时为 `1`，不能为 `0`；
- 边界和 step 必须是整数；边界可以省略，负数下标从末尾计数；
- 越界下标会被截断到合法范围，不会报错；
- 切片表达式返回新值，不修改原值；暂不支持切片赋值；
- 省略 `stop` 时可直接写 `a[start::step]`，例如 `a[i::j]`、`a[2::2]`。

```text
let xs = [0, 1, 2, 3, 4];
xs[1:4]       // [1, 2, 3]
xs[::-1]      // [4, 3, 2, 1, 0]
"你好世界"[1:3] // "好世"
bytes_from_string("abcd")[1:3] // 两个字节 "bc"
```

### 4.7 函数调用

```text
f()
f(1, 2)
f(1, 2,)
obj["fn"](3)
```

函数调用参数从左到右求值。

管道 `|>` 把左侧值插入右侧调用的第一个参数：

```text
xs |> f          // f(xs)
xs |> f(a)       // f(xs, a)
```

- 右侧不是调用：等价于 `f(xs)`；
- 右侧是调用：等价于 `f(xs, ...)`；
- 左结合，适合 `std.list` / `std.iter` 等主体在第一个参数的 API。

`|>>` 是 apply-to-result 形式：先求值右侧，再用左侧调用结果。

```text
4 |>> h(3)      // (h(3))(4)
xs |>> f($, g)  // (f($, g))(xs)
```

调用参数中的 `$` 是占位符：

```text
add($, 2)       // x -> add(x, 2)
f($, $, 3)      // (a, b) -> f(a, b, 3)
```

- `$` 只在调用参数位置生效；
- 每个 `$` 生成一个参数，按从左到右的顺序填充；
- 多占位符返回多参数函数，不是自动柯里化；
- 非占位参数在被调用时求值；
- 嵌套调用的 `$` 绑定到最近的调用；
- `_` 仍然是 REPL 的上一次结果别名。

参数表达式中出现的 `return` / `break` / `continue` 会向外传播，当前函数调用不会执行：

```text
func f(x) { return x; }
func g() {
  let r = f(match (1) { case 1 => { return 10; } default => 0; });
  return 99;
}
g()    // 10
```

### 4.8 匿名函数

```text
let add = func(a, b) { return a + b; };
let inc = func(x) -> x + 1;

(func(x) { return x + 1; })(41)
(func(x) -> x + 1)(41)
```

也可以使用 `->` 简写，函数体是单个表达式：

```text
let inc = x -> x + 1;
let add = (a, b) -> a + b;
let zero = () -> 7;
```

- `->` 只会生成表达式体；多语句请使用 `func`；
- 单参数使用 `x -> ...`，多参数使用 `(a, b) -> ...`，零参数使用 `() -> ...`；
- `->` 的优先级最低，函数体延伸到表达式末尾。

命名函数和 `func` 匿名函数的参数可以直接解构：

```text
func sum([a, b]) { return a + b; }
func pair({x, y}) { return x * y; }
struct P { x: int; y: int; }
func point(P {x, y}) { return x + y; }
func first([head, ...tail]) { return head; }
```

解构参数占一个参数位置；支持 list、dict、struct 和 enum 变体模式。解构失败会报
`function parameter does not match its pattern`。

匿名函数体使用 `list` 语法，支持多语句和显式 `return`。

单表达式可以使用 `->` 简写，表达式结果就是返回值：

```text
std.list.map([1, 2, 3], func(x) -> x * 2)
std.list.filter([1, 2, 3], func(x) -> x % 2 == 1)
```

- `->` 的优先级最低，`func(x) -> x * 2 + 1` 的函数体是 `x * 2 + 1`；
- 匿名表达式函数不需要分号；
- 多语句仍然使用 `{ ... }` 和显式 `return`。

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
- `type_module(x)` 返回逻辑包名：入口为 `main`，包内类型为包名；
- `type_source(x)` 返回调试用源文件路径；普通值返回空字符串；
- `is` 用类型名判断运行时值是否匹配。

`kind_of` 返回值：

```text
int float bool string bytes list dict deque heap set
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

`is` 只接受类型名或 trait 名，不接受任意表达式作为右操作数。类型名按包解析，因此同名结构体不会混淆。

### 4.12 运算优先级

从低到高：

```text
->
= += -= *= /= %= &= |= ^= <<= >>=
? :
||
&&
|
^
&
比较运算
is
<< >>
+ -
* / %
|> |>>
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
- 不可变标量 `const` 在同一个编译单元内可参与常量折叠，不改变运行时语义；
- 初始化失败或类型不匹配时不会创建绑定。

支持的类型：

```text
any int float number bool string char byte bytes
list dict deque heap set function builtin callable null userdata result error
```

任意类型可以加 `?` 变成可空类型，例如 `int?`、`char?`、`Node?`：

```text
let a: int? = null;
let b: int? = 3;
let c: char?;       // 默认 null
```

可空类型接受 `null` 或对应类型的值，其它值仍会报错。

默认值：

| 类型 | 默认值 |
|---|---|
| `int` / `number` | `0` |
| `float` | `0.0` |
| `bool` | `false` |
| `string` | `""` |
| `char` | `'\0'` |
| `byte` | `byte(0)` |
| `bytes` | 空 bytes |
| `list` | `[]` |
| `dict` | `{}` |
| `deque` | 空 deque |
| `set` | 空 set |
| `any` / `null` | `null` |
| `T?` | `null` |

`function`、`builtin`、`callable`、`userdata`、`heap` 没有默认值，必须提供初始值。

已声明的 `struct` / `trait` 类型也可以用于类型标注，但没有默认值，必须提供初始值：

```text
let x: int = 1;
x = 2;              // 允许
x = "two";          // 错误：variable x expects int, got string

let s: string;
s == ""             // true
```

普通类型标注只约束绑定，不约束容器内部元素：

```text
let a: list = [0];
a[0] = "x";         // 允许
```

#### 容器泛型约束

内建容器可以写类型参数，泛型参数只用于静态检查，运行时会擦除：

```text
let xs: list[int] = [1, 2];
let d: dict[string, list[int]] = {"a": [1]};
let s: set[int] = std.set.from_list([1, 2]);
let q: deque[int] = std.deque.from_list([1, 2]);
let h: heap[int] = std.heap.from_list([1, 2], less);
```

- 支持 `list[T]`、`dict[K,V]`、`set[T]`、`deque[T]`、`heap[T]`，以及嵌套和可空形式，例如 `list[int?]`、`dict[int, list[int]]`、`list[int]?`；
- 只支持内建容器的类型参数，不支持用户自定义泛型：`struct Box<T>`、`func f<T>`、泛型 trait / 别名都不允许；
- 用户类型可以作为参数，例如 `list[Point]`、`dict[string, Point]`；
- 不带参数的 `list` / `dict` / `set` / `deque` / `heap` 仍然表示无元素约束。

静态检查覆盖初始化与插入操作：

```text
let xs: list[int] = [1, "a"];          // 错误：元素类型不匹配
xs[0] = "a";                           // 错误
xs = std.list.append(xs, "a");         // 错误
xs += ["a"];                           // 错误

let d: dict[int, int] = {1: "a"};      // 错误
d["a"] = 1;                            // 错误：key 类型不匹配
d[1] = "a";                            // 错误：value 类型不匹配

let s: set[int] = std.set.from_list([1, "a"]);  // 错误
s = std.set.add(s, "a");                         // 错误
let q: deque[int] = std.deque.from_list([1, "a"]); // 错误
q = std.deque.push_back(q, "a");                   // 错误
let h: heap[int] = std.heap.from_list([1, "a"], less); // 错误
h = std.heap.push(h, "a");                            // 错误
```

运行时只检查外层容器类型，不扫描元素：`list[int]` 在运行时就是 `list`。
因此通过未注解代码、`any`、FFI 或 JSON 解码进入的元素不会被运行时检查。

静态检查模式：

```text
SKEIN_TYPECHECK=off|loose|warn|strict
--typecheck=off|loose|warn|strict
```

- `off`：关闭静态检查，只保留运行时外层检查；
- `loose`：确定冲突报错，不产生 `any` 警告；
- `warn`（默认）：确定冲突报错，隐式 `any -> 容器[T]` 发 warning；
- `strict`：隐式 `any -> 容器[T]` 也报错。

对动态边界使用 `as` 显式断言：

```text
let raw = std.json.parse(text);
let xs = raw as list[int];            // 显式信任边界
let ys: list[int] = raw as list[int];
```

- `as` 只影响静态类型，运行时是恒等操作，不做类型转换；
- `as` 不会掩盖确定冲突：`[1, "a"] as list[int]` 仍然报错；
- 对 `as` 结果取下标或字段需要加括号：`(raw as list[int])[0]`、`(raw as Point).x`；
- `as` 不能作为赋值目标。

`is` 和模式类型只接受不带类型参数的类型：

```text
x is list              // 允许
x is list[int]         // 错误：generic type arguments are not allowed in 'is'
```

`for` 初始化部分也支持类型标注：

```text
for (let i: int = 0; i < 3; i++) {
  print(i);
}
```

解构声明：

```text
let {a, b} = {"a": 1, "b": 2};
let {a: x} = {"a": 9};              // key "a" 绑定到 x
let {"a-b": z} = {"a-b": 7};        // 非标识符 key 用字符串
let [h, ...t] = [1, 2, 3];
let {user: {name}} = {"user": {"name": "alice"}};

struct P { x: int; y: int; }
let P {x, y} = P { x: 1, y: 2 };
```

规则：

- `:` 左边是**字面量 key**，右边是绑定名；省略 `:` 时绑定名等于 key；
- dict 开放匹配：只要求列出的 key 存在，多余的忽略；
- `...rest` 收集未列出的键或剩余元素；
- list 没有 `...rest` 时要求长度完全相等；
- 缺 key、长度不匹配、struct 字段不匹配 → 运行时错误；
- 绑定进入当前作用域：`let` 可写，`const` 只读；
- 不支持默认值、解构赋值、多变量声明、`as` 整体绑定；
- 未绑定的 key 可以通过 `...rest` 或先保留原值访问。

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
- deque / heap / set 单变量遍历元素（heap 按层序，set 按插入顺序）；双变量形式仍只支持 dict；
- dict 单变量遍历 key，双变量得到 `key, value`；
- string 单变量遍历 UTF-8 字符，元素是 `char`；
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
char
byte
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

### 5.7 match

`match` 是**表达式**：按顺序尝试模式，返回第一个匹配 arm 的值。

```text
let next = match (state) {
  case "idle" => "planning";

  case {kind: "tool_result", ok: true, data: d} => d;

  case [head, ...tail] => head;

  case n if n > 10 => n;

  case is int as n => n;

  case is Point as p => p.x;

  case Point {x, y} => x + y;

  default => state;
};
```

规则：

- 选择值只求值一次；
- 首匹配生效，无 fallthrough；
- arm 体可以是单条语句或代码块；表达式 arm 需要 `;`，块不需要；
- arm 的值是块内最后一条语句的值；
- 绑定只读，作用域限于该 arm；
- `if` 守卫失败时继续尝试下一个 arm；
- 没有 arm 命中且没有 `default` → 运行时错误 `match: no arm matched`；
- enum 匹配在没有 `default` 且缺少变体时报告 `match is not exhaustive: missing Enum.Variant`；
- `case is Type` 是类型模式，例如 `case is int`、`case is Point`；
- 模式匹配无副作用，不能调用函数，也不能写比较表达式；
- 不支持 float 字面量，浮点判断用 `if/else`；
- `as` 可以把整个匹配值绑定到变量：`case Point {x} as p => ...`；
- `case == CONST` 匹配一个常量/名字/无载荷枚举变体；
- `match (a, b)` 是多值 scrutinee，会当作 `match ([a, b])` 处理。

支持的模式：

| 模式 | 示例 | 说明 |
|---|---|---|
| 字面量 | `1`、`"idle"`、`'a'`、`true`、`null` | 按 `==` 比较 |
| 值模式 | `case == IDLE`、`case == E.A` | 匹配常量、名字或无载荷枚举变体 |
| 绑定 | `x` | 匹配任意值并绑定 |
| 通配 | `_` | 匹配任意值，不绑定 |
| 整体绑定 | `p as Point {x}` | 先匹配子模式，再把整个值绑定到 `p` |
| 类型 | `case is int`、`case is Point as p` | 按运行时类型匹配，支持内建类型、结构体、enum 和 trait |
| or | `case 1 \| 2`、`case E.A \| E.B` | 任一子模式匹配即可 |
| 整数范围 | `case 1..9` | 包含两端，按 int 比较 |
| dict | `{kind: "tool", data: d}` | 开放匹配，只要求列出的字符串 key 存在 |
| dict rest | `{kind, ...rest}` | `rest` 收集未列出的键 |
| list | `[a, b]` | 定长匹配 |
| list rest | `[h, ...t]` | `t` 是剩余元素组成的新 list |
| struct | `Point {x, y}` | 按字段匹配，未知字段报错 |
| struct 任意 | `Point {}` / `Point { ... }` | 只校验类型 |
| dict 任意 | `{...}` | 匹配任意 dict |
| 守卫 | `case n if n > 0 => ...` | 守卫可访问已绑定变量 |

多值 scrutinee：

```text
match (state, event) {
  case [State.Idle, Event.UserMsg { text }] =>
    State.Planning { goal: text };
  default => state;
}
```

等价于 `match ([state, event])`，所以模式要写成 list 模式。

示例：

```text
print(match ({"kind": "tool", "ok": true, "data": [1, 2]}) {
  case {kind: "tool", ok: true, data: d} => d;
  default => null;
});
```

`std.regex.match` 的 `match` 是字段名，不受关键字影响，仍然可用：

```text
import std;
std.regex.match("a+", "aaa")
```

### 5.8 代码块

```text
{
  let x = 1;
  print(x);
}

{ ; }
```

空代码块可以写在控制流/函数体位置（如 `if (x) {}`、`func f() {}`），也可以显式写成 `{ ; }`。块内单独出现的 `{}` 按空字典表达式处理；表达式位置的 `{}` 也是空字典。

### 5.9 return / break / continue

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

### 5.10 错误处理

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

### 5.11 命名函数

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

func double(x) -> x * 2;
```

- 参数用逗号分隔；
- 参数可以带可选的类型约束：`name: type`；
- 支持空参数；
- 支持默认参数：`name = expr` 或 `name: type = expr`；
- 默认参数必须排在必填参数之后；
- 默认值在调用时求值，可以引用更早的参数和方法中的 `self`，不能引用后面的参数；
- 默认值每次调用重新求值；
- 支持最后一个参数为可变参数；
- 函数体使用 `{ list }`；
- 单表达式可以写成 `-> exp`，隐含 `return`；
- 函数可以带返回类型标注：`func f(...): T { ... }` 或 `func f(...): T -> exp;`；
- 函数/lambda 的表达式体统一使用 `->`；`=>` 只用于 match/switch arm 和 FFI 返回类型；
- 表达式函数需要以 `;` 结束；
- 函数调用深度上限为 1000。

参数类型约束在调用时进行运行时检查，不做隐式转换。支持：

```text
any int float number bool string char byte bytes
list dict deque heap set function builtin callable null userdata result error
```

示例：

```text
func apply(fn: callable, x: int) {
  return fn(x);
}

apply(sqrt, 16)
```

默认参数：

```text
func greet(name: string, greeting: string = "hello") {
  return greeting + ", " + name;
}

func offset(a: int, b: int = a + 1) -> b;

greet("alice");          // "hello, alice"
greet("alice", "hi");    // "hi, alice"
offset(10);              // 11
```

默认值可以引用更早的参数：

```text
func span(start: int, end: int = start + 1, step: int = end - start) {
  return [start, end, step];
}

span(3)                  // [3, 4, 1]
```

默认值读取后面的参数会报错：

```text
func bad(a = b + 1, b = 2) { ... }
bad()                    // parameter b is used before it is initialized
```

方法中的默认值可以引用 `self`：

```text
struct Box { v: int; }
impl Box {
  func add(self, x: int = self.v + 1) -> self.v + x;
}

Box { v: 10 }.add()      // 21
```

`self` 本身不能有默认值，trait 方法声明也不能有默认值。

表达式函数同样支持类型约束和 `async`：

```text
func typed_double(x: int) -> x * 2;
async func fetch_value(x) -> await fetch(x);
```

返回类型标注：

```text
func numbers(): list[int] {
  return [1, 2];
}

func numbers_fast(): list[int] -> [1, 2];

func maybe(): int? {
  return;
}
```

- `: type` 是可选的静态返回类型；运行时不做转换或逐元素检查；
- 检查器会检查每个 `return` 表达式；`return;` 对非可空返回类型报错；
- 返回类型会传播到调用方，例如 `let xs: list[int] = numbers();` 后 `xs[0] = "a";` 会报错；
- 表达式体函数把 `-> exp` 当作隐式 `return exp` 检查；
- 方法也支持返回类型标注；
- 匿名函数 / lambda 暂不支持返回类型标注；
- 没有返回类型标注时，函数结果对检查器仍是 `any`。

```text
func numbers(): list[int] {
  return [1, "a"];   // 错误：expected int, got string
}

func bad(): int {
  return;            // 错误：expected int, got null
}
```

结构体 / impl 方法也支持表达式体：

```text
struct Box { v: int; }
impl Box { func twice(self) -> self.v * 2; }
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

匿名函数的参数同样支持类型约束、默认值和可变参数。默认值规则与命名函数一致，例如 `let inc = func(x = 0) -> x + 1;`。

#### 5.11.1 命名函数装饰器

命名函数支持 `@expr` 装饰器：

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

规则：

- `@expr` 放在命名 `func` 声明前，可以连续写多个；
- 支持普通函数、`async func`、generator 函数和 `export func`；
- `@` 写在声明最前面，例如 `@dec export async func f`，而不是
  `export @dec func f`；
- 多个装饰器从下往上应用，`@a @b func f` 等价于 `a(b(f))`；
- 装饰器表达式在函数绑定前求值，因此不会被同名函数遮蔽；应用在函数创建后完成；
- 装饰器是普通高阶函数，不需要专用定义语法；
- 装饰器收到 unbound function；对 async/generator 函数，装饰器可以返回普通函数、async 函数或 generator 函数，最终结果必须是 callable；
- 装饰器在声明执行时应用一次，不随函数调用重复执行；
- 函数名最终绑定到装饰后的值，递归调用会经过装饰器；
- 装饰器表达式支持名称、成员访问、调用、下标和括号表达式；任意表达式请加括号，例如 `@(make_decorator(x))`；
- 装饰器结果非 callable 时在声明执行时报 `expects callable`。

带参数的装饰器：

```text
func retry(times: int) {
  return func(fn) {
    return func(...args) {
      let result = null;
      for (let i in range(times)) {
        result = fn(...args);
      }
      return result;
    };
  };
}

@retry(3)
func fetch(x) { return x; }
```

当前限制：

- 匿名函数和 `let f = func(...)` 仍然手动包装：`f = dec(f)`；
- 不支持 `struct` / `enum` / `trait` 声明装饰器；
- 不支持在 `let` / `const` 声明前写 `@`；
- 方法装饰器见 5.12.1。

### 5.12 结构体、impl 与 trait

结构体：

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
```

构造和访问：

```text
let p = Point { x: 3, y: 4 };

p.x
p.x = 5;
p.length()
```

copy-with 与字段默认值：

```text
struct Config {
  host: string = "localhost";
  port: int = 80;
}

let a = Config {};
let b = Config { port: 8080 };
let c = Config { ...a, port: 9090 };
let d = Point { ...p, x: 9 };
```

规则：

- 字段必须全部初始化；有默认值的字段可以省略；
- 未知字段、重复字段、字段类型不匹配都会报错；
- 字段顺序不影响构造；
- 支持 copy-with：`Point { ...p, x: 9 }`，`...` 必须放在最前，后面的显式字段覆盖原字段；
- 字段可以声明默认值：`struct Config { host: string = "localhost"; port: int = 80; }`；
- 默认值在定义包作用域中求值，每次构造重新求值；不能引用 `self` 或其他字段；
- `std.struct.decode` 不自动填充默认值，缺失字段仍然报错；
- `struct` 只声明字段；所有方法必须写在 `impl Type` 中；
- `impl Type` 用于声明该类型的固有方法；
- 方法第一个参数必须是显式 `self`；
- `self` 的类型隐式为当前结构体；
- 方法参数支持默认值，但 `self` 不能有默认值；默认值可以引用 `self` 和更早的参数；
- 方法通过实例调用：`p.length()`，调用时不传 `self`；
- 方法值可以提取：`let f = p.length; f()`；
- 方法值绑定提取时的结构体快照，之后修改原结构体不会影响它；
- 方法可以修改 `self` 的字段和下标，修改的是接收者副本，调用方原值不变；需要保留修改时返回新结构体；
- 结构体使用 copy-on-write 值语义；
- 所有成员都是公开的，`_` 前缀没有特殊含义；
- 字段类型必须是已声明的类型；支持自引用，例如 `struct Node { value: int; next: Node?; }`；
- 不允许创建环：`a.next = b; b.next = a;` 会报 `recursive value would create a cycle`；共享子结构（DAG）允许；
- 字段可以保存 `callable` / `function` 值，作为普通回调使用，不会自动绑定 `self`；
- 不支持继承、静态方法、用户自定义泛型、动态添加方法或运算符重载；内建容器的类型参数见变量类型标注章节。

#### 5.12.1 方法装饰器

`impl` 方法支持 `@expr` 装饰器：

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

let p = Point { x: 3, y: 4 };
p.area()      // enter / exit / 12
```

规则：

- `@expr` 放在 `impl` 方法声明前，可以连续写多个；
- 支持固有方法和 `impl Trait for Type` 方法；
- 多个装饰器从下往上应用，`@a @b func m` 等价于 `a(b(m))`；
- 装饰器在 `impl` 声明执行时应用一次，不随方法调用重复执行；
- 装饰器就是普通高阶函数，不新增 `decorator` 关键字；
- 装饰器表达式支持名称、成员访问、调用和下标；任意表达式请加括号，例如 `@(make_decorator(x))`；
- 装饰器必须返回 callable，否则在 `impl` 安装时报告 `expects callable`；
- 方法装饰器收到的是 unbound function，`self` 是第一个参数；推荐用 `...args` 转发；
- 装饰器可以修改参数，包括 `args[0]`（`self`），再调用原方法；
- 调用点不变：`p.area()`、`let f = p.area; f()`、trait 分派都经过装饰器；
- `std.meta.methods` / `std.meta.traits` 看到的方法签名不变。

方法装饰器的默认值由生成的 wrapper 解析，因此装饰器看到的是已经补全默认值的参数；默认值仍可引用 `self` 和更早的参数，并在每次调用时重新求值。

当前限制：

- 自由函数装饰器见 5.11.1；
- 不支持 `struct` / `enum` / `trait` 声明装饰器；
- 被装饰方法不能使用解构参数；
- 被装饰方法不能是 generator；
- async 方法装饰器要求装饰器返回 async callable，见 5.12.2；
- 装饰器不能改变方法名或声明签名。

带参数的装饰器就是返回装饰器的普通函数：

```text
func retry(times: int) {
  return func(fn) {
    return func(...args) {
      let result = null;
      for (let i in range(times)) {
        result = fn(...args);
      }
      return result;
    };
  };
}

impl Point {
  @retry(3)
  func area(self) { return self.x * self.y; }
}
```

#### 5.12.2 async 方法

方法可以声明为 `async`：

```text
struct Client {
  host: string;
}

impl Client {
  async func get(self, path: string) {
    return await std.http.get_async(self.host + path);
  }
}

let c = Client { host: "http://127.0.0.1:8080" };
let result = await c.get("/api");
```

规则：

- `p.m()` 返回 task，`await p.m()` 或 `std.task.wait(p.m())` 等待结果；
- 方法值 `let f = p.m; await f()` 同样返回 task；
- async 方法内部可以 `await`、`defer`、使用默认参数和可变参数；
- 默认值在 task 开始前求值；
- `self` 是接收者副本；字段中的 userdata 等共享句柄正常共享，普通字段修改不会自动回写调用方；
- enum 方法也可以声明为 `async func`；
- generator 方法仍然不支持 async generator。

async trait 方法：

```text
trait Service {
  async func handle(self, request);
}

impl Service for Client {
  async func handle(self, request) {
    return await self.get(request["path"]);
  }
}

async func dispatch(service: Service, request) {
  return await service.handle(request);
}
```

规则：

- trait 声明 `async func` 时，实现也必须是 `async func`；
- trait 声明普通 `func` 时，实现不能是 `async func`；
- `impl Service for Client;` 可以适配已有的 async 方法；
- async trait 方法仍然不能有默认值或可变参数。

async 方法装饰器：

```text
func timed(fn) {
  return async func(...args) {
    print("before");
    let result = await fn(...args);
    print("after");
    return result;
  };
}

impl Client {
  @timed
  async func get(self, path) {
    return await std.http.get_async(self.host + path);
  }
}
```

- 装饰器本身仍是普通函数，返回 async callable；
- 直接把 `async func` 当装饰器会返回 task，不是 callable，会报错；
- 生成的 wrapper 是 async，并 `await` 装饰后的方法，因此 before/after 能包围真实异步工作；
- 当前取消语义：取消外层 task 不会自动取消被 `await` 的内层 task；
- `std.meta.methods` 的方法描述包含 `async` 字段。

结构体类型可用于类型标注：

```text
let p: Point = Point { x: 1, y: 2 };

func show(p: Point) {
  print(p.x);
}
```

跨包类型：

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

- 类型默认只在声明包内可见；
- `export struct` / `export trait` 后才能通过包路径引用；
- 限定类型名使用 `.`；
- 未导出的类型不能通过 `aa.Hidden` 引用，但它的值仍能正常传递和调用方法；
- 其他包可以为自己的类型实现导入的已导出 trait：

```text
// main
import aa;
struct Q { y: int; }
impl aa.D for Q {
  func d(self) { return self.y; }
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
- trait 方法声明不支持默认值；实现方法可以额外提供默认值；
- 一个类型对同一个 trait 只能实现一次；
- trait 方法名不能与类型已有方法冲突；enum 上也不能与任意变体字段重名；
- `impl` 必须写在接收者类型所在的包中；trait 可以来自其他包，但必须是 `export trait`；
- 导出的 trait 签名只能引用已导出的类型或内建类型；
- 不支持 trait 继承、默认方法、泛型、静态方法；
- struct、enum 和 trait 名称共享类型命名空间，不能在同一个包中重名；
- 类型名按包注册，一个包对应一个源文件；不同包可以定义同名结构体或 trait；
- 类型名不会自动跨包可见；跨包返回的结构体值仍可以通过字段和方法使用。

### 5.13 枚举

```text
enum Event {
  UserMsg { text: string; };
  ToolResult { ok: bool; data: any; };
  Timeout;
}
```

构造：

```text
let a = Event.UserMsg { text: "hi" };
let b = Event.Timeout;
```

规则：

- 变体字段必须全部初始化；有默认值的字段可以省略；
- 未知字段、重复字段、字段类型不匹配都会报错；
- 无载荷变体直接写成 `Event.Timeout`；
- 变体字段支持默认值；所有字段都有默认值时允许 `Event.Variant {}`；
- 默认值在定义包作用域中求值，每次构造重新求值；不能引用 `self` 或其他字段；
- copy-with 目前只支持 struct，不支持 enum；
- 载荷字段只读：`a.text` 可读，`a.text = ...` 会报错；
- 字段访问按当前变体校验：`Event.Timeout.text` 报错；
- 方法只能写在 `impl Event` 中；方法内 `self` 只读，修改状态时返回新 enum；
- `impl Trait for Event` 可以让 enum 参与 trait 接口；enum 也能用于 `is` 和类型标注；
- 方法名不能与任意变体的字段重名；
- `kind_of(e)` → `"enum"`，`type_name(e)` → `"Event"`，`type_module(e)` → 包名；
- 类型标注：`let e: Event = Event.UserMsg { text: "hi" };`；
- 相等按 `def + variant + payload` 比较。

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

`match` 变体模式：

```text
match (e) {
  case Event.UserMsg { text } => text;
  case Event.ToolResult { ok: true, data: d } => d;
  case Event.Timeout => "timeout";
  default => "other";
}
```

- 同一变体可以出现多个 arm，靠字段模式或守卫分流；
- 未列出的载荷字段忽略，写错字段名报错；
- `case Event.Timeout` 只匹配该变体；
- `case Event.ToolResult { ... }` 匹配该变体的任意载荷。

JSON：

```text
std.json.stringify(Event.UserMsg { text: "hi" })
// {"$enum":"Event.UserMsg","payload":{"text":"hi"}}

std.json.stringify(Event.Timeout)
// {"$enum":"Event.Timeout","payload":{}}
```

暂不支持：互递归 enum（自引用 enum 已支持）、enum 作为 dict key、enum 的 JSON 自动解码。

### 5.14 导出声明

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
导出类型后，其他包可以用 `包名.类型名` 作为类型标注或 `is` 的右操作数。

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

- 入口脚本的包固定是 `main`；`package main {}` 可以位于任意目录，也可以省略；
- `package name {}` 创建独立作用域；
- 入口脚本声明其他包名会报错；普通包文件不能直接作为入口运行；
- 被 `import` 的包名必须与所在文件夹名相同，且不能是 `main`；
- `main` 不能被 `import`；
- stdin / REPL 没有文件包身份，`package name {}` 仍可创建临时命名空间；
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
- 项目根由从主脚本目录向上查找的 `.skeinroot` 标记文件确定；没有标记时使用主脚本目录（stdin 模式使用当前目录）；
- 裸 `std` 才是内置标准库；`.std`、`..std`、`@std` 都按文件系统包路径解析；
- `main` 不能被导入；`import main;` 和 `import main.*;` 会报错；
- 普通包文件不能声明 `package main {}`；
- 别名必须是普通标识符；`as` 是保留字；
- import 语句需要分号；
- 包名绑定到当前作用域。

### 6.3 目录约定

```text
aa/
  bb/
    cc/
      cc.skein
```

导入：

```text
import aa.bb.cc;
```

入口文件优先使用 `<name>.skein`，其次 `index.skein`；如果目录中只有一个 `.skein` 文件，也会使用它。

父目录没有源文件时，也可以作为纯子包容器。

### 6.4 内置标准库 std

```text
import std;
```

`std` 是内置包，不需要文件系统目录，也不参与包目录名规则；`std` 名称保留，不能被源码包覆盖。

模块：

```text
std.list
std.fn
std.iter
std.dict
std.json
std.string
std.bytes
std.encoding
std.unicode
std.regex
std.time
std.random
std.env
std.crypto
std.compress
std.math
std.io
std.fs
std.process
std.worker
std.type
std.meta
std.module
std.net
std.http
std.tls（可选）
std.task
std.generator
std.result
std.error
std.enum
std.struct
std.lru
std.ffi（可选）
std.fsm（内嵌 Skein 源码包）
std.set（原生实现）
std.deque（原生实现）
std.heap（原生实现）
std.route（内嵌 Skein 源码包）
std.graph（内嵌 Skein 源码包）
std.arena（内嵌 Skein 源码包）
std.path（内嵌 Skein 源码包）
```

常用示例：

```text
std.math.sqrt(9)
std.math.sin(0)
std.math.min(3, 1, 2)
std.io.print("hello")
std.io.println("hello")
std.io.read(stream, 4096)
std.io.write_all(stream, data)
std.io.read_all(stream, 16 * 1024 * 1024)
std.fs.read_file("data.txt")
std.fs.read_text("data.txt", "gbk")
std.fs.write_text("data.txt", "中国", "gbk")
std.regex.replace("[0-9]+", "a1b2", "#")
std.time.format(0, "%Y-%m-%d", true)
std.random.uuid()
std.env.get("HOME")
std.crypto.sha256("abc")
std.compress.gzip(bytes_from_string("x"))
std.process.command_status("true")
std.type.name(1)
std.net.resolve("localhost")

std.list.slice([1, 2, 3], 1)
std.list.slice([1, 2, 3], 1, 2)
std.list.contains([1, 2, 3], 2)
std.list.reverse([1, 2, 3])
std.list.sort([3, 1, 2])
std.list.sort([3, 1, 2], func(a, b) { return a - b; })
std.list.sort_by([{"n": 2}, {"n": 1}], func(p) { return p["n"]; })
std.list.sort_by(xs, key, {"desc": true})
std.list.sort_desc([3, 1, 2])
std.list.is_sorted([1, 2, 3])
std.list.append([1, 2], 3)
std.list.map([1, 2], func(x) { return x * 2; })
std.list.filter([1, 2, 3], func(x) { return x % 2 == 1; })
std.list.reduce([1, 2, 3], func(a, b) { return a + b; })

std.fn.compose(func(x) -> x + 1, func(x) -> x * 2)(5)
std.fn.pipe(func(x) -> x + 1, func(x) -> x * 2)(5)
std.fn.partial(func(a, b) -> a + b, 10)(5)
std.fn.always(7)()

std.iter.collect(std.iter.map([1, 2, 3], func(x) -> x * 2))
std.iter.collect(std.iter.filter(range(0, 6), func(x) -> x % 2 == 0))
std.iter.collect(std.iter.take(range(0, 100), 3))
std.iter.collect(std.iter.enumerate(["a", "b"]))

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
std.encoding.decode(bytes_from_hex("d6d0"), "gbk")
std.encoding.encode("中国", "gbk")
std.encoding.supports("gb18030")
std.unicode.normalize("é", "nfc")
std.unicode.is_normalized("é", "nfc")
std.unicode.version()
```

排序：

```text
std.list.sort(xs, cmp = null)              // 数字/字符串，升序
std.list.sort_by(xs, key, options = null)  // options: {"desc": true}
std.list.sort_desc(xs)
std.list.is_sorted(xs, cmp = null)
```

- 稳定排序，返回新 list，不修改输入；
- 无 `cmp` 时元素必须全是数字、字符串、`char` 或 `byte`；
- `cmp` 返回 `< 0` / `0` / `> 0`；
- `sort_by` 的 key 必须返回数字、字符串、`char` 或 `byte`；
- `desc` 不破坏相等元素的原始顺序；
- 默认排序和 `sort_by` 中，`NaN` 永远排在最后（升序和降序都一样）。

列表算法：

```text
std.list.unique(xs)                 // 保序去重
std.list.unique_by(xs, key)         // 按 key 保序去重
std.list.group_by(xs, key)          // dict: key -> list
std.list.chunk(xs, n)               // [[...], [...], ...]
std.list.window(xs, n[, step])      // 滑动窗口，step 默认 1
std.list.zip(a, b, ...)             // [[a0, b0], ...]，取最短
std.list.top_k(xs, k, cmp = null)        // 排序后取前 k 个（O(n log k)）
std.list.binary_search(xs, value, cmp = null) -> {found, index}
```

- `unique` / `unique_by` 保留第一次出现的元素；
- `group_by` 的 key 必须是合法 dict key，结果 dict 保持首次出现顺序；
- `chunk` / `window` 的 `n` 必须 >= 1，`window` 的 `step` 必须 >= 1；
- `chunk` 最后一块可以不满；`window` 只返回完整窗口；
- `zip` 以最短输入为准，多余元素丢弃；
- `top_k` 不修改输入，`k` 必须 >= 0；有 `cmp` 时语义与 `sort` 一致；
- `binary_search` 要求输入已升序；返回 `{found, index}`，未找到时 `index` 是插入点；
- 空列表的 `binary_search` 返回 `{found: false, index: 0}`。

示例：

```text
std.list.unique([1, 2, 1, 3, 2])        // [1, 2, 3]
std.list.chunk([1, 2, 3, 4, 5], 2)      // [[1, 2], [3, 4], [5]]
std.list.window([1, 2, 3, 4], 2)        // [[1, 2], [2, 3], [3, 4]]
std.list.zip([1, 2], ["a", "b"])        // [[1, a], [2, b]]
std.list.top_k([5, 1, 4, 2, 3], 3)      // [1, 2, 3]
std.list.binary_search([1, 3, 5], 4)    // {found: false, index: 1}
```

`unique` / `unique_by` 在 key 可哈希（int/bool/string/char/byte/bytes/null）时使用哈希去重，O(n)；否则回退到 O(n²) 比较。`top_k` 使用 partial sort，O(n log k)，结果仍按升序排列。

编码转换：

```text
std.encoding.decode(bytes, encoding[, errors])  → string
std.encoding.encode(string, encoding[, errors]) → bytes
std.encoding.supports(encoding)                 → bool
```

支持的编码包括 `utf-8`、`ascii`、`latin-1`、`windows-1252`、`utf-16le`、
`utf-16be`、`utf-16`、`utf-32le`、`utf-32be`、`utf-32`、`gbk` / `cp936`、
`gb18030`。GBK 和 GB18030 使用内置码表，不依赖平台 iconv 或代码页。
`errors` 支持 `strict`、`replace`、`ignore`。

```text
import std;

let s = std.encoding.decode(bytes_from_hex("d6d0b9fa"), "gbk");
print(s);                                      // 中国

bytes_hex(std.encoding.encode("中国", "gbk"))  // d6d0b9fa
bytes_hex(std.encoding.encode("€", "gbk"))     // 80
bytes_hex(std.encoding.encode("€", "gb18030")) // a2e3
```

Unicode 归一化：

```text
std.unicode.normalize(text, form)      → string
std.unicode.is_normalized(text, form)  → bool
std.unicode.version()                  → string
```

`form` 支持 `nfc`、`nfd`、`nfkc`、`nfkd`。字符串归一化只在显式调用时发生，
字符串比较、拼接、下标和 `len` 不会自动归一化。标识符按 NFC 归一化，不做 NFKC。

```text
import std;

let decomposed = std.encoding.decode(bytes_from_hex("65cc81"), "utf-8");
let composed = std.unicode.normalize(decomposed, "nfc");
bytes_hex(std.encoding.encode(composed, "utf-8"))  // c3a9

std.unicode.is_normalized(decomposed, "nfc")       // false
std.unicode.is_normalized(composed, "nfc")         // true
std.unicode.version()                              // "16.0.0"
```

正则表达式：

```text
std.regex.match(pattern, text[, flags])       → bool
std.regex.search(pattern, text[, flags])      → dict / null
std.regex.find_all(pattern, text[, flags[, limit]]) → list
std.regex.replace(pattern, text, replacement[, flags]) → string
std.regex.split(pattern, text[, flags])       → list
```

使用 PCRE2 语法，兼容常用 ECMAScript 写法；Unicode 码点转义使用 `\x{...}`（如 `\x{4f60}`），不是 `\uXXXX`。`flags` 支持 `i`（忽略大小写）、`m`（多行）和 `u`（Unicode 模式）。

```text
std.regex.match(".", "你", "u")               // true
std.regex.match("\\p{L}+", "中文", "u")        // true
std.regex.match("é", "É", "iu")               // true

let m = std.regex.search("(\\p{L}+)", "123中文456", "u");
m["text"]                                     // 中文
m["start"]                                    // 3（码点偏移）
m["end"]                                      // 5（码点偏移）
```

- 不带 `u` 时按 UTF-8 字节匹配，`search` / `find_all` 的 `start` / `end` 是字节偏移；
- 带 `u` 时按 Unicode 码点匹配，支持 `\p{...}`、Unicode 字符类和 Unicode 大小写折叠，`start` / `end` 是码点偏移；
- `u` 模式要求模式和文本是合法 UTF-8；
- `replace` 使用 `$1` / `${1}` 引用捕获组。

时间、随机数和编码工具：

```text
std.time.now() / now_ms() / monotonic_ms() / utc_offset()
std.time.format(timestamp[, format[, utc]])
std.time.parse(text[, format[, utc]])
std.time.sleep(ms)

std.random.seed(n)
std.random.int(min, max)
std.random.float()
std.random.bytes(n)
std.random.choice(list)
std.random.shuffle(list)
std.random.uuid()

std.encoding.base64_encode(bytes) / base64_decode(string)
std.encoding.url_encode(string) / url_decode(string)
std.crypto.sha256(data)
std.crypto.hmac_sha256(key, data)
std.compress.gzip(bytes) / gunzip(bytes)
std.compress.zlib(bytes) / unzlib(bytes)
std.compress.deflate(bytes) / inflate(bytes)
```

环境变量、目录、路径和进程：

```text
std.env.get(name[, default]) / has(name) / set(name, value)
std.env.unset(name) / all()

std.fs.listdir(path) / mkdir(path[, recursive])
std.fs.remove(path) / rename(old, new) / stat(path)
std.fs.cwd() / chdir(path)
std.fs.path_join(parts...) / path_basename(path)
std.fs.path_dirname(path) / path_extname(path)
std.fs.path_abs(path) / path_normalize(path)

std.process.run(command[, options]) → dict
```

`std.process.run` 的 `options` 支持 `stdin`、`cwd`、`env`，返回
`{"status", "stdout", "stderr"}`，其中 stdout/stderr 是 `bytes`。

长驻进程：

```text
std.process.spawn(file[, options]) → process
std.process.wait(process[, timeout_ms]) → status
std.process.kill(process[, signal])
std.process.close(process)
std.process.pid(process)
```

`options` 支持 `args`、`env`、`cwd`、`stderr`（`pipe` 或 `inherit`）。
`spawn` 返回包含 `pid`、`stdin`、`stdout`、`stderr` 和 `handle` 的 dict；
四个流都支持 `std.io` 的同步和异步读写；`stderr = "inherit"` 时 `stderr` 为 `null`。

```text
import std;

let p = std.process.spawn("/bin/cat");
std.io.write_all(p["stdin"], "hello");
std.io.close(p["stdin"]);
bytes_to_string(std.io.read(p["stdout"], 4096))
std.process.wait(p)
std.process.close(p)
```

`std.io.stdin` / `std.io.stdout` / `std.io.stderr` 是标准流对象。

Worker 进程池：

```text
std.worker.pool(size, script[, options]) -> pool
std.worker.call(pool, method, args[, timeout_ms]) -> result
std.worker.map(pool, method, items[, timeout_ms]) -> list
std.worker.submit(pool, method, args) -> job
std.worker.wait(job[, timeout_ms]) -> result
std.worker.shutdown(pool)
std.worker.stats(pool) -> dict
```

worker 脚本使用 JSON Lines 协议，并先发送 ready 帧：

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
std.result.unwrap(std.worker.call(pool, "add", [2, 3]))
std.worker.map(pool, "add", [[1, 2], [3, 4]])
std.worker.shutdown(pool)
```

`call` / `map` 的超时或 worker 崩溃会返回错误，并重启对应 worker。
当前协议是 JSON Lines，不支持原始 `bytes`。

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
- `stringify` 拒绝 `bytes`、`function`、`userdata`、`result`、`error` 和 `NaN` / `Inf`；
- `struct` 编码为普通 object，`enum` 编码为 `{"$enum":"Type.Variant","payload":{...}}`，`char` 编码为单字符字符串，`byte` 编码为整数；
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

带编码的文本文件：

```text
std.fs.read_text(path, encoding[, errors])       → string
std.fs.write_text(path, text, encoding[, errors]) → null
```

`encoding` 使用 `std.encoding` 支持的编码名；`errors` 支持 `strict`、`replace`、
`ignore`。例如：

```text
import std;

std.fs.write_text("data-gbk.txt", "中国", "gbk");
std.fs.read_text("data-gbk.txt", "gbk")          // 中国
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
- `std.io`：`print`、`println`、`format`、`input`，以及文件和 socket 通用的流式读写；`print` 不追加换行，`println` 输出后自动追加换行符；
- `std.fs`：文件读写和文件句柄操作；
- `std.process`：命令执行、`command_status` / `run` / `spawn`；
- `std.worker`：JSON Lines 进程池；
- `std.json`：JSON 解析与序列化，object 映射为 `dict`；
- `std.websocket`：WebSocket 握手与帧编解码；
- `std.type`：`kind_of`、`name`、`module`、`source`；
- `std.meta`：只读类型反射，查询字段、变体、方法和 trait；
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
retry(factory[, options]) → result
timeout(task, timeout_ms) → result
rate_limit(rate, burst) → rate_limiter
acquire(limiter[, tokens]) → bool
bounded_parallel_map(items, limit, fn) → list[result]
```

`std.task.yield` 是函数字段；单独的 `yield` 关键字用于生成器。`wait` 超时不会自动取消 task；需要显式调用 `cancel`。

task 是协作式调度的：只有运行事件循环时才会推进，例如 `wait`、`wait_all`、`await` 或同步网络调用期间。

目前 task 覆盖原生定时器、async 函数和 `std.net.*_async` 异步 I/O。

可靠性工具：

```text
std.task.retry(factory[, options]) -> result
std.task.timeout(task, timeout_ms) -> result
std.task.rate_limit(rate, burst)   -> rate_limiter
std.task.acquire(limiter[, tokens]) -> bool
std.task.bounded_parallel_map(items, limit, fn) -> list[result]
```

`retry` 每次调用 `factory()`；factory 可以直接返回值、返回 `result`，或返回 task。失败后按退避等待再试，最终失败返回最后一个 `err`。

options：

```text
attempts      总尝试次数，默认 3
delay_ms      首次等待毫秒数，默认 0
backoff       退避倍数，默认 2.0
max_delay_ms  单次等待上限，0 表示不限制
retry_on      错误 kind 字符串或列表；省略表示所有错误都重试
```

`timeout` 等价于带超时的 `try_wait`，超时返回 `err`，不会自动取消 task。

`rate_limit(rate, burst)` 返回共享的限流器；`acquire` 阻塞到拿到令牌为止。rate 是每秒补充的令牌数，burst 是上限。

`bounded_parallel_map` 最多同时运行 `limit` 个 task，结果按输入顺序返回，每个元素是 `result`；单项失败不会中断其他项。

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

协议升级（Upgrade）：

客户端：

```text
let r = std.http.get(url, {
  "upgrade": true,
  "headers": {"Upgrade": "websocket"}
});

if (r["upgraded"]) {
  std.io.write_all(r["socket"], "hello");
  bytes_to_string(std.io.read(r["socket"], 4096));
  std.io.close(r["socket"]);
}
```

服务端：

```text
let req = std.http.accept(s, 5000);
let stream = std.http.upgrade(req, {"X-Test": "yes"});

std.io.write_all(stream, "hello");
bytes_to_string(std.io.read(stream, 4096));
std.io.close(stream);
```

- `upgrade: true` 时客户端使用独立连接，不进入 keep-alive 连接池；
- 服务端返回 `101` 时，响应 dict 包含 `"upgraded": true` 和 `"socket"`；
- `std.http.upgrade(request[, headers])` 发送 `101 Switching Protocols` 并返回原始 stream；
- 升级后的读写使用 `std.io`；
- 当前只支持 HTTP/1.1 Upgrade；HTTP/2 extended CONNECT 尚未实现。

WebSocket：

```text
std.websocket.connect(url[, options])          → ws
std.websocket.connect_async(url[, options])    → task
std.websocket.accept(request)                  → ws
std.websocket.send(ws, data[, timeout_ms])     → bool
std.websocket.send_async(ws, data[, timeout_ms]) → task
std.websocket.receive(ws[, timeout_ms])        → message
std.websocket.receive_async(ws[, timeout_ms])  → task
std.websocket.ping(ws[, data])                 → bool
std.websocket.close(ws[, code[, reason]])      → bool
```

客户端：

```text
let ws = std.websocket.connect("ws://127.0.0.1:8080/");

std.websocket.send(ws, "hello");

let msg = std.websocket.receive(ws);
print(msg["type"] + ": " + msg["data"]);

std.websocket.close(ws);
```

服务端：

```text
let req = std.http.accept(s, 5000);
let ws = std.websocket.accept(req);

let msg = std.websocket.receive(ws);
std.websocket.send(ws, "echo:" + msg["data"]);
std.websocket.close(ws);
```

消息 dict：

```text
text   → {"type": "text", "data": string}
binary → {"type": "binary", "data": bytes}
pong   → {"type": "pong", "data": bytes}
close  → {"type": "close", "code": int, "reason": string}
```

规则：

- 支持 `ws://` 和 `wss://`；
- 客户端发出的帧自动 masking，服务端发出的帧不 masking；
- 自动回复 ping；
- 支持分片消息重组；
- 单条消息默认上限 16 MiB；
- `send` 对 string 使用 text 帧，对 bytes 使用 binary 帧；
- `close` 发送 close 帧并关闭底层 stream；
- 目前只支持 HTTP/1.1 WebSocket 握手，不支持 HTTP/2 extended CONNECT。

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
std.http.upgrade(request[, headers])
std.http.cookies(response)
std.http.read_body(request)
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

handler 拉取请求 body：

```text
let s = std.http.listen("127.0.0.1", 8080);
s["stream_request_body"] = true;

let served = std.http.serve(s, func(req) {
  let body = "";
  let chunk = std.http.read_body(req);
  while (chunk != null) {
    body = body + bytes_to_string(chunk);
    chunk = std.http.read_body(req);
  }
  return {"status": 200, "body": body};
});
```

- `stream_request_body` 为 `true` 时，handler 在请求 headers 到达后立即执行；
- 此时 `req["body"]` 为空，使用 `std.http.read_body(req)` 逐块拉取；
- 请求 body 结束时 `std.http.read_body` 返回 `null`；
- 不能与 `on_request_body` 同时设置；
- HTTP/1.1 下未读完 body 就返回响应会关闭连接；
- HTTP/2 下未读取的 body 会被丢弃；
- `std.http2.serve` 使用相同配置。

请求 dict：

```text
method, path, headers, body, http_major, http_minor, socket
```

启用 `stream_request_body` 后请求 dict 还包含 `body_reader`。

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
- 客户端 `options` 支持 `upgrade`，用于 HTTP/1.1 协议升级；
- `max_body_bytes` 默认 16 MiB；
- 同名 header 只保留最后一个；
- 客户端和服务端都支持同步和 async。

重定向：

```text
let r = std.http.get(url, {
  "follow_redirects": true,
  "max_redirects": 5
});
```

- `follow_redirects` 默认 `false`；
- `max_redirects` 默认 5，`0` 表示不跟随；
- 支持 `301` / `302` / `303` / `307` / `308`；
- `303` 和 `301/302` 的 POST 会改为 GET 并丢弃 body；
- `307` / `308` 保留方法和 body；
- `Location` 支持绝对 URL、`//host/path`、`/path`、`?query` 和相对路径；
- 跨 origin 重定向会删除 `Authorization` / `Cookie`；
- 重定向时删除 `Host`、`Content-Length`、`Transfer-Encoding`、`Connection`、`Upgrade`；
- `body_stream` 无法重放：`301/302/303` 会丢弃 body，`307/308` 会报错；
- 同步和 async 客户端都支持。

Cookie：

```text
let jar = {};

let r = std.http.get(url, {"cookie_jar": jar});
let cookies = std.http.cookies(r);

let next = std.http.get(url + "/next", {"cookie_jar": jar});
```

- `cookie_jar` 是普通 dict，可跨请求复用；
- 自动读取响应中的 `Set-Cookie`；
- 自动发送匹配 domain / path / secure / expires 的 `Cookie`；
- `Max-Age=0` 会删除对应 cookie；
- 用户显式提供 `Cookie` header 时不会覆盖；
- `std.http.cookies(response)` 返回解析后的 cookie 列表。

cookie dict：

```text
name
value
domain
path
secure
http_only
max_age
expires
same_site
host_only
```

- HTTP/1.1 客户端支持自动 cookie jar；
- HTTP/2 响应也提供 `set_cookie`，可用 `std.http.cookies` 解析；
- HTTP/2 单请求和 `request_many` 都支持自动 cookie jar。

压缩：

```text
let r = std.http.get(url, {"decompress": true});
print(bytes_to_string(r["body"]));
```

- `decompress` 默认 `false`；
- 开启后自动添加 `Accept-Encoding: gzip, deflate, br`（取决于构建支持）；
- 自动解码 `Content-Encoding`；
- 支持 `gzip`、`deflate`、`br`；
- 解码后响应带 `"decompressed": true`；
- 删除响应 header 中的 `Content-Encoding`，并更新 `Content-Length`；
- 不能与 `on_body` 同时使用；
- 同步和 async 客户端都支持；
- 构建选项 `-DSKEIN_ENABLE_COMPRESSION=OFF` 可关闭；
- HTTP/2 单请求和 `request_many` 都支持自动解压。


资源限制：

客户端：

```text
let r = std.http.get(url, {
  "max_header_bytes": 65536,
  "max_headers": 100,
  "max_body_bytes": 1048576
});
```

服务端：

```text
let s = std.http.listen("127.0.0.1", 8080);

s["max_header_bytes"] = 65536;
s["max_headers"] = 100;
s["max_body_bytes"] = 1048576;
s["max_connections"] = 100;
```

- 超过限制时请求失败或连接关闭；
- `max_connections = 0` 表示不限制；
- `std.http.accept` / `accept_async` 也使用 server dict 中的限制；
- HTTP/2 使用 `max_body_bytes` / `max_connections`，header 限制通过 `settings.max_header_list_size`。

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
- 每个请求项可以独立流式发送请求 body 和流式接收响应 body；
- `options` 中的 `cookie_jar` / `decompress` 会分别应用到每个请求项的 URL / path。

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
- 达到 `max_requests` 后进入 graceful drain：停止接受新连接和新 stream，已启动的 handler 会继续完成并发送响应；
- 达到 `max_per_connection` 后关闭当前连接，未处理的 stream 由客户端按幂等规则重试；
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
- defer 表达式只能引用在 defer 之前已经声明的局部变量、参数或 upvalue；
  引用同一函数/代码块中后续才声明的同名局部会报编译错误；
- 顶层 `defer` 不允许；
- 生成器正常结束会执行 defer；
- `std.generator.close(g)` 会执行生成器的 defer；
- 直接丢弃挂起生成器不会执行脚本级 defer；如果仍有待执行 defer，会打印 warning。

声明式 FFI 绑定：

```text
extern "c" lib shape from "./libshape.so" {
  type Point {
    x: f64;
    y: f64;
  }

  type PointPtr = *Point;

  callback Compare(a: i32, b: i32) => i32;

  func point_len(p: Point) => f64;
  func point_new(x: f64, y: f64) => Point;
  func sort(base: *Point, n: usize, cmp: Compare);

  const POINT_MAX: i32 = 16;
}

shape.point_len({"x": 1.0, "y": 2.0});
shape.POINT_MAX;
```

块内条目用 `;` 结束，返回类型箭头是 `=>`；声明 lowering 到 manifest，
运行时等价于 `std.ffi.open(path, manifest)`。

函数可以带所有权注解：

```text
func point_new(x: f64, y: f64) => *Point owns "point_free";
func point_destroy(p: *Point) consumes p;
func point_sort(base: *Point, n: usize, cmp: Compare) retains cmp;
```

- `owns "free_fn"`：返回的指针由绑定自动释放；
- `consumes p`：参数所有权转移给 C，之后脚本再访问会报错；
- `retains cb`：C 保存该回调/指针，绑定保持其存活。

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

- 只支持 C ABI 边界（C 库或 `extern "C"` 导出）；不支持 C++ ABI；
- 拒绝 packed/bitfield、union、柔性数组、匿名 struct/union、
  `long double`、`_Complex`、`__int128`、平台不支持的调用约定，以及
  `aligned` / `vector_size` / `mode` 等影响布局的属性；
- 变参函数通过 `std.ffi.call_variadic` 显式传入变参类型；
- `stdcall` / `fastcall` 通过 manifest 的 `calling_convention` 指定；
- `ffi.call_async` 不可取消、没有超时，当前 fiber 会等到 C 返回；可能挂死的调用应放到外部进程；
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

### 状态机 std.fsm

`std.fsm` 是**内嵌 Skein 源码包**：源码在 `lib/std/fsm.skein`，构建时嵌入二进制，`import std.fsm;` 即可使用，不需要安装额外文件。

```text
import std.fsm;

let m = fsm.define(0, func(state, event) {
  return state + event;
});

let ctx = fsm.start(m);
print(ctx.state);            // 0

ctx = fsm.step(m, ctx, 1);
ctx = fsm.step(m, ctx, 2);
print(ctx.state);            // 3

print(fsm.history(ctx));     // [0, 1, 3]

ctx = fsm.undo(ctx);
print(ctx.state);            // 1

ctx = fsm.redo(ctx);
print(ctx.state);            // 3

ctx = fsm.run(m, ctx, [1, 2, 3]);
print(ctx.state);            // 9
```

API：

```text
fsm.define(initial, step, on_transition = null) -> Machine
fsm.initial(m)                 -> any
fsm.start(m)                   -> Context
fsm.step(m, ctx, event)        -> Context
fsm.run(m, ctx, events)        -> Context
fsm.history(ctx)               -> list
fsm.undo(ctx)                  -> Context
fsm.redo(ctx)                  -> Context
fsm.snapshot(ctx)              -> dict
fsm.restore(snap, decode = null)    -> Context
```

规则：

- `step` 是纯函数 `(state, event) -> next_state`；
- `define` 的第三个参数可选，是 `on_transition(from, event, to)` 回调；
- 如果初始状态是 enum 或 struct，`step` 返回值必须保持同一类型，否则 `fail("step must return ...")`；
- `Context` 是值，可以赋值、传参、比较；
- `history` 包含初始状态和每一步的状态；
- `undo` / `redo` 是线性的；`undo` 之后再 `step` 会清空 redo，形成新分支；
- `snapshot` 返回普通 dict，可 JSON 序列化；`restore` 会重建历史并校验；
- 枚举状态在 JSON 往返时通过 `std.enum.decode` 自动恢复；
- struct 状态可传解码回调：`fsm.restore(snap, func(v) { return std.struct.decode(v, "S"); })`；
- `ctx._history` / `ctx._head` / `ctx._redo` 是内部字段，不要依赖；
- 非法 `step` 会 `fail("step must be callable")`，非法回调会 `fail("on_transition must be callable")`。

性能提示：单步 `step` 每次都会为值语义复制历史表，O(history)/次；`run` 在单次调用内复用同一份历史，整体 O(events)，`history` 为 O(n)。线性撤销/重做适合中小规模，超长历史请等待原生 fsm 实现。

### std.meta

```text
std.meta.type_info(type)                -> dict
std.meta.fields(type)                   -> list
std.meta.variants(type)                 -> list
std.meta.methods(type)                  -> list
std.meta.traits(type)                   -> list
std.meta.implements(type, trait_name)   -> bool
std.meta.has_type(type)                 -> bool
```

`std.meta` 是只读类型反射。`type` 可以是 struct / enum 实例、内建类型的值，
或类型名字符串（支持 `module.Type`）。`kind_of` 返回的 `struct` / `enum` /
`method` 等是运行时 kind，不是类型名，不能传给 `std.meta`；应使用
`type_name(value)` 或字符串。

字符串参数始终按类型名解释：`std.meta.type_info("Hello")` 查找名为
`Hello` 的类型；要查询 string 类型本身，请写 `std.meta.type_info("string")`。

```text
import std;

struct Point { x: int; y: int = 0; }
impl Point {
  func moved(self, dx: int, dy: int = 0) {
    return Point { x: self.x + dx, y: self.y + dy };
  }
}

let info = std.meta.type_info("Point");
info["kind"];                  // "struct"
info["name"];                  // "Point"
info["module"];                // "main"
info["fields"][0]["name"];     // "x"
info["fields"][1]["has_default"]; // true
info["methods"][0]["name"];    // "moved"
info["methods"][0]["params"];  // ["int", "int"]，不包含 self
info["methods"][0]["has_defaults"]; // true
info["methods"][0]["async"];   // false
```

`type_info` 的返回结构：

```text
{
  "kind": "struct" | "enum" | "trait" | "builtin",
  "name": "Point",
  "module": "main",
  "source": "main",
  "exported": true,
  "fields": [ ... ],
  "variants": [ ... ],
  "methods": [ ... ],
  "traits": [ ... ]
}
```

字段描述：

```text
{
  "name": "x",
  "type": "int",
  "nullable": false,
  "readonly": false,
  "has_default": false
}
```

enum 变体字段的 `readonly` 为 `true`。变体描述：

```text
{
  "name": "UserMsg",
  "fields": [ ... 字段描述 ... ]
}
```

方法描述中的 `params` 不包含 `self`，是调用时显式传入的参数类型；
`async` 表示该方法是否为 `async func`。

规则：

- `fields` / `variants` / `methods` / `traits` 对不适用的类型返回空列表；
- 只查询**已加载且可见**的类型：当前包/当前源文件中的类型可见；其他包的
  类型必须 `export`。不可见类型在 `type_info` 等函数中报
  `type X is not visible`，`has_type` 返回 `false`；
- 值形式的参数同样受可见性约束，不能用隐藏类型的值绕过；
- 未限定类型名跨包同名时报歧义，使用 `pkg.Type`；
- 内建类型使用 `kind = "builtin"`，例如 `"str"` 规范化为 `"string"`，
  `"func"` 规范化为 `"function"`；
- 内建类型的内建类型名包括 `any` / `int` / `float` / `number` / `bool` /
  `string` / `char` / `byte` / `bytes` / `list` / `dict` / `deque` / `heap` /
  `set` / `function` / `builtin` / `callable` / `null` / `userdata` /
  `result` / `error`；
- `lru` / `method` / `native` / `class` / `struct` / `enum` 是运行时 kind，
  不是类型名；
- `has_type` 对未知类型返回 `false`；其他函数对未知类型报错；
- `implements` 要求 trait 名已知，未知 trait 报错；
- 不返回值默认值，只提供 `has_default` 标记。

### std.enum

```text
std.enum.decode(value)                          -> enum
std.enum.decode(value, type_name)               -> enum
std.enum.decode(value, options)                 -> enum
std.enum.decode(value, type_name, options)      -> enum
std.enum.tag(value)                             -> string
std.enum.tag(type_name, variant_name)           -> string
```

`decode` 把 `std.json.parse` 得到的 `{"$enum":"Event.UserMsg","payload":{...}}` 还原成枚举值；嵌套枚举会递归还原。输入不是枚举对象时报错。

```text
import std;
enum Event { UserMsg { text: string; }; }
let e = std.enum.decode(std.json.parse(
  std.json.stringify(Event.UserMsg { text: "hi" })
));
```

可选类型名用于校验 tag 并消除跨包同名歧义：

```text
std.enum.decode(value, "aa.Event")
std.enum.decode(value, "aa.Event", {"defaults": true})
```

`options` 当前支持：

```text
{"defaults": true}
```

`defaults: true` 时，载荷中缺失且有默认值的字段会在声明包作用域中重新求值；
可空字段缺失时填 `null`。不传 options 时保持原有行为。



`tag` 返回 `module.Enum.Variant` 形式的规范字符串，丢弃载荷：

- 同一变体的不同载荷得到同一个 tag；
- 不同 enum 类型即使变体名相同，tag 也不同；
- tag 是普通字符串，可直接作 dict key、写进 JSON；
- 无实例时可用 `std.enum.tag("Event", "UserMsg")` 或跨包 `std.enum.tag("aa.Event", "UserMsg")`。

```text
enum E { A { n: int; }; B; }
std.enum.tag(E.A { n: 1 })   // "main.E.A"
std.enum.tag(E.A { n: 2 })   // "main.E.A"
std.enum.tag(E.B)            // "main.E.B"
std.enum.tag("E", "A")        // "main.E.A"
```

### std.struct

```text
std.struct.decode(value, type_name)                 -> struct
std.struct.decode(value, type_name, options)        -> struct
std.struct.decode(value, "aa.Point")               -> 跨包
```

schema 驱动解码：JSON 保持普通 object，类型由调用方提供。

```text
import std;
struct Point { x: int; y: int; }

let p = std.struct.decode({"x": 1, "y": 2}, "Point");
print(p);          // Point { x: 1, y: 2 }
```

`options` 当前支持：

```text
{"defaults": true}
```

`defaults: true` 时，缺失且有默认值的字段会在声明包作用域中重新求值。
默认值每次 decode 都重新计算，可能有副作用；求值失败或类型不匹配时报错。

```text
struct Config {
  host: string = "localhost";
  port: int = 80;
}

let c = std.struct.decode({"port": 8080}, "Config", {"defaults": true});
print(c.host);     // localhost
print(c.port);     // 8080
```

规则：

- `value` 必须是 dict；
- 未知字段、类型不匹配报错；
- 没有 `defaults` 时，缺字段报错；
- 有 `defaults` 时，缺失字段优先使用声明默认值；
- 可空字段 `T?` 缺失时填 `null`；
- 嵌套 struct / enum 递归解码，`defaults` 同样作用于嵌套字段；
- 递归 struct 依赖 JSON 深度限制；
- 跨包类型写成 `module.Type`；
- `std.json.stringify` 的输出格式不变，不引入 `$struct` 标签。

### std.lru

```text
std.lru.new(capacity)              -> lru
std.lru.get(lru, key[, default])   -> {lru, found, value}
std.lru.peek(lru, key[, default])  -> {lru, found, value}
std.lru.put(lru, key, value)       -> {lru, evicted}
std.lru.delete(lru, key)           -> {lru, removed}
std.lru.has(lru, key)              -> bool
std.lru.keys(lru)                  -> list
std.lru.values(lru)                -> list
std.lru.len(lru)                   -> int
std.lru.capacity(lru)              -> int
std.lru.clear(lru)                 -> lru
```

`std.lru` 是 C++ 值类型，具有值语义（每次更新返回新 lru）：

```text
import std;

let l = std.lru.new(2);
let r = std.lru.put(l, "a", 1);
l = r["lru"];

r = std.lru.put(l, "b", 2);
l = r["lru"];

r = std.lru.get(l, "a");       // a 变成最近使用
l = r["lru"];

r = std.lru.put(l, "c", 3);    // 淘汰最久未使用的 b
print(r["evicted"]);           // {key: b, value: 2}
```

规则：

- `capacity` 必须 >= 1；
- key 必须是合法 dict key（`int/bool/string/char/byte/bytes/null`）；
- 顺序从最久未使用到最近使用：`keys` / `values` 按此顺序返回；
- `get` 命中时把 key 移到最近使用端，`peek` 不改变顺序；
- 未命中时返回 `found: false`，`value` 是第三个参数或 `null`；
- `put` 返回被淘汰的 `{key, value}`，没有淘汰时 `evicted` 为 `null`；
- `delete` 返回 `{lru, removed}`；
- `clear` 保留 capacity；
- lru 目前不能 JSON 序列化。

性能提示：`put` / `get` 每次都会为值语义 clone 整个 lru，O(n)/次；适合小容量缓存。

### std.set / std.deque / std.heap

`std.set` / `std.deque` / `std.heap` 都是原生实现。`deque` 的 push/pop/size/peek 为均摊 O(1)，`to_list` 为 O(n)；`heap` 的 push/pop 为 O(log n)，`from_list` 为 O(n log n)，`to_list` 为 O(n)；`set` 的 add/remove/has 为 O(log n)，`from_list` O(n log n)，`values` O(n log n) 且保持插入顺序。

#### std.set

```text
import std.set;

let s = set.from_list([1, 2, 2, 3]);
set.size(s)              // 3
set.values(s)            // [1, 2, 3]
set.has(s, 2)            // true
set.add(s, 4)
set.remove(s, 2)
set.union(a, b)
set.intersect(a, b)
set.difference(a, b)
set.is_subset(a, b)
```

元素必须是合法 dict key：`int / bool / string / char / byte / bytes / null`。

`std.set` 是 C++ 原生持久化集合（哈希 treap + 插入日志），具有值语义：`add` / `remove` 返回新 set，不修改原 set。`add` / `remove` / `has` 为 O(log n)，`from_list` 为 O(n log n)，`values` 为 O(n log n) 并保持插入顺序（删除后重新添加会排到末尾）。也可以只 `import std;` 然后使用 `std.set.*`。`len(s)`、`for ... in s`、JSON 编码（数组）和类型标注 `set` 均支持；`set` 默认值为空集合。

#### std.deque

```text
import std.deque;

let d = deque.from_list([2, 3]);
d = deque.push_front(d, 1);
d = deque.push_back(d, 4);
deque.size(d)            // 4
deque.to_list(d)         // [1, 2, 3, 4]
deque.peek_front(d)      // 1
deque.peek_back(d)       // 4

let r = deque.pop_front(d);
r["ok"]                  // true
r["value"]               // 1
d = r["deque"];
```

`pop_front` / `pop_back` 返回 `{ok, deque, value}`；空队列返回 `{ok: false, error: "empty deque", deque}`。

`std.deque` 是 C++ 原生值类型，提供均摊 O(1) 的双端 push/pop 和 O(1) 的 `size` / `peek`，`from_list` / `to_list` 为 O(n)。与 `list` / `dict` / `std.lru` 一样具有值语义：`let e = deque.push_back(d, 1)` 不会修改 `d`。也可以只 `import std;` 然后使用 `std.deque.*`。`len(d)`、`for ... in d`、JSON 编码（数组）和类型标注 `deque` 均支持。

#### std.heap

```text
import std.heap;

let h = heap.new(func(a, b) { return a < b; });
h = heap.push(h, 3);
h = heap.push(h, 1);
heap.peek(h)             // 1
let r = heap.pop(h);
r["value"]               // 1
h = r["heap"];

let h2 = heap.from_list([3, 1, 2], func(a, b) { return a > b; });  // 大顶堆
```

`less(a, b)` 返回 `true` 表示 `a` 优先出堆。`pop` 返回 `{ok, heap, value}`；空堆返回 `{ok: false, error: "empty heap", heap}`。

`std.heap` 是 C++ 原生持久化左偏堆（leftist heap），具有值语义：push/pop 返回新堆，不修改原堆；push/pop 为 O(log n)，`from_list` 为 O(n log n)，`to_list` 为 O(n)（层序）。也可以只 `import std;` 然后使用 `std.heap.*`。`len(h)`、`for ... in h`（层序）、JSON 编码（数组）和类型标注 `heap` 均支持；`heap` 没有默认值，声明时必须提供初始值。

### std.route

```text
import std.route;

let app = route.new();
app.get("/users/:id", func(req) {
  return {"status": 200, "body": req["params"]["id"]};
});
app.get("/users/me", func(req) {
  return {"status": 200, "body": "me"};
});
app.get("/files/*path", func(req) {
  return {"status": 200, "body": req["params"]["path"]};
});
app.use(func(req) {
  if (req["path"] == "/private") {
    return {"status": 401, "body": "no"};
  }
  return null;
});

app.dispatch({"method": "GET", "path": "/users/42"});
```

API：

```text
route.new()                         -> Router
app.add(method, pattern, handler)
app.get/post/put/delete(pattern, handler)
app.use(middleware)
app.dispatch(request)               -> response | task
app.inspect()                       -> [{method, pattern, handler}, ...]
```

`Router` 是可变引用句柄：`let b = a;` 之后 `b` 和 `a` 共享同一个路由表；注册方法原地更新，不返回新 Router。

规则：

- `:name` 匹配单段并写入 `req["params"][name]`；
- `*name` 匹配剩余路径，必须是最后一段，并且可以匹配 0 段；
- 优先级逐段比较：静态段 > `:name` > `*name`；靠前的段优先；
- 同一个路径形状的 param / wildcard 名必须一致，否则注册时 `fail`；
- 同一个 `(method, pattern)` 重复注册会 `fail`；
- 方法不匹配但路径存在 → 405；完全不存在 → 404；
- middleware 签名是 `func(req) -> response|null`，返回 `null` 继续，否则短路；
- handler 返回 response dict；`req["params"]` 已填好；
- handler 可以是 `async func`；`dispatch` 会原样返回 task，可交给 `std.http.serve` 或外层 `await`；
- middleware 也可以是 `async func`。任一 middleware 返回 task 时，`dispatch` 返回 task；调用方 `await` 后得到 response；
- 静态路由使用精确表，dispatch 为 O(1)；动态路由使用段级 trie，dispatch 与路由总数基本无关；注册为 O(pattern 深度)；
- pattern 深度上限为 32 段；
- 注册应在开始服务前完成，运行期修改 Router 不是线程安全的。

`std.http.serve` 会等待 handler 返回的 task，因此可以直接注册：

```text
let s = std.http.listen("127.0.0.1", 8080);
std.http.serve(s, app.dispatch);
```

装饰器可用于包装 handler，路由本身不要求装饰器：

```text
@logged
func user(req) {
  return {"status": 200, "body": req["params"]["id"]};
}

app.get("/users/:id", user);
```

### std.graph

```text
import std.graph;

let g = graph.new();
let r = graph.add_node(g, "a");
g = r["graph"];
let a = r["id"];

r = graph.add_node(g, "b");
g = r["graph"];
let b = r["id"];

r = graph.add_edge(g, a, b, "next", 1);
g = r["graph"];

graph.bfs(g, a)               // [1, 2]
graph.dfs(g, a)               // [1, 2]
graph.neighbors(g, a)         // [2]
graph.topo_sort(g)            // {ok: true, order: [1, 2]}
graph.has_cycle(g)            // false
graph.path(g, a, b)           // {ok: true, path: [1, 2]}
graph.shortest_path(g, a, b)  // {ok: true, path: [1, 2], cost: 1}
graph.components(g)           // [[1, 2]]
graph.reverse(g)              // 反向图
graph.subgraph(g, [a])        // 只保留指定节点
graph.to_dot(g)               // Graphviz 文本
```

API：

```text
graph.new()                                 -> Graph
graph.add_node(g, value)                    -> {graph, id}
graph.add_nodes(g, values)                  -> {graph, ids}
graph.remove_node(g, id)                    -> Graph
graph.has_node(g, id)                       -> bool
graph.get_node(g, id)                       -> value
graph.set_node(g, id, value)                -> Graph
graph.node_count(g) / graph.edge_count(g)   -> int
graph.node_ids(g) / graph.nodes(g)          -> list
graph.add_edge(g, from, to, tag = "", weight = 1) -> {graph, id}
graph.add_edges(g, edges)                   -> Graph
graph.from_edges(count, edges)              -> Graph
graph.remove_edge(g, id)                    -> Graph
graph.edges(g)                              -> list
graph.neighbors(g, id, tag = null)          -> list
graph.bfs(g, start) / graph.dfs(g, start)   -> list
graph.topo_sort(g)                          -> {ok, order} | {ok: false, error}
graph.has_cycle(g)                          -> bool
graph.path(g, from, to)                     -> {ok, path} | {ok: false, error}
graph.shortest_path(g, from, to, weight_fn = null) -> {ok, path, cost} | {ok: false, error}
graph.components(g)                         -> list[list[id]]
graph.reverse(g)                            -> Graph
graph.subgraph(g, ids)                      -> Graph
graph.to_dot(g)                             -> string
```

规则：

- 节点 id 和边 id 都是递增整数；
- `add_node` / `add_edge` 返回新 graph，必须用 `r["graph"]` 取回；
- `add_nodes(g, values)` 返回 `{graph, ids}`；`add_edges(g, edges)` 的每条边是 `[from, to[, tag[, weight]]]`；`from_edges(count, edges)` 创建 1..count 节点；三者都只触发一次 COW；
- `add_edge` 的两个端点必须已存在，否则 `fail`；
- `remove_node` 会级联删除所有相关边；
- 边标签字段叫 `tag`（`label` 是保留字）；
- `topo_sort` 遇到环返回 `{ok: false, error: "cycle"}`；
- `bfs` / `dfs` 返回访问顺序，不存在的起点返回空列表；
- `path` 是 BFS 最短路径，不可达返回 `{ok: false, error: "not found"}`；
- `shortest_path` 使用 Dijkstra；默认用边的 `weight`（默认 1），也可传 `func(edge) -> number` 自定义权重；
- 负权重或非数字权重返回 `{ok: false, error: "negative edge weight"}` / `"edge weight must be a number"`；
- `add_edge` 的 `weight` 必须是非负数字；
- `edges` 返回的 dict 包含 `id / from / to / tag / weight`；
- `components` 返回弱连通分量，按节点插入顺序；
- `reverse` 保留节点和边 id，只反转方向；
- `subgraph` 保留给定节点，以及两端都在集合内的边；
- `neighbors` 可按 `tag` 过滤。

性能提示：`add_node` / `add_edge` / `set_node` 每次都会为值语义复制对应的 dict，分别是 O(V) / O(E)/次；大图请用 `add_nodes` / `add_edges` / `from_edges` 批量构造（只触发一次 COW）。`bfs` / `dfs` / `topo_sort` / `path` / `components` / `shortest_path` 会先构建一次邻接表，遍历为 O(V+E)；单次 `neighbors` 仍扫描全部边 O(E)。

### std.arena

```text
import std.arena;

let s = arena.new();
let r = arena.add(s, "a");
s = r["arena"];
let a = r["id"];

r = arena.add(s, "b");
s = r["arena"];
let b = r["id"];

arena.get(s, a)        // {ok: true, value: a}
arena.set(s, a, "A")   // {arena, ok: true}
arena.remove(s, b)     // {arena, removed: true}
arena.has(s, b)        // false
arena.size(s)          // 1
arena.values(s)        // [A]
```

API：

```text
arena.new()                 -> Store
arena.from_list(values)     -> Store
arena.add(store, value)     -> {arena, id}
arena.get(store, id)        -> {ok, value} | {ok: false, error}
arena.set(store, id, value) -> {arena, ok}
arena.remove(store, id)     -> {arena, removed}
arena.has(store, id)        -> bool
arena.size(store)           -> int
arena.capacity(store)       -> int
arena.ids(store)            -> list
arena.values(store)         -> list
arena.clear(store)          -> Store
```

性能提示：`add` / `set` / `remove` 每次都会为值语义复制 `slots` 列表，O(capacity)/次；`from_list` 批量构造 O(n)，`ids` / `values` 也是 O(n)。适合中小规模；超大规模请等待原生 arena 实现。

规则：

- 句柄是整数，包含槽位和 generation；
- 槽位复用后 generation 递增，旧句柄永久失效，不会误指向新值；
- `set` / `remove` 对失效句柄返回 `ok: false` / `removed: false`，不报错；
- `size` 是存活条目数，`capacity` 是历史槽位数；
- `ids` / `values` 按槽位顺序返回；
- `clear` 丢弃全部槽位；
- Store 是值语义，每次更新返回新 Store。

### std.path

```text
import std.path;

path.parse(path, style = "auto")             -> dict
path.join(parts..., options?)                -> string
path.join_list(parts, style = "auto")        -> string
path.normalize(path, style = "auto")         -> string
path.is_absolute(path, style = "auto")       -> bool
path.is_relative(path, style = "auto")       -> bool
path.absolute(path, style = "auto", cwd = null) -> string
path.resolve(base, path, style = "auto")     -> string
path.relative(from, to, style = "auto")      -> string
path.basename(path, style = "auto")          -> string
path.dirname(path, style = "auto")           -> string
path.extname(path, style = "auto")           -> string
path.stem(path, style = "auto")              -> string
path.with_ext(path, ext, style = "auto")     -> string
path.equals(a, b, style = "auto")            -> bool
path.to_posix(path, style = "auto")          -> string
path.to_windows(path, style = "auto")        -> string
path.sep(style = "auto")                     -> "/" | "\\"
path.delimiter(style = "auto")               -> ":" | ";"
path.detect(path)                            -> "posix" | "windows"
path.host()                                  -> "posix" | "windows"
```

`style` 取 `"auto"`、`"posix"` 或 `"windows"`。`std.path` 是纯词法路径库，
不访问文件系统；只有宿主风格下的 `absolute` 会读取 `std.fs.cwd()`。
Windows 模式支持盘符、UNC、`\\?\`、drive-relative、rooted 路径和混合分隔符，
输出统一使用 `\`。`parse` 返回 `style / input / root / drive / dir / base /
stem / ext / parts / absolute / rooted / normalized / sep`；`parts` 保留原始
`.` 和 `..`。

`join` 遇到带根的后续片段时重置累积结果，最后一个 dict 参数可传
`{"style": "windows"}`。`resolve` 要求 base 是绝对路径；`relative` 在根不同时
返回规范化后的目标绝对路径。`\\?\` 只作为词法前缀处理；`equals` 在 Windows
模式下只做 ASCII 大小写不敏感比较。真实文件 I/O 仍使用 `std.fs`，现有
`std.fs.path_*` 保持宿主原生行为。

### 6.5 查找路径

优先级：

```text
入口脚本所在目录
> 命令行 -I / --package-path
> 环境变量 SKEIN_PACKAGE_PATH
```

示例：

```bash
./skein -I ./lib --package-path ./vendor app/main.skein
SKEIN_PACKAGE_PATH=./lib:./vendor ./skein app/main.skein
```

POSIX 使用 `:`，Windows 使用 `;`。

## 7. 内置函数

### 数学

```text
sqrt exp log abs sin cos tan asin acos atan atan2
pow floor ceil round trunc log2 log10 hypot min max
sign clamp fmod mod is_nan is_inf is_finite cbrt
gcd lcm degrees radians
```

常量：

```text
std.math.pi     // 3.141592653589793
std.math.e      // 2.718281828459045
std.math.tau    // 6.283185307179586
```

规则：

- `sqrt`、`exp`、`log`、三角函数、`pow`、`log2`、`log10`、`hypot` 返回 `float`；
- `floor`、`ceil`、`round`、`trunc` 返回 `int`，超出整数范围会报错；
- `round` 按远离零的方向处理半值，例如 `round(-1.5) == -2`；
- `min` / `max` 至少需要一个数值参数；全为 `int` 时返回 `int`，否则返回 `float`；
- `sqrt(-1)`、`log(0)`、`log2(0)`、`log10(<=0)` 返回 `nan`；
- `sign(x)` 返回 `-1` / `0` / `1`；`sign(nan)` 返回 `nan`；
- `clamp(x, lo, hi)` 要求 `lo <= hi`，全为 `int` 时返回 `int`，否则返回 `float`；
- `fmod` 始终返回 `float`，`mod` 的结果符号跟随除数；
- `is_nan` / `is_inf` / `is_finite` 返回 `bool`；
- `gcd` / `lcm` 只接受整数并返回非负值，`lcm(0, x) == 0`；
- `degrees` / `radians` 返回 `float`；
- `atan2(y, x)` 返回 `atan(y / x)` 的角度；
- 所有数学函数的参数必须是数值，不做隐式转换。

### 输出和转换

```text
print println list dict int float bool str format len range
kind_of type_name type_module type_source
```

`print` 输出后不追加换行，`println` 输出后自动追加换行符；两者都返回 `null`。

```text
print("a");
print("b");     // 输出 ab
println("a");
println("b");   // 输出 a\nb\n
```

### 进程

进程命令通过 `std.process` 提供：

```text
std.process.command
std.process.command_status
std.process.run
std.process.spawn
std.process.wait
std.process.kill
std.process.close
std.process.pid

std.worker.pool
std.worker.call
std.worker.map
std.worker.submit
std.worker.wait
std.worker.shutdown
std.worker.stats
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
- `deque`：值语义，push/pop 返回新 deque，内部节点结构共享；
- `heap`：值语义，push/pop 返回新 heap，内部节点结构共享；
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

递归深度保护：

- `print` / `str` 超过 100 层后输出 `...`；
- `value_equals` 超过 512 层后返回 `false`；
- `freeval` 超过 1000 层后停止释放，避免 C++ 栈溢出（极深值可能泄漏）；
- JSON 编解码深度上限为 512。

### 8.1 对象生命周期与回收

- 普通值由引用计数管理；
- 闭包按引用捕获定义处的作用域，局部变量在闭包存活期间继续存在；
- 运行时有一个**作用域粒度的标记式环回收器**：从全局作用域、当前作用域链和恢复栈出发标记可达 scope，再清理不可达 scope 的符号表；
- 每 64 个顶层语句或 scope 数量明显增长时触发，只在顶层语句边界执行；
- 标记遍历覆盖 `list` / `dict` / `struct` / `enum` / `lru` / `result` / `method`；
- 它不是完整的逐值 tracing GC：值本身仍由引用计数管理，没有压缩、移动或 finalizer；
- 单个超长顶层语句内部产生的闭包环，可能要到该语句结束后才会被回收。

## 9. 错误和退出码

脚本默认在第一条错误后停止；REPL 继续交互。

```text
skein -k script.skein
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
多变量声明
for ... of
try/catch
通配导入
命名参数
静态方法 / 关联函数
类型声明装饰器
可变 self
私有成员
struct 继承
trait 继承 / 默认方法
用户自定义泛型（仅内建容器支持 `[...]` 类型参数）/ Option / 匿名联合类型
```

动态链接库加载目前是设计方向，尚未作为语言语法实现。
