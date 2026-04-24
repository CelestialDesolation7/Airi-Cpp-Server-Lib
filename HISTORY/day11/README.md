# Day 11 — StressTest 压力测试

## 项目状态

在 Day 10（ThreadPool 线程池）基础上，新增 **StressTest** 多线程压力测试客户端：

- `StressTest` 启动 N 个客户端线程，各发 M 条消息，校验 Echo 正确性
- `Server` / `Connection` / `Channel` / `Epoll` 沿用 Day 10 接口，无新增方法
- 业务逻辑仍通过 ThreadPool 异步执行

## 文件结构

```
day11/
├── CMakeLists.txt
├── server.cpp
├── client.cpp
├── include/
│   ├── ThreadPool.h
│   ├── Connection.h
│   ├── Server.h
│   ├── Buffer.h
│   ├── Channel.h
│   ├── Acceptor.h
│   ├── EventLoop.h
│   ├── Epoll.h
│   ├── Socket.h
│   ├── InetAddress.h
│   └── util.h
├── common/
│   ├── ThreadPool.cpp
│   ├── Connection.cpp
│   ├── Server.cpp
│   ├── Buffer.cpp
│   ├── Channel.cpp
│   ├── Acceptor.cpp
│   ├── Eventloop.cpp
│   ├── Epoll.cpp
│   ├── Socket.cpp
│   ├── InetAddress.cpp
│   └── util.cpp
└── test/
    ├── ThreadPoolTest.cpp
    └── StressTest.cpp    ← 新增
```

## 编译与运行

```bash
cmake -S . -B build
cmake --build build

# 终端 1：启动服务器
./build/server

# 终端 2：运行压力测试（10 线程，每线程 100 条消息）
./build/StressTest 10 100

# 终端 3：运行客户端（手动测试）
./build/client
```

## 与 Day 10 的区别

| 变更 | 说明 |
|------|------|
| 新增 `test/StressTest.cpp` | 多线程压力测试，验证 Echo 服务正确性 |
| 其余文件无变化 | Server/Connection/Channel/Epoll/Buffer 完全沿用 Day 10 |
