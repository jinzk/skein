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

函数与闭包：

```text
def add(a, b) {
  return a + b;
}

def fib(n) {
  if (n < 2) { return n; }
  return fib(n - 1) + fib(n - 2);
}

def make_counter() {
  let n = 0;
  def inc() {
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

内置函数：`sqrt`、`exp`、`log`、`abs`、`print`、`list`、`dict`。

字符串支持常用转义：`\n`、`\t`、`\r`、`\\`、`\"`、`\'`。

## 语法约定

- 顶层可以直接写表达式，不需要分号。
- 块 `{ ... }` 内的普通表达式语句需要以 `;` 结尾。
- `{}` 表示空代码块；空字典请使用 `dict()`。
- 比较运算符包括 `>`、`<`、`<>`/`!=`、`==`、`>=`、`<=`。
- `//` 开始单行注释。
- 花括号、方括号、圆括号内的换行会被自动忽略，支持多行表达式。
- 闭包捕获定义处的词法作用域；闭包环境会在程序退出时统一清理。

## 目录结构

```text
tokens.l      Flex 词法分析
parser.y      Bison 语法分析
ast.*         AST 定义、函数代码共享与释放
eval.*        树遍历求值
scopestack.*  词法作用域链与闭包环境
symbol.*      符号表
valuetype.*   运行时值类型、深拷贝与释放
tests/        自动化回归测试
```
