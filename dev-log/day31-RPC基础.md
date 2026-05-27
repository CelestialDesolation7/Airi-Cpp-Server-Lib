# Day 31 — RPC 基础层 / 二进制帧协议 / RpcServer / RpcClient

## 目录

| 章节 | 内容 |
|------|------|
| [§1 引言](#1-引言) | 为什么要有独立 RPC 层；设计约束与选型说明 |
| [§2 改进 A — RpcMessage](#2-改进-a--rpcmessage二进制帧协议) | 新建 `RpcMessage.h/cpp`：12 字节定长头 + JSON payload，encode/decode，粘包处理 |
| [§3 改进 B — RpcServer](#3-改进-b--rpcserver监听端) | 新建 `RpcServer.h/cpp`：复用 TcpServer，方法名→处理函数注册表，连接上下文拼包缓冲区 |
| [§4 改进 C — RpcClient](#4-改进-c--rpcclient调用端) | 新建 `RpcClient.h/cpp`：短连接同步阻塞，SO_RCVTIMEO/SO_SNDTIMEO 超时保护 |
| [§5 改进 D — rpc_demo](#5-改进-d--rpc_demo端到端演示) | 新建 `examples/src/rpc_demo.cpp`：echo + add 两个方法，端到端验证 |
| [§6 整体运行时理解](#6-整体运行时理解) | 对象所有权图 + 完整 RPC 调用链追踪（发送/接收/超时路径）|
| [§7 各模块职责速查表](#7-各模块职责速查表) | 本日新增函数的线程/时机/职责一览 |
| [§8 工程化](#8-工程化) | CMakeLists.txt 变动；端口约定 |
| [§9 验证](#9-验证) | 构建命令 + 完整可运行演示步骤 |
| [§10 局限与下一步](#10-局限与下一步) | connect() 无独立超时等遗留问题；Day32 计划 |

---

## 本日变更文件一览

| 文件 | 变更 | 核心改动 |
|------|------|---------|
| `src/include/rpc/RpcMessage.h` | **新建** | 帧结构体 + encode/decode 声明 |
| `src/common/rpc/RpcMessage.cpp` | **新建** | 序列化 / 反序列化 + 粘包处理实现 |
| `src/include/rpc/RpcServer.h` | **新建** | RPC 监听端接口声明 |
| `src/common/rpc/RpcServer.cpp` | **新建** | 连接上下文、onMessage 循环解帧、handler 派发 |
| `src/include/rpc/RpcClient.h` | **新建** | RPC 调用端接口声明 |
| `src/common/rpc/RpcClient.cpp` | **新建** | 短连接同步阻塞实现，含收发循环和超时 |
| `examples/src/rpc_demo.cpp` | **新建** | echo + add 端到端演示 |
| `CMakeLists.txt` | **修改** | 新增 `rpc_demo` 可执行目标 |

---

## 1. 引言

### 1.1 为什么要有独立 RPC 层

Raft 算法要求节点之间互发两种消息：**RequestVote**（candidate 广播拉票，要求在 election timeout 150–300ms 内回复）和 **AppendEntries**（leader 广播日志条目，要求 quorum 在 50ms 心跳间隔内确认）。

最直接的替代方案是复用已有的 HTTP 层——把 RequestVote 参数 JSON 化，用 `POST /raft/request_vote` 发送。HTTP 可以工作，但引入了路由匹配、头部解析、Content-Type 协商等约 3~5µs 的额外开销，而且对于内部节点间通信，HTTP 的文本协议比二进制帧冗长。更重要的是：**项目目前没有 TcpClient，HTTP 层是服务端框架，不能直接作为客户端发出请求**。

因此需要一个独立的轻量 RPC 层：
- 服务端：复用现有 `TcpServer`，只需要加一个"方法名 → 处理函数"的注册表
- 客户端：用 POSIX raw socket，短连接，每次 `call()` 直接 connect→send→recv→close

### 1.2 设计约束

| 约束 | 来源 | 选型结果 |
|------|------|---------|
| 不引入外部依赖 | 项目保持精简 | 手写 JSON payload + 二进制定长头 |
| 无现有 TcpClient | 项目只有 TcpServer 侧 | RpcClient 直接 POSIX raw socket |
| RPC 频率低（< 100/s） | Raft 论文 §5.2 | 短连接同步阻塞，无需连接池 |
| 必须有超时保护 | 网络分区/peer 宕机 | `SO_RCVTIMEO` + `SO_SNDTIMEO` |

---

## 2. 改进 A — RpcMessage：二进制帧协议

### 2.1 为什么需要这个结构体

TCP 是字节流协议，没有天然的消息边界。如果我们直接发 JSON 字符串，接收方不知道一条消息在哪里结束、下一条在哪里开始（这就是所谓的"粘包"问题）。

解决粘包最简单的方式是**定长头**：每条消息前加一个固定大小的头部，头部里说明后面的 body 有多少字节。接收方先读够头部，知道 body 长度，再确认 body 到齐后才解析——否则等待更多数据。

帧格式（12 字节头 + N 字节 JSON body）：

```
 0               4               8               12      12+N
 ┌───────────────┬───────────────┬───────────────┬────── ... ──┐
 │   length (4B) │  msgType (4B) │   reqId  (4B) │  JSON body  │
 │  big-endian   │  big-endian   │  big-endian   │   (N bytes) │
 └───────────────┴───────────────┴───────────────┴────── ... ──┘
```

JSON body 格式固定为 `{"method":"<method>","body":<payload>}`。`reqId` 用于客户端匹配响应——发出请求 id=5，收到 id=5 的响应时才认为是自己的回包。

### 2.2 编码实现步骤

**第一步：新建 `src/include/rpc/RpcMessage.h`，写入以下全部内容**

来自 [HISTORY/day31/src/include/rpc/RpcMessage.h](HISTORY/day31/src/include/rpc/RpcMessage.h)：

```cpp
#pragma once
#include <cstdint>
#include <string>

struct RpcMessage {
    enum class Type : uint32_t {
        kRequest = 0,
        kResponse = 1,
        kOneWay = 2,
    };

    Type type{Type::kRequest};
    uint32_t reqId{0};
    std::string method;
    std::string payload;

    std::string encode() const;
    static bool decode(const char *data, int len, RpcMessage *out, int *consumed);
};
```

`Type` 是 `uint32_t` 底层——这样 `htonl` 转字节序时不需要额外 cast。`payload` 是调用方传进来的 JSON 字符串，`RpcMessage` 本身不解析它。`decode` 是 `static`：它不需要访问任何实例成员，只从字节流里读数据。

---

**第二步：新建 `src/common/rpc/RpcMessage.cpp`，写入 encode 和 decode**

来自 [HISTORY/day31/src/common/rpc/RpcMessage.cpp](HISTORY/day31/src/common/rpc/RpcMessage.cpp)：

```cpp
#include "rpc/RpcMessage.h"
#include <arpa/inet.h>
#include <cstring>

// encode 把 method + payload 打包成 JSON，再加上 12 字节二进制头
std::string RpcMessage::encode() const {
    // 1. 构造 JSON body
    //    格式：{"method":"<method>","body":<payload>}
    //    payload 本身已经是合法 JSON 字符串（调用方保证）
    std::string json;
    json += "{\"method\":\"";
    json += method;
    json += "\",\"body\":";
    json += payload;
    json += "}";

    // 2. 计算总长度
    uint32_t payloadLen = static_cast<uint32_t>(json.size());

    // 3. 构造 12 字节定长头（全部转网络字节序）
    uint32_t netLen     = htonl(payloadLen);
    uint32_t netType    = htonl(static_cast<uint32_t>(type));
    uint32_t netReqId   = htonl(reqId);

    // 4. 拼装完整帧
    std::string frame;
    frame.resize(12 + payloadLen);
    memcpy(frame.data() + 0, &netLen,   4);
    memcpy(frame.data() + 4, &netType,  4);
    memcpy(frame.data() + 8, &netReqId, 4);
    memcpy(frame.data() + 12, json.data(), payloadLen);
    return frame;
}
```

`encode` 不做任何分配除了最后一个 `frame`。注意 `json` 是手动拼接而非用 nlohmann/json——day31 故意不引入 JSON 库，因为这里的格式完全固定，用字符串操作足够，且更快。

```cpp
// decode 尝试从字节流里解析出一条完整 RpcMessage
// 返回 false = 数据不足或格式错误
bool RpcMessage::decode(const char *data, int len,
                        RpcMessage *out, int *consumed) {
    // 1. 至少要有 12 字节头
    if (len < 12) return false;

    uint32_t netLen, netType, netReqId;
    memcpy(&netLen,   data + 0, 4);
    memcpy(&netType,  data + 4, 4);
    memcpy(&netReqId, data + 8, 4);

    uint32_t payloadLen = ntohl(netLen);
    out->type   = static_cast<RpcMessage::Type>(ntohl(netType));
    out->reqId  = ntohl(netReqId);

    // 2. 检查 payload 是否到齐（粘包处理核心）
    if (len < 12 + static_cast<int>(payloadLen)) return false;

    // 3. 解析 JSON：只提取 method 和 body
    //    格式：{"method":"<method>","body":<payload>}
    std::string json(data + 12, payloadLen);

    // 简单字符串解析：找 "method":"..." 和 "body":...
    auto findStr = [&](const std::string &key) -> std::string {
        std::string needle = "\"" + key + "\":\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    };

    out->method = findStr("method");

    // body 的值是从 "body": 之后到最后一个 } 之前的内容
    std::string bodyKey = "\"body\":";
    auto bodyPos = json.find(bodyKey);
    if (bodyPos != std::string::npos) {
        out->payload = json.substr(bodyPos + bodyKey.size(),
                                   json.size() - (bodyPos + bodyKey.size()) - 1);
    }

    *consumed = 12 + static_cast<int>(payloadLen);
    return true;
}
```

`decode` 的核心是**两次长度检查**：先确认有 12 字节读头部，再确认 `12 + payloadLen` 字节读 body。任何一次不满足就返回 `false`，调用方继续等待更多数据——这就是定长头解决粘包的完整机制。

`findStr` 用简单字符串搜索提取 method。body 的提取略粗糙：取 `"body":` 之后到最后一个 `}` 之前的内容——这在 payload 本身不含 `"body":` 子串的情况下正确工作（Day31 的已知局限之一，见 §10）。

`*consumed` 告诉调用方这次消费了多少字节，调用方据此从缓冲区里 erase 掉这部分，循环解析下一条消息。

---

## 3. 改进 B — RpcServer：监听端

### 3.1 为什么需要这个类

如果没有 `RpcServer`，我们需要直接用 `TcpServer` 的 `onMessage` 回调，在回调里手动：①拼包，②解帧，③根据 method 名字分发到不同处理函数，④构造响应，⑤发送——这些代码会散落在每个 Raft handler 里，重复且难以维护。

`RpcServer` 把这些通用逻辑封装起来，对外只暴露一个接口：`addHandler("方法名", 处理函数)`。处理函数只需关心业务逻辑（RequestVote 同意还是拒绝），不需要关心字节流拼包。

### 3.2 编码实现步骤

**第一步：新建 `src/include/rpc/RpcServer.h`**

来自 [HISTORY/day31/src/include/rpc/RpcServer.h](HISTORY/day31/src/include/rpc/RpcServer.h)：

```cpp
#pragma once
#include "rpc/RpcMessage.h"
#include "net/TcpServer.h"
#include <functional>
#include <string>
#include <unordered_map>

// RpcServer：基于 TcpServer 的 RPC 监听端
//   用法：
//     RpcServer srv(loop, "0.0.0.0", 18901);
//     srv.addHandler("RequestVote", [](const std::string& req) -> std::string {
//         // req 是 JSON 字符串；返回 JSON 字符串作为响应
//         return "{\"voteGranted\":true}";
//     });
//     srv.start();

class RpcServer {
    public:
    // json 请求体处理函数
    using Handler = std::function<std::string(const std::string&)>;
    RpcServer(const std::string& ip, uint16_t port, int ioThreads = 1);

    void addHandler(const std::string& method, Handler handler);
    void start();
    void stop();

    private:
    void onMessage(Connection* conn);
    void onNewConn(Connection* conn);
    TcpServer server_;
    std::unordered_map<std::string, Handler> handlers_;
};
```

`Handler` 的签名是 `string → string`：输入请求 JSON，输出响应 JSON。这足够简单，让 Raft handler 专注于业务，不暴露任何 RPC 细节。`handlers_` 是 `unordered_map`，按方法名索引，在启动前注册完毕后就只读——不需要线程安全保护。

---

**第二步：新建 `src/common/rpc/RpcServer.cpp`**

来自 [HISTORY/day31/src/common/rpc/RpcServer.cpp](HISTORY/day31/src/common/rpc/RpcServer.cpp)：

```cpp
#include "rpc/RpcServer.h"
#include "net/Connection.h"
#include <string>

struct RpcConnCtx {
    std::string buf; // 已收到但未解析完整帧的字节数据
};

RpcServer::RpcServer(const std::string &ip, uint16_t port, int ioThreads)
    : server_([&] {
          TcpServer::Options opt;
          opt.listenIp = ip;
          opt.listenPort = port;
          opt.ioThreads = ioThreads;
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

void RpcServer::onNewConn(Connection *conn) {
    conn->setContext(RpcConnCtx{});
}

void RpcServer::onMessage(Connection *conn) {
    auto *ctx = conn->getContextAs<RpcConnCtx>();

    Buffer *buf = conn->getInputBuffer();
    size_t n = buf->readableBytes();
    ctx->buf.append(buf->peek(), n);
    buf->retrieve(n);

    while (true) {
        RpcMessage msg;
        int consumed = 0;
        bool ok = RpcMessage::decode(ctx->buf.data(), static_cast<int>(ctx->buf.size()), &msg,
                                     &consumed);
        if(!ok) break;
        ctx->buf.erase(0, consumed);
        if(msg.type != RpcMessage::Type::kRequest) continue;
        std::string responsePayload = "{\"error\":\"unknown method\"}";
        auto it = handlers_.find(msg.method);
        if(it != handlers_.end()){
            responsePayload = it->second(msg.payload);
        }
        RpcMessage resp;
        resp.type    = RpcMessage::Type::kResponse;
        resp.reqId   = msg.reqId;
        resp.method  = msg.method;
        resp.payload = responsePayload;
        conn->send(resp.encode());
    }
}
```

**`RpcConnCtx`** 是每个 TCP 连接的"私有拼包缓冲区"。TCP 可能把一条消息拆成多次回调（或把多条消息合并成一次回调），所以需要**每个连接独立保存已收到但尚未凑成完整帧的字节**。`conn->setContext` 是 TcpServer 框架提供的，把任意对象绑定到连接上；`getContextAs<T>()` 取出时强制转型。

**`onMessage` 的循环解帧逻辑**：

```
while (true) {
    decode(ctx->buf) → ok?
      false → break（等更多数据）
      true  → erase(consumed)
               msg.type == kRequest? → 找 handler → send(response.encode())
                                     → 继续下一条
}
```

每次 decode 成功后立刻 `erase(0, consumed)`，确保 `ctx->buf` 的头部始终是下一条消息的开始。若 decode 返回 false（数据不足），退出循环等待下次 onMessage 触发。这就是**零拷贝循环解帧**的标准写法。

`resp.reqId = msg.reqId` 把请求 ID 原样回传——客户端靠这个 ID 匹配响应，确认是自己请求的回包而非其他连接的响应。

---

## 4. 改进 C — RpcClient：调用端

### 4.1 为什么需要这个类

如果没有 `RpcClient`，Raft 节点发起 RequestVote 时需要手动：socket → setsockopt → connect → write 循环 → read 循环 → decode → close。这段代码约 50 行，要在每个 RPC 调用处重复。更糟的是，超时保护的设置很容易遗漏，导致 peer 宕机时 Raft 节点被永久阻塞。

`RpcClient` 把这 50 行封装成一个 `call(method, req, resp)` 调用，并在构造时统一设置超时参数。

**为什么用短连接而不是长连接？**

Raft RPC 频率很低（每次选举 < 5 个 RequestVote，心跳 < 10/s per peer），连接复用的收益接近零。短连接让代码简单很多：不需要管连接状态，不需要处理连接断开重连，每次 `call()` 都是完全独立的。

### 4.2 编码实现步骤

**第一步：新建 `src/include/rpc/RpcClient.h`**

来自 [HISTORY/day31/src/include/rpc/RpcClient.h](HISTORY/day31/src/include/rpc/RpcClient.h)：

```cpp
#pragma once
#include "rpc/RpcMessage.h"
#include <cstdint>
#include <string>

class RpcClient {
  public:
    RpcClient(const std::string &serverIp, uint16_t serverPort, int timeoutMs = 200);

    // 发起一次 RPC 调用
        //   method      ：方法名，须与 RpcServer::addHandler 注册的一致
        //   requestJson ：请求正文（JSON 字符串）
        //   responseJson：成功时填入响应正文
        //   返回 true = 调用成功
        bool call(const std::string &method, const std::string &requestJson,
                  std::string &responseJson);

  private:
  std::string ip_;
  uint16_t port_;
  int timeoutMs_;

  static uint32_t nextReqId();
};
```

`timeoutMs` 默认 200ms——比 Raft 选举超时下限（150ms）稍大，确保在一次选举超时内能完成 RPC 调用并得到结果（或失败）。`nextReqId()` 是 `static`，全局单调递增，用于匹配请求和响应。

---

**第二步：新建 `src/common/rpc/RpcClient.cpp`**

来自 [HISTORY/day31/src/common/rpc/RpcClient.cpp](HISTORY/day31/src/common/rpc/RpcClient.cpp)：

```cpp
#include "rpc/RpcClient.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static uint32_t g_reqId{0};
uint32_t RpcClient::nextReqId() { return ++g_reqId; }

RpcClient::RpcClient(const std::string &ip, uint16_t port, int timeoutMs)
    : ip_(ip), port_(port), timeoutMs_(timeoutMs) {}

bool RpcClient::call(const std::string &method, const std::string &requestJson,
                     std::string &responseJson) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    // 设置发送/接收超时，防止 Raft 选举被阻塞
    struct timeval tv;
    tv.tv_sec = timeoutMs_ / 1000;
    tv.tv_usec = (timeoutMs_ % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    ::inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    RpcMessage req;
    req.type = RpcMessage::Type::kRequest;
    req.reqId = nextReqId();
    req.method = method;
    req.payload = requestJson;
    std::string frame = req.encode();

    // ── 1. 发送完整帧（循环 write 直到所有字节发完）──────────────────────────
    const char *p = frame.data();
    int rem = static_cast<int>(frame.size());
    while (rem > 0) {
        int n = static_cast<int>(::write(fd, p, rem));
        if (n <= 0) { ::close(fd); return false; }
        p += n;
        rem -= n;
    }

    // ── 2. 接收响应帧（循环 read 直到能解析出一条完整消息）──────────────────
    std::string recvBuf;
    char tmp[4096];
    while (true) {
        int n = static_cast<int>(::read(fd, tmp, sizeof(tmp)));
        if (n <= 0) { ::close(fd); return false; }
        recvBuf.append(tmp, n);

        RpcMessage resp;
        int consumed = 0;
        if (RpcMessage::decode(recvBuf.data(),
                               static_cast<int>(recvBuf.size()),
                               &resp, &consumed)) {
            if (resp.reqId == req.reqId) {
                responseJson = resp.payload;
                ::close(fd);
                return true;
            }
        }
    }
}
```

**`SO_RCVTIMEO` / `SO_SNDTIMEO`** 的作用：让 `read()` 和 `write()` 在等待超过 `timeoutMs_` 后返回 `EAGAIN`（errno），而不是永久阻塞。这两个选项必须在 `connect()` 之前设置才能生效于整个连接生命周期。

**注意**：`SO_RCVTIMEO` / `SO_SNDTIMEO` 只影响 `read()`/`write()`，不影响 `connect()` 本身。`connect()` 的超时由操作系统的 TCP SYN 超时控制（约 75s）。这是 Day31 已知的局限（见 §10）。

**发送循环** (`while rem > 0`)：TCP `write()` 不保证一次写完所有字节（内核缓冲区满时只写部分），所以需要循环直到 `rem=0`。`SO_SNDTIMEO` 保证每次 `write()` 不会永久等待。

**接收循环**：每次 `read()` 后立刻尝试 `decode()`。如果解码成功且 `reqId` 匹配，说明是自己请求的响应，赋值 `responseJson` 后关闭 fd 返回 true。`reqId` 校验防止收到其他连接的延迟响应（虽然短连接里几乎不会发生，但这是正确做法）。

---

## 5. 改进 D — rpc_demo：端到端演示

### 5.1 为什么需要这个文件

演示和验证 RPC 层工作正常，同时作为 Raft 使用 RPC 的代码样例。注册两个方法：`echo`（原样返回请求）和 `add`（解析 `{"a":x,"b":y}` 返回 `{"result":z}`）。

### 5.2 编码实现步骤

**新建 `examples/src/rpc_demo.cpp`**

来自 [HISTORY/day31/examples/src/rpc_demo.cpp](HISTORY/day31/examples/src/rpc_demo.cpp)（文件头注释里的 "Day37" 是源码里的笔误，按原样摘抄）：

```cpp
// rpc_demo.cpp —— Day37 端到端验证
//
//   服务端：./rpc_demo --server [--port 18901]
//   客户端：./rpc_demo --client [--port 18901] [--n 5]

#include "net/SignalHandler.h"
#include "rpc/RpcClient.h"
#include "rpc/RpcServer.h"
#include <cstring>
#include <iostream>
#include <string>

static bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}
static int getIntArg(int argc, char** argv, const char* key, int def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return std::stoi(argv[i + 1]);
    return def;
}

static void runServer(uint16_t port) {
    RpcServer srv("0.0.0.0", port, /*ioThreads=*/1);

    srv.addHandler("echo", [](const std::string& req) -> std::string {
        return req;
    });
    srv.addHandler("add", [](const std::string& req) -> std::string {
        auto av = req.find("\"a\":"); auto bv = req.find("\"b\":");
        if (av == std::string::npos || bv == std::string::npos)
            return "{\"error\":\"bad input\"}";
        int a = std::stoi(req.substr(av + 4));
        int b = std::stoi(req.substr(bv + 4));
        return "{\"result\":" + std::to_string(a + b) + "}";
    });

    Signal::signal(SIGINT,  [&srv]() { srv.stop(); });
    Signal::signal(SIGTERM, [&srv]() { srv.stop(); });

    std::cout << "[server] RPC server listening on 0.0.0.0:" << port << "\n";
    srv.start();
    std::cout << "[server] stopped.\n";
}

static void runClient(uint16_t port, int n) {
    RpcClient client("127.0.0.1", port, /*timeoutMs=*/500);

    int ok = 0;
    for (int i = 0; i < n; ++i) {
        std::string req  = "{\"msg\":\"hello-" + std::to_string(i) + "\"}";
        std::string resp;
        if (client.call("echo", req, resp)) {
            std::cout << "[client] echo #" << i << " ✓  resp=" << resp << "\n";
            ++ok;
        } else {
            std::cout << "[client] echo #" << i << " ✗  (timeout/refused)\n";
        }
    }
    std::string addResp;
    if (client.call("add", "{\"a\":3,\"b\":4}", addResp))
        std::cout << "[client] add(3,4) => " << addResp << "\n";

    std::cout << "[client] done: " << ok << "/" << n << " success\n";
}

int main(int argc, char** argv) {
    uint16_t port = static_cast<uint16_t>(getIntArg(argc, argv, "--port", 18901));
    int      n    = getIntArg(argc, argv, "--n", 5);

    if (hasFlag(argc, argv, "--server")) {
        runServer(port);
    } else if (hasFlag(argc, argv, "--client")) {
        runClient(port, n);
    } else {
        std::cerr << "Usage:\n"
                  << "  rpc_demo --server [--port 18901]\n"
                  << "  rpc_demo --client [--port 18901] [--n 5]\n";
        return 1;
    }
    return 0;
}
```

服务端同进程支持两个方法，通过 `--server` / `--client` 标志区分运行模式。`Signal::signal` 注册 SIGINT/SIGTERM 让 `Ctrl+C` 能优雅停机。

---

## 6. 整体运行时理解

### 6.1 对象所有权与线程归属

```
main()（T_main）
 │
 ├── RpcServer srv
 │    ├── TcpServer server_              ← T_srv_main（accept 新连接）
 │    │    └── sub-reactor 线程          ← T_srv_sub（IO 读写）
 │    │         └── Connection（per fd）
 │    │              └── RpcConnCtx.buf  ← T_srv_sub 独占（粘包缓冲区）
 │    └── handlers_（unordered_map）     ← 启动前写入，之后只读，无并发问题
 │
 └── RpcClient client（调用方）
      └── call()                         ← 调用方线程（同步阻塞）
           └── raw fd（短连接）          ← call() 内部创建和销毁
```

**线程边界**：

```
调用方线程                    T_srv_sub（服务端）
────────────                  ───────────────────────────
call()                        onMessage(conn)
  socket()                      ctx->buf.append(data)
  connect()                     decode() → 找 handler
  write 循环                    handler(payload) → 响应 JSON
  read 循环       ←──TCP────    conn->send(resp.encode())
  decode()
  return true/false
```

- 客户端侧：完全在调用方线程，同步阻塞，没有跨线程通信。
- 服务端侧：`onMessage` 在 T_srv_sub 线程，`handlers_` 是只读的，`ctx->buf` 是 per-connection 的，都不需要锁。
- `handlers_` 的 key/value 在 `start()` 之前全部写入，之后只读——多个 T_srv_sub 线程并发调用 `handlers_.find()` 是安全的（不修改 unordered_map 的并发读是 C++ 标准保证的线程安全操作）。

---

### 6.2 场景 A — 一次完整的 echo RPC 调用

**场景设定**：客户端调用 `call("echo", {"msg":"hello-0"}, resp)`，服务端已注册 `echo` handler（原样返回），本机回环 RTT ≈ 0.2ms，`timeoutMs=500`。

---

#### 第 1 步：encode() 构造帧

打开 [HISTORY/day31/src/common/rpc/RpcMessage.cpp](HISTORY/day31/src/common/rpc/RpcMessage.cpp)，`encode` 函数：

```cpp
std::string json;
json += "{\"method\":\"";
json += method;          // "echo"
json += "\",\"body\":";
json += payload;         // {"msg":"hello-0"}
json += "}";
// json = {"method":"echo","body":{"msg":"hello-0"}}  (38字节)

uint32_t payloadLen = static_cast<uint32_t>(json.size());  // = 38
uint32_t netLen     = htonl(38);      // = 0x00000026 (大端)
uint32_t netType    = htonl(0);       // kRequest=0
uint32_t netReqId   = htonl(1);       // reqId=1

frame.resize(12 + 38);  // = 50 字节
memcpy(frame.data() + 0,  &netLen,   4);
memcpy(frame.data() + 4,  &netType,  4);
memcpy(frame.data() + 8,  &netReqId, 4);
memcpy(frame.data() + 12, json.data(), 38);
```

帧的前 12 字节（十六进制）：
```
00 00 00 26  00 00 00 00  00 00 00 01  7b 22 6d 65 ...
└── len=38 ┘ └ type=0(Req)┘ └ reqId=1 ┘ └── JSON start ──
```

**此刻状态快照**：
```
frame.size()  = 50
frame[0..3]   = 0x00000026（大端，body 38 字节）
frame[4..7]   = 0x00000000（kRequest）
frame[8..11]  = 0x00000001（reqId=1）
frame[12..]   = {"method":"echo","body":{"msg":"hello-0"}}
```

---

#### 第 2 步：socket() + setsockopt() + connect()

打开 [HISTORY/day31/src/common/rpc/RpcClient.cpp](HISTORY/day31/src/common/rpc/RpcClient.cpp)，`call` 函数头部：

```cpp
int fd = ::socket(AF_INET, SOCK_STREAM, 0);

struct timeval tv;
tv.tv_sec = 500 / 1000;          // = 0
tv.tv_usec = (500 % 1000) * 1000; // = 500000µs = 0.5s
::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

if (::connect(fd, &addr, sizeof(addr)) < 0) {
    ::close(fd);
    return false;
}
```

`SO_RCVTIMEO` 和 `SO_SNDTIMEO` 必须在 `connect()` 之前设置。设置后，`read()` 超过 500ms 未收到数据会返回 -1（errno=EAGAIN），`write()` 超过 500ms 写不出去同样返回 -1——触发各自循环里的 `close(fd); return false`。

**此刻状态快照**：
```
fd            = 5（或某个非负整数）
SO_RCVTIMEO   = 500ms
SO_SNDTIMEO   = 500ms
connect()     = 成功（RTT ≈ 0.2ms 完成三次握手）
```

---

#### 第 3 步：发送循环

```cpp
const char *p = frame.data();
int rem = 50;  // frame.size()
while (rem > 0) {
    int n = ::write(fd, p, rem);  // 本机回环一次写完，n=50
    if (n <= 0) { ::close(fd); return false; }
    p += n;    // p 移动 50
    rem -= n;  // rem = 0
}
// rem=0，退出循环
```

本机回环一次 `write()` 通常能发完全部 50 字节。在高延迟或内核发送缓冲区满的情况下，`write()` 可能只写部分字节（如 30），`rem` 变为 20，循环再次写剩余的 20 字节——这是 TCP 写的正确处理方式。

**此刻状态快照**：
```
rem           = 0（50 字节已全部写入内核发送缓冲区）
服务端内核    接收缓冲区有 50 字节等待读取
```

---

#### 第 4 步：服务端 onMessage 触发，循环解帧

打开 [HISTORY/day31/src/common/rpc/RpcServer.cpp](HISTORY/day31/src/common/rpc/RpcServer.cpp)，`onMessage`：

```cpp
void RpcServer::onMessage(Connection *conn) {
    auto *ctx = conn->getContextAs<RpcConnCtx>();

    Buffer *buf = conn->getInputBuffer();
    size_t n = buf->readableBytes();  // = 50
    ctx->buf.append(buf->peek(), n);  // ctx->buf 从 "" → 50字节
    buf->retrieve(n);                 // 清空 TcpBuffer

    while (true) {
        RpcMessage msg;
        int consumed = 0;
        bool ok = RpcMessage::decode(ctx->buf.data(), 50, &msg, &consumed);
        // ok=true, consumed=50
        if(!ok) break;
        ctx->buf.erase(0, 50);  // ctx->buf 从 50字节 → ""
        // msg.type == kRequest ✓
        auto it = handlers_.find("echo");  // 找到
        responsePayload = it->second(msg.payload);  // = {"msg":"hello-0"}

        RpcMessage resp;
        resp.type    = kResponse;
        resp.reqId   = 1;
        resp.method  = "echo";
        resp.payload = {"msg":"hello-0"};
        conn->send(resp.encode());  // 发出 50 字节响应帧
        // 第二次循环：ctx->buf.size()=0 < 12 → decode()=false → break
    }
}
```

`ctx->buf.append` 把所有新到达字节追加到拼包缓冲区，然后 `buf->retrieve(n)` 清空 TcpBuffer——把字节的"所有权"从框架的缓冲区转移到 `ctx->buf`，避免重复消费。

**此刻状态快照**：
```
ctx->buf.size()  = 0（已全部消费）
resp.encode()    = 50 字节帧（type=kResponse, reqId=1）
conn 已发出      = 50 字节响应帧
```

---

#### 第 5 步：客户端接收循环，校验 reqId，返回

```cpp
std::string recvBuf;
char tmp[4096];
while (true) {
    int n = ::read(fd, tmp, sizeof(tmp));  // n = 50
    recvBuf.append(tmp, 50);              // recvBuf.size() = 50

    RpcMessage resp;
    int consumed = 0;
    if (RpcMessage::decode(recvBuf.data(), 50, &resp, &consumed)) {
        // resp.type = kResponse, resp.reqId = 1 == req.reqId = 1 ✓
        if (resp.reqId == req.reqId) {
            responseJson = resp.payload;  // = {"msg":"hello-0"}
            ::close(fd);
            return true;
        }
    }
}
```

**此刻状态快照**：
```
responseJson  = {"msg":"hello-0"}
fd            已关闭
return value  = true
```

---

#### 第 6 步（error path）：peer 无响应，SO_RCVTIMEO 触发

假设服务端进程已 crash，`read()` 在 500ms 后返回 -1（errno=EAGAIN）：

```cpp
int n = ::read(fd, tmp, sizeof(tmp));  // n = -1, errno = EAGAIN
if (n <= 0) {
    ::close(fd);
    return false;  // ← 超时路径
}
```

**此刻状态快照**：
```
n             = -1（EAGAIN/EWOULDBLOCK）
fd            已关闭
return value  = false
调用方处理    = Raft 层将此 peer 的投票视为拒绝/失败
```

---

#### 调用链总结图

```
调用方线程                          T_srv_sub
    │                                   │
    │  call("echo", req, resp)          │
    │    socket() + setsockopt()        │
    │    connect() ─────TCP─────────────▶
    │    write 循环 ──50B───────────────▶
    │    read 循环                      │ onMessage() 触发
    │    （等待响应）                    │   ctx->buf.append(50B)
    │                                   │   decode() → ok
    │                                   │   handler("echo") → payload
    │                                   │   conn->send(resp.encode())
    │    read() ←──50B──────────────────│
    │    decode() → ok                  │
    │    reqId 匹配 → return true       │
```

---

## 7. 各模块职责速查表

| 模块/函数 | 所在线程 | 调用时机 | 职责一句话 |
|-----------|---------|---------|-----------|
| `RpcMessage::encode` | 调用方线程 | `call()` 发送前 | 把 method+payload 打包成 12B头 + JSON 帧 |
| `RpcMessage::decode` | 调用方线程 / T_srv_sub | `read()` 后 / `onMessage` 内 | 从字节流解析一条完整消息；`*consumed` 告知消费字节数 |
| `RpcServer::onNewConn` | T_srv_sub | 新 TCP 连接建立 | 在 conn 上附加 `RpcConnCtx{}`，初始化拼包缓冲区 |
| `RpcServer::onMessage` | T_srv_sub | POLLIN 事件 | 追加字节 → 循环解帧 → 派发 handler → 发响应 |
| `RpcServer::addHandler` | T_main（启动前） | 初始化 | 注册方法名到处理函数的映射 |
| `RpcClient::call` | 调用方线程 | Raft RPC 发起时 | 短连接同步阻塞：connect→encode→发送→接收→decode→close |
| `RpcClient::nextReqId` | 调用方线程 | `call()` 内部 | 生成单调递增 reqId，用于响应匹配 |

---

## 8. 工程化

### CMakeLists.txt 变动

`GLOB_RECURSE "src/common/*.cpp"` 已自动覆盖 `src/common/rpc/*.cpp`，无需手动追加源文件到 `NetLib`。

新增 `rpc_demo` 可执行目标（在根 `CMakeLists.txt` 中追加）：

```cmake
add_executable(rpc_demo examples/src/rpc_demo.cpp)
target_link_libraries(rpc_demo NetLib pthread)
set_target_properties(rpc_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${MCPP_EXAMPLE_DIR})
```

### 端口约定

| 用途 | 端口 |
|------|------|
| Raft RPC Node 0/1/2/3/4 | 19001–19005 |
| HTTP KV 接口 Node 0/1/2/3/4 | 8901–8905 |

---

## 9. 验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target rpc_demo -j

# 启动服务端（后台）
./build/examples/rpc_demo --server --port 18901 &
SERVER_PID=$!
sleep 0.3

# 客户端调用 3 次 echo + 1 次 add
./build/examples/rpc_demo --client --port 18901 --n 3

kill $SERVER_PID
```

预期输出：

```
[server] RPC server listening on 0.0.0.0:18901
[client] echo #0 ✓  resp={"msg":"hello-0"}
[client] echo #1 ✓  resp={"msg":"hello-1"}
[client] echo #2 ✓  resp={"msg":"hello-2"}
[client] add(3,4) => {"result":7}
[client] done: 3/3 success
[server] stopped.
```

3/3 全部通过，`add(3,4)` 正确返回 7。

---

## 10. 局限与下一步

| 局限 | 描述 |
|------|------|
| `connect()` 无独立超时 | `SO_RCVTIMEO/SO_SNDTIMEO` 只影响 `read/write`，`connect()` 超时由 TCP 栈控制（约 75s）。peer 完全不可达时调用方会长时间阻塞 |
| `g_reqId` 非线程安全 | `static uint32_t g_reqId{0}` 是非原子递增，多线程并发调用 `call()` 存在 data race。Raft 通常在单线程发 RPC，暂可接受 |
| 无连接池 | 每次 `call()` 都 connect+close，适合低频 Raft RPC（< 100/s），不适合高频场景 |
| JSON 解析极简 | `decode()` 用字符串搜索提取 method/body，对 payload 中含 `"body":` 子串的嵌套 JSON 可能误匹配；Day37 引入 Protobuf 彻底解决 |
| `RpcClient` 同步阻塞 | 在 EventLoop 线程调用会阻塞整个 reactor（所有其他连接无法收发）。Day32 会重构为异步版本，Raft 节点在 loop_ 线程用 `callAsync` 发出请求后立即返回 |

接下来 Day32 会在今天的 RPC 层之上实现 **Raft 选主**：新建 `RaftTypes.h`（State/LogEntry/RequestVote/AppendEntries 消息类型）、`RaftNode.h/cpp`（三角色状态机 + 随机选举超时）；同时将 `RpcClient` 重构为异步版本（`callAsync`），避免在 EventLoop 线程同步阻塞。
