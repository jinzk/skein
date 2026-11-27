# 用户自定义泛型：设计与开发计划

状态：**已完成（当前范围）**。

协议类型声明关键字为 `proto`，属于全局保留字（`proto Keyed[K any]`）；
`protocol` 不再是关键字。泛型约束列表里的 `proto` 与 `type` 一样是 contextual
标记，只在 `[V proto Drawable]` 这类位置生效。反射 kind 字符串为 `"proto"`。

本文记录用户自定义泛型的最终语法、语义决议、实现阶段和测试要求。当前已实现：

- 泛型 `struct` / `enum` / `proto` / 自由函数声明；
- `Box[int]`、`Option[int]` 类型注解；
- `Box[int] { ... }`、`Option[int].Some { ... }`、`Option[int].None`；
- 泛型函数调用推断、`f[int](...)` 和 `let g = f[int]`；
- `|` 类型集合、`&` proto 组和约束检查；
- `type` / `proto` 约束标记；
- 泛型 proto 的结构式匹配、类型参数推断、默认方法和 `[T Keyed[int]]` 约束；
- 默认类型参数的声明、尾部/约束校验和推断缺失时的补全；
- 裸 `Box` 类型注解按默认类型实参补全；
- `std.meta.generic_info` / `type_args` / `type_params`；
- `std.struct.decode` / `std.enum.decode` 接受泛型类型描述符；
- `std.struct.encode` / `std.enum.encode` 和 JSON envelope；
- 泛型 struct 递归和嵌套泛型字段；
- VM / JIT 类型擦除路径。

当前范围内的泛型能力已全部实现。通用二进制值格式不在当前范围：如未来出现明确的吞吐、
体积或跨语言协议需求，再基于同一类型描述符和 envelope 模型单独设计。

`SYNTAX.md` 和 `docs/tutorial.md` 已同步用户自定义泛型能力。

## 1. 目标

- 支持泛型 `struct` / `enum` / `proto` / 自由函数；
- 类型参数只参与静态检查，运行时擦除；
- 支持 proto 约束（含 `Hashable` / `Iterable`）和有限类型集合约束；
- 保持现有非泛型 struct 的值语义、COW、递归和序列化边界；
- 不引入泛型特化、泛型方法、运行时泛型标签或隐式泛型转换。

## 2. 非目标

明确不做：

- `impl Box[int]` / `impl Box[any]` 特化；
- 泛型方法（`func map[U](self, ...)`）；
- proto 继承、关联类型、泛型 proto 特化；
- proto 并集；
- `&` 和 `|` 混合约束；
- 运行时 `is Box[int]`；
- 从任意运行时值反推类型实参；
- 泛型实例之间的隐式转换或自动协变；
- 默认类型参数补全部分显式类型实参。

## 3. 最终语法

### 3.1 类型参数声明

类型参数统一写在声明名字后的方括号中：

```skein
struct Box[T any] {
  value: T;
}

enum Option[T any] {
  None;
  Some { value: T; }
}

proto Keyed[K any] {
  func key(self): K;
}

func identity[T any](x: T): T {
  return x;
}

impl Box {
  func get(self): T {
    return self.value;
  }
}
```

约束使用 `name constraint`：

```skein
import {Hashable} from std.traits;

[T any]
[T Hashable]
[T Drawable]
[T Drawable & std.dict_key.DictKey]
[T int | string | Box[int]]
```

推荐在需要明确约束类别时使用每个参数自己的 `type` / `proto` 标记：

```skein
[K type int | string]
[V proto Drawable]
[T Hashable & Drawable]
[K type int | string, V proto Drawable]
```

- `type` 表示后面的约束是具体类型集合或单一具体类型；
- `proto` 表示后面的约束是 proto / 内置约束组；
- 这里的 `type` / `proto` 都是 contextual 标记，只在类型参数约束位置识别；
  作为协议声明关键字的 `proto` 则是全局保留字；
- 不同参数可以使用不同标记；
- 不写标记的旧形式继续兼容。

不使用：

```skein
[T]
[T type]
[T: any]
```

### 3.2 类型实参和默认值

```skein
struct Box[T = any] {
  value: T;
}
```

规则：

- 默认类型参数只能出现在尾部；
- 默认类型必须满足声明的约束；
- `Box` 等价于 `Box[any]`；
- `Box[int]` 显式覆盖默认值；
- `Box {}` 在无法推断时使用默认 `any`，但字段合法性仍按普通 struct 规则检查；
- `Box { value: 1 }` 推断为 `Box[int]`。

