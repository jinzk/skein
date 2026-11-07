# Skein 函数与方法装饰器实施计划（方案 A：AST 脱糖）

- 更新日期：2026-11-07
- 状态：M1 方法装饰器、M2 命名函数装饰器、M3 async 方法、
  M4 async 方法装饰器、M5 generator 方法装饰器已实现（2026-11-07）
- 方案：A（AST lowering / 编译器生成包装方法）
- 关联文档：[README.md](../README.md)、[SYNTAX.md](../SYNTAX.md)、
  [architecture.md](architecture.md)、[metaprogramming-plan.md](metaprogramming-plan.md)、
  [bytecode.md](bytecode.md)

## 1. 结论与范围

### 1.1 结论

Skein 增加 `impl` 方法装饰器，采用**固定应用语法 + 运行时装饰器协议 + AST 脱糖**：

```skein
impl Point {
  @logged
  func area(self, scale: int = 1) {
    return self.x * self.x * scale;
  }
}
```

编译器把被装饰方法改写为：

1. 一个隐藏的普通函数值（原方法体）；
2. 一个隐藏的装饰后 callable；
3. 一个签名不变的转发方法。

方法表仍保存普通 `function_def`，方法分派、trait 检查、方法值提取、
`std.meta` 查询都不需要改运行时模型。

### 1.2 目标

1. 支持 `impl` 固有方法和 trait 实现方法上的 `@expr` 装饰器。
2. 支持命名自由函数（含 `async func`、generator 函数、`export func`）上的
   `@expr` 装饰器。
3. 调用点保持 `p.area()` / `x.draw()` 不变。
4. 装饰器在包加载（或包含声明的语句执行）时应用一次，不是每次调用。
5. 支持多个装饰器、带参装饰器、默认参数、可变参数。
6. `std.meta.methods` / `std.meta.traits` 看到的签名不变。
7. AST 与 VM 两个后端行为一致，VM-only 构建可用。
8. 不引入用户自定义宏，不做编译期任意代码执行。

### 1.3 非目标

- 不新增 `decorator` 关键字或专用“装饰器定义语法”。
- 不支持匿名函数/`let f = func(...)` 前的 `@` 语法（继续手写 `f = dec(f)`）。
- 不支持 `struct` / `enum` / `trait` 声明的装饰器。
- 不支持解构参数方法上的装饰器（v1 明确报错）。
- generator 方法和 async generator 方法装饰器由 M5 支持（见 §2.10、§12）。
- 不支持装饰器改变方法签名、返回值类型约束或方法名。
- 不支持 VM 级方法分派钩子或运行时可变方法表。
- 不实现 `@derive` 或任何编译期代码生成属性。

## 2. 语法与语义

### 2.1 应用语法

```text
decorator      := '@' decorator_expr
decorated_method := decorator+ method

decorator_expr := NAME
                | BUILTIN
                | decorator_expr '(' explist ')'
                | decorator_expr '[' exp ']'
                | decorator_expr '.' fieldname
                | '(' exp ')'
```

规则：

- `@expr` 放在 `impl` 方法声明前，习惯上每行一个；lexer 在 `impl` 花括号内
  不产生 `EOL`，因此语法本身不依赖换行；
- 只允许出现在 `impl` 的方法上；
- 装饰器表达式支持名称、成员访问、调用和下标；任意表达式用括号包起来，
  例如 `@(make_decorator(x))`；
- `@` 已经是 lexer 的字符 token，不需要新增词法规则；
- `import @pkg` 的路径语法不受影响。

示例：

```skein
impl Point {
  @logged
  @cached({"ttl": 100})
  func area(self, scale: int = 1) { ... }
}
```

应用顺序与 Python 一致：从下往上。

```skein
@a
@b
func m(self) { ... }
```

等价于：

```text
m = a(b(m))
```

`@a` 是外层，`@b` 先应用。

### 2.2 装饰器定义：不引入专用语法

**不新增** `decorator` 关键字或专门的函数声明形式。装饰器就是普通高阶函数。

理由：

- 编译器只关心 `@expr` 中的 `expr` 求值结果是否 callable，不关心它如何定义；
- 普通函数、匿名函数、导入函数、内置函数、`std.fn.compose` / `partial`
  都能直接使用；
- 专用语法会限制参数化装饰器、组合、导入和一等值使用；
- Python 也没有装饰器定义语法，保持一致可降低学习成本；
- Skein 是动态类型语言，定义处无法静态验证返回 callable，专用语法带来的
  静态检查收益有限。

固定的是**调用契约**，不是定义语法。

### 2.3 装饰器调用契约

简单装饰器：

