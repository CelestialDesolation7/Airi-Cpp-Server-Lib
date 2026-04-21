# Day 20：TcpServer 安全关闭 + 成员析构顺序修正

> TcpServer 新增 `stop()` 方法，支持优雅关闭（SIGINT → stop → 各 Reactor 退出 loop）。
> 成员声明顺序调整，确保析构时 connections_ 先于 subReactors_ 销毁。
> `deleteConnection` 改用 `shared_ptr` guard 将 Connection 析构投递到归属子线程。
> `server.cpp` 改为栈对象 + RAII 风格，不再 `new`/`delete`。

---

## 1. 引言

### 1.1 问题上下文

到 Day 19，`unique_ptr` 接管了所有权，但析构次序仍可能出问题：如果 `subReactors_` 先于 `connections_` 析构，每个 Connection 析构时还想去自己所在 subReactor 的 `removeChannel`，但 subReactor 的 EventLoop 已经销毁——典型 use-after-free。

C++ 类成员的析构次序是声明的逆序，所以解决办法是**调整成员声明顺序**（subReactors 早于 connections 声明 → connections 先析构 → 此时 subReactor 还活着），并且在 TcpServer 加显式 `stop()` 让 sub-loop 退出 `loop()`，否则 ThreadPool join 时会永远阻塞在 `epoll_wait`。

跨线程析构也很微妙：在 mainReactor 线程触发 `deleteConnection(int fd)` 时，Connection 实际归属某个 subReactor 线程——直接 delete 会跨线程释放仍可能在被该 sub 用中的 Channel。muduo 的解法是 `shared_ptr<Connection> guard` + `runInLoop`：把 delete 任务派发到 Connection 自己的线程。

### 1.2 动机

优雅停机不是可选项——任何长期运行的服务都要支持热升级、优雅重启、零停机部署。本日把 TcpServer 的析构路径打磨到生产可用。

成员声明顺序、析构顺序、跨线程释放——这三件事是 C++ Reactor 的真正难点，本日全部串起来。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 析构里直接 `delete this` | 早期 | 简单 | 跨线程不安全 |
| 显式 stop() + 析构顺序对齐 (本日) | muduo | 控制权明确、可逐步 join | 用户必须记得调 stop |
| `shared_ptr<Connection>` + weak guard | muduo | 跨线程释放安全 | 引用计数开销 |
| RAII 析构链（声明顺序保证） | C++ 标准 | 零代码、自动 | 一旦顺序错就难调试 |
| Rust drop order + Send/Sync | Rust | 编译期检查 | 跨语言 |

### 1.4 本日方案概述

本日实现：
1. `TcpServer` 新增 `stop()`：遍历所有 subReactor 调 `setQuit()` + `wakeup()`；mainReactor 同样处理。
2. `TcpServer::~TcpServer()` 显式调 `stop()`，再让 `unique_ptr` 析构。
3. **成员声明顺序调整**：mainReactor → threadPool → subReactors → acceptor → connections（析构反序：connections 先死，subReactors 还在）。
4. `deleteConnection(int fd)` 改用 shared_ptr guard：在 mainReactor 中拿到 unique_ptr，转 shared_ptr，runInLoop 到归属 subReactor 析构。
5. `server.cpp` 改为栈对象 `TcpServer server`（替代 `new`/`delete`），SIGINT 调 `server.stop()`。

至此 Day 13 引入的多 Reactor 架构在生命周期上彻底安全，可以放心进入 Day 21 的 EventLoopThread 真线程模型。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/TcpServer.h` | **修改** | 新增 `stop()`；成员声明顺序调整（subReactors_ 在 connections_ 之前） |
| `common/TcpServer.cpp` | **修改** | 实现 `stop()`；析构函数调用 `stop()`；`deleteConnection` 用 shared_ptr guard |
| `server.cpp` | **修改** | 栈对象 `TcpServer server`；SIGINT 调 `stop()` 而非 `delete` |

---

## 3. 核心改动详解

### 3.1 `stop()` 与安全析构

```cpp
void TcpServer::stop() {
    for (auto &sub : subReactors_) {
        sub->setQuit();
        sub->wakeup();
    }
    mainReactor_->setQuit();
    mainReactor_->wakeup();
}

TcpServer::~TcpServer() { stop(); }
```

- `stop()` 令所有 sub-reactor 退出 `loop()`，线程从 `kevent`/`epoll_wait` 返回
- 析构函数显式调 `stop()`，确保 ThreadPool join 时线程不阻塞

### 3.2 成员声明顺序（析构逆序）

```
声明顺序（上 → 下）：
  subReactors_    → 后析构（EventLoop 最后销毁）
  connections_    → 先析构（Connection::~Connection 调 loop_->deleteChannel）
  threadPool_     → 最先析构（join 线程）
```

若 connections_ 在 subReactors_ 之后声明，析构时 loop_ 已是野指针 → 段错误。

### 3.3 shared_ptr guard 安全销毁 Connection

```cpp
void TcpServer::deleteConnection(int fd) {
    mainReactor_->queueInLoop([this, fd]() {
        ...
        std::shared_ptr<Connection> guard(std::move(conn));
        ioLoop->queueInLoop([guard]() { /* guard 析构 → Connection 释放 */ });
    });
}
```

- 用 `shared_ptr` 包装 `unique_ptr` 满足 `std::function` 可复制要求
- Connection 在归属子线程的 `doPendingFunctors()` 中被析构，当前 poll 事件已遍历完毕

---

## 4. 模块全景与所有权树（Day 20）

```
main()
├── Signal::signal(SIGINT, [&]{ server.stop(); })
└── TcpServer server                             ← 栈对象，RAII
    ├── unique_ptr<Eventloop> mainReactor_
    ├── unique_ptr<Acceptor> acceptor_
    ├── vector<unique_ptr<Eventloop>> subReactors_   ← 声明在 connections_ 前
    ├── unordered_map<int, unique_ptr<Connection>> connections_
    └── unique_ptr<ThreadPool> threadPool_
```

---

## 5. 构建

```bash
cmake -S . -B build && cmake --build build -j4
```

生成 `server`、`client`、`ThreadPoolTest`、`StressTest` 四个可执行文件。


### 5.1 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day20/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day20/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

