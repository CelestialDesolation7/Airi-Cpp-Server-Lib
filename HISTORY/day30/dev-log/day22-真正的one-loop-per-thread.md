# Day 22：真正的 one-loop-per-thread —— EventLoopThread 接线 + tid 自检

> **主题**：把 Day 21 已经写好的 `EventLoopThread` / `EventLoopThreadPool` 真正接进 `TcpServer`；给 `Eventloop` 加 `tid_` 让"我现在是不是该 reactor 的归属线程"成为可在运行时校验的事实；用 `runInLoop()` 把"本线程直接调 / 跨线程排队"的二选一封进一个调用。
> **基于**：Day 21（stop / shared_ptr guard / EventLoopThread 类骨架）

---

## 1. 引言

### 1.1 问题上下文

Day 21 留了三个明显的"半成品"：

1. `subReactors_` 是 `vector<unique_ptr<Eventloop>>`，全部在主线程构造，再被 `ThreadPool` 调度去跑 `loop()`。这意味着每个 `Eventloop` 内部如果想知道"当前线程是不是我的归属线程"，唯一可用的对比物只有"调构造函数时的线程"——而那是主线程，不是真正跑 loop 的 worker。结果：跨线程投递只能靠"程序员心算 fd → idx → 哪个 sub-reactor"，运行时无任何防御。
2. `EventLoopThread` / `EventLoopThreadPool` 已经写好了类、写好了 `condition_variable` 同步、写好了 `start()` / `nextLoop()` / `stopAll()` / `joinAll()` 接口，但 `TcpServer` 一行都没用。
3. Day 20-21 的所有"跨线程投递"调用都直接走 `xxxLoop->queueInLoop(...)`：哪怕调用方就在该 loop 的归属线程里，也要写进 `pendingFunctors_`、再 wakeup、再下一轮 doPendingFunctors 才执行——一次本来不必要的延迟和一次锁。

这一天就是把这三件事一次合上：把骨架装进 TcpServer，把 `tid_` 注入 Eventloop，把"是否本线程"的判断封成 `runInLoop`。

### 1.2 动机

one-loop-per-thread 的真正含义是 **"同一个 EventLoop 上的所有状态都只被同一个线程读写，所以不需要锁"**。要让这条承诺成真，必须满足两个前置：(a) `EventLoop` 在它将要运行的线程内部被构造，`tid_ = std::this_thread::get_id()` 才能等于跑 `loop()` 的线程；(b) 所有"我想在这个 loop 上跑这段代码"的入口都通过统一接口（`runInLoop`）路由——本线程直接执行（零延迟，零锁），跨线程才进 `pendingFunctors_` + wakeup。

这是 muduo / asio / Tokio 都遵循的模型；Reactor 框架成熟与否，看的就是这两条。

第二个动机是把 Day 21 引入的 `EventLoopThread` 类付诸实用——让它持有 `Eventloop` 的 `unique_ptr`、让线程退出后 EventLoop 仍然存活到 EventLoopThread 析构时才销毁，这给 `TcpServer` 析构序列开辟了一个安全窗口："IO 线程已 join，但 EventLoop 还在"，正是 `~Connection()` 调 `loop_->deleteChannel()` 的安全前提。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 主线程构造 EventLoop，worker 线程 run loop()（Day 13–21 路径） | 教学代码 | 简单 | tid 不一致，isInLoopThread 全失效 |
| EventLoop 在子线程内构造 + cv 同步（本日） | muduo | tid 正确，无锁优化生效；EventLoopThread 持有所有权打开析构安全窗口 | 需要 mutex / cv 配合一次握手 |
| 主线程构造 + thread-local 重写 tid | 一些定制实现 | 不需要 cv | 需要侵入 EventLoop 构造路径 |
| Tokio worker + LocalSet | Tokio | 协程级局部性 | 依赖运行时 |
| Erlang scheduler + reduction count | BEAM | 抢占式调度，公平性极强 | 跨语言 |

