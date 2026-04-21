# Day 06：EventLoop + Server + Channel 回调

> 引入 EventLoop 封装事件循环，Server 封装监听/accept/echo 逻辑，Channel 新增 `callback` + `handleEvent()`。
> server.cpp 精简为 3 行：创建 EventLoop → 创建 Server → loop()。

---

## 1. 引言

### 1.1 问题上下文

Day 05 让事件循环可以 `for (ch : ready) ch->handleEvent()`，但 `main()` 仍然在手动调 `ep->poll()`、维护循环、检查退出条件。同一时间，监听 fd 的 accept 处理逻辑直接写在 `main()` 里，新连接的 echo 处理也在 `main()` 里——`main()` 同时承担"事件循环驱动者"和"业务实现者"两个角色。

Reactor 模式的下一步是把这两个角色分开：**EventLoop** 只负责"驱动事件循环"，**Server**（应用层）负责"在新连接到来时该做什么"。Channel 提供回调插槽（`callback_` + `setCallback()` + `handleEvent()`），让 Server 把业务逻辑注入 Channel，EventLoop 不需要知道任何业务细节。

### 1.2 动机

没有这层分离，每写一个新业务（echo / chat / http）都要改事件循环代码。分离后：EventLoop 是稳定的库代码；Server 是应用代码；Channel 是它们之间的契约。

这是单 Reactor 架构的最终形态，也是 Day 13 多 Reactor 的基础——只有 EventLoop 是独立可复用的对象，才能在多线程里给每个线程配一个 EventLoop。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| `main()` 内联事件循环 | Day 05 | 零抽象 | 业务与框架耦合 |
| EventLoop + Channel 回调 | Reactor / muduo | 业务可插拔、循环可复用 | 多了 EventLoop 类，理解成本上升 |
| boost.asio `io_context::run` | Asio | 跨平台、proactor 风格 | 需要 strand 等额外概念 |
| Node.js `libuv` event loop | Node.js | JS 语言级集成 | 单语言绑定 |
| Rust `tokio::runtime` | Tokio | 协程友好、零成本 | 异步函数染色、学习曲线陡 |

### 1.4 本日方案概述

本日实现：
1. 新建 `EventLoop`：持有 `Epoll*`，提供 `loop()`（循环 `poll()` → `handleEvent()`）和 `updateChannel(Channel*)`（中转给 Epoll）。
2. 新建 `Server`：持有 `EventLoop*`、监听 `Socket*`、监听 `Channel*`，提供 `handleReadEvent()` 处理 accept。
3. `Channel` 新增 `callback_` (`std::function<void()>`) + `setCallback()` + `handleEvent()`：发生事件时调 `callback_()`。
4. `server.cpp` 精简为 3 行：new EventLoop → new Server(loop) → loop->loop()。

仍然存在的问题：(a) accept 后产生的客户端 fd 仍然 `new Channel` 然后泄漏；(b) Server 同时管理监听 fd 和业务逻辑，单一职责未到位——下一天 Acceptor 会把 accept 拆出去。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/EventLoop.h` | **新建** | Eventloop 类：持有 Epoll*，提供 `loop()` 和 `updateChannel()` |
| `common/Eventloop.cpp` | **新建** | loop() 循环调 poll() → handleEvent()；updateChannel() 中转到 Epoll |
| `include/Server.h` | **新建** | Server 类：持有 EventLoop*/Socket*/Channel*，`handleReadEvent()` |
| `common/Server.cpp` | **新建** | 构造中初始化 Socket/Channel；handleReadEvent() accept → 创建客户端 Channel |
| `include/Channel.h` | **修改** | 持有者从 `Epoll*` → `Eventloop*`；新增 `callback` + `handleEvent()` + `setCallback()` |
| `common/Channel.cpp` | **修改** | enableReading() 通过 `loop->updateChannel(this)` 注册；handleEvent() 调用 callback |
| `include/Epoll.h` | 不变 | 跨平台 kqueue/epoll |
| `common/Epoll.cpp` | 不变 | |
| `server.cpp` | **修改** | 仅 new EventLoop → new Server → loop->loop() |
| `client.cpp` | 不变 | |

---

## 3. 模块全景与所有权树（Day 06）

```
main()
├── Eventloop* loop                    ← new
│   ├── Epoll* ep                      ← new（loop 拥有）
│   └── bool quit
└── Server* server                     ← new Server(loop)
    ├── Eventloop* loop                ← 非拥有指针
    ├── Socket* server_sock            ← new（server 拥有）
    ├── InetAddress* server_addr       ← new（server 拥有）
    └── Channel* server_sock_channel   ← new Channel(loop, fd)
        ├── Eventloop* loop            ← 非拥有
        ├── int fd
        ├── callback = Server::handleReadEvent   ← std::function<void()>
        └── ...
    [每个客户端]
    ├── Socket* client_sock            ← new，泄漏！
    └── Channel* clientChannel         ← new，泄漏！
        └── callback = [lambda 闭包]   ← 捕获 client_sockfd
```

---

## 4. 全流程调用链

**场景 A：启动初始化**

```
main()
① Eventloop* loop = new Eventloop()
   └── Eventloop::Eventloop()
       ep = new Epoll()
       └── Epoll::Epoll()
           [macOS] kqueue()
           [Linux] epoll_create1(0)

