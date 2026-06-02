// RaftNode.cpp —— Raft 共识节点（选举 + 日志复制 + 持久化 + 快照 + C++20 协程）
//
// 阅读顺序：
//   §1 构造 / 析构 / start / stop      —— 线程编排与生命周期（含持久化恢复）
//   §2 RPC server 回调（fire-and-forget）—— AppendEntries / InstallSnapshot
//   §3 角色切换 + 选举定时器           —— 全部在 loop_ 线程，无锁
//   §4 选举：Pre-Vote → runElection + collectVote 协程
//   §5 日志复制 + 心跳：heartbeatTick + replicateLog + sendInstallSnapshot 协程
//   §6 提交推进：advanceCommitIndex + applyCommitted
//   §7 外部写入/读取接口：propose + proposeRead + takeSnapshot
//   §8 辅助：lastLogIndex / lastLogTerm / logAt / persistHardState / persistLog
//
#include "raft/RaftNode.h"
#include "log/Logger.h"
#include <chrono>
// Protobuf 生成的 Raft RPC 消息类型（cmake protobuf_generate_cpp 输出到 build/）
#include "raft.pb.h"

namespace raft {

// ── Protobuf 序列化辅助 + Value 旁路透传拆分 ──────────────────────────────
// 替换旧的 mpEncode/mpDecode（json → msgpack），使用 Protobuf 编解码。
// AppendEntries 特别处理：cmd 中 value 部分通过 bypass 字段旁路透传，
// 避免 value 字节经过 Protobuf 序列化（节省 2~4 次 value 拷贝）。
namespace {

// ── 命令分割 / 重组（AppendEntries bypass 旁路透传核心）─────────────────
// splitCmd("PUT mykey value_bytes") → {"PUT mykey", "value_bytes"}
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
// joinCmd("PUT mykey", "value_bytes") → "PUT mykey value_bytes"
// joinCmd("DEL mykey", "")            → "DEL mykey"
static std::string joinCmd(const std::string &hdr, const std::string &val) {
    if (val.empty()) return hdr;
    return hdr + " " + val;
}

// ── RequestVote ────────────────────────────────────────────────────────────
static std::string encodeRV(const RequestVoteArgs &a) {
    raft_proto::RequestVoteReq pb;
    pb.set_term(a.term);
    pb.set_candidate_id(a.candidateId);
    pb.set_last_log_index(a.lastLogIndex);
    pb.set_last_log_term(a.lastLogTerm);
    pb.set_pre_vote(a.preVote);
    std::string s; pb.SerializeToString(&s); return s;
}
static RequestVoteArgs decodeRV(const std::string &s) {
    raft_proto::RequestVoteReq pb; pb.ParseFromString(s);
    return {static_cast<uint64_t>(pb.term()), static_cast<int>(pb.candidate_id()),
            static_cast<uint64_t>(pb.last_log_index()),
            static_cast<uint64_t>(pb.last_log_term()), pb.pre_vote()};
}
static std::string encodeRVRep(const RequestVoteReply &r) {
    raft_proto::RequestVoteRep pb;
    pb.set_term(r.term); pb.set_vote_granted(r.voteGranted);
    std::string s; pb.SerializeToString(&s); return s;
}
static RequestVoteReply decodeRVRep(const std::string &s) {
    raft_proto::RequestVoteRep pb; pb.ParseFromString(s);
    return {static_cast<uint64_t>(pb.term()), pb.vote_granted()};
}

// ── AppendEntries（含 bypass 旁路透传）────────────────────────────────────
// 返回 {proto_bytes, bypass_bytes}：
//   proto_bytes  = Protobuf 编码的元数据（term/index/条目 cmd_header/vlen）
//   bypass_bytes = 所有条目 value 字节顺序拼接（不经过任何序列化）
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
        le.term        = ep.term();
        std::string val = bypass.substr(offset, ep.vlen());
        offset += ep.vlen();
        le.cmd = joinCmd(std::string(ep.cmd()), val);
        a.entries.push_back(std::move(le));
    }
    return a;
}
static std::string encodeAERep(const AppendEntriesReply &r) {
    raft_proto::AppendEntriesRep pb;
    pb.set_term(r.term); pb.set_success(r.success);
    pb.set_conflict_index(r.conflictIndex); pb.set_conflict_term(r.conflictTerm);
    std::string s; pb.SerializeToString(&s); return s;
}
static AppendEntriesReply decodeAERep(const std::string &s) {
    raft_proto::AppendEntriesRep pb; pb.ParseFromString(s);
    return {static_cast<uint64_t>(pb.term()), pb.success(),
            static_cast<uint64_t>(pb.conflict_index()),
            static_cast<uint64_t>(pb.conflict_term())};
}

