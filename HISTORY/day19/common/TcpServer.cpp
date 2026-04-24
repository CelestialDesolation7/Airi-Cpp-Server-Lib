#include "TcpServer.h"
#include "Acceptor.h"
#include "Connection.h"
#include "EventLoop.h"
#include "Exception.h"
#include "ThreadPool.h"

#include <functional>
#include <iostream>
#include <thread>

TcpServer::TcpServer() {
    mainReactor_ = std::make_unique<Eventloop>();

    acceptor_ = std::make_unique<Acceptor>(mainReactor_.get());
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this, std::placeholders::_1));

    int threadNum = static_cast<int>(std::thread::hardware_concurrency());
    threadPool_ = std::make_unique<ThreadPool>(threadNum);

    for (int i = 0; i < threadNum; ++i)
        subReactors_.push_back(std::make_unique<Eventloop>());

    std::cout << "[TcpServer] Main Reactor + " << threadNum << " Sub Reactors ready." << std::endl;
}

void TcpServer::Start() {
    // 启动所有 subReactor 线程
    for (auto &sub : subReactors_)
        threadPool_->add(std::bind(&Eventloop::loop, sub.get()));

    // 主线程进入 main reactor 循环（阻塞直到 setQuit）
    mainReactor_->loop();
}

void TcpServer::newConnection(int fd) {
    if (fd == -1)
        throw Exception(ExceptionType::INVALID_SOCKET, "newConnection: invalid fd");

    int idx = fd % static_cast<int>(subReactors_.size());
    auto conn = std::make_unique<Connection>(fd, subReactors_[idx].get());

    conn->setOnMessageCallback(onMessageCallback_);
    conn->setDeleteConnectionCallback(
        std::bind(&TcpServer::deleteConnection, this, std::placeholders::_1));

    Connection *rawConn = conn.get();
    connections_[fd] = std::move(conn); // unique_ptr 转移进 map

    if (newConnectCallback_)
        newConnectCallback_(rawConn);

    std::cout << "[TcpServer] new connection fd=" << fd << std::endl;
}

void TcpServer::deleteConnection(int fd) {
    // 必须在主线程执行（确保 connections_ map 线程安全）
    auto task = [this, fd]() {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            connections_.erase(it); // unique_ptr 自动 delete Connection
            std::cout << "[TcpServer] connection fd=" << fd << " deleted." << std::endl;
        }
    };
    mainReactor_->queueInLoop(task);
}

void TcpServer::onMessage(std::function<void(Connection *)> fn) {
    onMessageCallback_ = std::move(fn);
}
void TcpServer::newConnect(std::function<void(Connection *)> fn) {
    newConnectCallback_ = std::move(fn);
}

TcpServer::~TcpServer() = default;