### 3.3 约束

约束只能是以下两种形态之一，不能混用：

```text
ProtoGroup := Proto ('&' Proto)*
TypeUnion     := ConcreteType ('|' ConcreteType)*
```

`type` 标记用于 `TypeUnion`，`proto` 标记用于 `ProtoGroup`；
不写标记时由编译器根据名字和已有声明推断。`Hashable` 可出现在 `&` 组中。

`ProtoGroup`：

```skein
[T Drawable]
[T Drawable & std.dict_key.DictKey]
```

- `&` 两侧必须是 proto；
- `Hashable` 是 `std.traits` 提供的 proto，可参与 `&`；
- 不允许具体类型出现在 `&` 中。

`TypeUnion`：

```skein
[T int | string]
[T int | Box[string] | MyEnum]
```

- `|` 两侧必须是具体类型或已实例化的泛型类型；
- proto 不能作为 `|` 成员；
- `any` 不能作为 `|` 成员；
- `Hashable` 不能作为 `|` 成员。

禁止：

```skein
[T Drawable | Keyed[int]]
[T A & B | int]
[T int & string]
[T Hashable | int]
```

### 3.4 类型实参 union

`Box[int | string]` 合法，定义为：

```skein
Box[union[int, string]]
```

它是一个 Box，字段 `value` 的静态类型是 `int | string`，不是两个 Box 实例化类型。

如果类型参数声明为：

```skein
struct Box[T int | string] {
  value: T;
}
```

则允许：

```skein
Box[int]
Box[string]
Box[int|string]
```

不允许：

```skein
Box[bool]
Box[int|bool]
```

规则：union 类型实参满足约束，当且仅当 union 的所有成员都满足约束。

### 3.5 `any` 和 proto

`any` 是动态顶层值类型：

```skein
let x: any = user;   // user 可以是实现 proto 的 struct/enum/function
```

但 proto 不是类型实参：

```skein
Box[any]      // 合法
Box[Keyed]    // 非法
```

需要约束时使用类型参数：

```skein
struct Holder[T Keyed] {
  value: T;
}
```

`T any` 可以绑定到 struct / enum / function 等具体类型，但不自动拥有
`callable` 或 proto 方法；需要时使用约束。

### 3.6 显式类型实例化

自由泛型函数支持：

```skein
identity(1)              // 全部隐式推断
identity[int](1)         // 全部显式
let g = identity[int];   // 显式实例化的函数值
```

规则：

- 只有 `f` 是已知泛型函数时，`f[...]` 才按类型实参解释；
- 非泛型值仍按索引解释；
- 显式类型实参必须列出全部类型参数，不能部分显式；
- 默认类型参数不能补全部分显式列表；
- 不支持对方法写类型实参；
- 不支持 `f[int][0]` 这类实例化后继续索引的写法；
- 需要函数值时可以直接写 `let g = f[int]`，不需要 lambda 包装；
- 显式类型实参在 parser 阶段解析并保存在 AST 上，typechecker 不再回读源码文本；
  宏展开会复制这些实参（roadmap G1）。G2 的受限 `type` 片段只能把调用点已有
  类型名放进类型位置，不能生成 `Box[#T]` 这类泛型实参。

## 4. 语义决议

### 4.1 运行时擦除

- 类型参数不进入运行时值表示；
- `Box[int]` / `Box[string]` / `Box[any]` 运行时共享同一 struct 形状；
- 泛型函数只有一份实现，不做特化；
- 不允许 `is Box[int]`，只允许 `is Box`。

### 4.2 类型身份和转换

- 泛型实例 invariant；
- `Box[int]`、`Box[string]`、`Box[int|string]`、`Box[any]` 是不同类型；
- 不自动允许：

  ```skein
  Box[int] -> Box[int|string]
  Box[int] -> Box[any]
  ```

- 转换由用户显式构造：

  ```skein
  let a: Box[int] = Box[int] { value: 1 };
  let u: Box[int|string] = Box[int|string] { value: a.value };
  let d: Box[any] = Box[any] { value: a.value };
  ```

- 值层面的 `any` 赋值仍按现有动态规则处理，不等价于 `Box[int] -> Box[any]`。

### 4.3 `|` 类型集合的能力

类型检查器只检查实例化成员关系：

```skein
[T int | string]
```

不自动计算“所有成员的共同能力”。泛型体里对 `T` 的操作由写泛型代码的作者保证；
如果作者需要静态保证，应使用 proto 约束：