```skein
func logged(fn: callable) {
  return func(...args) {
    print("[before]");
    let result = fn(...args);
    print("[after]");
    return result;
  };
}
```

带参数的装饰器工厂：

```skein
func retry(times: int) {
  return func(fn: callable) {
    return func(...args) {
      let result = null;
      for (let i in range(times)) {
        result = fn(...args);
      }
      return result;
    };
  };
}
```

契约：

1. 装饰器表达式必须求值为 callable。
2. 编译器/runtime 对原函数调用一次装饰器：`expr(fn)`。
3. 返回值必须是 callable；否则在 `impl` 安装时通过
   `let $dec: callable = ...` 报错。
4. 方法装饰器收到的是 **unbound function**，不是 bound method：
   - 定义方法时没有实例，不能传 bound method；
   - 生成的方法 wrapper 在调用时以 `$dec(self, 参数...)` 传入 receiver。
5. 装饰器返回的函数必须转发全部参数：
   - 推荐 `func(...args)`，`args[0]` 是 `self`；
   - 或显式写 `func(self, ...args)`。
6. 装饰器可以修改参数（包括 `args[0]`）后再调用原函数。
7. 装饰器可以返回不同值、抛错、短路，但不能改变方法的声明签名。

### 2.4 求值时机与顺序

- `@expr` 在 `impl` 语句执行时求值，和 Python 在 import/def 时执行装饰器类似。
- 多个装饰器的表达式按源码从上到下求值，应用按从下到上。
- 同一 `impl` 中，所有隐藏装饰绑定在该 `impl` 安装前求值。
- 因此，装饰器表达式不能引用同一 `impl` 中稍后才安装的方法。
- 装饰器失败时，`impl` 不会安装方法，避免部分安装。
- 装饰器在包加载时只执行一次，不随方法调用重复执行。

### 2.5 默认值、可变参数、解构

v1 语义：

- 默认值移动到生成的 wrapper 上；
- 隐藏的原方法体要求所有参数必填；
- 调用方省略参数时，wrapper 先求值默认值，再把实参传给装饰器；
- 装饰器看到的是**已补全默认值**的参数；
- 装饰器不能观察调用方到底省略了哪个参数；
- 默认值仍可引用 `self` 和更早的参数；
- 默认值仍在调用时求值，每次调用重新计算。

可变参数：

```skein
func sum(self, first: int, ...rest: int) { ... }
```

生成 wrapper 保留 `...rest`，转发为：

```text
$dec(self, first, ...rest)
```

解构参数：

```skein
func m(self, [a, b]) { ... }
```

v1 明确报错：

```text
decorated method parameters cannot use destructuring patterns
```

原因：wrapper 需要把解构后的绑定重新组装成原始参数，可能改变 list/struct
身份、`...rest` 内容或 enum 模式语义。

### 2.6 trait、枚举与方法值

- `impl Type` 固有方法和 `impl Trait for Type` 实现方法都支持装饰器。
- `impl Trait for Type;` 适配已有方法时，已有方法如果被装饰，适配仍然成立。
- wrapper 的签名必须与 trait 声明一致；trait 方法不支持默认值和可变参数。
- `p.m` 方法值提取返回绑定到 wrapper 的 `method` 值，调用仍经过装饰器。
- 枚举方法支持装饰器，规则同 struct 方法；枚举 `self` 仍保持只读语义。
- `std.meta.methods(type)` 返回 wrapper 的参数列表和 `has_defaults`，
  与原方法一致；装饰器本身不出现在元数据中。

### 2.7 命名函数装饰器

命名函数使用同一套 `@expr` 语法：

```skein
@logged
@cached({"ttl": 100})
func load(path) { ... }
```

lower 成：

```skein
let $d$func$load$10$1$1: callable = logged;
let $d$func$load$10$1$2: callable = cached({"ttl": 100});
func load(path) { ... }
let $dec$func$load$10$1: callable =
  $d$func$load$10$1$1($d$func$load$10$1$2(load));
load = $dec$func$load$10$1;
```

规则：

- 装饰器表达式在函数绑定前按源码顺序求值，避免同名函数遮蔽装饰器；
- 装饰器应用在函数创建后，从下往上；
- 函数名最终绑定到装饰后的 callable；
- 递归调用会走装饰后的函数；
- 支持 `export func`；隐藏绑定不导出，导出命名空间看到装饰后的值；
- 支持 `async func` 和 generator 函数；装饰器最终必须返回 callable，可以
  返回普通函数、async 函数或 generator 函数；
- 与 Python 的 `@dec def f` 语义一致：`f = dec(f)`；
- 匿名函数、`let f = func(...)`、`const f = func(...)` 仍手动包装。

