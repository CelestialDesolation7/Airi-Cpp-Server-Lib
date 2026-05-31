#include "lb/BackendPool.h"
#include "http/HttpRequest.h"
#include <nlohmann/json.hpp>

BackendPool::BackendPool(std::unique_ptr<LbStrategy> strategy)
    : strategy_(std::move(strategy)) {}

void BackendPool::addBackend(const std::string &host, int port) {
    backends_.push_back(std::make_unique<Backend>(host, port));
}

void BackendPool::build() {
    strategy_->build(backends_);
}

Backend *BackendPool::pick(const HttpRequest &req) {
    int idx = strategy_->pick(backends_, req);
    if (idx < 0 || idx >= static_cast<int>(backends_.size())) return nullptr;
    return backends_[idx].get();
}

std::string BackendPool::statusJson() const {
    using json = nlohmann::json;
    json arr   = json::array();
    for (const auto &b : backends_) {
        arr.push_back({
            {"address",  b->address()},
            {"alive",    b->alive.load()},
            {"inflight", b->inflight.load()},
            {"failStreak", b->failStreak.load()},
        });
    }
    return arr.dump(2);
}
