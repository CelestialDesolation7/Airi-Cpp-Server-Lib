# Day 11：StressTest 压力测试 + onMessage 回调解耦

> 新增 `StressTest.cpp` 多线程压力测试客户端，验证 Echo 服务正确性。
> `Connection` 新增 `onMessageCallback` 回调，Server 通过 lambda 注册业务逻辑到 ThreadPool。
> Channel/Epoll/Buffer 沿用 Day 10 接口，无新增方法。

---

## 1. 引言

### 1.1 问题上下文

Day 10 引入了 ThreadPool 但只跑了一个 Echo 业务，并没有真正在压力下验证：(a) Echo 数据是否完整无丢失？(b) 多连接并发时 Buffer / Connection 有没有 race？(c) IO 线程在高 QPS 下还能撑住吗？

工业级网络库（muduo / asio）在每次重大改动后都会跑压测套件——muduo 自带的 `pingpong` benchmark、asio 的 `chat_server` example 就是为此而生。回归测试是设计的一部分，不是事后补丁。

### 1.2 动机

没有压测就没法判断"看起来对"和"真的对"的差距。压测客户端是后续所有性能优化（多 Reactor / 异步日志 / io_uring）的对照实验工具——没有它，所有优化结论都是猜的。

同时，把 Echo 业务从硬编码变成 `onMessageCallback`，让 server.cpp 可以注入任意业务，是迈向"网络库与应用解耦"的关键一步。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 手动 `nc` / `curl` 测试 | Unix 传统 | 零依赖、易上手 | 无法压并发 |
| 自写多线程压测客户端 (本日) | muduo `pingpong` / 本项目 | 完全控制场景、易调试 | 实现简陋、复用差 |
| `wrk` / `wrk2` | Will Glozer | HTTP 专用、性能高 | 仅 HTTP |
| `ab` (apache bench) | Apache | 老牌、易用 | 旧、无 keep-alive 控制 |
| `vegeta` / `k6` | 现代压测 | 报表完善、脚本化 | HTTP 为主 |
| `iperf3` | TCP/UDP 带宽测试 | 测吞吐上限 | 不测业务正确性 |

### 1.4 本日方案概述

本日实现：
1. 新建 `test/StressTest.cpp`：N 个客户端线程，各自建立 socket 连接服务器，发 M 条递增编号消息，校验 echo 回的内容与发出的一致。
2. `Connection::onMessageCallback_` 把 Echo 业务从 `Connection::handleRead()` 内联代码移出。
3. `Server` 在 `newConnection` 中通过 lambda 注册业务到 `threadPool_->add()`。
4. server.cpp 仍是最简 main：`new Eventloop → new Server(loop) → loop->loop()`。

后续每次架构改动都可以跑这套压测验证不退化。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `test/StressTest.cpp` | **新建** | 多线程压力测试：N 客户端各发 M 条消息，校验 Echo 正确性 |
| `include/Connection.h` | 沿用 Day10 | `onMessageCallback` + `readBuffer()` / `outBuffer()` |
| `common/Connection.cpp` | 沿用 Day10 | handleRead 调用 onMessageCallback(this) |
| `include/Server.h` | 沿用 Day10 | `ThreadPool* threadPool` 成员 |
| `common/Server.cpp` | 沿用 Day10 | newConnection 中将 Echo 逻辑提交到 threadPool->add |
| `server.cpp` | 沿用 Day10 | 最简 main：new Eventloop → new Server → loop |
| 其余文件 | 不变 | Channel/Epoll/Buffer/Acceptor/EventLoop/ThreadPool 沿用 Day 10 |

---

## 3. 模块全景与所有权树（Day 11）

```
main()
├── Eventloop* loop
│   └── Epoll* ep
└── Server* server
    ├── Acceptor* acceptor
    │   ├── Socket* + InetAddress*
    │   └── Channel* acceptChannel
    ├── ThreadPool* threadPool（4 线程）
    │   ├── workers[0..3]
    │   └── tasks queue
    └── map<int, Connection*> connection
        └── Connection* conn
            ├── Socket* + Channel*
            ├── Buffer inputBuffer_ / outputBuffer_
            ├── onMessageCallback → [lambda: threadPool->add(Echo)]
            └── deleteConnectionCallback → Server::deleteConnection
```

