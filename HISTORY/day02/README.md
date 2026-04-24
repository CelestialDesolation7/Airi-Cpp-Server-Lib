# Day 02：Echo 循环 + errif 错误检查

TCP echo 服务器——可以接收客户端输入并原样返回。

## 文件结构

```
day02/
├── CMakeLists.txt
├── server.cpp      ← Echo 服务器：accept → while(read/write)
├── client.cpp      ← 交互客户端：while(scanf/write/read)
├── util.h          ← errif() 声明
├── util.cpp        ← errif() 实现
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

在客户端终端输入文字后回车，服务器会回显相同内容。客户端 Ctrl+C 退出。