```skein
[T Drawable]
```

### 4.4 约束包含检查

- `A & B` 可以传给要求 `A` 的约束；
- `A` 不能传给要求 `A & B` 的约束；
- `{int}` 可以传给 `int | string`；
- `int | string` 不能传给 `int`；
- `any` 不能自动满足更窄约束；
- 无法证明时直接报类型错误，不做运行时降级。

### 4.5 `Hashable`

`Hashable` 是 `std.traits` 提供的 marker proto：

```skein
import {Hashable} from std.traits;
```

语义复用内置 key 契约：

满足：

- `int`、非 NaN `float`、`bool`、`string`、`char`、`byte`、`bytes`、`null`；
- frozen tuple/pair，且元素递归满足；
- struct/enum，字段或载荷递归满足，或实现合法 `dict_hash`。

不满足：

- `list` / `dict` / `set` / `deque` / `heap`；
- `ref` / `weak`；
- function / builtin / callable；
- task / generator；
- error / result / userdata；
- `any`、未约束的类型参数。

`Hashable` 没有泛型参数；`Hashable[T]` 不是有效写法。

### 4.5.1 `Iterable`

`Iterable[T]` 是 `std.traits` 提供的结构式迭代器 proto：

```skein
import {Iterable} from std.traits;

proto Iterable[T any] {
  func iter(self): generator[T];
}
```

- 内置容器通过内置 conformance 表满足它；
- 用户类型需要 `iter(self): generator[T]`；
- 返回 `list[...]` 等值的类型不满足 `Iterable`；
- `std.meta.implements` / `std.meta.protocols` 能看到内置 conformance；
- `for in` 接受内置容器和满足 `Iterable` 的用户类型；具体用户类型在编译期检查，`any` 在运行时检查。

`Hashable` / `Iterable` 可以出现在 `&` 组中，不能作为 `|` 成员：

```skein
[T Hashable & Drawable]
[T Iterable & Drawable]
[T Hashable | int]   // error
```

### 4.6 泛型 proto

支持：

```skein
proto Keyed[K any] {
  func key(self): K;
}
```

- 结构式方法签名匹配；
- 类型参数替换；
- 现有默认方法机制可用，但不能引入方法级类型参数；
- 不支持 proto 继承和关联类型；
- `x is Keyed` 只检查结构；
- `x is Keyed[int]` 禁止；
- 约束位置支持显式 proto 类型实参，例如 `[T Keyed[int]]`；
- 未写类型实参时从实现类型的方法签名推断 proto 类型参数，并要求多个方法一致。

### 4.7 递归

泛型 struct 沿用现有非泛型规则：

```skein
struct Node[T any] {
  value: T;
  next: Node[T]?;
}
```

- 可选自引用字段按现有 COW 规则处理；
- 普通值互递归仍不支持；
- 需要环时使用 `ref` / `weak`；
- 泛型参数不改变递归规则。

### 4.8 `std.meta`

编译器保存泛型声明信息，运行时值不携带类型参数。

API：

```skein
std.meta.generic_info("Box[int]")
std.meta.type_args("Box[int]")
std.meta.type_params("Box[int]")
```

行为：

- 泛型信息通过显式类型描述符查询；
- 内建容器同样支持：`type_params("list")`、`type_args("list[int]")`、
  `generic_info("generator[int]")`；
- `type_info` 对描述符只返回外层类型，实参查询使用 `type_args` / `generic_info`；
- `Box[int]` 返回 `args: ["int"]`；
- `Box[any]` 返回 `args: ["any"]`；
- `Box` 有默认值时返回默认实参并标记 `defaulted: true`；
- 直接传运行时值时 `type_args` 返回 `null`，因为类型实参已擦除；
- `std.meta.type_info(value)` 仍只返回外层 `Box`；
- 不提供从任意运行时值反推类型参数的 API。

### 4.9 序列化

- 只支持已实例化的泛型类型；
- 普通 payload 不保存类型实参；
- `std.struct.decode` / `std.enum.decode` 接受泛型类型描述符；
- 反序列化按类型实参递归校验字段和容器元素；
- 自描述数据使用 JSON envelope：

  ```json
  {"$version": 1, "$type": "Box[int]", "payload": {"value": 1}}
  ```

- `std.struct.encode` / `std.enum.encode` 生成 envelope；
- `std.struct.decode` / `std.enum.decode` 无显式类型时读取 `$type`；
- 泛型声明元数据随线程 payload 类型描述一起传输；
- 跨线程/进程默认依赖静态 schema，动态消息使用 envelope；
- 当前正式支持 JSON envelope；通用二进制值格式不在当前范围，按需再设计。

