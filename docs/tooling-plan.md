# Skein 工具链实现计划：lint / format / LSP

> 状态：阶段 0–4 已实现：`lint`、`fmt`、`lsp --stdio` 均可用，VS Code / Vim / Neovim 接入示例与文档已完成。总路线见 [`roadmap.md`](roadmap.md)。
> 适用范围：在现有 `skein` CLI 基础上新增 `lint`、`fmt`、`lsp` 三个子命令，并让 Vim / VS Code 通过 LSP 调用。  
> 实现偏差：`diag_collect.*` 虽位于 `src/interpreter/`，但归属 `skein_value_runtime` target，以满足架构边界检查。

---

## 1. 背景与现状

仓库原本**没有**独立的 format、lint 或 LSP 服务；目前已实现 `skein lint` 与 `skein fmt`：

- 新增 `skein lint [--format=text|json] [--typecheck=...] [-k] <file|->`，check-only 模式跳过副作用语句、保留声明/导入求值、默认 keep-going，并输出结构化诊断。
- 新增 `skein fmt [--check|-w|--write] <file...|->`，基于 token/trivia 保留原始字面量与注释，输出前做 token 序列等价校验与 check-only parse 校验。
- 新增 `skein lsp [--stdio] [--typecheck=...] [-I DIR]`，以 stdio JSON-RPC 提供诊断、格式化、补全、悬停、定义与文档符号；整个服务复用 check-only 分析，不执行用户代码。

实施前的状态（保留作为背景）：

- `README.md`、`editors/vscode/README.md`、`editors/vim/README.md` 原本都说明编辑器插件不包含 LSP、语义补全或诊断；现已更新。
- `src/app/main.cpp` 原本的 usage 与参数解析中没有工具子命令；现已改为子命令早分发。
- `--run-entry`（`main.cpp:225-226,321-346`）是进程 IPC，不是语言服务。
- `--check-ffi`、`--ffi-gen`（`main.cpp:88-117`）是已有的“检查/生成型子命令”先例，可作为早分发参考。
- 仓库中没有源码 unparser / pretty-printer；`src/runtime/format.*` 是运行时字符串格式化，与源码格式化无关。

已有可复用能力：

| 能力 | 位置 |
|---|---|
| Flex/Bison lexer 与 parser | `src/interpreter/tokens.l`、`src/interpreter/parser.y` |
| 静态类型检查 | `src/interpreter/typecheck.h/.cpp`，`check_statement()` |
| 位置感知诊断 | `src/interpreter/diagnostics.h/.cpp` |
| AST 与释放 | `src/interpreter/ast.h/.cpp`、`treefree()` |
| 包/模块解析 | `src/interpreter/package.h/.cpp` |
| 源码编码处理 | `src/interpreter/source_encoding.h/.cpp` |
| std 命名空间 | `src/interpreter/std_namespace.h/.cpp` |
| 符号与类型表 | `scopestack.h`、`symbol.h`、`structtype.h` |
| JSON 解析 | `src/runtime/json.h` 的 `json_parse_text()` |
| 分层构建 | `docs/architecture.md`、`CMakeLists.txt` |

---

## 2. 总体设计

### 2.1 CLI

新增三个子命令，与现有运行模式并存：

```bash
skein lint [--typecheck=off|loose|warn|strict] [-k] [--format=text|json] <file|->
skein fmt  [--check] [-w|--write] <file...|->
skein lsp  [--stdio] [--typecheck=...] [-I DIR ...]
```

采用**子命令早分发**，而不是新增 `--check` 这类全局 flag。原因：`--watch` 的子进程重建会复制 script path 之前的所有参数（`main.cpp:291-314`），全局 flag 会被 watch 子进程继承；子命令在 `main.cpp:88-117` 阶段处理，可完全绕开 watch 参数重建。

### 2.2 设计原则

1. **lint 与 lsp 不执行用户代码**：只 parse + typecheck，不运行脚本。
2. **复用现有前端**：不重写 parser，不重写类型系统。
3. **保持 L4 编排**：CLI/LSP 入口放 `src/app/tools/`，分析/格式化库放 `src/interpreter/`，继续遵守 `docs/architecture.md` 的分层。
4. **先能力后协议**：先做 `lint`/`fmt`，再把同一套能力接到 LSP。
5. **诊断结构化**：新增 collector，让文本输出、JSON 输出和 LSP 共享同一份诊断数据。

### 2.3 退出码

