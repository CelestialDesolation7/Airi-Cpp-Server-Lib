#include "Server.h"
#include "Acceptor.h"
#include "Connection.h"
#include "EventLoop.h"
#include "Exception.h"
#include "InetAddress.h"
#include "Socket.h"
#include "ThreadPool.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#define READ_BUFFER 1024

Server::Server(Eventloop *_loop) : mainReactor_(_loop) {
    if (mainReactor_ == nullptr)
        throw Exception(ExceptionType::INVALID, "[server] main reactor cannot be nullptr!");
    // 现在资源初始化交由 Acceptor
    acceptor_ = new Acceptor(mainReactor_);
    std::function<void(Socket *, InetAddress *)> cb =
        std::bind(&Server::newConnection, this, std::placeholders::_1, std::placeholders::_2);
    acceptor_->setNewConnectionCallback(cb);

    // 1. 初始化线程池
    int threadNum = std::thread::hardware_concurrency();
    threadPool_ = new ThreadPool(threadNum);
    std::cout << "[server] ThreadPool initialized" << std::endl;

    for (int i = 0; i < threadNum; ++i) {
        Eventloop *subReactor = new Eventloop();
        subReactors_.push_back(subReactor);
        // loop 函数作为任务加入 threadPool
        // 每个线程现在取走一个 loop 任务，陷入死循环，这就是 subReactor 的启动
        threadPool_->add(std::bind(&Eventloop::loop, subReactor));
    }
    std::cout << "[server] Main Reactor and " << threadNum << " Sub Reactors initialized"
              << std::endl;
}

Server::~Server() {
    delete acceptor_;
    delete threadPool_;
    for (Eventloop *loop : subReactors_) {
        delete loop;
    }
}

// 这个函数就是以前 accept 之后的那部分逻辑
void Server::newConnection(Socket *client_sock, InetAddress *client_addr) {
    std::cout << "[server] new client fd" << client_sock->getFd()
              << "! IP:" << inet_ntoa(client_addr->addr.sin_addr) << " PortL "
              << ntohs(client_addr->addr.sin_port) << std::endl;

    if (client_sock->getFd() == -1)
        throw Exception(ExceptionType::INVALID_SOCKET,
                        "New connection error, invalid client socket!");

    int dispatchIdx = client_sock->getFd() % subReactors_.size();
    Eventloop *ioLoop = subReactors_[dispatchIdx];

    // day 12 修改：连接被绑定至 subReactor
    Connection *conn = new Connection(ioLoop, client_sock);

    conn->setOnMessageCallback(onMessageCallback_);
    if (newConnectCallback_)
        newConnectCallback_(conn);

    // deleteConnection 可能在子线程中被调用（连接断开由 SubReactor 检测），
    // 通过 queueInLoop 将实际的删除操作投递到主线程执行，确保 connections_ map 的线程安全。
    std::function<void(Socket *)> deleteCb =
        std::bind(&Server::deleteConnection, this, std::placeholders::_1);
    conn->setDeleteConnectionCallback(deleteCb);

    connections_[client_sock->getFd()] = conn;

    delete client_addr;
}

void Server::deleteConnection(Socket *sock) {
    int sockfd = sock->getFd();

    auto task = [this, sockfd]() {
        if (connections_.find(sockfd) != connections_.end()) {
            Connection *conn = connections_[sockfd];
            connections_.erase(sockfd);
            delete conn;
            std::cout << "[server] client fd " << sockfd
                      << " closed, memory resources of the connection is deleted" << std::endl;
        }
    };

    mainReactor_->queueInLoop(task);
}

void Server::newConnect(std::function<void(Connection *)> fn) {
    newConnectCallback_ = std::move(fn);
}

void Server::onMessage(std::function<void(Connection *)> fn) { onMessageCallback_ = std::move(fn); }