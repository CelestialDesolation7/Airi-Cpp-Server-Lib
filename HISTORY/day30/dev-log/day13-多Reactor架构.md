# Day 13：多 Reactor 架构 — mainReactor + subReactors

> 将单 Reactor 重构为 Multi-Reactor 架构：mainReactor 仅负责 accept，多个 subReactor 各自运行独立 Eventloop 处理 IO。
> 连接按 `fd % subReactors.size()` 分配到 subReactor，业务逻辑直接在 IO 线程执行而非提交到 ThreadPool。
> Server 新增析构函数清理 subReactor 资源。

---

## 1. 引言

### 1.1 问题上下文

到 Day 12，所有 IO 仍然集中在单一事件循环上：`accept()` 与所有连接的 `read/write` 都跑在同一个线程里。CPU 多核时只有一个核被用满，其余核闲置。

muduo 论文的标志性贡献之一就是 **one-loop-per-thread + multi-Reactor**：mainReactor 只接受新连接，subReactor 池每个线程跑独立 EventLoop 处理 IO。Netty 的 `bossGroup` + `workerGroup`、Nginx 的 `accept_mutex` + worker process 都是同一思想的不同实现。

### 1.2 动机

单 Reactor 的瓶颈在于"事件循环线程"本身——再多 worker 线程也无法分担"从内核取事件"的工作。多 Reactor 把这件事并行化，让多核 CPU 真正被利用。

同时，业务回调直接在 IO 线程里执行（不再过 ThreadPool），减少了一次跨线程拷贝/锁开销，对纯转发型业务（HTTP/RPC 网关）显著提速。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 单 Reactor + ThreadPool | Day 10-12 | 简单 | IO 线程是瓶颈 |
| Multi-Reactor (one-loop-per-thread) | muduo / 本日 | 多核线性扩展、IO 与业务在同一线程无锁 | 跨连接广播需要 runInLoop |
| Master/Worker 进程模型 | Nginx | 进程隔离、单进程崩溃不影响整体 | IPC 复杂 |
| SO_REUSEPORT 多进程同端口 | Linux 3.9+ | 内核级负载均衡 | Linux only、调试复杂 |
| Tokio multi-thread runtime | Tokio | 协程 + 工作窃取 | 协程染色 |

### 1.4 本日方案概述

本日实现：
1. `Server::loop` 重命名为 `mainReactor`；新增 `vector<Eventloop*> subReactors_`。
2. 构造函数：创建 N 个 subReactor，把 `subReactor->loop()` 提交给 ThreadPool（每个 worker 跑一个 subReactor）。
3. `newConnection(int fd)`：按 `fd % subReactors_.size()` 选一个 subReactor，把新 Connection 注册到它。
4. 业务 `msgCb` 直接执行（不再 `threadPool->add`）——因为业务现在跑在 subReactor 自己的线程里。
5. 析构函数：清理所有 subReactor。

仍然存在的问题：(a) Channel 没有禁拷贝，理论上可以被误复制；(b) `subReactor->loop()` 在子线程里跑，但 `tid_` 是主线程的——`runInLoop` 还无法跨线程派发任务。这两个问题分别由 Day 14 / Day 21 解决。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Server.h` | **修改** | `loop` 重命名为 `mainReactor`；新增 `vector<Eventloop*> subReactors` |
| `common/Server.cpp` | **重写** | 构造函数创建 subReactors 各跑独立 Eventloop；newConnection 按 fd 分配到 subReactor；msgCb 直接执行不再入线程池；新增析构函数 |
| 其余文件 | 不变 | Channel/Epoll/Connection/Acceptor/EventLoop 沿用 Day 12 |

---

## 3. 模块全景与所有权树（Day 13）

```
main()
├── signal(SIGINT, signalHandler)
├── Eventloop* mainReactor                   ← 原 loop 改名
│   └── Epoll* ep（仅处理 accept）
└── Server* server
    ├── Acceptor* acceptor
    │   └── Channel* (listen fd → mainReactor)
    ├── ThreadPool* threadPool
    │   └── 每个 worker 跑一个 subReactor->loop()    ← 新增
    ├── vector<Eventloop*> subReactors               ← 新增
    │   ├── subReactors[0] → Epoll* (处理 IO)
    │   ├── subReactors[1] → Epoll*
    │   └── subReactors[N] → Epoll*
    └── map<int, Connection*> connections
        └── Connection* conn
            └── Channel* (conn fd → subReactor[fd % N])
```

---

## 4. 全流程调用链

**场景 A：Server 初始化（多 Reactor）**
```
Server::Server(mainReactor)
  → new Acceptor(mainReactor)
  → int threadCount = std::thread::hardware_concurrency()
  → new ThreadPool(threadCount)
  → for i in [0, threadCount):
      → subReactors[i] = new Eventloop()
      → threadPool->add(bind(&Eventloop::loop, subReactors[i]))
        → worker 线程中运行 subReactor->loop()
