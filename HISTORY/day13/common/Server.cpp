#include "Server.h"
#include "Acceptor.h"
#include "Buffer.h"
#include "Connection.h"
#include "EventLoop.h"
#include "InetAddress.h"
#include "Socket.h"
#include "ThreadPool.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#define READ_BUFFER 1024

Server::Server(Eventloop *_loop) : mainReactor(_loop) {
    // 现在资源初始化交由 Acceptor
    acceptor = new Acceptor(mainReactor);
    std::function<void(Socket *, InetAddress *)> cb =
        std::bind(&Server::newConnection, this, std::placeholders::_1, std::placeholders::_2);
    acceptor->setNewConnectionCallback(cb);

    // 1. 初始化线程池
    int threadNum = std::thread::hardware_concurrency();
    threadPool = new ThreadPool(threadNum);
    std::cout << "[server] ThreadPool initialized" << std::endl;

    for (int i = 0; i < threadNum; ++i) {
        Eventloop *subReactor = new Eventloop();
        subReactors.push_back(subReactor);
        // loop 函数作为任务加入 threadPool
        // 每个线程现在取走一个 loop 任务，陷入死循环，这就是 subReactor 的启动
        threadPool->add(std::bind(&Eventloop::loop, subReactor));
    }
    std::cout << "[server] Main Reactor and " << threadNum << " Sub Reactors initialized"
              << std::endl;
}

Server::~Server() {
    delete acceptor;
    delete threadPool;
    for (Eventloop *loop : subReactors) {
        delete loop;
    }
}

// 这个函数就是以前 accept 之后的那部分逻辑
void Server::newConnection(Socket *client_sock, InetAddress *client_addr) {
    std::cout << "[server] new client fd" << client_sock->getFd()
              << "! IP:" << inet_ntoa(client_addr->addr.sin_addr) << " PortL "
              << ntohs(client_addr->addr.sin_port) << std::endl;

    int dispatchIdx = client_sock->getFd() % subReactors.size();
    Eventloop *ioLoop = subReactors[dispatchIdx];

    // day 12 修改：连接被绑定至 subReactor
    Connection *conn = new Connection(ioLoop, client_sock);

    // 定义一个 lambda 表达式，捕获 this （Server类指针）拿到ThreadPool
    // 定义 Server socket 的业务逻辑：
    // 把业务函数写在这里，作为一个Task加入线程池
    std::function<void(Connection *)> msgCb = [this](Connection *conn) {
        std::string msg = conn->readBuffer()->retrieveAllAsString();
        std::cout << "[Thread " << std::this_thread::get_id() << "] recv: " << msg << std::endl;
        conn->send(msg);
    };
    //
    conn->setOnMessageCallback(msgCb);

    // deleteConnection 会在子线程被调用，但 connections map 在主线程
    // 这会导致竞态条件。
    // 我们暂时不修复杂的跨线程安全问题，先跑通逻辑。
    std::function<void(Socket *)> deleteCb =
        std::bind(&Server::deleteConnection, this, std::placeholders::_1);
    conn->setDeleteConnectionCallback(deleteCb);

    connection[client_sock->getFd()] = conn;

    delete client_addr;
}

void Server::deleteConnection(Socket *sock) {
    int sockfd = sock->getFd();
    if (connection.find(sockfd) != connection.end()) {
        Connection *conn = connection[sockfd];
        connection.erase(sockfd);
        delete conn;
        std::cout << "[server] client fd " << sockfd
                  << " closed, memory resources of the connection is deleted" << std::endl;
    }
}