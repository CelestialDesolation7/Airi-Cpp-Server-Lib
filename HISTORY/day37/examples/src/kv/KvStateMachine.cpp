#include "kv/KvStateMachine.h"
#include "log/Logger.h"
#include "raft.pb.h"
#include <sstream>

void KvStateMachine::apply(uint64_t index, const std::string &cmd) {
    std::istringstream ss(cmd);
    std::string op;
    ss >> op;

    if (op == "PUT") {
        std::string key;
        ss >> key;
        std::string value;
        // value 取第一个空白后的全部剩余内容（允许空格）
        if (std::getline(ss >> std::ws, value)) {
            std::unique_lock<std::shared_mutex> lk(mu_);
            map_[key] = value;
            LOG_DEBUG << "[KvSM] apply index=" << index
                      << " PUT " << key << "=" << value;
        }
    } else if (op == "DEL") {
        std::string key;
        ss >> key;
        std::unique_lock<std::shared_mutex> lk(mu_);
        map_.erase(key);
        LOG_DEBUG << "[KvSM] apply index=" << index << " DEL " << key;
    } else {
        LOG_WARN << "[KvSM] apply index=" << index
                 << " 未知命令: " << cmd;
    }
}

bool KvStateMachine::get(const std::string &key, std::string &value) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    value = it->second;
    return true;
}

size_t KvStateMachine::size() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return map_.size();
}

std::unordered_map<std::string, std::string> KvStateMachine::scan() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return map_;  // 返回 map 的完整副本，锁仅在拷贝期间持有
}

std::string KvStateMachine::serialize() const {
    // Protobuf 二进制格式：KvSnapshot 消息，entries 字段存储全部 KV 对
    std::shared_lock<std::shared_mutex> lk(mu_);
    raft_proto::KvSnapshot pb;
    for (const auto &[k, v] : map_) {
        auto *e = pb.add_entries();
        e->set_key(k);
        e->set_value(v);
    }
    std::string out;
    pb.SerializeToString(&out);
    return out;
}

void KvStateMachine::applySnapshot(uint64_t index, const std::string &data) {
    raft_proto::KvSnapshot pb;
    if (!pb.ParseFromString(data)) {
        LOG_WARN << "[KvSM] applySnapshot index=" << index
                 << " 解析 KvSnapshot Protobuf 失败 — 保留当前状态";
        return;
    }
    std::unordered_map<std::string, std::string> newMap;
    newMap.reserve(static_cast<size_t>(pb.entries_size()));
    for (const auto &e : pb.entries()) {
        newMap[e.key()] = e.value();
    }
    std::unique_lock<std::shared_mutex> lk(mu_);
    map_ = std::move(newMap);
    LOG_INFO << "[KvSM] applySnapshot index=" << index
             << " entries=" << map_.size();
}
