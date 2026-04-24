# Day 20：Connection 生命周期安全化（懒注册 + 析构注销 + Business 状态分流）

> **主题**：把 Connection 从「构造即上线」改成「构造 → 装配回调 → 入 map → 跨线程启用」，并把销毁路径改为 mainReactor 转移所有权 + subReactor 析构。
> **基于**：Day 19（TcpServer 重命名 + `unique_ptr` 全面接管）

---

## 1. 引言

### 1.1 问题上下文

到 Day 19，`TcpServer` 已经用 `unique_ptr` 接管所有资源、`Acceptor` 只把 `int fd` 透出，`Connection` 自建 `Socket` / `Channel`。看起来"接口干净 + RAII"全齐了，但实际跑起来仍然有两类隐藏 bug，只在高并发或客户端突然断连时复现：

第一类是 **「事件先于回调」竞态**。Day 19 的 `Connection` 构造函数里直接 `channel_->enableReading() / enableET()`，意思是"构造一完成立刻向 Poller 注册可读 + 边缘触发"。问题是：构造完成 → 注册 Channel → 上层 `TcpServer::newConnection` 还来不及调 `setOnMessageCallback` / `setDeleteConnectionCallback`，Poller 这一头如果立刻有数据到达就会触发 `Channel::handleEvent` → `Connection::doRead` → `Business`，此时 `onMessageCallback_` 还是空的，`deleteConnectionCallback_` 也是空的——业务回调被吞，断连被忽略，Connection 永远留在 `connections_` 里。

第二类是 **「析构野指针」**：Day 19 的 `Connection::~Connection()` 是空的，没有从 Poller 注销 Channel。下一次 `kevent` / `epoll_wait` 返回的 `event.udata` 还指向已经析构的 Channel，立刻发生 use-after-free。`unique_ptr` 把析构时机管好了，但析构时该做什么仍然要手写。

第三类是 **「读到一半 close 自己」**：Day 19 的 `doRead()` 检测到 `n == 0` 直接调 `close()`；`close()` 触发 `deleteConnectionCallback_(fd)` → mainReactor `queueInLoop` 立刻删 Connection。结果：`doRead()` 还没返回，`Business()` 还没执行 `onMessageCallback_(this)`，`this` 已经被释放——典型的"调函数把自己删了"。

### 1.2 动机

Reactor 模式的"魔鬼细节"几乎全部集中在 Connection 的生命周期边界上：注册时机、注销时机、跨线程归属、回调链中的 self-delete。这一天不引入新功能，专门把这三类隐藏 bug 一次修干净。修完后，stress test 跑几万条短连接才不会偶尔段错误。

业内做这件事的范本是 muduo 的 `TcpConnection`：注册延后到 "连接已建立 + 回调已就绪" 之后，析构主动 `loop_->removeChannel`，所有跨线程销毁通过 `runInLoop` / `queueInLoop` 投递。本日把这三件事一次性补齐，也为 Day 21 加 `stop()` / 安全停机、Day 22 真正引入 EventLoopThread 做铺垫。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 构造即注册 + 析构什么也不做 | Day 19 / 教学代码 | 写起来短 | 事件先于回调；析构后 Poller 留野指针 |
| 构造延迟注册 + 析构主动注销（本日） | muduo `TcpConnection::connectEstablished` | 注册时回调齐备；Poller 状态干净 | 多一次 `queueInLoop` 跨线程跳转 |
| 构造时即注册 + close-only-via-state-machine | Boost.Asio | 状态机统一管理 | 状态枚举更复杂 |
| Erlang / Akka actor mailbox | BEAM / Akka | 永远只在自己 actor 处理消息 | 需要语言/框架运行时 |
| Tokio `TcpStream` + drop guard | Rust | 编译期保证生命周期 | 跨语言 |

### 1.4 本日方案概述

本日实现：

1. `Connection::~Connection()` 主动 `loop_->deleteChannel(channel_.get())`：从 Poller 注销，消除野指针。
2. `Connection` 构造函数不再 `enableReading() / enableET()`，新增 `enableInLoop()` 由 `TcpServer` 在 "回调注册完毕 + 已入 map" 之后通过 `queueInLoop` 在归属 sub-reactor 线程触发。
3. `Connection::doRead() / doWrite()` 不再就地 `close()`，只设 `state_`。最终的 `close()` 调用集中到 `Business()`：连接活则跑业务回调，连接死则在所有访问 `this` 的代码都返回后单点 `close()`。
4. `Connection::getLoop()` 暴露归属 EventLoop 指针，供 `TcpServer::deleteConnection` 决定把析构投递到哪个线程。
5. `TcpServer::deleteConnection` 改为 mainReactor 锁住 map / 移走所有权 → 用 `release()` + `queueInLoop` 把 raw `delete` 投递到归属 sub-reactor 线程。
6. `Eventloop` 新增 `deleteChannel(Channel*)` 转发到底层 Poller。

