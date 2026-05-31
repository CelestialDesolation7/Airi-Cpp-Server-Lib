#pragma once
// FileStorage —— 基于普通文件的 RaftStorage 实现
//
// 文件布局（均在 dataDir/ 下）：
//   hardstate.json   —— {"term": N, "votedFor": N}
//   log.ndjson       —— 每行一条 LogEntry JSON（Newline-Delimited JSON，可 cat 查看）
//   snapshot.json    —— {"lastIndex": N, "lastTerm": N, "data": "..."}
//
// 写入策略（崩溃安全）：
//   先将内容写到 path.tmp，再 rename 到目标文件。
//   POSIX 保证同目录内 rename 是原子操作：崩溃只能看到旧文件或新文件，
//   不会看到部分写入的半截文件。

#include "raft/RaftStorage.h"
#include <string>

namespace raft {

class FileStorage : public RaftStorage {
  public:
    explicit FileStorage(std::string dataDir);

    void      saveHardState(const HardState &hs) override;
    HardState loadHardState() override;

    void                  saveLog(const std::vector<LogEntry> &entries) override;
    std::vector<LogEntry> loadLog() override;

    void saveSnapshot(uint64_t lastIndex, uint64_t lastTerm,
                      const std::string &data) override;
    bool loadSnapshot(uint64_t &lastIndex, uint64_t &lastTerm,
                      std::string &data) override;

  private:
    std::string dir_;

    std::string hardStatePath() const;
    std::string logPath()       const;
    std::string snapshotPath()  const;

    // 先写 path.tmp 再 rename（原子替换，POSIX 保证）
    static void atomicWrite(const std::string &path, const std::string &content);
};

} // namespace raft