沿用当前 CLI 约定：

- `0`：无错误 / 正常完成
- `1`：有诊断错误或运行错误（`diag_error_count() > 0`，`main.cpp:435-436`）
- `2`：参数错误、文件读取/编码失败（`main.cpp` 多处）

warning 不改变退出码，因为 `diag_report_warning()` 不增加 `error_count`。

---

## 3. 阶段 0：工具层与诊断基础设施

### 3.1 CLI 分发

修改 `src/app/main.cpp`：

- 在通用参数循环（`main.cpp:144`）之前识别 `argv[1]` 为 `lint` / `fmt` / `lsp`。
- 参考 `--check-ffi`（`main.cpp:89-106`）和 `--ffi-gen`（`main.cpp:107-116`）的早分发模式。
- 更新 `print_usage()`（`main.cpp:59-69`），列出新子命令。
- 保持旧用法兼容：第一个非选项仍是脚本路径；仅精确匹配子命令时进入工具模式。

新增文件：

```text
src/app/tools/tool_common.h/.cpp    // 源文件读取、退出码、参数校验
src/app/tools/lint_tool.cpp
src/app/tools/format_tool.cpp
src/app/tools/lsp_tool.cpp
```

`CMakeLists.txt:423` 附近的 `add_executable(skein ...)` 增加上述源文件。

### 3.2 结构化诊断

当前 `diagnostics.h` 已有 1-based line/column 与 `diag_capture/restore`，但 `diag_report()` 直接打印文本。

新增：

```text
src/interpreter/diag_collect.h/.cpp
```

定义：

```cpp
enum class DiagnosticSeverity { Error, Warning, Information, Hint };

struct diagnostic_record {
    std::string source;
    int line = 0;
    int column = 0;       // Unicode display column，与现有诊断一致
    std::size_t byte_offset = 0;   // 供 JSON/LSP 使用
    std::size_t utf16_offset = 0;  // 供 LSP 使用
    DiagnosticSeverity severity;
    std::string code;     // 例如 PAR0001、TYP0002、LNT0001
    std::string category;
    std::string message;
};
```

改造 `diag_report()` / `diag_report_warning()`：在打印前同时写入 thread-local collector。默认 CLI 文本输出保持不变；`lint --format=json` 与 LSP 从 collector 读取。

> 过渡期可按 category 映射稳定 code，后续再为各检查点显式编号。

注意：`diag_scan_token()`（`diagnostics.cpp:42-52`）的 column 使用 `unicode_ident::display_width`，**不是字节偏移**。因此 collector 中应基于源码行文本额外计算 `byte_offset` / `utf16_offset`，LSP 不要直接把 display column 当 UTF-16 offset。

---

## 4. 阶段 1：实现 `skein lint`

### 4.1 Check-only 解析模式

关键接入点是 `top_statement`（`parser.y:2168-2252`）：

```cpp
// parser.y:2189-2191
if (!g_statement_invalid && $1 &&
    !skein::typecheck::check_statement($1))
    g_statement_invalid = true;

// parser.y:2195-2199
bool skip_eval = invalid || sys_exit_requested() ||
                 (!g_interactive && g_script_stop);
```

新增 analysis-mode 标志：

```text
src/interpreter/analysis_mode.h/.cpp
```

MVP 逻辑：

```cpp
bool skip_eval = invalid || sys_exit_requested() ||
                 (!g_interactive && g_script_stop) ||
                 (g_check_only && !ast_is_declaration($1));
```

不能直接跳过所有 eval。原因：

- `import` / `include` / `package` / `struct` / `enum` / `trait` 等语句在 `eval.cpp:73-177` 中加载包、发布命名空间、注册类型。
- typechecker 虽然记录 import alias（`typecheck.cpp:2569-2576`）与 exported types，但纯跳过 eval 会导致符号/类型表不完整。

因此：

1. check-only 时跳过“运行时副作用语句”，保留声明/导入类语句的 eval，以建立类型与符号表。
2. 把 check-only 标志传播到 `parse_package_source()`（`package.cpp:1451-1525`）等嵌套解析路径，避免被导入包中的顶层副作用代码被执行。
3. 后续增加 resolution-only traversal，使用 `package_import_cache_key()`、manifest 加载和静态类型表直接填充 typechecker 状态，实现完全无副作用的 import 校验。

`ast_is_declaration()` 加入 `src/interpreter/ast_analysis.cpp`（已有 `collect_referenced_names()` 等 helper 可复用）。

