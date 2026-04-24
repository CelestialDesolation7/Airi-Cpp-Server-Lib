# Day 28 — 测试框架与 CI（策略可测试化 / 回压机制 / 连接上限 / 外部库示例）

> **主题**：Phase 3。把核心决策逻辑从执行路径里剥离成纯函数，使其可被无网络环境的单元测试覆盖；同时引入回压、连接上限、epoll_ctl 自愈、HTTP pipeline 与 GitHub Actions CI，并用 `app_example/` 演示如何作为独立子项目复用网络库。
> **基于**：Day 27（调试与性能优化）。

---

## 0. 阅读指南

本日变更跨越 7 个核心模块 + 1 个示例工程 + 1 套 CI 流水线，按"功能改进 → 业务场景 → 源码逐段 → 调用链追踪"的顺序展开。所有代码块都**原封不动地摘抄自源码**，并在标题中注明文件路径，方便结合 `git diff` 阅读。

| § | 主题 | 涉及文件 |
|---|------|---------|
| 1 | 引言 — 为什么 Day 28 必须做"可测试性" | — |
| 2 | 改进 A — Connection 回压机制 | `include/Connection.h`、`common/Connection.cpp` |
| 3 | 改进 B — TcpServer 连接上限 + Options | `include/TcpServer.h`、`common/TcpServer.cpp` |
| 4 | 改进 C — Socket 返回值语义重构 | `include/Socket.h`、`common/Socket.cpp` |
| 5 | 改进 D — EpollPoller 自愈重试 | `include/Poller/EpollPoller.h`、`common/Poller/epoll/EpollPoller.cpp` |
| 6 | 改进 E — HttpContext pipeline 解析 | `include/http/HttpContext.h`、`common/http/HttpContext.cpp`、`common/http/HttpServer.cpp` |
| 7 | 改进 F — HttpServer Options + 透传 | `include/http/HttpServer.h`、`common/http/HttpServer.cpp` |
| 8 | 改进 G — app_example 示例工程 | `app_example/CMakeLists.txt`、`app_example/src/http_server.cpp` |
| 9 | 改进 H — GitHub Actions CI + .gitignore | `.github/workflows/ci.yml`、`.gitignore`、`CMakeLists.txt` |
| 10 | 工程化收尾 — `util.cpp/h` 移除、test/ 目录化 | `test/server.cpp`、`test/client.cpp` |

---

## 1. 引言

### 1.1 问题上下文

经过前 27 天的开发，本库已经具备 Reactor 网络模型、HTTP 解析、路由分发、日志系统和性能优化。但有一类问题始终悬而未决：**核心决策逻辑都耦合在 I/O 执行路径里，没有一行能被单元测试单独验证**。

具体看几个真实痛点：

1. **OutputBuffer 无限增长**：客户端如果一直不读（典型的"慢消费者"，比如 `nc 127.0.0.1 8888` 之后摁住 Ctrl-S 暂停终端输出），服务器 `outputBuffer_` 会持续追加直到耗尽进程内存。Day 27 之前完全没有任何上限保护。
2. **连接洪峰**：SYN flood 或者短连接风暴下，`accept()` 不停成功，每条连接占一个 fd + 一个 `Connection` 对象，最终 fd 耗尽 / OOM。
3. **epoll_ctl 假性失败**：高并发下 fd 复用很常见。`fork+exec` 之类的操作可能让某个 fd 在你不知情的情况下被关闭并重新分配。`EPOLL_CTL_ADD` 可能拿到 `EEXIST`、`EPOLL_CTL_MOD` 可能拿到 `ENOENT`。Day 27 的代码看到这种错误就只打日志，下一次 epoll 循环依然按错误的状态推进。
4. **Socket 操作 void 返回**：`Socket::bind()` / `listen()` 是 `void`，失败时只打日志。`Acceptor` 构造时压根不知道监听是不是真的成功了——上线后才发现 8888 端口被占用。
5. **HTTP pipeline 半丢请求**：客户端在同一个 TCP 包里发了三个请求 `GET /a\r\n\r\nGET /b\r\n\r\nGET /c\r\n\r\n`。Day 27 的 `HttpContext::parse()` 只解析第一个，剩下两个在 buffer 里被 `retrieveAll()` 错误地当作"已消费"清空，客户端永远等不到 `/b`、`/c` 的响应。

### 1.2 这些问题的共同根因

它们的共同根因不是"算法不对"，而是**"决策"和"执行"被绑死在同一个函数里**。

举例：要验证"当 outputBuffer 恰好等于 hardLimit 时是否触发断连"，按 Day 27 的代码结构必须：
- 启动真实服务器
- 建立 TCP 连接
- 想办法让客户端不读
- 服务器拼命发数据直到缓冲达到精确阈值
- 检查连接是否真的被关闭

这个流程是脆弱的集成测试，无法稳定覆盖所有边界。

### 1.3 Day 28 的核心动作：提取"纯策略函数"

**纯策略函数 = static + 无成员变量访问 + 无 I/O + 确定性输出**。

把回压判断、连接上限判断、epoll_ctl 错误恢复判断这些"决策"都改造成纯函数后：

```cpp
// 测试只需要传入数字，不需要任何网络
TEST(BackpressureDecisionTest, HardLimitDecision) {
    Connection::BackpressureConfig cfg{};
    cfg.hardLimitBytes = 30;
    auto d = Connection::evaluateBackpressure(31, false, cfg);
    EXPECT_TRUE(d.shouldCloseConnection);
}
```

后面 8 节会逐个介绍这些纯函数怎么从原本的执行路径里被剥离出来。

### 1.4 Day 28 工程结构变化总览

```
HISTORY/day28/
├── .github/workflows/ci.yml   # 新增：Linux+macOS 矩阵 build + ctest
├── .gitignore                 # 新增：屏蔽 build/、*Config.cmake 等
├── CMakeLists.txt             # 新增：enable_testing + add_test 注册 8 个用例
├── include/
│   ├── Connection.h           # 新增：BackpressureConfig/Decision、纯函数
│   ├── TcpServer.h            # 新增：Options、shouldRejectNewConnection、normalizeIoThreadCount
│   ├── Socket.h               # 修改：bind/listen/connect/setnonblocking 改返回 bool
│   ├── Poller/EpollPoller.h   # 新增：shouldRetryWithMod/Add、shouldIgnoreCtlError
│   ├── http/HttpContext.h     # 新增：parse(... , consumedBytes) 重载
│   ├── http/HttpServer.h      # 新增：Options、setMaxConnections、onRequest 改返回 bool
│   ├── http/HttpRequest.h     # 新增：urlDecode、parseMultipart
│   └── http/HttpResponse.h    # 新增：setContentTypeByFilename、setRedirect
├── test/                      # 测试与 main 程序统一收编到此处
│   ├── server.cpp             # 旧 server.cpp 移入；改用 TcpServer::Options
│   ├── client.cpp             # 旧 client.cpp 移入
│   ├── BackpressureDecisionTest.cpp  # 新增：纯函数测试
│   ├── TcpServerPolicyTest.cpp       # 新增
│   ├── SocketPolicyTest.cpp          # 新增
│   ├── HttpContextTest.cpp           # 新增：含 pipeline 用例
│   └── EpollPolicyTest.cpp           # 新增（macOS 自动跳过）
└── app_example/               # 新增：作为独立子工程使用本库
    ├── CMakeLists.txt
    └── src/http_server.cpp
```

被删除的文件：根目录的 `util.cpp`、`util.h`（`errif()` 全部替换成 `Logger`）、根目录的 `server.cpp`、`client.cpp`（移入 `test/`）。

---

## 2. 改进 A — Connection 回压机制

### 2.1 业务场景

想象你正在跑一个文件下载服务，单个文件 2 GB，客户端是树莓派通过 4G 弱网下载。

服务器侧：
1. `read(file_fd)` 一次拿 64 KB
2. `conn->send(chunk)` 把这 64 KB 塞进 `outputBuffer_`
3. `doWrite()` 把 outputBuffer 里能写出去的部分 `write(socket_fd, ...)` 出去

弱网下，**步骤 2 速度 ≫ 步骤 3 速度**。`outputBuffer_` 会快速增长，可能到 1 GB 都没写出去多少。这就是经典的"快生产者 + 慢消费者"问题。

回压机制的核心思路：**当 outputBuffer 涨到一定阈值，临时关掉读事件，让上层先停下来；等 outputBuffer 排空到安全水位，再恢复读事件**。

### 2.2 三条水位线设计

来自 [HISTORY/day28/include/Connection.h](HISTORY/day28/include/Connection.h)：

