#include "Server.h"
#include "Connection.h"
#include "EventLoop.h"
#include "SignalHandler.h"
#include "Socket.h"
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#define READ_BUFFER 1024

int main() {
    Eventloop *loop = new Eventloop();
    std::cout << "[server] Main Loop Created" << std::endl;
    Server *server = new Server(loop);
    std::cout << "[server] Server Created" << std::endl;

    Signal::signal(SIGINT, [&] {
        std::cout << "[server] Caught SIGINT, shutting down." << std::endl;
        loop->setQuit();
    });

    // 新建连接时触发，进行一次性通知
    server->newConnect([](Connection *conn) {
        std::cout << "[server] New client connected, fd=" << conn->getSocket()->getFd()
                  << std::endl;
    });

    // 在有消息到来时触发
    // 这里是业务逻辑注入点
    server->onMessage([](Connection *conn) {
        if (conn->getState() != Connection::State::kConnected)
            return;
        std::string msg = conn->getInputBuffer()->retrieveAllAsString();
        if (!msg.empty()) {
            std::cout << "[Thread " << std::this_thread::get_id() << "] recv: " << msg << std::endl;
            conn->send(msg);
        }
    });

    loop->loop();

    delete server;
    delete loop;
    return 0;
}