### 2.8 async 方法

- `impl Type { async func m(self, ...) ... }` 和 enum 方法支持 `async`；
- `p.m()` 返回 task，`await p.m()` 等待；
- async trait 方法使用 `trait T { async func m(self); }`；
- trait 与实现的 async 标记必须一致；
- `impl T for Type;` 可适配已有 async 方法；
- 默认值在 task 开始前求值；
- `self` 是接收者副本，userdata 句柄共享；
- `std.meta.methods` 增加 `async` 字段；
- 取消外层 task 不自动取消被 await 的内层 task。

### 2.9 async 方法装饰器

- 去掉对 async 方法的拒绝；
- 隐藏 `$body` 保持 `is_async`；
- 生成 async wrapper：
  ```text
  async func m(self, ...) {
    return await $dec(self, ...);
  }
  ```
- 装饰器是普通函数，返回 async callable；
- `async func` 直接作为装饰器会返回 task，不是 callable。

### 2.10 generator 方法装饰器（M5）

- 去掉对 generator 方法的拒绝；
- generator 方法的 wrapper 是普通函数，不标记 `is_generator`，也不标记
  `is_async`（async generator 同理）；
- wrapper 直接返回装饰器链产生的 generator：
  ```text
  func m(self, ...) {
    return $dec(self, ...);
  }
  ```
- 是否惰性由装饰器决定，与自由函数 generator 装饰器一致；
- 迭代、发送值、`close` / `defer` 由装饰器返回的 generator 原样驱动；
- 不增加逐元素转发层；每个元素只有装饰器自身的逻辑开销；
- 静态类型从隐藏方法体恢复为 `generator[T, S]`；
- async generator 方法的 `std.meta.methods.async` 仍保持为 `true`。

## 3. Lowering 设计

### 3.1 源码到 AST 的形态

源码：

```skein
impl Point {
  @logged
  func area(self, scale: int = 1) {
    return self.x * self.x * scale;
  }
}
```

概念上 lower 成：

```skein
// 1. 原方法体变成普通函数值；默认值已移走
let $body$Point$area$12$3 = func(self, scale) {
  return self.x * self.x * scale;
};

// 2. 装饰器在定义时执行一次
let $dec$Point$area$12$3: callable = logged($body$Point$area$12$3);

impl Point {
  // 3. 签名不变的转发方法
  func area(self, scale: int = 1) {
    return $dec$Point$area$12$3(self, scale);
  }
}
```

调用链：

```text
p.area(2)
  -> Point.area wrapper
  -> $dec(p, 2)
  -> decorated closure
  -> $body(p, 2)
  -> 原方法体
```

### 3.2 插入位置

在 `parser.y` 的 `impl_decl` 动作中调用 lowering，返回 `impl` 本身或
`'L'` 序列：

```text
hidden_let_1
  -> hidden_let_2
    -> ...
      -> impl
```

使用现有 `'L'` 节点：

- AST 后端 `eval` 的 `case 'L'` 已支持顺序求值；
- VM 后端 `Compiler::compile_statement_list` 已支持；
- 不需要修改 `compiler.cpp` / `vm.cpp` 的语句分派；
- `impl` 写在函数或 block 内时也能 lowering，隐藏绑定位于同一作用域，
  wrapper 方法的闭包会捕获它。

### 3.3 隐藏绑定命名

名字必须无法从源码写出，且在同一作用域唯一：

```text
$body$<type>$<method>$<line>$<column>
$dec$<type>$<method>$<line>$<column>
```

规则：

- 不导出；
- 不被 `std.meta` / `std.module` 暴露；
- line/column 来自方法声明，避免同名方法跨 impl 块冲突；
- 用户源码无法声明 `$` 开头的标识符。

### 3.4 wrapper 生成

对每个被装饰方法：

1. 保留原 `funcdef` 作为 `$body` 的初值，`name` 保持 `NULL`；
2. 设置 `$body` 的 `is_method = 1`；
3. 创建 wrapper 的新 `symlist`：
   - 参数名、类型约束、`variadic` 从原参数复制；
   - 默认值 AST 从原参数移动到 wrapper；
   - 隐藏 `$body` 的参数不再有默认值；
4. 创建 `$dec` 的 `let`：
   - 初值为装饰器链 `a(b($body))`；
   - 类型标注 `callable`；
5. 创建 wrapper 的 `funcdef`：
   - 参数列表为第 3 步的新 `symlist`；
   - body 为 `return $dec(self, 参数...);`；
   - 可变参数使用 `newspread` 转发；
6. 用 wrapper 替换 `impl` 中的原方法。

### 3.5 `self` const 语义