### 4.2 Keep-going 默认值

`diag_mark_script_failure()`（`diagnostics.cpp:189-192`）在没有 `-k` 时会设置 `g_script_stop`，导致只报第一个错误。

lint 模式应在入口自动设置 `g_keep_going = true`，以收集全部诊断；最终仍按 `diag_error_count() > 0` 返回 1。若需要遇错即停，再提供 `--no-keep-going`。

### 4.3 typecheck 状态隔离

这些状态以 `diag_current_source_name()` 为 key 持久化：

- `g_persistent_bindings`
- `g_import_aliases`
- `g_exported_types`

位置：`typecheck_state.h:86-105`，使用点 `typecheck.cpp:3001-3010`。

LSP 会反复分析同一 URI 的不同版本，lint 也可能一次处理多文件，因此需要新增：

```cpp
void typecheck_forget_source(const std::string &source_name);
```

在每次 per-file/per-version 分析前清理，避免旧绑定/别名污染新诊断。

### 4.4 lint 规则

第一版直接复用：

- parse 语法错误
- typecheck 错误/警告（`--typecheck=strict`）
- 基础 AST 规则：
  - 未使用的 import / 局部变量
  - 不可达语句
  - 重复字段 / 变体
  - 空语句（可选）

规则注册表：

```text
src/interpreter/lint_rules.h/.cpp
```

```cpp
struct lint_rule {
    const char *id;
    DiagnosticSeverity default_severity;
    bool enabled_by_default;
    void (*check)(const ast *root, lint_context &ctx);
};
```

### 4.5 输出

```bash
skein lint file.skein                       # 文本诊断
skein lint --format=json file.skein         # JSON diagnostics
skein lint --typecheck=strict -k file.skein # 继续收集多个错误
```

JSON 示例：

```json
{
  "diagnostics": [
    {
      "file": "demo.skein",
      "line": 3,
      "column": 7,
      "byteOffset": 42,
      "utf16Offset": 40,
      "severity": "error",
      "code": "TYP0002",
      "message": "expected int, got string"
    }
  ]
}
```

### 4.6 验收

```bash
skein lint tests/cases/lint/type_error.skein --typecheck=strict
# 有错误 -> exit 1，且不产生任何脚本输出

skein lint tests/cases/lint/ok.skein --typecheck=strict
# 干净 -> exit 0

skein lint --format=json tests/cases/lint/type_error.skein | python -m json.tool
# JSON 合法
```

新测试模块：`tests/cases/lint.py`，注册进 `tests/test_skein.py:32-51` 的 `MODULES`。

---

## 5. 阶段 2：实现 `skein fmt`

### 5.1 必须基于 token/trivia，而不是 AST

现有 lexer 会丢掉 formatter 需要的 trivia：

- shebang：`tokens.l:591`
- `//` 注释：`tokens.l:1051`
- 水平空白：`tokens.l:1052`
- 括号内换行：`tokens.l:1140-1145`

AST 会进一步丢掉：

- 原始字面量拼写：字符串已解码、数字下划线被剥离、非 ASCII 标识符已 NFC 归一化。
- original syntax form：slice、`|>`、`$` 占位符、`->` lambda、decorator、`with` 等都会被 lowering。
- 没有任何 unparser / pretty-printer。

因此采用**独立、保留 trivia 的 tokenizer**，从 `tokens.l:573-587` 的 Flex pattern 与 `parser.y:893-929` 的 token 定义派生。每个 token 记录：

```text
kind + raw lexeme + 起止位置 + 前置 trivia（空白/注释/换行/shebang）
```

### 5.2 Formatter 流程

新增：

```text
src/interpreter/formatter.h/.cpp
src/interpreter/fmt_tokens.l
```

流程：

1. 用 `load_source_file()`（`source_encoding.cpp:129-136`）读取并解码源码。
2. trivia-preserving scanner 输出 token 流；必须在 `yyless()` 与解码前记录 raw lexeme。
3. 按规范输出：
   - 2 空格缩进（与 `editors/vim/indent/skein.vim:13-32`、`language-configuration.json:24-26`、`lib/std/*.skein` 一致）
   - header 与 `{` 同行
   - `}` 与 `else` 同行
   - `do ... while` 的 `while` 与 `}` 同行
   - 操作符、逗号、分号周围的空格规则
   - 保留字符串/字符字面量与注释内容
   - 最多一个连续空行
