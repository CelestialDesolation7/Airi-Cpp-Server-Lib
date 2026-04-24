# Airi-Cpp-Server-Lib 面试演示手册

> 面向面试场景的"按图索骥"操作清单。每节都给出可直接复制粘贴的命令。
> 适用版本：day30 收官状态及之后任意提交。
> 仓库根目录假定为 `~/Desktop/MyCppServerLib`，文中以 `$REPO` 代称。

---

## 0. 30 秒电梯介绍

> "Airi-Cpp-Server-Lib 是参考 muduo 设计、从零实现的主从 Reactor 网络库，
> 跨平台 epoll / kqueue 多路复用，提供完整 HTTP/1.x 应用层（Keep-Alive、
> ETag/304、Range/206、sendfile 零拷贝），可组合的中间件管道（per-IP 令牌桶
> 限流、Bearer / API Key 鉴权、CORS、gzip），TLS 通过 OpenSSL 可选接入。
> 34 个 GoogleTest 用例 + GitHub Actions 7-job CI（ASan/TSan/UBSan/coverage/clang-tidy/benchmark）。
> 8 核 macOS 上峰值 18.8 万 QPS，P99 2.15 ms。"

如果面试官追问，按下面分章节展开。

---

## 1. 仓库结构速览

```
$REPO/
├── src/
│   ├── include/                    # 公开头文件（按模块分目录）
│   │   ├── base/                   # NonCopyable / SteadyClock 等基础设施
│   │   ├── log/                    # AsyncLogging / Logger / LogContext
│   │   ├── timer/                  # TimerQueue / Timer
│   │   ├── net/                    # EventLoop / Channel / TcpServer / Connection / Buffer
│   │   ├── net/Poller/             # 跨平台多路复用：EpollPoller / KqueuePoller
│   │   └── http/                   # HttpServer / HttpContext / 中间件 / StaticFileHandler / ServerMetrics
│   ├── common/                     # 上述 .h 对应的实现 .cpp
│   ├── linux/ + src/mac/           # 平台特化：epoll / kqueue 各编一份
│   └── test/                       # GoogleTest 单元测试（10 套件 / 34 case）
├── examples/
│   ├── src/http_server.cpp         # 全特性演示服务器（中间件链 + 静态文件 + 指标）
│   ├── static/index.html           # 演示前端（前后端已分离，仅展示端点 + curl 示例）
│   └── files/                      # 静态文件示例（readme.txt / scores.csv / server.log）
├── demo/                           # Phase 4 早期演示，默认 EXCLUDE_FROM_ALL
├── benchmark/                      # 长连接 / QPS 压测脚本与基线报告
├── cmake/                          # find_package 模板
├── .github/workflows/ci.yml        # 7 项并行 CI Job
├── .vscode/tasks.json              # VS Code 一键 build / run / test 任务
├── dev-log/                        # day01 → day36 实现日志
├── HISTORY/                        # day01 → day36 每日只读快照
└── INTERVIEW_GUIDE.md              # ← 当前文件
```

> **主分支代码停留在 day30**。day31–day36 的 6 项加分实验（WebSocket / C++20 协程 /
> io_uring / 无锁队列 / 内存池 / muduo 横向基准）只在 `HISTORY/day31..day36/` 与
> `dev-log/day3X-*.md` 中作为实验分支保留，不在主代码树里。

**口播一句话**：核心代码在 `src/`，演示在 `examples/`，测试与 CI 全自动。

---

## 2. 三分钟构建 & 运行

### 2.1 命令行

```bash
cd $REPO
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)

# 跑全部单元测试（应 100% PASS）
cd build && ctest --output-on-failure -j8
# 期望：100% tests passed, 0 tests failed out of 34

# 启动演示服务器（默认 127.0.0.1:8888）
./examples/http_server &
SERVER_PID=$!

# 浏览器打开 http://127.0.0.1:8888 看到 demo 首页（端点目录 + curl 示例）
curl -i http://localhost:8888/health                                    # 200
curl -i http://localhost:8888/api/users                                 # 鉴权失败 → 403
curl -i -H "Authorization: Bearer demo-token-2024" \
     http://localhost:8888/api/users                                    # → 200
curl -i -X OPTIONS -H "Origin: https://example.com" \
     -H "Access-Control-Request-Method: GET" \
     http://localhost:8888/api/users                                    # 预检 → 204

kill $SERVER_PID
```

### 2.2 VS Code 一键

打开命令面板 → `Tasks: Run Task`：

