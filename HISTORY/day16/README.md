# Day 16 — 异常处理、信号注册与 IO/业务分离

## 核心变更
- **新增 `Exception.h`**：自定义异常类继承 `std::runtime_error`，带 `ExceptionType` 枚举
- **新增 `SignalHandler.h`**：`Signal::signal()` 封装 POSIX signal，支持 lambda 注册
- **新增 `pine.h`**：伞形头文件，一行 include 全部核心模块
- **Connection IO/业务分离**：`doRead()`/`doWrite()` 处理底层 IO，`Business()` 串联读 + 回调
- **Server 双回调**：`onMessage()` 处理业务，`newConnect()` 处理连接事件

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
│   ├── Exception.h                 自定义异常（新增）
│   ├── SignalHandler.h             信号注册（新增）
│   ├── pine.h                      伞形头文件（新增）
│   ├── Connection.h                doRead/doWrite/Business 分离
│   ├── Server.h                    双回调：onMessage + newConnect
│   └── ...
├── common/                         实现文件
└── test/                           ThreadPoolTest / StressTest
```
