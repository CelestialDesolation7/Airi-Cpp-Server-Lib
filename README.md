# Day 22 — ET 循环读 + Connection 状态守卫

## 核心变更
- **ET 循环读**：`Connection::doRead()` 改为循环读至 `EAGAIN`，解决 ET 模式下缓冲区残留导致客户端卡死
- **Business() 状态守卫**：kqueue/epoll 同批次多事件时防止已 close 的 Connection 被重入处理
- **doWrite() 状态检查**：关闭态不再写入数据

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
│   ├── TcpServer.h                 同 Day 21
│   ├── EventLoopThread.h           同 Day 21
│   ├── EventLoopThreadPool.h       同 Day 21
│   ├── EventLoop.h                 同 Day 21
│   ├── Connection.h                同 Day 21
│   ├── Poller/                     策略模式（同 Day 18）
│   └── ...
├── common/
│   ├── Connection.cpp              ET 循环读 + 状态守卫
│   ├── TcpServer.cpp               同 Day 21
│   ├── EventLoopThread.cpp         同 Day 21
│   ├── EventLoopThreadPool.cpp     同 Day 21
│   └── Poller/                     同 Day 18
└── test/                           ThreadPoolTest / StressTest
```