| 任务 | 作用 |
| --- | --- |
| `CMake Configure` | 生成 `build/`（Debug 默认） |
| `Build`（默认 build 任务，⇧⌘B） | 编译 NetLib + GTest 套件 + `examples/http_server` |
| `Run Demo Server` | 编译并启动演示服务器 |
| `CTest`（默认 test 任务） | 跑全部 GoogleTest |
| `Clean` | 删除整个 `build/` 强制全量重建 |

### 2.3 build/ 目录约定

构建产物分类输出，避免根目录一片混乱：

```
build/
├── examples/   # 面向用户的可执行：http_server / demo_server
│   ├── http_server      ← 主演示服务器
│   ├── static/          ← POST_BUILD 自动从 examples/static 复制过来
│   └── files/           ← POST_BUILD 自动从 examples/files 复制过来
├── tests/      # GoogleTest 二进制（ctest 默认从这里发现）
├── tools/      # 手动跑的小工具：StressTest / TimerTest / LogTest
└── _deps/      # FetchContent 拉的 GoogleTest 等
```

> 注：`tools/` 下的目标加了 `EXCLUDE_FROM_ALL`，默认不会被构建。
> 需要时显式 `cmake --build build --target StressTest`。

**面试官常问**：`-DCMAKE_BUILD_TYPE=Release` 与 `Debug` 的区别？
> Release 默认 `-O3 -DNDEBUG`，关闭 assert，开启高级优化（向量化、循环展开）；
> Debug 是 `-O0 -g`，调试信息完整。Sanitizer 必须用 Debug 配置。

---

## 3. 单元测试（GoogleTest）

### 3.1 列出全部

```bash
cd $REPO/build
ctest -N           # -N = no-execute，仅列出
```

期望（部分）：

```
  Test #1: HttpContextTest.ParseSimpleGet
  Test #2: HttpContextTest.ParseChunkedBody
  ...
Total Tests: 34
```

### 3.2 跑单个测试 / 单个用例

```bash
cd $REPO/build
ctest -R HttpContextTest --output-on-failure   # 跑名字含 HttpContextTest 的全部
./tests/HttpContextTest                         # 直接跑二进制
./tests/HttpContextTest --gtest_filter="*ChunkedBody"
./tests/HttpContextTest --gtest_list_tests      # 列出该二进制内部所有 TEST
```

### 3.3 关键测试用例导览

| 测试套件 | 验证什么 | 何时引用 |
| --- | --- | --- |
| `HttpContextTest` | 状态机解析正确性、limits 边界 | 面试官问 HTTP 解析 |
| `HttpRequestLimitsTest` | 行 / 头 / 体三层限流均能触发 413 | 问"如何防 OOM 攻击" |
| `BackpressureDecisionTest` | 高水位回调触发条件 | 问回压机制 |
| `StaticFileHandlerTest` | ETag / 304；Range / 206；路径遍历 → 400 | 问 HTTP 缓存协商 |
| `CorsMiddlewareTest` | OPTIONS 预检短路 → 204；Allow-Origin 匹配 | 问跨域 |
| `MetricsTest` | 计数器原子递增、Prometheus 文本格式 | 问性能监控 |
| `LogContextTest` | 进程内 traceId / requestId 跨函数透传 | 问日志关联 |

---

## 4. Sanitizer 三件套

### 4.1 AddressSanitizer（堆 / 栈越界、UAF、泄漏）

```bash
cd $REPO
rm -rf build-asan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_ASAN=ON
cmake --build build-asan -j8
cd build-asan && ctest --output-on-failure -j4
```

**面试官追问**：ASan 怎么找堆越界？
> ASan 在分配的内存周围插入"红区"（红色保护带），并把 free 后的内存放进延迟释放
> 的"隔离区"（quarantine）。读写红区或隔离区会立即报告。运行期开销约 2× 时间、3× 内存。

### 4.2 ThreadSanitizer（数据竞争）

```bash
cd $REPO && rm -rf build-tsan
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_TSAN=ON
cmake --build build-tsan -j8
cd build-tsan && ctest --output-on-failure -j2   # TSan 不要并发太高
```

**面试官追问**：TSan 与 ASan 能否同时开？
> 不能。两者都使用 shadow memory 但布局冲突，必须分开构建。

### 4.3 UndefinedBehaviorSanitizer（整数溢出、空指针解引用、左移负数）

