#include "Server.h"
#include "Connection.h"
#include "EventLoop.h"
#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

#define READ_BUFFER 1024

Eventloop *g_loop = nullptr;

void signalHandler(int signum) {
    std::cout << "[server] Caught signal " << signum << " , shutting down server." << std::endl;
    if (g_loop) {
        g_loop->setQuit();
    }
}

int main() {
    signal(SIGINT, signalHandler);
    Eventloop *loop = new Eventloop();
    g_loop = loop;
    std::cout << "[server] Main Loop Created" << std::endl;
    Server *server = new Server(loop);
    std::cout << "[server] Server Created" << std::endl;

    server->onConnect([](Connection *conn) {
        if (conn->getState() != Connection::State::kConnected) {
            conn->close();
            return;
        }
        std::string msg = conn->readBuffer()->retrieveAllAsString();
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