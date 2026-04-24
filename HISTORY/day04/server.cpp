#include "include/Epoll.h"
#include "include/InetAddress.h"
#include "include/Socket.h"
#include "include/util.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#define READ_BUFFER 1024

int main() {
  Socket *serv_sock = new Socket();
  InetAddress *serv_addr = new InetAddress("127.0.0.1", 8888);
  serv_sock->bind(serv_addr);
  serv_sock->listen();

  Epoll *ep = new Epoll();
  setnonblocking(serv_sock->getFd());
  ep->addFd(serv_sock->getFd(), POLLER_READ | POLLER_ET);

  std::cout << "[server] Server start success!" << std::endl;

  while (true) {
    std::vector<ActiveEvent> events = ep->poll();
    int nfds = events.size();
    for (int i = 0; i < nfds; ++i) {
      if (events[i].fd == serv_sock->getFd()) {
        // 新连接
        InetAddress *client_addr = new InetAddress();
        int client_sockfd = serv_sock->accept(client_addr);
        std::cout << "[server] new client fd " << client_sockfd
                  << "! IP:" << inet_ntoa(client_addr->addr.sin_addr)
                  << " Port: " << ntohs(client_addr->addr.sin_port)
                  << std::endl;
        setnonblocking(client_sockfd);
        ep->addFd(client_sockfd, POLLER_READ | POLLER_ET);
        delete client_addr;
      } else if (events[i].events & POLLER_READ) {
        // 数据可读
        int sockfd = events[i].fd;
        char buf[READ_BUFFER];
        while (true) {
          bzero(buf, sizeof(buf));
          ssize_t bytes_read = read(sockfd, buf, sizeof(buf));
          if (bytes_read > 0) {
            std::cout << "[server] message received from client fd " << sockfd
                      << ": " << buf << std::endl;
            write(sockfd, buf, bytes_read);
          } else if (bytes_read == -1 && errno == EINTR) {
            continue;
          } else if (bytes_read == -1 &&
                     ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
            break;
          } else if (bytes_read == 0) {
            std::cout << "[server] EOF received from client fd " << sockfd
                      << " and it's now disconnected" << std::endl;
            close(sockfd);
            break;
          }
        }
      }
    }
  }
  delete serv_sock;
  delete serv_addr;
  delete ep;
  return 0;
}
