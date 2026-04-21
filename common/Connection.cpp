#include "Connection.h"
#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <sys/types.h>
#include <unistd.h>

#define READ_BUFFER 1024

Connection::Connection(int fd, Eventloop *loop)
    : loop_(loop), sock_(std::make_unique<Socket>(fd)), channel_(nullptr) {
    channel_ = std::make_unique<Channel>(loop_, sock_->getFd());
    channel_->setReadCallback(std::bind(&Connection::doRead, this));
    channel_->setWriteCallback(std::bind(&Connection::doWrite, this));
    // 不在构造函数中 enableReading / enableET，
    // 由 TcpServer::newConnection 设置好所有回调后，通过 enableInLoop() 启用
    state_ = State::kConnected;
}

Connection::~Connection() {
    // 先从 Poller 中注销 Channel，防止 kqueue/epoll 保留悬空 udata 指针
    loop_->deleteChannel(channel_.get());
}

void Connection::doRead() {
    int sockfd = sock_->getFd();
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(sockfd, &savedErrno);

    if (n == 0) {
        state_ = State::kClosed;
        std::cout << "[server] client fd " << sockfd << " disconnected." << std::endl;
        // 不在此处调用 close()，由 Business() 在所有回调完成后统一触发
        // 防止 close() → queueInLoop(删除) 后 Main Reactor 立刻析构 Connection
        // 而 Business() 中 onMessageCallback_(this) 尚未执行完毕，导致 use-after-free
    } else if (n < 0) {
        if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK) {
            state_ = State::kFailed;
            std::cerr << "[server] read error on fd " << sockfd << ": " << strerror(savedErrno)
                      << std::endl;
            // 同上，由 Business() 统一触发删除
        }
    }
    // 若 n > 0，数据已被读入 inputBuffer
    // 此函数返回后指令流将转移到 Business() 中
}

void Connection::doWrite() {
    if (channel_->isWriting()) {
        ssize_t n = ::write(sock_->getFd(), outputBuffer_.peek(), outputBuffer_.readableBytes());
        // 不一定能一次全发出去
        if (n > 0) {
            // 一部分数据已被输出，对应 Buffer 区域被划入废弃区域
            outputBuffer_.retrieve(n);
            // 如果全发完了，通知 epoll 本 channel 不再需要发数据
            if (outputBuffer_.readableBytes() == 0) {
                channel_->disableWriting();
            }
        }
    }
}

void Connection::send(const std::string &msg) {
    if (msg.empty())
        return;

    ssize_t nwrote = 0;
    size_t remaining = msg.size();
    bool faultError = false;

    // 1. 如果当前没有正在等待写的（OutputBuffer 为空），且 socket 可写
    // 尝试直接 write，减少一次 buffer copy
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(sock_->getFd(), msg.data(), msg.size());
        if (nwrote >= 0) {
            remaining = msg.size() - nwrote;
        } else {
            nwrote = 0;
            //
            if ((errno != EWOULDBLOCK) && (errno == EPIPE || errno == ECONNRESET)) {
                faultError = true;
            }
        }
    }

    // 2. 如果刚才没写完（或者 OutputBuffer 本来就有积压），把剩下的追加到 OutputBuffer
    // 并注册写事件，等待内核通知
    if (!faultError && remaining > 0) {
        outputBuffer_.append(msg.data() + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void Connection::Business() {
    doRead();
    if (state_ == State::kConnected) {
        // 连接正常：执行业务回调
        if (onMessageCallback_)
            onMessageCallback_(this);
    } else {
        // 连接已关闭或出错：close() 作为最后一步执行
        // 此时所有对 this 的访问已结束，close() 触发 queueInLoop(删除) 后
        // Business() 立刻返回，不再访问任何成员，Main Reactor 可安全析构 Connection
        close();
    }
}

void Connection::Read() { doRead(); }

void Connection::Write() { doWrite(); }

void Connection::close() {
    if (deleteConnectionCallback_)
        deleteConnectionCallback_(sock_->getFd()); // 传 fd 不传指针
}

Connection::State Connection::getState() const { return state_; }

Socket *Connection::getSocket() { return sock_.get(); }

Buffer *Connection::getInputBuffer() { return &inputBuffer_; }

Buffer *Connection::getOutputBuffer() { return &outputBuffer_; };

void Connection::setDeleteConnectionCallback(std::function<void(int)> cb) {
    deleteConnectionCallback_ = std::move(cb);
}

void Connection::setOnConnectCallback(std::function<void(Connection *)> const &_cb) {
    onConnectCallback_ = _cb;
}

void Connection::setOnMessageCallback(std::function<void(Connection *)> const &cb) {
    onMessageCallback_ = cb;
    channel_->setReadCallback(std::bind(&Connection::Business, this));
}

void Connection::enableInLoop() {
    channel_->enableReading();
    channel_->enableET();
}

Eventloop *Connection::getLoop() const { return loop_; }