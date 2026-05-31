#ifdef MCPP_HAS_ROCKSDB

#include "raft/RocksDBStorage.h"
#include "log/Logger.h"
#include <nlohmann/json.hpp>
#include <rocksdb/write_batch.h>
#include <filesystem>
#include <stdexcept>

using nlohmann::json;
namespace fs = std::filesystem;

namespace raft {

// ── Key 编码 ──────────────────────────────────────────────────────────────────────────────
// log key 格式：1 字节前缀 'l' + 8 字节 big-endian globalIndex（共 9 字节）
// 字典序 == 数值序，满足 RocksDB range scan 要求。
// 'h' (0x68) < 'l' (0x6C) < 'm' (0x6D) < 's' (0x73)
// 因此 "hs" < "l..." < "snap"，范围删除不会误伤 meta key。

std::string RocksDBStorage::encodeLogKey(uint64_t idx) {
    std::string k(9, '\0');
    k[0] = 'l';
    for (int i = 7; i >= 0; --i)
        k[1 + (7 - i)] = static_cast<char>((idx >> (8 * i)) & 0xFF);
    return k;
}

bool RocksDBStorage::isLogKey(const rocksdb::Slice &s) {
    return s.size() == 9 && static_cast<unsigned char>(s[0]) == 'l';
}

uint64_t RocksDBStorage::decodeLogKey(const rocksdb::Slice &s) {
    uint64_t idx = 0;
    for (int i = 1; i <= 8; ++i)
        idx = (idx << 8) | static_cast<uint8_t>(s[i]);
    return idx;
}

// ── 打开 DB ──────────────────────────────────────────────────────────────────────────────
RocksDBStorage::RocksDBStorage(const std::string &dir) : dir_(dir) {
    fs::create_directories(dir_);
    openDB();
}

void RocksDBStorage::openDB() {
    rocksdb::Options opts;
    opts.create_if_missing              = true;
    opts.create_missing_column_families = true;

    // 检查是否已有 "log" CF（DB 已存在时）
    std::vector<std::string> existingCFs;
    rocksdb::Status          s = rocksdb::DB::ListColumnFamilies(opts, dir_, &existingCFs);

    bool hasLogCF = false;
    if (s.ok()) {
        for (const auto &cf : existingCFs)
            if (cf == "log") { hasLogCF = true; break; }
    }

    std::unique_ptr<rocksdb::DB> dbUptr;

    if (hasLogCF) {
        // 已有 "log" CF：用多 CF 方式打开
        // 注意：多 CF Open 需要 DBOptions（Options 可隐式转）
        std::vector<rocksdb::ColumnFamilyDescriptor> cfDescs = {
            {rocksdb::kDefaultColumnFamilyName, {}},
            {"log", {}},
        };
        std::vector<rocksdb::ColumnFamilyHandle *> handles;
        s = rocksdb::DB::Open(
                static_cast<const rocksdb::DBOptions &>(opts),
                dir_, cfDescs, &handles, &dbUptr);
        if (!s.ok())
            throw std::runtime_error("RocksDBStorage: open failed: " + s.ToString());
        db_    = dbUptr.release();
        // handles[0] = default CF（由 DB 自己持有，我们用 DestroyColumnFamilyHandle 释放引用）
        db_->DestroyColumnFamilyHandle(handles[0]);
        logCF_ = handles[1];
    } else {
        // 全新 DB：单 CF 打开，再创建 "log" CF
        s = rocksdb::DB::Open(opts, dir_, &dbUptr);
        if (!s.ok())
            throw std::runtime_error("RocksDBStorage: create failed: " + s.ToString());
        db_ = dbUptr.release();
        s   = db_->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), "log", &logCF_);
        if (!s.ok())
            throw std::runtime_error("RocksDBStorage: CreateColumnFamily failed: " + s.ToString());
    }
}

RocksDBStorage::~RocksDBStorage() {
    if (logCF_) {
        db_->DestroyColumnFamilyHandle(logCF_);  // 必须在 delete db_ 之前调用
        logCF_ = nullptr;
    }
    delete db_;
}

// ── HardState ──────────────────────────────────────────────────────────────────────────────
void RocksDBStorage::saveHardState(const HardState &hs) {
    json j = {{"term", hs.term}, {"votedFor", hs.votedFor}};
    auto s = db_->Put(rocksdb::WriteOptions(), "hs", j.dump());
    if (!s.ok())
        LOG_WARN << "[RocksDBStorage] saveHardState: " << s.ToString();
}

RaftStorage::HardState RocksDBStorage::loadHardState() {
    std::string val;
    auto        s = db_->Get(rocksdb::ReadOptions(), "hs", &val);
    if (!s.ok()) return {};
    try {
        auto j = json::parse(val);
        return {j.at("term").get<uint64_t>(), j.at("votedFor").get<int>()};
    } catch (...) {
        return {};
    }
}