```cpp
    // ── 回压配置（Phase 1-2）──────────────────────────────────────────────
    // low < high < hardLimit
    // - buffered > high      -> 暂停读事件，优先排空写缓冲
    // - buffered <= low      -> 恢复读事件
    // - buffered > hardLimit -> 触发保护性断连，防止内存失控
    struct BackpressureConfig {
      size_t lowWatermarkBytes{4 * 1024 * 1024};
      size_t highWatermarkBytes{16 * 1024 * 1024};
      size_t hardLimitBytes{64 * 1024 * 1024};
    };

    struct BackpressureDecision {
      bool shouldPauseRead{false};
      bool shouldResumeRead{false};
      bool shouldCloseConnection{false};
    };
```

**为什么 high ≠ low？** 这是控制理论里的**滞回（hysteresis）** 设计。如果暂停和恢复的阈值相等（比如都是 10 MB），缓冲在边界微小抖动（10.1 → 9.9 → 10.1）会让 `disableReading` / `enableReading` 来回切换，每次都是一个 `epoll_ctl` 系统调用，CPU 烧在切换上。non-symmetric 让回压一旦触发就要"真的排空一大段"才解除。

**为什么 hardLimit 还要再高一档？** high 只是"暂停信号"，真实情况下消息可能正好处于"已经被业务追加进 outputBuffer，但回压决策来不及生效"的瞬间。hardLimit 是**第二道防线**：超出就直接断连，防止恶意客户端利用回压窗口耗尽内存。

### 2.3 纯策略函数 — 决策与执行解耦

来自 [HISTORY/day28/common/Connection.cpp](HISTORY/day28/common/Connection.cpp)：

```cpp
bool Connection::isValidBackpressureConfig(const BackpressureConfig &cfg) {
    return cfg.lowWatermarkBytes > 0 &&
           cfg.lowWatermarkBytes < cfg.highWatermarkBytes &&
           cfg.highWatermarkBytes < cfg.hardLimitBytes;
}

Connection::BackpressureDecision Connection::evaluateBackpressure(
    size_t bufferedBytes,
    bool readPaused,
    const BackpressureConfig &cfg) {
    BackpressureDecision d;
    if (bufferedBytes > cfg.hardLimitBytes) {
        d.shouldCloseConnection = true;
        return d;
    }
    if (!readPaused && bufferedBytes > cfg.highWatermarkBytes)
        d.shouldPauseRead = true;
    if (readPaused && bufferedBytes <= cfg.lowWatermarkBytes)
        d.shouldResumeRead = true;
    return d;
}
```

注意函数签名：参数全是值类型，没有 `this`，没有 `loop_`、`channel_`、`sock_` 等成员。这就是"纯策略函数"——把 `bufferedBytes` 和 `readPaused` 作为输入显式注入，决策结果作为返回值显式输出。

### 2.4 把决策"嵌入"执行路径

光有纯函数还不够，需要在 `send()` 路径和 `doWrite()` 路径上各调用一次。

来自 [HISTORY/day28/common/Connection.cpp](HISTORY/day28/common/Connection.cpp)：

```cpp
void Connection::applyBackpressureAfterAppend() {
    if (state_ != State::kConnected)
        return;

    BackpressureDecision d =
        evaluateBackpressure(outputBuffer_.readableBytes(),
                             readPausedByBackpressure_,
                             backpressureConfig_);

    if (d.shouldCloseConnection) {
        state_ = State::kFailed;
        LOG_ERROR << "[连接] 输出缓冲超出硬上限，触发保护性断连，fd=" << sock_->getFd()
                  << " buffered=" << outputBuffer_.readableBytes()
                  << " hard=" << backpressureConfig_.hardLimitBytes;
        close();
        return;
    }

    if (d.shouldPauseRead) {
        channel_->disableReading();
        readPausedByBackpressure_ = true;
        LOG_WARN << "[连接] 触发回压，暂停读事件，fd=" << sock_->getFd()
                 << " buffered=" << outputBuffer_.readableBytes()
                 << " high=" << backpressureConfig_.highWatermarkBytes;
    }
}

void Connection::tryResumeReadAfterDrain() {
    if (state_ != State::kConnected)
        return;

    BackpressureDecision d =
        evaluateBackpressure(outputBuffer_.readableBytes(),
                             readPausedByBackpressure_,
                             backpressureConfig_);
    if (d.shouldResumeRead) {
        channel_->enableReading();
        readPausedByBackpressure_ = false;
        LOG_INFO << "[连接] 回压解除，恢复读事件，fd=" << sock_->getFd()
                 << " buffered=" << outputBuffer_.readableBytes()
                 << " low=" << backpressureConfig_.lowWatermarkBytes;
    }
}
```

`applyBackpressureAfterAppend` 在每次 `send()` 把数据追加到 outputBuffer 后调用；`tryResumeReadAfterDrain` 在 `doWrite()` 把数据成功写出后调用。

### 2.5 全流程追踪 —— 一个 2 GB 文件下载

配置：`low = 4 MB`、`high = 16 MB`、`hardLimit = 64 MB`。客户端是树莓派经 4G 弱网下载 `/huge.bin`，业务侧每次 `read(file_fd, 64KB)` 后立刻 `conn->send(chunk)`。

#### 时序总览

| 时刻 | 业务动作 | outputBuffer | readPaused | 决策 | 副作用 |
|------|---------|-------------|-----------|------|--------|
| T0 | 客户端 GET /huge.bin | 0 | false | — | 开始读文件 |
| T1 | `send(64KB)` × 256 次 | 16.0 MB | false | 不动 | 每次 append 后调用一次纯函数，结果都是空决策 |
| T2 | `send(64KB)` 第 257 次 | 16.06 MB | false | shouldPauseRead | `disableReading()` + `readPausedByBackpressure_=true` |
| T3 | `doWrite()` 多轮推进 | 16.06 → 4.1 MB | true | 都不触发 resume | 4.1 MB 仍 > low(4 MB)，继续等 |
| T4 | `doWrite()` 又写出 200 KB | 3.9 MB | true | shouldResumeRead | `enableReading()` + `readPausedByBackpressure_=false` |
| T5 | 业务继续 read 文件 → send | 缓慢回升 | false | 进入下一轮循环 | — |

下面用具体数字逐函数追踪 T1 → T2 → T3 → T4 这一轮回压。

#### 第一步：写数据时触发检查（send → applyBackpressureAfterAppend）

T2 时刻业务调 `conn->send(64KB)`，进入 [HISTORY/day28/common/Connection.cpp](HISTORY/day28/common/Connection.cpp) 的 `send()`：

```cpp
void Connection::send(const std::string &msg) {
    if (msg.empty())
        return;

    ssize_t nwrote = 0;
    size_t remaining = msg.size();
    bool faultError = false;

    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(sock_->getFd(), msg.data(), msg.size());
        ...
    }
```

初始状态 `outputBuffer_ = 16.0 MB ≠ 0`，所以**第一段"零拷贝直写"分支被跳过**，`nwrote` 保持 0，`remaining = 64KB`。然后进入第二段：

```cpp
    if (!faultError && remaining > 0) {
        const size_t projected = outputBuffer_.readableBytes() + remaining;
        if (projected > backpressureConfig_.hardLimitBytes) {
            state_ = State::kFailed;
            LOG_ERROR << "[连接] 发送前检测到缓冲即将超限，保护性断连，fd=" << sock_->getFd()
                      << " projected=" << projected
                      << " hard=" << backpressureConfig_.hardLimitBytes;
            close();
            return;
        }
        outputBuffer_.append(msg.data() + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
        applyBackpressureAfterAppend();
    }
}
```

这里有 **三个动作按序执行**：

1. **前置硬限制检查**：`projected = 16.0 MB + 64 KB = 16.06 MB`，远小于 `hardLimit = 64 MB`，通过。这一步是"追加之前的最后防线"——避免 `outputBuffer_` 真的把 64 MB 涨过去再处理。
2. **追加**：`outputBuffer_.append(msg.data(), 64 KB)`，`outputBuffer_` 现在 = 16.06 MB。
3. **追加后回压决策**：`applyBackpressureAfterAppend()`。

进入 `applyBackpressureAfterAppend()`：

```cpp
void Connection::applyBackpressureAfterAppend() {
    if (state_ != State::kConnected)
        return;

    BackpressureDecision d =
        evaluateBackpressure(outputBuffer_.readableBytes(),
                             readPausedByBackpressure_,
                             backpressureConfig_);
```

传入的实参：`bufferedBytes = 16.06 MB`、`readPaused = false`、`cfg = {4MB, 16MB, 64MB}`。

纯函数 `evaluateBackpressure` 的判断分支（来自 [HISTORY/day28/common/Connection.cpp](HISTORY/day28/common/Connection.cpp)）：

```cpp
    if (bufferedBytes > cfg.hardLimitBytes) {
        d.shouldCloseConnection = true;
        return d;
    }
    if (!readPaused && bufferedBytes > cfg.highWatermarkBytes)
        d.shouldPauseRead = true;
    if (readPaused && bufferedBytes <= cfg.lowWatermarkBytes)
        d.shouldResumeRead = true;
    return d;
```

