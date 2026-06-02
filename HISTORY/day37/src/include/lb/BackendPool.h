#pragma once
#include "lb/Backend.h"
#include "lb/LbStrategy.h"
#include <memory>
#include <string>
#include <vector>

class HttpRequest;

// BackendPool：后端节点池，持有 LbStrategy 并暴露 pick() 接口。
//
// 生命周期：
//   1. addBackend() 若干次（单线程，服务器启动前）
//   2. build()（触发一致性哈希等策略的预计算）
//   3. pick() 并发调用（多 I/O 线程）

class BackendPool {
  public:
    explicit BackendPool(std::unique_ptr<LbStrategy> strategy);

    void addBackend(const std::string &host, int port);

    // 所有 addBackend 完成后调用一次
    void build();

    // 选出一个存活后端；全部不可用时返回 nullptr
    Backend *pick(const HttpRequest &req);

    // JSON 格式的后端状态快照（供 /admin/backends 端点）
    std::string statusJson() const;

    const std::vector<std::unique_ptr<Backend>> &backends() const { return backends_; }

  private:
    std::vector<std::unique_ptr<Backend>> backends_;
    std::unique_ptr<LbStrategy>           strategy_;
};
