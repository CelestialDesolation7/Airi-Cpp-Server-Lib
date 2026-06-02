#ifdef MCPP_HAS_ROCKSDB

#include "kv/RocksKvStore.h"
#include <rocksdb/utilities/checkpoint.h>
#include <filesystem>
#include <chrono>
#include <stdexcept>

namespace fs = std::filesystem;

// ── 构造 / 析构 ──────────────────────────────────────────────────────────
RocksKvStore::RocksKvStore(const std::string &dbPath) : dbPath_(dbPath) {
    options_.create_if_missing = true;
    options_.compression       = rocksdb::kSnappyCompression;
    fs::create_directories(dbPath_);
    openDB();
}

void RocksKvStore::openDB() {
    std::unique_ptr<rocksdb::DB> dbUptr;
    auto s = rocksdb::DB::Open(options_, dbPath_, &dbUptr);
    if (!s.ok())
        throw std::runtime_error("RocksKvStore: cannot open DB at " + dbPath_
                                 + ": " + s.ToString());
    db_ = dbUptr.release();
}

RocksKvStore::~RocksKvStore() {
    delete db_;
}

// ── CRUD ─────────────────────────────────────────────────────────────────
void RocksKvStore::put(const std::string &key, const std::string &value) {
    auto s = db_->Put(rocksdb::WriteOptions(), key, value);
    if (!s.ok())
        throw std::runtime_error("RocksKvStore::put failed: " + s.ToString());
}

bool RocksKvStore::get(const std::string &key, std::string &value) const {
    auto s = db_->Get(rocksdb::ReadOptions(), key, &value);
    if (s.IsNotFound()) return false;
    if (!s.ok())
        throw std::runtime_error("RocksKvStore::get failed: " + s.ToString());
    return true;
}

void RocksKvStore::del(const std::string &key) {
    auto s = db_->Delete(rocksdb::WriteOptions(), key);
    if (!s.ok())
        throw std::runtime_error("RocksKvStore::del failed: " + s.ToString());
}

// ── Checkpoint（RocksDB 硬链接快照，O(1)）───────────────────────────────
std::string RocksKvStore::createCheckpoint(const std::string &baseDir) {
    fs::create_directories(baseDir);

    // 用纳秒时间戳保证目录名唯一
    auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string cpDir = baseDir + "/cp_" + std::to_string(ns);

    rocksdb::Checkpoint *cp = nullptr;
    auto s = rocksdb::Checkpoint::Create(db_, &cp);
    if (!s.ok())
        throw std::runtime_error("RocksKvStore::createCheckpoint: Checkpoint::Create failed: "
                                 + s.ToString());

    s = cp->CreateCheckpoint(cpDir);
    delete cp;
    if (!s.ok())
        throw std::runtime_error("RocksKvStore::createCheckpoint: CreateCheckpoint failed: "
                                 + s.ToString());
    return cpDir;
}

// ── restoreCheckpoint：关闭 → 替换目录 → 重新打开 ───────────────────────
void RocksKvStore::restoreCheckpoint(const std::string &cpDir) {
    // 1. 关闭当前 DB
    delete db_;
    db_ = nullptr;

    // 2. 替换数据目录（删除旧目录，复制 checkpoint 到原路径）
    std::error_code ec;
    fs::remove_all(dbPath_, ec);
    if (ec)
        throw std::runtime_error("RocksKvStore::restoreCheckpoint: remove_all failed: "
                                 + ec.message());
    fs::copy(cpDir, dbPath_,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec)
        throw std::runtime_error("RocksKvStore::restoreCheckpoint: copy failed: "
                                 + ec.message());

    // 3. 重新打开
    openDB();
}

#endif // MCPP_HAS_ROCKSDB
