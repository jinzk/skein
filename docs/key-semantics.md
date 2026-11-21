# Dict / Set Key Semantics

本文集中说明 Skein 中 `dict` key 和 `set` 元素的规则。相关 API 的完整列表见
[stdlib.md](stdlib.md)，语言语法见 [SYNTAX.md](../SYNTAX.md)。

## 1. Key 模型

当前 `dict` 和 `set` 是 hash 驱动的容器：

```text
hash(key)  -> 候选桶
eq(a, b)   -> 确认两个 key 是否为同一个 key
```

因此 key 必须同时具备：

- 可哈希
- 可判等
- 两者一致：相等的 key 必须得到相同的 hash

`dict` 当前是“按插入顺序保存的数组 + 延迟构建的 hash 索引”；小 dict 会直接
线性扫描。`set` 是按 key hash 排序的持久化 treap，并用 bucket 处理 hash 冲突。

## 2. 可自动作为 key 的类型

以下类型自动满足 hash + eq：

- `int`
- `bool`
- `string`
- `char`
- `byte`
- `bytes`
- `null`
- frozen tuple/pair，且所有元素递归可作为 key
- struct，且所有字段递归可作为 key
- enum，且所有载荷字段递归可作为 key

tuple/pair 字面量、`zip`、`dict.items`、`iter.enumerate` 产生的 pair 是 frozen
list，不可索引赋值。

```skein
let d = {
  (1, "a"): 1,
  (2, ("b", true)): 2,
};

d[(1, "a")]
```

含可变 list/dict、`float` 等不可哈希值的 tuple/struct/enum 不能自动作为 key。

## 3. 自定义 hash：`DictKey`

`std.dict_key.DictKey` 允许 struct/enum 跳过字段递归检查，使用自定义 hash：

```skein
import std.dict_key;

struct User {
  id: int;
  cache: list[int];
}

impl dict_key.DictKey for User {
  func dict_hash(self) {
    return self.id;
  }
}
```

契约：

- 返回 `int`
- 纯函数、确定性、无副作用
- 与 key 相等语义一致
- hash 期间不能重入 dict/set key hash

如果只实现 `DictKey`，key 相等仍然使用结构相等。此时 `dict_hash` 必须与结构
相等保持一致。

## 4. 自定义相等：`KeyEq`

`KeyEq` 是可选的 key 相等接口：

```skein
impl dict_key.KeyEq for User {
  func key_eq(self, other) {
    return self.id == other.id;
  }
}
```

规则：

- 只有同时实现 `DictKey` 的类型才会使用 `key_eq`
- 未实现 `KeyEq` 时继续使用结构相等
- `key_eq` 只影响 dict/set 以及内部使用 dict/set 的操作，例如 `unique`、
  `group_by`、LRU
- **普通 `==` / `!=` 不改变**，仍使用结构相等
- `key_eq` 返回 `bool`，必须是纯函数、确定性、无副作用
- key_eq 期间不能重入 dict/set key equality

必须满足：

```text
a.key_eq(b) == true  =>  dict_hash(a) == dict_hash(b)
```

否则相等 key 可能落入不同桶，导致查找或去重错误。

## 5. Key 相等与 `==` 的区别

实现 `KeyEq` 后可能出现：

```skein
a != b
```

但：

```skein
d[a] 与 d[b] 命中同一个 key
```

这是 key identity 与语言级 `==` 分离的结果。需要普通相等时仍使用 `==`；
需要 key 语义时使用 `dict` / `set`。

## 6. 快照语义

struct 是 copy-on-write 值类型。struct 作为 key 后，修改原变量不会改变 dict 中
保存的 key 快照：

```skein
let k = Key { id: 1 };
let d = {k: "x"};

k.id = 2;

d[Key { id: 1 }]  // "x"
d[k]              // 查找失败
```

自定义 hash 也必须只依赖稳定的值内容，不能依赖全局状态、时间或随机数。

## 7. `std.dict.index_by`

如果不需要 `d[user]` 语法，而只是想按字段建立索引，优先使用显式投影：

```skein
let by_id = std.dict.index_by(users, func(u) { return u.id; });
by_id[user.id]
```

语义：

- 遍历输入 list
- `key_fn(element)` 作为 key
- 原始 element 作为 value
- 返回新 dict，不修改输入
- 重复 key 时 **last wins**，保留首次出现位置
- key 必须是合法 dict key
- 时间复杂度 O(n)

`index_by` 不需要 `DictKey` 或 `KeyEq`，因为 key 是显式投影出来的普通值。

## 8. 线程和进程传输

- frozen tuple key 会保留不可变标记
- struct/enum key 按普通 struct/enum 传输
- hash cache 不传输，接收端重新计算
- 自定义 `dict_hash` / `key_eq` 方法必须随类型元数据可用
- 依赖不可传输值（函数、heap 等）的 key 方法会在传输时被拒绝
- `spawn_script` / `spawn_source` 会在父线程尽量预编译消息值触达类型的方法体，
  使 AST worker 也能调用；`dict_hash` / `key_eq` 编译失败会终止启动，其他方法
  失败则忽略，由实际调用时报错
- `spawn_entry` / `spawn_function` / 进程传输会把传输方法安装为接收线程的
  方法覆盖，不修改进程共享的类型方法表；嵌套传输和反射会看到这些覆盖

## 9. 错误速查

| 情况 | 结果 |
|---|---|
| `float` 标量 key | 拒绝 |
| mutable list key | 拒绝 |
| tuple 含 mutable list | 拒绝 |
| struct 含 mutable list 且无自定义 hash | 拒绝 |
| enum 载荷含 mutable list 且无自定义 hash | 拒绝 |
| `dict_hash` 返回非 int | 运行时错误 |
| `key_eq` 返回非 bool | 运行时错误 |
| hash/eq 重入 key 计算 | 运行时错误 |
| `KeyEq` 未配合 `DictKey` | 不使用自定义相等 |

## 10. 推荐用法

- 普通标量 key：直接使用。
- 组合字段：使用 frozen tuple，例如 `(region, id)`。
- 大量字段且只按少数字段识别：使用 `DictKey` + `KeyEq`。
- 只需要建立索引：使用 `std.dict.index_by`。
- 不要依赖 `KeyEq` 改变普通 `==`。
- 不要在 hash/eq 中做 I/O、修改容器或依赖全局状态。
