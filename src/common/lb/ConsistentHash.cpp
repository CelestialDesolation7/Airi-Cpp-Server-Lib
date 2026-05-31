#include "lb/ConsistentHash.h"
#include "http/HttpRequest.h"

// FNV-1a 32-bit（速度快、分布均匀，非密码学用途）
uint32_t ConsistentHash::fnv1a(const std::string &s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s)
        h = (h ^ c) * 16777619u;
    return h;
}

void ConsistentHash::build(const std::vector<std::unique_ptr<Backend>> &backends) {
    ring_.clear();
    for (int i = 0; i < static_cast<int>(backends.size()); ++i) {
        for (int v = 0; v < kVnodesPerBackend; ++v) {
            // 虚拟节点键 = "host:port#vnode_id"
            std::string vkey = backends[i]->address() + "#" + std::to_string(v);
            ring_[fnv1a(vkey)] = i;
        }
    }
}

int ConsistentHash::pick(const std::vector<std::unique_ptr<Backend>> &backends,
                          const HttpRequest &req) {
    if (ring_.empty() || backends.empty()) return -1;

    // 请求 key：path + 会话 ID（同 key 多次请求 → 同后端，Session Affinity）
    std::string key = req.url() + req.header("x-session-id");
    uint32_t    h   = fnv1a(key);

    // 顺时针找第一个 >= h 的虚拟节点；绕环一圈跳过死亡后端
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) it = ring_.begin();

    auto start = it;
    do {
        int idx = it->second;
        if (backends[idx]->isAlive()) return idx;
        ++it;
        if (it == ring_.end()) it = ring_.begin();
    } while (it != start);

    return -1;
}
