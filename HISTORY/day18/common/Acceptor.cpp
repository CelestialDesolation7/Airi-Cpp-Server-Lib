#include "Acceptor.h"
#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"
#include "util.h"
#include <cstdio>
#include <functional>
#include <sys/socket.h>

Acceptor::Acceptor(Eventloop *_loop)
    : loop_(_loop), sock_(nullptr), addr_(nullptr), acceptChannel_(nullptr) {
    sock_ = new Socket();
    addr_ = new InetAddress("127.0.0.1", 8888);
    int opt = 1;
    ErrIf(setsockopt(sock_->getFd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
          "[server] set sock failed.");
    sock_->bind(addr_);
    sock_->listen();
    sock_->setnonblocking();

    acceptChannel_ = new Channel(loop_, sock_->getFd());

    // 只要有新连接请求，Channel 就会调用我们注册的 acceptConnection
    std::function<void()> cb = std::bind(&Acceptor::acceptConnection, this);
    acceptChannel_->setReadCallback(cb);
    acceptChannel_->enableReading();
    // 注意：Acceptor 不使用 ET 模式，避免多个连接同时到达时只 accept 一次导致丢连接
}

Acceptor::~Acceptor() {
    delete sock_;
    delete addr_;
    delete acceptChannel_;
}

void Acceptor::acceptConnection() {
    InetAddress *client_addr = new InetAddress();
    int client_fd = sock_->accept(client_addr);

    if (client_fd == -1) {
        delete client_addr;
        return;
    }

    Socket *client_sock = new Socket(client_fd);
    client_sock->setnonblocking();

    if (newConnectionCallback_) {
        newConnectionCallback_(client_sock, client_addr);
    } else {
        delete client_addr;
        delete client_sock;
    }
}

void Acceptor::setNewConnectionCallback(std::function<void(Socket *, InetAddress *)> _cb) {
    newConnectionCallback_ = _cb;
}