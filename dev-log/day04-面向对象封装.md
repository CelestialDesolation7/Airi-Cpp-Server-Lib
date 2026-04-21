# Day 04：面向对象封装——Socket / InetAddress / Epoll 类

> 将 Day 03 中散落在 main() 的裸系统调用封装为 C++ 类。
> 引入 include/ 目录（头文件）和 common/ 目录（实现文件），形成初步的模块划分。

---

## 1. 引言

### 1.1 问题上下文

Day 03 完成了多路复用，但所有逻辑塞在一个 `main()` 里：`socket()`、`bind()`、`epoll_ctl()`、`accept()`、`read()` 互相嵌套，错误处理穿插，难以扩展。这是 C 风格"过程式服务器"的天花板。

C++ 的核心抽象工具是"类 + RAII"——把"获得资源即获得对象，对象销毁即释放资源"作为不变量，可以彻底消除 fd 泄漏与错误路径上的资源遗漏。muduo / boost.asio / folly 的设计基础都在这一层。

### 1.2 动机

不封装的代价不仅是难读：每加一个新功能都要修改 `main()`，导致测试不可能、平台兼容性差（kqueue 与 epoll 的差异散落各处）、生命周期错乱（哪个 fd 在哪里关？）。

封装为类后：`Socket` 只关心 fd 生命周期，`InetAddress` 只关心地址结构体，`Epoll` 只关心多路复用细节。`main()` 退化为"组装积木"——这是后续几十天能持续叠加抽象的前提。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 裸 fd + 全局函数 | C 风格 | 简单直观 | 无 RAII、易泄漏、不可测试 |
| C++ 类 + 析构 close fd | 本日做法 / muduo | RAII、单一职责、易组合 | 早期版本忘记禁拷贝时仍可能 double-close |
| `std::unique_ptr<int, FdDeleter>` | 现代 C++ 惯用 | 类型系统强制 ownership | 语法略繁琐，难以扩展行为 |
| `boost::asio::ip::tcp::socket` | Boost.Asio | 已工业化、跨平台 | 引入大型依赖、概念重 |
| Rust `TcpListener/TcpStream` | Rust std | 所有权静态检查、无 GC | 跨语言成本 |

### 1.4 本日方案概述

本日实现：
1. 新建 `include/` 和 `common/` 两个目录，区分声明与实现。
2. `Socket` 类：构造创建 fd 并 `bind`/`listen`/`accept`/`connect`，析构 `close(fd_)`。
3. `InetAddress` 类：包装 `sockaddr_in`，提供地址初始化与 getter。
4. `Epoll` 类：跨平台封装 epoll/kqueue，提供 `addFd()` / `poll()`，统一返回 `vector<epoll_event*>` 风格的事件结构。
5. `util.cpp` 增 `setnonblocking(fd)`。
6. `server.cpp` 全部用类对象重写。

仍然存在的问题：`Channel` 还没出现，事件与回调没有绑定——下一天解决。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/Socket.h` | **新建** | Socket 类声明：封装 socket/bind/listen/accept/connect |
| `common/Socket.cpp` | **新建** | Socket 类实现：RAII 管理 fd，析构时 close |
| `include/InetAddress.h` | **新建** | InetAddress 类声明：封装 sockaddr_in |
| `common/InetAddress.cpp` | **新建** | InetAddress 类实现：构造时填充 addr 结构体 |
| `include/Epoll.h` | **新建** | Epoll 类声明：跨平台封装（kqueue/epoll），统一 ActiveEvent 接口 |
| `common/Epoll.cpp` | **新建** | Epoll 类实现：`addFd()` + `poll()` |
| `include/util.h` | **修改** | 新增 `setnonblocking()` 声明 |
| `common/util.cpp` | **修改** | 新增 `setnonblocking()` 实现 |
| `server.cpp` | **重写** | 使用 Socket/InetAddress/Epoll 类重写事件循环 |
| `client.cpp` | 不变 | 同 Day 03 |

---

## 3. 模块全景与所有权树（Day 04）

```
main() (server)
├── Socket* serv_sock            ← new 在堆上，main 负责 delete
│   └── int fd                   ← Socket 构造时 socket() 创建，析构时 close()
├── InetAddress* serv_addr       ← new 在堆上
│   └── struct sockaddr_in addr  ← 构造时 inet_addr + htons
├── Epoll* ep                    ← new 在堆上，main 负责 delete
│   ├── int epfd                 ← kqueue()/epoll_create1() 创建，析构时 close()
│   └── kevent*/epoll_event*     ← new[] 创建，析构时 delete[]
└── [client fd]                  ← accept() 返回的裸 int，main 中 close()

