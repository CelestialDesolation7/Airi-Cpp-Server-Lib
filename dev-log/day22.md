# Day 22：ET 循环读 + Connection 状态守卫 + 析构防护

> Connection::doRead() 改为 ET 模式循环读（读至 EAGAIN），避免内核不再通知导致数据残留。
> Business() 增加状态守卫：kqueue/epoll 同批次多事件时，防止已 close 的 Connection 被重入。
> doWrite() 增加状态检查，关闭态不再写。

---

## 1. 引言

### 1.1 问题上下文

到 Day 21，one-loop-per-thread 完整跑通，但还隐藏着一个 ET 模式的经典 bug：`Connection::doRead()` 只读一次，如果客户端一次发了 2KB（超过单次 read 的临时 buf），第二次 epoll/kqueue 不会再通知（边缘触发只在状态变化时通知一次），剩余数据永远滞留在内核缓冲区。

同时，多事件并发场景有另一个 bug：kqueue/epoll 同一批次可能同时返回某 fd 的 READ + WRITE 事件，第一个事件触发 `close()` 后 Connection 状态变 `kClosed` 但实际 delete 是异步投递——第二个事件仍会调 `Business()`，访问已半死的 Connection 是 UAF 边缘行为。

### 1.2 动机

ET 循环读是边缘触发模式的硬性要求——任何使用 `EPOLLET` / `EV_CLEAR` 的代码不循环读到 `EAGAIN` 都是 bug。

状态守卫不是性能优化，是正确性补丁——多事件批处理 + 异步 delete 这两件事一组合，状态机就必须在每个回调入口防御。

这两个 fix 是 Day 22 唯一改动，但都是 Reactor 模式从"能跑"到"能上生产"必须修的债。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| LT + 单次 read | 教学代码 | 简单 | LT 反复触发，CPU 浪费 |
| ET + 循环 read 到 EAGAIN (本日) | muduo / 工业惯用 | 单次通知、CPU 友好 | 必须正确处理 EAGAIN |
| `recvmsg` + `MSG_DONTWAIT` | POSIX | 不修改 fd 标志 | 仍需循环 |
| io_uring SQE 批量提交 | Linux 5.1+ | 完成通知模型，无需循环 | 内核版本要求 |
| 状态守卫 + atomic state | 多事件批处理场景必备 | 防 UAF | 需在每个回调入口加 |

### 1.4 本日方案概述

本日实现：
1. `Connection::doRead()` 改为 ET 循环：`while (true) { n = inputBuffer_.readFd(...); if (n>0) continue; if (n==0) EOF break; if (EAGAIN) break; else error break; }`。
2. `Business()` 入口加状态守卫：`if (state_ != kConnected) return;`。
3. `doWrite()` 同样加状态检查：关闭态不再写。

代码量很小，但消灭的是 Reactor 模式最容易踩的两个生产事故。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `common/Connection.cpp` | **修改** | `doRead()` ET 循环读；`Business()` 状态守卫；`doWrite()` 状态检查 |

其余文件与 Day 21 相同（EventLoopThread/EventLoopThreadPool 架构不变）。

---

## 3. 核心改动详解

### 3.1 ET 循环读

```cpp
void Connection::doRead() {
    while (true) {
        ssize_t n = inputBuffer_.readFd(sockfd, &savedErrno);
        if (n > 0) continue;           // 继续读
        if (n == 0) { /* EOF */ break; }
        if (savedErrno == EAGAIN) break; // 读空，正常退出
        /* 其他错误 */ break;
    }
}
```

ET 模式下只触发一次通知，必须循环读至 `EAGAIN`，否则缓冲区残留数据客户端永远收不到回复。

### 3.2 Business() 状态守卫

kqueue/epoll 同一批次可能返回同一 fd 的多个事件（如 READ + WRITE）。第一个事件触发 `close()` 后 Connection 尚未被实际删除（异步投递到 `doPendingFunctors`），第二个事件仍会调 `Business()`。状态守卫在入口检查 `state_`，避免重入。

### 3.3 doWrite() 状态检查

```cpp
void Connection::doWrite() {
    if (state_ != State::kConnected) return;
    ...
}
```

---

## 4. 模块全景与所有权树（Day 22）

与 Day 21 相同：

```
main()
├── Signal::signal(SIGINT, [&]{ server.stop(); })
└── TcpServer server
    ├── unique_ptr<Eventloop> mainReactor_
    ├── unique_ptr<Acceptor> acceptor_
    ├── unique_ptr<EventLoopThreadPool> threadPool_
    │   └── vector<unique_ptr<EventLoopThread>> threads_
    └── unordered_map<int, unique_ptr<Connection>> connections_
```

---

## 5. 构建

```bash
cmake -S . -B build && cmake --build build -j4
```

生成 `server`、`client`、`ThreadPoolTest`、`StressTest` 四个可执行文件。


### 5.1 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day22/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day22/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

