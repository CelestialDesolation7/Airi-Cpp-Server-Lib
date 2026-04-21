# Day 16：异常处理、信号注册与 IO/业务分离

> 引入 `Exception.h` 自定义异常类，替代 `ErrIf` 直接 exit 的粗暴做法。
> 新增 `SignalHandler.h`，提供 `Signal::signal()` 注册信号处理函数，取代裸 `signal()` 调用。
> 新增 `pine.h` 伞形头文件，一行 include 全部核心模块。
> Connection 重构为 IO 与业务分离：`doRead`/`doWrite` 处理底层 IO，`Business()` 串联读 + 回调。

---

## 1. 引言

### 1.1 问题上下文

到 Day 15，错误处理仍然是"`ErrIf` 直接 `exit`"的粗暴方式。这在 demo 里能跑，但对长期运行的服务器是灾难——任何一次 `bind` 冲突、文件描述符耗尽、客户端异常断开，都会让整个进程被强杀，所有现有连接陪葬。

工业级 C++ 服务器的标准做法是抛 `std::system_error` / 自定义 `Exception`，让上层决定"重试 / 降级 / 终止"。同时，信号处理函数应该用统一的注册器（避免裸 `signal()`，因为它在不同平台行为不同），便于把多个信号处理函数挂到同一进程。

最后，连接的 IO 阶段（doRead/doWrite）和业务阶段（onMessage 回调）应该严格分层——之前两者揉在 `handleRead` 里，无法独立测试。

### 1.2 动机

异常机制让错误能跨函数边界传播，不再"一处出错全程退出"。统一信号注册让多模块（业务 + 异步日志 + 监控）可以协作处理同一个信号。IO 与业务分离让 Connection 的两层职责（"我读到了什么字节" vs "应用想做什么"）独立演进。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| `errno` + `perror` + `exit` | C 风格 / 早期 | 简单 | 全局退出、无法恢复 |
| 自定义 Exception 类 (本日) | C++ 惯用 | 可分类、可追溯 | 需 RAII 配套、性能开销 |
| `std::error_code` / `std::expected` | C++17 / C++23 | 零开销、显式 | 调用方语法重 |
| `signal()` 直接注册 | POSIX | 简单 | 跨平台行为不一致、不可重复注册 |
| `sigaction()` + 全局注册器 | POSIX 推荐 | 行为可控、可堆叠 | API 略复杂 |
| `signalfd` + 事件循环 | Linux | 信号变事件，无 async-signal-safe 限制 | Linux only |

### 1.4 本日方案概述

本日实现：
1. 新增 `Exception.h`：继承 `std::runtime_error`，带 `ExceptionType` 枚举。
2. 新增 `SignalHandler.h`：`Signal::signal(signo, handler)` + 全局 `handlers_` map，统一管理信号注册。
3. 新增 `pine.h` 伞形头文件：一行 include 所有核心模块。
4. `Connection` 重构 IO 与业务分离：`doRead` / `doWrite` 处理纯 IO；`Business()` 串联 doRead + onMessageCallback。
5. `Server` 新增 `onMessageCallback_` / `newConnectCallback_` 成员 + 对应 setter。
6. `server.cpp` 改用 `Signal::signal(SIGINT, ...)` + `server->newConnect()` + `server->onMessage()` 风格。

下一天会把跨平台机制（epoll/kqueue 的 `#ifdef` 分支）从散落改为集中。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Exception.h` | **新增** | `Exception` 类继承 `std::runtime_error`；`ExceptionType` 枚举 |
| `include/SignalHandler.h` | **新增** | `Signal` 结构体；静态 `signal()` 注册信号 + handler；全局 `handlers_` map |
| `include/pine.h` | **新增** | 伞形头文件，统一 include Buffer / Connection / EventLoop / Server / SignalHandler / Socket |
| `include/Connection.h` | **修改** | 新增 `doRead()`、`doWrite()`、`Business()`、`Read()`、`Write()`、`getSocket()`、`getInputBuffer()`、`getOutputBuffer()` |
| `common/Connection.cpp` | **重写** | IO（doRead/doWrite）与业务（Business → doRead + onMessageCallback_）分离；`setOnMessageCallback` 将 channel readCallback 绑到 Business |
| `include/Server.h` | **修改** | 新增 `onMessageCallback_`、`newConnectCallback_` 成员；新增 `onMessage()` 和 `newConnect()` setter |
| `common/Server.cpp` | **修改** | 使用 Exception 验证参数；`newConnection` 中设置 onMessage 和 newConnect 回调 |
| `server.cpp` | **修改** | 使用 `Signal::signal()` 注册 SIGINT；使用 `server->newConnect()` 和 `server->onMessage()` 回调 |

---

## 3. 模块全景与所有权树（Day 16）

```
main()
├── Signal::signal(SIGINT, handler)        ← 新增
├── Eventloop* loop
│   ├── Epoll* ep_
│   └── Channel* evtChannel_
└── Server* server
    ├── Acceptor* acceptor_
    ├── ThreadPool* threadPool_
    ├── vector<Eventloop*> subReactors_
    ├── onMessageCallback_                 ← 新增
    ├── newConnectCallback_                ← 新增
    └── map<int, Connection*> connections_
        └── Connection*
            ├── Channel* channel_
            │   └── readCallback → Business()   ← 改为 Business
            ├── Buffer inputBuffer_
            ├── Buffer outputBuffer_
            ├── onMessageCallback_
            └── doRead() / doWrite()            ← IO 分离
