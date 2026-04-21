#include "Connection.h"
#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"
#include "log/Logger.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
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

bool Connection::isValidBackpressureConfig(const BackpressureConfig &cfg) {
    return cfg.lowWatermarkBytes > 0 &&
           cfg.lowWatermarkBytes < cfg.highWatermarkBytes &&
           cfg.highWatermarkBytes < cfg.hardLimitBytes;
}

Connection::BackpressureDecision Connection::evaluateBackpressure(
    size_t bufferedBytes,
    bool readPaused,
    const BackpressureConfig &cfg) {
    BackpressureDecision d;
    if (bufferedBytes > cfg.hardLimitBytes) {
        d.shouldCloseConnection = true;
        return d;
    }
    if (!readPaused && bufferedBytes > cfg.highWatermarkBytes)
        d.shouldPauseRead = true;
    if (readPaused && bufferedBytes <= cfg.lowWatermarkBytes)
        d.shouldResumeRead = true;
    return d;
}

void Connection::setBackpressureConfig(const BackpressureConfig &cfg) {
    if (!isValidBackpressureConfig(cfg)) {
        LOG_WARN << "[连接] 回压配置非法，忽略本次设置"
                 << " low=" << cfg.lowWatermarkBytes
                 << " high=" << cfg.highWatermarkBytes
                 << " hard=" << cfg.hardLimitBytes;
        return;
    }
    backpressureConfig_ = cfg;
    LOG_INFO << "[连接] 回压配置已更新"
             << " low=" << backpressureConfig_.lowWatermarkBytes
             << " high=" << backpressureConfig_.highWatermarkBytes
             << " hard=" << backpressureConfig_.hardLimitBytes;
}

Connection::~Connection() {
    // alive_ 置 false：通知所有持有 weak_ptr<bool> 的定时器回调此连接已销毁，
    // 避免回调在 Connection 内存释放后通过 raw pointer 访问悬空对象
    *alive_ = false;
    // 再从 Poller 中注销 Channel，防止 kqueue/epoll 保留悬空 udata 指针
    loop_->deleteChannel(channel_.get());
}

void Connection::doRead() {
    int sockfd = sock_->getFd();
    // ET 模式必须循环读直到 EAGAIN，否则状态只变化一次，内核不再通知，
    // 导致缓冲区中残留数据却永远不被读取，客户端卡死在 read 等待 echo
    while (true) {
        int savedErrno = 0;
        ssize_t n = inputBuffer_.readFd(sockfd, &savedErrno);
        if (n > 0) {
            // 本次读到数据，继续循环尝试读取更多（ET 模式可能有大量数据）
            continue;
        } else if (n == 0) {
            state_ = State::kClosed;
            LOG_INFO << "[server] client fd " << sockfd << " disconnected.";
            // 不在此处调用 close()，由 Business() 在所有回调完成后统一触发
            // 防止 close() → queueInLoop(删除) 后 Main Reactor 立刻析构 Connection
            // 而 Business() 中 onMessageCallback_(this) 尚未执行完毕，导致 use-after-free
            break;
        } else {
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
                // 内核缓冲区已读空，ET 下这是正常退出条件
                break;
            }
            state_ = State::kFailed;
            LOG_ERROR << "[server] read error on fd " << sockfd << ": " << strerror(savedErrno);
            // 同上，由 Business() 统一触发删除
            break;
        }
    }
    // 执行到此处：数据已尽数读入 inputBuffer（或连接异常）
    // 指令流转移到 Business() 继续处理
}

void Connection::applyBackpressureAfterAppend() {
    if (state_ != State::kConnected)
        return;

    BackpressureDecision d =
        evaluateBackpressure(outputBuffer_.readableBytes(),
                             readPausedByBackpressure_,
                             backpressureConfig_);

    if (d.shouldCloseConnection) {
        state_ = State::kFailed;
        LOG_ERROR << "[连接] 输出缓冲超出硬上限，触发保护性断连，fd=" << sock_->getFd()
                  << " buffered=" << outputBuffer_.readableBytes()
                  << " hard=" << backpressureConfig_.hardLimitBytes;
        close();
        return;
    }

    if (d.shouldPauseRead) {
        channel_->disableReading();
        readPausedByBackpressure_ = true;
        LOG_WARN << "[连接] 触发回压，暂停读事件，fd=" << sock_->getFd()
                 << " buffered=" << outputBuffer_.readableBytes()
                 << " high=" << backpressureConfig_.highWatermarkBytes;
    }
}

void Connection::tryResumeReadAfterDrain() {
    if (state_ != State::kConnected)
        return;

    BackpressureDecision d =
        evaluateBackpressure(outputBuffer_.readableBytes(),
                             readPausedByBackpressure_,
                             backpressureConfig_);
    if (d.shouldResumeRead) {
        channel_->enableReading();
        readPausedByBackpressure_ = false;
        LOG_INFO << "[连接] 回压解除，恢复读事件，fd=" << sock_->getFd()
                 << " buffered=" << outputBuffer_.readableBytes()
                 << " low=" << backpressureConfig_.lowWatermarkBytes;
    }
}

