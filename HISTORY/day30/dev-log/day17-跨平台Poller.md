# Day 17：跨平台 Poller — 用 `#ifdef` 统一 epoll/kqueue

> 删除 `Epoll.h/cpp`，引入 `Poller.h/cpp`，在同一文件中用 `#ifdef OS_LINUX / OS_MACOS` 分支分别实现 epoll 和 kqueue。
> `Macros.h` 新增平台检测宏 `OS_LINUX` / `OS_MACOS`。
> `Channel` 不再直接引用 `<sys/epoll.h>`，改用自定义平台中立标志 `READ_EVENT / WRITE_EVENT / ET`。

---

## 1. 引言

### 1.1 问题上下文

从 Day 03 到 Day 16，跨平台 IO 多路复用一直靠"在每个使用 epoll/kqueue 的地方写 `#ifdef`"。这种散布的条件编译有三个问题：(a) 重复——同一段平台逻辑在 4-5 个文件里反复出现；(b) 易错——某个文件忘加 `#ifdef` 就只在某一平台编译过；(c) Channel 暴露了 `EPOLLIN` 等平台原生标志，使用方被迫了解两套常量。

正确做法是**集中跨平台细节**：把 epoll 与 kqueue 的实现都收到一个 `Poller.cpp` 里，文件内部用 `#ifdef` 切两个分支，对外只暴露统一接口；同时 Channel 用自定义平台中立标志（READ_EVENT / WRITE_EVENT / ET），与底层多路复用解耦。

### 1.2 动机

这是一次"消除条件编译散布"的重构。优点：(a) 只改 Poller 一个文件就能新增对 io_uring / IOCP 的支持；(b) Channel 不再暴露平台细节，用户层代码完全平台中立；(c) `#ifdef` 只在两个文件出现（Poller + EventLoop 唤醒机制）。

这一步还为 Day 18 的进一步重构（用策略模式 OOP 拆分而非 `#ifdef` 分支）做铺垫——先证明"集中"是对的，再讨论"集中后是否换更优雅的形态"。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| `#ifdef` 散布全项目 | 早期跨平台 C 代码 | 改动小 | 重复、易错 |
| 同文件内 `#ifdef` 集中 (本日) | libevent / 本项目 | 一处修改 | 仍有两套实现交织 |
| 策略模式（基类 + 子类） | muduo `Poller` / `EpollPoller` / `PollPoller` (Day 18) | OOP 清晰、可单测 | 多了虚函数调用开销（实际可忽略） |
| `boost::asio::*` 透明跨平台 | Asio | 用户完全无感知 | 黑盒、定制困难 |
| Rust mio backend | mio | 编译期选 backend | 跨语言 |

### 1.4 本日方案概述

本日实现：
1. 删除 `Epoll.h/cpp`，新建 `Poller.h/cpp`。
2. `Poller` 类：用 `#ifdef __linux__` / `#ifdef __APPLE__` 分支声明 epoll_event* / kevent* 成员；`updateChannel` / `deleteChannel` / `poll` 三个方法在同文件内分别给出两套实现。
3. `Macros.h` 新增 `OS_LINUX` / `OS_MACOS` 平台宏（基于 `__linux__` / `__APPLE__`）。
4. `Channel` 删除对 `<sys/epoll.h>` 的依赖，新增静态常量 `READ_EVENT` / `WRITE_EVENT` / `ET`，由 Poller 翻译成平台原生标志。
5. `EventLoop` 唤醒机制保留 `#ifdef`：Linux `eventfd`、macOS `pipe`。

下一天用策略模式（`EpollPoller` / `KqueuePoller` 两个子类 + 工厂）替代同文件 `#ifdef`，让结构更 OOP。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Macros.h` | **修改** | 新增 `OS_LINUX` / `OS_MACOS` 平台宏（基于 `__linux__` / `__APPLE__`） |
| `include/Poller.h` | **新增** | 替代 `Epoll.h`；`#ifdef` 分支声明 `epoll_event*` 或 `kevent*` 成员 |
| `common/Poller.cpp` | **新增** | 替代 `Epoll.cpp`；同一文件内 epoll/kqueue 两套实现 |
| `include/Channel.h` | **修改** | 删除 `Epoll` 依赖；新增静态常量 `READ_EVENT`、`WRITE_EVENT`、`ET` |
| `common/Channel.cpp` | **修改** | 移除 `<sys/epoll.h>`；使用自定义标志位 |
| `include/EventLoop.h` | **修改** | `#ifdef OS_LINUX` eventfd / `#ifdef OS_MACOS` pipe 唤醒机制 |
| `common/Eventloop.cpp` | **修改** | 条件编译 eventfd / pipe 初始化、读写、析构 |
| `include/Epoll.h` | **删除** | 被 `Poller.h` 取代 |
| `common/Epoll.cpp` | **删除** | 被 `Poller.cpp` 取代 |