- `16.06 MB > 64 MB`？否 → 跳过 close 分支。
- `!false && 16.06 MB > 16 MB`？是 → `d.shouldPauseRead = true`。
- `false && ...`？否 → `d.shouldResumeRead = false`。

回到 `applyBackpressureAfterAppend` 的执行段：

```cpp
    if (d.shouldPauseRead) {
        channel_->disableReading();
        readPausedByBackpressure_ = true;
        LOG_WARN << "[连接] 触发回压，暂停读事件，fd=" << sock_->getFd()
                 << " buffered=" << outputBuffer_.readableBytes()
                 << " high=" << backpressureConfig_.highWatermarkBytes;
    }
}
```

副作用：`channel_->disableReading()` 触发一次 `epoll_ctl(MOD)`，把 fd 的关注事件从 `EPOLLIN | EPOLLOUT` 改为只剩 `EPOLLOUT`；`readPausedByBackpressure_ = true` 记录"是被回压暂停的"（与 Day21 的"空闲超时暂停"区分开）。

此刻全局状态：

```
outputBuffer_              = 16.06 MB
readPausedByBackpressure_  = true
epoll 监听位               = EPOLLOUT （EPOLLIN 已移除）
业务侧                     = 客户端的新请求堆在 TCP 内核缓冲区里，应用层不读
```

**为什么暂停读可以缓解压力？** 因为如果继续读，业务回调（HTTP onRequest）会继续产生新的响应，新响应再调 `send()`，`outputBuffer_` 只会更大。暂停读之后，服务器专心把现有 16 MB 排空，不接新活。

#### 第二步：写完数据后尝试恢复（doWrite → tryResumeReadAfterDrain）

T3 时刻内核 socket 发送缓冲区有空间，epoll 通知 `EPOLLOUT` 就绪，进入 `doWrite()`：

```cpp
void Connection::doWrite() {
    int saveErrno = 0;
    ssize_t n = outputBuffer_.writeFd(sock_->getFd(), &saveErrno);
    if (n > 0) {
        outputBuffer_.retrieve(static_cast<size_t>(n));
        if (outputBuffer_.readableBytes() == 0) {
            channel_->disableWriting();
            ...
        }
        tryResumeReadAfterDrain();
        return;
    }
```

假设本轮 `writeFd` 写出了 12 MB，`outputBuffer_` 从 16.06 MB 降到 4.06 MB。然后调用 `tryResumeReadAfterDrain()`：

```cpp
void Connection::tryResumeReadAfterDrain() {
    if (state_ != State::kConnected)
        return;

    BackpressureDecision d =
        evaluateBackpressure(outputBuffer_.readableBytes(),
                             readPausedByBackpressure_,
                             backpressureConfig_);
    if (d.shouldResumeRead) {
        channel_->enableReading();
        readPausedByBackpressure_ = false;
        LOG_INFO << "[连接] 回压解除，恢复读事件，fd=" << sock_->getFd()
                 << " buffered=" << outputBuffer_.readableBytes()
                 << " low=" << backpressureConfig_.lowWatermarkBytes;
    }
}
```

传入实参：`bufferedBytes = 4.06 MB`、`readPaused = true`、`cfg = {4MB, 16MB, 64MB}`。

- `4.06 MB > 64 MB`？否。
- `!true && ...`？否（已经暂停了，不会再次暂停）。
- `true && 4.06 MB <= 4 MB`？**否**——4.06 MB 仍然高于 low。 → `shouldResumeRead = false`。

→ `tryResumeReadAfterDrain` 静默返回，本轮**不恢复**。这正是滞回设计想要的：缓冲只是从 16 MB 降到 4.06 MB，离 low 还差一点，先别恢复，免得下一轮 send 又冲高。

T4 时刻 `doWrite` 又写出 200 KB，`outputBuffer_` 降到 3.86 MB，再次进入 `tryResumeReadAfterDrain`：

- `true && 3.86 MB <= 4 MB`？**是** → `shouldResumeRead = true`。
- `channel_->enableReading()` 触发 `epoll_ctl(MOD)`，把 fd 的关注位重新加上 `EPOLLIN`。
- `readPausedByBackpressure_ = false`。

此刻全局状态：

```
outputBuffer_              = 3.86 MB （还有少量待发）
readPausedByBackpressure_  = false
epoll 监听位               = EPOLLIN | EPOLLOUT
业务侧                     = 又能接收客户端新请求；新一轮循环开始
```

#### 第三步：恶意客户端击穿 high → hardLimit 的预检查防线

上面追踪的是"16.06 MB → 暂停 → 排空 → 3.86 MB → 恢复"的健康循环。但如果客户端**完全不读数据**（恶意慢客户端，或 TCP 接收窗口被故意调到 1 字节），`doWrite` 写不出去，`outputBuffer_` 会持续增长。

这里 **send() 的预检查就是最后防线**。回到 `send()` 的关键片段：

```cpp
const size_t projected = outputBuffer_.readableBytes() + remaining;
if (projected > backpressureConfig_.hardLimitBytes) {
    state_ = State::kFailed;
    LOG_ERROR << "[连接] 发送前检测到缓冲即将超限，保护性断连，fd=" << sock_->getFd()
              << " projected=" << projected
              << " hard=" << backpressureConfig_.hardLimitBytes;
    close();
    return;
}
```

例如 `outputBuffer_ = 60 MB`、`remaining = 10 MB`，则 `projected = 70 MB > 64 MB`，立刻 `close()`。**追加都没发生**，`outputBuffer_` 永远不会真的越过 hardLimit。

`applyBackpressureAfterAppend` 内部还有一条对称的 `shouldCloseConnection` 分支（来自纯函数 `evaluateBackpressure` 的第一条 if），用于兜底"绕开 send() 预检查的非常规追加路径"，构成二重保险。

#### 状态机全景

```
                  ┌──────────── outputBuffer_ 大小 ────────────┐
                  │ 0 ─── low(4MB) ─── high(16MB) ─── hard(64MB)│
                  └──────────────────────────────────────────────┘
                          ↑ send().append              ↓ doWrite().retrieve

  ┌──────────────────┐  outputBuffer > 16MB          ┌─────────────────┐
  │ 状态:正常         │ ───────────────────────────►  │ 状态:回压中     │
  │ readPaused=false │  applyBackpressureAfterAppend │ readPaused=true │
  │ 监听 IN | OUT    │  → disableReading()           │ 监听 OUT only   │
  └──────────────────┘                               └─────────────────┘
           ▲                                                  │
           │  outputBuffer ≤ 4MB                              │
           │  tryResumeReadAfterDrain                         │
           │  → enableReading()                               │
           └──────────────────────────────────────────────────┘

  特殊路径：
    send() 内 projected > 64MB         → state_=kFailed → close()
    applyBackpressure 时 buffered>64MB → state_=kFailed → close()
```

#### 各函数职责一句话

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| `evaluateBackpressure` | `applyBackpressureAfterAppend` 与 `tryResumeReadAfterDrain` 内部 | **纯判断、无副作用**，告诉调用方"现在该不该 pause / resume / close" |
| `applyBackpressureAfterAppend` | `send()` 把数据追加到 outputBuffer 之后 | 根据决策暂停读事件或触发兜底断连 |
| `tryResumeReadAfterDrain` | `doWrite()` 每次成功写出之后 | 根据决策恢复读事件 |
| `send()` 里的 projected 检查 | `send()` 内、追加之前 | "追加前"的最后防线，超 hardLimit 直接断连 |

### 2.6 单元测试如何覆盖

来自 [HISTORY/day28/test/BackpressureDecisionTest.cpp](HISTORY/day28/test/BackpressureDecisionTest.cpp)：

```cpp
void testConfigValidation() {
    std::cout << "[BackpressureDecisionTest] 用例1：配置合法性校验\n";

    Connection::BackpressureConfig good{};
    check(Connection::isValidBackpressureConfig(good), "默认配置应合法");

    Connection::BackpressureConfig bad1{};
    bad1.lowWatermarkBytes = 0;
    check(!Connection::isValidBackpressureConfig(bad1), "low=0 应非法");
```

整个测试不依赖 socket、不需要 EventLoop、跑一次 < 1 秒。这就是"决策与执行解耦"的最大收益——**边界条件可被穷举验证**。

---

## 3. 改进 B — TcpServer 连接上限 + Options

### 3.1 业务场景

凌晨 3 点，监控告警："服务器进程被 OOM Killer 干掉"。复盘发现：某个爬虫脚本在十秒内打了 5 万个连接没关，每个连接占 ~8 KB（Connection 对象 + Buffer + Channel），瞬间 400 MB。

需要的能力：**配置最大并发连接数；超过就直接 `close(fd)`，连 Connection 对象都不要构造**。

### 3.2 Options 配置结构体 — 把全局常量集中化

