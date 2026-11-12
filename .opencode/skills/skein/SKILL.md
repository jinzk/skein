---
name: Skein
description: Write and run Skein (.skein) programs. Use when creating, editing, reviewing, or debugging Skein scripts and examples, especially newline-sensitive control flow, for-in loops, match expressions, structs, enums, and error handling.
---

# Skein

Skein 是一个动态类型脚本语言，源文件使用 `.skein` 后缀。本 skill 只覆盖
**写 Skein 程序**；完整语法见仓库的 `SYNTAX.md`，标准库函数见
`docs/stdlib.md`。

## 先跑再交付

写完的代码要实际执行，不要只靠阅读：

```bash
skein path/to/file.skein          # 运行文件
printf '1 + 2\n' | skein -        # 运行 stdin 片段
skein -i                          # REPL
```

在本仓库内用 `./build/skein`。错误格式是
`line:col: syntax error: ...` 或 `line:col: runtime error: ...`，先读第一行。

最小程序：

```skein
import std;

func main() {
  println("hello");
}

main();
```

`main` 不会自动执行；顶层代码按顺序执行。标准库 `std.*` 需要
`import std;`，`print` / `println` / `str` / `len` / `range` / `int` /
`float` / `bool` 等是全局内置。

## 必须记住的规则

### 1. 换行和分号

括号外换行通常表示语句结束；`()`、`[]`、`{}` 内部换行会被忽略。

- 块内每条表达式语句都要以 `;` 结尾，包括块的最后一条；
- 顶层表达式语句可以省略 `;`；
- `let` / `const` / `return` / `break` / `continue` / `fail` 等语句要 `;`；
- `if` / `while` / `for` / `func` 的块结束处不需要 `;`。

### 2. `{`、`else`、`while` 必须和头部同一行

```text
if x {
  ...
} else {
  ...
}
```

- body 的 `{` 必须和 `if` / `while` / `for` / `switch` / `match` / `func` /
  `struct` / `enum` / `trait` / `impl` / `package` / `extern` 头部同一行；
- `else` 必须和前面的 `}` 同一行；
- `do { ... } while cond;` 的 `while` 必须和 `}` 同一行。

进入 `{}` 之后可以自由换行。

### 3. 条件括号可以省略，但 body 必须是块

```skein
let x = 3;

if x > 0 {
  println("positive");
} else {
  println("other");
}

while x > 0 {
  x = x - 1;
}
```

- 省略条件括号时，`if` / `while` / `for` / `switch` / `match` 的 body 必须是
  `{ ... }`，不能写单条语句；
- 带括号的形式仍支持单条语句：`if (x > 0) println("positive");`；
- 条件里的 dict、struct、函数、`match` 等复合字面量必须加括号：
  `if (d == {"a": 1}) { ... }`；
- 条件本身以 `(` 开头时按带括号形式解析。

### 4. for 和 for-in

```skein
for x in [1, 2, 3] {
  print(x);
}

for key, value in {"a": 1, "b": 2} {
  println(key + "=" + str(value));
}

for let i = 0; i < 3; i = i + 1 {
  println(str(i));
}
```

- `for x in xs` / `for k, v in d` 不带 `let`，也不带圆括号；
- 双变量 for-in 只支持 dict；list / string / bytes / set / deque / heap /
  generator 只支持单变量；
- 旧的 `for (let x in xs) { ... }` 仍然有效；
- C 风格 `for i = 0; ...` 是赋值，`i` 必须已经声明；要新建变量写
  `for let i = 0; ...`。

需要下标时用 `std.iter.for_each`，不要给 list 写双变量 for-in：

```skein
import std;

std.iter.for_each(["a", "b"], func(i, value) {
  println(str(i) + ":" + value);
});
```

回调 1 个参数时接收元素（dict 接收 key）；2 个参数时 list / string / bytes /
set / deque / heap / generator 接收 `(index, value)`，dict 接收
`(key, value)`。回调里的 `break` / `continue` 不可用，`return` 只退出回调。

### 5. `{}` 既是空字典也是空块

- 表达式位置：`let d = {};` 是空 dict；
- 语句体位置：`if x {}` 是空块；
- 需要明确的空块可以写 `{ ; }`；
- 非空 dict 字面量是 `{"a": 1}`，注意它和代码块长得一样，条件中要加括号。

### 6. match 是表达式

```skein
func describe(n) {
  return match n {
    case 0 => "zero";
    case 1 | 2 => "small";
    default => "other";
  };
}

println(describe(2));
```

- 表达式 arm 以 `;` 结尾，块 arm 不用；
- 没有 `default` 且没有覆盖所有情况时，运行时报
  `match: no arm matched`；
- 多值 subject 写 `match (a, b) { case [1, 2] => ...; }`。

### 7. struct 和 enum

```skein
struct Point {
  x: int;
  y: int;
}

impl Point {
  func length_sq(self) -> self.x * self.x + self.y * self.y;
}

let p = Point { x: 3, y: 4 };
println(str(p.length_sq()));
```