本日不做的是：还没把 `subReactors_` / `ThreadPool` 抽象成 `EventLoopThreadPool`（留到 Day 22），也还没引入 `tid_` / `isInLoopThread()`（同样 Day 22）。所以"跨线程"投递在本日只是一种"逻辑约定"，不是"代码自检"。

---

## 2. 模块全景与所有权树

```
TcpServer  ── 业务入口，栈对象由 server.cpp main 持有
├── unique_ptr<Eventloop>       mainReactor_       // 主线程 reactor
├── unique_ptr<Acceptor>        acceptor_          // 监听 + accept
├── vector<unique_ptr<Eventloop>> subReactors_     // 子线程 reactor 池
├── unordered_map<int, unique_ptr<Connection>> connections_   // 全部活动连接
└── unique_ptr<ThreadPool>      threadPool_         // 跑各 subReactor::loop()

Connection
├── unique_ptr<Socket>   sock_       // RAII 关闭 fd
├── unique_ptr<Channel>  channel_    // 析构时由 Connection 主动从 Poller 注销
├── Buffer               inputBuffer_
├── Buffer               outputBuffer_
├── State                state_       // kConnected / kDisconnecting / kDisconnected
└── std::function...     onMessageCallback_ / deleteConnectionCallback_
```

**所有权规则**

- `Connection` 的所有权由 `TcpServer::connections_` 的 `unique_ptr` 唯一持有；销毁时把所有权"移"到一个 lambda 里跨线程投递，析构在归属 sub-reactor 的 `doPendingFunctors()` 中发生。
- `Channel` 由 `Connection` 拥有，但**注册到 Poller 的指针副本**由 Connection 在析构时负责注销，否则 Poller 持有野指针。
- `loop_` 在 Connection 中是观察者裸指针；保证 `loop_` 比 Connection 活得久——由 `TcpServer.h` 的成员声明顺序保证（`subReactors_` 在 `connections_` 后声明，先析构后者）。

---

## 3. 初始化顺序（构造阶段）

```
[main 线程, server.cpp]
①  TcpServer server;                                 // 栈对象
    │
    ├─ mainReactor_ = make_unique<Eventloop>()       // mainReactor 就绪（Poller 通过工厂创建）
    ├─ acceptor_    = make_unique<Acceptor>(mainReactor_.get())
    │     └─ Acceptor 内部 socket → bind → listen → register READ on mainReactor
    ├─ acceptor_->setNewConnectionCallback(TcpServer::newConnection)
    ├─ threadNum = std::thread::hardware_concurrency()
    ├─ threadPool_ = make_unique<ThreadPool>(threadNum)
    └─ for i in 0..threadNum: subReactors_.emplace_back(make_unique<Eventloop>())
                              // 注意：所有 subReactor 在主线程构造，tid_ 暂未使用

②  server.newConnect(...);  server.onMessage(...);   // 注入业务回调
③  server.Start();
    │
    ├─ for sub : subReactors_: threadPool_->add([=]{ sub->loop(); })   // 子线程开始 loop
    └─ mainReactor_->loop();                                            // 主线程进入 loop
```

构造阶段不向任何 Connection 注册事件——本日的关键改动就是把"注册"从 Connection 构造函数里移走。

---

## 4. 全流程调用链

### 4.1 场景 A：新连接建立（懒注册的核心路径）

```
① [main 线程] mainReactor_ 的 poll() 返回 → Acceptor::handleRead()
② [main 线程] Acceptor::acceptConnection() 调 ::accept → fcntl O_NONBLOCK
                → newConnectionCallback_(clientFd)
③ [main 线程] TcpServer::newConnection(int fd)
                ├─ idx = fd % subReactors_.size()
                ├─ conn = make_unique<Connection>(fd, subReactors_[idx].get())
                │       └─ Connection 构造：sock_ / channel_ / Buffer 准备就绪
                │          但 channel_ 尚未 enableReading()，Poller 看不到此 fd
                ├─ conn->setOnMessageCallback(...)              ★ 回调注入
                ├─ conn->setDeleteConnectionCallback(...)       ★ 回调注入
                ├─ rawConn = conn.get()
                ├─ connections_[fd] = std::move(conn)           ★ 入 map
                ├─ newConnectCallback_(rawConn)                  // 业务"on connect"
                └─ subReactors_[idx]->queueInLoop(
                       [rawConn]{ rawConn->enableInLoop(); }     ★ 跨线程注册
                   );
④ [sub-reactor 线程] doPendingFunctors() 执行 lambda
                → Connection::enableInLoop()
                → channel_->enableReading() + enableET()         ★ 此刻 Poller 才看到 fd
```

