# Day 21 — EventLoopThread + EventLoopThreadPool（one-loop-per-thread）

## 核心变更
- **新增 `EventLoopThread`**：将 EventLoop 与线程绑定，EventLoop 在子线程内构造，`tid_` 始终正确
- **新增 `EventLoopThreadPool`**：管理 N 个 IO 线程，`nextLoop()` 轮询分配连接，`stopAll()` / `joinAll()` 安全关闭
- **EventLoop 增强**：新增 `isInLoopThread()` / `runInLoop()`；`quit_` 改为 `atomic<bool>`；成员改 `unique_ptr`
- **TcpServer 重构**：用 `EventLoopThreadPool` 替代 `ThreadPool + vector<Eventloop>`
- **析构安全**：`~TcpServer()` 先 `stop()` 再 `joinAll()`，确保 Connection 析构时 EventLoop 仍存活

## 构建

```bash
cmake -S . -B build
cmake --build build -j4
```

生成 `server`、`client`、`ThreadPoolTest`、`StressTest` 四个可执行文件。

## 文件结构

```
├── server.cpp / client.cpp         入口
├── include/
│   ├── TcpServer.h                 使用 EventLoopThreadPool
│   ├── EventLoopThread.h           新增：线程与 EventLoop 绑定
│   ├── EventLoopThreadPool.h       新增：IO 线程池管理
│   ├── EventLoop.h                 新增 tid_ / isInLoopThread / runInLoop
│   ├── Poller/                     策略模式（同 Day 18）
│   └── ...
├── common/
│   ├── TcpServer.cpp               nextLoop() 轮询 + joinAll() 安全析构
│   ├── EventLoopThread.cpp         新增
│   ├── EventLoopThreadPool.cpp     新增
│   ├── Eventloop.cpp               构造时捕获 tid_
│   └── Poller/                     同 Day 18
└── test/                           ThreadPoolTest / StressTest
```
