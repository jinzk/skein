# Skein 编码支持方案（设计稿）

- 状态：已实现（首版）
- 目标版本：首版
- 相关模块：`std.encoding`
- 决策：GBK 与 GB18030 均使用内置码表，始终可用

## 1. 背景与目标

Skein 当前的 `string` 是 UTF-8 文本，二进制数据使用独立的 `bytes` 类型。
现有的 `bytes_to_string` 只接受合法 UTF-8，并且拒绝 NUL 字节；这满足内部一致性，
但无法处理 GBK、GB18030 等常见中文编码。

本方案新增 `std.encoding` 模块，提供显式的解码/编码能力：

- `bytes` + 源编码 → UTF-8 `string`
- UTF-8 `string` → `bytes` + 目标编码

核心目标：

1. GBK 内置码表，跨平台行为一致，不依赖 iconv 或 Windows 代码页。
2. GB18030 内置码表，跨平台行为一致。
3. 不改变现有 `bytes_to_string` / `bytes_from_string` 的严格 UTF-8 语义。
4. 对非法输入提供确定的错误处理策略。
5. 不引入新的强制第三方依赖。

## 2. 范围

### 2.1 本方案包含

- 新增 `std.encoding.decode` / `std.encoding.encode` / `std.encoding.supports`
- 内置以下编码：
  - `utf-8`
  - `ascii`
  - `latin-1` / `iso-8859-1`
  - `windows-1252`
  - `utf-16le` / `utf-16be` / `utf-16`
  - `utf-32le` / `utf-32be` / `utf-32`
  - `gbk` / `cp936`
  - `gb18030`
- GBK / GB18030 的双向转换
- `strict` / `replace` / `ignore` 错误策略
- 码表生成脚本、测试、文档

### 2.2 本方案不包含

- 修改 `string` 的内部表示以支持内嵌 NUL
- Base64 / URL 编码
- Big5、Shift_JIS、EUC-JP 等其它旧编码的内置码表
- GB2312 的严格子集编码（GBK 可解码 GB2312 字节，但不保证只生成 GB2312 字符）
- GB18030-2022 的差异映射（首版先固定一个版本，后续可升级）

其它旧编码可以在后续通过可选的 iconv / Windows 代码页后端支持，但不影响
GBK / GB18030 的可用性。

## 3. 总体设计

### 3.1 能力分层

| 层级 | 编码 | 实现 | 可用性 |
|---|---|---|---|
| L0 | `utf-8`、`ascii`、`latin-1`、`windows-1252`、`utf-16*`、`utf-32*` | 内置算法 | 始终 |
| L1 | `gbk` / `cp936` | 内置 GBK 码表 | 始终 |
| L2 | `gb18030` | 内置双字节表 + 四字节范围表 | 始终 |
| L3 | 其它旧编码 | 可选 iconv / Windows 代码页 | 有条件 |

GBK 和 GB18030 都不依赖平台后端，因此 `std.encoding.supports("gbk")` 和
`std.encoding.supports("gb18030")` 在正常构建中始终为 `true`。

`supports` 保留给后续可选编码使用，也方便脚本在不支持某个编码时提前分支。

### 3.2 与现有 API 的关系

- `bytes_to_string(bytes)` 保持严格 UTF-8，等价于：

  ```text
  std.encoding.decode(bytes, "utf-8", "strict")
  ```

- `bytes_from_string(string)` 保持现有行为。
- 首版已补充严格 UTF-8 的别名：

  ```text
  std.bytes.to_string(bytes)   → string
  std.bytes.from_string(string) → bytes
  ```

  这两个别名只做严格 UTF-8，不参与 GBK / GB18030 转换。

- GBK / GB18030 文本统一走 `std.encoding`。
- 任意二进制仍然使用 `bytes`、`bytes_hex`，不进入 `string`。

## 4. 公开 API

### 4.1 函数

```text
std.encoding.decode(bytes, encoding[, errors])  → string
std.encoding.encode(string, encoding[, errors]) → bytes
std.encoding.supports(encoding)                 → bool
```

参数说明：

