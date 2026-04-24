# Day 01：原始 TCP 服务器——socket / bind / listen / accept

> 本节从零开始，用纯 POSIX 系统调用建立最小可用的 TCP 服务器：接受单个客户端连接后关闭。
> 没有数据收发，没有任何抽象，只展示 socket 生命周期。

---

## 1. 引言

### 1.1 问题上下文

单进程网络程序的最小骨架是 TCP 三次握手所需的四个系统调用：`socket()` 创建端点、`bind()` 绑定地址、`listen()` 标记为被动监听、`accept()` 提取已完成握手的连接。1983 年 4.2BSD 把 socket API 引入 UNIX 后，几乎所有现代网络栈都沿用了这个模型——同一套接口在 Linux、macOS、Windows（WinSock）上几乎只在大小写与头文件上有差别。

理解这层裸 API 是后续所有 Reactor、协程、io_uring 抽象的起点：抽象再多，最底下永远是 `accept()` 返回的那个 `int fd`。

### 1.2 动机

很多教程在介绍服务器编程时直接跳到框架（libevent / boost.asio / muduo），跳过了系统调用本身，结果学习者无法判断框架在解决什么问题、为什么需要非阻塞、为什么需要事件循环。

本日的目标是**先把抽象拆光**：用最少的代码体现一个 TCP 服务器从无到有的过程，让后续每一次封装都能对回这一层，理解"被封装掉的是什么、为什么要封装"。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 裸 socket + accept 阻塞 | 4.2BSD (1983) | 概念最少、跨平台、零依赖 | 单连接、无并发、阻塞期间 CPU 闲置 |
| inetd / systemd socket activation | BSD inetd / systemd | 系统统一管理监听 fd | 每连接 fork 进程，不适合高并发 |
| select/poll 多路复用 | POSIX | 单线程多连接 | O(n) 扫描、fd 数量受限 |
| epoll/kqueue + 事件回调 | Linux 2.6 / FreeBSD | O(1) 就绪通知，是现代服务器主流 | API 复杂，需要正确处理 ET/LT |
| io_uring 异步提交 | Linux 5.1+ | 系统调用批量化、零拷贝 | 内核版本要求高，编程模型新 |

### 1.4 本日方案概述

本日实现：
1. `server.cpp`：完成 socket → bind → listen → accept → close 全流程，仅服务一个连接后退出。
2. `client.cpp`：socket → connect → close，验证服务端可被连上。
3. 不做 echo、不做并发、不做错误抽象——保留所有 `if(fd<0) perror; exit;` 让读者直面错误处理细节。

后续每一天都会在这套裸代码上加一层抽象，并明确指出"这一层解决了上一层的哪个具体问题"。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `server.cpp` | **新建** | TCP 服务器：socket → bind → listen → accept → close |
| `client.cpp` | **新建** | TCP 客户端：socket → connect → close |

---

## 3. 核心知识：TCP socket 生命周期

| 系统调用 | 内核行为 | 返回值 |
|----------|----------|--------|
| `socket(AF_INET, SOCK_STREAM, 0)` | 在内核分配一个 `struct socket` 结构体，关联到进程的文件描述符表 | `int fd`（非负整数） |
| `bind(fd, addr, len)` | 将 `(IP, port)` 二元组绑定到此 socket；若端口被占用返回 `-1` | `0` 成功 / `-1` 失败 |
| `listen(fd, backlog)` | 将 socket 从 `CLOSED` 状态标记为 `LISTEN`；内核为其维护 SYN 队列和 Accept 队列 | `0` 成功 / `-1` 失败 |
| `accept(fd, addr, len)` | **阻塞**：从 Accept 队列取出一个已完成三次握手的连接，返回全新的 fd | `int connfd`（连接 socket） |
| `close(fd)` | 递减引用计数，为 0 时释放内核缓冲区并发送 FIN | `0` 成功 |

**socket 的两种类型：**

- **监听 socket（Listen Socket）**：工厂，永不传输数据。内核为其维护：
  - **半连接队列（SYN Queue）**：正在三次握手中的连接，OS 自动处理。
  - **已完成队列（Accept Queue）**：三次握手完成，等待 `accept()` 提取。`backlog` 参数限制此队列长度。
- **连接 socket（Connected Socket）**：由 `accept()` 返回，拥有完整五元组 `(源IP, 源Port, 目的IP, 目的Port, TCP)`，各自独立的发送/接收内核缓冲区。

