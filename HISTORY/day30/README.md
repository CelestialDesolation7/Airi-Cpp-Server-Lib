# Airi-Cpp-Server-Lib

一个参考 muduo 思路、基于 **Reactor 模型** 从零实现的 C++ 高性能 HTTP 服务器框架。
覆盖从 epoll/kqueue 系统调用直到中间件链 / 限流 / 鉴权 / Prometheus 指标的全栈。

> **当前主线版本：day30 生产就绪发布。**
> 30 天逐日开发记录见 [`dev-log/`](dev-log/)。
> day31-36（WebSocket / 协程 / io_uring / 无锁队列 / 内存池 / muduo 横向基准）
> 是后续实验性扩展，**仅以历史快照形式保留在 [`HISTORY/`](HISTORY/) 目录中作为参考**，不计入主线发布。

![demo](./imgs/image-20260407224115056.png)

---

## 关键性能指标

> 8-core macOS · 2 IO 线程 · localhost wrk 长连接（`-t4 -c200 -d30s --latency`）

| 指标 | 数值 |
|------|------|
| 峰值 QPS | **~188,000** |
| P99 延迟 | **2.15 ms** |
| 单连接 RAM 占用 | **3.72 KB** |
| 平均 CPU 占用（全负载） | **22%** |
| 单元测试 | **17 GTest 用例 / 88 子断言 全 PASS** |
| CI 矩阵 | **7 个 job 并行**（Linux × {gcc, clang} / macOS / ASan / TSan / coverage / clang-tidy）|

完整复现脚本见 `INTERVIEW_GUIDE.md` §7。

---

## 功能速览

按开发阶段（day1-30）汇总。

### 网络层（day1-13, 17-22）

- **跨平台 IO 多路复用**：Linux **epoll**（LT/ET）/ macOS **kqueue**，策略模式统一接口（`Poller`/`EpollPoller`/`KqueuePoller`）
- **Main-Sub Reactor 多线程**：one-loop-per-thread，`Acceptor` 跑在 mainReactor，连接均匀分发到 N 个 subReactor
- **非阻塞 + 边缘触发**：`readv` + 栈 64KB extra buffer，`Buffer` 自增长（prepend / readableBytes / writableBytes）
- **跨线程任务投递**：`runInLoop(f)` / `queueInLoop(f)` + eventfd/pipe wakeup
- **连接生命周期安全**：`shared_ptr<Connection>` + `enable_shared_from_this`；析构由 `queueInLoop(deleteConnection)` 延迟到归属 sub loop 线程
- **回压保护**：高/低水位回调、写缓冲硬上限（默认 64 MB）超阈值强制断连
- **最大连接数保护**：超过 `maxConnections` 拒绝 accept 但不退出循环
- **优雅关闭**：`SIGINT/SIGTERM` → `server.stop()` → wakeup 唤醒所有 IO 线程退出

### 定时器与日志（day23, 24, 27）

- **跨平台定时器**：`TimeStamp` / `Timer` / `TimerQueue`，poll 超时驱动，**不占额外 fd**
- **空闲连接自动关闭**：`weak_ptr<bool>` alive flag 防止跨连接误杀
- **四层异步日志**：`FixedBuffer` → `LogStream` → `Logger` → `AsyncLogging`（双缓冲），业务线程零阻塞
- **结构化日志上下文**：`thread_local LogContext`，自动携带 `[req-N METHOD /url]` 前缀
- **单调时钟**：`SteadyClock` 替代 `time(nullptr)`，免疫 NTP 跳变 / 闰秒回退

### HTTP 层（day25-26, 29-30）

- **HTTP/1.1 状态机解析**：`HttpContext`（kExpectRequestLine → kExpectHeaders → kExpectBody → kGotAll/kInvalid），**pipeline 安全**
- **路由表**：精确路径 `addRoute(method, path, handler)` + 前缀匹配 `addPrefixRoute`
- **三层请求大小限流**：请求行 / 头部块 / 请求体各自独立上限，防 OOM 攻击（默认 8K / 16K / 8M）
- **请求超时 / 空闲超时**：Slowloris 防护
- **Connection: keep-alive** 默认开启，`autoClose` 可控
- **TLS 接口预留**：`MCPP_ENABLE_OPENSSL` 启用，OpenSSL 链接（day29 接口完成）

### 中间件链（day29-30，"洋葱模型"）

按推荐注册顺序：

| 顺序 | 中间件 | 作用 |
|------|--------|------|
| ① | **AccessLog** | `next()` 前后打日志 + 计时（喂给 `ServerMetrics`）|
| ② | **RateLimiter** | per-IP 令牌桶，超额返回 `429 Too Many Requests`|
| ③ | **CorsMiddleware** | OPTIONS 预检直接 `204` 短路；为非预检注入 CORS 响应头 |
| ④ | **AuthMiddleware** | Bearer Token / API Key 双模式，白名单路径 / 前缀，未通过返回 `403`|
| ⑤ | **GzipMiddleware** | `next()` 后看到完整响应体，按 Content-Type 与大小阈值压缩 |

> **顺序里的两条铁律**（详见 [`dev-log/day30-项目收官.md`](dev-log/day30-项目收官.md) §13.1）：
> - **CORS 必须在 Auth 之前**——浏览器预检不带 `Authorization` 头，否则一律 403
> - **Gzip 必须在最内层**——只有 `next()` 之后才能看到完整 body

### 静态文件服务（day29）

- 自动 MIME 推断（按扩展名表）
- **ETag** + `If-None-Match` → `304 Not Modified`
- **Range** + `If-Range` → `206 Partial Content`，越界返回 `416`
- **`sendfile()` 零拷贝**（Linux），macOS 自动降级为 `pread + send`
- TLS 模式自动跳过零拷贝（数据须经用户态加密）

### 可观测性（day29）

