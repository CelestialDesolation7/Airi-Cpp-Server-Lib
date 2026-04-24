#include "Socket.h"
#include "InetAddress.h"
#include "util.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

Socket::Socket() : fd_(-1) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    ErrIf(fd_ == -1, "socket create error");
}

Socket::Socket(int _fd) : fd_(_fd) { ErrIf(fd_ == -1, "socket create error"); }

Socket::~Socket() {
    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
}

void Socket::bind(InetAddress *addr) {
    ErrIf(::bind(fd_, (sockaddr *)&addr->addr, addr->addr_len) == -1, "socket bind error");
}

void Socket::listen() { ErrIf(::listen(fd_, SOMAXCONN), "socket listen error"); }

int Socket::accept(InetAddress *addr) {
    int client_sockfd = ::accept(fd_, (sockaddr *)&addr->addr, &addr->addr_len);
    ErrIf(client_sockfd == -1, "socket accept error");
    return client_sockfd;
}

void Socket::connect(InetAddress *addr) {
    ErrIf(::connect(fd_, (sockaddr *)&addr->addr, addr->addr_len) == -1, "socket connect error");
}

int Socket::getFd() { return fd_; }

void Socket::setnonblocking() {
    int oldoptions = fcntl(fd_, F_GETFL);
    int new_option = oldoptions | O_NONBLOCK;
    fcntl(fd_, F_SETFL, new_option);
}

bool Socket::isNonBlocking() { return (fcntl(fd_, F_GETFL) & O_NONBLOCK) != 0; }