- `bytes`：必须是 `bytes` 类型。
- `string`：必须是 `string` 类型，内部按合法 UTF-8 处理。
- `encoding`：编码名字符串，大小写不敏感。
- `errors`：错误策略，默认 `"strict"`。
  - `"strict"`：遇到非法序列或不可表示字符时报错。
  - `"replace"`：解码时用 U+FFFD 替换；编码时用 `?`（0x3F）替换。
  - `"ignore"`：跳过非法序列或不可表示字符。
- 返回值：
  - `decode` 返回 `string`。
  - `encode` 返回 `bytes`。
  - `supports` 返回 `bool`。

参数数量或类型错误、未知编码名、未知 `errors` 值都会报错。

### 4.2 编码名与别名

建议支持的名字：

| 规范名 | 别名 |
|---|---|
| `utf-8` | `utf8`、`utf_8` |
| `ascii` | `us-ascii` |
| `latin-1` | `latin1`、`iso-8859-1`、`iso8859-1` |
| `windows-1252` | `cp1252` |
| `utf-16le` | `utf16le`、`utf-16-le` |
| `utf-16be` | `utf16be`、`utf-16-be` |
| `utf-16` | `utf16` |
| `utf-32le` | `utf32le`、`utf-32-le` |
| `utf-32be` | `utf32be`、`utf-32-be` |
| `utf-32` | `utf32` |
| `gbk` | `cp936`、`ms936`、`windows-936` |
| `gb18030` | `gb-18030` |

命名规则：

- 大小写不敏感。
- `_` 和 `-` 在归一化时等价。
- 未知名字报错，不做模糊匹配。

不建议把 `gb2312` 直接作为 `gbk` 的别名：
GBK 是 GB2312 的超集，编码时可能生成 GB2312 范围外的字符。如后续需要，
应单独实现 `gb2312` 的受限编码器。

### 4.3 示例

```text
import std;

let s = std.encoding.decode(bytes_from_hex("d6d0b9fa"), "gbk");
print(s);                                      // 中国

let b = std.encoding.encode("中国", "gbk");
print(bytes_hex(b));                           // d6d0b9fa

print(std.encoding.supports("gbk"));           // 1 / true
print(std.encoding.supports("gb18030"));       // 1 / true

let e = std.encoding.decode(bytes_from_hex("ff"), "utf-8", "replace");
print(e);                                      // U+FFFD
```

### 4.4 Base64 和 URL 工具

`std.encoding` 还提供：

```text
std.encoding.base64_encode(bytes) → string
std.encoding.base64_decode(string) → bytes
std.encoding.url_encode(string) → string
std.encoding.url_decode(string) → string
```

- Base64 使用标准字母表和 `=` 填充，解码时忽略空白。
- URL 编码只保留 `A-Z a-z 0-9 - . _ ~`，其余字节编码为 `%XX`。
- `url_decode` 把 `+` 解释为空格，并校验解码结果是合法 UTF-8。
- 这些函数不做字符集转换；需要时先配合 `decode` / `encode` 使用。

## 5. 语义细则

### 5.1 错误策略

#### decode

- `strict`：遇到第一个非法序列时立即报错，错误信息包含编码名和字节偏移。
- `replace`：每个非法序列输出一个 U+FFFD。
- `ignore`：跳过非法序列，不输出替换字符。

#### encode

- `strict`：遇到不可表示字符时立即报错，错误信息包含字符和 UTF-8 偏移。
- `replace`：输出 `?`（0x3F）。
- `ignore`：跳过该字符。

GB18030 覆盖全部 Unicode 标量值，因此正常合法 `string` 中的字符都可以编码；
`replace` / `ignore` 主要用于防御非法内部 UTF-8 或未来受限编码。

### 5.2 NUL 约束

Skein 的 `string` 以 `char*` 保存，`newstrval` 通过 `strlen` 复制，
不能保存内嵌 NUL。因此：

- `decode` 的结果中只要出现 `U+0000`，无论 `errors` 是什么，都必须报错。
- `encode` 的输入 `string` 本身不会包含 NUL。
- 该限制是硬约束，不在本方案中修改 string 表示。

### 5.3 BOM

- `utf-16le` / `utf-16be`、`utf-32le` / `utf-32be`：
  - 不自动剥离 BOM；BOM 按 `U+FEFF` 处理。
- `utf-16` / `utf-32`：
  - 解码时读取 BOM，自动选择大小端并剥离 BOM。
  - 没有 BOM 时报错，提示使用显式字节序版本。
  - 编码时固定输出 BOM + 小端（`FF FE` / `FF FE 00 00`），保证跨平台一致。