原方法通过 `function_code::is_method = true` 让第一个参数 `self` 在绑定时
成为 const。隐藏的 `$body` 作为普通函数被调用时，也必须保持这个语义。

实现：

- `struct funcdef` 增加 `is_method` 字段；
- `newfunc` 初始化为 `0`；
- `get_function_code` 把 `node->is_method` 传给 `function_code::is_method`；
- lowering 把隐藏 `$body` 的 `is_method` 设为 `1`；
- wrapper 仍由现有 `evaluate_method` / `declare_impl_meta` 设置
  `function_code::is_method = true`。

这样：

- `self = ...` 仍然被禁止；
- `self.field = ...` 仍然允许；
- 枚举方法 `self` 只读语义不变；
- VM fast-call 对隐藏 `$body` 关闭，走通用参数绑定路径。

### 3.6 默认值 AST 所有权

默认值 AST 目前保存在 `ast.cpp` 的 `symlist_metadata_map` side table 中。
lowering 需要把默认值从隐藏函数移到 wrapper，避免深拷贝和双重释放。

新增：

```cpp
struct ast *take_symlist_default_ast(struct symlist *sl);
```

语义：

- 返回该参数的默认值 AST；
- 从 `symlist_metadata_map` 移除；
- 将 `sl->has_default` 置为 `false`；
- 不释放 AST。

然后对 wrapper 参数调用 `set_symlist_default_ast`。

所有权检查：

- `$body` 的 `funcdef` 被求值后 `moved = 1`，其 `function_code` 由运行时
  `release_function_code` 释放；
- wrapper 的 `funcdef` 同理；
- lowering 失败时返回原 impl，原方法参数和默认值保持不变；
- 所有失败路径由 ASan/LSan 覆盖。

### 3.7 装饰器链

装饰器列表按源码顺序保存。lowering 从后往前构建调用 AST：

```text
cur = $body
for decorator in reverse(decorators):
    cur = decorator(cur)
```

生成 `a(b($body))` 后，求值顺序仍为 `a` → `b`，应用顺序为 `b` → `a`。

带参装饰器：

```skein
@retry(3)
```

`retry(3)` 是普通表达式，lowering 生成：

```text
retry(3)($body)
```

### 3.8 错误与部分安装

- 预扫描所有带装饰器的方法，先检查 v1 限制；
- 任一检查失败：`yyerror`，返回原 impl，不做部分改写；
- 装饰器表达式求值失败：`'L'` 序列停止，impl 不安装方法；
- 装饰器返回非 callable：`$dec: callable` 声明失败，impl 不安装方法；
- 方法签名 / trait 检查仍由现有 `structeval.cpp` 逻辑执行；
- 错误行列优先指向 `@` 行或方法声明行。

## 4. 代码改动

### 4.1 `src/interpreter/ast.h` / `ast.cpp`

- `struct struct_member` 增加：
  ```cpp
  struct ast *decorators;
  ```
- `newstructfield` / `newstructmethod` 初始化 `decorators = NULL`。
- parser 使用 `with_decorators(...)` 把表达式链挂到方法节点。
- `treefree` 的 `'m'` 分支释放 `decorators`。
- `struct funcdef` 增加：
  ```cpp
  int is_method;
  ```
- `newfunc` 初始化 `is_method = 0`。
- `get_function_code` 把 `node->is_method` 传给 `function_code`。
- 新增 `take_symlist_default_ast`。
- 更新 `symlist_metadata_map` 的清理逻辑。

### 4.2 `src/interpreter/parser.y`

- 新增 `%type <a> decorators decorator decorator_expr`。
- 新增规则：
  ```text
  decorators:
      decorator
    | decorators decorator

  decorator:
      '@' decorator_expr
  ```
- `decorator_expr` 只覆盖名称、成员访问、调用、下标和括号表达式，
  避免与 `yield` 等前缀运算符产生 shift/reduce 冲突。
- `method_member` 增加三种带装饰器的形式：
  - 空 body；
  - `{ list }` body；
  - `=> exp;` body。
- 把命名函数声明集中到 `func_decl`，覆盖普通/async/export/export async；
- `stmt` 增加 `decorators func_decl`，在动作中调用
  `lower_function_decorators`；
- 在 `impl_decl` 动作中调用 `lower_impl_decorators`。
- `decorator` 规则带可选 `eols`：顶层有 EOL，`impl` 花括号内没有 EOL。
- `@` 已经是现有字符 token，无需修改 lexer。
- 确认 `import @pkg` 的语法不受影响。

### 4.3 新增 `src/interpreter/decorators.h` / `decorators.cpp`

接口：

