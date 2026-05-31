#pragma once
#include <atomic>
#include <chrono>
#include <string>

// Backend：上游服务节点的元数据容器。
//
// 设计要点：
//   · inflight   —— 正在处理的请求数（ReverseProxy 在转发前 +1，回包/超时后 -1）
//   · failStreak —— 连续失败次数；>=3 触发被动健康检查下线
//   · alive      —— 当前是否可接收新请求
//   · cooldownUntil —— 下线后的冷却截止时间（steady_clock 纳秒）；到期后 isAlive() 自动恢复
//
// 线程安全：所有字段均为 std::atomic，可在任意线程读写。
// 不可拷贝（std::atomic 语义），通过 unique_ptr 存储于 BackendPool。

struct Backend {
    std::string host;
    int         port{0};

    std::atomic<int>     inflight{0};
    std::atomic<int>     failStreak{0};
    std::atomic<bool>    alive{true};
    std::atomic<int64_t> cooldownUntil{0}; // nanoseconds since steady_clock epoch

    Backend() = default;
    Backend(std::string h, int p) : host(std::move(h)), port(p) {}

    Backend(const Backend &)            = delete;
    Backend &operator=(const Backend &) = delete;

    std::string address() const { return host + ":" + std::to_string(port); }

    // 检查是否存活；冷却期结束后自动将 alive 翻回 true。
    bool isAlive() {
        if (alive.load(std::memory_order_relaxed)) return true;
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        if (now >= cooldownUntil.load(std::memory_order_relaxed)) {
            alive.store(true, std::memory_order_relaxed);
            failStreak.store(0, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    void onSuccess() {
        failStreak.store(0, std::memory_order_relaxed);
        alive.store(true, std::memory_order_relaxed);
    }

    // 连续失败 3 次 → 下线 60 s（被动健康检查）
    void onFailure() {
        int streak = failStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (streak >= 3) {
            alive.store(false, std::memory_order_relaxed);
            auto deadline = std::chrono::steady_clock::now().time_since_epoch()
                          + std::chrono::seconds(60);
            cooldownUntil.store(deadline.count(), std::memory_order_relaxed);
        }
    }
};
