#include "Server.h"
#include "Channel.h"
#include "InetAddress.h"
#include "Socket.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <ostream>
#include <strings.h>
#include <unistd.h>

#define READ_BUFFER 1024

Server::Server(Eventloop *_loop) : loop(_loop) {
    // 现在在这里初始化服务器资源
    server_sock = new Socket();
    server_addr = new InetAddress("127.0.0.1", 8888);
    server_sock->bind(server_addr);
    server_sock->listen();
    server_sock->setnonblocking();

    server_sock_channel = new Channel(loop, server_sock->getFd());
    std::function<void()> cb = std::bind(&Server::handleReadEvent, this);
    server_sock_channel->setCallback(cb);
    server_sock_channel->enableReading();
}

Server::~Server() {
    delete server_sock;
    delete server_addr;
    delete server_sock_channel;
}

void Server::handleReadEvent() {
    // 这里是原来的 Accept 逻辑
    InetAddress *client_addr = new InetAddress();
    int client_sockfd = server_sock->accept(client_addr);

    std::cout << "[server] new client fd" << client_sockfd
              << "! IP:" << inet_ntoa(client_addr->addr.sin_addr) << " PortL "
              << ntohs(client_addr->addr.sin_port) << std::endl;

    // 为新的客户端连接创建 Socket 和 Channel
    // 还是有内存泄漏，以后处理
    Socket *client_sock = new Socket(client_sockfd);
    client_sock->setnonblocking();
    Channel *clientChannel = new Channel(loop, client_sockfd);

    // 数据处理的lambda函数
    std::function<void()> cb = [=] {
        char buf[READ_BUFFER];
        while (true) {
            bzero(&buf, sizeof(buf));
            ssize_t bytes_read = read(client_sockfd, buf, sizeof(buf));
            if (bytes_read > 0) {
                std::cout << "[server] message from client fd " << client_sockfd << ": " << buf
                          << std::endl;
                write(client_sockfd, buf, bytes_read);
            } else if (bytes_read == -1 && errno == EINTR) {
                continue;
            } else if (bytes_read == -1 && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
                break;
            } else if (bytes_read == 0) {
                std::cout << "[server] client fd " << client_sockfd << " disconnected."
                          << std::endl;
                close(client_sockfd);
                // 这里需要 delete clntChannel，但 lambda 里很难删自己
                // 所以我们暂且不管内存泄漏
                break;
            }
        }
    };

    clientChannel->setCallback(cb);
    clientChannel->enableReading();

    delete client_addr;
}