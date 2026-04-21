# Day 03：IO 多路复用（epoll / kqueue）+ 非阻塞 IO

> 引入 IO 多路复用机制，单线程同时监控多个 socket fd，支持多客户端并发。
> 使用边缘触发（Edge-Triggered）模式 + 非阻塞 fd，必须循环读空缓冲区。

---

## 1. 引言

### 1.1 问题上下文

Day 02 的 echo 服务器有一个致命限制：`read()` 会阻塞当前线程直到数据到达，期间既不能 `accept()` 新连接，也不能服务其他客户端。要在单线程里同时跟踪多个 fd 的可读/可写状态，必须借助内核提供的 IO 多路复用机制。

POSIX 提供了 `select` (1983) → `poll` (System V) → `epoll` (Linux 2.6, 2002) → `kqueue` (FreeBSD 4.1, 2000) 的演进谱系，到今天 Linux 用 `epoll`、BSD 系（含 macOS）用 `kqueue`，是几乎所有高性能 C++ 服务器的事件机制基座。

### 1.2 动机

如果不引入多路复用，要么用线程-per-连接（C10K 问题下不可行），要么用进程-per-连接（开销更大）。多路复用的本质是把"我等谁"这件事从用户态搬到内核——用户线程一次睡眠，可以被任何已就绪的 fd 唤醒。

引入它的同时还要解决一个配套问题：默认 fd 是阻塞的，多路复用返回"可读"后调 `read` 仍可能再次阻塞（伪唤醒/对端断开等），所以必须把 fd 设为非阻塞，并配合边缘触发（ET）模式循环读到 `EAGAIN`。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| `select` | 4.2BSD | 跨平台、API 简单 | fd 上限 1024、O(n) 扫描、用户态/内核态拷贝整 fd_set |
| `poll` | System V | 解除 fd 上限 | 仍是 O(n) 扫描、API 略繁琐 |
| `epoll` (LT/ET) | Linux 2.6 | O(1) 就绪通知、ET 减少系统调用次数 | Linux only、ET 易写错（必须循环读） |
| `kqueue` | FreeBSD/macOS | 不止 IO，还能监听文件、信号、定时器 | BSD 系专属 |
| `IOCP` | Windows | 真异步（完成通知） | Windows 专属、API 复杂 |
| `io_uring` | Linux 5.1+ | 异步提交 + 零拷贝 + 批量 | 内核版本要求高 |

### 1.4 本日方案概述

本日实现：
1. `server.cpp` 重写为多路复用版：Linux 用 `epoll_create1 + epoll_ctl + epoll_wait`，macOS 用 `kqueue + EV_SET + kevent`，通过 `#ifdef __linux__/__APPLE__` 切换。
2. 监听 fd 与所有 connection fd 都用 `setnonblocking()` 切到非阻塞；多路复用用边缘触发（`EPOLLET` / `EV_CLEAR`）。
3. 抽 `handle_read()` 处理就绪 fd 的循环读，遇到 `EAGAIN` 才退出。
4. `client.cpp` 把 `scanf` 换成 `fgets` 避免缓冲区溢出。

代码仍然散落在 `main()` 里——下一天会做面向对象封装。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `server.cpp` | **重写** | 引入 epoll/kqueue + 非阻塞 + 边缘触发；支持多客户端；提取 `handle_read()` |
| `client.cpp` | **修改** | `scanf` 改为 `fgets` 避免缓冲区溢出，去除行末换行 |
| `util.h` | 不变 | 同 Day 02 |
| `util.cpp` | 不变 | 同 Day 02 |

---

## 3. 核心知识：IO 多路复用

**为什么需要 IO 多路复用？**

Day 02 的 echo 服务器只能服务一个客户端——`read()` 阻塞期间无法 `accept()` 新连接。
IO 多路复用让一个线程同时监控多个 fd 的事件状态。

| 概念 | epoll (Linux) | kqueue (macOS/BSD) |
|------|---------------|---------------------|
| 创建实例 | `epoll_create1(0)` → `epfd` | `kqueue()` → `kqfd` |
| 注册 fd | `epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev)` | `EV_SET(&change, fd, EVFILT_READ, EV_ADD\|EV_CLEAR, ...)` + `kevent(kqfd, &change, 1, ...)` |
| 等待事件 | `epoll_wait(epfd, events, MAX, -1)` | `kevent(kqfd, NULL, 0, events, MAX, NULL)` |
| 内核数据结构 | 红黑树存储注册的 fd + 就绪链表 | 哈希表 + knote 链表 |
| 边缘触发 | `EPOLLET` 标志 | `EV_CLEAR` 标志（等效边缘触发） |

