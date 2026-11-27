# Skein Unicode 归一化与大小写折叠方案（首版已实现）

> 当前状态与总优先级见 [`roadmap.md`](roadmap.md)。

- 状态：已实现（首版）
- 相关模块：`std.unicode`
- Unicode 版本：16.0.0
- 实现依赖：vendored `utf8proc` 2.10.0

## 1. 目标

为字符串提供显式的 Unicode 归一化能力：

- NFC
- NFD
- NFKC
- NFKD

归一化不会自动发生在字符串创建、比较、拼接或索引时；调用方必须显式使用
`std.unicode.normalize`。

## 2. API

```text
import std;
std.unicode.normalize(text, form)      → string
std.unicode.is_normalized(text, form)  → bool
std.unicode.version()                  → string
std.unicode.casefold(text)             → string
```

`form`：

```text
"nfc" "nfd" "nfkc" "nfkd"
```

大小写不敏感。

参数约束：

- `text` 必须是 `string`，内部按合法 UTF-8 处理。
- `form` 必须是 `string`。
- `version` 不接受参数。
- `casefold` 接受一个 `string`，返回折叠后的新字符串。
- 未知 form、参数类型错误、参数个数错误都会报运行时错误。
- 归一化结果不得包含 `U+0000`；现有 `string` 不能保存内嵌 NUL。

## 3. 实现

### 3.1 依赖

使用 vendored `utf8proc` 2.10.0：

```text
third_party/utf8proc/
  utf8proc.c
  utf8proc.h
  utf8proc_data.c
  LICENSE.md
  MANIFEST
```

选择 2.10.0 的原因：

- 对应 Unicode 16.0.0；
- 与当前 `src/runtime/data/unicode_ident_data.inc` 的 Unicode 版本一致；
- MIT 许可；
- 支持完整的 NFC / NFD / NFKC / NFKD；
- 不需要系统 ICU。

### 3.2 调用方式

`src/runtime/unicode.cpp` 使用 `utf8proc_map`：

```text
NFC   = UTF8PROC_STABLE | UTF8PROC_COMPOSE
NFD   = UTF8PROC_STABLE | UTF8PROC_DECOMPOSE
NFKC  = UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_COMPAT
NFKD  = UTF8PROC_STABLE | UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT
```

`is_normalized` 和 `normalize` 会先做 Unicode Quick_Check：

- `Yes`：直接判定已归一化，不分配新字符串；
- `No`：直接判定未归一化；
- `Maybe`：再调用 `utf8proc_map` 做完整检查和归一化。

quick-check 范围表由
`tools/gen_unicode_quickcheck.py` 从 Unicode 16.0.0 的
`DerivedNormalizationProps.txt` 生成到
`src/runtime/data/unicode_quickcheck.inc`。同时检查组合字符的 canonical
ordering，避免只依赖单码点属性造成误判。

`version()` 返回 `utf8proc_unicode_version()`，当前为 `"16.0.0"`。

### 3.3 构建接入

CMake 新增静态库：

```cmake
add_library(skein_utf8proc STATIC third_party/utf8proc/utf8proc.c)
target_compile_definitions(skein_utf8proc PUBLIC UTF8PROC_STATIC=1)
```

`src/runtime/unicode.cpp` 加入主程序源文件，并链接 `skein_utf8proc`。

## 4. 语义决定

### 4.1 不自动归一化

`==`、`+`、下标、`length()`、`std.string.find` 等操作保持现有码点序列语义，不做 NFC/NFKC。
这样不会改变已有程序行为，也避免输入边界产生隐式数据变化。

### 4.2 标识符 NFC 归一化

非 ASCII 标识符在词法分析后按 NFC 归一化：

- 规范等价写法视为同一标识符，例如 `e + U+0301` 与 `é`；
- 不做 NFKC，不把全角字符、连字、罗马数字等兼容字符折叠为 ASCII；
- 关键字和内置函数名仍只识别 ASCII；
- 函数名、参数名、结构体字段、类型名、label、包名和 import 路径组件
  使用同一套 NFC 规则；
- 同一作用域中归一化后重名的声明报 `symbol repeated`；
- 诊断中的标识符名称是 NFC 后的形式。

包名和 import 路径组件也会被 NFC 归一化，因此对应的目录名必须使用 NFC；
这是引入标识符归一化后的明确约束。

NFKC 不作为标识符默认规则。它会把 `ａ`、`Ⅳ`、`µ`、`ſ` 等折叠为 ASCII
或其它字符，容易产生意外冲突，也可能把一个字符展开成多个字符。

### 4.3 NUL 和非法 UTF-8

- `string` 不能包含内嵌 NUL，归一化结果出现 `U+0000` 时报错。
- 输入字符串按合法 UTF-8 处理；非法 UTF-8 由 `utf8proc_map` 报错。
- 不提供 `bytes` 输入；二进制数据应先通过 `std.encoding.decode` 转成文本。