## 5. 开发阶段

### Phase 0：规范与测试骨架

工作项：

- 新增 `docs/generics.md`（本文）；
- 在 `docs/roadmap.md` 增加泛型主线状态和链接；
- 准备 parser/typechecker 正反例测试清单；
- 明确 `SYNTAX.md` / `docs/tutorial.md` 暂不宣称已实现。

验收：

- 设计文档与最终决议一致；
- 测试用例列表覆盖语法、约束、实例化、meta、序列化边界。

### Phase 1：词法、语法和 AST

预计涉及：

- `src/interpreter/tokens.l`
- `src/interpreter/parser.y`
- `src/interpreter/ast.h`
- `src/interpreter/ast.cpp`

工作项：

- 声明上的类型参数列表：`struct` / `enum` / `proto` / `func`；
- `impl Box` 绑定声明中的全部类型参数；
- 类型实参 `Box[int]`、`Box[int|string]`；
- 约束解析：`&` proto 组、`|` 类型集合，禁止混用；
- 显式类型实参 `f[int]` / `f[int](...)`；
- `let g = f[int]` 的 AST 形态；
- `is Box` 允许，`is Box[int]` 在类型检查阶段拒绝。

验收：

- 正例可解析；
- 混用 `&` / `|`、proto `|`、部分显式类型实参产生明确语法/类型错误；
- 非泛型值的 `x[...]` 仍按索引解析。

### Phase 2：静态类型模型

预计涉及：

- `src/interpreter/static_type.h`
- `src/interpreter/static_type.cpp`
- `src/interpreter/typecheck_types.h`
- `src/interpreter/typecheck_types.cpp`

工作项：

- 类型参数绑定和替换；
- 泛型实例 `Box[int]` 的类型身份和 invariant 比较；
- union 类型实参；
- 默认类型参数实例化；
- 约束包含检查；
- `Hashable` 与 `key_contract()` 集成；
- 类型参数作用域、重复参数和可见性诊断。

验收：

- 类型相等/赋值/函数签名检查使用实例化后的类型；
- `Box[int]` 与 `Box[int|string]` 不被错误视为相同；
- 约束包含方向正确；
- `any` 不能自动满足更窄约束。

### Phase 3：类型检查器

预计涉及：

- `src/interpreter/typecheck.cpp`
- `src/interpreter/typecheck_builtins.cpp`
- `src/interpreter/structtype.cpp`
- `src/interpreter/typeinfo.cpp`

工作项：

- 泛型 struct/enum 字段、构造和模式匹配；
- 泛型函数声明、调用推断和显式实例化；
- 泛型 proto 的结构式匹配；
- `impl Box` 方法签名使用 struct 的类型参数；
- `|` 实例化成员检查，不做共同能力强制；
- `&` proto 组和内置 `Hashable` 检查；
- 泛型实例不可隐式转换；
- `is Box` / `is Keyed` 的静态限制；
- 诊断信息和错误位置。

验收：

- 所有正反例覆盖；
- 泛型函数体不会因方法级类型参数而接受非法代码；
- `Box[Keyed]` 报错；
- `Box[int]` 不能传给 `Box[any]`；
- `Box[int|string]` 的构造和字段访问正确。

### Phase 4：编译器、VM 和 JIT

预计涉及：

- `src/interpreter/compiler.cpp`
- `src/interpreter/bytecode.h`
- `src/interpreter/vm.cpp`
- `src/interpreter/vm_bridge.cpp`
- `src/jit/jit.cpp`
- `src/jit/jit_helpers.cpp`
- `src/runtime/structtype.*`
- `src/runtime/valuetype.*`

工作项：

- 类型参数擦除；
- 泛型 struct/enum 运行时使用普通类型定义和具体字段值；
- 泛型函数单份实现；
- 显式实例化函数值生成静态绑定 thunk/closure；
- 不生成特化方法表；
- 保持 VM/JIT 行为一致；
- 线程传输和 GC 遍历不依赖类型实参。

验收：

- `Box[int]` / `Box[string]` 运行结果正确；
- 泛型函数 VM/JIT 结果一致；
- 没有额外运行时类型参数存储；
- `is Box[int]` 不可用。

### Phase 5：`std.meta`

预计涉及：

- `src/builtins/`
- `src/runtime/typeinfo.cpp`
- `lib/std/meta` 或现有 `std.meta` 注册位置

