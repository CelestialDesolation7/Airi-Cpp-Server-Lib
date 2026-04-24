#pragma once
#include <functional>

class Eventloop;
class Socket;
class Channel;

class Connection {
  private:
    Eventloop *loop;
    Socket *sock;
    Channel *channel;

    std::function<void(Socket *)> deleteConnectionCallback;

  public:
    Connection(Eventloop *_loop, Socket *_sock);
    ~Connection();

    // 原来；业务逻辑没有合适的地方放，现在可以放这了，很显然不同的连接有不同业务逻辑
    void echoRead();
    void setDeleteConnectionCallback(std::function<void(Socket *)> _cb);
};