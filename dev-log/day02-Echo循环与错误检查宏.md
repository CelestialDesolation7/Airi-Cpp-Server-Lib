# Day 02：Echo 循环 + errif 错误检查宏

> 在 Day 01 基础上加入 `read` / `write` 的 echo 循环，实现完整的数据收发。
> 抽取 `errif()` 工具函数统一错误处理。

---

## 1. 引言

### 1.1 问题上下文

Day 01 的服务器一旦 `accept()` 一次便退出，连最基本的 echo 也做不到。要让客户端持续与服务器对话，需要两件事：(1) 在连接 fd 上反复 `read`/`write` 直到对端关闭；(2) 把散落在每个系统调用后的 `if (rc < 0) { perror; exit; }` 收拢为可复用的工具，否则代码很快被错误检查淹没。

历史上这一步是所有教学型 socket 代码必经的"echo server"练习——它仍然是 `accept` 之后人类第一次能"看到"网络数据流动的最小程序。

### 1.2 动机

如果不把错误处理抽出来，每加一个系统调用就要复制 4 行模板代码。更糟的是不同位置写法不一致（有的 `perror`，有的 `printf`，有的忘了 `exit`），出错时定位困难。

抽出 `errif()` 不是为了"工程美观"，而是为后续每一个新模块（Buffer / Channel / Epoll）提供统一的错误现场，让真正的业务逻辑不被噪声掩盖。

### 1.3 现代解决方式

| 方案 | 出处 | 优势 | 局限 |
|------|------|------|------|
| 内联 `if(rc<0) perror;exit;` | 教科书 K&R 风格 | 显式、零依赖 | 重复、不一致、难维护 |
| `errif(cond, msg)` 工具函数 | 本项目 / muduo | 一处实现、统一行为 | 过于粗暴：直接 `exit()` |
| C++ 异常 (`throw std::system_error`) | C++11 | 错误传播跨函数边界 | 性能开销、需要 RAII 配套 |
| `std::expected<T,E>` / Rust `Result<T,E>` | C++23 / Rust | 强制处理错误、零开销 | 需较新编译器，调用方语法变重 |
| `errno` + 手动检查 + 日志 | Linux 系统编程惯用 | 与系统调用风格一致 | 错过检查会静默失败 |

### 1.4 本日方案概述

本日实现：
1. `server.cpp` 加入 `while(true) { read; print; write; }` 的 echo 循环。
2. `client.cpp` 加入 `while(true) { scanf; write; read; print; }` 交互循环。
3. 抽出 `util.h` / `util.cpp`，提供 `errif(cond, msg)`：条件为真时 `perror` + `exit(EXIT_FAILURE)`。
4. 全部裸 `if/perror/return` 替换为 `errif`，让代码主线只剩业务逻辑。

仍然是单连接、阻塞 IO，下一天才引入多路复用。

---
## 2. 本日文件变更总览

| 文件 | 操作 | 说明 |
|------|------|------|
| `server.cpp` | **修改** | 新增 echo while 循环（read → print → write），用 `errif` 替代 `if/perror/return` |
| `client.cpp` | **修改** | 新增交互循环（scanf → write → read → print），用 `errif` 替代手动错误检查 |
| `util.h` | **新建** | 声明 `errif(bool, const char*)` |
| `util.cpp` | **新建** | 实现 `errif()`：条件为真时 `perror` + `exit(EXIT_FAILURE)` |

---

## 3. 模块全景与所有权树（Day 02）

```
main() (server)
├── int sockfd                  ← 监听 socket，main 负责 close()
├── int client_sockfd           ← 连接 socket，main 负责 close()
└── char buf[1024]              ← 栈上读写缓冲区

main() (client)
├── int sockfd                  ← 连接 socket，main 负责 close()
└── char buf[1024]              ← 栈上读写缓冲区

工具层：
└── errif(bool, const char*)    ← 全局工具函数，不持有任何资源
```

与 Day 01 相比，唯一的新"模块"是 `errif` 工具函数。

---

## 4. 全流程调用链

**场景 A：echo 服务（服务器收到数据 → 回显）**

```
[主线程, server main()]

① socket / bind / listen / accept 同 Day 01（省略）
   └── 得到 client_sockfd

② while(true) {
       memset(buf, 0, 1024)
       read_bytes = read(client_sockfd, buf, sizeof(buf))
       └── 阻塞：内核将 client_sockfd 的接收缓冲区数据拷贝到用户空间 buf
       └── 返回值：
           > 0：实际读取字节数
           = 0：对端调用了 close()，TCP 收到 FIN
           < 0：错误（errno 已设置）

       if (read_bytes > 0)
           std::cout << buf            ← 打印收到的内容
           write(client_sockfd, buf, sizeof(buf))
           └── 将 buf 中 1024 字节写入内核发送缓冲区
           └── 注意：此处写 sizeof(buf) 而非 read_bytes，会多发零字节（Day 02 的小瑕疵）

       else if (read_bytes == 0)
           std::cout << "客户端断开"
           close(client_sockfd)
           break                       ← 退出循环

       else if (read_bytes == -1)
           close(client_sockfd)
           errif(true, "读取错误")
           └── perror("读取错误")      ← 打印 errno 对应的描述
           └── exit(EXIT_FAILURE)      ← 进程终止
   }
```

**场景 B：客户端交互循环**