来自 [HISTORY/day28/include/TcpServer.h](HISTORY/day28/include/TcpServer.h)：

```cpp
  public:
    // ── Day 28：服务器配置参数集（Phase 3）────────────────────────────
    // 之前版本通过环境变量 / 全局常量配置，难以在测试中固定。把所有可调参数
    // 收敛到 Options 后，应用层可以通过 `TcpServer(Options{...})` 一次性注入，
    // 单元测试也可以构造任意组合而不依赖外部状态。
    struct Options {
      std::string listenIp{"127.0.0.1"};
      uint16_t listenPort{8888};
      int ioThreads{0};            // <=0 表示按 hardware_concurrency 自动选择
      size_t maxConnections{10000}; // 最大并发连接数；超出即关闭新 fd
    };
```

### 3.3 两个纯策略函数

来自 [HISTORY/day28/include/TcpServer.h](HISTORY/day28/include/TcpServer.h)：

```cpp
    // ── Day 28：纯策略函数（Phase 3）────────────────────────────────
    // 这两个 static 函数零副作用、无 I/O，把"是否拒绝新连接""应起多少 IO
    // 线程"两个决策从 TcpServer 的执行路径中剥离出来。这样：
    //   1) handleNewConnection() 只需调用 shouldRejectNewConnection() 即可；
    //   2) 单元测试只需要传入数字即可枚举所有边界（参见
    //      test/TcpServerPolicyTest.cpp）。
    static bool shouldRejectNewConnection(size_t currentCount, size_t maxConnections);
    // configured<=0 时按 hardware_concurrency 取值；硬件返回 0（容器 / 沙箱）则保底 1。
    static int normalizeIoThreadCount(int configured, unsigned int hardwareCount);
```

### 3.4 实现细节

来自 [HISTORY/day28/common/TcpServer.cpp](HISTORY/day28/common/TcpServer.cpp)：

```cpp
int TcpServer::normalizeIoThreadCount(int configured, unsigned int hardwareCount) {
    if (configured > 0)
        return configured;
    if (hardwareCount == 0)
        return 1;
    return static_cast<int>(hardwareCount);
}
```

```cpp
bool TcpServer::shouldRejectNewConnection(size_t currentCount, size_t maxConnections) {
    return currentCount >= maxConnections;
}
```

### 3.5 嵌入执行路径

来自 [HISTORY/day28/common/TcpServer.cpp](HISTORY/day28/common/TcpServer.cpp)：

```cpp
void TcpServer::newConnection(int fd) {
    if (fd == -1) {
        LOG_WARN << "[TcpServer] 收到无效连接 fd=-1，已忽略";
        return;
    }

    if (shouldRejectNewConnection(connections_.size(), maxConnections_)) {
        LOG_WARN << "[TcpServer] 连接数达到上限，拒绝新连接 fd=" << fd
                 << " current=" << connections_.size()
                 << " max=" << maxConnections_;
        ::close(fd);
        return;
    }
```

注意 `::close(fd)` 在分配 `Connection` 对象之前——**这条路径上没有任何堆分配**，能扛住短时间的连接洪峰。

### 3.6 调用链全貌

```
TCP SYN ─→ kqueue/epoll: READ on listenfd
        ─→ Acceptor::acceptConnection() — accept 拿 fd
        ─→ TcpServer::newConnection(fd)
              ├── shouldRejectNewConnection? yes ─→ close(fd) 退出
              └── no ─→ nextLoop 选 sub-reactor ─→ make_unique<Connection>
```

---

## 4. 改进 C — Socket 返回值语义重构

### 4.1 业务场景

启动服务器，控制台一片绿，但 `curl http://127.0.0.1:8888/` 拒绝连接。翻日志才发现："errif: bind failed"——8888 被另一个进程占了，但 `Socket::bind()` 是 void，调用方 `Acceptor` 完全不知情，照常 `listen` 照常 epoll，只是永远没有 accept 事件来。

这种"沉默失败"在生产环境是大忌。

### 4.2 接口语义重构

来自 [HISTORY/day28/include/Socket.h](HISTORY/day28/include/Socket.h)：

```cpp
    bool isValid() const { return fd_ != -1; }

    // ── Day 28：返回值语义重构（Phase 3）──────────────────────────────
    // 之前 bind/listen/connect/setnonblocking 都是 void，失败仅打日志。
    // 改成 bool 后：
    //   * Acceptor::Acceptor() 可以根据返回值放弃监听并抛错；
    //   * test/SocketPolicyTest.cpp 可以断言"绑定到 0 端口""对已用端口
    //     再次 bind"等异常路径的精确返回值，而不是只能看 stderr。
    bool bind(InetAddress *addr);
    bool listen();

    // 返回新连接 fd；失败时返回 -1（由上层根据 errno 决定是否忽略或记录）
    int accept(InetAddress *addr);

    // 在且仅在客户端 Socket 调用
    bool connect(InetAddress *addr);

    int getFd();

    bool setnonblocking();
```

### 4.3 调用方升级 — Acceptor 现在能感知失败

来自 [HISTORY/day28/common/Acceptor.cpp](HISTORY/day28/common/Acceptor.cpp)：

```cpp
Acceptor::Acceptor(Eventloop *_loop, const char *ip, uint16_t port) : loop_(_loop) {
    sock_ = std::make_unique<Socket>();
    if (!sock_->setnonblocking()) {
        LOG_ERROR << "[Acceptor] setnonblocking 失败，listen socket 不可用";
        return;
    }

    addr_ = std::make_unique<InetAddress>(ip, port);
    if (!sock_->bind(addr_.get())) {
        LOG_ERROR << "[Acceptor] bind 失败 " << ip << ":" << port;
        return;
    }
    if (!sock_->listen()) {
        LOG_ERROR << "[Acceptor] listen 失败 " << ip << ":" << port;
        return;
    }
```

如果中途任何一步失败，后续 `Channel` 注册不会发生，`isValid()` 返回 false，上层可以选择 `exit(1)`。

### 4.4 单元测试

来自 [HISTORY/day28/test/SocketPolicyTest.cpp](HISTORY/day28/test/SocketPolicyTest.cpp)：

测试构造 Socket → bind 到 0 端口（让内核自动选）→ 验证 `bind()` 返回 true；再构造一个 Socket 试图 bind 到上一个 Socket 已占用的端口，验证返回 false。**完整路径不到 30 行 C++，不依赖 EventLoop**。

---

## 5. 改进 D — EpollPoller 自愈重试

### 5.1 业务场景

某次压测发现日志里大量 `epoll_ctl ADD failed: File exists (EEXIST)`，QPS 抖动严重。事后复盘：有个第三方 SDK 在子线程里 fork+close，把 fd 编号回收又分配，导致 Channel 以为自己是新 fd 调用 `EPOLL_CTL_ADD`，但内核认为这个 fd 早就在 epoll 里。

这种情况下"打日志了事"不够——读事件根本注册不上去，整个连接卡死。需要：**自动判断错误类型，把 ADD 改 MOD、MOD 改 ADD 重试一次**。

### 5.2 三个纯策略函数

来自 [HISTORY/day28/include/Poller/EpollPoller.h](HISTORY/day28/include/Poller/EpollPoller.h)：

```cpp
    // ── Day 28：epoll_ctl 自愈重试策略（Phase 3）──────────────────────
    // 真实环境中 fd 可能被 dup/close 等操作复用，导致：
    //   * EPOLL_CTL_ADD 返回 EEXIST：fd 已注册，应改用 EPOLL_CTL_MOD
    //   * EPOLL_CTL_MOD 返回 ENOENT：fd 未注册，应改用 EPOLL_CTL_ADD
    //   * EPOLL_CTL_DEL 返回 ENOENT/EBADF：fd 已经被对端清理，可静默忽略
    // 把这些判断从 updateChannel/deleteChannel 中提取为纯函数后，单元测试
    // 不再需要构造 epoll fd，仅需传入 op + errno 即可枚举全部分支。
    static bool shouldRetryWithMod(int op, int err);   // ADD 失败 → 是否改 MOD 重试
    static bool shouldRetryWithAdd(int op, int err);   // MOD 失败 → 是否改 ADD 重试
    static bool shouldIgnoreCtlError(int op, int err); // DEL 等可接受的失败
```

### 5.3 实现 — 三行函数

来自 [HISTORY/day28/common/Poller/epoll/EpollPoller.cpp](HISTORY/day28/common/Poller/epoll/EpollPoller.cpp)：

```cpp
bool EpollPoller::shouldRetryWithMod(int op, int err) {
    return op == EPOLL_CTL_ADD && err == EEXIST;
}

bool EpollPoller::shouldRetryWithAdd(int op, int err) {
    return op == EPOLL_CTL_MOD && err == ENOENT;
}

bool EpollPoller::shouldIgnoreCtlError(int op, int err) {
    return op == EPOLL_CTL_DEL && (err == ENOENT || err == EBADF);
}
```