关键时序：第 ④ 步真正向 kqueue/epoll 注册 fd 的时候，`onMessageCallback_` 与 `deleteConnectionCallback_` 都已就绪、`connections_[fd]` 已经能找到此连接——根本不可能再触发"事件先于回调"。

### 4.2 场景 B：客户端断开连接（自删 self-delete 的拆解）

```
① [sub-reactor 线程] poll 返回，Channel::handleEvent → Connection::doRead()
② [sub-reactor 线程] readv 返回 0 (EOF)
                ├─ 不 close()，只置 state_ = kDisconnecting        ★ 关键：不立即 self-delete
                └─ 函数返回
③ [sub-reactor 线程] Channel::handleEvent 继续把控制权交给 Connection::Business()
                ├─ if (state_ == kConnected)
                │     onMessageCallback_(this);            // 正常路径
                └─ else
                      close();                              ★ 此处才唯一调用 close
                      └─ deleteConnectionCallback_(fd)
                          └─ TcpServer::deleteConnection(fd)
④ [main 线程, queueInLoop 异步执行]
   TcpServer::deleteConnection lambda
   ├─ it = connections_.find(fd)
   ├─ ioLoop = it->second->getLoop()
   ├─ unique_ptr<Connection> conn = std::move(it->second)
   ├─ connections_.erase(it)
   └─ Connection* raw = conn.release();
      ioLoop->queueInLoop([raw]{ delete raw; });            ★ 投递到归属 sub-reactor 析构
⑤ [sub-reactor 线程, doPendingFunctors] delete raw
                → Connection::~Connection()
                → loop_->deleteChannel(channel_.get())      ★ 主动从 Poller 注销
                → unique_ptr 链反向回收 sock_ / channel_
```

为什么不在第 ② 步 `close()`：那时 Channel::handleEvent 的栈帧里还会接着访问 `this`（如调 `Business()` → `onMessageCallback_(this)` → 业务里读 `conn->getXxx()`）。提前 self-delete 等于在自己脚下抽地板。

为什么不在第 ④ 步直接 `delete conn` 而要再投递一次：第 ④ 步在 main 线程，但 Channel 的 udata 仍可能在 sub-reactor 当前 `poll()` 的 `events_` 数组里。再 `queueInLoop` 一次保证析构发生在 sub-reactor 完成本轮 IO 处理之后。

### 4.3 场景 C：进程关闭（信号触发）

```
① [信号处理函数] SIGINT → handler
② [信号处理函数] 暂未实现 stop()（留到 Day 21），目前 Day 20 的 server.cpp 仍是
                  delete server; exit(0);  这里有 Day 19 遗留的 double-free 风险，
                  Day 21 会用 stop() + atomic_flag 修掉。
③ [析构序列, 由 Day 21 的 stop() 才能完整跑通]
   ~TcpServer()
   ├─ connections_  反向析构 → 每个 Connection 析构 → loop_->deleteChannel()
   │                          因 subReactors_ 仍存活，安全
   ├─ subReactors_  析构 → ~Eventloop()
   ├─ threadPool_   析构 → join 所有 worker（前提是 loop 已退出）
   ├─ acceptor_     析构 → ~Acceptor()
   └─ mainReactor_  析构
```

本日只把"析构时该做什么"修对——"何时优雅触发析构"是 Day 21 的题目。

---

## 5. 析构顺序与生命周期安全分析

`TcpServer` 在本日仍按 Day 19 的成员声明顺序，析构顺序（声明逆序）：

| 析构顺序 | 成员 | 此时其它成员状态 | 安全性 |
|----|----|----|----|
| 1 | `threadPool_` | 所有 reactor `loop()` 仍可能阻塞在 `poll()` | **不安全**：本日仍可能死锁，留给 Day 21 的 `stop()` 解决 |
| 2 | `connections_` | `subReactors_` 仍存活 | 安全：`Connection::~` 调 `loop_->deleteChannel` 时 EventLoop 仍在 |
| 3 | `subReactors_` | Channel 已全部注销 | 安全：Poller 中无任何 Connection 的 Channel |
| 4 | `acceptor_` | mainReactor_ 仍存活 | 安全：Acceptor 的 Channel 也能从 mainReactor 注销 |
| 5 | `mainReactor_` | — | 安全 |

`Connection` 自身的析构顺序（成员声明逆序）：

