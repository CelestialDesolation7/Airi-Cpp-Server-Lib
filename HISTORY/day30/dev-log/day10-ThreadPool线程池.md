# Day 10：ThreadPool 线程池——业务逻辑异步执行

> 引入 ThreadPool 类，将业务处理（如 Echo）从主 IO 线程移到工作线程。
> Connection 新增 `onMessageCallback`，Server 在回调中通过 ThreadPool::add 提交任务。
> 同时提供 ThreadPoolTest 验证线程池功能。

---

## 1. 引言

### 1.1 问题上下文

到 Day 09 为止，所有 IO 与业务（echo）都跑在同一个事件循环线程里。这对纯转发型业务尚可，但只要业务里有任何阻塞操作（数据库查询、磁盘读、密码哈希），单一 IO 线程就会被堵死，所有连接都受影响。

这是 Reactor 架构的固有局限：事件循环线程必须保持"快"，慢任务必须移到独立的 worker 线程。muduo / Netty 都提供了内置 ThreadPool；asio 用 strand + thread group；Tokio 把 IO 和 CPU 任务用 `spawn` / `spawn_blocking` 分开。

### 1.2 动机

单线程 Reactor 的吞吐上限取决于"每个事件回调耗时"。引入工作线程池后，IO 线程只负责把任务投递到队列，业务计算在独立线程里跑，IO 线程可以立即继续处理下一个事件。

这一步还为 Day 13 的多 Reactor、Day 21 的 EventLoopThreadPool 做铺垫：先理解最简单的"任务队列 + worker 线程"模型，后面才能理解为什么"任务队列 + N 个事件循环"是更优解。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 单线程 Reactor | Day 06-09 | 无锁、零线程切换 | 业务阻塞会拖死全部连接 |
| ThreadPool + 任务队列 (本日) | Boost.Pool / muduo | 简单、易上手 | 跨线程数据要加锁、缓存不友好 |
| EventLoopThreadPool (one-loop-per-thread) | muduo / Day 21 | 每连接绑定固定线程，无锁 | 负载均衡靠分配策略 |
| Work-stealing pool | Tokio / Java ForkJoinPool | 自动负载均衡 | 实现复杂、缓存抖动 |
| 协程 + M:N 调度 | Go runtime / Rust Tokio | 用户态调度、海量任务 | 栈管理、debugger 友好性差 |

### 1.4 本日方案概述

本日实现：
1. 新建 `ThreadPool` 类：N 个 worker `std::thread` + `queue<function<void()>>` + `mutex` + `condition_variable`。
2. `add(F&& f, Args&&... args)` 是模板，写在头文件里：把可调用对象与参数 `std::bind`/`forward` 到队列。
3. 析构函数：设 `stop_=true` + 唤醒所有 worker + `join`。
4. `Connection` 新增 `onMessageCallback_`，`handleRead()` 不再硬编码 echo，调 `onMessageCallback_(this)`。
5. `Server` 持有 `ThreadPool*`，在 `newConnection` 设 `onMessageCallback_` 为 `[&](conn){ threadPool->add([conn]{ /* echo */ }); }`。
6. 新增 `ThreadPoolTest.cpp` 验证：8 个任务被 4 个 worker 并行消费。

仍未解决：跨线程访问 Connection / Channel 的安全问题（只有把"修改 Channel"也搬回 IO 线程才彻底安全，Day 14 用 `eventfd` + `runInLoop` 解决）。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/ThreadPool.h` | **新建** | 线程池：workers 数组 + tasks 队列 + 互斥锁/条件变量 |
| `common/ThreadPool.cpp` | **新建** | 构造启动线程；析构 stop=true + join；模板 add 在头文件 |
| `test/ThreadPoolTest.cpp` | **新建** | 验证 ThreadPool：8 个任务分配到 4 线程 |
| `include/Connection.h` | **修改** | 新增 `onMessageCallback`；暴露 `readBuffer()` / `outBuffer()` |
| `common/Connection.cpp` | **修改** | handleRead 不再硬编码 Echo，改为调用 onMessageCallback(this) |
| `include/Server.h` | **修改** | 新增 `ThreadPool* threadPool` 成员 |
| `common/Server.cpp` | **修改** | 构造中 new ThreadPool；newConnection 设置 onMessageCallback 里用 threadPool->add |
| 其余文件 | 不变 | Channel/Epoll/Buffer/Acceptor/EventLoop 沿用 Day 09 |

---

## 3. 模块全景与所有权树（Day 10）

```
main()
├── Eventloop* loop
│   └── Epoll* ep
└── Server* server
    ├── Acceptor* acceptor
    │   └── ...
    ├── ThreadPool* threadPool          ← 新增（4 线程）
    │   ├── workers[0..3]
    │   └── tasks queue
    └── map<int, Connection*> connection
        └── Connection* conn
            ├── Socket* + Channel*
            ├── Buffer inputBuffer_ / outputBuffer_
            ├── onMessageCallback       ← 新增
            │   └── [lambda: threadPool->add(Echo 任务)]
            └── deleteConnectionCallback
