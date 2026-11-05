# Skein 阶段 0：元编程基础设施实施计划

- 更新日期：2026-11-05
- 状态：M0-0 ~ M0-5 已完成（2026-11-06）
- 关联文档：[README.md](../README.md)、[SYNTAX.md](../SYNTAX.md)、
  [architecture.md](architecture.md)、[bytecode-plan.md](bytecode-plan.md)
- 阶段结论：**不引入用户自定义宏**，先用运行时反射、受控代码生成和少量
  编译器常量折叠覆盖真实的元编程需求。

## 1. 目标与非目标

### 1.1 目标

1. 新增 `std.meta`，把已有的 struct / enum / trait 元数据暴露为普通值。
2. 补强 `std.struct` / `std.enum` 的解码能力：默认值、类型提示、歧义处理。
3. 让 `const` 的不可变标量初始化在同一个编译单元内可参与常量折叠。
4. 明确并保留 FFI bindgen 作为“外部代码生成”路径。
5. 为阶段 1 的 `@derive` / 属性式元编程留出稳定的运行时查询接口。

### 1.2 非目标

- 不新增 `macro` 关键字、宏调用语法或文本替换预处理器。
- 不引入 SwiftUI 风格的前导点成员（`.title`）、静态成员
  （`Color.yellow`）或引用对象语义；链式构建继续使用工厂函数、返回
  `self` 的方法和 enum `::` 变体。
- 不实现编译期执行任意用户代码（I/O、网络、随机数等）。
- 不实现泛型、静态方法、trait 默认方法或运算符重载。
- 不改变现有 `std.json` 输出格式。
- 不新增与 `std.json.stringify` 重复的 `std.struct.encode` /
  `std.enum.encode` 线格式。
- 不重写 FFI bindgen，也不把 bindgen 逻辑移进解释器运行时。
- 不在本阶段做 AOT 代码生成。

## 2. 当前基线与缺口

| 能力 | 当前状态 | 位置 |
|---|---|---|
| 值类型判断 | `kind_of` / `type_name` / `type_module` / `type_source` | `src/builtins/builtins_convert.cpp` |
| struct 字段元数据 | 运行时已有 `struct_def::fields`，脚本不可见 | `src/runtime/structtype.h` |
| enum 变体元数据 | 运行时已有 `enum_def::variants`，脚本不可见 | `src/runtime/structtype.h` |
| trait / impl 元数据 | 运行时已有 `trait_def` 与 `type_def::traits`，脚本不可见 | `src/runtime/structtype.h` |
| struct 解码 | `std.struct.decode(value, type_name)`；不填默认值 | `src/runtime/serialization.cpp` |
| enum 解码 | `std.enum.decode(value)`；两段 tag 在跨包同名时可能歧义 | `src/runtime/serialization.cpp` |
| enum tag | `std.enum.tag` 输出 `module::Enum::Variant` | `src/builtins/builtins.cpp` |
| 常量 | `const` 仅禁止重新赋值；运行时存储 | `src/interpreter/eval.cpp`、`src/interpreter/vm.cpp` |
| 编译期折叠 | 仅字面量、算术、位运算、比较 | `src/interpreter/compiler.cpp` |
| 外部代码生成 | FFI bindgen（`--ffi-gen` / `--check-ffi`） | `src/builtins/ffi/ffi_bindgen.cpp` |

核心判断：缺的不是宏，而是**稳定的运行时类型描述**。有了 `std.meta`，
序列化、校验、CLI、调试输出、路由注册等场景都可以用普通函数完成。

## 3. API 契约

### 3.1 `std.meta`

`std.meta` 只读、无副作用，返回全新构造的 `list` / `dict` / 字符串 /
布尔值 / `null`，不得把内部 `struct_def*`、`enum_def*`、`function_def*`
暴露给脚本。

#### 类型参数

所有需要“类型”的参数支持三种形式：

- struct / enum 实例，例如 `Point { x: 1 }`、`Event::Timeout`；
- 内建类型的值，例如 `1`、`[1, 2]`、`sqrt`、`null`；
- 类型名字符串，例如 `"Point"`、`"aa::Point"`、`"int"`、`"Drawable"`。

定义是：**只要 `type_name(value)` 是一个类型名，就可以把该值传给
`std.meta`**。`kind_of` 返回的 `struct` / `enum` / `method` / `native` /
`class` 是运行时 kind 标签，不是类型名，不能传给 `std.meta`。

