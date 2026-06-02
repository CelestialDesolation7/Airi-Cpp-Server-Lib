#pragma once
#include "lb/LbStrategy.h"
#include <atomic>

// RoundRobin：原子计数器轮询，跳过已下线后端。
// 无状态热路径，O(n) 最坏情况（全部后端都死时扫一遍）。

class RoundRobin : public LbStrategy {
  public:
    int pick(const std::vector<std::unique_ptr<Backend>> &backends,
             const HttpRequest &req) override;

  private:
    std::atomic<uint64_t> counter_{0};
};
