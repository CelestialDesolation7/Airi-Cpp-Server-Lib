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

    bool isValid() const { return fd_ != -1; }

    // ── Day 28：返回值语义重构（Phase 3）──────────────────────────────────
    // 之前 bind/listen/connect/setnonblocking 都是 void，失败仅打日志。
    // 改成 bool 后：
    //   * Acceptor::Acceptor() 可以根据返回值放弃监听并抛错；
    //   * test/SocketPolicyTest.cpp 可以断言"绑定到 0 端口""对已用端口
    //     再次 bind"等异常路径的精确返回值，而不是只能看 stderr。
    bool bind(InetAddress *addr);
    bool listen();

    // 返回新连接 fd；失败时返回 -1（由上层根据 errno 决定是否忽略或记录）
    int accept(InetAddress *addr);

    // 在且仅在客户端 Socket 调用
    bool connect(InetAddress *addr);

    int getFd();

    bool setnonblocking();

    bool isNonBlocking();
};