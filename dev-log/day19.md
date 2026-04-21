# Day 19：Server → TcpServer 重命名 + 智能指针改造

> `Server` 重命名为 `TcpServer`，语义更精确。
> TcpServer 内部全面采用 `std::unique_ptr` 管理 Acceptor、ThreadPool、subReactors、connections。
> Acceptor / Connection 接口简化：回调参数从 `Socket*/InetAddress*` 改为 `int fd`。
> Connection 构造参数从 `(Eventloop*, Socket*)` 改为 `(int fd, Eventloop*)`，内部自建 Socket。
> 新增 `RC` 返回码枚举（Macros.h）。

---

## 1. 引言

### 1.1 问题上下文

到 Day 18，类名 `Server` 太泛——既可能指 TCP 服务器、HTTP 服务器，也可能被误以为是某个抽象 Server 接口。命名歧义在大型项目里会拖累所有 grep / autocomplete。

同时，所有资源仍然是裸指针 + 手动 `delete`：Acceptor、ThreadPool、subReactors、connections 都靠 Server 析构函数依次 delete。任何一处忘删/重复删都是 UB。C++11 后，`std::unique_ptr` 是默认的所有权表达——muduo / folly / boost 都已全面切换。

Connection 的构造接口也有问题：`Connection(EventLoop*, Socket*)` 让上层要先 `new Socket(fd)` 再传进来，多一次堆分配且 Socket 的所有权语义模糊。改成 `Connection(int fd, EventLoop*)` 让 Connection 内部 `make_unique<Socket>(fd)` 更干净。

### 1.2 动机

命名清晰、所有权表达清晰、构造参数清晰——这三件事让 API 自解释，新读者不需要看实现就能猜对用法。

`unique_ptr` 是零开销抽象（编译后代码与裸指针几乎相同），但能让"谁拥有谁"在类型层面写死，析构顺序由声明顺序自动保证。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 裸指针 + 手动 delete | C 风格 | 最低开销 | 易泄漏、易 double-free |
| `unique_ptr` (本日) | C++11 | 零开销 RAII、所有权显式 | 不能拷贝（这是优点） |
| `shared_ptr` 引用计数 | C++11 / muduo `TcpConnection` | 跨线程共享安全 | 引用计数原子操作开销 |
| GC（Java/Go） | 历史悠久 | 用户无感 | STW、内存翻倍 |
| Rust ownership + borrow checker | Rust | 编译期检查、零开销 | 学习曲线陡 |

### 1.4 本日方案概述

本日实现：
1. `Server` → `TcpServer` 全局重命名（类名、文件名、include 路径）。
2. `TcpServer` 内部所有成员改 `unique_ptr`：`unique_ptr<Eventloop> mainReactor_` / `unique_ptr<Acceptor> acceptor_` / `unique_ptr<ThreadPool> threadPool_` / `vector<unique_ptr<Eventloop>> subReactors_` / `map<int, unique_ptr<Connection>> connections_`。
3. `Acceptor::newConnectionCallback_` 签名从 `void(Socket*, InetAddress*)` 简化为 `void(int)`——只传 fd。
4. `Connection` 构造改 `(int fd, Eventloop*)`，内部 `make_unique<Socket>(fd)`。
5. `TcpServer::Start()` 显式启动方法（替代构造完成自动启动）。
6. `Macros.h` 新增 `enum RC` 返回码枚举（供后续模块使用）。
7. `Airi-Cpp-Server-Lib.h` 更新 include。

下一天解决 TcpServer 析构时的资源释放顺序与跨线程析构安全。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/TcpServer.h` | **新增** | 替代 `Server.h`；全 `unique_ptr` 成员；`Start()` 启动方法 |
| `common/TcpServer.cpp` | **新增** | 替代 `Server.cpp`；构造函数自建 mainReactor；`newConnection(int fd)` |
| `include/Server.h` | **删除** | 被 `TcpServer.h` 取代 |
| `common/Server.cpp` | **删除** | 被 `TcpServer.cpp` 取代 |
| `include/Acceptor.h` | **修改** | 成员改为 `unique_ptr<Socket>` / `unique_ptr<Channel>`；回调签名 `void(int)` |
| `common/Acceptor.cpp` | **修改** | 用 `std::make_unique` 创建资源；`acceptConnection` 只传 fd |
| `include/Connection.h` | **修改** | 构造 `(int fd, Eventloop*)`；成员 `unique_ptr<Socket>` / `unique_ptr<Channel>`；deleteCallback 改为 `void(int)` |
| `common/Connection.cpp` | **修改** | 构造函数内 `make_unique<Socket>(fd)` + `make_unique<Channel>` |
| `include/Macros.h` | **修改** | 新增 `enum RC` 返回码枚举 |
| `include/Airi-Cpp-Server-Lib.h` | **修改** | `Server.h` → `TcpServer.h` |
| `server.cpp` | **修改** | 使用 `TcpServer`；调用 `server->Start()` 启动 |

---

## 3. 模块全景与所有权树（Day 19）

```
main()
├── Signal::signal(SIGINT, handler)
└── TcpServer* server                              ← Server → TcpServer
    ├── unique_ptr<Eventloop> mainReactor_          ← 自建，非外部传入
    │   ├── Poller* (工厂创建)
    │   └── Channel* evtChannel_
    ├── unique_ptr<Acceptor> acceptor_              ← unique_ptr
    │   ├── unique_ptr<Socket> sock_
    │   └── unique_ptr<Channel> acceptChannel_
    ├── unique_ptr<ThreadPool> threadPool_           ← unique_ptr
    ├── vector<unique_ptr<Eventloop>> subReactors_   ← unique_ptr
    └── unordered_map<int, unique_ptr<Connection>> connections_  ← unique_ptr
        └── Connection
            ├── unique_ptr<Socket> sock_             ← 内部自建
            ├── unique_ptr<Channel> channel_         ← unique_ptr
            ├── Buffer inputBuffer_
            └── Buffer outputBuffer_
