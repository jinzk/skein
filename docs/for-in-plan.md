# Skein 无 let 的 for-in 语法实施计划

- 更新日期：2026-11-08
- 状态：已实现（2026-11-08）
- 目标版本：下一轮语法增强
- 关联文档：[README.md](../README.md)、[SYNTAX.md](../SYNTAX.md)、
  [architecture.md](architecture.md)、[bytecode.md](bytecode.md)

## 1. 结论与范围

### 1.1 结论

Skein 增加省略 `let` 的无括号 `for ... in` 语法：

```skein
for x in xs { ... }
for k, v in d { ... }
```

循环变量默认是**块级新变量**，等价于现有的：

```skein
for (let x in xs) { ... }
for (let k, v in d) { ... }
```

实现方式是把新语法直接 lowering 为现有的 `forinloop` AST，因此 AST 解释器、
字节码 VM 和类型检查器都不需要修改。

### 1.2 目标

1. 支持 `for x in xs { ... }` 单变量形式。
2. 支持 `for k, v in d { ... }` 双变量形式。
3. 循环变量是块级新绑定，遮蔽外层同名变量；循环结束后外层变量不变。
4. 迭代对象在进入循环作用域之前求值一次。
5. 保留 `break` / `continue` / `return` / label 的现有行为。
6. 保留类型推断：单变量得到元素/key 类型，双变量得到 key/value 类型。
7. 保留现有 `for (let ... in ...)` 和 C 风格 `for (;;)`，不破坏兼容性。
8. AST 后端、VM 后端、VM-only 构建行为一致。

### 1.3 非目标

- 不实现普通语句中的 `x := 1`。
- 不实现 Go 风格的 `for x := range xs`。
- 不实现 `for x in 5` 直接遍历整数区间。
- 不实现 `_` 丢弃语义；`for _ in xs` 仍会绑定一个名为 `_` 的普通变量。
- 不支持单语句循环体；新语法必须使用 `{ ... }`。
- 不改变 `range()` 内置函数的行为。
- 不改变双变量只支持 dict 的现有规则。

## 2. 语法与语义

### 2.1 新增语法

```text
for NAME in exp scope_block
for NAME ',' NAME in exp scope_block
```

其中 `scope_block` 必须以 `{` 开始。

示例：

```skein
for x in [1, 2, 3] {
  print(x);
}

for key, value in {"a": 1, "b": 2} {
  print(key + "=" + str(value));
}

for i in range(3) {
  print(i);
}

label outer: for x in xs {
  for y in ys {
    continue outer;
  }
}
```

### 2.2 Lowering

```text
for x in xs { body }
    =>  for (let x in xs) { body }

for k, v in d { body }
    =>  for (let k, v in d) { body }
```

因此文法动作直接调用现有的 `newforin()`：

```c
newforin(name, value_name, iterable, body)
```

### 2.3 作用域与求值顺序

- 迭代对象 `exp` 在进入循环作用域之前求值，因此 `for x in x { ... }` 中的
  第二个 `x` 指向外层变量。
- 循环变量在循环体的子作用域中定义，和 `let` 声明一致。
- 循环变量遮蔽外层同名变量；循环结束不修改外层变量。
- 循环变量只存在于循环作用域内。

### 2.4 限制

1. **循环体必须是块**：`for x in xs { ... }`。不支持
   `for x in xs print(x);`。
2. **以 dict 字面量开头的操作数可以省略括号**：

   ```skein
   for x in {1: 2} { ... }
   ```

   但操作数本身以 struct / 函数 / `match` 等复合字面量结尾时仍需括号：

   ```skein
   for x in (Point { x: 1 }) { ... }
   for x in (func() { return [1]; }()) { ... }
   ```

   后者与 Go 对 range 后 composite literal 的限制一致。
3. **顶层换行后接 `{` 仍不支持**，与现有 `if (...) \n {` 的行为一致；
   函数体内部没有 `EOL` token，所以可以换行。
4. **双变量只支持 dict**，沿用现有运行时错误。

### 2.5 保留语法

以下形式保持不变：

```text
for (let x in xs) { ... }
for (let k, v in d) { ... }
for (let i = 0; i < n; i = i + 1) { ... }
for (;;) { ... }
```

## 3. 词法与文法设计

### 3.1 问题

当前 lexer 的 `brace_starts_block()`（`src/interpreter/tokens.l`）只在 `)`、
`else`、`do`、`=>`、`package NAME` 后把 `{` 识别为代码块。对于：