trait 没有实例，只能使用字符串。内建类型名接受类型标注中的拼写和别名，
例如 `"str"` 规范化到 `"string"`，`"func"` 规范化到 `"function"`。
未限定包名的用户类型名必须唯一；跨包同名时报 `ambiguous type X`，
调用方应使用 `pkg::Type`。

#### 函数

| 函数 | 返回 | 说明 |
|---|---|---|
| `std.meta.type_info(type)` | `dict` | 完整描述任意命名类型 |
| `std.meta.fields(type)` | `list` | struct 字段；非 struct 返回 `[]` |
| `std.meta.variants(type)` | `list` | enum 变体；非 enum 返回 `[]` |
| `std.meta.methods(type)` | `list` | 方法描述；无方法返回 `[]` |
| `std.meta.traits(type)` | `list<string>` | 已实现的 trait；无则返回 `[]` |
| `std.meta.implements(type, trait)` | `bool` | 是否显式实现了指定 trait |
| `std.meta.has_type(type)` | `bool` | 命名类型是否存在；未知返回 `false` |

所有访问器对合法类型名都是**全函数**：不适用的部分返回空列表，而不是
按 kind 抛错。这样调用方可以统一遍历 `fields` / `variants` / `methods` /
`traits`，不需要先判断类型来自内置还是用户定义。只有以下情况报错：

- 类型名未知；
- 未限定类型名有歧义；
- 参数不是合法类型值或类型名字符串；
- `implements` 的 trait 名未知。

#### `type_info` 返回值

```text
{
  "kind": "struct" | "enum" | "trait" | "builtin",
  "name": "Point",         // 内建类型使用规范名，例如 str -> string
  "module": "aa",          // 内建类型为 ""
  "source": "aa",          // 内建类型为 ""
  "exported": true,        // 内建类型恒为 true
  "fields": [ ... ],       // 仅 struct 非空
  "variants": [ ... ],     // 仅 enum 非空
  "methods": [ ... ],      // 无方法为 []
  "traits": [ ... ]        // 无实现为 []
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

变体描述：

```text
{
  "name": "UserMsg",
  "fields": [
    {
      "name": "text",
      "type": "string",
      "nullable": false,
      "readonly": true,
      "has_default": false
    }
  ]
}
```

方法描述：

```text
{
  "name": "draw",
  "params": ["int"],   // 类型名；不含 self，是调用时显式传入的参数
  "variadic": false,
  "has_defaults": false
}
```

规则：

- `fields` / `variants` 保持声明顺序；`methods` 按方法名排序；
  `traits` 输出规范名 `pkg::Trait` 并排序。
- 方法参数只给出类型字符串，不暴露参数名；`self` 不出现在 `params` 中，
  因为脚本调用 `p.draw(x)` 时不会显式传入接收者。
- `has_defaults` 表示除 `self` 外是否有带默认值的参数。trait 方法不允许
  默认值，因此 trait 的 `has_defaults` 恒为 `false`。
- `readonly` 表示字段是否只读：struct 字段为 `false`，enum 载荷字段为
  `true`；`std.meta.fields` 只返回 struct 字段，因此恒为 `false`。
- `has_default` 只表示字段有默认值，不在 `std.meta` 中求值默认值。
- 只查询**已加载且可见**的类型：当前源文件 / 当前逻辑模块中的类型可见；
  其他包的类型必须 `exported`。不可见类型在 `type_info` / `fields` 等函数中
  报 `type X is not visible`，`has_type` 返回 `false`。
- 未限定名优先查当前源文件/模块，再查唯一的可见 exported 类型；多个匹配
  时报歧义。值形式的参数同样受可见性约束，不能借一个隐藏类型的值绕过。
- 内建类型 `kind` 为 `"builtin"`，`module` / `source` 为空字符串，
  `exported` 为 `true`，`fields` / `variants` / `methods` / `traits` 都为
  `[]`。`number`、`callable`、`any` 的子类型关系不在阶段 0 展开；运行时
  匹配继续使用 `is`。
- 内建类型名使用类型标注接受的拼写：`int`、`float`、`number`、`bool`、
  `string`/`str`、`char`、`byte`、`bytes`、`list`、`dict`、`deque`、
  `heap`、`set`、`function`/`func`、`builtin`、`callable`、`null`、
  `userdata`、`result`、`error`、`any`。别名返回规范名。
- `lru`、`method`、`native`、`class`、`struct`、`enum` 不是命名类型，
  `std.meta` 不接收这些名字；它们属于 `kind_of` 的运行时 kind 分类。
- 访问器对合法类型名都是全函数：非 struct 的 `fields`、非 enum 的
  `variants`、无方法的 `methods`、无实现的 `traits` 都返回 `[]`。
- `implements` 对不可实现 trait 的类型（包括内建类型和 trait 自身）
  返回 `false`；未知 trait 名报错。
- 返回值是可修改的普通值；修改它们不会影响类型定义。

#### 示例

```skein
import std;

