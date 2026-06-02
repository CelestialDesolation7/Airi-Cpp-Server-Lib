// kv_node.cpp —— 分布式 KV 存储节点（Raft + KvStateMachine + KvHttpServer）
//
// 用法（默认 3 节点集群）：
//   ./kv_node --id 0 [--persist] [--snapshot-every 100] [--http-port 8901]
//   ./kv_node --id 1 [--persist]
//   ./kv_node --id 2 [--persist]
//
// 端口约定：
//   节点 N  Raft RPC 端口 = 19001 + N   （19001 / 19002 / 19003）
//   节点 N  HTTP API 端口 = 8901  + N   （8901  / 8902  / 8903）
//
// 浏览器打开 http://127.0.0.1:890x/ 查看实时可视化仪表盘。

#include "kv/KvHttpServer.h"
#include "kv/KvStateMachine.h"
#include "log/Logger.h"
#include "net/SignalHandler.h"
#include "raft/FileStorage.h"
#include "raft/RaftNode.h"
#ifdef MCPP_HAS_ROCKSDB
#    include "raft/RocksDBStorage.h"
#endif
#include <atomic>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>

static constexpr uint16_t kRpcBasePort  = 19001;
static constexpr uint16_t kHttpBasePort = 8901;

// ── ANSI 颜色宏 ──────────────────────────────────────────────────────────────
#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_GREEN  "\033[32m"
#define C_CYAN   "\033[36m"
#define C_YELLOW "\033[33m"
#define C_RED    "\033[31m"
#define C_GRAY   "\033[90m"
#define C_BLUE   "\033[34m"

static void printBanner(int myId, int nodes, uint16_t httpPort, uint16_t rpcPort,
                        bool persist, const std::string &staticDir) {
    std::cout
        << "\n"
        << C_BOLD C_BLUE
        << "╔══════════════════════════════════════════════════════════╗\n"
        << "║         Airi Distributed KV Cluster  —  Node " << myId << "          ║\n"
        << "╚══════════════════════════════════════════════════════════╝"
        << C_RESET "\n\n"
        << C_BOLD "  集群信息" C_RESET "\n";

    for (int i = 0; i < nodes; ++i) {
        std::cout << "    Node " << i
                  << "  RPC :1900" << (i+1)
                  << "  HTTP :" << (kHttpBasePort + i);
        if (i == myId) std::cout << C_YELLOW "  ← YOU" C_RESET;
        std::cout << "\n";
    }
    std::cout
        << "\n" C_BOLD "  本节点" C_RESET "\n"
        << "    HTTP 监听   :" << httpPort << "\n"
        << "    RPC  监听   :" << rpcPort  << "\n"
        << "    持久化      " << (persist ? C_GREEN "开启" C_RESET : C_GRAY "关闭" C_RESET) << "\n"
        << "    静态文件    " C_GRAY << staticDir << C_RESET "\n"
        << "\n" C_BOLD "  仪表盘 (浏览器打开)" C_RESET "\n"
        << "    " C_CYAN "http://127.0.0.1:" << httpPort << "/" C_RESET "\n"
        << "\n" C_BOLD "  curl 示例" C_RESET "\n"
        << C_GRAY "    # 写入 (-L 跟随 307 重定向到 Leader)\n"
        << "    curl -s -L -X PUT http://127.0.0.1:" << httpPort << "/kv/hello -d \"world\"\n"
        << "    # 读取\n"
        << "    curl -s http://127.0.0.1:" << httpPort << "/kv/hello\n"
        << "    # 删除\n"
        << "    curl -s -L -X DELETE http://127.0.0.1:" << httpPort << "/kv/hello\n"
        << "    # 节点状态\n"
        << "    curl -s http://127.0.0.1:" << httpPort << "/admin/raft | python3 -m json.tool\n"
        << "    # 扫描所有 KV\n"
        << "    curl -s http://127.0.0.1:" << httpPort << "/admin/scan | python3 -m json.tool\n"
        << C_RESET
        << "\n" C_GRAY "  按 Ctrl+C 退出" C_RESET "\n"
        << "──────────────────────────────────────────────────────────\n\n"
        << std::flush;
}

