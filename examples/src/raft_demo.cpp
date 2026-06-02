// raft_demo.cpp —— Raft 选举 + 日志复制 + 持久化 + 快照演示（最多 10 节点）
//
// 用法（N 节点集群，默认 3）：
//   ./raft_demo --id 0 [--nodes N] [--propose-interval <ms>] [--persist]
//   ./raft_demo --id 1 [--nodes N] [--persist]
//   ...
//
// Day33 新增：
//   --propose-interval <ms>   Leader 自动 propose 命令的间隔（默认 2000ms，0=不自动提交）
//
// Day34 新增：
//   --persist                 启用持久化（数据目录 ./raft_state/node_<id>/）
//   --snapshot-every <N>      每 N 条 apply 后触发一次 takeSnapshot（默认 0=不触发）
//
// 验证崩溃恢复：
//   1. 启动 3 节点（均加 --persist），让 Leader propose 若干命令
//   2. Ctrl+C 杀掉 Leader
//   3. 重新启动 Leader（相同 --id --persist），观察它从 WAL 恢复 term/log，
//      并在 heartbeat 期间追赶缺失条目
//
// 验证快照传输：
//   1. 启动 3 节点（加 --persist --snapshot-every 5）
//   2. 让 Leader 提交 10+ 条命令，触发快照压缩
//   3. 停一个 Follower，继续 propose 10+ 条
//   4. 重启该 Follower（加 --persist），观察它收到 InstallSnapshot 而不是逐条追赶

#include "log/Logger.h"
#include "net/SignalHandler.h"
#include "raft/FileStorage.h"
#include "raft/RaftNode.h"
#ifdef MCPP_HAS_ROCKSDB
#  include "raft/RocksDBStorage.h"
#endif
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    int  myId             = -1;
    int  nodes            = 3;
    int  proposeIntervalMs = 2000;
    bool persist          = false;
    int  snapshotEvery    = 0; // 0 = 不触发

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--persist") == 0) {
            persist = true;
        } else if (i + 1 < argc) {
            if (std::strcmp(argv[i], "--id") == 0)
                myId = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--nodes") == 0)
                nodes = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--propose-interval") == 0)
                proposeIntervalMs = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--snapshot-every") == 0)
                snapshotEvery = std::stoi(argv[i + 1]);
        }
    }

    if (nodes < 2 || nodes > 10) {
        std::cerr << "错误：--nodes 必须在 2~10 范围内\n";
        return 1;
    }
    if (myId < 0 || myId >= nodes) {
        std::cerr << "用法：raft_demo --id <0.." << (nodes - 1)
                  << "> [--nodes " << nodes
                  << "] [--propose-interval <ms>] [--persist] [--snapshot-every <N>]\n";
        return 1;
    }

    Logger::setLogLevel(Logger::INFO);

    const std::vector<raft::Peer> allPeers = {
        {0, "127.0.0.1", 18901},
        {1, "127.0.0.1", 18902},
        {2, "127.0.0.1", 18903},
        {3, "127.0.0.1", 18904},
        {4, "127.0.0.1", 18905},
        {5, "127.0.0.1", 18906},
        {6, "127.0.0.1", 18907},
        {7, "127.0.0.1", 18908},
        {8, "127.0.0.1", 18909},
        {9, "127.0.0.1", 18910},
    };

    std::vector<raft::Peer> peers(allPeers.begin(), allPeers.begin() + nodes);
    uint16_t myPort = peers[myId].port;

    raft::RaftNode node(myId, peers, myPort);

    // Day34.5：启用持久化 —— 优先 RocksDB，回退 FileStorage
    if (persist) {
        std::string dataDir = "./raft_state/node_" + std::to_string(myId);
        std::cout << "[Node " << myId << "] 持久化目录：" << dataDir << "\n";
#ifdef MCPP_HAS_ROCKSDB
        std::cout << "[Node " << myId << "] 使用 RocksDBStorage 后端\n";
        node.setStorage(std::make_unique<raft::RocksDBStorage>(dataDir));
#else
        std::cout << "[Node " << myId << "] 使用 FileStorage 后端（未编译 RocksDB）\n";
        node.setStorage(std::make_unique<raft::FileStorage>(dataDir));
#endif
    }

    // 注册状态机回调：每条提交的命令都打印一行（可换成真正的 KV 应用）
    std::atomic<int> applyCount{0};
    node.setApplyCallback([&node, myId, snapshotEvery, &applyCount](
                              uint64_t index, const std::string &cmd) {
        std::cout << "[Node " << myId << "] ✓ APPLIED  index=" << index
                  << "  cmd=" << cmd << "\n"
                  << std::flush;
        // Day34：达到阈值时触发快照压缩
        if (snapshotEvery > 0 && ++applyCount % snapshotEvery == 0) {
            // data 字段：真实系统这里放状态机序列化结果；demo 用 "snap@<index>"
            node.takeSnapshot(index, "snap@" + std::to_string(index));
        }
    });

    static std::atomic<bool> stopFlag{false};
    Signal::signal(SIGINT,  [] { stopFlag.store(true); });
    Signal::signal(SIGTERM, [] { stopFlag.store(true); });

    node.start();

    // ── 主循环：状态展示 + 自动 propose ────────────────────────────────
    int proposeCounter = 0;
    auto lastPropose   = std::chrono::steady_clock::now();

    while (!stopFlag.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 状态行：每 500ms 打印一次
        const char *stateStr = "Follower";
        if (node.getState() == raft::State::Leader)   stateStr = "LEADER";
        if (node.getState() == raft::State::Candidate) stateStr = "Candidate";

        std::cout << "[Node " << myId << "] "
                  << stateStr
                  << "  term=" << node.getCurrentTerm()
                  << "  logSize=" << (node.getLastLogIndex() + 1)
                  << "  commit=" << node.getCommitIndex()
                  << "  applied=" << node.getLastApplied()
                  << "  leaderId=" << node.getLeaderId()
                  << "\n" << std::flush;

        // 自动 propose：仅当自己是 Leader 且开启了 propose
        if (proposeIntervalMs > 0 && node.isLeader()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastPropose).count();
            if (elapsed >= proposeIntervalMs) {
                lastPropose = now;
                std::string cmd = "cmd-" + std::to_string(proposeCounter++);
                std::cout << "[Node " << myId << "] → propose \"" << cmd << "\"\n"
                          << std::flush;
                node.propose(cmd);
            }
        }
    }

    node.stop();
    return 0;
}

