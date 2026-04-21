# Day 08：Connection 类——客户端连接的生命周期管理

> 引入 Connection 类，封装客户端 Socket + Channel + 读写回调。
> Server 用 `map<int, Connection*>` 管理所有连接，解决了之前的内存泄漏问题。

---

## 1. 引言

### 1.1 问题上下文

从 Day 05 起，我们一直留着一个未解的债务：每个客户端连接 `new Socket` + `new Channel` 后没有任何对象拥有它们，断开时也没有对应的 `delete`。这是 C++ 项目最经典的内存泄漏来源——每来一个连接，就泄漏几十字节加一个 fd。

工程界对这个问题的标准答案是 **Connection 抽象**：把"一个客户端连接相关的所有资源（fd、读写缓冲区、回调、状态）"打包成一个对象，由一个集中的容器（通常是 Server 持有的 `map<fd, Connection*>`）统一管理生命周期。muduo `TcpConnection` / Netty `Channel` / asio `tcp::socket` 都是这一抽象的不同形态。

### 1.2 动机

没有 Connection：(a) fd 与回调的对应关系散落在 lambda 闭包里，无法集中追踪；(b) 断开时无法可靠地释放资源；(c) 后续要加缓冲区、状态机、应用层上下文时无处挂载。

引入 Connection 后：每个连接是一个明确的对象，Server 负责创建/销毁，Connection 负责自己资源的析构——RAII 终于覆盖到客户端连接这一层。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 裸 fd + lambda 闭包 | Day 05-07 | 简单 | 泄漏、不可追踪 |
| `Connection*` 集中管理 (本日) | muduo `TcpConnection` | 资源集中、可扩展 | 仍是裸指针，删除时机要小心 |
| `shared_ptr<Connection>` 引用计数 | muduo 后期 / asio | 跨线程安全删除 | 循环引用风险、性能略损 |
| `unique_ptr<Connection>` + 集中容器 | 现代 C++ 惯用 | 所有权清晰 | 跨线程访问需小心 |
| Rust `Arc<TcpStream>` + tokio task | Tokio | 编译期所有权检查 | 跨语言 |

### 1.4 本日方案概述

本日实现：
1. 新建 `Connection` 类：构造接收 `(EventLoop*, Socket*)`，内部 new 自己的 Channel 并绑定 `echoRead()` 回调。
2. `echoRead()` 实现完整 echo：read → write，遇到 EOF 触发 `deleteConnectionCallback_(this)`。
3. Server 新增 `map<int, Connection*> connections_` + `deleteConnection(Connection*)`：从 map erase 并 `delete`。
4. `newConnection(int fd)` 把 `new Connection` 存入 map，设置 `deleteConnectionCallback_ = Server::deleteConnection`。

仍然存在的限制：echo 逻辑硬编码在 Connection 内、没有应用层缓冲区、阻塞写——下一天 Buffer 会一次性解决。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Connection.h` | **新建** | Connection 类：持有 Socket* + Channel*，提供 echoRead() |
| `common/Connection.cpp` | **新建** | 构造中创建 Channel 并绑定回调；echoRead() 实现 echo；断开时回调 Server |
| `include/Server.h` | **修改** | 新增 `map<int, Connection*> connection` + `deleteConnection()` |
| `common/Server.cpp` | **修改** | `newConnection()` 创建 Connection 存入 map；`deleteConnection()` 从 map 删除 |
| 其余文件 | 不变 | Channel/Epoll/EventLoop/Acceptor 沿用 Day 07 |

---

## 3. 模块全景与所有权树（Day 08）

```
main()
├── Eventloop* loop
│   └── Epoll* ep
└── Server* server
    ├── Acceptor* acceptor
    │   ├── Socket* sock (监听)
    │   ├── Channel* acceptChannel
    │   └── newConnectionCallback → Server::newConnection
    └── map<int, Connection*> connection
        └── Connection* conn (每个客户端)
            ├── Socket* sock     ← Connection 拥有（析构时 delete）
            ├── Channel* channel ← Connection 拥有（析构时 delete）
            └── deleteConnectionCallback → Server::deleteConnection