工作项：

- 编译器记录实例化类型描述；
- `std.meta.generic_info(value)` 静态入口；
- `std.meta.type_args(value)` 便捷入口；
- 返回结构与现有 `type_info` 对齐；
- `any` / 未具体化 `T` / 裸 `Box` 的行为；
- 不提供运行时反推。

验收：

- `Box[int]`、`Box[any]`、`Box[int|string]` 的 meta 结果正确；
- `any` 值返回 `null` 或明确错误；
- `type_info` 仍只返回外层类型；
- 跨线程/worker 下静态元数据行为一致。

### Phase 6：序列化

预计涉及：

- `src/runtime/serialization.cpp`
- `src/runtime/json.cpp`
- `std.struct` / `std.enum` decode 路径
- 线程/进程消息序列化

工作项：

- 只允许已实例化泛型类型；
- 反序列化目标类型或 type descriptor；
- 普通 payload 不保存类型实参；
- 可选自描述 envelope；
- JSON envelope 的版本字段和类型描述；
- 通用二进制值格式不在当前范围，作为未来可选优化；
- 版本兼容和错误诊断。

验收：

- `Box[int]` / `Box[string]` 可重复序列化；
- 裸 `Box` 反序列化失败；
- `Box[int]` 不能误读为 `Box[string]`；
- 跨线程/进程测试通过。

### Phase 7：文档、测试和发布门槛

工作项：

- 更新 `SYNTAX.md`、`docs/tutorial.md`、`docs/stdlib.md`、`docs/key-semantics.md`；
- 从“当前不支持”列表移除用户自定义泛型；
- 增加泛型专项测试；
- 运行 release / JIT 全量测试；
- 检查 docs sync 和 diff check；
- 评估是否需要更新 `docs/roadmap.md` 状态。

验收：

- 全量测试通过；
- 文档示例可执行；
- 不破坏现有内建容器泛型行为；
- 用户自定义泛型状态从“待实现”更新为“已完成”。

## 6. 测试矩阵

### 语法

- 泛型 struct / enum / proto / function 声明；
- 默认类型参数；
- 类型实参 union；
- `&` proto 组；
- `|` 类型集合；
- 禁止 `&` / `|` 混用；
- 禁止 proto `|`；
- `f[int](...)` 和 `let g = f[int]`；
- 部分显式类型实参报错；
- 泛型方法声明报错；
- `impl Box[int]` 报错。

### 类型检查

- 默认 `Box` → `Box[any]`；
- `Box {}` → `Box[any]`；
- `Box { value: 1 }` → `Box[int]`；
- `Box[int]` / `Box[string]` / `Box[int|string]` 区分；
- `Box[Keyed]` 报错；
- `Box[bool]` 在 `T int|string` 下报错；
- `Box[int|string]` 在 `T int|string` 下允许；
- `Box[int]` 不能赋给 `Box[any]`；
- `Box[int]` 不能赋给 `Box[int|string]`；
- proto 约束包含检查；
- `any` 不能传给更窄约束；
- `Hashable` 正反例；
- `is Box` / `is Keyed`；
- `is Box[int]` / `is Keyed[int]` 报错。

### 运行时

- 泛型 struct 字段和 COW；
- 泛型 enum variant；
- 泛型函数调用；
- `f[int]` 函数值；
- VM / JIT 差分；
- 跨线程/进程传输；
- 泛型递归 struct；
- `ref` / `weak` 字段限制不变。

### Meta / 序列化

- `generic_info` 返回 args；
- `type_args` 返回列表；
- `any` / 未具体化 `T` 的行为；
- 序列化目标类型；
- 自描述 envelope；
- 裸 `Box` 反序列化失败；
- 版本和错误诊断。

## 7. 主要风险

- `f[x]` 的类型实参/索引歧义；
- 泛型类型身份和 invariant 检查遗漏；
- `|` 实例化成员检查和作者责任边界；
- 约束包含检查过于宽松或过于严格；
- 显式实例化函数值的 thunk 生命周期和 GC；
- 运行时擦除后 meta/serialization 无法恢复类型参数；
- VM/JIT 行为不一致；
- 文档与实现状态不同步。

## 8. 完成标准

用户自定义泛型可视为完成，需要同时满足：

- 本文语法和语义全部实现；
- 正反例测试覆盖；
- release 和 JIT 全量测试通过；
- meta 和序列化行为符合本文；
- 文档同步；
- 现有内部容器泛型和 `_` / `proto` / `ref` 行为无回归。
