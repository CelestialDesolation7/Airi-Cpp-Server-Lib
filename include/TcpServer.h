#pragma once
#include "Macros.h"
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class Connection;
class Eventloop;
class Acceptor;
class ThreadPool;

class TcpServer {
    DISALLOW_COPY_AND_MOVE(TcpServer)
  private:
    std::unique_ptr<Eventloop> mainReactor_;
    std::unique_ptr<Acceptor> acceptor_;
    // 声明顺序决定析构的逆序：threadPool_ 最先析构（join 线程），
    // connections_ 其次（Connection 析构调用 loop_->deleteChannel()），
    // subReactors_ 最后析构（此时 EventLoop/kqueue fd 仍然有效）。
    // 若 connections_ 在 subReactors_ 之后析构，loop_ 是野指针，必然段错误。
    std::vector<std::unique_ptr<Eventloop>> subReactors_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::unique_ptr<ThreadPool> threadPool_;

    std::function<void(Connection *)> onMessageCallback_;
    std::function<void(Connection *)> newConnectCallback_;

  public:
    TcpServer();
    ~TcpServer();

    void Start(); // 启动所有 subReactor 线程和 mainReactor 循环
    void stop();  // 安全关闭：令所有 Reactor 退出循环

    void newConnection(int fd);
    void deleteConnection(int fd);

    void onMessage(std::function<void(Connection *)> fn);
    void newConnect(std::function<void(Connection *)> fn);
};