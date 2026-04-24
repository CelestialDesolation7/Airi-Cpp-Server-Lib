#pragma once
#ifdef __linux__

#include "Poller.h"
#include <sys/epoll.h>

class EpollPoller : public Poller {
  private:
    int epollFd_;
    struct epoll_event *events_; // 平台独有的数据结构

  public:
    explicit EpollPoller(Eventloop *loop);
    ~EpollPoller() override; // 释放 events_

    // 加上 override 关键字
    void updateChannel(Channel *channel) override;
    void deleteChannel(Channel *channel) override;
    std::vector<Channel *> poll(int timeout = -1) override;
};

#endif