#pragma once
/**
 * @file IoUringPoller.h
 * @brief io_uring 后端 Poller (Linux 5.1+)
 *
 * io_uring 是 Linux 内核提供的高性能异步 I/O 接口。
 * 与 epoll 相比，它提供了：
 * - 真正的异步 I/O（不只是就绪通知）
 * - 零拷贝提交/完成队列（SQ/CQ）
 * - 批量系统调用（一次 io_uring_enter 提交/收割多个操作）
 * - Linked operations（操作链）
 *
 * @note 仅在 Linux 上可用，且需要 liburing 库
 */

#include "Poller.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

#if defined(__linux__) && defined(MCPP_HAS_IO_URING)

// 前向声明 liburing 类型，避免在头文件中包含 liburing.h
struct io_uring;
struct io_uring_sqe;
struct io_uring_cqe;

namespace mcpp::net {

/**
 * @brief io_uring 操作类型
 */
enum class UringOpType : uint8_t {
    kPollAdd, ///< 添加 poll 监听 (兼容 epoll 模式)
    kRead,    ///< 异步读
    kWrite,   ///< 异步写
    kAccept,  ///< 异步 accept
    kClose,   ///< 异步 close
    kTimeout, ///< 定时器
};

/**
 * @brief io_uring 操作上下文
 *
 * 通过 user_data 传递给内核，CQE 完成时返回。
 */
struct UringOp {
    UringOpType type;
    int fd;
    void *userData; ///< 用户回调上下文 (通常是 Channel*)
    void *buf;      ///< 读写缓冲区
    size_t len;     ///< 缓冲区长度
    int32_t result; ///< 完成结果 (从 CQE 复制)
};

/**
 * @brief io_uring Poller 实现
 *
 * 使用 io_uring 替代 epoll。可以工作在两种模式：
 * 1. **兼容模式**（默认）：使用 IORING_OP_POLL_ADD 模拟 epoll 的就绪通知
 * 2. **完全异步模式**：使用 IORING_OP_READ/WRITE 等真正的异步 I/O
 */
class IoUringPoller : public Poller {
  public:
    /**
     * @brief 构造函数
     * @param loop 所属 EventLoop
     * @param queueDepth 提交队列深度 (默认 256)
     */
    explicit IoUringPoller(Eventloop *loop, unsigned queueDepth = 256);

    ~IoUringPoller() override;

    void updateChannel(Channel *channel) override;
    void deleteChannel(Channel *channel) override;
    std::vector<Channel *> poll(int timeout = -1) override;

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        uint64_t submittedOps{0}; ///< 累计提交操作数
        uint64_t completedOps{0}; ///< 累计完成操作数
        uint64_t pollAddOps{0};   ///< POLL_ADD 操作数
    };

    const Stats &stats() const noexcept { return stats_; }

    /**
     * @brief 检查 io_uring 是否可用
     */
    static bool isAvailable() noexcept;

  private:
    /**
     * @brief 获取一个空闲的 SQE (Submission Queue Entry)
     */
    io_uring_sqe *getSqe();

    /**
     * @brief 提交并等待完成
     */
    int submitAndWait(int timeoutMs);

    /**
     * @brief 处理一个完成事件
     */
    void handleCqe(io_uring_cqe *cqe, std::vector<Channel *> &activeChannels);

    io_uring *ring_;
    unsigned queueDepth_;
    std::unordered_map<int, Channel *> channels_; ///< fd -> Channel
    Stats stats_;
};

} // namespace mcpp::net

#else // !__linux__ || !MCPP_HAS_IO_URING

namespace mcpp::net {

/**
 * @brief io_uring 不可用时的 stub 类
 */
class IoUringPoller {
  public:
    static bool isAvailable() noexcept { return false; }
};

} // namespace mcpp::net

#endif