### 5.4 嵌入执行路径

来自 [HISTORY/day28/common/Poller/epoll/EpollPoller.cpp](HISTORY/day28/common/Poller/epoll/EpollPoller.cpp)：

```cpp
    if (runCtl(op) == 0) {
        channel->setInEpoll(op != EPOLL_CTL_DEL);
        return;
    }

    const int firstErr = errno;

    if (shouldRetryWithMod(op, firstErr)) {
        if (runCtl(EPOLL_CTL_MOD) == 0) {
            channel->setInEpoll(true);
            LOG_WARN << "[EpollPoller] ADD 命中 EEXIST，已自动切换 MOD，fd=" << fd;
            return;
        }
        const int retryErr = errno;
        LOG_ERROR << "[EpollPoller] epoll_ctl ADD->MOD 重试失败，fd=" << fd
                  << " 首错=" << strerror(firstErr) << "(" << firstErr << ")"
                  << " 重试错=" << strerror(retryErr) << "(" << retryErr << ")";
        return;
    }

    if (shouldRetryWithAdd(op, firstErr)) {
        if (runCtl(EPOLL_CTL_ADD) == 0) {
            channel->setInEpoll(true);
            LOG_WARN << "[EpollPoller] MOD 命中 ENOENT，已自动切换 ADD，fd=" << fd;
            return;
        }
```

### 5.5 全流程追踪 — fd 复用引起的 EEXIST

业务场景：服务器跑 7 天，期间不断有 `Connection` 析构和新建。某次 `epoll_ctl(ADD, fd=42)` 突然返回 `-1`，`errno = EEXIST`。这是个真实可复现的内核行为：DEL 操作在 epoll 内部并非完全同步，且 fd 一被 close 就会立刻被新 socket 复用，新旧 Channel 撞车。

#### 时序总览

| 时刻 | 事件 | epoll 内部 fd=42 | Channel 状态 | 代码动作 |
|------|------|----------------|-------------|---------|
| T0 | Connection A 注册 fd=42 | 在 | `inEpoll=true`、`listenEvents=READ` | `epoll_ctl(ADD)` 成功 |
| T1 | A 析构 → `removeChannel(A)` | 应该不在 | A 即将销毁 | `epoll_ctl(DEL)` 调用，但内核回执前异步进行 |
| T2 | A 的 fd 被 `::close()` | 内核延迟清理 | — | — |
| T3 | 新 socket `accept()` 拿到 fd=42 | 仍残留 | — | 内核 fd 表把 42 立刻分配给新连接 |
| T4 | Connection B 用 fd=42，调 `updateChannel` | 仍残留 | `inEpoll=false`、`listenEvents=READ` | `op = EPOLL_CTL_ADD` |
| T5 | `runCtl(ADD)` 返回 -1 | 仍残留 | — | `firstErr = EEXIST` |
| T6 | `shouldRetryWithMod(ADD, EEXIST)` = true | 仍残留 | — | 进入 MOD 自愈分支 |
| T7 | `runCtl(MOD)` 返回 0 | 状态被覆盖为 B 的事件位 | `inEpoll=true` | `LOG_WARN` 记录这次自愈，函数返回 |

下面用代码追踪 T4 → T7 这段自愈过程。

#### 第一步：updateChannel 选 op

T4 时刻业务侧 Channel B 调 `EpollPoller::updateChannel(channel_B)`。来自 [HISTORY/day28/common/Poller/epoll/EpollPoller.cpp](HISTORY/day28/common/Poller/epoll/EpollPoller.cpp)：

```cpp
    int op = channel->getInEpoll() ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (channel->getInEpoll() && channel->getListenEvents() == 0)
        op = EPOLL_CTL_DEL;

    auto runCtl = [&](int ctlOp) {
        return epoll_ctl(epollFd_, ctlOp, fd,
                         ctlOp == EPOLL_CTL_DEL ? nullptr : &ev);
    };

    if (runCtl(op) == 0) {
        channel->setInEpoll(op != EPOLL_CTL_DEL);
        return;
    }

    const int firstErr = errno;
```

`channel_B->getInEpoll() == false`（因为 B 是新对象，从未注册过），→ `op = EPOLL_CTL_ADD`。`runCtl(EPOLL_CTL_ADD)` 进 syscall。内核发现 fd=42 仍在 epoll 红黑树里（A 的残留），返回 -1，`errno = EEXIST`。代码读出 `firstErr = EEXIST`。

#### 第二步：纯函数判断要不要重试

```cpp
    if (shouldRetryWithMod(op, firstErr)) {
        if (runCtl(EPOLL_CTL_MOD) == 0) {
            channel->setInEpoll(true);
            LOG_WARN << "[EpollPoller] ADD 命中 EEXIST，已自动切换 MOD，fd=" << fd;
            return;
        }
```

调用纯函数 `shouldRetryWithMod(EPOLL_CTL_ADD, EEXIST)`，定义本身只有一行：

```cpp
bool EpollPoller::shouldRetryWithMod(int op, int err) {
    return op == EPOLL_CTL_ADD && err == EEXIST;
}
```

传入实参 `(EPOLL_CTL_ADD, EEXIST)` → 返回 `true`。

#### 第三步：重试以 MOD 完成自愈

进入 if 体，`runCtl(EPOLL_CTL_MOD)` 第二次发 syscall。内核此时把 fd=42 在 epoll 中的事件位重写为 B 的 `listenEvents`，返回 0。代码：

- `channel_B->setInEpoll(true)` 把内部状态对齐到"已注册"。
- `LOG_WARN` 记录一行运维可读的日志：`[EpollPoller] ADD 命中 EEXIST，已自动切换 MOD，fd=42`。
- `return`，函数结束。

T7 之后全局状态：

```
epoll 内部 fd=42 事件位 = channel_B 的 listenEvents
channel_B->inEpoll       = true
channel_A                = 已析构（其 inEpoll 状态已无意义）
日志                     = 一条 WARN，便于事后审计内核竞态频次
```

#### 第四步：另两条对称分支

下方还有对称的两段，处理 "MOD 命中 ENOENT" 与 "DEL 命中 ENOENT/EBADF"：

```cpp
    if (shouldRetryWithAdd(op, firstErr)) {
        if (runCtl(EPOLL_CTL_ADD) == 0) {
            channel->setInEpoll(true);
            LOG_WARN << "[EpollPoller] MOD 命中 ENOENT，已自动切换 ADD，fd=" << fd;
            return;
        }
```

```cpp
    if (shouldIgnoreCtlError(op, firstErr)) {
        channel->setInEpoll(false);
        LOG_WARN << "[EpollPoller] 忽略可恢复错误 op=" << epollOpName(op)
                 << " fd=" << fd << " 错误=" << strerror(firstErr)
                 << "(" << firstErr << ")";
        return;
    }
```

两条纯函数定义同样只有一行：

```cpp
bool EpollPoller::shouldRetryWithAdd(int op, int err) {
    return op == EPOLL_CTL_MOD && err == ENOENT;
}

bool EpollPoller::shouldIgnoreCtlError(int op, int err) {
    return op == EPOLL_CTL_DEL && (err == ENOENT || err == EBADF);
}
```

所有真正的自愈逻辑都集中在这三个 `if`，每个 `if` 的判定都是一次纯函数调用。

#### 各函数职责一句话

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| `shouldRetryWithMod` | `updateChannel` 第一次 syscall 失败之后 | 纯判断："ADD + EEXIST" 时返回 true，意为应改 MOD 重试 |
| `shouldRetryWithAdd` | 同上 | 纯判断："MOD + ENOENT" 时返回 true，意为应改 ADD 重试 |
| `shouldIgnoreCtlError` | 同上、且 `removeChannel` 内 | 纯判断："DEL + ENOENT/EBADF" 时返回 true，意为可静默吞掉 |
| `runCtl` (lambda) | 仅 `updateChannel` 内部 | 真实 syscall，唯一有副作用的入口 |
| `updateChannel` | `Channel::update()` 触发 | 编排：先选 op、发 syscall、按错误码请教三个纯函数、按结论二次 syscall 或忽略 |

**单元测试只需穷举那三个纯函数的输入组合**——例如 `shouldRetryWithMod(EPOLL_CTL_ADD, EEXIST)` 期望 true、`shouldRetryWithMod(EPOLL_CTL_MOD, EEXIST)` 期望 false——不需要真造一个 fd 复用场景。这就是把决策剥离出来的价值。

### 5.6 跨平台兼容 — macOS 为何能编译通过

`shouldRetryWithMod` 的实现里用到了 `EPOLL_CTL_ADD` / `EEXIST`，这些在 macOS 上需要 `<sys/epoll.h>`，而 macOS 没有这个头。我们的 CMake 已经按平台分发：

