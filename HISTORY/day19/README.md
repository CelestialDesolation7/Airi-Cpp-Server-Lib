# Day 19 — Server → TcpServer 重命名 + 智能指针改造

## 核心变更
- **`Server` → `TcpServer`** 重命名，语义更精确
- **全面 `unique_ptr`**：TcpServer、Acceptor、Connection 内部资源均用智能指针管理
- **接口简化**：Acceptor 回调从 `void(Socket*, InetAddress*)` 改为 `void(int fd)`；Connection 构造从 `(Eventloop*, Socket*)` 改为 `(int fd, Eventloop*)`
- **新增 `RC` 返回码枚举**（Macros.h），为后续错误处理改造铺路
- **`TcpServer::Start()`**：自包含启动方法，内部自建 mainReactor

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
│   ├── TcpServer.h                 替代 Server.h（新增）
│   ├── Connection.h                unique_ptr 成员 + int fd 构造
│   ├── Acceptor.h                  unique_ptr 成员 + void(int) 回调
│   ├── Poller/                     策略模式（同 Day 18）
│   ├── Airi-Cpp-Server-Lib.h            伞形头文件（含 TcpServer.h）
│   └── ...
├── common/
│   ├── TcpServer.cpp               替代 Server.cpp（新增）
│   ├── Connection.cpp              make_unique 创建 Socket/Channel
│   ├── Acceptor.cpp                简化回调，只传 fd
│   └── Poller/                     同 Day 18
└── test/                           ThreadPoolTest / StressTest
```
