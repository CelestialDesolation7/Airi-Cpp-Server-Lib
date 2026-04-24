# Day 21：安全停机与所有权 RAII（stop / shared_ptr guard / EventLoopThread 准备）

> **主题**：把 TcpServer 的"开机—运行—关机"链路修成可控、幂等、无死锁；把 Eventloop 内部资源全部 `unique_ptr` 化；引入（暂未启用的）`EventLoopThread` / `EventLoopThreadPool` 类，为 Day 22 真正的 one-loop-per-thread 铺底座。
> **基于**：Day 20（Connection 懒注册 + 析构注销）

---

## 1. 引言

### 1.1 问题上下文

Day 20 把 Connection 的生命周期边界修干净了，但**进程级别的关机路径仍是坏的**。`server.cpp` 里只是：

```cpp
TcpServer *server = new TcpServer();
Signal::signal(SIGINT, [&] { delete server; exit(0); });
server->Start();   // 阻塞在 mainReactor_->loop()
delete server;     // Start() 返回后又删一次 → double free
```

主进程进入 `mainReactor_->loop()` 之后陷在 `kevent`/`epoll_wait` 里出不来。`Eventloop::quit_` 是 `bool`、不是 `atomic`、改它对 poll 中阻塞的线程不可见；信号到来后只能靠 `delete server` 这种"硬拔"的方式收场，与 `Start()` 退出后的 `delete server` 撞车。这条路径在调试时偶尔表现为段错误、偶尔表现为死锁——典型的"信号 + 多线程"经典坑。

ThreadPool join 同样存在死锁：worker 线程跑的是 `subReactor->loop()`，loop 永远不会自己返回。`~ThreadPool` 在 worker 还在 loop 中时调 `join()`，等于永远等。要么 worker 主动退出 loop，要么 join 永远阻塞。

第二条线是**资源所有权**：Day 20 的 `Eventloop` 仍然 `Poller* poller_{nullptr}` / `Channel* evtChannel_{nullptr}` / `bool quit_{false}`，整一组裸指针 + 普通 bool。析构靠手写 `delete`，跨线程读 `quit_` 是 data race。

第三条线是**多线程模型**：Day 20 的 `subReactors_` 是 vector<unique_ptr<Eventloop>>，全部在主线程构造、随后被 ThreadPool 调度去跑 `loop()`。`Eventloop` 没有 `tid_`，没人校验"我现在是不是该 reactor 的归属线程"。日志说"投递到归属 sub-reactor"完全靠程序员心算 fd → idx，运行时无任何防御。

### 1.2 动机

要支持热升级、滚动重启、零停机部署，"能优雅停机"是任何长期服务的入门门槛。本日不上业务功能，只把这三件事做完：可控关机、所有权 RAII、为真正的多线程模型 EventLoopThread 准备好类（虽然今天还不接线）。

之所以"今天准备类、明天才启用"是有意安排的：`EventLoopThread` 引入了 `condition_variable` 同步、`tid_` 捕获、`joinAll`/`stopAll` 接口几个新概念，单独成立、先有完整自测的"骨架"提交，再在 Day 22 切换 `TcpServer` 接线，比一天里把"骨架 + 接线 + 上线"全做完更容易回滚。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| `delete server; exit(0)` | Day 19/20 | 写起来短 | 信号上下文里调任意 C++ 析构非异步信号安全；与 Start() 返回后 delete 撞车 |
| `stop()` 设置原子 quit + `wakeup()`（本日） | muduo | 异步信号安全（只动 atomic + 写 8 字节到 wake fd）；幂等 | 需要 wakeup eventfd / pipe 配合 |
| `signalfd` / `kqueue EVFILT_SIGNAL` | Linux/BSD | 信号变成普通可读事件 | 平台特定 |
| Boost.Asio `signal_set` | Asio | 跨平台 + 协程友好 | 引入 Asio 全家桶 |
| Java `Runtime.addShutdownHook` | JVM | 由 JVM 调度执行 | 跨语言 |

`shared_ptr<Connection>` guard（取代 Day 20 的 `release()` + raw `delete`）也是 muduo 的标准技法：跨线程移交销毁时，用 `shared_ptr` 包装 `unique_ptr` 满足 `std::function` 的可复制性，引用计数归零自动析构，不再手写 `release` / `delete` 配对。

