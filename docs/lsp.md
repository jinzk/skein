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

## 能力

`initialize` 返回的能力：

| 能力 | 说明 |
|---|---|
| `textDocumentSync` | `openClose: true`、`change: 1`（Full）、`save: true` |
| `publishDiagnosticsProvider` | `didOpen` / `didChange` / `didSave` 后发布诊断 |
| `documentFormattingProvider` | 复用 `skein fmt` 的 token/trivia formatter，输出前做 token 等价与 parse 校验 |
| `documentRangeFormattingProvider` | 与整文档格式化相同（MVP 忽略 range，返回整文档替换） |
| `completionProvider` | 触发字符 `.`；提供关键字、当前可见符号、`std.*` 命名空间成员 |
| `hoverProvider` | 返回关键字说明、局部变量类型或 `std.*` 签名 |
| `definitionProvider` | 在当前文档内按声明名跳转 |
| `documentSymbolProvider` | `func` / `let` / `const` / `struct` / `enum` / `trait` / `impl` 等顶层声明 |

诊断字段映射：`source` 固定为 `skein`，`code` 为 `PAR####` / `TYP####` /
`LNT####` 等稳定分类，`severity` 为 LSP 的 1–4。位置使用 0-based line 和
UTF-16 code unit character；内部来自结构化诊断的 `utf16_offset`，不把显示列宽
当作 character 偏移。

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
- 补全来自当前文档可见符号和 `std.*` 命名空间，不含全项目索引或用户包导出；
- `definition` 只搜索当前文档内的声明，不跨文件跳转；
- `rangeFormatting` 返回整个文档的替换编辑；
- 诊断在每次文档变化时立即发布，没有 debounce；大文件可能需要客户端侧节流。

实现设计见 [`tooling-plan.md`](tooling-plan.md)，会话测试见 `tests/cases/lsp.py`。