| 析构顺序 | 成员 | 安全性 |
|----|----|----|
| 1 | `outputBuffer_` / `inputBuffer_` | 纯数据，无外部句柄 |
| 2 | `channel_` (`unique_ptr`) | 析构前已被 `Connection::~` 主动 `loop_->deleteChannel` 注销，Poller 无悬空指针 |
| 3 | `sock_` (`unique_ptr`) | `~Socket()` 调 `::close(fd)`，此时 Channel 已注销，无人再触发该 fd 事件 |

注意：`sock_` 必须在 `channel_` 之后析构（即先关 fd，后销毁数据成员）才安全的说法不对——实际上**反过来**才安全：先注销 Channel（让 Poller 不再回调），再关 fd。本日的成员声明顺序天然满足这一点：`sock_` 在 `channel_` 之前声明 → `channel_` 先析构 → 在 `Connection::~` 里此前已经显式 `deleteChannel` → 然后 `sock_` 析构关 fd。

---

## 6. 文件清单与变更总览

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `include/EventLoop.h` | 修改 | 新增 `void deleteChannel(Channel*)` 接口 |
| `common/Eventloop.cpp` | 修改 | 实现 `deleteChannel`，转发到 `poller_->deleteChannel` |
| `include/Connection.h` | 修改 | 新增 `getLoop()` / `enableInLoop()`；构造函数不再就地 enable |
| `common/Connection.cpp` | 修改 | 构造函数移除 `enableReading/enableET`；析构主动 `deleteChannel`；`doRead/doWrite` 去掉 inline `close()`；`Business()` 按 state 分流 |
| `common/TcpServer.cpp` | 修改 | `newConnection` 增加 `queueInLoop(enableInLoop)` 注册；`deleteConnection` 改为转移所有权 + `release` + 投递到归属 sub-reactor `delete` |

净行数变化非常小（约 +60 / −20），但行为变化巨大。

---

## 7. 测试与验证

本日没有新增 GTest 单元测试（GTest 迁移在 Day 30），仍沿用 Day 11 的 `StressTest`。

### 7.1 复现 Day 19 的崩溃

构造一个"短连接洪水"客户端：每次 `connect → send 16B → close`，并发 200 路、5 万次。在 Day 19 的代码下，约 1/3 概率：

```
[TcpServer] new connection fd=42
==4321==ERROR: AddressSanitizer: heap-use-after-free on address ...
  READ of size 8 at ... thread T3
    #0 KqueuePoller::poll  ...
    #1 Eventloop::loop     ...
    #2 std::thread::_Invoker  ...
```

ASan 指向的就是已经被析构的 Channel 在 `events_[i].udata` 里被解引用。

### 7.2 修后的行为

```bash
cmake -S . -B build && cmake --build build -j4
./build/server &
./build/StressTest 127.0.0.1 8888 200 50000
```

预期输出（节选）：

```
[TcpServer] Main Reactor + 8 Sub Reactors ready.
[TcpServer] new connection fd=24
[TcpServer] connection fd=24 deleted.
...
StressTest done: total=50000, failed=0, elapsed=4.2s
```

`failed=0` 且无 ASan / TSan 报错，说明懒注册 + 析构注销 + Business 分流全部生效。

### 7.3 单步验证 deleteConnection 的所有权移交

在 `TcpServer::deleteConnection` 的 lambda 末尾加临时 `std::cerr << "main thread released, sub will delete on " << ioLoop << "\n";`，并在 Connection 析构函数首行加 `std::cerr << "~Connection in tid=" << std::this_thread::get_id() << "\n";`。预期日志：先看到 main 释放、再看到归属 sub-reactor 线程析构。任何"main 线程析构 Connection"的日志都意味着所有权移交失败。

---

## 8. 已知限制与未来改进

| 当前局限 | 后续改进方向 |
|----------|------------|
| `TcpServer` 析构时若 `loop()` 仍在 `poll()` 阻塞，`threadPool_` join 会死锁 | Day 21 引入 `stop()` 与 `atomic<bool> quit_` |
| 跨线程"投递到归属 sub-reactor"目前只是约定（`getLoop()` 返回什么就投到那里），无 `tid_` 自检 | Day 22 加 `EventLoop::tid_` + `isInLoopThread()` 校验 |
| `release()` + raw `delete` 仍是裸指针风格，需要心智注意"投递成功才能 delete" | Day 21 改为 `shared_ptr<Connection>` guard，引用计数自动析构 |
| `subReactors_` 在主线程构造，EventLoop::tid_ 实际是主线程 ID（本日无影响，因为暂无 isInLoopThread 判断） | Day 22 由 `EventLoopThread::threadFunc` 在子线程内构造 |