```cpp
struct ast *lower_impl_decorators(struct impl_node *impl);
struct ast *lower_function_decorators(struct ast *decorators,
                                      struct funcdef *func);
```

职责：

- 预扫描 v1 限制；
- 生成隐藏 `$body` / `$dec` 绑定；
- 生成 wrapper 方法；
- 生成 `'L'` 序列；
- 维护 AST 所有权；
- 通过 `yyerror` 报告错误。

内部辅助（实际实现名）：

- `hidden_name(...)` / `function_hidden_name(...)`；
- `copy_symlist(...)`；
- `move_defaults(...)`；
- `build_decorator_chain(...)`；
- `build_wrapper_body(...)`；
- `validate_decorated_method(...)`。

### 4.4 `CMakeLists.txt`

在 `skein_interpreter` 源文件列表加入：

```text
src/interpreter/decorators.cpp
```

### 4.5 不需要改动的部分

- `type_def::methods` 仍是
  `std::unordered_map<std::string, std::shared_ptr<function_def>>`；
- `newmethodval` / 方法分派逻辑不变；
- `std.meta` 实现不变；
- `structeval.cpp` 的 trait 签名检查不变；
- `compiler.cpp` / `vm.cpp` 的语句和函数分派不变（只依赖 `'L'` 和
  `is_method` 字段）；
- 不新增运行时 `value_t` 类型；
- 不新增宏或编译期执行。

## 5. 已拒绝的替代方案

| 方案 | 结论 | 原因 |
|---|---|---|
| 运行时 Proxy 类型 | 仅作应用层模式 | 只能覆盖 trait 调用点，需要手写 Proxy |
| 在 `declare_impl_meta` 中合成 `function_def` | 不作为首选 | AST/VM 两条路都要改，调试和所有权更复杂 |
| 方法表存任意 `value_t*` callable | 拒绝 | 改变方法分派模型，性能与兼容风险高 |
| VM 方法分派钩子 | 拒绝 | 与当前静态方法设计冲突，影响所有方法调用 |
| 用户自定义宏 / `@derive` | 不在本计划 | 与 `metaprogramming-plan.md` 的阶段边界冲突 |

## 6. 任务拆分

规模：S（0.5–1 人日）、M（1–2 人日）。总估算约 **4–6 人日**。

### M1-1：AST 与 parser 基础设施（S/M，P0）

状态：**已完成（2026-11-06）**。

- `struct_member.decorators`、`funcdef.is_method`；
- `take_symlist_default_ast`；
- `treefree` / `newfunc` / `get_function_code` 更新；
- `parser.y` 的 `decorators` 规则和 `method_member` 变体；
- `impl_decl` 调用 lowering 入口。

验收：

- `@dec` 方法能解析并通过现有 parser 测试；
- `import @pkg` 不受影响；
- 无装饰器时 AST 与当前完全一致；
- `git diff` 限于 parser/AST/CMake。

### M1-2：lowering 核心（M，P0）

状态：**已完成（2026-11-06）**。

- 隐藏 `$body` / `$dec` 绑定；
- 装饰器链；
- wrapper 生成；
- 默认值迁移；
- 可变参数转发；
- `is_method` 语义；
- 隐藏命名和错误处理。

验收：

- 单装饰器、多装饰器、带参装饰器在 AST 后端可用；
- 默认值和 variadic 行为正确；
- `std.meta.methods` 签名不变；
- 无装饰器路径无行为变化。

### M1-3：VM 集成与所有权（M，P0）

状态：**已完成（2026-11-06）**。

- 确认 `'L'` 序列在 VM 下编译；
- 确认 `$body` / `$dec` 在 VM 下按定义包作用域解析；
- 确认 `is_method` 对 VM fast-call 和 `self` const 的影响；
- 修复默认值 AST 所有权；
- ASan/UBSan/LSan 通过。

验收：

- `SKEIN_VM=0` / `SKEIN_VM=1` 行为一致；
- VM-only 构建通过；
- 无泄漏、无重复释放；
- 嵌套 `impl`（函数/block 内）可调用。

### M1-4：测试与边界（M，P0）

状态：**已完成（2026-11-06）**。

- 新增 `tests/cases/decorators.py`；
- 注册到 `tests/test_skein.py`；
- 覆盖第 7 节全部功能与错误用例；
- 增加 VM/AST 差分测试；
- 增加跨包、导出类型、递归、方法值测试。

验收：

- `ctest` 全绿；
- `vm_differential_test` 无差异；
- ASan/UBSan/LSan 通过。

### M1-5：文档与全量验收（S，P0）

状态：**已完成（2026-11-06）**。