```
[客户端进程, client main()]

① socket / connect 同 Day 01（省略）

② while(true) {
       bzero(buf, sizeof(buf))
       scanf("%s", buf)               ← 阻塞：等待用户终端输入一个单词
       write_bytes = write(sockfd, buf, sizeof(buf))
       └── 将 buf 写入内核发送缓冲区

       if (write_bytes == -1)
           break                      ← 连接已关闭

       bzero(buf, sizeof(buf))
       read_bytes = read(sockfd, buf, sizeof(buf))
       └── 阻塞：等待服务器回显数据

       if (read_bytes > 0)
           std::cout << buf           ← 打印回显内容
       else if (read_bytes == 0)
           break                      ← 服务器断开
       else
           errif(true, "读取失败")
   }
```

---

## 5. 代码逐段解析

### 5.1 util.h — 错误检查工具声明

```cpp
#ifndef UTIL_H
#define UTIL_H

void errif(bool, const char *);

#endif
```

> 标准 include guard 防止重复包含。`errif` 接受条件和错误消息两个参数。

### 5.2 util.cpp — errif 实现

```cpp
#include "util.h"
#include <cstdio>
#include <cstdlib>

void errif(bool condition, const char *message) {
    if (condition) {
        perror(message);
        perror("\n");
        exit(EXIT_FAILURE);
    }
}
```

> `errif` 是"error if"的缩写：条件为真时打印错误并终止进程。
> `perror()` 会在 message 后附加 errno 对应的系统错误描述（如 ": Connection refused"）。
> 第二个 `perror("\n")` 是多余的（只为换行），后续版本会改进。
> `exit(EXIT_FAILURE)` 直接终止进程，不做清理——适合早期开发阶段快速定位错误。

### 5.3 server.cpp — echo 循环核心

**errif 替代 if/perror/return：**

```cpp
int sockfd = socket(AF_INET, SOCK_STREAM, 0);
errif(sockfd == -1, "[服务器] socket创建失败");
```

> 将 Day 01 的三行 `if(..==-1){ perror(); return -1; }` 压缩为一行。
> 所有系统调用的错误检查都使用此模式。

**echo while 循环：**

```cpp
while (true) {
    char buf[1024];
    memset(&buf, 0, sizeof(buf));
    ssize_t read_bytes = read(client_sockfd, buf, sizeof(buf));
    if (read_bytes > 0) {
        std::cout << "[服务器] 接收到信息，来自客户端 fd " << client_sockfd
                  << ": " << buf << std::endl;
        write(client_sockfd, buf, sizeof(buf));
    } else if (read_bytes == 0) {
        std::cout << "[服务器] 客户端 fd " << client_sockfd << " 断开与本机的连接"
                  << std::endl;
        close(client_sockfd);
        break;
    } else if (read_bytes == -1) {
        close(client_sockfd);
        errif(true, "[服务器] socket 读取遭遇错误");
    }
}
```

> **`read()` 返回值三分支**是 TCP 编程的核心模式：
> - `> 0`：正常数据，执行业务逻辑（此处为回显）
> - `== 0`：对端关闭连接（收到 FIN），应清理并退出
> - `< 0`：系统错误，检查 errno
>
> **问题**：`write(client_sockfd, buf, sizeof(buf))` 写入固定 1024 字节而非 `read_bytes` 字节，
> 多发的零字节不影响 echo 功能但浪费带宽。

### 5.4 client.cpp — 交互循环

```cpp
while (true) {
    char buf[1024];
    bzero(buf, sizeof(buf));
    scanf("%s", buf);
    ssize_t write_bytes = write(sockfd, buf, sizeof(buf));

    if (write_bytes == -1) {
        std::cout << "[客户端] socket连接已经关闭，无法再写入数据\n";
        break;
    }

    bzero(buf, sizeof(buf));
    ssize_t read_bytes = read(sockfd, buf, sizeof(buf));
    if (read_bytes > 0) {
        std::cout << "[客户端] 收到来自服务器的数据：\n" << buf << std::endl;
    } else if (read_bytes == 0) {
        std::cout << "[客户端] 服务器 socket 断开连接\n";
        break;
    } else if (read_bytes == -1) {
        close(sockfd);
        errif(true, "[客户端] socket 读取失败");
    }
}
```

> `bzero()` 是 BSD 遗留函数，功能等同 `memset(buf, 0, sizeof(buf))`。
> `scanf("%s", buf)` 从终端读取一个空格分隔的单词。
> 客户端采用"一问一答"模式：先 write 再 read，严格交替。
> **问题**：`scanf("%s", buf)` 无长度限制，存在缓冲区溢出风险（后续版本会改进）。

---

### 5.5 CMakeLists.txt 与 README.md（构建与文档同步）

`HISTORY/day02/CMakeLists.txt` 是本日可独立编译的最小构建脚本：把当日新增 / 修改的 `.cpp` 全部加入 `add_executable`，`include_directories(include)` 让头文件路径与源码同步。
`HISTORY/day02/README.md` 记录当日快照的项目状态、文件结构与构建命令——既是当日工作的自检清单，也是后续翻阅时无需切换 git 历史就能看到“那一天项目长什么样”的入口。这两份文件不引入新的网络/系统行为，但让快照真正自洽可重现。

## 6. 职责划分表（Day 02）

| 模块 | 职责 |
|------|------|
| `errif()` | 条件错误检查 → perror → exit |
| `main()` (server) | socket 生命周期 + echo while 循环 |
| `main()` (client) | socket 生命周期 + 交互 while 循环 |

---

## 7. Day 02 的局限

1. **仍只能服务一个客户端**：accept 只调用一次
2. **全阻塞**：read 阻塞期间无法处理其他连接
3. **write 固定 1024 字节**：应写 read_bytes 字节
4. **scanf 缓冲区溢出风险**：无长度限制

→ Day 03 将引入 epoll 多路复用，突破"单客户端、全阻塞"限制。

---

## 8. 对应 HISTORY

→ `HISTORY/day02/`
