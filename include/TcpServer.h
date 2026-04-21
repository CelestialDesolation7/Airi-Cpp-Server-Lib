#pragma once
#include "Macros.h"
#include <functional>
#include <memory>
#include <unordered_map>

class Connection;
class Eventloop;
class Acceptor;
class EventLoopThreadPool;

class TcpServer {
    DISALLOW_COPY_AND_MOVE(TcpServer)
  private:
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

    std::function<void(Connection *)> onMessageCallback_;
    std::function<void(Connection *)> newConnectCallback_;

  public:
    TcpServer();
    ~TcpServer();

    void Start(); // 启动所有 sub-reactor 线程和 main-reactor 循环
    void stop();  // 令所有 Reactor 退出循环

    void newConnection(int fd);
    void deleteConnection(int fd);

    void onMessage(std::function<void(Connection *)> fn);
    void newConnect(std::function<void(Connection *)> fn);
};
