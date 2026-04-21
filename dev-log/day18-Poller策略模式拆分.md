# Day 18：Poller 策略模式拆分 + Airi-Cpp-Server-Lib 伞形头文件

> 将 Day 17 的单文件 `Poller.h/cpp`（`#ifdef` 分支）拆分为 OOP 策略模式：
> 抽象基类 `Poller`、Linux 子类 `EpollPoller`、macOS 子类 `KqueuePoller`、工厂 `DefaultPoller.cpp`。
> 新增 `Airi-Cpp-Server-Lib.h` 伞形头文件。
> `Macros.h` 移除自定义 `OS_LINUX/OS_MACOS`，改用编译器预定义 `__linux__` / `__APPLE__`。

---

## 1. 引言

### 1.1 问题上下文

Day 17 的 `Poller.cpp` 一个文件里同时维护两个平台的实现，虽然集中了，但 200+ 行的 `#ifdef` 分支让编辑器跳转、代码搜索、分平台 bug 修复都不直观。

策略模式（Strategy Pattern）是这种"同一接口、多种实现、运行时/编译时选择"场景的经典 OOP 解法：定义抽象基类 `Poller`，派生 `EpollPoller`、`KqueuePoller`，工厂 `newDefaultPoller()` 在编译期选择合适子类。muduo 的 `Poller` / `EPollPoller` / `PollPoller` 就是这套结构；JVM 的 NIO Selector 也是同形态。

同时，到了 Day 18，模块越来越多，让用户每次写 `#include "Channel.h" "Connection.h" "TcpServer.h" "Buffer.h" ...` 太繁琐——需要一个伞形头文件 `Airi-Cpp-Server-Lib.h` 一行包含所有公开 API。

### 1.2 动机

OOP 拆分让 Linux 的 EpollPoller 与 macOS 的 KqueuePoller 完全独立编译——平台特定文件用 `#ifdef` 整体守卫，不进入对方编译。新增平台（如 io_uring）只需要再写一个子类，工厂里加一个分支。

伞形头文件 `Airi-Cpp-Server-Lib.h` 是库的"公开 ABI"——用户只要 include 它就能用整个网络库，不需要了解内部目录结构。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 同文件 `#ifdef` (Day 17) | 集中跨平台 | 文件少 | 阅读体验差 |
| 抽象基类 + 子类 + 工厂 (本日) | Strategy / muduo | OOP 清晰、平台代码独立 | 一次虚函数调用开销 |
| Concept-based polymorphism (C++20 concepts) | C++20 | 零虚函数开销 | 模板膨胀、错误信息差 |
| CRTP 静态多态 | C++ 模板惯用 | 零开销 | 类型推导复杂 |
| 伞形头文件 (本日) | C/C++ 库惯用（如 `<boost/asio.hpp>`） | 用户体验好 | 编译时间略增 |
| Modules (C++20) | C++20 | 编译加速、严格隔离 | 工具链支持参差 |

### 1.4 本日方案概述

本日实现：
1. 新建 `include/Poller/Poller.h`：抽象基类，纯虚 `updateChannel` / `deleteChannel` / `poll`；静态工厂 `newDefaultPoller(EventLoop*)`。
2. 新建 `include/Poller/EpollPoller.h`（`#ifdef __linux__` 守卫）+ `include/Poller/KqueuePoller.h`（`#ifdef __APPLE__` 守卫），分别继承 Poller。
3. 新建 `common/Poller/DefaultPoller.cpp` 工厂：`__linux__ → new EpollPoller`、`__APPLE__ → new KqueuePoller`。
4. 新建 `common/Poller/epoll/EpollPoller.cpp`、`common/Poller/kqueue/KqueuePoller.cpp` 各自完整实现。
5. 删除旧 `Poller.h/cpp`。
6. 新建 `include/Airi-Cpp-Server-Lib.h` 伞形头文件。
7. `Macros.h` 移除 `OS_LINUX/OS_MACOS`，全项目改用编译器原生 `__linux__` / `__APPLE__`。

