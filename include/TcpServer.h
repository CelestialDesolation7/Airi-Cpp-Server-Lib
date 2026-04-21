#pragma once
#include "Macros.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class Connection;
class Eventloop;
class Acceptor;
class EventLoopThreadPool;

class TcpServer {
    DISALLOW_COPY_AND_MOVE(TcpServer)
  public:
    // ── Day 28：服务器配置参数集（Phase 3）─────────────────────────────────
    // 之前版本通过环境变量 / 全局常量配置，难以在测试中固定。把所有可调参数
    // 收敛到 Options 后，应用层可以通过 `TcpServer(Options{...})` 一次性注入，
    // 单元测试也可以构造任意组合而不依赖外部状态。
    struct Options {
        std::string listenIp{"127.0.0.1"};
        uint16_t listenPort{8888};
        int ioThreads{0};             // <=0 表示按 hardware_concurrency 自动选择
        size_t maxConnections{10000}; // 最大并发连接数；超出即关闭新 fd
    };

  private:
    Options options_{};
    std::unique_ptr<Eventloop> mainReactor_;
    std::unique_ptr<Acceptor> acceptor_;

    // ── 析构顺序说明（C++ 成员按声明逆序析构）──────────────────────────────
    // 声明顺序：mainReactor_(1) → acceptor_(2) → threadPool_(3) → connections_(4)
    // 析构顺序：connections_(1st) → threadPool_(2nd) → acceptor_(3rd) → mainReactor_(4th)
    //
    // 但 connections_ 析构时 Connection::~Connection() 会调 loop->deleteChannel()，
    // 此时要求：① IO 线程已退出（无竞态）；② EventLoop 对象仍然存活（不是野指针）。
    //
    // 满足条件的方式：
    //   ~TcpServer() 先显式调 threadPool_->joinAll()（join 线程但不销毁 EventLoop），
    //   然后成员按逆序自动析构：
    //     connections_ 析构：IO 线程已 join，EventLoop 存活于 threadPool_ 中 ✓
    //     threadPool_  析构：EventLoopThread 析构 → join(no-op) → EventLoop 最终销毁 ✓
    // ────────────────────────────────────────────────────────────────────────
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    size_t maxConnections_{10000};

    std::function<void(Connection *)> onMessageCallback_;
    std::function<void(Connection *)> newConnectCallback_;

  public:
    TcpServer();
    explicit TcpServer(const Options &options);
    ~TcpServer();

    // 最大连接数保护（防止连接洪峰导致资源耗尽）
    void setMaxConnections(size_t maxConnections);
    size_t maxConnections() const { return maxConnections_; }
    size_t connectionCount() const { return connections_.size(); }

    // ── Day 28：纯策略函数（Phase 3）──────────────────────────────────────
    // 这两个 static 函数零副作用、无 I/O，把"是否拒绝新连接""应起多少 IO
    // 线程"两个决策从 TcpServer 的执行路径中剥离出来。这样：
    //   1) handleNewConnection() 只需调用 shouldRejectNewConnection() 即可；
    //   2) 单元测试只需要传入数字即可枚举所有边界（参见
    //      test/TcpServerPolicyTest.cpp）。
    static bool shouldRejectNewConnection(size_t currentCount, size_t maxConnections);
    // configured<=0 时按 hardware_concurrency 取值；硬件返回 0（容器 / 沙箱）则保底 1。
    static int normalizeIoThreadCount(int configured, unsigned int hardwareCount);

    void Start(); // 启动所有 sub-reactor 线程和 main-reactor 循环
    void stop();  // 令所有 Reactor 退出循环

    void newConnection(int fd);
    void deleteConnection(int fd);

    void onMessage(std::function<void(Connection *)> fn);
    void newConnect(std::function<void(Connection *)> fn);
};
