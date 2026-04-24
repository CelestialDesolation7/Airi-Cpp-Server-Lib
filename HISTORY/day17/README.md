# Day 17 — 跨平台 Poller：用 `#ifdef` 统一 epoll/kqueue

## 核心变更
- **删除 `Epoll.h/cpp`**，新增 `Poller.h/cpp`：同一文件内 `#ifdef OS_LINUX` epoll / `#ifdef OS_MACOS` kqueue 两套实现
- **`Macros.h`** 新增平台检测宏 `OS_LINUX` / `OS_MACOS`
- **`Channel`** 移除 `<sys/epoll.h>` 依赖，使用自定义标志 `READ_EVENT / WRITE_EVENT / ET`
- **`Eventloop`** 条件编译 eventfd（Linux）/ pipe（macOS）唤醒机制

## 构建

```bash
cmake -S . -B build
cmake --build build -j4
```

生成 `server`、`client`、`ThreadPoolTest`、`StressTest` 四个可执行文件。

## 文件结构

```
├── server.cpp / client.cpp         入口
├── include/
│   ├── Poller.h                    跨平台 IO 多路复用（新增）
│   ├── Channel.h                   平台中立标志位
│   ├── EventLoop.h                 条件编译唤醒机制
│   ├── Macros.h                    OS_LINUX / OS_MACOS 宏
│   └── ...
├── common/
│   ├── Poller.cpp                  epoll/kqueue 双实现（新增）
│   └── ...
└── test/                           ThreadPoolTest / StressTest
```