### 4.4 大小写：映射与折叠

`std.string.to_lower` / `to_upper` 与 `std.unicode.casefold` 解决不同问题：

| | `to_lower` / `to_upper` | `casefold` |
|---|---|---|
| 目的 | 展示、格式化 | 大小写不敏感比较 |
| 契约 | Unicode 默认完整大小写映射 | 完整大小写折叠 |
| 长度 | 可能变长（`ß` → `SS`） | 可能变长（`ß` → `ss`） |
| 上下文 | `Σ` 受 Final_Sigma 影响 | 无条件映射 |
| 可逆 | 否 | 否 |
| 幂等 | 不保证 | 保证 |

因此：

- `to_upper("ß")` 是 `SS`，`to_upper("Straße")` 是 `STRASSE`；
- `to_upper("ﬁ")` 是 `FI`，与 `to_upper("fi")` 碰撞；
- `to_lower("İ")` 是 `i` + `U+0307`，保留组合点；
- `to_lower("ΟΣ")` 是 `ος`（词尾 ς），`to_lower("ΟΣΑ")` 是 `οσα`；
- `casefold("ß")`、`casefold("ẞ")`、`casefold("SS")` 都是 `ss`；
- `casefold("ΟΣ")`、`casefold("ος")`、`casefold("οσ")` 都是 `οσ`。

大小写映射是有损、不可逆的：

```text
to_upper("ß") == to_upper("ss")          // 都是 "SS"
to_lower(to_upper("ß")) == "ss"          // 回不到 "ß"
```

不要用大小写转换生成键、标识符或文件名；需要保留原值就存原串。

大小写不敏感比较的推荐流程：

```text
import std;
std.unicode.normalize(std.unicode.casefold(
    std.unicode.normalize(text, "nfc")), "nfc")
```

`casefold` 本身不做归一化，`Å` 与 `A` + `U+030A` 折叠后仍然不同。

实现上：

- `to_lower` / `to_upper` 先查 `SpecialCasing.txt` 的无条件完整映射，其余码点
  回退到 `utf8proc_tolower` / `utf8proc_toupper` 的简单映射；
- `Σ` 的小写使用 `Final_Sigma` 上下文规则，依赖 `Cased` 与 `Case_Ignorable`
  派生属性；
- `casefold` 使用 `utf8proc_map` 的 `UTF8PROC_CASEFOLD`（完整折叠）；
- 两者都拒绝非法 UTF-8，且与 locale 无关（不实现 `tr` / `az` / `lt` 变体）。

数据由 `tools/gen_unicode_case.py` 从 `SpecialCasing.txt` 与
`DerivedCoreProperties.txt` 生成 `src/runtime/data/unicode_case_data.inc`。

## 5. 测试

已覆盖：

- NFC：分解形式 `e + U+0301` → `é`
- NFD：`é` → `e + U+0301`
- NFKC / NFKD：`①` → `1`
- 兼容字符：`ﬁ` 在 NFC 下保持，在 NFKC 下变为 `fi`
- 全角字符：`１` 在 NFKC 下变为 `1`
- Hangul：Jamo 序列 NFC 组合为音节
- 规范顺序：组合附加符按 CCC 重排
- Singleton decomposition：`U+212B` → NFC `U+00C5`，NFD `A + U+030A`
- `is_normalized` 的正反例
- `version()` 返回 `16.0.0`
- `to_lower` / `to_upper`：ASCII、`ß` → `SS`、`Straße` → `STRASSE`、`ﬁ` → `FI`、
  `İ` → `i` + `U+0307`、希腊文 `ΟΣ` / `ΟΣΑ` / `Σ` 的 Final_Sigma 结果
- 大小写映射不可逆：`to_upper("ß") == to_upper("ss")`，
  `to_lower(to_upper("ß")) == "ss"` 且不等于 `"ß"`
- `casefold`：`Straße` / `STRASSE` / `ẞ` 折叠为 `strasse` / `ss`，`ς` 折叠为 `σ`，
  幂等性，以及不做归一化
- 未知 form、参数类型错误、参数个数错误
- 标识符 NFC：变量、函数、结构体、字段、label 的规范等价写法互通
- 归一化后重复声明会报 `symbol repeated`
- 包名和 import 路径组件按 NFC 解析
- 已用 Unicode 16.0.0 `NormalizationTest.txt` 的 19965 个用例、399300 次
  归一化操作做全量校验，全部通过

## 6. 后续

- 评估 grapheme cluster 等 `std.unicode` 扩展。
- 评估 titlecase 与 locale 相关映射（`tr` / `az` / `lt`）；默认 API 保持
  locale 无关。
- 若要升级 Unicode 版本，同时升级 `utf8proc` 和 `unicode_ident` 数据，并重新跑
  `NormalizationTest.txt`。
- 只有在明确产品需求下才评估标识符 NFKC；默认保持 NFC。