```

---

## 4. 全流程调用链

**场景 A：初始化**

```
main()
├── loop = new Eventloop()
└── server = new Server(loop)
    ├── acceptor = new Acceptor(loop)    ← bind/listen/accept
    └── threadPool = new ThreadPool(4)   ← 启动 4 个工作线程
        └── workers[i]: for(;;) { wait → task() }
```

**场景 B：新连接**

```
ep->poll() → acceptChannel->handleEvent()
└── Server::newConnection(client_sock, client_addr)
    conn = new Connection(loop, client_sock)
    conn->setOnMessageCallback([threadPool](Connection *c) {
        threadPool->add([c]() {
            msg = c->readBuffer()->retrieveAllAsString()
            c->send(msg)    // Echo
        })
    })
    conn->setDeleteConnectionCallback(...)
    connection[fd] = conn
```

**场景 C：数据可读 → 异步处理**

```
ep->poll() → clientChannel->handleEvent()
    revents & POLLER_READ
    └── Connection::handleRead()
        n = inputBuffer_.readFd(sockfd, &savedErrno)
        if n > 0:
            onMessageCallback(this)     ← 不再硬编码 Echo
            └── threadPool->add(Echo 任务)
                └── 工作线程取走任务执行
                    msg = readBuffer()->retrieveAllAsString()
                    send(msg)
```

**场景 D：线程池析构**

```
delete server
└── ~Server()
    delete threadPool
    └── ~ThreadPool()
        { lock; stop = true; }
        condition.notify_all()
        for (worker : workers) worker.join()
```

---

## 5. 代码逐段解析

### 5.1 ThreadPool.h — 任务提交模板

```cpp
template <class F, class... Args>
auto ThreadPool::add(F &&f, Args &&...args)
    -> std::future<typename std::result_of<F(Args...)>::type> {
    using return_type = typename std::result_of<F(Args...)>::type;
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
        tasks.emplace([task]() { (*task)(); });
    }
    condition.notify_one();
    return res;
}
```

> 核心模板：接收任意可调用对象 + 参数 → packaged_task 包装 → shared_ptr 使其可复制进 queue → notify_one 唤醒一个工作线程。

### 5.2 ThreadPool 构造 — 工作线程循环

```cpp
ThreadPool::ThreadPool(size_t threads) : stop(false) {
    for (size_t i = 0; i < threads; ++i)
        workers.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    condition.wait(lock, [this] { return stop || !tasks.empty(); });
                    if (stop && tasks.empty()) return;
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                task();
            }
        });
}
```

> 每个线程死循环：加锁 → wait 条件（stop 或有任务）→ 取任务 → 解锁 → 执行。

### 5.3 Connection — onMessageCallback

```cpp
void Connection::handleRead() {
    ssize_t n = inputBuffer_.readFd(sockfd, &savedErrno);
    if (n > 0) {
        if (onMessageCallback)
            onMessageCallback(this);   // 不再硬编码 Echo
    }
}
```

> Day 09 的 `send(msg)` 被替换为 `onMessageCallback(this)`，业务逻辑可由 Server 自定义。

### 5.4 Server — 业务逻辑提交线程池

```cpp
std::function<void(Connection *)> msgCb = [this](Connection *c) {
    threadPool->add([c]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string msg = c->readBuffer()->retrieveAllAsString();
        c->send(msg);
    });
};
conn->setOnMessageCallback(msgCb);
```

> 捕获 this 拿到 threadPool；内层 lambda 捕获 Connection* 执行实际业务。
> sleep(2) 模拟耗时操作，验证不阻塞主线程。

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day10/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day10/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表（Day 10）

| 模块 | 职责 |
|------|------|
| `ThreadPool` | 维护工作线程 + 任务队列；add 提交任务，析构时 join |
| `Connection` | 拥有 Socket + Channel + 双 Buffer；handleRead → onMessageCallback；暴露 readBuffer/outBuffer |
| `Server` | Acceptor + Connection map + ThreadPool；newConnection 时设回调，把业务 add 到线程池 |
| `Channel` | fd + 读写事件 + 双回调 |
| `Buffer` | 应用层缓冲区 |
| `Acceptor` | bind/listen/accept → 回调 |
| `Eventloop` | poll → handleEvent 循环 |

---

## 7. Day 10 的局限

1. **线程不安全**：Connection 的 Buffer 在主线程写（handleRead）、工作线程读（retrieveAllAsString），存在数据竞争
2. **单 Reactor**：仍然只有一个 EventLoop，所有 IO 在主线程，ThreadPool 只处理业务计算
3. **无 EventLoopThreadPool**：理想架构是 main loop 只做 accept，sub-loop 处理 IO

→ Day 11+ 将引入 EventLoopThreadPool（多 Reactor）解决线程安全和 IO 分发。

---

## 8. 对应 HISTORY

→ `HISTORY/day10/`