### 1.4 本日方案概述

本日实现：

1. `Eventloop` 三件套全部 `unique_ptr` 化：`unique_ptr<Poller> poller_` / `unique_ptr<Channel> evtChannel_` / `atomic<bool> quit_`。析构函数从手写 `delete` 改成 `= default`，依赖声明顺序保证正确性。
2. `Poller::newDefaultPoller` 返回 `unique_ptr<Poller>`，从源头消除"工厂吐裸指针"歧义。`EpollPoller::events_` / `KqueuePoller::events_` 从 `T*` 改成 `std::vector<T>`。
3. `ThreadPool::add` 从 `std::result_of` 升级到 C++17 `std::invoke_result_t`，消除 `-Wdeprecated` 噪音。
4. `TcpServer::stop()`：遍历 `subReactors_` 调 `setQuit() + wakeup()`、再处理 `mainReactor_`，保证所有 `loop()` 能从 `poll()` 返回；`~TcpServer()` 在成员析构前显式调 `stop()`。
5. `TcpServer::deleteConnection` 用 `std::shared_ptr<Connection> guard(std::move(conn))` 取代 Day 20 的 `release()` + `delete raw`，析构由引用计数触发。
6. `TcpServer.h` 写明完整的"声明顺序 / 析构顺序 / 安全性"注释（被外部依赖的内部约定显式化）。
7. `server.cpp` 改为栈对象 `TcpServer server`，SIGINT 处理函数用 `std::atomic_flag` 保证幂等（信号可能被任意线程接收、可能多次到达），处理函数只调 `server.stop()`，不再 `delete` / `exit`。
8. **新增但本日未启用** 的 `EventLoopThread` / `EventLoopThreadPool`：完整定义 + 实现 + 注释，但 `TcpServer` 仍走 `vector<unique_ptr<Eventloop>> + ThreadPool` 路径。

本日不做：

- 不切换 `TcpServer` 到 `EventLoopThreadPool`（Day 22）。
- 不引入 `Eventloop::tid_` / `isInLoopThread()` / `runInLoop()`（Day 22）。
- 不引入定时器（Day 23）。

---

## 2. 模块全景与所有权树

```
TcpServer  ── 栈对象，由 main() 持有
├── unique_ptr<Eventloop>          mainReactor_
│   ├── unique_ptr<Poller>         poller_       ★ 本日 unique_ptr 化
│   ├── unique_ptr<Channel>        evtChannel_   ★ 本日 unique_ptr 化
│   └── atomic<bool>               quit_          ★ 本日原子化
├── unique_ptr<Acceptor>           acceptor_
├── vector<unique_ptr<Eventloop>>  subReactors_   // 仍存在，Day 22 才会被 EventLoopThreadPool 替换
├── unordered_map<int, unique_ptr<Connection>> connections_
└── unique_ptr<ThreadPool>         threadPool_    // 跑 sub-reactor::loop()，仍为本日主路径

★ 已定义但 TcpServer 尚未使用：
EventLoopThread
├── unique_ptr<Eventloop>  loop_       // 由 threadFunc 在子线程内 make_unique
├── std::thread            thread_
├── std::mutex / std::condition_variable
└── bool                   loopReady_

EventLoopThreadPool
├── Eventloop*                            mainLoop_   // nextLoop 无子线程时回退
├── vector<unique_ptr<EventLoopThread>>   threads_
└── vector<Eventloop*>                    loops_      // 观察者裸指针
```

**所有权规则**

- `Eventloop` 拥有 `Poller` / `Channel(wakeup)`，析构顺序由声明顺序自动保证：先析构 `evtChannel_`（从 `poller_` 注销），再析构 `poller_`。
- `quit_` 是跨线程读写的状态位，必须 `atomic<bool>`；signal handler 改 `quit_` 之后再写 wakeup fd，poll 一旦返回就能看到最新值。
- `EventLoopThread` 持有 `Eventloop` 的 `unique_ptr`，但**对象本身在子线程中构造**——这是 Day 22 真正解决"`tid_` 与运行线程一致"问题的前置条件。

---

## 3. 初始化顺序（构造阶段）

