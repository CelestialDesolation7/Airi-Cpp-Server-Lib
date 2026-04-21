#include "util.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/event.h> // kqueue (macOS)
#else
#include <sys/epoll.h> // epoll (Linux)
#endif

#define MAX_EVENTS 1024
#define READ_BUFFER 1024

void setnonblocking(int fd) {
    int oldoptions = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, oldoptions | O_NONBLOCK);
}

void handle_read(int fd) {
    char buf[READ_BUFFER];
    while (true) {
        bzero(buf, sizeof(buf));
        ssize_t bytes_read = read(fd, buf, sizeof(buf));
        if (bytes_read > 0) {
            std::cout << "[服务器] 收到客户端 fd " << fd << " 的消息: " << buf << std::endl;
            write(fd, buf, bytes_read);
        } else if (bytes_read == -1 && errno == EINTR) {
            continue;
        } else if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (bytes_read == 0) {
            std::cout << "[服务器] 客户端 fd " << fd << " 已关闭连接" << std::endl;
            close(fd);
            break;
        }
    }
}

int main() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    errif(sockfd == -1, "[服务器] socket创建失败");

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(8888);

    errif(bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1,
          "[服务器] socket绑定失败");
    errif(listen(sockfd, SOMAXCONN) == -1, "[服务器] 建立listen失败");
    std::cout << "[服务器] 正在监听 127.0.0.1:8888" << std::endl;

    setnonblocking(sockfd);

#ifdef __APPLE__
    int kqfd = kqueue();
    errif(kqfd == -1, "[服务器] kqueue 创建错误");

    struct kevent change;
    EV_SET(&change, sockfd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    kevent(kqfd, &change, 1, nullptr, 0, nullptr);

    struct kevent events[MAX_EVENTS];
    while (true) {
        int nfds = kevent(kqfd, nullptr, 0, events, MAX_EVENTS, nullptr);
        for (int i = 0; i < nfds; ++i) {
            int fd = static_cast<int>(events[i].ident);
            if (fd == sockfd) {
                struct sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);
                bzero(&client_addr, sizeof(client_addr));
                int client_sockfd =
                    accept(sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
                errif(client_sockfd == -1, "[服务器] socket accept error");
                std::cout << "[服务器] 新的客户端连接，fd: " << client_sockfd
                          << " IP: " << inet_ntoa(client_addr.sin_addr)
                          << " Port: " << ntohs(client_addr.sin_port) << std::endl;
                setnonblocking(client_sockfd);
                EV_SET(&change, client_sockfd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
                kevent(kqfd, &change, 1, nullptr, 0, nullptr);
            } else if (events[i].filter == EVFILT_READ) {
                handle_read(fd);
            }
        }
    }
    close(kqfd);
#else
    int epfd = epoll_create1(0);
    errif(epfd == -1, "[服务器] Epoll 创建错误");

    struct epoll_event events[MAX_EVENTS];
    struct epoll_event ev;
    ev.data.fd = sockfd;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);

    while (true) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == sockfd) {
                struct sockaddr_in client_addr;
                socklen_t client_addr_len = sizeof(client_addr);
                bzero(&client_addr, sizeof(client_addr));
                int client_sockfd =
                    accept(sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
                errif(client_sockfd == -1, "[服务器] socket accept error");
                std::cout << "[服务器] 新的客户端连接，fd: " << client_sockfd
                          << " IP: " << inet_ntoa(client_addr.sin_addr)
                          << " Port: " << ntohs(client_addr.sin_port) << std::endl;
                bzero(&ev, sizeof(ev));
                ev.data.fd = client_sockfd;
                ev.events = EPOLLIN | EPOLLET;
                setnonblocking(client_sockfd);
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_sockfd, &ev);
            } else if (events[i].events & EPOLLIN) {
                handle_read(events[i].data.fd);
            }
        }
    }
    close(epfd);
#endif

    close(sockfd);
    return 0;
}