② Server* server = new Server(loop)
   └── Server::Server(loop)
       server_sock = new Socket()           ← socket(AF_INET, SOCK_STREAM, 0)
       server_addr = new InetAddress("127.0.0.1", 8888)
       server_sock->bind(server_addr)       ← ::bind()
       server_sock->listen()                ← ::listen(fd, SOMAXCONN)
       server_sock->setnonblocking()        ← fcntl(fd, F_SETFL, O_NONBLOCK)
       server_sock_channel = new Channel(loop, server_sock->getFd())
       cb = std::bind(&Server::handleReadEvent, this)
       server_sock_channel->setCallback(cb)
       server_sock_channel->enableReading()
       └── Channel::enableReading()
           events = POLLER_READ | POLLER_ET
           loop->updateChannel(this)
           └── Eventloop::updateChannel(ch)
               ep->updateChannel(ch)
               └── Epoll::updateChannel(channel)
                   [macOS] EV_SET + kevent()
                   [Linux] epoll_ctl(EPOLL_CTL_ADD)

③ loop->loop()
   └── Eventloop::loop()
       while (!quit):
           channels = ep->poll()
           for ch : channels:
               ch->handleEvent()       ← 调用 callback
```

**场景 B：新连接到达**

```
Eventloop::loop() → ep->poll() 返回 servChannel
└── servChannel->handleEvent()
    └── callback() → Server::handleReadEvent()
        ① client_addr = new InetAddress()
        ② client_sockfd = server_sock->accept(client_addr)
        ③ client_sock = new Socket(client_sockfd)
           client_sock->setnonblocking()
        ④ clientChannel = new Channel(loop, client_sockfd)
        ⑤ clientChannel->setCallback([lambda])   ← lambda 捕获 client_sockfd
           clientChannel->enableReading()
           └── loop->updateChannel(clientChannel)
               └── ep->updateChannel(clientChannel)
        ⑥ delete client_addr
        ⚠️ client_sock 泄漏
```

**场景 C：客户端数据可读**

```
Eventloop::loop() → ep->poll() 返回 clientChannel
└── clientChannel->handleEvent()
    └── callback() → [lambda]
        while (true):
            read(client_sockfd, buf, sizeof(buf))
            if bytes_read > 0: write() echo
            if EAGAIN/EWOULDBLOCK: break
            if bytes_read == 0:
                close(client_sockfd)
                break
```

---

## 5. 代码逐段解析

### 5.1 EventLoop.h / Eventloop.cpp

```cpp
class Eventloop {
  Epoll *ep;
  bool quit;
public:
  void loop();
  void updateChannel(Channel *ch);
};
```

```cpp
void Eventloop::loop() {
  while (!quit) {
    std::vector<Channel *> channels = ep->poll();
    for (auto it = channels.begin(); it != channels.end(); ++it) {
      (*it)->handleEvent();
    }
  }
}
```

> EventLoop 是 Reactor 模式的核心循环：poll → dispatch（handleEvent）。
> `updateChannel()` 是 Channel 注册到 Epoll 的中转站——Channel 不再直接持有 Epoll 指针。

### 5.2 Server.h / Server.cpp

```cpp
Server::Server(Eventloop *_loop) : loop(_loop) {
  server_sock = new Socket();
  server_addr = new InetAddress("127.0.0.1", 8888);
  server_sock->bind(server_addr);
  server_sock->listen();
  server_sock->setnonblocking();

  server_sock_channel = new Channel(loop, server_sock->getFd());
  std::function<void()> cb = std::bind(&Server::handleReadEvent, this);
  server_sock_channel->setCallback(cb);
  server_sock_channel->enableReading();
}
```

> 构造函数中完成全部初始化：socket → bind → listen → setnonblocking → Channel → callback → enableReading。
> `std::bind(&Server::handleReadEvent, this)` 将成员函数绑定为 `std::function<void()>` 闭包。

### 5.3 Channel 的回调机制

```cpp
void Channel::handleEvent() { callback(); }
void Channel::setCallback(std::function<void()> _cb) { callback = _cb; }
```

> Day 05 中 server.cpp 的 if/else 判断事件类型，现在改为 Channel 直接调用注册的回调函数。
> 客户端 Channel 的回调是一个 lambda，捕获 `client_sockfd` 执行 read/write echo。

### 5.4 server.cpp — 最简 main

```cpp
int main() {
  Eventloop *loop = new Eventloop();
  Server *server = new Server(loop);
  loop->loop();
  delete server;
  delete loop;
  return 0;
}
```

> 与 Day 05 对比：main 从 80 行缩减到 6 行。
> 所有的 accept/echo 逻辑都已分散到 Server 和 Channel 的回调中。

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day06/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day06/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表（Day 06）

| 模块 | 职责 |
|------|------|
| `Eventloop` | 持有 Epoll；执行 poll → handleEvent 循环；Channel 注册中转 |
| `Server` | 持有监听 Socket/Channel；accept 新连接 → 创建客户端 Channel + lambda 回调 |
| `Channel` | fd + events + callback；handleEvent() 调用 callback() |
| `Epoll` | kqueue/epoll 封装；updateChannel / poll |
| `Socket` | RAII fd + bind/listen/accept/connect + setnonblocking |

---

## 7. Day 06 的局限

1. **内存泄漏依旧**：client_sock new 后无 delete；客户端断开时 clientChannel 未 delete
2. **无 Acceptor 抽象**：accept 逻辑写在 Server::handleReadEvent() 中，与 Server 高度耦合
3. **无 Connection 抽象**：客户端 Channel 的回调用 lambda 闭包，无法管理生命周期

→ Day 07–08 将引入 Acceptor 和 Connection 类来解决这些问题。

---

## 8. 对应 HISTORY

→ `HISTORY/day06/`
