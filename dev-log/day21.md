# Day 21：EventLoopThread + EventLoopThreadPool（one-loop-per-thread）

> 引入 `EventLoopThread` 和 `EventLoopThreadPool`，实现真正的 one-loop-per-thread 模型。
> EventLoop 在子线程内部构造，保证 `tid_` 与运行线程一致。
> TcpServer 从 `ThreadPool + vector<Eventloop>` 切换到 `EventLoopThreadPool`。
> EventLoop 新增 `isInLoopThread()` / `runInLoop()` 方法。

---

## 1. 引言

### 1.1 问题上下文

到 Day 20，subReactor 虽然各自跑在独立 worker 线程里，但 EventLoop 对象本身是在主线程构造的——`tid_`（构造时捕获的 `gettid()`）记录的是主线程 ID，导致 `isInLoopThread()` 永远返回 false，`runInLoop` 永远走 `queueInLoop` 慢路径。

正确做法是 muduo 的 **EventLoopThread**：每个子线程"自己创建自己的 EventLoop"，主线程通过 `condition_variable` 同步等待 EventLoop 构造完成再拿指针。这才是真正的 one-loop-per-thread——`tid_` 与运行线程一致，无锁优化生效。

### 1.2 动机

one-loop-per-thread 的核心承诺是"同一 EventLoop 上的所有操作都在同一线程，无需加锁"。如果 `tid_` 是错的，所有"`isInLoopThread` 走快路径"的优化全部失效，等于多 Reactor 退化成"多线程共享一个慢路径"。

EventLoopThreadPool 把 N 个 EventLoopThread 封装成一个池，`nextLoop()` 轮询返回下一个 loop——这就是上层 TcpServer 看到的"线程池"接口，与 Day 10 的 ThreadPool 形成对照（任务式 vs 事件循环式）。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 主线程 new EventLoop，子线程 run | Day 13-20 | 简单 | tid 不一致、isInLoopThread 失效 |
| EventLoopThread 子线程内部构造 (本日) | muduo | tid 正确、无锁优化生效 | 需要 mutex/cv 同步初始化 |
| 协程 + 事件循环（Tokio runtime）| Tokio | 调度更细粒度 | 协程染色 |
| Erlang scheduler + reduction count | Erlang/BEAM | 抢占式公平 | 跨语言 |
| `pthread_create` + 共享 epoll | 一些早期 C 服务器 | 无线程绑定 | 同 fd 多线程读 race |

### 1.4 本日方案概述

本日实现：
1. 新增 `EventLoopThread`：构造接收 thread name；`startLoop()` 开线程、`condition_variable` 等待线程内 `loop_` 就绪、返回 `Eventloop*`。
2. `EventLoopThread::threadFunc()`：子线程内 `make_unique<Eventloop>()` → `notify` 主线程 → 进入 `loop_->loop()`。
3. 新增 `EventLoopThreadPool`：管理 N 个 `EventLoopThread`，`nextLoop()` 轮询、`stopAll()`、`joinAll()`。
4. `EventLoop` 新增 `tid_`（构造时 `gettid()`）+ `isInLoopThread()` + `runInLoop(f)`：在本线程直接执行，否则 `queueInLoop(f) + wakeup()`。
5. `EventLoop::quit_` 改为 `atomic<bool>`；成员改 `unique_ptr`。
6. `TcpServer` 用 `EventLoopThreadPool` 替代 `ThreadPool + vector<Eventloop>`。
7. `server.cpp` 用 `atomic_flag` 实现幂等 SIGINT 处理。

后续所有跨线程操作（HTTP keep-alive 定时关闭、异步日志投递、TimerQueue 跨线程添加）都基于 `runInLoop`。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/EventLoopThread.h` | **新增** | 将 EventLoop 与线程绑定；`startLoop()` 同步等待就绪 |
| `common/EventLoopThread.cpp` | **新增** | 子线程构造 EventLoop → 通知 → 进入 `loop()` |
| `include/EventLoopThreadPool.h` | **新增** | 管理 N 个 IO 线程；`nextLoop()` 轮询、`stopAll()` / `joinAll()` |
| `common/EventLoopThreadPool.cpp` | **新增** | 线程池启动、轮询、安全关闭实现 |
| `include/EventLoop.h` | **修改** | 新增 `tid_`、`isInLoopThread()`、`runInLoop()`；`quit_` 改为 `atomic<bool>`；成员改 `unique_ptr` |
| `common/Eventloop.cpp` | **修改** | 构造时捕获 `tid_`；实现 `isInLoopThread()` / `runInLoop()`；改用 `unique_ptr` |
| `include/TcpServer.h` | **修改** | 用 `EventLoopThreadPool` 替代 `ThreadPool + subReactors_` |
| `common/TcpServer.cpp` | **修改** | 使用 `threadPool_->nextLoop()` 轮询；析构时 `joinAll()` |
| `server.cpp` | **修改** | 栈对象 + `atomic_flag` 幂等信号处理 |

---

## 3. 核心改动详解

### 3.1 EventLoopThread — 线程与 EventLoop 绑定

```cpp
Eventloop *EventLoopThread::startLoop() {
    thread_ = std::thread(&EventLoopThread::threadFunc, this);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return loopReady_; });
    return loop_.get();
}

void EventLoopThread::threadFunc() {
    loop_ = std::make_unique<Eventloop>();   // tid_ 在子线程捕获
    { lock; loopReady_ = true; cv_.notify_one(); }
    loop_->loop();                            // 阻塞直到 setQuit
}
```

- EventLoop 在归属线程内构造 → `tid_` 一致 → `isInLoopThread()` 始终正确
- `startLoop()` 同步等待，返回后 EventLoop 立即可用

### 3.2 EventLoopThreadPool — 线程池管理

```
EventLoopThreadPool
├── start()     → N 个 EventLoopThread::startLoop()
├── nextLoop()  → 轮询返回 sub-reactor（fd % N）
├── stopAll()   → 所有 sub-loop setQuit() + wakeup()
└── joinAll()   → 等待所有线程退出（不销毁 EventLoop）
```

### 3.3 TcpServer 析构安全

```
~TcpServer()
  → stop()           // 令所有 reactor 退出 loop
  → joinAll()        // join 线程但不销毁 EventLoop
  → connections_ 析构  // loop->deleteChannel() 安全调用
  → threadPool_ 析构   // EventLoopThread → EventLoop 最终销毁
```

---

## 4. 模块全景与所有权树（Day 21）

```
main()
├── Signal::signal(SIGINT, [&]{ server.stop(); })
└── TcpServer server
    ├── unique_ptr<Eventloop> mainReactor_
    ├── unique_ptr<Acceptor> acceptor_
    ├── unique_ptr<EventLoopThreadPool> threadPool_
    │   ├── vector<unique_ptr<EventLoopThread>> threads_
    │   │   └── EventLoopThread
    │   │       ├── unique_ptr<Eventloop> loop_    ← 子线程构造
    │   │       └── std::thread thread_
    │   └── vector<Eventloop*> loops_              ← 观察者指针
    └── unordered_map<int, unique_ptr<Connection>> connections_
```

---

## 5. 构建

```bash
cmake -S . -B build && cmake --build build -j4
```

生成 `server`、`client`、`ThreadPoolTest`、`StressTest` 四个可执行文件。


### 5.1 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day21/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day21/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