```
[main 线程]
①  TcpServer server;                                // 栈对象
    ├─ mainReactor_   = make_unique<Eventloop>()
    │    ├─ poller_   = Poller::newDefaultPoller(this)   // unique_ptr<Poller> 直接 move 赋值
    │    ├─ evtfd / wakeup pipe 创建
    │    └─ evtChannel_ = make_unique<Channel>(this, wakeFd) → enableReading
    ├─ acceptor_      = make_unique<Acceptor>(mainReactor_.get())
    ├─ acceptor_->setNewConnectionCallback(TcpServer::newConnection)
    ├─ threadNum      = std::thread::hardware_concurrency()
    ├─ threadPool_    = make_unique<ThreadPool>(threadNum)
    └─ for i in 0..threadNum: subReactors_.emplace_back(make_unique<Eventloop>())

② signal handler 注册：
    static atomic_flag fired = ATOMIC_FLAG_INIT;
    signal(SIGINT, [&]{
        if (fired.test_and_set()) return;          // 幂等
        server.stop();                              // 仅信号安全的两个原子写 + wakeup write
    });

③ server.newConnect / server.onMessage              // 注入业务回调

④ server.Start();
    ├─ for sub : subReactors_: threadPool_->add([sub]{ sub->loop(); })
    └─ mainReactor_->loop();                        // 主线程进入 loop（阻塞）
```

`EventLoopThread` / `EventLoopThreadPool` 在这条链路上一次都不会被构造——本日只是把这两类放进库里，等 Day 22 才接到 `TcpServer` 上。

---

## 4. 全流程调用链

### 4.1 场景 A：优雅停机（信号触发）

```
① [任一线程, 内核交付 SIGINT] handler 触发
② [signal handler]
   if (fired.test_and_set()) return;   // 二次进入直接返回
   server.stop();
       │
       ├─ for sub : subReactors_: sub->setQuit(); sub->wakeup();
       │      // setQuit 写 atomic<bool>; wakeup 向 sub 的唤醒 fd write 8 字节
       │      // 两步均异步信号安全（仅写 atomic + write(2)）
       └─ mainReactor_->setQuit(); mainReactor_->wakeup();

③ [所有 reactor 线程] poll(...) 因 wakeup fd 就绪而返回
   handleEvent 处理 wakeup → 进入 doPendingFunctors → 回到 loop 顶部
   while (!quit_) 检查 atomic<bool>，发现已 true → loop() 返回
④ [worker 线程] sub->loop() 返回 → ThreadPool::worker 返回到 worker 主循环
   ThreadPool 析构时 join 才能成功

⑤ [main 线程] mainReactor_->loop() 返回 → server.Start() 返回 → main 函数收尾
   server 作为栈对象走出作用域 → ~TcpServer()
       ├─ stop()  // 幂等：再次调用安全（quit_ 已经为 true，wakeup 是 idempotent）
       ├─ 成员按声明逆序析构：
       │   threadPool_ → connections_ → subReactors_ → acceptor_ → mainReactor_
       └─ 详见 §5
```

**为什么 `stop()` 是异步信号安全的**：唯一两类操作是 `atomic<bool>::store` 和 `write(wakeFd, ...)`，前者按 C++ 标准对 `std::atomic` 提供保证，后者是 POSIX 列出的 async-signal-safe 函数。整个 stop 路径不分配内存、不调可能持锁的 libc 函数。

### 4.2 场景 B：连接销毁（shared_ptr guard 取代 release/delete）

```
① [sub-reactor 线程] Connection 检测到 EOF → state_ = kDisconnecting
② [sub-reactor 线程] Business() 走 close 分支 → deleteConnectionCallback_(fd)
                                              → TcpServer::deleteConnection(fd)
③ [sub-reactor 线程, 仅一次 queueInLoop 跨线程跳转]
   mainReactor_->queueInLoop([this, fd]{
       auto it = connections_.find(fd);
       if (it == connections_.end()) return;
       Eventloop* ioLoop = it->second->getLoop();
       std::unique_ptr<Connection> conn = std::move(it->second);
       connections_.erase(it);
       std::shared_ptr<Connection> guard(std::move(conn));     // ★ 本日改动
       ioLoop->queueInLoop([guard]{ /* guard 析构 = Connection 析构 */ });
   });
④ [main 线程, doPendingFunctors] 上面的 lambda 在主线程里执行：
   - connections_ map 在 main 单线程修改 → 无 mutex
   - shared_ptr<Connection> guard 拷贝进第二个 lambda → 引用计数 +1
   - 第一个 lambda 返回 → guard 局部变量析构 → 引用计数 -1（但 lambda 内引用还在）
⑤ [sub-reactor 线程, doPendingFunctors] 第二个 lambda 执行 → guard 析构 → 引用计数归零
   → ~Connection() → loop_->deleteChannel(channel_.get()) → 安全销毁
```