```cmake
if(APPLE)
    list(APPEND POLLER_SRCS common/Poller/kqueue/KqueuePoller.cpp)
else()
    list(APPEND POLLER_SRCS common/Poller/epoll/EpollPoller.cpp)
endif()
```

所以 macOS 上根本不会编 `EpollPoller.cpp`。对应 `EpollPolicyTest.cpp` 在 macOS 上跑时会自动 `return 0`（条件编译跳过）。

---

## 6. 改进 E — HttpContext pipeline 解析

### 6.1 业务场景

某次集成测试发现：`ab -n 100 -c 1 -k http://127.0.0.1:8888/` 中只有约 50 条请求拿到响应，剩下都超时。`ab` 的 `-k` 模式启用 keep-alive 并大量复用 TCP 连接，会触发 HTTP/1.1 pipelining——同一个 TCP 段里塞多个请求。

Day 27 的 `HttpContext::parse()` 一次只解一个请求，剩下字节被 `Buffer::retrieveAll()` 错误地清空。

### 6.2 接口设计 — consumed 出参

来自 [HISTORY/day28/include/http/HttpContext.h](HISTORY/day28/include/http/HttpContext.h)：

```cpp
    // 将 data[0..len) 喂入状态机，返回 false 表示报文格式非法。
    // 可多次调用；每次调用都从上次中断的状态继续。
    //
    // ── Day 28：HTTP pipeline 支持（Phase 3）────────────────────────
    // consumedBytes 输出本次实际消费的字节数。当客户端在同一 TCP 包中发
    // 送多个请求（HTTP/1.1 pipelining）时，HttpServer::onMessage 需要
    // 知道：第一条请求结束后，buffer 里还剩多少字节属于第二条。
    // Day27 之前 parse() 一次只能解析一条请求，剩余字节会被错误地丢弃。
    bool parse(const char *data, int len, int *consumedBytes);

    // 兼容旧调用方：若不关心消费字节，可使用该重载。
    bool parse(const char *data, int len) {
      int ignored = 0;
      return parse(data, len, &ignored);
    }
```

### 6.3 实现要点 — 把指针进度回写

来自 [HISTORY/day28/common/http/HttpContext.cpp](HISTORY/day28/common/http/HttpContext.cpp)：

```cpp
bool HttpContext::parse(const char *data, int len, int *consumedBytes) {
    if (consumedBytes)
        *consumedBytes = 0;
```

```cpp
    if (consumedBytes)
        *consumedBytes = static_cast<int>(p - data);
```

`p` 是状态机内部维护的当前游标，每解析完一个 token（method、url、header、body）就前进若干字节。函数末尾把"游标 - 起点"作为消费长度返回。

### 6.4 上层循环 — onMessage 的循环化

来自 [HISTORY/day28/common/http/HttpServer.cpp](HISTORY/day28/common/http/HttpServer.cpp)：

```cpp
    Buffer *buf = conn->getInputBuffer();
    // 关键修复：同一个 TCP 包中可能包含多个 HTTP 请求（pipeline / 粘包）。
    // 逐轮 parse + 按"实际消费字节"retrieve，避免把后续请求误清空。
    while (buf->readableBytes() > 0) {
        int consumed = 0;
        if (!ctx->parse(buf->peek(), static_cast<int>(buf->readableBytes()), &consumed)) {
            LOG_WARN << "[HttpServer] 非法请求，fd=" << conn->getSocket()->getFd();
            conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
            conn->close();
            return;
        }

        if (consumed > 0) {
            buf->retrieve(static_cast<size_t>(consumed));
        } else {
            // 理论上 len>0 时应至少消费 1 字节。该保护用于规避异常输入导致的死循环。
            LOG_WARN << "[HttpServer] 解析未前进，等待更多数据，fd="
                     << conn->getSocket()->getFd();
            break;
        }

        // 只解析到半包时先退出，等待下一个 read 事件继续。
        if (!ctx->isComplete())
            break;

        if (!onRequest(conn, ctx->request()))
            return;
        ctx->reset();
    }
```

三个关键护栏：
1. `if (consumed > 0)` — 防止解析器有 bug 导致死循环（`while` 永远转下去）。
2. `if (!ctx->isComplete()) break` — 半包就退出，等下次 `read` 事件继续。
3. `ctx->reset()` — 解析完一条立刻清空状态机，给下一条让位。

### 6.5 全流程追踪 — pipelining

业务场景：`ab -n 100 -c 1 -k http://127.0.0.1:8888/` 用 keep-alive 模式压测，OS 把多次写聚合到一个 TCP 段发出，服务器一次 `read()` 拿到多个完整请求。

假设客户端一次性发出（注意：真实 HTTP 头还有 `Host` 等字段，这里为了让数字直观，简化为最短形式）：

```
GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n
```

两条请求各 19 字节，共 38 字节，全部进入 `conn->getInputBuffer()`。

#### 时序总览

| 轮次 | 进入 while 时 buf | parse 行为 | consumed | isComplete | 后续动作 |
|------|------------------|-----------|----------|-----------|---------|
| 1 | `GET /a...GET /b...`（38 B） | 解出 /a 完整请求 | 19 | true | `retrieve(19)` → `onRequest(/a)` 返回 true → `ctx->reset()` |
| 2 | `GET /b...`（19 B） | 解出 /b 完整请求 | 19 | true | `retrieve(19)` → `onRequest(/b)` 返回 true → `ctx->reset()` |
| 3 | `""`（0 B） | while 守卫直接 false | — | — | 跳出 while，函数返回 |

下面跟着代码逐轮走。

#### 第一步：进入 onMessage 循环

来自 [HISTORY/day28/common/http/HttpServer.cpp](HISTORY/day28/common/http/HttpServer.cpp)：

```cpp
    Buffer *buf = conn->getInputBuffer();
    // 关键修复：同一个 TCP 包中可能包含多个 HTTP 请求（pipeline / 粘包）。
    // 逐轮 parse + 按"实际消费字节"retrieve，避免把后续请求误清空。
    while (buf->readableBytes() > 0) {
        int consumed = 0;
        if (!ctx->parse(buf->peek(), static_cast<int>(buf->readableBytes()), &consumed)) {
            LOG_WARN << "[HttpServer] 非法请求，fd=" << conn->getSocket()->getFd();
            conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
            conn->close();
            return;
        }
```

第 1 轮入口：`buf->readableBytes() = 38`，进入 while。`consumed = 0`。`parse(peek, 38, &consumed)` 把状态机推进。

#### 第二步：parse 内部记录消费长度

来自 [HISTORY/day28/common/http/HttpContext.cpp](HISTORY/day28/common/http/HttpContext.cpp)：

```cpp
bool HttpContext::parse(const char *data, int len, int *consumedBytes) {
    if (consumedBytes)
        *consumedBytes = 0;
```

```cpp
    if (consumedBytes)
        *consumedBytes = static_cast<int>(p - data);
```

`p` 是状态机的内部游标，每解出一个 token（method / url / version / 空行）就前进。当 /a 的整个请求被解析完（直到第二个 `\r\n`），`p - data = 19`。函数返回 true，`consumed = 19`。注意此时 `p` 并没有继续往下扫描 /b——状态机一旦进入 `gotAll` 就立刻返回，把后续字节留给下一轮。

#### 第三步：按消费长度 retrieve、调业务、reset

回到 onMessage：

```cpp
        if (consumed > 0) {
            buf->retrieve(static_cast<size_t>(consumed));
        } else {
            // 理论上 len>0 时应至少消费 1 字节。该保护用于规避异常输入导致的死循环。
            LOG_WARN << "[HttpServer] 解析未前进，等待更多数据,fd="
                     << conn->getSocket()->getFd();
            break;
        }

        // 只解析到半包时先退出，等待下一个 read 事件继续。
        if (!ctx->isComplete())
            break;

        if (!onRequest(conn, ctx->request()))
            return;
        ctx->reset();
    }
```

第 1 轮的执行：

1. `consumed = 19 > 0` → `buf->retrieve(19)`，`buf` 内剩下 19 字节即 /b 请求原文。
2. `ctx->isComplete() == true` → 不 break。
3. `onRequest(conn, /a)`：业务回调发出 /a 的响应；若返回 false 表示连接已被关掉立即 return。这里假设是普通 GET 200，返回 true。
4. `ctx->reset()` 把状态机清空，给下一条让位。回到 while 顶部。

第 2 轮入口：`buf->readableBytes() = 19`，进入 while。`parse(peek, 19, &consumed)` 把 /b 解完，`consumed = 19`，`isComplete = true`。同样 `retrieve(19)` → `onRequest(/b)` → `reset()`。

第 3 轮入口：`buf->readableBytes() = 0`，**while 守卫直接 false**，跳出。函数返回。

#### 第四步：三条护栏防止死循环 / 半包丢失 / 关闭后续跑

这段循环里有三处看起来像样板、但缺一不可的保护：

