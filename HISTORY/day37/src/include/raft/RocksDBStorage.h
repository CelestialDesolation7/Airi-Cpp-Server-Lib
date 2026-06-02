#pragma once
#ifdef MCPP_HAS_ROCKSDB

// RocksDBStorage —— 基于 RocksDB 的 Raft 持久化后端
//
// 单 ColumnFamily（default）+ "log" CF 设计：
//
//   Default CF  key="hs"              → JSON { term, votedFor }
//               key="snap"            → JSON { lastIndex, lastTerm, data }
//   Log CF      key=<9 bytes>         → JSON { term, cmd }
//                    [0]='l' + [1..8]=big-endian(globalIndex)
//
// 相比 FileStorage 的改进：
//   ① 追加写（propose 热路径）只 Put 1 条，而非覆盖整个 log 文件 —— O(1)
//   ② 截断写 DeleteRange + 重写，原子 WriteBatch，无需 .tmp rename
//   ③ takeSnapshot 后 DeleteRange 清理旧条目，WAL 不会无限膨胀

#include "raft/RaftStorage.h"
#include <rocksdb/db.h>
#include <string>
#include <vector>

namespace raft {

class RocksDBStorage : public RaftStorage {
  public:
    /// dir: RocksDB 数据目录（不存在时自动创建）
    explicit RocksDBStorage(const std::string &dir);
    ~RocksDBStorage() override;

    // 禁止拷贝
    RocksDBStorage(const RocksDBStorage &)            = delete;
    RocksDBStorage &operator=(const RocksDBStorage &) = delete;

    void      saveHardState(const HardState &hs) override;
    HardState loadHardState() override;

    /// 增量写：追加时只写新条目，截断时 DeleteRange + 重写
    void                  saveLog(const std::vector<LogEntry> &entries) override;
    std::vector<LogEntry> loadLog() override;

    void saveSnapshot(uint64_t lastIndex, uint64_t lastTerm,
                      const std::string &data) override;
    bool loadSnapshot(uint64_t &lastIndex, uint64_t &lastTerm,
                      std::string &data) override;

  private:
    void openDB();

    /// 9 字节 log key：'l' + big-endian uint64_t
    static std::string  encodeLogKey(uint64_t globalIdx);
    static bool         isLogKey(const rocksdb::Slice &s);
    static uint64_t     decodeLogKey(const rocksdb::Slice &s);

    std::string  dir_;
    rocksdb::DB *db_{nullptr};

    // "log" CF handle（由我们管理生命周期，delete before db_）
    rocksdb::ColumnFamilyHandle *logCF_{nullptr};

    // 增量写状态（仅在内存中跟踪，从 loadLog/loadSnapshot 中恢复）
    uint64_t logBaseIndex_{0};        // entries[0] 对应的全局 index = snapshotIndex + 1
    uint64_t lastPersistedLogIdx_{0}; // 最近一次 saveLog 写入的最大全局 index
    bool     logEmpty_{true};
};

} // namespace raft

#endif // MCPP_HAS_ROCKSDB
