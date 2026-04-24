# Day 01：原始 TCP 服务器

最小 TCP 服务器/客户端——纯 POSIX 系统调用，接受单个连接后关闭。

## 文件结构

```
day01/
├── CMakeLists.txt
├── server.cpp      ← TCP 服务器：socket → bind → listen → accept → close
├── client.cpp      ← TCP 客户端：socket → connect → close
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

# 终端 2：启动客户端
./build/client
```

服务器接受一个连接后自动退出。

## 功能

- 服务器在 `127.0.0.1:8888` 上监听
- 接受一个客户端连接，打印客户端 IP 和端口
- 无数据收发，连接建立后立即关闭