- `utf-8`：
  - 默认不剥离 BOM，`EF BB BF` 解码为 `U+FEFF`。
- 源文件加载层会先按 BOM 识别并剥离 BOM，再调用对应显式编码解码。

### 5.4 非法序列消费规则

为了让 `replace` / `ignore` 行为可预测，明确以下规则：

#### GBK

- 首字节 `0x00..0x7F`：单字节 ASCII。
- 首字节 `0x80`：`U+20AC`（欧元符号，CP936 / Web 习惯）。
- 首字节 `0x81..0xFE`：
  - 有合法尾字节（`0x40..0xFE`，排除 `0x7F`）时消费 2 字节。
  - 尾字节非法时，只消费首字节；尾字节重新按单字节处理。
  - 文件末尾只剩首字节时，按截断序列处理，消费 1 字节。
- 其它首字节（如 `0xFF`）：非法单字节。
- `replace`：每个非法单字节/截断序列输出一个 U+FFFD。
- `ignore`：直接跳过。

#### GB18030

- 首字节 `0x00..0x7F`：单字节 ASCII。
- 首字节 `0x80`：非法（GB18030 的欧元是 `A2 E3`）。
- 首字节 `0x81..0xFE`：
  - 如果第二字节是 `0x30..0x39`，按四字节序列处理：
    - 需要第三字节 `0x81..0xFE`、第四字节 `0x30..0x39`。
    - 结构完整但落在保留区间时，视为非法四字节序列，消费 4 字节。
    - 结构不完整时，按截断序列处理，消费 1 字节。
  - 否则按双字节序列处理，尾字节规则同 GBK。
- `replace`：每个非法序列输出一个 U+FFFD。
- `ignore`：直接跳过。

## 6. 内置码表

### 6.1 GBK

GBK 双字节区：

- 首字节：`0x81..0xFE`，共 126 个。
- 尾字节：`0x40..0x7E` 和 `0x80..0xFE`，共 190 个。
- 总位置数：`126 × 190 = 23940`。
- 单字节 `0x80` 单独映射为 `U+20AC`，不进双字节表。

索引公式：

```text
index = (lead - 0x81) * 190 + (trail - (trail < 0x7F ? 0x40 : 0x41))
```

**重要优化：GBK 不单独存储一张 48 KB 表。**

经数据验证，GBK 的 21791 个有效双字节位置是 GB18030 的 23940 个位置的子集，
并且两者映射完全一致，没有冲突：

- GBK 有效位置：21791 个；
- GB18030 有效位置：23940 个；
- GB18030 多出：2149 个（在 GBK 中无效）；
- 两者映射冲突：0 个。

因此以 GB18030 双字节表作为唯一规范表，GBK 只额外保存一张有效性位图：

- 位图长度：23940 bit，原始约 2993 字节；
- 也可用 RLE 存储：约 82 段，约 86 字节；
- GBK 解码：查 GB18030 表，再用位图判断该位置在 GBK 中是否有效；
- GBK 编码：从 GB18030 反向表过滤掉位图中无效的位置；
- `0x80` 仍单独按 `U+20AC` 处理。

这样可以直接省掉约 45 KB，且不会引入两套表之间的映射漂移。
生成脚本必须断言“GBK 是 GB18030 的子集且映射一致”，防止更换数据源时破坏该前提。

### 6.2 GB18030

GB18030 需要两部分数据。

#### 双字节表

- 与 GBK 相同的 126 × 190 布局。
- GB18030 的双字节区全部 23940 个位置都有映射，
  其中约 2149 个位置在 GBK 中未分配，在 GB18030 中映射到 PUA 或扩展字符。
- 使用 `uint16_t[23940]` 作为唯一规范表；GBK 复用该表并配合有效性位图。
- 未压缩表大小：约 48 KB；压缩存储方式见 6.3。

#### 四字节范围表

GB18030 四字节区：

- 结构：`b1 b2 b3 b4`
  - `b1`：`0x81..0xFE`
  - `b2`：`0x30..0x39`
  - `b3`：`0x81..0xFE`
  - `b4`：`0x30..0x39`