- `/metrics` 端点直出 **Prometheus text exposition format**
- 指标：`mcpp_connections_total` / `mcpp_requests_total` / `mcpp_responses_2xx|3xx|4xx|5xx` / `mcpp_request_duration_microseconds_bucket`（8 桶 ≤100µs ... >100ms）
- 全部为 lock-free `std::atomic` 累加，零锁开销

### 测试与工程化（day28, 30）

- **GoogleTest 17 个 TEST 用例 / 88 子断言**（FetchContent 自动拉取 v1.15.2，无需系统安装）
- **GitHub Actions CI 7-job 矩阵**：
    - `build-linux-gcc` / `build-linux-clang` / `build-macos`：基线编译 + ctest
    - `asan`：AddressSanitizer + UBSan
    - `tsan`：ThreadSanitizer（多 reactor 数据竞争守门员）
    - `coverage`：gcov + lcov，结果上传 codecov
    - `clang-tidy`：bugprone-* / cppcoreguidelines-* 规则集
- **VS Code Tasks**：`Cmd+Shift+B` 一键 configure / build / install / 链接 examples
- **app_example/** 子项目：通过 `find_package(Airi-Cpp-Server-Lib)` 验证库的对外接口稳定性

---

## 架构

```
┌──────────────────────────────────────────────────────────────────┐
│                            TcpServer                             │
│    maxConnections=10000   IO Threads=N（默认=CPU 核数）          │
└──────────────────────┬───────────────────────────────────────────┘
                       │
          ┌────────────┴────────────┐
          │                         │
 ┌────────┴───────┐       ┌─────────┴──────────────┐
 │  Main Reactor  │       │  EventLoopThreadPool   │
 │  (accept 线程) │       │  Sub Reactor × N       │
 └────────┬───────┘       └─────────┬──────────────┘
          │ accept() + 轮询分配 fd            │
          └───────────────────────────────────┘
                                              │
                                          subLoopᵢ
                                              │
                                  Channel(fd 事件分发)
                                              │
                                  Connection(读写状态机 + Buffer)
                                              │
                ┌─────────────────────────────┴──────────────────────┐
                │                  HttpServer                        │
                │  HttpContext(FSM) → 中间件链(限流→CORS→鉴权→Gzip)  │
                │  → Router → 业务 handler                           │
                │  → ServerMetrics(原子计数+延迟分桶)                │
                └────────────────────────────────────────────────────┘
```

线程安全机制：
- **跨线程任务投递**：`loop->runInLoop(f)` / `queueInLoop(f)` + wakeup
- **连接析构**：`HttpServer::onClose` → `queueInLoop(deleteConnection)` → 归属 sub loop 线程执行，避免 use-after-free
- **日志**：各 IO 线程 push 到本地 `FixedBuffer`，`AsyncLogging` 后端线程定时 swap + 写文件

---

## 代码目录

```
Airi-Cpp-Server-Lib/
├── CMakeLists.txt               ← 顶层构建，定义所有 MCPP_* 选项
├── README.md                    ← 本文档
├── INTERVIEW_GUIDE.md           ← 面试演示速查手册（必读）
│
├── src/
│   ├── include/                 ← 对外公开头文件
│   │   ├── Airi-Cpp-Server-Lib.h     ← 统一入口
│   │   ├── net/                 ← Acceptor / Buffer / Channel / Connection / EventLoop / TcpServer
│   │   │   └── Poller/          ← Epoll / Kqueue
│   │   ├── http/                ← HttpServer / Context / Request / Response
│   │   │                          + RateLimiter / AuthMiddleware / CorsMiddleware
│   │   │                          + GzipMiddleware / StaticFileHandler / ServerMetrics
│   │   ├── log/                 ← Logger / AsyncLogging / LogContext / SteadyClock
│   │   └── timer/               ← Timer / TimerQueue
│   ├── common/                  ← 实现文件（与 include/ 镜像）
│   └── test/                    ← 17 个 GTest 套件 + server/client 入口
│
├── examples/                    ← 全特性演示应用
│   ├── CMakeLists.txt           ← find_package(Airi-Cpp-Server-Lib) 接入
│   ├── src/http_server.cpp      ← 路由 + 5 层中间件链 + 优雅停机
│   ├── static/index.html        ← 演示首页（前后端分离）
│   └── files/readme.txt         ← /files/* 下载演示文件
│
├── benchmark/                   ← 基准测试
│   └── conn_scale_test.cpp      ← 大规模连接扩展性测试
├── demo/demo_server.cpp         ← Phase 4 阶段性演示（限流 + 鉴权 + 指标）
│
├── dev-log/                     ← 完整 30 天开发日志（每 day 一个 .md）
│   └── README.md                ← 日志索引
├── HISTORY/                     ← 历史快照归档
│   ├── day01/ ... day30/        ← 各阶段代码完整快照
│   └── day31/ ... day36/        ← 实验性扩展归档（不计入主线，仅作参考）
├── cmake/                       ← CMake 包配置模板（find_package 用）
└── imgs/                        ← 文档图片
```

---

## 构建

### 环境要求

| 依赖 | 必需 | 说明 |
|------|------|------|
| CMake ≥ 3.21 | ✅ | 构建系统 |
| C++17 编译器 | ✅ | macOS Apple Clang / Linux GCC ≥ 9 / Clang ≥ 10 |
| pthread | ✅ | POSIX 线程 |
| zlib | 推荐 | gzip 中间件需要（`find_package(ZLIB)` 自动探测）|
| OpenSSL | 可选 | TLS 支持（`-DMCPP_ENABLE_OPENSSL=ON`）|

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `MCPP_ENABLE_TESTING` | OFF | 启用 GoogleTest 单元测试（FetchContent 自动拉取 v1.15.2）|
| `MCPP_ENABLE_OPENSSL` | OFF | 启用 TLS（需系统 OpenSSL）|
| `MCPP_ENABLE_ASAN` | OFF | AddressSanitizer + UBSan |
| `MCPP_ENABLE_TSAN` | OFF | ThreadSanitizer |
| `MCPP_ENABLE_COVERAGE` | OFF | 代码覆盖率（gcov / lcov）|

### 一行构建运行

```bash
# 配置 + 编译（包含演示服务器和所有测试）
cmake -B build -S . -DMCPP_ENABLE_TESTING=ON
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)

# 运行演示服务器（默认 127.0.0.1:8888）
cd build && ./app_demo

# 运行全部单元测试
cd build && ctest --output-on-failure        # 88/88 PASS
```

### 链接为外部库

```bash
# 1) 安装到 examples/external/
cmake --install build --prefix examples/external/Airi-Cpp-Server-Lib

# 2) examples/ 通过 find_package 接入
cmake -S examples -B examples/build
cmake --build examples/build -j
```

详细 VS Code Task / Sanitizer / coverage 操作步骤见 `INTERVIEW_GUIDE.md` §2-§6。

---

## 演示服务器（`app_demo`）

`examples/src/http_server.cpp` 是面向面试场景的**唯一推荐演示入口**，一次性串联：

- 路由表（精确 `addRoute` + 前缀 `addPrefixRoute`）
- 5 层中间件链（顺序见上方"中间件链"小节）
- `/static/*` + `/files/*` 双前缀（ETag / 304 / Range / sendfile）
- `/metrics` 直出 `ServerMetrics::toPrometheus()`
- `SIGINT/SIGTERM` 优雅停机
- 环境变量覆盖：`MYCPPSERVER_BIND_IP` / `MYCPPSERVER_BIND_PORT`

### 冒烟脚本（face-to-face 演示用）

```bash
# 启动
cd build && ./app_demo &

# 开放端点
curl -i http://localhost:8888/                                          # 200 + HTML
curl -i http://localhost:8888/health                                    # 200 + JSON
curl -i http://localhost:8888/metrics                                   # 200 + Prometheus

# 鉴权
curl -i http://localhost:8888/api/users                                 # 403（无凭证）
curl -i -H "Authorization: Bearer demo-token-2024" \
     http://localhost:8888/api/users                                    # 200
curl -i -H "X-API-Key: api-key-001" http://localhost:8888/api/users     # 200

# CORS 预检
curl -i -X OPTIONS -H "Origin: http://x" \
     -H "Access-Control-Request-Method: GET" \
     http://localhost:8888/api/users                                    # 204

# 静态文件 + Range
curl -i http://localhost:8888/static/index.html                         # 200
curl -i -H "Range: bytes=0-99" http://localhost:8888/files/readme.txt   # 206

# 触发限流（每 IP 200 突发 / 100 RPS 稳态）
for i in $(seq 1 300); do
    curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8888/
done | sort | uniq -c                                                   # 200 + 429 混合
```

---

## 文档

- [`INTERVIEW_GUIDE.md`](INTERVIEW_GUIDE.md) — **面试演示速查手册**（30s 电梯演讲 / 测试操作 / Sanitizer / coverage / clang-tidy / FAQ Q1-Q8）
- [`dev-log/`](dev-log/) — 30 天逐日开发记录，每天对应一份独立 markdown
- [`HISTORY/`](HISTORY/) — 各阶段代码完整快照（day01-day36；day31-36 为实验性归档，未合入主线）

---

## 关于 day31-36

day31 起的内容（WebSocket / C++20 协程 IO / io_uring 后端 / 无锁队列 + Work-Stealing 线程池 / 内存池 / muduo 横向基准）属于 **Phase 5 实验性扩展**，主要用于探索更激进的性能与协议演进，不在主线"生产就绪"目标内。这些工作的代码、测试与开发日志完整保留在 [`HISTORY/day31/`](HISTORY/) ~ [`HISTORY/day36/`](HISTORY/) 目录下作为学习参考，但不再随主分支演进。

主线（`main` 分支）至 day30 即止，所有性能数字、CI 矩阵、面试手册均围绕 day30 的代码状态。

---

## License

MIT
# Airi-Cpp-Server-Lib

一个参考 muduo 思路、基于 Reactor 模型从零实现的 C++ 网络库学习项目。day1-day36 完整开发记录见 [`dev-log/`](dev-log/)。

目标不是复刻完整的工业级框架，而是通过亲手实现所有关键组件——从 epoll/kqueue 直到 WebSocket、协程 IO、无锁队列与内存池——深入理解高并发 TCP/HTTP 服务器的核心机制。

![demo](./imgs/image-20260407224115056.png)

---

## 功能速览

按开发阶段（day1-36）汇总：

### 网络层（day1-13, 24）

- 跨平台 IO 多路复用：Linux **epoll**、macOS **kqueue**、可选 Linux **io_uring**（day33 实验性）
- Main-Sub Reactor 多线程，one-loop-per-thread
- 非阻塞 + 边缘触发，readv + 栈溢出缓冲的自增长 Buffer
- 跨线程任务投递（`runInLoop` / `queueInLoop` + wakeup）
- 连接生命周期安全销毁（queueInLoop 延迟 + Channel 当轮 events 遍历后释放）
- 连接回压保护（高/低水位、硬上限保护断连）
- 最大连接数保护、accept 非致命错误自愈、epoll 错误降级
- sendfile 零拷贝（Linux）+ pread 兜底，TLS 模式自动降级
- 优雅关闭（SIGINT 触发 stop，wakeup 唤醒阻塞循环）

### 定时器与日志（day13, 27, 28）

- TimeStamp / Timer / TimerQueue，poll 超时驱动，无额外 fd
- 空闲连接自动关闭（`weak_ptr<bool>` alive flag）
- 四层日志：FixedBuffer → LogStream → Logger → AsyncLogging（双缓冲，业务线程零阻塞）
- 结构化日志上下文（`thread_local LogContext`，自动携带 `[req-N METHOD /url]`）
- 单调时钟（`SteadyClock`）替代 wall clock，免疫 NTP 跳变

### HTTP / WebSocket（day14-22, 31）

- HTTP/1.1 状态机解析（`HttpContext`，流水线安全）
- 路由（精确 + 前缀）+ 中间件链（洋葱模型）
- 写事件异步发送，请求体大小限制（413）
- **WebSocket**（day31）：RFC 6455 帧编解码、握手、Ping/Pong/Close、文本/二进制
- 中间件：限流（令牌桶 + 每 IP）、鉴权（Bearer + APIKey）、CORS、gzip、静态文件（ETag / 304 / Range / 416）
- TLS（OpenSSL，可选）

### 可观测性（day25）

- `/metrics` Prometheus text exposition format，lock-free 原子计数 + 延迟分桶

### 高级组件（day32-36）

- **C++20 协程 IO**（day32）：`Task<T>` / `Awaitable` / EventLoop scheduler
- **io_uring 后端**（day33）：Linux 5.1+，macOS 自动降级 stub
- **无锁队列与 Work-Stealing 线程池**（day34）：SPSC / MPMC（Vyukov）/ WorkStealingPool
- **内存池**（day35）：`FixedSizePool` / `ConcurrentFixedSizePool` / `SlabAllocator`（10 size class）+ STL allocator 适配器
- **muduo 真实横向基准**（day36）：Docker + wrk 一键复现，本项目 vs muduo 同 CPU/同负载实测对比

---

## 架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                            TcpServer                                │
│    maxConnections=10000   IO Threads=N（默认=CPU 核数）              │
└──────────────────────┬──────────────────────────────────────────────┘
                       │
          ┌────────────┴────────────┐
          │                         │
 ┌────────┴───────┐       ┌─────────┴──────────────┐
 │  Main Reactor  │       │  EventLoopThreadPool   │
 │  (accept 线程) │       │  Sub Reactor × N       │
 └────────┬───────┘       └─────────┬──────────────┘
          │ accept() + 轮询分配 fd            │
          └───────────────────────────────────┘
                                              │
                                          subLoopᵢ
                                              │
                                  Channel(fd 事件分发)
                                              │
                                  Connection(读写状态机 + Buffer)
                                              │
                ┌─────────────────────────────┴──────────────────────┐
                │                  HttpServer                        │
                │  HttpContext(FSM) → 中间件链(限流→鉴权→CORS→Gzip)  │
                │  → Router → 业务 handler                           │
                │  → ServerMetrics(原子计数+延迟分桶)                │
                │                                                    │
                │  WebSocket: 升级握手 → 帧编解码 → handler 回调     │
                └────────────────────────────────────────────────────┘
```

线程安全机制：
- 跨线程任务投递：`loop->runInLoop(f)` / `queueInLoop(f)` + wakeup
- 连接析构：`HttpServer::onClose` → `queueInLoop(deleteConnection)` → 归属 sub loop 线程执行
- 日志：各 IO 线程写本地 FixedBuffer，AsyncLogging 后端线程 swap + 写文件

---

## 代码目录

```
Airi-Cpp-Server-Lib/
├── CMakeLists.txt               ← 顶层构建，定义所有 MCPP_* 选项
├── README.md                    ← 本文档
├── 日志内容要求.md               ← 开发日志写作规范（day32+ 起强制遵循）
├── day17.5日志示例.md           ← 日志写作的 reference example
│
├── src/
│   ├── include/                 ← 对外公开头文件
│   │   ├── Airi-Cpp-Server-Lib.h     ← 统一入口
│   │   ├── net/                 ← Acceptor / Buffer / Channel / Connection / EventLoop
│   │   │   └── Poller/          ← Epoll / Kqueue / IoUring (day33)
│   │   ├── http/                ← HttpServer / Context / Request / Response / WebSocket(day31)
│   │   ├── log/                 ← Logger / AsyncLogging / LogContext / SteadyClock
│   │   ├── timer/               ← Timer / TimerQueue
│   │   ├── async/               ← Coroutine(day32) / LockFreeQueue / WorkStealingPool (day34)
│   │   └── memory/              ← MemoryPool (day35)
│   ├── common/                  ← 实现文件（与 include/ 镜像）
│   └── test/                    ← 各 GTest 套件 + 服务器/客户端入口
│
├── examples/                    ← 独立示例应用（find_package 使用已安装库）
│   ├── CMakeLists.txt
│   ├── src/http_server.cpp      ← 完整 HTTP + WebSocket 演示
│   ├── static/                  ← 演示用 HTML（含 ws.html WebSocket demo）
│   └── files/                   ← 文件管理演示目录
├── demo/                        ← Phase 4 综合演示
├── benchmark/                   ← 基准测试
│   ├── conn_scale_test.cpp      ← 大规模连接扩展性
│   ├── muduo_compare/           ← Docker + wrk 真实 muduo 对照 (day36)
│   │   ├── Dockerfile           ←   构建 muduo + 本项目 + wrk
│   │   ├── bench_server.cpp     ←   本项目对照服务器（无中间件）
│   │   ├── muduo_bench_server.cc ←  muduo 对照服务器
│   │   └── run_bench.sh         ←   一键 wrk 压测脚本
│   └── benchmark_report.md      ← 历史性能报告
├── HISTORY/                     ← 各阶段历史代码快照
├── dev-log/                     ← 完整开发日志（每 day 一个 .md）
│   └── README.md                ← 日志索引
├── cmake/                       ← CMake 包配置模板
└── imgs/                        ← 文档图片
```

---

## 构建

### 环境要求

| 依赖 | 必需 | 说明 |
|------|------|------|
| CMake ≥ 3.21 | ✅ | 构建系统 |
| C++17 编译器 | ✅ | macOS Apple Clang / Linux GCC ≥ 9 / Clang ≥ 10 |
| C++20 编译器 | 可选 | 启用 `MCPP_ENABLE_COROUTINES` 时需要 |
| pthread | ✅ | POSIX 线程 |
| zlib | 可选 | gzip 中间件 |
| OpenSSL | 可选 | TLS 支持 |
| liburing | 可选 | Linux io_uring 后端（day33）|

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `MCPP_ENABLE_TESTING` | OFF | 启用 GoogleTest 单元测试（FetchContent 自动拉取）|
| `MCPP_ENABLE_OPENSSL` | OFF | 启用 TLS（需系统 OpenSSL）|
| `MCPP_HAS_ZLIB` | OFF | 启用 gzip 压缩（需系统 zlib）|
| `MCPP_ENABLE_COROUTINES` | OFF | 启用 C++20 协程 IO（day32）|
| `MCPP_ENABLE_IO_URING` | OFF | 启用 Linux io_uring 后端（需 liburing 与 5.1+ 内核）|
| `MCPP_ENABLE_ASAN` | OFF | AddressSanitizer |
| `MCPP_ENABLE_TSAN` | OFF | ThreadSanitizer |
| `MCPP_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer |
| `MCPP_ENABLE_COVERAGE` | OFF | 代码覆盖率（gcov/lcov）|

### 命令行构建

```bash
# 完整开发模式：所有特性 + 测试
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Debug \
    -DMCPP_ENABLE_TESTING=ON \
    -DMCPP_ENABLE_COROUTINES=ON \
    -DMCPP_HAS_ZLIB=ON
cmake --build build -j4

# Release 压测模式
cmake -B build-release -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DMCPP_ENABLE_TESTING=ON
cmake --build build-release -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)

# 全套 Sanitizer
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
    -DMCPP_ENABLE_ASAN=ON -DMCPP_ENABLE_UBSAN=ON
cmake --build build -j4
```

### VS Code 任务

`Cmd+Shift+B` 选择任务：

| 任务名 | 说明 |
|--------|------|
| **CMake Configure Debug** | 配置 build/ |
| **Build Core** | 构建核心库与所有测试（默认依赖 Configure）|
| **Install Core Library** | 安装库到 `examples/external/Airi-Cpp-Server-Lib` |
| **Configure App Example** | 配置 `examples/build` |
| **Build App Example** | 构建示例（默认任务）|

### 构建示例应用

```bash
# 1. 构建并安装核心库
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
cmake --install build --prefix examples/external/Airi-Cpp-Server-Lib

# 2. 构建示例
cmake -S examples -B examples/build
cmake --build examples/build -j4

# 3. 运行示例（含 HTTP + WebSocket）
cd examples/build && ./http_server
```

启动后浏览器访问：

| URL | 功能 |
|-----|------|
| `http://127.0.0.1:9090/` | 首页 |
| `http://127.0.0.1:9090/login.html` | 登录表单 |
| `http://127.0.0.1:9090/fileserver` | 文件管理（上传/下载/删除）|
| `http://127.0.0.1:9090/ws.html` | **WebSocket Echo 演示**（day31）|

WebSocket 端点：`ws://127.0.0.1:9090/ws/echo`，发送任意文本会原样回写；发 `[PING]` 字符串会收到 `[PONG] <len>` 模拟应答。

---

## 运行说明

### 主要可执行文件

| 文件 | 说明 |
|------|------|
| `build/server` | 简单 TCP echo 服务器 |
| `build/client` | 配套 TCP 客户端 |
| `build/demo_server` | Phase 4 功能综合演示（限流/鉴权/指标/结构化日志）|
| `build/bench_server` | 性能基准服务器（无中间件，用于 wrk / day36 muduo 对照）|
| `build/StressTest` | TCP echo 并发压力测试 |
| `examples/build/http_server` | 完整 HTTP + WebSocket 应用演示 |

### Phase 4 功能演示

```bash
cd build && ./demo_server &
bash demo/demo_phase4.sh   # 一键体验所有能力
```

```bash
# 可观测性指标
curl http://127.0.0.1:9090/metrics

# 鉴权
curl http://127.0.0.1:9090/api/admin/status                          # → 403
curl -H "Authorization: Bearer demo-token-2024" \
     http://127.0.0.1:9090/api/admin/status                          # → 200
curl -H "X-API-Key: demo-key-001" \
     http://127.0.0.1:9090/api/admin/status                          # → 200

# 限流（连续请求触发 429）
for i in {1..20}; do curl -s -o /dev/null -w "%{http_code}\n" \
    http://127.0.0.1:9090/api/data; done
```

### WebSocket 演示

```bash
# 启动示例服务器
cd examples/build && ./http_server &

# 浏览器打开
open http://127.0.0.1:9090/ws.html

# 或用命令行 (需要 websocat / wscat)
websocat ws://127.0.0.1:9090/ws/echo
> hello
< hello
> [PING]
< [PONG] 6
```

---

## 性能测试

### 单机简易压测（wrk，业界标准）

```bash
# Release 构建
cmake -B build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j4 --target bench_server

# 启动 bench_server（HTTP，无中间件，便于压测）
./build-release/bench_server 9090 4 &

# brew install wrk  /  apt install wrk
wrk -t4 -c100 -d30s --latency http://127.0.0.1:9090/
wrk -t4 -c1000 -d30s --latency http://127.0.0.1:9090/
```

### 与 muduo 真实横向对比（day36，推荐）

由于 muduo 仅支持 Linux，本项目通过 Docker 提供一键复现：

```bash
# 1. 构建测试镜像（首次 5-10 分钟，编译 muduo + 本项目 + wrk）
docker build -f benchmark/muduo_compare/Dockerfile -t mcpp-bench:latest .

# 2. 跑测试（依次启动 bench_server 和 muduo_bench_server，wrk 各压测两轮）
mkdir -p benchmark/muduo_compare/results
docker run --rm \
    -v "$PWD/benchmark/muduo_compare/results:/work/results" \
    mcpp-bench:latest

# 3. 看 summary
cat benchmark/muduo_compare/results/summary.md
```

最近一次实测（2026-04-20，Docker on macOS M3，4 IO 线程，wrk 4 线程，30s）：

| 用例 | bench_server (本项目) RPS | muduo RPS | 比值 |
|------|--------------------------|----------|------|
| keep-alive c=100  | 156,062 | 420,439 | **muduo 快 ~2.7×** |
| keep-alive c=1000 | 133,917 | 419,922 | **muduo 快 ~3.1×** |

详细分析与差距溯源见 [dev-log/day36.md](dev-log/day36.md) 与 [benchmark/muduo_compare/README.md](benchmark/muduo_compare/README.md)。**结论是坦诚的：本项目在短包高并发场景显著落后 muduo，day37+ 将基于 perf 数据逐项优化。**

### 大规模长连接（10k+ 连接 RSS 测量）

```bash
./build/server &
SERVER_PID=$!
./build/conn_scale_test 127.0.0.1 8888 10000 $SERVER_PID
```

### 系统参数优化（压测前）

```bash
ulimit -n 65535
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"   # Linux
sudo sysctl -w net.ipv4.tcp_tw_reuse=1                      # Linux 谨慎
```

---

## 单元测试

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
    -DMCPP_ENABLE_TESTING=ON -DMCPP_ENABLE_COROUTINES=ON
cmake --build build -j4

# 运行所有 (CTest)
cd build && ctest --output-on-failure

# 单独运行
./HttpContextTest
./WebSocketTest
./CoroutineTest
# ...
```

完整测试套件（约 74 个用例 / 17 个套件）：

| 套件 | 用例 | 覆盖核心特性 |
|------|------|-------------|
| HttpContextTest | 3 | HTTP FSM 解析、流水线、分段 body |
| HttpRequestLimitsTest | 4 | 请求行/Header/Body 超限 413 |
| BackpressureDecisionTest | 3 | 高水位暂停/低水位恢复/硬上限断连 |
| CorsMiddlewareTest | 5 | OPTIONS 204、自定义 origin/methods/headers/credentials |
| StaticFileHandlerTest | 8 | ETag→304、Range→206、416、路径遍历 |
| MetricsTest | 6 | 原子计数器、延迟分桶、Prometheus 格式 |
| LogContextTest | 5 | RAII Guard、requestId 递增、线程隔离 |
| SocketPolicyTest | 多 | Socket 策略 |
| TcpServerPolicyTest | 多 | TcpServer 策略 |
| SteadyClockTest | 多 | 单调时钟 |
| TimerTest | 多 | 定时器调度 |
| WebSocketTest | 21 | 帧编解码、握手、各类 opcode、掩码、close |
| **CoroutineTest** | 19 | `Task<T>`、Awaitable、异常透传（day32）|
| **IoUringPollerTest** | 2 | 跨平台 isAvailable / 不崩溃（day33）|
| **LockFreeQueueTest** | 17 | SPSC / MPMC / WorkStealingPool（day34）|
| **MemoryPoolTest** | 14 | FixedSizePool / SlabAllocator / STL 适配器（day35）|

---

## 推荐阅读顺序

1. `src/include/net/TcpServer.h` + `src/common/net/` → 整体控制流
2. `src/include/net/EventLoop.h` + `Poller/` + `Channel.h` → 事件循环与分发
3. `src/include/net/Connection.h` + `Buffer.h` → 连接读写与缓冲管理
4. `src/include/net/EventLoopThread.h` / `EventLoopThreadPool.h` → 多 Reactor 线程模型
5. `src/include/timer/` → 定时器系统
6. `src/include/log/` → 日志系统（含 AsyncLogging）
7. `src/include/http/` → HTTP / WebSocket 协议层与中间件
8. `src/include/async/Coroutine.h` → C++20 协程 IO（day32）
9. `src/include/async/LockFreeQueue.h` + `WorkStealingPool.h` → 无锁结构与调度器（day34）
10. `src/include/memory/MemoryPool.h` → 内存池（day35）
11. `examples/src/http_server.cpp` → 应用层综合示例
12. `dev-log/` → 完整开发过程，每个 day 单独深入

---

## 开发日志

完整开发记录在 [`dev-log/`](dev-log/README.md)，写作规范参见 [`日志内容要求.md`](日志内容要求.md)。最近的高级组件：

- [day31 WebSocket 实现](dev-log/day31.md)
- [day32 C++20 协程 IO](dev-log/day32.md)
- [day33 io_uring 后端](dev-log/day33.md)
- [day34 无锁队列与 Work-Stealing 线程池](dev-log/day34.md)
- [day35 内存池](dev-log/day35.md)
- [day36 与 muduo 横向基准](dev-log/day36.md)

---

## 当前限制

- 监听参数（地址/端口/线程数）已支持环境变量注入，尚无统一命令行参数解析
- 无流式请求体，大文件上传受内存限制
- 限流中间件使用全局 `std::mutex`，>100K QPS 场景可优化为分片锁
- TLS / gzip / 协程 / io_uring 均为可选编译开关，需相应依赖
- io_uring 后端目前仅 stub + 兼容模式，未接入 `Connection::send/recv`（day37+ 计划）
- WebSocket 不支持自定义子协议、压缩扩展（permessage-deflate）

---

## 致谢

学习项目，参考 [muduo 网络库](https://github.com/chenshuo/muduo) 设计思路；协程实现参考 cppcoro 与 folly::coro；无锁队列参考 Vyukov MPMC 算法。仅供学习。
# Airi-Cpp-Server-Lib

一个参考 muduo 思路、基于 Reactor 模型从零实现的 C++ 网络库学习项目。

目标不是复刻完整的工业级框架，而是通过亲手实现所有关键组件，深入理解高并发 TCP/HTTP 服务器的核心机制。

![image-20260407224115056](./imgs/image-20260407224115056.png)

---

## 功能特性

### 网络层

| 功能 | 说明 |
|------|------|
| **跨平台 IO 多路复用** | Linux 使用 epoll，macOS 使用 kqueue |
| **Main + Sub Reactor 多线程** | 主 Reactor accept，N 个子 Reactor 处理 IO（one-loop-per-thread） |
| **非阻塞 IO + 边缘触发** | 循环读到 EAGAIN，循环写到 EAGAIN，ET 模式安全 |
| **自增长 Buffer** | prependable/readable/writable 三段，readv + 栈溢出缓冲 |
| **EventLoopThread / EventLoopThreadPool** | 每个 IO 线程内部构造自己的 EventLoop，线程归属正确 |
| **跨线程安全** | `runInLoop` / `queueInLoop` + wakeup 机制，非 IO 线程任务投递 |
| **连接安全销毁** | `queueInLoop` 延迟删除，Channel* 在当轮 events 遍历结束后释放 |
| **连接回压保护** | 输出缓冲高/低水位自动暂停/恢复读事件；硬上限保护性断连 |
| **最大连接数保护** | 超出上限主动拒绝新连接并关闭 fd |
| **accept 非致命错误处理** | 可恢复错误忽略记录，不触发进程退出 |
| **epoll 运行期错误降级** | ADD/MOD 冲突自愈重试，DEL 可恢复错误忽略 |
| **sendfile 零拷贝** | Linux sendfile(2) 快路径 + pread 兜底；TLS 模式自动降级 |
| **优雅关闭** | SIGINT 触发 stop()，wakeup 唤醒阻塞循环 |

### 定时器

| 功能 | 说明 |
|------|------|
| **跨平台定时器** | TimeStamp / Timer / TimerQueue，基于 poll 超时驱动，无额外 fd |
| **空闲连接自动关闭** | `HttpServer::setAutoClose`，`weak_ptr<bool>` alive flag 安全超时 |

### 日志

| 功能 | 说明 |
|------|------|
| **四层日志系统** | FixedBuffer → LogStream → Logger → AsyncLogging |
| **异步日志** | 双缓冲区，独立写线程，业务线程零阻塞 |
| **结构化日志上下文** | `thread_local` LogContext，每条请求日志自动携带 `[req-N METHOD /url]` |
| **单调时钟** | `steady_clock` 替代 wall clock 用于延迟/超时计算，免疫 NTP 跳变 |

### HTTP 层

| 功能 | 说明 |
|------|------|
| **HTTP/1.1 解析** | HttpContext 有限状态机，流水线安全消费（按消费字节 retrieve）|
| **HTTP 应用服务器** | 精确路由 + 前缀路由，中间件链（洋葱模型）|
| **写事件异步发送** | 大响应首次直接写，未写完注册 EPOLLOUT，避免阻塞事件循环 |
| **请求体大小限制** | `HttpRequestLimits`，超限返回 413 |

### 中间件与可观测性（Phase 2~4）

| 功能 | 说明 |
|------|------|
| **可观测性指标** | `/metrics` Prometheus text exposition format，lock-free 原子计数器 + 延迟分桶 |
| **令牌桶限流** | 每 IP 独立限流，可配置容量与速率，超限 429 + `Retry-After` |
| **鉴权中间件** | Bearer Token + API Key 双模式，白名单路径/前缀免鉴权 |
| **CORS 中间件** | OPTIONS 预检自动 204，Fluent API 配置来源/方法/头/凭证 |
| **gzip 压缩** | 响应体自动压缩，按 Content-Type + 最小阈值决策，需 zlib（可选）|
| **静态文件服务** | ETag / If-None-Match → 304，Range → 206 / 416，MIME 自动推断，路径遍历防护 |
| **TLS (HTTPS)** | 可选 OpenSSL 集成，TLS 握手与加密读写透明集成 |

---

## 架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                            TcpServer                                │
│    maxConnections=10000   IO Threads=N（默认=CPU核数）               │
└──────────────────────┬──────────────────────────────────────────────┘
                       │
          ┌────────────┴────────────┐
          │                         │
 ┌────────┴───────┐       ┌─────────┴──────────────┐
 │  Main Reactor  │       │  EventLoopThreadPool    │
 │  (accept 线程) │       │  Sub Reactor × N        │
 └────────┬───────┘       └─────────┬──────────────┘
          │ accept() + 轮询分配 fd            │
          └───────────────────────────────────┘
                                              │
                     ┌────────┬───────────────┤
                     │        │               │
                 subLoop0  subLoop1  ...   subLoopN-1
                     │
                  Channel(fd 事件分发)
                     │
                  Connection(读写状态机 + Buffer)
                     │
               ┌─────┴──────────────────────────────┐
               │           HttpServer                │
               │  HttpContext(FSM 解析)              │
               │  → 中间件链(限流→鉴权→CORS→Gzip→...) │
               │  → Router(精确/前缀路由匹配)         │
               │  → 业务 handler                     │
               │  → ServerMetrics(原子计数+延迟分桶)  │
               └────────────────────────────────────┘
```

线程安全机制：
- **跨线程任务投递**：`loop->runInLoop(f)` 在 IO 线程执行；`queueInLoop(f)` 加入待执行队列并 wakeup
- **连接析构**：`HttpServer::onClose` → `queueInLoop(deleteConnection)` → 在归属 sub loop 线程执行
- **日志**：前端 `Logger`（各 IO 线程写到各自 FixedBuffer）→ `AsyncLogging` 后端线程 swap + 写文件

---

## 代码目录

```
Airi-Cpp-Server-Lib/
├── CMakeLists.txt               ← 顶层构建，支持 MCPP_* 选项
├── README.md
├── src/
│   ├── include/                 ← 对外公开头文件
│   │   ├── Airi-Cpp-Server-Lib.h     ← 统一入口头文件
│   │   ├── net/                 ← 网络层头文件（Acceptor/Buffer/Channel/...）
│   │   │   └── Poller/          ← EpollPoller.h / KqueuePoller.h / Poller.h
│   │   ├── http/                ← HTTP 层头文件（HttpServer/Context/Request/Response/...）
│   │   ├── log/                 ← 日志系统头文件（Logger/AsyncLogging/LogContext/...）
│   │   └── timer/               ← 定时器头文件（TimeStamp/Timer/SteadyClock/...）
│   ├── common/                  ← 实现文件（与 include/ 目录对应）
│   │   ├── net/
│   │   │   └── Poller/          ← DefaultPoller.cpp / epoll/ / kqueue/
│   │   ├── http/
│   │   ├── log/
│   │   └── timer/
│   └── test/                    ← 测试与示例入口（server/client/各类 Test）
├── examples/                    ← 独立示例应用（通过 find_package 使用已安装的库）
│   ├── CMakeLists.txt
│   ├── src/http_server.cpp      ← 完整 HTTP 应用演示
│   ├── static/                  ← 演示用静态资源
│   └── files/                   ← 演示用文件管理目录
├── demo/                        ← Phase 4 功能综合演示
│   ├── demo_server.cpp          ← 限流/鉴权/指标/结构化日志完整演示
│   └── demo_phase4.sh           ← 自动化演示脚本
├── HISTORY/                     ← 各开发阶段历史代码快照（day1~day26）
├── dev-log/                     ← 完整开发日志（每阶段独立 .md 文件）
│   └── README.md                ← 日志索引
├── cmake/                       ← CMake 包配置模板
└── imgs/                        ← 文档图片资源
```

---

## 构建

### 环境要求

- CMake ≥ 3.21
- C++17 编译器（macOS: Apple Clang；Linux: GCC ≥ 9 或 Clang ≥ 10）
- POSIX 线程库（pthread）
- 可选：zlib（gzip 中间件）、OpenSSL（TLS）

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `MCPP_ENABLE_TESTING` | OFF | 启用 GoogleTest 单元测试（自动 FetchContent） |
| `MCPP_ENABLE_OPENSSL` | OFF | 启用 TLS 支持（需系统安装 OpenSSL）|
| `MCPP_ENABLE_ASAN` | OFF | 启用 AddressSanitizer |
| `MCPP_ENABLE_TSAN` | OFF | 启用 ThreadSanitizer |
| `MCPP_ENABLE_UBSAN` | OFF | 启用 UndefinedBehaviorSanitizer |
| `MCPP_ENABLE_COVERAGE` | OFF | 启用代码覆盖率（gcov/lcov）|
| `MCPP_HAS_ZLIB` | OFF | 启用 gzip 压缩中间件（需系统安装 zlib）|

### VS Code 任务（推荐方式）

使用 `Cmd+Shift+B` 选择任务，或通过终端面板运行：

| 任务名 | 说明 |
|--------|------|
| **Build Debug** | Configure + Build（Debug 模式，构建到 build/）|
| **Build Release** | Configure + Build（Release 模式，构建到 build-release/）|
| **Run Tests** | 在 build/ 中运行 CTest（需先 Build Debug）|
| **Install Core Library** | 安装库到 examples/external/Airi-Cpp-Server-Lib/ |
| **Configure Examples** | 配置 examples/ 子项目 |
| **Build Examples** | 构建 examples/（默认构建任务，自动执行所有依赖）|

### 命令行构建

```bash
# Debug 模式（开发 / Sanitizer）
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_TESTING=ON
cmake --build build -j4

# Release 模式（压测 / 生产）
cmake -B build-release -S . -DCMAKE_BUILD_TYPE=Release -DMCPP_ENABLE_TESTING=ON
cmake --build build-release -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)

# 运行所有单元测试
cd build && ctest --output-on-failure

# AddressSanitizer
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_ASAN=ON
cmake --build build -j4
cd build && ctest --output-on-failure
```

### 构建示例应用

```bash
# 1. 构建并安装核心库
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
cmake --install build --prefix examples/external/Airi-Cpp-Server-Lib

# 2. 构建示例
cmake -S examples -B examples/build
cmake --build examples/build -j4

# 3. 运行示例 HTTP 服务器
./examples/build/http_server
```

---

## 推荐阅读顺序

1. `src/include/net/TcpServer.h` + `src/common/net/` → 整体控制流
2. `src/include/net/EventLoop.h` + `Poller/` + `Channel.h` → 事件循环与分发
3. `src/include/net/Connection.h` + `Buffer.h` → 连接读写与缓冲管理
4. `src/include/net/EventLoopThread.h` + `EventLoopThreadPool.h` → 多 Reactor 线程模型
5. `src/include/timer/` → 定时器系统
6. `src/include/log/` → 日志系统
7. `src/include/http/` → HTTP 协议层与中间件
8. `examples/src/http_server.cpp` → 应用层综合示例
9. `dev-log/` → 完整开发过程与架构解析

---

## 开发日志

完整的开发过程记录在 [dev-log/](dev-log/README.md) 目录，按阶段分文件整理，包含：
- 每个阶段的知识背景与设计思路
- 关键代码解析与架构图
- 对象所有权与生命周期分析
- 运行时工作流程的逐步追踪

---

## 当前限制

- 监听参数（地址/端口/线程数）已支持环境变量注入，尚无统一命令行参数解析
- 无流式请求体，大文件上传受内存限制
- 限流中间件使用全局 `std::mutex`，>100K QPS 场景可优化为分片锁
- TLS 需系统安装 OpenSSL 并以 `-DMCPP_ENABLE_OPENSSL=ON` 构建
- gzip 中间件需 zlib（`-DMCPP_HAS_ZLIB=ON`）

---

学习项目，参考 [muduo 网络库](https://github.com/chenshuo/muduo) 设计思路，仅供学习使用。