至此跨平台 IO 多路复用结构稳定，可以专注上层抽象。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Poller/Poller.h` | **新增** | 抽象基类，纯虚函数 `updateChannel` / `deleteChannel` / `poll`；静态工厂 `newDefaultPoller` |
| `include/Poller/EpollPoller.h` | **新增** | `#ifdef __linux__` guard；继承 Poller，持有 `epollFd_` + `epoll_event*` |
| `include/Poller/KqueuePoller.h` | **新增** | `#ifdef __APPLE__` guard；继承 Poller，持有 `kqueueFd_` + `kevent*` |
| `common/Poller/DefaultPoller.cpp` | **新增** | 工厂实现：`__linux__` → `new EpollPoller`，`__APPLE__` → `new KqueuePoller` |
| `common/Poller/epoll/EpollPoller.cpp` | **新增** | `#ifdef __linux__` guard；epoll 完整实现 |
| `common/Poller/kqueue/KqueuePoller.cpp` | **新增** | `#ifdef __APPLE__` guard；kqueue 完整实现 |
| `include/Poller.h` | **删除** | 被 `Poller/Poller.h` 取代 |
| `common/Poller.cpp` | **删除** | 被三个 Poller/*.cpp 取代 |
| `include/Airi-Cpp-Server-Lib.h` | **新增** | 伞形头文件，统一 include 全部核心模块 |
| `include/Macros.h` | **修改** | 移除 `OS_LINUX/OS_MACOS`，全项目改用 `__linux__` / `__APPLE__` |
| `include/EventLoop.h` | **修改** | `#ifdef` 改为 `__linux__` / `__APPLE__` |
| `common/Eventloop.cpp` | **修改** | 改用 `Poller::newDefaultPoller(this)` 工厂创建 |

---

## 3. 模块全景与所有权树（Day 18）

```
main()
├── Signal::signal(SIGINT, handler)
├── Eventloop* loop
│   ├── Poller* poller_  ← newDefaultPoller(this) 工厂
│   │   ├── [Linux] EpollPoller : Poller
│   │   │   ├── epollFd_
│   │   │   └── epoll_event* events_
│   │   └── [macOS] KqueuePoller : Poller
│   │       ├── kqueueFd_
│   │       └── kevent* events_
│   └── Channel* evtChannel_
└── Server* server
    ├── Acceptor* acceptor_
    ├── ThreadPool* threadPool_
    ├── vector<Eventloop*> subReactors_
    └── map<int, Connection*> connections_
```

---

## 4. 全流程调用链

**场景 A：Poller 工厂创建**
```
Eventloop::Eventloop()
  → poller_ = Poller::newDefaultPoller(this)
    → DefaultPoller.cpp
      ├── [__linux__] return new EpollPoller(loop)
      └── [__APPLE__] return new KqueuePoller(loop)
```

**场景 B：KqueuePoller 事件循环**
```
KqueuePoller::updateChannel(channel)
  → EV_SET(EVFILT_READ, EV_ADD|EV_ENABLE, udata=channel)
  → kevent(kqueueFd_, ev, n, nullptr, 0, nullptr)

KqueuePoller::poll(timeout)
  → kevent(kqueueFd_, nullptr, 0, events_, MAX_EVENTS, &ts)
  → 遍历 events_[i].udata → Channel*
  → EVFILT_READ → Channel::READ_EVENT
  → 返回 activeChannels
```

**场景 C：EpollPoller 事件循环（Linux）**
```
EpollPoller::updateChannel(channel)
  → epoll_ctl(EPOLL_CTL_ADD/MOD, ev)

EpollPoller::poll(timeout)
  → epoll_wait → events_[i].data.ptr → Channel*
  → EPOLLIN → Channel::READ_EVENT
```

---

## 5. 代码逐段解析

### 5.1 Poller 抽象基类
```cpp
class Poller {
protected:
    Eventloop *ownerLoop_;
public:
    explicit Poller(Eventloop *loop) : ownerLoop_(loop) {}
    virtual ~Poller() = default;
    virtual void updateChannel(Channel *channel) = 0;
    virtual void deleteChannel(Channel *channel) = 0;
    virtual std::vector<Channel *> poll(int timeout = -1) = 0;
    static Poller *newDefaultPoller(Eventloop *loop);
};
```
- 纯虚接口，子类实现平台特定逻辑
- 静态工厂方法避免 Eventloop 直接依赖具体 Poller 子类

### 5.2 DefaultPoller.cpp — 工厂方法
```cpp
Poller *Poller::newDefaultPoller(Eventloop *loop) {
#ifdef __linux__
    return new EpollPoller(loop);
#else
    return new KqueuePoller(loop);
#endif
}
```
- 编译期选择，零运行时开销
- 工厂方法放在独立 .cpp 中，避免基类头文件引入子类头文件

### 5.3 EpollPoller / KqueuePoller 对称结构
- 两者均 override `updateChannel` / `deleteChannel` / `poll`
- 各自用 `#ifdef __linux__` / `#ifdef __APPLE__` guard 整个文件
- 在非目标平台上编译为空翻译单元，不产生符号冲突

### 5.4 Airi-Cpp-Server-Lib.h — 伞形头文件
```cpp
#include "Buffer.h"
#include "Connection.h"
#include "EventLoop.h"
#include "Server.h"
#include "SignalHandler.h"
#include "Socket.h"
```
- 用户只需 `#include "Airi-Cpp-Server-Lib.h"` 即可使用全部核心 API

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day18/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day18/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 模块 | 职责 |
|------|------|
| **Poller（基类）** | 定义 IO 多路复用抽象接口 + 静态工厂 |
| **EpollPoller** | Linux epoll 实现 |
| **KqueuePoller** | macOS/BSD kqueue 实现 |
| **DefaultPoller.cpp** | 编译期工厂，按平台返回具体 Poller |
| **Eventloop** | 通过工厂获取 Poller，不感知具体实现 |
| **Airi-Cpp-Server-Lib.h** | 伞形头文件，简化外部使用 |

---

## 7. 局限

1. 工厂方法用 `new` 返回裸指针，Eventloop 负责 `delete`，未使用 `unique_ptr`
2. EpollPoller.cpp 在 macOS 上编译为空翻译单元，虽无害但增加构建时间
3. Poller 子目录结构较深（`common/Poller/epoll/`、`common/Poller/kqueue/`），可考虑扁平化
4. `Airi-Cpp-Server-Lib.h` 与旧的 `pine.h` 并存，功能重复
