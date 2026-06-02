#pragma once
//
// RaftNode —— 基于本项目 EventLoop 的 Raft 节点（全异步 RPC + C++20 协程版）
//
// Day33 新增：
//   - AppendEntries 携带真实日志条目（prevLogIndex/prevLogTerm/entries/leaderCommit）
//   - Leader 维护 nextIndex_[]/matchIndex_[] per-peer 追踪状态
//   - commitIndex / lastApplied + applyCallback（复制状态机）
//   - sendHeartbeat → replicateLog（心跳与复制合一）
//   - propose(cmd)：外部写入接口（线程安全，内部 runInLoop）
//
#include "EventLoop.h"
#include "coro/Task.h"
#include "raft/RaftStorage.h"
#include "raft/RaftTypes.h"
#include "rpc/AsyncRpcClient.h"
#include "rpc/RpcServer.h"
#include <atomic>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

    // 非阻塞：启动 loop_ 线程 + RPC server 线程。
    void start();
    // 幂等：停止所有线程并 join。
    void stop();

    // ── 外部只读快照（基于 atomic，无锁，可从任意线程调用）────────────
    State    getState() const { return state_.load(); }
    uint64_t getCurrentTerm() const { return currentTerm_.load(); }
    bool     isLeader() const { return state_.load() == State::Leader; }
    int      getId() const { return id_; }
    int      getPeerCount() const { return static_cast<int>(peers_.size()); } // 集群总节点数（含自己）
    int      getQuorum() const { return quorum_; }
    uint64_t getCommitIndex() const { return commitIndex_.load(); }
    uint64_t getLastApplied() const { return lastApplied_.load(); }
    int      getLeaderId() const { return leaderId_; }  // -1 = 未知
    uint64_t getLastLogIndex() const {
        // 仅近似值（loop_ 线程外读 log_ 可能不精确），仅供展示用
        return static_cast<uint64_t>(log_.size()) - 1;
    }

    // ── 写入接口（线程安全：内部 runInLoop 投递到 loop_ 线程）─────────
    // 若当前节点不是 Leader，命令被丢弃（真实系统应转发给 Leader）。
    // 返回时命令已入队（异步执行），不等待提交完成。
    void propose(const std::string &cmd);

    // ── 持久化（必须在 start() 之前注册）────────────────────────────────
    // 不调用 = 纯内存模式（崩溃后状态丢失）。
    // 调用后，term/votedFor/log/snapshot 均自动落盘，重启后自动恢复。
    void setStorage(std::unique_ptr<RaftStorage> storage) {
        storage_ = std::move(storage);
    }

    // ── 日志压缩（可从任意线程调用；实际执行在 loop_ 线程）──────────────
    // appliedIndex：data 对应的 lastApplied 快照点（state machine 已 apply 到此）。
    //              必须由调用方在生成 data 的同一时刻捕获，避免与 loop_ 内
    //              异步执行时已被推进的 lastApplied_ 错配（错配会导致重启后
    //              snapshotIndex 跳过若干 apply，状态机数据缺失）。
    // data：状态机快照序列化结果（对 Raft 透明）。
    // 调用后 log_ 中快照点之前的条目会被删除，对落后 peer 改发 InstallSnapshot。
    void takeSnapshot(uint64_t appliedIndex, const std::string &data);

    // ── 状态机回调（必须在 start() 之前注册）────────────────────────────
    // 当 lastApplied 推进时，在 loop_ 线程回调 cb(index, cmd)。
    void setApplyCallback(std::function<void(uint64_t, const std::string &)> cb) {
        applyCallback_ = std::move(cb);
    }

    // ── 快照安装回调（必须在 start() 之前注册）──────────────────────────
    // 当收到 Leader 发来的 InstallSnapshot 并安装完毕时，在 loop_ 线程回调
    // cb(lastIncludedIndex, snapshotData)，让状态机用快照数据重建状态。
    void setSnapshotApplyCallback(
        std::function<void(uint64_t, const std::string &)> cb) {
        snapshotApplyCallback_ = std::move(cb);
    }

    // ── 带完成通知的写入接口（线程安全）─────────────────────────────────
    // 与 propose() 相同，但在命令被 apply 到状态机后回调 done(true)。
    // 若当前节点不是 Leader，立即回调 done(false)。
    // 若在 apply 前丢失 Leader 身份，同样回调 done(false, 0)。
    // 第二个参数 logIndex 是该命令被分配到的全局日志下标（失败时为 0）。
    void proposeAndNotify(const std::string &cmd, std::function<void(bool, uint64_t)> done);

    // ── 线性一致读：ReadIndex 协议（线程安全）────────────────────────────
    // 确认 Leader 身份后（等待一轮心跳 quorum ack），在 lastApplied >= readIndex
    // 时回调 cb(true)；非 Leader 或在确认前失去 Leader 身份时回调 cb(false)。
    // 调用方在 cb(true) 后读取状态机数据即可得到线性一致的视图。
    using ReadCallback = std::function<void(bool)>;
    void proposeRead(ReadCallback cb);

    // ── Follower 线性一致读（线程安全）───────────────────────────────────
    // 无论当前节点是 Leader 还是 Follower 均可调用。
    //   Leader：直接走本地 ReadIndex 协议（与 proposeRead 等价）。
    //   Follower：向 Leader 发送 ReadIndex RPC，收到确认的 readIndex 后，
    //            等 lastApplied >= readIndex 时 cb(true) 再读本地状态机，
    //            无需将请求转发至 Leader，避免二次外部 RTT。
    void proposeFollowerRead(ReadCallback cb);

    // ── 主动 Leader Transfer（让贤）：线程安全 ──────────────────────────
    // targetId == -1 表示自动选择 matchIndex 最高的 Follower；返回是否发起。
    //   ・如果当前不是 Leader 或集群只有一个节点，返回 false。
    //   ・发起后本节点拒绝新的写入，避免追赶死循环。
    //   ・若 1s 内未能完成转移（target 未在限期内 becomeLeader），
    //     清除转移状态、恢复写入，避免集群卡死。
    bool transferLeadership(int targetId = -1);

    // 调试/可观测：返回推荐的转移目标（-1 表示无可选）。不修改状态。
    int  pickTransferTarget() const;

    // 可观测计数器（仅供外部实时面板读取）
    uint64_t getTransfersInitiated() const { return leaderTransfersInitiated_.load(); }
    uint64_t getTransfersSucceeded() const { return leaderTransfersSucceeded_.load(); }
    int      getTransferTarget()    const { return leadershipTransferTarget_; }

  private:
    // ── RPC server 回调（由 sub-reactor 线程调用，内部异步投递到 loop_）──
    // bypass: 旁路透传的原始 value 字节（仅 AppendEntries 使用；其余 RPC 中为空字符串）
    void handleRequestVote(const std::string &payload, const std::string &bypass, RpcServer::Done done);
    void handleAppendEntries(const std::string &payload, const std::string &bypass, RpcServer::Done done);
    void handleInstallSnapshot(const std::string &payload, const std::string &bypass, RpcServer::Done done);
    // 重启上线通知：对端广播自己已就绪，接收方重置对其的连接退避时钟
    void handleNodeAnnounce(const std::string &payload, const std::string &bypass, RpcServer::Done done);
    // Follower ReadIndex：Leader 侧接收 Follower 的 readIndex 查询请求
    void handleReadIndex(const std::string &payload, const std::string &bypass, RpcServer::Done done);
    // Leader Transfer：Follower 收到 Leader 发来的“立即起票”指令
    void handleTimeoutNow(const std::string &payload, const std::string &bypass, RpcServer::Done done);    // Leader Transfer 内部：检查 target 是否追平，追平则发 TimeoutNow
    void doTransferLeadership(int targetId);
    // ── 以下所有方法都必须在 loop_ 线程执行 ─────────────────────────
    void becomeFollower(uint64_t term);
    void becomeCandidate();
    void becomeLeader();

    void resetElectionTimer();
    void electionTimerFired(uint64_t epoch);

    // ── 选举主流程：runElection + collectVote 协程
    void          runElection();
    FireAndForget collectVote(uint64_t electionTerm, Peer peer);
    // Pre-vote：在真正发起选举前先探测 quorum 可达性，防止分区节点干扰集群
    FireAndForget collectPreVote(uint64_t myEpoch, uint64_t nextTerm, Peer peer);

    // 日志复制 + 心跳合一：每次心跳 = 一次 replicateLog
    // 无新条目时 entries=[] 作为心跳；有新条目时附带日志段
    void          heartbeatTick();
    FireAndForget replicateLog(Peer peer);
    FireAndForget sendInstallSnapshot(Peer peer);  // Day34：快照传输

    // ── 提交推进：Leader 在 matchIndex 更新后调用
    void advanceCommitIndex();
    // ── 应用已提交但尚未 apply 的条目（lastApplied → commitIndex）
    void applyCommitted();

    // ── ReadIndex 内部：确认 leadership 后尝试兑现所有待处理读请求 ───────
    void drainConfirmedReads();
    // Follower ReadIndex 内部：当 lastApplied 推进时兑现等待中的远端读请求
    void drainFollowerReads();

    // ── 索引辅助 ─────────────────────────────────────────────────────
    uint64_t lastLogIndex() const;
    uint64_t lastLogTerm()  const;
    // 全局 index → log_ 局部下标（必须在 snapshotIndex_ <= idx <= lastLogIndex() 时调用）
    LogEntry       &logAt(uint64_t globalIdx);
    const LogEntry &logAt(uint64_t globalIdx) const;

    // ── 持久化辅助（loop_ 线程调用）──────────────────────────────────
    void persistHardState();  // 落盘 currentTerm_ + votedFor_
    void persistLog();        // 落盘 log_[1..] （快照点之后的全量条目）

    // ── RPC 客户端缓存 ────────────────────────────────────────────────
    AsyncRpcClient *getOrCreateClient(const Peer &peer);

    // ── 配置 ────────────────────────────────────────────────────────────
    int               id_;
    std::vector<Peer> peers_;
    int               quorum_;

    // ── Raft 状态（只在 loop_ 线程读写；外部只读字段额外用 atomic 暴露）──
    std::atomic<uint64_t> currentTerm_{0};
    int                   votedFor_{-1};
    std::vector<LogEntry> log_;           // log_[0] 是哨兵条目（无快照时 term=0）
    std::atomic<State>    state_{State::Follower};
    int                   leaderId_{-1};
    int                   currentElectionVotes_{0};
    uint64_t              electionEpoch_{0};
    // 启动宽限期：节点刚起动时用加长的选举超时，避免一台重启节点立刻调高 term
    // 抢走现有健康 Leader 的身份。任何“已感知集群”的事件（收到 Leader 的
    // AppendEntries / 向其他候选人授出票）后切换为正常 150~300ms 超时。
    bool                  startupGrace_{true};
    // 已收到 NodeAnnounce 的 peer id 集合（仅 grace 期间记录）。
    // 当 announcedPeers_.size() + 1 达到 quorum 且本节点仍未听到 Leader，
    // 表示整个多数派正在同时启动、集群中不存在在任 Leader，
    // 可提前退出 grace 并立即发起选举，避免"全集群同时重启需等 30s"。
    std::unordered_set<int> announcedPeers_;

    // ── 日志复制状态 ────────────────────────────────────────────────────
    std::atomic<uint64_t> commitIndex_{0};  // 已提交的最高 index
    std::atomic<uint64_t> lastApplied_{0};  // 已应用到状态机的最高 index

    // Leader 专属（仅在 loop_ 线程访问，becomeLeader 初始化，角色切换后可能过时）
    std::unordered_map<int, uint64_t> nextIndex_;   // peer.id → 下次发送的 index
    std::unordered_map<int, uint64_t> matchIndex_;  // peer.id → 已确认复制的最高 index

    // ── 快照状态（loop_ 线程访问）───────────────────────────────────────
    // snapshotIndex_ 是 log_[0] 哨兵代表的全局 index：
    //   全局 index i 对应 log_[i - snapshotIndex_]
    // 初始 snapshotIndex_=0，log_[0].term=0（无快照的哨兵）。
    uint64_t    snapshotIndex_{0};
    uint64_t    snapshotTerm_{0};
    std::string snapshotData_;  // 状态机快照数据（Raft 透明）

    // ── Pre-vote 状态（仅在 loop_ 线程访问）──────────────────────────────
    int preVoteCount_{0};  // 当前预投票轮次已收到的赞成票数（含自己）

    // ── ReadIndex 状态（仅在 loop_ 线程访问）────────────────────────────
    // 每次 heartbeatTick 递增 readHeartbeatEpoch_；replicateLog 成功后
    // 累计 readHeartbeatAcks_；当 acks 达到 quorum 时记录 readConfirmedEpoch_，
    // 此时所有 requestEpoch < readConfirmedEpoch_ 且 readIndex <= lastApplied_
    // 的 pendingReads_ 可以被安全兑现（线性一致性保证）。
    uint64_t readHeartbeatEpoch_{0};
    int      readHeartbeatAcks_{0};
    uint64_t readConfirmedEpoch_{0};

    struct PendingRead {
        uint64_t     readIndex;      // 注册时的 commitIndex
        uint64_t     requestEpoch;   // 注册时的 readHeartbeatEpoch_
        ReadCallback cb;
    };
    std::vector<PendingRead> pendingReads_;

    // ── Follower ReadIndex 状态（仅在 loop_ 线程访问）───────────────────────
    // Follower 侧：向 Leader 发出 ReadIndexReq 后，等待回包确认 readIndex。
    // readIndex==0 表示 Leader 回包尚未到达。
    struct FollowerPendingRead {
        uint64_t     readIndex{0};   // Leader 返回的确认 readIndex（0 = 待填充）
        ReadCallback cb;
    };
    uint64_t followerReadSeq_{0};
    std::unordered_map<uint64_t, FollowerPendingRead> followerPendingReads_;

    // Leader 侧：来自 Follower 的 ReadIndex 请求（等待本轮心跳 quorum 确认后回包）
    struct PendingRemoteRead {
        uint64_t        readIndex;     // Leader 注册时的 commitIndex
        uint64_t        requestEpoch;  // Leader 注册时的 readHeartbeatEpoch_
        uint64_t        requestId;     // Follower 请求 ID（原样返回）
        RpcServer::Done done;          // 回包给 Follower 的异步回调
    };
    std::vector<PendingRemoteRead> pendingRemoteReads_;

    // ── Leader Transfer 状态（仅在 loop_ 线程访问）───────────────────────
    // -1 表示未在转移中；非 -1 时 Leader 拒绝新写入并等待 target 成为 Leader。
    int leadershipTransferTarget_{-1};
    // 超时守护：1s 内 target 未能接任则清除转移状态。存取 仅在 loop_。
    uint64_t transferDeadlineMs_{0};
    // 外部可观测的原子计数器（仅供 /admin/raft 面板读取）。
    std::atomic<uint64_t> leaderTransfersInitiated_{0};
    std::atomic<uint64_t> leaderTransfersSucceeded_{0};

    // ── 持久化后端（nullptr = 纯内存）───────────────────────────────────
    std::unique_ptr<RaftStorage> storage_;

    // 状态机回调：commit 推进时在 loop_ 线程回调
    std::function<void(uint64_t, const std::string &)> applyCallback_;
    // 快照安装回调：InstallSnapshot 完成后在 loop_ 线程回调
    std::function<void(uint64_t, const std::string &)> snapshotApplyCallback_;
    // 等待 apply 的写回调：logIndex → done(ok)（仅在 loop_ 线程访问，无需加锁）
    std::unordered_map<uint64_t, std::function<void(bool, uint64_t)>> writeCallbacks_;

    // ── 基础设施 ────────────────────────────────────────────────────────
    Eventloop         loop_;
    std::thread       loopThread_;
    std::atomic<bool> running_{false};
    std::mt19937      rng_;

    RpcServer   rpcServer_;
    std::thread rpcServerThread_;

    // 出站 RPC：每个 peer 一个长连接异步客户端，复用 loop_ 的 IO。
    // 仅在 loop_ 线程访问。
    std::unordered_map<int, std::unique_ptr<AsyncRpcClient>> peerClients_;
};

} // namespace raft


