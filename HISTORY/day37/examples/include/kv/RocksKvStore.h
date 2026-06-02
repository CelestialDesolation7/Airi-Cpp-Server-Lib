#pragma once
#ifdef MCPP_HAS_ROCKSDB

// RocksKvStore —— 基于 RocksDB 的持久化 KV 引擎
//
// 接口：
//   put / get / del   —— 基本 CRUD，对应 RocksDB Put/Get/Delete
//
// 快照（供 Raft 状态机使用）：
//   createCheckpoint(baseDir) → checkpointDir
//     利用 RocksDB Checkpoint API（SST 文件硬链接，O(1)），
//     返回的目录路径可作为 snapshotData 写入 Raft 快照。
//     同一台机器的多个节点可直接使用该路径恢复（单机 Demo）。
//
//   restoreCheckpoint(checkpointDir)
//     关闭 DB → 替换数据目录 → 重新打开。
//     生产环境需先通过网络传输 checkpoint 文件，单机 Demo 可直接引用路径。

#include <rocksdb/db.h>
#include <string>

class RocksKvStore {
  public:
    explicit RocksKvStore(const std::string &dbPath);
    ~RocksKvStore();

    RocksKvStore(const RocksKvStore &)            = delete;
    RocksKvStore &operator=(const RocksKvStore &) = delete;

    void put(const std::string &key, const std::string &value);
    /// 返回 false 表示 key 不存在
    bool get(const std::string &key, std::string &value) const;
    void del(const std::string &key);

    /// 创建 RocksDB Checkpoint（硬链接，O(1)），返回 checkpoint 目录路径
    std::string createCheckpoint(const std::string &baseDir);

    /// 从 checkpointDir 恢复：关闭 → 替换数据目录 → 重新打开
    void restoreCheckpoint(const std::string &checkpointDir);

  private:
    void openDB();

    std::string     dbPath_;
    rocksdb::DB    *db_{nullptr};
    rocksdb::Options options_;
};

#endif // MCPP_HAS_ROCKSDB