```bash
cd $REPO && rm -rf build-ubsan
cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_UBSAN=ON
cmake --build build-ubsan -j8
cd build-ubsan && ctest --output-on-failure -j8
```

**口播**：UBSan 比 ASan 轻量，运行期开销可忽略，建议长期开发分支默认开启。

---

## 5. 代码覆盖率（gcov + lcov）

仅 Linux / GCC（macOS 上 lcov 与 gcov-13 兼容性差）。在 GitHub Actions 上自动跑。本地手动：

```bash
brew install lcov   # 或 apt install gcc-11 lcov

cd $REPO && rm -rf build-cov
CC=gcc-11 CXX=g++-11 cmake -S . -B build-cov \
    -DCMAKE_BUILD_TYPE=Debug -DMCPP_ENABLE_COVERAGE=ON
cmake --build build-cov -j8
cd build-cov && ctest -j8 || true   # 容忍个别失败，仍能产出报告

lcov --capture --directory . --output-file coverage.info \
     --gcov-tool gcov-11 --rc lcov_branch_coverage=0 \
     --ignore-errors mismatch,unused,empty
lcov --remove coverage.info '/usr/*' '*/_deps/*' '*/test/*' \
     --output-file coverage.filtered.info
genhtml coverage.filtered.info --output-directory cov-report
open cov-report/index.html
```

---

## 6. 静态分析（clang-tidy）

```bash
cd $REPO
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j8                        # 先编译生成所有头文件
cmake --build build --target clang-tidy        # 触发自定义 target，扫描全 src/
# 或直接：
clang-tidy --quiet -p build src/common/net/Connection.cpp
```

`.clang-tidy` 主要打开 `cppcoreguidelines-*`、`bugprone-*`、`performance-*`、`modernize-*` 几大类。
项目根目录的 `compile_commands.json` 由 CMake 自动同步给 LSP / clang-tidy 使用。

---

## 7. 性能基准

### 7.1 wrk 复现简历数据

```bash
brew install wrk     # 或 apt install wrk
cd $REPO/build
./examples/http_server &
sleep 1
wrk -t4 -c100 -d10s --latency http://127.0.0.1:8888/health
kill %1
```

简历数字（8 核 macOS, M-series）：

- 峰值 QPS：**18.8 万 req/s**
- P99 延迟：**2.15 ms**
- 单连接内存：**3.72 KB**（10 000 并发，RSS 线性增长无泄漏）
- 峰值 CPU：**22%**（仅 2 个 IO 线程）

### 7.2 长连接规模基线

```bash
cd $REPO
g++ -O2 -std=c++17 benchmark/conn_scale_test.cpp -o /tmp/conn_scale
./build/examples/http_server &
/tmp/conn_scale 10000 127.0.0.1 8888
```

逐步爬升 1000 → 5000 → 10 000 并发，对比 RSS。

---

## 8. CI 工作流速查

`.github/workflows/ci.yml` 共 **7 个并行 Job**：

| Job | 作用 | 失败常见原因 |
| --- | --- | --- |
| `build-and-test (ubuntu-latest)` | Linux Release 构建 + ctest | 平台特定头文件遗漏 |
| `build-and-test (macos-latest)` | macOS Release 构建 + ctest | kqueue 接口差异 |
| `sanitizer-asan` | Debug + ASan 全测试 | 内存泄漏 / UAF |
| `sanitizer-tsan` | Debug + TSan 全测试 | 数据竞争 / 死锁 |
| `sanitizer-ubsan` | Debug + UBSan 全测试 | 整数溢出 / 空指针 |
| `coverage` | gcov + lcov 上传 artifact | lcov 版本不兼容 |
| `static-analysis` | clang-tidy 全文件 | 任何新引入的 warning |
| `benchmark-regression` | QPS < 阈值告警 | http_server 启动失败 |

**口播**：每次 push 到 main / PR 都触发，平均 8 分钟跑完。

---

## 9. 高频追问 & 标准回答

### Q1：为什么自己写 Reactor 而不直接用 boost::asio / muduo？
> 学习目标驱动。从零实现能让我准确理解：non-blocking + epoll + 状态机这条主线
> 是怎么衔接的；asio 的 executor / strand / awaitable 抽象层次太高，看不到底层。
> 生产环境我会用 asio 或 muduo。