```

**生命周期链**：
- 新连接：Acceptor::accept → Server::newConnection → new Connection → 存入 map
- 断开：Connection::echoRead 检测 EOF → deleteConnectionCallback → Server::deleteConnection → 从 map erase → delete Connection → ~Connection delete Channel + Socket

---

## 4. 全流程调用链

**场景 A：新连接**

```
ep->poll() → acceptChannel->handleEvent()
└── Acceptor::acceptConnection()
    client_sock = new Socket(accept_fd)
    newConnectionCallback(client_sock, client_addr)
    └── Server::newConnection(client_sock, client_addr)
        conn = new Connection(loop, client_sock)
        └── Connection::Connection()
            channel = new Channel(loop, sock->getFd())
            cb = std::bind(&Connection::echoRead, this)
            channel->setCallback(cb)
            channel->enableReading()
        conn->setDeleteConnectionCallback(bind(&Server::deleteConnection, ...))
        connection[fd] = conn
```

**场景 B：数据可读**

```
ep->poll() → clientChannel->handleEvent()
└── Connection::echoRead()
    read(sockfd, buf, sizeof(buf))
    if bytes_read > 0: write() echo
    if EAGAIN: break
```

**场景 C：客户端断开**

```
Connection::echoRead()
    bytes_read == 0
    └── deleteConnectionCallback(sock)
        └── Server::deleteConnection(sock)
            sockfd = sock->getFd()
            conn = connection[sockfd]
            connection.erase(sockfd)
            delete conn
            └── ~Connection()
                delete channel
                delete sock   ← close(fd)
```

---

## 5. 代码逐段解析

### 5.1 Connection.h — 客户端连接的封装

```cpp
class Connection {
  Eventloop *loop;
  Socket *sock;
  Channel *channel;
  std::function<void(Socket *)> deleteConnectionCallback;
public:
  Connection(Eventloop *_loop, Socket *_sock);
  ~Connection();
  void echoRead();
  void setDeleteConnectionCallback(std::function<void(Socket *)> _cb);
};
```

> Connection 拥有 Socket 和 Channel——析构时一并删除。
> `deleteConnectionCallback` 在 EOF 时通知 Server 清理自己。

### 5.2 Connection 构造 — 自动注册 Channel

```cpp
Connection::Connection(Eventloop *_loop, Socket *_sock)
    : loop(_loop), sock(_sock), channel(nullptr) {
    channel = new Channel(loop, sock->getFd());
    std::function<void()> cb = std::bind(&Connection::echoRead, this);
    channel->setCallback(cb);
    channel->enableReading();
}
```

> 构造即注册：创建 Channel → 绑定 echoRead 回调 → enableReading。

### 5.3 Connection::echoRead() — 断开时的回调链

```cpp
} else if (bytes_read == 0) {
    if (deleteConnectionCallback) {
        deleteConnectionCallback(sock);
    }
    break;
}
```

> 读到 EOF 时不再直接 close(fd)，而是通过 `deleteConnectionCallback` 通知 Server。
> Server 从 map 中移除后 delete Connection，析构函数自动 close fd。

### 5.4 Server — map 管理连接生命周期

```cpp
void Server::newConnection(Socket *client_sock, InetAddress *client_addr) {
    Connection *conn = new Connection(loop, client_sock);
    conn->setDeleteConnectionCallback(bind(&Server::deleteConnection, ...));
    connection[client_sock->getFd()] = conn;
}

void Server::deleteConnection(Socket *sock) {
    int sockfd = sock->getFd();
    Connection *conn = connection[sockfd];
    connection.erase(sockfd);
    delete conn;
}
```

> `map<int, Connection*>` 以 fd 为 key 管理所有活跃连接。
> 至此，Day 06-07 的内存泄漏问题全部解决。

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day08/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day08/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表（Day 08）

| 模块 | 职责 |
|------|------|
| `Connection` | 拥有客户端 Socket + Channel；echoRead 处理数据；断开时回调 Server |
| `Server` | 创建 Acceptor + 管理 Connection map；newConnection/deleteConnection |
| `Acceptor` | bind/listen/accept → 回调 |
| `Eventloop` | poll → handleEvent 循环 |
| `Channel` | fd + events + callback |

---

## 7. Day 08 的局限

1. **业务逻辑硬编码**：echoRead() 写死了 echo，无法自定义
2. **无 Buffer 缓冲区**：直接在栈上分配 char buf[1024]，无法处理半包/粘包
3. **单线程**：所有 IO 在一个线程中，无法利用多核

→ Day 09 引入 Buffer 缓冲区，Day 10 引入 ThreadPool。

---

## 8. 对应 HISTORY

→ `HISTORY/day08/`
