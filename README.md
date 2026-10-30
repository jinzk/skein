# calc-demo

一个使用 Flex + Bison + C++17 实现的动态类型脚本语言解释器（REPL）。

## 构建与运行

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/calc-demo
```

运行测试：

```bash
ctest --test-dir build --output-on-failure
```

Windows / MinGW（64 位 MinGW-w64 GCC）：

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
build\calc-demo.exe
```

Windows 构建需要 MSYS2 中的 MinGW-w64 GCC、CMake、Bison 和 Flex。平台相关代码集中在 `platform_win.cpp`：

- 设置控制台输入输出代码页为 UTF-8；
- 管道输入使用二进制模式，避免 CRLF / `0x1A` 转换；
- 使用 `SetConsoleCtrlHandler` 实现 Ctrl-C。

当前只面向 MinGW-w64/GCC，不包含 MSVC 专用适配。

脚本文件建议保存为 UTF-8；输入开头的 UTF-8 BOM 会被忽略。如果控制台无法切换到 UTF-8，中文下标、格式化和输出可能无法正常工作，需要在输入输出边界额外做代码页转换。

## 脚本模式与 shebang

可以直接把脚本文件作为第一个参数运行：

```bash
./build/calc-demo script.calc
```

也可以使用 shebang：

```text
#!/usr/bin/env calc-demo
print("hello");
```

```bash
chmod +x script.calc
./script.calc
```

使用 `/usr/bin/env calc-demo` 时，请确保 `calc-demo` 在 `PATH` 中；也可以直接写解释器的绝对路径。

脚本模式会：

- 跳过第一行的 `#!`；
- 跳过 UTF-8 BOM；
- 不打印 `> ` 提示符；
- 不打印顶层表达式的 `= result` 结果，需要输出时请使用 `print`；
- 忽略第一行之外的普通注释仍使用 `//`；
- 使用 `calc-demo -` 可以从 stdin 运行脚本模式；
- 脚本中出现词法、语法或运行时错误时，退出码为 `1`。

Windows/MinGW 没有原生 shebang，直接使用：

```bat
build\calc-demo.exe script.calc
```

shebang 行同样会被跳过。

交互模式：

- `Ctrl-C` 取消当前输入（包括未闭合的多行块），或中断正在执行的程序，回到 `> ` 提示符。
- `Ctrl-D` 退出解释器。
- 退格键在真实终端和管道输入下均可使用。

## 语言概览

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

变量：

```text
let x = 10;
x = x + 1;
x
```

变量名可以包含下划线：`my_value`。

列表和字典：

```text
let a = [1, 2, 3];
a[0] = 9;
a

let d = {"a": 1};
d["b"] = 2;
d

let empty_list = list();
let empty_dict = dict();
```

支持链式下标赋值：`a[0][1] = 9`、`d["a"]["b"] = 1`。

字典支持点号访问，等价于字符串下标：

```text
let p = {"x": 1, "y": 2};
p.x          // 等价于 p["x"]
p.y = 3;     // 等价于 p["y"] = 3
p.x + p.y
```

点号访问要求字段名是普通标识符；如果键是保留字，请使用 `d["if"]` 这类下标写法。嵌套访问和赋值同样可用：`p.a.b = 1`。

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

for (let i = 0; i < 3; i = i + 1) {
  print(i);
}
```

`break` / `continue` 可用于 `while` 和 `for`。

函数、匿名函数与闭包：

```text
func add(a, b) {
  return a + b;
}

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

函数是一等值，可以保存到变量、列表或字典中，并通过任意表达式调用：

```text
let fs = [adder(1), adder(2)];
fs[0](10)
d["handler"](42)
adder(3)(4)
```

匿名函数使用同一个 `func` 关键字，省略函数名：

```text
let double = func(x) { return x * 2; };
double(21)

(func(x) { return x + 1; })(41)
```

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
- 仍然使用显式 `return`；
- 支持空参数和空函数体；
- 立即调用时建议给匿名函数加括号，例如 `(func(x) { ... })(1)`。

没有显式 `return` 的函数，以及 `print`，返回 `null`。`null` 是假值，可用于条件判断，也可以作为参数传递。

