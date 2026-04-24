# Day 27 — 调试与性能优化

针对 Day 26 HTTP 应用在压测中暴露的性能瓶颈进行系统优化：静态文件内存缓存、Connection::send 移动语义、HttpContext 解析热路径加速、Socket 错误诊断增强。

## 变更模块

| 文件 | 说明 |
|------|------|
| `http_server.cpp` | 启动时预加载静态文件到内存缓存 (`g_staticCache`)，消除请求热路径磁盘 I/O |
| `include/Connection.h` | 新增 `send(std::string&&)` 移动重载 |
| `common/Connection.cpp` | 实现 send 移动语义：直写路径零拷贝 |
| `include/http/HttpContext.h` | 新增 `bodyLen_` 成员缓存 Content-Length |
| `common/http/HttpContext.cpp` | Content-Length 仅查找一次并缓存，消除热路径哈希查找 |
| `common/Socket.cpp` | 所有操作增加 `errno` + `strerror` 诊断日志 |
| `common/Poller/kqueue/KqueuePoller.cpp` | kevent 错误日志增强 |
| `test/StressTest.cpp` | 统计数据增加最大/最小延迟 |

## 构建 & 运行

```bash
cd HISTORY/day27
cmake -S . -B build && cmake --build build -j4

# 启动服务器
./build/http_server

# 压测（对比 day26 QPS）
./build/BenchmarkTest 127.0.0.1 8888 / 4 10
```

## 可执行文件

| 名称 | 说明 |
|------|------|
| `http_server` | HTTP 文件服务器（带静态缓存） |
| `server` | Echo TCP 服务器 |
| `client` | TCP 客户端 |
| `BenchmarkTest` | 多线程 HTTP 压测工具 |
| `LogTest` | 日志系统测试 |
| `TimerTest` | 定时器测试 |
| `ThreadPoolTest` | 线程池测试 |
| `StressTest` | 压力测试客户端 |

## 核心优化

| 优化 | 效果 |
|------|------|
| `g_staticCache` 预加载 | 请求热路径零磁盘 I/O，QPS +15-20% |
| `send(string&&)` | 小响应直写路径零拷贝，避免 OutputBuffer 中转 |
| `bodyLen_` 缓存 | 大文件分段上传消除重复 unordered_map 查找 |
| Socket 诊断 | errno + strerror 详细日志，快速定位网络故障 |
