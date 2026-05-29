# Day 32.5 — C++20 协程重构 / setOnMessageCallback 副作用消除 / std::bind 替换

## 目录

| 章节 | 内容 |
|------|------|
| [§1 引言](#1-引言) | 两段式跳板的三个问题；协程版效果对比 |
| [§2 C++20 协程基础知识](#2-c20-协程基础知识) | promise_type 协议；Awaiter 三方法；与 TypeScript async/await 对比 |
| [§3 改进 A — std::bind → lambda](#3-改进-a--stdbind--lambda) | 修改 `Connection.cpp` / `Acceptor.cpp` / `Eventloop.cpp` |
| [§4 改进 B — setOnMessageCallback 副作用消除](#4-改进-b--setonmessagecallback-副作用消除) | 修改 `Connection.h/cpp`；新增 `enableMessageMode()` |
| [§5 改进 C — 协程基础设施：Task.h](#5-改进-c--协程基础设施taskh) | 新建 `src/include/coro/Task.h`：FireAndForget + ResumeOnLoop |
| [§6 改进 D — RpcCallAwaiter：回调桥接为 co_await](#6-改进-d--rpccallawaiter回调桥接为-co_await) | 修改 `AsyncRpcClient.h/cpp`：新增 `callAsyncCo` + `RpcCallAwaiter` |
| [§7 改进 E — RaftNode 协程化](#7-改进-e--raftnode-协程化) | 修改 `RaftNode.h/cpp`：`startElection+onVoteReply` → `runElection+collectVote`；`heartbeatTick+onHeartbeatReply` → `heartbeatTick+sendHeartbeat` |
| [§8 整体运行时理解](#8-整体运行时理解) | 协程帧内存模型；完整 collectVote 执行追踪；stop() 清理路径 |
| [§9 各模块职责速查表](#9-各模块职责速查表) | 本日所有新增/修改函数一览 |
| [§10 工程化](#10-工程化) | CMakeLists.txt：C++17→C++20；coro include 路径 |
| [§11 验证](#11-验证) | 构建命令 + 功能验证 |
| [§12 局限与下一步](#12-局限与下一步) | 协程异常策略；Day33 计划 |

---

## 本日变更文件一览

| 文件 | 变更 | 核心改动 |
|------|------|---------|
| `src/common/net/Connection.cpp` | **修改** | 3 处 `std::bind` → lambda；`setOnMessageCallback` 移除副作用；新增 `enableMessageMode()` |
| `src/include/net/Connection.h` | **修改** | 新增 `enableMessageMode()` 声明 |
| `src/common/net/Acceptor.cpp` | **修改** | `std::bind` → lambda |
| `src/common/net/Eventloop.cpp` | **修改** | `std::bind` → lambda |
| `src/include/coro/Task.h` | **新建** | `FireAndForget` + `ResumeOnLoop<Loop>` 协程基础设施 |
| `src/include/rpc/AsyncRpcClient.h` | **修改** | 新增 `callAsyncCo()` 声明；末尾新增 `RpcCallAwaiter` 类；`#include <coroutine>` |
| `src/common/rpc/AsyncRpcClient.cpp` | **修改** | 新增 `callAsyncCo()` 和 `RpcCallAwaiter::await_suspend()` 实现 |
| `src/include/raft/RaftNode.h` | **修改** | `#include "coro/Task.h"`；`startElection/onVoteReply/onHeartbeatReply` → `runElection/collectVote/sendHeartbeat` |
| `src/common/raft/RaftNode.cpp` | **修改** | 两段式跳板重构为协程：`runElection+collectVote`；`heartbeatTick+sendHeartbeat` |
| `CMakeLists.txt` | **修改** | C++17→C++20；新增 coro include 路径 |

---

## 1. 引言

day32 完成后，RaftNode 的选举和心跳流程都是"两段式跳板"：第一段发射 `callAsync`，第二段是回调函数，两段之间通过多个参数传递上下文。

**两段式的三个问题**：

```
becomeCandidate()
  └─ startElection()              ← 第一段：构建请求、调用 callAsync
       └─ callAsync("RequestVote", ..., callback)
            └─ [网络传输，异步等待]
                 └─ callback(ok, resp)       ← 第二段：调用 onVoteReply
                      └─ onVoteReply(term, peerId, ok, reply)
```

1. **逻辑分散**：同一选举轮次的"发请求"和"处理响应"在两个函数里，中间通过 `electionTerm`/`peerId` 参数传递上下文，读代码需要在两个函数之间来回跳跃
2. **守卫重复**：`onVoteReply` 开头的两个守卫（检查 state_/currentTerm_）在 `startElection` 里已隐含了这层逻辑
3. **可扩展性差**：将来在发票和收票中间插入日志重放、prevLogIndex 校验等逻辑，两段式会快速膨胀为三段、四段

**协程版效果**——把两段合并为一段连续线性代码：

```cpp
FireAndForget RaftNode::collectVote(uint64_t electionTerm, Peer peer) {
    RequestVoteArgs args{electionTerm, id_, lastLogIndex(), lastLogTerm()};

    // ── 挂起点 ── callAsync 被派发，协程挂起，loop_ 继续处理其他事件
    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo(
        "RequestVote", json(args).dump(), 150);

    // ── 恢复点 ── 响应到达后在 loop_ 线程继续，上下文（electionTerm/peer）全在帧内
    if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) co_return;
    if (!ok) co_return;
    // ... 处理投票结果，与第一段在同一函数体，无需跨函数传参
}
```

---

## 2. C++20 协程基础知识

C++20 协程是理解后续改动的前提。如果你熟悉 TypeScript 的 `async/await`，这节会帮你快速建立类比。

**协程 vs 普通函数 vs 线程**：

| | 普通函数 | 线程 | C++20 协程 |
|---|---|---|---|
| 调用时 | 立即执行 | 新线程执行 | 视 initial_suspend 决定 |
| 暂停点 | 不可暂停 | 不可暂停（除非 mutex/sleep）| `co_await` 处主动挂起 |
| 挂起时保存 | 不适用 | 整个线程栈（MB 级）| 协程帧（KB 级，堆分配）|
| 恢复 | 不适用 | 调度器决定 | 显式调用 `handle.resume()` |

**与 TypeScript 最直接的对比**：

TypeScript 的 `await` 表达式：
```typescript
const [ok, result] = await rpcClient.callAsync(method);
// ↑ 挂起等待 Promise resolve，resolve 后继续
```

C++20 的 `co_await` 表达式：
```cpp
auto [ok, result] = co_await client->callAsyncCo(method);
// ↑ 挂起等待 callback 触发，callback 调 h.resume() 后继续
```

**关键区别**：TypeScript 的事件循环自动恢复协程；C++ 需要手动决定谁在哪个线程调用 `handle.resume()`。本项目的约定：**RpcCallAwaiter 的 callback 在 `loop_` 线程触发，`h.resume()` 也在 `loop_` 线程**，由此保证协程体始终在 `loop_` 线程执行——Raft 状态无需加锁。

**`promise_type` 协议**（编译器展开协程的机制）：

编译器看到函数体中有 `co_await`/`co_return`/`co_yield` 时，该函数是协程。编译器会把所有局部变量和恢复状态打包进**协程帧**（堆分配），生成一个状态机。调用协程函数时：
1. 在堆上分配协程帧，把函数参数拷入帧（不在调用方栈上）
2. 调用 `promise.get_return_object()` 生成返回值（如 `FireAndForget{}`）
3. `co_await promise.initial_suspend()`：若 `suspend_never`，立刻开始执行；否则挂起
4. 执行函数体，遇到 `co_await expr`：求值 expr 得到 awaiter，调用 awaiter 的三个方法
5. 函数体结束：`co_await promise.final_suspend()`，若 `suspend_never`，帧自动销毁

**Awaiter 三方法**：

```cpp
struct MyAwaiter {
    // 1. 是否跳过挂起（优化路径）
    //    false → 调用 await_suspend；true → 直接调 await_resume，不挂起
    bool await_ready() const noexcept;

    // 2. 挂起时调用。h 是本协程的 handle（可用来恢复）
    //    返回 void → 协程挂起，控制权返回调用方
    void await_suspend(std::coroutine_handle<> h);

    // 3. 协程恢复后，co_await 表达式的求值结果
    ResultType await_resume();
};
```

类比 TypeScript：`await_ready()` ≈ Promise 已 resolved；`await_suspend()` ≈ `.then(callback)` 注册回调；`await_resume()` ≈ 回调收到的 value。

---

## 3. 改进 A — std::bind → lambda

### 3.1 为什么需要这一步

项目中三处使用了 `std::bind`，是 C++11 的历史遗留：

```cpp
// 旧版（来自 day32 的 Connection.cpp / Acceptor.cpp / Eventloop.cpp）
channel_->setReadCallback(std::bind(&Connection::doRead, this));
channel_->setWriteCallback(std::bind(&Connection::doWrite, this));
acceptChannel_->setReadCallback(std::bind(&Acceptor::acceptConnection, this));
evtChannel_->setReadCallback(std::bind(&Eventloop::handleWakeup, this));
```

`std::bind` 返回不透明的 `std::_Bind<...>` 类型，编译器通常无法内联；项目其他地方已全部用 lambda；C++20 中 `std::bind` 的部分用法已被标记为不推荐。替换为 lambda 统一风格，且性能更好。

### 3.2 编码实现步骤

**修改 `src/common/net/Connection.cpp`，构造函数里的两处 bind**：

```cpp
// 修改前
channel_->setReadCallback(std::bind(&Connection::doRead, this));
channel_->setWriteCallback(std::bind(&Connection::doWrite, this));

// 修改后
channel_->setReadCallback([this] { doRead(); });
channel_->setWriteCallback([this] { doWrite(); });
```

**修改 `src/common/net/Acceptor.cpp`**：

```cpp
// 修改前
acceptChannel_->setReadCallback(std::bind(&Acceptor::acceptConnection, this));

// 修改后
acceptChannel_->setReadCallback([this] { acceptConnection(); });
```

**修改 `src/common/net/Eventloop.cpp`**：

```cpp
// 修改前
evtChannel_->setReadCallback(std::bind(&Eventloop::handleWakeup, this));

// 修改后
evtChannel_->setReadCallback([this] { handleWakeup(); });
```

三处 `std::bind` 都换成了等价的 lambda，行为完全不变——这是纯粹的等价重构。注意 `#include <functional>` 不能移除：`Connection`/`Eventloop` 的回调 setter 签名仍以 `std::function` 为参数类型，头文件依然需要。

---

## 4. 改进 B — setOnMessageCallback 副作用消除

### 4.1 为什么需要这一步

旧版 `setOnMessageCallback` 实现存在隐藏副作用：

```cpp
// 旧版（day32 的 Connection.cpp）
void Connection::setOnMessageCallback(std::function<void(Connection *)> const &cb) {
    onMessageCallback_ = cb;
    // ← 隐藏副作用：改变了 channel_ 的 read 回调！
    channel_->setReadCallback([this] { Business(); });
}
```

setter 函数不应该改变另一个对象的回调。问题：
- 调用者无法"只更新回调但不切换读模式"
- `AsyncRpcClient::onConnected` 调用它时，必须清楚地知道这会触发 channel 切换——这是隐式知识，容易踩坑
- 如果有多处调 `setOnMessageCallback`，每次都会触发 `channel_` 切换，行为不直观

### 4.2 编码实现步骤

**修改 `src/include/net/Connection.h`，新增声明**：

```cpp
// 切换到"业务模式"：把 channel_ 的 read 回调设为 Business()。
// 必须在 setOnMessageCallback() 之后、enableInLoop() 之前显式调用。
void enableMessageMode();
```

**修改 `src/common/net/Connection.cpp`，拆分实现**：

```cpp
// 修改后：setter 只存储回调，无副作用
void Connection::setOnMessageCallback(std::function<void(Connection *)> const &cb) {
    onMessageCallback_ = cb;
}

// 新增：显式切换读模式
void Connection::enableMessageMode() {
    channel_->setReadCallback([this] { Business(); });
}
```

`doRead` 是底层读取（从 fd 读字节到 `inputBuffer_`），`Business` 是调用 `onMessageCallback_`（应用层处理）。两者都是 read 回调，但用途不同——构造时指向 `doRead`（底层接收），业务模式下切换到 `Business`（触发用户回调）。

**相应地，修改所有调用方——`TcpServer::newConnection`**（添加 `enableMessageMode()` 调用）：

```cpp
// 修改后的 TcpServer::newConnection
conn->setOnMessageCallback(onMessageCallback_);
conn->enableMessageMode();  // ← 新增：明确切换
conn->setDeleteConnectionCallback(...);
conn->enableInLoop();
```

**`AsyncRpcClient::onConnected`**（同样需要显式调用）：

```cpp
// 修改后的 AsyncRpcClient::onConnected
conn_ = std::make_unique<Connection>(fd, loop_);
conn_->setDeleteConnectionCallback([this](int) {
    loop_->queueInLoop([this] { handleConnectionClosed(); });
});
conn_->setOnMessageCallback([this](Connection *c) { onResponse(c); });
conn_->enableMessageMode();  // ← 新增：明确切换
conn_->enableInLoop();
```

---

## 5. 改进 C — 协程基础设施：Task.h

### 5.1 为什么需要这个文件

C++20 协程需要用户自己定义返回类型（`FireAndForget`）和辅助工具（`ResumeOnLoop`）。集中放在一个头文件里，让所有用到协程的模块只需 `#include "coro/Task.h"`。

### 5.2 编码实现步骤

**新建 `src/include/coro/Task.h`，写入以下全部内容**

来自 [HISTORY/day32.5/src/include/coro/Task.h](HISTORY/day32.5/src/include/coro/Task.h)：

```cpp
#pragma once
//
// coro/Task.h —— C++20 协程基础设施（为本项目量身定制的最小化实现）
//
// 提供两个工具：
//
//   1. FireAndForget — "点火即忘"协程返回类型。
//      - initial_suspend = suspend_never → 被调用时立刻开始执行，不挂起。
//      - final_suspend   = suspend_never → 到达 co_return 后自动销毁协程帧。
//      - 调用方无需 co_await，也无需保存返回值；适合 selectVote / sendHeartbeat
//        这类「发射后不管」的异步工作单元。
//
//   2. ResumeOnLoop<Loop> — 把协程恢复切换到指定 EventLoop 线程的 awaitable。
//      用法：
//          co_await ResumeOnLoop<Eventloop>{&loop_};
//          // 此后代码运行在 loop_ 线程
//      await_suspend 将 h.resume() 以 lambda 的形式投入 loop_->queueInLoop，
//      保证协程在 loop_ 线程恢复，维持 single-thread invariant。
//
#include <coroutine>
#include <exception>

// ─── FireAndForget ────────────────────────────────────────────────────────────

struct FireAndForget {
    struct promise_type {
        // 被调用时立刻开始运行协程
        FireAndForget get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        // 完成后自动销毁帧，不需要 co_await 等待
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        // 未捕获异常终止进程而不是静默吞掉
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

// ─── ResumeOnLoop<Loop> ───────────────────────────────────────────────────────
//
// 泛型设计避免 Task.h 直接依赖 EventLoop.h，减少包含层次。
// Loop 需要提供 queueInLoop(std::function<void()>) 成员函数。
//
template<typename Loop>
struct ResumeOnLoop {
    Loop *loop;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) const {
        // 将 resume 投入目标 loop 的 pending functors 队列；
        // 调用方线程立刻得到控制权，协程在 loop 线程被唤醒。
        loop->queueInLoop([h]() mutable { h.resume(); });
    }

    void await_resume() const noexcept {}
};
```

**`FireAndForget` 的关键设计**：`initial_suspend = suspend_never`——被调用后立刻执行协程体，遇到第一个 `co_await` 才挂起。`final_suspend = suspend_never`——协程完成后协程帧自动销毁，调用方不需要保存返回值或 handle。这就是"点火即忘"——`runElection()` 调用 `collectVote(term, peer)` 后，协程已经在运行，`runElection` 直接返回处理下一件事。

**`ResumeOnLoop`** 用泛型设计避免 `Task.h` 依赖 `EventLoop.h`（防止循环包含）。当前版本中 `collectVote`/`sendHeartbeat` 都从 `loop_` 线程启动，不需要用到 `ResumeOnLoop`。它是为未来"从 sub-reactor 线程启动协程"的场景准备的基础设施。

---

## 6. 改进 D — RpcCallAwaiter：回调桥接为 co_await

### 6.1 为什么需要这一步

`AsyncRpcClient::callAsync` 是基于回调的：

```cpp
void callAsync(method, json, Callback callback, timeoutMs);
```

协程需要"可 co_await 的"接口：

```cpp
auto [ok, resp] = co_await client->callAsyncCo("RequestVote", reqJson, 150);
```

`RpcCallAwaiter` 负责把"回调风格"桥接为"awaitable 风格"——这是 C++20 协程最常见的模式，类比 TypeScript 中用 `new Promise((resolve) => callbackApi(resolve))` 包装 callback API。

### 6.2 编码实现步骤

**修改 `src/include/rpc/AsyncRpcClient.h`：新增 `#include <coroutine>`，在类内部声明 `callAsyncCo`，在文件末尾新增 `RpcCallAwaiter` 类**

来自 [HISTORY/day32.5/src/include/rpc/AsyncRpcClient.h](HISTORY/day32.5/src/include/rpc/AsyncRpcClient.h)（新增部分）：

```cpp
// 在头文件的 include 区新增：
#include <coroutine>
#include <utility>
```

在 `callAsync` 声明之后新增：

```cpp
// 协程版：在 FireAndForget 协程内使用。
//   auto [ok, resp] = co_await client->callAsyncCo("method", json, timeoutMs);
// 语义与 callAsync 完全一致；返回值通过 co_await 表达式解构。
class RpcCallAwaiter callAsyncCo(const std::string &method, const std::string &requestJson,
                                  int timeoutMs = 200);
```

在文件末尾（`AsyncRpcClient` 类定义之后）新增：

```cpp
// ─── RpcCallAwaiter ──────────────────────────────────────────────────────────
//
// co_await 一次 RPC 调用的 awaitable 对象。
//
// 工作原理：
//   1. await_ready() 始终返回 false → 协程必然先挂起。
//   2. await_suspend(h) 调用 callAsync，把协程句柄 h 存入 callback lambda。
//   3. 当 callAsync 的 callback 在 loop_ 线程触发时：
//      - 将 ok/resp 写入 awaiter（awaiter 仍在协程帧上，安全）
//      - 调用 h.resume() 恢复协程
//   4. await_resume() 在 h.resume() 内被调用，返回 {ok, resp} 给 co_await 表达式。
//
// 线程安全：
//   callback 保证在 loop_ 线程触发（callAsync 约定），因此 h.resume() 也在 loop_ 线程，
//   恢复后的协程代码仍在 loop_ 线程——维持 single-thread invariant，无须额外锁。
//
// 生命周期：
//   RpcCallAwaiter 作为临时对象存活于 co_await 表达式的协程帧上，
//   await_suspend 执行后 lambda 捕获 this（帧内指针）和 h（帧句柄）。
//   先写 ok_/resp_，再调 h.resume()，期间帧始终有效，无 UAF。
//
class RpcCallAwaiter {
  public:
    RpcCallAwaiter(AsyncRpcClient *client, std::string method, std::string json, int timeoutMs)
        : client_(client),
          method_(std::move(method)),
          json_(std::move(json)),
          timeoutMs_(timeoutMs) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h);

    std::pair<bool, std::string> await_resume() noexcept {
        return {ok_, std::move(resp_)};
    }

  private:
    AsyncRpcClient *client_;
    std::string     method_;
    std::string     json_;
    int             timeoutMs_;
    bool            ok_{false};
    std::string     resp_;
};
```

`ok_` 和 `resp_` 存储在 `RpcCallAwaiter` 对象上，而 `RpcCallAwaiter` 是 `co_await` 表达式的临时对象，被编译器存入协程帧——只要协程帧存活（从挂起到 `co_return` 之前），callback 写入 `ok_/resp_` 就是安全的。

---

**修改 `src/common/rpc/AsyncRpcClient.cpp`，在文件末尾新增 `callAsyncCo` 和 `await_suspend` 实现**

来自 [HISTORY/day32.5/src/common/rpc/AsyncRpcClient.cpp](HISTORY/day32.5/src/common/rpc/AsyncRpcClient.cpp)（§6 协程出口，追加在文件末尾）：

```cpp
// ════════════════════════════════════════════════════════════════════════════
// §6 协程出口：RpcCallAwaiter + callAsyncCo
// ════════════════════════════════════════════════════════════════════════════

// callAsyncCo — 构造 RpcCallAwaiter 并返回（工厂函数）。
// 不需要在 loop_ 线程调用（callAsync 内部已 runInLoop）。
RpcCallAwaiter AsyncRpcClient::callAsyncCo(const std::string &method,
                                            const std::string &requestJson,
                                            int timeoutMs) {
    return RpcCallAwaiter{this, method, requestJson, timeoutMs};
}

// RpcCallAwaiter::await_suspend — co_await 的挂起入口。
//
// 调用时序：
//   1. 协程在 co_await 处被编译器挂起（帧已保存），await_suspend(h) 被调用。
//   2. 向 callAsync 注册 callback lambda，lambda 捕获：
//      - this：RpcCallAwaiter* （存储在协程帧上，帧存活时安全）
//      - h   ：std::coroutine_handle<> （拷贝廉价，句柄是个指针）
//   3. callAsync 立刻返回，await_suspend 返回 void（协程继续挂起）。
//   4. 当 RPC 响应到达（或超时），callback 在 loop_ 线程触发：
//      先写 ok_/resp_（帧存活），再 h.resume()。
//   5. h.resume() 内部调用 await_resume()，取出 {ok_, resp_} 作为 co_await 结果。
//
void RpcCallAwaiter::await_suspend(std::coroutine_handle<> h) {
    client_->callAsync(
        method_, json_,
        [this, h](bool ok, std::string resp) mutable {
            // callback 在 loop_ 线程触发（callAsync 约定）。
            // 写入结果到协程帧（写在 resume 之前，保证 await_resume 可见）。
            ok_   = ok;
            resp_ = std::move(resp);
            // 恢复协程：也在 loop_ 线程，维持 single-thread invariant。
            h.resume();
        },
        timeoutMs_);
}
```

`await_suspend` 返回 `void`——意味着协程一定挂起，控制权返回给调用方（`runElection`），`runElection` 继续为下一个 peer 发射协程。`callAsync` 立刻返回，IO 循环继续正常运转。

---

## 7. 改进 E — RaftNode 协程化

### 7.1 为什么需要这一步

把 `startElection+onVoteReply`（两段式）合并为 `runElection+collectVote`（协程），`heartbeatTick+onHeartbeatReply`（两段式）合并为 `heartbeatTick+sendHeartbeat`（协程）。

旧版两段式需要删除的函数：`startElection()`、`onVoteReply()`、`onHeartbeatReply()`。

### 7.2 编码实现步骤

**修改 `src/include/raft/RaftNode.h`：新增 `#include "coro/Task.h"`，替换私有方法声明**

来自 [HISTORY/day32.5/src/include/raft/RaftNode.h](HISTORY/day32.5/src/include/raft/RaftNode.h)（完整内容）：

```cpp
#pragma once
#include "EventLoop.h"
#include "coro/Task.h"
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

    // 选举主流程：为每个 peer 发射一个 collectVote 协程（并发收票）
    void runElection();
    // 心跳主循环：为每个 peer 发射一个 sendHeartbeat 协程
    void heartbeatTick();

    // ── 协程方法（FireAndForget，在 loop_ 线程启动并挂起）─────────────
    // collectVote: 向 peer 发送 RequestVote RPC，处理回包并更新投票计数。
    // 等效于旧版 startElection 中的 callAsync lambda + onVoteReply()。
    FireAndForget collectVote(uint64_t electionTerm, Peer peer);

    // sendHeartbeat: 向 peer 发送 AppendEntries(心跳) RPC，处理 term 退位。
    // 等效于旧版 heartbeatTick 中的 callAsync lambda + onHeartbeatReply()。
    FireAndForget sendHeartbeat(Peer peer);

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

与 day32 的头文件相比，变化是：新增 `#include "coro/Task.h"`；删除 `startElection`/`onVoteReply`/`onHeartbeatReply`；新增 `runElection`/`collectVote`/`sendHeartbeat`。

---

**修改 `src/common/raft/RaftNode.cpp`：替换 §4（选举）和 §5（心跳）**

来自 [HISTORY/day32.5/src/common/raft/RaftNode.cpp](HISTORY/day32.5/src/common/raft/RaftNode.cpp)（§1–§3 与 day32 完全相同，只展示 §4 和 §5 的变化）：

```cpp
// ════════════════════════════════════════════════════════════════════════════
// §4  选举主流程：runElection + collectVote 协程
//
// 旧版两段式：startElection()（发射 callAsync）+ onVoteReply()（处理回包）。
// 每个 peer 的"发请求 → 处理回包"逻辑分散在两个函数，调用链为：
//   electionTimerFired → becomeCandidate → startElection → callAsync → [callback] → onVoteReply
//
// 协程版：runElection 为每个 peer 发射一个 collectVote 协程（并发），
//   每个协程在 co_await callAsyncCo 挂起，响应到达后在 loop_ 线程恢复，
//   将"发请求"和"处理回包"合并为一段连续的线性代码。
// ════════════════════════════════════════════════════════════════════════════

AsyncRpcClient *RaftNode::getOrCreateClient(const Peer &peer) {
    auto it = peerClients_.find(peer.id);
    if (it != peerClients_.end()) return it->second.get();
    auto client = std::make_unique<AsyncRpcClient>(&loop_, peer.ip, peer.port);
    auto *raw   = client.get();
    peerClients_.emplace(peer.id, std::move(client));
    return raw;
}

void RaftNode::runElection() {
    // 为每个 peer 发射一个 collectVote 协程（并发）。
    // collectVote 是 FireAndForget：调用后立刻开始执行，到达 co_await callAsyncCo
    // 时挂起，把 callAsync 派发出去；runElection 不等待它们完成即可返回。
    uint64_t term = currentTerm_.load();
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        collectVote(term, peer);
    }
}

FireAndForget RaftNode::collectVote(uint64_t electionTerm, Peer peer) {
    // 构造请求（在挂起前，仍在 loop_ 线程）
    RequestVoteArgs args{electionTerm, id_, lastLogIndex(), lastLogTerm()};
    std::string reqJson = json(args).dump();

    // ── 挂起点：发出 RPC，等待响应/超时 ────────────────────────────────
    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo(
        "RequestVote", reqJson, /*timeoutMs=*/150);

    // ── 恢复点（loop_ 线程）────────────────────────────────────────────
    // 守卫①：在等待期间，节点状态可能已改变（如收到更高 term 退为 Follower）。
    // 守卫②：currentTerm_ 可能已递增（本轮选举已作废）。
    // 任一不满足，说明这个回包对当前轮次无效，直接丢弃。
    if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) co_return;

    if (!ok) co_return; // RPC 超时或网络故障，忽略

    RequestVoteReply reply{};
    try {
        reply = json::parse(respJson).get<RequestVoteReply>();
    } catch (...) {
        co_return; // 解码失败，丢弃
    }

    // 【Raft 规则 §5.1】回包 term 更大：立刻退位
    if (reply.term > currentTerm_.load()) {
        becomeFollower(reply.term);
        co_return;
    }

    if (reply.voteGranted) {
        ++currentElectionVotes_;
        LOG_INFO << "[Node " << id_ << "] 收到节点 " << peer.id
                 << " 的投票（已得票=" << currentElectionVotes_ << "/" << quorum_ << "）";
        if (currentElectionVotes_ >= quorum_) becomeLeader();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §5  心跳：heartbeatTick + sendHeartbeat 协程
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::heartbeatTick() {
    if (state_.load() != State::Leader) return;
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        sendHeartbeat(peer); // 并发发射，每个 peer 独立协程
    }
}

FireAndForget RaftNode::sendHeartbeat(Peer peer) {
    AppendEntriesArgs args{currentTerm_.load(), id_};
    std::string reqJson = json(args).dump();

    // ── 挂起点 ──────────────────────────────────────────────────────────
    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo(
        "AppendEntries", reqJson, /*timeoutMs=*/100);

    // ── 恢复点（loop_ 线程）────────────────────────────────────────────
    if (!ok) co_return; // 超时或网络故障：下一个 50ms 周期重试

    try {
        auto reply = json::parse(respJson).get<AppendEntriesReply>();
        // 回包 term 更大：自己是"僵尸 Leader"，立刻退位
        if (reply.term > currentTerm_.load()) becomeFollower(reply.term);
    } catch (...) {
        // 解码失败，忽略
    }
}

uint64_t RaftNode::lastLogIndex() const { return static_cast<uint64_t>(log_.size() - 1); }
uint64_t RaftNode::lastLogTerm() const { return log_.back().term; }
```

**对比旧版代码量**：

| 函数 | 旧版行数 | 新版行数 |
|------|---------|---------|
| `startElection` + `onVoteReply` | ~40 行，2 个函数 | `runElection` + `collectVote` ~30 行，2 个函数 |
| `heartbeatTick` + `onHeartbeatReply` | ~25 行，2 个函数 | `heartbeatTick` + `sendHeartbeat` ~20 行，2 个函数 |

逻辑完全等价，但协程版上下文全在同一函数体内，无需跨函数传参，守卫条件也更集中。

**`electionTimerFired` 中 `startElection()` 改为 `runElection()`**：

```cpp
void RaftNode::electionTimerFired(uint64_t epoch) {
    if (epoch != electionEpoch_) return;
    if (state_.load() == State::Leader) return;
    becomeCandidate();
    runElection(); // 旧版：startElection()，现在改为 runElection()
}
```

---

## 8. 整体运行时理解

### 8.1 对象所有权与线程归属

协程版与 day32 线程模型完全相同，新增的协程帧是堆对象，归属 loop_ 线程（协程在 loop_ 线程启动和恢复）：

```
RaftNode（T_main 构造，loopThread_ 运行）
 │
 └── loop_（T_raft）
      ├── peerClients_[peer0]→AsyncRpcClient
      │    └── pending_[reqId]→PendingCall::cb
      │         └── lambda: [this=&awaiter, h=coroutine_handle]
      │                           │
      │                           └── coroutine_handle → 协程帧（堆）
      │                                ├── electionTerm, peer（函数参数，拷入帧）
      │                                ├── args, reqJson（局部变量，在帧）
      │                                ├── __resume_point（状态机跳转标签）
      │                                └── RpcCallAwaiter {ok_, resp_}（co_await 临时对象）
      │
      ├── collectVote 协程帧（peer0）  ← FireAndForget，final_suspend=never，帧随协程完成销毁
      ├── collectVote 协程帧（peer1）
      └── collectVote 协程帧（peer2）
```

**关键认知**：`runElection()` 调用 `collectVote(term, peer0)` 后，`collectVote` 立刻执行到 `co_await`，此时：
1. 协程帧已经在堆上分配，参数 `electionTerm`/`peer` 已拷入帧（不在 `runElection` 的栈上）
2. `await_suspend(h)` 调 `callAsync`，callback 捕获 `this`（帧内 awaiter 地址）和 `h`（帧句柄）
3. `callAsync` 立刻返回，`await_suspend` 返回，协程挂起
4. 控制权回到 `runElection`，继续为 peer1/peer2 发射协程

`runElection` 的栈帧销毁后，协程帧纹丝不动，静静躺在堆里，等待 `pending_[reqId].cb` 被触发。

---

### 8.2 场景 A — collectVote 完整执行追踪

**场景设定**：3 节点集群，Node 2 是 Candidate（term=1），正在向 Node 0（peer）发 RequestVote，`peerClients_[0]` 的 AsyncRpcClient 已建立连接（kConnected）。

---

#### 第 1 步：runElection 启动 collectVote 协程

打开 [HISTORY/day32.5/src/common/raft/RaftNode.cpp](HISTORY/day32.5/src/common/raft/RaftNode.cpp)，`runElection`：

```cpp
void RaftNode::runElection() {
    uint64_t term = currentTerm_.load();  // = 1
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;     // 跳过自己（id_=2）
        collectVote(term, peer);           // peer0：启动协程
    }
}
```

`collectVote(1, peer0)` 被调用：编译器在堆上分配协程帧，`electionTerm=1`、`peer={id=0,...}` 拷入帧。`initial_suspend = suspend_never`，协程立刻开始执行：

```cpp
FireAndForget RaftNode::collectVote(uint64_t electionTerm, Peer peer) {
    // 此刻：在 loop_ 线程，electionTerm=1，peer.id=0
    RequestVoteArgs args{1, 2, 0, 0};         // lastLogIndex=0, lastLogTerm=0
    std::string reqJson = json(args).dump();   // = '{"term":1,"candidateId":2,...}'
    // 执行到 co_await，协程即将挂起
```

**此刻状态快照**：
```
协程帧（堆，peer0）：
  electionTerm = 1
  peer.id      = 0
  args         = {term:1, candidateId:2, lastLogIndex:0, lastLogTerm:0}
  reqJson      = '{"term":1,...}'
  RpcCallAwaiter: {client_=&peerClients_[0], ok_=false, resp_=""}
__resume_point = LABEL_AFTER_CO_AWAIT  ← 恢复后从这里继续
```

---

#### 第 2 步：co_await 触发挂起，await_suspend 发出 RPC

编译器调用 `awaiter.await_ready()` → `false`（始终挂起）。协程帧保存，调用 `await_suspend(h)`：

```cpp
void RpcCallAwaiter::await_suspend(std::coroutine_handle<> h) {
    // h 指向 peer0 的 collectVote 协程帧
    client_->callAsync(
        "RequestVote", '{"term":1,...}',
        [this, h](bool ok, std::string resp) mutable {
            ok_   = ok;
            resp_ = std::move(resp);
            h.resume();
        },
        /*timeoutMs=*/150);
    // callAsync 立刻返回（注册 pending_[reqId=1]），
    // await_suspend 返回 void → 协程挂起
}
```

`callAsync` 内部 `doCall`：`reqId=1`，`pendingFrames_` 或直接 `conn_->send(frame)`（已连接），注册 150ms 超时定时器。

**此刻状态快照**：
```
peerClients_[0].pending_ = {1: {cb=lambda[this=&awaiter, h=coro_handle_peer0], timerEpoch=1}}
collectVote 协程（peer0） = 挂起于 co_await
T_raft（loop_）          = 空闲，继续 runElection 为 peer1 发射协程
```

---

#### 第 3 步：Node 0 回包，callback 触发，协程恢复

Node 0 处理 RequestVote，回包 `{"term":1,"voteGranted":true}` 到达 Node 2 的 `AsyncRpcClient::onResponse`，解帧后调 `pending_[1].cb(true, '{"term":1,"voteGranted":true}')`：

```cpp
// callback lambda（在 T_raft 线程执行）
ok_   = true;
resp_ = '{"term":1,"voteGranted":true}';
h.resume();  // 恢复 collectVote 协程
```

协程从 `LABEL_AFTER_CO_AWAIT` 继续，`await_resume()` 返回 `{true, '{"term":1,"voteGranted":true}'}`：

```cpp
// ── 恢复点（loop_ 线程）────────────────────────────────────────────
auto [ok, respJson] = {true, '{"term":1,"voteGranted":true}'};

// 守卫①：state_=Candidate ✓，守卫②：currentTerm_=1==electionTerm=1 ✓
if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) co_return;

if (!ok) co_return;   // ok=true，不 return

reply = json::parse(respJson).get<RequestVoteReply>();
// reply = {term:1, voteGranted:true}

// reply.term=1 <= currentTerm_=1，不退位
if (reply.voteGranted) {
    ++currentElectionVotes_;  // 1 → 2
    if (currentElectionVotes_ >= quorum_) becomeLeader();  // 2 >= 2 → 成为 Leader
}
```

**此刻状态快照**：
```
currentElectionVotes_ = 2
state_               = Leader（becomeLeader 设置）
协程帧（peer0）      = 到达 co_return / 函数末尾，final_suspend=never → 自动销毁
pending_[1]          = 已被 onResponse 中 erase
```

---

#### 第 4 步：stop() 路径（清理飞行中的协程）

若在协程挂起期间（等待 Node 0 回包时）调用 `node.stop()`：

打开 [HISTORY/day32.5/src/common/raft/RaftNode.cpp](HISTORY/day32.5/src/common/raft/RaftNode.cpp)，`stop()`：

```cpp
void RaftNode::stop() {
    // ...
    // 2. 把所有 AsyncRpcClient 析构投递到 loop_ 线程
    loop_.queueInLoop([this] { peerClients_.clear(); });
    // peerClients_.clear() → ~AsyncRpcClient() → stop() → failAllPending("client stopped")
    //   → pending_[1].cb(false, "") 触发
    //     → awaiter.ok_ = false
    //     → h.resume()（在 loop_ 线程）
    //   → collectVote 协程恢复：ok=false → if (!ok) co_return;
    //   → co_return → final_suspend=never → 协程帧自动销毁
    // 3. setQuit + wakeup → loop_ 退出
    // ...
}
```

所有飞行中的协程都会通过 `failAllPending` → `cb(false, "")` → `h.resume()` → `co_return` 路径被清理，无内存泄漏，无 use-after-free。

---

#### 调用链总结

```
T_raft
    │
    │ runElection()
    │   collectVote(1, peer0) ─── FireAndForget 协程启动
    │   co_await callAsyncCo ─── 协程挂起，callAsync 派发
    │   collectVote(1, peer1) ─── 另一协程启动
    │   co_await callAsyncCo ─── 挂起
    │   [runElection 返回]
    │
    │   ... loop_ 继续处理其他事件（心跳、RPC）...
    │
    │ [TCP 收包，onResponse]
    │   pending_[1].cb(true, resp)
    │     awaiter.ok_=true, resp_=...
    │     h.resume() ──── 协程 peer0 恢复执行
    │       守卫通过
    │       reply.voteGranted=true → votes=2 → becomeLeader()
    │       co_return → 帧自动销毁
```

---

### 8.3 协程帧 vs 回调闭包：本质是同一件事

协程帧和回调闭包在功能上是等价的——都是"把状态（上下文）和后续逻辑（continuation）打包到堆上"。

**旧版（回调闭包）**：
```cpp
getOrCreateClient(peer)->callAsync("RequestVote", reqJson,
    [this, term, peerId=peer.id](bool ok, std::string resp) {
        // term, peerId 存在闭包对象（堆上）
        // ok, resp 是回调参数
        onVoteReply(term, peerId, ok, resp);
    }, 150);
```

**新版（协程帧）**：
```cpp
// 编译器把 electionTerm, peer, args, reqJson 全部存入协程帧（堆上）
auto [ok, respJson] = co_await callAsyncCo("RequestVote", reqJson, 150);
// 恢复点从帧里取 electionTerm, peer 继续执行
```

两者的内存占用和生命周期几乎相同，区别只是代码的组织形式——协程让续体（continuation）以线性代码的形式呈现，而不是散落在多个回调函数里。

---

## 9. 各模块职责速查表

| 模块/函数 | 所在线程 | 调用时机 | 职责一句话 |
|-----------|---------|---------|-----------|
| `FireAndForget` | — | 用作协程返回类型 | initial/final_suspend=never：立刻执行，完成后自动销毁帧 |
| `ResumeOnLoop<Loop>` | 任意→Loop线程 | `co_await ResumeOnLoop{&loop_}` | 把协程恢复切到目标 loop 线程 |
| `RpcCallAwaiter::await_suspend` | T_raft | co_await callAsyncCo 时 | 调 callAsync 注册 callback，callback 捕获 h 在 loop_ 线程 resume |
| `RpcCallAwaiter::await_resume` | T_raft | h.resume() 后 | 返回 {ok_, resp_} 给 co_await 表达式 |
| `AsyncRpcClient::callAsyncCo` | T_raft | collectVote/sendHeartbeat 内 | 工厂函数：构造 RpcCallAwaiter 并返回 |
| `Connection::enableMessageMode` | loop_ 线程 | onConnected / TcpServer::newConnection | 显式将 channel_ 的 read 回调切换为 Business()；与 setOnMessageCallback 解耦 |
| `RaftNode::runElection` | T_raft | electionTimerFired | 为每个 peer 发射一个 collectVote 协程 |
| `RaftNode::collectVote` | T_raft（协程帧在堆） | runElection | 挂起于 callAsyncCo → 恢复后处理 RequestVote 回包 + 更新投票计数 |
| `RaftNode::heartbeatTick` | T_raft | runEvery(50ms) | 仅 Leader：为每个 peer 发射 sendHeartbeat 协程 |
| `RaftNode::sendHeartbeat` | T_raft（协程帧在堆）| heartbeatTick | 挂起于 callAsyncCo → 恢复后检测僵尸 Leader（reply.term > currentTerm_）|

---

## 10. 工程化

### CMakeLists.txt 变更

来自 [CMakeLists.txt](CMakeLists.txt)（相对 day32 的变更，只有两处）：

```cmake
# 修改：C++17 → C++20（协程需要 C++20）
set(CMAKE_CXX_STANDARD 20)
...
# 在已有的 target_include_directories(NetLib PUBLIC ...) 块中新增一行 coro 路径：
target_include_directories(NetLib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
        ...
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/memory>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/coro>
        ...
)
```

`<coroutine>` 头文件需要 C++20。Apple Clang 14+（Xcode 14+）和 GCC 11+ 已完整支持 C++20 协程。除了把标准从 17 提到 20、在 NetLib 的 include 列表里补上 `src/include/coro` 之外，day32.5 没有改动其它构建配置。

---

## 11. 验证

```bash
# 完整清理 + 重新 configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target NetLib -j

# 编译并运行 raft_demo（行为与 day32 完全一致）
cmake --build build --target raft_demo -j

# 三个终端
./build/examples/raft_demo --id 0 --nodes 3
./build/examples/raft_demo --id 1 --nodes 3
./build/examples/raft_demo --id 2 --nodes 3
```

协程版与 day32 行为完全等价（协程只改变了"发请求+处理响应"的代码组织方式，不影响 Raft 协议逻辑）。预期观察与 day32 相同：150~300ms 内选出 Leader，kill Leader 后约 300ms 内重新选出。

---

## 12. 局限与下一步

| 局限 | 描述 |
|------|------|
| **协程异常 = terminate** | `FireAndForget::promise_type::unhandled_exception` 直接 `std::terminate()`。未来可改为捕获异常并记录日志，但需确保协程体内所有异常都能被妥善处理 |
| **日志复制未实现** | `AppendEntries` 当前只发空心跳。Day33 补 `prevLogIndex/entries/leaderCommit`，`sendHeartbeat` 需扩展为 `replicateLog` 协程 |
| **持久化未实现** | `currentTerm_/votedFor_/log_` 全在内存。Day33 接入文件存储 |
| **`ResumeOnLoop` 未被使用** | 基础设施已就绪，等待未来有从非 loop_ 线程启动协程的需求 |

接下来 **Day33** 将实现 Raft 日志复制（AppendEntries 完整语义）、持久化，并引入 `RaftTestHarness` 白盒测试框架。`sendHeartbeat` 届时将扩展为 `replicateLog` 协程，直接携带日志条目——协程线性代码的组织优势在多步骤操作（发心跳/发日志/处理响应/推进 commitIndex）中会更加明显。
