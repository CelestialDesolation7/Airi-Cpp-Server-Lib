#include "Server.h"
#include "EventLoop.h"
#include <iostream>

#define READ_BUFFER 1024

int main() {
    Eventloop *loop = new Eventloop();
    std::cout << "[server] Main Loop Created" << std::endl;
    Server *server = new Server(loop);
    std::cout << "[server] Server Created" << std::endl;

    loop->loop();

    delete server;
    delete loop;
    return 0;
}