---

## 4. 模块全景与所有权树（Day 01）

```
main()                          ← 栈帧，程序唯一的作用域
├── int sockfd                  ← 监听 socket fd，main 负责 close()
├── struct sockaddr_in server_addr  ← 栈上地址结构体
├── struct sockaddr_in client_addr  ← 栈上地址结构体（accept 填充）
└── int client_sockfd           ← 连接 socket fd，main 负责 close()
```

Day 01 没有任何类和抽象——所有资源都是 `main()` 里的局部变量。

---

## 5. 初始化顺序

```
[主线程, main()]

① socket(AF_INET, SOCK_STREAM, 0)
   └── 内核：分配 struct socket，关联 fd 到进程文件描述符表
   └── 返回 sockfd（例如 3）

② memset(&server_addr, 0, sizeof(server_addr))
   server_addr.sin_family = AF_INET
   server_addr.sin_addr.s_addr = inet_addr("127.0.0.1")
       └── 将点分十进制字符串 "127.0.0.1" 转为 32 位网络字节序整数 0x0100007F
   server_addr.sin_port = htons(8888)
       └── 将主机字节序（小端 0xB822）转为网络字节序（大端 0x22B8）

③ bind(sockfd, (sockaddr*)&server_addr, sizeof(server_addr))
   └── 内核：将 (127.0.0.1, 8888) 绑定到 sockfd
   └── 返回 0（成功）

④ listen(sockfd, SOMAXCONN)
   └── 内核：sockfd 状态 CLOSED → LISTEN，创建 SYN/Accept 队列
   └── SOMAXCONN 通常为 128（macOS）或 4096（Linux）
```

---

## 6. 全流程调用链

**场景 A：服务器启动 → 等待客户端 → 接受连接 → 关闭**

```
[主线程]

① socket(AF_INET, SOCK_STREAM, 0) → sockfd = 3
② bind(sockfd, {127.0.0.1:8888}) → 0
③ listen(sockfd, SOMAXCONN) → 0
④ std::cout << "[服务器] 正在监听 127.0.0.1:8888"
⑤ accept(sockfd, &client_addr, &client_addr_len)
   └── 阻塞：主线程挂起，进程进入 S（interruptible sleep）状态
   └── [此时另一终端运行 client，客户端调用 connect()]
   └── 内核完成三次握手：
       SYN → SYN+ACK → ACK
       连接从 SYN Queue 移入 Accept Queue
   └── accept() 返回 client_sockfd = 4

⑥ inet_ntoa(client_addr.sin_addr)
   └── 将 32 位网络字节序 IP 转回点分十进制字符串（如 "127.0.0.1"）
   ntohs(client_addr.sin_port)
   └── 将网络字节序端口转为主机字节序（如 54321）

⑦ close(client_sockfd)
   └── 内核：向客户端发送 FIN，释放连接 socket 资源

⑧ close(sockfd)
   └── 内核：释放监听 socket 资源
```

**场景 B：客户端连接 → 关闭**

```
[客户端进程, main()]

① socket(AF_INET, SOCK_STREAM, 0) → sockfd = 3
② server_addr = {127.0.0.1:8888}
③ connect(sockfd, &server_addr, sizeof(server_addr))
   └── 内核：发送 SYN 到 127.0.0.1:8888
   └── 三次握手完成后 connect() 返回 0
   └── std::cout << "[客户端] 成功与server建立TCP连接"
④ close(sockfd)
   └── 内核：发送 FIN，完成四次挥手
```

---

## 7. 代码逐段解析

### 7.1 server.cpp

**头文件引入：**

```cpp
#include <arpa/inet.h>      // inet_addr(), inet_ntoa(), htons(), ntohs()
#include <cstdio>            // perror()
#include <cstring>           // memset()
#include <iostream>          // std::cout
#include <netinet/in.h>      // struct sockaddr_in, AF_INET
#include <sys/socket.h>      // socket(), bind(), listen(), accept()
#include <unistd.h>          // close()
```

> 这些都是 POSIX 标准头文件，Linux 和 macOS 通用。
> `<arpa/inet.h>` 提供 IP 地址转换函数；`<sys/socket.h>` 提供核心 socket API。

**创建监听 socket：**

```cpp
int sockfd = socket(AF_INET, SOCK_STREAM, 0);
if (sockfd == -1) {
    perror("[服务器] socket创建失败");
    return -1;
}
```