- 线性索引：

  ```text
  index = (((b1 - 0x81) * 10 + (b2 - 0x30)) * 126
           + (b3 - 0x81)) * 10 + (b4 - 0x30)
  ```

- 索引总空间：`126 × 10 × 126 × 10 = 1,587,600`。
- 映射不是完全连续的，不能简单用 `index + 0x10000`。
- 使用范围表保存连续映射段：

  ```cpp
  struct Gb18030Range {
      uint32_t first_index;
      uint32_t last_index;
      uint32_t first_codepoint;
  };
  ```

- 预计约 207 段，表大小约 2.5 KB。
- 解码时二分查找 `index` 所在范围；不在任何范围则为非法。
- 编码时对码点二分查找范围，计算 `index` 后转回四字节。

#### 编解码优先级

- 编码时优先使用双字节表。
- 双字节表中不存在时，再使用四字节范围表。
- 这样 GBK 兼容字符会生成标准的双字节形式，而不是四字节形式。

### 6.3 压缩存储

可以压缩，而且建议至少做“共享 GBK 表”这一项；如果在意二进制体积，再对
GB18030 双字节表做 RLE。

#### 方案对比

| 方案 | 常量数据大小 | 码表本体运行时内存 | 复杂度 |
|---|---:|---:|---|
| 原始：GBK 表 + GB18030 表 + 范围表 | 约 96 KB | 约 96 KB | 最低 |
| 共享 GBK：GB18030 表 + 位图 + 范围表 | 约 52 KB | 约 53 KB | 低 |
| 共享 GBK + GB18030 表 RLE + 原始位图 + 固定范围表 | 约 32 KB | 约 53 KB | 中 |
| 共享 GBK + GB18030 表 RLE + 位图 RLE + 范围表 varint | 约 27 KB | 约 53 KB | 中 |

注意：RLE 减少的是**二进制/常量数据体积**；首次使用时仍会解码成
`uint16_t[23940]`，所以运行时内存不会显著下降。若连运行时内存也要压缩，
需要做“带检查点的按需解码”，会增加查找延迟和实现复杂度，首版不建议。
上表的运行时内存不包含编码反向表；反向表另计，见 6.6。

#### GB18030 双字节表 RLE 格式

GB18030 双字节表的 23940 个位置全部有效，可以按“连续码点段”压缩：

- 共 10351 段；
- 每段保存：
  - `uint8_t length`：段长，最大 115，能放进一个字节；
  - `zigzag varint start_delta`：本段起始码点相对上一段起始码点的差值。
- 起始码点差值有正有负（约 2939 段为负），所以必须用 **zigzag varint**，
  不能用无符号 varint；
- 长度数组约 10351 字节；
- 差值数组约 16337 字节；
- 合计约 26688 字节（约 26 KB），加上头部约 26 KB。

解码时：

```text
cp = 0
index = 0
for each run:
    cp += start_delta
    for k in 0..length-1:
        table[index++] = cp + k
```

使用 `std::call_once` 或函数局部静态变量，在首次需要 GB18030 双字节表时
解码一次。10351 次循环的启动成本可以忽略。

#### GBK 有效性位图

- 原始位图：23940 bit = 2993 字节；
- RLE：约 82 段，约 86 字节；
- 首版建议使用原始位图，代码更简单；如果追求最小体积，再改为 RLE。

#### 四字节范围表

- 固定结构：207 段 × 12 字节 = 2484 字节；
- 可选 delta + varint：约 682 字节；
- 首版建议保留固定结构，便于二分查找和调试；体积极小，不值得为它增加解码逻辑。

#### 推荐

- **必须做**：GBK 复用 GB18030 表 + 有效性位图，直接从约 96 KB 降到约 52 KB；
- **默认目标**：GB18030 双字节表使用 RLE，最终约 32 KB；
- **可选极限**：位图 RLE + 范围表 varint，最终约 27 KB；
- **不推荐**：zlib 压缩原始表。实测 47880 字节只能压到约 44927 字节，
  收益很小，还会引入解压依赖和启动成本。

### 6.4 数据来源与版本

首版建议固定为 **GB18030-2005** 映射：

- 与 Python 3.x `gb18030` codec、常见 glibc iconv、Windows CP54936
  的映射保持高度一致。
