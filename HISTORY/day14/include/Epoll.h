#pragma once
#include "Channel.h"
#include "Macros.h"
#include <cstdint>
#include <vector>

#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

class Epoll {
    DISALLOW_COPY_AND_MOVE(Epoll)
  private:
    int epfd_;
#ifdef __APPLE__
    struct kevent *events_;
#else
    struct epoll_event *events_;
#endif

  public:
    Epoll();
    ~Epoll();
    void updateChannel(Channel *channel);
    void deleteChannel(Channel *channel);

    // 等待时间发生，返回活跃 Channel 表,而不是 epoll_event
    std::vector<Channel *> poll(int timeout = -1);
};