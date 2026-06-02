#include "lb/RoundRobin.h"
#include "http/HttpRequest.h"

int RoundRobin::pick(const std::vector<std::unique_ptr<Backend>> &backends,
                     const HttpRequest & /*req*/) {
    size_t n = backends.size();
    if (n == 0) return -1;

    // 从当前计数器位置开始，最多轮询一整圈，跳过已下线后端
    uint64_t start = counter_.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (start + i) % n;
        if (backends[idx]->isAlive()) return static_cast<int>(idx);
    }
    return -1;
}