---

## 4. 全流程调用链

**场景 A：初始化**
```
main → new Eventloop → new Server(loop)
  → new Acceptor(loop)
    → new Socket, bind, listen, setnonblocking
    → new Channel(loop, fd), setReadCallback(acceptConnection), enableReading
  → new ThreadPool(4)
  → loop->loop()
```

**场景 B：新连接**
```
Epoll::poll → Channel::handleEvent → readCallback → Acceptor::acceptConnection
  → Socket::accept → new Socket(client_fd), setnonblocking
  → Server::newConnection(client_sock, client_addr)
    → new Connection(loop, client_sock)
      → new Channel, setReadCallback(handleRead), setWriteCallback(handleWrite), enableReading
    → conn->setOnMessageCallback(lambda: threadPool->add(Echo))
    → conn->setDeleteConnectionCallback(Server::deleteConnection)
    → connection[fd] = conn
```

**场景 C：数据读写 (Echo)**
```
Epoll::poll → Channel::handleEvent → readCallback → Connection::handleRead
  → inputBuffer_.readFd(sockfd)
  → onMessageCallback(this)
    → threadPool->add(lambda)
      → worker 线程取到任务
      → retrieveAllAsString → conn->send(msg)
        → 尝试直接 write; 若写不完 → outputBuffer_.append + enableWriting
```

**场景 D：StressTest**
```
main → 创建 N 个 thread，每个执行 oneClient(id, msgs)
  → connect(127.0.0.1:8888)
  → 循环 M 次：write(msg) → read 直到凑够 → 比较是否一致
  → 统计 failed_count
```

---

## 5. 代码逐段解析

### 5.1 `test/StressTest.cpp`

| 行 | 逻辑 |
|----|------|
| `oneClient(id, msgs)` | 创建 Socket，connect 服务器，循环发送/接收 msgs 条消息 |
| 发送循环 | `write(sock, msg)` 发送，`read` 循环直到读够 `target_len` 字节 |
| 校验 | `readBuffer->retrieveAsString(target_len) != msg` 则 failed_count++ |
| `main` | 解析参数(threads_num, msgs_num, wait_seconds)，启动 N 个线程 |

### 5.2 `common/Server.cpp` — onMessageCallback 注册

```cpp
std::function<void(Connection *)> msgCb = [this](Connection *conn) {
    threadPool->add([conn]() {
        std::string msg = conn->readBuffer()->retrieveAllAsString();
        conn->send(msg);  // Echo
    });
};
conn->setOnMessageCallback(msgCb);
```

- 外层 lambda 捕获 `this`（Server 指针）以访问 threadPool
- 内层 lambda 捕获 `conn` 指针，作为线程池任务执行 Echo

---

### 5.3 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day11/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day11/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 模块 | 职责 |
|------|------|
| `StressTest` | 多线程压力测试客户端，验证 Echo 正确性 |
| `Connection` | 管理单个连接的读写和缓冲区，通过回调驱动业务逻辑 |
| `Server` | 管理所有连接，注册 onMessage 业务逻辑到线程池 |
| `ThreadPool` | 工作线程池，异步执行业务任务 |
| `Acceptor` | 监听新连接，回调 Server::newConnection |
| `Channel` | 封装 fd + 事件，分发 read/write 回调 |
| `Epoll` | 封装 epoll/kqueue 多路复用 |
| `Buffer` | 自动扩容的读写缓冲区 |

---

## 7. 局限

1. **线程安全问题**：onMessageCallback 中 threadPool->add 将 conn 传入工作线程，但 inputBuffer 在主线程写、子线程读，存在竞态条件
2. **连接生命周期**：deleteConnection 在回调中直接 delete conn，如果工作线程还持有 conn 指针会导致 UAF
3. **StressTest 无超时机制**：如果服务器无响应，客户端线程会永久阻塞在 read
4. **单 Reactor**：所有连接都在同一个 Eventloop 上，高并发时成为瓶颈