与 Day 20 的对比：Day 20 用 `release()` + `delete raw`，意味着 lambda 执行失败（极端情况：lambda 投递成功但 sub-reactor 已先析构）就泄漏。本日 `shared_ptr` 让"无论谁先析构、引用计数自然归零"，泄漏路径被堵死。

### 4.3 场景 C：信号被多次触发 / 多线程同时收到

```
① 客户端按 Ctrl-C 后再快速按一次（或 supervisor 同时发两次 SIGINT）
② 两个线程同时进入 handler
   thread A: fired.test_and_set() → 之前是 false，set 为 true，返回 false → 继续 stop()
   thread B: fired.test_and_set() → 之前已是 true，返回 true → 立即 return
③ 只有一次 stop() 被实际执行；后续 ~TcpServer() 内的 stop() 也因 quit_ 已 true、wakeup 幂等而无副作用
```

幂等是这一段的核心保障——任何"信号可能多次到达"的环境（systemd / supervisor / shell trap）都能稳定收场。

---

## 5. 析构顺序与生命周期安全分析

`TcpServer` 析构顺序（声明逆序）：

| 步骤 | 析构成员 | 此时其他成员状态 | 安全性 |
|----|----|----|----|
| 0 (显式) | `stop()` 调用 | 所有 reactor 仍在 loop 中或已退出（幂等） | 安全：stop 只动 atomic + wakeup |
| 1 | `threadPool_` | worker 线程已从 sub->loop() 返回（因 stop 触发）；可正常 join | 安全：本日的关键修复 |
| 2 | `connections_` | `subReactors_` 仍存活；worker 已退出 → 无并发 | 安全：~Connection 调 loop_->deleteChannel 时 EventLoop 仍在、无竞态 |
| 3 | `subReactors_` | 所有 Channel 已注销 | 安全 |
| 4 | `acceptor_` | mainReactor_ 仍存活 | 安全 |
| 5 | `mainReactor_` | — | 安全：unique_ptr 链按声明逆序销毁 evtChannel_ → poller_ |

`Eventloop` 析构顺序（声明逆序）：

| 步骤 | 成员 | 安全性 |
|----|----|----|
| 1 | `evtChannel_` (`unique_ptr<Channel>`) | ~Channel 不主动注销（poller_ 仍存活） |
| 2 | `poller_` (`unique_ptr<Poller>`) | 析构时清理整个 kqueue/epoll fd，所有 Channel 句柄一并失效 |
| 3 | `quit_` (`atomic<bool>`) | 平凡析构 |

**关键不变量**：`evtChannel_` 必须先析构（即声明在 `poller_` 之后），否则 ~Poller 在析构 epoll/kqueue fd 时如果 Channel 还活着，Channel 持有的 `loop_->deleteChannel` 期望路径就反过来了。本日把这条规则写进了 `EventLoop.h` 的注释，未来加新成员时可参照。

---

