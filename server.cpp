#include "Server.h"
#include "EventLoop.h"

#define READ_BUFFER 1024

int main() {
    Eventloop *loop = new Eventloop();
    Server *server = new Server(loop);

    loop->loop();

    delete server;
    delete loop;
    return 0;
}