// ── InstallSnapshot ────────────────────────────────────────────────────────
static std::string encodeIS(const InstallSnapshotArgs &a) {
    raft_proto::InstallSnapshotReq pb;
    pb.set_term(a.term); pb.set_leader_id(a.leaderId);
    pb.set_last_included_index(a.lastIncludedIndex);
    pb.set_last_included_term(a.lastIncludedTerm);
    pb.set_data(a.data);
    std::string s; pb.SerializeToString(&s); return s;
}
static InstallSnapshotArgs decodeIS(const std::string &s) {
    raft_proto::InstallSnapshotReq pb; pb.ParseFromString(s);
    InstallSnapshotArgs a;
    a.term               = pb.term();
    a.leaderId           = pb.leader_id();
    a.lastIncludedIndex  = pb.last_included_index();
    a.lastIncludedTerm   = pb.last_included_term();
    a.data               = pb.data();
    return a;
}
static std::string encodeISRep(const InstallSnapshotReply &r) {
    raft_proto::InstallSnapshotRep pb; pb.set_term(r.term);
    std::string s; pb.SerializeToString(&s); return s;
}
static InstallSnapshotReply decodeISRep(const std::string &s) {
    raft_proto::InstallSnapshotRep pb; pb.ParseFromString(s);
    return {static_cast<uint64_t>(pb.term())};
}

// ── Follower ReadIndex ────────────────────────────────────────────────────
static std::string encodeReadIndexReq(uint64_t followerId, uint64_t requestId) {
    raft_proto::ReadIndexReq pb;
    pb.set_follower_id(followerId);
    pb.set_request_id(requestId);
    std::string s; pb.SerializeToString(&s); return s;
}
static std::pair<uint64_t, uint64_t> decodeReadIndexReq(const std::string &s) {
    raft_proto::ReadIndexReq pb; pb.ParseFromString(s);
    return {static_cast<uint64_t>(pb.follower_id()),
            static_cast<uint64_t>(pb.request_id())};
}
static std::string encodeReadIndexResp(uint64_t requestId, uint64_t readIndex, bool ok) {
    raft_proto::ReadIndexResp pb;
    pb.set_request_id(requestId);
    pb.set_read_index(readIndex);
    pb.set_ok(ok);
    std::string s; pb.SerializeToString(&s); return s;
}
static std::tuple<uint64_t, uint64_t, bool> decodeReadIndexResp(const std::string &s) {
    raft_proto::ReadIndexResp pb; pb.ParseFromString(s);
    return {static_cast<uint64_t>(pb.request_id()),
            static_cast<uint64_t>(pb.read_index()),
            pb.ok()};
}

