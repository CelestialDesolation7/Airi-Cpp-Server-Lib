#pragma once
#ifdef __linux__

#include "Poller.h"
#include <sys/epoll.h>
#include <vector>

class EpollPoller : public Poller {
  private:
    int epollFd_;
    std::vector<struct epoll_event> events_;

  public:
    explicit EpollPoller(Eventloop *loop);
    ~EpollPoller() override; // 释放 events_

    // ── Day 28：epoll_ctl 自愈重试策略（Phase 3）─────────────────────────
    // 真实环境中 fd 可能被 dup/close 等操作复用，导致：
    //   * EPOLL_CTL_ADD 返回 EEXIST：fd 已注册，应改用 EPOLL_CTL_MOD
    //   * EPOLL_CTL_MOD 返回 ENOENT：fd 未注册，应改用 EPOLL_CTL_ADD
    //   * EPOLL_CTL_DEL 返回 ENOENT/EBADF：fd 已经被对端清理，可静默忽略
    // 把这些判断从 updateChannel/deleteChannel 中提取为纯函数后，单元测试
    // 不再需要构造 epoll fd，仅需传入 op + errno 即可枚举全部分支。
    static bool shouldRetryWithMod(int op, int err);   // ADD 失败 → 是否改 MOD 重试
    static bool shouldRetryWithAdd(int op, int err);   // MOD 失败 → 是否改 ADD 重试
    static bool shouldIgnoreCtlError(int op, int err); // DEL 等可接受的失败

    // 加上 override 关键字
    void updateChannel(Channel *channel) override;
    void deleteChannel(Channel *channel) override;
    std::vector<Channel *> poll(int timeout = -1) override;
};

#endif