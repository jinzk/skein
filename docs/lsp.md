# Skein 语言服务器（LSP）

`skein lsp --stdio` 是基于标准 LSP over stdio 的语言服务器，供 VS Code、Neovim、
coc.nvim、vim-lsp 等客户端使用。它复用 `skein lint` 的 check-only 前端分析，
**不会运行被编辑的脚本**：不进入 VM、不执行普通表达式语句、不触发 I/O / 进程 /
网络 builtin。导入和声明只用于建立符号与类型表，导入包中的运行时副作用语句同样会被跳过。

## 启动

```bash
skein lsp --stdio
skein lsp --stdio --typecheck=strict
skein lsp --stdio -I ./packages --encoding utf-8
```

支持的参数：

| 参数 | 说明 |
|---|---|
| `--stdio` / `--server` | 使用 stdio 传输；省略时也默认使用 stdio |
| `--typecheck=off\|loose\|warn\|strict` | 与 `skein lint` 相同的检查强度，默认 `warn` |
| `-I DIR` / `--package-path DIR` | 追加包搜索路径，可重复 |
| `--encoding NAME` | 读取 `didOpen`/`didChange` 文本时使用的编码，默认 UTF-8 |

协议帧使用 LSP 标准格式：`Content-Length: N\r\n\r\n<JSON>`。所有日志只写 stderr，
stdout 仅包含协议消息。Windows 下 stdin/stdout 都设为二进制模式，写入由全局
输出锁序列化。EOF 会正常退出；收到 `exit` 时，若之前已收到 `shutdown` 则返回 0，
否则返回 1。

`didChange` 支持增量 range 替换，并在最后一次修改后等待
`SKEIN_LSP_DEBOUNCE_MS` 毫秒再分析，默认 75，最大 5000。收到补全、hover 等请求
时会先刷新待处理的分析，避免返回过期符号。

check-only 模式下，外部 module import 会按 `skein.mod` 做 resolution-only 校验；
如果 module 已缓存，还会读取 `module.skein` / 导出包结构，但始终不会下载或执行
module 代码。

## 能力

`initialize` 返回的能力：

| 能力 | 说明 |
|---|---|
| `textDocumentSync` | `openClose: true`、`change: 2`（Incremental）、`save: true` |
| `publishDiagnosticsProvider` | `didOpen` / `didChange` / `didSave` 后发布诊断；`didChange` 使用 debounce |
| `documentFormattingProvider` | 复用 `skein fmt` 的 token/trivia formatter，输出前做 token 等价与 parse 校验 |
| `documentRangeFormattingProvider` | 与整文档格式化相同（MVP 忽略 range，返回整文档替换） |
| `completionProvider` | 触发字符 `.`；提供关键字、当前可见符号、selective import 别名、`std.*` 命名空间成员，以及工作区 module 的导出符号（含 `as` 别名） |
| `hoverProvider` | 返回关键字说明、局部变量类型、selective import 的静态解析结果、`std.*` 签名或导入模块的导出符号签名 |
| `definitionProvider` | 先在当前文档内按声明名跳转；selective import 别名按原导出路径解析到工作区 module 的声明（跨文件） |
| `documentSymbolProvider` | `func` / `let` / `const` / `struct` / `enum` / `proto` / `impl` 等顶层声明；struct 内联方法和 `impl` 方法是类型成员，不另列为顶层符号 |
| `workspace.workspaceFolders` | 声明支持多根工作区；文件夹与 `-I` 路径共同构成索引根 |

诊断字段映射：`source` 固定为 `skein`，`code` 为 `PAR####` / `TYP####` /
`LNT####` 等稳定分类，`severity` 为 LSP 的 1–4。位置使用 0-based line 和
UTF-16 code unit character；内部来自结构化诊断的 `utf16_offset`，不把显示列宽
当作 character 偏移。

### 工作区符号索引

服务器在收到 `initialized` 后，一次性扫描索引根并建立 module 导出符号表；
索引根是客户端 workspace folders 与命令行 `-I` / `--package-path` 路径的并集。
扫描只读取 `module.skein` 清单及其导出包文件，并复用 check-only 前端分析，
**不执行任何被索引脚本**。索引结果用于：

- `textDocument/completion`：`math.` 列出 `math` module 的导出名；顶层补全
  也会提示 module 名；`import x as y` 的别名按原路径解析；
  `import {name as n} from math;` 的 `n` 按 `math.name` 解析。
- `textDocument/definition`：按光标所在词的 import 限定符解析到导出声明所在
  文件与行列，实现跨文件跳转；`import {name}` 的别名走同一条静态路径；本地
  同名声明优先。
- `textDocument/hover`：显示导入模块的导出符号签名，包括 selective import
  别名；全程只读取索引，不执行被导入脚本。

索引只在启动时构建一次；运行期文件变化不会重建索引，需重启语言服务器刷新。

## VS Code

`editors/vscode` 已依赖 `vscode-languageclient`，激活时自动启动：

```js
const serverOptions = {
  run: {
    command: executablePath(),
    args: ['lsp', '--stdio', '--typecheck=warn'],
    transport: TransportKind.stdio,
  },
};
```

相关设置：

- `skein.lsp.enabled`：是否启动语言服务器；
- `skein.executablePath`：解释器路径，相对带斜杠的路径从工作区根解析；
- `skein.typecheck`：传给服务器的检查强度；
- `skein.packagePaths`：每项以 `-I` 传给服务器。

命令 `Skein: Format Document` 调用标准 `editor.action.formatDocument`，也可使用
`Shift+Alt+F` 或 `"editor.formatOnSave": true`。

## Neovim 0.5+

插件自带 `editors/vim/lua/skein/lsp.lua`：

```lua
require('skein.lsp').setup({
  cmd = { 'skein', 'lsp', '--stdio', '--typecheck=strict' },
})
```

它会在 `FileType skein` 时调用 `vim.lsp.start()`，默认命令为
`{ g:skein_executable or 'skein', 'lsp', '--stdio' }`，`root_dir` 默认当前工作目录。
也可直接自行注册 autocmd，见 `editors/vim/README.md`。

## coc.nvim / vim-lsp

coc.nvim 的 `coc-settings.json`：

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

vim-lsp 的 `vimrc`：

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

## 已知限制

- 只实现了 Full document sync；频繁保存时每次都会重新分析整个文档；
- `rangeFormatting` 返回整个文档的替换编辑；
- 诊断在每次文档变化时立即发布，没有 debounce；大文件可能需要客户端侧节流。

实现与配置见本文，后续路线见 [`roadmap.md`](roadmap.md)，会话测试见 `tests/cases/lsp.py`。
