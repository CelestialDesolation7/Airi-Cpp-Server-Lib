# Day 03：IO 多路复用 + 非阻塞 IO

使用 epoll（Linux）/ kqueue（macOS）实现多客户端并发 echo 服务器。

## 文件结构

```
day03/
├── CMakeLists.txt
├── server.cpp      ← 多路复用 echo 服务器（边缘触发 + 非阻塞）
├── client.cpp      ← 交互客户端（fgets 安全输入）
├── util.h / util.cpp
└── README.md
```

## 构建

```bash
cmake -S . -B build
cmake --build build
```

## 运行

```bash
# 终端 1：启动服务器
./build/server

# 终端 2, 3, ...：分别启动客户端
./build/client
```

可以同时开多个客户端终端，服务器支持并发处理。输入文字回车后服务器回显。
