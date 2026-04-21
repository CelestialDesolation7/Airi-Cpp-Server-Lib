# Day 18 — Poller 策略模式拆分 + Airi-Cpp-Server-Lib 伞形头文件

## 核心变更
- **Poller 拆分为策略模式**：抽象基类 `Poller` + `EpollPoller`（Linux）+ `KqueuePoller`（macOS）+ `DefaultPoller.cpp` 工厂
- **新增 `Airi-Cpp-Server-Lib.h`** 伞形头文件
- **`Macros.h`** 移除自定义 `OS_LINUX/OS_MACOS`，改用编译器预定义 `__linux__` / `__APPLE__`
- **`Eventloop`** 改用 `Poller::newDefaultPoller(this)` 工厂创建

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
│   ├── Poller/
│   │   ├── Poller.h                抽象基类 + 工厂方法
│   │   ├── EpollPoller.h           Linux epoll 实现
│   │   └── KqueuePoller.h          macOS kqueue 实现
│   ├── Airi-Cpp-Server-Lib.h            伞形头文件（新增）
│   └── ...
├── common/
│   ├── Poller/
│   │   ├── DefaultPoller.cpp       工厂：按平台创建具体 Poller
│   │   ├── epoll/EpollPoller.cpp   epoll 实现
│   │   └── kqueue/KqueuePoller.cpp kqueue 实现
│   └── ...
└── test/                           ThreadPoolTest / StressTest
```