struct Point { x: int; y: int = 0; }

let info = std.meta.type_info("Point");
print(info["kind"]);          // struct
print(info["fields"][0]["name"]); // x
print(info["fields"][1]["has_default"]); // true
print(std.meta.fields(Point { x: 1 })[0]["type"]); // int
```

内建类型与用户定义类型使用同一种描述：

```skein
let builtin = std.meta.type_info("str");
print(builtin["kind"]);   // builtin
print(builtin["name"]);   // string
print(std.meta.fields(builtin)); // []
print(std.meta.has_type("list")); // true
print(std.meta.implements("int", "Drawable")); // false
```

`kind_of` 不是类型名：

```skein
print(kind_of(1));        // int
print(type_name(1));      // int
print(kind_of(Point { x: 1 })); // struct
print(type_name(Point { x: 1 })); // Point

std.meta.type_info(kind_of(Point { x: 1 })); // 错误：struct 不是类型名
std.meta.type_info(type_name(Point { x: 1 })); // 正确
```

### 3.2 `std.struct.decode` / `std.enum.decode`

本阶段只补 **decode**，不新增 encode 线格式。编码仍然使用
`std.json.stringify`，避免出现两套 JSON 语义。

#### `std.struct.decode(value, type_name[, options])`

- 两参数行为完全不变。
- `options` 必须是 dict，当前只接受 `{"defaults": bool}`；未知 key 报错。
- `defaults: true` 时，输入中缺失且有默认值的字段会用声明时的默认值填充。
- 默认值在解码时、声明包作用域中求值；每次 decode 都重新求值，可能有副作用。
- 默认值求值失败、返回控制流或类型不匹配时，沿用现有错误格式。
- 可空字段没有默认值时仍然填 `null`，与现有行为一致。
- 未知字段仍然报错，不提供 `ignore_unknown`（等真实需求出现再加）。
- 嵌套 struct / enum 递归解码逻辑保持不变。

```skein
struct Config { host: string = "localhost"; port: int = 80; }

let c = std.struct.decode({"port": 8080}, "Config", {"defaults": true});
print(c.host); // localhost
```

#### `std.enum.decode(value[, type_name][, options])`

- 单参数行为完全不变。
- 第二个参数是字符串时作为类型提示；是 dict 时作为 options。
- 第三个参数存在时，第二个参数必须是类型名字符串或 `null`。
- 类型提示用于：
  1. 校验 tag 指向的 enum 与提示一致；
  2. 在 `E::A` 这种不含包名的两段 tag 出现跨包同名时消除歧义。
- 当前只接受 `{"defaults": bool}`；enum 载荷字段的默认值填充与 struct
  使用同一套规则。
- 错误信息优先包含类型提示，例如
  `enum main::E does not match expected aa::E`。

```skein
enum E { A { n: int; }; }