```text
for x in xs { ... }
```

`xs` 后面的 `{` 会被当成字典/结构体字面量的开始。

### 3.2 方案：for 头部状态机 + 专用体 token

lexer 维护一个很小的状态机：

| 当前状态 | 输入 | 下一状态 | 动作 |
|---|---|---|---|
| `FOR_NONE` | `for` | `FOR_SEEN` | — |
| `FOR_SEEN` | `NAME` | `FOR_ONE` | — |
| `FOR_ONE` | `,` | `FOR_COMMA` | — |
| `FOR_COMMA` | `NAME` | `FOR_TWO` | — |
| `FOR_ONE` / `FOR_TWO` | `in` | `FOR_NONE` | 记录 `pending_body = true`，`body_indent = indent_count` |
| 任意非 `FOR_NONE` 状态 | `(` | `FOR_NONE` | 重置为 C 风格 for |
| 任意 | 普通 `{` / `;` / `}` | `FOR_NONE` | 重置，处理 `impl A for B { ... }` |

当 `pending_body` 为真，且遇到 `{` 时：

- 若 `indent_count == body_indent`，返回新 token `FOR_BODY_LBRACE`；
- 否则（位于 `(` / `[` 内）按普通 `{` 处理；
- 转换后清除 `pending_body`。

`FOR_BODY_LBRACE` 入栈时必须使用 block frame：

```c
brace_stack.push_back(brace_frame{false, true});  // is_dict = false
indent_count++;
```

这样空循环体 `{}` 的 `}` 会变成 `EMPTY_BLOCK_CLOSE`，非空体则保持普通 `'}'`。

### 3.3 为什么需要专用 token

仅靠文法无法消除歧义：`exp` 本身可以是 `NAME '{' ... '}'` 结构体字面量，
解析到 `for x in xs` 时，`xs` 后的 `{` 会默认 shift 进结构体字面量分支。
因此把循环体 `{` 标成独立 terminal `FOR_BODY_LBRACE`，从词法层消除冲突。

### 3.4 parser 规则

新增 token：

```text
%token FOR_BODY_LBRACE "for body"
```

新增规则（复用现有 `IN`、`exp`、`newscope`、`newforin`）：

```text
| FOR NAME IN exp FOR_BODY_LBRACE EMPTY_BLOCK_CLOSE
    { $$ = newforin($2, NULL, $4, newscope(NULL)); }
| FOR NAME IN exp FOR_BODY_LBRACE list '}'
    { $$ = newforin($2, NULL, $4, newscope($6)); }

| FOR NAME ',' NAME IN exp FOR_BODY_LBRACE EMPTY_BLOCK_CLOSE
    { $$ = newforin($2, $4, $6, newscope(NULL)); }
| FOR NAME ',' NAME IN exp FOR_BODY_LBRACE list '}'
    { $$ = newforin($2, $4, $6, newscope($8)); }
```

现有规则 `FOR '(' forinit ';' ...`、`FOR '(' LET NAME IN ...` 不变。
`FOR NAME` 是全新的前缀，预计不新增 shift/reduce 冲突；构建后确认
`%expect 2` 保持不变。

## 4. 后端复用

新语法不新增 AST 节点，直接复用 `forinloop`：

| 组件 | 位置 | 是否需要改 |
|---|---|---|
| AST 构造 | `src/interpreter/ast.cpp` 的 `newforin()` | 否 |
| AST 解释器 | `src/interpreter/eval.cpp` 的 `case 'F'` | 否 |
| 字节码编译 | `src/interpreter/compiler.cpp` 的 `compile_forin()` | 否 |
| 类型检查 | `src/interpreter/typecheck.cpp` 的 `case 'F'` | 否 |
| VM 迭代 | `src/interpreter/vm.cpp` 的 `IterNew` / `IterNext` | 否 |

现有 `forinloop` 已经完成：

- 迭代对象求值后再 `PushScope` / `eval_pushscope()`；
- 在子作用域中定义 key/value 变量；
- 统一处理 list / dict / string / bytes / set / deque / heap / generator；
- 双变量仅 dict 的运行时校验；
- `break` / `continue` / label / `return` 控制流。

## 5. 改动文件清单

### 5.1 `src/interpreter/tokens.l`