```

---

## 4. 全流程调用链

**场景 A：TcpServer 启动**
```
TcpServer::TcpServer()
  → mainReactor_ = make_unique<Eventloop>()
  → acceptor_ = make_unique<Acceptor>(mainReactor_.get())
  → acceptor_->setNewConnectionCallback(bind(&TcpServer::newConnection, fd))
  → threadPool_ = make_unique<ThreadPool>(N)
  → for i in 0..N: subReactors_.push_back(make_unique<Eventloop>())

TcpServer::Start()
  → for sub : subReactors_: threadPool_->add(bind(&Eventloop::loop, sub.get()))
  → mainReactor_->loop()   ← 阻塞
```

**场景 B：新连接（简化接口）**
```
Acceptor::acceptConnection()
  → clientFd = sock_->accept(&clientAddr)
  → fcntl(clientFd, O_NONBLOCK)
  → newConnectionCallback_(clientFd)         ← 只传 fd

TcpServer::newConnection(fd)
  → conn = make_unique<Connection>(fd, subReactors_[idx].get())
    → Connection 内部: sock_ = make_unique<Socket>(fd)
    → channel_ = make_unique<Channel>(loop, fd)
  → conn->setOnMessageCallback(onMessageCallback_)
  → conn->setDeleteConnectionCallback(bind(&TcpServer::deleteConnection, fd))
  → connections_[fd] = std::move(conn)
```

**场景 C：连接销毁（RAII）**
```
Connection::close()
  → deleteConnectionCallback_(sock_->getFd())   ← 传 fd 不传指针

TcpServer::deleteConnection(fd)
  → mainReactor_->queueInLoop([fd] {
      connections_.erase(fd);   ← unique_ptr 自动 delete Connection
    })
```

---

## 5. 代码逐段解析

### 5.1 TcpServer — 自包含服务器
```cpp
class TcpServer {
    std::unique_ptr<Eventloop> mainReactor_;       // 自建 Reactor
    std::unique_ptr<Acceptor> acceptor_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::vector<std::unique_ptr<Eventloop>> subReactors_;
    std::unique_ptr<ThreadPool> threadPool_;
public:
    TcpServer();           // 构造时完成所有初始化
    void Start();          // 启动 subReactor 线程 + mainReactor 循环
};
```
- 与 Day 18 的 `Server` 对比：不再需要外部传入 `Eventloop*`
- 所有资源用 `unique_ptr` 管理，析构 `= default` 即可

### 5.2 Acceptor — 简化回调
```cpp
// Day 18: void(Socket*, InetAddress*)  — 需要手动 delete
// Day 19: void(int fd)                 — 只传 fd，无所有权问题
std::function<void(int)> newConnectionCallback_;
```
- 减少了裸指针传递，降低内存泄漏风险

### 5.3 Connection — 内部构建 Socket
```cpp
Connection::Connection(int fd, Eventloop *loop)
    : sock_(std::make_unique<Socket>(fd)),
      channel_(std::make_unique<Channel>(loop, fd)) { ... }
```
- 外部只需知道 fd，Socket 的生命周期由 Connection 全权管理

### 5.4 RC 返回码枚举
```cpp
enum RC {
    RC_SUCCESS, RC_SOCKET_ERROR, RC_POLLER_ERROR, ...
};
```
- 为后续用返回码替代 `ErrIf(exit)` 做铺垫

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day19/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day19/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表

| 模块 | 职责 |
|------|------|
| **TcpServer** | 自包含的 TCP 服务器；自建 mainReactor、管理所有子系统生命周期 |
| **Acceptor** | 监听 + accept；回调只传 fd，不传所有权 |
| **Connection** | 构造时自建 Socket/Channel；RAII 析构自动清理 |
| **Macros.h** | 新增 RC 返回码枚举，为错误处理改造铺路 |
| **Airi-Cpp-Server-Lib.h** | 伞形头文件更新为 `TcpServer.h` |

---

## 7. 局限

1. `TcpServer` 硬编码绑定 `127.0.0.1:8888`（在 Acceptor 内部），不支持配置
2. `RC` 枚举已定义但尚未被任何函数使用
3. `deleteConnection` 通过 `queueInLoop` 投递到主线程，高并发下可能积压
4. `server.cpp` 中 `delete server` 在 `Signal::signal` 和 `main` 结尾各调一次，存在 double-free 风险
