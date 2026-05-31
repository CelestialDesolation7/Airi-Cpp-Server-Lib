// RaftNode.cpp —— Raft 共识节点（选举 + 日志复制 + 持久化 + 快照 + C++20 协程）
//
// 阅读顺序：
//   §1 构造 / 析构 / start / stop      —— 线程编排与生命周期（含持久化恢复）
//   §2 RPC server 回调（fire-and-forget）—— AppendEntries / InstallSnapshot
//   §3 角色切换 + 选举定时器           —— 全部在 loop_ 线程，无锁
//   §4 选举：runElection + collectVote 协程
//   §5 日志复制 + 心跳：heartbeatTick + replicateLog + sendInstallSnapshot 协程
//   §6 提交推进：advanceCommitIndex + applyCommitted
//   §7 外部写入接口：propose + takeSnapshot
//   §8 辅助：lastLogIndex / lastLogTerm / logAt / persistHardState / persistLog
//
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
      // Raft 安全性要求：超过半数节点同意才能做决定（选举/提交日志）。
      // 3 节点 → quorum=2；5 节点 → quorum=3。即使一个节点宕机仍可运作。
      quorum_(static_cast<int>(peers_.size()) / 2 + 1),
      rng_(std::random_device{}()),
      rpcServer_("0.0.0.0", rpcPort, /*ioThreads=*/1) {
    // 哨兵条目：让 lastLogIndex() 和 lastLogTerm() 在日志为空时也能安全返回。
    // log_[0].term = 0，任何真实日志条目的 term >= 1，不会与哨兵混淆。
    log_.push_back(LogEntry{0, ""});

    // 注册异步 handler：handler 立刻 return，由 done 异步回写响应
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
    rpcServer_.addHandler(
        "InstallSnapshot",
        [this](const std::string &req, RpcServer::Done done) {
            handleInstallSnapshot(req, std::move(done));
        });
}

RaftNode::~RaftNode() { stop(); }