---

## 3. 模块全景与所有权树（Day 17）

```
main()
├── Signal::signal(SIGINT, handler)
├── Eventloop* loop
│   ├── Poller* poller_                    ← Epoll → Poller
│   │   ├── [Linux] epoll_event* events_
│   │   └── [macOS] kevent* events_
│   └── Channel* evtChannel_
│       ├── [Linux] eventfd
│       └── [macOS] pipe(read/write)
└── Server* server
    ├── Acceptor* acceptor_
    ├── ThreadPool* threadPool_
    ├── vector<Eventloop*> subReactors_
    └── map<int, Connection*> connections_
        └── Connection*
            ├── Channel* channel_
            ├── Buffer inputBuffer_
            └── Buffer outputBuffer_
```

---

## 4. 全流程调用链

**场景 A：Poller 跨平台抽象**
```
Eventloop::Eventloop()
  → poller_ = new Poller()
    ├── [Linux] epoll_create1(0)  → epoll_event[1024]
    └── [macOS] kqueue()          → kevent[1024]

Eventloop::loop()
  → poller_->poll()
    ├── [Linux] epoll_wait → EPOLLIN/EPOLLOUT → Channel::READ_EVENT/WRITE_EVENT
    └── [macOS] kevent     → EVFILT_READ/WRITE → Channel::READ_EVENT/WRITE_EVENT
  → Channel::handleEvent()
```

**场景 B：Channel 平台中立标志**
```
Channel::enableReading()
  → listen_events_ |= READ_EVENT (=1)
  → loop_->updateChannel(this)
    → Poller::updateChannel(channel)
      ├── [Linux] EPOLLIN | EPOLLPRI
      └── [macOS] EV_SET(EVFILT_READ, EV_ADD|EV_ENABLE)
```

**场景 C：Eventloop 唤醒机制**
```
Eventloop::wakeup()
  ├── [Linux] write(evtfd_, 1)
  └── [macOS] write(wakeupWriteFd_, 'w')

Eventloop::handleWakeup()
  ├── [Linux] read(evtfd_)
  └── [macOS] while(read(wakeupReadFd_) > 0)
```

---

## 5. 代码逐段解析

### 5.1 Macros.h — 平台检测
```cpp
#ifdef __linux__
#define OS_LINUX
#endif
#ifdef __APPLE__
#define OS_MACOS
#endif
```
- 编译器预定义 `__linux__` / `__APPLE__`，转为项目内部宏
- 所有条件编译统一使用 `OS_LINUX` / `OS_MACOS`

### 5.2 Poller.h — 条件编译成员
```cpp
#ifdef OS_LINUX
    struct epoll_event *events_{nullptr};
#endif
#ifdef OS_MACOS
    struct kevent *events_{nullptr};
#endif
```
- 同一个 `Poller` 类，按平台持有不同的底层事件数组

### 5.3 Poller.cpp — 双实现
- **Linux 段**：`epoll_create1` / `epoll_ctl` / `epoll_wait`
- **macOS 段**：`kqueue` / `EV_SET + kevent` / `kevent(wait)`
- 两段代码结构完全对称，输出统一的 `Channel::READ_EVENT / WRITE_EVENT`

### 5.4 Channel — 自定义标志位
```cpp
const int Channel::READ_EVENT = 1;
const int Channel::WRITE_EVENT = 2;
const int Channel::ET = 4;
```
- 彻底消除 `Channel` 对 `<sys/epoll.h>` 的编译期依赖
- Poller 内部负责 READ_EVENT ↔ EPOLLIN / EVFILT_READ 的翻译

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day17/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day17/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 模块 | 职责 |
|------|------|
| **Macros.h** | 统一平台检测宏 `OS_LINUX` / `OS_MACOS` |
| **Poller** | 封装 epoll/kqueue，对外提供 `updateChannel` / `deleteChannel` / `poll` |
| **Channel** | 平台中立的事件标志；不再引用任何系统 IO 多路复用头文件 |
| **Eventloop** | 条件编译 eventfd/pipe 唤醒；通过 Poller 间接操作内核 |
| **Server / Connection** | 无变化，完全不感知底层 IO 多路复用差异 |

---

## 7. 局限

1. `Poller.cpp` 单文件两套实现，随着功能增加会变得臃肿
2. `OS_LINUX` / `OS_MACOS` 宏是自定义的，不如直接用 `__linux__` / `__APPLE__` 通用
3. kqueue 默认 Edge-Triggered，而 `Channel::ET` 标志在 kqueue 路径下被忽略（语义不一致）
4. 没有 FreeBSD/Windows 等其他平台的支持路径
