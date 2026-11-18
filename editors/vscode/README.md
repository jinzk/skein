# Skein for VS Code

Skein 的 VS Code 扩展：

- `.skein` 文件识别；
- TextMate 语法高亮（关键字、类型、内置函数、`std.*`、struct/enum、字符串、数字、注释）；
- 常用结构片段（`main`、`fn`、`if`、`forin`、`match`、`struct`、`impl`、`enum` 等）；
- 括号自动闭合、`//` 注释、简单缩进规则；
- 通过 `vscode-languageclient` 自动启动 `skein lsp --stdio`，提供诊断、补全、悬停提示、跳转定义、文档符号与格式化；
- `Skein: Run Current File` 在终端运行当前文件；
- `Skein: Open REPL` 打开 `skein -i`；
- `Skein: Format Document` 调用语言服务器格式化当前文件（也可用 `Shift+Alt+F` 或 `editor.formatOnSave`）。

语言服务器只做静态分析，不会执行被编辑的 Skein 程序。

## 安装

### 本地开发

用 VS Code 打开 `editors/vscode` 目录，按 `F5` 启动 Extension Development Host，
然后打开任意 `.skein` 文件。

### 打包安装

```bash
cd editors/vscode
npx @vscode/vsce package
code --install-extension skein-0.1.0.vsix
```

也可以直接把 `editors/vscode` 复制到 VS Code 的扩展目录（不推荐，升级时容易残留旧文件）。

## 配置

| 设置 | 默认值 | 说明 |
|---|---|---|
| `skein.executablePath` | `skein` | Skein 解释器路径；相对路径从工作区根目录解析 |
| `skein.extraArgs` | `[]` | 在脚本路径前追加的参数，例如 `["--typecheck=strict"]` |
| `skein.saveBeforeRun` | `true` | 运行前保存当前文件 |
| `skein.lsp.enabled` | `true` | 是否启动语言服务器 |
| `skein.typecheck` | `warn` | 传给语言服务器的检查强度：`off` / `loose` / `warn` / `strict` |
| `skein.packagePaths` | `[]` | 传给语言服务器的额外包搜索路径，每项会以 `-I` 传入 |

修改以上 LSP 相关设置后扩展会重启语言服务器。示例：

```json
{
  "skein.executablePath": "./build/skein",
  "skein.extraArgs": ["--vm"],
  "skein.typecheck": "strict",
  "skein.packagePaths": ["./packages"],
  "editor.formatOnSave": true
}
```

## 命令

| 命令 | 作用 |
|---|---|
| `Skein: Run Current File` | 保存后在终端运行当前 `.skein` 文件 |
| `Skein: Open REPL` | 打开 `skein -i` 终端 |
| `Skein: Format Document` | 调用语言服务器格式化当前文件 |

运行命令复用名为 `Skein` 的终端；REPL 复用名为 `Skein REPL` 的终端。

## 文件结构

```text
editors/vscode/
├── extension.js
├── language-configuration.json
├── package.json
├── snippets/skein.json
└── syntaxes/skein.tmLanguage.json
```

## 已知限制

- 语法高亮基于 TextMate 正则，不是完整解析器；极端的嵌套结构可能着色不完美；
- LSP 诊断来自 parse/typecheck，补全数据来自当前可见符号与 `std.*` 命名空间，不是全项目索引；
- 运行命令和语言服务器都依赖 PATH 中的 `skein`，或通过 `skein.executablePath` 指定路径；
- 首次开发调试需先执行 `npm install` 安装 `vscode-languageclient`。
