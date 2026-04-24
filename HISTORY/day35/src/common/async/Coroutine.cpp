/**
 * @file Coroutine.cpp
 * @brief C++20 协程 I/O 实现
 *
 * 提供 AsyncIO.h 中定义的 Awaitable 实现。
 * 这些实现与 EventLoop 集成，将协程挂起/恢复与 epoll/kqueue 事件关联。
 */

#include "async/Coroutine.h"
#include "async/AsyncIO.h"

#if MCPP_HAS_COROUTINES

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __APPLE__
// macOS 没有 accept4，需要手动实现
static int accept4_compat(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    int fd = ::accept(sockfd, addr, addrlen);
    if (fd < 0)
        return fd;

    // 设置 non-blocking
    int fl = ::fcntl(fd, F_GETFL);
    if (fl >= 0) {
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    // 设置 close-on-exec
    int fdfl = ::fcntl(fd, F_GETFD);
    if (fdfl >= 0) {
        ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
    }
    return fd;
}
#define MCPP_ACCEPT4(fd, addr, len, flags) accept4_compat(fd, addr, len, flags)
#else
#define MCPP_ACCEPT4(fd, addr, len, flags) ::accept4(fd, addr, len, flags)
#endif

namespace mcpp::coro {

// ─────────────────────────────────────────────────────────────────
//  EventLoopScheduler
// ─────────────────────────────────────────────────────────────────

void EventLoopScheduler::schedule(std::coroutine_handle<> h) {
    // TODO: 与 EventLoop::runInLoop() 集成
    // 当前简单实现：立即恢复
    if (h && !h.done()) {
        h.resume();
    }
}

// ─────────────────────────────────────────────────────────────────
//  SleepAwaitable
// ─────────────────────────────────────────────────────────────────

void SleepAwaitable::await_suspend(std::coroutine_handle<> h) {
    handle_ = h;

    // TODO: 与 EventLoop::runAfter() 集成
    // EventLoop *loop = static_cast<EventLoop*>(loopPtr_);
    // auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration_);
    // loop->runAfter(ms.count(), [h]() { h.resume(); });

    // 当前简单实现：立即恢复 (忽略延迟)
    // 生产环境应使用定时器
    if (h && !h.done()) {
        h.resume();
    }
}

// ─────────────────────────────────────────────────────────────────
//  ReadAwaitable
// ─────────────────────────────────────────────────────────────────

void ReadAwaitable::await_suspend(std::coroutine_handle<> h) {
    handle_ = h;

    // 尝试非阻塞读取
    ssize_t n = ::read(fd_, buf_, len_);
    if (n >= 0) {
        result_ = IOResult{n, 0};
        h.resume();
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // TODO: 注册读事件到 EventLoop
        // EventLoop *loop = static_cast<EventLoop*>(loopPtr_);
        // loop->registerReadCallback(fd_, [this, h]() {
        //     ssize_t n = ::read(fd_, buf_, len_);
        //     result_ = IOResult{n, n < 0 ? errno : 0};
        //     h.resume();
        // });

        // 当前简单实现：立即返回 EAGAIN
        result_ = IOResult{-1, EAGAIN};
        h.resume();
    } else {
        result_ = IOResult{-1, errno};
        h.resume();
    }
}

// ─────────────────────────────────────────────────────────────────
//  WriteAwaitable
// ─────────────────────────────────────────────────────────────────

void WriteAwaitable::await_suspend(std::coroutine_handle<> h) {
    handle_ = h;

    // 尝试非阻塞写入
    ssize_t n = ::write(fd_, buf_, len_);
    if (n >= 0) {
        result_ = IOResult{n, 0};
        h.resume();
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // TODO: 注册写事件到 EventLoop
        result_ = IOResult{-1, EAGAIN};
        h.resume();
    } else {
        result_ = IOResult{-1, errno};
        h.resume();
    }
}

// ─────────────────────────────────────────────────────────────────
//  AcceptAwaitable
// ─────────────────────────────────────────────────────────────────

void AcceptAwaitable::await_suspend(std::coroutine_handle<> h) {
    handle_ = h;

    // 尝试非阻塞 accept
    int fd = MCPP_ACCEPT4(listenFd_, nullptr, nullptr, 0);
    if (fd >= 0) {
        acceptedFd_ = fd;
        h.resume();
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // TODO: 注册读事件到 EventLoop (监听 socket 可读 = 有新连接)
        acceptedFd_ = -1;
        h.resume();
    } else {
        acceptedFd_ = -1;
        h.resume();
    }
}

} // namespace mcpp::coro

#endif // MCPP_HAS_COROUTINES
