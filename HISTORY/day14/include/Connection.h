#pragma once
#include "Buffer.h"
#include "Macros.h"
#include <functional>

class Eventloop;
class Socket;
class Channel;
class Buffer;

class Connection {
    DISALLOW_COPY_AND_MOVE(Connection)
  private:
    Eventloop *loop_;
    Socket *sock_;
    Channel *channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;

    std::function<void(Socket *)> deleteConnectionCallback_;
    // 业务处理回调，当 Buffer 有数据时被调用
    std::function<void(Connection *)> onMessageCallback_;

  public:
    Connection(Eventloop *_loop, Socket *_sock);
    ~Connection();

    void handleRead();
    void handleWrite();
    void send(const std::string &msg);
    Buffer *readBuffer();
    Buffer *outBuffer();

    void setDeleteConnectionCallback(std::function<void(Socket *)> _cb);
    void setOnMessageCallback(std::function<void(Connection *)> const &_cb);
};