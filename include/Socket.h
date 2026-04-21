#pragma once
#include "InetAddress.h"
#include "Macros.h"

class Socket {
    DISALLOW_COPY_AND_MOVE(Socket)
  private:
    int fd_;

  public:
    Socket();
    Socket(int fd);
    ~Socket();

    void bind(InetAddress *addr);
    void listen();

    // 返回一个新的 Socket 对象，代表客户端
    int accept(InetAddress *addr);

    // 在且仅在客户端 Socket 调用
    void connect(InetAddress *addr);

    int getFd();

    void setnonblocking();

    bool isNonBlocking();
};