内置函数：`sqrt`、`exp`、`log`、`abs`、`print`、`list`、`dict`、`int`、`float`、`str`、`format`、`command`、`command_status`、`read_file`、`write_file`、`append_file`、`file_exists`、`file_open`、`file_read`、`file_write`、`file_seek`、`file_tell`、`file_eof`、`file_close`、`file_flush`、`input`、`split`、`split_ws`、`trim`、`find`、`substr`、`bytes_from_string`、`bytes_to_string`、`bytes_len`、`bytes_get`、`bytes_slice`、`bytes_concat`、`bytes_hex`、`bytes_from_hex`、`bytes_to_int`、`int_to_bytes`、`read_bytes`、`write_bytes`、`append_bytes`、`file_read_bytes`、`file_write_bytes`。内置函数同样是一等值：

```text
let f = sqrt;
f(9)

func apply(fn, x) { return fn(x); }
apply(sqrt, 16)
```

函数调用会先按从左到右的顺序求值参数，再检查被调用者类型和参数个数，因此参数中的副作用总会执行。

字符串支持常用转义：`\n`、`\t`、`\r`、`\\`、`\"`、`\'`。

## 语法约定

- 顶层可以直接写表达式，不需要分号。
- 块 `{ ... }` 内的普通表达式语句需要以 `;` 结尾。
- `{}` 表示空代码块；空字典请使用 `dict()`。
- 比较运算符包括 `>`、`<`、`<>`/`!=`、`==`、`>=`、`<=`。
- `//` 开始单行注释。
- 花括号、方括号、圆括号内的换行会被自动忽略，支持多行表达式。
- 闭包捕获定义处的词法作用域；闭包环境会在程序退出时统一清理。
- 用户函数调用深度上限为 1000，超出时报告 `maximum recursion depth`。

## 类型与转换

- `int` 是 64 位有符号整数；超出 64 位的整数字面量按 `float` 解析，整数运算溢出会报 `integer overflow`。
- `int` 与 `float` 混合运算提升为 `float`，例如 `1 + 2.5` 得到 `3.5`；`1 == 1.0` 为真。超过 2^53 的整数与 float 比较仍按数学值精确判断，但转成 `float`（混合运算或 `float()`）时会丢失精度。
- 整数除法向零截断，取模结果符号随被除数：`-7 / 4 == -1`，`-7 % 4 == -3`，`7 % -4 == 3`。
- 位运算按 64 位进行，移位次数必须在 `0..63` 之间。
- 真值规则：`null`、`0`、`0.0`、空字符串、空列表、空字典为假；其余值（包括函数和内置函数）为真。
- `null` 是空值字面量，表示“没有值”：没有 `return` 的用户函数、`print` 和 `return;` 都返回 `null`。顶层 `null` 结果不会显示，但可以赋值、比较、传递，或通过 `print(null)` / `format("{}", null)` 输出。
- `&&`、`||`、`!` 返回整数 `0` / `1`，并短路求值。
- `==` / `!=` 对所有类型有效；大小比较只支持数值之间或字符串之间。
- 列表和字符串下标必须是 `int`，`1.0` 不会被隐式截断；字符串下标按 UTF-8 码点计数，`"你好"[0]` 得到 `你`。
- 字典键按值相等判断，`1` 和 `1.0` 是同一个键；字面量中的重复键以最后一个为准。
- `list` 和 `dict` 是值类型：赋值、传参和返回在语义上复制，但内部使用写时复制（COW），只读或复制时共享底层数据，真正修改时才分离。
- `bytes` 和字符串目前仍然复制内容；函数值共享闭包环境，文件句柄等原生资源共享底层句柄。
- 显式转换：`int(x)`、`float(x)`、`str(x)`。`int` / `float` 接受整数、浮点数或字符串，`str` 接受任意值。
- 浮点数输出使用可往返的最短表示并保留 `.0`：`1.0` 显示为 `1.0`，`1.23456` 显示为 `1.23456`。
- 没有字符串与数字之间的隐式转换，`1 + "2"` 会报错；需要时请使用 `int()` / `float()` / `str()`。