```

---

## 4. 全流程调用链

**场景 A：数据到达 → IO/业务分离**
```
kqueue/epoll 通知 POLLER_READ
  → Channel::handleEvent() → readCallback = Business
  → Connection::Business()
    → doRead()                         ← 仅读 socket → inputBuffer_
    → onMessageCallback_(this)          ← 业务逻辑回调
```

**场景 B：Server 设置双回调**
```
server->newConnect(lambda1)   → newConnectCallback_ = lambda1
server->onMessage(lambda2)    → onMessageCallback_ = lambda2

Server::newConnection(sock, addr)
  → conn = new Connection(subReactor, sock)
  → conn->setOnConnectCallback(newConnectCallback_)
  → conn->setOnMessageCallback(onMessageCallback_)
    → channel_->setReadCallback(bind(&Connection::Business, this))
```

**场景 C：Signal::signal 注册**
```
Signal::signal(SIGINT, handler)
  → handlers_[SIGINT] = handler
  → ::signal(SIGINT, Signal::signalHandler)   ← 安装 C 信号处理
    → Signal::signalHandler(signum)
      → handlers_[signum]()                   ← 分发到用户注册的 handler
```

**场景 D：Exception 异常处理**
```
Server::Server(mainReactor)
  → if (mainReactor == nullptr)
    → throw Exception(ExceptionType::kInvalidArg, "mainReactor is nullptr")
  → 上层 catch (Exception &e)
    → e.what()  → 错误信息
    → e.type()  → 异常类型枚举
```

---

## 5. 代码逐段解析

### 5.1 Exception.h — 自定义异常
```cpp
enum ExceptionType {
    kInvalidArg,
    kInvalidOperation,
    kSystemError,
};

class Exception : public std::runtime_error {
    ExceptionType type_;
public:
    Exception(ExceptionType type, const std::string &msg)
        : std::runtime_error(msg), type_(type) {}
    ExceptionType type() const { return type_; }
};
```
- 比 `ErrIf → perror → exit(1)` 更可控：可被 catch，可携带类型信息

### 5.2 SignalHandler.h — 信号注册
```cpp
struct Signal {
    static std::map<int, std::function<void()>> handlers_;

    static void signal(int sig, std::function<void()> handler) {
        handlers_[sig] = handler;
        ::signal(sig, signalHandler);
    }

    static void signalHandler(int sig) {
        handlers_[sig]();
    }
};
```
- 通过全局 map 将信号号映射到 `std::function`，支持 lambda
- 仅一层封装，底层仍用 POSIX `signal()`

### 5.3 Connection IO/业务分离
```cpp
void Connection::Business() {
    doRead();                     // 底层 IO
    onMessageCallback_(this);     // 业务回调
}

void Connection::doRead() {
    inputBuffer_.readFd(sock_->getFd());
}

void Connection::doWrite() {
    outputBuffer_.writeFd(sock_->getFd());
}
```
- `Business()` 是 Channel 的 readCallback
- 上层通过 `onMessage` 注入业务逻辑，无需关心 IO 细节

### 5.4 pine.h — 伞形头文件
```cpp
#include "Buffer.h"
#include "Connection.h"
#include "EventLoop.h"
#include "Server.h"
#include "SignalHandler.h"
#include "Socket.h"
```
- 用户只需 `#include "pine.h"` 即可使用全部核心组件

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day16/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day16/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 模块 | 职责 |
|------|------|
| **Exception.h** | 提供 ExceptionType 枚举 + Exception 类，替代 exit(1) 实现可恢复的错误处理 |
| **SignalHandler.h** | 封装 POSIX signal，支持 lambda/std::function 注册信号处理 |
| **pine.h** | 伞形头文件，简化使用方 include |
| **Connection** | IO 分离为 doRead/doWrite；Business 串联读 + 回调；对外提供 Read/Write/getSocket 等接口 |
| **Server** | 双回调模式：onMessage 处理业务，newConnect 处理连接事件 |

---

## 7. 局限

1. `SignalHandler` 使用全局 map + `::signal()`，非异步信号安全（handler 中不应做复杂操作）
2. `Business()` 中 `doRead` 失败（返回 -1）未检查，直接调 `onMessageCallback_`
3. `pine.h` 包含了所有头文件，可能引入不必要的编译依赖
4. Exception 类型枚举较少，未覆盖网络错误等场景