// ── TimeoutNow（Leader Transfer）─────────────────────────────────────────
static std::string encodeTimeoutNowReq(uint64_t term) {
    raft_proto::TimeoutNowReq pb; pb.set_term(term);
    std::string s; pb.SerializeToString(&s); return s;
}
static uint64_t decodeTimeoutNowReq(const std::string &s) {
    raft_proto::TimeoutNowReq pb; pb.ParseFromString(s);
    return static_cast<uint64_t>(pb.term());
}
static std::string encodeTimeoutNowResp(uint64_t term, bool ok) {
    raft_proto::TimeoutNowResp pb;
    pb.set_term(term); pb.set_ok(ok);
    std::string s; pb.SerializeToString(&s); return s;
}

} // namespace

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
    // 新签名：handler(payload, bypass, done)
    rpcServer_.addHandler(
        "RequestVote",
        [this](const std::string &payload, const std::string &bypass, RpcServer::Done done) {
            handleRequestVote(payload, bypass, std::move(done));
        });
    rpcServer_.addHandler(
        "AppendEntries",
        [this](const std::string &payload, const std::string &bypass, RpcServer::Done done) {
            handleAppendEntries(payload, bypass, std::move(done));
        });
    rpcServer_.addHandler(
        "InstallSnapshot",
        [this](const std::string &payload, const std::string &bypass, RpcServer::Done done) {
            handleInstallSnapshot(payload, bypass, std::move(done));
        });
    rpcServer_.addHandler(
        "NodeAnnounce",
        [this](const std::string &payload, const std::string &bypass, RpcServer::Done done) {
            handleNodeAnnounce(payload, bypass, std::move(done));
        });
    rpcServer_.addHandler(
        "ReadIndex",
        [this](const std::string &payload, const std::string &bypass, RpcServer::Done done) {
            handleReadIndex(payload, bypass, std::move(done));
        });
    rpcServer_.addHandler(
        "TimeoutNow",
        [this](const std::string &payload, const std::string &bypass, RpcServer::Done done) {
            handleTimeoutNow(payload, bypass, std::move(done));
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
                    // 将快照数据应用到状态机——必须在此处调用，否则重启后状态机为空。
                    if (snapshotApplyCallback_)
                        snapshotApplyCallback_(snapIdx, snapData);
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
            loop_.runAfter(0.1, [this] {
                raft_proto::NodeAnnounceReq pb;
                pb.set_node_id(id_);
                std::string body; pb.SerializeToString(&body);
                for (const auto &peer : peers_) {
                    if (peer.id == id_) continue;
                    getOrCreateClient(peer)->callAsync(
                        "NodeAnnounce", body, /*bypass=*/{},
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

void RaftNode::handleRequestVote(const std::string &payload, const std::string & /*bypass*/,
                                  RpcServer::Done done) {
    RequestVoteArgs args;
    try {
        args = decodeRV(payload);
    } catch (...) {
        done(encodeRVRep({0, false}));
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        // ── Pre-vote 路径：不修改任何持久化状态 ──────────────────────────
        // 候选人询问"若我以 args.term 发起真实选举，你会投我吗？"
        // 判断标准与真实投票相同，但不更新 term/votedFor，不落盘，不重置选举计时器。
        if (args.preVote) {
            bool eligible = (args.term >= currentTerm_.load()) &&
                            (args.lastLogTerm > lastLogTerm() ||
                             (args.lastLogTerm == lastLogTerm() && args.lastLogIndex >= lastLogIndex()));
            done(encodeRVRep({currentTerm_.load(), eligible}));
            return;
        }

        // ── 真实投票路径 ─────────────────────────────────────────────────
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
            votedFor_ = args.candidateId;
            startupGrace_ = false;
            resetElectionTimer();
            persistHardState();  // votedFor 变化必须立即落盘
            LOG_INFO << "[Node " << id_ << "] 已投票给节点 " << args.candidateId
                     << "，term=" << args.term;
        }

        done(encodeRVRep({currentTerm_.load(), grant}));
    });
}

void RaftNode::handleAppendEntries(const std::string &payload, const std::string &bypass,
                                    RpcServer::Done done) {
    AppendEntriesArgs args;
    try {
        args = decodeAE(payload, bypass);
    } catch (...) {
        done(encodeAERep({0, false, 0, 0}));
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        AppendEntriesReply reply{currentTerm_.load(), false, 0, 0};

        // 规则①：过时 term 的 RPC 直接拒绝（发送方是旧 Leader，已被淘汰）
        if (args.term < currentTerm_.load()) {
            done(encodeAERep(reply));
            return;
        }

        // 收到有效 Leader 的消息：确认自己是 Follower
        state_.store(State::Follower);
        leaderId_ = args.leaderId;
        startupGrace_ = false;  // 已感知现任 Leader，退出启动宽限期
        resetElectionTimer();  // 压制自己的选举超时

        // 规则②（一致性检查）：我的日志里是否存在 prevLogIndex 处的条目，且 term 匹配？
        //
        // 这是 Raft 的核心安全机制：Leader 通过「前缀匹配」保证 Follower 和自己历史一致。
        // 若这里不匹配，Follower 无法安全追加 entries，必须拒绝并给出冲突提示。

        // 快照点之前的条目已压缩，Leader 不应该再发 prevLogIndex < snapshotIndex_，
        // 但如果由于 InstallSnapshot 和并发飞行的 AE 时序交错，确实收到了：
        // 必须裁掉 entries 中 index <= snapshotIndex_ 的部分（已被快照覆盖），
        // 但 index > snapshotIndex_ 的部分仍需正常追加，否则 follower 状态机
        // 会永久落后这批 entries（leader 误以为成功后不再重传这部分）。
        if (args.prevLogIndex < snapshotIndex_) {
            uint64_t skipN = snapshotIndex_ - args.prevLogIndex;
            if (skipN >= args.entries.size()) {
                reply.success = true;
                done(encodeAERep(reply));
                return;
            }
            args.entries.erase(args.entries.begin(),
                               args.entries.begin() + skipN);
            args.prevLogIndex = snapshotIndex_;
            args.prevLogTerm  = snapshotTerm_;
            // 继续走下面的正常追加 + commit 推进流程
        }
        if (args.prevLogIndex > lastLogIndex()) {
            reply.conflictIndex = lastLogIndex() + 1;
            reply.conflictTerm  = 0;
            done(encodeAERep(reply));
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
            done(encodeAERep(reply));
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
        done(encodeAERep(reply));
    });
}
// ════════════════════════════════════════════════════════════════════════════
// §2b  InstallSnapshot handler
//
// 当 Follower 严重落后（nextIndex <= snapshotIndex_），Leader 改发快照。
// Follower 用快照替换本地日志前缀，保留快照点之后已有的条目，
// 并更新 commitIndex/lastApplied 到快照点（无需逐条 apply）。
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::handleInstallSnapshot(const std::string &payload, const std::string & /*bypass*/,
                                      RpcServer::Done done) {
    InstallSnapshotArgs args;
    try {
        args = decodeIS(payload);
    } catch (...) {
        done(encodeISRep({0}));
        return;
    }

    loop_.runInLoop([this, args, done = std::move(done)]() mutable {
        if (args.term > currentTerm_.load()) becomeFollower(args.term);

        InstallSnapshotReply reply{currentTerm_.load()};
        if (args.term < currentTerm_.load()) {
            done(encodeISRep(reply));
            return;
        }

        state_.store(State::Follower);
        leaderId_ = args.leaderId;
        startupGrace_ = false;  // 已感知到现任 Leader，退出启动宽限期
        resetElectionTimer();

        // 快照比我现有的更旧：忽略（避免状态倒退）
        if (args.lastIncludedIndex <= snapshotIndex_) {
            done(encodeISRep(reply));
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

        // 通知状态机用快照数据重建自身状态
        if (snapshotApplyCallback_)
            snapshotApplyCallback_(snapshotIndex_, snapshotData_);

        done(encodeISRep(reply));
    });
}

// ════════════════════════════════════════════════════════════════════════════
// §3  角色切换 + 选举定时器（必须在 loop_ 线程调用）
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::becomeFollower(uint64_t term) {
    if (state_.load() != State::Follower)
        LOG_INFO << "[Node " << id_ << "] " << stateName(state_.load()) << " → 跟随者（任期="
                 << term << "）";
    // 丢失 Leader 身份：将所有未完成的写请求全部通知失败
    if (!writeCallbacks_.empty()) {
        for (auto &[idx, cb] : writeCallbacks_) cb(false, idx);
        writeCallbacks_.clear();
    }
    // 丢失 Leader 身份：将所有待确认的线性读请求全部通知失败
    for (auto &pr : pendingReads_) pr.cb(false);
    pendingReads_.clear();
    // 丢失 Leader 身份：失败所有来自 Follower 的远端 ReadIndex 请求
    for (auto &rr : pendingRemoteReads_)
        rr.done(encodeReadIndexResp(rr.requestId, 0, false));
    pendingRemoteReads_.clear();
    // 如果本节点之前是 Follower，清理本地 Follower ReadIndex 等待队列
    for (auto &[reqId, pr] : followerPendingReads_) pr.cb(false);
    followerPendingReads_.clear();
    // Leader Transfer 收尾：若本次 step-down 是因为 transfer target 起票成功，
    // 计入"成功让贤"指标；任何情况下都清理 transfer 标记。
    if (state_.load() == State::Leader && leadershipTransferTarget_ != -1) {
        leaderTransfersSucceeded_.fetch_add(1);
        LOG_INFO << "[Node " << id_ << "] *** Leader Transfer 完成：让贤至 Node "
                 << leadershipTransferTarget_ << " ***";
    }
    leadershipTransferTarget_ = -1;
    transferDeadlineMs_       = 0;
    state_.store(State::Follower);
    currentTerm_.store(term);
    votedFor_ = -1;
    persistHardState();  // term 变化必须立即落盘
    resetElectionTimer();
}

void RaftNode::becomeCandidate() {
    // 以防万一：如果以 Leader 身份进入候选（理论上不应发生），同样清空写回调
    if (!writeCallbacks_.empty()) {
        for (auto &[idx, cb] : writeCallbacks_) cb(false, idx);
        writeCallbacks_.clear();
    }
    // 候选期间不再跟随任何 Leader，清空本节点的 Follower ReadIndex 等待队列
    for (auto &[reqId, pr] : followerPendingReads_) pr.cb(false);
    followerPendingReads_.clear();
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
    startupGrace_ = false;
    LOG_INFO << "[Node " << id_ << "] *** 成为领导者，任期=" << currentTerm_.load() << " ***";
    ++electionEpoch_;
    // 上任后清理上一任可能遗留的 transfer 标记
    leadershipTransferTarget_ = -1;
    transferDeadlineMs_       = 0;

    // ── No-op 条目（Raft Figure 8 安全性 + ReadIndex 前提）─────────────────
    // Leader 不能直接提交前任 term 的条目（可能引发日志覆盖）。
    // 通过追加一条属于当前 term 的 no-op 空条目，利用正常多数派确认规则，
    // 间接将前任遗留的已复制但未提交的条目一并提交，恢复一致状态。
    // ReadIndex 协议同样要求 Leader 必须先提交过本任期内的至少一条日志，
    // 才能安全地用 commitIndex 作为 readIndex 的基准。
    log_.push_back(LogEntry{currentTerm_.load(), ""});  // 空 cmd = no-op
    persistLog();

    // ── 重置 ReadIndex 心跳跟踪（新 Leader 上任，旧 epoch 全部作废）──────
    readHeartbeatEpoch_ = 0;
    readHeartbeatAcks_  = 1;  // 自己算一票
    readConfirmedEpoch_ = 0;

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

    // 立刻广播一次复制（含 no-op 条目），尽快通知所有 Follower 新 Leader 存在。
    heartbeatTick();
}

void RaftNode::resetElectionTimer() {
    // EventLoop 的定时器无法主动取消，用「版本号」实现软取消：
    // 每次 reset 都递增 epoch，旧定时器触发时发现 epoch 不匹配就自动放弃。
    ++electionEpoch_;
    uint64_t myEpoch = electionEpoch_;  // 捕获当前 epoch 进 lambda
    // 选举超时随机化（1500~3000ms 启动宽限期 / 150~300ms 稳态期）。启动宽限期避免
    // 一台重启节点在未听到 Leader 心跳前起初始选举超时 → 调高 term 抢走 Leader
    // 身份。启动宽限期 ≥ 2× 稳态期上限，保证 50ms 心跳足以在该窗口内到达。
    int timeoutMs = startupGrace_
        ? std::uniform_int_distribution<int>(1500, 3000)(rng_)
        : std::uniform_int_distribution<int>(150, 300)(rng_);
    loop_.runAfter(timeoutMs / 1000.0,
                   [this, myEpoch] { electionTimerFired(myEpoch); });
}

void RaftNode::electionTimerFired(uint64_t epoch) {
    if (epoch != electionEpoch_) return;
    if (state_.load() == State::Leader) return;
    if (startupGrace_) {
        LOG_INFO << "[Node " << id_ << "] 选举超时但仍在启动宽限期内，抑制本次选举";
        resetElectionTimer();
        return;
    }
    // ── Pre-Vote 阶段 ─────────────────────────────────────────────────────
    // 在真正递增 term 发起选举之前，先用 preVote=true 探测多数派。
    // 如果探测成功，再调用 becomeCandidate() + runElection()。
    // 这防止因短暂网络分区而被隔离的节点不断自增 term 后重新接入集群时
    // 强制所有节点 step down（因为它的 term 远高于现任 Leader）。
    preVoteCount_ = 1;  // 自己先给自己一票
    uint64_t myEpoch  = electionEpoch_;
    uint64_t nextTerm = currentTerm_.load() + 1;
    LOG_INFO << "[Node " << id_ << "] 选举超时，发起预投票（预测 term=" << nextTerm << "）";
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        collectPreVote(myEpoch, nextTerm, peer);
    }
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
    RequestVoteArgs args{electionTerm, id_, lastLogIndex(), lastLogTerm(), /*preVote=*/false};

    // ── 挂起点：发出 RPC，等待响应/超时 ────────────────────────────────
    auto [ok, respBytes] = co_await getOrCreateClient(peer)->callAsyncCo(
        "RequestVote", encodeRV(args), /*bypass=*/{}, /*timeoutMs=*/150);

    // ── 恢复点（loop_ 线程）────────────────────────────────────────────
    // 守卫①：在等待期间，节点状态可能已改变（如收到更高 term 退为 Follower）。
    // 守卫②：currentTerm_ 可能已递增（本轮选举已作废）。
    // 任一不满足，说明这个回包对当前轮次无效，直接丢弃。
    if (state_.load() != State::Candidate || currentTerm_.load() != electionTerm) co_return;

    if (!ok) co_return; // RPC 超时或网络故障，忽略

    RequestVoteReply reply{};
    try {
        reply = decodeRVRep(respBytes);
    } catch (...) {
        co_return;
    }

    // 【Raft 规则 §5.1】回包 term 更大：立刻退位
    if (reply.term > currentTerm_.load()) {
        becomeFollower(reply.term);
        co_return;
    }

    if (reply.voteGranted) {
        ++currentElectionVotes_;
        LOG_INFO << "[Node " << id_ << "] 收到节点 " << peer.id
                 << " 的选票（已得票=" << currentElectionVotes_ << "/" << quorum_ << "）";
        if (currentElectionVotes_ >= quorum_) becomeLeader();
    }
}

// ── Pre-Vote 协程：收集预投票，若达到 quorum 再转入真正选举 ─────────────
FireAndForget RaftNode::collectPreVote(uint64_t myEpoch, uint64_t nextTerm, Peer peer) {
    RequestVoteArgs args{nextTerm, id_, lastLogIndex(), lastLogTerm(), /*preVote=*/true};

    auto [ok, respBytes] = co_await getOrCreateClient(peer)->callAsyncCo(
        "RequestVote", encodeRV(args), /*bypass=*/{}, /*timeoutMs=*/150);

    // 若在等待期间 epoch 已变（超时被重置或收到 Leader 心跳），则放弃本轮预投票
    if (electionEpoch_ != myEpoch) co_return;
    if (state_.load() != State::Follower) co_return;
    if (!ok) co_return;

    RequestVoteReply reply{};
    try {
        reply = decodeRVRep(respBytes);
    } catch (...) {
        co_return;
    }

    if (reply.term > currentTerm_.load()) {
        becomeFollower(reply.term);
        co_return;
    }

    if (reply.voteGranted) {
        ++preVoteCount_;
        LOG_INFO << "[Node " << id_ << "] 预投票：Node " << peer.id
                 << " 赞成（已得=" << preVoteCount_ << "/" << quorum_ << "）";
        if (preVoteCount_ >= quorum_) {
            LOG_INFO << "[Node " << id_ << "] 预投票成功，发起正式选举（term=" << nextTerm << "）";
            becomeCandidate();
            runElection();
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// §4b NodeAnnounce：重启上线通知
//
// 重启节点在 start() 后 100ms 广播此消息；接收方重置对发送方的
// AsyncRpcClient 退避时钟并触发立即重连，消除指数退避造成的重连延迟。
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::handleNodeAnnounce(const std::string &payload, const std::string & /*bypass*/,
                                   RpcServer::Done done) {
    int senderId = -1;
    try {
        raft_proto::NodeAnnounceReq pb;
        pb.ParseFromString(payload);
        senderId = pb.node_id();
    } catch (...) {
        raft_proto::NodeAnnounceRep rep;
        std::string s; rep.SerializeToString(&s);
        done(s);
        return;
    }

    loop_.runInLoop([this, senderId, done = std::move(done)]() mutable {
        raft_proto::NodeAnnounceRep rep;
        std::string s; rep.SerializeToString(&s);
        done(s);  // 立即回复
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
    // 每次心跳递增 epoch，初始化 ack 计数（自己算一票）。
    // replicateLog 成功后累计 ack；达到 quorum 时记录 readConfirmedEpoch_，
    // 这表明我在该 epoch 仍是合法 Leader，可以安全兑现 ReadIndex 请求。
    ++readHeartbeatEpoch_;
    readHeartbeatAcks_ = 1;
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

    // 在 co_await 前捕获当前 epoch，恢复后用于 ReadIndex 确认
    uint64_t myReadEpoch = readHeartbeatEpoch_;

    // ── 挂起点：发出 AppendEntries RPC ──────────────────────────────────
    auto [protoBytes, bypass] = encodeAE(args);
    auto [ok, respBytes] = co_await getOrCreateClient(peer)->callAsyncCo(
        "AppendEntries", protoBytes, bypass, /*timeoutMs=*/100);

    // ── 恢复点（loop_ 线程）────────────────────────────────────────────
    if (!ok) co_return;                         // 超时/故障：下个 50ms 重试
    if (state_.load() != State::Leader) co_return; // 期间失去了 Leader 身份

    AppendEntriesReply reply{};
    try {
        reply = decodeAERep(respBytes);
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

        // ── Leader Transfer：target 追平日志后立即发 TimeoutNow ─────────
        if (leadershipTransferTarget_ == peer.id
            && matchIndex_[peer.id] >= lastLogIndex()) {
            doTransferLeadership(peer.id);
        }

        // ── ReadIndex：累计此 epoch 的 quorum ack ───────────────────────
        // 每个 replicateLog 成功即代表该 peer 确认"我仍是 Leader"，
        // 当 ack 数达到 quorum 时，将 readConfirmedEpoch_ 推进到当前 epoch，
        // 之后可以安全地兑现所有 requestEpoch < readConfirmedEpoch_ 的读请求。
        if (readHeartbeatEpoch_ == myReadEpoch) {
            if (++readHeartbeatAcks_ >= quorum_) {
                readConfirmedEpoch_ = readHeartbeatEpoch_;
                drainConfirmedReads();
            }
        }
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

    auto [ok, respBytes] = co_await getOrCreateClient(peer)->callAsyncCo(
        "InstallSnapshot", encodeIS(args), /*bypass=*/{}, /*timeoutMs=*/1000);

    if (!ok || state_.load() != State::Leader) co_return;

    InstallSnapshotReply reply{};
    try { reply = decodeISRep(respBytes); }
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
        // no-op 条目（becomeLeader 时插入的空 cmd）不传递给状态机
        if (!logAt(idx).cmd.empty()) {
            LOG_INFO << "[Node " << id_ << "] 应用日志 index=" << idx
                     << " 命令=" << logAt(idx).cmd;
            if (applyCallback_) applyCallback_(idx, logAt(idx).cmd);
        }
        auto it = writeCallbacks_.find(idx);
        if (it != writeCallbacks_.end()) {
            it->second(true, it->first);
            writeCallbacks_.erase(it);
        }
    }
    // lastApplied_ 推进后，尝试兑现所有满足 readIndex <= lastApplied_ 的读请求
    drainConfirmedReads();
    // 尝试兑现 Follower 侧等待 lastApplied >= readIndex 的 ReadIndex 请求
    drainFollowerReads();
}

// ── 兑现已确认 Leader 身份的 ReadIndex 请求 ─────────────────────────────
// 调用时机：readConfirmedEpoch_ 更新时 或 lastApplied_ 推进时。
void RaftNode::drainConfirmedReads() {
    auto it = pendingReads_.begin();
    while (it != pendingReads_.end()) {
        if (state_.load() != State::Leader) {
            // 丢失 Leader 身份：通知所有待处理读请求失败
            it->cb(false);
            it = pendingReads_.erase(it);
        } else if (readConfirmedEpoch_ > it->requestEpoch
                   && lastApplied_.load() >= it->readIndex) {
            // Leadership 已在 requestEpoch 之后得到多数派确认，
            // 且状态机已至少应用到注册时的 commitIndex。线性一致性成立。
            it->cb(true);
            it = pendingReads_.erase(it);
        } else {
            ++it;
        }
    }
    // 远端 ReadIndex（来自 Follower 的查询）：Leader 确认 epoch 后直接回包，
    // Follower 侧会再等 lastApplied >= readIndex 后才真正读状态机。
    for (auto it2 = pendingRemoteReads_.begin(); it2 != pendingRemoteReads_.end(); ) {
        if (state_.load() != State::Leader) {
            it2->done(encodeReadIndexResp(it2->requestId, 0, false));
            it2 = pendingRemoteReads_.erase(it2);
        } else if (readConfirmedEpoch_ > it2->requestEpoch) {
            it2->done(encodeReadIndexResp(it2->requestId, it2->readIndex, true));
            it2 = pendingRemoteReads_.erase(it2);
        } else {
            ++it2;
        }
    }
}

// ── Follower 侧：当 lastApplied 推进时，尝试兑现等待 readIndex 的读请求 ──
void RaftNode::drainFollowerReads() {
    for (auto it = followerPendingReads_.begin(); it != followerPendingReads_.end(); ) {
        auto &pr = it->second;
        if (pr.readIndex > 0 && lastApplied_.load() >= pr.readIndex) {
            pr.cb(true);
            it = followerPendingReads_.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Leader 侧 handler：接收 Follower 发来的 ReadIndex 查询 ────────────────
void RaftNode::handleReadIndex(const std::string &payload, const std::string & /*bypass*/,
                               RpcServer::Done done) {
    auto [followerId, requestId] = decodeReadIndexReq(payload);
    loop_.runInLoop([this, followerId, requestId, done = std::move(done)]() mutable {
        if (state_.load() != State::Leader) {
            done(encodeReadIndexResp(requestId, 0, false));
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

// ── Follower/Leader 统一线性读接口 ────────────────────────────────────────
// Leader 走本地 ReadIndex 协议（等价于 proposeRead）；
// Follower 向 Leader 发 ReadIndexReq RPC，收到确认的 readIndex 后
// 在本地等 lastApplied >= readIndex，再回调 cb(true) 让调用方读本地状态机。
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
                if (it == followerPendingReads_.end()) return;  // 已被清理
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
                // 如果 lastApplied 已够（极少数情况：RPC 延迟内日志已追上），立即兑现
                drainFollowerReads();
            });
    });
}

// ════════════════════════════════════════════════════════════════════════════
// §6c  Leader Transfer（主动让贤）
//
// 协议步骤（Leader 视角）：
//   1. 选定一个 target Follower（matchIndex 最高优先）
//   2. 标记 leadershipTransferTarget_，从此拒绝新的 propose（避免追赶死循环）
//   3. 若 target 已追平日志（matchIndex == lastLogIndex），立即发 TimeoutNow
//      否则继续 replicateLog，等下一次 onAppendEntriesReply 看到追平再发
//   4. target 收到 TimeoutNow → 跳过 election timeout 与 Pre-Vote → becomeCandidate
//   5. 1s 兜底：若超时仍未感知到对方 becomeLeader，清理状态、恢复写入
// ════════════════════════════════════════════════════════════════════════════

namespace {
inline uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}

// ── 选 transfer 目标：matchIndex 最高的 Follower（同分时取 id 最小）──────
int RaftNode::pickTransferTarget() const {
    int      best       = -1;
    uint64_t bestMatch  = 0;
    for (const auto &peer : peers_) {
        if (peer.id == id_) continue;
        auto it = matchIndex_.find(peer.id);
        uint64_t mi = (it == matchIndex_.end()) ? 0 : it->second;
        if (best < 0 || mi > bestMatch || (mi == bestMatch && peer.id < best)) {
            best      = peer.id;
            bestMatch = mi;
        }
    }
    return best;
}

bool RaftNode::transferLeadership(int targetId) {
    if (state_.load() != State::Leader) return false;
    if (peers_.size() <= 1)             return false;  // 单节点集群无意义

    leaderTransfersInitiated_.fetch_add(1);
    loop_.runInLoop([this, targetId]() mutable {
        if (state_.load() != State::Leader) return;

        // 解析目标：-1 = 自动挑选
        int target = targetId;
        if (target < 0) target = pickTransferTarget();
        if (target < 0 || target == id_) {
            LOG_WARN << "[Node " << id_ << "] Leader Transfer：无可用目标";
            return;
        }
        // 必须是已知 peer
        bool found = false;
        for (const auto &p : peers_)
            if (p.id == target) { found = true; break; }
        if (!found) {
            LOG_WARN << "[Node " << id_ << "] Leader Transfer：目标 " << target << " 不在集群中";
            return;
        }

        LOG_INFO << "[Node " << id_ << "] *** 主动 Leader Transfer：目标 -> Node "
                 << target << " ***";
        leadershipTransferTarget_ = target;
        transferDeadlineMs_       = nowMs() + 1000;  // 1s 超时

        // 1s 兜底超时：若到期仍未让贤成功，清状态恢复写入
        loop_.runAfter(1.0, [this, target] {
            if (state_.load() == State::Leader && leadershipTransferTarget_ == target) {
                LOG_WARN << "[Node " << id_ << "] Leader Transfer 超时：放弃让贤，恢复 Leader 服务";
                leadershipTransferTarget_ = -1;
                transferDeadlineMs_       = 0;
            }
        });

        // 立即评估一次：若 target 已追平日志，可直接发 TimeoutNow
        doTransferLeadership(target);
    });
    return true;
}

// 内部：检查 target 是否已追平日志，若已追平立即发 TimeoutNow，否则触发一次复制
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
        LOG_INFO << "[Node " << id_ << "] Leader Transfer：等待 target=" << targetId
                 << " 追平日志（match=" << matched << " last=" << lastIdx << "）";
        replicateLog(*targetPeer);
        return;
    }

    // 已追平：发 TimeoutNow，让 target 立刻起票
    LOG_INFO << "[Node " << id_ << "] Leader Transfer：target=" << targetId
             << " 已追平 lastIdx=" << lastIdx << "，发送 TimeoutNow";
    auto *client = getOrCreateClient(*targetPeer);
    uint64_t myTerm = currentTerm_.load();
    client->callAsync(
        "TimeoutNow", encodeTimeoutNowReq(myTerm), /*bypass=*/{},
        [this](bool ok, const std::string & /*resp*/) {
            // 不论 ack 成功与否，对方一旦起票成功 currentTerm 会涨，本节点
            // 在 handleRequestVote/handleAppendEntries 中会 becomeFollower。
            // 这里只做调试日志。
            if (!ok) LOG_WARN << "[Node " << id_ << "] TimeoutNow 发送失败（对端不可达）";
        });
}

// ── Follower 侧：收到 TimeoutNow，立即起票（跳过 election timeout 与 Pre-Vote）──
void RaftNode::handleTimeoutNow(const std::string &payload, const std::string & /*bypass*/,
                                RpcServer::Done done) {
    uint64_t senderTerm = decodeTimeoutNowReq(payload);
    loop_.runInLoop([this, senderTerm, done = std::move(done)]() mutable {
        // 仅当 sender term 与本节点 currentTerm 一致时接受
        // （避免过时 Leader 的 TimeoutNow 干扰更新的集群状态）
        if (senderTerm < currentTerm_.load()) {
            done(encodeTimeoutNowResp(currentTerm_.load(), false));
            return;
        }
        if (senderTerm > currentTerm_.load()) {
            // 不该发生（sender 是当前 Leader），但严谨处理：跟进 term 后拒绝本次
            becomeFollower(senderTerm);
            done(encodeTimeoutNowResp(currentTerm_.load(), false));
            return;
        }
        // term 一致：接受让贤指令
        LOG_INFO << "[Node " << id_ << "] *** 收到 TimeoutNow（来自 Leader），立即发起选举 ***";
        done(encodeTimeoutNowResp(currentTerm_.load(), true));

        // becomeCandidate 内部会 ++currentTerm_、persist、resetElectionTimer，
        // 然后由调用者负责发起 RequestVote。这里直接调用 runElection 走完流程。
        // 关键点：跳过 Pre-Vote —— Leader 已为我们做完了 quorum 可达 + 日志追平的检查。
        becomeCandidate();
        runElection();
    });
}

// ════════════════════════════════════════════════════════════════════════════
// §7  外部写入接口：propose
// ════════════════════════════════════════════════════════════════════════════

void RaftNode::proposeAndNotify(const std::string &cmd, std::function<void(bool, uint64_t)> done) {
    // 线程安全：实际工作在 loop_ 线程执行。
    // done(true, idx)  = 命令已应用到状态机，idx 是被分配的全局日志下标
    // done(false, idx) = 丢失 Leader 身份（idx 为该条目被分配的下标，未分配时为 0）
    loop_.runInLoop([this, cmd, done = std::move(done)]() mutable {
        if (state_.load() != State::Leader) {
            done(false, 0);
            return;
        }
        // Leader Transfer 进行中：拒绝新写避免追赶死循环（target 永远追不上 lastLogIndex）
        if (leadershipTransferTarget_ != -1) {
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

void RaftNode::propose(const std::string &cmd) {
    // propose 可以从任意线程调用（线程安全）。
    // 通过 runInLoop 把实际追加操作投递到 loop_ 线程，维持 Raft 状态的单线程访问。
    loop_.runInLoop([this, cmd] {
        if (state_.load() != State::Leader) {
            LOG_WARN << "[Node " << id_ << "] 提案被拒绝：当前节点非领导者"
                     << "（当前领导者ID=" << leaderId_ << ")";
            return;
        }
        // Leader Transfer 进行中：拒绝新写避免追赶死循环
        if (leadershipTransferTarget_ != -1) {
            LOG_WARN << "[Node " << id_ << "] 提案被拒绝：Leader Transfer 进行中 (target="
                     << leadershipTransferTarget_ << ")";
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

// ── 线性一致读：ReadIndex 协议 ───────────────────────────────────────────
// 不写日志，不增加 term，仅确认"我现在仍是合法 Leader"后即可安全读取状态机。
// 协议步骤：
//   1. 记录当前 commitIndex 为 readIndex
//   2. 向多数派发一轮心跳（下一次 heartbeatTick 自动完成，epoch 机制追踪）
//   3. 当该 epoch 的心跳被多数派 ack（readConfirmedEpoch_ 推进）且
//      lastApplied_ >= readIndex 时，cb(true) 返回给调用方
//   4. 任何中途失去 Leader 身份的情况都 cb(false)
void RaftNode::proposeRead(ReadCallback cb) {
    loop_.runInLoop([this, cb = std::move(cb)]() mutable {
        if (state_.load() != State::Leader) {
            cb(false);
            return;
        }
        uint64_t readIndex = commitIndex_.load();
        // 快速路径：当前 epoch 已被多数派确认，且状态机已应用到 readIndex
        if (readConfirmedEpoch_ >= readHeartbeatEpoch_
            && lastApplied_.load() >= readIndex) {
            cb(true);
            return;
        }
        // 慢速路径：等待下一次心跳 epoch 确认后兑现
        pendingReads_.push_back({readIndex, readHeartbeatEpoch_, std::move(cb)});
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

void RaftNode::takeSnapshot(uint64_t appliedIndex, const std::string &data) {
    // ── 关键：snapIdx 必须使用调用方在生成 data 时捕获的 appliedIndex ──────
    // 不能在 lambda 内取 lastApplied_.load()。因为 takeSnapshot 通常从
    // applyCallback 中调用，而 applyCallback 自身位于 applyCommitted 的
    // while 循环中；本 lambda 走 queueInLoop（loop_ 的 tid 在构造线程捕获，
    // 异步排队），真正执行时 lastApplied_ 已被推进到 appliedIndex+k，
    // 而 data 仍是 state@appliedIndex。这种错位会让 saveSnapshot 写入
    // (lastIndex=appliedIndex+k, data=state@appliedIndex)，重启后
    // snapshotApplyCallback 用旧数据恢复状态机但 lastApplied_ 跳到 +k，
    // 中间 k 条 apply 永久丢失（用户观测到的 "kv 总数变少" 即此原因）。
    loop_.runInLoop([this, appliedIndex, data] {
        uint64_t snapIdx = appliedIndex;
        if (snapIdx <= snapshotIndex_) return; // 没有新的 apply，无需压缩
        // 安全护栏：appliedIndex 不应超过 lastApplied_（调用方传错时拒绝）
        if (snapIdx > lastApplied_.load()) {
            LOG_WARN << "[Node " << id_ << "] takeSnapshot 拒绝：appliedIndex="
                     << snapIdx << " 超过 lastApplied=" << lastApplied_.load();
            return;
        }
        // 同样要保证 snapIdx 在当前 log_ 范围内（被覆盖写或截断的极端情况）
        if (snapIdx > lastLogIndex()) return;

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
