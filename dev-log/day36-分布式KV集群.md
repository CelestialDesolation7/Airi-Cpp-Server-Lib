# Day 36 — 分布式 KV 集群 / proposeAndNotify / NodeAnnounce / Bug 修复

## 目录

| 章节 | 内容 |
|------|------|
| [§1 引言](#1-引言) | 整体三层架构图 + 单节点线程模型；建立阅读地图 |
| [§2 改进 A — KvStateMachine](#2-改进-a--kvstatemachine raft-应用层) | 新建 `examples/include/kv/KvStateMachine.h/cpp`：PUT/DEL 命令解析；JSON 序列化快照；`std::mutex` 保护 map |
| [§3 改进 B — KvHttpServer](#3-改进-b--kvhttpserver-rest-api-层) | 新建 `examples/include/kv/KvHttpServer.h/cpp` + 修改 `HttpServer.h`：GET 本地读（最终一致性）、PUT/DEL chunked 响应、307 重定向、/admin 路由 |
| [§4 改进 C — proposeAndNotify](#4-改进-c--proposeandnotify带回调的写接口) | 修改 `RaftNode.h/cpp`：带完成回调的写入接口 + writeCallbacks_ 期货表 + becomeFollower 批量失败 |
| [§5 改进 D — NodeAnnounce + startupGrace](#5-改进-d--nodeannounce--startupgrace) | 修改 `RaftNode.cpp`：重启广播 + 宽限期防抢主 + 多数派同时启动加速选举 |
| [§6 改进 E — AE 快照边界 Bug 修复](#6-改进-e--appendentries-快照边界-bug-修复) | 修改 `RaftNode.cpp`：裁掉快照已覆盖的 entries 前缀，防止日志永久丢失 |
| [§7 整体运行时理解](#7-整体运行时理解) | 对象所有权图 + 3 个完整追踪场景（PUT 全路径 / 节点重启 / AE 快照边界 Bug）|
| [§8 各模块职责速查表](#8-各模块职责速查表) | 本日所有新增/修改函数的线程/时机/职责一览 |
| [§9 工程化 — kv_node 装配与 Web Dashboard](#9-工程化kv_node-主程序与-web-dashboard) | 新建 `examples/src/kv_node.cpp`：组件装配、快照触发、优雅停机 |
| [§10 验证](#10-验证) | 构建命令 + 完整可运行演示步骤 |
| [§11 局限与下一步](#11-局限与下一步) | 遗留问题与 Day37 计划 |

---

## 本日变更文件一览

| 文件 | 变更 | 核心改动 |
|------|------|---------|
| `examples/include/kv/KvStateMachine.h/cpp` | **新建** | Raft 应用层：解释 PUT/DEL 命令；JSON 序列化快照；`std::mutex` 保护 map |
| `examples/include/kv/KvHttpServer.h/cpp` | **新建** | REST API：GET /kv/:key（本地读，最终一致性）；PUT/DEL 异步 chunked 响应；307 重定向；/admin 路由 |
| `examples/src/kv_node.cpp` | **新建** | 5 节点 kv_node 主程序（装配 RaftNode + KvStateMachine + KvHttpServer；优雅停机 kvSrv.stop → node.stop）|
| `src/include/raft/RaftNode.h` | **修改** | 新增 `proposeAndNotify`、`handleNodeAnnounce`、`getPeerCount`、`getQuorum`、`setSnapshotApplyCallback`、`writeCallbacks_`；新增 `startupGrace_/announcedPeers_/snapshotApplyCallback_` |
| `src/common/raft/RaftNode.cpp` | **修改** | NodeAnnounce 处理；startupGrace 逻辑；AE 快照边界 bug fix；proposeAndNotify；applyCommitted 兑现回调 |
| `src/include/http/HttpServer.h` | **修改** | 新增 `addAsyncPrefixRoute`（支持异步路由 handler 持有 Connection*）|

---

## 1. 引言

### 1.1 整体架构

KV 集群的每个节点由三层组成：

```
         ┌──────────────────────────────────────────────────┐
         │                 kv_node 进程                      │
         │                                                   │
  HTTP   │   ┌─────────────────────────────────────────┐    │
客户端 ──┼──▶│  KvHttpServer（HTTP REST 门面）          │    │
         │   │  GET /kv/:key  PUT /kv/:key  DEL /kv/:key│   │
         │   └────────────┬────────────────┬────────────┘    │
         │                │ sm_.get()      │ proposeAndNotify│
         │                │ （本地读）      │                  │
         │   ┌────────────▼────────────────▼────────────┐    │
         │   │  KvStateMachine（命令解释 / 状态存储）    │    │
         │   │  map_[key] = value   (std::mutex 保护)    │   │
         │   │  applyCallback_ ← RaftNode 调用            │   │
         │   └────────────────────┬─────────────────────┘    │
         │                        │ applyCallback / propose  │
         │   ┌────────────────────▼─────────────────────┐    │
         │   │  RaftNode（Raft 共识引擎）               │    │
  RPC ───┼──▶│  loop_（单线程，所有 Raft 状态在这里）   │◀──┼── 其他节点
         │   │  AppendEntries / RequestVote / ...        │    │
         │   └──────────────────────────────────────────┘    │
         └──────────────────────────────────────────────────┘
```

- **写请求**（PUT/DELETE）：必须路由到 Leader，Leader 通过 `proposeAndNotify` 写 Raft 日志，等 apply 后 HTTP 响应。非 Leader 节点返回 307 重定向。
- **读请求**（GET）：任意节点均可处理。Day36 的 GET 是**本地读**——直接读本节点的 `KvStateMachine`，不经过 Raft。这意味着最终一致性：Follower 的 `lastApplied` 可能略落后于 Leader 的 `commitIndex`，所以可能读到略旧的值（甚至读不到刚写入 Leader、尚未复制到本节点的 key）。强一致读（ReadIndex / 线性一致）是后续工作。
- **每个节点**都有自己的 HTTP 服务器，端口 = `kHttpBasePort(8901) + id`。

### 1.2 线程模型（单节点视角）

一个 `kv_node` 进程包含以下线程：

```
kv_node 进程
├── T_main       main()：构造对象、start()、状态行输出、等 SIGINT
├── T_raft       RaftNode::loop_   ← 所有 Raft 状态只在这里读写
├── T_raft_rpc   RaftNode::rpcServer_ 的 sub-reactor（处理入站 RPC）
├── T_http_main  KvHttpServer 的 main-reactor（accept 新 HTTP 连接）
├── T_http_sub   KvHttpServer 的 sub-reactor（处理 HTTP 读写）
└── T_log        AsyncLogging 后端写线程
```

关键跨线程路径（§7.2 会详细展开）：

```
T_http_sub ──runInLoop──▶ T_raft ──queueInLoop──▶ T_http_sub
  PUT 请求                proposeAndNotify         发 applied 响应
```

---

## 2. 改进 A — KvStateMachine（Raft 应用层）

### 2.1 为什么需要这个类

Raft 引擎是通用共识机器，只保证"这条字符串命令被多数派日志写入并 apply 了"，对字符串语义一无所知。没有 `KvStateMachine`，`applyCallback_` 收到 `"PUT foo bar"` 只能打印日志——集群存不住任何东西。

**两类访问者，一把锁**

`apply` 由 T_raft 调用（顺序、单线程），`get/serialize/size` 由 T_http_sub 调用（与 apply 并发）。两者并发访问同一个 `map_`，因此需要一把锁保护。Day36 保持简单：用一把普通的 `std::mutex mu_`，每次访问（无论读写）都先 `std::lock_guard` 上锁。读写分离的 `shared_mutex` 是可选的优化，本日不引入。

👉 apply 的完整调用链（从 commitIndex 推进到 HTTP 响应）见 [§7.2 第 8 步](#第-8-步applycommitted-触发状态机--兑现-http-回调)

### 2.2 编码实现步骤

**第一步：新建 `examples/include/kv/KvStateMachine.h`，写入以下全部内容**

```cpp
#pragma once
// KvStateMachine —— 基于内存哈希表的 Raft 复制状态机
//
// 命令格式（由 KvHttpServer 构造）：
//   "PUT <key> <value>"  — 写入或覆盖（value 可包含空格，取首个空白之后的全部）
//   "DEL <key>"          — 删除（key 不存在时无操作）
//
// 线程安全性：
//   apply / applySnapshot 由 RaftNode 的 loop_ 线程调用（顺序执行，互不竞争）。
//   get / serialize       由 HTTP handler 线程调用（与 apply 并发），mu_ 保护 map_。
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

class KvStateMachine {
  public:
    // 应用一条已提交的 Raft 命令（在 loop_ 线程回调）
    void apply(uint64_t index, const std::string &cmd);

    // 读取 key。返回 false 表示 key 不存在。可从任意线程调用。
    bool get(const std::string &key, std::string &value) const;

    // 返回所有 key-value 对的 JSON 字符串，供 takeSnapshot / scan 使用。
    std::string serialize() const;

    // 当前 key 数量（近似，lock-free）
    size_t size() const;

    // 从 JSON 字符串重建完整状态（InstallSnapshot 后恢复）。
    void applySnapshot(uint64_t index, const std::string &data);

  private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> map_;
};
```

`mutable` 让 `const` 方法（`get/serialize/size`）也能对 `mu_` 加锁——这是 `const` 成员函数中使用互斥锁的标准惯用法，不加 `mutable` 则编译报错。

---

**第二步：新建 `examples/src/kv/KvStateMachine.cpp`，写入以下全部内容**

来自 [examples/src/kv/KvStateMachine.cpp](examples/src/kv/KvStateMachine.cpp)：

```cpp
#include "kv/KvStateMachine.h"
#include "log/Logger.h"
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

void KvStateMachine::apply(uint64_t index, const std::string &cmd) {
    std::istringstream ss(cmd);
    std::string op;
    ss >> op;

    if (op == "PUT") {
        std::string key;
        ss >> key;
        std::string value;
        // value 取第一个空白后的全部剩余内容（允许空格）
        if (std::getline(ss >> std::ws, value)) {
            std::lock_guard<std::mutex> lk(mu_);
            map_[key] = value;
            LOG_DEBUG << "[KvSM] apply index=" << index
                      << " PUT " << key << "=" << value;
        }
    } else if (op == "DEL") {
        std::string key;
        ss >> key;
        std::lock_guard<std::mutex> lk(mu_);
        map_.erase(key);
        LOG_DEBUG << "[KvSM] apply index=" << index << " DEL " << key;
    } else {
        LOG_WARN << "[KvSM] apply index=" << index
                 << " 未知命令: " << cmd;
    }
}

bool KvStateMachine::get(const std::string &key, std::string &value) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    value = it->second;
    return true;
}

size_t KvStateMachine::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

std::string KvStateMachine::serialize() const {
    std::lock_guard<std::mutex> lk(mu_);
    json j = map_;
    return j.dump();
}

void KvStateMachine::applySnapshot(uint64_t index, const std::string &data) {
    try {
        auto j = json::parse(data);
        auto newMap = j.get<std::unordered_map<std::string, std::string>>();
        std::lock_guard<std::mutex> lk(mu_);
        map_ = std::move(newMap);
        LOG_INFO << "[KvSM] applySnapshot index=" << index
                 << " entries=" << map_.size();
    } catch (const std::exception &e) {
        LOG_WARN << "[KvSM] applySnapshot parse error: " << e.what()
                 << " — 保留当前状态";
    }
}
```

**逐段理解：**

**`apply`**：`ss >> op` 取操作码，`ss >> key` 取键名。PUT 时用 `getline(ss >> std::ws, value)` 吸收键名后**全部剩余内容**（含空格），这样 `"PUT greeting hello world"` 的 value 是 `"hello world"` 而非 `"hello"`。`op` 既不是 PUT 也不是 DEL 时进 `else` 打 WARN——不会误改 map。写操作用 `lock_guard` 上锁。

**`get/size`**：读方法同样用 `lock_guard<std::mutex>` 上锁后读 `map_`，与 T_raft 的 `apply` 写入互斥。

**`serialize`**：上锁后把 `map_` 直接 dump 成 JSON 字符串——`json j = map_; return j.dump();`，nlohmann/json 能直接序列化 `unordered_map<string,string>`。这个字符串对 Raft 层完全透明，Raft 只是存储和转发它。

**`applySnapshot`**：在 `try` 块里 `json::parse(data).get<unordered_map<string,string>>()` 解析出 `newMap`，再在 `lock_guard` 保护下用 `std::move` 把新 map 换入——`move` 是 O(1) 指针交换，不拷贝任何数据。解析失败时捕获异常、保留当前状态并打 WARN，不崩溃。

---

## 3. 改进 B — KvHttpServer（REST API 层）

### 3.1 为什么需要这个类

RaftNode 只有 RPC 接口，外部无法用 curl / 浏览器 / 任何 HTTP 客户端直接访问集群。`KvHttpServer` 是暴露给用户的最外层，把整个分布式系统包装成标准 REST API。

路由概览：
```
GET    /kv/:key           → 本地读（最终一致性，任意节点）
PUT    /kv/:key  body=val → Raft 写（必须 Leader；非 Leader → 307 重定向）
DELETE /kv/:key           → Raft 写（同上）
GET    /admin/raft        → 节点 Raft 状态 JSON
GET    /admin/scan        → 所有 KV 对
GET    /                  → Web 仪表盘（静态文件）
```

### 3.2 编码实现步骤

**第一步：修改 `src/include/http/HttpServer.h`，在 `addPrefixRoute` 声明之后新增异步路由接口**

来自 [src/include/http/HttpServer.h](src/include/http/HttpServer.h)（新增部分）：

```cpp
// 异步路由：handler 收到 Connection* 以支持跳过同步发送。
// 与同步路由的区别：匹配到异步路由时，中间件已运行完毕（可向 resp 写入头），
//   handler 内调用 resp->setDeferred(true) 后可手动将 resp.headers() 拷贝进自定义头并直接发送。
using AsyncRouteHandler =
    std::function<void(const HttpRequest &, HttpResponse *, Connection *)>;

void addAsyncRoute(HttpRequest::Method method, const std::string &path,
                   AsyncRouteHandler handler);
void addAsyncPrefixRoute(HttpRequest::Method method, const std::string &prefix,
                         AsyncRouteHandler handler);
```

普通路由 handler 签名 `(HttpRequest&, HttpResponse*)`，返回时框架自动发送 `resp`。异步路由多一个 `Connection*`，handler 可以在任意时刻（如 Raft apply 回调里）自己 `conn->send()`，并用 `resp->setDeferred(true)` 告诉框架不要自动发。

---

**第二步：新建 `examples/include/kv/KvHttpServer.h`**

来自 [examples/include/kv/KvHttpServer.h](examples/include/kv/KvHttpServer.h)：

```cpp
#pragma once
// KvHttpServer —— 把 KvStateMachine + RaftNode 暴露为 REST HTTP 接口
#include "Connection.h"
#include "http/HttpServer.h"
#include "kv/KvStateMachine.h"
#include "raft/RaftNode.h"
#include <cstdint>
#include <memory>
#include <string>

class KvHttpServer {
  public:
    KvHttpServer(raft::RaftNode &node, KvStateMachine &sm,
                 uint16_t httpPort, uint16_t baseHttpPort = 8901,
                 std::string staticDir = "examples/static/kv");
    ~KvHttpServer() = default;

    void start();
    void stop();

  private:
    static std::string extractKey(const std::string &path);
    std::string leaderUrl(const std::string &path) const;
    void handleWriteAsync(const std::string &cmd,
                          const std::string &path,
                          const HttpRequest &req,
                          HttpResponse *resp,
                          Connection *conn);

    raft::RaftNode   &node_;
    KvStateMachine   &sm_;
    uint16_t          baseHttpPort_;
    std::string       staticDir_;
    std::unique_ptr<HttpServer> srv_;
};
```

---

**第三步：新建 `examples/src/kv/KvHttpServer.cpp`，写入以下全部内容**

来自 [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp)：

```cpp
#include "kv/KvHttpServer.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/StaticFileHandler.h"
#include "log/Logger.h"
#include <chrono>
#include <cstdio>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── chunked 帧：返回 "hex-size\r\ndata\r\n" ───────────────────────────────
static std::string makeChunk(std::string_view data) {
    char hdr[32];
    int n = std::snprintf(hdr, sizeof(hdr), "%zx\r\n", data.size());
    std::string chunk;
    chunk.reserve(static_cast<size_t>(n) + data.size() + 2);
    chunk.append(hdr, static_cast<size_t>(n));
    chunk.append(data);
    chunk.append("\r\n");
    return chunk;
}

KvHttpServer::KvHttpServer(raft::RaftNode &node, KvStateMachine &sm,
                           uint16_t httpPort, uint16_t baseHttpPort,
                           std::string staticDir)
    : node_(node), sm_(sm), baseHttpPort_(baseHttpPort),
      staticDir_(std::move(staticDir)) {
    HttpServer::Options opts;
    opts.tcp.listenPort = httpPort;
    srv_ = std::make_unique<HttpServer>(opts);
}

std::string KvHttpServer::extractKey(const std::string &path) {
    constexpr std::string_view prefix = "/kv/";
    if (path.size() > prefix.size())
        return path.substr(prefix.size());
    return {};
}

std::string KvHttpServer::leaderUrl(const std::string &path) const {
    int lid = node_.getLeaderId();
    if (lid < 0) return {};
    return "http://127.0.0.1:" + std::to_string(baseHttpPort_ + lid) + path;
}

void KvHttpServer::handleWriteAsync(const std::string &cmd,
                                    const std::string &path,
                                    const HttpRequest & /*req*/,
                                    HttpResponse *resp,
                                    Connection *conn) {
    const int      myId   = node_.getId();
    const uint64_t myTerm = node_.getCurrentTerm();

    if (!node_.isLeader()) {
        int         lid = node_.getLeaderId();
        std::string url = leaderUrl(path);
        if (!url.empty()) {
            resp->setStatus(HttpResponse::StatusCode::k307TemporaryRedirect, "Temporary Redirect");
            resp->addHeader("Location", url);
            resp->setContentType("application/json");
            resp->setBody(R"({"ok":false,"status":"redirect","leader":)" +
                          std::to_string(lid) + R"(,"leaderUrl":")" + url + R"("})");
        } else {
            resp->setStatus(HttpResponse::StatusCode::k503ServiceUnavailable, "No Leader");
            resp->setContentType("application/json");
            resp->setBody(R"({"ok":false,"status":"no_leader","term":)" +
                          std::to_string(myTerm) + "}");
        }
        return;
    }

    // ── Leader 路径：chunked 首块立即发，尾块在 apply 后发 ────────────────
    std::string hdrs = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/x-ndjson\r\n"
                       "Transfer-Encoding: chunked\r\n";
    for (const auto &[k, v] : resp->headers()) {
        hdrs += k + ": " + v + "\r\n";
    }
    hdrs += "Connection: keep-alive\r\n\r\n";

    const int      clusterSize  = node_.getPeerCount();
    const int      quorum       = node_.getQuorum();
    const uint64_t commitBefore = node_.getCommitIndex();
    const uint64_t lastLogBefore= node_.getLastLogIndex();

    {
        json j = {
            {"status",            "accepted"},
            {"leader",            myId},
            {"term",              myTerm},
            {"clusterSize",       clusterSize},
            {"quorum",            quorum},
            {"commitIndexBefore", commitBefore},
            {"lastLogIndexBefore",lastLogBefore},
            {"cmd",               cmd}
        };
        conn->send(hdrs + makeChunk(j.dump() + "\n"));
    }
    resp->setDeferred(true);

    auto alive   = conn->aliveFlag();
    auto *loop   = conn->getLoop();
    auto  t0     = std::chrono::steady_clock::now();
    auto *nodePtr= &node_;
    auto *smPtr  = &sm_;

    node_.proposeAndNotify(cmd,
        [alive, loop, conn, myId, myTerm, clusterSize, quorum, commitBefore, nodePtr, smPtr, t0]
        (bool ok, uint64_t logIndex) {
            loop->queueInLoop([alive, conn, ok, logIndex,
                               myId, myTerm, clusterSize, quorum, commitBefore,
                               nodePtr, smPtr, t0]() {
                if (auto f = alive.lock(); !f || !*f) return;
                const double dtMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                std::string body;
                if (ok) {
                    json j = {
                        {"status",            "applied"},
                        {"ok",                true},
                        {"leader",            myId},
                        {"term",              myTerm},
                        {"logIndex",          logIndex},
                        {"commitIndexBefore", commitBefore},
                        {"commitIndexAfter",  nodePtr->getCommitIndex()},
                        {"lastApplied",       nodePtr->getLastApplied()},
                        {"quorum",            quorum},
                        {"clusterSize",       clusterSize},
                        {"kvSize",            smPtr->size()},
                        {"latencyMs",         dtMs}
                    };
                    body = j.dump() + "\n";
                } else {
                    body = R"({"status":"error","ok":false,"reason":"leadership lost"})" "\n";
                }
                conn->send(makeChunk(body) + "0\r\n\r\n");
            });
        });
}

void KvHttpServer::start() {
    // ── CORS 中间件 ─────────────────────────────────────────────────────────
    srv_->use([](const HttpRequest &req, HttpResponse *resp,
                 const HttpServer::MiddlewareNext &next) {
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, PUT, DELETE, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
        if (req.method() == HttpRequest::Method::kOptions) {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            return;
        }
        next();
    });

    srv_->addRoute(HttpRequest::Method::kGet, "/",
        [this](const HttpRequest &req, HttpResponse *resp) {
            StaticFileHandler::Options opts;
            opts.cacheControl = "no-cache, no-store, must-revalidate";
            if (!StaticFileHandler::serve(req, resp, staticDir_ + "/index.html", opts)) {
                resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
                resp->setContentType("text/plain");
                resp->setBody("Dashboard not found.\n"
                              "Start kv_node with --static-dir pointing to examples/static/kv/\n");
            }
        });

    srv_->addRoute(HttpRequest::Method::kGet, "/app.js",
        [this](const HttpRequest &req, HttpResponse *resp) {
            StaticFileHandler::Options opts;
            opts.cacheControl = "no-cache, no-store, must-revalidate";
            if (!StaticFileHandler::serve(req, resp, staticDir_ + "/app.js", opts)) {
                resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
                resp->setContentType("text/plain");
                resp->setBody("app.js not found\n");
            }
        });

    // ── GET /admin/raft ─────────────────────────────────────────────────────
    srv_->addRoute(HttpRequest::Method::kGet, "/admin/raft",
        [this](const HttpRequest &, HttpResponse *resp) {
            const char *s = "Follower";
            if (node_.getState() == raft::State::Leader)    s = "Leader";
            if (node_.getState() == raft::State::Candidate) s = "Candidate";
            json j{{"id",          node_.getId()},
                   {"state",       s},
                   {"term",        node_.getCurrentTerm()},
                   {"leaderId",    node_.getLeaderId()},
                   {"commitIndex", node_.getCommitIndex()},
                   {"lastApplied", node_.getLastApplied()},
                   {"kvSize",      sm_.size()}};
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("application/json");
            resp->setBody(j.dump());
        });

    // ── GET /admin/scan ─────────────────────────────────────────────────────
    srv_->addRoute(HttpRequest::Method::kGet, "/admin/scan",
        [this](const HttpRequest &, HttpResponse *resp) {
            json pairs = json::array();
            try {
                auto j = json::parse(sm_.serialize());
                for (auto &[k, v] : j.items())
                    pairs.push_back({{"key", k}, {"value", v}});
            } catch (...) {}
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("application/json");
            resp->setBody(json{{"ok", true},
                               {"count", pairs.size()},
                               {"pairs", pairs}}.dump());
        });

    // ── GET /kv/:key — 本地读（最终一致性）──────────────────────────────────
    srv_->addPrefixRoute(HttpRequest::Method::kGet, "/kv/",
        [this](const HttpRequest &req, HttpResponse *resp) {
            std::string key = extractKey(req.url());
            if (key.empty()) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setContentType("application/json");
                resp->setBody(R"({"ok":false,"error":"missing key"})");
                return;
            }
            // 附带读请求的节点元数据，供前端展示「本地读 / 可能脏读」提示
            const int   servedBy = node_.getId();
            const auto  st       = node_.getState();
            const char *stateStr = (st == raft::State::Leader)    ? "Leader"
                                 : (st == raft::State::Candidate) ? "Candidate"
                                                                  : "Follower";
            std::string value;
            if (sm_.get(key, value)) {
                resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
                resp->setContentType("application/json");
                resp->setBody(json{{"ok",           true},
                                   {"key",          key},
                                   {"value",        value},
                                   {"servedBy",     servedBy},
                                   {"servedByState",stateStr},
                                   {"term",         node_.getCurrentTerm()},
                                   {"lastApplied",  node_.getLastApplied()}}.dump());
            } else {
                resp->setStatus(HttpResponse::StatusCode::k404NotFound, "Not Found");
                resp->setContentType("application/json");
                resp->setBody(json{{"ok",           false},
                                   {"key",          key},
                                   {"error",        "not found"},
                                   {"servedBy",     servedBy},
                                   {"servedByState",stateStr},
                                   {"term",         node_.getCurrentTerm()},
                                   {"lastApplied",  node_.getLastApplied()}}.dump());
            }
        });

    // ── PUT /kv/:key ─────────────────────────────────────────────────────────
    srv_->addAsyncPrefixRoute(HttpRequest::Method::kPut, "/kv/",
        [this](const HttpRequest &req, HttpResponse *resp, Connection *conn) {
            std::string key = extractKey(req.url());
            if (key.empty()) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setContentType("application/json");
                resp->setBody(R"({"ok":false,"error":"missing key"})");
                return;
            }
            handleWriteAsync("PUT " + key + " " + req.body(), req.url(), req, resp, conn);
        });

    // ── DELETE /kv/:key ──────────────────────────────────────────────────────
    srv_->addAsyncPrefixRoute(HttpRequest::Method::kDelete, "/kv/",
        [this](const HttpRequest &req, HttpResponse *resp, Connection *conn) {
            std::string key = extractKey(req.url());
            if (key.empty()) {
                resp->setStatus(HttpResponse::StatusCode::k400BadRequest, "Bad Request");
                resp->setContentType("application/json");
                resp->setBody(R"({"ok":false,"error":"missing key"})");
                return;
            }
            handleWriteAsync("DEL " + key, req.url(), req, resp, conn);
        });

    srv_->start();
}

void KvHttpServer::stop() {
    srv_->stop();
}
```

**关键设计理解：**

- **非 Leader 分支**：307 + Location 让 `curl -L` 自动重试到 Leader；leaderId==-1（选举中）则返回 503。两种情况都是**同步返回**，框架正常发 `resp`。
- **Leader 分支**：手动拼 HTTP 头（含中间件 CORS 头，从 `resp->headers()` 拷贝）+ 首块 JSON，调 `conn->send()` 立即发出。`resp->setDeferred(true)` 阻止框架自动发送。`conn->aliveFlag()` 返回 `weak_ptr<bool>`：连接析构时 `*bool = false`，防止 Raft 回调触发时 use-after-free。
- **GET 本地读路径**：GET /kv/:key 是**同步**路由（`addPrefixRoute`），直接 `sm_.get(key, value)` 读本节点状态机，不经过 Raft。响应里带上 `servedBy`（哪个节点处理的）、`servedByState`（Leader/Follower/Candidate）、`term`、`lastApplied` 元数据，供前端展示「本地读 / 可能脏读」提示——Follower 的 `lastApplied` 可能略落后于 Leader，因此可能读到略旧的值。强一致读是后续工作。


## 4. 改进 C — proposeAndNotify（带回调的写接口）

### 4.1 为什么需要这个接口

Day33 的 `propose(cmd)` 是 fire-and-forget：命令写入后无任何通知。HTTP handler 调完 `propose` 就只能蒙着眼睛等——等多久？成功了吗？无从知晓。常见的绕法是"轮询 lastApplied"，但这既低效又有竞态。

`proposeAndNotify(cmd, done)` 解决这个问题：命令被 apply 后调 `done(true, logIndex)`；若在 apply 前丢失 Leader 身份，批量调所有待回调的 `done(false, 0)`。HTTP handler 完全不需要轮询。

👉 完整跨线程执行路径见 [§7.2](#72-场景-a--一次-put-请求的完整路径)

### 4.2 编码实现步骤

**第一步：在 `src/include/raft/RaftNode.h` 新增接口声明和成员变量**

打开 [src/include/raft/RaftNode.h](src/include/raft/RaftNode.h)，在 `propose` 声明之后新增：

```cpp
// ── 带完成通知的写入接口（线程安全）─────────────────────────────────
// 与 propose() 相同，但在命令被 apply 到状态机后回调 done(true)。
// 若当前节点不是 Leader，立即回调 done(false)。
// 若在 apply 前丢失 Leader 身份，同样回调 done(false, 0)。
// 第二个参数 logIndex 是该命令被分配到的全局日志下标（失败时为 0）。
void proposeAndNotify(const std::string &cmd, std::function<void(bool, uint64_t)> done);
```

在私有成员变量区找到持久化/回调相关的成员，新增：

```cpp
// 等待 apply 的写回调：logIndex → done(ok)（仅在 loop_ 线程访问，无需加锁）
std::unordered_map<uint64_t, std::function<void(bool, uint64_t)>> writeCallbacks_;
```

`writeCallbacks_` 是一张"期货表"：key = 日志 index，value = HTTP 回调函数。写入日志时登记，apply 到这个 index 时兑现，`becomeFollower` 时全部标记失败并清空。它只在 T_raft 线程读写，无需加锁。

---

**第二步：在 `src/common/raft/RaftNode.cpp` 实现 `proposeAndNotify`**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§7 外部写入接口段，propose 实现之后）：

```cpp
void RaftNode::proposeAndNotify(const std::string &cmd, std::function<void(bool, uint64_t)> done) {
    // 线程安全：实际工作在 loop_ 线程执行。
    // done(true, idx)  = 命令已应用到状态机，idx 是被分配的全局日志下标
    // done(false, idx) = 丢失 Leader 身份（idx 为该条目被分配的下标，未分配时为 0）
    loop_.runInLoop([this, cmd, done = std::move(done)]() mutable {
        if (state_.load() != State::Leader) {
            done(false, 0);
            return;
        }
        log_.push_back(LogEntry{currentTerm_.load(), cmd});
        persistLog();
        uint64_t idx = lastLogIndex();
        LOG_INFO << "[Node " << id_ << "] proposeAndNotify 追加日志 index=" << idx
                 << " cmd=" << cmd;
        writeCallbacks_[idx] = std::move(done);
        for (const auto &peer : peers_) {
            if (peer.id == id_) continue;
            replicateLog(peer);
        }
    });
}
```

`loop_.runInLoop` 使得这个函数可以从任意线程（如 T_http_sub）调用：如果当前不在 T_raft 线程，lambda 会被投递进 T_raft 的 pendingFunctors 并唤醒 T_raft 的 poll——这是跨线程安全的关键。

---

**第三步：修改 `applyCommitted`，在 apply 后兑现 writeCallbacks_**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§6 提交推进段）：

```cpp
void RaftNode::applyCommitted() {
    // 把 [lastApplied+1, commitIndex] 范围的条目逐条应用到状态机。
    // applyCallback_ 在 loop_ 线程回调 → 状态机代码天然单线程，无需加锁。
    while (lastApplied_.load() < commitIndex_.load()) {
        uint64_t idx = lastApplied_.load() + 1;
        if (idx > lastLogIndex()) break; // 防御：不应发生
        lastApplied_.store(idx);
        LOG_INFO << "[Node " << id_ << "] 应用日志 index=" << idx
                 << " 命令=" << logAt(idx).cmd;
        if (applyCallback_) applyCallback_(idx, logAt(idx).cmd);
        // 如果有等待应用的写回调，处理完成后通知
        auto it = writeCallbacks_.find(idx);
        if (it != writeCallbacks_.end()) {
            it->second(true, it->first);
            writeCallbacks_.erase(it);
        }
    }
}
```

这里有两个并列操作：`applyCallback_` 通知状态机（KvStateMachine::apply），`writeCallbacks_` 通知 HTTP 层（触发 queueInLoop 发送响应尾块）。两者都在同一个 apply 循环里顺序执行，都在 T_raft 线程，无竞态。Day36 对每条已提交的日志都调 `applyCallback_`（不做 no-op 过滤），状态机自行解析命令字符串（未知命令打 WARN 不改 map）。

---

**第四步：修改 `becomeFollower`，批量失败所有待回调请求**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§3 角色切换段）：

```cpp
void RaftNode::becomeFollower(uint64_t term) {
    if (state_.load() != State::Follower)
        LOG_INFO << "[Node " << id_ << "] " << stateName(state_.load()) << " → 跟随者（任期="
                 << term << "）";
    // 丢失 Leader 身份：将所有未完成的写请求全部通知失败
    if (!writeCallbacks_.empty()) {
        for (auto &[idx, cb] : writeCallbacks_) cb(false, idx);
        writeCallbacks_.clear();
    }
    state_.store(State::Follower);
    currentTerm_.store(term);
    votedFor_ = -1;
    persistHardState();  // term 变化必须立即落盘
    resetElectionTimer();
}
```

一旦 Leader 失去身份（收到更高 term 的消息），所有在途的 `proposeAndNotify` 等待就成了"孤儿"——它们等待的 index 可能永远不会被 apply（新 Leader 可能覆盖这些日志）。`for (auto &[idx, cb] : writeCallbacks_) cb(false, idx)` 立刻回调 `done(false)` 让 HTTP 层向客户端返回 `{"status":"error","reason":"leadership lost"}`。

---

## 5. 改进 D — NodeAnnounce + startupGrace

### 5.1 两个问题

**问题 1：连接退避导致重建延迟**

Leader 的 `AsyncRpcClient` 对宕机节点采用指数退避（100→200→400→800→1600→3200→6400ms，累积 ~13s）。节点重启后，Leader 可能要再等 ~6400ms 才重连，在此期间心跳发不到重启节点。

**问题 2：重启节点"抢主"**

重启节点未听到 Leader 心跳 → 150ms 后选举超时 → term++ → 发 RequestVote → 现任 Leader 看到更高 term 被迫 step down → 集群短暂写中断。

**组合修复**：NodeAnnounce（广播"我起来了"→ Leader 立即重连）+ startupGrace（宽限期防止重启节点在 Leader 未到达前发起选举）。

👉 完整时序对比见 [§7.3](#73-场景-b--节点重启-nodeannouncestartupgrace-的协作)

### 5.2 编码实现步骤

**第一步：在 `src/include/raft/RaftNode.h` 新增两个成员变量**

打开 [src/include/raft/RaftNode.h](src/include/raft/RaftNode.h)，找到 Raft 状态成员区，新增：

```cpp
// 启动宽限期：节点刚起动时用加长的选举超时，避免一台重启节点立刻调高 term
// 抢走现有健康 Leader 的身份。任何"已感知集群"的事件（收到 Leader 的
// AppendEntries / 向其他候选人授出票）后切换为正常 150~300ms 超时。
bool                  startupGrace_{true};
// 已收到 NodeAnnounce 的 peer id 集合（仅 grace 期间记录）。
// 当 announcedPeers_.size() + 1 达到 quorum 且本节点仍未听到 Leader，
// 表示整个多数派正在同时启动、集群中不存在在任 Leader，
// 可提前退出 grace 并立即发起选举，避免"全集群同时重启需等 30s"。
std::unordered_set<int> announcedPeers_;
```

---

**第二步：修改 `RaftNode::start()` 中的 runInLoop 段，添加冷/热启动判断与上线广播**

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，在 `start()` 里的 `runInLoop` lambda 内（持久化恢复完成之后）加入以下代码：

```cpp
// ── 冷启动检测：无持久化状态 = 首次启动 ────────────────────────
// 首次启动整集群时，所有节点都没有 Leader，不存在"重启节点抢主"问题，
// 无需宽限期，直接使用正常选举超时（150~300ms），让第一次选举快速完成。
// 反之，节点存在 term>0 或快照/日志 = 真正的重启 → 保留 grace 避免扰主。
bool freshStart = (currentTerm_.load() == 0
                   && log_.size() == 1       // 只有哨兵条目
                   && snapshotIndex_ == 0);
startupGrace_ = !freshStart;
if (freshStart) {
    LOG_INFO << "[Node " << id_ << "] 冷启动（无持久化状态），跳过宽限期";
}

resetElectionTimer();
// 上线广播：100ms 后向所有 peer 发 NodeAnnounce。
// 各 peer 收到后立即重置对本节点的连接退避时钟并尝试重连，
// 彻底消除"指数退避累积过长导致 Leader 迟迟无法发心跳"的问题。
// 100ms 延迟足以让 rpcServerThread_ 启动其 EventLoop 并开始 accept。
loop_.runAfter(0.1, [this] {
    std::string body = json{{"nodeId", id_}}.dump();
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        getOrCreateClient(peer)->callAsync(
            "NodeAnnounce", body,
            [id = id_, peerId = peer.id](bool ok, const std::string &) {
                if (ok)
                    LOG_INFO << "[Node " << id << "] NodeAnnounce → Node "
                             << peerId << " 已送达";
            },
            /*timeoutMs=*/500);
    }
    LOG_INFO << "[Node " << id_ << "] 已向所有 peer 广播上线通知（NodeAnnounce）";
});
// 启动宽限期硬上限：30s 后无论是否收到 AE 都强制退出 grace。
// 仅在真正的节点重启（freshStart=false）时挂此定时器。
// 宽限期需足够长：手动重启场景下 Leader 的指数退避最长可累积到 ~13s
// （100ms→200→400→800→1600→3200→6400ms），30s 留有充足余量。
if (!freshStart) {
    loop_.runAfter(30.0, [this] {
        if (startupGrace_) {
            LOG_INFO << "[Node " << id_ << "] 启动宽限期硬超时（30s），退出 grace";
            startupGrace_ = false;
        }
    });
}
```

`freshStart` 的三个条件须**全部满足**才算冷启动：term=0（没有参与过选举）、log 只有哨兵（没写过数据）、snapshotIndex=0（没有快照）。任一不满足 = 热启动。

---

**第三步：修改 `resetElectionTimer`，宽限期内使用加长超时**

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，在 `resetElectionTimer` 内找到选举超时计算处，改为：

```cpp
int timeoutMs = startupGrace_
    ? std::uniform_int_distribution<int>(1500, 3000)(rng_)
    : std::uniform_int_distribution<int>(150, 300)(rng_);
loop_.runAfter(timeoutMs / 1000.0,
               [this, myEpoch] { electionTimerFired(myEpoch); });
```

宽限期超时 1500~3000ms 是稳态 150~300ms 的 10 倍。50ms 心跳间隔下，即使 Leader 重建连接需要 200ms，宽限期内也会多次收到心跳，宽限期随即结束。

---

**第四步：修改 `electionTimerFired`，宽限期内抑制选举**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§3 选举定时器段）：

```cpp
void RaftNode::electionTimerFired(uint64_t epoch) {
    // 守卫①：epoch 不匹配 = 这个 timer 已被 resetElectionTimer 「覆盖」，是旧的，直接丢弃。
    if (epoch != electionEpoch_) return;
    // 守卫②：如果自己已经是 Leader，不应再发起选举（双重保险）。
    if (state_.load() == State::Leader) return;
    // 守卫③：启动宽限期内绝不主动发起选举。
    if (startupGrace_) {
        LOG_INFO << "[Node " << id_ << "] 选举超时但仍在启动宽限期内，抑制本次选举";
        resetElectionTimer();
        return;
    }
    // 选举超时 = Leader 失联（可能宕机或网络分区）。转为候选人，发起新一轮选举。
    becomeCandidate();
    runElection(); // 为每个 peer 并发发射 collectVote 协程
}
```

宽限期内触发超时不发选举，只重置定时器继续等待。宽限期退出后正常走 `becomeCandidate` → `runElection` 流程。

---

**第五步：在 `RaftNode.cpp` 实现 `handleNodeAnnounce`（§4b 段）**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§4b NodeAnnounce 段）：

```cpp
void RaftNode::handleNodeAnnounce(const std::string &reqJson, RpcServer::Done done) {
    int senderId = -1;
    try {
        senderId = json::parse(reqJson).at("nodeId").get<int>();
    } catch (...) {
        done(R"({})");
        return;
    }

    loop_.runInLoop([this, senderId, done = std::move(done)]() mutable {
        done(R"({})");  // 立即回复，无需等待
        // 找到对应 peer，唤醒其出站客户端
        for (const auto &peer : peers_) {
            if (peer.id == senderId) {
                LOG_INFO << "[Node " << id_ << "] 收到 Node " << senderId
                         << " 的重上线通知（NodeAnnounce），重置连接退避并立即重连";
                getOrCreateClient(peer)->wakeUp();
                break;
            }
        }
        // ── 多数派同时启动：抢占式退出 startupGrace_ ──────────────────
        // 当 --persist 且磁盘有非零 term 时，freshStart=false → startupGrace_=true，
        // 选举超时被拉长到 1500–3000ms 以保护"潜在仍存活的旧 Leader"。
        // 但若多个节点同时冷启动（无任何 Leader 在线），grace 会持续重置，
        // 直到 30s 硬上限才有节点突然成为 Leader（用户观感：迟迟不发选举）。
        // 修复：在 grace 期间统计已收到 NodeAnnounce 的 peers，一旦
        // (announcedPeers_.size() + 1) 达到 quorum 且本节点仍未听到 Leader，
        // 推断"多数派同时启动"，立即退出 grace 并触发 50ms 选举。
        if (startupGrace_ && leaderId_ == -1 && state_.load() == State::Follower) {
            announcedPeers_.insert(senderId);
            if (static_cast<int>(announcedPeers_.size()) + 1 >= quorum_) {
                LOG_INFO << "[Node " << id_ << "] 已收到 " << announcedPeers_.size()
                         << " 个 peer 的上线通知（含自己 = quorum），"
                         << "推断多数派同时启动，提前退出 startupGrace_ 并发起选举";
                startupGrace_ = false;
                announcedPeers_.clear();
                ++electionEpoch_;
                uint64_t ep = electionEpoch_;
                loop_.runAfter(0.05, [this, ep] { electionTimerFired(ep); });
                return;
            }
        }
        // 冷启动场景加速选举：
        // 若当前是 Follower 且无已知 Leader（leaderId_==-1），说明集群尚无主节点。
        // 收到任意 peer 上线通知后，立即抢占一个 50ms 快速选举机会，
        // 而不是干等当前 election timer 的剩余时间（最多 300ms）。
        // 保留 epoch 守卫，不会与正在进行中的选举冲突。
        if (state_.load() == State::Follower && leaderId_ == -1 && !startupGrace_) {
            ++electionEpoch_;
            uint64_t ep = electionEpoch_;
            loop_.runAfter(0.05, [this, ep] { electionTimerFired(ep); });
        }
    });
}
```

两个行为：
1. **`wakeUp()`**：重置 `AsyncRpcClient` 内部退避时钟为"立即重试"，下一轮 IO 循环（<50ms）发起 TCP connect，彻底消除指数退避积累的延迟。
2. **`announcedPeers_`**：解决全集群同时重启导致的"假死 30 秒"问题——每个节点都在 grace 期等 Leader，但集群里根本没有 Leader。收集足够多的"我也在线"后，推断集群可以安全选主，立即退出 grace 发起选举。

`startupGrace_` 的所有清除点（任一触发即退出宽限期）：

| 触发事件 | 说明 |
|---------|------|
| `handleAppendEntries` 收到合法 AE | 现任 Leader 存在且健康 |
| `handleRequestVote` 给候选人投票 | 集群中有候选人在运作 |
| `handleInstallSnapshot` 收到快照 | 现任 Leader 存在 |
| `becomeLeader` 自己当选 | 自己就是 Leader |
| `handleNodeAnnounce` 多数派上线 | 多数派同时重启，无旧 Leader |
| 30s 硬上限定时器 | 兜底 |

---

## 6. 改进 E — AppendEntries 快照边界 Bug 修复

### 6.1 问题场景

快照和日志复制**同时进行**时存在数据丢失 bug。

```
时刻 A：Leader 发出 AE(prevLogIndex=99, entries=[{100},{101},{102}])
时刻 B：Leader 触发 takeSnapshot(100) → snapshotIndex_=100
时刻 C：Follower 收到 InstallSnapshot(lastIndex=100)
         → Follower.snapshotIndex_ = 100
时刻 D：Follower 收到时刻 A 发出的旧 AE（网络延迟才到）
         → args.prevLogIndex=99 < snapshotIndex_=100
```

**旧代码在时刻 D 的行为**（buggy）：

```cpp
// 旧代码（handleAppendEntries 快照边界处理）
if (args.prevLogIndex < snapshotIndex_) {
    reply.success = true;
    done(json(reply).dump());
    return;  // ← entries=[{100},{101},{102}] 全部丢弃！
}
// Leader 收到 success，认为 Follower 已有 100-102，不再重发
// 但 Follower 的 log_ 中永久缺少 101、102 两条 → 数据永久不一致
```

### 6.2 修复实现步骤

**打开 `src/common/raft/RaftNode.cpp`，找到 `handleAppendEntries` 中处理快照边界的部分，替换为以下代码**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（handleAppendEntries § 一致性检查之前）：

```cpp
if (args.prevLogIndex < snapshotIndex_) {
    // 将 prevLogIndex 推到 snapshotIndex_，对应裁掉 entries 前缀
    uint64_t skipN = snapshotIndex_ - args.prevLogIndex;
    if (skipN >= args.entries.size()) {
        // entries 全部已被快照覆盖，无需追加；仍然 ack 让 leader 推进 nextIndex
        reply.success = true;
        done(json(reply).dump());
        return;
    }
    // 裁掉 index <= snapshotIndex_ 的前缀，保留后面部分继续追加
    args.entries.erase(args.entries.begin(),
                       args.entries.begin() + skipN);
    args.prevLogIndex = snapshotIndex_;
    args.prevLogTerm  = snapshotTerm_;
    // 继续走下面的正常一致性检查和追加流程
}
```

修复的关键在于精确区分两种情况：

| 情况 | skipN 与 entries.size() 关系 | 行为 |
|------|------------------------------|------|
| `prevLogIndex=99, snap=100, entries=[{100}]` | skipN=1 >= size=1 | 直接 ack（100 已被快照覆盖，无新条目）|
| `prevLogIndex=99, snap=100, entries=[{100},{101},{102}]` | skipN=1 < size=3 | 裁掉 entries[0]（index=100），追加 {101},{102} |
| `prevLogIndex=99, snap=100, entries=[]（心跳）` | skipN=1 >= size=0 | 直接 ack（正确）|

修复后，Follower 在时刻 D 的 log_ 变为：
```
修复前：[哨兵@100]                 ← 缺少 101、102
修复后：[哨兵@100] [101] [102]     ← 与 Leader 一致
```

---

## 7. 整体运行时理解

> 本节"放慢镜头"，对三个核心场景做完整的多线程逐步追踪。每步记录精确的变量状态快照，解释设计动机。

### 7.1 kv_node 对象所有权与线程归属

先打开 [examples/src/kv_node.cpp](examples/src/kv_node.cpp) 浏览顶层结构，建立对象归属的心理模型：

```
main()（T_main）
 │
 ├── raft::RaftNode node
 │    │
 │    ├── Eventloop loop_                    ← T_raft 独占
 │    │    ├── log_[]                         Raft 日志（只在 T_raft 读写）
 │    │    ├── commitIndex_（atomic）         任意线程可读
 │    │    ├── lastApplied_（atomic）         任意线程可读
 │    │    ├── state_（atomic<State>）        任意线程可读
 │    │    ├── nextIndex_/matchIndex_         只在 T_raft 读写（Leader 专用）
 │    │    ├── writeCallbacks_               只在 T_raft 读写（无锁）
 │    │    ├── startupGrace_ / announcedPeers_  只在 T_raft 读写
 │    │    └── peerClients_（AsyncRpcClient） 只在 T_raft 读写
 │    │
 │    ├── TcpServer rpcServer_               ← T_raft_rpc 线程
 │    │    └── 入站 RPC → handler → loop_.runInLoop → T_raft
 │    │
 │    └── std::thread loopThread_            ─── T_raft
 │
 ├── KvStateMachine sm
 │    └── std::mutex mu_ + map_[key]=value
 │         ├── 写：T_raft（apply/applySnapshot，通过 applyCallback_）
 │         └── 读：T_http_sub（get/serialize/size）
 │
 └── KvHttpServer kvSrv
      └── HttpServer srv_（T_http_main + T_http_sub）
           └── Connection（per client fd）← 归属某个 T_http_sub
```

**三大线程域的交互边界**（两处 `runInLoop`/`queueInLoop` 跨线程切换）：

```
T_http_sub                    T_raft                     T_http_sub
    │                            │                            │
    │──proposeAndNotify──────────▶  (跨线程切换点 1)           │
    │   loop_.runInLoop(lambda)   │                            │
    │                            │── applyCallback_→sm.apply()│
    │                            │── writeCallbacks_[idx]()   │
    │                            │   loop->queueInLoop  ──────▶ (切换点 2)
    │                            │                    conn->send│
    │◀────────── 客户端收到 applied 响应 ──────────────────────│
```

- `sm_.apply`（T_raft 写）与 `sm_.get`（T_http_sub 读）跨线程 → `std::mutex` 保护
- `writeCallbacks_`（T_raft 读写）→ 单线程，无锁
- `conn->send` 操作 → 只在 T_http_sub（`queueInLoop` 保证线程归属）

---

### 7.2 场景 A — 一次 PUT 请求的完整路径

**背景：为什么需要两阶段 chunked 响应**

标准 HTTP 在发送前必须知道完整内容（Content-Length）或用 Transfer-Encoding: chunked 流式发送。PUT /kv 的问题在于：

- **立刻知道的**（<1ms）：Leader 已接受命令，进入 Raft 队列
- **延迟才知道的**（1~100ms）：Raft 多数派确认并 apply 到状态机

如果同步等 Raft apply 再返回，IO 线程被占用，无法处理其他请求。如果只发"接受"就关连接，客户端无法知道写是否成功。

chunked 的解法：`HTTP 头 + 首块（accepted）`立即发 → handler 返回，线程空闲 → Raft apply 后通过 `queueInLoop` 发`尾块（applied）+ 终止块（0\r\n\r\n）`。

**场景设定**：3 节点集群，Node 0 是 Leader（term=2），`commitIndex=5, lastApplied=5`，log 共 6 条（index 0-5）。

客户端执行：`curl -s -L -X PUT http://localhost:8901/kv/hello -d "world"`

---

#### 第 1 步：TCP 字节到达，HttpServer 解析，路由分发

打开 [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp)，找到 `start()` 中的 PUT 路由注册：

```cpp
srv_->addAsyncPrefixRoute(HttpRequest::Method::kPut, "/kv/",
    [this](const HttpRequest &req, HttpResponse *resp, Connection *conn) {
        std::string key = extractKey(req.url());
        if (key.empty()) { /* 400 */ return; }
        handleWriteAsync("PUT " + key + " " + req.body(), req.url(), req, resp, conn);
    });
```

Node 0 的 T_http_sub 从 kevent 唤醒，读入 `PUT /kv/hello body=world`，匹配 `addAsyncPrefixRoute(kPut, "/kv/")` → 调用 `handleWriteAsync("PUT hello world", ...)`。

**此刻状态快照**：
```
线程              = T_http_sub
cmd               = "PUT hello world"
node_.isLeader()  = true（atomic 读）
resp->deferred_   = false
```

---

#### 第 2 步：非 Leader 分支 vs Leader 分支

打开 [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp)，`handleWriteAsync` 函数开头：

```cpp
const int      myId   = node_.getId();          // = 0
const uint64_t myTerm = node_.getCurrentTerm(); // atomic 读 = 2

if (!node_.isLeader()) {
    int         lid = node_.getLeaderId();
    std::string url = leaderUrl(path);
    if (!url.empty()) {
        resp->setStatus(HttpResponse::StatusCode::k307TemporaryRedirect, "Temporary Redirect");
        resp->addHeader("Location", url);
        resp->setContentType("application/json");
        resp->setBody(R"({"ok":false,"status":"redirect","leader":)" +
                      std::to_string(lid) + R"(,"leaderUrl":")" + url + R"("})");
    } else {
        resp->setStatus(HttpResponse::StatusCode::k503ServiceUnavailable, "No Leader");
        // ...
    }
    return;  // 同步返回，框架自动发 resp
}
```

Node 0 是 Leader，跳过非 Leader 分支，进入 Leader 路径。

**此刻状态快照**：
```
myId     = 0
myTerm   = 2
isLeader = true → 进入 Leader 分支
```

---

#### 第 3 步：发送 HTTP 头 + 首块（立即）

打开 [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp)，`handleWriteAsync` Leader 分支：

```cpp
std::string hdrs = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/x-ndjson\r\n"
                   "Transfer-Encoding: chunked\r\n";
for (const auto &[k, v] : resp->headers()) {
    hdrs += k + ": " + v + "\r\n";  // 加入中间件写入的 CORS 头
}
hdrs += "Connection: keep-alive\r\n\r\n";

const int      clusterSize  = node_.getPeerCount();   // = 3
const int      quorum       = node_.getQuorum();      // = 2
const uint64_t commitBefore = node_.getCommitIndex(); // atomic = 5
const uint64_t lastLogBefore= node_.getLastLogIndex(); // ≈ 5

{
    json j = {
        {"status",            "accepted"},
        {"leader",            0},
        {"term",              2},
        {"clusterSize",       3},
        {"quorum",            2},
        {"commitIndexBefore", 5},
        {"lastLogIndexBefore",5},
        {"cmd",               "PUT hello world"}
    };
    conn->send(hdrs + makeChunk(j.dump() + "\n"));
}
resp->setDeferred(true);
```

`conn->send()` 把数据写入 `Connection::outputBuffer_`，注册 EVFILT_WRITE，下轮 poll 发出网卡。客户端**立刻**收到第一行 JSON。

`resp->setDeferred(true)` 告诉框架：跳过自动发送 resp，handler 自己管后续发送。

**此刻状态快照**：
```
conn->outputBuf_  ≈ 600字节（HTTP头 + 首块 JSON）
resp->deferred_   = true
客户端已收到      = {"status":"accepted","cmd":"PUT hello world",...}
```

---

#### 第 4 步：获取 alive/loop，调 proposeAndNotify，handler 返回

打开 [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp)，`handleWriteAsync` 尾部：

```cpp
auto alive   = conn->aliveFlag();   // weak_ptr<bool>，连接析构时 *bool = false
auto *loop   = conn->getLoop();     // T_http_sub 的 Eventloop*
auto  t0     = std::chrono::steady_clock::now();
auto *nodePtr= &node_;
auto *smPtr  = &sm_;

node_.proposeAndNotify(cmd,
    [alive, loop, conn, myId, myTerm, clusterSize, quorum, commitBefore, nodePtr, smPtr, t0]
    (bool ok, uint64_t logIndex) {
        loop->queueInLoop([alive, conn, ok, logIndex,
                           myId, myTerm, clusterSize, quorum, commitBefore,
                           nodePtr, smPtr, t0]() {
            if (auto f = alive.lock(); !f || !*f) return;
            const double dtMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            std::string body;
            if (ok) {
                json j = {
                    {"status",           "applied"},
                    {"ok",               true},
                    {"logIndex",         logIndex},
                    {"commitIndexAfter", nodePtr->getCommitIndex()},
                    {"lastApplied",      nodePtr->getLastApplied()},
                    {"kvSize",           smPtr->size()},
                    {"latencyMs",        dtMs}
                };
                body = j.dump() + "\n";
            } else {
                body = R"({"status":"error","ok":false,"reason":"leadership lost"})" "\n";
            }
            conn->send(makeChunk(body) + "0\r\n\r\n");
        });
    });
// handler 函数体结束，T_http_sub 继续 poll 其他事件
```

为什么要 `aliveFlag()`？`proposeAndNotify` 的回调在 T_raft 触发，可能 1~100ms 后。期间连接可能断开，`Connection` 被析构。裸指针 `conn` 就会是悬空指针（use-after-free）。`aliveFlag()` 返回 `weak_ptr<bool>`：连接存活时 `alive.lock()` 非空且 `*f=true`；连接析构时 `*f=false`，回调安全跳过。

**此刻状态快照**：
```
T_http_sub       = 空闲，继续 poll
T_raft pendingFunctors = [<proposeAndNotify lambda>]（等待 T_raft 执行）
writeCallbacks_  = {}（还未写入）
```

---

#### 第 5 步：runInLoop 投递到 T_raft（跨线程切换点 1）

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `proposeAndNotify` 实现：

```cpp
void RaftNode::proposeAndNotify(const std::string &cmd, std::function<void(bool, uint64_t)> done) {
    // 线程安全：实际工作在 loop_ 线程执行。
    // done(true, idx)  = 命令已应用到状态机，idx 是被分配的全局日志下标
    // done(false, idx) = 丢失 Leader 身份（idx 为该条目被分配的下标，未分配时为 0）
    loop_.runInLoop([this, cmd, done = std::move(done)]() mutable {
        if (state_.load() != State::Leader) {
            done(false, 0);
            return;
        }
        log_.push_back(LogEntry{currentTerm_.load(), cmd});
        persistLog();
        uint64_t idx = lastLogIndex();
        LOG_INFO << "[Node " << id_ << "] proposeAndNotify 追加日志 index=" << idx
                 << " cmd=" << cmd;
        writeCallbacks_[idx] = std::move(done);
        for (const auto &peer : peers_) {
            if (peer.id == id_) continue;
            replicateLog(peer);
        }
    });
}
```

`loop_.runInLoop`：当前线程是 T_http_sub（不是 T_raft）→ lambda 被 push_back 到 `loop_.pendingFunctors_`（mutex 保护）→ 向 T_raft 的 `wakeupWriteFd_` 写 1 字节 → T_raft 的 kevent 返回 → `doPendingFunctors()` 执行 lambda。

**此刻状态快照（lambda 执行后，在 T_raft）**：
```
log_             = [..., {term=2, cmd="PUT hello world"}]  (index 0-6)
writeCallbacks_  = {6: <HTTP done lambda>}
nextIndex_       = {1:6, 2:6}
matchIndex_      = {1:5, 2:5}
```

---

#### 第 6 步：replicateLog 协程并发发出 AppendEntries

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `replicateLog` 协程（§5 段）。它为 peer 1 和 peer 2 各发射一个 `FireAndForget` 协程：

```
peer 1 的 AppendEntriesArgs：
  prevLogIndex = 5    (nextIndex[1]=6, prevIdx=5)
  prevLogTerm  = 2    (log_[5].term)
  leaderCommit = 5    (commitIndex_)
  entries      = [{term=2, cmd="PUT hello world"}]  (log_[6])
→ co_await callAsyncCo("AppendEntries", ...) — 协程挂起，T_raft 继续执行其他任务
```

Follower（peer 1）的 `handleAppendEntries` 在 T_raft（Node 1）处理：
```
args.term=2 >= currentTerm_=2 ✓
prevLogIndex=5 >= snapshotIndex_=0 ✓（无快照边界问题）
log_[5].term=2 == prevLogTerm=2 ✓（前缀一致性通过）
追加 log_[6] = {2, "PUT hello world"}
leaderCommit=5 > commitIndex_=5？ 否 → commitIndex 不变
reply = {term=2, success=true}
```

---

#### 第 7 步：advanceCommitIndex 推进 commitIndex

peer 1 先回复，协程在 T_raft（Node 0）恢复。打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `replicateLog` 处理成功回复的部分，以及 `advanceCommitIndex`：

```cpp
void RaftNode::advanceCommitIndex() {
    // Raft Figure 8 规则：Leader 只能直接提交「当前 term」的条目。
    uint64_t lastIdx = lastLogIndex();  // = 6
    for (uint64_t n = lastIdx; n > commitIndex_.load(); --n) {  // n=6 > 5
        if (n <= snapshotIndex_) break;              // 0 < 6，不触发
        if (logAt(n).term != currentTerm_.load()) continue; // term=2 == 2 ✓
        int count = 1; // 算上自己
        for (const auto &peer : peers_) {
            if (peer.id == id_) continue;
            if (matchIndex_.count(peer.id) && matchIndex_[peer.id] >= n) ++count;
        }
        // peer1: matchIndex_[1]=6 >= 6 → count=2，达到 quorum=2
        if (count >= quorum_) {
            commitIndex_.store(n);  // commitIndex_ = 6
            applyCommitted();
            break;
        }
    }
}
```

**此刻状态快照**：
```
matchIndex_[1]  = 6（peer 1 已确认）
commitIndex_    = 6（从 5 推进到 6）
lastApplied_    = 5（尚未 apply）
```

---

#### 第 8 步：applyCommitted 触发状态机 + 兑现 HTTP 回调

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `applyCommitted`：

```cpp
void RaftNode::applyCommitted() {
    while (lastApplied_.load() < commitIndex_.load()) {  // 5 < 6
        uint64_t idx = lastApplied_.load() + 1;          // = 6
        if (idx > lastLogIndex()) break;
        lastApplied_.store(idx);                          // lastApplied_ = 6
        LOG_INFO << "[Node " << id_ << "] 应用日志 index=" << idx
                 << " 命令=" << logAt(idx).cmd;
        if (applyCallback_) applyCallback_(idx, logAt(idx).cmd);
        // → sm_.apply(6, "PUT hello world")
        //   → lock_guard lk(mu_)
        //   → map_["hello"] = "world"
        //   → 释放 mu_
        auto it = writeCallbacks_.find(idx);  // 找到 index=6 的回调
        if (it != writeCallbacks_.end()) {
            it->second(true, it->first);   // done(true, 6) — 在 T_raft 触发
            writeCallbacks_.erase(it);
        }
    }
}
```

`done(true, 6)` 触发的是 `handleWriteAsync` 中注册的 lambda（仍在 T_raft 线程）：

```cpp
[alive, loop, conn, ...](bool ok=true, uint64_t logIndex=6) {
    loop->queueInLoop([alive, conn, ok, logIndex, ...]() {
        // 这部分被投递到 T_http_sub（跨线程切换点 2）
    });
}
```

`queueInLoop` 把 lambda 推入 T_http_sub 的 pendingFunctors，写 wakeupWriteFd，唤醒 T_http_sub。

**此刻状态快照**：
```
sm_.map_         = {"hello": "world"}
lastApplied_     = 6
writeCallbacks_  = {}
T_http_sub pendingFunctors = [<发送 applied 块的 lambda>]
```

---

#### 第 9 步：T_http_sub 发送最终响应（跨线程切换点 2）

T_http_sub 被唤醒，`doPendingFunctors` 取出 lambda。打开 [examples/src/kv/KvHttpServer.cpp](examples/src/kv/KvHttpServer.cpp)，即 `proposeAndNotify` 回调中的 `queueInLoop` 内的 lambda：

```cpp
if (auto f = alive.lock(); !f || !*f) return;  // 连接仍活着
const double dtMs = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - t0).count();  // ≈ 1.3ms
json j = {
    {"status",           "applied"},
    {"ok",               true},
    {"logIndex",         6},
    {"commitIndexAfter", 6},
    {"lastApplied",      6},
    {"kvSize",           1},
    {"latencyMs",        1.3}
};
conn->send(makeChunk(j.dump() + "\n") + "0\r\n\r\n");
```

客户端收到的完整 chunked 响应：
```http
HTTP/1.1 200 OK
Content-Type: application/x-ndjson
Transfer-Encoding: chunked

[首块]
{"status":"accepted","leader":0,"term":2,"cmd":"PUT hello world",...}

[尾块]
{"status":"applied","ok":true,"logIndex":6,"latencyMs":1.3,...}

[终止块]
0\r\n\r\n
```

**端到端路径总结**：
```
T_http_sub          T_raft              T_raft(Node1)     T_http_sub
    │                  │                    │                 │
    │ handleWriteAsync │                    │                 │
    ├─conn.send(首块)  │                    │                 │
    │ setDeferred=true │                    │                 │
    ├──proposeAndNotify│                    │                 │
    │  runInLoop ──────▶                    │                 │
    │ [handler返回]    │ log_.push(6)       │                 │
    │                  │ writeCallbacks_[6] │                 │
    │                  │ replicateLog ─────▶│                 │
    │                  │ co_await[挂起]     │ handleAE        │
    │                  │                    │ reply.success   │
    │                  │ ◀────reply─────────│                 │
    │                  │ commitIndex_=6     │                 │
    │                  │ applyCommitted()   │                 │
    │                  │ sm_.apply()        │                 │
    │                  │ writeCallbacks_[6] │                 │
    │                  │ queueInLoop ───────┼─────────────────▶
    │                  │                    │   conn.send     │
    │ ◀── 客户端收到 applied ────────────────────────────────│
```

**error path：Leader step down**

若 T_raft 在 `applyCommitted` 之前触发 `becomeFollower(newTerm)`，打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`becomeFollower` 函数：

```cpp
if (!writeCallbacks_.empty()) {
    for (auto &[idx, cb] : writeCallbacks_) cb(false, idx);  // cb(false, 6)
    writeCallbacks_.clear();
}
```

`cb(false, 6)` 触发 `queueInLoop`，切回 T_http_sub，`conn->send` 发出 `{"status":"error","ok":false,"reason":"leadership lost"}` 尾块，客户端应重试。

---

### 7.3 场景 B — 节点重启（NodeAnnounce/startupGrace 的协作）

**背景：指数退避为何在 Raft 重启场景变成问题**

指数退避是正确的——避免对"真正死掉"的节点疯狂重试。但 Raft 的特殊性是：死掉的节点可能**随时重启**，此时需要立刻重连而不是等 6400ms。

**场景设定**：3 节点集群，Node 0 是 Leader（term=2），Node 2 宕机 5 秒后重启。Leader 对 Node 2 的 `AsyncRpcClient` 已退避到 3200ms 重试间隔。

---

#### 第 1 步：Node 2 重启，start() 检测冷/热启动

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`start()` 内 `runInLoop` 的启动初始化段（持久化恢复完成之后）：

```cpp
bool freshStart = (currentTerm_.load() == 0
                   && log_.size() == 1
                   && snapshotIndex_ == 0);
startupGrace_ = !freshStart;
```

Node 2 有持久化数据（`currentTerm_=2, votedFor=0`，log 有 5 条）→ `freshStart=false` → `startupGrace_=true`。

`resetElectionTimer()` 读取 `startupGrace_=true` → 超时取 1500~3000ms（而非稳态 150~300ms）。

**此刻状态快照（Node 2，T_raft）**：
```
startupGrace_  = true
选举超时       ≈ 2000ms（随机）
```

---

#### 第 2 步：Node 2 在 100ms 后广播 NodeAnnounce

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`start()` 内 `loop_.runAfter(0.1, ...)` 段：

```cpp
loop_.runAfter(0.1, [this] {
    std::string body = json{{"nodeId", id_}}.dump();  // id_ = 2
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        getOrCreateClient(peer)->callAsync(
            "NodeAnnounce", body,
            [id = id_, peerId = peer.id](bool ok, const std::string &) {
                if (ok)
                    LOG_INFO << "[Node " << id << "] NodeAnnounce → Node "
                             << peerId << " 已送达";
            },
            /*timeoutMs=*/500);
    }
    LOG_INFO << "[Node " << id_ << "] 已向所有 peer 广播上线通知（NodeAnnounce）";
});
```

Node 2 向 Node 0 和 Node 1 发出 NodeAnnounce RPC。Node 2 重启后 `peerClients_` 为空，`getOrCreateClient` 创建新客户端并立即 TCP connect（通常 <10ms 建连成功）。

**此刻状态快照（Node 2，t=100ms）**：
```
NodeAnnounce 已发出到 Node 0 和 Node 1
```

---

#### 第 3 步：Node 0（Leader）收到 NodeAnnounce，立即重置退避

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `handleNodeAnnounce`：

```cpp
loop_.runInLoop([this, senderId=2, done = std::move(done)]() mutable {
    done(R"({})");  // 立即回复

    for (const auto &peer : peers_) {
        if (peer.id == 2) {
            LOG_INFO << "[Node 0] 收到 Node 2 的重上线通知（NodeAnnounce），重置连接退避并立即重连";
            getOrCreateClient(peer)->wakeUp();  // ← 重置退避时钟为"立即重试"
            break;
        }
    }
    // Node 0 是 Leader（leaderId_=0），不进入 announcedPeers_ 分支
});
```

`wakeUp()` 重置 `AsyncRpcClient` 内部的 `nextRetryMs_ = 0`，下一轮 IO 循环（<50ms）发起 TCP connect 到 Node 2。

**此刻状态快照（Node 0，t≈110ms）**：
```
Node 2 的 AsyncRpcClient：nextRetryMs_ = 0（立即重试）
下次心跳（50ms 后）→ replicateLog(Node 2) → TCP connect → 建连 → AE 到达
```

---

#### 第 4 步：连接恢复，Node 0 发心跳到 Node 2

50ms 心跳 `heartbeatTick()` → `replicateLog(peer2)` → AppendEntries 发往 Node 2。

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`handleAppendEntries` 在 Node 2 的 T_raft 执行：

```cpp
loop_.runInLoop([this, args, done = std::move(done)]() mutable {
    if (args.term > currentTerm_.load()) becomeFollower(args.term);
    // args.term=2 == currentTerm_=2，不触发 becomeFollower
    state_.store(State::Follower);
    leaderId_ = 0;
    startupGrace_ = false;  // ← 收到合法 Leader AE，立刻退出宽限期！
    resetElectionTimer();   // 重置为正常 150-300ms 超时
    // ...
```

**时序对比（三种情况）**：
```
情况 1：无任何修复
  t=0      Node 2 重启
  t=150ms  选举超时，term++=3，发 RequestVote
  t=200ms  Node 0 看到 term=3 > 2，step down
  写中断   ≈ 350ms+

情况 2：只有 NodeAnnounce，无 grace
  t=0      Node 2 重启
  t=100ms  NodeAnnounce → Node 0 立即重连
  t=150ms  选举超时可能在重连完成前触发（150ms 边界模糊）
  写中断   偶发

情况 3：NodeAnnounce + startupGrace（当前实现）
  t=0      Node 2 重启，startupGrace_=true，超时=1500~3000ms
  t=100ms  NodeAnnounce → Node 0 重置退避
  t=150ms  Node 0 与 Node 2 重连成功
  t=200ms  Node 0 心跳到 Node 2 → startupGrace_=false
  写中断   0ms（集群完全无感知）
```

---

### 7.4 场景 C — AE 快照边界 Bug 的触发与修复

**背景：Raft 全异步，快照和 AE 会并发到达**

Leader 每 50ms 发心跳/复制，快照由 `applyCallback_` 触发（每 N 条 apply 一次），两者可能在网络上交错到达 Follower。

```
时刻 A（t=0ms）：
  Leader.snapshotIndex_ = 99
  Leader 发出 AE：prevLogIndex=99, entries=[{100,cmd1},{101,cmd2},{102,cmd3}]
  此 AE 在网络飞行中

时刻 B（t=1ms）：
  Leader 触发 takeSnapshot(100, data)
  → snapshotIndex_ = 100
  → 发出 InstallSnapshot(lastIndex=100)

时刻 C（t=5ms）：
  Follower 收到 InstallSnapshot(lastIndex=100)
  → Follower.snapshotIndex_ = 100，状态机恢复到 index=100

时刻 D（t=10ms）：
  Follower 收到时刻 A 的旧 AE
  → args.prevLogIndex=99 < snapshotIndex_=100
```

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，找到 `handleAppendEntries` 中快照边界处理段，对比修复前后：

**旧代码（bug）**：
```cpp
if (args.prevLogIndex < snapshotIndex_) {  // 99 < 100 ✓
    reply.success = true;
    done(json(reply).dump());
    return;  // ← entries=[{100},{101},{102}] 全部丢弃！
}
// Leader 认为 Follower 已有 100-102，不再重发
// 但 Follower log_ 中永久缺少 101、102 → 数据不一致
```

**新代码（修复）**：
```cpp
if (args.prevLogIndex < snapshotIndex_) {
    uint64_t skipN = snapshotIndex_ - args.prevLogIndex;  // = 100 - 99 = 1
    if (skipN >= args.entries.size()) {  // 1 >= 3？ 否
        reply.success = true;
        done(json(reply).dump());
        return;
    }
    // 裁掉 entries[0]（代表 index=100，已被快照覆盖）
    args.entries.erase(args.entries.begin(),
                       args.entries.begin() + skipN);
    // entries 现在 = [{101,cmd2},{102,cmd3}]
    args.prevLogIndex = snapshotIndex_;  // = 100
    args.prevLogTerm  = snapshotTerm_;
    // 继续正常追加流程
}
// 追加 entries[0]={101,cmd2}, entries[1]={102,cmd3}
// Follower 的 log_ = [哨兵@100][101][102] ← 与 Leader 一致 ✓
```

**Follower log_ 状态对比**：
```
修复前（时刻 D 后）：
  [哨兵@100]          ← snapshotIndex_=100
  ← 缺少 101、102！后续 AE prevLogIndex=102 > lastLogIndex=100，
    一致性检查一直失败，Leader 逐步回退 nextIndex 但 101 可能也被快照了 → 不可恢复

修复后（时刻 D 后）：
  [哨兵@100]
  [101, cmd2]
  [102, cmd3]
  ✓ 与 Leader 一致
```

```
main()（T_main）
 │
 ├── raft::RaftNode node
 │    │
 │    ├── Eventloop loop_                    ← T_raft 独占
 │    │    ├── log_[]                         Raft 日志（只在 T_raft 读写）
 │    │    ├── commitIndex_（atomic）         任意线程可读
 │    │    ├── lastApplied_（atomic）         任意线程可读
 │    │    ├── state_（atomic<State>）        任意线程可读
 │    │    ├── nextIndex_/matchIndex_         只在 T_raft 读写（Leader 专用）
 │    │    ├── writeCallbacks_               只在 T_raft 读写（无锁）
 │    │    ├── startupGrace_ / announcedPeers_  只在 T_raft 读写
 │    │    └── peerClients_（AsyncRpcClient） 只在 T_raft 读写
 │    │
 │    ├── TcpServer rpcServer_               ← T_raft_rpc 线程
 │    │    └── 入站 RPC → handler → loop_.runInLoop → T_raft
 │    │
 │    └── std::thread loopThread_            ─── T_raft
 │
 ├── KvStateMachine sm
 │    └── std::mutex mu_ + map_[key]=value
 │         ├── 写：T_raft（apply/applySnapshot，通过 applyCallback_）
 │         └── 读：T_http_sub（get/serialize/size）
 │
 └── KvHttpServer kvSrv
      └── HttpServer srv_（T_http_main + T_http_sub）
           └── Connection（per client fd）← 归属某个 T_http_sub
```

**三大线程域的交互边界**：

```
T_http_sub                    T_raft                     T_http_sub（同一或不同）
    │                            │                            │
    │──proposeAndNotify──────────▶                            │
    │   (runInLoop 投递到T_raft)  │                            │
    │                            │── applyCallback_ ──▶ sm.apply()
    │                            │── writeCallbacks_[idx]()   │
    │                            │   queueInLoop ────────────▶│
    │                            │                  conn->send│
    │◀──────── 客户端收到 applied 响应 ─────────────────────────│
```

关键点：
- `sm_.apply`（T_raft 写）与 `sm_.get`（T_http_sub 读）跨线程——`std::mutex` 保护
- `writeCallbacks_`（T_raft 写/读）——单线程，无需加锁
- `conn` 的所有 `send` 操作——只在 T_http_sub（`queueInLoop` 保证）

---

## 8. 各模块职责速查表

| 模块/函数 | 所在线程 | 调用时机 | 职责一句话 |
|-----------|---------|---------|-----------|
| `KvStateMachine::apply` | T_raft | `applyCallback_`（commitIndex 推进时） | 解析命令字符串，加锁写/删 `map_` |
| `KvStateMachine::get` | T_http_sub | GET /kv/:key handler | 加锁读 `map_`，返回 value |
| `KvStateMachine::serialize` | T_raft / T_http_sub | `applyCallback_` 每 N 条触发 / /admin/scan handler | 加锁把 map dump 成 JSON 字符串 |
| `KvStateMachine::applySnapshot` | T_raft | `snapshotApplyCallback_`（InstallSnapshot 后） | 加锁 move 替换整个 map |
| `KvHttpServer::handleWriteAsync` | T_http_sub | PUT/DELETE 路由 handler | Leader 判断 → 发首块 → proposeAndNotify → setDeferred |
| `KvHttpServer::start` (GET /kv/) | T_http_sub | GET 路由 handler | 直接 `sm_.get()` 本地读（最终一致性），带 servedBy 元数据 |
| `RaftNode::proposeAndNotify` | 任意（内部切 T_raft） | HTTP PUT/DELETE handler | 跨线程入口：追加日志 + 注册 writeCallbacks_ + 触发复制 |
| `RaftNode::applyCommitted` | T_raft | advanceCommitIndex 或 AE commit 推进时 | 顺序 apply 到状态机 + 兑现 writeCallbacks_ |
| done 回调中的 `queueInLoop` | T_raft → T_http_sub | `applyCommitted` 兑现时 | 切回 T_http_sub 发送 applied 块 |
| `RaftNode::handleNodeAnnounce` | T_raft_rpc → T_raft | peer 广播 NodeAnnounce 时 | `wakeUp` 出站 client（重置退避）+ 多数派同时启动加速选举 |
| `electionTimerFired` 的 grace 守卫 | T_raft | 选举计时器超时 | `startupGrace_` 期间抑制选举，仅重置定时器 |
| AE 快照边界修复（handleAppendEntries 内） | T_raft | handleAppendEntries | 裁掉快照范围内的前缀条目，保留后续条目继续追加 |

---

## 9. 工程化（kv_node 主程序与 Web Dashboard）

### 新建 `examples/src/kv_node.cpp`，写入以下全部内容

这是三个组件（RaftNode / KvStateMachine / KvHttpServer）的装配点，`applyCallback_` 和 `snapshotApplyCallback_` 必须在 `node.start()` 之前注册。

来自 [examples/src/kv_node.cpp](examples/src/kv_node.cpp)：

```cpp
// kv_node.cpp —— 分布式 KV 存储节点（Raft + KvStateMachine + KvHttpServer）
//
// 用法（默认 5 节点集群）：
//   ./kv_node --id 0 [--persist] [--snapshot-every 100] [--http-port 8901]
//
// 端口约定：
//   节点 N  Raft RPC 端口 = 19001 + N
//   节点 N  HTTP API 端口 = 8901  + N

#include "kv/KvHttpServer.h"
#include "kv/KvStateMachine.h"
#include "log/Logger.h"
#include "net/SignalHandler.h"
#include "raft/FileStorage.h"
#include "raft/RaftNode.h"
#ifdef MCPP_HAS_ROCKSDB
#    include "raft/RocksDBStorage.h"
#endif
#include <atomic>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>

static constexpr uint16_t kRpcBasePort  = 19001;
static constexpr uint16_t kHttpBasePort = 8901;

// ── ANSI 颜色宏 ──────────────────────────────────────────────────────────────
#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_GREEN  "\033[32m"
#define C_CYAN   "\033[36m"
#define C_YELLOW "\033[33m"
#define C_GRAY   "\033[90m"
#define C_BLUE   "\033[34m"

static void printBanner(int myId, int nodes, uint16_t httpPort, uint16_t rpcPort,
                        bool persist, const std::string &staticDir) {
    std::cout
        << "\n"
        << C_BOLD C_BLUE
        << "╔══════════════════════════════════════════════════════════╗\n"
        << "║         Airi Distributed KV Cluster  —  Node " << myId << "          ║\n"
        << "╚══════════════════════════════════════════════════════════╝"
        << C_RESET "\n\n"
        << C_BOLD "  集群信息" C_RESET "\n";

    for (int i = 0; i < nodes; ++i) {
        std::cout << "    Node " << i
                  << "  RPC :1900" << (i+1)
                  << "  HTTP :" << (kHttpBasePort + i);
        if (i == myId) std::cout << C_YELLOW "  ← YOU" C_RESET;
        std::cout << "\n";
    }
    std::cout
        << "\n" C_BOLD "  本节点" C_RESET "\n"
        << "    HTTP 监听   :" << httpPort << "\n"
        << "    RPC  监听   :" << rpcPort  << "\n"
        << "    持久化      " << (persist ? C_GREEN "开启" C_RESET : C_GRAY "关闭" C_RESET) << "\n"
        << "    静态文件    " C_GRAY << staticDir << C_RESET "\n"
        << "\n" C_BOLD "  仪表盘 (浏览器打开)" C_RESET "\n"
        << "    " C_CYAN "http://127.0.0.1:" << httpPort << "/" C_RESET "\n"
        << "\n" C_BOLD "  curl 示例" C_RESET "\n"
        << C_GRAY "    curl -s -L -X PUT http://127.0.0.1:" << httpPort << "/kv/hello -d \"world\"\n"
        << "    curl -s http://127.0.0.1:" << httpPort << "/kv/hello\n"
        << C_RESET
        << "\n" C_GRAY "  按 Ctrl+C 退出" C_RESET "\n"
        << "──────────────────────────────────────────────────────────\n\n"
        << std::flush;
}

int main(int argc, char **argv) {
    int      myId          = -1;
    int      nodes         = 5;
    bool     persist       = false;
    int      snapshotEvery = 100;
    uint16_t httpPortOverride = 0;
    std::string staticDir = "examples/static/kv";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--persist") == 0) {
            persist = true;
        } else if (i + 1 < argc) {
            if      (std::strcmp(argv[i], "--id") == 0)
                myId = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--nodes") == 0)
                nodes = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--snapshot-every") == 0)
                snapshotEvery = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--http-port") == 0)
                httpPortOverride = static_cast<uint16_t>(std::stoi(argv[i + 1]));
            else if (std::strcmp(argv[i], "--static-dir") == 0)
                staticDir = argv[i + 1];
        }
    }

    if (myId < 0 || myId >= nodes) {
        std::cerr << "用法: kv_node --id <0.." << (nodes-1) << ">"
                  << " [--nodes N] [--persist] [--snapshot-every N]\n";
        return 1;
    }

    Logger::setLogLevel(Logger::WARN);

    const uint16_t myRpcPort  = static_cast<uint16_t>(kRpcBasePort  + myId);
    const uint16_t myHttpPort = httpPortOverride
                              ? httpPortOverride
                              : static_cast<uint16_t>(kHttpBasePort + myId);

    printBanner(myId, nodes, myHttpPort, myRpcPort, persist, staticDir);

    // ── 集群成员表 ────────────────────────────────────────────────────────────
    std::vector<raft::Peer> peers;
    for (int i = 0; i < nodes; ++i)
        peers.push_back({i, "127.0.0.1", static_cast<uint16_t>(kRpcBasePort + i)});

    // ── Raft 节点 ─────────────────────────────────────────────────────────────
    raft::RaftNode node(myId, peers, myRpcPort);

    if (persist) {
        std::string dataDir = "./kv_raft_state/node_" + std::to_string(myId);
#ifdef MCPP_HAS_ROCKSDB
        node.setStorage(std::make_unique<raft::RocksDBStorage>(dataDir));
#else
        node.setStorage(std::make_unique<raft::FileStorage>(dataDir));
#endif
    }

    // ── 状态机 ────────────────────────────────────────────────────────────────
    KvStateMachine sm;

    std::atomic<int> applyCount{0};
    node.setApplyCallback([&node, &sm, myId, snapshotEvery, &applyCount](
                              uint64_t index, const std::string &cmd) {
        sm.apply(index, cmd);
        if (snapshotEvery > 0 && ++applyCount % snapshotEvery == 0)
            node.takeSnapshot(sm.serialize());
    });

    node.setSnapshotApplyCallback([&sm](uint64_t index, const std::string &data) {
        sm.applySnapshot(index, data);
    });

    // ── HTTP KV 服务器 ────────────────────────────────────────────────────────
    KvHttpServer kvSrv(node, sm, myHttpPort, kHttpBasePort, staticDir);

    // ── 信号处理 ─────────────────────────────────────────────────────────────
    static std::atomic<bool> stopFlag{false};
    Signal::signal(SIGINT,  [] { stopFlag.store(true); });
    Signal::signal(SIGTERM, [] { stopFlag.store(true); });

    node.start();

    std::thread httpThread([&kvSrv] { kvSrv.start(); });

    // ── 主循环：ANSI 彩色状态行，每 2s 刷新 ─────────────────────────────────
    auto lastTerm    = uint64_t(-1);
    auto lastState   = raft::State::Follower;

    while (!stopFlag.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        auto state  = node.getState();
        auto term   = node.getCurrentTerm();
        auto commit = node.getCommitIndex();
        auto applied= node.getLastApplied();
        auto leader = node.getLeaderId();
        auto kvSz   = sm.size();

        if (state != lastState || term != lastTerm) {
            lastState = state;
            lastTerm  = term;
            if (state == raft::State::Leader)
                std::cout << C_BOLD C_GREEN
                          << "  *** Node " << myId << " 成为 Leader (term=" << term << ") ***"
                          << C_RESET "\n";
            else if (state == raft::State::Candidate)
                std::cout << C_YELLOW
                          << "  Node " << myId << " 发起选举 (term=" << term << ")"
                          << C_RESET "\n";
            else
                std::cout << C_CYAN
                          << "  Node " << myId << " 成为 Follower (term=" << term
                          << ", leader=" << leader << ")"
                          << C_RESET "\n";
        }

        const char *stateColor = C_CYAN;
        const char *stateName  = "Follower";
        if (state == raft::State::Leader)    { stateColor = C_GREEN;  stateName = "LEADER   "; }
        if (state == raft::State::Candidate) { stateColor = C_YELLOW; stateName = "Candidate"; }

        std::cout << C_GRAY "[Node " << myId << "] " C_RESET
                  << stateColor << stateName << C_RESET
                  << C_GRAY "  term=" C_RESET << std::setw(3) << term
                  << C_GRAY "  commit=" C_RESET << std::setw(4) << commit
                  << C_GRAY "  applied=" C_RESET << std::setw(4) << applied
                  << C_GRAY "  leader=" C_RESET << (leader < 0 ? "?" : std::to_string(leader))
                  << C_GRAY "  kv=" C_RESET << kvSz
                  << "\n" << std::flush;
    }

    kvSrv.stop();
    httpThread.join();
    node.stop();
    std::cout << C_GRAY "[Node " << myId << "] 已退出" C_RESET "\n";
    return 0;
}
```

**关键点理解**：

- **快照触发**：`applyCallback_` 里每 apply `snapshotEvery` 条就调一次 `node.takeSnapshot(sm.serialize())`，把状态机当前的 JSON 序列化结果交给 Raft 层压缩日志。`takeSnapshot` 通过 `runInLoop` 异步执行，对 Raft 层透明。
- **`applyCallback_` 和 `snapshotApplyCallback_` 必须在 `node.start()` 之前注册**，否则启动后第一批 apply 找不到回调（尤其是持久化恢复阶段会立刻调用 `snapshotApplyCallback_`）。
- **Web Dashboard**：`examples/static/kv/index.html` + `app.js`，每 1s 轮询 `/admin/raft`，实时展示集群拓扑和 KV 操作面板。

---

## 10. 验证

```bash
cmake --build build --target kv_node -j

# 启动 5 节点集群（HTTP 端口：8901-8905）
./build/examples/kv_node --id 0 --nodes 5 &
./build/examples/kv_node --id 1 --nodes 5 &
./build/examples/kv_node --id 2 --nodes 5 &
./build/examples/kv_node --id 3 --nodes 5 &
./build/examples/kv_node --id 4 --nodes 5 &

sleep 2  # 等待选举

# 写入（-L 自动跟随 307 重定向到 Leader）
curl -s -L -X PUT http://localhost:8901/kv/hello -d "world"
# 预期输出（两行 NDJSON）：
# {"status":"accepted","cmd":"PUT hello world",...}
# {"status":"applied","ok":true,"logIndex":2,...,"latencyMs":1.3}

# 从任意节点读（本地读，最终一致性）
curl -s http://localhost:8902/kv/hello
# → {"ok":true,"key":"hello","value":"world","servedBy":1,"servedByState":"Follower",...}

# 节点状态
curl -s http://localhost:8901/admin/raft | python3 -m json.tool

# Web 仪表盘
open http://localhost:8901/
```

验证 NodeAnnounce（重启场景）：
```bash
# Kill Node 4，等 3s，重启，观察日志
kill %5 && sleep 3
./build/examples/kv_node --id 4 --nodes 5 &
# 预期 Node 4 日志：[Node 4] 已向所有 peer 广播上线通知（NodeAnnounce）
# 预期 Leader 日志：[Node 0] 收到 Node 4 的重上线通知（NodeAnnounce），重置连接退避并立即重连
# 集群写请求全程无 503
```

验证快照边界 Bug 修复：
```bash
./build/examples/kv_node --id 0 --nodes 3 --persist --snapshot-every 10 &
./build/examples/kv_node --id 1 --nodes 3 --persist --snapshot-every 10 &
./build/examples/kv_node --id 2 --nodes 3 --persist --snapshot-every 10 &
sleep 2

# 写入 25 条（触发快照）
for i in $(seq 1 25); do
    curl -s -L -X PUT http://localhost:8901/kv/key$i -d "val$i" > /dev/null
done
sleep 1

# 所有节点应有相同 KV 数量
curl -s http://localhost:8901/admin/scan | python3 -m json.tool | grep '"count"'
curl -s http://localhost:8902/admin/scan | python3 -m json.tool | grep '"count"'
curl -s http://localhost:8903/admin/scan | python3 -m json.tool | grep '"count"'
# 三者应相同
```

---

## 11. 局限与下一步

| 局限 | Day37 的解法 |
|------|-------------|
| RPC payload 是 JSON（大 value 时 base64 膨胀 33%，多次拷贝） | Day37 迁移到 Protobuf + bypass 旁路（value 字节绕过序列化，零拷贝） |
| 运维切主需 kill 进程（即使有 NodeAnnounce，仍有短暂心跳空窗） | Day37 实现完整 Leader Transfer（TimeoutNow RPC），写空窗 ~1ms |
| Pre-Vote 机制尚不完整（孤立节点可能反复自增 term） | Day37 完整实现 Pre-Vote：先探测多数派可达性，再发起真实选举 |
| GET 是本地读（最终一致性）：Follower 的 `lastApplied` 落后于 Leader `commitIndex` 时会读到旧值，甚至读不到刚写入 Leader 尚未复制过来的 key | Day37 引入 ReadIndex 线性一致读（Follower 向 Leader 确认 readIndex，等本地 `lastApplied` 追上后再读），需要时可换取强一致性 |