void RaftNode::start() {
    if (running_.exchange(true)) return; // 幂等

    // (a) Eventloop 线程：所有 Raft 状态变迁 + 出站 RPC IO 都在这条线程上
    loopThread_ = std::thread([this] {
        loop_.runInLoop([this] {
            // ── 持久化恢复（必须在 loop_ 线程，确保无竞争）──────────────
            if (storage_) {
                // ① 恢复 HardState：term + votedFor
                auto hs = storage_->loadHardState();
                currentTerm_.store(hs.term);
                votedFor_ = hs.votedFor;

                // ② 恢复快照：重设哨兵并推进 commitIndex/lastApplied
                uint64_t snapIdx{0}, snapTerm{0};
                std::string snapData;
                if (storage_->loadSnapshot(snapIdx, snapTerm, snapData)) {
                    snapshotIndex_ = snapIdx;
                    snapshotTerm_  = snapTerm;
                    snapshotData_  = snapData;
                    log_.clear();
                    log_.push_back(LogEntry{snapTerm, ""}); // 新哨兵
                    commitIndex_.store(snapIdx);
                    lastApplied_.store(snapIdx);
                }

                // ③ 恢复快照点之后的日志条目
                auto entries = storage_->loadLog();
                log_.insert(log_.end(), entries.begin(), entries.end());

                LOG_INFO << "[Node " << id_ << "] 从持久化存储恢复："
                         << " term=" << currentTerm_.load()
                         << " votedFor=" << votedFor_
                         << " snapshotIndex=" << snapshotIndex_
                         << " logSize=" << (log_.size() - 1) << " 条";
            }
            resetElectionTimer();
        });
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
//
// RpcServer 在自己的 sub-reactor 线程上回调 handle*()。我们把"读写 Raft 状态
// + 生成响应"封装成 lambda 投递到 loop_，**不等结果**直接返回。
// 响应通过 done() 异步写回——sub-reactor 线程立刻可以处理下一个请求，
// 永远不会被同步阻塞，单 IO 线程也能处理任意并发的 inbound RPC。
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
        // 【Raft 规则 §5.1】任何 RPC 只要携带更高 term，接收方必须立刻退回 Follower。
        // 这保证了任期单调递增——旧 Leader 看到新 term 后不再自以为是 Leader。
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        bool grant = false;
        // 投票需同时满足三个条件（全部满足才投，任一不满足就拒绝）：
        if (
            // 条件①：候选人的 term 不小于我的 term。
            // term 相等（= 是候选人已在本 term 发起选举，我在同 term 还没投票）也可以。
            // 若 term < currentTerm_，说明候选人信息过时，直接拒绝。
            args.term >= currentTerm_.load() &&

            // 条件②：本任期内我还没投过票，或者我之前就已经投给了这个候选人。
            // 保证同一 term 内每个节点最多只投一票，防止选出两个 Leader。
            (votedFor_ == -1 || votedFor_ == args.candidateId) &&

            // 条件③：候选人的日志至少和我一样「新」（Raft 的选举安全性）。
            // 比较规则：先比最后一条日志的 term，term 更大的更新；
            //           term 相同时，日志更长的（index 更大）更新。
            // 目的：确保当选的 Leader 一定包含所有已提交的日志，不会丢数据。
            (args.lastLogTerm > lastLogTerm() ||
             (args.lastLogTerm == lastLogTerm() && args.lastLogIndex >= lastLogIndex()))
        ) {
            grant     = true;
            votedFor_ = args.candidateId;  // 记录投票，本 term 内不再投给其他人
            // 收到合法投票请求后重置自己的选举计时器：
            // 既然已经有候选人在运作，就不要再发起竞争选举，让它先跑完。
            resetElectionTimer();
            persistHardState();  // votedFor 变化必须立即落盘
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
        //
        // 这是 Raft 的核心安全机制：Leader 通过「前缀匹配」保证 Follower 和自己历史一致。
        // 若这里不匹配，Follower 无法安全追加 entries，必须拒绝并给出冲突提示。

        // 快照点之前的条目已压缩，Leader 不应该再发 prevLogIndex < snapshotIndex_，
        // 但防御性处理：视为已一致（Leader 应已发过 InstallSnapshot 覆盖了这部分）。
        if (args.prevLogIndex < snapshotIndex_) {
            reply.success = true;
            done(json(reply).dump());
            return;
        }
        if (args.prevLogIndex > lastLogIndex()) {
            // Follower 日志太短，根本没有 prevLogIndex 处的条目
            reply.conflictIndex = lastLogIndex() + 1;  // 告知 Leader 从这里开始重发
            reply.conflictTerm  = 0;                   // 0 = 该位置不存在
            done(json(reply).dump());
            return;
        }
        if (logAt(args.prevLogIndex).term != args.prevLogTerm) {
            // prevLogIndex 处 term 不匹配：找到该冲突 term 在我这里的第一条 index，
            // 让 Leader 跳过整个冲突 term（比逐条 -1 快得多）
            uint64_t ct = logAt(args.prevLogIndex).term;
            uint64_t ci = args.prevLogIndex;
            // 不能回退到快照点之前（那部分已压缩，log_ 里没有）
            while (ci > snapshotIndex_ && logAt(ci - 1).term == ct) --ci;
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
            if (logIdx <= lastLogIndex()) {
                if (logAt(logIdx).term != args.entries[i].term) {
                    log_.resize(logIdx - snapshotIndex_); // 截断冲突点之后的所有条目
                    log_.push_back(args.entries[i]);
                }
                // else：term 相同 = 已有该条目（重传），跳过
            } else {
                log_.push_back(args.entries[i]); // 追加新条目
            }
        }
        // 日志变更后落盘（全量覆盖写）
        persistLog();

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

// ════════════════════════════════════════════════════════════════════════════
// §2b  InstallSnapshot handler
//
// 当 Follower 严重落后（nextIndex <= snapshotIndex_），Leader 改发快照。
// Follower 用快照替换本地日志前缀，保留快照点之后已有的条目，
// 并更新 commitIndex/lastApplied 到快照点（无需逐条 apply）。
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::handleInstallSnapshot(const std::string &reqJson, RpcServer::Done done) {
    InstallSnapshotArgs args;
    try {
        args = json::parse(reqJson).get<InstallSnapshotArgs>();
    } catch (...) {
        done(R"({"term":0})");
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        InstallSnapshotReply reply{currentTerm_.load()};
        if (args.term < currentTerm_.load()) {
            done(json(reply).dump());
            return;
        }

        state_.store(State::Follower);
        leaderId_ = args.leaderId;
        resetElectionTimer();

        // 快照比我现有的更旧：忽略（避免状态倒退）
        if (args.lastIncludedIndex <= snapshotIndex_) {
            done(json(reply).dump());
            return;
        }

        LOG_INFO << "[Node " << id_ << "] 安装快照 lastIndex=" << args.lastIncludedIndex
                 << " lastTerm=" << args.lastIncludedTerm;

        // 保留快照点之后我已有的日志条目（避免丢弃已知的更新条目）
        std::vector<LogEntry> retained;
        if (args.lastIncludedIndex < lastLogIndex()) {
            // 从旧 log_ 中截取快照点之后的部分
            uint64_t keepFrom = args.lastIncludedIndex - snapshotIndex_ + 1; // 局部下标
            if (keepFrom < log_.size())
                retained = std::vector<LogEntry>(log_.begin() + keepFrom, log_.end());
        }

        // 重建 log_：新哨兵（term = lastIncludedTerm）+ 保留条目
        log_.clear();
        log_.push_back(LogEntry{args.lastIncludedTerm, ""});
        log_.insert(log_.end(), retained.begin(), retained.end());

        snapshotIndex_ = args.lastIncludedIndex;
        snapshotTerm_  = args.lastIncludedTerm;
        snapshotData_  = args.data;

        if (args.lastIncludedIndex > commitIndex_.load())
            commitIndex_.store(args.lastIncludedIndex);
        if (args.lastIncludedIndex > lastApplied_.load())
            lastApplied_.store(args.lastIncludedIndex);

        // 持久化
        if (storage_) {
            storage_->saveSnapshot(snapshotIndex_, snapshotTerm_, snapshotData_);
            persistLog();
            persistHardState();
        }

        done(json(reply).dump());
    });
}

// ════════════════════════════════════════════════════════════════════════════
// §3  角色切换 + 选举定时器（必须在 loop_ 线程调用）
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::becomeFollower(uint64_t term) {
    if (state_.load() != State::Follower)
        LOG_INFO << "[Node " << id_ << "] " << stateName(state_.load()) << " → 跟随者（任期="
                 << term << "）";
    state_.store(State::Follower);
    currentTerm_.store(term);
    votedFor_ = -1;
    persistHardState();  // term 变化必须立即落盘
    resetElectionTimer();
}

void RaftNode::becomeCandidate() {
    // 【Raft 规则 §5.2】发起新选举时必须先递增自己的 term。
    // 这防止旧的投票响应（来自网络延迟）污染新一轮选举：
    // 旧回包的 term 不等于新 term，会在 onVoteReply 里被过滤掉。
    currentTerm_.store(currentTerm_.load() + 1);
    state_.store(State::Candidate);
    votedFor_             = id_;  // 候选人给自己投一票（Raft 允许自投）
    currentElectionVotes_ = 1;    // 票数从 1 开始（已含自己那票）
    LOG_INFO << "[Node " << id_ << "] 选举超时 → 候选人，任期=" << currentTerm_.load();
    persistHardState();  // term++ 和 votedFor=self 必须同步落盘
    // 重置选举计时器：如果这一轮在随机超时内没选出 Leader（平票/网络分区），
    // 计时器到期后会自动发起下一轮选举（term 再递增）。随机超时使平票概率极低。
    resetElectionTimer();
}

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

void RaftNode::resetElectionTimer() {
    // EventLoop 的定时器无法主动取消，用「版本号」实现软取消：
    // 每次 reset 都递增 epoch，旧定时器触发时发现 epoch 不匹配就自动放弃。
    ++electionEpoch_;
    uint64_t myEpoch = electionEpoch_;  // 捕获当前 epoch 进 lambda
    // 随机化超时（150~300ms）是 Raft 避免同时选举的关键机制：
    // 如果所有节点超时相同，它们会在同一时刻都发起选举，导致无休止的平票。
    // 随机化后，最快超时的节点抢先发起，其他节点收到它的 RequestVote 后重置计时器，
    // 大概率只有一个节点真正进入 Candidate 状态。
    int timeoutMs = std::uniform_int_distribution<int>(150, 300)(rng_);
    loop_.runAfter(timeoutMs / 1000.0,
                   [this, myEpoch] { electionTimerFired(myEpoch); });
}

void RaftNode::electionTimerFired(uint64_t epoch) {
    // 守卫①：epoch 不匹配 = 这个 timer 已被 resetElectionTimer 「覆盖」，是旧的，直接丢弃。
    // 常见场景：收到 Leader 心跳后 resetElectionTimer，旧 timer 延迟触发 → 无效。
    if (epoch != electionEpoch_) return;
    // 守卫②：如果自己已经是 Leader，不应再发起选举（正常不会走到这里，双重保险）。
    if (state_.load() == State::Leader) return;
    // 选举超时 = Leader 失联（可能宕机或网络分区）。转为候选人，发起新一轮选举。
    becomeCandidate();
    runElection(); // 为每个 peer 并发发射 collectVote 协程
}

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
                 << " 的选票（已得票=" << currentElectionVotes_ << "/" << quorum_ << "，需要=" << quorum_ << "）";
        if (currentElectionVotes_ >= quorum_) becomeLeader();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §5  日志复制 + 心跳合一：heartbeatTick + replicateLog 协程
//
// Day33 核心改动：sendHeartbeat（只发空 AppendEntries）→ replicateLog
//   - entries 为空时 = 纯心跳（维持 Leader 存在感）
//   - entries 非空时 = 日志复制（携带 [nextIndex, lastLogIndex] 段）
// 两种情况用同一条 RPC，处理逻辑完全统一。
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::heartbeatTick() {
    if (state_.load() != State::Leader) return;
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        replicateLog(peer);
    }
}

FireAndForget RaftNode::replicateLog(Peer peer) {
    if (state_.load() != State::Leader) co_return;

    // 读取本次要发送的起始 index（loop_ 线程，nextIndex_ 无竞争）
    uint64_t ni = nextIndex_.count(peer.id) ? nextIndex_[peer.id] : lastLogIndex() + 1;
    if (ni < 1) ni = 1; // 安全下限（哨兵条目不发送）

    // Day34：若 peer 需要的条目已被快照压缩，改发 InstallSnapshot
    if (ni <= snapshotIndex_) {
        sendInstallSnapshot(peer);
        co_return;
    }

    uint64_t prevIdx  = ni - 1;
    // prevIdx 一定 >= snapshotIndex_（上面已保证 ni > snapshotIndex_），
    // logAt(prevIdx) 安全访问 log_[prevIdx - snapshotIndex_]
    uint64_t prevTerm = logAt(prevIdx).term;

    // 构造 AppendEntriesArgs：收集 [ni, lastLogIndex] 范围的条目
    AppendEntriesArgs args;
    args.term         = currentTerm_.load();
    args.leaderId     = id_;
    args.prevLogIndex = prevIdx;
    args.prevLogTerm  = prevTerm;
    args.leaderCommit = commitIndex_.load();
    for (uint64_t i = ni; i <= lastLogIndex(); ++i)
        args.entries.push_back(logAt(i));

    // ── 挂起点：发出 AppendEntries RPC ──────────────────────────────────
    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo(
        "AppendEntries", json(args).dump(), /*timeoutMs=*/100);

    // ── 恢复点（loop_ 线程）────────────────────────────────────────────
    if (!ok) co_return;                         // 超时/故障：下个 50ms 重试
    if (state_.load() != State::Leader) co_return; // 期间失去了 Leader 身份

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
        // 注意：期间可能另一个 replicateLog 协程也成功更新了 matchIndex（并发 RPC），
        // 只取更大值，避免「回退」（idempotent 更新）。
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
            for (int64_t i = (int64_t)lastLogIndex(); i > (int64_t)snapshotIndex_; --i) {
                if (logAt(i).term == reply.conflictTerm) { found = i + 1; break; }
            }
            newNext = (found >= 0) ? (uint64_t)found : reply.conflictIndex;
        }
        // 只允许减小 nextIndex（不能因为并发成功回包而增大）
        if (newNext < nextIndex_[peer.id])
            nextIndex_[peer.id] = std::max(newNext, uint64_t{1});
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §5b  快照传输协程：sendInstallSnapshot
//
// 当 Follower 需要的 nextIndex 已被快照压缩时，从 replicateLog 跳转到此协程。
// 发完快照后更新 matchIndex/nextIndex，后续可继续用 AppendEntries 追赶。
// ════════════════════════════════════════════════════════════════════════════

FireAndForget RaftNode::sendInstallSnapshot(Peer peer) {
    if (state_.load() != State::Leader || snapshotIndex_ == 0) co_return;

    InstallSnapshotArgs args;
    args.term              = currentTerm_.load();
    args.leaderId          = id_;
    args.lastIncludedIndex = snapshotIndex_;
    args.lastIncludedTerm  = snapshotTerm_;
    args.data              = snapshotData_;

    LOG_INFO << "[Node " << id_ << "] 向节点 " << peer.id
             << " 发送快照 lastIndex=" << args.lastIncludedIndex;

    auto [ok, respJson] = co_await getOrCreateClient(peer)->callAsyncCo(
        "InstallSnapshot", json(args).dump(), /*timeoutMs=*/1000);

    if (!ok || state_.load() != State::Leader) co_return;

    InstallSnapshotReply reply{};
    try { reply = json::parse(respJson).get<InstallSnapshotReply>(); }
    catch (...) { co_return; }

    if (reply.term > currentTerm_.load()) { becomeFollower(reply.term); co_return; }

    // 快照安装成功：把 peer 的 matchIndex 推到快照点，nextIndex 推到快照点之后
    if (snapshotIndex_ > matchIndex_[peer.id]) {
        matchIndex_[peer.id] = snapshotIndex_;
        nextIndex_[peer.id]  = snapshotIndex_ + 1;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §6  提交推进 + 状态机应用
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::advanceCommitIndex() {
    // Raft Figure 8 规则：Leader 只能直接提交「当前 term」的条目。
    // 旧 term 的条目会随着新条目的提交「顺带」被提交（commitIndex 单调递增）。
    // 若允许提交旧 term 条目，会破坏安全性（参见 Raft 论文 Figure 8 的反例）。
    uint64_t lastIdx = lastLogIndex();
    for (uint64_t n = lastIdx; n > commitIndex_.load(); --n) {
        if (n <= snapshotIndex_) break;              // 快照点之前已通过快照提交
        if (logAt(n).term != currentTerm_.load()) continue; // Figure 8 过滤
        int count = 1; // 算上自己
        for (const auto &peer : peers_) {
            if (peer.id == id_) continue;
            if (matchIndex_.count(peer.id) && matchIndex_[peer.id] >= n) ++count;
        }
        if (count >= quorum_) {
            commitIndex_.store(n);
            LOG_INFO << "[Node " << id_ << "] 提交进度推进到 index=" << n
                     << "（任期=" << logAt(n).term << " 命令=" << logAt(n).cmd << ")";
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
        if (idx > lastLogIndex()) break; // 防御：不应发生
        lastApplied_.store(idx);
        LOG_INFO << "[Node " << id_ << "] 应用日志 index=" << idx
                 << " 命令=" << logAt(idx).cmd;
        if (applyCallback_) applyCallback_(idx, logAt(idx).cmd);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §7  外部写入接口：propose
// ════════════════════════════════════════════════════════════════════════════

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
        persistLog();  // Leader 本地追加后立即落盘，崩溃不丢已接受的命令
        LOG_INFO << "[Node " << id_ << "] 追加日志条目 index=" << lastLogIndex()
                 << " 命令=" << cmd;
        // 立刻触发一轮复制（不等下一个 50ms heartbeat 周期）
        for (const auto &peer : peers_) {
            if (peer.id == id_) continue;
            replicateLog(peer);
        }
    });
}

uint64_t RaftNode::lastLogIndex() const {
    return snapshotIndex_ + static_cast<uint64_t>(log_.size()) - 1;
}
uint64_t RaftNode::lastLogTerm() const { return log_.back().term; }

LogEntry &RaftNode::logAt(uint64_t idx) {
    return log_[idx - snapshotIndex_];
}
const LogEntry &RaftNode::logAt(uint64_t idx) const {
    return log_[idx - snapshotIndex_];
}

void RaftNode::persistHardState() {
    if (!storage_) return;
    storage_->saveHardState({currentTerm_.load(), votedFor_});
}

void RaftNode::persistLog() {
    if (!storage_) return;
    // log_[0] 是哨兵，不持久化；存储的是 snapshotIndex_+1 之后的真实条目
    storage_->saveLog(std::vector<LogEntry>(log_.begin() + 1, log_.end()));
}

// ════════════════════════════════════════════════════════════════════════════
// §7b  日志压缩：takeSnapshot
//
// 由上层状态机调用（通常在 apply 达到某阈值时触发）。
// 将 [0, lastApplied_] 范围的日志条目压缩为一个快照，
// 压缩后 log_ 只保留快照点之后的条目，大幅减少内存占用和重启恢复时间。
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::takeSnapshot(const std::string &data) {
    loop_.runInLoop([this, data] {
        uint64_t snapIdx = lastApplied_.load();
        if (snapIdx <= snapshotIndex_) return; // 没有新的 apply，无需压缩

        uint64_t snapTerm = logAt(snapIdx).term;

        // 保留快照点之后的条目（以免丢弃已复制但未提交的新条目）
        uint64_t keepFrom = snapIdx - snapshotIndex_ + 1; // 局部下标
        std::vector<LogEntry> keep;
        if (keepFrom < log_.size())
            keep = std::vector<LogEntry>(log_.begin() + keepFrom, log_.end());

        // 重建 log_：新哨兵 + 保留条目
        log_.clear();
        log_.push_back(LogEntry{snapTerm, ""});
        log_.insert(log_.end(), keep.begin(), keep.end());

        snapshotIndex_ = snapIdx;
        snapshotTerm_  = snapTerm;
        snapshotData_  = data;

        LOG_INFO << "[Node " << id_ << "] 创建快照 snapshotIndex=" << snapshotIndex_
                 << " 压缩后剩余=" << (log_.size() - 1) << " 条";

        if (storage_) {
            storage_->saveSnapshot(snapshotIndex_, snapshotTerm_, snapshotData_);
            persistLog();
        }
    });
}

} // namespace raft
