# MySQL native plugin 示例

这个目录用 MySQL C client 验证 Skein 的 native module / plugin ABI：

- `native/mysql_module.cpp`：纯 C ABI 插件，只包含 `skein_module.h`，
  不链接解释器符号；通过 opaque userdata 持有 `MYSQL*` / `MYSQL_RES*`；
- `mysql.skein`：Skein 侧封装，提供 `Connection` / `Result` 方法；
- `example.skein`：最小可运行示例。

它不是核心语言的依赖，也不随默认构建产出。需要先安装 MySQL/MariaDB
客户端开发头文件（Debian/Ubuntu 上是 `libmysqlclient-dev` 或
`libmariadb-dev`）。

## 构建

```bash
cmake -S . -B build -DSKEIN_BUILD_MYSQL_PLUGIN=ON
cmake --build build --parallel
```

产物是 `build/libskein_mysql_module.so`（Linux）。CMake 只在找到
`mysql.h` 和客户端库时构建该目标；找不到时目标与测试会跳过。

## 运行

`mysql.skein` 通过 `SKEIN_MYSQL_PLUGIN` 定位动态库：

```bash
export SKEIN_MYSQL_PLUGIN="$PWD/build/libskein_mysql_module.so"
export SKEIN_TEST_MYSQL_HOST=127.0.0.1
export SKEIN_TEST_MYSQL_PORT=3306
export SKEIN_TEST_MYSQL_USER=root
export SKEIN_TEST_MYSQL_PASSWORD=
export SKEIN_TEST_MYSQL_DATABASE=skein_test
./build/skein contrib/mysql/example.skein
```

也可以在自己的项目里复制 `mysql.skein` 后直接 `import mysql;`。封装层只依赖
native plugin 导出的函数与 userdata，不绑定固定 .so 路径。

## API

Skein 封装：

```text
mysql.VERSION
mysql.library_version()
mysql.connect(host, user, password, database, port = 3306) -> Connection

Connection.query(sql)        -> Result
Connection.execute(sql)      -> null
Connection.ping()            -> true
Connection.close()           -> null
Connection.select_db(name)   -> null
Connection.set_charset(name) -> null
Connection.quote(text)       -> string
Connection.affected_rows()   -> int
Connection.insert_id()       -> int
Connection.last_error()      -> string

Result.rows()                -> list[dict]
Result.count()               -> int
Result.fields()              -> list[string]
Result.close()               -> null
```

原生插件导出模块名 `skein.mysql`，函数都是同步的；错误通过
`skein_api.error` 返回，Skein 侧看到 `runtime` 错误类别。

`fetch_all` 的转换规则：SQL `NULL` → `null`，文本列 → `string`，二进制列
（charsetnr 63）→ `bytes`。结果对象持有连接 userdata 的引用，连接不会在
结果仍存活时被释放。

## 测试

```bash
ctest --test-dir build -R mysql_plugin_test --output-on-failure
```

未配置数据库时只跑 ABI 测试：版本/常量、错误传播、参数校验、
`std.plugin.unload` 后已有绑定继续可用。设置
`SKEIN_TEST_MYSQL_HOST/USER/PASSWORD/DATABASE` 和可选的
`SKEIN_TEST_MYSQL_PORT` 后，还会跑建表、插入、查询、NULL/BLOB、错误恢复、
unload 后继续使用 userdata、资源关闭等真实集成用例。

## 当前范围与限制

- 只覆盖同步 API；ABI v2 的 `call_task` 异步导出留到第二阶段；
- 不支持 prepared statement、事务封装、连接池、unix socket；
- `Connection` 是 copy-on-write struct，但里面的 userdata 是共享句柄：
  复制出来的多个 `Connection` 指向同一个 MySQL 连接；
- MySQL 连接不要跨线程共享。插件内部用 mutex 防止同一连接的并发 C API
  调用，但这不等于连接池或事务隔离。
