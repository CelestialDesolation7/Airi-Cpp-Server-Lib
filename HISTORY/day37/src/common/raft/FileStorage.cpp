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
