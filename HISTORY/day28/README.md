# Day 28 — 测试框架与 CI（Phase 3）

将核心决策逻辑提取为纯策略函数，编写不依赖网络的单元测试；引入回压（backpressure）机制和连接上限保护；移除 util.cpp/util.h；新增外部库使用示例 `app_example/`。

## 新增 / 变更模块

| 文件 | 说明 |
|------|------|
| `include/Connection.h` | `BackpressureConfig` / `BackpressureDecision` 结构体；`evaluateBackpressure()` 纯策略函数 |
| `include/TcpServer.h` | `Options` 配置结构体；`shouldRejectNewConnection()` / `normalizeIoThreadCount()` 纯策略函数 |
| `include/Socket.h` | 所有操作改为返回 `bool` |
| `include/Poller/EpollPoller.h` | `shouldRetryWithMod/Add()` / `shouldIgnoreCtlError()` 策略函数 |
| `include/http/HttpContext.h` | `parse()` 新增 `consumed` 出参，支持 HTTP pipeline |
| `test/BackpressureDecisionTest.cpp` | 回压策略纯函数测试 |
| `test/EpollPolicyTest.cpp` | epoll 自愈策略测试（macOS 自动跳过） |
| `test/HttpContextTest.cpp` | HTTP 解析器测试：pipeline / 分段 body / 非法方法 |
| `test/SocketPolicyTest.cpp` | Socket 返回值语义测试 |
| `test/TcpServerPolicyTest.cpp` | 连接上限 / IO 线程归一化测试 |
| `test/server.cpp` | 从顶层移入 test/，改用 Options + 环境变量配置 |
| `test/client.cpp` | 从顶层移入 test/，精简为独立测试工具 |
| `app_example/` | 外部库使用示例（find_package + link） |
| ~~`util.cpp` / `util.h`~~ | **已移除**（errif 全部替换为 Logger） |

## 构建 & 运行

```bash
cd HISTORY/day28
cmake -S . -B build && cmake --build build -j4

# 纯策略测试（不需要网络）
./build/BackpressureDecisionTest
./build/TcpServerPolicyTest
./build/SocketPolicyTest
./build/HttpContextTest

# Linux 专属测试（macOS 自动跳过）
./build/EpollPolicyTest

# 启动 server + client
./build/server &
./build/client
```

## 可执行文件

| 名称 | 说明 |
|------|------|
| `server` | TCP Echo 服务器 (test/) |
| `client` | TCP 客户端 (test/) |
| `BackpressureDecisionTest` | 回压策略决策测试 |
| `EpollPolicyTest` | epoll 自愈重试策略测试 |
| `HttpContextTest` | HTTP 解析器测试 |
| `SocketPolicyTest` | Socket 返回值语义测试 |
| `TcpServerPolicyTest` | 连接上限 / 线程归一化测试 |
| `LogTest` | 日志系统测试 |
| `TimerTest` | 定时器测试 |
| `ThreadPoolTest` | 线程池测试 |
| `StressTest` | 压力测试客户端 |
| `BenchmarkTest` | HTTP 压测工具 |

## 核心设计

- **纯策略函数**：`evaluateBackpressure()` / `shouldRejectNewConnection()` 等零副作用纯函数，可直接在单元测试中覆盖全部边界
- **回压机制**：low/high/hard 三级水位线，自动暂停/恢复/断连
- **连接上限**：`maxConnections` 超限时直接 close(fd)，保护服务器
- **HTTP Pipeline**：`consumed` 出参精确追踪每条请求消费的字节数
- **外部库示例**：`app_example/` 演示 find_package + target_link_libraries 完整集成流程