1. **`if (consumed > 0) ... else break`**：防止 parser 出 bug 时返回 true 却没前进，触发 `while` 死循环。在第二个 break 之前必须先 break 出去等更多数据。
2. **`if (!ctx->isComplete()) break`**：客户端可能只发了 "GET /a HTTP/1.1\r\nHost: "，半包就要等下一次 `read()` 事件，不能继续推进 onRequest。
3. **`if (!onRequest(...)) return`**：onRequest 内部如果决定 `conn->close()`（HTTP/1.0 默认关闭、或 `Connection: close`），`conn` 已进入关闭流程，绝不能再 `ctx->reset()` 然后跑下一轮——后续 send 会失败、状态混乱。直接 return 退出整个 onMessage。

#### 各函数职责一句话

| 函数 | 调用时机 | 职责 |
|------|---------|------|
| `HttpContext::parse(data, len, &consumed)` | onMessage while 每轮 | 把状态机推进，**输出本轮消费字节数**，让上层精确 retrieve |
| `HttpContext::isComplete()` | parse 返回 true 之后 | 判断是不是"整条请求结束"（区分半包和完整包） |
| `HttpContext::reset()` | onRequest 返回 true 之后 | 清空状态机，给同一连接的下一条请求让位 |
| `Buffer::retrieve(n)` | parse 后 | 按 parse 实际消费长度切走前 n 字节，剩余字节给下一轮 |
| `HttpServer::onRequest` | parse + isComplete 都 ok 后 | 业务回调；返回 false 表示连接已关，调用方必须立刻 return |

### 6.6 单元测试

来自 [HISTORY/day28/test/HttpContextTest.cpp](HISTORY/day28/test/HttpContextTest.cpp) — 测试三个场景：
1. 单条完整请求（baseline）
2. pipelining：两条请求拼成一个字符串，调用两次 parse 都能成功
3. 分段 body：先送 header，再送一半 body，最后送另一半，期间 `isComplete()` 始终为 false 直到全部 body 到达

---

## 7. 改进 F — HttpServer Options + 透传

### 7.1 接口

来自 [HISTORY/day28/include/http/HttpServer.h](HISTORY/day28/include/http/HttpServer.h)：

```cpp
    // ── Day 28：HttpServer 配置参数集（Phase 3）───────────────────────
    // 与 TcpServer::Options 同样的设计动机：把分散的开关集中到一个结构
    // 体里，方便测试用例 / app_example 一行注入完整配置。
    struct Options {
      TcpServer::Options tcp;       // 透传给底层 TcpServer 的网络参数
      bool autoClose{false};        // 是否启用空闲超时自动关闭
      double idleTimeoutSec{60.0};  // autoClose=true 时的超时阈值
    };
```

```cpp
    // 最大连接数保护（透传到 TcpServer）
    void setMaxConnections(size_t maxConnections) {
      server_->setMaxConnections(maxConnections);
    }

    // ── Day21：空闲连接自动关闭 ──────────────────────────────────────────────
    // ...

    // 返回 true 表示连接可继续处理后续请求；false 表示已进入关闭流程。
    bool onRequest(Connection *conn, const HttpRequest &req);
```

### 7.2 onRequest 改返回 bool 的原因

回看 § 6.4 的 onMessage 循环：

```cpp
        if (!onRequest(conn, ctx->request()))
            return;
        ctx->reset();
```

如果 onRequest 内部决定 `conn->close()`（比如 HTTP/1.0 默认关闭），后续再去 `ctx->reset()` 并继续 while 是危险的——`conn` 已经在关闭流程里。所以 onRequest 用返回值告诉调用方："我已经关了，你赶紧退"。

来自 [HISTORY/day28/common/http/HttpServer.cpp](HISTORY/day28/common/http/HttpServer.cpp)：

```cpp
    if (resp.closeConnection()) {
        conn->close();
        return false;
    }
    return true;
```

---

## 8. 改进 G — app_example 示例工程

### 8.1 设计意图

到 Day 28 为止，库已经足够稳定，可以"对外宣布有 API 了"。需要一个示例工程演示：
- 如何作为独立 CMake 子项目复用本库
- 如何用环境变量配置 Options
- 如何写业务层路由

但**关键设计选择**是：示例工程**不应该有任何头文件副本**。否则每次库头改动，git diff 会同时记录两份变更，污染历史。

### 8.2 CMakeLists — 直接 include 父目录源码

来自 [HISTORY/day28/app_example/CMakeLists.txt](HISTORY/day28/app_example/CMakeLists.txt)：

```cmake
# ─────────────────────────────────────────────────────────────────────────
# Day 28 引入 app_example 的目的：演示"应用层如何作为独立 CMake 子项目，
# 复用上一层目录中的网络库头文件与源代码"。
#
# 设计选择：直接以相对路径 include 父目录的 include/ 与 common/，避免
# 在 external/ 下保留任何头文件副本（那会让 git diff 在每次库头改动时
# 都重复记录两份变更，污染历史）。
# ─────────────────────────────────────────────────────────────────────────

set(LIB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# 网络库源码：与父项目 CMakeLists.txt 中 server 目标使用的列表保持一致
set(LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/DefaultPoller.cpp)
if(APPLE)
    list(APPEND LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/kqueue/KqueuePoller.cpp)
else()
    list(APPEND LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/epoll/EpollPoller.cpp)
endif()
```

```cmake
add_executable(http_server src/http_server.cpp ${LIB_SRCS})
target_include_directories(http_server PRIVATE ${LIB_ROOT}/include)
target_link_libraries(http_server PRIVATE pthread)
```

### 8.3 应用代码 — Options 一行注入

来自 [HISTORY/day28/app_example/src/http_server.cpp](HISTORY/day28/app_example/src/http_server.cpp)：

```cpp
#include <SignalHandler.h>
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <http/HttpServer.h>
```

`#include` 路径无前缀（不是 `<Airi-Cpp-Server-Lib/http/HttpServer.h>`），因为 CMake 已经把 `${LIB_ROOT}/include` 加进搜索路径。

### 8.4 全流程追踪 — `cmake -S app_example -B app_example/build`

业务场景：你拿到 `HISTORY/day28/` 整个目录，想跑示例工程。在仓库根目录执行：

```bash
cd HISTORY/day28
cmake -S app_example -B app_example/build
cmake --build app_example/build -j
./app_example/build/http_server
```

#### 时序总览

| 阶段 | CMake / 编译器实际动作 | 关键输入 | 关键输出 |
|------|---------------------|---------|---------|
| ① configure | CMake 解析 `app_example/CMakeLists.txt` | `CMAKE_CURRENT_SOURCE_DIR = HISTORY/day28/app_example` | `LIB_ROOT = HISTORY/day28/` |
| ② poller 选择 | 走 `if(APPLE)` 分支 | 当前 OS 是 macOS | `LIB_POLLER_SRCS` 选 `KqueuePoller.cpp`，否则选 `EpollPoller.cpp` |
| ③ 源码列表组装 | `set(LIB_SRCS ...)` 列出父目录所有 `common/*.cpp` | `LIB_ROOT` | 一个绝对路径列表 |
| ④ add_executable | 把 `src/http_server.cpp` 与 `LIB_SRCS` 共同编译为 `http_server` | 上面的列表 | 待编译目标 |
| ⑤ include 路径 | `target_include_directories(http_server PRIVATE ${LIB_ROOT}/include)` | `LIB_ROOT/include` | 编译器 `-I HISTORY/day28/include` |
| ⑥ build | `cmake --build` 触发 ninja/make | 上面所有 | `app_example/build/http_server` 自包含可执行文件 |

下面对应代码追踪。

#### 第一步：configure 阶段定位父目录

来自 [HISTORY/day28/app_example/CMakeLists.txt](HISTORY/day28/app_example/CMakeLists.txt)：

```cmake
set(LIB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

执行 `cmake -S app_example -B app_example/build` 时：

- `CMAKE_CURRENT_SOURCE_DIR` 被 CMake 设为 `HISTORY/day28/app_example`（即 `-S` 指向的目录）。
- `LIB_ROOT` 因此被求值为 `HISTORY/day28/app_example/..`，规范化后即 `HISTORY/day28/`。

这一步**没有任何头文件副本**——所有 include 路径都通过这个变量再相对引用，根本不存在 `external/` 下的镜像头。

#### 第二步：按平台选 poller 实现

```cmake
set(LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/DefaultPoller.cpp)
if(APPLE)
    list(APPEND LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/kqueue/KqueuePoller.cpp)
else()
    list(APPEND LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/epoll/EpollPoller.cpp)