4. 安全校验：
   - 对输入、输出各跑一次 trivia-preserving tokenizer；
   - 比较 `(token_kind, raw_lexeme)` 序列是否完全一致，保证只改 trivia；
   - 用阶段 1 的 check-only parse 验证格式化结果语法合法。

MVP 不需要 `ast_fingerprint()`；token 序列比对更简单可靠。

### 5.3 必须遵守的语法约束

- **不插入/删除分号**，不把 token 移过顶层换行；顶层 newline 是语句终止符。
- `()[]{}` 内部换行可忽略，但 formatter 仍要遵守：
  - header + `{` 同行（`SYNTAX.md:1182`，`tokens.l:673-675`）
  - `}` + `else` 同行（`tokens.l:759-763`）
  - `do...while` 的 `}` + `while` 同行
- `{}` 在表达式位置是空 dict、在 body 位置是空 block；注释-only 的 `{}` 也要保留（`SYNTAX.md:198-213`）。
- 只有 `//` 行注释；字符串是单行，不能 reflow（`tokens.l:1062-1068`）。
- 保留原始字面量与标识符字节，不要重新解码/归一化。

### 5.4 编码策略

`load_source_file()` 解码为 UTF-8；如需保持原编码可用 `encode_text()`（`runtime/encoding.h:16-18`）。需在 `fmt` 文档中明确 BOM 与 CRLF/LF 输出策略，测试统一归一化 CRLF（`tests/harness.py:60-63`）。

### 5.5 CLI

```bash
skein fmt file.skein            # 输出到 stdout
skein fmt -w file.skein         # 原地写回
skein fmt --check file.skein    # 仅检查；有差异 exit 1
skein fmt - < input.skein       # stdin -> stdout
```

### 5.6 验收

- golden 测试：`tests/cases/fmt/*.input` → `*.expected`
- 幂等性：`fmt(fmt(x)) == fmt(x)`
- 注释、字符串、原始字面量不变
- 格式化后 `skein lint` 仍干净
- `--check` 在未格式化文件上 exit 1、已格式化文件上 exit 0
- Windows CRLF 归一化测试

---

## 6. 阶段 3：实现 `skein lsp --stdio`

### 6.1 JSON-RPC 传输

新增：

```text
src/app/tools/lsp/json_rpc.h/.cpp
```

- LSP 标准帧：`Content-Length: N\r\n\r\n<json>`
- 请求解析用现有 `json_parse_text()`（`src/runtime/json.h:14`）
- 序列化：`JsonEncoder` 当前是 `json.cpp:469` 的内部类，需新增公开封装：

```cpp
std::string json_stringify_text(const value_t *value, std::string &error);
```

Windows：

- `platform_win.cpp:74-82` 已把 pipe 模式 stdin 设为 binary；
- 参照 `main.cpp:326-328` 为 stdout 也设置 `_O_BINARY`；
- 写响应时持有 `stdio_output_mutex()`，避免 worker 输出插入协议流；
- 日志只能写 stderr，绝不能写 stdout。

### 6.2 服务器状态

新增：

```text
src/app/tools/lsp/lsp_server.h/.cpp
src/app/tools/lsp/document_store.h/.cpp
src/app/tools/lsp/position.h/.cpp
```

最小 capabilities：

```text
textDocumentSync: Full
publishDiagnosticsProvider
documentFormattingProvider
documentRangeFormattingProvider
completionProvider
hoverProvider
definitionProvider
documentSymbolProvider
```

文档模型：

```cpp
struct lsp_document {
    std::string uri;
    int version;
    std::string text;
    std::vector<diagnostic_record> diagnostics;
};
```

首版用 Full sync，后续再加 incremental sync 与 debounce。

### 6.3 diagnostics 与 formatting

- `didOpen` / `didChange` / `didSave`：调用阶段 1 的 in-process lint，生成 diagnostics，发 `textDocument/publishDiagnostics`。
- `textDocument/formatting`：调用阶段 2 的 formatter。
- 位置转换：诊断 column 是 Unicode display width，不是 UTF-16 offset；使用 collector 中的 `utf16_offset`，或由 `position.cpp` 基于源码行文本换算。

### 6.4 补全数据源

按优先级：

