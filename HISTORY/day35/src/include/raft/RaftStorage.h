#pragma once
// RaftStorage —— Raft 持久化抽象接口
//
// Raft 需要在崩溃重启后恢复三类状态：
//
//   ① HardState（term + votedFor）
//      - 每次 term 递增或 votedFor 改变后必须「立即」落盘。
//      - 原因：若 term/votedFor 丢失，节点重启后可能在同一 term 投两票，
//        或以更低 term 接受旧 Leader 的 AppendEntries，破坏一致性。
//
//   ② Log entries（不含哨兵 log_[0]）
//      - 所有已 propose 但未压缩的条目，重启后需完整恢复。
//      - saveLog 采用全量覆盖写：实现简单、正确性高；
//        面向写吞吐优化时可改为增量 append-only WAL。
//
//   ③ Snapshot（状态机快照 + 压缩元数据）
//      - lastIncludedIndex/Term 是新 log_[0] 哨兵的 term 来源。
//      - data 对 Raft 透明，由上层状态机序列化后传入。
//
// 接口职责分离：RaftNode 只依赖此抽象，不关心底层实现。
// 测试时可用 NullStorage（空实现）或 MemStorage（内存 mock）替换 FileStorage。

#include "raft/RaftTypes.h"
#include <cstdint>
#include <string>
#include <vector>

namespace raft {

class RaftStorage {
  public:
    struct HardState {
        uint64_t term{0};
        int      votedFor{-1};
    };

    virtual ~RaftStorage() = default;

    // ── HardState ────────────────────────────────────────────────────────
    virtual void      saveHardState(const HardState &hs) = 0;
    virtual HardState loadHardState()                    = 0;

    // ── Log entries（不含哨兵；快照压缩后只保留快照点之后的条目）──────
    // saveLog 全量覆盖写：先写 .tmp 再 rename，崩溃安全
    virtual void                  saveLog(const std::vector<LogEntry> &entries) = 0;
    virtual std::vector<LogEntry> loadLog()                                     = 0;

    // ── Snapshot ─────────────────────────────────────────────────────────
    // saveSnapshot：原子替换旧快照
    // loadSnapshot：返回 false 表示还没有任何快照
    virtual void saveSnapshot(uint64_t lastIndex, uint64_t lastTerm,
                               const std::string &data)                              = 0;
    virtual bool loadSnapshot(uint64_t &lastIndex, uint64_t &lastTerm,
                               std::string &data)                                    = 0;
};

} // namespace raft
