#include "util.h"
#include <arpa/inet.h> // IP地址翻译库
#include <cstdio>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <ostream>
#include <sys/socket.h> // 操作系统网络库
#include <sys/types.h>
#include <unistd.h>

int main() {
  // 创建套接字，参数：用ipv4 tcp
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  errif(sockfd == -1, "[服务器] socket创建失败");

  // ip和端口绑定
  struct sockaddr_in server_addr;
  std::memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  // 转化字符串ip到整数形式
  server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  // 处理主机字节的小端序转化为网路字节的大端序
  server_addr.sin_port = htons(8888);

  errif(bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) ==
            -1,
        "[服务器] socket绑定失败");

  errif(listen(sockfd, SOMAXCONN) == -1, "[服务器] 建立listen失败");

  std::cout << "[服务器] 正在监听 127.0.0.1:8888" << std::endl;

  // 存储客户端地址信息
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  std::memset(&client_addr, 0, sizeof(client_addr));

  // 阻塞，直到客户端发起连接
  int client_sockfd =
      accept(sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
  errif(client_sockfd == -1, "[服务器] 接受连接失败");

  std::cout << "[服务器] 接收到来自此ip和端口的client连接"
            << inet_ntoa(client_addr.sin_addr) // 整数形式ip地址换回字符
            << ":" << ntohs(client_addr.sin_port) << std::endl;

  while (true) {
    char buf[1024];
    memset(&buf, 0, sizeof(buf));
    ssize_t read_bytes = read(client_sockfd, buf, sizeof(buf));
    if (read_bytes > 0) {
      std::cout << "[服务器] 接收到信息，来自客户端 fd " << client_sockfd
                << ": " << buf << std::endl;
      write(client_sockfd, buf, sizeof(buf));
    } else if (read_bytes == 0) {
      std::cout << "[服务器] 客户端 fd " << client_sockfd << " 断开与本机的连接"
                << std::endl;
      close(client_sockfd);
      break;
    } else if (read_bytes == -1) {
      close(client_sockfd);
      errif(true, "[服务器] socket 读取遭遇错误");
    }
  }

  // 通信结束，关连接（销毁文件对象）
  close(client_sockfd);
  close(sockfd);

  return 0;
}