工具函数：
├── errif(cond, msg)             ← 条件错误 → perror → exit
└── setnonblocking(fd)           ← fcntl 设置 O_NONBLOCK
```

**设计问题**：`serv_sock` 和 `ep` 用 `new` 在堆上分配但在无限循环后 delete——实际不可达。后续版本会改进。

---

## 4. 全流程调用链

**场景 A：服务器启动**

```
[main()]

① Socket* serv_sock = new Socket()
   └── Socket::Socket()
       └── fd = socket(AF_INET, SOCK_STREAM, 0)
       └── errif(fd == -1, ...)

② InetAddress* serv_addr = new InetAddress("127.0.0.1", 8888)
   └── InetAddress::InetAddress(ip, port)
       └── bzero(&addr, sizeof(addr))
       └── addr.sin_family = AF_INET
       └── addr.sin_addr.s_addr = inet_addr("127.0.0.1")
       └── addr.sin_port = htons(8888)

③ serv_sock->bind(serv_addr)
   └── Socket::bind()
       └── ::bind(fd, (sockaddr*)&addr->addr, addr->addr_len)

④ serv_sock->listen()
   └── Socket::listen()
       └── ::listen(fd, SOMAXCONN)

⑤ Epoll* ep = new Epoll()
   └── Epoll::Epoll()
       └── [macOS] kqueue() → epfd
       └── [Linux] epoll_create1(0) → epfd
       └── events = new kevent/epoll_event[1024]

⑥ setnonblocking(serv_sock->getFd())
   └── fcntl(fd, F_SETFL, oldflags | O_NONBLOCK)

⑦ ep->addFd(serv_sock->getFd(), POLLER_READ | POLLER_ET)
   └── [macOS] EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR, ...)
       kevent(epfd, &change, 1, nullptr, 0, nullptr)
   └── [Linux] epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev{EPOLLIN|EPOLLET})
```

**场景 B：新连接到达**

```
[事件循环]

① ep->poll() → vector<ActiveEvent>
   └── [macOS] kevent(kqfd, NULL, 0, events, MAX, NULL)
   └── [Linux] epoll_wait(epfd, events, MAX, -1)

② events[i].fd == serv_sock->getFd()
   └── 监听 socket 可读 → 有新连接

③ InetAddress* client_addr = new InetAddress()
④ serv_sock->accept(client_addr) → client_sockfd
   └── Socket::accept()
       └── ::accept(fd, (sockaddr*)&addr->addr, &addr->addr_len)

⑤ setnonblocking(client_sockfd)
⑥ ep->addFd(client_sockfd, POLLER_READ | POLLER_ET)
⑦ delete client_addr  ← 地址信息打印后不再需要
```

**场景 C：数据到达 → echo**

```
[事件循环]

① events[i].events & POLLER_READ（非监听 fd）
② while(true) {
       read(sockfd, buf, 1024)
       > 0 → write(sockfd, buf, bytes_read)
       EINTR → continue
       EAGAIN → break（读空）
       == 0 → close(sockfd) + break（对端断开）
   }
```

---

## 5. 代码逐段解析

### 5.1 InetAddress 类

```cpp
class InetAddress {
public:
  struct sockaddr_in addr;
  socklen_t addr_len;