`runInLoop` 的"本线程直接调 / 跨线程排队"二选一封装也是 muduo 范本：上层调用者**不需要知道**自己处在哪个线程，统一调 `loop->runInLoop(f)` 即可。本线程时退化成 `f()`、跨线程时升级为 `queueInLoop + wakeup`。

### 1.4 本日方案概述

本日实现：

1. `Eventloop` 加 `std::thread::id tid_`，构造时 `tid_ = std::this_thread::get_id()` 捕获——配合 `EventLoopThread::threadFunc` 在子线程内 `make_unique<Eventloop>()`，`tid_` 自动与跑 loop 的线程一致。
2. `Eventloop::isInLoopThread()`：`return tid_ == std::this_thread::get_id();`。
3. `Eventloop::runInLoop(func)`：本线程直接 `func()`；跨线程退化为 `queueInLoop(std::move(func))`。
4. `TcpServer.h` 把 `vector<unique_ptr<Eventloop>> subReactors_` 删除，换成 `unique_ptr<EventLoopThreadPool> threadPool_`（同时把"声明顺序 / 析构顺序 / 安全性"注释扩写到一整段）。
5. `TcpServer.cpp` 的接线：构造时 `threadPool_ = make_unique<EventLoopThreadPool>(mainReactor_.get())` + `setThreadNums(N)`；`Start()` 内 `threadPool_->start()` 同步等待所有 sub-reactor 就绪后再 `mainReactor_->loop()`；`newConnection` 用 `threadPool_->nextLoop()` 选 sub-reactor；`stop()` 用 `threadPool_->stopAll()`；`~TcpServer()` 调 `stop()` + `threadPool_->joinAll()` 后再让成员按声明逆序自动析构。
6. `Airi-Cpp-Server-Lib.h` 暴露 `EventLoopThread` / `EventLoopThreadPool` 头文件。
7. `hardware_concurrency()` 可能返回 0，本日加 `std::max(1, ...)` 兜底。

本日不做：

- 不改 Connection（Day 20 的生命周期模型继续用）。
- 不改 Acceptor（仍跑在 mainReactor）。
- 不引入定时器（Day 23）。

---

## 2. 模块全景与所有权树

```
TcpServer  ── 栈对象，由 main() 持有
├── unique_ptr<Eventloop>           mainReactor_
│   ├── tid_                        = main 线程              ★ 本日
│   ├── unique_ptr<Poller>          poller_
│   ├── unique_ptr<Channel>         evtChannel_
│   └── atomic<bool>                quit_
├── unique_ptr<Acceptor>            acceptor_
├── unique_ptr<EventLoopThreadPool> threadPool_              ★ 替换 Day 21 的 ThreadPool + vector
│   ├── Eventloop*                          mainLoop_         (观察者)
│   ├── vector<unique_ptr<EventLoopThread>> threads_
│   │   └── EventLoopThread
│   │       ├── unique_ptr<Eventloop> loop_   ← 由子线程 threadFunc 内 make_unique
│   │       │   └── tid_ = 该子线程的 id      ★ 本日真正一致
│   │       ├── std::thread          thread_
│   │       ├── std::mutex / cv
│   │       └── bool                 loopReady_
│   └── vector<Eventloop*>                  loops_           // 观察者裸指针
└── unordered_map<int, unique_ptr<Connection>> connections_
```

**所有权规则**

- `EventLoopThread` 是 `Eventloop` 的真正持有者，`unique_ptr<Eventloop>` 在 `EventLoopThread` 析构时才销毁。`EventLoopThreadPool::loops_` 是观察者，存储裸指针仅供 `nextLoop()` 轮询。
- `TcpServer::~TcpServer()` 显式 `joinAll()`，把"IO 线程已退出"和"EventLoop 已销毁"切成两个不重叠的窗口：joinAll 之后、`threadPool_` 析构之前，IO 线程已死但 EventLoop 仍活——这是 `connections_` 析构（触发 `~Connection` → `loop_->deleteChannel()`）安全的精确前提。
- `mainLoop_` 在 `EventLoopThreadPool` 中只用于 `nextLoop()` 在零子线程时回退，非所有权。

