# Day 10 — ThreadPool 线程池

## 项目状态

在 Day 09（Buffer 缓冲区）基础上，引入 **ThreadPool** 线程池：

- `ThreadPool` 管理工作线程 + 任务队列，支持任意可调用对象提交
- `Connection` 新增 `onMessageCallback`，业务逻辑不再硬编码
- `Server` 在 onMessageCallback 中将 Echo 任务提交到 ThreadPool 异步执行

## 文件结构

```
day10/
├── CMakeLists.txt
├── server.cpp
├── client.cpp
├── include/
│   ├── ThreadPool.h    ← 新增
│   ├── Connection.h    ← 修改：新增 onMessageCallback + readBuffer/outBuffer
│   ├── Server.h        ← 修改：新增 ThreadPool* 成员
│   ├── Buffer.h
│   ├── Channel.h
│   ├── Acceptor.h
│   ├── EventLoop.h
│   ├── Epoll.h
│   ├── Socket.h
│   ├── InetAddress.h
│   └── util.h
├── common/
│   ├── ThreadPool.cpp  ← 新增
│   ├── Connection.cpp  ← 修改
│   ├── Server.cpp      ← 修改
│   ├── Buffer.cpp
│   ├── Channel.cpp
│   ├── Acceptor.cpp
│   ├── Eventloop.cpp
│   ├── Epoll.cpp
│   ├── Socket.cpp
│   ├── InetAddress.cpp
│   └── util.cpp
└── test/
    └── ThreadPoolTest.cpp  ← 新增
```

## 编译与运行

```bash
cmake -S . -B build
cmake --build build

./build/server          # 终端 1
./build/client          # 终端 2
./build/ThreadPoolTest  # 线程池单元测试
```

## 改进

- 业务逻辑解耦：Connection 通过 onMessageCallback 回调，不再硬编码 Echo
- 异步处理：耗时任务提交到 ThreadPool，不阻塞主 IO 线程
- 线程池模板：支持任意函数签名 + std::future 获取返回值

## 已知问题

- Buffer 非线程安全：主线程 handleRead 写 inputBuffer，工作线程读 inputBuffer 存在竞争
- 单 Reactor：所有 IO 仍在主线程，ThreadPool 仅处理业务计算