  InetAddress();
  InetAddress(const char *ip, uint16_t port);
  ~InetAddress();
};
```

```cpp
InetAddress::InetAddress(const char *ip, uint16_t port)
    : addr_len(sizeof(addr)) {
  bzero(&addr, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(ip);
  addr.sin_port = htons(port);
}
```

> InetAddress 封装 `sockaddr_in`，构造时完成 `inet_addr` + `htons` 转换。
> 默认构造函数留空（用于 accept 时接收客户端地址）。
> `addr` 是 public 的（Day 04 尚未做访问控制）。

### 5.2 Socket 类

```cpp
class Socket {
private:
  int fd;
public:
  Socket();
  Socket(int fd);
  ~Socket();
  void bind(InetAddress *addr);
  void listen();
  int accept(InetAddress *addr);
  void connect(InetAddress *addr);
  int getFd();
};
```

```cpp
Socket::Socket() : fd(-1) {
  fd = socket(AF_INET, SOCK_STREAM, 0);
  errif(fd == -1, "socket create error");
}

Socket::~Socket() {
  if (fd != -1) {
    close(fd);
    fd = -1;
  }
}
```

> Socket 是 fd 的 RAII 包装器：构造时 `socket()`，析构时 `close()`。
> `bind()`、`listen()` 使用 `::bind()`、`::listen()` 带前缀 `::` 调用全局函数，避免与成员函数名冲突。

### 5.3 Epoll 类（跨平台）

```cpp
struct ActiveEvent {
  int fd;
  uint32_t events;
};

class Epoll {
private:
  int epfd;
  // macOS: struct kevent*; Linux: struct epoll_event*
public:
  Epoll();
  ~Epoll();
  void addFd(int fd, uint32_t op);
  std::vector<ActiveEvent> poll(int timeout = -1);
};
```

> `ActiveEvent` 是统一的跨平台事件结构体，只包含 `fd` 和 `events`。
> `addFd()` 在 macOS 上使用 `EV_SET` + `kevent()` 注册；Linux 上使用 `epoll_ctl`。
> `poll()` 在 macOS 上使用 `kevent()` 等待；Linux 上使用 `epoll_wait()`。
> 析构时 `close(epfd)` + `delete[] events` 释放资源。

### 5.4 server.cpp — 使用类重写事件循环

```cpp
Socket *serv_sock = new Socket();
InetAddress *serv_addr = new InetAddress("127.0.0.1", 8888);
serv_sock->bind(serv_addr);
serv_sock->listen();

Epoll *ep = new Epoll();
setnonblocking(serv_sock->getFd());
ep->addFd(serv_sock->getFd(), POLLER_READ | POLLER_ET);
```

> 创建 socket 不再需要手动调用 `socket()` + `bind()` + 检查返回值。
> 所有错误检查都在类内部的 `errif()` 中完成。

```cpp
std::vector<ActiveEvent> events = ep->poll();
for (int i = 0; i < nfds; ++i) {
    if (events[i].fd == serv_sock->getFd()) {
        // 新连接
    } else if (events[i].events & POLLER_READ) {
        // 数据
    }
}
```

> 事件循环代码从 Day 03 的 `epoll_event` 结构体改为统一的 `ActiveEvent`。

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day04/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day04/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表（Day 04）

| 模块 | 职责 |
|------|------|
| `Socket` | RAII 管理 fd；封装 bind/listen/accept/connect |
| `InetAddress` | 封装 sockaddr_in 地址构造 |
| `Epoll` | 跨平台 IO 多路复用（kqueue/epoll）；返回统一 ActiveEvent |
| `errif()` | 条件错误检查 → exit |
| `setnonblocking()` | fcntl 设置 O_NONBLOCK |
| `main()` (server) | 组装上述模块 + 事件循环 |
| `main()` (client) | 裸 socket 连接 + 交互循环 |

---

## 7. Day 04 的局限

1. **堆分配 new 无匹配 delete**：`while(true)` 后的 delete 不可达
2. **client fd 未封装**：accept 返回裸 int，手动 close
3. **Epoll 只有 addFd**：无 modFd / delFd
4. **无 Channel 抽象**：事件回调仍在 main 中硬编码

→ Day 05 引入 Channel 类，将事件与回调函数绑定。

---

## 8. 对应 HISTORY

→ `HISTORY/day04/`
