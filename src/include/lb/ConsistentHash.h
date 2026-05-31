#pragma once
#include "lb/LbStrategy.h"
#include <cstdint>
#include <map>

// ConsistentHash：虚拟节点一致性哈希。
//
// 原理：
//   · 每个后端在哈希环上放 kVnodesPerBackend 个虚拟节点（键 = "addr#N" 的 FNV-1a 哈希）
//   · 请求 key = url + X-Session-Id，映射到环上顺时针第一个存活虚拟节点所属后端
//   · 增减后端时，只有相邻虚拟节点区间的请求发生迁移（影响比例 ≈ 1/n）
//
// 使用前须调用 build()；pick() 中不再重建环（多线程安全）。

class ConsistentHash : public LbStrategy {
  public:
    static constexpr int kVnodesPerBackend = 100;

    void build(const std::vector<std::unique_ptr<Backend>> &backends) override;

    int pick(const std::vector<std::unique_ptr<Backend>> &backends,
             const HttpRequest &req) override;

  private:
    static uint32_t fnv1a(const std::string &s);

    std::map<uint32_t, int> ring_; // hash → backend index
};
