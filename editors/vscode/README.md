# Skein for VS Code

Skein 的 VS Code 扩展：

- `.skein` 文件识别；
- TextMate 语法高亮（关键字、类型、内置函数、`std.*`、struct/enum、字符串、数字、注释）；
- 常用结构片段（`main`、`fn`、`if`、`forin`、`match`、`struct`、`impl`、`enum` 等）；
- 括号自动闭合、`//` 注释、简单缩进规则；
- `Skein: Run Current File` 在终端运行当前文件；
- `Skein: Open REPL` 打开 `skein -i`。

> 这个插件只做编辑器集成，不包含 LSP、补全、跳转定义或诊断。

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

示例：

```json
{
  "skein.executablePath": "./build/skein",
  "skein.extraArgs": ["--vm"]
}
```

## 命令

| 命令 | 作用 |
|---|---|
| `Skein: Run Current File` | 保存后在终端运行当前 `.skein` 文件 |
| `Skein: Open REPL` | 打开 `skein -i` 终端 |

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
- 没有语言服务器，因此没有语义补全、诊断和跳转；
- 运行命令依赖 PATH 中的 `skein`，或通过 `skein.executablePath` 指定路径。
