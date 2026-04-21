# Day 04：面向对象封装

将裸系统调用封装为 Socket、InetAddress、Epoll 三个 C++ 类。

## 文件结构

```
day04/
├── CMakeLists.txt
├── server.cpp
├── client.cpp
├── include/
│   ├── Epoll.h        ← 跨平台 IO 多路复用封装
│   ├── InetAddress.h   ← sockaddr_in 封装
│   ├── Socket.h        ← RAII socket fd 封装
│   └── util.h
├── common/
│   ├── Epoll.cpp
│   ├── InetAddress.cpp
│   ├── Socket.cpp
│   └── util.cpp
└── README.md
```

## 构建

```bash
cmake -S . -B build
cmake --build build
```

## 运行

```bash
./build/server    # 终端 1
./build/client    # 终端 2
```

功能同 Day 03（多客户端 echo），但代码结构更清晰。