endif()
```

在 macOS 上 `APPLE` 为真，`LIB_POLLER_SRCS = [DefaultPoller.cpp, KqueuePoller.cpp]`。Linux 上则 `LIB_POLLER_SRCS = [DefaultPoller.cpp, EpollPoller.cpp]`。EpollPoller 的源码引用了 `<sys/epoll.h>`，在 macOS 上压根编不过——这一行 `if(APPLE)` 让 macOS 根本不去触碰它。

#### 第三步：编译期合并库源码与应用源码

```cmake
add_executable(http_server src/http_server.cpp ${LIB_SRCS})
target_include_directories(http_server PRIVATE ${LIB_ROOT}/include)
target_link_libraries(http_server PRIVATE pthread)
```

- `add_executable` 把 `src/http_server.cpp`（应用层路由代码）与 `LIB_SRCS`（父目录全部 `common/*.cpp`）放进同一目标。CMake 会一次性把每个 `.cpp` 编为 `.o`，再链成 `http_server`。
- `target_include_directories` 加上 `-I HISTORY/day28/include`，让 `http_server.cpp` 里的 `#include <SignalHandler.h>` 能找到 `HISTORY/day28/include/SignalHandler.h`。
- `target_link_libraries(... PRIVATE pthread)` 链入线程库，仅此而已——**没有 `find_package(Airi-Cpp-Server-Lib)`**。

#### 第四步：应用代码以无前缀方式 include

来自 [HISTORY/day28/app_example/src/http_server.cpp](HISTORY/day28/app_example/src/http_server.cpp)：

```cpp
#include <SignalHandler.h>
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <http/HttpServer.h>
```

注意路径前缀是空的（不是 `<Airi-Cpp-Server-Lib/http/HttpServer.h>`）——因为 `${LIB_ROOT}/include` 已经在 `-I` 列表里。**和库内部源文件用同一个 include 习惯**，意味着如果未来把 app_example 拆成独立仓库，只要保留这条 `-I` 即可，应用代码本身一行不改。

#### 第五步：build 阶段产生自包含可执行文件

```bash
cmake --build app_example/build -j
```

ninja/make 把约 30 个 `.cpp` 各编一个 `.o`，再链成 `app_example/build/http_server`。该可执行文件里**已静态链了网络库的全部目标代码**，运行时不再依赖父项目 `build/lib/` 下的 `.a` 或 `.so`。

#### 各角色职责一句话

| 角色 | 在哪里出现 | 职责 |
|------|----------|------|
| `LIB_ROOT` | `app_example/CMakeLists.txt` | 描述"父项目根目录"的相对位置，所有路径都基于它派生 |
| `LIB_POLLER_SRCS` 内 `if(APPLE)` | 同上 | 按平台二选一 poller，避免 macOS 误编 epoll |
| `LIB_SRCS` | 同上 | 把父项目 `common/*.cpp` 全部纳入示例编译，无需 `find_package` |
| `target_include_directories(... PRIVATE ${LIB_ROOT}/include)` | 同上 | 让应用源码以"和库内部一致"的 include 路径写代码 |
| `external/` | **不存在** | 任何头副本都不再保存，git 历史里库头改动只在 `include/` 下记一份 |

---

## 9. 改进 H — GitHub Actions CI + .gitignore

### 9.1 CI 配置

来自 [HISTORY/day28/.github/workflows/ci.yml](HISTORY/day28/.github/workflows/ci.yml)：

```yaml
name: CI

# Day 28 引入：每次 push / PR 在 Linux + macOS 上完成构建与全部 CTest 用例。
# 后续 day31 起会基于此基础矩阵扩展 ASan / TSan / UBSan / Coverage / clang-tidy
# / 基准回归等任务，这里保持最小可运行集合。

on:
  push:
    branches: [main, master, develop]
  pull_request:
    branches: [main, master]

env:
  BUILD_TYPE: Release

jobs:
  build-and-test:
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-latest, macos-latest]
    runs-on: ${{ matrix.os }}

    steps:
      - uses: actions/checkout@v4

      - name: Configure CMake
        run: cmake -B build -DCMAKE_BUILD_TYPE=${{ env.BUILD_TYPE }}

      - name: Build
        run: cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

      - name: Run CTest
        working-directory: build
        run: ctest --output-on-failure -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

### 9.2 CTest 注册

来自 [HISTORY/day28/CMakeLists.txt](HISTORY/day28/CMakeLists.txt)：

```cmake
# ── CTest 注册（Day 28 引入）──
# 这些测试用例采用最小化的 stdlib 断言：失败 std::exit(1)，成功 return 0。
# 把它们注册到 CTest 后，CI 工作流即可通过 `ctest --output-on-failure` 自动跑全集。
# 与网络相关的 StressTest / BenchmarkTest 不进 CTest，避免 CI 端口冲突或网络抖动。
enable_testing()
add_test(NAME BackpressureDecisionTest COMMAND BackpressureDecisionTest)
add_test(NAME TcpServerPolicyTest      COMMAND TcpServerPolicyTest)
add_test(NAME SocketPolicyTest         COMMAND SocketPolicyTest)
add_test(NAME HttpContextTest          COMMAND HttpContextTest)
add_test(NAME EpollPolicyTest          COMMAND EpollPolicyTest)
add_test(NAME LogTest                  COMMAND LogTest)
add_test(NAME TimerTest                COMMAND TimerTest)
add_test(NAME ThreadPoolTest           COMMAND ThreadPoolTest)
```

注意 `StressTest` / `BenchmarkTest` 没有注册——它们需要真实端口和网络，CI 环境跑可能受外部干扰。

### 9.3 .gitignore — 屏蔽中间产物

来自 [HISTORY/day28/.gitignore](HISTORY/day28/.gitignore)：

```gitignore
# 构建产物
build/
build-*/
cmake-build-*/

# CMake 安装/导出生成物（不应纳入版本控制）
*Config.cmake
*ConfigVersion.cmake
*Targets.cmake
*Targets-*.cmake
install_manifest.txt
CMakeCache.txt
CMakeFiles/
CTestTestfile.cmake
cmake_install.cmake

# 编辑器/系统
.DS_Store
.vscode/
.idea/
*.swp
*~

# 应用层生成物
app_example/build/
app_example/external/Airi-Cpp-Server-Lib/lib/cmake/

# 日志/运行时
*.log
logs/
core
```

这一份 ignore 把 day28 之前历史里被错误纳入版本控制的 `Airi-Cpp-Server-LibConfig.cmake` / `Airi-Cpp-Server-LibTargets*.cmake` 等中间产物从此屏蔽。

---

## 10. 工程化收尾

### 10.1 util.cpp / util.h 移除

Day 27 之前所有错误检查走的都是 `errif()` 宏：

```cpp
errif(bind(...) == -1, "bind error");  // 失败时 perror + exit(1)
```

`exit(1)` 在生产环境是灾难——一个 socket 失败把整个进程拖死。Day 28 全部替换成 `LOG_ERROR`，调用方决定是否优雅降级。`util.cpp/h` 没有其他用途，连同删除。

### 10.2 test/ 目录化

旧版根目录的 `server.cpp` / `client.cpp` / `*Test.cpp` 各自散落；Day 28 统一收拢到 `test/`，CMake 用前缀路径 `test/server.cpp` 引用。这样 `find . -name "*.cpp"` 在根目录看到的只有 `common/` 实现源码，逻辑清爽。

来自 [HISTORY/day28/test/server.cpp](HISTORY/day28/test/server.cpp) — server.cpp 改造为通过环境变量配置：

```cpp
TcpServer::Options opts;
opts.listenIp = envOr("MYCPPSERVER_BIND_IP", opts.listenIp);
opts.listenPort = static_cast<uint16_t>(envIntOr("MYCPPSERVER_BIND_PORT", opts.listenPort));
opts.ioThreads = envIntOr("MYCPPSERVER_IO_THREADS", opts.ioThreads);
opts.maxConnections = static_cast<size_t>(envIntOr("MYCPPSERVER_MAX_CONNECTIONS", static_cast<int>(opts.maxConnections)));

TcpServer server(opts);
```

这套 env 注入风格在 day29、day30 的 `app_example/src/http_server.cpp` 里继续沿用。

---

## 11. 验证

```bash
cd HISTORY/day28
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

预期：8/8 PASS。

```bash
cmake -S app_example -B app_example/build
cmake --build app_example/build -j 8
./app_example/build/http_server
# 另一个终端
curl -v http://127.0.0.1:8888/
```

---

## 12. 局限与下一步

- **回压机制**目前只覆盖 OutputBuffer 单方向。完整方案还需要对 InputBuffer 做对称的"读限速"（Day 29+ 在 HTTP 层做）。
- **连接上限**在单 TcpServer 实例内统计；多进程部署下需要外部共享内存或负载均衡器协助。
- **HTTP pipeline** 实现的是"按序解析"，并没有"并行响应"——后者需要响应队列与发送侧排序。
- **CI 矩阵**目前只跑 build + ctest。ASan / TSan / UBSan / Coverage / clang-tidy / 基准回归等任务在 day31 引入。


接下来 Day 29 会在这套基础上引入生产特性：请求限流、TLS 预留、sendFile 零拷贝、路由表、中间件链。