## 对象生命周期

- 普通值由引用计数管理；`list` / `dict` 的 payload 会被别名共享，写入时才分离；
- 函数值复制时共享函数代码和闭包环境，文件句柄等原生资源共享底层句柄；
- 闭包按引用捕获定义处的作用域，因此局部变量会在闭包存活期间继续存在；
- 运行时会在顶层语句边界执行作用域级循环回收：每 64 个顶层语句或 scope 数量明显增长时，清理从活动作用域不可达的闭包环；
- 回收只在顶层语句边界进行。单个超长顶层语句内部创建的循环引用，可能要到该语句结束后才会被回收；
- 当前没有完整 GC；如果未来原生资源也能保存语言值，需要扩展循环回收的遍历规则。

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

`command(cmd[, env])` 执行一个 shell 命令，并返回其标准输出字符串：

```text
command("echo hi")                    → "hi"
command("echo $FOO", {"FOO": "bar"})  → "bar"
print(command("ls"))
```

- POSIX 使用 `/bin/sh -c`；
- Windows/MinGW 使用 `cmd.exe /C`；
- 可选的 `env` 是 dict，键和值必须是字符串；
- `env` 在父进程环境基础上覆盖，只对当前命令生效；
- 标准错误继承到当前进程，不会被捕获；
- 返回值末尾的 `\r` / `\n` 会被去掉；
- 命令无法启动时报告 `command failed to start`；
- 非零退出码不会自动报错，当前返回值只有标准输出，没有退出码。

如果只需要退出码，使用 `command_status(cmd[, env])`：

```text
command_status("exit 0")                     → 0
command_status("exit 3")                     → 3
command_status("exit $CODE", {"CODE": "3"})  → 3
```

`command_status` 会直接继承标准输入/输出/错误，因此命令输出会正常显示；它返回退出码整数。命令处理器无法启动时报告 `command_status failed to start`。

环境变量名不能为空，也不能包含 `=`。在 Windows cmd 中引用变量使用 `%FOO%`，POSIX shell 使用 `$FOO`。

Windows/MinGW 下 `command` 使用 `CreateProcessW` 传递 Unicode 命令行和 Unicode 环境块；`command` 捕获的 stdout 约定为 UTF-8，`command_status` 直接继承控制台输出，不做额外编码转换。

示例：

```text
let files = command("ls");
print(files);

let name = command("echo world");
format("hello {}", name)
```

`command` 是普通内置函数，因此也可以保存和传递：

```text
let run = command;
run("echo ok")
```

**安全提示**：`command()` 会执行任意 shell 命令，只应对可信输入使用。

Windows 下命令输出需要是 UTF-8 字节流，否则中文内容可能与解释器内部编码不一致。

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

### 用 dict + 闭包封装文件对象

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
f.read(10)
f.close()
```

底层文件资源使用通用的原生 userdata 生命周期管理；上层对象接口可以用语言本身扩展，不需要修改内置实现。

文件函数可以访问进程有权限的任意路径，只应在可信脚本中使用。

## 错误信息

错误输出统一包含位置、类别和源码片段：

```text
3:1: runtime error: division by zero
  1/0;
  ^
```

- 行号和列号从 `1` 开始；
- 类别分为 `lexical error`、`syntax error`、`runtime error`；
- 运行时错误指向实际出错的 AST 节点，而不是语句结束位置；
- 源码行下的 `^` 指向出错列；
- 词法错误会显示字符和十六进制字节，例如 `unexpected character '@' (0x40)`；
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

## 目录结构

```text
tokens.l      Flex 词法分析
parser.y      Bison 语法分析
ast.*         AST 定义、函数代码共享与释放
diagnostics.* 错误位置、源码片段与调用栈
eval.*        树遍历求值
filetype.*    原生文件资源类型
format.*      Python 风格格式化
scopestack.*  词法作用域链与闭包环境
symbol.*      符号表
valuetype.*   运行时值类型、深拷贝与释放
platform.*    POSIX / Windows 平台适配
tests/        自动化回归测试
```
