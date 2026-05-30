# Day 33 — Raft 日志复制 / commitIndex 推进 / 复制状态机

## 目录

| 章节 | 内容 |
|------|------|
| [§1 引言](#1-引言) | day32 只有心跳，无日志复制；本日目标 |
| [§2 改进 A — AppendEntries 完整语义](#2-改进-a--appendentries-完整语义) | 修改 `RaftTypes.h`：扩展 AppendEntriesArgs/Reply；修改 `handleAppendEntries`：完整四规则 |
| [§3 改进 B — becomeLeader 初始化追踪状态](#3-改进-b--becomeleader-初始化追踪状态) | 修改 `RaftNode.h/cpp`：新增 `nextIndex_/matchIndex_`；`becomeLeader` 初始化追踪状态 |
| [§4 改进 C — replicateLog 协程（心跳与复制合一）](#4-改进-c--replicatelog-协程心跳与复制合一) | 修改 `RaftNode.cpp`：`sendHeartbeat` → `replicateLog`；成功/失败分支处理；冲突加速回退 |
| [§5 改进 D — advanceCommitIndex + applyCommitted](#5-改进-d--advancecommitindex--applycommitted) | 修改 `RaftNode.cpp`：Figure 8 规则；commitIndex/lastApplied 分层；applyCallback_ |
| [§6 改进 E — propose() 外部写入接口](#6-改进-e--propose-外部写入接口) | 修改 `RaftNode.h/cpp`：线程安全的外部写入；立刻触发复制 |
| [§7 改进 F — raft_demo 更新](#7-改进-f--raft_demo-更新) | 修改 `examples/src/raft_demo.cpp`：`--propose-interval`；状态行新增 logSize/commit/applied |
| [§8 整体运行时理解](#8-整体运行时理解) | 对象所有权图；场景 A（一条命令从 propose 到 applied 的完整路径）；场景 B（Follower 落后追赶）|
| [§9 各模块职责速查表](#9-各模块职责速查表) | 本日所有新增/修改函数一览 |
| [§10 工程化](#10-工程化) | `RaftNode.h` 新增 getter |
| [§11 验证](#11-验证) | 构建命令 + 完整演示 |
| [§12 局限与下一步](#12-局限与下一步) | 无持久化；Day34 计划 |

---

## 本日变更文件一览

| 文件 | 变更 | 核心改动 |
|------|------|---------|
| `src/include/raft/RaftTypes.h` | **修改** | `AppendEntriesArgs` 新增 4 个字段；`AppendEntriesReply` 新增 `conflictIndex/conflictTerm` |
| `src/include/raft/RaftNode.h` | **修改** | 新增 `nextIndex_/matchIndex_`、`commitIndex_/lastApplied_`、`applyCallback_`；新增 `propose`/`setApplyCallback`/`getCommitIndex`/`getLastApplied`/`getLeaderId`/`getLastLogIndex`；`sendHeartbeat` → `replicateLog` |
| `src/common/raft/RaftNode.cpp` | **修改** | `handleAppendEntries` 完整四规则；`becomeLeader` 追踪状态初始化；`sendHeartbeat` → `replicateLog`；新增 `advanceCommitIndex`/`applyCommitted`/`propose` |
| `examples/src/raft_demo.cpp` | **修改** | 新增 `--propose-interval` 参数；状态行增加 logSize/commit/applied/leaderId；`setApplyCallback` 注册 |

---

## 1. 引言

day32 的 `AppendEntries` 是个空占位符：发送方发 `{term, leaderId}`，接收方只做"重置选举计时器"，集群里的日志永远只有哨兵一条。

**day33 要解决三个问题**：

| 问题 | Raft 解法 |
|------|-----------|
| 怎么把命令广播给所有 Follower？ | AppendEntries 携带 `prevLogIndex/prevLogTerm/entries/leaderCommit`，Follower 用**前缀匹配**保证历史一致 |
| 几票算"安全"（可以提交）？ | **多数派确认**：`matchIndex[peer] >= N` 的节点数 ≥ quorum，才推进 commitIndex |
| 新 Leader 能直接提交旧 term 的条目吗？ | **不能**（Figure 8 规则）。`advanceCommitIndex` 只统计**当前 term** 的条目能否达成多数派；一旦某条当前 term 的条目被提交，它之前的旧 term 条目就"顺带"被提交 |

**前缀匹配（Prefix Consistency）的核心直觉**：如果两个节点的日志在 index=N 处的 term 相同，那么从 index 0 到 N 的所有条目必然完全相同。这个性质在所有正确运行的时刻始终成立——`prevLogIndex/prevLogTerm` 就是用来验证这个性质的。

---

## 2. 改进 A — AppendEntries 完整语义

### 2.1 为什么需要扩展 AppendEntries

day32 的 `AppendEntriesArgs` 只有 `term` 和 `leaderId`，无法携带日志内容。`AppendEntriesReply` 也没有冲突位置信息，Leader 无法快速定位 Follower 落后到哪里。

**四规则**（Follower 处理 AppendEntries 的完整逻辑）：
1. **规则① 过期 term**：拒绝 term < currentTerm 的 RPC
2. **规则② 前缀检查**：`log[prevLogIndex].term != prevLogTerm` → 拒绝，返回冲突信息
3. **规则③ 幂等追加**：逐条对比 term，遇到不一致则截断并覆盖（相同则跳过）
4. **规则④ 推进 commitIndex**：`leaderCommit > 本地 commitIndex` → 更新，触发 apply

**冲突加速回退**（Raft §5.3 优化）：

朴素方式是 `nextIndex--` 每轮减一，最坏 O(N) 轮 RPC。优化方式：Follower 在拒绝时返回 `conflictTerm`（prevLogIndex 处的 term）和该 term 在 Follower 日志中的第一条 index，让 Leader 一步跳过整个冲突 term 段。

### 2.2 编码实现步骤

**第一步：修改 `src/include/raft/RaftTypes.h`，扩展 AppendEntries 相关结构体**

打开 [src/include/raft/RaftTypes.h](src/include/raft/RaftTypes.h)，将 `AppendEntriesArgs` 和 `AppendEntriesReply` 替换为：

```cpp
// AppendEntries：日志复制 + 心跳合一（空 entries = 纯心跳）
struct AppendEntriesArgs {
    uint64_t              term{0};
    int                   leaderId{-1};
    // 一致性检查字段：「我发的这批条目之前紧接着哪一条？」
    uint64_t              prevLogIndex{0};
    uint64_t              prevLogTerm{0};
    // 要复制的日志条目（心跳时为空）
    std::vector<LogEntry> entries{};
    // Leader 当前已提交到的位置，Follower 用它来推进自己的 commitIndex
    uint64_t              leaderCommit{0};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesArgs,
    term, leaderId, prevLogIndex, prevLogTerm, entries, leaderCommit)

// 冲突提示（success=false 时有效）：让 Leader 快速定位应该回退到哪里
//   conflictTerm==0：Follower 日志太短，conflictIndex = len(log)
//   conflictTerm!=0：Follower 在 prevLogIndex 处的 term = conflictTerm，
//                    conflictIndex = 该 term 在 Follower 日志中的第一条 index
struct AppendEntriesReply {
    uint64_t term{0};
    bool     success{false};
    uint64_t conflictIndex{0};
    uint64_t conflictTerm{0};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppendEntriesReply, term, success, conflictIndex, conflictTerm)
```

`prevLogIndex/prevLogTerm` 是"前缀证明"——告诉 Follower"我发的是 index N 之后的内容，你的 log[N].term 应该等于 prevLogTerm"。`entries` 为空时就是纯心跳。`leaderCommit` 让 Follower 知道可以安全 apply 到哪个 index。`NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` 宏自动生成 JSON 序列化/反序列化，`LogEntry` 也有同样的宏，所以 `vector<LogEntry>` 可以整体序列化。

---

**第二步：修改 `src/common/raft/RaftNode.cpp`，重写 `handleAppendEntries`（Follower 侧四规则）**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§2 RPC 回调段）：

```cpp
void RaftNode::handleAppendEntries(const std::string &reqJson, RpcServer::Done done) {
    AppendEntriesArgs args;
    try {
        args = json::parse(reqJson).get<AppendEntriesArgs>();
    } catch (...) {
        done(R"({"term":0,"success":false,"conflictIndex":0,"conflictTerm":0})");
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        AppendEntriesReply reply{currentTerm_.load(), false, 0, 0};

        // 规则①：过时 term 的 RPC 直接拒绝（发送方是旧 Leader，已被淘汰）
        if (args.term < currentTerm_.load()) {
            done(json(reply).dump());
            return;
        }

        // 收到有效 Leader 的消息：确认自己是 Follower
        state_.store(State::Follower);
        leaderId_ = args.leaderId;
        resetElectionTimer();  // 压制自己的选举超时

        // 规则②（一致性检查）：我的日志里是否存在 prevLogIndex 处的条目，且 term 匹配？
        if (args.prevLogIndex > lastLogIndex()) {
            // 日志太短：根本没有 prevLogIndex 处的条目
            reply.conflictIndex = lastLogIndex() + 1;
            reply.conflictTerm  = 0;
            done(json(reply).dump());
            return;
        }
        if (log_[args.prevLogIndex].term != args.prevLogTerm) {
            // term 不匹配：找到冲突 term 在我这里的第一条 index，让 Leader 跳过整个冲突 term
            uint64_t ct = log_[args.prevLogIndex].term;
            uint64_t ci = args.prevLogIndex;
            while (ci > 0 && log_[ci - 1].term == ct) --ci;
            reply.conflictIndex = ci;
            reply.conflictTerm  = ct;
            done(json(reply).dump());
            return;
        }

        // 规则③：幂等追加
        // 逐条检查：若现有条目 term 与 entries[i].term 不同，则截断并覆盖；
        // 若已存在且 term 相同，则跳过（重传消息的幂等处理）。
        uint64_t insertAt = args.prevLogIndex + 1;
        for (size_t i = 0; i < args.entries.size(); ++i) {
            uint64_t logIdx = insertAt + (uint64_t)i;
            if (logIdx < log_.size()) {
                if (log_[logIdx].term != args.entries[i].term) {
                    log_.resize(logIdx);
                    log_.push_back(args.entries[i]);
                }
                // else：term 相同 = 已有该条目（重传），跳过
            } else {
                log_.push_back(args.entries[i]); // 追加新条目
            }
        }

        // 规则④：推进 commitIndex
        // Leader 已提交到 leaderCommit，我也可以安全应用到同样位置（取两者较小）
        if (args.leaderCommit > commitIndex_.load()) {
            commitIndex_.store(std::min(args.leaderCommit, lastLogIndex()));
            applyCommitted();
        }

        reply.success = true;
        done(json(reply).dump());
    });
}
```

规则② 的冲突回退逻辑：以 `prevLogIndex=3, prevLogTerm=2` 为例，如果 Follower 的 `log_[3].term != 2`（比如是 term=1），说明在 index 3 处历史已经分叉。Follower 找到 term=1 在自己日志里的第一条 index（比如 index=2），返回 `conflictTerm=1, conflictIndex=2`，Leader 就能一次跳回 nextIndex=2，而不是逐步从 3→2。

规则③ 的"截断"处理是幂等性的关键：如果 Leader 重传了已有的条目（term 相同 → 跳过），如果要覆盖一个 term 不同的旧条目（`log_.resize(logIdx)` 截断后追加）——两种情况都是幂等的，无论 RPC 重传多少次结果都一样。

规则④ 里 `min(leaderCommit, lastLogIndex())` 取较小值：Follower 可能只收到了部分 entries（网络分包），不能 commit 到一个自己还没有的 index。

---

## 3. 改进 B — becomeLeader 初始化追踪状态

### 3.1 为什么需要 nextIndex_ 和 matchIndex_

Leader 管理着一批 Follower，每个 Follower 的日志长度各不相同。Leader 需要记录"对每个 Follower，下次应该发从哪里开始的日志"：

```
[Leader 刚上任，log_ = [哨兵, A, B, C]，lastLogIndex=3]

nextIndex_  = {1: 4, 2: 4}   ← 乐观：先假设 Follower 和我一样新
matchIndex_ = {1: 0, 2: 0}   ← 保守：还不知道 Follower 有什么

[Follower 1 实际只有到 index=1]
replicateLog(peer=1): 发 prevIdx=3, prevTerm=t(C)
→ 回复 success=false, conflictIndex=2, conflictTerm=t(A)
→ Leader: nextIndex_[1] = 2

[下一轮]
replicateLog(peer=1): 发 prevIdx=1, prevTerm=t(A), entries=[B,C]
→ success=true, matchIndex_[1]=3, nextIndex_[1]=4
→ advanceCommitIndex(): commitIndex=3
```

`nextIndex_` 是乐观估计（可能超前），`matchIndex_` 是安全下界（已确认到达）。`advanceCommitIndex` 只能用 `matchIndex_` 做多数派统计——用 `nextIndex_` 会不安全（还未确认）。

### 3.2 编码实现步骤

**第三步：修改 `src/include/raft/RaftNode.h`，新增成员变量和接口**

打开 [src/include/raft/RaftNode.h](src/include/raft/RaftNode.h)，在私有成员变量区新增：

```cpp
// ── 日志复制状态（Day33 新增）────────────────────────────────────────
std::atomic<uint64_t> commitIndex_{0};  // 已提交的最高 index（多数派确认）
std::atomic<uint64_t> lastApplied_{0};  // 已应用到状态机的最高 index

// Leader 专属（仅在 loop_ 线程访问，becomeLeader 初始化，角色切换后可能过时）
std::unordered_map<int, uint64_t> nextIndex_;   // peer.id → 下次发送的 index
std::unordered_map<int, uint64_t> matchIndex_;  // peer.id → 已确认复制的最高 index

// 状态机回调（必须在 start() 之前注册）
std::function<void(uint64_t, const std::string &)> applyCallback_;
```

在 public 接口区新增：

```cpp
void propose(const std::string &cmd);
void setApplyCallback(std::function<void(uint64_t, const std::string &)> cb) {
    applyCallback_ = std::move(cb);
}
uint64_t getCommitIndex()  const { return commitIndex_.load(); }
uint64_t getLastApplied()  const { return lastApplied_.load(); }
int      getLeaderId()     const { return leaderId_; }
uint64_t getLastLogIndex() const {
    // 仅近似值（loop_ 线程外读 log_ 可能不精确），仅供展示用
    return static_cast<uint64_t>(log_.size()) - 1;
}
```

在私有方法区将 `sendHeartbeat(Peer)` 改为 `replicateLog(Peer)`，并新增 `advanceCommitIndex()`/`applyCommitted()`：

```cpp
// 日志复制 + 心跳合一：每次心跳 = 一次 replicateLog
// 无新条目时 entries=[] 作为心跳；有新条目时附带日志段
FireAndForget replicateLog(Peer peer);

// 提交推进：Leader 在 matchIndex 更新后调用
void advanceCommitIndex();
// 应用已提交但尚未 apply 的条目（lastApplied → commitIndex）
void applyCommitted();
```

---

**第四步：修改 `src/common/raft/RaftNode.cpp`，更新 `becomeLeader`**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§3 角色切换段）：

```cpp
void RaftNode::becomeLeader() {
    state_.store(State::Leader);
    leaderId_ = id_;
    LOG_INFO << "[Node " << id_ << "] *** 成为领导者，任期=" << currentTerm_.load() << " ***";
    ++electionEpoch_;

    // 初始化 Leader 专属的 per-peer 追踪状态：
    //   nextIndex[i]  = lastLogIndex + 1   （乐观：先假设 follower 和我一样新）
    //   matchIndex[i] = 0                  （保守：还不知道 follower 有什么）
    // 如果 follower 落后，replicateLog 协程会在收到 success=false 后回退 nextIndex。
    uint64_t nextIdx = lastLogIndex() + 1;
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        nextIndex_[peer.id]  = nextIdx;
        matchIndex_[peer.id] = 0;
    }

    // 立刻广播一次复制（空 entries = 心跳），尽快通知所有 Follower 新 Leader 存在。
    heartbeatTick();
}
```

`nextIndex_` 从 `lastLogIndex()+1` 出发——乐观地认为 Follower 和我一样新；`matchIndex_` 从 0 出发——保守地认为 Follower 什么都没有。`heartbeatTick()` 立刻触发一轮复制（无新条目时即纯心跳），尽快通知所有 Follower 新 Leader 的存在并压制它们的选举超时。

---

## 4. 改进 C — replicateLog 协程（心跳与复制合一）

### 4.1 为什么需要这一步

day32 的 `sendHeartbeat` 只发 `{term, leaderId}`，是纯占位。现在需要一个"心跳与复制合一"的协程：有新条目时携带日志段，没有新条目时发空 entries（纯心跳）——两种情况用同一条 RPC，处理逻辑完全统一。

### 4.2 编码实现步骤

**第五步：修改 `src/common/raft/RaftNode.cpp`，将 `heartbeatTick` 改为调 `replicateLog`，实现 `replicateLog` 协程**

`heartbeatTick` 的变化只是把 `sendHeartbeat(peer)` 改为 `replicateLog(peer)`：

```cpp
void RaftNode::heartbeatTick() {
    if (state_.load() != State::Leader) return;
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        replicateLog(peer); // 旧版：sendHeartbeat(peer)
    }
}
```

新增 `replicateLog` 协程。来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§5 日志复制段）：

```cpp
FireAndForget RaftNode::replicateLog(Peer peer) {
    if (state_.load() != State::Leader) co_return;

    // 读取本次要发送的起始 index（loop_ 线程，nextIndex_ 无竞争）
    uint64_t ni = nextIndex_.count(peer.id) ? nextIndex_[peer.id] : lastLogIndex() + 1;
    if (ni < 1) ni = 1; // 安全下限（哨兵条目不发送）

    uint64_t prevIdx  = ni - 1;
    uint64_t prevTerm = log_[prevIdx].term;

    // 构造 AppendEntriesArgs：收集 [ni, lastLogIndex] 范围的条目
    AppendEntriesArgs args;
    args.term         = currentTerm_.load();
    args.leaderId     = id_;
    args.prevLogIndex = prevIdx;
    args.prevLogTerm  = prevTerm;
    args.leaderCommit = commitIndex_.load();
    for (uint64_t i = ni; i <= lastLogIndex(); ++i)
        args.entries.push_back(log_[i]);

    // ── 挂起点：发出 AppendEntries RPC ──────────────────────────────────
    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo(
        "AppendEntries", json(args).dump(), /*timeoutMs=*/100);

    // ── 恢复点（loop_ 线程）────────────────────────────────────────────
    if (!ok) co_return;                             // 超时/故障：下个 50ms 重试
    if (state_.load() != State::Leader) co_return;  // 期间失去了 Leader 身份

    AppendEntriesReply reply{};
    try {
        reply = json::parse(respJson).get<AppendEntriesReply>();
    } catch (...) { co_return; }

    if (reply.term > currentTerm_.load()) {
        becomeFollower(reply.term);  // 僵尸 Leader：立刻退位
        co_return;
    }

    if (reply.success) {
        // ── 成功：推进 matchIndex / nextIndex ───────────────────────────
        uint64_t newMatch = args.prevLogIndex + (uint64_t)args.entries.size();
        if (newMatch > matchIndex_[peer.id]) {
            matchIndex_[peer.id] = newMatch;
            nextIndex_[peer.id]  = newMatch + 1;
        }
        advanceCommitIndex();
    } else {
        // ── 失败（一致性检查不过）：用冲突提示加速回退 ─────────────────
        //
        // 朴素方式：nextIndex-- 直到匹配，最坏需要 O(log_length) 轮。
        // 优化方式（Raft 论文 §5.3 hint）：
        //   conflictTerm==0  → Follower 日志比 prevLogIndex 短，直接跳到 conflictIndex
        //   conflictTerm!=0  → 在 Leader 日志里找该 term 的最后一条；
        //                        如果 Leader 也没有该 term，跳到 conflictIndex。
        uint64_t newNext;
        if (reply.conflictTerm == 0) {
            newNext = reply.conflictIndex;
        } else {
            // 在 Leader 日志里找 conflictTerm 的最后一条
            int64_t found = -1;
            for (int64_t i = (int64_t)lastLogIndex(); i >= 1; --i) {
                if (log_[i].term == reply.conflictTerm) { found = i + 1; break; }
            }
            newNext = (found >= 0) ? (uint64_t)found : reply.conflictIndex;
        }
        // 只允许减小 nextIndex（不能因为并发成功回包而增大）
        if (newNext < nextIndex_[peer.id])
            nextIndex_[peer.id] = std::max(newNext, uint64_t{1});
    }
}
```

**关键细节**：

`ni = nextIndex_[peer.id]` 决定了本次发多少条目。`ni > lastLogIndex()` 时 entries 为空——这就是纯心跳。`ni <= lastLogIndex()` 时 entries 包含 `[ni, lastLogIndex]` 范围的条目。心跳和复制用同一套代码路径，大幅简化逻辑。

成功分支里 `newMatch = prevLogIndex + entries.size()`——这是 Follower 实际到达的最高 index。只有当 `newMatch > matchIndex_[peer.id]` 时才更新，防止乱序回包（并发飞行中的旧包比新包晚到）倒退 matchIndex。

失败分支的冲突加速回退：`conflictTerm != 0` 时，在 Leader 自己的日志里找该 term 的**最后一条**的下一位（`found = i + 1`），如果 Leader 也没有这个 term（说明 Follower 的日志里有一段 Leader 根本没有），直接跳到 `conflictIndex`。

---

## 5. 改进 D — advanceCommitIndex + applyCommitted

### 5.1 为什么要分层（committed vs applied）

**committed（commitIndex）**：多数派已写入日志 → 即使节点崩溃重启，这条命令不会丢失。

**applied（lastApplied）**：命令已被状态机执行 → 外部可查到效果。

为什么不合并？applied 可以比 committed 慢（状态机可能需要批量处理），但**绝不能比 committed 快**（未 committed 的命令不能执行，否则崩溃后可能被撤销，状态机状态与已应用命令不一致）。

**Figure 8 规则**（`advanceCommitIndex` 的核心约束）：

Leader 只能直接提交**当前 term** 的条目。旧 term 的条目只能通过"顺带"方式提交（commitIndex 单调递增，推进时自然带上旧 term 条目）。

这条规则解决了一个微妙的安全漏洞：假设旧 Leader A 在 term=2 将条目 X 复制给了部分节点，然后崩溃。新 Leader B 在 term=3 上任，如果 B 直接提交 term=2 的条目 X，可能会覆盖另一个分区里已经在 term=3 提交的数据。解法是 B 不直接为旧 term 条目统计多数派——只有当 B 在 term=3 提交了一条属于当前 term 的条目后，commitIndex 推过 term=2 条目所在的位置，旧条目才随之被顺带提交。

### 5.2 编码实现步骤

**第六步：修改 `src/common/raft/RaftNode.cpp`，新增 `advanceCommitIndex` 和 `applyCommitted`**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§6 提交推进段）：

```cpp
void RaftNode::advanceCommitIndex() {
    // Raft Figure 8 规则：Leader 只能直接提交「当前 term」的条目。
    // 旧 term 的条目会随着新条目的提交「顺带」被提交（commitIndex 单调递增）。
    // 若允许提交旧 term 条目，会破坏安全性（参见 Raft 论文 Figure 8 的反例）。
    uint64_t lastIdx = lastLogIndex();
    for (uint64_t n = lastIdx; n > commitIndex_.load(); --n) {
        if (log_[n].term != currentTerm_.load()) continue; // Figure 8 过滤
        int count = 1; // 算上自己
        for (const auto &peer : peers_) {
            if (peer.id == id_) continue;
            if (matchIndex_.count(peer.id) && matchIndex_[peer.id] >= n) ++count;
        }
        if (count >= quorum_) {
            commitIndex_.store(n);
            LOG_INFO << "[Node " << id_ << "] 提交进度推进到 index=" << n
                     << "（任期=" << log_[n].term << " 命令=" << log_[n].cmd << ")";
            applyCommitted();
            break; // 找到最大可提交 N 后即停（更小的 n 在下轮自然覆盖）
        }
    }
}

void RaftNode::applyCommitted() {
    // 把 [lastApplied+1, commitIndex] 范围的条目逐条应用到状态机。
    // applyCallback_ 在 loop_ 线程回调 → 状态机代码天然单线程，无需加锁。
    while (lastApplied_.load() < commitIndex_.load()) {
        uint64_t idx = lastApplied_.load() + 1;
        if (idx >= log_.size()) break; // 防御：不应发生
        lastApplied_.store(idx);
        LOG_INFO << "[Node " << id_ << "] 应用日志 index=" << idx
                 << " 命令=" << log_[idx].cmd;
        if (applyCallback_) applyCallback_(idx, log_[idx].cmd);
    }
}
```

`advanceCommitIndex` 从最新 index 向前遍历，找到满足"当前 term + 多数派 matchIndex >= N"的最大 N，更新 commitIndex。从后往前遍历是因为 commitIndex 单调递增，找到最大满足条件的 N 就够了（更小的 N 在后续轮次自然被覆盖）。

`applyCommitted` 无条件应用 `[lastApplied+1, commitIndex]` 区间内的每一条已提交条目：逐条递增 `lastApplied_`，并把命令交给 `applyCallback_`。它对所有节点（Leader/Follower）共用，保证状态机看到的命令顺序与 commitIndex 推进顺序完全一致。

**Follower 侧的 `applyCommitted` 调用时机**：`handleAppendEntries` 规则④ 推进 commitIndex 后立刻调 `applyCommitted`。Leader 侧的 `applyCommitted` 由 `advanceCommitIndex` 在 quorum 达成后调用。**两者共用同一个函数**，保持逻辑一致。

---

## 6. 改进 E — propose() 外部写入接口

### 6.1 为什么需要这一步

外部线程（HTTP 服务器、测试代码、raft_demo 主循环）需要向 Raft 集群写入命令。但 Raft 状态（`log_`、`nextIndex_` 等）只能在 `loop_` 线程修改。`propose()` 通过 `runInLoop` 跨线程安全投递。

### 6.2 编码实现步骤

**第七步：修改 `src/common/raft/RaftNode.cpp`，新增 `propose`**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§7 外部写入接口段）：

```cpp
void RaftNode::propose(const std::string &cmd) {
    // propose 可以从任意线程调用（线程安全）。
    // 通过 runInLoop 把实际追加操作投递到 loop_ 线程，维持 Raft 状态的单线程访问。
    loop_.runInLoop([this, cmd] {
        if (state_.load() != State::Leader) {
            LOG_WARN << "[Node " << id_ << "] 提案被拒绝：当前节点非领导者"
                     << "（当前领导者ID=" << leaderId_ << ")";
            return;
        }
        // 追加到本地日志（Leader 自己的那份），term = currentTerm
        log_.push_back(LogEntry{currentTerm_.load(), cmd});
        LOG_INFO << "[Node " << id_ << "] 追加日志条目 index=" << lastLogIndex()
                 << " 命令=" << cmd;
        // 立刻触发一轮复制（不等下一个 50ms heartbeat 周期）
        for (const auto &peer : peers_) {
            if (peer.id == id_) continue;
            replicateLog(peer);
        }
    });
}
```

`runInLoop` 保证 `log_.push_back` 和 `replicateLog` 的发射都在 `loop_` 线程执行。非 Leader 节点直接返回——真实系统应转发给 Leader，此处只打 WARN（day36 的 `proposeAndNotify` 会解决这个问题）。

`propose` 后立刻触发 `replicateLog`（而不是等 50ms 的 heartbeatTick）：减少提交延迟，命令写入后通常在 1~2 个 RTT（约 1ms 本机回环）内即可提交。

---

## 7. 改进 F — raft_demo 更新

### 7.1 为什么需要更新

day32 的 raft_demo 只展示 state/term，看不出日志复制的效果。需要：
1. 状态行增加 `logSize/commit/applied` 让复制进展可见
2. `--propose-interval` 让 Leader 自动定期写入命令，方便观察
3. 注册 `setApplyCallback` 打印每条被 apply 的命令

### 7.2 编码实现步骤

**修改 `examples/src/raft_demo.cpp`，替换为以下全部内容**

来自 [examples/src/raft_demo.cpp](examples/src/raft_demo.cpp)：

```cpp
// raft_demo.cpp —— Raft 选举 + 日志复制演示（最多 10 节点）
//
// 用法（N 节点集群，默认 3）：
//   ./raft_demo --id 0 [--nodes N] [--propose-interval <ms>]
//   ./raft_demo --id 1 [--nodes N]
//   ...
//
// Day33 新增：
//   --propose-interval <ms>   Leader 自动 propose 命令的间隔（默认 2000ms，0=不自动提交）

#include "log/Logger.h"
#include "net/SignalHandler.h"
#include "raft/RaftNode.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    int  myId              = -1;
    int  nodes             = 3;
    int  proposeIntervalMs = 2000;

    for (int i = 1; i < argc; ++i) {
        if (i + 1 < argc) {
            if (std::strcmp(argv[i], "--id") == 0)
                myId = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--nodes") == 0)
                nodes = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--propose-interval") == 0)
                proposeIntervalMs = std::stoi(argv[i + 1]);
        }
    }

    if (nodes < 2 || nodes > 10) {
        std::cerr << "错误：--nodes 必须在 2~10 范围内\n";
        return 1;
    }
    if (myId < 0 || myId >= nodes) {
        std::cerr << "用法：raft_demo --id <0.." << (nodes - 1)
                  << "> [--nodes " << nodes << "] [--propose-interval <ms>]\n";
        return 1;
    }

    Logger::setLogLevel(Logger::INFO);

    const std::vector<raft::Peer> allPeers = {
        {0, "127.0.0.1", 18901}, {1, "127.0.0.1", 18902}, {2, "127.0.0.1", 18903},
        {3, "127.0.0.1", 18904}, {4, "127.0.0.1", 18905}, {5, "127.0.0.1", 18906},
        {6, "127.0.0.1", 18907}, {7, "127.0.0.1", 18908}, {8, "127.0.0.1", 18909},
        {9, "127.0.0.1", 18910},
    };

    std::vector<raft::Peer> peers(allPeers.begin(), allPeers.begin() + nodes);
    uint16_t myPort = peers[myId].port;

    raft::RaftNode node(myId, peers, myPort);

    // 注册状态机回调：每条提交的命令都打印一行
    node.setApplyCallback([myId](uint64_t index, const std::string &cmd) {
        std::cout << "[Node " << myId << "] ✓ APPLIED  index=" << index
                  << "  cmd=" << cmd << "\n" << std::flush;
    });

    static std::atomic<bool> stopFlag{false};
    Signal::signal(SIGINT,  [] { stopFlag.store(true); });
    Signal::signal(SIGTERM, [] { stopFlag.store(true); });

    node.start();

    // ── 主循环：状态展示 + 自动 propose ────────────────────────────────
    int  proposeCounter = 0;
    auto lastPropose    = std::chrono::steady_clock::now();

    while (!stopFlag.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const char *stateStr = "Follower";
        if (node.getState() == raft::State::Leader)    stateStr = "LEADER";
        if (node.getState() == raft::State::Candidate) stateStr = "Candidate";

        std::cout << "[Node " << myId << "] "
                  << stateStr
                  << "  term="    << node.getCurrentTerm()
                  << "  logSize=" << (node.getLastLogIndex() + 1)
                  << "  commit="  << node.getCommitIndex()
                  << "  applied=" << node.getLastApplied()
                  << "  leaderId="<< node.getLeaderId()
                  << "\n" << std::flush;

        // 自动 propose：仅当自己是 Leader 且开启了 propose
        if (proposeIntervalMs > 0 && node.isLeader()) {
            auto now     = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastPropose).count();
            if (elapsed >= proposeIntervalMs) {
                lastPropose = now;
                std::string cmd = "cmd-" + std::to_string(proposeCounter++);
                std::cout << "[Node " << myId << "] → propose \"" << cmd << "\"\n"
                          << std::flush;
                node.propose(cmd);
            }
        }
    }

    node.stop();
    return 0;
}
```

---

## 8. 整体运行时理解

### 8.1 对象所有权与线程归属

```
RaftNode（T_main 构造，loopThread_ 运行）
 │
 └── loop_（T_raft）
      ├── log_[]                ← Raft 日志（只在 T_raft 读写）
      ├── commitIndex_（atomic） 任意线程可读
      ├── lastApplied_（atomic） 任意线程可读
      ├── nextIndex_/matchIndex_ 只在 T_raft 读写（Leader 专用）
      ├── applyCallback_         只在 T_raft 调用
      │
      ├── replicateLog 协程帧（堆，per-peer）
      │    └── AppendEntriesArgs（args），prevIdx/prevTerm/entries/leaderCommit
      │
      └── peerClients_[N]→AsyncRpcClient
           └── pending_[reqId]→cb → 协程 handle

 外部线程（T_main / HTTP / 测试）
      └── propose(cmd) ──runInLoop──▶ T_raft
                                       log_.push_back()
                                       replicateLog(peer0,1,2)
```

**两个 apply 路径**：
- **Leader 侧**：`advanceCommitIndex()` 在 `replicateLog` 成功分支调用 → `applyCommitted()`
- **Follower 侧**：`handleAppendEntries` 规则④ 推进 commitIndex → `applyCommitted()`

两者共用 `applyCommitted()`，逻辑一致。

---

### 8.2 场景 A — 一条命令从 propose 到 applied 的完整路径

**场景设定**：3 节点集群，Node 0 是 Leader（term=1），`commitIndex=0, lastApplied=0`，log_ 只有哨兵（index=0）。外部调用 `node.propose("SET foo bar")`。

---

#### 第 1 步：propose 切入 loop_ 线程，追加日志

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`propose`：

```cpp
loop_.runInLoop([this, cmd="SET foo bar"] {
    // 现在在 T_raft
    log_.push_back(LogEntry{currentTerm_.load()=1, "SET foo bar"});
    // log_[1] = {term=1, cmd="SET foo bar"}
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        replicateLog(peer);  // 为 peer 1 和 peer 2 各发射协程
    }
});
```

**此刻状态快照**：
```
log_             = [{t=0,""}, {t=1,"SET foo bar"}]  (index 0-1)
nextIndex_       = {1:1, 2:1}  (Leader 上任时初始化为 lastLogIndex+1=1)
matchIndex_      = {1:0, 2:0}
commitIndex_     = 0
lastApplied_     = 0
```

---

#### 第 2 步：replicateLog 协程挂起，发出 AppendEntries

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`replicateLog`（以 peer 1 为例）：

```cpp
FireAndForget RaftNode::replicateLog(Peer peer) {
    // ni = nextIndex_[1] = 1（lastLogIndex=1，"SET foo bar" 这一条要发）
    uint64_t ni      = 1;
    uint64_t prevIdx  = 0;
    uint64_t prevTerm = log_[0].term = 0;  // 哨兵

    AppendEntriesArgs args;
    args.term         = 1;
    args.prevLogIndex = 0;
    args.prevLogTerm  = 0;
    args.leaderCommit = 0;
    args.entries      = [{t=1,"SET foo bar"}];  // [ni, lastLogIndex] = [1, 1]
```

`ni=1` 时发 entries=`[log_[1]]`，也就是刚 propose 的 "SET foo bar"。prevLogIndex=0 指向哨兵，Follower 用它做前缀检查。

```cpp
    // ── 挂起点 ──────────────────────────────────────────────────────────
    auto [ok, respJson] = co_await getOrCreateClient(peer1)->callAsyncCo(
        "AppendEntries", json(args).dump(), 100);
    // 协程挂起，loop_ 继续处理其他事件（peer 2 的协程、心跳等）
```

**此刻状态快照**：
```
replicateLog 协程（peer 1）= 挂起于 co_await
replicateLog 协程（peer 2）= 同时挂起于 co_await（并发）
```

---

#### 第 3 步：Follower（Node 1）处理 AppendEntries，四规则执行

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，Node 1 的 `handleAppendEntries`（T_raft，Node 1）：

```cpp
loop_.runInLoop([this, args, done]() mutable {
    // 规则①：args.term=1 >= currentTerm_=1 ✓，不拒绝
    // 规则②：args.prevLogIndex=0，log_[0].term=0 == args.prevLogTerm=0 ✓
    // 规则③：
    //   log_[1] 不存在 → log_.push_back({t=1,"SET foo bar"})
    // 规则④：args.leaderCommit=0 <= commitIndex_=0，不触发 apply
    reply.success = true;
    done(json(reply).dump());
});
```

**此刻状态快照（Node 1）**：
```
log_         = [{t=0,""}, {t=1,"SET foo bar"}]
commitIndex_ = 0
lastApplied_ = 0
```

---

#### 第 4 步：replicateLog 协程恢复，advanceCommitIndex

Node 1 的响应回到 Node 0 的协程（T_raft，Node 0）：

```cpp
// ── 恢复点 ──────────────────────────────────────────────────────────
reply.success = true;
uint64_t newMatch = 0 + 1 = 1;  // prevLogIndex + entries.size()
matchIndex_[1] = 1;
nextIndex_[1]  = 2;
advanceCommitIndex();
```

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`advanceCommitIndex`：

```cpp
// 从 n=1 向前遍历
// n=1: log_[1].term=1 == currentTerm_=1 ✓（Figure 8 过滤通过）
//   count=1（Leader 自己）
//   peer 1: matchIndex_[1]=1 >= 1 → count=2
//   peer 2: 假设 matchIndex_[2]=1 → count=3（或 2 就已达 quorum）
//   count=2 >= quorum_=2 → commitIndex_.store(1)
commitIndex_.store(1);
applyCommitted();
```

---

#### 第 5 步：applyCommitted 触发状态机

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`applyCommitted`（Node 0，T_raft）：

```cpp
// lastApplied_=0 < commitIndex_=1，进入循环
// idx=1：log_[1].cmd="SET foo bar"，调 applyCallback_(1, "SET foo bar")
//   → raft_demo 打印：[Node 0] ✓ APPLIED  index=1  cmd=SET foo bar
//   lastApplied_.store(1)
// lastApplied_=1 == commitIndex_=1，退出循环
```

**此刻状态快照（Node 0）**：
```
commitIndex_  = 1
lastApplied_  = 1
applyCallback_ 已调用，外部状态机已执行 "SET foo bar"
```

---

#### 第 6 步：Follower 侧由下一次 heartbeat 追上 commitIndex

Leader 下一次 `heartbeatTick` 发出的 AppendEntries 会携带 `leaderCommit=1`。Follower 的 `handleAppendEntries` 规则④：

```cpp
if (args.leaderCommit=1 > commitIndex_=0) {
    commitIndex_.store(min(1, lastLogIndex()=1) = 1);
    applyCommitted();
    // → APPLIED index=1 cmd=SET foo bar（Follower 侧）
}
```

---

#### 调用链总结图

```
T_main/外部                  T_raft（Node0）              T_raft（Node1）
    │                           │                            │
    │ propose("SET foo bar")     │                            │
    │ runInLoop ─────────────────▶                            │
    │                           │ log_.push_back(idx=1)      │
    │                           │ replicateLog(peer1) ────────TCP→
    │                           │ co_await[挂起]             │ handleAppendEntries
    │                           │                            │ 四规则→追加
    │                           │ ◀───success────────────────│
    │                           │ matchIndex_[1]=1           │
    │                           │ advanceCommitIndex()       │
    │                           │ commitIndex_=1             │
    │                           │ applyCommitted()           │
    │                           │ applyCallback_(1,"SET...")  │
    │ ✓ APPLIED (打印) ◀─────────│                            │
    │                           │ [下次心跳 leaderCommit=1]─────▶
    │                           │                            │ commitIndex_=1
    │                           │                            │ applyCommitted()
    │                           │                            │ ✓ APPLIED（Follower）
```

---

### 8.3 场景 B — Follower 落后，冲突加速回退

**场景设定**：Node 1 宕机重启后只有 log_ = [{哨兵}, {t=1,旧cmd}]（index=0,1），Leader 的 log_ 有 5 条（index=0-4），nextIndex_[1]=5（乐观估计）。

---

#### 第 1 步：replicateLog 发出（prevIdx=4, entries=[]）

Leader 发 `prevLogIndex=4, prevLogTerm=log_[4].term=1`，entries 为空（nextIndex[1]=5=lastLogIndex+1）。

---

#### 第 2 步：Follower 回复 success=false

Node 1 的 `handleAppendEntries` 规则②：

```cpp
// args.prevLogIndex=4 > lastLogIndex()=1
reply.conflictIndex = 2;  // lastLogIndex()+1
reply.conflictTerm  = 0;  // 日志太短，用 0
done(json(reply).dump());
```

---

#### 第 3 步：replicateLog 协程恢复，快速回退

```cpp
// reply.conflictTerm == 0 → newNext = reply.conflictIndex = 2
// newNext=2 < nextIndex_[1]=5 → nextIndex_[1] = 2
nextIndex_[1] = 2;
// 下次 replicateLog 从 ni=2 开始发，entries=[log_[2],log_[3],log_[4]]
```

一步从 nextIndex=5 跳回 nextIndex=2，跳过了 3 次无效的 `nextIndex--` 尝试。如果 Follower 的日志是 term 不匹配（而不是太短），`conflictTerm != 0`，Leader 还可以进一步跳过整个冲突 term 段，通常只需 1~2 轮就能收敛。

---

## 9. 各模块职责速查表

| 模块/函数 | 所在线程 | 调用时机 | 职责一句话 |
|-----------|---------|---------|-----------|
| `handleAppendEntries`（Follower） | T_raft_rpc → T_raft | 收到 AE 帧时 | 四规则：过期 term/前缀检查/幂等追加/推进 commitIndex |
| `becomeLeader`（Leader）| T_raft | 票数达 quorum 时 | 初始化 nextIndex_/matchIndex_；立刻心跳 |
| `heartbeatTick` | T_raft | runEvery(50ms) | 仅 Leader：为每个 peer 发射 replicateLog 协程 |
| `replicateLog` | T_raft（协程帧在堆）| heartbeatTick / propose | 挂起于 AE callAsyncCo → 恢复后处理成功/失败，更新 nextIndex_/matchIndex_ |
| `advanceCommitIndex` | T_raft | replicateLog 成功分支 | Figure 8 过滤 → 多数派统计 → 推进 commitIndex → applyCommitted |
| `applyCommitted` | T_raft | advanceCommitIndex / handleAppendEntries 规则④ | 顺序应用 [lastApplied+1, commitIndex] 的条目，逐条调 applyCallback_ |
| `propose` | 任意线程 → T_raft | 外部写入时 | runInLoop 切回 T_raft → log_.push_back → replicateLog |
| `applyCallback_` | T_raft（由 applyCommitted 调用）| 每条命令被 apply 时 | 用户注册的状态机回调（KV 存储、打印等）|

---

## 10. 工程化

`RaftNode.h` 新增以下 getter（用于 raft_demo 状态行展示，读取 atomic 值，无需进入 loop_ 线程）：

```cpp
uint64_t getCommitIndex()  const { return commitIndex_.load(); }
uint64_t getLastApplied()  const { return lastApplied_.load(); }
int      getLeaderId()     const { return leaderId_; }  // -1 = 未知
uint64_t getLastLogIndex() const {
    // 仅近似值（loop_ 线程外读 log_ 可能不精确），仅供展示用
    return static_cast<uint64_t>(log_.size()) - 1;
}
```

`getLeaderId()` 没有 atomic——`int` 读取在 x86 上是原子的（注释"仅近似值"），偶尔读到旧值对状态展示无害。

---

## 11. 验证

```bash
cmake --build build --target raft_demo -j

# 3 节点集群，每 500ms propose 一条命令
./build/examples/raft_demo --id 0 --nodes 3 --propose-interval 500 &
./build/examples/raft_demo --id 1 --nodes 3 &
./build/examples/raft_demo --id 2 --nodes 3 &
```

预期输出（约 2s 后，Leader 节点）：

```
[Node 0] *** 成为领导者，任期=1 ***
[Node 0] → propose "cmd-0"
[Node 0] 追加日志条目 index=1 命令=cmd-0
[Node 0] 提交进度推进到 index=1（任期=1 命令=cmd-0)
[Node 0] ✓ APPLIED  index=1  cmd=cmd-0
...
[Node 0] LEADER  term=1  logSize=5  commit=4  applied=4  leaderId=0
[Node 1] Follower  term=1  logSize=5  commit=4  applied=4  leaderId=0
[Node 2] Follower  term=1  logSize=5  commit=4  applied=4  leaderId=0
```

`commit` 和 `applied` 在所有节点同步递增，Follower 最多滞后 1 个心跳周期（50ms）。`logSize` 比实际命令数多 1（含哨兵），所以 propose 4 条命令后 logSize=5。

Kill 某个 Follower 后集群继续运行（3 节点 quorum=2，一个宕机仍能工作）。Kill Leader 后约 300ms 内重新选出 Leader，日志复制在新 Leader 上任后继续。

---

## 12. 局限与下一步

| 局限 | Day34 的解法 |
|------|-------------|
| **纯内存，重启后日志丢失** | 引入 `RaftStorage` 接口 + `FileStorage`/`RocksDBStorage` 持久化，重启自动恢复 term/votedFor/log |
| **日志无上限**：运行时间长后内存无限增长 | 引入 `takeSnapshot()` + `InstallSnapshot` RPC，用快照压缩已 apply 的日志段 |
| **`propose()` fire-and-forget**：调用方不知道何时 apply | Day36 引入 `proposeAndNotify(cmd, done)`，apply 后回调 `done(true, logIndex)` |
| **单节点集群直接 commit**：`quorum=1` 时 `advanceCommitIndex` 在 `propose` 后立刻提交，无网络往返，但逻辑路径和多节点相同，已正确处理 | — |

接下来 **Day34** 将实现持久化：新增 `RaftStorage` 抽象接口（纯虚类）+ `FileStorage`（POSIX 原子 rename 崩溃安全写）+ `RocksDBStorage`（可选），`RaftNode::start()` 在 loop_ 线程内恢复 term/votedFor/log/snapshot，崩溃重启后能从上次 commitIndex 继续运行。