- 生成脚本使用 Python codec 生成表，生成结果提交到仓库。
- 生成文件头部记录：
  - 生成脚本路径
  - Python 版本
  - 生成日期
  - 数据来源/标准版本
- 后续如需 GB18030-2022，只替换表数据，API 不变；届时单独评估差异字符。

GBK 有效性位图从 Python `gbk` codec 生成，并单独处理 `0x80 → U+20AC`。
生成脚本必须断言：

- GBK 有效位置全部存在于 GB18030 表中；
- 同一位置的 Unicode 码点完全一致；
- GBK 有效位置数为 21791，GB18030 多出的位置数为 2149。

生成后应与 iconv `GBK` / `GB18030` 的常见向量交叉验证。

### 6.5 生成脚本与产物

新增：

```text
tools/gen_encoding_tables.py
src/runtime/data/encoding_gb18030_data.inc
src/runtime/data/encoding_gbk_validity.inc
```

生成脚本：

- `--check`：校验已提交的表是否与生成结果一致，并校验 GBK/GB18030 子集关系。
- 默认生成 GB18030 规范表、GBK 有效性位图和四字节范围表。
- 使用固定 Python codec，不依赖网络。
- 输出稳定的 C++17 数组和 varint 数据，避免每次生成出现无意义 diff。

建议的产物内容：

```cpp
// encoding_gb18030_data.inc
static const uint8_t GB18030_RUN_LENGTHS[10351] = { ... };
static const uint8_t GB18030_RUN_DELTAS[16337] = { ... }; // zigzag varint
static const Gb18030Range GB18030_RANGES[207] = { ... };

// encoding_gbk_validity.inc
static const uint8_t GBK_VALID_BITS[2993] = { ... };
```

为便于调试，首版也可以先提交未压缩的 `GB18030_TO_UNICODE[23940]`，
RLE 作为后续优化；无论哪种形式，GBK 都复用同一张规范表。

### 6.6 内存与性能

- 默认压缩方案的常量数据：约 32 KB；
- 运行时解码后的 GB18030 双字节表：约 48 KB；
- GBK 有效性位图：约 3 KB；
- 四字节范围表：约 2.5 KB；
- 运行时总计约 53 KB，与未压缩方案基本相同。
- RLE 解码在首次使用时执行一次，约 10351 段，成本可忽略。
- 反向映射建议在首次编码时惰性构建：
  - 可使用 `std::unordered_map<uint32_t, uint16_t>`，或
  - 建立 65536 项的 `uint16_t` 反向表（约 128 KB），换取更快编码。
- 首版优先使用 64K 直接反向表，行为简单、查找稳定；如内存敏感再改为哈希表。

## 7. 实现结构

### 7.1 新文件

```text
src/runtime/encoding.h
src/runtime/encoding.cpp
src/runtime/data/encoding_gb18030_data.inc
src/runtime/data/encoding_gbk_validity.inc
tools/gen_encoding_tables.py
```

`src/runtime/encoding.cpp` 负责：

- 编码名归一化与别名解析
- UTF-8 / ASCII / Latin-1 / Windows-1252 / UTF-16 编解码
- GBK / GB18030 编解码
- `strict` / `replace` / `ignore`
- 错误信息与偏移量
- 调用内置表

### 7.2 枚举与分发

在 `src/interpreter/ast.h` 的 `enum bifs` 末尾新增：

```cpp
B_encoding_decode,
B_encoding_encode,
B_encoding_supports
```

在 `src/builtins/builtins.cpp` 中：

- 引入 `src/runtime/encoding.h`
- 在 `call_builtin()` 中增加范围分发：

  ```cpp
  if (functype >= B_encoding_decode && functype <= B_encoding_supports)
      return call_encoding_builtin(functype, args);
  ```

`call_encoding_builtin()` 自行处理参数数量和类型，不依赖默认的单参数检查。

### 7.3 std 模块注册

在 `src/interpreter/package.cpp` 的 `build_std_namespace()` 中新增：

```cpp
root->payload->items.push_back(std::make_pair(
    newstrval("encoding"),
    builtin_module({
        {"decode", B_encoding_decode},
        {"encode", B_encoding_encode},
        {"supports", B_encoding_supports},
    })));
```

只通过 `std.encoding.*` 暴露，不新增全局函数名，因此不需要修改
`src/interpreter/tokens.l` / `src/interpreter/parser.y`。

