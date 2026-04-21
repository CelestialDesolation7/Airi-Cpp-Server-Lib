#include "Connection.h"
#include "SignalHandler.h"
#include "TcpServer.h"
#include <iostream>
#include <thread>

int main() {
    TcpServer *server = new TcpServer();

    Signal::signal(SIGINT, [&] {
        std::cout << "[server] Caught SIGINT, shutting down." << std::endl;
        delete server;
        exit(0);
    });

    server->newConnect([](Connection *conn) {
        std::cout << "[server] New client connected, fd=" << conn->getSocket()->getFd()
                  << std::endl;
    });

    server->onMessage([](Connection *conn) {
        if (conn->getState() != Connection::State::kConnected)
            return;
        std::string msg = conn->getInputBuffer()->retrieveAllAsString();
        if (!msg.empty()) {
            std::cout << "[Thread " << std::this_thread::get_id() << "] recv: " << msg << std::endl;
            conn->send(msg);
        }
    });

    server->Start(); // 阻塞，直到 SIGINT

    delete server;
    return 0;
}