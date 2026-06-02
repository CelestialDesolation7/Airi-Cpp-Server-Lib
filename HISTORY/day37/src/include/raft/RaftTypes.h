#pragma once
//
// RaftTypes.h —— Raft 协议的 C++ 内存数据结构
//
// 序列化策略（双轨设计）：
//   · RPC 线路格式：Protobuf（所有 encode/decode 在 RaftNode.cpp 匿名命名空间）
//     → 高效紧凑，适合高频心跳与日志复制
//   · 磁盘持久化：nlohmann/json（仅 LogEntry 和 HardState）
//     → 人类可读，cat raft_state/node_0/log.ndjson 即可 debug
//
// 因此：
//   · 只有 LogEntry 保留 NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE（供 FileStorage
//     和 RocksDBStorage 使用）
//   · 其余结构体（RequestVoteArgs 等）只是内存 DTO，不需要 JSON 宏
//

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace raft {

enum class State { Follower, Candidate, Leader };

// LogEntry 保留 NLOHMANN 宏：仅用于磁盘持久化（NDJSON 格式）
// RPC 线路上的 LogEntry 通过 raft.proto 中的 message LogEntry 编码
struct LogEntry {
    uint64_t    term{0};
    std::string cmd;   // 完整命令字符串（持久化时不做 cmd/value 拆分）
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LogEntry, term, cmd)

struct RequestVoteArgs {
    uint64_t term{0};
    int      candidateId{-1};
    uint64_t lastLogIndex{0};
    uint64_t lastLogTerm{0};
    // preVote=true：预投票阶段，接收方不更新 term/votedFor，
    // 只判断"若候选人以 term 发起真实选举，我是否会投票给它"。
    // 用于防止被网络分区隔离的节点不断递增 term 来干扰健康集群。
    bool     preVote{false};
};

struct RequestVoteReply {
    uint64_t term{0};
    bool     voteGranted{false};
};

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

// InstallSnapshot：Leader 把完整状态机快照发给严重落后的 Follower。
// 触发条件：Follower 需要的 nextIndex 已被 Leader 的快照压缩（不在 log_ 中），
//   此时无法用 AppendEntries 补齐，必须先用快照把 Follower 的状态机拉到快照点，
//   再继续用 AppendEntries 追赶快照点之后的条目。
struct InstallSnapshotArgs {
    uint64_t    term{0};
    int         leaderId{-1};
    uint64_t    lastIncludedIndex{0};  // 快照覆盖到的最后一条日志 index
    uint64_t    lastIncludedTerm{0};   // 该条日志的 term（用于重置哨兵）
    std::string data;                  // 状态机快照，由上层序列化，Raft 透明传输
};

struct InstallSnapshotReply {
    uint64_t term{0};  // Follower 当前 term，供 Leader 检测僵尸状态
};

inline const char* stateName(State s) {
    switch (s) {
        case State::Follower:  return "Follower";
        case State::Candidate: return "Candidate";
        case State::Leader:    return "Leader";
    }
    return "?";
}

// ── NodeAnnounce（已在 RaftNode.cpp 内联构造，此处不再重复声明）──────────

} // namespace raft
