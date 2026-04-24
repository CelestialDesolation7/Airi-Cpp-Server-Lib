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
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::vector<std::unique_ptr<Eventloop>> subReactors_;
    std::unique_ptr<ThreadPool> threadPool_;

    std::function<void(Connection *)> onMessageCallback_;
    std::function<void(Connection *)> newConnectCallback_;

  public:
    TcpServer();
    ~TcpServer();

    void Start(); // 启动所有 subReactor 线程和 mainReactor 循环

    void newConnection(int fd);
    void deleteConnection(int fd);

    void onMessage(std::function<void(Connection *)> fn);
    void newConnect(std::function<void(Connection *)> fn);
};