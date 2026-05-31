# Day 35 — L7 反向代理与负载均衡

## 目录

| 章节 | 内容 |
|------|------|
| [§1 引言](#1-引言) | 单点故障与容量上限问题；本日独立模块；三种策略适用场景 |
| [§2 改进 A — Backend：后端元数据](#2-改进-a--backend后端元数据) | 新建 `Backend.h`：inflight/failStreak/alive/cooldownUntil；被动健康检查；冷却恢复 |
| [§3 改进 B — LbStrategy 接口 + 三种算法](#3-改进-b--lbstrategy-接口--三种算法) | 新建 `LbStrategy.h`/`RoundRobin.h/cpp`/`LeastConnections.h/cpp`/`ConsistentHash.h/cpp` |
| [§4 改进 C — BackendPool：后端池管理](#4-改进-c--backendpool后端池管理) | 新建 `BackendPool.h/cpp`：addBackend/build/pick/statusJson |
| [§5 改进 D — ReverseProxy：HTTP 转发引擎](#5-改进-d--reverseproxy-http-转发引擎) | 新建 `ReverseProxy.h/cpp`：callUpstream（HTTP/1.0 阻塞）；被动健康检查 |
| [§6 改进 E — lb_demo：3 后端演示程序](#6-改进-e--lb_demo3-后端演示程序) | 新建 `examples/src/lb_demo.cpp`：回显后端工厂；/admin/backends 路由 |
| [§7 整体运行时理解](#7-整体运行时理解) | 架构图；场景 A（一次 RR 请求完整路径）；场景 B（后端下线与冷却恢复）；场景 C（一致性哈希 Session Affinity）|
| [§8 各模块职责速查表](#8-各模块职责速查表) | 本日所有新增类/函数一览 |
| [§9 工程化](#9-工程化) | CMakeLists.txt 变更 |
| [§10 验证](#10-验证) | 构建命令 + 三种策略完整演示 |
| [§11 局限与下一步](#11-局限与下一步) | 同步阻塞客户端；Day36 KV 集群 |

---

## 本日变更文件一览

| 文件 | 变更 | 核心改动 |
|------|------|---------|
| `src/include/lb/Backend.h` | **新建** | 后端元数据：inflight/failStreak/alive/cooldownUntil；isAlive/onSuccess/onFailure |
| `src/include/lb/LbStrategy.h` | **新建** | 策略纯虚接口：`build(backends)` + `pick(backends, req)` |
| `src/include/lb/RoundRobin.h/cpp` | **新建** | 原子计数器轮询；跳过下线后端 |
| `src/include/lb/LeastConnections.h/cpp` | **新建** | 最小 inflight 后端；跳过下线后端 |
| `src/include/lb/ConsistentHash.h/cpp` | **新建** | FNV-1a 哈希；100 虚拟节点/后端；url+X-Session-Id 路由键 |
| `src/include/lb/BackendPool.h/cpp` | **新建** | 后端池：addBackend/build/pick；statusJson 监控接口 |
| `src/include/lb/ReverseProxy.h/cpp` | **新建** | HTTP/1.0 阻塞转发；hop-by-hop 头过滤；被动健康检查 |
| `examples/src/lb_demo.cpp` | **新建** | 3 回显后端 + 前端代理；`--strategy` 参数；/admin/backends 路由 |
| `CMakeLists.txt` | **修改** | 新增 `lb_demo` 可执行目标 |

---

## 1. 引言

Day35 暂时离开 Raft 话题，在 HTTP 层之上搭建一个可配置的反向代理与负载均衡器。它是**独立模块**，不依赖 Raft，将在 Day36 的 KV 集群中被复用（HTTP 客户端可以向任意 KV 节点发请求）。

**单节点 HTTP 服务器的两个问题**：
1. **单点故障**：服务器宕机，所有请求立刻失败，没有降级能力
2. **容量上限**：一台服务器的 CPU/内存/网络带宽有限，无法水平扩展

**反向代理负载均衡器的解法**：

```
客户端  →  LB(:28900)  →  Backend-0 (:29001)
                       →  Backend-1 (:29002)
                       →  Backend-2 (:29003)
```

客户端只知道 LB 的地址，后端的扩容/缩容/故障对客户端透明。LB 还负责检测后端健康状态——连续失败 3 次的后端会被临时下线 60 秒，流量自动绕过它。

**三种调度策略的适用场景**：

| 策略 | 适用场景 | 特点 |
|------|---------|------|
| **轮询（RR）** | 后端性能均等、请求耗时相近 | 无状态，O(n) 最坏；原子计数器线程安全 |
| **最少连接（LC）** | 请求耗时差异大（如混合 API 和文件下载）| 慢后端 inflight 累积 → 自动被回避 |
| **一致性哈希（CH）** | 有缓存亲和性需求（同一个 session 总路由到同一后端）| 增减后端时只有相邻区间迁移（影响 1/n）|

---

## 2. 改进 A — Backend：后端元数据

### 2.1 为什么需要这个结构体

后端不只是一个地址，还需要跟踪运行时状态：当前有多少请求在飞行中（供 LC 策略参考）、连续失败了多少次（供被动健康检查）、是否还在线（决定是否接受新请求）。

**被动健康检查**（与主动探活对比）：

- **主动探活**：定时器每 10s 发一次 `/health` 请求，感知慢（延迟最多 10s），消耗额外资源
- **被动健康检查**：每次真实请求失败才计数，连续失败 N 次才下线——感知快（第 3 次失败即下线），零额外 overhead，代价是前几次失败请求会被转发到问题后端

本项目采用被动健康检查：连续失败 3 次 → 下线，冷却 60 秒后自动恢复。

### 2.2 编码实现步骤

**新建 `src/include/lb/Backend.h`，写入以下全部内容**

来自 [src/include/lb/Backend.h](src/include/lb/Backend.h)：

```cpp
#pragma once
#include <atomic>
#include <chrono>
#include <string>

// Backend：上游服务节点的元数据容器。
//
// 设计要点：
//   · inflight    —— 正在处理的请求数（ReverseProxy 在转发前 +1，回包/超时后 -1）
//   · failStreak  —— 连续失败次数；>=3 触发被动健康检查下线
//   · alive       —— 当前是否可接收新请求
//   · cooldownUntil —— 下线后的冷却截止时间（steady_clock 纳秒）；到期后 isAlive() 自动恢复
//
// 线程安全：所有字段均为 std::atomic，可在任意线程读写。
// 不可拷贝（std::atomic 语义），通过 unique_ptr 存储于 BackendPool。

struct Backend {
    std::string host;
    int         port{0};

    std::atomic<int>     inflight{0};
    std::atomic<int>     failStreak{0};
    std::atomic<bool>    alive{true};
    std::atomic<int64_t> cooldownUntil{0}; // nanoseconds since steady_clock epoch

    Backend() = default;
    Backend(std::string h, int p) : host(std::move(h)), port(p) {}

    Backend(const Backend &)            = delete;
    Backend &operator=(const Backend &) = delete;

    std::string address() const { return host + ":" + std::to_string(port); }

    // 检查是否存活；冷却期结束后自动将 alive 翻回 true。
    bool isAlive() {
        if (alive.load(std::memory_order_relaxed)) return true;
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (now >= cooldownUntil.load(std::memory_order_relaxed)) {
            alive.store(true, std::memory_order_relaxed);
            failStreak.store(0, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    void onSuccess() {
        failStreak.store(0, std::memory_order_relaxed);
        alive.store(true, std::memory_order_relaxed);
    }

    // 连续失败 3 次 → 下线 60 s（被动健康检查）
    void onFailure() {
        int streak = failStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (streak >= 3) {
            alive.store(false, std::memory_order_relaxed);
            auto deadline = std::chrono::steady_clock::now().time_since_epoch()
                          + std::chrono::seconds(60);
            cooldownUntil.store(deadline.count(), std::memory_order_relaxed);
        }
    }
};
```

**关键设计点**：

`std::atomic` 全字段——`inflight` 被多个 IO 线程并发修改（`++inflight` 在请求开始，`--inflight` 在请求完成），`failStreak`/`alive` 也可能被多个线程同时读写。全用 `atomic` 消除了对 mutex 的需求，减少锁竞争。

`Backend` 不可拷贝（`=delete`）——`std::atomic` 不支持拷贝。`BackendPool` 用 `vector<unique_ptr<Backend>>` 存储，通过指针传递，避免拷贝。

`isAlive()` 的自动恢复逻辑：下线时不是永久下线，而是设置一个 `cooldownUntil` 时间戳。每次调用 `isAlive()` 时检查是否过了冷却期——过了就自动把 `alive` 翻回 `true`，无需定时器或外部干预。`memory_order_relaxed` 足够：健康状态读写不需要强顺序保证，只需要原子性。

`onFailure()` 的 `fetch_add + 1`：先加后检查，确保并发的多次失败只有最后一次达到阈值时才下线，不会重复设置 `cooldownUntil`（因为 `alive=false` 时后续请求不会再路由到这个后端）。

---

## 3. 改进 B — LbStrategy 接口 + 三种算法

### 3.1 为什么需要策略接口

把"选哪个后端"抽象为接口，让 `BackendPool`/`ReverseProxy` 不依赖具体算法。换策略时只需传入不同的实现，代理代码不变——这是经典的**策略模式（Strategy Pattern）**。

### 3.2 编码实现步骤

**新建 `src/include/lb/LbStrategy.h`**

来自 [src/include/lb/LbStrategy.h](src/include/lb/LbStrategy.h)：

```cpp
#pragma once
#include "lb/Backend.h"
#include <memory>
#include <vector>

class HttpRequest;

// LbStrategy：负载均衡策略的纯虚接口。
//
// pick() 返回 backends 的下标（存活后端），或 -1（全部不可用）。
// build() 在所有 backend 加入后调用一次，供一致性哈希等需要预计算的策略使用；
// 默认空实现，RoundRobin / LeastConnections 无需 override。

class LbStrategy {
  public:
    virtual ~LbStrategy() = default;

    // 在所有 addBackend 完成后调用，允许策略预计算（如构建哈希环）。
    virtual void build(const std::vector<std::unique_ptr<Backend>> &) {}

    // 从存活后端中选一个，返回下标；全部不可用时返回 -1。
    virtual int pick(const std::vector<std::unique_ptr<Backend>> &backends,
                     const HttpRequest &req) = 0;
};
```

`build()` 默认空实现——RoundRobin 和 LeastConnections 不需要预计算，不用 override。只有 ConsistentHash 需要 override `build` 来构建哈希环。`pick()` 接收 `HttpRequest`——一致性哈希需要从请求中提取 URL 和 Session ID 作为路由键。

---

**新建 `src/include/lb/RoundRobin.h` 和 `src/common/lb/RoundRobin.cpp`**

来自 [src/include/lb/RoundRobin.h](src/include/lb/RoundRobin.h)：

```cpp
#pragma once
#include "lb/LbStrategy.h"
#include <atomic>

// RoundRobin：原子计数器轮询，跳过已下线后端。
// 无状态热路径，O(n) 最坏情况（全部后端都死时扫一遍）。

class RoundRobin : public LbStrategy {
  public:
    int pick(const std::vector<std::unique_ptr<Backend>> &backends,
             const HttpRequest &req) override;

  private:
    std::atomic<uint64_t> counter_{0};
};
```

来自 [src/common/lb/RoundRobin.cpp](src/common/lb/RoundRobin.cpp)：

```cpp
#include "lb/RoundRobin.h"
#include "http/HttpRequest.h"

int RoundRobin::pick(const std::vector<std::unique_ptr<Backend>> &backends,
                     const HttpRequest & /*req*/) {
    size_t n = backends.size();
    if (n == 0) return -1;

    // 从当前计数器位置开始，最多轮询一整圈，跳过已下线后端
    uint64_t start = counter_.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (start + i) % n;
        if (backends[idx]->isAlive()) return static_cast<int>(idx);
    }
    return -1;
}
```

`counter_.fetch_add(1, relaxed)` 原子递增：`relaxed` 足够，因为这里只需要原子性（防止两个线程拿到相同计数），不需要内存屏障。多线程并发 `fetch_add` 时，每个线程拿到唯一的 `start` 值，然后各自取模决定从哪个后端开始轮询。

`for (i = 0; i < n; ++i)` 最多走一整圈：如果 `start % n` 对应的后端已下线，就尝试 `(start+1) % n`，直到找到存活的或全部扫完（返回 -1）。

---

**新建 `src/include/lb/LeastConnections.h` 和 `src/common/lb/LeastConnections.cpp`**

来自 [src/include/lb/LeastConnections.h](src/include/lb/LeastConnections.h)：

```cpp
#pragma once
#include "lb/LbStrategy.h"

// LeastConnections：选 inflight 最小的存活后端。
// 平衡处理时间差异大的后端（慢后端 inflight 累积 → 被自动回避）。

class LeastConnections : public LbStrategy {
  public:
    int pick(const std::vector<std::unique_ptr<Backend>> &backends,
             const HttpRequest &req) override;
};
```

来自 [src/common/lb/LeastConnections.cpp](src/common/lb/LeastConnections.cpp)：

```cpp
#include "lb/LeastConnections.h"
#include "http/HttpRequest.h"
#include <climits>

int LeastConnections::pick(const std::vector<std::unique_ptr<Backend>> &backends,
                            const HttpRequest & /*req*/) {
    int best      = -1;
    int bestValue = INT_MAX;

    for (size_t i = 0; i < backends.size(); ++i) {
        if (!backends[i]->isAlive()) continue;
        int f = backends[i]->inflight.load(std::memory_order_relaxed);
        if (f < bestValue) {
            bestValue = f;
            best      = static_cast<int>(i);
        }
    }
    return best;
}
```

LeastConnections 没有内部状态，无需锁。`inflight` 是 `atomic<int>`，各线程读取快照值，选最小的——这个"最小"是近似值（另一个线程可能同时在修改），但对负载均衡来说近似已经足够好，不需要加锁保证精确最小。

---

**新建 `src/include/lb/ConsistentHash.h` 和 `src/common/lb/ConsistentHash.cpp`**

来自 [src/include/lb/ConsistentHash.h](src/include/lb/ConsistentHash.h)：

```cpp
#pragma once
#include "lb/LbStrategy.h"
#include <cstdint>
#include <map>

// ConsistentHash：虚拟节点一致性哈希。
//
// 原理：
//   · 每个后端在哈希环上放 kVnodesPerBackend 个虚拟节点（键 = "addr#N" 的 FNV-1a 哈希）
//   · 请求 key = url + X-Session-Id，映射到环上顺时针第一个存活虚拟节点所属后端
//   · 增减后端时，只有相邻虚拟节点区间的请求发生迁移（影响比例 ≈ 1/n）
//
// 使用前须调用 build()；pick() 中不再重建环（多线程安全）。

class ConsistentHash : public LbStrategy {
  public:
    static constexpr int kVnodesPerBackend = 100;

    void build(const std::vector<std::unique_ptr<Backend>> &backends) override;

    int pick(const std::vector<std::unique_ptr<Backend>> &backends,
             const HttpRequest &req) override;

  private:
    static uint32_t fnv1a(const std::string &s);

    std::map<uint32_t, int> ring_; // hash → backend index
};
```

来自 [src/common/lb/ConsistentHash.cpp](src/common/lb/ConsistentHash.cpp)：

```cpp
#include "lb/ConsistentHash.h"
#include "http/HttpRequest.h"

// FNV-1a 32-bit（速度快、分布均匀，非密码学用途）
uint32_t ConsistentHash::fnv1a(const std::string &s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s)
        h = (h ^ c) * 16777619u;
    return h;
}

void ConsistentHash::build(const std::vector<std::unique_ptr<Backend>> &backends) {
    ring_.clear();
    for (int i = 0; i < static_cast<int>(backends.size()); ++i) {
        for (int v = 0; v < kVnodesPerBackend; ++v) {
            // 虚拟节点键 = "host:port#vnode_id"
            std::string vkey = backends[i]->address() + "#" + std::to_string(v);
            ring_[fnv1a(vkey)] = i;
        }
    }
}

int ConsistentHash::pick(const std::vector<std::unique_ptr<Backend>> &backends,
                          const HttpRequest &req) {
    if (ring_.empty() || backends.empty()) return -1;

    // 请求 key：path + 会话 ID（同 key 多次请求 → 同后端，Session Affinity）
    std::string key = req.url() + req.header("x-session-id");
    uint32_t    h   = fnv1a(key);

    // 顺时针找第一个 >= h 的虚拟节点；绕环一圈跳过死亡后端
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) it = ring_.begin();

    auto start = it;
    do {
        int idx = it->second;
        if (backends[idx]->isAlive()) return idx;
        ++it;
        if (it == ring_.end()) it = ring_.begin();
    } while (it != start);

    return -1;
}
```

**一致性哈希的核心思想**：

把 `[0, 2^32)` 的整数空间想象成一个环。每个后端在环上放 100 个虚拟节点（均匀分布），请求的 hash 值落在环的某个位置，顺时针找到的第一个虚拟节点决定路由到哪个后端。

为什么需要虚拟节点？如果每个后端只有 1 个节点，3 个后端把环分成 3 段，负载分布不均（每段长度依赖 hash 值的偶然性）。100 个虚拟节点让每个后端均匀覆盖整个环，负载分布更均匀。

增减后端时，只有新/删节点在环上"相邻区间"的请求受影响（迁移比例约 1/n），其余请求的路由不变——这就是"一致性"的含义。

`do {...} while (it != start)` 是绕环一圈跳过死亡后端的完整循环，如果全部死了返回 -1。

---

## 4. 改进 C — BackendPool：后端池管理

### 4.1 为什么需要 BackendPool

`BackendPool` 是策略和后端数据的聚合点：它持有 `vector<unique_ptr<Backend>>` 和 `unique_ptr<LbStrategy>`，对外暴露简洁的 `pick(req)` 接口。调用方（`ReverseProxy`）不需要知道有多少后端，也不需要知道用的是什么策略。

### 4.2 编码实现步骤

**新建 `src/include/lb/BackendPool.h`**

来自 [src/include/lb/BackendPool.h](src/include/lb/BackendPool.h)：

```cpp
#pragma once
#include "lb/Backend.h"
#include "lb/LbStrategy.h"
#include <memory>
#include <string>
#include <vector>

class HttpRequest;

// BackendPool：后端节点池，持有 LbStrategy 并暴露 pick() 接口。
//
// 生命周期：
//   1. addBackend() 若干次（单线程，服务器启动前）
//   2. build()（触发一致性哈希等策略的预计算）
//   3. pick() 并发调用（多 I/O 线程）

class BackendPool {
  public:
    explicit BackendPool(std::unique_ptr<LbStrategy> strategy);

    void addBackend(const std::string &host, int port);

    // 所有 addBackend 完成后调用一次
    void build();

    // 选出一个存活后端；全部不可用时返回 nullptr
    Backend *pick(const HttpRequest &req);

    // JSON 格式的后端状态快照（供 /admin/backends 端点）
    std::string statusJson() const;

    const std::vector<std::unique_ptr<Backend>> &backends() const { return backends_; }

  private:
    std::vector<std::unique_ptr<Backend>> backends_;
    std::unique_ptr<LbStrategy>           strategy_;
};
```

---

**新建 `src/common/lb/BackendPool.cpp`**

来自 [src/common/lb/BackendPool.cpp](src/common/lb/BackendPool.cpp)：

```cpp
#include "lb/BackendPool.h"
#include "http/HttpRequest.h"
#include <nlohmann/json.hpp>

BackendPool::BackendPool(std::unique_ptr<LbStrategy> strategy)
    : strategy_(std::move(strategy)) {}

void BackendPool::addBackend(const std::string &host, int port) {
    backends_.push_back(std::make_unique<Backend>(host, port));
}

void BackendPool::build() {
    strategy_->build(backends_);
}

Backend *BackendPool::pick(const HttpRequest &req) {
    int idx = strategy_->pick(backends_, req);
    if (idx < 0 || idx >= static_cast<int>(backends_.size())) return nullptr;
    return backends_[idx].get();
}

std::string BackendPool::statusJson() const {
    using json = nlohmann::json;
    json arr   = json::array();
    for (const auto &b : backends_) {
        arr.push_back({
            {"address",   b->address()},
            {"alive",     b->alive.load()},
            {"inflight",  b->inflight.load()},
            {"failStreak", b->failStreak.load()},
        });
    }
    return arr.dump(2);
}
```

`addBackend` 只在启动前的单线程阶段调用，之后 `backends_` 不再增删，所以 `pick` 不需要加锁——`backends_` 本身不变，只有元素内部的 `atomic` 字段在变。

`build()` 必须在 `addBackend` 全部完成后调用一次：ConsistentHash 的 `ring_` 在这里构建，之后 `ring_` 只读，多线程 `pick()` 安全。如果调用顺序错误（先 build 再 addBackend），ConsistentHash 就会漏掉后加的后端。

`statusJson()` 遍历所有后端，生成 JSON 数组——供 `/admin/backends` 监控接口使用。注意 `const` 限定但内部读取 `atomic` 字段，是安全的。

---

## 5. 改进 D — ReverseProxy：HTTP 转发引擎

### 5.1 为什么用 HTTP/1.0 而不是 HTTP/1.1

HTTP/1.0 的一个特性：服务端发完响应后会主动关闭连接，客户端收到 EOF 就知道响应完整接收了——不需要解析 `Content-Length` 或处理 chunked 编码。

HTTP/1.1 的长连接（keep-alive）更高效，但需要解析 `Content-Length`/`Transfer-Encoding: chunked` 才能知道一次响应的边界，实现更复杂。本项目是演示 Demo，选 HTTP/1.0 大幅简化了接收逻辑。

**Hop-by-hop 头**是只对单段连接有意义的头（如 `Connection: keep-alive`），不应该转发给后端——转发这些头会让后端产生混乱（比如后端以为客户端要保持长连接，但实际上是 LB 和客户端之间的连接，不是 LB 和后端之间的）。

### 5.2 编码实现步骤

**新建 `src/include/lb/ReverseProxy.h`**

来自 [src/include/lb/ReverseProxy.h](src/include/lb/ReverseProxy.h)：

```cpp
#pragma once
#include "lb/BackendPool.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

// ReverseProxy：L7 HTTP 反向代理处理器。
//
// 核心流程：
//   pick backend → ++inflight → 阻塞转发（blocking POSIX socket） → --inflight
//   → onSuccess / onFailure（被动健康检查）→ 回写响应
//
// 注意：handle() 在 HttpServer 的 I/O 线程中同步调用。
// 上游连接使用阻塞 socket + SO_RCVTIMEO=500ms，Demo 场景下可接受；
// 生产环境应替换为 EventLoop 异步客户端。

class ReverseProxy {
  public:
    explicit ReverseProxy(BackendPool &pool) : pool_(pool) {}

    // HttpServer::setHttpCallback 的实现目标
    void handle(const HttpRequest &req, HttpResponse *resp);

  private:
    // 向上游发送 HTTP/1.0 请求并读取完整响应（阻塞）
    struct UpstreamResult {
        bool        ok{false};
        int         statusCode{502};
        std::string statusMsg{"Bad Gateway"};
        std::string contentType{"text/plain"};
        std::string body;
    };

    static UpstreamResult callUpstream(const std::string &host, int port,
                                       const HttpRequest &req);

    BackendPool &pool_;
};
```

`callUpstream` 是 `static`——它不访问任何成员状态，接受 host/port/req 作为参数，输出 `UpstreamResult`，是纯函数（除了 POSIX socket 调用）。

---

**新建 `src/common/lb/ReverseProxy.cpp`**

来自 [src/common/lb/ReverseProxy.cpp](src/common/lb/ReverseProxy.cpp)：

```cpp
#include "lb/ReverseProxy.h"
#include "lb/Backend.h"
#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>
#include <string>

// ── 上游 HTTP/1.0 阻塞客户端 ──────────────────────────────────────────────────
// 使用 HTTP/1.0：服务端发完响应即关闭连接，读取到 EOF = 读完整响应，无需解析 chunk。
// SO_RCVTIMEO / SO_SNDTIMEO = 500 ms：避免慢后端无限阻塞 I/O 线程。

ReverseProxy::UpstreamResult ReverseProxy::callUpstream(const std::string &host, int port,
                                                         const HttpRequest &req) {
    UpstreamResult result;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return result;

    // 500 ms 读写超时
    struct timeval tv{0, 500000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

#ifdef SO_NOSIGPIPE
    // macOS：避免往关闭的连接写时产生 SIGPIPE，用 socket 选项代替 MSG_NOSIGNAL
    int nosigpipe = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

    // Connect
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return result;
    }
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return result;
    }

    // ── 构造 HTTP/1.0 请求 ──────────────────────────────────────────────────
    // 跳过 hop-by-hop 头（这些头只对单段连接有意义，不应转发给上游）
    static const std::vector<std::string> kHopByHop = {
        "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
        "te", "trailers", "transfer-encoding", "upgrade", "host",
    };
    auto isHopByHop = [&](const std::string &k) {
        return std::find(kHopByHop.begin(), kHopByHop.end(), k) != kHopByHop.end();
    };

    std::string raw;
    raw.reserve(512 + req.body().size());
    raw += req.methodString() + " " + req.url() + " HTTP/1.0\r\n";
    raw += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    raw += "X-Forwarded-For: proxy\r\n";

    for (const auto &[k, v] : req.headers()) {
        if (!isHopByHop(k)) raw += k + ": " + v + "\r\n";
    }

    if (!req.body().empty())
        raw += "Content-Length: " + std::to_string(req.body().size()) + "\r\n";

    raw += "\r\n";
    raw += req.body();

    // ── 发送 ────────────────────────────────────────────────────────────────
    const char *ptr = raw.data();
    size_t      rem = raw.size();
    while (rem > 0) {
        ssize_t n = ::send(fd, ptr, rem, 0);
        if (n <= 0) { ::close(fd); return result; }
        ptr += n;
        rem -= static_cast<size_t>(n);
    }

    // ── 接收直到 EOF（HTTP/1.0 无 Content-Length 也能正确终止）────────────
    std::string response;
    response.reserve(4096);
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        response.append(buf, static_cast<size_t>(n));
    ::close(fd);

    if (response.empty()) return result;

    // ── 解析响应头 ──────────────────────────────────────────────────────────
    size_t sep = response.find("\r\n\r\n");
    if (sep == std::string::npos) return result;

    std::string header_section = response.substr(0, sep);
    result.body                = response.substr(sep + 4);

    // 状态行：HTTP/1.x NNN Message
    size_t eol  = header_section.find("\r\n");
    std::string sl = (eol != std::string::npos) ? header_section.substr(0, eol) : header_section;
    if (sl.size() >= 12) {
        try {
            result.statusCode = std::stoi(sl.substr(9, 3));
            result.statusMsg  = sl.size() > 13 ? sl.substr(13) : "OK";
        } catch (...) {
            result.statusCode = 200;
            result.statusMsg  = "OK";
        }
    } else {
        result.statusCode = 200;
        result.statusMsg  = "OK";
    }

    // 扫描响应头，提取 Content-Type（覆盖默认值）
    std::istringstream iss(header_section);
    std::string        line;
    std::getline(iss, line); // skip status line
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(": ");
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "content-type")
            result.contentType = line.substr(colon + 2);
    }

    result.ok = true;
    return result;
}

// ── 主处理逻辑 ────────────────────────────────────────────────────────────────
void ReverseProxy::handle(const HttpRequest &req, HttpResponse *resp) {
    Backend *b = pool_.pick(req);
    if (!b) {
        resp->setStatus(HttpResponse::StatusCode::k503ServiceUnavailable,
                        "Service Unavailable");
        resp->setContentType("text/plain");
        resp->setBody("No backends available\n");
        return;
    }

    b->inflight.fetch_add(1, std::memory_order_relaxed);
    UpstreamResult res = callUpstream(b->host, b->port, req);
    b->inflight.fetch_sub(1, std::memory_order_relaxed);

    if (!res.ok) {
        b->onFailure();
        resp->setStatus(HttpResponse::StatusCode::k502BadGateway, "Bad Gateway");
        resp->setContentType("text/plain");
        resp->setBody("Upstream connection failed\n");
        return;
    }

    b->onSuccess();
    resp->setStatus(static_cast<HttpResponse::StatusCode>(res.statusCode), res.statusMsg);
    resp->setContentType(res.contentType);
    resp->setBody(res.body);
}
```

**`handle` 的完整流程**：

1. `pool_.pick(req)` → 选一个存活后端（返回 nullptr → 503）
2. `++inflight` → 告诉 LC 策略"这个后端又多了一个请求"
3. `callUpstream(b->host, b->port, req)` → 阻塞转发（新建 TCP 连接 → 发 HTTP/1.0 请求 → 读响应 → 关闭连接）
4. `--inflight` → 无论成功失败，请求结束了
5. `onFailure()`/`onSuccess()` → 更新被动健康检查计数

注意 `--inflight` 在 `onFailure`/`onSuccess` 之前：确保 LC 策略在下次 pick 时能看到最新的 inflight 值，不会因为健康检查逻辑延误而选出不准确的后端。

---

## 6. 改进 E — lb_demo：3 后端演示程序

### 6.1 为什么需要这个文件

端到端验证所有 LB 策略，同时展示：①回显后端的简单构建方式；②`--strategy` 参数在运行时切换策略；③`/admin/backends` 监控端点。

### 6.2 编码实现步骤

**新建 `examples/src/lb_demo.cpp`，写入以下全部内容**

来自 [examples/src/lb_demo.cpp](examples/src/lb_demo.cpp)：

```cpp
// lb_demo —— L7 反向代理 + 多策略负载均衡演示
//
//  架构：
//    3 个回显后端（:29001 / :29002 / :29003）各跑在独立线程
//    1 个反向代理前端（:28900）接受外部请求，按策略转发
//
//  启动：
//    ./build/examples/lb_demo                     # 默认 RoundRobin
//    ./build/examples/lb_demo --strategy lc       # LeastConnections
//    ./build/examples/lb_demo --strategy ch       # ConsistentHash

#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/HttpServer.h"
#include "lb/BackendPool.h"
#include "lb/ConsistentHash.h"
#include "lb/LeastConnections.h"
#include "lb/ReverseProxy.h"
#include "lb/RoundRobin.h"
#include "log/Logger.h"
#include "net/SignalHandler.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ── 回显后端工厂 ──────────────────────────────────────────────────────────────
// 每个后端是一个普通 HttpServer，返回自己的 ID + 请求路径
static std::thread startBackend(int id, int port) {
    return std::thread([id, port] {
        HttpServer::Options opts;
        opts.tcp.listenPort = static_cast<uint16_t>(port);
        opts.tcp.ioThreads  = 1;

        HttpServer srv(opts);
        srv.setHttpCallback([id](const HttpRequest &req, HttpResponse *resp) {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("text/plain");
            resp->setBody("backend-" + std::to_string(id) + " " + req.url() + "\n");
        });
        srv.start();
    });
}

int main(int argc, char *argv[]) {
    // ── 解析 --strategy 参数 ─────────────────────────────────────────────────
    std::string strategyName = "rr";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--strategy") == 0)
            strategyName = argv[i + 1];
    }

    // ── 选择策略 ─────────────────────────────────────────────────────────────
    std::unique_ptr<LbStrategy> strategy;
    if (strategyName == "lc") {
        strategy = std::make_unique<LeastConnections>();
        std::cout << "[LB] 策略：LeastConnections（最少连接）\n";
    } else if (strategyName == "ch") {
        strategy = std::make_unique<ConsistentHash>();
        std::cout << "[LB] 策略：ConsistentHash（一致性哈希，key = url + X-Session-Id）\n";
    } else {
        strategy = std::make_unique<RoundRobin>();
        std::cout << "[LB] 策略：RoundRobin（轮询）\n";
    }

    // ── 建立后端池 ───────────────────────────────────────────────────────────
    BackendPool pool(std::move(strategy));
    pool.addBackend("127.0.0.1", 29001);
    pool.addBackend("127.0.0.1", 29002);
    pool.addBackend("127.0.0.1", 29003);
    pool.build(); // 触发一致性哈希预计算（其他策略此调用为空操作）

    // ── 启动 3 个后端服务（后台线程） ─────────────────────────────────────────
    std::vector<std::thread> backendThreads;
    backendThreads.push_back(startBackend(0, 29001));
    backendThreads.push_back(startBackend(1, 29002));
    backendThreads.push_back(startBackend(2, 29003));

    // 给后端一点时间完成监听
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── 反向代理前端 ─────────────────────────────────────────────────────────
    ReverseProxy proxy(pool);

    HttpServer::Options proxyOpts;
    proxyOpts.tcp.listenPort = 28900;
    proxyOpts.tcp.ioThreads  = 2;

    HttpServer proxySrv(proxyOpts);

    // /admin/backends：返回后端池 JSON 状态（不经过代理）
    proxySrv.addRoute(
        HttpRequest::Method::kGet, "/admin/backends",
        [&pool](const HttpRequest & /*req*/, HttpResponse *resp) {
            resp->setStatus(HttpResponse::StatusCode::k200OK, "OK");
            resp->setContentType("application/json");
            resp->setBody(pool.statusJson());
        });

    // 其余所有请求 → 反向代理
    proxySrv.setHttpCallback(
        [&proxy](const HttpRequest &req, HttpResponse *resp) { proxy.handle(req, resp); });

    // ── 优雅关闭 ─────────────────────────────────────────────────────────────
    Signal::signal(SIGINT,  [&proxySrv] { proxySrv.stop(); });
    Signal::signal(SIGTERM, [&proxySrv] { proxySrv.stop(); });

    std::cout << "[LB] 反向代理监听 :28900，后端 :29001/:29002/:29003\n"
              << "[LB] Ctrl+C 退出\n";

    proxySrv.start(); // 阻塞直到 stop()

    for (auto &t : backendThreads)
        t.detach(); // 进程退出时由 OS 回收

    return 0;
}
```

`pool.build()` 必须在 `addBackend` 之后、`proxySrv.start()` 之前调用——这是 `BackendPool` 的生命周期约定。对 RoundRobin/LeastConnections 是空操作，对 ConsistentHash 是构建哈希环。

`/admin/backends` 路由优先于全局 `setHttpCallback`——因为 `addRoute` 注册的精确路由优先匹配，未命中精确路由才走 `setHttpCallback`。

---

## 7. 整体运行时理解

### 7.1 架构图

```
外部客户端 → 主线程(proxySrv.start)
              └── HttpServer(T_proxy_main + T_proxy_sub × 2)
                   └── ReverseProxy::handle(req, resp)  [T_proxy_sub]
                        │
                        │ pool_.pick(req)  [无锁，atomic 读]
                        │   ├── RoundRobin: counter_.fetch_add
                        │   ├── LeastConnections: 遍历 inflight 最小
                        │   └── ConsistentHash: fnv1a(url+session) → ring_.lower_bound
                        │
                        │ ++inflight
                        │ callUpstream(host, port, req)  [阻塞 POSIX socket]
                        │ --inflight
                        │ onSuccess() 或 onFailure()
                        │
                        └── resp 填充 → HttpServer 发送给客户端

后端线程（各独立）：
  T_backend_0 → HttpServer(:29001) → "backend-0 /path\n"
  T_backend_1 → HttpServer(:29002) → "backend-1 /path\n"
  T_backend_2 → HttpServer(:29003) → "backend-2 /path\n"
```

### 7.2 场景 A — 一次 RoundRobin 请求的完整路径

**场景设定**：3 后端均存活（alive=true），`counter_=0`，客户端发 `GET /hello`。

---

#### 第 1 步：proxySrv 解析 HTTP 请求，调 ReverseProxy::handle

打开 [src/common/lb/ReverseProxy.cpp](src/common/lb/ReverseProxy.cpp)，`handle`：

```cpp
Backend *b = pool_.pick(req);
// → RoundRobin::pick(backends, req)
//   start = counter_.fetch_add(1) = 0  (counter_ 变成 1)
//   idx = (0 + 0) % 3 = 0
//   backends[0]->isAlive() = true → return 0
//   → b = backends_[0].get() = Backend{host="127.0.0.1", port=29001}
```

**此刻状态快照**：
```
counter_     = 1（已递增）
b            = Backend{29001, inflight=0, failStreak=0, alive=true}
```

---

#### 第 2 步：++inflight，callUpstream，--inflight

```cpp
b->inflight.fetch_add(1);  // inflight = 1

UpstreamResult res = callUpstream("127.0.0.1", 29001, req);
// → socket() + connect(29001) + send("GET /hello HTTP/1.0\r\nHost: 127.0.0.1:29001\r\nX-Forwarded-For: proxy\r\n\r\n")
// → recv 直到 EOF，得到 "HTTP/1.0 200 OK\r\n...\r\n\r\nbackend-0 /hello\n"
// res = {ok=true, statusCode=200, body="backend-0 /hello\n"}

b->inflight.fetch_sub(1);  // inflight = 0
```

**此刻状态快照**：
```
b.inflight   = 0（转发完成）
res.ok       = true
res.body     = "backend-0 /hello\n"
```

---

#### 第 3 步：onSuccess，填充 resp

```cpp
b->onSuccess();
// failStreak = 0, alive = true（无变化，只是重置）

resp->setStatus(200, "OK");
resp->setContentType("text/plain");
resp->setBody("backend-0 /hello\n");
```

客户端收到：`HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nbackend-0 /hello\n`

---

#### 第 4 步：下一次请求（counter_=1）

```
start = 1，idx = 1 % 3 = 1 → Backend{29002}
再下一次：counter_=2，idx = 2 → Backend{29003}
再下一次：counter_=3，idx = 0 → Backend{29001}（循环）
```

---

### 7.3 场景 B — 后端下线与冷却恢复（被动健康检查）

**场景设定**：Backend{29002} 宕机（进程 kill），第一次请求正好路由到它。

---

#### 第 1 步：callUpstream 失败（connect 被拒绝）

```cpp
// callUpstream("127.0.0.1", 29002, req)
// connect(29002) → ECONNREFUSED → ::close(fd) → return {ok=false}

b->inflight.fetch_sub(1);  // inflight = 0
// res.ok = false

b->onFailure();
// failStreak: 0 → fetch_add(1) + 1 = 1
// streak=1 < 3 → 不下线（还有机会）

// 客户端收到 502 Bad Gateway
```

---

#### 第 2 步：连续再失败 2 次 → 下线

第 2 次失败：`failStreak=2`，仍存活。
第 3 次失败（`failStreak.fetch_add(1)+1 = 3 >= 3`）：

```cpp
alive.store(false);
cooldownUntil = now + 60s;
// 此后 60 秒内 isAlive() 返回 false，RoundRobin 跳过它
```

**此刻状态快照（Backend{29002}）**：
```
alive         = false
cooldownUntil = T + 60s（纳秒时间戳）
failStreak    = 3
```

---

#### 第 3 步：60 秒后，isAlive() 自动恢复

```cpp
// 某次 pick 调用 backends[1]->isAlive()
// alive = false → 检查 cooldownUntil
// now >= cooldownUntil → true
alive.store(true);
failStreak.store(0);
// 后端重新加入轮询
```

整个恢复过程无需定时器，无需外部干预——全靠 `isAlive()` 里的懒检查逻辑。

---

### 7.4 场景 C — 一致性哈希 Session Affinity

**场景设定**：3 后端，ConsistentHash，同一用户（session="alice"）连续发 4 次请求。

```
请求 1: url="/order/1", X-Session-Id: alice
  key = "/order/1alice"
  h = fnv1a("/order/1alice") = 0x3F2A...
  ring_.lower_bound(0x3F2A...) → 指向某虚拟节点 → index=1 (Backend{29002})
  → "backend-1 /order/1\n"

请求 2: url="/order/2", X-Session-Id: alice
  key = "/order/2alice"
  h = fnv1a("/order/2alice") = 0x3F31...（与 /order/1alice 的 hash 相近）
  ring_.lower_bound(0x3F31...) → 顺时针下一个虚拟节点 → 可能仍是 index=1
  → "backend-1 /order/2\n"（同一后端！）
```

只要 URL 路径差异不大（同一 session 通常访问相邻 URL），且哈希环上该区域被同一后端的虚拟节点占据，多次请求就会路由到同一后端——这就是 Session Affinity（会话亲和性）。

---

## 8. 各模块职责速查表

| 模块/函数 | 所在线程 | 调用时机 | 职责一句话 |
|-----------|---------|---------|-----------|
| `Backend::isAlive()` | 任意（T_proxy_sub）| pick 时 | 检查存活；冷却期满自动翻回 true |
| `Backend::onSuccess()` | T_proxy_sub | callUpstream 成功后 | 清零 failStreak，标记 alive |
| `Backend::onFailure()` | T_proxy_sub | callUpstream 失败后 | `++failStreak`；≥3 则下线 60s |
| `RoundRobin::pick` | T_proxy_sub | 每次请求 | `fetch_add` 原子轮询；跳过下线后端 |
| `LeastConnections::pick` | T_proxy_sub | 每次请求 | 遍历所有存活后端，返回 inflight 最小的 |
| `ConsistentHash::build` | 单线程（启动时）| `pool.build()` 时 | 构建哈希环（100 虚拟节点/后端）|
| `ConsistentHash::pick` | T_proxy_sub | 每次请求 | `fnv1a(url+session)` → 顺时针找存活虚拟节点 |
| `BackendPool::pick` | T_proxy_sub | `ReverseProxy::handle` 内 | 调 `strategy_->pick` 并返回裸指针 |
| `ReverseProxy::callUpstream` | T_proxy_sub（阻塞）| handle 内 | 新建 TCP 连接，发 HTTP/1.0，读 EOF，关闭 |
| `ReverseProxy::handle` | T_proxy_sub | HttpServer 路由 | pick → `++inflight` → callUpstream → `--inflight` → 更新健康状态 |
| `BackendPool::statusJson` | T_proxy_sub（或 T_proxy_main）| GET /admin/backends | JSON 序列化所有后端的实时状态 |

---

## 9. 工程化

`CMakeLists.txt` 新增 `lb_demo` 可执行目标：

```cmake
add_executable(lb_demo examples/src/lb_demo.cpp)
target_link_libraries(lb_demo NetLib pthread)
set_target_properties(lb_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${MCPP_EXAMPLE_DIR})
```

`file(GLOB_RECURSE COMMON_SOURCES "src/common/*.cpp")` 已自动扫描 `src/common/lb/*.cpp`，无需手动列举。

---

## 10. 验证

```bash
cmake --build build --target lb_demo -j

# ── 测试 RoundRobin（默认）──────────────────────────────────────────────────
./build/examples/lb_demo &
sleep 0.3

# 6 次请求应均匀落在 3 个后端（backend-0/1/2 各 2 次）
for i in $(seq 1 6); do curl -s http://localhost:28900/hello; done
# 预期输出：
# backend-0 /hello
# backend-1 /hello
# backend-2 /hello
# backend-0 /hello
# backend-1 /hello
# backend-2 /hello

# 后端状态
curl -s http://localhost:28900/admin/backends | python3 -m json.tool
# [{"address":"127.0.0.1:29001","alive":true,"inflight":0,"failStreak":0}, ...]

kill %1

# ── 测试 LeastConnections ────────────────────────────────────────────────────
./build/examples/lb_demo --strategy lc &
sleep 0.3
# 并发 10 个请求：inflight 最小的后端优先接受
for i in $(seq 1 10); do curl -s http://localhost:28900/test & done; wait
kill %1

# ── 测试 ConsistentHash（Session Affinity）──────────────────────────────────
./build/examples/lb_demo --strategy ch &
sleep 0.3
# 同 session "alice" 的 4 次请求应路由到同一后端
for i in $(seq 1 4); do
    curl -s -H "X-Session-Id: alice" http://localhost:28900/order
done
# 预期：4 行输出都是 "backend-N /order"（N 相同）

kill %1
```

---

## 11. 局限与下一步

| 局限 | 描述 |
|------|------|
| **同步阻塞转发** | `callUpstream` 在 T_proxy_sub 阻塞等待后端响应（最多 500ms）。并发请求多时，ioThreads 个 IO 线程可能全部被慢后端占用，新请求排队。生产环境应用 EventLoop 异步客户端（本项目已有 `AsyncRpcClient` 的 pattern，可参照） |
| **主动健康检查缺失** | 只有被动检查（失败 3 次下线）。后端偶发超时但未超过 3 次不会被下线，客户端会感知偶发延迟 |
| **LB 本身单节点** | LB 是单点，自己宕机就全挂。生产环境用 keepalived/VRRP 做 LB HA |
| **HTTP/1.0 连接不复用** | 每次请求新建 TCP 连接，有额外的三次握手开销。HTTP/1.1 keep-alive 或连接池可显著降低延迟 |

接下来 **Day36** 将把 Raft 引擎接上 HTTP 层，搭建完整的分布式 KV 集群——每个 KV 节点自带 HTTP 接口，客户端可以向任意节点发读写请求，写请求通过 307 重定向到 Leader，读请求利用 ReadIndex 协议在任意节点提供线性一致性保证。