- 构造用 `Point { x: 1, y: 2 }`；字段有默认值时可写 `Config {}`；
- 方法写在 `impl Type` 中，第一个参数必须是显式 `self`，调用时写 `p.method()`；
- `struct` 只声明字段，方法不能直接写在 `struct` 里；
- enum 变体也是结构体风格：`Event.Msg { text: "hi" }`、`Event.Timeout`；
- struct 是 copy-on-write 值语义，方法内改 `self` 不会改调用方原值。

### 8. 字符串没有插值

```skein
let name = "skein";
println("hello, " + name);
println("n=" + str(42));
```

`+` 用于数值加法、字符串/char 拼接和 list 拼接。数字、bool、list 等和
string 混合拼接时先套 `str()`，否则报 `cannot add string and int`。

### 9. dict 缺 key 会直接报错

```skein
import std;

let d = {"a": 1};
println(str(std.dict.has(d, "a")));
println(str(std.dict.get(d, "b", 0)));
```

`d["missing"]` 报 `runtime error: key not found`。需要默认值时用
`std.dict.get(d, key, default)`，需要判断存在性用 `std.dict.has(d, key)`。

### 10. 错误处理

```skein
func parse_positive(text) {
  let n = int(text);
  if n <= 0 {
    fail "must be positive: " + str(n);
  }
  return n;
}

let n = parse_positive("0") on error(e) {
  println("bad input: " + str(e));
  recover 0;
};

println(str(n));
```

- `fail expr;` 抛出错误，`fail;` 抛出空错误；
- `expr on error(e) { ... }` 捕获错误；
- handler 必须以 `recover`、`pass` 或 `fail` 结尾；
- `recover expr;` 的值成为整个 `on error` 表达式的值。

## 常见报错速查

| 现象 | 原因 / 修复 |
|---|---|
| `syntax error: unexpected end of line`，指向 header 末尾 | `{` 写到了下一行；把它移到 header 同一行 |
| `syntax error: opening brace must be on the same line as its header` | 同上，块内的 `if` / `struct` / `enum` / `switch` / `for` 头部也会报这个 |
| `syntax error: else must be on the same line as the preceding token` | `else` 另起了一行；写成 `} else {` |
| `syntax error: while must be on the same line as the preceding '}'` | `do { }` 后的 `while` 换行了 |
| `syntax error: unexpected built-in function` | 省略条件括号后写了单语句 body，或 for-in 后直接写语句；改用 `{ ... }` |
| `runtime error: cannot add string and int` | 字符串拼接里混入了非字符串，套 `str()` |
| `runtime error: key not found` | dict 没有该 key；用 `std.dict.get` / `std.dict.has` |
| `runtime error: two-variable for-in requires dict, got list` | list 不能用 `for i, x in xs`；用 `std.iter.for_each` |
| `runtime error: match: no arm matched` | match 未穷尽且没有 `default` |
| `runtime error: cannot assign to constant` | 给 `const` 重新赋值 |
| `runtime error: symbol not found X` | 名字拼错，或忘了 `import std;` |

## 查标准库，不要猜

模块和函数很多，先查再写：

```bash
grep '^std\.dict\.' docs/stdlib.md
grep -n '^std\.' docs/stdlib.md | head -50
grep -n 'std\.iter' docs/stdlib.md SYNTAX.md
```

也可以看 `src/interpreter/std_namespace.cpp` 和
`src/interpreter/package.cpp` 的注册表。没有 `import std;` 时 `std.*` 不可用；
模块清单在 `docs/stdlib.md` 的「模块：」代码块中。

## 语法地图（SYNTAX.md）

| 内容 | 章节 |
|---|---|
| 注释、空白、换行 | §1.3–1.4 |
| 整数/浮点/字符串/字符/bytes/null | §2 |
| 真值规则 | §3 |
| 算术、比较、逻辑、三元、索引、切片、lambda | §4 |
| 变量与类型标注 | §5.2 |
| if / while / do while | §5.3–5.4 |
| for / for-in / `std.iter.for_each` | §5.5 |
| switch | §5.6 |
| match | §5.7 |
| 错误处理 | §5.10 |
| 函数、默认参数、可变参数 | §5.11 |
| struct / impl / trait | §5.12 |
| enum | §5.13 |
| package / import / std | §6 |

## 交付前检查

- [ ] 用 `skein -` 或实际文件跑过，退出码为 0；
- [ ] 每个 header 的 `{` 在同一行，`else` 和 `do ... while` 的 `while` 也在同一行；
- [ ] 省略条件括号时，body 用了 `{ ... }`；
- [ ] for-in 没写 `let`，双变量 for-in 只用于 dict；
- [ ] 字符串拼接里的非字符串都套了 `str()`；
- [ ] dict 可疑 key 用了 `std.dict.get` / `std.dict.has`；
- [ ] 用到的 `std.*` 函数在 `docs/stdlib.md` 中存在，且已 `import std;`。
