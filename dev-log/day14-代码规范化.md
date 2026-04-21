# Day 14：DISALLOW_COPY_AND_MOVE 宏与代码规范化

> 引入 `Macros.h`，统一 DISALLOW_COPY / DISALLOW_MOVE / DISALLOW_COPY_AND_MOVE 宏。
> 所有核心类（Channel、Epoll、Eventloop、Connection、Server、Acceptor）标记为不可拷贝不可移动。
> 成员命名全部加下划线后缀（`fd_`、`events_`），错误检查函数 `errif` → `ErrIf`。
> EventLoop 新增 `eventfd`（macOS 用 `pipe`）唤醒机制和 `queueInLoop` 跨线程投递。

---

## 1. 引言

### 1.1 问题上下文

到 Day 13，多 Reactor 架构能跑了，但代码工程化程度不够：

1. **类可以被误拷贝**：`Channel`、`Epoll`、`Connection` 等持有 fd 的类如果被拷贝，会发生 double-close。
2. **跨线程函数派发缺失**：subReactor 在子线程跑，但主线程想调 `subReactor->updateChannel()` 或 `subReactor->queueInLoop(f)` 时无安全机制——子线程可能正阻塞在 `epoll_wait`，新任务不会被立即处理。
3. **命名风格不统一**：`fd`、`events`、`callback` 没有 `_` 后缀，与成员/参数歧义。

工业级 C++ 的做法：(a) 用 `DISALLOW_COPY_AND_MOVE` 宏批量禁拷贝；(b) 用 Linux `eventfd` (macOS 用 `pipe`) 做事件循环唤醒，配合 `runInLoop` / `queueInLoop` 实现跨线程任务派发；(c) 成员加 `_` 后缀。

### 1.2 动机

禁拷贝是 RAII 的前提；跨线程派发是 one-loop-per-thread 模型的核心——没有它，多 Reactor 的"无锁"承诺就破产了。

`eventfd` 是 Linux 2.6.22 引入的"内核计数器 + fd"，64-bit 计数加减一次系统调用即可在 epoll 中触发可读事件，是事件循环唤醒的最优解。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 默认拷贝/移动 | C++ 默认 | 零代码 | double-close、不易察觉 |
| `DISALLOW_COPY_AND_MOVE` 宏 (本日) | Google C++ Style / muduo | 一行禁掉、清晰 | 旧 C++03 风格 |
| `= delete` 显式删除 | C++11 | 标准、无宏 | 需写 4 行 |
| `eventfd` 唤醒 | Linux 2.6.22 / muduo | 8 字节读写、原生 epoll 支持 | Linux only |
| `pipe(2)` 唤醒 | POSIX 通用 | 跨平台 | 占两个 fd |
| `kqueue EVFILT_USER` | macOS | 无 fd 占用 | macOS only |
| `signalfd` | Linux | 信号变事件 | 不通用 |

### 1.4 本日方案概述

本日实现：
1. 新增 `Macros.h`：`DISALLOW_COPY` / `DISALLOW_MOVE` / `DISALLOW_COPY_AND_MOVE` / `ASSERT` 宏。
2. 所有核心类标记 `DISALLOW_COPY_AND_MOVE`；成员名统一加 `_` 后缀。
3. `EventLoop` 新增 `evtfd_`（Linux `eventfd` / macOS `pipe[0]`）+ `evtChannel_` 唤醒 channel + `pendingFunctors_` 任务队列 + `mutex_`。
4. `EventLoop::queueInLoop(f)`：把任务 push 到 `pendingFunctors_`；如果当前不在 IO 线程，调 `wakeup()` 写 `evtfd_`。
5. `EventLoop::loop()` 在每轮 `poll` 后调 `doPendingFunctors()` 执行待办任务。
6. `Connection` 引入 `onMessageCallback_`，业务从 Server 移到 Connection 注入。
7. 错误工具 `errif` → `ErrIf` 统一命名。

跨线程派发就绪后，Day 15-22 都能放心地"在任意线程里调度任务到 IO 线程执行"。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Macros.h` | **新增** | 定义 `DISALLOW_COPY`、`DISALLOW_MOVE`、`DISALLOW_COPY_AND_MOVE`、`ASSERT` 宏 |
| `include/Channel.h` | **修改** | 加 `DISALLOW_COPY_AND_MOVE`；成员改名加 `_` 后缀 |
| `include/Epoll.h` | **修改** | 加 `DISALLOW_COPY_AND_MOVE`；成员改名加 `_` 后缀 |
| `include/EventLoop.h` | **重写** | 加宏；新增 `evtfd_`、`evtChannel_`、`pendingFunctors_`、`mutex_`；新增 `queueInLoop()`、`wakeup()` |
| `include/Connection.h` | **修改** | 加宏；新增 `onMessageCallback_`，业务逻辑从 Server 移到 Connection 通过回调注入 |
| `include/Server.h` | **修改** | 加宏；删除内联业务逻辑 |
| `include/util.h` | **修改** | `errif` → `ErrIf` |
| `common/Eventloop.cpp` | **重写** | 构造函数创建 eventfd/pipe + wakeup Channel；新增 `wakeup()`/`handleWakeup()`/`doPendingFunctors()` |
| `common/Channel.cpp` | **修改** | 成员名统一为 `_` 后缀 |
| `common/Epoll.cpp` | **修改** | 成员名统一；`errif` → `ErrIf` |
| `common/Connection.cpp` | **修改** | 新增 `handleRead()`/`handleWrite()`/`send()` 分离 IO 与业务 |
| `common/Server.cpp` | **修改** | 业务 msgCb lambda 移入 `newConnection()` 内部 |

---

## 3. 模块全景与所有权树（Day 14）

```
main()
├── signal(SIGINT, signalHandler)
├── Eventloop* loop (mainReactor)
│   ├── Epoll* ep_
│   ├── int evtfd_ (eventfd / pipe[0])
│   └── Channel* evtChannel_ (wakeup 通道)   ← 新增
└── Server* server
    ├── Acceptor* acceptor_
    │   └── Channel* (listen fd)
    ├── ThreadPool* threadPool_
    │   └── worker → subReactor->loop()
    ├── vector<Eventloop*> subReactors_
    │   └── 每个 subReactor 同样持有 evtfd_ + evtChannel_
    └── map<int, Connection*> connections_
        └── Connection*
            ├── Channel* channel_ (conn fd)
            ├── Buffer inputBuffer_
            ├── Buffer outputBuffer_
            └── onMessageCallback_   ← 新增：业务回调
