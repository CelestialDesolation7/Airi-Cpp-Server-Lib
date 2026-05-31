# Day 34 — Raft 持久化与快照恢复

## 目录

| 章节 | 内容 |
|------|------|
| [§1 引言](#1-引言) | 为什么需要持久化；必须持久化的三个字段；快照解决内存无限增长 |
| [§2 改进 A — RaftStorage 抽象接口](#2-改进-a--raftstorage-抽象接口) | 新建 `RaftStorage.h`：saveHardState/loadHardState/saveLog/loadLog/saveSnapshot/loadSnapshot |
| [§3 改进 B — FileStorage：文件系统持久化](#3-改进-b--filestorage文件系统持久化) | 新建 `FileStorage.h/cpp`：原子 rename 写入；JSON/NDJSON 格式；WAL 截断语义 |
| [§4 改进 C — RocksDBStorage：高性能持久化（可选）](#4-改进-c--rocksdbstorage高性能持久化可选) | 新建 `RocksDBStorage.h/cpp`：Big-endian key 编码；增量追加写；WriteBatch 原子性 |
| [§5 改进 D — RaftNode 持久化调用点](#5-改进-d--raftnode-持久化调用点) | 修改 `RaftNode.h/cpp`：`setStorage`/`persistHardState`/`persistLog`；各角色切换/写入点 |
| [§6 改进 E — start() 恢复路径](#6-改进-e--start-恢复路径) | 修改 `RaftNode.cpp`：三步恢复（HardState → 快照 → 日志）；`logAt` 全局-局部 index 偏移 |
| [§7 改进 F — 快照：takeSnapshot + sendInstallSnapshot](#7-改进-f--快照takesnapshot--sendinstallsnapshot) | 修改 `RaftNode.cpp`：日志压缩；`sendInstallSnapshot` 协程；`handleInstallSnapshot` Follower 侧 |
| [§8 整体运行时理解](#8-整体运行时理解) | 对象所有权图；场景 A（崩溃重启恢复）；场景 B（快照发送与 Follower 追赶）|
| [§9 各模块职责速查表](#9-各模块职责速查表) | 本日所有新增/修改函数一览 |
| [§10 工程化](#10-工程化) | CMakeLists.txt RocksDB 可选依赖；MCPP_HAS_ROCKSDB 宏 |
| [§11 验证](#11-验证) | 构建命令 + 崩溃恢复演示 + 快照触发演示 |
| [§12 局限与下一步](#12-局限与下一步) | FileStorage 全量写性能；Day35/36 计划 |

---

## 本日变更文件一览

| 文件 | 变更 | 核心改动 |
|------|------|---------|
| `src/include/raft/RaftStorage.h` | **新建** | 持久化抽象接口：HardState/Log/Snapshot 三类操作 |
| `src/include/raft/FileStorage.h` | **新建** | 文件持久化声明：`atomicWrite`；JSON/NDJSON 路径辅助 |
| `src/common/raft/FileStorage.cpp` | **新建** | 原子 rename；NDJSON 日志；WAL 截断语义 |
| `src/include/raft/RocksDBStorage.h` | **新建**（可选）| Big-endian 9 字节 log key；Log/Default 双 CF |
| `src/common/raft/RocksDBStorage.cpp` | **新建**（可选）| 增量追加写热路径；DeleteRange 截断；WriteBatch 原子 |
| `src/include/raft/RaftNode.h` | **修改** | 新增 `setStorage`/`takeSnapshot`/`handleInstallSnapshot`/`sendInstallSnapshot`；新增 `snapshotIndex_/snapshotTerm_/snapshotData_/storage_` |
| `src/common/raft/RaftNode.cpp` | **修改** | `start()` 三步恢复；`persistHardState`/`persistLog`；`handleInstallSnapshot`；`sendInstallSnapshot` 协程；`takeSnapshot`；`logAt` 偏移计算；`lastLogIndex` 修正 |
| `CMakeLists.txt` | **修改** | 可选 RocksDB 发现；`MCPP_HAS_ROCKSDB` 宏 |

---

## 1. 引言

### 1.1 为什么需要持久化

day33 运行正常的集群只要有任何节点重启，它就以 term=0、空日志重新加入。这破坏了 Raft 的两个关键保证：

**场景：重启节点的 term 丢失**

```
集群运行中，currentTerm=3，Node 1 投了票给 Node 0（votedFor=0）
Node 1 崩溃重启
Node 1 重启后 term=0，votedFor=-1
→ Node 1 可能在 term=3 再次给另一个候选人投票
→ 同一个 term 可能产生两个 Leader（脑裂）
```

Raft 论文明确要求持久化三个字段：

| 字段 | 必须持久化的原因 |
|------|----------------|
| `currentTerm` | 避免在同一 term 内两次投票，或以更低 term 接受旧 Leader 的 AppendEntries |
| `votedFor` | 每个 term 只能投一票——投票后必须记住，重启后不能"忘记"然后再投 |
| `log` | 已 committed 的命令绝不能丢失——重启后必须能重放所有已提交的条目 |

`commitIndex` 和 `lastApplied` **不需要**持久化——重启后从 0 开始重放日志到末尾即可恢复状态机。

### 1.2 为什么需要快照

day33 集群运行 30 天后，日志里有 10 万条命令，占用大量内存。更糟的是：某个 Follower 宕机 1 小时后重启，Leader 需要通过 AppendEntries 把 1 小时内的所有命令逐一发给它——可能需要数百轮 RPC，这段时间 Follower 的状态机不可用。

**快照（Snapshot）解决两个问题**：
1. **内存控制**：Leader 和 Follower 定期把状态机序列化为快照，截断快照点之前的日志，内存不会无限增长
2. **快速追赶**：落后太多的 Follower 收到 InstallSnapshot RPC，直接用快照恢复状态机，再追赶快照后少量日志，避免逐条 AppendEntries

---

## 2. 改进 A — RaftStorage 抽象接口

### 2.1 为什么需要这个接口

如果直接把 `fstream` 操作写进 RaftNode，以后换 RocksDB 时需要修改 Raft 核心逻辑，风险很高。接口分离后，RaftNode 不感知底层是文件还是 RocksDB；换后端时 RaftNode 的代码不变。同时，测试时可以用 `NullStorage`（空实现）或 `MemStorage`（内存 mock）替换，无需真实磁盘。

### 2.2 编码实现步骤

**新建 `src/include/raft/RaftStorage.h`，写入以下全部内容**

来自 [src/include/raft/RaftStorage.h](src/include/raft/RaftStorage.h)：

```cpp
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
```

`HardState` 嵌套结构体让 `term` 和 `votedFor` 总是一起读写——这防止"只持久化了 term 但 votedFor 丢失"的半持久化状态。`saveLog` 不包含哨兵（`log_[0]`）——哨兵是运行时辅助的，从 snapshot 的元数据中重建即可，不需要单独存储。

---

## 3. 改进 B — FileStorage：文件系统持久化

### 3.1 为什么需要原子写入

直接 `ofstream f(path)` 写到一半断电，文件会是半写状态（既不是旧内容也不是新内容）——下次 `loadHardState` 时 JSON 解析失败，节点无法启动。

**解法：先写 `.tmp`，再 `rename` 到目标文件。** POSIX 保证同目录内的 `rename` 是原子操作——崩溃只能看到旧文件或新文件，不会有中间状态。

```
正常情况：  write(path.tmp) → rename(path.tmp → path) → 旧文件原子替换
崩溃在 write 后 rename 前：path.tmp 残留，path 仍是旧版 → 安全
崩溃在 rename 后：path 是新版 → 安全
```

### 3.2 编码实现步骤

**新建 `src/include/raft/FileStorage.h`，写入以下全部内容**

来自 [src/include/raft/FileStorage.h](src/include/raft/FileStorage.h)：

```cpp
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
```

`atomicWrite` 是 `static`——它不访问任何成员状态，是纯工具函数，全部三类写入都复用它。

---

**新建 `src/common/raft/FileStorage.cpp`，写入以下全部内容**

来自 [src/common/raft/FileStorage.cpp](src/common/raft/FileStorage.cpp)：

```cpp
// FileStorage.cpp —— 基于文件的 Raft 持久化实现
//
// 设计要点：
//   写入：先写 .tmp（全量），再 rename 到目标文件。
//     POSIX 保证同目录内 rename 是原子操作：断电/崩溃只能看到旧版或新版，
//     不会看到部分写入的损坏文件。
//
//   读取：loadHardState / loadLog / loadSnapshot 仅在 start() 时调用一次，
//     之后全量在内存中操作，不再频繁读磁盘。
//
//   格式：人类可读的 JSON / NDJSON，方便 debug：
//     cat raft_state/node_0/hardstate.json
//     cat raft_state/node_0/log.ndjson
//   生产环境可替换为 protobuf / flatbuffers 后端。
//
//   容错：loadLog 遇到损坏行时截断到该行之前，丢弃后续数据
//     （与 WAL 的约定：最后一条不完整写的条目可以安全丢弃）。

#include "raft/FileStorage.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace raft {

FileStorage::FileStorage(std::string dataDir) : dir_(std::move(dataDir)) {
    fs::create_directories(dir_);
}

std::string FileStorage::hardStatePath() const { return dir_ + "/hardstate.json"; }
std::string FileStorage::logPath()       const { return dir_ + "/log.ndjson"; }
std::string FileStorage::snapshotPath()  const { return dir_ + "/snapshot.json"; }

void FileStorage::atomicWrite(const std::string &path, const std::string &content) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        f.flush();
        // 注：此处省略 fsync；rename 原子性已足够保证 crash-safety：
        // 崩溃后只会看到旧文件（rename 未完成）或新文件（rename 已完成）。
        // 若需绝对 durability 可在 f.flush() 后加 fsync(fileno(...))。
    }
    fs::rename(tmp, path);
}

// ── HardState ─────────────────────────────────────────────────────────────

void FileStorage::saveHardState(const HardState &hs) {
    json j = {{"term", hs.term}, {"votedFor", hs.votedFor}};
    atomicWrite(hardStatePath(), j.dump() + "\n");
}

FileStorage::HardState FileStorage::loadHardState() {
    std::ifstream f(hardStatePath());
    if (!f) return {};
    try {
        json j = json::parse(f);
        return {j.at("term").get<uint64_t>(), j.at("votedFor").get<int>()};
    } catch (...) {
        return {};
    }
}

// ── Log entries ───────────────────────────────────────────────────────────

void FileStorage::saveLog(const std::vector<LogEntry> &entries) {
    std::ostringstream oss;
    for (const auto &e : entries)
        oss << json(e).dump() << '\n';
    atomicWrite(logPath(), oss.str());
}

std::vector<LogEntry> FileStorage::loadLog() {
    std::vector<LogEntry> entries;
    std::ifstream f(logPath());
    if (!f) return entries;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        try {
            entries.push_back(json::parse(line).get<LogEntry>());
        } catch (...) {
            // 损坏的行：停止加载（WAL 约定：末尾不完整写可以丢弃）
            break;
        }
    }
    return entries;
}

// ── Snapshot ──────────────────────────────────────────────────────────────

void FileStorage::saveSnapshot(uint64_t lastIndex, uint64_t lastTerm,
                                const std::string &data) {
    json j = {{"lastIndex", lastIndex}, {"lastTerm", lastTerm}, {"data", data}};
    atomicWrite(snapshotPath(), j.dump() + "\n");
}

bool FileStorage::loadSnapshot(uint64_t &lastIndex, uint64_t &lastTerm,
                                std::string &data) {
    std::ifstream f(snapshotPath());
    if (!f) return false;
    try {
        json j  = json::parse(f);
        lastIndex = j.at("lastIndex").get<uint64_t>();
        lastTerm  = j.at("lastTerm").get<uint64_t>();
        data      = j.at("data").get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace raft
```

**NDJSON（Newline-Delimited JSON）格式**的好处：每行是一条完整 JSON，`cat log.ndjson` 直接 debug，`jq -c '.' log.ndjson` 可格式化查看。

**`loadLog` 的 `break`（不是 `continue`）**：这实现了 WAL（Write-Ahead Log）的标准约定——最后一条不完整写的条目可以安全丢弃，但它之后的所有条目都不可信（因为写入是顺序追加的，后面有内容意味着写入中途被中断）。如果用 `continue`，可能保留损坏条目后面的"幻象数据"，产生状态不一致。

**`loadHardState` 的双重容错**：`if (!f) return {}` + `catch (...) return {}`——文件不存在或 JSON 损坏，都当作全新节点（term=0, votedFor=-1）处理。`loadSnapshot` 返回 `bool` 而不是抛异常——因为"没有快照文件"是正常的启动状态（第一次启动时从未创建过快照），不是错误。

---

## 4. 改进 C — RocksDBStorage：高性能持久化（可选）

### 4.1 为什么需要 RocksDB 后端

FileStorage 每次 `saveLog` 都是全量覆盖写整个 log 文件。日志有 10000 条时，每次 `propose` 都要重写 10000 条，即使只新增了 1 条。

RocksDB 的 LSM-Tree 支持增量写：每次 `propose` 只追加 1 条（O(1)），不触碰已有条目。对长期运行的节点，性能差距是数量级级别的。

### 4.2 Key 编码设计

```
Key 格式（9 字节）：
  [0]    = 'l'（前缀，区分 log 条目与 HardState/Snapshot）
  [1..8] = Big-Endian uint64_t（全局 index）

示例：
  log[1]  → "l\x00\x00\x00\x00\x00\x00\x00\x01"
  log[2]  → "l\x00\x00\x00\x00\x00\x00\x00\x02"

三类 Key 的字典序不重叠：
  "hs"（HardState）  → 'h' = 0x68
  "l..."（log）      → 'l' = 0x6c
  "snap"（Snapshot） → 's' = 0x73
```

Big-Endian 保证 Key 的字典序 = index 的数值序，RocksDB 的范围扫描（`Seek` + `Next`）就能高效加载 `[startIdx, endIdx]` 范围的日志。

### 4.3 编码实现步骤

**新建 `src/include/raft/RocksDBStorage.h`**

来自 [src/include/raft/RocksDBStorage.h](src/include/raft/RocksDBStorage.h)（核心部分，整体被 `#ifdef MCPP_HAS_ROCKSDB` 包裹）：

```cpp
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
    explicit RocksDBStorage(const std::string &dir);
    ~RocksDBStorage() override;

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
```

`logBaseIndex_` 和 `lastPersistedLogIdx_` 是增量写的关键状态：`saveLog` 时比较新的 `entries` 末尾 index 和 `lastPersistedLogIdx_`，确定"追加"还是"截断重写"路径。这两个字段从 `loadLog`/`loadSnapshot` 中恢复，不需要额外持久化。

---

## 5. 改进 D — RaftNode 持久化调用点

### 5.1 三个调用时机

持久化调用分散在 RaftNode 的三处：

| 调用位置 | 持久化内容 | 理由 |
|---------|-----------|------|
| `becomeFollower` | `persistHardState()` | term 变化必须立即落盘 |
| `becomeCandidate` | `persistHardState()` | term++ + votedFor=self 必须同步落盘 |
| `handleRequestVote`（投票后）| `persistHardState()` | votedFor 变化必须立即落盘 |
| `handleAppendEntries`（追加后）| `persistLog()` | 日志变更后落盘 |
| `propose` / `becomeLeader`（no-op）| `persistLog()` | Leader 本地追加后立即落盘 |

### 5.2 编码实现步骤

**修改 `src/include/raft/RaftNode.h`，新增接口和成员变量**

在 `public` 接口区新增：

```cpp
// ── 持久化（必须在 start() 之前注册）────────────────────────────────
// 不调用 = 纯内存模式（崩溃后状态丢失）。
// 调用后，term/votedFor/log/snapshot 均自动落盘，重启后自动恢复。
void setStorage(std::unique_ptr<RaftStorage> storage) {
    storage_ = std::move(storage);
}

// ── 日志压缩（可从任意线程调用；实际执行在 loop_ 线程）──────────────
// data：状态机快照序列化结果（对 Raft 透明）。
// 快照点（snapIdx）在 loop_ 线程内读取 lastApplied_ 决定，不由外部传入。
// 调用后 log_ 中快照点之前的条目会被删除，对落后 peer 改发 InstallSnapshot。
void takeSnapshot(const std::string &data);
```

在私有成员变量区新增：

```cpp
// ── 快照状态（loop_ 线程访问）───────────────────────────────────────
// snapshotIndex_ 是 log_[0] 哨兵代表的全局 index：
//   全局 index i 对应 log_[i - snapshotIndex_]
// 初始 snapshotIndex_=0，log_[0].term=0（无快照的哨兵）。
uint64_t    snapshotIndex_{0};
uint64_t    snapshotTerm_{0};
std::string snapshotData_;  // 状态机快照数据（Raft 透明）

// ── 持久化后端（nullptr = 纯内存）───────────────────────────────────
std::unique_ptr<RaftStorage> storage_;
```

在私有方法区新增：

```cpp
FireAndForget sendInstallSnapshot(Peer peer);  // 快照传输协程

void persistHardState();  // 落盘 currentTerm_ + votedFor_
void persistLog();        // 落盘 log_[1..] （快照点之后的全量条目）

// 全局 index → log_ 局部下标（必须在 snapshotIndex_ <= idx <= lastLogIndex() 时调用）
LogEntry       &logAt(uint64_t globalIdx);
const LogEntry &logAt(uint64_t globalIdx) const;
```

---

**修改 `src/common/raft/RaftNode.cpp`，新增 `persistHardState`/`persistLog`/`logAt`**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§8 辅助函数段）：

```cpp
void RaftNode::persistHardState() {
    if (!storage_) return;
    storage_->saveHardState({currentTerm_.load(), votedFor_});
}

void RaftNode::persistLog() {
    if (!storage_) return;
    // log_[0] 是哨兵，不持久化；存储的是 snapshotIndex_+1 之后的真实条目
    storage_->saveLog(std::vector<LogEntry>(log_.begin() + 1, log_.end()));
}

uint64_t RaftNode::lastLogIndex() const {
    return snapshotIndex_ + static_cast<uint64_t>(log_.size()) - 1;
}

LogEntry &RaftNode::logAt(uint64_t idx) {
    return log_[idx - snapshotIndex_];
}
const LogEntry &RaftNode::logAt(uint64_t idx) const {
    return log_[idx - snapshotIndex_];
}
```

`lastLogIndex()` 变更了！day33 是 `log_.size() - 1`，day34 加了 `snapshotIndex_` 偏移：`snapshotIndex_ + log_.size() - 1`。引入快照后，`log_[0]` 不再代表全局 index=0，而是代表 `snapshotIndex_`。

`logAt(idx)` 是全局 index 到 log_ 局部下标的转换，`idx - snapshotIndex_` 是偏移量。调用前必须保证 `idx >= snapshotIndex_`（否则这个条目已被快照压缩，不在 `log_` 中）。

`persistLog` 不存储哨兵（`log_.begin() + 1`）——哨兵从 snapshot 的 `lastIncludedTerm` 中重建，不需要单独存储。

---

**修改各角色切换函数，在关键点调用 persistHardState**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§3 角色切换段）：

```cpp
void RaftNode::becomeFollower(uint64_t term) {
    // ...（其余代码不变）
    persistHardState();  // term 变化必须立即落盘
    resetElectionTimer();
}

void RaftNode::becomeCandidate() {
    currentTerm_.store(currentTerm_.load() + 1);
    state_.store(State::Candidate);
    votedFor_ = id_;
    currentElectionVotes_ = 1;
    // ...
    persistHardState();  // term++ 和 votedFor=self 必须同步落盘
    resetElectionTimer();
}
```

**修改 `handleRequestVote`，投票后 persistHardState**

```cpp
// handleRequestVote lambda 内（loop_ 线程）：
if (/* 三个投票条件满足 */) {
    grant     = true;
    votedFor_ = args.candidateId;
    resetElectionTimer();
    persistHardState();  // votedFor 变化必须立即落盘
}
```

**修改 `handleAppendEntries`，追加后 persistLog**

```cpp
// handleAppendEntries lambda 内（规则③ 完成后）：
// 规则③：幂等追加
// ...（追加逻辑）
// 日志变更后落盘（全量覆盖写）
persistLog();
```

**修改 `propose` 和 `becomeLeader`，追加 no-op/命令后 persistLog**

```cpp
// becomeLeader 内：
log_.push_back(LogEntry{currentTerm_.load(), ""});  // no-op
persistLog();

// propose 的 runInLoop lambda 内：
log_.push_back(LogEntry{currentTerm_.load(), cmd});
persistLog();  // Leader 本地追加后立即落盘
```

---

## 6. 改进 E — start() 恢复路径

### 6.1 为什么三步顺序不能调换

恢复必须严格按**HardState → Snapshot → Log** 的顺序，原因：

1. **HardState 先**：确立 `currentTerm_` 和 `votedFor_`，这两个值是其他一切的基础
2. **Snapshot 第二**：确立 `snapshotIndex_`，它决定了 `logAt()` 的偏移量。如果先加载 Log，`log_[i - snapshotIndex_]` 中的 `snapshotIndex_` 还是 0，索引计算会出错
3. **Log 最后**：基于已确立的 `snapshotIndex_` 偏移量，把快照点之后的条目追加到 `log_`

### 6.2 编码实现步骤

**修改 `src/common/raft/RaftNode.cpp`，在 `start()` 的 `loopThread_` 里的 `runInLoop` 中添加恢复逻辑**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§1 start() 段）：

```cpp
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
            // ... （其余 start() 逻辑）
        });
        loop_.runEvery(0.05, [this] { heartbeatTick(); });
        loop_.loop();
    });

    // (b) RPC server 线程
    rpcServerThread_ = std::thread([this] { rpcServer_.start(); });
}
```

**②中快照数据的去向**：重启时如果有快照，这里只重设哨兵、推进 `commitIndex_`/`lastApplied_` 到快照点，并把 `snapshotData_` 留在内存里。状态机的重建依赖上层在 `start()` 之后读取 `snapshotData_` 自行恢复——day34 还没有接入「快照安装即回调状态机」的实时通路，这部分留作后续工作。注意 `lastApplied_` 已推进到快照点，意味着 `applyCommitted` 不会再重放快照点之前的命令，所以状态机必须从 `snapshotData_` 里重建，否则读到的是空数据。

**③中 `log_.insert`**：`storage_->loadLog()` 返回的是快照点之后的条目（不含哨兵），直接追加到 `log_` 末尾即可。此时 `log_[0]` 已经是步骤②设置的新哨兵，`log_[1..]` 是快照后的真实条目。

---

## 7. 改进 F — 快照：takeSnapshot + sendInstallSnapshot

### 7.1 takeSnapshot 的快照点选择

`takeSnapshot(const std::string &data)` 不接收外部传入的 index——它通过 `runInLoop` 把工作排到 `loop_` 线程，在 lambda 内部读取 `lastApplied_.load()` 作为快照点 `snapIdx`。也就是说，「压缩到哪个 index」由真正执行压缩那一刻已 apply 的位置决定。

逻辑很短：

- `snapIdx = lastApplied_.load()`——以当前已 apply 的位置作为快照点；
- 若 `snapIdx <= snapshotIndex_`，说明自上次快照以来没有新的 apply，直接跳过（无需重复压缩）；
- 否则用 `logAt(snapIdx).term` 取出该点的 term 作为新哨兵的 `snapTerm`。

> 前瞻：day34 的 `takeSnapshot` 在 lambda 内部读取 `lastApplied_`，而 `data` 是调用方在更早时刻序列化的。两者之间存在时间差，`lastApplied_` 可能已经被推进，导致 `data` 描述的状态比 `snapIdx` 更旧。这个竞态在后续的某一天才会被处理（让调用方在生成 `data` 的同一时刻锁定快照点），此处先按当前实现理解即可。

### 7.2 log_ 的快照后偏移量

快照后，`log_` 内部结构改变：

```
快照前（snapshotIndex_=0，log_ 从全局 0 开始）：
  log_[0] = 哨兵   ← 全局 index 0
  log_[1] = cmd1   ← 全局 index 1
  log_[2] = cmd2   ← 全局 index 2
  log_[3] = cmd3   ← 全局 index 3

takeSnapshot 在 lastApplied_=2 时执行后（snapshotIndex_=2）：
  log_[0] = 新哨兵（term = log_[2 - 0].term = cmd2 的 term）← 全局 index 2
  log_[1] = cmd3   ← 全局 index 3

  logAt(3) = log_[3 - 2] = log_[1] ✓
  lastLogIndex() = snapshotIndex_(2) + log_.size()(2) - 1 = 3 ✓
```

### 7.3 编码实现步骤

**修改 `src/common/raft/RaftNode.cpp`，新增 `takeSnapshot`**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§7b 日志压缩段）：

```cpp
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
```

"保留快照点之后的条目"非常重要：`log_[keepFrom..]` 是快照点之后已有的日志（可能是已复制但未提交的新条目）。如果不保留，这些条目会丢失，Leader 下次 `replicateLog` 就找不到它们，需要重新 propose——但客户端可能已经等待这些条目的 apply 回调，造成超时。

---

**修改 `src/common/raft/RaftNode.cpp`，实现 `handleInstallSnapshot`（Follower 侧）**

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§2b InstallSnapshot handler 段）：

```cpp
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
```

**`args.lastIncludedIndex <= snapshotIndex_` 检查**：如果收到的快照比我现有的更旧（可能是网络延迟导致的旧包），直接忽略，不能让 `snapshotIndex_` 倒退（会破坏 `logAt` 的偏移计算）。

**`commitIndex_` 和 `lastApplied_` 都推进到快照点**：快照点之前的所有命令都已经"通过"了（包含在快照数据里），不需要再逐条 apply。

**Follower 侧状态机如何重建**：day34 的 `handleInstallSnapshot` 只把 `snapshotData_` 落盘，并没有在这里直接回调状态机（没有实时的快照-apply 通路）。Follower 的 KV 状态是在下次 `start()` 的恢复路径里、由状态机重新读取 `snapshotData_` 来重建的——把「快照安装即回调状态机」接成实时通路属于后续工作。

---

**修改 `src/common/raft/RaftNode.cpp`，新增 `sendInstallSnapshot` 协程（Leader 侧）**

同时修改 `replicateLog`：当 `ni <= snapshotIndex_` 时跳转到 `sendInstallSnapshot`：

```cpp
FireAndForget RaftNode::replicateLog(Peer peer) {
    if (state_.load() != State::Leader) co_return;

    uint64_t ni = nextIndex_.count(peer.id) ? nextIndex_[peer.id] : lastLogIndex() + 1;
    if (ni < 1) ni = 1;

    // Day34：若 peer 需要的条目已被快照压缩，改发 InstallSnapshot
    if (ni <= snapshotIndex_) {
        sendInstallSnapshot(peer);
        co_return;
    }
    // ... （其余 replicateLog 逻辑不变）
}
```

来自 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)（§5b 快照传输协程段）：

```cpp
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
```

快照传输超时设置为 1000ms（而不是 replicateLog 的 100ms）——快照数据可能很大（几十 MB），需要更长的传输时间。快照安装成功后，把 `matchIndex_[peer.id]` 推到快照点，`nextIndex_[peer.id]` 推到快照点之后，后续的 `replicateLog` 可以继续用 AppendEntries 追赶快照后的少量日志。

---

## 8. 整体运行时理解

### 8.1 对象所有权与线程归属

```
RaftNode（T_main 构造，loopThread_ 运行）
 │
 ├── storage_（unique_ptr<RaftStorage>）← start() 之前由外部注入
 │    ├── FileStorage（文件系统）
 │    │    └── dir_（数据目录路径，构造时确定）
 │    └── RocksDBStorage（可选，RocksDB 实例）
 │
 └── loop_（T_raft）
      ├── snapshotIndex_ / snapshotTerm_ / snapshotData_  ← loop_ 独占
      ├── log_[]                                           ← loop_ 独占
      └── commitIndex_ / lastApplied_（atomic）            ← 任意线程可读

外部（T_main / raft_demo 主循环 / applyCallback_）
  └── takeSnapshot(data)
       └── runInLoop ──▶ T_raft（loop_）执行实际压缩
                         （快照点 snapIdx 在 loop_ 内读 lastApplied_ 决定）
```

**持久化的线程安全**：`persistHardState` / `persistLog` 总是在 `loop_` 线程调用（它们在各角色切换函数和 handler lambda 里），`storage_->save*` 调用也因此天然无竞争——`storage_` 只在 `loop_` 线程被访问（除了构造时的 `setStorage`，那在 `start()` 之前，没有竞争）。

---

### 8.2 场景 A — 崩溃重启恢复

**场景设定**：3 节点集群，Node 0 是 Leader（term=3），`commitIndex=50, lastApplied=50`。Node 0 已持久化：`hardstate.json={term:3, votedFor:0}`，`log.ndjson` 有 50 条非空条目（index 1-50，不含 no-op）。Node 0 进程崩溃，重启。

---

#### 第 1 步：start() 触发持久化恢复

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`start()` 的 `runInLoop` 段：

```cpp
// ① 恢复 HardState
auto hs = storage_->loadHardState();
// → {term: 3, votedFor: 0}
currentTerm_.store(3);
votedFor_ = 0;
```

**此刻状态快照**：
```
currentTerm_ = 3
votedFor_    = 0（不会再给其他人投票）
snapshotIndex_ = 0（尚未恢复快照）
log_           = [{t=0,""}]（只有初始哨兵）
```

---

#### 第 2 步：恢复快照（本场景无快照）

```cpp
// ② 尝试加载快照
if (storage_->loadSnapshot(snapIdx, snapTerm, snapData)) {
    // 本场景无快照，返回 false → 跳过
}
```

---

#### 第 3 步：恢复日志

```cpp
// ③ 恢复快照点之后的日志条目
auto entries = storage_->loadLog();
// → 50 条 LogEntry（index 1-50）
log_.insert(log_.end(), entries.begin(), entries.end());
// log_ = [{t=0,""}, {t=3,"cmd1"}, ..., {t=3,"cmd50"}]（51 项，index 0-50）

LOG_INFO << "[Node 0] 从持久化存储恢复：term=3 votedFor=0 snapshotIndex=0 logSize=50 条";
```

**此刻状态快照**：
```
currentTerm_   = 3
votedFor_      = 0
log_.size()    = 51（哨兵 + 50 条真实命令）
lastLogIndex() = snapshotIndex_(0) + log_.size()(51) - 1 = 50
commitIndex_   = 0（尚未恢复，从 0 开始）
lastApplied_   = 0（尚未恢复）
```

---

#### 第 4 步：Node 0 重新加入集群，重置选举计时器

`resetElectionTimer()` 注册 150~300ms 的选举超时。在此期间，当前 Leader（已被其他节点重新选出）会给 Node 0 发心跳/AppendEntries。

Node 0 的 `handleAppendEntries` 收到心跳：
- `args.term >= currentTerm_=3` ✓ → `state_=Follower`，`resetElectionTimer`
- 通过 `replicateLog` 把缺失的 `commitIndex` 信息同步过来（`leaderCommit`）

**此刻状态快照（Node 0，几个心跳后）**：
```
commitIndex_ = 50（由 leaderCommit 推进）
lastApplied_ = 50（applyCommitted 重放 50 条命令，触发 applyCallback_）
状态机        已重建到 index=50 的状态
```

---

### 8.3 场景 B — 快照发送与 Follower 追赶

**场景设定**：3 节点集群，Leader 已在 `lastApplied_=100` 时触发过 `takeSnapshot(data)`（snapshotIndex_=100）。Node 2 宕机前只有 log 到 index=30，需要追赶。Leader 下次 `replicateLog(peer=Node2)` 时 `nextIndex_[2]=31`，但 `31 <= snapshotIndex_=100`。

---

#### 第 1 步：replicateLog 检测到需要发快照

打开 [src/common/raft/RaftNode.cpp](src/common/raft/RaftNode.cpp)，`replicateLog`：

```cpp
uint64_t ni = nextIndex_[2];  // = 31（上次记录）
if (ni <= snapshotIndex_) {  // 31 <= 100 ✓
    sendInstallSnapshot(peer2);  // 跳转到快照协程
    co_return;
}
```

---

#### 第 2 步：sendInstallSnapshot 发出快照，协程挂起

```cpp
FireAndForget RaftNode::sendInstallSnapshot(Peer peer) {
    // snapshotIndex_=100, snapshotTerm_=2, snapshotData_=<状态机序列化>
    InstallSnapshotArgs args{...lastIncludedIndex=100, lastIncludedTerm=2, data=...};

    LOG_INFO << "[Node 0] 向节点 2 发送快照 lastIndex=100";

    auto [ok, respJson] = co_await callAsyncCo("InstallSnapshot", ..., 1000ms);
    // 协程挂起，等待 Node 2 安装快照（可能需要几百 ms）
```

---

#### 第 3 步：Node 2 安装快照

Node 2 的 `handleInstallSnapshot`（T_raft，Node 2）：

```cpp
// args.lastIncludedIndex=100 > snapshotIndex_=0 ✓（不忽略）

// 保留 log_[101..] 的条目（Node 2 没有，retained=[]）
log_.clear();
log_.push_back(LogEntry{t=2, ""});  // 新哨兵，term=lastIncludedTerm=2

snapshotIndex_ = 100;
snapshotTerm_  = 2;
snapshotData_  = <数据>;
commitIndex_.store(100);
lastApplied_.store(100);

storage_->saveSnapshot(100, 2, data);
persistLog();     // log_ 只有新哨兵，保存空日志
persistHardState();
// 注：day34 这里不回调状态机；snapshotData_ 已落盘，状态机在下次 start() 恢复
done(reply)
```

**此刻状态快照（Node 2）**：
```
snapshotIndex_ = 100
log_           = [{t=2,""}]（只有哨兵）
commitIndex_   = 100
lastApplied_   = 100
状态机          快照数据 snapshotData_ 已落盘，留待下次 start() 重建
```

---

#### 第 4 步：sendInstallSnapshot 协程恢复，更新 matchIndex_/nextIndex_

```cpp
// ok=true, reply.term=2 <= currentTerm_=2 ✓
if (snapshotIndex_(100) > matchIndex_[2](0)) {
    matchIndex_[2] = 100;
    nextIndex_[2]  = 101;
}
```

后续 `replicateLog(peer2)` 从 index=101 开始，`ni=101 > snapshotIndex_=100`，走正常 AppendEntries 路径，把 101 之后的少量日志追赶过来。

---

#### 调用链总结

```
T_raft（Leader）                  T_raft（Node 2）
    │                                  │
    │ replicateLog(peer2)              │
    │   ni=31 <= snapshotIndex_=100   │
    │   sendInstallSnapshot(peer2) ─── co_await ───TCP→
    │   协程挂起（等待 1000ms 超时）   │ handleInstallSnapshot
    │                                  │ log_.clear()
    │                                  │ log_.push_back(哨兵)
    │                                  │ snapshotIndex_=100
    │                                  │ commitIndex_=100
    │                                  │ saveSnapshot(snapshotData_ 落盘)
    │ ◀──reply─────────────────────────│
    │ matchIndex_[2]=100               │
    │ nextIndex_[2]=101                │
    │ [下次 replicateLog 从 101 开始]  │
```

---

## 9. 各模块职责速查表

| 模块/函数 | 所在线程 | 调用时机 | 职责一句话 |
|-----------|---------|---------|-----------|
| `RaftStorage::saveHardState` | T_raft（通过 persistHardState）| term/votedFor 变化时 | 原子写 term+votedFor 到持久化后端 |
| `RaftStorage::saveLog` | T_raft（通过 persistLog）| 日志追加/截断后 | 原子写 log_[1..] 到持久化后端 |
| `FileStorage::atomicWrite` | T_raft | save* 方法内部 | 先写 .tmp 再 rename，POSIX 原子替换 |
| `FileStorage::loadLog` | T_raft（start() 时）| 恢复路径 | NDJSON 逐行读，末尾损坏行截断（WAL 语义）|
| `RaftNode::persistHardState` | T_raft | becomeFollower/Candidate；投票后 | 调 storage_->saveHardState（nullptr 则跳过）|
| `RaftNode::persistLog` | T_raft | handleAppendEntries/propose/becomeLeader/takeSnapshot | 调 storage_->saveLog 不含哨兵 |
| `RaftNode::start()` 恢复段 | T_raft | start() 内 runInLoop | 三步恢复：HardState → Snapshot → Log |
| `RaftNode::logAt(idx)` | T_raft | 任何需要访问 log_[n] 的地方 | 全局 index → log_ 局部下标（减 snapshotIndex_ 偏移）|
| `RaftNode::lastLogIndex()` | T_raft | 任何计算 log 末尾的地方 | `snapshotIndex_ + log_.size() - 1`（含偏移修正）|
| `RaftNode::takeSnapshot` | 任意线程 → T_raft | 上层状态机每 N 条 apply 触发 | 截断 log_，重置哨兵，持久化快照+日志 |
| `RaftNode::sendInstallSnapshot` | T_raft（协程帧在堆）| replicateLog 发现 ni <= snapshotIndex_ 时 | 向落后 Follower 发整个快照；成功后更新 matchIndex_/nextIndex_ |
| `RaftNode::handleInstallSnapshot` | T_raft_rpc → T_raft | 收到 InstallSnapshot 帧时 | 重建 log_；推进 commitIndex_/lastApplied_；落盘 snapshotData_（状态机在下次 start() 重建）|

---

## 10. 工程化

**CMakeLists.txt 变更（RocksDB 可选依赖）**：

```cmake
# 尝试发现 RocksDB（可选）
find_library(ROCKSDB_LIB rocksdb PATHS /usr/local/lib /opt/homebrew/lib)
if(ROCKSDB_LIB)
    message(STATUS "Found RocksDB: ${ROCKSDB_LIB}")
    target_compile_definitions(NetLib PUBLIC MCPP_HAS_ROCKSDB=1)
    target_link_libraries(NetLib PUBLIC ${ROCKSDB_LIB})
else()
    message(STATUS "RocksDB not found, RocksDBStorage disabled")
    # 从源文件列表中排除 RocksDBStorage.cpp
    list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*/RocksDBStorage\\.cpp")
endif()
```

`RocksDBStorage.cpp` 整体被 `#ifdef MCPP_HAS_ROCKSDB ... #endif` 包裹——未安装 RocksDB 时编译单元为空文件，不报错。使用方：

```cpp
#ifdef MCPP_HAS_ROCKSDB
    node.setStorage(std::make_unique<raft::RocksDBStorage>(dataDir));
#else
    node.setStorage(std::make_unique<raft::FileStorage>(dataDir));
#endif
```

---

## 11. 验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target raft_demo -j

# 启动 3 节点集群（开启持久化，每 5 条触发快照）
./build/examples/raft_demo --id 0 --nodes 3 --persist --snapshot-every 5 --propose-interval 500 &
./build/examples/raft_demo --id 1 --nodes 3 --persist &
./build/examples/raft_demo --id 2 --nodes 3 --persist &

# 等待约 5 秒（propose 10 条命令，触发 2 次快照压缩）
sleep 5

# 查看持久化文件
cat raft_state/node_0/hardstate.json    # {"term":1,"votedFor":0}
cat raft_state/node_0/log.ndjson        # 只剩快照后的少量条目
cat raft_state/node_0/snapshot.json     # {"lastIndex":10,"lastTerm":1,"data":"snap@10"}

# Kill Leader，等新 Leader 选出
kill %1; sleep 1

# 重启 Node 0，观察从磁盘恢复
./build/examples/raft_demo --id 0 --nodes 3 --persist --snapshot-every 5 &

# 预期日志：
# [Node 0] 从持久化存储恢复：term=1 votedFor=0 snapshotIndex=10 logSize=? 条
# [Node 0] Follower  term=1  logSize=...  commit=...  applied=...
```

验证快照发送：

```bash
# 停一个 Follower，继续 propose 15 条以触发快照
kill %3; sleep 3
# 重启 Follower，观察 InstallSnapshot
./build/examples/raft_demo --id 2 --nodes 3 --persist &
# 预期日志：
# [Node 0] 向节点 2 发送快照 lastIndex=15
# [Node 2] 安装快照 lastIndex=15 lastTerm=1
```

---

## 12. 局限与下一步

| 局限 | 描述 |
|------|------|
| **FileStorage 全量写**：每次 `persistLog` 重写整个日志文件，日志长时性能差 | RocksDBStorage 已解决（增量追加），但需要安装 RocksDB |
| **快照数据是字符串**：`raft_demo` 里 `snapshotData` 是 `"snap@N"`，真实系统应是状态机的序列化结果 | Day36 的 `KvStateMachine.serialize()` 用 Protobuf 序列化 KV map，传给 `takeSnapshot` |
| **RPC payload 是 JSON**：大 value 时 base64 膨胀 33%，多次拷贝 | Day37 迁移到 Protobuf + bypass 旁路透传，零拷贝传输 |

接下来 **Day35** 将实现 L7 反向代理与负载均衡（独立模块，与 Raft 解耦）；**Day36** 将把 Raft 引擎接上应用层，搭建完整的分布式 KV 集群。