- 新增 `FOR_BODY_LBRACE` 的返回路径。
- 新增 for 头部状态机及其重置逻辑。
- 保留 `"range"` 的 `BUILTIN(B_range)` 行为不变。
- 保留普通 `{` / `}` / 空块判定逻辑不变。

### 5.2 `src/interpreter/parser.y`

- 声明 `%token FOR_BODY_LBRACE "'{'"`。
- 增加 2 条单变量规则和 2 条双变量规则。
- 不变更现有 for 规则。

### 5.3 `SYNTAX.md`

- §5.5 增加 `for x in xs` / `for k, v in d` 示例。
- 说明循环变量是块级新变量、遮蔽外层变量。
- 说明必须带块体、复合字面量需括号的限制。
- 明确双变量仍只支持 dict。

### 5.4 `README.md`

- 更新 §for 示例（当前约 3163 行起），补充新写法。
- 不删除现有 `for (let ... in ...)` 示例。

### 5.5 测试

- `tests/cases/language_core.py`：新增语法、作用域、控制流和错误用例。
- `tests/vm_differential.py`：修正第 75 行非法的
  `for (v in [...])`，改为 `for (let v in [...])`。
- 如需要，增加 typecheck 用例验证循环变量推断类型。

## 6. 测试计划

### 6.1 正向用例

1. `for x in [1, 2, 3]` 输出元素。
2. `for k in {"a": 1}` 输出 key。
3. `for k, v in {"a": 1}` 输出 key/value。
4. `for i in range(3)` 输出 `0, 1, 2`。
5. 字符串、bytes、set、deque、heap、generator 的单变量遍历。
6. 嵌套循环 + `break` / `continue`。
7. `label outer: for x in xs` + `continue outer` / `break outer`。
8. 循环内 `return`。
9. 遮蔽：外层 `x` 在循环后保持原值。
10. `for x in x`：迭代对象使用外层 `x`。
11. dict 字面量操作数：`for x in {1: 2}` 可直接书写；struct 字面量仍需括号：
    `for x in (Point { x: 1 })`。
12. 空体：`for x in xs {}`。
13. 循环变量类型推断与 `for (let ... in ...)` 一致。

### 6.2 负向用例

1. `for k, v in [1, 2]`：双变量只支持 dict 的现有错误。
2. `for x in {1: 2}`：操作数缺括号，报语法错误。
3. `for x in xs print(x);`：缺块体，报语法错误。
4. `for x, y, z in d`：报语法错误。
5. `for x in xs` 后无循环体：报语法错误。

### 6.3 命令

```bash
cmake --build build
ctest --test-dir build -R skein_tests --output-on-failure
```

补充验证：

```bash
# AST 后端
SKEIN_VM=0 ctest --test-dir build -R skein_tests --output-on-failure

# VM-only 构建
cmake --build build-vm-only
```

手动 smoke test：

```bash
printf 'for x in [1,2,3] { print(x); }\n' | ./build/skein -
printf 'for k, v in {"a":1} { print(k, v); }\n' | ./build/skein -
SKEIN_VM=0 ./build/skein -
```

## 7. 验收标准

1. `for x in xs { ... }` 和 `for k, v in d { ... }` 在 AST 与 VM 后端输出一致。
2. 现有 `for (let ... in ...)`、C 风格 `for` 和 `range()` 无回归。
3. 构建无新增 Bison 冲突，`%expect 2` 不变。
4. 类型检查对循环变量的推断与 `let` 形式一致。
5. 循环变量不泄漏、不修改外层同名变量。
6. 新增正向、负向测试全部通过。
7. VM-only 构建可用。

## 8. 风险与后续工作

### 8.1 风险

- **lexer 状态泄漏**：错误恢复或 `-k` keep-going 时，for 头部状态可能残留。
  通过在普通 `{`、`;`、`}`、`(` 处重置状态降低影响。
- **块字面量操作数**：函数字面量、`match`、dict/struct 字面量作操作数时
  必须加括号，需要文档明确说明。
- **顶层换行**：沿用现有控制流语法的限制，不额外放宽。

### 8.2 后续可选工作

- 支持带括号的 `for (x in xs)`（不需要 lexer 状态机，可作为兼容形式）。
- 支持 `_` 丢弃语义。
- 支持 `for i in 5` 整数区间（需要扩展迭代协议与三个后端）。
- 评估是否将 `for (let ... in ...)` 标记为旧写法，但不强制移除。
