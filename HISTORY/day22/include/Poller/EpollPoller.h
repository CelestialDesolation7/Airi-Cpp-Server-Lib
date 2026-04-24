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

    // 加上 override 关键字
    void updateChannel(Channel *channel) override;
    void deleteChannel(Channel *channel) override;
    std::vector<Channel *> poll(int timeout = -1) override;
};

#endif