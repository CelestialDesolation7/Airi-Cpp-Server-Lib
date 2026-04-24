# Day 24 — 异步日志系统（AsyncLogging）

构建生产级异步日志框架：同步前端（Logger + LogStream）+ 异步后端（AsyncLogging 双缓冲区）+ 文件写入器（LogFile 自动滚动）。

## 新增模块

| 文件 | 说明 |
|------|------|
| `include/log/LogStream.h` | FixedBuffer + LogStream 流式格式化 |
| `common/log/LogStream.cpp` | 各类型 operator<< 实现 |
| `include/log/Logger.h` | 同步日志前端 + LOG_* 宏 |
| `common/log/Logger.cpp` | 时间戳 + 线程编号 + 级别组装 |
| `include/log/LogFile.h` | 日志文件写入器（自动滚动） |
| `common/log/LogFile.cpp` | fopen/fwrite + rollFile |
| `include/log/AsyncLogging.h` | 异步后端（双缓冲 + 后端写线程） |
| `common/log/AsyncLogging.cpp` | 前端 append + 后端 threadFunc |
| `include/Latch.h` | CountdownLatch 启动同步 |
| `test/LogTest.cpp` | 日志系统测试 |

## 构建 & 运行

```bash
cd HISTORY/day24
cmake -S . -B build && cmake --build build -j4

# 运行日志测试
./build/LogTest

# 运行 echo 服务器
./build/server
```

## 可执行文件

| 名称 | 说明 |
|------|------|
| `server` | Echo 服务器（带定时器 + 日志） |
| `client` | TCP 客户端 |
| `LogTest` | 日志系统测试（同步 + 异步 + 多线程） |
| `TimerTest` | 定时器测试 |
| `ThreadPoolTest` | 线程池测试 |
| `StressTest` | 压力测试客户端 |

## 核心设计

- **零堆分配前端**：FixedBuffer 栈分配，LOG_INFO 一次调用 ~200ns
- **双缓冲交换**：前端 append 持锁仅 memcpy，后端无锁批量 fwrite
- **LOG_DEBUG 零开销**：级别不足时 Logger 不构造，用户表达式不求值
- **自动滚动**：文件超过 rollSizeBytes 后创建新文件（basename.YYYYMMDD_HHMMSS.log）
