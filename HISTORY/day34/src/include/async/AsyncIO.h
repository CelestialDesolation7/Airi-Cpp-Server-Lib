#pragma once
/**
 * @file AsyncIO.h
 * @brief 协程化异步 I/O 操作
 *
 * 提供与 EventLoop / Connection 集成的 co_await 接口。
 */

#include "Coroutine.h"

#if MCPP_HAS_COROUTINES

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <sys/types.h>

namespace mcpp::coro {

// 前向声明
class EventLoop;
class Connection;

// ─────────────────────────────────────────────────────────────────
//  异步读写结果
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 异步 I/O 操作结果
 */
struct IOResult {
    ssize_t bytes{0}; ///< 实际读写字节数 (-1 表示错误)
    int error{0};     ///< errno 错误码 (0 表示成功)

    bool ok() const noexcept { return error == 0 && bytes >= 0; }
    bool eof() const noexcept { return ok() && bytes == 0; }
};

// ─────────────────────────────────────────────────────────────────
//  Timer Awaitable
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 定时器 Awaitable
 *
 * @code
 * co_await sleepFor(std::chrono::milliseconds(100));
 * @endcode
 */
class SleepAwaitable {
  public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    SleepAwaitable(Duration duration, void *loopPtr) : duration_(duration), loopPtr_(loopPtr) {}

    bool await_ready() const noexcept { return duration_.count() <= 0; }

    void await_suspend(std::coroutine_handle<> h);

    void await_resume() const noexcept {}

  private:
    Duration duration_;
    void *loopPtr_; // EventLoop* (避免循环依赖)
    std::coroutine_handle<> handle_;
};

/**
 * @brief 创建 sleep awaitable
 *
 * @param duration 等待时长
 * @param loop EventLoop 指针
 */
template <typename Rep, typename Period>
SleepAwaitable sleepFor(std::chrono::duration<Rep, Period> duration, void *loop) {
    return SleepAwaitable{std::chrono::duration_cast<SleepAwaitable::Duration>(duration), loop};
}

// ─────────────────────────────────────────────────────────────────
//  I/O Awaitables
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 异步读取 Awaitable
 */
class ReadAwaitable {
  public:
    ReadAwaitable(int fd, char *buf, size_t len, void *loopPtr)
        : fd_(fd), buf_(buf), len_(len), loopPtr_(loopPtr) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h);

    IOResult await_resume() const noexcept { return result_; }

  private:
    int fd_;
    char *buf_;
    size_t len_;
    void *loopPtr_;
    std::coroutine_handle<> handle_;
    IOResult result_;
};

/**
 * @brief 异步写入 Awaitable
 */
class WriteAwaitable {
  public:
    WriteAwaitable(int fd, const char *buf, size_t len, void *loopPtr)
        : fd_(fd), buf_(buf), len_(len), loopPtr_(loopPtr) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h);

    IOResult await_resume() const noexcept { return result_; }

  private:
    int fd_;
    const char *buf_;
    size_t len_;
    void *loopPtr_;
    std::coroutine_handle<> handle_;
    IOResult result_;
};

/**
 * @brief 异步 accept Awaitable
 */
class AcceptAwaitable {
  public:
    AcceptAwaitable(int listenFd, void *loopPtr) : listenFd_(listenFd), loopPtr_(loopPtr) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h);

    /**
     * @return accepted fd, or -1 on error
     */
    int await_resume() const noexcept { return acceptedFd_; }

  private:
    int listenFd_;
    void *loopPtr_;
    std::coroutine_handle<> handle_;
    int acceptedFd_{-1};
};

// ─────────────────────────────────────────────────────────────────
//  EventLoop 调度器
// ─────────────────────────────────────────────────────────────────

/**
 * @brief EventLoop 调度器适配器
 *
 * 将协程恢复操作投递到 EventLoop 的任务队列。
 */
class EventLoopScheduler : public Scheduler {
  public:
    explicit EventLoopScheduler(void *loop) : loop_(loop) {}

    void schedule(std::coroutine_handle<> h) override;

  private:
    void *loop_;
};

// ─────────────────────────────────────────────────────────────────
//  协程化 Server Handler
// ─────────────────────────────────────────────────────────────────

/**
 * @brief 协程化请求处理上下文
 *
 * 封装 fd 和 EventLoop，提供协程化 I/O 方法。
 */
class CoroContext {
  public:
    CoroContext(int fd, void *loop) : fd_(fd), loop_(loop) {}

    /**
     * @brief 异步读取数据
     */
    ReadAwaitable read(char *buf, size_t len) { return ReadAwaitable{fd_, buf, len, loop_}; }

    /**
     * @brief 异步写入数据
     */
    WriteAwaitable write(const char *buf, size_t len) {
        return WriteAwaitable{fd_, buf, len, loop_};
    }

    /**
     * @brief 异步等待
     */
    template <typename Rep, typename Period>
    SleepAwaitable sleep(std::chrono::duration<Rep, Period> duration) {
        return sleepFor(duration, loop_);
    }

    int fd() const noexcept { return fd_; }

  private:
    int fd_;
    void *loop_;
};

} // namespace mcpp::coro

#endif // MCPP_HAS_COROUTINES
