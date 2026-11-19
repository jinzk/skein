# REPL 计划

本文记录 REPL 当前能力和后续计划。

## 1. 当前实现

### 行编辑

- POSIX `termios` 行编辑器
- Windows 控制台 `ReadConsoleInputW` 行编辑器
- 历史上下键
- 左右移动、Home/End、Delete
- Ctrl-A/E/K/U/W、Ctrl-L、Ctrl-D
- UTF-8 光标移动

### 显示

- 词法高亮：关键字、字符串、注释、数字、REPL 命令
- 未闭合字符串标红
- `SKEIN_REPL_COLOR=auto|always|never`
- `NO_COLOR`
- `TERM=dumb`
- 默认只在 stdin/stdout 都是终端时启用

### 补全

Tab 补全支持：

- 语言关键字
- REPL 命令
- 当前全局符号
- `std.*` 命名空间
- struct / enum variant 字段与方法
- `:load` 文件路径

行为：

- 唯一候选直接补全
- 多候选扩展公共前缀
- 无公共扩展时列出候选
- 字符串/注释内部不补全

### 缩进

- 多行输入按 `indent_count` 自动缩进
- 每级 2 空格
- 行首输入 `}`、`)`、`]` 自动回退一级
- 纯空白行不写入历史

## 2. 代码结构

```text
src/interpreter/repl.cpp
src/interpreter/repl_ui.cpp
src/interpreter/repl_ui.h
tests/test_repl_ui.cpp
```

`repl_ui` 只包含无运行时依赖的逻辑：

- tokenizer / ANSI 渲染
- 补全上下文
- 候选过滤、排序、公共前缀
- 缩进与回退

## 3. 下一阶段

### P1

- struct 字段补全：`user.<Tab>`（已完成）
- `:load` 路径补全（已完成）
- 长行水平滚动
- 运算符/逗号 continuation 缩进

### P2

- 可配置缩进宽度
- 同一行连续多个闭合符逐级回退
- 语义高亮：类型、函数、builtin
- 括号匹配
- 签名提示 / hover

### P3

- Ctrl-R 历史搜索
- 模糊补全排序
- 补全候选弹窗/分栏
- 多行历史条目

## 4. 测试策略

- `repl_ui_test` 覆盖纯逻辑
- 非 TTY 回归走现有脚本测试
- Linux/macOS 使用 PTY 测试真实行编辑
- Windows 使用 ConPTY 测试
- 覆盖：
  - 补全上下文
  - 高亮 token
  - UTF-8 光标/宽度
  - 自动缩进与闭合回退
  - 历史过滤
