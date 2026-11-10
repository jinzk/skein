# serve_threads vs serve_processes 对比报告

日期：2026-11-10  
基线提交：`7d6de5a Add thread and HTTP benchmark regression gates`  
工具：`tools/bench_http.py`（含 server 进程树 RSS 采样）

## 环境

| 平台 | 构建 | 说明 |
|---|---|---|
| Windows | `build-msys2-jit-mi2` | UCRT64，JIT 开启 |
| Linux | `/home/alex/skein-build` | WSL2 Ubuntu，release 构建 |

统一参数：`workers=4`、`concurrency=8`、`max_requests=requests`，
worker 返回固定 `200 ok`，客户端使用 keep-alive HTTP/1.1。

## 吞吐与延迟

### Windows（dispatch）

| 模式 | 请求数 | 吞吐 | mean | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| `serve_threads` | 400 | 10499 req/s | 0.71 ms | 0.57 ms | 1.35 ms | 4.86 ms |
| `serve_processes` | 400 | 8921 req/s | 0.85 ms | 0.65 ms | 1.61 ms | 6.00 ms |

### Linux（dispatch）

| 模式 | 请求数 | 吞吐 | mean | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| `serve_threads` | 800 | 5530 req/s | 1.39 ms | 1.21 ms | 2.75 ms | 4.31 ms |
| `serve_processes` | 800 | 5475 req/s | 1.40 ms | 1.24 ms | 2.58 ms | 3.79 ms |

### Linux（processes + reuse_port）

| 模式 | 请求数 | 吞吐 | mean | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| `serve_processes` reuse_port | 2000 | 5692 req/s | 1.37 ms | 1.19 ms | 2.79 ms | 4.07 ms |

## 内存

RSS 为 server 进程树（父进程 + 所有 worker）在 ready 后与请求高峰期采样：

| 平台 / 模式 | ready | 峰值 | 增量 |
|---|---:|---:|---:|
| Windows threads | 14.2 MiB | 14.9 MiB | +0.7 MiB |
| Windows processes | 61.6 MiB | 63.1 MiB | +1.5 MiB |
| Linux threads | 11.3 MiB | 12.0 MiB | +0.7 MiB |
| Linux processes | 50.9 MiB | 53.3 MiB | +2.4 MiB |
| Linux processes reuse_port | 52.7 MiB | 53.6 MiB | +0.9 MiB |

进程模式每个 worker 都是一个完整解释器，基础内存约为线程模式的 4~4.5 倍；
两种模式在请求高峰期都没有持续增长（长跑趋势见 `memory_soak_test`）。

## 隔离与故障语义

| 维度 | serve_threads | serve_processes |
|---|---|---|
| 地址空间 | 共享，worker 状态必须线程隔离 | 独立进程，天然隔离 |
| 单 worker 崩溃 | 只能作为线程错误上报/重启线程 | 子进程退出，launcher 可重启，HTTP 502 |
| 阻塞/死循环 | 影响同进程调度 | 只影响该子进程 |
| 共享配置 | 直接共享 server/线程池对象 | 通过脚本与 CLI 重新加载 |
| 平台限制 | 全平台 | `reuse_port` 仅 Linux；dispatch 全平台 |

现有测试覆盖：

- `tests/harness.py` 的 `serve_threads` / `serve_processes` 崩溃用例验证 502 与重启；
- `memory_soak_test` 验证两种模式的 RSS 趋势；
- `http_dispatch_bench_test` / `thread_bench_test` 固定基线与回归阈值。

## 结论

- **吞吐/延迟**：两种模式在同一量级；Windows 上线程模式略快（约 15%），
  Linux dispatch 基本持平，`reuse_port` 与 dispatch 吞吐接近。
- **内存**：线程模式显著更省内存；进程模式基础占用约为 4 倍，适合需要故障隔离的场景。
- **选型建议**：默认优先 `serve_threads`；需要崩溃隔离、独立重启或第三方脚本
  隔离时使用 `serve_processes`。Linux 高并发监听优先 `reuse_port`。

## 复现

```bash
# Windows
python tools/bench_http.py --bin build-msys2-jit-mi2/skein.exe \
  --mode both --strategy dispatch --requests 400 --concurrency 8 --workers 4

# Linux
python3 tools/bench_http.py --bin /home/alex/skein-build/skein \
  --mode both --strategy dispatch --requests 800 --concurrency 8 --workers 4
python3 tools/bench_http.py --bin /home/alex/skein-build/skein \
  --mode processes --strategy reuse_port --requests 2000 --concurrency 8 \
  --workers 4
```
