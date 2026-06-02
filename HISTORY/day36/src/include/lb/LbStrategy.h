#pragma once
#include "lb/Backend.h"
#include <memory>
#include <vector>

class HttpRequest;

// LbStrategy：负载均衡策略的纯虚接口。
//
// pick() 返回 backends 的下标（存活后端），或 -1（全部不可用）。
// build() 在所有 backend 加入后调用一次，供一致性哈希等需要预计算的策略使用；
// 默认空实现，RoundRobin / LeastConnections 无需 override。

class LbStrategy {
  public:
    virtual ~LbStrategy() = default;

    // 在所有 addBackend 完成后调用，允许策略预计算（如构建哈希环）。
    virtual void build(const std::vector<std::unique_ptr<Backend>> &) {}

    // 从存活后端中选一个，返回下标；全部不可用时返回 -1。
    virtual int pick(const std::vector<std::unique_ptr<Backend>> &backends,
                     const HttpRequest &req) = 0;
};
