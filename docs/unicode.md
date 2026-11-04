# Skein Unicode 归一化方案

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
std.unicode.normalize(text, form)      → string
std.unicode.is_normalized(text, form)  → bool
std.unicode.version()                  → string
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

`is_normalized` 首版通过“归一化后与输入比较”实现，行为简单，后续可以改为
quick-check 优化。

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

`==`、`+`、下标、`len`、`find` 等操作保持现有码点序列语义，不做 NFC/NFKC。
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
- 未知 form、参数类型错误、参数个数错误
- 标识符 NFC：变量、函数、结构体、字段、label 的规范等价写法互通
- 归一化后重复声明会报 `symbol repeated`
- 包名和 import 路径组件按 NFC 解析
- 已用 Unicode 16.0.0 `NormalizationTest.txt` 的 19965 个用例、399300 次
  归一化操作做全量校验，全部通过

## 6. 后续

- 用 NFC/NFKC quick-check 优化 `is_normalized`。
- 评估 case folding、grapheme cluster 等 `std.unicode` 扩展。
- 若要升级 Unicode 版本，同时升级 `utf8proc` 和 `unicode_ident` 数据，并重新跑
  `NormalizationTest.txt`。
- 只有在明确产品需求下才评估标识符 NFKC；默认保持 NFC。