**水平触发 vs 边缘触发：**

- **水平触发（LT）**：只要缓冲区有数据，每次 wait 都返回该 fd → 简单但可能重复通知
- **边缘触发（ET）**：只在状态变化（新数据到达）时通知一次 → 必须循环读空

---

## 4. 模块全景与所有权树（Day 03）

```
main()
├── int sockfd                  ← 监听 socket
├── int kqfd / epfd             ← IO 多路复用实例 fd
├── struct kevent / epoll_event ← 事件数组（栈上）
└── [多个 client_sockfd]        ← 由 accept() 动态创建，注册到 kqueue/epoll

辅助函数：
├── setnonblocking(fd)          ← fcntl 设置 O_NONBLOCK
├── handle_read(fd)             ← 边缘触发下的循环读取 + echo
└── errif(cond, msg)            ← 条件错误退出
```

---

## 5. 初始化顺序

```
[主线程, main()]

① socket(AF_INET, SOCK_STREAM, 0) → sockfd
② bind(sockfd, {127.0.0.1:8888})
③ listen(sockfd, SOMAXCONN)
④ setnonblocking(sockfd)
   └── fcntl(sockfd, F_GETFL) → oldflags
   └── fcntl(sockfd, F_SETFL, oldflags | O_NONBLOCK)
       └── 监听 socket 设为非阻塞，accept() 无连接时返回 EAGAIN 而非阻塞

⑤ [macOS] kqueue() → kqfd
   └── 内核：分配 kqueue 文件描述符
   EV_SET(&change, sockfd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr)
   kevent(kqfd, &change, 1, nullptr, 0, nullptr)
   └── 将 sockfd 的可读事件注册到 kqueue，EV_CLEAR = 边缘触发

   [Linux] epoll_create1(0) → epfd
   └── 内核：分配 epoll 实例（红黑树 + 就绪链表）
   ev.data.fd = sockfd; ev.events = EPOLLIN | EPOLLET
   epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev)
   └── 将 sockfd 加入 epoll 红黑树
```

---

## 6. 全流程调用链

**场景 A：新客户端连接**

```
[主线程, 事件循环]

① kevent(kqfd, NULL, 0, events, MAX, NULL) / epoll_wait(...)
   └── 阻塞：等待任一已注册 fd 发生事件

② events[i].ident == sockfd (kqueue) / events[i].data.fd == sockfd (epoll)
   └── 监听 socket 可读 → 有新连接到达

③ accept(sockfd, &client_addr, &len) → client_sockfd
   └── 从 Accept Queue 取出已完成握手的连接

④ setnonblocking(client_sockfd)
   └── 新连接也设为非阻塞

⑤ [macOS] EV_SET(&change, client_sockfd, EVFILT_READ, EV_ADD | EV_CLEAR, ...)
   kevent(kqfd, &change, 1, ...)
   └── 将新 fd 注册到 kqueue

   [Linux] ev.data.fd = client_sockfd; ev.events = EPOLLIN | EPOLLET
   epoll_ctl(epfd, EPOLL_CTL_ADD, client_sockfd, &ev)
   └── 将新 fd 加入 epoll 红黑树
```

**场景 B：客户端发送数据（边缘触发读取）**

```
[主线程, handle_read(fd)]

① events[i].filter == EVFILT_READ (kqueue) / events[i].events & EPOLLIN (epoll)
   └── 某连接 fd 可读

② handle_read(fd) {
       while(true) {
           bzero(buf, 1024)
           bytes_read = read(fd, buf, 1024)
           └── 非阻塞 read：
               > 0：有数据，处理并 write() 回显
               EINTR：被信号中断 → continue
               EAGAIN/EWOULDBLOCK：缓冲区读空 → break（正常退出）
               == 0：对端关闭 → close(fd) + break
       }
   }
```

**场景 C：客户端断开**

```
[handle_read(fd)]

read(fd, ...) 返回 0
└── 对端发送了 FIN
close(fd)
└── 内核自动将 fd 从 kqueue/epoll 中移除（fd 关闭时自动注销）
```

---

## 7. 代码逐段解析

### 7.1 setnonblocking — 设置非阻塞

```cpp
void setnonblocking(int fd) {
  int oldoptions = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, oldoptions | O_NONBLOCK);
}
```