```

**场景 B：新连接分配到 subReactor**
```
mainReactor::poll → Acceptor::acceptConnection
  → Server::newConnection(client_sock, client_addr)
    → int idx = client_fd % subReactors.size()
    → Eventloop* sub = subReactors[idx]
    → new Connection(sub, client_sock)        ← 注意：传入 subReactor
      → Channel 注册到 sub 的 Epoll
    → conn->setOnMessageCallback(echo)
    → connections[fd] = conn
```

**场景 C：数据读写（IO 线程直接执行）**
```
subReactor[i]::poll → Channel::handleEvent → readCallback
  → Connection::handleRead → inputBuffer.readFd
  → onMessageCallback(this)
    → msg = readBuffer->retrieveAllAsString()
    → conn->send(msg)                        ← 直接 Echo，无线程池
```

**场景 D：Server 销毁**
```
Server::~Server()
  → for each subReactor: delete subReactor
  → delete acceptor
  → delete threadPool
```

---

## 5. 代码逐段解析

### 5.1 `Server.h` — 多 Reactor 成员

```cpp
class Server {
    Eventloop *mainReactor;                    // 原 loop
    std::vector<Eventloop *> subReactors;      // 新增
    Acceptor *acceptor;
    ThreadPool *threadPool;
    std::map<int, Connection *> connections;
};
```

### 5.2 `Server.cpp` — 构造函数

```cpp
Server::Server(Eventloop *loop) : mainReactor(loop) {
    acceptor = new Acceptor(mainReactor);
    int size = std::thread::hardware_concurrency();
    threadPool = new ThreadPool(size);
    for (int i = 0; i < size; i++) {
        subReactors.push_back(new Eventloop());
        threadPool->add(std::bind(&Eventloop::loop, subReactors[i]));
    }
    // ...
}
```

- ThreadPool 的线程数 = 硬件并发数（CPU 核数）
- 每个 subReactor 在独立线程中运行自己的 `loop()`

### 5.3 `Server.cpp` — newConnection 分发

```cpp
void Server::newConnection(Socket *sock, InetAddress *addr) {
    int fd = sock->getFd();
    Eventloop *sub = subReactors[fd % subReactors.size()];
    Connection *conn = new Connection(sub, sock);  // 注册到 subReactor
    // ...
}
```

- `fd % N` 简单哈希，将连接均匀分配到各 subReactor

### 5.4 `Server.cpp` — msgCb 不再入线程池

```cpp
// Day 12 (旧):
threadPool->add([conn]() {
    std::string msg = conn->readBuffer()->retrieveAllAsString();
    conn->send(msg);
});

// Day 13 (新):
std::string msg = conn->readBuffer()->retrieveAllAsString();
conn->send(msg);
```

- 业务逻辑直接在 IO 线程执行，避免 Day 11/12 的跨线程竞态问题
- 对于 CPU 密集型任务可以重新引入 ThreadPool，但 Echo 场景下 IO 线程执行更高效

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day13/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day13/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 对比：Day 12 → Day 13

| 维度 | Day 12（单 Reactor） | Day 13（多 Reactor） |
|------|---------------------|---------------------|
| Reactor 数 | 1 个 Eventloop 处理 accept + IO | 1 个 mainReactor + N 个 subReactor |
| 连接分配 | 全部在同一 Eventloop | `fd % N` 分到 subReactor |
| IO 处理线程 | 主线程 | 各 subReactor 的 worker 线程 |
| 业务逻辑执行 | ThreadPool 异步执行 | IO 线程直接执行 |
| 并发性能 | 受单线程 Reactor 限制 | N 核并行处理 IO |
| 线程安全 | conn 指针跨线程使用（危险） | 同一 conn 始终在同一线程（安全） |

---

## 7. 职责划分表

| 模块 | 职责 |
|------|------|
| `mainReactor` | 仅负责 accept 新连接 |
| `subReactors` | 各自独立处理分配到的连接 IO |
| `Server` | 管理多 Reactor 生命周期，分发连接 |
| `ThreadPool` | 运行 subReactor 线程（不再执行业务任务） |
| `Connection` | 绑定到特定 subReactor，IO + 业务在同一线程 |

---

## 8. 局限

1. **fd % N 分配不均**：fd 编号连续时可能导致分配不均；更好的策略是 Round-Robin 或最少连接
2. **跨线程创建 Connection**：mainReactor 线程创建 Connection 并注册到 subReactor 的 Epoll，存在线程安全问题（subReactor 可能同时在操作 Epoll）；应使用 `runInLoop` / `queueInLoop` 机制
3. **deleteConnection 线程安全**：Connection 断开时从 connections map 中删除仍在 mainReactor 线程，但 Connection 的 IO 在 subReactor 线程
4. **subReactor 无法通知退出**：setQuit 只设置 mainReactor 的 quit 标志，subReactor 的 loop 没有退出机制
