# Day 14 — DISALLOW_COPY_AND_MOVE 宏与代码规范化

## 核心变更
- **新增 `Macros.h`**：定义 `DISALLOW_COPY`、`DISALLOW_MOVE`、`DISALLOW_COPY_AND_MOVE`、`ASSERT` 宏
- **EventLoop 唤醒机制**：新增 eventfd（Linux）/ pipe（macOS）唤醒通道 + `queueInLoop()` 跨线程任务投递
- **代码规范化**：所有成员变量加 `_` 后缀；`errif` → `ErrIf`
- **Connection 读写分离**：handleRead / handleWrite / send，业务通过 `onMessageCallback_` 注入

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
│   ├── Macros.h                    宏定义（新增）
│   ├── EventLoop.h                 新增 wakeup / queueInLoop
│   ├── Connection.h                新增 onMessageCallback_
│   └── ...
├── common/                         实现文件
└── test/                           ThreadPoolTest / StressTest
```
