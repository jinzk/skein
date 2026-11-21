# MySQL native plugin 示例

这个目录用 MySQL C client 验证 Skein 的 native module / plugin ABI，同时也是
一个源码形式发布的 native module：

- `module.skein`：模块声明、`native { ... }` 构建配置和导出表；
- `native/mysql_module.cpp`：纯 C ABI 插件，只包含随模块分发的
  `skein_module.h`，不链接解释器符号；通过 opaque userdata 持有
  `MYSQL*` / `MYSQL_RES*`；
- `native/mysql_headers.h`：兼容 CMake 构建和 `skein build` 源码构建的
  MySQL/MariaDB 头文件探测层；
- `api/mysql.skein`：Skein 侧封装，提供 `Connection` / `Result` 方法；
- `example.skein`：最小可运行示例。

它不是核心语言的依赖，也不随默认构建产出。需要先安装 MySQL/MariaDB
客户端开发库（Debian/Ubuntu 上是 `libmysqlclient-dev` 或
`libmariadb-dev`）。

## 源码 native module 流程

把 `contrib/mysql` 单独发布成 git 仓库，模块路径使用
`skein.contrib/mysql`。在应用项目里：

```bash
skein get skein.contrib/mysql@v0.1.0
skein build skein.contrib/mysql@v0.1.0
SKEIN_ALLOW_NATIVE=1 ./skein main.skein
```

`main.skein`：

```text
import "skein.contrib/mysql" as mysql;
println(mysql.VERSION);
println(mysql.library_version());
```

第三方 module 内的 `std.plugin.load` 默认禁止，因此运行脚本时必须设置
`SKEIN_ALLOW_NATIVE=1`。`skein build` 会在缓存目录旁生成动态库，封装包通过
`std.module.native("skein.contrib/mysql")` 取得路径，不绑定固定 `.so` 位置。

本地验证时可以用 URL override 指向仓库副本：

```bash
export SKEIN_MODULE_URL=/path/to/mysql-module-repo
```

模块在 `native/skein_module.h` 自带一份 ABI 头文件副本；升级宿主 ABI 时需要
同步更新该副本。

## 直接构建插件

也可以只构建动态库，用于开发或单独测试：

```bash
cmake -S . -B build -DSKEIN_BUILD_MYSQL_PLUGIN=ON
cmake --build build --parallel
```

产物是 `build/libskein_mysql_module.so`（Linux）。CMake 只在找到
`mysql.h` 和客户端库时构建该目标；找不到时目标与测试会跳过。

直接复制 `api/mysql.skein` 到项目里时，封装包优先读取
`SKEIN_MYSQL_PLUGIN`，未设置才回退到 `std.module.native`：

```bash
export SKEIN_MYSQL_PLUGIN="$PWD/build/libskein_mysql_module.so"
export SKEIN_TEST_MYSQL_HOST=127.0.0.1
export SKEIN_TEST_MYSQL_PORT=3306
export SKEIN_TEST_MYSQL_USER=root
export SKEIN_TEST_MYSQL_PASSWORD=
export SKEIN_TEST_MYSQL_DATABASE=skein_test
./build/skein contrib/mysql/example.skein
```

## API

模块根导出常用函数；`Connection` / `Result` 类型通过 `mysql.client.*` 暴露：

```text
mysql.VERSION
mysql.library_version()
mysql.connect(host, user, password, database, port = 3306) -> client.Connection
mysql.wrap_result(task)              -> client.Result

client.Connection.query(sql)        -> client.Result
client.Connection.query_task(sql)   -> task[userdata]
client.Connection.execute(sql)      -> null
client.Connection.ping()            -> true
client.Connection.close()           -> null
client.Connection.select_db(name)   -> null
client.Connection.set_charset(name) -> null
client.Connection.quote(text)       -> string
client.Connection.affected_rows()   -> int
client.Connection.insert_id()       -> int
client.Connection.last_error()      -> string

client.Result.rows()                -> list[dict]
client.Result.count()               -> int
client.Result.fields()              -> list[string]
client.Result.close()               -> null
```

原生插件导出模块名 `skein.mysql`，函数都是同步的；错误通过
`skein_api.error` 返回，Skein 侧看到 `runtime` 错误类别。

`fetch_all` 的转换规则：SQL `NULL` → `null`，文本/数值列 → `string`，
BLOB/BINARY/VARBINARY/BIT 列 → `bytes`。结果对象持有连接 userdata 的引用，
连接不会在结果仍存活时被释放。

## 异步查询（ABI v2）

`query_task` 使用 `skein_api.call_task` 在 worker 线程执行 SQL，返回标准
Skein task。task 的结果是需要包装的 native userdata，用 `mysql.wrap_result`
转成 `client.Result`：

```text
let t = conn.query_task("SELECT 42 AS answer");
let result = mysql.wrap_result(t);
println(result.rows()[0]["answer"]);
result.close();
```

- `std.task.cancel(t)` 是协作式取消：worker 在进入 SQL 前和取得连接后检查
  取消状态；已经在执行的 MySQL 语句不会被强制中断；
- 同一连接上的同步方法会在 mutex 上等待异步语句结束，不应把同一连接当作
  并发查询通道；
- SQL 失败让 task 失败（`native task failed`），具体信息保留在
  `conn.last_error()`；
- `finish` / `destroy` 在 owner 线程运行，异步任务持有连接 userdata 和模块
  引用，`std.plugin.unload` 后仍可安全完成。

## 测试

```bash
ctest --test-dir build -R mysql_plugin_test --output-on-failure
```

未配置数据库时测试 ABI、源码 native module 打包路径、异步导出存在性、错误
传播、参数校验、`std.plugin.unload` 后已有绑定继续可用。设置
`SKEIN_TEST_MYSQL_HOST/USER/PASSWORD/DATABASE` 和可选的
`SKEIN_TEST_MYSQL_PORT` 后，还会跑建表、插入、同步查询、NULL/BLOB、
异步查询、task 失败与取消、错误恢复、unload 后继续使用 userdata、资源关闭
等真实集成用例。

## 当前范围与限制

- 异步只覆盖 `query_task`；`execute_task`、prepared statement 的异步版本尚未提供；
- 不支持 prepared statement、事务封装、连接池、unix socket；
- `Connection` 是 copy-on-write struct，但里面的 userdata 是共享句柄：
  复制出来的多个 `Connection` 指向同一个 MySQL 连接；
- MySQL 连接不要跨线程共享。插件内部用 mutex 防止同一连接的并发 C API
  调用，但这不等于连接池或事务隔离。