首版已补充 `std.bytes.to_string` / `std.bytes.from_string`，在
`std.bytes` 模块中映射到现有枚举：

```cpp
{"to_string", B_bytes_to_string},
{"from_string", B_bytes_from_string},
```

### 7.4 CMake

在 `CMakeLists.txt` 的 `add_executable` 列表中加入：

```text
encoding.cpp
```

首版不需要新增依赖。

如果后续加入 L3 可选旧编码，再增加：

- `src/builtins/encoding_backend.cpp`
- `SKEIN_ENABLE_LEGACY_ENCODINGS` 选项
- POSIX `iconv` 检测
- Windows 代码页 API

这些不影响 GBK / GB18030。

### 7.5 后续可选后端

L3 旧编码建议通过统一的平台接口实现：

```cpp
bool legacy_decode(const std::string &input, const std::string &encoding,
                   const std::string &errors, std::string &output,
                   std::string &error);
bool legacy_encode(const std::string &input, const std::string &encoding,
                   const std::string &errors, std::string &output,
                   std::string &error);
```

- POSIX：`iconv`。
- Windows：`MultiByteToWideChar` / `WideCharToMultiByte`。
- 可用性通过 `std.encoding.supports` 暴露。
- 不支持时明确报错，不静默回退。

## 8. 测试

### 8.1 固定向量

GBK：

```text
中   D6 D0
国   B9 FA
你   C4 E3
好   BA C3
，   A3 AC
€    80
```

GB18030：

```text
中         D6 D0
€          A2 E3
U+0080     81 30 81 30
U+10000    90 30 81 30
😀         94 39 FC 36
```

### 8.2 错误与边界

- 非法首字节（GBK `0xFF`，GB18030 `0x80`）
- 非法尾字节
- 截断的双字节/四字节序列
- `strict` 报错
- `replace` 输出 U+FFFD
- `ignore` 跳过
- 编码不可表示字符（如把 emoji 编码到 ASCII）
- 解码结果含 `U+0000` 必须报错
- 未知编码名
- `encoding` 参数不是 string
- `decode` 输入不是 bytes
- `encode` 输入不是 string
- 参数个数错误
- `supports("gbk") == true`
- `supports("gb18030") == true`

### 8.3 穷举一致性

建议增加 C++ 单元测试，直接调用编解码核心函数：

1. 表关系：
   - GBK 有效位图置位数为 21791；
   - 每个 GBK 有效位置在 GB18030 表中都存在且码点一致；
   - GB18030 比 GBK 多出的位置数为 2149；
   - 两者映射冲突数为 0。
2. RLE 解码：
   - 解码压缩表，与未压缩规范表逐项比较；
   - 断言 10351 段、段长最大 115、范围表 207 段。
3. GBK：
   - 遍历 23940 个双字节位置。
   - 有效位置：`decode` 后再 `encode`，应回到原字节。
   - 无效位置：`strict` 必须报错。
4. GB18030 双字节：
   - 遍历 23940 个位置，验证解码/编码往返。
5. GB18030 四字节：
   - 遍历全部 1,587,600 个索引。
   - 有效索引：`encode(decode(seq))` 应生成等价的最短表示；
     对只能在四字节区表示的码点，应回到原序列。
   - 保留区间：`strict` 必须报错。
6. Unicode 全量：
   - 可选遍历所有 Unicode 标量值，验证 GB18030 编码后能解码回原码点。

如果 C++ 测试成本较高，至少保证固定向量、表关系断言和生成脚本的 `--check`。

### 8.4 集成测试

在 `tests/test_skein.py` 中增加 `check(...)` 用例：

```python
check(
    "gbk roundtrip",
    'import std;\n'
    'let s = std.encoding.decode(bytes_from_hex("d6d0b9fa"), "gbk");\n'
    'print(s);\n'
    'print(bytes_hex(std.encoding.encode(s, "gbk")));\n',
    stdout_all=["= 中国", "= d6d0b9fa"],
)
```

GB18030 用例类似，并测试 `std.encoding.supports("gb18030")`。

## 9. 文档更新

实现阶段需要同步更新：

- `README.md`
  - 模块列表加入 `std.encoding`
  - 函数列表加入 `std.encoding.decode` / `encode` / `supports`
  - 新增“编码转换”章节：支持的编码、错误策略、NUL 限制、示例
