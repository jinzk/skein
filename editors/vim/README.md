# Skein for Vim / Neovim

Skein 的 Vim/Neovim 支持：

- `.skein` 文件类型识别；
- 语法高亮（关键字、类型、内置函数、`std.*`、字符串、数字、注释、声明）；
- `//` 注释格式和 `commentstring`；
- 基于大括号的缩进；
- `:SkeinRun` 运行当前文件（先保存未保存的修改）；
- `:SkeinRepl` 打开 REPL；
- Neovim 0.5+ 可通过内置 `vim.lsp` 启动 `skein lsp --stdio`，获得诊断、补全、悬停、跳转定义、文档符号和格式化；
- Vim 可通过 coc.nvim 或 vim-lsp 接入同一个语言服务器。

语言服务器只做静态分析，不会执行被编辑的 Skein 程序。

## 安装

把 `editors/vim` 加入 `runtimepath`。

手工安装：

```vim
set runtimepath+=/path/to/skein/editors/vim
filetype plugin indent on
syntax on
```

vim-plug：

```vim
Plug 'jinzk/skein', { 'rtp': 'editors/vim' }
```

lazy.nvim：

```lua
{
  'jinzk/skein',
  config = function()
    vim.opt.runtimepath:append('/path/to/skein/editors/vim')
  end,
}
```

如果使用 lazy.nvim 的本地目录，也可以用：

```lua
{ dir = '/path/to/skein/editors/vim', name = 'skein' }
```

Vim 自带包管理：

```bash
mkdir -p ~/.vim/pack/skein/start
ln -s /path/to/skein/editors/vim ~/.vim/pack/skein/start/skein
```

## 配置

默认执行 `skein`，可以在 `vimrc` 中指定其他二进制（`:SkeinRun` / `:SkeinRepl`
和下面的 Neovim LSP 示例都会使用该值）：

```vim
let g:skein_executable = '/path/to/skein/build/skein'
```

### Neovim 内置 LSP

在 `init.lua` 中调用自带的 bootstrap 模块：

```lua
require('skein.lsp').setup({
  cmd = { vim.g.skein_executable or 'skein', 'lsp', '--stdio', '--typecheck=strict' },
})
```

`setup()` 会为 `FileType skein` 自动调用 `vim.lsp.start()`；可以传入
`root_dir`、`on_attach`、`capabilities` 和 `settings`。若不想用该模块，也可以直接写：

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

### coc.nvim

在 `coc-settings.json` 中加入：

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

### vim-lsp

在 `vimrc` 中加入：

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

## 命令

| 命令 | 作用 |
|---|---|
| `:SkeinRun` | 保存并运行当前文件 |
| `:SkeinRun path/to/file.skein` | 运行指定文件 |
| `:SkeinRepl` | 在终端中打开 `skein -i` |

`:SkeinRun` 使用 `:!` 同步执行，输出显示在 Vim 的命令区域；按回车返回。
`:SkeinRepl` 需要 `+terminal`，不支持时回退到 `:!`。

## 文件结构

```text
editors/vim/
├── autoload/skein.vim
├── ftdetect/skein.vim
├── ftplugin/skein.vim
├── indent/skein.vim
├── lua/skein/lsp.lua
└── syntax/skein.vim
```

## 已知限制

- 语法高亮按 ASCII 标识符匹配，Unicode 标识符仍能正常显示，但函数名/类型名的特殊高亮可能不生效；
- 缩进是简单的大括号规则，不处理括号内多行参数的额外缩进；
- 传统 Vim 没有内置 LSP，必须使用 coc.nvim / vim-lsp 等插件；补全基于当前可见符号与 `std.*`，不是全项目索引。