let e = std.enum.decode(
  {"$enum": "E::A", "payload": {}},
  "E"
);
```

### 3.3 编译期常量表达式

这里的“常量表达式”是**编译器优化**，不是新的语言特性：

- 只有 `const` 绑定参与，`let` 不参与。
- 只传播不可变标量：`int`、`float`、`bool`、`string`、`char`、`byte`、
  `null`；list / dict / struct / enum / function 不传播。
- 可折叠语法：字面量、已传播的 const 名、`+ - * / %`、位运算、
  移位、比较、字符串 `+`、一元 `-` / `~`。
- 只在一个编译单元内传播：同一个函数体、同一个 block、同一个 package
  块。不跨函数、不跨包、不跨 REPL 顶层语句。
- 遇到任何可能改变语义的情况（名称遮蔽、闭包捕获、类型标注、运行时
  错误）宁可不折叠。
- 不改变错误文本、类型检查和运行时行为。

```skein
func area() {
  const SIDE = 3;
  const AREA = SIDE * SIDE;
  return AREA;
}
```

编译后 `AREA` 的引用应直接使用常量，而不是读取局部 slot；`SIDE` 仍保留
为可被闭包捕获的局部绑定。

## 4. 任务分解

规模：S（0.5–1 人日）、M（1–2 人日）、L（3 人日以上）。总估算约
**7–10 人日**（不含 review 和 CI 排队）。

### M0-0：冻结设计与基线（S）

- 新增本计划文档。
- 在 `SYNTAX.md` 的“当前不支持”中明确：不提供用户自定义宏 / 语法扩展。
- 确认 `std.meta` 不进入内核语法，不修改 lexer / parser。
- 记录 `std.struct.decode` 两参数行为不得变化的兼容约束。

验收：文档评审通过；`git diff` 只有文档。

### M0-1：运行时类型查询（M，P0）

状态：**已完成（2026-11-05）**。

- 在 `src/runtime/structtype.h/.cpp` 增加：
  - `structtype_find_trait_by_name(module_name, name)`；
  - `structtype_find_struct_qualified(name)`、
    `structtype_find_enum_qualified(name)`、
    `structtype_find_trait_qualified(name)`，统一处理 `pkg::Type` 与歧义。
- 新增 `src/runtime/typeinfo.h/.cpp`（L1）：
  - 把 `field_def`、`variant_def`、`method_sig`、`function_def` 转成普通值；
  - 只复制字符串和标量，不暴露内部指针；
  - 方法参数从 `function_code::sl` 或 `method_sig` 生成，并跳过第一个
    `self` 参数；
  - 在 `symbol.h/.cpp` 提供唯一的内建类型名表、规范名和别名映射，
    `typeinfo` 与 parser 的类型标注解析共用，避免两套映射漂移；
  - 使用当前函数的 module/source 作为可见性上下文；AST 与 VM 在函数调用
    时保存/恢复该上下文，使包内函数能查询自己的非导出类型和 import 别名；
  - 错误通过 `yyerror` 上报。
- 更新 `CMakeLists.txt` 的 `skein_runtime` 源文件列表。
- 更新 `docs/architecture.md` 的 L1 文件归属和 `README.md` 目录结构。

验收：

- `typeinfo` 在 AST / VM 两种后端下行为一致；
- 不新增对 `eval.h`、`ast.h`、`builtins.h` 的 include；
- `runtime_link_test` 仍只链接 L1 即可通过。

### M0-2：注册 `std.meta`（S，P0）

状态：**已完成（2026-11-05）**。

- 在 `src/runtime/builtin_ids.h` 增加 `B_meta_*`。
- 在 `src/interpreter/std_namespace.cpp` 注册 `std.meta` 子模块。
- 新增 `src/builtins/builtins_meta.cpp`，实现：
  - 参数个数和类型检查；
  - 类型参数解析（实例或字符串）；
  - 七个函数的返回值和错误信息。
- 在 `src/builtins/builtins.cpp` 分发 `B_meta_*`。
- 更新 `CMakeLists.txt` 的 `skein_builtins` 源文件列表。

验收：

- `import std; std.meta...` 和 `import std.meta; meta...` 都可用；
- 用户类型和内建类型都返回同一形状的 `type_info`；
- 未知类型、歧义、非法类型参数有稳定错误；
- `fields` / `variants` / `methods` / `traits` 对不适用的类型返回 `[]`；
- 修改返回的 list/dict 不影响后续查询。

### M0-3：`std.meta` 测试与文档（S，P0）

状态：**已完成（2026-11-05）**。

- 新增 `tests/cases/meta.py`，覆盖：
  - struct 字段、默认值标记、可空类型、`readonly`；
  - enum 变体与载荷字段（载荷字段 `readonly` 为 `true`）；
  - 方法参数不含 `self`、`has_defaults`、trait 签名；
  - 内建类型描述、`str`/`func` 别名规范化、`number`/`callable`/`any`；
  - 类型标注中的每个内建类型名/别名都能被 `std.meta` 解析；
  - `kind_of` 标签不能当类型名（`struct` / `enum` / `method`）；
  - 跨包类型与未限定名歧义；
  - 可见性：当前包/源文件的本地类型、其他包的 exported 类型、隐藏类型
    在 `type_info` 中报错且 `has_type` 返回 `false`；
  - `has_type` 的 true/false；
  - `fields` / `variants` / `methods` / `traits` 对不适用类型返回 `[]`；
  - 返回值的可修改性与隔离。
- 在 `tests/test_skein.py` 的 `MODULES` 中加入 `meta`。
- 更新 `README.md` / `SYNTAX.md`：
  - 模块列表加入 `std.meta`；
  - README 函数列表加入七个函数；
  - SYNTAX 6.4 增加 `### std.meta` 和示例。
