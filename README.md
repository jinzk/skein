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
"hello" + " world"
[1, 2, 3][1]
{"a": 1, "b": 2}["b"]
```

变量：

```text
let x = 10;
x = x + 1;
x
```

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

函数：

```text
def add(a, b) {
  return a + b;
}

def fib(n) {
  if (n < 2) { return n; }
  return fib(n - 1) + fib(n - 2);
}

add(2, 3)
fib(10)
```

内置函数：`sqrt`、`exp`、`log`、`abs`、`print`。

字符串支持常用转义：`\n`、`\t`、`\r`、`\\`、`\"`、`\'`。

## 语法约定

- 顶层可以直接写表达式，不需要分号。
- 块 `{ ... }` 内的普通表达式语句需要以 `;` 结尾。
- `{}` 表示空代码块；字典字面量至少需要一个键值对。
- 列表和字典支持下标读取；暂不支持下标赋值。
- 比较运算符包括 `>`、`<`、`<>`/`!=`、`==`、`>=`、`<=`。
- `//` 开始单行注释。
- 花括号、方括号、圆括号内的换行会被自动忽略，支持多行表达式。

## 目录结构

```text
tokens.l      Flex 词法分析
parser.y      Bison 语法分析
ast.*         AST 定义与释放
eval.*        树遍历求值
scopestack.*  作用域栈
symbol.*      符号表
valuetype.*   运行时值类型、深拷贝与释放
tests/        自动化回归测试
```
