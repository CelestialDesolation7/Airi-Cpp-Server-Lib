#include "kv/KvStateMachine.h"
#include "log/Logger.h"
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

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
            std::lock_guard<std::mutex> lk(mu_);
            map_[key] = value;
            LOG_DEBUG << "[KvSM] apply index=" << index
                      << " PUT " << key << "=" << value;
        }
    } else if (op == "DEL") {
        std::string key;
        ss >> key;
        std::lock_guard<std::mutex> lk(mu_);
        map_.erase(key);
        LOG_DEBUG << "[KvSM] apply index=" << index << " DEL " << key;
    } else {
        LOG_WARN << "[KvSM] apply index=" << index
                 << " 未知命令: " << cmd;
    }
}

bool KvStateMachine::get(const std::string &key, std::string &value) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    value = it->second;
    return true;
}

size_t KvStateMachine::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

std::string KvStateMachine::serialize() const {
    std::lock_guard<std::mutex> lk(mu_);
    json j = map_;
    return j.dump();
}

void KvStateMachine::applySnapshot(uint64_t index, const std::string &data) {
    try {
        auto j = json::parse(data);
        auto newMap = j.get<std::unordered_map<std::string, std::string>>();
        std::lock_guard<std::mutex> lk(mu_);
        map_ = std::move(newMap);
        LOG_INFO << "[KvSM] applySnapshot index=" << index
                 << " entries=" << map_.size();
    } catch (const std::exception &e) {
        LOG_WARN << "[KvSM] applySnapshot parse error: " << e.what()
                 << " — 保留当前状态";
    }
}