### Q2：epoll 的 LT 和 ET 有什么区别？项目用哪个？
> LT（Level Triggered）默认模式：只要 fd 可读 / 可写就持续通知，编程容易，不易丢事件。
> ET（Edge Triggered）仅状态变化时通知一次，必须循环读到 EAGAIN 才能确保读完，
> 吞吐更高但难写。
> 项目 **EpollPoller 用 LT**（兼容性好），**Connection::handleRead 用 while-loop +
> EAGAIN 退出**模拟 ET 语义，两者结合：行为正确 + 吞吐接近 ET。

### Q3：Connection 的生命周期怎么管？
> `std::shared_ptr<Connection>` + `enable_shared_from_this`。
> Channel 注册的回调通过 `weak_ptr` 弱持有 Connection，每次回调里 `lock()`
> 提升为 shared_ptr，避免"Connection 已析构但 Channel 还在 epoll 中"的悬挂。
> 关闭时通过 `EventLoop::queueInLoop` 把销毁延后到下一轮事件循环，
> 确保正在执行的回调安全完成。

### Q4：HTTP 解析为什么用状态机？
> HTTP/1.x 报文边界由分隔符（`\r\n`）和长度字段决定，**TCP 是流式的，会发生分包/粘包**。
> 状态机模型保证：每收到一个字节就推进一次状态，不要求一次拿到完整报文。
> 状态：`kExpectRequestLine` → `kExpectHeaders` → `kExpectBody` → `kGotAll` / `kInvalid`。
> 同时在每个状态入口累加字节计数 → 三层限流（行 / 头 / 体）一站式实现。

### Q5：中间件顺序为什么是 AccessLog → RateLimiter → CORS → Auth → Gzip？
> **顺序敏感**，写测试固化了：
> - **CORS 必须在 Auth 之前**：否则 OPTIONS 预检会被 Auth 拦成 401/403，浏览器跨域直接挂。
> - **Gzip 必须在最里层**：否则压缩数据会被后续中间件再处理一次。
> - **RateLimiter 在 AccessLog 之后**：被限流的 429 也得记访问日志。

### Q6：令牌桶 vs 漏桶 vs 滑动窗口？
> **令牌桶**：允许突发（桶可瞬时满），长期速率不超 refillRate。本项目使用。
> **漏桶**：请求强制以恒定速率流出，平滑但不允许突发。
> **滑动窗口**：精确，但需要 O(N) 时间或环形缓冲。
> API 网关场景令牌桶最常见——既保护后端又允许合理突发。

### Q7：Bearer Token vs API Key？为什么三档查找？
> Bearer 通常承载 JWT，含到期时间和签名，**有状态**（短期）。
> API Key 是固定字符串，**无状态**（长期），适合机器间调用。
> 优先级：Authorization Header（最规范）> X-API-Key Header（不进 URL log）
> > `?api_key=` 查询参数（兼容旧客户端）。
> 失败返回 **403 Forbidden** 而非 401 —— 401 必须带 `WWW-Authenticate`，会触发浏览器原生登录框。

### Q8：sendfile 零拷贝省了几次拷贝？
> 传统 read+write：4 次上下文切换 + 2 次内核↔用户态拷贝。
> sendfile：2 次上下文切换 + 1 次拷贝（从 page cache 直接到 socket buffer）。
> 100 MB 文件下载，CPU 利用率从 100% 降到约 30%。

---

## 10. 万一现场出错的兜底应对

| 现象 | 可能原因 | 应对 |
| --- | --- | --- |
| `cmake` 找不到 OpenSSL | macOS 默认无 OpenSSL | `brew install openssl@3 && cmake -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) ...` |
| `ctest` 全挂 | 没先 `cmake --build` | 先 build 再 test |
| ASan / TSan 编译报 `__asan_init` 未定义 | 编译器与运行时不匹配 | 切到 GCC 而非 Apple clang，或装 LLVM |
| `port already in use` | 上次的 server 没杀 | `lsof -i :8888` + `kill <pid>` |
| `./examples/http_server` 提示 no such file | 还没 build 或路径错 | 先 `cmake --build build`，二进制在 `build/examples/http_server` |
| clang-tidy 找不到 `compile_commands.json` | 没开 `CMAKE_EXPORT_COMPILE_COMMANDS` | 配置时加 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`（默认已开） |

---

## 11. 一句话总结

> "这个项目的设计目标是让面试官看到：我能从 epoll 系统调用一直串到 HTTP 中间件链，
> 中间任何一层（线程模型、状态机、回压、限流、鉴权、零拷贝）都能展开讲；
> 同时它的工程化（CI、Sanitizer、覆盖率、静态分析）证明我不是只写'能跑'的代码。"

祝面试顺利。
