# Day 15 — 连接状态机：Connection::State 枚举

## 核心变更
- **Connection::State 枚举**：`kInvalid` / `kConnected` / `kClosed` / `kFailed`，跟踪连接生命周期
- **新增 `close()` 方法**和 `onConnectCallback_`，连接关闭/出错时自动转移状态并通知 Server
- **server.cpp** 使用 `server->onConnect(lambda)` 回调，根据 `conn->getState()` 判断连接状态

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
│   ├── Connection.h                新增 State 枚举 / close() / onConnectCallback_
│   ├── Server.h                    新增 onConnect() setter
│   └── ...
├── common/                         实现文件
└── test/                           ThreadPoolTest / StressTest
```