void Connection::doWrite() {
    if (state_ != State::kConnected)
        return;
    if (!channel_->isWriting())
        return;

    const int fd = sock_->getFd();

    // ET 模式下写事件也应尽可能“写到 EAGAIN 为止”，避免残留数据长时间滞留。
    while (outputBuffer_.readableBytes() > 0) {
        ssize_t n = ::write(fd, outputBuffer_.peek(), outputBuffer_.readableBytes());
        if (n > 0) {
            outputBuffer_.retrieve(static_cast<size_t>(n));
            continue;
        }

        if (n == -1 && errno == EINTR) {
            // 被信号中断，立即重试
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 发送缓冲区暂满，保留写事件，等待下一次可写通知
            break;
        }

        state_ = State::kFailed;
        LOG_ERROR << "[连接] 写入失败，fd=" << fd << " errno=" << errno
                  << " 错误=" << strerror(errno);
        close();
        return;
    }

    if (outputBuffer_.readableBytes() == 0) {
        // 全部发完后及时取消写关注，避免空转唤醒
        channel_->disableWriting();
    }

    // 写缓冲下降后，尝试解除回压并恢复读事件。
    tryResumeReadAfterDrain();
}

// ── 发送数据：先尝试直接 write，写不完再追加到 OutputBuffer ──────────────────
// 优先路径（loopback 小响应）：OutputBuffer 为空 + socket 可写 →
//   直接 write(fd, ...) 一次写完，不经过 OutputBuffer，零额外拷贝。
// 降级路径（大响应 / 写缓冲满）：剩余数据追加到 OutputBuffer，
//   注册 EPOLLOUT / EVFILT_WRITE，等内核通知后由 doWrite() 续写。
void Connection::send(const std::string &msg) {
    if (msg.empty())
        return;

    ssize_t nwrote = 0;
    size_t remaining = msg.size();
    bool faultError = false;

    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(sock_->getFd(), msg.data(), msg.size());
        if (nwrote >= 0) {
            remaining = msg.size() - nwrote;
        } else {
            nwrote = 0;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                faultError = true;
                state_ = State::kFailed;
                LOG_ERROR << "[连接] 发送失败(fd=" << sock_->getFd() << ")，错误="
                          << strerror(errno);
                close();
            }
        }
    }

    if (!faultError && remaining > 0) {
        const size_t projected = outputBuffer_.readableBytes() + remaining;
        if (projected > backpressureConfig_.hardLimitBytes) {
            state_ = State::kFailed;
            LOG_ERROR << "[连接] 发送前检测到缓冲即将超限，保护性断连，fd=" << sock_->getFd()
                      << " projected=" << projected
                      << " hard=" << backpressureConfig_.hardLimitBytes;
            close();
            return;
        }
        outputBuffer_.append(msg.data() + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
        applyBackpressureAfterAppend();
    }
}

// 移动重载：对 conn->send(resp.serialize()) 这类调用，
// serialize() 返回的临时字符串直接被移入，避免在 "零拷贝写路径" 上额外复制。
// 若需要 fallback 到 OutputBuffer，仍然 append（字节级拷贝无法避免）。
void Connection::send(std::string &&msg) {
    if (msg.empty())
        return;

    ssize_t nwrote = 0;
    size_t sz = msg.size();
    bool faultError = false;

    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(sock_->getFd(), msg.data(), sz);
        if (nwrote >= 0) {
            // 全部写完：msg 直接废弃，无任何拷贝
            if (static_cast<size_t>(nwrote) == sz)
                return;
        } else {
            nwrote = 0;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                faultError = true;
                state_ = State::kFailed;
                LOG_ERROR << "[连接] 发送失败(fd=" << sock_->getFd() << ")，错误="
                          << strerror(errno);
                close();
            }
        }
    }

    if (!faultError && static_cast<size_t>(nwrote) < sz) {
        const size_t remaining = sz - static_cast<size_t>(nwrote);
        const size_t projected = outputBuffer_.readableBytes() + remaining;
        if (projected > backpressureConfig_.hardLimitBytes) {
            state_ = State::kFailed;
            LOG_ERROR << "[连接] 发送前检测到缓冲即将超限，保护性断连，fd=" << sock_->getFd()
                      << " projected=" << projected
                      << " hard=" << backpressureConfig_.hardLimitBytes;
            close();
            return;
        }

        outputBuffer_.append(msg.data() + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
        applyBackpressureAfterAppend();
    }
}

void Connection::Business() {
    // 状态守卫：kqueue/epoll 的同一批次事件中，同一 fd 可能出现多次
    // （如 EVFILT_READ + EVFILT_WRITE 同帧返回）。
    // 若第一个事件已经探测到 EOF/错误并触发 close()，Connection 尚未被删除
    // （删除是异步的，通过 queueInLoop 投递到本线程的 doPendingFunctors() 延迟执行），
    // 第二个事件不应再进入 doRead() 对一个关闭中的 fd 做读写。
    if (state_ != State::kConnected)
        return;

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