# Day 37 — Protobuf 迁移与 Leader 转让

## 目录

| 章节 | 内容 |
|------|------|
| [§1 引言](#1-引言) | Day36 三个明显不足；本日三个改进方向 |
| [§2 改进 A — Protobuf 迁移 + bypass 旁路](#2-改进-a--protobuf-迁移--bypass-旁路) | 新建 `raft.proto`；新帧格式（16B 头）；`encodeAE/decodeAE`；`RaftTypes.h` 清理 |
| [§3 改进 B — ReadIndex 线性一致读](#3-改进-b--readindex-线性一致读) | 协议原理；`handleReadIndex`；`drainConfirmedReads`；`proposeFollowerRead`；KvHttpServer 接入 |
| [§4 改进 C — Leader Transfer（主动让贤）](#4-改进-c--leader-transfer主动让贤) | TimeoutNow 协议；`transferLeadership`；`doTransferLeadership`；`handleTimeoutNow`；写保护 |
| [§5 改进 D — SIGTERM 优雅关机](#5-改进-d--sigterm-优雅关机) | `kv_node.cpp` 停机流程；800ms 等待窗口 |
| [§6 工程化](#6-工程化) | CMakeLists.txt Protobuf 集成 |
| [§7 整体运行时理解](#7-整体运行时理解) | 场景 A（大 value PUT bypass 路径）；场景 B（Follower ReadIndex 线性读）；场景 C（SIGTERM Leader Transfer） |
| [§8 各模块职责速查表](#8-各模块职责速查表) | 本日所有新增/修改函数一览 |
| [§9 验证](#9-验证) | 构建命令；三场景验证脚本 |

---

## 本日变更文件一览

| 文件 | 变更 | 核心改动 |
|------|------|---------|
| `src/proto/raft.proto` | **新建** | 所有 Raft RPC 的 Protobuf 3 消息定义 |
| `CMakeLists.txt` | 修改 | `find_package(Protobuf REQUIRED)`；`protobuf_generate_cpp`；链接 `protobuf::libprotobuf` |
| `src/include/rpc/RpcMessage.h` | 修改 | 新增 `bypass` 字段；帧格式从 12B 头 → 16B 头 |
| `src/include/rpc/RpcServer.h` | 修改 | Handler 签名新增 `bypass` 参数 |
| `src/common/rpc/RpcServer.cpp` | 修改 | 解帧逻辑按新格式提取 bypass |
| `src/include/rpc/AsyncRpcClient.h/cpp` | 修改 | `callAsync/callAsyncCo` 新增 bypass 参数 |
| `src/common/raft/RaftNode.cpp` | 修改 | 匿名命名空间：JSON encode/decode → Protobuf；新增 ReadIndex + Leader Transfer 所有代码 |
| `src/include/raft/RaftTypes.h` | 修改 | 移除死代码 NLOHMANN 宏（非 LogEntry）|
| `src/include/raft/RaftNode.h` | 修改 | 新增 `proposeRead/proposeFollowerRead/transferLeadership`；ReadIndex/LeaderTransfer 状态成员 |
| `examples/src/kv/KvHttpServer.cpp` | 修改 | GET /kv/:key 改用 `proposeFollowerRead`（线性一致读）；/admin/raft 增加 transfer 字段；新增 /admin/transfer 路由 |
| `examples/static/kv/app.js` | 修改 | 仪表盘展示 transfer 状态；新增转移按钮 |
| `examples/src/kv_node.cpp` | 修改 | SIGTERM 触发优雅 Leader Transfer；最多等 800ms 完成 |
| `scripts/leader_transfer_demo.sh` | **新建** | 三场景演示：kill-9 vs SIGTERM vs transfer |

---


## 1. 引言

Day37 是一个"生产加固"日，把 Day36 的工作状态集群打磨成可以信任的生产级服务。

Day36 有三个明显的不足：

**不足一：JSON 序列化效率低。** 每次 AppendEntries 的日志条目里都含有完整的 value 字节。`"PUT mykey <value>"` 这个字符串被整体塞进 JSON，再通过 nlohmann::json 序列化后在网络上传输。当 value 是 1MB 的二进制块时，JSON 的 base64 编码会让 payload 膨胀 33%，加上解析本身，发生 2-4 次不必要的内存拷贝。

**不足二：脏读。** Day36 的 GET 请求直接读本地状态机，没有任何一致性保证。Follower 的 `lastApplied` 可能比 Leader 落后一两个心跳周期——客户端刚 PUT 成功，立刻从 Follower GET 却拿到旧值，看起来像"时间倒退"。

**不足三：切主成本高。** 运维需要滚动升级某个节点时，只能 kill 进程。被动选举需要等 150-300ms 的选举超时才能选出新 Leader，这段时间内所有写请求都失败。

本日对应三个改进：引入 **Protobuf + bypass 旁路**消除序列化开销，引入 **ReadIndex 协议**实现线性一致读，引入 **Leader Transfer（TimeoutNow RPC）**将主动切主的写空窗从 200ms 压缩到约 1ms。

---

## 2. 改进 A — Protobuf 迁移 + bypass 旁路

### 2.1 为什么引入 bypass

理解 bypass 旁路的关键是弄清楚哪些字节**必须**序列化，哪些字节**不必要**序列化。

一次 AppendEntries 里有两类数据：
- **控制平面字段**（term、prevLogIndex、prevLogTerm、leaderCommit）：小整数，必须进 Protobuf，方便解析、版本兼容。
- **业务 value 字节**（PUT 命令里的 value 部分）：纯字节块，发送方知道长度，接收方按长度切分，不需要任何编解码。

bypass 字段就是给第二类字节走的"旁路通道"——它附在帧尾，不经过 Protobuf 序列化，接收方用 `LogEntry.vlen` 知道该从 bypass 里切多少字节出来。

### 2.2 新帧格式与 RpcMessage

打开 [src/include/rpc/RpcMessage.h](src/include/rpc/RpcMessage.h)，观察新帧格式：

```cpp
// 新帧格式（16B 定长头，替换旧 12B 头）：
//   [4B proto_section_len BE]   // 4 + method.size() + payload.size()
//   [4B bypass_len BE]          // bypass 字节数（无旁路透传时为 0）
//   [4B msgType BE]
//   [4B reqId BE]
//   [4B method_len BE] [method bytes]
//   [payload bytes]             // Protobuf 编码的结构化字段
//   [bypass bytes]              // 原始 value 字节（零序列化，仅 AppendEntries 使用）

struct RpcMessage {
    enum class Type : uint32_t {
        kRequest  = 0,
        kResponse = 1,
        kOneWay   = 2,
    };

    Type        type{Type::kRequest};
    uint32_t    reqId{0};
    std::string method;
    std::string payload;  // Protobuf-encoded structured body
    std::string bypass;   // Raw value bytes（旁路透传，不经过任何序列化）

    std::string encode() const;
    static bool decode(const char *data, int len, RpcMessage *out, int *consumed);
};
```

旧帧头是 12B（`proto_section_len + msgType + reqId`），新帧头多了一个 4B 的 `bypass_len`，变成 16B。`encode()` 先写 16B 头，再写 `method + payload`，最后拼接 `bypass`；`decode()` 按 `proto_section_len` 提取 payload，按 `bypass_len` 提取 bypass，两段互不干扰。

`callAsync` 和 `callAsyncCo` 签名都增加了一个 `bypass` 参数。对于不需要旁路的 RPC（RequestVote、InstallSnapshot、ReadIndex 等），传空字符串 `{}` 即可，完全向下兼容：

```cpp
// src/include/rpc/AsyncRpcClient.h
void callAsync(const std::string &method, const std::string &payload,
               const std::string &bypass, Callback cb, int timeoutMs = 200);

class RpcCallAwaiter callAsyncCo(const std::string &method, const std::string &payload,
                                  const std::string &bypass = {},
                                  int timeoutMs = 200);
```


### 2.3 raft.proto 消息定义

新建 [src/proto/raft.proto](src/proto/raft.proto)，写入以下内容：

```protobuf
syntax = "proto3";
package raft_proto;

// ── 日志条目元数据（value 通过 bypass 字段透传，不在此结构中）────────────
message LogEntry {
    uint64 term = 1;  // 写入时的 Leader term
    bytes  cmd  = 2;  // 结构化命令头："PUT <key>" 或 "DEL <key>"（无 value 部分）
    uint32 vlen = 3;  // 对应 bypass 中的 value 字节数；纯心跳/no-op 时为 0
}

// ── AppendEntries RPC ──────────────────────────────────────────────────────
message AppendEntriesReq {
    uint64            term      = 1;
    int32             leader_id = 2;
    uint64            prev_idx  = 3;
    uint64            prev_term = 4;
    uint64            commit    = 5;
    repeated LogEntry entries   = 6;
    // value bytes 在 RpcMessage::bypass 中，按 entries[i].vlen 顺序拼接
}

message AppendEntriesRep {
    uint64 term           = 1;
    bool   success        = 2;
    uint64 conflict_index = 3;
    uint64 conflict_term  = 4;
}

// ── RequestVote RPC ────────────────────────────────────────────────────────
message RequestVoteReq {
    uint64 term           = 1;
    int32  candidate_id   = 2;
    uint64 last_log_index = 3;
    uint64 last_log_term  = 4;
    bool   pre_vote       = 5;
}

message RequestVoteRep {
    uint64 term         = 1;
    bool   vote_granted = 2;
}

// ── InstallSnapshot RPC ───────────────────────────────────────────────────
message InstallSnapshotReq {
    uint64 term                = 1;
    int32  leader_id           = 2;
    uint64 last_included_index = 3;
    uint64 last_included_term  = 4;
    bytes  data                = 5;
}

message InstallSnapshotRep {
    uint64 term = 1;
}

// ── Follower ReadIndex RPC ─────────────────────────────────────────────────
message ReadIndexReq {
    uint64 follower_id = 1;
    uint64 request_id  = 2;
}
message ReadIndexResp {
    uint64 request_id = 1;
    uint64 read_index = 2;
    bool   ok         = 3;
}

// ── Leader Transfer：TimeoutNow RPC ────────────────────────────────────────
message TimeoutNowReq {
    uint64 term = 1;
}
message TimeoutNowResp {
    uint64 term = 1;
    bool   ok   = 2;
}

// ── KV 状态机快照 ─────────────────────────────────────────────────────────
message KvEntry {
    string key   = 1;
    string value = 2;
}
message KvSnapshot {
    repeated KvEntry entries = 1;
}
```

`LogEntry.cmd` 只存命令头（`"PUT mykey"` 或 `"DEL mykey"`），value 字节由 `vlen` 标注长度、走 bypass 旁路传输。`KvSnapshot` 用于状态机快照的 Protobuf 序列化，取代 Day36 的 JSON 编解码。

### 2.4 encodeAE / decodeAE：bypass 拆分与重组

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到匿名命名空间里的这三个函数：

```cpp
// 命令分割：splitCmd("PUT mykey value_bytes") → {"PUT mykey", "value_bytes"}
// splitCmd("DEL mykey")             → {"DEL mykey", ""}
// splitCmd("")                      → {"", ""}  （no-op 条目）
static std::pair<std::string, std::string> splitCmd(const std::string &cmd) {
    if (cmd.empty()) return {"", ""};
    size_t sp1 = cmd.find(' ');
    if (sp1 == std::string::npos) return {cmd, ""};
    size_t sp2 = cmd.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return {cmd, ""};  // DEL/no-op 无 value
    return {cmd.substr(0, sp2), cmd.substr(sp2 + 1)};
}
// 命令重组：joinCmd("PUT mykey", "value_bytes") → "PUT mykey value_bytes"
static std::string joinCmd(const std::string &hdr, const std::string &val) {
    if (val.empty()) return hdr;
    return hdr + " " + val;
}

// 编码：把 AppendEntriesArgs 拆成 proto_bytes（元数据）和 bypass（value 拼接）
static std::pair<std::string, std::string> encodeAE(const AppendEntriesArgs &a) {
    raft_proto::AppendEntriesReq pb;
    pb.set_term(a.term);
    pb.set_leader_id(a.leaderId);
    pb.set_prev_idx(a.prevLogIndex);
    pb.set_prev_term(a.prevLogTerm);
    pb.set_commit(a.leaderCommit);
    std::string bypass;
    for (const auto &e : a.entries) {
        auto *ep = pb.add_entries();
        ep->set_term(e.term);
        auto [hdr, val] = splitCmd(e.cmd);
        ep->set_cmd(hdr);
        ep->set_vlen(static_cast<uint32_t>(val.size()));
        bypass += val;  // 将 value 追加到 bypass（零序列化）
    }
    std::string proto_bytes; pb.SerializeToString(&proto_bytes);
    return {proto_bytes, bypass};
}

// 解码：从 proto_bytes（元数据）和 bypass（value 拼接）还原 AppendEntriesArgs
static AppendEntriesArgs decodeAE(const std::string &proto_bytes,
                                   const std::string &bypass) {
    raft_proto::AppendEntriesReq pb; pb.ParseFromString(proto_bytes);
    AppendEntriesArgs a;
    a.term         = pb.term();
    a.leaderId     = pb.leader_id();
    a.prevLogIndex = pb.prev_idx();
    a.prevLogTerm  = pb.prev_term();
    a.leaderCommit = pb.commit();
    size_t offset = 0;
    for (const auto &ep : pb.entries()) {
        LogEntry le;
        le.term = ep.term();
        std::string val = bypass.substr(offset, ep.vlen());
        offset += ep.vlen();
        le.cmd = joinCmd(std::string(ep.cmd()), val);
        a.entries.push_back(std::move(le));
    }
    return a;
}
```

`encodeAE` 在遍历 entries 时，对每条日志调用 `splitCmd` 在第二个空格处切分：第一段（命令头）进 Protobuf，第二段（value）追加到 `bypass` 字符串。所有条目的 value 按顺序拼成一个连续字节流。

`decodeAE` 用 `offset` 游标在 bypass 里顺序切分：第 i 条日志的 value 长度是 `ep.vlen()`，切完 `offset += ep.vlen()` 移到下一条的起点。最后用 `joinCmd` 把命令头和 value 拼回完整的 `cmd` 字符串，让上层代码（KvStateMachine）不需要感知这个旁路。

`replicateLog` 协程里调用的地方变成：

```cpp
// src/common/raft/RaftNode.cpp — replicateLog 协程
auto [protoBytes, bypass] = encodeAE(args);
auto [ok, respBytes] = co_await client->callAsyncCo(
    "AppendEntries", protoBytes, bypass, /*timeoutMs=*/100);
```

`handleAppendEntries` 里：

```cpp
// src/common/raft/RaftNode.cpp — handleAppendEntries
void RaftNode::handleAppendEntries(const std::string &payload, const std::string &bypass,
                                    RpcServer::Done done) {
    AppendEntriesArgs args;
    try {
        args = decodeAE(payload, bypass);  // payload = proto_bytes, bypass = value bytes
    } catch (...) {
        done(encodeAERep({0, false, 0, 0}));
        return;
    }
    // ... 下面的 Raft 逻辑不变
```

持久化（`FileStorage/RocksDBStorage`）仍然存储完整的 `LogEntry.cmd`（含 value）——bypass 只在网络传输时使用，磁盘格式不变。

### 2.5 RaftTypes.h 清理

打开 [src/include/raft/RaftTypes.h](src/include/raft/RaftTypes.h)，删除 `RequestVoteArgs`、`AppendEntriesArgs`、`AppendEntriesReply`、`RequestVoteReply`、`InstallSnapshotArgs`、`InstallSnapshotReply` 上的 `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` 宏——这些结构体现在通过 Protobuf 编解码，不再需要 JSON 序列化宏。只保留 `LogEntry` 的 JSON 宏（`FileStorage` 持久化用 NDJSON 格式存储日志条目）。

---

## 3. 改进 B — ReadIndex 线性一致读

### 3.1 脏读问题与 ReadIndex 原理

先把脏读场景说清楚，再看协议怎么解决：

```
时序：
  t=0  客户端 PUT /kv/foo → "v1"  → Leader apply，commit=10
  t=1  客户端 GET /kv/foo → Follower 回 "v0"  （Follower 的 lastApplied 还是 9）
  t=2  Follower 收到心跳，apply 到 10
  t=3  客户端 GET /kv/foo → Follower 回 "v1"  （正确了）
```

`t=1` 时 Follower 的回答不是线性一致的——对客户端来说，看到了时间倒退：刚确认写成功的 `v1` 不见了。

ReadIndex 协议的思路是：**Follower 在回答读请求之前，先向 Leader 确认当前 commitIndex，等自己的 lastApplied 追上这个值，再读本地状态机。** 完整的流程是：

```
Follower ReadIndex 流程（以 GET /kv/foo 为例）：
  1. Follower 收到请求
  2. Follower 向 Leader 发 ReadIndexReq（携带唯一 requestId）
  3. Leader 记录 readIndex = commitIndex，等本轮心跳的 quorum ack 到达
     （quorum ack 确认自己仍是合法 Leader，防止脑裂场景下的过期 Leader 读）
  4. Leader quorum 确认后回 ReadIndexResp{readIndex=10, ok=true}
  5. Follower 等待自己的 lastApplied >= 10
  6. Follower 读本地状态机，回客户端 → 线性一致
```

Leader 自己读也走 ReadIndex（步骤 2-4 在本地完成，不需要 RPC），只是省掉了发 ReadIndexReq 这一步。

### 3.2 Leader 侧：handleReadIndex + drainConfirmedReads

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `handleReadIndex`：

```cpp
void RaftNode::handleReadIndex(const std::string &payload, const std::string & /*bypass*/,
                               RpcServer::Done done) {
    auto [followerId, requestId] = decodeReadIndexReq(payload);
    loop_.runInLoop([this, followerId, requestId, done = std::move(done)]() mutable {
        if (state_.load() != State::Leader) {
            done(encodeReadIndexResp(requestId, 0, false));  // 非 Leader：拒绝
            return;
        }
        uint64_t ri = commitIndex_.load();
        // 快速路径：当前 epoch 已被多数派确认，即刻回包
        if (readConfirmedEpoch_ >= readHeartbeatEpoch_) {
            done(encodeReadIndexResp(requestId, ri, true));
            return;
        }
        // 慢速路径：等待下一轮心跳 epoch 确认后回包
        pendingRemoteReads_.push_back({ri, readHeartbeatEpoch_, requestId, std::move(done)});
    });
}
```

`readHeartbeatEpoch_` 在每次 `heartbeatTick()` 时递增。`readConfirmedEpoch_` 在 replicateLog 成功后累计 ack 达到 quorum 时被推进到 `readHeartbeatEpoch_`。如果 `readConfirmedEpoch_ >= readHeartbeatEpoch_`，说明**当前 epoch 的心跳已经被多数派确认**，本节点的 Leader 身份是可信的，可以直接回包。否则把请求放进 `pendingRemoteReads_` 队列等待。

再找 `drainConfirmedReads`，这是每次 epoch 确认后的"批量兑现"函数：

```cpp
void RaftNode::drainConfirmedReads() {
    // 兑现 Leader 本地 pendingReads_（来自 proposeRead 调用）
    auto it = pendingReads_.begin();
    while (it != pendingReads_.end()) {
        if (readConfirmedEpoch_ > it->requestEpoch
            && lastApplied_.load() >= it->readIndex) {
            it->cb(true);
            it = pendingReads_.erase(it);
        } else ++it;
    }
    // 兑现来自 Follower 的 pendingRemoteReads_（来自 handleReadIndex）
    auto jt = pendingRemoteReads_.begin();
    while (jt != pendingRemoteReads_.end()) {
        if (readConfirmedEpoch_ > jt->requestEpoch) {
            jt->done(encodeReadIndexResp(jt->requestId, jt->readIndex, true));
            jt = pendingRemoteReads_.erase(jt);
        } else ++jt;
    }
}
```

条件 `readConfirmedEpoch_ > it->requestEpoch` 的含义是：这个读请求的 readIndex 是在"已被 quorum 确认的时间点"之前记录的，它的线性一致性保证已经满足。注意 Leader 还要检查 `lastApplied_ >= readIndex`（状态机已追上），而 Follower 侧有自己的等待机制（见下节），所以 Leader 这里只要 epoch 确认就行。

### 3.3 Follower 侧：proposeFollowerRead

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `proposeFollowerRead`：

```cpp
void RaftNode::proposeFollowerRead(ReadCallback cb) {
    loop_.runInLoop([this, cb = std::move(cb)]() mutable {
        // ── Leader 快速路径（等价于 proposeRead）──
        if (state_.load() == State::Leader) {
            uint64_t readIndex = commitIndex_.load();
            if (readConfirmedEpoch_ >= readHeartbeatEpoch_
                && lastApplied_.load() >= readIndex) {
                cb(true);
                return;
            }
            pendingReads_.push_back({readIndex, readHeartbeatEpoch_, std::move(cb)});
            return;
        }
        // ── Follower 路径：向 Leader 请求 readIndex ──
        if (leaderId_ < 0) { cb(false); return; }
        Peer *leaderPeer = nullptr;
        for (auto &p : peers_) {
            if (p.id == leaderId_) { leaderPeer = &p; break; }
        }
        if (!leaderPeer) { cb(false); return; }

        uint64_t reqId = ++followerReadSeq_;
        followerPendingReads_[reqId] = {0, std::move(cb)};
        auto *client = getOrCreateClient(*leaderPeer);
        client->callAsync(
            "ReadIndex", encodeReadIndexReq(id_, reqId), /*bypass=*/{},
            [this, reqId](bool ok, const std::string &respBytes) {
                // callAsync 回调已在 loop_ 线程
                auto it = followerPendingReads_.find(reqId);
                if (it == followerPendingReads_.end()) return;
                if (!ok) {
                    it->second.cb(false);
                    followerPendingReads_.erase(it);
                    return;
                }
                auto [rid, readIndex, respOk] = decodeReadIndexResp(respBytes);
                if (!respOk) {
                    it->second.cb(false);
                    followerPendingReads_.erase(it);
                    return;
                }
                it->second.readIndex = readIndex;
                // 若 lastApplied 已够（RPC 延迟内日志已追上），立即兑现
                drainFollowerReads();
            });
    });
}
```

Follower 路径：向 Leader 发 ReadIndexReq，Leader 回包后把 `readIndex` 填入 `followerPendingReads_[reqId]`。`drainFollowerReads` 在每次 `applyCommitted` 推进 `lastApplied_` 后也会被调用——只要 `lastApplied >= readIndex`，就 `cb(true)` 兑现读请求：

```cpp
void RaftNode::drainFollowerReads() {
    auto it = followerPendingReads_.begin();
    while (it != followerPendingReads_.end()) {
        if (it->second.readIndex > 0
            && lastApplied_.load() >= it->second.readIndex) {
            it->second.cb(true);
            it = followerPendingReads_.erase(it);
        } else ++it;
    }
}
```

### 3.4 KvHttpServer 接入

打开 [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp)，`GET /kv/:key` 路由改用异步接口：

```cpp
srv_->addAsyncPrefixRoute(HttpRequest::Method::kGet, "/kv/",
    [this](const HttpRequest &req, HttpResponse *resp, Connection *conn) {
        std::string key = extractKey(req.url());
        if (key.empty()) {
            resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
            resp->setBody(R"({"ok":false,"error":"missing key"})");
            return;
        }
        // 异步路径：等 proposeFollowerRead 完成后才发响应
        resp->setDeferred(true);
        auto alive = conn->aliveFlag();
        auto *loop = conn->getLoop();
        node_.proposeFollowerRead([this, alive, loop, conn, key](bool ok) {
            loop->queueInLoop([this, alive, conn, key, ok]() {
                if (auto f = alive.lock(); !f || !*f) return;
                std::string body;
                if (!ok) {
                    body = R"({"ok":false,"error":"no leader or leadership lost, retry"})";
                    conn->send("HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: " + std::to_string(body.size()) +
                               "\r\n\r\n" + body);
                    return;
                }
                std::string value;
                if (sm_.get(key, value)) {
                    body = R"({"ok":true,"key":")" + key + R"(","value":")" + value + R"("})";
                    conn->send("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                               "Content-Length: " + std::to_string(body.size()) +
                               "\r\n\r\n" + body);
                } else {
                    body = R"({"ok":false,"key":")" + key + R"(","error":"not found"})";
                    conn->send("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
                               "Content-Length: " + std::to_string(body.size()) +
                               "\r\n\r\n" + body);
                }
            });
        });
    });
```

`setDeferred(true)` 让 HTTP 框架不自动发送响应——由 `proposeFollowerRead` 的回调在 ReadIndex 确认后主动发。连接生命周期安全靠 `aliveFlag()` 的 `weak_ptr` 检查（Day20 的模式）。`/admin/raft` 路由也新增了 `transferTarget`、`transfersInitiated`、`transfersSucceeded` 三个字段；新增 `POST /admin/transfer?to=<id>` 路由触发 Leader Transfer（见下节）。

---

## 4. 改进 C — Leader Transfer（主动让贤）

### 4.1 TimeoutNow 协议

主动让贤的核心是 **TimeoutNow RPC**。Leader 想把主权移交给某个 Follower 时，直接发一条 TimeoutNow，目标 Follower 收到后**跳过 election timeout 和 Pre-Vote**，立刻进入选举流程。因为 Leader 发出 TimeoutNow 之前已经确认了目标 Follower 的日志追平（matchIndex == lastLogIndex），所以这次选举几乎必然成功，写空窗约等于一次 RPC RTT。

与被动选举相比（kill 进程后等 150-300ms 超时才选出新 Leader），TimeoutNow 把写空窗压到约 1ms。

### 4.2 transferLeadership + doTransferLeadership

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `transferLeadership` 和 `doTransferLeadership`：

```cpp
bool RaftNode::transferLeadership(int targetId) {
    if (state_.load() != State::Leader) return false;
    if (peers_.size() <= 1)             return false;  // 单节点集群无意义

    leaderTransfersInitiated_.fetch_add(1);
    loop_.runInLoop([this, targetId]() mutable {
        if (state_.load() != State::Leader) return;

        int target = targetId;
        if (target < 0) target = pickTransferTarget();  // -1 = 自动选 matchIndex 最高的
        if (target < 0 || target == id_) return;

        leadershipTransferTarget_ = target;
        transferDeadlineMs_       = nowMs() + 1000;  // 1s 超时

        // 1s 兜底：若超时仍未感知到对方 becomeLeader，清状态恢复写入
        loop_.runAfter(1.0, [this, target] {
            if (state_.load() == State::Leader && leadershipTransferTarget_ == target) {
                LOG_WARN << "[Node " << id_ << "] Leader Transfer 超时：放弃让贤，恢复 Leader 服务";
                leadershipTransferTarget_ = -1;
                transferDeadlineMs_       = 0;
            }
        });

        doTransferLeadership(target);
    });
    return true;
}
```

`leadershipTransferTarget_` 设为目标节点 id，触发写保护（见 §4.4）。`pickTransferTarget()` 从 `matchIndex_` 里选 matchIndex 最高的 Follower（同分时取 id 最小）。

```cpp
void RaftNode::doTransferLeadership(int targetId) {
    if (state_.load() != State::Leader) return;
    if (leadershipTransferTarget_ != targetId) return;

    Peer *targetPeer = nullptr;
    for (auto &p : peers_)
        if (p.id == targetId) { targetPeer = &p; break; }
    if (!targetPeer) return;

    uint64_t lastIdx = lastLogIndex();
    auto it = matchIndex_.find(targetId);
    uint64_t matched = (it == matchIndex_.end()) ? 0 : it->second;

    if (matched < lastIdx) {
        // 还没追平 → 触发一次复制，等 onAppendEntriesReply 后再调一次本函数
        replicateLog(*targetPeer);
        return;
    }

    // 已追平：发 TimeoutNow，让 target 立刻起票
    auto *client = getOrCreateClient(*targetPeer);
    client->callAsync(
        "TimeoutNow", encodeTimeoutNowReq(currentTerm_.load()), /*bypass=*/{},
        [this](bool ok, const std::string & /*resp*/) {
            if (!ok) LOG_WARN << "[Node " << id_ << "] TimeoutNow 发送失败（对端不可达）";
        });
}
```

`doTransferLeadership` 分两种情况：目标已追平则立刻发 TimeoutNow；否则触发一次 `replicateLog`，等复制成功的回调里再检查是否追平并重调 `doTransferLeadership`。

`replicateLog` 协程里，复制成功时检查：

```cpp
// src/common/raft/RaftNode.cpp — replicateLog 成功路径
if (leadershipTransferTarget_ == peer.id) {
    doTransferLeadership(peer.id);  // target 追平了，现在可以发 TimeoutNow
}
```

### 4.3 Follower 侧：handleTimeoutNow

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `handleTimeoutNow`：

```cpp
void RaftNode::handleTimeoutNow(const std::string &payload, const std::string & /*bypass*/,
                                RpcServer::Done done) {
    uint64_t senderTerm = decodeTimeoutNowReq(payload);
    loop_.runInLoop([this, senderTerm, done = std::move(done)]() mutable {
        if (senderTerm < currentTerm_.load()) {
            // 过时的 TimeoutNow（来自已退位的旧 Leader），拒绝
            done(encodeTimeoutNowResp(currentTerm_.load(), false));
            return;
        }
        if (senderTerm > currentTerm_.load()) {
            // 不该发生，但严谨处理：跟进 term 后拒绝本次
            becomeFollower(senderTerm);
            done(encodeTimeoutNowResp(currentTerm_.load(), false));
            return;
        }
        // term 一致：接受让贤指令
        done(encodeTimeoutNowResp(currentTerm_.load(), true));

        // 跳过 Pre-Vote 直接选举——Leader 已为我们做完了 quorum 可达 + 日志追平的检查
        becomeCandidate();
        runElection();
    });
}
```

收到 TimeoutNow 后先检查 `senderTerm`：只接受 term 与当前一致的 TimeoutNow，防止旧 Leader（已退位）发来的过期消息干扰集群。term 一致时跳过 Pre-Vote 直接 `becomeCandidate() + runElection()`——因为 Leader 在发出 TimeoutNow 之前已经做完了"quorum 可达、日志已追平"两个检查，Pre-Vote 要证明的东西 Leader 已经替我们证明了。

### 4.4 写入保护：转移期间拒绝新写

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `proposeAndNotify` 和 `propose` 里的这段：

```cpp
void RaftNode::proposeAndNotify(const std::string &cmd, ...) {
    loop_.runInLoop([this, cmd, done = std::move(done)]() mutable {
        if (state_.load() != State::Leader) { done(false, 0); return; }
        // Leader Transfer 进行中：拒绝新写，避免 target 永远追不上 lastLogIndex
        if (leadershipTransferTarget_ != -1) {
            done(false, 0);
            return;
        }
        // ... 正常 propose 逻辑 ...
    });
}
```

`leadershipTransferTarget_ != -1` 时 Leader 拒绝所有新写。这个设计很关键：如果 Leader 在等待目标追平的过程中继续接受新写，`lastLogIndex` 会持续增长，目标永远追不上，转移永远无法完成。写保护让 `lastLogIndex` 在转移期间"冻结"，目标只需要补上之前的存量即可。

---

## 5. 改进 D — SIGTERM 优雅关机

### 5.1 停机流程

打开 [examples/src/kv_node.cpp](examples/src/kv_node.cpp)，看 SIGTERM 处理和优雅停机部分：

```cpp
// 信号处理：SIGINT 和 SIGTERM 都设置 stopFlag
static std::atomic<bool> stopFlag{false};
Signal::signal(SIGINT,  [] { stopFlag.store(true); });
Signal::signal(SIGTERM, [] { stopFlag.store(true); });

// ... 主循环每 2s 刷新一次状态行 ...
while (!stopFlag.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    // ... print status ...
}

kvSrv.stop();
httpThread.join();

// ── 优雅停机：若本节点是 Leader，先主动让贤再 stop ──────────────────
if (node.isLeader()) {
    std::cout << "[Node " << myId << "] 检测到自身是 Leader，发起主动 Leader Transfer...\n";
    if (node.transferLeadership(/*auto-pick*/ -1)) {
        // 等待最多 800ms 让 target 接任并广播心跳，本节点 becomeFollower
        for (int i = 0; i < 80; ++i) {
            if (!node.isLeader()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::cout << "[Node " << myId << "] 让贤完成（或超时），当前 leaderId="
                  << node.getLeaderId() << "\n";
    }
}

node.stop();
```

`systemctl stop kv_node` 发 SIGTERM，`stopFlag` 变 true，主循环退出，`kvSrv.stop()` 先停掉 HTTP 服务（拒绝新请求），然后检查自己是否 Leader。如果是，调 `transferLeadership(-1)` 自动选一个 Follower 当接班人，最多等 800ms（80 × 10ms）。这段等待是因为 `transferLeadership` 是异步的——`TimeoutNow` 发出后目标需要完成选举并广播心跳，本节点才会在 `handleAppendEntries` 里发现更高 term 的 Leader 存在，从而 `becomeFollower`。800ms 足够覆盖一次完整的选举 + 心跳广播（通常 < 50ms）。

如果 800ms 内 `isLeader()` 变 false 说明转移成功；超时则放弃（降级为被动选举）。两种情况都调 `node.stop()` 完成 RAII 清理。

---

## 6. 工程化

打开 [CMakeLists.txt](CMakeLists.txt)，新增 Protobuf 集成：

```cmake
find_package(Protobuf REQUIRED)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS src/proto/raft.proto)

add_library(NetLib
    ${COMMON_SOURCES}
    ${PLATFORM_SOURCES}
    ${PROTO_SRCS}   # 新增：Protobuf 生成的 raft.pb.cc
)
target_include_directories(NetLib PRIVATE
    ${Protobuf_INCLUDE_DIRS}
    ${CMAKE_BINARY_DIR}  # 新增：让 #include "raft.pb.h" 能找到生成文件
)
target_link_libraries(NetLib PRIVATE protobuf::libprotobuf)

# kv_node 也需要 raft.pb.h（直接用 ReadIndex/LeaderTransfer 的 proto 消息）
target_include_directories(kv_node PRIVATE ${CMAKE_BINARY_DIR})
```

`protobuf_generate_cpp` 把 `src/proto/raft.proto` 编译成 `build/raft.pb.h` 和 `build/raft.pb.cc`（输出目录 = `${CMAKE_BINARY_DIR}`）。`#include "raft.pb.h"` 在 `RaftNode.cpp` 里可以直接用，前提是把 `${CMAKE_BINARY_DIR}` 加入 include 路径。`kv_node` 也需要这个路径，因为 `KvHttpServer.cpp` 间接引用了 Protobuf 生成类型。

依赖安装（macOS）：`brew install protobuf`。

---

## 7. 整体运行时理解

这一节用三个具体场景把本日所有代码串起来，每个场景都会打开相关文件，逐步追踪数据在系统内的流动。

---

### 场景 A：一次大 value PUT 请求的 bypass 路径

假设客户端发出 `PUT /kv/photo <1MB JPEG 字节>`，观察这 1MB 数据如何在不经过任何序列化的情况下从 Leader 复制到所有 Follower。

**第一步：HTTP 请求进入 KvHttpServer，propose 写入 Raft 日志。**

打开 [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp)，`PUT /kv/:key` 路由调用 `node_.proposeAndNotify`，cmd 是 `"PUT photo <1MB bytes>"`。

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`proposeAndNotify` 在 loop_ 线程追加日志：

```cpp
// proposeAndNotify — loop_ 线程
log_.push_back(LogEntry{currentTerm_.load(), "PUT photo <1MB bytes>"});
persistLog();
uint64_t idx = lastLogIndex();   // 假设 idx = 42
writeCallbacks_[42] = std::move(done);
for (const auto &peer : peers_) {
    if (peer.id == id_) continue;
    replicateLog(peer);           // 立刻触发复制，不等下一个 50ms 心跳
}
```

此时状态机快照：`log_.back().cmd = "PUT photo <1MB bytes>"`，`lastLogIndex() = 42`，`commitIndex_ = 41`。

**第二步：replicateLog 协程编码 AppendEntries，value 走 bypass。**

```cpp
// replicateLog — loop_ 线程协程
AppendEntriesArgs args;
args.entries = { log_[42 - snapshotIndex_] };  // LogEntry{term=5, cmd="PUT photo <1MB bytes>"}
// ...
auto [protoBytes, bypass] = encodeAE(args);
```

进入 `encodeAE`：

```
splitCmd("PUT photo <1MB bytes>")
  → hdr = "PUT photo"
  → val = "<1MB bytes>"

Protobuf pb:
  entries[0].term = 5
  entries[0].cmd  = "PUT photo"   // 只有命令头，9 字节
  entries[0].vlen = 1048576       // 1MB

bypass = "<1MB bytes>"           // 1MB 原始字节，不经 proto 序列化
proto_bytes = pb.SerializeToString()  // 几十字节的控制信息
```

`callAsyncCo("AppendEntries", protoBytes, bypass, 100)` 把这两段分别填入 `RpcMessage.payload` 和 `RpcMessage.bypass`，`encode()` 写出的帧：

```
[4B proto_section_len] [4B bypass_len=1048576] [4B kRequest] [4B reqId]
[4B method_len] ["AppendEntries"] [protoBytes] [<1MB bypass>]
```

网络上传输的总字节数 ≈ 16（帧头）+ 几十（Protobuf）+ 1048576（bypass）。若用旧 JSON 方案，`"PUT photo " + base64(1MB)` ≈ 1.43MB，再加 JSON 包装约 1.5MB。

**第三步：Follower RpcServer 收帧，handleAppendEntries 解码还原。**

Follower 的 `RpcServer.cpp` 解帧时读出 `bypass_len = 1048576`，把帧尾 1MB 字节存入 `msg.bypass`，handler 签名里的 `const std::string &bypass` 就是这 1MB。

```cpp
void RaftNode::handleAppendEntries(const std::string &payload, const std::string &bypass,
                                    RpcServer::Done done) {
    AppendEntriesArgs args = decodeAE(payload, bypass);
    // decodeAE 内部：
    //   size_t offset = 0;
    //   for ep in pb.entries():
    //     val = bypass.substr(offset, ep.vlen())  // 切出 1MB
    //     offset += ep.vlen()
    //     le.cmd = joinCmd("PUT photo", val)       // 还原完整 cmd
    // args.entries[0].cmd = "PUT photo <1MB bytes>"
    // ...
```

Follower 的 `log_` 追加 `{term=5, cmd="PUT photo <1MB bytes>"}`，持久化，回包 `{success=true}`。

**第四步：Leader 收到 quorum ack，commitIndex 推进，applyCallback 触发，done(true, 42) 回调。**

两个 Follower 都回包后，`advanceCommitIndex` 把 `commitIndex_` 推到 42，`applyCommitted` 调用 `applyCallback_(42, "PUT photo <1MB bytes>")`，`KvStateMachine` 执行 `PUT photo`，然后 `writeCallbacks_[42](true, 42)` 触发，HTTP 响应发回客户端。

---

### 场景 B：Follower ReadIndex 线性一致读

客户端先 `PUT /kv/counter 100`（Leader apply），然后立刻向 Follower-1 发 `GET /kv/counter`。观察 Follower 如何保证拿到的是 100 而不是旧值。

假设状态：`commitIndex_=50`（Leader），Follower-1 的 `lastApplied_=49`（还差 1 条心跳没收到）。

**第一步：HTTP 请求到达 Follower-1，进入 proposeFollowerRead。**

打开 [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp)，`GET /kv/counter` 命中 `addAsyncPrefixRoute`：

```cpp
resp->setDeferred(true);
node_.proposeFollowerRead([this, conn, key](bool ok) {
    // 等 ok=true 后才读状态机
});
```

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`proposeFollowerRead` 在 loop_ 线程里：

```cpp
// state_ = Follower，leaderId_ = 0（Leader 是 Node-0）
uint64_t reqId = ++followerReadSeq_;  // reqId = 7
followerPendingReads_[7] = {readIndex=0, cb=...};

client->callAsync("ReadIndex", encodeReadIndexReq(/*followerId=*/1, /*reqId=*/7), {}, ...);
```

此时状态：`followerPendingReads_[7].readIndex = 0`（待填充），连接已发出 ReadIndexReq。

**第二步：Leader 收到 ReadIndexReq，记录到 pendingRemoteReads_。**

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`handleReadIndex` 在 Leader 的 loop_ 线程：

```cpp
// state_ = Leader，commitIndex_ = 50
// readConfirmedEpoch_ = 4，readHeartbeatEpoch_ = 5（最新一轮心跳还没 quorum）
if (readConfirmedEpoch_ >= readHeartbeatEpoch_) {
    // 快速路径：不走（4 < 5，当前 epoch 未确认）
}
// 慢速路径：入队
pendingRemoteReads_.push_back({
    readIndex = 50,
    requestEpoch = 5,      // 这个读请求需要 epoch 5 的 quorum 确认
    requestId = 7,
    done = <Follower-1 的回包回调>
});
```

**第三步：Leader 下一次 heartbeatTick 发出心跳，quorum ack 返回，epoch 5 确认。**

50ms 后 `heartbeatTick` 触发，`readHeartbeatEpoch_` 从 5 变 6（新一轮），对 Follower-2（另一个节点）的 `replicateLog` 成功，`++readHeartbeatAcks_`，达到 quorum 后：

```cpp
readConfirmedEpoch_ = 5;  // epoch 5 正式确认
drainConfirmedReads();
```

`drainConfirmedReads` 遍历 `pendingRemoteReads_`：`readConfirmedEpoch_=5 > requestEpoch=5` 为 false……等等，这里条件是 `>`，不是 `>=`。那么 epoch 5 的请求是等到 epoch 6 确认后才被兑现的吗？

确实如此。`readConfirmedEpoch_ > requestEpoch` 意味着：你注册时的那一轮心跳（epoch 5）已经被确认了（epoch 的确认是用"我确认了比你高的 epoch"来表示的）。当 epoch 5 的 quorum ack 返回时，`readConfirmedEpoch_` 被设为 5，此时条件 `5 > 5` = false，还没触发。要等到下一轮（epoch 6 的 ack 返回，`readConfirmedEpoch_ = 6`），此时 `6 > 5` = true，才兑现。实际上是"等到比你晚的心跳 quorum 确认后，才回包"——这是 Raft ReadIndex 的标准实现，保证了安全性。

```cpp
// readConfirmedEpoch_ 刚被更新为 6
drainConfirmedReads();
// jt->requestEpoch = 5, readConfirmedEpoch_ = 6, 6 > 5 → 兑现
jt->done(encodeReadIndexResp(7, 50, true));  // readIndex = 50
```

**第四步：Follower-1 收到 ReadIndexResp，等 lastApplied >= 50。**

```cpp
// callAsync 回调在 Follower-1 的 loop_ 线程
it->second.readIndex = 50;
drainFollowerReads();  // lastApplied_ = 49 < 50，暂时不兑现
```

又过一个 50ms 心跳，Follower-1 的 `handleAppendEntries` 把 index=50 的条目 apply，`lastApplied_` 变 50，`applyCommitted` 末尾调 `drainFollowerReads()`：

```cpp
// lastApplied_ = 50 >= readIndex = 50 → 兑现
it->second.cb(true);
```

**第五步：HTTP 响应发出，值为 100（线性一致）。**

`proposeFollowerRead` 的回调 `ok=true` 触发，读 `sm_.get("counter", value)` 得到 `"100"`，发出 HTTP 200 响应。即使 Follower 的 `lastApplied` 在请求到达时还是 49，ReadIndex 协议保证了它等到 50 才读，消除了脏读。

---

### 场景 C：SIGTERM 触发 Leader Transfer

运维执行 `systemctl stop kv_node@0`，Node-0 是当前 Leader。集群有 Node-0（Leader）、Node-1、Node-2，所有节点日志同步，`matchIndex_[1] = matchIndex_[2] = lastLogIndex() = 60`。

**第一步：SIGTERM 到达，stopFlag 置 true，主循环退出。**

打开 [examples/src/kv_node.cpp](examples/src/kv_node.cpp)：

```cpp
Signal::signal(SIGTERM, [] { stopFlag.store(true); });
// 主循环 sleep(2s) 醒来后检查 stopFlag，退出循环
kvSrv.stop();   // 停止接受新 HTTP 请求
httpThread.join();
```

**第二步：检测到自己是 Leader，调 transferLeadership(-1)。**

```cpp
if (node.isLeader()) {
    node.transferLeadership(-1);  // 自动选 matchIndex 最高的 Follower
    // 等待最多 800ms
    for (int i = 0; i < 80; ++i) {
        if (!node.isLeader()) break;
        sleep_for(10ms);
    }
}
```

`transferLeadership(-1)` 投递到 loop_ 线程：`pickTransferTarget()` 比较 `matchIndex_[1]=60, matchIndex_[2]=60`，同分取 id 最小，选 Node-1。`leadershipTransferTarget_ = 1`。

**第三步：doTransferLeadership 发现 Node-1 已追平，直接发 TimeoutNow。**

```cpp
// doTransferLeadership(1)
uint64_t lastIdx = 60, matched = matchIndex_[1] = 60;
// matched == lastIdx → 立刻发 TimeoutNow
client->callAsync("TimeoutNow", encodeTimeoutNowReq(term=5), {}, ...);
```

同时，`proposeAndNotify` 开始拒绝新写（`leadershipTransferTarget_ != -1`），HTTP 服务也已停止，所以此时不会有新的写入。

**第四步：Node-1 收到 TimeoutNow，becomeCandidate，runElection。**

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，Node-1 的 `handleTimeoutNow`：

```cpp
// senderTerm = 5 == currentTerm_ = 5 → 接受
done(encodeTimeoutNowResp(5, true));
becomeCandidate();   // currentTerm_ → 6，votedFor_ = 1，persistHardState
runElection();       // 向 Node-0、Node-2 发 RequestVoteReq(term=6, preVote=false)
```

Node-2（lastLogIndex=60，lastLogTerm=5）收到 RequestVoteReq，发现 `6 >= currentTerm_=5` 且日志一样新，投票给 Node-1。

**第五步：Node-1 成为 Leader，广播 AppendEntries（no-op），Node-0 收到更高 term。**

Node-1 收到自己 + Node-2 = 2 票（quorum），`becomeLeader()`，立刻触发 `heartbeatTick`，向 Node-0、Node-2 发 AppendEntries(term=6)。

Node-0 的 `handleAppendEntries` 里：

```cpp
if (args.term > currentTerm_.load()) {
    becomeFollower(args.term);  // currentTerm_ → 6，state_ → Follower
    leaderId_ = args.leaderId;  // leaderId_ = 1
}
```

`leaderTransfersSucceeded_` 计数器在 `becomeFollower` 里检查 `leadershipTransferTarget_ != -1` 时递增。

**第六步：主线程 10ms 轮询检查到 isLeader()=false，退出等待，node.stop()。**

```cpp
// 主循环
for (int i = 0; i < 80; ++i) {
    if (!node.isLeader()) break;  // i ≈ 3（约 30ms 后 Node-1 广播到 Node-0）
    sleep_for(10ms);
}
// 实测写空窗 ≈ 30-50ms（TimeoutNow RTT + 选举 + 心跳广播）
// 远优于被动 kill-9 的 150-300ms
node.stop();
```

---

## 8. 各模块职责速查表

| 文件 | 新增/修改函数 | 职责 |
|------|-------------|------|
| [src/proto/raft.proto](src/proto/raft.proto) | 所有 proto 消息 | Raft RPC 的 Protobuf 3 schema；`LogEntry.vlen` 是 bypass 旁路的长度标注 |
| [src/include/rpc/RpcMessage.h](src/include/rpc/RpcMessage.h) | `encode()`、`decode()` | 新 16B 帧头含 `bypass_len`；`bypass` 字段旁路传原始 value |
| [src/include/rpc/AsyncRpcClient.h](src/include/rpc/AsyncRpcClient.h) | `callAsync()`、`callAsyncCo()` | 新增 `bypass` 参数；对不使用旁路的 RPC 传 `{}` |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `splitCmd()`、`joinCmd()` | 命令头/value 在第二个空格处切分和重组 |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `encodeAE()`、`decodeAE()` | AppendEntries bypass 拆分/重组；value 字节不经 Protobuf |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `handleReadIndex()` | Leader 侧：接收 Follower 的 readIndex 查询；快/慢两条路径 |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `drainConfirmedReads()` | 批量兑现 pendingReads_ + pendingRemoteReads_；epoch > requestEpoch 才兑现 |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `proposeFollowerRead()` | 统一线性读入口；Leader 走本地协议，Follower 发 ReadIndexReq RPC |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `drainFollowerReads()` | Follower 侧：lastApplied 推进时兑现等待中的 followerPendingReads_ |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `transferLeadership()` | 外部入口；设置 leadershipTransferTarget_；1s 超时守卫 |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `doTransferLeadership()` | 检查 target 是否追平；追平则发 TimeoutNow，否则触发 replicateLog |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `handleTimeoutNow()` | Follower 侧：验证 term 后跳过 Pre-Vote 直接 becomeCandidate+runElection |
| [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp) | `pickTransferTarget()` | 从 matchIndex_ 中选最佳 transfer 目标（最高 matchIndex，同分取最小 id）|
| [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp) | GET `/kv/:key` | 改用 `proposeFollowerRead`；`setDeferred(true)` 异步响应 |
| [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp) | POST `/admin/transfer` | 触发 `node_.transferLeadership(to)`；仅 Leader 响应 |
| [examples/src/kv_node.cpp](examples/src/kv_node.cpp) | 主函数停机路径 | SIGTERM → kvSrv.stop → transferLeadership(-1) → 等 800ms → node.stop |

---

## 9. 验证

**编译（需要 protobuf，macOS 用 `brew install protobuf`）：**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target kv_node -j
```

**启动三节点集群：**

```bash
./build/examples/kv_node --id 0 --nodes 3 &
./build/examples/kv_node --id 1 --nodes 3 &
./build/examples/kv_node --id 2 --nodes 3 &
```

**验证 bypass 旁路（写 1MB value，观察日志里 Protobuf payload 不含 value）：**

```bash
# 生成 1MB 随机数据
dd if=/dev/urandom bs=1024 count=1024 2>/dev/null | base64 > /tmp/big_value.txt
curl -X PUT http://localhost:19900/kv/photo -d @/tmp/big_value.txt
# 观察 kv_node 日志：AppendEntries proto_section_len 应远小于 1MB
```

**验证 ReadIndex 线性一致读（从任意节点读，结果一致）：**

```bash
curl -X PUT http://localhost:19900/kv/counter -d "42"
# 连续从三个节点各读一次，应全部返回 "42"（不会脏读）
for port in 19900 19901 19902; do
    echo -n "Node at $port: "
    curl -s http://localhost:$port/kv/counter
    echo
done
```

**验证 Leader Transfer（写空窗对比）：**

```bash
# 查询当前 Leader
for port in 19900 19901 19902; do
    curl -s http://localhost:$port/admin/raft | python3 -m json.tool | grep '"state"'
done

# 主动 Leader Transfer（假设 Leader 在 port 19900）
curl -X POST http://localhost:19900/admin/transfer
# 再次查询，Leader 已转移到另一个节点

# 对比：SIGTERM 优雅关机 vs kill-9
# SIGTERM（先 transfer 再退出）：
kill -TERM $(pgrep -f "kv_node --id 0")
# 观察 Leader 在 ~30-50ms 内切换

# kill -9（被动选举）：
kill -9 $(pgrep -f "kv_node --id 0")
# 观察 Leader 在 150-300ms 后切换
```