- 运行 `python3 tools/check_docs_sync.py`。

验收：文档同步检查通过；新增用例在 `SKEIN_VM=0/1` 下都通过。

### M0-4：struct / enum decode 增强（M，P1）

状态：**已完成（2026-11-06）**。

- 在 `serialization.cpp` 中增加 L1 decode 的 `defaults` 通路：
  - `decode_struct_object` / `decode_enum_object` 在缺字段时调用
    `field_def::default_value`；
  - 通过 `skein_call_function` 调用默认值，并做类型校验；
  - 递归把 `defaults` 传给嵌套 struct / enum。
- 在 `builtins.cpp` 增加 options 解析和 enum type hint 解析：
  - `std.struct.decode(value, type_name[, options])`；
  - `std.enum.decode(value[, type_name][, options])`。
- 为 enum decode 增加可选类型提示；tag 与提示冲突时报错。
- 复用 M0-1 的 qualified lookup，避免重复解析 `pkg::Type`。
- 明确 enum 载荷默认值是否在 M0-4 完成；若时间不足，降级为
  “仅 struct 默认值”，并在 SYNTAX 中写清楚。

验收：

- 两参数 `std.struct.decode` 和单参数 `std.enum.decode` 输出与错误文本不变；
- 新选项只在显式传入时生效；
- 默认值在声明包作用域求值，嵌套字段与可空字段行为正确；
- 默认值求值失败不会泄漏值（LSan 通过）。

### M0-5：常量传播（M，P2）

状态：**已完成（2026-11-06）**。

- 把 `compiler.cpp` 的 `fold_node` 改为可使用当前作用域常量表。
- 在 `SlotScope` 中增加 `const_values`（仅不可变标量）。
- `case 'D'` 中：`const` 初始化折叠成功后登记；`case 'N'` 中优先命中常量表
  并发出 `LoadConst`。
- 名称查找必须遵守遮蔽：内层 `let` / `const` 声明会截断外层常量。
- 扩展字符串 `+`、一元 `!`（仅 bool/null）等安全折叠。
- 不跨函数、不跨包、不跨 REPL 顶层语句；闭包仍通过 slot/upvalue 捕获。
- 增加可回退开关（例如 `SKEIN_NO_CONST_PROP=1`）便于定位编译器问题。

验收：

- 语义测试、遮蔽测试、闭包测试、const 容器测试全部通过；
- VM 与 AST 差分测试无差异；
- `SKEIN_VM_TRACE=1` 下能看到 const 引用变为 `LoadConst`；
- 基准无回退，编译时间无显著增长。

### M0-6：集成与验收（S，P0）

- 更新 `docs/architecture.md`、`README.md` 目录结构。
- 运行完整矩阵：
  - `ctest --test-dir build --output-on-failure`
  - `tools/check_all.sh --clean`
  - ASan/UBSan、LSan、VM-only
  - `tools/check_docs_sync.py`
- 记录阶段结果和未完成项。

## 5. 测试计划

### 5.1 功能用例

| 模块 | 重点 |
|---|---|
| `tests/cases/meta.py` | `type_info` / `fields` / `variants` / `methods` / `traits` / `implements` / `has_type` |
| `tests/cases/match_types.py` | struct 默认值、enum 类型提示、嵌套解码、错误文本 |
| `tests/cases/language_core.py` | const 折叠、遮蔽、const 容器、闭包捕获 |
| `tests/cases/io_scripts.py` | 跨包类型名、`pkg::Type`、歧义错误 |

### 5.2 差分与后端

- `skein_tests`（`SKEIN_VM=0`）与 `skein_vm_tests`（`SKEIN_VM=1`）必须
  同时通过。
- `vm_differential_test` 增加或随机覆盖 `std.meta` / decode 用例。
- VM-only 构建必须通过；不能依赖 AST 参考后端。