- `SYNTAX.md`
  - 对应的语法/标准库条目
- 本文件
  - 实现完成后把状态改为“已实现”，并记录实际行为差异

## 10. 实施步骤

1. 编写 `tools/gen_encoding_tables.py`，生成 GB18030 规范表、GBK 有效性位图和
   四字节范围表；断言 GBK 是 GB18030 的子集且映射一致。
2. 交叉验证固定向量与 iconv / Python codec。
3. 实现 `src/runtime/encoding.h` / `src/runtime/encoding.cpp` 的核心编码、GBK/GB18030 和 RLE 解码。
4. 增加 `src/interpreter/ast.h` 枚举、`src/builtins/builtins.cpp` 分发、`src/interpreter/package.cpp` 模块注册。
5. 更新 `CMakeLists.txt`。
6. 增加 C++ 穷举测试与 Python 集成测试。
7. 更新 `README.md` / `SYNTAX.md`。
8. 运行完整测试，检查 Linux 与 MinGW 构建。
9. 评估是否顺带加入 `std.bytes.to_string` / `from_string` 别名。

## 11. 验收标准

- `std.encoding.decode` / `encode` / `supports` 可用。
- GBK、GB18030 固定向量全部通过。
- GBK / GB18030 穷举往返测试通过。
- GBK 有效性位图与 GB18030 规范表的子集关系通过断言。
- RLE 解码结果与未压缩规范表逐项一致。
- 非法输入在 `strict` 下报错，在 `replace` / `ignore` 下行为确定。
- 解码结果含 NUL 时始终报错。
- `bytes_to_string` 行为不变。
- 不新增强制第三方依赖。
- Linux 与 MinGW 构建通过，现有测试无回归。
- 文档与实现一致。

## 12. 风险与后续

### 风险

1. **GB18030 版本差异**
   - 首版固定 GB18030-2005；如果目标环境要求 GB18030-2022，需要替换表数据并补充差异测试。
2. **码表体积与压缩**
   - 共享 GBK 表后，未压缩约 52 KB；RLE 后常量数据约 32 KB。
   - RLE 只降低二进制体积，运行时仍会解码成约 48 KB 的表。
   - 需要测试锁定 RLE 解码正确性，避免生成数据与解码逻辑不一致。
3. **非法序列消费规则**
   - 不同库对替换/跳过的粒度不同；本方案在文档中固定规则，并用测试锁定。
4. **数据来源可复现性**
   - 生成脚本依赖 Python codec 版本；生成文件提交仓库，并在头部记录版本，避免隐式变化。
5. **NUL 限制**
   - 无法解码包含 U+0000 的文本；这是现有 string 表示的硬约束。

### 后续

- 增加 GB18030-2022 表数据。
- 增加 Big5、Shift_JIS、EUC-JP 等可选后端。
- 评估 `std.encoding.detect`（BOM / 常见编码探测）。
- 评估 `std.encoding.convert(bytes, from, to)` 直接转换接口。
- 如内存敏感，把 65536 项直接反向表改为哈希表。

## 13. 首版实现记录

首版已实现：

- `src/runtime/encoding.h` / `src/runtime/encoding.cpp`
- `tools/gen_encoding_tables.py`
- `src/runtime/data/encoding_gb18030_data.inc`
- `src/runtime/data/encoding_gbk_validity.inc`
- `std.encoding.decode` / `encode` / `supports`
- `std.bytes.to_string` / `from_string` 严格 UTF-8 别名
- `tests/test_skein.py` 中的 GBK / GB18030 / UTF-16 / 错误模式测试
- README / SYNTAX 文档更新

实际实现与设计一致：

- GB18030 双字节表使用 10351 段 RLE，四字节范围表 207 段；
- GBK 复用 GB18030 表 + 23940 bit 有效性位图；
- GBK `0x80` 映射为 `€`；
- GB18030 的 `€` 为 `A2 E3`；
- 反向编码表使用 65536 项直接表；
- 普通构建不需要 iconv 或 Windows 代码页 API。

已知未覆盖项：

- GB18030-2022 差异映射；
- Big5 / Shift_JIS / EUC-JP 等其它旧编码；
- 内嵌 NUL 的 `string` 表示。
