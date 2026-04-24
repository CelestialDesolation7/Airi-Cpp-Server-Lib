#pragma once
#include "Buffer.h"
#include "Channel.h"
#include "Macros.h"
#include "Socket.h"
#include <functional>
#include <memory>

class Eventloop;
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

    // 构造参数改为 int fd，Socket 在内部创建
    Connection(int fd, Eventloop *loop);
    ~Connection();

    void send(const std::string &msg);

    void setOnMessageCallback(std::function<void(Connection *)> const &cb);
    // deleteCallback 改为 void(int fd)
    void setDeleteConnectionCallback(std::function<void(int)> _cb);
    void setOnConnectCallback(std::function<void(Connection *)> const &_cb);

    void close();

    // 对外接口，调用 doRead() 或 doWrite()
    void Read();
    void Write();
    void Business(); // Read() 后调用 on_message_callback_

    Socket *getSocket();
    State getState() const;
    Buffer *getInputBuffer();
    Buffer *getOutputBuffer();

  private:
    State state_{State::kInvalid};
    Eventloop *loop_;
    std::unique_ptr<Socket> sock_;
    std::unique_ptr<Channel> channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;

    std::function<void(int)> deleteConnectionCallback_;
    // 业务处理回调，当 Buffer 有数据时被调用
    std::function<void(Connection *)> onConnectCallback_;
    std::function<void(Connection *)> onMessageCallback_;

    // 原本调用 callback 的逻辑从这里分离，现在这两个函数只管IO
    void doRead();
    void doWrite();
};