> `fcntl(fd, F_GETFL)` 获取 fd 当前的文件状态标志。
> `O_NONBLOCK` 位或上去后，`read()`/`accept()` 在无数据/无连接时返回 `-1 + EAGAIN` 而非阻塞。

### 7.2 handle_read — 边缘触发循环读取

```cpp
void handle_read(int fd) {
  char buf[READ_BUFFER];
  while (true) {
    bzero(buf, sizeof(buf));
    ssize_t bytes_read = read(fd, buf, sizeof(buf));
    if (bytes_read > 0) {
      std::cout << "[服务器] 收到客户端 fd " << fd << " 的消息: " << buf
                << std::endl;
      write(fd, buf, bytes_read);
    } else if (bytes_read == -1 && errno == EINTR) {
      continue;
    } else if (bytes_read == -1 &&
               (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else if (bytes_read == 0) {
      std::cout << "[服务器] 客户端 fd " << fd << " 已关闭连接" << std::endl;
      close(fd);
      break;
    }
  }
}
```

> **边缘触发的关键**：内核只通知一次"有新数据"。如果不循环读空：
> - 假设缓冲区有 2048 字节，`read()` 读了 1024 字节
> - 剩余 1024 字节永远不会被通知（除非对端再发新数据）
> - 所以必须 `while(true)` 循环，直到 `EAGAIN` 表示读空
>
> `EINTR` 表示 read 被信号中断（如 SIGCHLD），不是错误，继续重试。
> `write(fd, buf, bytes_read)` 此处改用 `bytes_read` 而非 `sizeof(buf)`，修复了 Day 02 多发零字节的问题。

### 7.3 kqueue 注册（macOS 路径）

```cpp
int kqfd = kqueue();
errif(kqfd == -1, "[服务器] kqueue 创建错误");

struct kevent change;
EV_SET(&change, sockfd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
kevent(kqfd, &change, 1, nullptr, 0, nullptr);
```

> `kqueue()` 创建一个内核事件队列。
> `EV_SET` 是填充 `struct kevent` 的宏：
> - `sockfd`：要监控的 fd
> - `EVFILT_READ`：监控可读事件
> - `EV_ADD`：添加到 kqueue
> - `EV_CLEAR`：等效边缘触发——事件返回后自动清除，只在下次状态变化时重新触发
> `kevent()` 第 2-3 参数是要提交的变更，第 4-5 参数是接收就绪事件（此处为 nullptr，仅提交）。

### 7.4 epoll 注册（Linux 路径）

```cpp
int epfd = epoll_create1(0);
errif(epfd == -1, "[服务器] Epoll 创建错误");

struct epoll_event events[MAX_EVENTS];
struct epoll_event ev;
ev.data.fd = sockfd;
ev.events = EPOLLIN | EPOLLET;
epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);
```

> `epoll_create1(0)` 创建 epoll 实例。
> `EPOLLIN`：监控可读事件。`EPOLLET`：边缘触发模式。
> `epoll_ctl` 的 `EPOLL_CTL_ADD` 将 sockfd 加入内核红黑树。

### 7.5 client.cpp — fgets 改进

```cpp
if (fgets(buf, sizeof(buf), stdin) == NULL) {
    break;
}
size_t len = strlen(buf);
if (len > 0 && buf[len - 1] == '\n') {
    buf[len - 1] = '\0';
    len--;
}
if (len == 0) continue;
```

> `fgets` 替代 Day 02 的 `scanf`：有长度限制（`sizeof(buf)`），防止缓冲区溢出。
> `fgets` 保留换行符 `\n`，需手动剥除。
> 空行跳过，不发送。

---

### 7.6 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day03/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day03/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 8. 职责划分表（Day 03）

| 模块 | 职责 |
|------|------|
| `setnonblocking(fd)` | fcntl 设置 O_NONBLOCK |
| `handle_read(fd)` | 边缘触发循环读取 + echo 回显 |
| `errif()` | 条件错误 → perror → exit |
| `main()` (server) | socket 初始化 + kqueue/epoll 事件循环 |
| `main()` (client) | socket 连接 + fgets 交互循环 |

---

## 9. Day 03 的局限

1. **单线程事件循环**：`handle_read()` 中耗时操作会阻塞其他 fd
2. **accept 只调用一次**：边缘触发下，若多个连接同时到达，只 accept 第一个
3. **所有逻辑在 main 中**：无面向对象封装
4. **平台相关代码混在一起**：`#ifdef` 散落在 main 中

→ Day 04 开始封装 Channel / Socket / InetAddress 等类。

---

## 10. 对应 HISTORY

→ `HISTORY/day03/`
