# Day 13 — 多 Reactor 架构

## 项目状态

在 Day 12（优雅关闭 + Channel 细粒度控制）基础上，重构为 **Multi-Reactor** 架构：

- `Server` 中 `loop` 重命名为 `mainReactor`，新增 `vector<Eventloop*> subReactors`
- mainReactor 仅负责 accept，subReactor 各自在独立线程中处理 IO
- 新连接按 `fd % subReactors.size()` 分配到对应 subReactor
- 业务逻辑（Echo）直接在 IO 线程执行，不再提交到 ThreadPool
- ThreadPool 改为运行 subReactor 事件循环

## 文件结构

```
day13/
├── CMakeLists.txt
├── server.cpp
├── client.cpp
├── include/
│   ├── ThreadPool.h
│   ├── Connection.h
│   ├── Server.h            ← 修改：mainReactor + subReactors
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
│   ├── Server.cpp          ← 重写：多 Reactor + 析构函数
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
    └── StressTest.cpp
```

## 编译与运行

```bash
cmake -S . -B build
cmake --build build

# 启动多 Reactor 服务器
./build/server

# 压力测试
./build/StressTest 10 100
```

## 与 Day 12 的区别

| 变更 | 说明 |
|------|------|
| `Server.h` | `loop` → `mainReactor`；新增 `vector<Eventloop*> subReactors` |
| `Server.cpp` | 构造函数创建 N 个 subReactor 各跑独立线程；newConnection 按 fd 分配；Echo 在 IO 线程直接执行；新增析构函数 |
| 其余文件 | 与 Day 12 完全一致 |

## 架构对比

| 维度 | Day 12（单 Reactor） | Day 13（多 Reactor） |
|------|---------------------|---------------------|
| Reactor | 1 个 | 1 mainReactor + N subReactor |
| IO 处理 | 主线程 | subReactor worker 线程 |
| 业务执行 | ThreadPool 异步 | IO 线程直接执行 |
| 并发能力 | 受单线程限制 | N 核并行 |
