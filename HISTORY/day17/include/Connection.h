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
  public:
    enum class State {
        kInvalid = 1,
        kConnected,
        kClosed,
        kFailed,
    };

    Connection(Eventloop *_loop, Socket *_sock);
    ~Connection();

    void send(const std::string &msg);

    void setOnMessageCallback(std::function<void(Connection *)> const &cb);
    void setDeleteConnectionCallback(std::function<void(Socket *)> _cb);
    void setOnConnectCallback(std::function<void(Connection *)> const &_cb);

    void close();

    // 对外接口，调用 doRead() 或 doWrite()
    void Read();
    void Write();
    void Business(); // Read() 后调用 on_message_callback_

    Socket *getSocket();
    State getState() const;
    const char *readBuffer();
    const char *outBuffer();
    Buffer *getInputBuffer();
    Buffer *getOutputBuffer();

  private:
    State state_{State::kInvalid};
    Eventloop *loop_;
    Socket *sock_;
    Channel *channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;

    std::function<void(Socket *)> deleteConnectionCallback_;
    // 业务处理回调，当 Buffer 有数据时被调用
    std::function<void(Connection *)> onConnectCallback_;
    std::function<void(Connection *)> onMessageCallback_;

    // 原本调用 callback 的逻辑从这里分离，现在这两个函数只管IO
    void doRead();
    void doWrite();
};