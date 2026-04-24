#include "Connection.h"
#include "Channel.h"
#include "Socket.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <unistd.h>

#define READ_BUFFER 1024

Connection::Connection(Eventloop *_loop, Socket *_sock)
    : loop(_loop), sock(_sock), channel(nullptr) {
    channel = new Channel(loop, sock->getFd());
    std::function<void()> cb = std::bind(&Connection::echoRead, this);
    channel->setCallback(cb);
    channel->enableReading();
}

Connection::~Connection() {
    delete channel;
    delete sock;
}

void Connection::echoRead() {
    int sockfd = sock->getFd();
    char buf[READ_BUFFER];

    while (true) {
        bzero(&buf, sizeof(buf));
        ssize_t bytes_read = read(sockfd, buf, sizeof(buf));
        if (bytes_read > 0) {
            std::cout << "[server] message from client fd " << sockfd << ": " << buf << std::endl;
            write(sockfd, buf, bytes_read);
        } else if (bytes_read == -1 && errno == EINTR) {
            continue;
        } else if (bytes_read == -1 && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
            break;
        } else if (bytes_read == 0) {
            std::cout << "[server] client fd " << sockfd << " disconnected." << std::endl;
            // 关键：连接断开时，通过回调通知 Server 移除自己
            if (deleteConnectionCallback) {
                deleteConnectionCallback(sock);
            }
            break;
        }
    }
}

void Connection::setDeleteConnectionCallback(std::function<void(Socket *)> _cb) {
    deleteConnectionCallback = _cb;
}