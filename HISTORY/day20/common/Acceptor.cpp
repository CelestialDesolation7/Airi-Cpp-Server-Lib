#include "Acceptor.h"
#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"
#include "util.h"
#include <fcntl.h>
#include <functional>
#include <sys/socket.h>

Acceptor::Acceptor(Eventloop *loop) {
    sock_ = std::make_unique<Socket>();
    InetAddress addr("127.0.0.1", 8888);
    int opt = 1;
    ErrIf(setsockopt(sock_->getFd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
          "[server] setsockopt failed.");
    sock_->bind(&addr);
    sock_->listen();
    sock_->setnonblocking();

    acceptChannel_ = std::make_unique<Channel>(loop, sock_->getFd());
    // 只要有新连接请求，Channel 就会调用我们注册的 acceptConnection
    acceptChannel_->setReadCallback(std::bind(&Acceptor::acceptConnection, this));
    // 注意：Acceptor 不使用 ET 模式，避免多个连接同时到达时只 accept 一次导致丢连接
    acceptChannel_->enableReading();
}

Acceptor::~Acceptor() {}

void Acceptor::acceptConnection() {
    InetAddress clientAddr;
    int clientFd = sock_->accept(&clientAddr);
    if (clientFd == -1)
        return;
    // 直接 fcntl，不通过 Socket 包装（避免析构时 close）
    fcntl(clientFd, F_SETFL, fcntl(clientFd, F_GETFL) | O_NONBLOCK);
    if (newConnectionCallback_)
        newConnectionCallback_(clientFd);
}

void Acceptor::setNewConnectionCallback(std::function<void(int)> cb) {
    newConnectionCallback_ = std::move(cb);
}