---

## 3. 初始化顺序（构造阶段）

```
[main 线程]
①  TcpServer server;
    ├─ mainReactor_  = make_unique<Eventloop>()
    │     └─ mainReactor_->tid_ = main 线程 id        ★
    ├─ acceptor_     = make_unique<Acceptor>(mainReactor_.get())
    ├─ acceptor_->setNewConnectionCallback(...)
    ├─ threadNum     = max(1, hardware_concurrency())
    ├─ threadPool_   = make_unique<EventLoopThreadPool>(mainReactor_.get())
    └─ threadPool_->setThreadNums(threadNum)         // 仅记录数量，未启动

② signal handler 注册（同 Day 21）

③ server.newConnect / server.onMessage

④ server.Start();
    ├─ threadPool_->start();
    │   for i in 0..threadNum:
    │     auto t = make_unique<EventLoopThread>();
    │     loops_.push_back(t->startLoop());           ★ 同步握手
    │     │   ├─ thread_ = std::thread(threadFunc)   [→ 子线程开跑]
    │     │   └─ cv.wait(loopReady_)                 [主线程阻塞]
    │     │
    │     │   [子线程 i] threadFunc()
    │     │     ├─ loop_ = make_unique<Eventloop>()   ★ 在子线程内构造
    │     │     │     └─ Eventloop::tid_ = 子线程 i 的 id
    │     │     ├─ { lock; loopReady_ = true; cv.notify_one(); }
    │     │     └─ loop_->loop();                     // 阻塞，等 setQuit + wakeup
    │     │
    │     [主线程被 cv 唤醒] startLoop() 返回 loop_.get()
    │     threads_.push_back(std::move(t))
    │
    └─ mainReactor_->loop();                          // 主线程进入 loop
```

`start()` 同步等待每个 sub-reactor 就绪后才返回——返回那一刻，`loops_` 中的指针全部"指向已构造好、`tid_` 正确、loop() 已在跑"的 EventLoop。后续 `nextLoop()` 不需要再做就绪检查。

---

## 4. 全流程调用链

### 4.1 场景 A：新连接路由 + 跨线程注册

```
① [main 线程] Acceptor 触发 → TcpServer::newConnection(int fd)
② [main 线程]
   Eventloop* subLoop = threadPool_->nextLoop();    // 轮询，可能回退到 mainLoop_
   conn = make_unique<Connection>(fd, subLoop);
   conn->setOnMessageCallback(...);
   conn->setDeleteConnectionCallback(...);
   Connection* rawConn = conn.get();
   connections_[fd] = std::move(conn);
   if (newConnectCallback_) newConnectCallback_(rawConn);
   subLoop->queueInLoop([rawConn]{ rawConn->enableInLoop(); });

③ [子线程 i, doPendingFunctors] enableInLoop → channel->enableReading + enableET
   此时 sub-reactor 的 tid_ 等于当前线程 → 后续任何 isInLoopThread() 调用结果正确
```

注意第 ② 步本日用的仍是 `queueInLoop`（不是 `runInLoop`），因为 `newConnection` 一定在 main 线程跑、目标是 sub-reactor 线程，已知必跨线程。`runInLoop` 的优化只在"调用方与目标 loop 可能同线程也可能不同线程"时才有意义——见 §4.2。

### 4.2 场景 B：业务回调里调度任务（runInLoop 的典型用法）

设想业务代码在 `onMessage` 回调里要安排"500ms 后给客户端发心跳"（Day 23 引入定时器后会高频出现）。回调本身是在 sub-reactor 线程跑的，要安排的任务要回到同一 sub-reactor 线程执行：

```cpp
server.onMessage([](Connection* conn) {
    Eventloop* ioLoop = conn->getLoop();
    ioLoop->runInLoop([conn]{
        // 想做的事：例如给 conn 发心跳
        conn->send("PING");
    });
});
```