> `AF_INET` 指定 IPv4，`SOCK_STREAM` 指定 TCP（字节流）。
> `socket()` 失败返回 -1，`perror()` 会打印 errno 对应的错误信息（如 "Too many open files"）。

**配置服务器地址结构体：**

```cpp
struct sockaddr_in server_addr;
std::memset(&server_addr, 0, sizeof(server_addr));
server_addr.sin_family = AF_INET;
server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
server_addr.sin_port = htons(8888);
```

> `sockaddr_in` 是 IPv4 地址的标准结构体，`memset` 清零防止未初始化字段引起未定义行为。
> `inet_addr()` 将点分十进制字符串转为 32 位网络字节序整数。
> `htons()` 将 16 位主机字节序（小端）转为网络字节序（大端）。

**绑定地址并监听：**

```cpp
if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
    perror("[服务器] socket绑定失败");
    return -1;
}

if (listen(sockfd, SOMAXCONN) == -1) {
    perror("[服务器] 建立listen失败");
    return -1;
}

std::cout << "[服务器] 正在监听 127.0.0.1:8888" << std::endl;
```

> `bind()` 将 `(127.0.0.1, 8888)` 绑定到 sockfd。强制类型转换 `(struct sockaddr *)` 是因为
> `bind()` 接受通用地址结构体指针，`sockaddr_in` 是其 IPv4 特化。
> `listen()` 第二个参数 `SOMAXCONN` 设定 Accept Queue 的最大长度。

**接受客户端连接：**

```cpp
struct sockaddr_in client_addr;
socklen_t client_addr_len = sizeof(client_addr);
std::memset(&client_addr, 0, sizeof(client_addr));

int client_sockfd =
    accept(sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
if (client_sockfd == -1) {
    perror("[服务器] 接受连接失败");
    return -1;
}
```

> `accept()` 是**阻塞调用**：程序在此挂起，直到 Accept Queue 中有已完成握手的连接。
> 返回值是一个**新的 fd**（连接 socket），与监听 socket 完全独立。
> `client_addr` 由内核填充为客户端的 IP 和端口。

**打印客户端信息并关闭：**

```cpp
std::cout << "[服务器] 接收到来自此ip和端口的client连接："
          << inet_ntoa(client_addr.sin_addr)
          << ":" << ntohs(client_addr.sin_port) << std::endl;

close(client_sockfd);
close(sockfd);
```

> `inet_ntoa()` 将 32 位网络字节序 IP 转为点分十进制字符串。
> `ntohs()` 将 16 位网络字节序端口转为主机字节序。
> **必须先关闭连接 socket，再关闭监听 socket**，确保客户端收到正常的 FIN。

### 7.2 client.cpp

**创建 socket 并连接服务器：**

```cpp
int sockfd = socket(AF_INET, SOCK_STREAM, 0);
if (sockfd == -1) {
    perror("[客户端] socket创建失败");
    return -1;
}

struct sockaddr_in server_addr;
std::memset(&server_addr, 0, sizeof(server_addr));
server_addr.sin_family = AF_INET;
server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
server_addr.sin_port = htons(8888);

if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
    perror("[客户端] 与server建立连接时失败");
    return -1;
}

std::cout << "[客户端] 成功与server建立TCP连接" << std::endl;
close(sockfd);
```

> `connect()` 主动发起三次握手。内核自动分配客户端的临时端口（ephemeral port）。
> 连接成功后 `connect()` 返回 0，此时 TCP 连接已建立。
> `close(sockfd)` 发送 FIN，触发四次挥手。

---

### 7.3 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day01/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day01/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 8. 职责划分表（Day 01）

| 模块 | 职责 |
|------|------|
| `main()` (server) | 创建监听 socket → 绑定地址 → 监听 → 接受连接 → 打印信息 → 关闭 |
| `main()` (client) | 创建 socket → 连接服务器 → 打印信息 → 关闭 |

---

## 9. Day 01 的局限

1. **只能服务一个客户端**：`accept()` 只调用一次，第二个客户端无法被处理
2. **无数据收发**：连接建立后直接关闭，没有 `read()`/`write()`
3. **全阻塞**：`accept()` 期间主线程完全挂起
4. **无错误恢复**：任何 API 失败直接退出

→ Day 02 将加入 echo 循环（`read` + `write`）解决局限 2。

---

## 10. 对应 HISTORY

→ `HISTORY/day01/`