- `SYNTAX.md` 增加方法装饰器章节；
- `README.md` 增加特性说明和示例；
- `docs/architecture.md` 的 L2 文件列表加入 `decorators.*`；
- 更新 `docs/metaprogramming-plan.md` 的关联链接；
- 运行 `tools/check_docs_sync.py`；
- 运行 `tools/check_all.sh --clean`。

验收：

- 文档同步通过；
- Debug / Release / ASan+UBSan / LSan / VM-only 全部通过；
- 非装饰器方法性能无明显回退；
- 记录设计偏差和未完成项。

### M2-1：命名函数装饰器 lowering（S/M，P0）

状态：**已完成（2026-11-06）**。

- 把命名函数规则集中到 `func_decl`，覆盖普通/async/export/export async；
- `stmt` 支持 `decorators func_decl`；
- 新增 `lower_function_decorators`；
- 装饰器表达式在函数绑定前求值，应用在函数创建后；
- 最终函数名绑定装饰后的 callable；
- 递归、导出、async、generator 函数可用。

验收：

- `@dec func f` 等价于 `func f; f = dec(f);`
- 多个装饰器顺序正确；
- 同名局部函数不会遮蔽装饰器表达式；
- `export func` 在包外看到装饰后的值；
- async/generator 函数可被普通装饰器包装。

### M2-2：命名函数测试（S/M，P0）

状态：**已完成（2026-11-06）**。

- 新增自由函数装饰器用例；
- 覆盖多装饰器、带参装饰器、递归、同名遮蔽；
- 覆盖 `export func` 跨包；
- 覆盖 async 函数和 generator 函数；
- 覆盖非 callable 结果错误；
- VM/AST 差分。

### M2-3：文档更新（S，P0）

状态：**已完成（2026-11-06）**。

- `SYNTAX.md` 增加 5.11.1 命名函数装饰器；
- `README.md` 增加自由函数装饰器示例；
- 更新方法装饰器的限制说明；
- `docs_sync_test` 通过。

### M3：async 方法与 async trait 方法（M，P0）

状态：**已完成（2026-11-06）**。

- `method_decl` 支持 `ASYNC funcstart`；
- `trait_method` 支持 `async func`；
- `method_sig` / trait 元数据增加 `is_async`；
- `method_signature_matches` 比较 async 标记；
- `std.meta.methods` 增加 `async`；
- `await` 非 task 明确报错；
- 修正 VM 函数缓存在退出时的重入释放问题；
- 覆盖固有方法、enum、trait、适配、默认值、variadic、方法值、defer、递归。

### M4：async 方法装饰器（S/M，P0）

状态：**已完成（2026-11-06）**。

- 移除 async 方法装饰器拒绝；
- 隐藏 `$body` 保持 async；
- 生成 async wrapper 并 await `$dec`；
- trait async 签名匹配；
- 覆盖 before/after、默认值、variadic、错误、取消语义。

### M5：generator 方法装饰器（S/M，P0）

状态：**已完成（2026-11-07）**。

- 移除 generator 方法装饰器拒绝；
- wrapper 保持普通函数，async generator 不生成 `await`；
- hidden body 记录 `method_self_type`，恢复 `generator[T, S]` 静态类型；
- 保持 `std.meta.methods.async` 对 async generator 为 `true`；
- 修复类型检查提前物化 `function_code` 时的 AST 双重释放；
- 覆盖同步/async generator、send、close/defer、trait、递归、跨包和类型检查。

## 7. 测试计划

### 7.1 功能用例

| 场景 | 重点 |
|---|---|
| 命名函数 | `@dec func f` 等价于 `f = dec(f)` |
| 命名函数多装饰器 | 表达式顺序、应用顺序、递归 |
| 命名函数遮蔽 | 局部同名函数不遮蔽装饰器表达式 |
| 命名函数导出 | `export func` 跨包看到装饰后的值 |
| 命名函数 async/generator | 普通装饰器可返回 async/generator 函数 |
| async 方法 | 返回 task、await、默认值、variadic、defer、递归 |
| async trait 方法 | 声明/实现/适配/跨包、async 标记匹配 |
| async 方法装饰器 | async wrapper 包围真实 await、错误、取消 |
| 固有方法 | `p.m()` 经过装饰器，调用点不变 |
| trait 方法 | `impl Trait for Type { @dec func ... }`；`render(x: Trait)` 不变 |
| trait 适配 | `impl Type { @dec func ... }` + `impl Trait for Type;` |
| generator 方法 | 普通/生成器装饰器、`for`、方法值、send、close/defer |
| async generator 方法 | 内部 `await`、迭代、`std.meta.methods.async` |
| generator 方法类型 | `generator[T, S]` 推断、send 类型检查 |
| 多装饰器 | `a(b(fn))` 顺序，外层/内层输出顺序 |
| 带参装饰器 | `@retry(3)`、`@cache({"ttl": 100})` |
| 默认值 | 省略参数、引用 `self`、引用更早参数、每次调用重新求值 |
| 可变参数 | `func m(self, x, ...rest)` 转发 |
| 方法值 | `let f = p.m; f()` 仍经过装饰器 |
| 递归 | 方法内 `self.m()` 每层经过装饰器 |
| 参数修改 | 装饰器修改 `args[0]` / 其他参数后调用原方法 |
| 装饰器状态 | 装饰器闭包状态在多次调用间保留 |
| 嵌套 impl | `impl` 在函数/block 内，函数返回后方法仍可用 |
| 跨包 | 导出类型的方法在导入方调用，隐藏绑定按定义包解析 |
| `std.meta` | `methods` / `traits` 签名不变 |

