# Day 20 — TcpServer 安全关闭 + 成员析构顺序修正

## 核心变更
- **`TcpServer::stop()`**：优雅关闭，令所有 sub-reactor 和 main-reactor 退出 `loop()`
- **成员析构顺序修正**：`subReactors_` 声明在 `connections_` 之前，确保 EventLoop 在 Connection 之后析构
- **`deleteConnection` 改用 `shared_ptr` guard**：将 Connection 析构投递到归属子线程，消除 Channel 悬空指针
- **栈对象 + RAII**：`server.cpp` 中 `TcpServer` 改为栈对象，SIGINT 调 `stop()` 而非 `delete`

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
│   ├── TcpServer.h                 新增 stop()；成员声明顺序调整
│   ├── Connection.h                同 Day 19
│   ├── EventLoop.h                 同 Day 19
│   ├── Poller/                     策略模式（同 Day 18）
│   └── ...
├── common/
│   ├── TcpServer.cpp               stop() + shared_ptr guard 安全析构
│   ├── Connection.cpp              同 Day 19
│   └── Poller/                     同 Day 18
└── test/                           ThreadPoolTest / StressTest
```
