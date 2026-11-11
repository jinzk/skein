# env / cwd / FFI 线程安全审计

## 结论

- `std.env` 与 `std.fs.cwd` / `chdir` 是**线程局部覆盖层**：读取先查当前线程覆盖，
  再回退进程环境/进程 cwd；写入只影响调用线程。
- 线程、线程池和 HTTP worker 启动时安装创建者快照；子进程从**创建它的线程**
  合并环境覆盖并使用该线程的 cwd。
- FFI 回调按**创建线程**标记 owner；回调任务只由 owner 线程 drain，避免在错误
  线程执行脚本函数。
- `std.ffi.call_task` 的 C 调用运行在独立 worker 线程；该线程看到的是**进程**
  环境和进程 cwd，不是 Skein 的线程局部覆盖。这是 C 运行时限制，已作为明确的
  使用约定记录。

## env / cwd

实现位置：`src/platform/runtime_state.cpp`

- `thread_environment` 保存：
  - `values`：本线程设置的环境变量；
  - `removed`：本线程 unset 的变量；
  - `has_cwd` / `cwd`：本线程 cwd 覆盖。
- `thread_env_get` / `thread_env_all` 合并“进程环境 + 覆盖 + 删除”。
- `thread_env_capture` / `thread_env_install` 用于线程启动：
  - `std.thread.spawn_script` / `spawn_source` / `spawn_function` / `spawn_entry`；
  - `std.thread.pool` worker；
  - `std.http.serve_threads` / `serve_processes` worker。

子进程：

- `std.process.spawn_command` / `run_command` / `command` 使用当前线程的
  `thread_env_all` 和 `thread_cwd`；
- 显式 `env` / `cwd` 选项优先于线程覆盖。

## FFI

### 同步回调

- `ffi.call` 的回调在**当前解释器线程**执行；跨线程回调要求返回 `void`，
  否则报 `cross-thread callbacks must return void`。
- 回调执行期间使用 `callback_call_scope`（thread_local）。

### `ffi.call_task`

- C 函数在独立 `std::thread` 上执行，结果通过 `std::atomic` 和 join 同步；
- 事件循环是 thread_local，因此多个 worker 线程可以并行调用 `call_task`；
- C 回调会封装成 `blocking_callback_job`，带 owner 线程 id；
- owner 线程在 `fiber_pump()` 中 drain 自己的回调任务；
- owner 线程退出前调用 `ffi::background_discard_owned()`，把未完成的回调标记为
  `ffi callback owner thread exited`，避免外部线程永久阻塞；
- `async_depth` 是 thread_local，每个线程独立限制递归深度。

### 已知限制

- `call_task` 的 C 代码无法看到 Skein 的线程局部 env/cwd 覆盖；需要把值作为
  参数传入，或在 C 侧自行处理。
- 回调函数值只在其 owner 线程有效，不能跨线程共享。
- `skein_vm_set_enabled` 是进程级开关，不随线程变化。

## 测试

`tests/env_ffi_stress.py` 覆盖：

1. 线程池 worker 并发设置不同 env 与 cwd，互不影响，父线程保持不变；
2. `spawn_script` 继承父线程的 env/cwd 快照；
3. 线程池 worker 启动子进程时，子进程看到该 worker 的 env/cwd 覆盖；
4. 多个 worker 并发 `ffi.call_task` + 回调，结果一致。

CTest：

- `env_ffi_stress_test`（core）；
- `env_ffi_tsan_test`（TSAN）；
- `nightly_env_ffi_test`（夜间长跑，`SKEIN_STRESS_ROUNDS=200`）。

## 夜间长跑

```text
cmake -S . -B build-nightly -DCMAKE_BUILD_TYPE=Release \
      -DSKEIN_ENABLE_NIGHTLY=ON
cmake --build build-nightly -j
ctest --test-dir build-nightly -L nightly --output-on-failure
```

默认门禁保持 80/40 轮、32 MiB 增长预算；夜间任务使用 600/300 轮、
64 MiB 预算，并额外运行 200 轮的 env/FFI stress。