### 7.2 错误用例

- `@dec` 后不是函数/方法声明；
- `@dec` 用在 `trait` / `struct` / `let` / `const`；
- 装饰器表达式未定义或求值失败；
- 装饰器返回非 callable；
- 解构参数方法；
- 方法没有 `self`；
- `self` 带默认值；
- 同一方法重复声明；
- 装饰器返回函数参数数量不匹配（调用时报错）；
- 跨包隐藏绑定不可见 / 解析错误。

### 7.3 后端与内存

- `skein_tests`（`SKEIN_VM=0`）与 `skein_vm_tests`（`SKEIN_VM=1`）同时通过；
- VM-only 构建通过；
- `vm_differential_test` 覆盖装饰器用例；
- ASan/UBSan：装饰器失败、默认值迁移、隐藏绑定释放路径；
- LSan：反复加载带装饰器的包不泄漏；
- `tools/check_all.sh --clean` 通过；
- `tools/check_docs_sync.py` 通过。

### 7.4 基准

- 非装饰器方法调用无回退；
- 装饰器方法记录额外一次函数转发开销；
- 记录 `p.m()`、方法值调用、trait 分派的基准对比；
- 不引入每次调用的 `std.meta` 查询。

## 8. 验收标准

- [x] `impl` 固有方法和 trait 实现方法支持 `@expr`。
- [x] 命名函数（含 async/generator/export）支持 `@expr`。
- [x] 命名函数装饰器表达式在函数绑定前求值，应用从下往上。
- [x] 命名函数递归、导出和同名遮蔽语义正确。
- [x] 多个装饰器按 Python 语义从下往上应用。
- [x] 带参装饰器、默认值、可变参数可用。
- [x] 装饰器在定义时执行一次，不随调用重复执行。
- [x] 方法调用点、方法值提取、trait 分派保持透明。
- [x] `std.meta.methods` / `std.meta.traits` 参数签名不变，新增 `async` 字段。
- [x] async 方法返回 task，`await`、默认值、variadic、defer、递归可用。
- [x] async trait 方法可声明、实现、适配，async 标记不匹配时报错。
- [x] async 方法装饰器生成 async wrapper 并包围真实 await。
- [x] `await` 非 task 报错。
- [x] VM 函数缓存退出释放问题已修复。
- [x] 隐藏 `$body` 保持 `self` const 语义。
- [x] 装饰器结果非 callable 时在安装时报错。
- [x] 解构参数方法在 v1 有明确错误；generator 方法装饰器已支持。
- [x] 嵌套 `impl` 和跨包方法可用。
- [x] AST / VM 行为一致，VM-only 通过。
- [x] ASan/UBSan/LSan 通过。
- [x] `tools/check_all.sh --clean` 通过。
- [x] 文档同步检查通过。
- [x] 没有新增宏、`decorator` 关键字或编译期任意代码执行。
- [x] 非装饰器路径性能无明显回退（完整 ctest / vm_bench_test 通过）。