### 5.3 内存与错误

- ASan/UBSan：默认值求值失败、meta 返回 dict/list 的释放路径。
- LSan：反复调用 `std.meta` 不泄漏；decode 选项 dict 不泄漏。
- 错误用例：未知类型、歧义类型、错误参数个数、未知 option、未知 trait、
  把 `kind_of` 标签当类型名。

### 5.4 文档

- `tools/check_docs_sync.py` 必须通过。
- 模块列表、README 函数列表、SYNTAX 示例保持一致。
- 新增的 `typeinfo.*` / `builtins_meta.*` 写入架构文档和目录结构。

## 6. 验收标准

- [ ] `std.meta` 七个函数可用，返回普通值，不泄漏内部指针。
- [ ] 用户定义类型和内建类型共用同一套 `type_info` 形状和访问器。
- [ ] `std.meta` 只暴露已加载且可见的类型：本地类型可用，其他包类型必须
      `export`；隐藏类型报错 / `has_type` 为 `false`。
- [ ] `kind_of` 的 `struct` / `enum` / `method` 等标签不被误当作类型名。
- [ ] `std.struct.decode` 两参数行为不变；`defaults` 为显式 opt-in。
- [ ] `std.enum.decode` 单参数行为不变；类型提示能消除跨包歧义。
- [ ] const 标量传播在函数体 / block / package 块内正确，且可回退。
- [ ] AST 与 VM 全部测试通过，VM-only 通过。
- [ ] ASan/UBSan/LSan 通过。
- [ ] `tools/check_all.sh --clean` 通过。
- [ ] 文档同步检查通过。
- [ ] 没有新增 `macro` 关键字、宏语法或编译期任意代码执行。
- [ ] 性能基线无明显回退。

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 类型名跨包歧义 | 查询结果不确定 | 未限定名歧义直接报错；文档推荐 `pkg::Type` |
| 内建类型名与 `kind_of` 混淆 | 用户把 `struct` 当类型名 | 文档明确两套 taxonomy；`type_info(kind_of(...))` 报错；测试覆盖 |
| 暴露内部元数据 | 脚本修改影响类型系统 | 只返回新构造的普通值；不暴露指针 |
| 默认值有副作用 | decode 行为不可预期 | 显式 `defaults` 开关；文档说明每次求值 |
| 常量传播破坏遮蔽/闭包 | 语义错误 | 只做同作用域标量；内层声明截断；差分 + 回退开关 |
| 方法元数据依赖 AST | AST 释放后信息缺失 | 只依赖 `function_code::sl` / `method_sig`；缺失时返回空参数并保持可解析 |
| 文档同步遗漏 | CI 失败 | 每个任务把 README/SYNTAX 更新列为 DoD |
| 架构边界破坏 | `runtime_link_test` 失败 | `typeinfo` 留在 L1；默认值求值留在 L3 |

## 8. 阶段 1 的接口预留

阶段 0 完成后，如果仍出现“必须捕获语法”的需求，优先做**内建声明式
属性**，而不是通用宏。下面的 `@derive` 只是语法示意，**不是已确定的
Skein 语法，也不在阶段 0 范围内**：

```skein
@derive(json)
struct Point { x: int; y: int; }
```

阶段 1 需要先回答：

- 哪些 derive 名称是内建的，是否允许用户扩展；
- `json` 是否真的需要 derive（当前 `std.json.stringify` 已能编码任意
  struct/enum，解码需要类型名）；
- 生成的是方法、trait impl，还是包级函数；
- 生成物与用户同名方法冲突时如何报错；
- 如何用 `--expand` 展示展开结果并保持源码行号。

阶段 1 应复用本阶段的 `std.meta` 元数据表示，避免再定义一套类型描述。
只有出现“函数无法表达的新绑定形式/控制流”时，才讨论受限宏；届时仍应
坚持 AST 级、卫生、可展开调试和编译期无 I/O。

## 9. 建议执行顺序

1. M0-0（冻结设计）
2. M0-1（运行时查询）→ M0-2（注册）→ M0-3（测试/文档）
3. M0-4（decode 增强）可与 M0-5（常量传播）并行
4. M0-6（全量验收）

如果排期紧张，优先保证 M0-1 / M0-2 / M0-3；M0-4 的 enum 默认值和
M0-5 可以拆到阶段 0.5，但不得因此提前引入宏。
