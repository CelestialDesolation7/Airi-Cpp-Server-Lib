# Day 15：连接状态机 — Connection::State 枚举

> 为 Connection 引入 `State` 枚举 `{kInvalid, kConnected, kClosed, kFailed}`，实现连接生命周期状态跟踪。
> 新增 `close()` 方法和 `onConnectCallback_`，当连接关闭/出错时自动转移状态并通知 Server。
> server.cpp 使用 `server->onConnect()` 设置回调，根据 `conn->getState()` 判断连接是否存活。

---

## 1. 引言

### 1.1 问题上下文

到 Day 14，跨线程派发就绪，但 Connection 缺少**显式生命周期状态**——上层无法判断"这个 Connection 是已建立、已关闭、还是出错"。这导致两类 bug：(a) Server 的 onMessage 回调拿到一个已经 close 的 Connection，访问 channel/buffer 出错；(b) Connection 自己的 handleRead 在 EOF / 出错时直接 `delete this`，但 Server 仍持有指针——典型悬挂指针。

muduo 的解法是 `TcpConnection::StateE` 枚举（kConnecting / kConnected / kDisconnecting / kDisconnected）+ `connectionCallback`：状态转移由 Connection 内部驱动，状态变更通过回调上报给 Server，Server 在回调里做集中清理。

### 1.2 动机

显式状态机让"Connection 当前能做什么"成为代码契约：业务回调可以先 `if (conn->getState() != kConnected) return;` 防御。

回调上报取代直接 `delete this`，让"删除时机"集中在 Server 里——关键的"先从 map erase 再 delete"操作不再分散。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 无状态字段 / `bool closed_` | 早期 | 简单 | 无法表达 connecting/disconnecting 等过渡状态 |
| State 枚举 + onConnectCallback (本日) | muduo `TcpConnection` | 状态显式、回调集中清理 | 仍需小心多线程修改 state |
| `enum class State` + `atomic<State>` | 现代 C++ | 类型安全、跨线程读安全 | 状态机迁移代码增多 |
| Erlang gen_server 行为机 | Erlang/OTP | 状态机框架化 | 跨语言 |
| Rust state-as-type pattern (typestate) | Rust | 编译期强制状态合法 | 模板/泛型重 |

### 1.4 本日方案概述

本日实现：
1. `Connection` 新增 `enum State { kInvalid, kConnected, kClosed, kFailed }` + `state_` 成员 + `getState()`。
2. 构造函数末尾 `state_ = kConnected`。
3. `Connection::handleRead()`：read=0 → state=kClosed；read<0 且非 EAGAIN → state=kFailed；两者都触发 `onConnectCallback_(this)`。
4. 新增 `Connection::close()` 方法，供上层主动关闭。
5. `Server` 新增 `onConnectCallback_` 成员 + `onConnect(cb)` setter；构造时给每个 Connection 设。
6. `server.cpp` 在回调里：`if (conn->getState() == kClosed) deleteConnection(conn);`。

后续 HTTP 层的 keep-alive、空闲连接关闭都依赖这套状态机。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Connection.h` | **修改** | 新增 `State` 枚举（kInvalid / kConnected / kClosed / kFailed）；新增 `state_`、`onConnectCallback_`、`close()` |
| `common/Connection.cpp` | **修改** | 构造函数设 `state_ = kConnected`；`handleRead` 失败时设 `kClosed`/`kFailed` 并调 `onConnectCallback_` |
| `include/Server.h` | **修改** | 新增 `onConnectCallback_` 成员；新增 `onConnect()` setter |
| `common/Server.cpp` | **修改** | `newConnection` 中为 Connection 设置 `onConnectCallback_` |
| `server.cpp` | **修改** | 使用 `server->onConnect(lambda)` 回调，检查 `conn->getState()` |
| 其余文件 | 不变 | Channel / Epoll / EventLoop / Buffer / Acceptor 沿用 Day 14 |

---

## 3. 模块全景与所有权树（Day 15）

```
main()
├── signal(SIGINT, signalHandler)
├── Eventloop* loop
│   ├── Epoll* ep_
│   └── Channel* evtChannel_
└── Server* server
    ├── Acceptor* acceptor_
    ├── ThreadPool* threadPool_
    ├── vector<Eventloop*> subReactors_
    ├── onConnectCallback_                 ← 新增
    └── map<int, Connection*> connections_
        └── Connection*
            ├── State state_               ← 新增
            │   kInvalid → kConnected → kClosed / kFailed
            ├── Channel* channel_
            ├── Buffer inputBuffer_
            ├── onConnectCallback_         ← 新增
            └── onMessageCallback_
```

---

## 4. 全流程调用链

**场景 A：客户端正常关闭**
```
client close(fd)
  → kqueue/epoll 通知 POLLER_READ 就绪
  → Channel::handleEvent() → readCallback
  → Connection::handleRead()
    → read() 返回 0 (EOF)
    → state_ = kClosed
    → onConnectCallback_(this)           ← 通知 Server
  → Server 回调中检查 conn->getState() == kClosed
    → deleteConnection(sock)
```

**场景 B：客户端异常断开**
```
client crash / network error
  → read() 返回 -1, errno == ECONNRESET
  → state_ = kFailed
  → onConnectCallback_(this)
```

**场景 C：onConnect 回调注册**
```
server->onConnect(lambda)
  → Server::onConnectCallback_ = lambda
  → Server::newConnection(sock, addr)
    → conn = new Connection(subReactor, sock)
    → conn->setOnConnectCallback(onConnectCallback_)
    → 连接状态变化时 → callback 被触发
```

---

## 5. 代码逐段解析

### 5.1 Connection::State 枚举
```cpp
enum State {
    kInvalid = 0,     // 初始无效
    kConnected = 1,   // 已连接
    kClosed = 2,      // 对端关闭
    kFailed = 3,      // 出错
};
```
- 状态只向前转移，不可回退
- `getState()` 返回当前状态，外部据此决定是否处理/清理

### 5.2 handleRead 中的状态迁移
```cpp
void Connection::handleRead() {
    int n = inputBuffer_.readFd(sock_->getFd());
    if (n > 0) {
        onMessageCallback_(this);  // 业务处理
    } else if (n == 0) {
        state_ = kClosed;          // EOF → 关闭
        onConnectCallback_(this);
    } else {
        state_ = kFailed;          // 出错
        onConnectCallback_(this);
    }
}
```

### 5.3 server.cpp 中的回调使用
```cpp
server->onConnect([](Connection *conn) {
    if (conn->getState() == Connection::State::kClosed) {
        // 对端关闭 → 可以打印日志或清理
        conn->close();
    }
});
```

---

### 5.4 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day15/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day15/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 模块 | 职责 |
|------|------|
| **Connection** | 新增 State 枚举，在 handleRead 中根据 read 返回值转移状态，并通过 onConnectCallback_ 通知 |
| **Server** | 持有 onConnectCallback_，在 newConnection 时注入给每个 Connection |
| **server.cpp** | 设置 onConnect lambda，实现连接关闭/出错时的业务逻辑 |

---

## 7. 局限

1. `onConnectCallback_` 命名不够直观，实际含义是"连接状态变化回调"而非"连接建立回调"
2. State 枚举缺少 `kDisconnecting` 半关闭状态，无法精确处理 shutdown(WR) 场景
3. 无心跳机制，无法检测静默断开