## 9. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 默认值 AST 所有权错误 | 双重释放 / 泄漏 | `take_symlist_default_ast`；ASan/LSan；失败路径测试 |
| 隐藏绑定跨包解析错误 | 导入方调用方法失败 | 隐藏绑定放在定义包作用域；跨包测试 |
| `self` const 语义丢失 | 方法可重新绑定 `self` | `funcdef.is_method`；AST/VM 双端测试 |
| 解构参数转发改变语义 | 值身份 / rest 内容变化 | v1 直接拒绝；文档明确 |
| generator 方法语义复杂 | 装饰器无法正确驱动 yield | wrapper 用普通转发器，装饰器返回的 generator 原样驱动；无逐元素转发 |
| 装饰器表达式引用同 impl 方法 | 求值时机不确定 | 规定 impl 安装前求值；文档和测试 |
| 命名函数遮蔽装饰器表达式 | 装饰到错误对象 | 表达式在函数绑定前求值；测试同名局部函数 |
| 导出函数赋值时机 | 导入方看到旧值 | 在包内、导出命名空间构建前赋值；跨包测试 |
| 隐藏命名冲突 | `symbol repeated` | 非源码可写前缀 + 行列 + 方法名 |
| 错误位置指向 wrapper | 调试体验差 | wrapper 使用原方法行列；后续可标记透明帧 |
| parser `@` 与 import 冲突 | 语法回归 | 现有 import 测试；上下文限定 |
| 性能回退 | 方法调用变慢 | 只对装饰器方法增加一次转发；基准对比 |

## 10. 后续扩展

以下不在本计划范围，但设计上保留空间：

1. **匿名函数 / `let f = func` 装饰器**
   需要决定 `@` 与 `let`/`const` 的语法位置；当前继续手写 `f = dec(f)`。
2. **真正的 generator wrapper（可选增强）**
   当前 wrapper 是普通函数，惰性由装饰器决定；如果未来要求即使装饰器返回
   普通函数也保持完全惰性，就需要 wrapper 自身标记 `is_generator` 并手动
   转发 `next` / `send` / `close`。这会带来逐元素转发成本和更复杂的协议
   语义，暂不做。
3. **解构参数方法**
   需要无损参数重建规则，可能只支持部分模式。
4. **`--expand` 调试输出**
   展示 lowering 后的隐藏绑定和 wrapper；不影响运行时。
5. **装饰器元数据**
   如果未来需要 `std.meta` 暴露“方法被哪些装饰器修饰”，需要额外的只读
   元数据，不在 v1。

## 11. 实施顺序

1. M1-1（AST/parser 基础设施）
2. M1-2（lowering 核心）
3. M1-3（VM 集成与所有权）
4. M1-4（测试与边界）
5. M1-5（文档与全量验收）
6. M2-1（命名函数 lowering）
7. M2-2（命名函数测试）
8. M2-3（命名函数文档）
9. M3（async 方法与 async trait 方法）
10. M4（async 方法装饰器）
11. M5（generator 方法装饰器）

先完成单装饰器、无默认值、固有方法的垂直切片，再扩展多装饰器、默认值、
variadic、trait、跨包、命名函数和 async 场景。

## 12. 实施结果（2026-11-07）

已实现：

- `src/interpreter/decorators.h` / `decorators.cpp`；
- `struct_member.decorators`、`funcdef.is_method`；
- `take_symlist_default_ast`；
- parser 的 `@` 方法/命名函数装饰器语法和 `decorator_expr`；
- `impl_decl` 的 AST lowering；
- `func_decl` + `lower_function_decorators` 的命名函数 lowering；
- async 方法、async trait 方法、async 方法装饰器；
- generator 方法和 async generator 方法装饰器（M5）：
  - wrapper 是普通函数，直接返回装饰器产生的 generator；
  - hidden body 记录 `method_self_type`，用于恢复 `generator[T, S]` 静态类型；
  - async generator 的 `std.meta.methods.async` 保持为 `true`；
  - 修复类型检查提前物化 `function_code` 时的 AST 双重释放；
- `std.meta.methods` 的 `async` 字段；
- VM 函数缓存退出释放修复；
- `tests/cases/decorators.py`、`tests/cases/async_methods.py` 及注册；
- README / SYNTAX / architecture 文档更新。

验证结果：

```text
ctest (build):            11/11 passed
tools/check_all.sh:         all checks passed（LSan 单测曾因网络用例超时
                           失败，单独重跑通过）
ASan/UBSan VM + AST:      decorators / async methods passed
LSan:                     decorator / async success and failure paths passed
VM-only:                  decorators / async methods passed
vm_differential_test:     passed
docs_sync_test:           passed
boundary_check_test:      passed
```

已知限制与计划一致：支持 `impl` 方法、命名自由函数、async 方法、
async trait 方法、generator 方法、async generator 方法和 async 方法装饰器；
匿名函数 / `let f = func(...)` 仍需手动包装；装饰器表达式支持名称、成员访问、
调用、下标和括号表达式；不支持解构参数方法和类型声明装饰器。

generator 方法装饰器采用“普通转发器”语义：wrapper 不标记 `is_generator`，
也不为 async generator 生成 `await`；装饰器返回的 generator 直接驱动迭代、
发送值和关闭。装饰器是普通函数时惰性由该装饰器决定，与自由函数行为一致。