// ── Log（增量写核心）──────────────────────────────────────────────────────────────────────────
void RocksDBStorage::saveLog(const std::vector<LogEntry> &entries) {
    // 首尾常量（static local 保证只初始化一次）
    static const std::string LOG_KEY_MIN  = encodeLogKey(0);
    static const std::string LOG_KEY_CEIL = "m"; // 'm' > 'l'，覆盖全部 log key

    rocksdb::WriteBatch batch;

    if (entries.empty()) {
        // 清空所有 log 条目
        if (!logEmpty_)
            batch.DeleteRange(logCF_, LOG_KEY_MIN, LOG_KEY_CEIL);
        logEmpty_            = true;
        lastPersistedLogIdx_ = 0;
        db_->Write(rocksdb::WriteOptions(), &batch);
        return;
    }

    const uint64_t newLastIdx = logBaseIndex_ + static_cast<uint64_t>(entries.size()) - 1;

    if (!logEmpty_ && newLastIdx < lastPersistedLogIdx_) {
        // 截断：删除 [newLastIdx+1, lastPersistedLogIdx_]，然后重写全部
        batch.DeleteRange(logCF_,
                          encodeLogKey(newLastIdx + 1),
                          encodeLogKey(lastPersistedLogIdx_ + 1));
        for (size_t i = 0; i < entries.size(); ++i)
            batch.Put(logCF_, encodeLogKey(logBaseIndex_ + i), json(entries[i]).dump());
    } else {
        // 追加：只写 [startGlobal, newLastIdx] 的新增部分（O(1) 热路径）
        const uint64_t startGlobal = logEmpty_ ? logBaseIndex_ : lastPersistedLogIdx_ + 1;
        const size_t   startLocal  = static_cast<size_t>(startGlobal - logBaseIndex_);
        for (size_t i = startLocal; i < entries.size(); ++i)
            batch.Put(logCF_, encodeLogKey(logBaseIndex_ + i), json(entries[i]).dump());
    }

    auto s = db_->Write(rocksdb::WriteOptions(), &batch);
    if (!s.ok())
        LOG_WARN << "[RocksDBStorage] saveLog: " << s.ToString();

    lastPersistedLogIdx_ = newLastIdx;
    logEmpty_            = false;
}

std::vector<LogEntry> RocksDBStorage::loadLog() {
    std::vector<LogEntry> entries;
    rocksdb::ReadOptions  rOpts;
    auto *it = db_->NewIterator(rOpts, logCF_);

    // 从 logBaseIndex_ 开始扫描（loadSnapshot 必须先于 loadLog 调用）
    for (it->Seek(encodeLogKey(logBaseIndex_)); it->Valid() && isLogKey(it->key()); it->Next()) {
        try {
            entries.push_back(json::parse(it->value().ToString()).get<LogEntry>());
        } catch (...) {
            LOG_WARN << "[RocksDBStorage] loadLog: 解码失败，截断于 index="
                     << decodeLogKey(it->key());
            break;
        }
    }
    delete it;

    if (!entries.empty()) {
        lastPersistedLogIdx_ = logBaseIndex_ + static_cast<uint64_t>(entries.size()) - 1;
        logEmpty_            = false;
    }
    return entries;
}

// ── Snapshot ─────────────────────────────────────────────────────────────────────────────
void RocksDBStorage::saveSnapshot(uint64_t lastIndex, uint64_t lastTerm,
                                  const std::string &data) {
    static const std::string LOG_KEY_MIN  = encodeLogKey(0);

    // 原子写：快照 JSON + 删除快照点之前的旧 log 条目（空间回收）
    rocksdb::WriteBatch batch;
    json j = {{"lastIndex", lastIndex}, {"lastTerm", lastTerm}, {"data", data}};
    batch.Put("snap", j.dump());  // default CF

    if (!logEmpty_ && logBaseIndex_ <= lastIndex)
        batch.DeleteRange(logCF_, LOG_KEY_MIN, encodeLogKey(lastIndex + 1));

    auto s = db_->Write(rocksdb::WriteOptions(), &batch);
    if (!s.ok())
        LOG_WARN << "[RocksDBStorage] saveSnapshot: " << s.ToString();

    logBaseIndex_ = lastIndex + 1;
}

bool RocksDBStorage::loadSnapshot(uint64_t &lastIndex, uint64_t &lastTerm,
                                  std::string &data) {
    std::string val;
    auto        s = db_->Get(rocksdb::ReadOptions(), "snap", &val);
    if (!s.ok()) return false;
    try {
        auto j   = json::parse(val);
        lastIndex = j.at("lastIndex").get<uint64_t>();
        lastTerm  = j.at("lastTerm").get<uint64_t>();
        data      = j.at("data").get<std::string>();
        // 恢复 logBaseIndex_，供后续 loadLog 使用
        logBaseIndex_ = lastIndex + 1;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace raft

#endif // MCPP_HAS_ROCKSDB
