# Day 32 — Raft 选举层 / 异步 RPC 重构 / 指数退避

## 目录

| 章节 | 内容 |
|------|------|
| [§1 引言](#1-引言) | day31 的三个坏味道；本日改动全貌 |
| [§2 改进 A — nlohmann/json 接入](#2-改进-a--nlohmannjson-接入) | vendor json.hpp 单头；CMake 接入 |
| [§3 改进 B — RpcMessage 切换 nlohmann/json](#3-改进-b--rpcmessage-切换-nlohmannjson) | 修改 `RpcMessage.cpp`：encode/decode 改用 json 库，消除 JSON 注入 |
| [§4 改进 C — RpcServer Handler 改异步 Done-callback](#4-改进-c--rpcserver-handler-改异步-done-callback) | 修改 `RpcServer.h/cpp`：Handler 签名从 `string→string` 改为 `(req, done)`；Done 跨线程写回 + aliveFlag |
| [§5 改进 D — AsyncRpcClient 全异步长连接客户端](#5-改进-d--asyncrpcclient全异步长连接客户端) | 新建 `AsyncRpcClient.h/cpp`：非阻塞 connect 状态机、请求多路复用、指数退避、超时定时器 |
| [§6 改进 E — RaftTypes.h 数据类型](#6-改进-e--rafttypesh-数据类型) | 新建 `RaftTypes.h`：State/LogEntry/RequestVote/AppendEntries + NLOHMANN 宏 |
| [§7 改进 F — RaftNode Actor 模式选举节点](#7-改进-f--raftnode-actor-模式选举节点) | 新建 `RaftNode.h/cpp`：两线程架构、epoch 软取消计时器、选举/心跳全路径 |
| [§8 改进 G — RPC DoS 加固 + SignalHandler](#8-改进-g--rpc-dos-加固--signalhandler-重写) | `RpcServer.cpp` / `AsyncRpcClient.cpp` 加 16MiB 帧上限；`SignalHandler` self-pipe 重写 |
| [§9 改进 H — raft_demo 演示程序](#9-改进-h--raft_demo-演示程序) | 新建 `examples/src/raft_demo.cpp` |
| [§10 整体运行时理解](#10-整体运行时理解) | 对象所有权图 + 3 个完整追踪场景 |
| [§11 各模块职责速查表](#11-各模块职责速查表) | 本日所有新增/修改函数一览 |
| [§12 工程化](#12-工程化) | CMakeLists.txt 变更 |
| [§13 验证](#13-验证) | 三终端启动 raft_demo |
| [§14 局限与下一步](#14-局限与下一步) | 无日志复制/持久化；Day33 计划 |

---

## 本日变更文件一览

| 文件 | 变更 | 核心改动 |
|------|------|---------|
| `third_party/nlohmann/json.hpp` | **新建** | vendor nlohmann/json v3.11.3 单头 |
| `src/common/rpc/RpcMessage.cpp` | **修改** | encode/decode 改用 nlohmann/json，消除手拼字符串 |
| `src/include/rpc/RpcServer.h` | **修改** | Handler 签名改为异步 Done-callback |
| `src/common/rpc/RpcServer.cpp` | **修改** | Done 闭包构造（aliveFlag + connLoop->runInLoop）；16MiB 帧上限 |
| `src/include/rpc/AsyncRpcClient.h` | **新建** | 全异步长连接 RPC 客户端头文件 |
| `src/common/rpc/AsyncRpcClient.cpp` | **新建** | 非阻塞 connect 状态机、请求多路复用、ConnectBackoff |
| `src/include/raft/RaftTypes.h` | **新建** | Raft 数据类型 + NLOHMANN_DEFINE 宏 |
| `src/include/raft/RaftNode.h` | **新建** | RaftNode 接口声明 |
| `src/common/raft/RaftNode.cpp` | **新建** | Raft 选举全实现（选举+心跳+角色切换） |
| `src/include/net/SignalHandler.h/.cpp` | **修改** | self-pipe trick 重写，async-signal-safe |
| `examples/src/raft_demo.cpp` | **新建** | 最多 10 节点 Raft 选举演示程序 |
| `CMakeLists.txt` | **修改** | 接入 json、添加 raft_demo 目标 |

---

## 1. 引言

day31 落地了一套同步短连接 RPC：`RpcServer + RpcClient + RpcMessage`。它能跑，但有三个明显的坏味道：

**坏味道①：客户端是同步阻塞短连接**

`RpcClient::call()` 内部 `socket → connect → write → read → close`，全栈阻塞。从 Raft 节点的 EventLoop 线程调用，一次失败的 connect（peer 宕机时 SYN 重传可达 75 秒）就把整个 reactor 冻结——无法接收心跳、无法响应其他 peer，整个节点逻辑死锁。

**坏味道②：服务端 handler 是同步的**

旧版签名 `using Handler = std::function<std::string(const std::string&)>` 要求 handler 立刻返回响应字符串。但 Raft 的 `handleRequestVote` 要读写 `currentTerm_/votedFor_`——这些状态只属于 `loop_` 线程（Raft 的 single-thread invariant）。handler 被 sub-reactor 调用时不在 `loop_` 线程，要么用锁（破坏 invariant），要么 promise 等待（本质阻塞 sub-reactor）。

**坏味道③：RpcMessage 手拼 JSON 字符串**

来自 [HISTORY/day31/src/common/rpc/RpcMessage.cpp](HISTORY/day31/src/common/rpc/RpcMessage.cpp)（旧版，已被替换）：

```cpp
// 旧版——只要 method 或 payload 含双引号就生成非法 JSON
json += "{\"method\":\"";
json += method;
json += "\",\"body\":";
json += payload;
json += "}";
```

Raft 需要发送结构化参数（term、candidateId 等），必须换成正经 JSON 库。

**day32 的核心任务**：把整条 RPC 路径反 reactor 化的部分修回来，再在其上搭 Raft 选举层。

---

## 2. 改进 A — nlohmann/json 接入

### 2.1 为什么需要这一步

直接 `brew install` 或 `FetchContent` 拉 GitHub 在离线环境下会失败。为可复现性 + 离线构建，把 v3.11.3 单头 `json.hpp`（≈ 898KB）直接 vendor 进仓库。

### 2.2 编码实现步骤

**第一步：下载并放置 json.hpp**

```bash
mkdir -p third_party/nlohmann
curl -sSL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
  -o third_party/nlohmann/json.hpp
```

目录结构：
```
third_party/
└── nlohmann/
    └── json.hpp     ← v3.11.3，一次性提交到仓库
```

---

**第二步：修改 `CMakeLists.txt`，让 NetLib 及所有消费者能找到这个头文件**

来自 [CMakeLists.txt](CMakeLists.txt)（相对 day31 新增的片段，两段不相邻，分别在文件头部和 NetLib 定义处）：

```cmake
# ── 第三方依赖：nlohmann/json（vendored 单头文件版本）─────────────
# 直接把 v3.11.3 的 json.hpp 放在 third_party/nlohmann/，避免网络拉取。
# 这里不创建独立 target，而是直接把 include 路径加到 NetLib（PRIVATE），
# 避免 install(EXPORT) 把外部 target 拖入导出集。
set(NLOHMANN_JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
...
# JSON 依赖（PUBLIC + BUILD_INTERFACE：让 NetLib 消费者也能 #include <nlohmann/json.hpp>，
# 但不把绝对路径写进安装后的配置文件）
target_include_directories(NetLib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
```

`PUBLIC` 使得 NetLib 的所有消费者（`raft_demo`、测试等）也能 `#include <nlohmann/json.hpp>`，无需重复配置。`$<BUILD_INTERFACE:...>` generator expression 确保这个路径只在本地构建时暴露，不污染 install 后的配置文件（`AiriConfig.cmake`）。

---

## 3. 改进 B — RpcMessage 切换 nlohmann/json

### 3.1 为什么需要这一步

旧版 `encode` 手拼字符串，`decode` 用字符串搜索解析——两者都对含特殊字符的 payload 不安全。换 json 库后，序列化/反序列化都由库保证正确性，`parse` 失败时直接返回 `false` 而不是默默生成错误数据。

### 3.2 编码实现步骤

**修改 `src/common/rpc/RpcMessage.cpp`，替换为以下全部内容**

来自 [HISTORY/day32/src/common/rpc/RpcMessage.cpp](HISTORY/day32/src/common/rpc/RpcMessage.cpp)：

```cpp
// RpcMessage —— 二进制帧协议实现
//
// 帧格式（12 字节定长头 + JSON payload）：
//   [4B length BE] [4B msgType BE] [4B reqId BE] [JSON body ...]
//
// JSON body 形如：{"method": "echo", "body": <任意 JSON 值>}

#include "rpc/RpcMessage.h"
#include <arpa/inet.h>
#include <cstring>
#include <nlohmann/json.hpp>

using nlohmann::json;

std::string RpcMessage::encode() const {
    // 1. 把 payload（调用方保证是合法 JSON 文本）解析成 JSON 值，再嵌入外层。
    //    payload 为空时 body 设为 null。
    json body = payload.empty() ? json(nullptr) : json::parse(payload);
    json envelope = {
        {"method", method},
        {"body",   body},
    };
    std::string jsonText = envelope.dump();
    uint32_t payloadLen = static_cast<uint32_t>(jsonText.size());

    // 2. 12 字节头（全部网络字节序）
    uint32_t netLen   = htonl(payloadLen);
    uint32_t netType  = htonl(static_cast<uint32_t>(type));
    uint32_t netReqId = htonl(reqId);

    // 3. 拼装完整帧
    std::string frame;
    frame.resize(12 + payloadLen);
    std::memcpy(frame.data() + 0, &netLen,   4);
    std::memcpy(frame.data() + 4, &netType,  4);
    std::memcpy(frame.data() + 8, &netReqId, 4);
    std::memcpy(frame.data() + 12, jsonText.data(), payloadLen);
    return frame;
}

bool RpcMessage::decode(const char *data, int len,
                        RpcMessage *out, int *consumed) {
    if (len < 12) return false;

    uint32_t netLen, netType, netReqId;
    std::memcpy(&netLen,   data + 0, 4);
    std::memcpy(&netType,  data + 4, 4);
    std::memcpy(&netReqId, data + 8, 4);

    uint32_t payloadLen = ntohl(netLen);
    if (len < 12 + static_cast<int>(payloadLen)) return false;

    out->type  = static_cast<RpcMessage::Type>(ntohl(netType));
    out->reqId = ntohl(netReqId);

    // 用 nlohmann::json 安全解析；任何异常都视为协议错误，丢弃这条帧
    try {
        json envelope = json::parse(data + 12, data + 12 + payloadLen);
        out->method   = envelope.value("method", std::string{});
        if (envelope.contains("body") && !envelope["body"].is_null()) {
            out->payload = envelope["body"].dump();
        } else {
            out->payload.clear();
        }
    } catch (const json::exception &) {
        return false;
    }

    *consumed = 12 + static_cast<int>(payloadLen);
    return true;
}
```

`encode` 里 `json::parse(payload)` 把 payload 作为已解析的 JSON 值嵌入，再 `dump()` 重新序列化——这样 payload 里无论有什么特殊字符，最终输出的 envelope 都是合法 JSON。

`decode` 用 `json::parse(data + 12, data + 12 + payloadLen)` 精确解析 payload 长度范围内的内容（而不是整个 `recvBuf_`），任何格式错误被 `catch` 兜住并返回 `false`——调用方循环里遇到 false 就等更多数据（粘包保护）或关连接（格式错误）。

---

## 4. 改进 C — RpcServer Handler 改异步 Done-callback

### 4.1 为什么需要这一步

旧 Handler 签名要求立刻返回响应，但 Raft 的 `handleRequestVote` 必须先通过 `runInLoop` 切到 `loop_` 线程读写 Raft 状态，等处理完再调 `done(result)`——这在旧版里不可能做到。

新方案把响应控制权从"立刻返回"改为"任意时刻调 done"：

```
handler(reqJson, done)
  │
  └─ loop_.runInLoop([=] { 处理 Raft 状态 → done(result) })
     handler 立刻返回，sub-reactor 继续解下一帧
```

### 4.2 编码实现步骤

**第一步：修改 `src/include/rpc/RpcServer.h` 中的 Handler/Done 类型定义**

来自 [HISTORY/day32/src/include/rpc/RpcServer.h](HISTORY/day32/src/include/rpc/RpcServer.h)（变更后的完整接口）：

```cpp
#pragma once
#include "rpc/RpcMessage.h"
#include "net/TcpServer.h"
#include <functional>
#include <string>
#include <unordered_map>

class RpcServer {
  public:
    using Done    = std::function<void(std::string responseJson)>;
    using Handler = std::function<void(const std::string &req, Done done)>;

    RpcServer(const std::string &ip, uint16_t port, int ioThreads = 1);
    void addHandler(const std::string &method, Handler handler);
    void start();
    void stop();

  private:
    void onMessage(Connection *conn);
    void onNewConn(Connection *conn);

    TcpServer                                server_;
    std::unordered_map<std::string, Handler> handlers_;
};
```

`Done` 是一个闭包，内部已经捕获了把响应写回连接所需的一切（`connLoop`、`aliveWeak`、`reqId`、`connPtr`）。handler 调用 `done(payload)` 时，框架自动 `runInLoop` 回到正确的 IO 线程写出帧，调用方完全不用关心线程问题。

---

**第二步：修改 `src/common/rpc/RpcServer.cpp`，重写 `onMessage` 中的 Done 构造逻辑**

来自 [HISTORY/day32/src/common/rpc/RpcServer.cpp](HISTORY/day32/src/common/rpc/RpcServer.cpp)：

```cpp
#include "rpc/RpcServer.h"
#include "net/Connection.h"
#include "net/EventLoop.h"
#include "log/Logger.h"
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace {
// RPC 帧 payload 上限：超过即视为协议错误，关闭连接，防止恶意 length 让缓冲无界膨胀。
constexpr uint32_t kMaxRpcPayloadBytes = 16u * 1024u * 1024u; // 16 MiB
} // namespace

struct RpcConnCtx {
    std::string buf; // 已收到但未解析完整帧的字节数据
};

RpcServer::RpcServer(const std::string &ip, uint16_t port, int ioThreads)
    : server_([&] {
          TcpServer::Options opt;
          opt.listenIp   = ip;
          opt.listenPort = port;
          opt.ioThreads  = ioThreads;
          return opt;
      }()) {
    server_.newConnect([this](Connection *conn) { onNewConn(conn); });
    server_.onMessage([this](Connection *conn) { onMessage(conn); });
}

void RpcServer::addHandler(const std::string &method, Handler handler) {
    handlers_[method] = std::move(handler);
}

void RpcServer::start() { server_.start(); }
void RpcServer::stop() { server_.stop(); }

void RpcServer::onNewConn(Connection *conn) { conn->setContext(RpcConnCtx{}); }

void RpcServer::onMessage(Connection *conn) {
    auto *ctx = conn->getContextAs<RpcConnCtx>();

    Buffer *buf = conn->getInputBuffer();
    size_t  n   = buf->readableBytes();
    ctx->buf.append(buf->peek(), n);
    buf->retrieve(n);

    while (true) {
        // ── 防御性：在 decode 前先 peek 头 4 字节的 length，拒绝过大的帧 ──────
        // 旧版直接 decode，恶意客户端可以声明 length=4GB 让 ctx->buf 无界膨胀。
        if (ctx->buf.size() >= 4) {
            uint32_t netLen = 0;
            std::memcpy(&netLen, ctx->buf.data(), 4);
            uint32_t payloadLen = ntohl(netLen);
            if (payloadLen > kMaxRpcPayloadBytes) {
                LOG_WARN << "[RpcServer] 拒绝过大帧并关闭连接 fd=" << conn->getSocket()->getFd()
                         << " claimed=" << payloadLen << " limit=" << kMaxRpcPayloadBytes;
                conn->close();
                return;
            }
        }

        RpcMessage msg;
        int        consumed = 0;
        bool       ok = RpcMessage::decode(ctx->buf.data(), static_cast<int>(ctx->buf.size()), &msg,
                                           &consumed);
        if (!ok) break;
        ctx->buf.erase(0, consumed);
        if (msg.type != RpcMessage::Type::kRequest) continue;

        auto it = handlers_.find(msg.method);
        if (it == handlers_.end()) {
            // 未注册方法：立即同步回包
            RpcMessage resp;
            resp.type    = RpcMessage::Type::kResponse;
            resp.reqId   = msg.reqId;
            resp.method  = msg.method;
            resp.payload = R"({"error":"unknown method"})";
            conn->send(resp.encode());
            continue;
        }

        // ── 构造 done 回调：handler 完成后调用即可把响应写回 ──────────────────
        Eventloop          *connLoop  = conn->getLoop();
        std::weak_ptr<bool> aliveWeak = conn->aliveFlag();
        const uint32_t      reqId     = msg.reqId;
        std::string         method    = msg.method;
        Connection         *connPtr   = conn;

        Done done = [connLoop, aliveWeak, reqId, method = std::move(method),
                     connPtr](std::string responsePayload) mutable {
            connLoop->runInLoop([connPtr, aliveWeak, reqId, method = std::move(method),
                                 payload = std::move(responsePayload)]() mutable {
                auto a = aliveWeak.lock();
                if (!a || !*a) return; // 连接已销毁，丢弃响应
                RpcMessage resp;
                resp.type    = RpcMessage::Type::kResponse;
                resp.reqId   = reqId;
                resp.method  = std::move(method);
                resp.payload = std::move(payload);
                connPtr->send(resp.encode());
            });
        };

        // handler 立刻返回；sub-reactor 继续解下一帧，永不阻塞。
        it->second(msg.payload, std::move(done));
    }
}
```

`done` 闭包里的 `connLoop->runInLoop` 保证写帧动作在 conn 的归属 loop 线程执行。`aliveWeak.lock()` 检查 `weak_ptr<bool>`：conn 析构时 `shared_ptr<bool>` 的值被设为 `false`，`lock()` 返回非空但 `!*a` 为 true，此时安全丢弃响应——避免向已析构的 Connection 写数据（use-after-free）。

---

## 5. 改进 D — AsyncRpcClient：全异步长连接 RPC 客户端

### 5.1 为什么需要这个类

旧版 `RpcClient::call()` 在 EventLoop 线程里执行，会阻塞整个 reactor。

```
Raft loop_ 线程
  │
  ├─ call("RequestVote", ...)
  │    ├─ connect()   ← peer 宕机时 SYN 重传可达 75 秒，整个 reactor 冻结
  │    ├─ write()     ← 内核缓冲区满时挂起
  │    └─ read()      ← 等响应
  │
  └─ 冻结期间：无法接收心跳/无法处理其他 RPC → 触发不必要的重新选举
```

`AsyncRpcClient` 把"发送 + 等待 + 超时"完全嫁接到 EventLoop 的 IO 多路复用上，`callAsync` 立刻返回，响应或超时通过回调在 `loop_` 线程触发。具体解决三个子问题：

1. **非阻塞 connect**：`O_NONBLOCK + EINPROGRESS + Channel(EPOLLOUT) + getsockopt(SO_ERROR)`
2. **请求多路复用**：同一连接并发多个请求，用 `reqId` 路由回包
3. **超时不阻塞**：`loop_->runAfter` 定时器，超时回调在 `loop_` 线程触发

### 5.2 线程模型

```
外部线程（任意，如 Raft loop_）
────────────────────────────────
callAsync(method, json, cb, ms)
    └─ loop_->runInLoop(lambda) ← 投递后立刻返回

              ▼
        loop_ 线程（所有状态读写在此）
        ─────────────────────────────────────────
        doCall()
          ├─ state_==kConnected → conn_->send(frame), pending_[reqId]=cb
          ├─ state_==kIdle      → pendingFrames_.push_back(frame), startConnectLocked()
          └─ state_==kStopped   → cb(false, "")

        startConnectLocked()
          ├─ backoff_.inBackoff()? → failAllPending("backoff")，不发 TCP
          ├─ socket() + O_NONBLOCK + connect()
          ├─ EINPROGRESS → Channel(EPOLLOUT) + runAfter(3s, connect-timeout)
          └─ 立即完成   → onConnected(fd)

        onConnectWritable()
          ├─ getsockopt(SO_ERROR)==0 → onConnected(fd)
          └─ err != 0 → close + kIdle + failAllPending

        onConnected()
          ├─ ++connectEpoch_（让遗留 connect-timer 失效）
          ├─ backoff_.reset()
          ├─ Connection(fd, loop_) + enableInLoop()
          └─ flushPendingFrames()

        onResponse()
          └─ decode → pending_[reqId].cb(true, resp)

stop() ─ queueInLoop ─▶ kStopped, failAllPending, conn_.reset()
```

### 5.3 数据成员分组

| 分组 | 成员 | 说明 |
|------|------|------|
| **不可变配置** | `loop_` `ip_` `port_` | 构造后不变；任意线程只读 |
| **loop_ 独占** | `state_` | kIdle→kConnecting→kConnected→kStopped |
| **loop_ 独占** | `conn_` | 已建立的长连接；nullptr 表示未连接 |
| **loop_ 独占** | `connectChannel_` | connect 进行中时持有；连接完成后置 null |
| **loop_ 独占** | `connectFd_` `connectEpoch_` | connect 阶段 fd 和版本号；用于 cancel 超时 timer |
| **loop_ 独占** | `backoff_` | 指数退避状态（首次失败 500ms，翻倍，上限 30s） |
| **loop_ 独占** | `pending_` | `reqId → {callback, timerEpoch}` 的等待表 |
| **loop_ 独占** | `pendingFrames_` | connect 进行中时缓存的待发帧；建连后 flush |
| **loop_ 独占** | `recvBuf_` `nextReqId_` | 接收拼包缓冲区；请求 ID 生成器 |

`connectChannel_` 和 `conn_` 不重叠：connect 阶段持有 Channel（等 EPOLLOUT），建连成功后 Channel 销毁、Connection 创建接管 fd。

### 5.4 函数依赖层次

| 层次 | 函数 | 被谁调用 | 核心职责 |
|------|------|---------|---------|
| 叶层 | `failAllPending` | stop/startConnect/handleConnectionClosed | `swap` 出 pending_，逐一以 ok=false 触发回调并清空 |
| 叶层 | `cleanupConnectChannel` | onConnectWritable / connect-timer | disableAll + deleteChannel + reset；不 close fd |
| 叶层 | `completeWithTimeout` | loop_->runAfter 回调 | 若 reqId 仍在 pending_ 则以 ok=false 触发，否则 return（已正常回包）|
| 叶层 | `flushPendingFrames` | onConnected | 把 pendingFrames_ 队列里缓存的帧逐一发出 |
| 中层 | `startConnectLocked` | doCall（kIdle 时）| 检查退避 → socket → O_NONBLOCK → connect |
| 中层 | `onConnectWritable` | connectChannel_ 的 write 回调 | getsockopt(SO_ERROR) → 0 则 onConnected；否则 failAllPending |
| 中层 | `onConnected` | startConnectLocked / onConnectWritable | ++epoch → Connection → enableInLoop → flushPendingFrames |
| 中层 | `onResponse` | Connection 的 onMessage 回调 | 解帧，按 reqId 路由到 pending_ 对应的回调 |
| 中层 | `handleConnectionClosed` | Connection 的 deleteCallback（queueInLoop 后）| conn_.reset() → kIdle → failAllPending |
| 根层 | `stop` | 外部（RaftNode::stop）| queueInLoop：kStopped + failAllPending + conn_.reset |
| 根层 | `callAsync` | 外部任意线程（RaftNode）| runInLoop 切回 loop_ 线程 → doCall |
| 根层 | `doCall` | callAsync 内（loop_ 线程）| 分配 reqId + 注册超时 + 编帧 + 投递 |

### 5.5 编码实现步骤

**新建 `src/include/rpc/AsyncRpcClient.h`，写入以下全部内容**

来自 [HISTORY/day32/src/include/rpc/AsyncRpcClient.h](HISTORY/day32/src/include/rpc/AsyncRpcClient.h)：

```cpp
#pragma once
#include "net/Connection.h"
#include "rpc/RpcMessage.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class Eventloop;
class Channel;

class AsyncRpcClient {
  public:
    using Callback = std::function<void(bool ok, std::string responseJson)>;

    AsyncRpcClient(Eventloop *loop, std::string ip, uint16_t port);
    ~AsyncRpcClient();

    // 任意线程调用；callback 在 loop_ 线程触发。
    void callAsync(const std::string &method, const std::string &requestJson, Callback cb,
                   int timeoutMs = 200);

    // 主动关闭：取消所有 pending 并关闭长连接。stop 后 callAsync 立刻以 ok=false 回调。
    void stop();

  private:
    enum class State { kIdle, kConnecting, kConnected, kStopped };

    struct PendingCall {
        Callback cb;
        uint64_t timerEpoch{0};
    };

    // —— 以下函数全部只在 loop_ 线程执行 ——
    void doCall(const std::string &method, const std::string &requestJson, Callback cb,
                int timeoutMs);
    void startConnectLocked();
    void onConnectWritable(int fd, uint64_t connEpoch);
    void onConnected(int fd);
    void onResponse(Connection *conn);
    void handleConnectionClosed();
    void cleanupConnectChannel();
    void flushPendingFrames();
    void failAllPending(const char *reason);
    void completeWithTimeout(uint32_t reqId, uint64_t timerEpoch);

    // —— 不可变配置 ——
    Eventloop  *loop_;
    std::string ip_;
    uint16_t    port_;

    // 指数退避（初始 500ms，每次翻倍，上限 30s；仅第一次连续失败打 WARN）
    struct ConnectBackoff {
        static constexpr int64_t kInitMs = 500;
        static constexpr int64_t kMaxMs  = 30'000;
        int64_t untilMs_   {0};
        int64_t durationMs_{0};
        int     failures_  {0};
        bool inBackoff(int64_t nowMs) const { return nowMs < untilMs_; }
        bool recordFailure(int64_t nowMs) {
            durationMs_ = (durationMs_ == 0) ? kInitMs
                        : std::min(durationMs_ * 2, kMaxMs);
            untilMs_    = nowMs + durationMs_;
            return (failures_++ == 0);
        }
        void reset() { untilMs_ = 0; durationMs_ = 0; failures_ = 0; }
    };

    // —— loop_ 线程独占的状态 ——
    State                              state_{State::kIdle};
    std::unique_ptr<Connection>        conn_;
    std::unique_ptr<Channel>           connectChannel_;
    int                                connectFd_{-1};
    uint64_t                           connectEpoch_{0};
    ConnectBackoff                     backoff_;
    std::deque<std::string>            pendingFrames_;
    std::unordered_map<uint32_t, PendingCall> pending_;
    std::string                        recvBuf_;
    uint32_t                           nextReqId_{0};
};
```

`ConnectBackoff` 嵌在头文件里（定义在类内部）。`recordFailure` 返回 `(failures_++ == 0)` ——只有第一次连续失败返回 true，外层代码据此决定是否打 WARN（对长期宕机的节点不频繁刷日志）。

---

**新建 `src/common/rpc/AsyncRpcClient.cpp`，写入以下全部内容**

来自 [HISTORY/day32/src/common/rpc/AsyncRpcClient.cpp](HISTORY/day32/src/common/rpc/AsyncRpcClient.cpp)：

```cpp
// AsyncRpcClient.cpp —— 基于 EventLoop 的全异步 RPC 客户端实现

#include "rpc/AsyncRpcClient.h"
#include "net/Channel.h"
#include "net/Connection.h"
#include "net/EventLoop.h"
#include "log/Logger.h"

#include <chrono>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr uint32_t kMaxRpcPayloadBytes = 16u * 1024u * 1024u;

inline int64_t nowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

// ── §1 构造 / 析构 / stop ─────────────────────────────────────────────────

AsyncRpcClient::AsyncRpcClient(Eventloop *loop, std::string ip, uint16_t port)
    : loop_(loop), ip_(std::move(ip)), port_(port) {}

AsyncRpcClient::~AsyncRpcClient() { stop(); }

void AsyncRpcClient::stop() {
    loop_->queueInLoop([this] {
        if (state_ == State::kStopped) return;
        state_ = State::kStopped;
        failAllPending("client stopped");
        cleanupConnectChannel();
        conn_.reset();
    });
}

// ── §2 callAsync —— 任意线程入口；真正逻辑在 doCall（loop_ 线程）─────────

void AsyncRpcClient::callAsync(const std::string &method, const std::string &requestJson,
                               Callback cb, int timeoutMs) {
    loop_->runInLoop(
        [this, method, requestJson, cb = std::move(cb), timeoutMs]() mutable {
            doCall(method, requestJson, std::move(cb), timeoutMs);
        });
}

void AsyncRpcClient::doCall(const std::string &method, const std::string &requestJson, Callback cb,
                            int timeoutMs) {
    if (state_ == State::kStopped) {
        cb(false, "");
        return;
    }

    // 1) 分配 reqId 并登记 pending
    const uint32_t reqId = ++nextReqId_;
    PendingCall    pc;
    pc.cb         = std::move(cb);
    pc.timerEpoch = reqId; // 每个 reqId 一个 epoch（够用，因为 reqId 单调递增）
    pending_.emplace(reqId, std::move(pc));

    // 2) 超时定时器：用 reqId 自身作为 epoch 标识
    loop_->runAfter(timeoutMs / 1000.0,
                    [this, reqId] { completeWithTimeout(reqId, reqId); });

    // 3) 编码消息帧
    RpcMessage msg;
    msg.type    = RpcMessage::Type::kRequest;
    msg.reqId   = reqId;
    msg.method  = method;
    msg.payload = requestJson;
    std::string frame = msg.encode();

    // 4) 投递：已连接直接发；否则入帧队列，kIdle 则触发 connect
    if (state_ == State::kConnected && conn_) {
        conn_->send(std::move(frame));
        return;
    }
    pendingFrames_.emplace_back(std::move(frame));
    if (state_ == State::kIdle) startConnectLocked();
}

// ── §3 非阻塞 connect 状态机 ─────────────────────────────────────────────

void AsyncRpcClient::startConnectLocked() {
    if (backoff_.inBackoff(nowSteadyMs())) {
        failAllPending("connect backoff");
        return;
    }
    state_ = State::kConnecting;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR << "[AsyncRpcClient] socket() 失败: " << strerror(errno);
        state_ = State::kIdle;
        failAllPending("socket() failed");
        return;
    }
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    ::inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

    int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (rc == 0) {
        onConnected(fd); // 极少见：loopback 立即完成
        return;
    }
    if (errno != EINPROGRESS) {
        if (backoff_.recordFailure(nowSteadyMs()))
            LOG_WARN << "[AsyncRpcClient] connect 立即失败 " << ip_ << ":" << port_
                     << " err=" << strerror(errno);
        ::close(fd);
        state_ = State::kIdle;
        failAllPending("connect failed");
        return;
    }

    // EINPROGRESS：等可写（内核正在完成三次握手）
    connectFd_                = fd;
    const uint64_t connEpoch  = ++connectEpoch_;
    connectChannel_           = std::make_unique<Channel>(loop_, fd);
    connectChannel_->setWriteCallback([this, fd, connEpoch] {
        if (connEpoch != connectEpoch_) return; // 已被新一轮 connect 顶替
        onConnectWritable(fd, connEpoch);
    });
    connectChannel_->enableWriting();

    // 连接整体超时（独立于业务超时）：3 秒兜底
    loop_->runAfter(3.0, [this, connEpoch] {
        if (connEpoch != connectEpoch_) return;
        if (state_ != State::kConnecting) return;
        if (backoff_.recordFailure(nowSteadyMs()))
            LOG_WARN << "[AsyncRpcClient] connect 超时 " << ip_ << ":" << port_;
        cleanupConnectChannel();
        state_ = State::kIdle;
        failAllPending("connect timeout");
    });
}

void AsyncRpcClient::onConnectWritable(int fd, uint64_t /*connEpoch*/) {
    int       err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    cleanupConnectChannel();
    if (err != 0) {
        if (backoff_.recordFailure(nowSteadyMs()))
            LOG_WARN << "[AsyncRpcClient] async connect 失败 " << ip_ << ":" << port_
                     << " err=" << strerror(err);
        ::close(fd);
        state_ = State::kIdle;
        failAllPending("async connect failed");
        return;
    }
    onConnected(fd);
}

void AsyncRpcClient::onConnected(int fd) {
    ++connectEpoch_; // 让任何遗留的 connect-timer 立刻失效
    backoff_.reset();
    state_ = State::kConnected;

    conn_ = std::make_unique<Connection>(fd, loop_);
    conn_->setDeleteConnectionCallback([this](int /*fd*/) {
        // 由 Connection::close() 同步调入；当下仍在调用栈上，
        // 不能直接 reset(conn_)（会 UAF）。延迟到 doPendingFunctors() 处理。
        loop_->queueInLoop([this] { handleConnectionClosed(); });
    });
    conn_->setOnMessageCallback([this](Connection *c) { onResponse(c); });
    conn_->enableInLoop();

    flushPendingFrames();
}

void AsyncRpcClient::cleanupConnectChannel() {
    if (connectChannel_) {
        connectChannel_->disableAll();
        loop_->deleteChannel(connectChannel_.get());
        connectChannel_.reset();
    }
    connectFd_ = -1;
}

void AsyncRpcClient::flushPendingFrames() {
    if (!conn_) return;
    while (!pendingFrames_.empty()) {
        conn_->send(std::move(pendingFrames_.front()));
        pendingFrames_.pop_front();
    }
}

// ── §4 收包路由：reqId → callback ────────────────────────────────────────

void AsyncRpcClient::onResponse(Connection *conn) {
    Buffer *buf = conn->getInputBuffer();
    size_t  n   = buf->readableBytes();
    recvBuf_.append(buf->peek(), n);
    buf->retrieve(n);

    while (true) {
        // 防御性：peek 头 4 字节 length，过大直接关连接
        if (recvBuf_.size() >= 4) {
            uint32_t netLen = 0;
            std::memcpy(&netLen, recvBuf_.data(), 4);
            uint32_t payloadLen = ntohl(netLen);
            if (payloadLen > kMaxRpcPayloadBytes) {
                LOG_WARN << "[AsyncRpcClient] 对端响应 length 超限，关闭连接 " << ip_ << ":"
                         << port_ << " claimed=" << payloadLen
                         << " limit=" << kMaxRpcPayloadBytes;
                conn->close();
                return;
            }
        }

        RpcMessage resp;
        int        consumed = 0;
        bool       ok = RpcMessage::decode(recvBuf_.data(), static_cast<int>(recvBuf_.size()),
                                           &resp, &consumed);
        if (!ok) break;
        recvBuf_.erase(0, consumed);

        if (resp.type != RpcMessage::Type::kResponse) continue;
        auto it = pending_.find(resp.reqId);
        if (it == pending_.end()) continue; // 已超时被清理，丢弃迟到回包
        Callback cb = std::move(it->second.cb);
        pending_.erase(it);
        if (cb) cb(true, std::move(resp.payload));
    }
}

// ── §5 错误 / 超时清理 ───────────────────────────────────────────────────

void AsyncRpcClient::handleConnectionClosed() {
    LOG_INFO << "[AsyncRpcClient] 与 " << ip_ << ":" << port_ << " 的连接已断开";
    conn_.reset();
    recvBuf_.clear();
    if (state_ == State::kStopped) {
        failAllPending("client stopped during close");
        return;
    }
    state_ = State::kIdle;
    failAllPending("connection lost");
}

void AsyncRpcClient::failAllPending(const char *reason) {
    // 业务回调可能在内部又触发 callAsync，因此先 swap 出来再清空。
    std::unordered_map<uint32_t, PendingCall> snapshot;
    snapshot.swap(pending_);
    pendingFrames_.clear();
    for (auto &kv : snapshot) {
        if (kv.second.cb) kv.second.cb(false, "");
    }
    if (reason && *reason)
        LOG_DEBUG << "[AsyncRpcClient] 待处理请求已清空：" << reason;
}

void AsyncRpcClient::completeWithTimeout(uint32_t reqId, uint64_t /*timerEpoch*/) {
    auto it = pending_.find(reqId);
    if (it == pending_.end()) return; // 已收到响应，timer 自然失效
    Callback cb = std::move(it->second.cb);
    pending_.erase(it);
    if (cb) cb(false, "");
}
```

**关键设计点：**

`failAllPending` 使用 `snapshot.swap(pending_)` 而不是直接遍历 `pending_`：业务回调（`cb(false, ...)`）可能在内部触发新的 `callAsync`，新的 reqId 会进入 `pending_`。如果边遍历边删除，新加入的 entry 可能被误删或导致 iterator 失效。swap 先把所有旧 entry 搬出来，再遍历触发，新的 entry 安全进入空的 `pending_`。

`handleConnectionClosed` 用 `queueInLoop` 异步化：`setDeleteConnectionCallback` 是在 `Connection::close()` 的调用栈上触发的，此时 `conn_` 还活着。如果直接 `conn_.reset()`，是在 `Connection` 自身的调用链上析构 `Connection`（UAF）。`queueInLoop` 让清理在调用链返回后才执行。

---

## 6. 改进 E — RaftTypes.h：数据类型

### 6.1 为什么需要这个文件

Raft 节点之间要序列化传输 5 种结构体。手写每种结构体的 `to_json`/`from_json` 很繁琐，`NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` 宏一行搞定——展开后生成两个自由函数 `to_json(j, obj)` 和 `from_json(j, obj)`，注册到 nlohmann/json 的 ADL 扩展点，让 `json(args).dump()` 序列化、`json::parse(str).get<T>()` 反序列化都能自动工作，不需要修改结构体本身（Non-Intrusive 的含义）。

### 6.2 编码实现步骤

**新建 `src/include/raft/RaftTypes.h`，写入以下全部内容**

来自 [HISTORY/day32/src/include/raft/RaftTypes.h](HISTORY/day32/src/include/raft/RaftTypes.h)：

```cpp
#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace raft {

enum class State { Follower, Candidate, Leader };

struct LogEntry {
    uint64_t    term{0};
    std::string cmd;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LogEntry, term, cmd)

struct RequestVoteArgs {
    uint64_t term{0};
    int      candidateId{-1};
    uint64_t lastLogIndex{0};
    uint64_t lastLogTerm{0};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RequestVoteArgs, term, candidateId, lastLogIndex, lastLogTerm)

struct RequestVoteReply {
    uint64_t term{0};
    bool     voteGranted{false};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RequestVoteReply, term, voteGranted)

// AppendEntries（Day32 只用心跳；Day33 会补 prevLogIndex / entries 等字段）
struct AppendEntriesArgs {
    uint64_t term{0};
    int      leaderId{-1};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesArgs, term, leaderId)

struct AppendEntriesReply {
    uint64_t term{0};
    bool     success{false};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesReply, term, success)

inline const char* stateName(State s) {
    switch (s) {
        case State::Follower:  return "Follower";
        case State::Candidate: return "Candidate";
        case State::Leader:    return "Leader";
    }
    return "?";
}

} // namespace raft
```

Day32 的 `AppendEntriesArgs` 只有 `term` 和 `leaderId`——它是纯心跳，不携带日志内容。Day33 会补上 `prevLogIndex`/`prevLogTerm`/`entries`/`leaderCommit`，实现真正的日志复制。

---

## 7. 改进 F — RaftNode：Actor 模式选举节点

### 7.1 为什么需要这个类

这是 day32 的核心新功能——实现 Raft 共识算法的选举部分：多台机器如何对"谁是 Leader"这件事达成一致？

三节点集群启动时，每个节点都是 Follower，等待 Leader 的心跳。如果在随机超时（150~300ms）内没收到心跳，Follower 认为 Leader 失联，转为 Candidate 并发起选举：自增 term、给自己投票、向所有 peer 广播 RequestVote。如果收到超过半数（quorum）的投票，就成为 Leader，开始每 50ms 广播一次心跳，压制其余节点的选举计时器。

**为什么是 Actor 模式**：所有 Raft 状态（`currentTerm_/votedFor_/log_/state_`）只在 `loop_` 线程读写，彻底无锁。入站 RPC 来自另一个线程（rpcServerThread_），通过 `loop_.runInLoop` 投递到 `loop_` 线程处理，不破坏 invariant。

### 7.2 两线程架构

```
┌─────────────────────────────────────────────────────────────────┐
│  loopThread_（Raft 状态机 + 出站 RPC IO）                       │
│                                                                 │
│  loop_.loop()                                                   │
│    ├─ resetElectionTimer()    ← 启动时注册，收到心跳时重置      │
│    ├─ runEvery(50ms) → heartbeatTick()                          │
│    ├─ AsyncRpcClient IO 回调（onConnect / onResponse / 等）     │
│    └─ runInLoop 投递来的 Raft 状态读写（handleRequestVote 等）  │
│                                                                 │
│  数据成员（loop_ 线程独占）：                                   │
│    currentTerm_(atomic)  votedFor_  log_  state_(atomic)        │
│    electionEpoch_  currentElectionVotes_  leaderId_             │
│    peerClients_  rng_                                           │
└─────────────────────────────────────────────────────────────────┘
         ↑ runInLoop（处理 inbound RPC）
┌─────────────────────────────────────────────────────────────────┐
│  rpcServerThread_（接收入站 RPC）                               │
│                                                                 │
│  rpcServer_.start()                                             │
│    └─ TcpServer（sub-reactor）                                  │
│         └─ onMessage → handler(req, done)                       │
│              ├─ handleRequestVote  → loop_.runInLoop(lambda)    │
│              └─ handleAppendEntries → loop_.runInLoop(lambda)   │
└─────────────────────────────────────────────────────────────────┘
```

### 7.3 数据成员分组

| 分组 | 成员 | 说明 |
|------|------|------|
| **不可变配置** | `id_` `peers_` `quorum_` | 构造时确定，之后只读 |
| **loop_ 独占** | `votedFor_` `log_` `leaderId_` `currentElectionVotes_` `electionEpoch_` | 只在 loop_ 读写 |
| **loop_ 独占** | `peerClients_` | 每个 peer 一个 AsyncRpcClient，仅 loop_ 访问 |
| **loop_ 独占** | `rng_` | 线程本地随机数，仅 loop_ 使用 |
| **对外原子快照** | `currentTerm_` `state_` | atomic；外部线程可读（如 raft_demo 轮询显示）|
| **基础设施** | `loop_` `loopThread_` `running_` `rpcServer_` `rpcServerThread_` | 生命周期管理 |

### 7.4 函数依赖层次

| 层次 | 函数 | 被谁调用 | 核心职责 |
|------|------|---------|---------|
| 叶层 | `lastLogIndex/Term` | handleRequestVote, startElection | 读 log_ 末尾 |
| 叶层 | `getOrCreateClient` | startElection, heartbeatTick | lazy 构造 AsyncRpcClient |
| 中层 | `becomeFollower` | handleRequestVote/AppendEntries, onVoteReply, onHeartbeatReply | 降级：更新 term, votedFor_=-1, resetElectionTimer |
| 中层 | `becomeCandidate` | electionTimerFired | 升级：++term, 自投票, votes=1 |
| 中层 | `becomeLeader` | onVoteReply（票数 >= quorum）| 升级：++epoch, 立刻广播心跳 |
| 中层 | `resetElectionTimer` | becomeFollower/Candidate, handleRequestVote（投票后）, handleAppendEntries（合法心跳）| ++epoch, runAfter(150~300ms) |
| 中层 | `electionTimerFired` | loop_->runAfter 回调 | epoch 守卫 → becomeCandidate → startElection |
| 中层 | `startElection` | electionTimerFired | 广播 RequestVote 到所有 peer |
| 中层 | `onVoteReply` | callAsync 的回调 | 过滤旧回包 → 累积票数 → 达 quorum 则 becomeLeader |
| 中层 | `heartbeatTick` | loop_->runEvery(50ms) | 仅 Leader 广播 AppendEntries |
| 中层 | `onHeartbeatReply` | callAsync 的回调 | 发现更高 term 则退位 |
| 中层 | `handleRequestVote/AppendEntries` | rpcServer_ handler | 解析 → loop_.runInLoop → 处理 → done(reply) |
| 根层 | `start` `stop` | 外部（raft_demo）| 线程编排 |

### 7.5 编码实现步骤

**新建 `src/include/raft/RaftNode.h`，写入以下全部内容**

来自 [HISTORY/day32/src/include/raft/RaftNode.h](HISTORY/day32/src/include/raft/RaftNode.h)：

```cpp
#pragma once
#include "EventLoop.h"
#include "raft/RaftTypes.h"
#include "rpc/AsyncRpcClient.h"
#include "rpc/RpcServer.h"
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace raft {

struct Peer {
    int         id;
    std::string ip;
    uint16_t    port;
};

class RaftNode {
  public:
    RaftNode(int id, std::vector<Peer> peers, uint16_t rpcPort);
    ~RaftNode();

    void start();
    void stop();

    // 外部线程可调用的只读快照（基于 atomic，无锁）
    State    getState() const { return state_.load(); }
    uint64_t getCurrentTerm() const { return currentTerm_.load(); }
    bool     isLeader() const { return state_.load() == State::Leader; }
    int      getId() const { return id_; }

  private:
    void handleRequestVote(const std::string &reqJson, RpcServer::Done done);
    void handleAppendEntries(const std::string &reqJson, RpcServer::Done done);

    void becomeFollower(uint64_t term);
    void becomeCandidate();
    void becomeLeader();

    void resetElectionTimer();
    void electionTimerFired(uint64_t epoch);

    void startElection();
    void onVoteReply(uint64_t electionTerm, int peerId, bool ok, RequestVoteReply reply);

    void heartbeatTick();
    void onHeartbeatReply(int peerId, bool ok, AppendEntriesReply reply);

    uint64_t lastLogIndex() const;
    uint64_t lastLogTerm() const;

    AsyncRpcClient *getOrCreateClient(const Peer &peer);

    // ── 配置 ────────────────────────────────────────────────────────────
    int               id_;
    std::vector<Peer> peers_;
    int               quorum_;

    // ── Raft 状态（只在 loop_ 线程读写；外部只读字段额外用 atomic 暴露）──
    std::atomic<uint64_t> currentTerm_{0};
    int                   votedFor_{-1};
    std::vector<LogEntry> log_;
    std::atomic<State>    state_{State::Follower};
    int                   leaderId_{-1};
    int                   currentElectionVotes_{0};
    uint64_t              electionEpoch_{0};

    // ── 基础设施 ────────────────────────────────────────────────────────
    Eventloop         loop_;
    std::thread       loopThread_;
    std::atomic<bool> running_{false};
    std::mt19937      rng_;

    RpcServer   rpcServer_;
    std::thread rpcServerThread_;

    std::unordered_map<int, std::unique_ptr<AsyncRpcClient>> peerClients_;
};

} // namespace raft
```

---

**新建 `src/common/raft/RaftNode.cpp`，写入以下全部内容**

来自 [HISTORY/day32/src/common/raft/RaftNode.cpp](HISTORY/day32/src/common/raft/RaftNode.cpp)：

```cpp
// RaftNode.cpp —— 与本项目 EventLoop 融合的 Raft 选举实现（全异步 RPC 版）
#include "raft/RaftNode.h"
#include "log/Logger.h"
#include <chrono>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace raft {

// ════════════════════════════════════════════════════════════════════════════
// §1  构造 / 析构 / start / stop
// ════════════════════════════════════════════════════════════════════════════

RaftNode::RaftNode(int id, std::vector<Peer> peers, uint16_t rpcPort)
    : id_(id),
      peers_(std::move(peers)),
      quorum_(static_cast<int>(peers_.size()) / 2 + 1),
      rng_(std::random_device{}()),
      rpcServer_("0.0.0.0", rpcPort, /*ioThreads=*/1) {
    // 哨兵条目：让 lastLogIndex() 和 lastLogTerm() 在日志为空时也能安全返回。
    // log_[0].term = 0，任何真实日志条目的 term >= 1，不会与哨兵混淆。
    log_.push_back(LogEntry{0, ""});

    rpcServer_.addHandler(
        "RequestVote",
        [this](const std::string &req, RpcServer::Done done) {
            handleRequestVote(req, std::move(done));
        });
    rpcServer_.addHandler(
        "AppendEntries",
        [this](const std::string &req, RpcServer::Done done) {
            handleAppendEntries(req, std::move(done));
        });
}

RaftNode::~RaftNode() { stop(); }

void RaftNode::start() {
    if (running_.exchange(true)) return; // 幂等

    // (a) Eventloop 线程：所有 Raft 状态变迁 + 出站 RPC IO 都在这条线程上
    loopThread_ = std::thread([this] {
        loop_.runInLoop([this] { resetElectionTimer(); });
        loop_.runEvery(0.05, [this] { heartbeatTick(); });
        loop_.loop();
    });

    // (b) RPC server 线程：rpcServer_.start() 阻塞，跑在自己的 std::thread
    rpcServerThread_ = std::thread([this] { rpcServer_.start(); });

    LOG_INFO << "[Node " << id_ << "] 已在端口 " << peers_[id_].port
             << " 启动（peers=" << peers_.size() << "）";
}

void RaftNode::stop() {
    if (!running_.exchange(false)) return;

    // 1. 停 RPC server（让 rpcServerThread_ 退出）
    rpcServer_.stop();
    if (rpcServerThread_.joinable()) rpcServerThread_.join();

    // 2. 先把所有 AsyncRpcClient 的析构投递到 loop_ 线程：
    //    它们持有的 Connection 必须在 loop_ 线程析构（poller 操作约束）。
    loop_.queueInLoop([this] { peerClients_.clear(); });

    // 3. 停 loop_（让 loopThread_ 退出）。setQuit + wakeup 后，
    //    loop_ 退出前会先 doPendingFunctors()，把上一步的 clear 跑掉。
    loop_.setQuit();
    loop_.wakeup();
    if (loopThread_.joinable()) loopThread_.join();

    LOG_INFO << "[Node " << id_ << "] 已停止";
}

// ════════════════════════════════════════════════════════════════════════════
// §2  RPC server 回调：fire-and-forget actor 模式
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::handleRequestVote(const std::string &reqJson, RpcServer::Done done) {
    RequestVoteArgs args;
    try {
        args = json::parse(reqJson).get<RequestVoteArgs>();
    } catch (...) {
        done(R"({"term":0,"voteGranted":false})");
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        // 【Raft §5.1】任何 RPC 只要携带更高 term，接收方必须立刻退回 Follower。
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        bool grant = false;
        if (
            // 条件①：候选人的 term 不小于我的 term
            args.term >= currentTerm_.load() &&
            // 条件②：本任期内我还没投过票，或者之前投给了这个候选人
            (votedFor_ == -1 || votedFor_ == args.candidateId) &&
            // 条件③：候选人日志至少和我一样「新」（Raft 选举安全性）
            (args.lastLogTerm > lastLogTerm() ||
             (args.lastLogTerm == lastLogTerm() && args.lastLogIndex >= lastLogIndex()))
        ) {
            grant     = true;
            votedFor_ = args.candidateId;
            resetElectionTimer();
            LOG_INFO << "[Node " << id_ << "] 已投票给节点 " << args.candidateId
                     << "，term=" << args.term;
        }

        RequestVoteReply reply{currentTerm_.load(), grant};
        done(json(reply).dump());
    });
}

void RaftNode::handleAppendEntries(const std::string &reqJson, RpcServer::Done done) {
    AppendEntriesArgs args;
    try {
        args = json::parse(reqJson).get<AppendEntriesArgs>();
    } catch (...) {
        done(R"({"term":0,"success":false})");
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        bool success = false;
        // 【Raft §5.2】只接受 term >= 当前 term 的 AppendEntries
        if (args.term >= currentTerm_.load()) {
            state_.store(State::Follower);
            leaderId_ = args.leaderId;
            success   = true;
            // 【关键】重置选举计时器：收到 Leader 心跳 = Leader 还活着
            resetElectionTimer();
        }
        AppendEntriesReply reply{currentTerm_.load(), success};
        done(json(reply).dump());
    });
}

// ════════════════════════════════════════════════════════════════════════════
// §3  角色切换 + 选举定时器（必须在 loop_ 线程调用）
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::becomeFollower(uint64_t term) {
    if (state_.load() != State::Follower)
        LOG_INFO << "[Node " << id_ << "] " << stateName(state_.load()) << " → Follower（term="
                 << term << "）";
    state_.store(State::Follower);
    currentTerm_.store(term);
    votedFor_ = -1;
    resetElectionTimer();
}

void RaftNode::becomeCandidate() {
    // 【Raft §5.2】发起新选举时必须先递增自己的 term。
    // 这防止旧的投票响应（来自网络延迟）污染新一轮选举。
    currentTerm_.store(currentTerm_.load() + 1);
    state_.store(State::Candidate);
    votedFor_             = id_;  // 候选人给自己投一票
    currentElectionVotes_ = 1;    // 票数从 1 开始（已含自己那票）
    LOG_INFO << "[Node " << id_ << "] 选举超时 → 候选人，term=" << currentTerm_.load();
    resetElectionTimer(); // 如果这轮平票，超时后自动发起下一轮
}

void RaftNode::becomeLeader() {
    state_.store(State::Leader);
    leaderId_ = id_;
    LOG_INFO << "[Node " << id_ << "] *** 成为 LEADER，term=" << currentTerm_.load() << " ***";
    // 成为 Leader 后用 ++epoch 让所有已安排的 electionTimerFired 回调失效
    ++electionEpoch_;
    // 立刻广播一次心跳，不等 50ms runEvery 的下一次触发
    heartbeatTick();
}

void RaftNode::resetElectionTimer() {
    // EventLoop 定时器无法主动取消，用「版本号」实现软取消：
    // 每次 reset 都递增 epoch，旧定时器触发时发现 epoch 不匹配就自动放弃。
    ++electionEpoch_;
    uint64_t myEpoch = electionEpoch_;
    // 随机化超时（150~300ms）是 Raft 避免同时选举的关键机制
    int timeoutMs = std::uniform_int_distribution<int>(150, 300)(rng_);
    loop_.runAfter(timeoutMs / 1000.0,
                   [this, myEpoch] { electionTimerFired(myEpoch); });
}

void RaftNode::electionTimerFired(uint64_t epoch) {
    if (epoch != electionEpoch_) return; // 已被新的 reset 覆盖
    if (state_.load() == State::Leader) return;
    becomeCandidate();
    startElection();
}

// ════════════════════════════════════════════════════════════════════════════
// §4  选举主流程：完全异步的 RPC 发射
// ════════════════════════════════════════════════════════════════════════════

AsyncRpcClient *RaftNode::getOrCreateClient(const Peer &peer) {
    auto it = peerClients_.find(peer.id);
    if (it != peerClients_.end()) return it->second.get();
    auto client = std::make_unique<AsyncRpcClient>(&loop_, peer.ip, peer.port);
    auto *raw   = client.get();
    peerClients_.emplace(peer.id, std::move(client));
    return raw;
}

void RaftNode::startElection() {
    uint64_t term     = currentTerm_.load();
    uint64_t lastIdx  = lastLogIndex();
    uint64_t lastTerm = lastLogTerm();

    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        RequestVoteArgs args{term, id_, lastIdx, lastTerm};
        std::string     reqJson = json(args).dump();
        int             peerId  = peer.id;

        AsyncRpcClient *client = getOrCreateClient(peer);
        client->callAsync("RequestVote", reqJson,
                          [this, term, peerId](bool ok, std::string respJson) {
                              RequestVoteReply reply{};
                              if (ok) {
                                  try {
                                      reply = json::parse(respJson).get<RequestVoteReply>();
                                  } catch (...) { ok = false; }
                              }
                              onVoteReply(term, peerId, ok, reply);
                          },
                          /*timeoutMs=*/150);
    }
}

void RaftNode::onVoteReply(uint64_t electionTerm, int peerId, bool ok, RequestVoteReply reply) {
    // 过滤旧回包：state_!=Candidate 说明已退位；term 变了说明是旧选举的回包
    if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) return;
    if (!ok) return;
    if (reply.term > currentTerm_.load()) { becomeFollower(reply.term); return; }
    if (reply.voteGranted) {
        ++currentElectionVotes_;
        LOG_INFO << "[Node " << id_ << "] 收到节点 " << peerId
                 << " 的投票（已得票=" << currentElectionVotes_ << "/" << quorum_ << "）";
        if (currentElectionVotes_ >= quorum_) becomeLeader();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §5  心跳：每 50ms 一次，仅 Leader 实际广播
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::heartbeatTick() {
    if (state_.load() != State::Leader) return;
    uint64_t term = currentTerm_.load();

    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        AppendEntriesArgs args{term, id_};
        std::string       reqJson = json(args).dump();
        int               peerId  = peer.id;

        AsyncRpcClient *client = getOrCreateClient(peer);
        client->callAsync("AppendEntries", reqJson,
                          [this, peerId](bool ok, std::string respJson) {
                              AppendEntriesReply reply{};
                              if (ok) {
                                  try {
                                      reply = json::parse(respJson).get<AppendEntriesReply>();
                                  } catch (...) { ok = false; }
                              }
                              onHeartbeatReply(peerId, ok, reply);
                          },
                          /*timeoutMs=*/100);
    }
}

void RaftNode::onHeartbeatReply(int /*peerId*/, bool ok, AppendEntriesReply reply) {
    if (!ok) return;
    // 回包 term 更大：说明集群已选出更新任期的 Leader，自己是「僵尸 Leader」，立刻退位
    if (reply.term > currentTerm_.load()) becomeFollower(reply.term);
}

uint64_t RaftNode::lastLogIndex() const { return static_cast<uint64_t>(log_.size() - 1); }
uint64_t RaftNode::lastLogTerm() const { return log_.back().term; }

} // namespace raft
```

---

## 8. 改进 G — RPC DoS 加固 + SignalHandler 重写

### 8.1 RPC 帧 16MiB 上限

已经包含在 §4 的 `RpcServer.cpp` 和 §5 的 `AsyncRpcClient.cpp` 里：在 `decode` 之前先 peek 头 4 字节，若声明 length > 16MiB 则关闭连接。旧版没有这个检查，恶意客户端可以声明 length=4GiB 让接收方 OOM。

### 8.2 SignalHandler self-pipe 重写

旧版 `SignalHandler` 在 OS 信号处理函数里直接执行 `handlers_[sig]()`。问题：`std::map::operator[]` 会在 key 不存在时 `malloc`（不在 async-signal-safe 白名单），`std::function::operator()` 同样不安全——生产中会偶发崩溃。

**新方案（self-pipe trick）**：信号处理函数只做一件事——把信号编号 `write` 到管道（`write` 是 async-signal-safe）。一个后台 dispatcher 线程 `read` 管道，在正常线程上下文中调用注册的回调。

接口与旧版完全兼容，调用方零改动：

```cpp
Signal::signal(SIGINT,  [&node] { node.stop(); });
Signal::signal(SIGTERM, [&node] { node.stop(); });
```

---

## 9. 改进 H — raft_demo 演示程序

### 9.1 为什么需要这个文件

端到端验证 Raft 选举能正常工作：启动后 150~300ms 内选出 Leader，kill Leader 后约 300ms 内重新选出。

### 9.2 编码实现步骤

**新建 `examples/src/raft_demo.cpp`，写入以下全部内容**

来自 [HISTORY/day32/examples/src/raft_demo.cpp](HISTORY/day32/examples/src/raft_demo.cpp)：

```cpp
// raft_demo.cpp —— Raft 选举演示（最多 10 节点）
//
// 用法（N 节点集群，默认 3）：
//   ./raft_demo --id 0 [--nodes N]
//   ./raft_demo --id 1 [--nodes N]
//   ...

#include "log/Logger.h"
#include "net/SignalHandler.h"
#include "raft/RaftNode.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    int myId  = -1;
    int nodes = 3;

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--id") == 0)
            myId = std::stoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--nodes") == 0)
            nodes = std::stoi(argv[i + 1]);
    }

    if (nodes < 2 || nodes > 10) {
        std::cerr << "错误：--nodes 必须在 2~10 范围内\n";
        return 1;
    }
    if (myId < 0 || myId >= nodes) {
        std::cerr << "用法：raft_demo --id <0.." << (nodes - 1)
                  << "> [--nodes " << nodes << "]\n";
        return 1;
    }

    Logger::setLogLevel(Logger::INFO);

    const std::vector<raft::Peer> allPeers = {
        {0, "127.0.0.1", 18901},
        {1, "127.0.0.1", 18902},
        {2, "127.0.0.1", 18903},
        {3, "127.0.0.1", 18904},
        {4, "127.0.0.1", 18905},
        {5, "127.0.0.1", 18906},
        {6, "127.0.0.1", 18907},
        {7, "127.0.0.1", 18908},
        {8, "127.0.0.1", 18909},
        {9, "127.0.0.1", 18910},
    };

    std::vector<raft::Peer> peers(allPeers.begin(), allPeers.begin() + nodes);
    uint16_t myPort = peers[myId].port;

    raft::RaftNode node(myId, peers, myPort);

    static std::atomic<bool> stopFlag{false};
    Signal::signal(SIGINT,  [] { stopFlag.store(true); });
    Signal::signal(SIGTERM, [] { stopFlag.store(true); });

    node.start();

    while (!stopFlag.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    node.stop();
    return 0;
}
```

---

## 10. 整体运行时理解

### 10.1 对象所有权与线程归属

```
main()（T_main）
 │
 └── raft::RaftNode node
      │
      ├── Eventloop loop_                    ← T_raft 独占
      │    ├── currentTerm_（atomic）         任意线程可读
      │    ├── state_（atomic）              任意线程可读
      │    ├── votedFor_ log_ electionEpoch_ 只在 T_raft 读写（无锁）
      │    └── peerClients_[N]              只在 T_raft 读写
      │         └── AsyncRpcClient
      │              ├── conn_（Connection） loop_ IO 归属
      │              ├── pending_           只在 T_raft 读写（无锁）
      │              └── backoff_           只在 T_raft 读写
      │
      ├── RpcServer rpcServer_              ← T_raft_rpc 线程
      │    └── TcpServer sub-reactor
      │         └── onMessage → handler(req, done)
      │              └── loop_.runInLoop ─────────────────▶ T_raft
      │
      ├── std::thread loopThread_           ─── T_raft
      └── std::thread rpcServerThread_      ─── T_raft_rpc
```

**跨线程交互边界**：

```
T_raft_rpc（入站 RPC）       T_raft（Raft 状态机）
    │                            │
    │ handleRequestVote          │
    │ loop_.runInLoop(lambda) ───▶
    │ [立刻返回]                 │ 读写 Raft 状态
    │                            │ done(reply) 调用
    │                            │
    │                            │ callAsync("RequestVote", ...)
    │                            │ loop_->runInLoop(lambda) ← 自身线程，立即执行
    │                            │ doCall → conn_->send(frame)
    │                            │ [io 事件] onResponse → cb(true, resp)
```

---

### 10.2 场景 A — 三节点集群初次选举，Node 2 最先超时成为 Leader

**场景设定**：3 节点集群（Node 0/1/2）全部刚 `start()`，`term=0`，全是 Follower。各节点 `resetElectionTimer()` 注册了随机超时：Node 0 ≈ 273ms，Node 1 ≈ 201ms，Node 2 ≈ 156ms。

---

#### 第 1 步：Node 2 的选举计时器触发（t=156ms）

打开 [HISTORY/day32/src/common/raft/RaftNode.cpp](HISTORY/day32/src/common/raft/RaftNode.cpp)，`electionTimerFired`：

```cpp
void RaftNode::electionTimerFired(uint64_t epoch) {
    if (epoch != electionEpoch_) return;
    if (state_.load() == State::Leader) return;
    becomeCandidate();
    startElection();
}
```

`epoch == electionEpoch_`（没有被更早的心跳 reset），`state_=Follower` → 进入 `becomeCandidate()`：

```cpp
void RaftNode::becomeCandidate() {
    currentTerm_.store(currentTerm_.load() + 1);  // 0 → 1
    state_.store(State::Candidate);
    votedFor_             = id_;   // = 2
    currentElectionVotes_ = 1;
    resetElectionTimer(); // 注册 ~230ms 后的 fallback 超时
}
```

**此刻状态快照（Node 2，T_raft）**：
```
currentTerm_ = 1
state_       = Candidate
votedFor_    = 2
electionVotes = 1
electionEpoch_ = 2（becomeCandidate 内 resetElectionTimer 递增了一次）
```

---

#### 第 2 步：Node 2 广播 RequestVote

打开 `startElection()`：

```cpp
void RaftNode::startElection() {
    uint64_t term     = 1;
    uint64_t lastIdx  = 0;   // log_.size()-1 = 0（只有哨兵）
    uint64_t lastTerm = 0;   // log_[0].term

    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;  // 跳过自己（id_=2）
        RequestVoteArgs args{1, 2, 0, 0};
        // peer 0: callAsync("RequestVote", {...}, cb, 150ms)
        // peer 1: callAsync("RequestVote", {...}, cb, 150ms)
    }
}
```

两个 `callAsync` 调用都在 T_raft 线程（`startElection` 在 loop_ 线程被调），`doCall` 立即分配 reqId、注册 150ms 超时、编帧、投递到 TCP（如果连接已建立）或 pendingFrames_（如果还在 connect 中）。

---

#### 第 3 步：Node 0 的 sub-reactor 收到 RequestVote 帧，投递到 loop_

打开 `handleRequestVote`（Node 0 的 rpcServerThread_）：

```cpp
void RaftNode::handleRequestVote(const std::string &reqJson, RpcServer::Done done) {
    RequestVoteArgs args = json::parse(reqJson).get<RequestVoteArgs>();
    // args = {term:1, candidateId:2, lastLogIndex:0, lastLogTerm:0}

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        // 现在在 T_raft（Node 0）
        if (args.term > currentTerm_.load()) becomeFollower(args.term);
        // 1 > 0 → becomeFollower(1)：currentTerm_=1, votedFor_=-1, resetElectionTimer
        // ...
    });
    // handler 立刻返回，sub-reactor 继续处理其他帧
}
```

`becomeFollower(1)` 被调用，Node 0 的 T_raft 内：

```cpp
void RaftNode::becomeFollower(uint64_t term) {
    state_.store(State::Follower);
    currentTerm_.store(1);
    votedFor_ = -1;
    resetElectionTimer();  // 重置选举计时器（之前的 273ms 被软取消）
}
```

随后三个投票条件全部满足（term=1 >= 1，votedFor_=-1，log 同样新），`grant=true`，`votedFor_=2`，调 `done({"term":1,"voteGranted":true})`。

`done` 闭包内 `connLoop->runInLoop` 把写帧动作投递回 sub-reactor 线程，响应帧发回 Node 2。

**此刻状态快照（Node 0，T_raft）**：
```
currentTerm_ = 1
state_       = Follower
votedFor_    = 2
electionEpoch_ 已更新（新的 resetElectionTimer 注册了新超时）
```

---

#### 第 4 步：Node 2 的 callAsync 回调触发，onVoteReply

Node 0 的响应帧到达 Node 2 的 `AsyncRpcClient::onResponse`，解帧后找到 `pending_[reqId=1]`，调 `cb(true, {"term":1,"voteGranted":true})`，即 `onVoteReply(electionTerm=1, peerId=0, ok=true, reply={voteGranted:true})`：

```cpp
void RaftNode::onVoteReply(uint64_t electionTerm, int peerId, bool ok, RequestVoteReply reply) {
    if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) return;
    // state_=Candidate ✓，term=1==1 ✓
    if (!ok) return;  // ok=true，不 return
    if (reply.term > currentTerm_.load()) { ... }  // 1 <= 1，不退位
    if (reply.voteGranted) {
        ++currentElectionVotes_;  // 1 → 2
        if (currentElectionVotes_ >= quorum_) becomeLeader();  // 2 >= 2 → 成为 Leader！
    }
}
```

`becomeLeader()` 立刻广播一次心跳压制其他节点：

**此刻状态快照（Node 2，T_raft）**：
```
currentTerm_  = 1
state_        = Leader
electionEpoch_ 已 ++（becomeLeader 里防止 electionTimerFired 触发）
```

---

#### 调用链总结

```
T_raft(Node2)            T_raft_rpc(Node0)       T_raft(Node0)
    │                        │                       │
    │ resetElectionTimer      │                       │
    │ [t=156ms timeout]       │                       │
    │ becomeCandidate()       │                       │
    │ startElection()         │                       │
    │ callAsync("RV") ────────TCP──────────────────────▶
    │ [立即返回，等回调]      │ handleRequestVote     │
    │                        │ loop_.runInLoop ───────▶
    │                        │ [立即返回]             │ becomeFollower(1)
    │                        │                       │ 三条件满足 → grant=true
    │                        │                       │ done(reply)
    │                        │ connLoop->runInLoop ◀──┤
    │                        │ 写帧 → TCP ─────────────▶ Node2的 onResponse
    │ onVoteReply(ok=true) ◀──────────────────────────┤
    │ votes=2 >= quorum=2     │                       │
    │ becomeLeader() !!       │                       │
    │ heartbeatTick()         │                       │
```

---

### 10.3 场景 B — Leader 宕机，集群重新选举

**场景设定**：Node 2 是 Leader（term=1），Node 0 和 Node 1 是 Follower，`electionTimeout` 每 50ms 被心跳重置。此时 `kill -9` Node 2 进程。

---

#### 第 1 步：Node 2 宕机，心跳停止，Node 1 选举计时器到期（约 200ms 后）

Node 1 的 `electionTimerFired` 触发（epoch 匹配，`state_=Follower`），`becomeCandidate()` → `startElection()`。

此时 Node 2 的 TCP 连接已关闭，Node 1 向 Node 2 的 `callAsync("RequestVote")` 会因 connect 失败或连接断开而触发 `onVoteReply(ok=false)` → 直接 return（不计票）。

---

#### 第 2 步：Node 0 投票给 Node 1

与场景 A 第 3 步类似，Node 0 的 `handleRequestVote` → `becomeFollower(2)`（term 从 1 → 2）→ 三条件满足 → `grant=true` → `done(reply)` → Node 1 收到回包。

---

#### 第 3 步：Node 1 票数 = 2 ≥ quorum = 2，becomeLeader

```
Node 1: Leader, term=2
Node 0: Follower, term=2
Node 2: dead（宕机）
```

两节点集群（Node 0 + Node 1）仍可正常运作——这就是 3 节点集群容忍 1 节点宕机的能力。

---

### 10.4 场景 C — AsyncRpcClient：connect 超时 + 指数退避

**场景设定**：Node 2 刚 start，向 Node 0（18901）发 callAsync，但 Node 0 尚未启动（connect 会一直 EINPROGRESS 直到超时）。

打开 [HISTORY/day32/src/common/rpc/AsyncRpcClient.cpp](HISTORY/day32/src/common/rpc/AsyncRpcClient.cpp)，追踪 `startConnectLocked`：

```
t=0ms：doCall 触发 startConnectLocked()
  backoff_.inBackoff(0) → false（首次，untilMs_=0 < 0? 不，0 < 0 false）
  socket(AF_INET, SOCK_STREAM) → fd=7
  fcntl(7, O_NONBLOCK)
  connect(7, 127.0.0.1:18901) → errno=EINPROGRESS
  connectFd_=7, connectEpoch_=1
  connectChannel_ 注册 EPOLLOUT
  loop_->runAfter(3.0, timer_epoch=1)
```

**此刻状态快照**：
```
state_         = kConnecting
connectFd_     = 7
connectEpoch_  = 1
pendingFrames_ = ["<RequestVote 帧>"]
```

```
t=3000ms：connect-timer 触发
  connEpoch=1 == connectEpoch_=1 ✓
  state_=kConnecting ✓
  backoff_.recordFailure(3000ms) → durationMs_=500, untilMs_=3500, failures_=1
    returns true（首次失败）→ LOG_WARN
  cleanupConnectChannel() → connectChannel_.reset(), connectFd_=-1
  state_ = kIdle
  failAllPending("connect timeout")
    → cb(false, "") 触发
    → Raft 层：onVoteReply(ok=false) → return（不计票）
```

**此刻状态快照**：
```
state_         = kIdle
backoff_.untilMs_   = 3500ms
backoff_.durationMs_= 500ms
pending_       = {}
```

```
t=3100ms：下一次 callAsync 触发 doCall → startConnectLocked()
  backoff_.inBackoff(3100) → 3100 < 3500 → true
  failAllPending("connect backoff") → cb(false, "") 直接失败，不发 TCP
```

```
t=3600ms：callAsync 触发，backoff_.inBackoff(3600) → 3600 < 3500 → false
  正常发起新 connect
  若又失败：durationMs_ = 1000ms，untilMs_ = 4600ms
```

每次连续失败，退避翻倍（500 → 1000 → 2000 → 4000 → ... → 30000ms），有效防止对宕机节点的 SYN 风暴。

---

## 11. 各模块职责速查表

| 模块/函数 | 所在线程 | 调用时机 | 职责一句话 |
|-----------|---------|---------|-----------|
| `RpcMessage::encode` | 调用方线程 | 发帧前 | nlohmann/json 序列化 method+payload → 12B头+JSON帧 |
| `RpcMessage::decode` | T_raft_rpc / T_raft | 收到字节后 | 粘包检测 + nlohmann/json 反序列化；失败返回 false |
| `RpcServer::onMessage` | T_raft_rpc | POLLIN | 追加字节 → 16MiB 守卫 → 循环解帧 → 构造 done → 调 handler |
| `RpcServer Done 闭包` | 任意线程 → T_raft_rpc | handler 完成后调用 | connLoop->runInLoop 切回 conn 归属线程，aliveFlag 判活写帧 |
| `AsyncRpcClient::callAsync` | 任意线程 | Raft RPC 发起时 | runInLoop 切回 loop_ 线程 → doCall |
| `AsyncRpcClient::doCall` | T_raft | callAsync 内 | 分配 reqId + 注册超时 + 编帧 + 投递（kConnected 直发/kIdle 触发 connect）|
| `AsyncRpcClient::startConnectLocked` | T_raft | doCall（kIdle 时）| 退避检查 → socket → O_NONBLOCK → connect → 处理三种结果 |
| `AsyncRpcClient::onConnected` | T_raft | connect 成功后 | ++epoch（让旧 timer 失效）→ Connection → enableInLoop → flushPendingFrames |
| `AsyncRpcClient::onResponse` | T_raft | EPOLLIN | 解帧 → reqId 路由到 pending_ 回调 |
| `AsyncRpcClient::failAllPending` | T_raft | stop/连接断/超时 | swap 出 pending_ 后逐一以 ok=false 触发回调 |
| `RaftNode::handleRequestVote` | T_raft_rpc → T_raft | 收到 RequestVote 帧 | 解析 → runInLoop → 三条件判断 → done(reply) |
| `RaftNode::handleAppendEntries` | T_raft_rpc → T_raft | 收到 AppendEntries 帧 | 解析 → runInLoop → term 检查 → resetElectionTimer → done(reply) |
| `RaftNode::becomeFollower/Candidate/Leader` | T_raft | 角色转换时 | 更新 term/state/votedFor_；Leader 立刻心跳；Candidate++term 自投票 |
| `RaftNode::resetElectionTimer` | T_raft | 任何需要重置计时器的时机 | ++epoch + runAfter(150~300ms) |
| `RaftNode::electionTimerFired` | T_raft | loop_->runAfter 回调 | epoch 双重守卫 → becomeCandidate → startElection |
| `RaftNode::startElection` | T_raft | electionTimerFired | 为每个 peer 调 callAsync("RequestVote", ..., 150ms) |
| `RaftNode::onVoteReply` | T_raft | callAsync 回调 | 过滤旧回包 → 累积票数 → 达 quorum 则 becomeLeader |
| `RaftNode::heartbeatTick` | T_raft | loop_->runEvery(50ms) | 仅 Leader 广播 AppendEntries；Follower/Candidate 直接 return |
| `RaftNode::onHeartbeatReply` | T_raft | callAsync 回调 | 发现更高 term 则退位（脑裂防护）|

---

## 12. 工程化

在 `CMakeLists.txt` 相对 day31 做以下更改：

来自 [CMakeLists.txt](CMakeLists.txt)（day32 新增/修改部分）：

```cmake
# ── 新增：nlohmann/json vendored（详见 §3）──────────────────────────
set(NLOHMANN_JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
...
target_include_directories(NetLib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
...
# ── 新增：raft_demo 可执行文件 ────────────────────────────────────
add_executable(raft_demo examples/src/raft_demo.cpp)
target_link_libraries(raft_demo NetLib pthread)
set_target_properties(raft_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${MCPP_EXAMPLE_DIR})
```

相对 day31，CMake 还**删除**了 GoogleTest 的 FetchContent 配置：day31 的 `CMakeLists.txt` 里有 `option(MCPP_ENABLE_TESTING ...)` + `include(FetchContent)` + `FetchContent_Declare(googletest ...)`，day32 暂时移除（`FetchContent_Declare` 在无网络环境下会阻塞构建，且 day32 没有新增 GTest 测试）。等 Day33 引入白盒测试时再按需接回。

---

## 13. 验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target raft_demo -j

# 三个终端分别启动
./build/examples/raft_demo --id 0 --nodes 3
./build/examples/raft_demo --id 1 --nodes 3
./build/examples/raft_demo --id 2 --nodes 3
```

**预期观察**：
1. 启动后 150~300ms 内，某个节点打印 `*** 成为 LEADER，term=1 ***`
2. 其余两个节点打印 `Follower → Follower（term=1）`（因 becomeFollower 有日志）
3. `Ctrl+C` 杀掉 Leader → 约 300ms 内剩余节点重新选出新 Leader（term=2）
4. 逐一启动时，未就绪对端的 connect 失败属于正常现象（退避机制会控制重试频率）

---

## 14. 局限与下一步

| 局限 | 描述 |
|------|------|
| **只有选举，没有日志复制** | AppendEntries 目前只携带 `term` 和 `leaderId`，作为纯心跳用。Day33 补 `prevLogIndex/entries/leaderCommit`，实现真正的日志同步 |
| **没有持久化** | `currentTerm_/votedFor_/log_` 全在内存，重启即丢失。Raft 要求 term 和 votedFor 落盘后再响应 RPC，Day33 接入简单的文件存储 |
| **没有单元测试** | 核心路径全靠 raft_demo 人工观察。Day33 引入 `RaftTestHarness`（单进程内直接调 handleRequestVote/handleAppendEntries，绕开 RPC，覆盖白盒分支）|
| **`wakeUp()` 接口未实现** | day36 引入 NodeAnnounce 机制时需要 `AsyncRpcClient::wakeUp()` 重置退避时钟——Day32 版本还没有这个接口 |

接下来 **Day32.5** 将把 `startElection` 和 `heartbeatTick` 里的"为每个 peer 发射 callAsync 等待回调"重构为 C++20 协程（`co_await`），让选举和日志复制的逻辑变成线性可读的顺序代码，而不是散落在多个回调函数里。
