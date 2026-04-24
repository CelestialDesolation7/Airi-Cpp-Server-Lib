# Day 06 — EventLoop + Server + Channel 回调

## 项目状态

在 Day 05（Channel 封装）基础上，引入 **EventLoop** 和 **Server** 类：

- `Eventloop` 封装 poll → handleEvent 事件循环
- `Server` 封装 Socket 初始化 + accept + 客户端 Channel 创建
- `Channel` 新增 `callback` / `setCallback()` / `handleEvent()` 回调机制
- `server.cpp` 精简为创建 EventLoop + Server + loop()

## 文件结构

```
day06/
├── CMakeLists.txt
├── server.cpp          ← 仅 new EventLoop → new Server → loop()
├── client.cpp          ← 交互式客户端
├── include/
│   ├── EventLoop.h     ← 新增
│   ├── Server.h        ← 新增
│   ├── Channel.h       ← 新增 callback + handleEvent()
│   ├── Epoll.h
│   ├── Socket.h
│   ├── InetAddress.h
│   └── util.h
└── common/
    ├── Eventloop.cpp   ← 新增
    ├── Server.cpp      ← 新增
    ├── Channel.cpp     ← 修改
    ├── Epoll.cpp
    ├── Socket.cpp
    ├── InetAddress.cpp
    └── util.cpp
```

## 编译与运行

```bash
cmake -S . -B build
cmake --build build

# 终端 1
./build/server

# 终端 2
./build/client
```

## 已知问题

- client_sock / clientChannel 仍有内存泄漏
- accept 逻辑耦合在 Server::handleReadEvent()，无独立 Acceptor
