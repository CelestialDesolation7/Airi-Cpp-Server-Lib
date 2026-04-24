#include "Server.h"
#include "Acceptor.h"
#include "Buffer.h"
#include "Connection.h"
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

Server::Server(Eventloop *_loop) : loop(_loop) {
    // 现在资源初始化交由 Acceptor
    acceptor = new Acceptor(loop);
    std::function<void(Socket *, InetAddress *)> cb =
        std::bind(&Server::newConnection, this, std::placeholders::_1, std::placeholders::_2);
    acceptor->setNewConnectionCallback(cb);
    threadPool = new ThreadPool();
    std::cout << "[server] ThreadPool initialized" << std::endl;
}

Server::~Server() {
    delete acceptor;
    delete threadPool;
}

// 这个函数就是以前 accept 之后的那部分逻辑
void Server::newConnection(Socket *client_sock, InetAddress *client_addr) {
    // 注意：clnt_addr 在这里打印完就可以删了，或者保存到 Connection 里（Day 8 先不保存）
    std::cout << "[server] new client fd" << client_sock->getFd()
              << "! IP:" << inet_ntoa(client_addr->addr.sin_addr) << " PortL "
              << ntohs(client_addr->addr.sin_port) << std::endl;

    Connection *conn = new Connection(loop, client_sock);

    // 定义一个 lambda 表达式，捕获 this （Server类指针）拿到ThreadPool
    // 定义 Server socket 的业务逻辑：
    // 把业务函数写在这里，作为一个Task加入线程池
    std::function<void(Connection *)> msgCb = [this](Connection *conn) {
        // 这里的 lambda 是为了捕获 this (Server) 拿到 threadPool
        // 把具体的业务 (Echo) 作为一个 Task 添加到线程池
        // 注意：这里有严重的线程不安全！c->readBuffer 在主线程写，子线程读
        // 后面会修

        threadPool->add([conn]() {
            // std::this_thread::sleep_for(std::chrono::seconds(2));
            std::string msg = conn->readBuffer()->retrieveAllAsString();
            std::cout << "Thread " << std::this_thread::get_id() << " recieve: " << msg
                      << std::endl;
            conn->send(msg);
        });
    };
    conn->setOnMessageCallback(msgCb);

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