调用 `runInLoop` 时：

- 若调用方在 ioLoop 的归属线程（onMessage 路径几乎总是如此）：`isInLoopThread()` 为 true → 直接 `func()`，零延迟、零分配、零锁。
- 若调用方在别的线程（极少数，例如主线程的某后台任务想给 conn 发数据）：退化成 `queueInLoop` + wakeup。

**调用方完全不需要知道自己当前在哪个线程**——这是 `runInLoop` 真正的价值。Day 21 写好的 `queueInLoop` 在快速路径上仍然要拿一次 mutex、push_back 进 vector、写 wakeup fd，是数百纳秒级开销；本线程直接调省掉这一切。

### 4.3 场景 C：优雅停机（接线后的完整链）

```
① SIGINT → handler → server.stop()（同 Day 21，atomic_flag 幂等）
② TcpServer::stop()
   ├─ threadPool_->stopAll();
   │     for loop in loops_: loop->setQuit(); loop->wakeup();
   └─ mainReactor_->setQuit(); mainReactor_->wakeup();

③ 各子线程 loop() 返回 → threadFunc() 自然结束 → std::thread 可被 join
④ main 线程 mainReactor_->loop() 返回 → server.Start() 返回 → main 收尾
⑤ ~TcpServer()
   ├─ stop();  // 幂等
   ├─ threadPool_->joinAll();   ★ 显式：等所有 IO 线程实际退出
   │     此时 IO 线程已死、EventLoop 对象仍活在 EventLoopThread 内
   └─ 成员按声明逆序自动析构：
       connections_     ← ~Connection → loop_->deleteChannel：EventLoop 仍在 ✓
       threadPool_      ← ~EventLoopThreadPool → ~EventLoopThread
                            → join(no-op，已 joined)
                            → ~Eventloop（unique_ptr 链）
       acceptor_
       mainReactor_
```

第 ⑤ 步的精妙之处：`joinAll()` 必须在 `connections_` 析构之前**显式**调用。如果不显式 join，按声明逆序，`threadPool_` 是 connections_ 之后才析构的，析构 `threadPool_` 时才隐式 join——但那时 `connections_` 已经析构、`~Connection` 已经调过 `loop_->deleteChannel()`，而那时 IO 线程可能还在 poll 里写 events_，是经典数据竞争。`joinAll()` 显式调用把这条窗口拍扁到零。

---

## 5. 析构顺序与生命周期安全分析

`TcpServer` 析构（声明顺序 → 逆序析构）：

| 步骤 | 析构成员 | 此时状态 | 安全性 |
|----|----|----|----|
| 0 (显式) | `stop()` | 全部 reactor 准备退出 loop | 安全（幂等） |
| 0 (显式) | `threadPool_->joinAll()` | 全部 IO 线程已 join；EventLoop 仍活 | **本日核心**：唯一安全窗口 |
| 1 | `connections_` | EventLoop 仍活；IO 线程已死 → 无并发 | 安全：~Connection 调 loop_->deleteChannel 无竞态 |
| 2 | `threadPool_` | connections 已清空 | 安全：~EventLoopThreadPool → ~EventLoopThread → join(no-op) → ~Eventloop |
| 3 | `acceptor_` | mainReactor_ 仍活 | 安全 |
| 4 | `mainReactor_` | — | 安全 |

`EventLoopThread` 析构：

| 步骤 | 成员 | 安全性 |
|----|----|----|
| 0 (显式) | `join()`（也由析构兜底调用） | 安全：检查 joinable，可重复调 |
| 1 | `loop_` (`unique_ptr<Eventloop>`) | join 后子线程已死 → 析构 EventLoop 无并发 |
| 2 | `cv_` / `mutex_` / `loopReady_` | 平凡析构 |
| 3 | `thread_` | 已 joined |

`Eventloop` 析构（同 Day 21）：声明顺序保证 `evtChannel_` 先于 `poller_` 析构。

---

