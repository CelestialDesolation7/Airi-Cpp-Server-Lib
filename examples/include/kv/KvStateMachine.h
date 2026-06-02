#pragma once
// KvStateMachine —— 基于内存哈希表的 Raft 复制状态机
//
// 命令格式（由 KvHttpServer 构造）：
//   "PUT <key> <value>"  — 写入或覆盖（value 可包含空格，取首个空白之后的全部）
//   "DEL <key>"          — 删除（key 不存在时无操作）
//
// 线程安全性：
//   apply / applySnapshot 由 RaftNode 的 loop_ 线程调用（顺序执行，互不竞争）。
//   get / serialize       由 HTTP handler 线程调用（与 apply 并发），mu_ 保护 map_。
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

class KvStateMachine {
  public:
    // 应用一条已提交的 Raft 命令（在 loop_ 线程回调）
    void apply(uint64_t index, const std::string &cmd);

    // 读取 key。返回 false 表示 key 不存在。可从任意线程调用。
    bool get(const std::string &key, std::string &value) const;

    // 返回所有 key-value 对的 JSON 字符串，供 takeSnapshot / scan 使用。
    std::string serialize() const;

    // 当前 key 数量（近似，lock-free）
    size_t size() const;

    // 从 JSON 字符串重建完整状态（InstallSnapshot 后恢复）。
    void applySnapshot(uint64_t index, const std::string &data);

  private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> map_;
};
