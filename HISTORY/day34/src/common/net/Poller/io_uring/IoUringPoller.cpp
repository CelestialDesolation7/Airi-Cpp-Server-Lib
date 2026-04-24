/**
 * @file IoUringPoller.cpp
 * @brief io_uring Poller 实现 (Linux only)
 */

#include "Poller/IoUringPoller.h"

#if defined(__linux__) && defined(MCPP_HAS_IO_URING)

#include "Channel.h"
#include "Logger.h"
#include <cerrno>
#include <cstring>
#include <liburing.h>
#include <sys/utsname.h>

namespace mcpp::net {

// ─────────────────────────────────────────────────────────────────
//  静态方法
// ─────────────────────────────────────────────────────────────────

bool IoUringPoller::isAvailable() noexcept {
    // 检查内核版本 >= 5.1
    struct utsname uts;
    if (uname(&uts) != 0)
        return false;

    int major = 0, minor = 0;
    if (std::sscanf(uts.release, "%d.%d", &major, &minor) != 2)
        return false;

    return (major > 5) || (major == 5 && minor >= 1);
}

// ─────────────────────────────────────────────────────────────────
//  构造与析构
// ─────────────────────────────────────────────────────────────────

IoUringPoller::IoUringPoller(Eventloop *loop, unsigned queueDepth)
    : Poller(loop), ring_(nullptr), queueDepth_(queueDepth) {
    ring_ = new io_uring;
    int ret = io_uring_queue_init(queueDepth_, ring_, 0);
    if (ret < 0) {
        delete ring_;
        ring_ = nullptr;
        throw std::runtime_error("io_uring_queue_init failed: " + std::string(std::strerror(-ret)));
    }
}

IoUringPoller::~IoUringPoller() {
    if (ring_) {
        io_uring_queue_exit(ring_);
        delete ring_;
    }
}

// ─────────────────────────────────────────────────────────────────
//  Channel 管理
// ─────────────────────────────────────────────────────────────────

void IoUringPoller::updateChannel(Channel *channel) {
    int fd = channel->fd();
    channels_[fd] = channel;

    // 提交一个 POLL_ADD 操作，等待 fd 可读/可写
    io_uring_sqe *sqe = getSqe();
    if (!sqe)
        return;

    unsigned poll_mask = 0;
    if (channel->isReading())
        poll_mask |= POLLIN;
    if (channel->isWriting())
        poll_mask |= POLLOUT;

    io_uring_prep_poll_add(sqe, fd, poll_mask);
    io_uring_sqe_set_data(sqe, channel);

    ++stats_.submittedOps;
    ++stats_.pollAddOps;
}

void IoUringPoller::deleteChannel(Channel *channel) {
    int fd = channel->fd();
    channels_.erase(fd);

    // 提交 POLL_REMOVE
    io_uring_sqe *sqe = getSqe();
    if (sqe) {
        io_uring_prep_poll_remove(sqe, channel);
        io_uring_sqe_set_data(sqe, nullptr); // 不需要回调
        ++stats_.submittedOps;
    }
}

// ─────────────────────────────────────────────────────────────────
//  Poll 主循环
// ─────────────────────────────────────────────────────────────────

std::vector<Channel *> IoUringPoller::poll(int timeoutMs) {
    std::vector<Channel *> activeChannels;

    // 提交所有挂起的 SQE 并等待至少一个 CQE
    int submitted = io_uring_submit(ring_);
    (void)submitted;

    // 等待完成
    io_uring_cqe *cqe = nullptr;
    if (timeoutMs < 0) {
        // 阻塞等待
        int ret = io_uring_wait_cqe(ring_, &cqe);
        if (ret < 0 && ret != -EINTR) {
            return activeChannels;
        }
    } else {
        struct __kernel_timespec ts;
        ts.tv_sec = timeoutMs / 1000;
        ts.tv_nsec = (timeoutMs % 1000) * 1000000;
        int ret = io_uring_wait_cqe_timeout(ring_, &cqe, &ts);
        if (ret == -ETIME || ret == -EINTR) {
            return activeChannels;
        }
        if (ret < 0)
            return activeChannels;
    }

    // 收割所有完成事件
    unsigned head;
    unsigned count = 0;
    io_uring_for_each_cqe(ring_, head, cqe) {
        handleCqe(cqe, activeChannels);
        ++count;
        ++stats_.completedOps;
    }
    io_uring_cq_advance(ring_, count);

    return activeChannels;
}

// ─────────────────────────────────────────────────────────────────
//  内部辅助方法
// ─────────────────────────────────────────────────────────────────

io_uring_sqe *IoUringPoller::getSqe() {
    io_uring_sqe *sqe = io_uring_get_sqe(ring_);
    if (!sqe) {
        // 队列满，先提交
        io_uring_submit(ring_);
        sqe = io_uring_get_sqe(ring_);
    }
    return sqe;
}

void IoUringPoller::handleCqe(io_uring_cqe *cqe, std::vector<Channel *> &activeChannels) {
    void *data = io_uring_cqe_get_data(cqe);
    if (!data)
        return; // 例如 POLL_REMOVE 的完成

    Channel *channel = static_cast<Channel *>(data);
    int res = cqe->res;

    if (res < 0) {
        // 错误
        channel->setRevents(POLLERR);
    } else {
        // POLL_ADD 完成时，res 是触发的事件掩码
        channel->setRevents(res);
    }

    activeChannels.push_back(channel);

    // POLL_ADD 是一次性的，需要重新注册（除非使用 IORING_POLL_ADD_MULTI）
    if (channels_.count(channel->fd())) {
        updateChannel(channel);
    }
}

} // namespace mcpp::net

#endif // __linux__ && MCPP_HAS_IO_URING