1. 关键字：来自 `parser.hpp` / `tokens.l` token 名。
2. 当前作用域符号：`scopestack.h` 的 `symbol_table`、`lookupsym()`、`global_symbol_slot()`。
3. std/native builtins：
   - `stdlib::build_namespace()`（`std_namespace.h:19`）返回权威 `std.*` 命名空间字典；
   - `builtin_ids.h` + 新增 id→canonical name 表；
   - `typecheck_builtins.h:41,48` 提供签名信息用于 hover/signature help。
4. 用户包导出：`package_build_namespace()`（`package.h:56`）。
5. 类型成员：`structtype.h` 的 `structtype_lookup_type()`、`field_def`、`variant_def`、`method_sig`；当前没有“列出所有类型”的 API，需补一个。
6. 引用/未解析名：`ast_analysis::collect_referenced_names()` 辅助判断补全上下文。

补全需要在 check-only parse 时构建独立 `analysis_scope`，不要依赖运行时 eval 后的全局 scope，避免 LSP 修改或依赖执行状态。

### 6.5 LSP 绝不执行用户代码

禁止调用：

- `skein_vm_eval_statement`
- `package_import` 的执行路径（必须走 check-only / resolution-only 变体）
- 任何 builtin I/O / process / network

### 6.6 验收

脚本化会话测试位于 `tests/cases/lsp.py`，注册在 `tests/test_skein.py` 的 `MODULES` 中。

覆盖：

```text
initialize → initialized → didOpen → publishDiagnostics
textDocument/formatting → 返回格式化文本
textDocument/completion → 返回 locals + std.* 项
textDocument/hover → 返回类型签名
textDocument/documentSymbol / definition → 返回文档内符号位置
didChange → 重新分析（ContentChangeEvent 对象中的 text）
shutdown → exit
```

测试同时断言被分析的源码中的 `println(...)` 不会执行，且退出码为 0。

---

## 7. 阶段 4：编辑器、文档与 CI

> 实现结果：VS Code 扩展已接入 `vscode-languageclient`，Vim/Neovim README 提供
> `vim.lsp` / coc.nvim / vim-lsp 示例，`docs/lsp.md` 汇总协议与配置，CTest
> 增加 `lint_tests`、`format_tests`、`lsp_tests`，`check_fast.sh --tools`
> 可单独运行工具链测试。

### 7.1 VS Code

修改 `editors/vscode/package.json`、`extension.js`：

- 增加依赖 `vscode-languageclient`
- 复用现有 `executablePath()`（`extension.js:10-24`）
- server 配置：

```js
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

const serverOptions = {
  command: executablePath(),
  args: ['lsp', '--stdio'],
  transport: TransportKind.stdio,
};

const clientOptions = {
  documentSelector: [{ scheme: 'file', language: 'skein' }],
  synchronize: { configurationSection: 'skein' },
};

const client = new LanguageClient(
  'skein',
  'Skein Language Server',
  serverOptions,
  clientOptions
);

context.subscriptions.push(client.start());
```

- 增加命令 `skein.formatDocument`
- 增加配置 `skein.typecheck`、formatter 选项
- 更新 `editors/vscode/README.md`，删除“不包含 LSP/诊断”的说明

### 7.2 Vim / Neovim

Vim 本身无内置 LSP，需要插件；Neovim 0.5+ 可用内置 `vim.lsp`。

Neovim 示例：

```lua
vim.api.nvim_create_autocmd('FileType', {
  pattern = 'skein',
  callback = function()
    vim.lsp.start({
      name = 'skein',
      cmd = { 'skein', 'lsp', '--stdio' },
      root_dir = vim.fn.getcwd(),
    })
  end,
})
```

coc.nvim 示例：

```json
{
  "languageserver": {
    "skein": {
      "command": "skein",
      "args": ["lsp", "--stdio"],
      "filetypes": ["skein"],
      "rootPatterns": [".skeinroot", ".git"]
    }
  }
}
```

vim-lsp 示例：

```vim
augroup LspSkein
  autocmd!
  autocmd User lsp_setup call lsp#register_server({
        \ 'name': 'skein',
        \ 'cmd': {server_info -> ['skein', 'lsp', '--stdio']},
        \ 'whitelist': ['skein'],
        \ })
augroup END
```

更新 `editors/vim/README.md`，保留原 syntax/indent/run/repl 行为，增加 LSP 配置示例。

### 7.3 文档

- `README.md:33` 删除“不包含 LSP、语义补全或诊断”，改为工具链说明。
- 新增本文档 `docs/tooling-plan.md`。
- 实现完成后可新增 `docs/lsp.md` 或把使用说明并入 `docs/tutorial.md`。
- `docs/architecture.md` 更新 L4 文件归属。
- `SYNTAX.md` 增加 canonical formatting style。
- 三个 editor README 同步更新。

