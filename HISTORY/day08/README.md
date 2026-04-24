# Day 08 — Connection 类

## 项目状态

在 Day 07（Acceptor 拆分）基础上，引入 **Connection** 类：

- `Connection` 封装客户端 Socket + Channel + echoRead 回调
- `Server` 用 `map<int, Connection*>` 管理所有连接的生命周期
- 客户端断开时通过 `deleteConnectionCallback` 回调链自动清理

## 文件结构

```
day08/
├── CMakeLists.txt
├── server.cpp
├── client.cpp
├── include/
│   ├── Connection.h    ← 新增
│   ├── Acceptor.h
│   ├── Server.h        ← 新增 map + deleteConnection
│   ├── EventLoop.h
│   ├── Channel.h
│   ├── Epoll.h
│   ├── Socket.h
│   ├── InetAddress.h
│   └── util.h
└── common/
    ├── Connection.cpp  ← 新增
    ├── Acceptor.cpp
    ├── Server.cpp      ← 修改
    ├── Eventloop.cpp
    ├── Channel.cpp
    ├── Epoll.cpp
    ├── Socket.cpp
    ├── InetAddress.cpp
    └── util.cpp
```

## 编译与运行

```bash
cmake -S . -B build
cmake --build build

./build/server    # 终端 1
./build/client    # 终端 2
```

## 改进

- 解决了 Day 06-07 的客户端 Socket/Channel 内存泄漏问题