int main(int argc, char **argv) {
    int      myId          = -1;
    int      nodes         = 5;
    bool     persist       = false;
    int      snapshotEvery = 100;
    uint16_t httpPortOverride = 0;
    std::string staticDir = "examples/static/kv";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--persist") == 0) {
            persist = true;
        } else if (i + 1 < argc) {
            if      (std::strcmp(argv[i], "--id") == 0)
                myId = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--nodes") == 0)
                nodes = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--snapshot-every") == 0)
                snapshotEvery = std::stoi(argv[i + 1]);
            else if (std::strcmp(argv[i], "--http-port") == 0)
                httpPortOverride = static_cast<uint16_t>(std::stoi(argv[i + 1]));
            else if (std::strcmp(argv[i], "--static-dir") == 0)
                staticDir = argv[i + 1];
        }
    }

    if (myId < 0 || myId >= nodes) {
        std::cerr << "用法: kv_node --id <0.." << (nodes-1) << ">"
                  << " [--nodes N] [--persist] [--snapshot-every N]"
                  << " [--http-port PORT] [--static-dir PATH]\n";
        return 1;
    }

    Logger::setLogLevel(Logger::WARN);  // 演示时只输出警告以上，保持终端清爽

    const uint16_t myRpcPort  = static_cast<uint16_t>(kRpcBasePort  + myId);
    const uint16_t myHttpPort = httpPortOverride
                              ? httpPortOverride
                              : static_cast<uint16_t>(kHttpBasePort + myId);

    printBanner(myId, nodes, myHttpPort, myRpcPort, persist, staticDir);

    // ── 集群成员表 ────────────────────────────────────────────────────────────
    std::vector<raft::Peer> peers;
    for (int i = 0; i < nodes; ++i)
        peers.push_back({i, "127.0.0.1", static_cast<uint16_t>(kRpcBasePort + i)});

    // ── Raft 节点 ─────────────────────────────────────────────────────────────
    raft::RaftNode node(myId, peers, myRpcPort);

    if (persist) {
        std::string dataDir = "./kv_raft_state/node_" + std::to_string(myId);
#ifdef MCPP_HAS_ROCKSDB
        node.setStorage(std::make_unique<raft::RocksDBStorage>(dataDir));
#else
        node.setStorage(std::make_unique<raft::FileStorage>(dataDir));
#endif
    }

    // ── 状态机 ────────────────────────────────────────────────────────────────
    KvStateMachine sm;

    std::atomic<int> applyCount{0};
    node.setApplyCallback([&node, &sm, myId, snapshotEvery, &applyCount](
                              uint64_t index, const std::string &cmd) {
        sm.apply(index, cmd);
        if (snapshotEvery > 0 && ++applyCount % snapshotEvery == 0) {
            // 必须把 index 与 sm.serialize() 一起传进去：takeSnapshot lambda
            // 是异步排队执行的，lambda 内部不能再用 lastApplied_ 推断快照点，
            // 否则会与 data 表征的状态时刻错位（参见 RaftNode::takeSnapshot 注释）。
            node.takeSnapshot(index, sm.serialize());
        }
    });

    node.setSnapshotApplyCallback([&sm](uint64_t index, const std::string &data) {
        sm.applySnapshot(index, data);
    });

    // ── HTTP KV 服务器 ────────────────────────────────────────────────────────
    KvHttpServer kvSrv(node, sm, myHttpPort, kHttpBasePort, staticDir);

    // ── 信号处理 ─────────────────────────────────────────────────────────────
    static std::atomic<bool> stopFlag{false};
    Signal::signal(SIGINT,  [] { stopFlag.store(true); });
    Signal::signal(SIGTERM, [] { stopFlag.store(true); });

    node.start();

    std::thread httpThread([&kvSrv] { kvSrv.start(); });

    // ── 主循环：ANSI 彩色状态行，每 2s 刷新 ─────────────────────────────────
    auto lastTerm    = uint64_t(-1);
    auto lastState   = raft::State::Follower;

    while (!stopFlag.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        auto state  = node.getState();
        auto term   = node.getCurrentTerm();
        auto commit = node.getCommitIndex();
        auto applied= node.getLastApplied();
        auto leader = node.getLeaderId();
        auto kvSz   = sm.size();

        // 角色/任期变化时打印一条高亮提示
        if (state != lastState || term != lastTerm) {
            lastState = state;
            lastTerm  = term;
            if (state == raft::State::Leader)
                std::cout << C_BOLD C_GREEN
                          << "  *** Node " << myId << " 成为 Leader (term=" << term << ") ***"
                          << C_RESET "\n";
            else if (state == raft::State::Candidate)
                std::cout << C_YELLOW
                          << "  Node " << myId << " 发起选举 (term=" << term << ")"
                          << C_RESET "\n";
            else
                std::cout << C_CYAN
                          << "  Node " << myId << " 成为 Follower (term=" << term
                          << ", leader=" << leader << ")"
                          << C_RESET "\n";
        }

        const char *stateColor = C_CYAN;
        const char *stateName  = "Follower";
        if (state == raft::State::Leader)    { stateColor = C_GREEN;  stateName = "LEADER   "; }
        if (state == raft::State::Candidate) { stateColor = C_YELLOW; stateName = "Candidate"; }

        std::cout << C_GRAY "[Node " << myId << "] " C_RESET
                  << stateColor << stateName << C_RESET
                  << C_GRAY "  term=" C_RESET << std::setw(3) << term
                  << C_GRAY "  commit=" C_RESET << std::setw(4) << commit
                  << C_GRAY "  applied=" C_RESET << std::setw(4) << applied
                  << C_GRAY "  leader=" C_RESET << (leader < 0 ? "?" : std::to_string(leader))
                  << C_GRAY "  kv=" C_RESET << kvSz
                  << "\n" << std::flush;
    }

    kvSrv.stop();
    httpThread.join();

    // ── 优雅停机：若本节点是 Leader，先主动让贤再 stop，避免选主写空窗 ─────
    if (node.isLeader()) {
        std::cout << C_YELLOW "[Node " << myId
                  << "] 检测到自身是 Leader，发起主动 Leader Transfer..." C_RESET "\n";
        if (node.transferLeadership(/*auto-pick*/ -1)) {
            // 等待最多 800ms 让 target 接任并广播心跳，本节点 becomeFollower
            for (int i = 0; i < 80; ++i) {
                if (!node.isLeader()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::cout << C_GRAY "[Node " << myId
                      << "] 让贤完成（或超时），当前 leaderId="
                      << node.getLeaderId() << C_RESET "\n";
        } else {
            std::cout << C_GRAY "[Node " << myId
                      << "] Leader Transfer 未发起（单节点或无可用目标）" C_RESET "\n";
        }
    }

    node.stop();
    std::cout << C_GRAY "[Node " << myId << "] 已退出" C_RESET "\n";
    return 0;
}