### 7.4 CI / CTest

- 新增 `lint_tests`、`format_tests`、`lsp_tests` CTest 条目。
- 加入 `tools/check_fast.sh`、`tools/check_all.sh`。
- 覆盖 Windows / Linux、VM=0/1、sanitizer。
- 更新 `tools/check_boundaries.py` 相关检查（若新增源文件到现有 target 通常无需改允许列表）。

---

## 8. 文件改动清单

| 路径 | 作用 |
|---|---|
| `src/app/main.cpp` | 子命令早分发、usage、LSP 二进制 stdio |
| `src/app/tools/tool_common.*` | 源文件读取、退出码、参数校验 |
| `src/app/tools/lint_tool.cpp` | lint CLI |
| `src/app/tools/format_tool.cpp` | fmt CLI |
| `src/app/tools/lsp/*` | JSON-RPC、文档模型、LSP server |
| `src/interpreter/analysis_mode.*` | check-only 标志与嵌套传播 |
| `src/interpreter/diag_collect.*` | 结构化诊断 |
| `src/interpreter/lint_rules.*` | lint 规则注册表 |
| `src/interpreter/ast_analysis.*` | `ast_is_declaration()`、引用收集复用 |
| `src/interpreter/typecheck_state.*` | `typecheck_forget_source()` |
| `src/interpreter/formatter.*`、`fmt_tokens.l` | formatter 与 trivia tokenizer |
| `src/runtime/json.h/.cpp` | 暴露 `json_stringify_text()` |
| `CMakeLists.txt` | 新源文件、target、测试 |
| `tests/cases/lint.py`、`fmt.py`、`lsp.py` | 测试模块 |
| `tests/test_skein.py` | 注册测试模块 |
| `editors/vscode/*`、`editors/vim/*`（含 `lua/skein/lsp.lua`） | 编辑器接入 |
| `README.md`、`docs/lsp.md`、`docs/architecture.md`、`docs/tooling-plan.md` | 文档 |
| `CMakeLists.txt`、`tools/check_fast.sh`、`tools/check_all.sh` | CTest 条目与检查脚本 |

---

## 9. 风险与规避

| 风险 | 规避 |
|---|---|
| parse 与 eval 交织，no-run 改造影响类型解析 | check-only 保留声明性 eval，只跳过副作用语句 |
| 被导入包执行顶层代码 | check-only 标志传播进 `parse_package_source()` 等嵌套解析 |
| typecheck 持久状态污染 | 新增 `typecheck_forget_source()`，每次分析前清理 |
| 诊断列是 display width 而非字节偏移 | collector 额外记录 byte/utf16 offset |
| 诊断无 code/structured sink | 先加 collector，category→code 映射过渡 |
| AST 无 span 且会 lowering | formatter 用 token/trivia 流，不用 AST |
| formatter 改坏语义 | 比较输入/输出的 `(kind, raw_lexeme)` token 序列，并做 check-only parse |
| formatter 破坏 same-line 规则 | 显式处理 header+`{`、 `}`+`else`、 `}`+`while` |
| LSP 全局状态污染 | 单线程请求处理 + `diag_save/restore`；analysis 使用独立 scope |
| Windows stdio CRLF/0x1A | stdin/stdout 均设 `_O_BINARY`，测试统一归一化 CRLF |
| 补全数据源分散 | 建 `completion_index` 聚合 std namespace、scope、package、struct 表 |
| watch 继承新 flag | 使用子命令而非全局 flag |

---

## 10. 推荐交付顺序

1. **阶段 0：工具层 + 结构化诊断**
2. **阶段 1：`skein lint` MVP**
   - check-only、keep-going、source 状态隔离、JSON diagnostics、测试
3. **阶段 2：`skein fmt --check/-w`**
   - trivia tokenizer、golden、幂等、token 序列校验
4. **阶段 3：`skein lsp --stdio`**
   - diagnostics + formatting + 基础 completion/hover
5. **阶段 4：编辑器与文档**
   - VS Code `vscode-languageclient`、Vim/Neovim 配置示例、CI

粗略工作量（1 人）：阶段 0+1 约 1–2 周，阶段 2 约 1–2 周，阶段 3 约 2–3 周，阶段 4 约数天到一周。