```

---

## 4. 全流程调用链

**场景 A：跨线程 queueInLoop 投递**
```
Server::deleteConnection(sock)
  → mainReactor_->queueInLoop(lambda)
    → mutex_.lock()
    → pendingFunctors_.push_back(lambda)
    → mutex_.unlock()
    → wakeup()                          ← 写 eventfd/pipe 唤醒 mainReactor
      → write(evtfd_/wakeupFd_[1], ...)
```

**场景 B：mainReactor 被唤醒后执行投递**
```
Eventloop::loop()
  → ep_->poll() 返回 evtChannel_
  → evtChannel_->handleEvent()
    → handleWakeup(): read(evtfd_, ...)  ← 清空 eventfd/pipe
  → doPendingFunctors()
    → swap(pendingFunctors_)             ← 减小临界区
    → 逐个执行 func()
```

**场景 C：Connection 读写分离**
```
Channel::handleEvent()              ← revents 包含 POLLER_READ
  → readCallback = Connection::handleRead
    → inputBuffer_.readFd(sockfd)
    → onMessageCallback_(this)       ← 业务逻辑由外部注入

Channel::handleEvent()              ← revents 包含 POLLER_WRITE
  → writeCallback = Connection::handleWrite
    → write(outputBuffer_)
    → 全部写完 → disableWriting()
```

---

## 5. 代码逐段解析

### 5.1 Macros.h — 禁止拷贝/移动宏
```cpp
#define DISALLOW_COPY(cname)         \
    cname(const cname &) = delete;   \
    cname &operator=(const cname &) = delete;

#define DISALLOW_MOVE(cname)         \
    cname(cname &&) = delete;        \
    cname &operator=(cname &&) = delete;

#define DISALLOW_COPY_AND_MOVE(cname) \
    DISALLOW_COPY(cname);             \
    DISALLOW_MOVE(cname);
```
- 放在 class 体 `public:` 或 `private:` 下使用
- 语义：这些类持有独占资源（fd、指针），拷贝/移动会导致 double-free

### 5.2 EventLoop 的唤醒机制
```cpp
// Linux: eventfd (单 fd，8 字节计数器)
evtfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
// macOS: pipe (两个 fd，读端 wakeupFd_[0]，写端 wakeupFd_[1])
pipe(wakeupFd_); fcntl(wakeupFd_[0], F_SETFL, O_NONBLOCK);
```
- `wakeup()` 往写端写 1 字节 → kqueue/epoll 检测到读就绪 → 执行 `handleWakeup()`

### 5.3 Connection::send() — 高低水位发送
```cpp
void Connection::send(const std::string &msg) {
    // 1. 尝试直写：若 socket 可写且 outputBuffer 为空
    nwrote = ::write(sock_->getFd(), msg.data(), msg.size());
    // 2. 写不完：剩余追加到 outputBuffer，注册 EPOLLOUT
    outputBuffer_.append(msg.data() + nwrote, remaining);
    channel_->enableWriting();
}
```

---

### 5.4 client.cpp（Day 14 客户端同步）

`client.cpp` 同步采用 Day 14 的命名规范（成员加 `_` 后缀、`ErrIf` 替换 `errif`），便于编译时与 server 端共享 `util` 工具集。客户端的交互循环逻辑未改变，仍然是 `fgets → write → read → print` 的同步流程。


### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day14/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day14/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 模块 | 职责 |
|------|------|
| **Macros.h** | 提供 DISALLOW_COPY_AND_MOVE 等宏，防止资源持有类被错误拷贝 |
| **EventLoop** | 新增 wakeup 机制（eventfd/pipe）+ queueInLoop 跨线程任务投递 |
| **Connection** | 持有读写 Buffer，读写分离为 handleRead/handleWrite，业务通过回调注入 |
| **Server** | 在 newConnection 中设置 msgCb，deleteConnection 通过 queueInLoop 投递到主线程 |

---

## 7. 局限

1. `deleteConnection` 投递到 mainReactor 执行，但 `connections_` map 无锁保护（依赖单线程执行）
2. 缺少连接超时和空闲检测机制
3. `send()` 中 `EPIPE`/`ECONNRESET` 仅设 `faultError` 标记，未通知上层
