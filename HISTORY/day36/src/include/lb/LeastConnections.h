#pragma once
#include "lb/LbStrategy.h"

// LeastConnections：选 inflight 最小的存活后端。
// 平衡处理时间差异大的后端（慢后端 inflight 累积 → 被自动回避）。

class LeastConnections : public LbStrategy {
  public:
    int pick(const std::vector<std::unique_ptr<Backend>> &backends,
             const HttpRequest &req) override;
};
