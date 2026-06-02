#pragma once
// KvStateMachine —— 基于内存哈希表的 Raft 复制状态机
//
// 命令格式（由 KvHttpServer 构造）：
//   "PUT <key> <value>"  — 写入或覆盖（value 可包含空格，取首个空白之后的全部）
//   "DEL <key>"          — 删除（key 不存在时无操作）
//
// 线程安全性：
//   apply / applySnapshot 由 RaftNode 的 loop_ 线程调用（顺序执行，互不竞争）。
//   get / serialize / scan 由 HTTP handler 线程调用（与 apply 并发）。
//   mu_（shared_mutex）保护 map_：读操作使用共享锁，写操作使用独占锁，
//   允许多个 HTTP 读请求并发执行，仅在写入时互斥。
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class KvStateMachine {
  public:
    // 应用一条已提交的 Raft 命令（在 loop_ 线程回调）
    void apply(uint64_t index, const std::string &cmd);

    // 读取 key。返回 false 表示 key 不存在。可从任意线程调用。
    bool get(const std::string &key, std::string &value) const;

    // 返回快照的 Protobuf 序列化字节（KvSnapshot 消息），供 takeSnapshot 使用。
    std::string serialize() const;

    // 返回当前 map 的完整副本，供 /admin/scan HTTP 接口使用。
    std::unordered_map<std::string, std::string> scan() const;

    // 当前 key 数量
    size_t size() const;

    // 从 Protobuf 字节重建完整状态（InstallSnapshot 后恢复）。
    void applySnapshot(uint64_t index, const std::string &data);

  private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::string> map_;
};