## 6. 文件清单与变更总览

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `include/EventLoop.h` | 修改 | 加 `std::thread::id tid_`；声明 `isInLoopThread()` / `runInLoop()`；include `<thread>` |
| `common/Eventloop.cpp` | 修改 | 构造函数初始化 `tid_(std::this_thread::get_id())`；实现 `isInLoopThread` / `runInLoop` |
| `include/TcpServer.h` | 修改 | 删除 `subReactors_` vector；换成 `unique_ptr<EventLoopThreadPool> threadPool_`；改写整段成员声明顺序注释 |
| `common/TcpServer.cpp` | 修改 | 构造接线 EventLoopThreadPool；`Start()` 调 `threadPool_->start()`；`newConnection` 用 `nextLoop()`；`stop()` 调 `stopAll()`；`~TcpServer()` 调 `stop() + joinAll()`；`hardware_concurrency()` 加 `std::max(1, ...)` 兜底 |
| `include/Airi-Cpp-Server-Lib.h` | 修改 | 暴露 `EventLoopThread.h` / `EventLoopThreadPool.h` |

净行数：~+30 / −10。本日的"代码量小、含义重"再次成立——真正切换运行模型只动了几十行。

---

## 7. 测试与验证

### 7.1 tid 一致性自测

写一个临时小程序（不入库，验证完即弃）：

```cpp
TcpServer server;
server.newConnect([](Connection* conn){
    Eventloop* ioLoop = conn->getLoop();
    assert(ioLoop->isInLoopThread());      // 在 onConnect 回调内必为 true
    ioLoop->runInLoop([ioLoop]{
        assert(ioLoop->isInLoopThread());  // runInLoop 进入后仍为 true
        std::cout << "tid OK on " << std::this_thread::get_id() << "\n";
    });
});
server.Start();
```

任何一条 `assert` 失败都意味着 `tid_` 没在子线程构造里捕获——本日核心改动失效。

### 7.2 跨线程投递不再死锁 / 不再竞态

```bash
cmake -S . -B build && cmake --build build -j4
./build/server &
./build/StressTest 127.0.0.1 8888 200 50000
```

预期：`failed=0`，吞吐与 Day 21 持平（接线本身不显著影响吞吐，只在每条连接建立路径多一次 atomic 比较）。

### 7.3 优雅停机（含 joinAll 保序）

```bash
./build/server &
SERVER_PID=$!
./build/StressTest 127.0.0.1 8888 100 1000 &     # 边压测边关
sleep 0.2
kill -INT $SERVER_PID
wait $SERVER_PID
echo "exit=$?"
```

预期 `exit=0`、关闭过程无 hang、ASan/TSan 在 connections_ 析构路径无任何 race 报警。如果误删 `joinAll()` 的显式调用，TSan 立刻在 `~Connection` → `Channel` 路径上报 "data race with thread T2"（T2 是仍未 join 的 sub-reactor 线程）。

### 7.4 nextLoop 单线程回退

把 `setThreadNums(0)` 临时设为 0 → `loops_` 为空 → `nextLoop()` 应回退到 `mainLoop_`。整个进程退化成单 Reactor 模式仍能跑通；仅用于本日快速回归测试。

---

## 8. 已知限制与未来改进

| 当前局限 | 后续改进方向 |
|----------|------------|
| 没有定时器，连接空闲不会被自动关闭 | Day 23 `TimerQueue` |
| `nextLoop()` 是简单轮询，不感知各 sub-reactor 的负载 | 可后续加最少连接数 / 一致性哈希等策略 |
| `runInLoop` 的"同线程直接执行"对调用栈深度无限制，递归回调可能栈溢出 | 可加深度阈值改投递 |
| 信号路径仍依赖 `signal()` API（Day 21 遗留） | 后续生产化改 `sigaction` + 屏蔽 worker 线程的 SIGINT |
| `EventLoopThread` 内 `loopReady_` 仅靠 cv 单次唤醒；如果未来 `startLoop` 失败需要超时机制 | 可加 `cv_.wait_for` |