## 6. 文件清单与变更总览

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `include/EventLoop.h` | 修改 | `poller_` / `evtChannel_` 改 `unique_ptr`；`quit_` 改 `atomic<bool>`；加成员声明顺序注释 |
| `common/Eventloop.cpp` | 修改 | 用 `make_unique` 创建；析构函数移除 `delete`，依赖 RAII |
| `include/Poller/Poller.h` | 修改 | `newDefaultPoller` 返回 `unique_ptr<Poller>` |
| `include/Poller/EpollPoller.h` | 修改 | `events_` 从 `epoll_event*` 改为 `std::vector<epoll_event>` |
| `include/Poller/KqueuePoller.h` | 修改 | `events_` 从 `struct kevent*` 改为 `std::vector<struct kevent>` |
| `common/Poller/DefaultPoller.cpp` | 修改 | `make_unique<EpollPoller / KqueuePoller>` 返回 |
| `common/Poller/epoll/EpollPoller.cpp` | 修改 | 适配 `vector<epoll_event>` |
| `common/Poller/kqueue/KqueuePoller.cpp` | 修改 | 适配 `vector<struct kevent>` |
| `include/ThreadPool.h` | 修改 | `result_of` → `invoke_result_t`（C++17） |
| `include/TcpServer.h` | 修改 | 加详尽的成员声明顺序 / 析构顺序注释；声明 `void stop()` |
| `common/TcpServer.cpp` | 修改 | 实现 `stop()`；`~TcpServer()` 调 `stop()`；`deleteConnection` 用 `shared_ptr<Connection> guard` |
| `server.cpp` | 修改 | 栈对象 `TcpServer server`；`atomic_flag` 幂等 SIGINT；handler 只调 `server.stop()` |
| `client.cpp` | 修改 | 配合 server 的接口微调（小） |
| `test/StressTest.cpp` | 修改 | 收尾路径配合 stop 行为 |
| `include/EventLoopThread.h` | **新增** | EventLoopThread 类定义（本日尚未启用） |
| `common/EventLoopThread.cpp` | **新增** | 实现：`startLoop` / `threadFunc` / `join` |
| `include/EventLoopThreadPool.h` | **新增** | EventLoopThreadPool 类定义（本日尚未启用） |
| `common/EventLoopThreadPool.cpp` | **新增** | 实现：`start` / `nextLoop` / `stopAll` / `joinAll` |

---

## 7. 测试与验证

### 7.1 信号路径

```bash
cmake -S . -B build && cmake --build build -j4
./build/server &
SERVER_PID=$!
sleep 0.5
kill -INT $SERVER_PID
wait $SERVER_PID
echo "exit=$?"
```

预期：`exit=0`（栈对象正常析构、无 abort）；多次重复 `kill -INT` 不影响结果（幂等）。

### 7.2 死锁回归

将 sub-reactor 的 `loop()` 内 sleep 模拟"长事件回调"，再发 SIGINT。Day 19/20 会卡在 `~ThreadPool` 的 join 上；本日的 `stop()` 通过 wakeup 立刻把 poll 唤醒，loop 即可退出，join 顺利完成。

### 7.3 StressTest 沿用

```bash
./build/server &
./build/StressTest 127.0.0.1 8888 200 50000
kill -INT %1
```

预期 `failed=0`，关闭过程无 hang，无 ASan/TSan 报错。`shared_ptr` guard 的引入对性能基本无影响（每条连接销毁路径多一对 atomic 引用计数操作，远小于 `epoll_ctl` 系统调用开销）。

### 7.4 EventLoopThread 自测（独立小程序）

由于 `TcpServer` 本日未接线，写一个临时小程序（不入库）确认骨架可用：

```cpp
EventLoopThread t;
Eventloop* loop = t.startLoop();        // 阻塞至 EventLoop 在子线程构造完成
loop->queueInLoop([]{ std::cout << "hello from sub\n"; });
std::this_thread::sleep_for(std::chrono::milliseconds(100));
loop->setQuit(); loop->wakeup();
t.join();
```

预期看到 `hello from sub` 输出后干净退出。Day 22 才把它装进 `TcpServer`。

---

## 8. 已知限制与未来改进

| 当前局限 | 后续改进方向 |
|----------|------------|
| `subReactors_` 仍在主线程构造，`Eventloop::tid_` 不准确 | Day 22 由 `EventLoopThread::threadFunc` 在子线程内构造 |
| 跨线程投递（场景 4.2 的 `ioLoop->queueInLoop`）仍无运行时校验 | Day 22 加 `isInLoopThread()` + `runInLoop()` |
| `EventLoopThread`/`Pool` 类已存在但 `TcpServer` 还在用 `ThreadPool + vector` | Day 22 切换 |
| `signal()` API 在多线程下行为定义不严格（应改 `sigaction` + 屏蔽其他线程） | 后续生产化改进 |
| 没有为定时器留位置 | Day 23 引入 `TimerQueue` |
